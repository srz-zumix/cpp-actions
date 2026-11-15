#include "sandbox/proxy.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <system_error>

#include "sandbox/connect.hpp"
#include "util/strings.hpp"

namespace cpa::sandbox {
namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kNoSignal = MSG_NOSIGNAL;
#else
constexpr int kNoSignal = 0;
#endif
constexpr std::size_t kMaxHeader = 64 * 1024;
constexpr std::size_t kMaxBody = 64 * 1024 * 1024;

void configure_socket(int fd) {
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  timeval timeout{5, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#if defined(SO_NOSIGPIPE)
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

bool send_all(int fd, std::string_view data) {
  while (!data.empty()) {
    const auto n = ::send(fd, data.data(), data.size(), kNoSignal);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    data.remove_prefix(static_cast<std::size_t>(n));
  }
  return true;
}

bool valid_host(std::string_view host) {
  if (host.empty() || host.size() > 253) return false;
  in6_addr v6{};
  if (::inet_pton(AF_INET6, std::string(host).c_str(), &v6) == 1) return true;
  if (host.back() == '.') host.remove_suffix(1);
  if (host.empty()) return false;
  for (auto label : util::split(host, '.')) {
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
    for (unsigned char c : label) {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
  }
  return true;
}

std::string canonical(std::string_view host) {
  auto result = util::to_lower(host);
  if (!result.empty() && result.back() == '.') result.pop_back();
  return result;
}

bool private_address(const sockaddr* addr) {
  if (addr->sa_family == AF_INET) {
    const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(addr)->sin_addr.s_addr);
    return (ip >> 24) == 0 || (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 24) >= 224 || (ip >> 16) == 0xa9fe ||
           (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8 || (ip >> 22) == 0x0191 || (ip >> 8) == 0xc00000 ||
           (ip >> 8) == 0xc00002 || (ip >> 17) == 0x6309 || (ip >> 8) == 0xc63364 || (ip >> 8) == 0xcb0071;
  }
  if (addr->sa_family == AF_INET6) {
    const auto& ip = reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr;
    if (IN6_IS_ADDR_V4MAPPED(&ip)) {
      sockaddr_in v4{};
      v4.sin_family = AF_INET;
      std::copy(ip.s6_addr + 12, ip.s6_addr + 16, reinterpret_cast<unsigned char*>(&v4.sin_addr));
      return private_address(reinterpret_cast<const sockaddr*>(&v4));
    }
    // Only global unicast is public; this also excludes loopback, ULA,
    // link-local, multicast and IPv4-compatible aliases.
    return (ip.s6_addr[0] & 0xe0) != 0x20 || (ip.s6_addr[0] == 0x20 && ip.s6_addr[1] == 0x02) ||
           (ip.s6_addr[0] == 0x20 && ip.s6_addr[1] == 0x01 && ip.s6_addr[2] < 2) ||
           (ip.s6_addr[0] == 0x20 && ip.s6_addr[1] == 0x01 && ip.s6_addr[2] == 0x0d && ip.s6_addr[3] == 0xb8) ||
           (ip.s6_addr[0] == 0x3f && ip.s6_addr[1] == 0xff && (ip.s6_addr[2] & 0xf0) == 0);
  }
  return true;
}

bool token(std::string_view value) {
  if (value.empty()) return false;
  for (unsigned char c : value) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos))
      return false;
  }
  return true;
}

bool number(std::string_view value, std::size_t limit, std::size_t& out) {
  if (value.empty()) return false;
  out = 0;
  for (char c : value) {
    if (c < '0' || c > '9' || out > limit / 10) return false;
    out = out * 10 + static_cast<unsigned>(c - '0');
    if (out > limit) return false;
  }
  return true;
}

void relay(int a, int b, bool tunnel) {
  char buf[16384];
  pollfd fds[2] = {{tunnel ? a : -1, POLLIN, 0}, {b, POLLIN, 0}};
  while (fds[0].fd >= 0 || fds[1].fd >= 0) {
    const int r = ::poll(fds, 2, 30000);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return;
    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd < 0 || !fds[i].revents) continue;
      const int from = i == 0 ? a : b;
      const int to = i == 0 ? b : a;
      const auto n = ::recv(from, buf, sizeof(buf), 0);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        fds[i].fd = -1;
        ::shutdown(to, SHUT_WR);
        if (n < 0) return;
      } else if (!send_all(to, std::string_view(buf, static_cast<std::size_t>(n)))) {
        return;
      }
    }
  }
}

const std::string kBadRequest = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
const std::string kBadGateway = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
const std::string kForbidden = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

// A stalled system resolver must not hold stop() hostage. Resolver tasks own
// their data, never access the proxy, and have a process-wide concurrency cap.
struct Resolution {
  std::mutex mutex;
  std::condition_variable ready;
  bool done = false;
  int error = 0;
  int system_error = 0;
  addrinfo* addresses = nullptr;
  ~Resolution() {
    if (addresses) ::freeaddrinfo(addresses);
  }
};

}  // namespace

bool detail::public_addresses(const addrinfo* first) {
  for (auto* address = first; address; address = address->ai_next) {
    if (private_address(address->ai_addr)) return false;
  }
  return true;
}

const std::vector<std::string>& default_allowed_hosts() {
  static const std::vector<std::string> hosts = {
      "github.com", "*.github.com", "githubusercontent.com", "*.githubusercontent.com", "ghcr.io", "*.ghcr.io",
      "builds.dotnet.microsoft.com", "ci.dot.net", "aka.ms", "api.nuget.org",
      "nugetregistryv2prod.blob.core.windows.net",
      "rubygems.org", "index.rubygems.org",
      "dl.google.com", "maven.google.com",
      "repo.maven.apache.org", "plugins.gradle.org",
      "api.adoptium.net",
      "www.apple.com",
  };
  return hosts;
}

const std::vector<std::string>& unity_allowed_hosts() {
  static const std::vector<std::string> hosts = {
      "packages.unity.com", "cdn.packages.unity.com", "download.packages.unity.com",
      "public-cdn.cloud.unity3d.com", "license.unity3d.com", "cdp.cloud.unity3d.com",
      "config.uca.cloud.unity3d.com",
      "crl3.digicert.com", "crl4.digicert.com",
  };
  return hosts;
}

bool host_allowed(std::string_view host, const std::vector<std::string>& patterns) {
  if (!valid_host(host)) return false;
  const auto h = canonical(host);
  for (const auto& raw : patterns) {
    const auto p = util::to_lower(util::trim(raw));
    if (p == "*") return true;
    if (p.starts_with("*.")) {
      const auto suffix = "." + canonical(std::string_view(p).substr(2));
      if (valid_host(std::string_view(p).substr(2)) && h.size() > suffix.size() && h.ends_with(suffix)) return true;
    } else if (valid_host(p) && h == canonical(p)) {
      return true;
    }
  }
  return false;
}

bool split_host_port(std::string_view authority, std::string& host, int& port, int default_port) {
  port = default_port;
  std::string_view service;
  if (authority.starts_with("[")) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos) return false;
    host = std::string(authority.substr(1, close - 1));
    if (host.find('%') != std::string::npos) return false;
    in6_addr v6{};
    if (::inet_pton(AF_INET6, host.c_str(), &v6) != 1) return false;
    authority.remove_prefix(close + 1);
    if (!authority.empty()) {
      if (authority.front() != ':') return false;
      service = authority.substr(1);
    }
  } else {
    const auto colon = authority.find(':');
    host = std::string(authority.substr(0, colon));
    if (colon != std::string_view::npos) service = authority.substr(colon + 1);
  }
  if (!valid_host(host)) return false;
  if (!service.empty()) {
    std::size_t parsed = 0;
    if (!number(service, 65535, parsed) || parsed == 0) return false;
    port = static_cast<int>(parsed);
  } else if (authority.ends_with(":")) {
    return false;
  }
  return port > 0 && port <= 65535;
}

NetworkProxy::NetworkProxy(std::vector<std::string> allowed_hosts, DenyFn on_deny, WarningFn on_warning,
                           bool allow_unity_hosts, int max_connections)
    : allowed_(std::move(allowed_hosts)),
      on_deny_(std::move(on_deny)),
      on_warning_(std::move(on_warning)),
      max_connections_(max_connections) {
  if (max_connections_ < 1 || max_connections_ > kMaxProxyConnections) {
    throw std::invalid_argument("proxy max connections must be between 1 and " +
                                std::to_string(kMaxProxyConnections));
  }
  // Only explicit, exact, non-default allowances grant private-address access.
  // Preset entries are appended afterwards and cannot confer that privilege.
  for (const auto& pattern : allowed_) {
    const auto host = util::trim(pattern);
    if (valid_host(host) && !host_allowed(host, default_allowed_hosts())) private_allowed_.emplace_back(host);
  }
  if (allow_unity_hosts)
    allowed_.insert(allowed_.end(), unity_allowed_hosts().begin(), unity_allowed_hosts().end());
}
NetworkProxy::~NetworkProxy() { stop(); }

std::string NetworkProxy::status_locked() const {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm local{};
  ::localtime_r(&seconds, &local);
  char time[32];
  std::strftime(time, sizeof(time), "%H:%M:%S", &local);
  char fraction[8];
  std::snprintf(fraction, sizeof(fraction), ".%03d", static_cast<int>(millis));
  return std::string("; at ") + time + fraction + ", active " + std::to_string(workers_) + "/" +
         std::to_string(max_connections_) + ", connecting upstream " + std::to_string(connecting_.load());
}
std::string NetworkProxy::url() const { return "http://localhost:" + std::to_string(port_); }

void NetworkProxy::start() {
  if (listen_fd_ >= 0) return;
  stopping_ = false;
  try {
    if (::pipe(wake_pipe_) != 0) throw std::system_error(errno, std::generic_category(), "pipe");
    for (int fd : wake_pipe_) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    for (int attempt = 0; attempt < 8; ++attempt) {
      listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
      if (listen_fd_ < 0) throw std::system_error(errno, std::generic_category(), "IPv4 proxy socket");
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::system_error(errno, std::generic_category(), "bind IPv4 proxy");
      socklen_t len = sizeof(addr);
      if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        throw std::system_error(errno, std::generic_category(), "proxy address");
      port_ = ntohs(addr.sin_port);
      listen_v6_fd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
      if (listen_v6_fd_ < 0) throw std::system_error(errno, std::generic_category(), "IPv6 proxy socket");
      const int one = 1;
      if (::setsockopt(listen_v6_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) != 0)
        throw std::system_error(errno, std::generic_category(), "IPv6-only proxy socket");
      sockaddr_in6 addr6{};
      addr6.sin6_family = AF_INET6;
      addr6.sin6_addr = in6addr_loopback;
      addr6.sin6_port = addr.sin_port;
      if (::bind(listen_v6_fd_, reinterpret_cast<sockaddr*>(&addr6), sizeof(addr6)) == 0) break;
      const int error = errno;
      if (error != EADDRINUSE || attempt == 7)
        throw std::system_error(error, std::generic_category(), "bind IPv6 loopback proxy");
      ::close(listen_fd_);
      ::close(listen_v6_fd_);
      listen_fd_ = listen_v6_fd_ = -1;
    }
    for (int fd : {listen_fd_, listen_v6_fd_}) {
      configure_socket(fd);
      if (::fcntl(fd, F_SETFL, O_NONBLOCK) != 0 || ::listen(fd, max_connections_) != 0)
        throw std::system_error(errno, std::generic_category(), "listen on loopback proxy");
    }
    accept_thread_ = std::thread([this] { accept_loop(); });
  } catch (...) {
    stop();
    throw;
  }
}

void NetworkProxy::stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_.exchange(true)) return;
  }
  workers_done_.notify_all();
  if (wake_pipe_[1] >= 0) {
    const char c = 0;
    [[maybe_unused]] const auto n = ::write(wake_pipe_[1], &c, 1);
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  {
    std::unique_lock lock(mutex_);
    for (int fd : active_fds_) ::shutdown(fd, SHUT_RDWR);
    workers_done_.wait(lock, [this] { return workers_ == 0; });
  }
  if (listen_fd_ >= 0) ::close(listen_fd_);
  if (listen_v6_fd_ >= 0) ::close(listen_v6_fd_);
  for (int fd : wake_pipe_)
    if (fd >= 0) ::close(fd);
  listen_fd_ = listen_v6_fd_ = -1;
  wake_pipe_[0] = wake_pipe_[1] = -1;
}

void NetworkProxy::register_fd(int fd) {
  std::lock_guard lock(mutex_);
  active_fds_.insert(fd);
  if (stopping_) ::shutdown(fd, SHUT_RDWR);
}
void NetworkProxy::unregister_fd(int fd) {
  std::lock_guard lock(mutex_);
  active_fds_.erase(fd);
}

struct NetworkProxy::Connector {
  NetworkProxy& proxy;
  const std::vector<const addrinfo*>& addresses;
  std::string& failure;

  auto now() const { return std::chrono::steady_clock::now(); }
  bool stopped() const { return proxy.stopping_.load(); }

  detail::ConnectionAttempt start(std::size_t index) {
    const auto* address = addresses[index];
    const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) return {-1, errno};
    auto cleanup = [](const int* socket) { ::close(*socket); };
    std::unique_ptr<const int, decltype(cleanup)> owned(&fd, cleanup);
    configure_socket(fd);
    proxy.register_fd(fd);
    owned.release();
    if (::fcntl(fd, F_SETFL, O_NONBLOCK) != 0) return {fd, errno};
    if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) return {fd, 0};
    return {fd, errno};
  }

  void wait(std::vector<detail::PendingConnection>& pending, std::chrono::milliseconds timeout) {
    std::vector<pollfd> fds;
    fds.reserve(pending.size());
    for (const auto& socket : pending) fds.push_back({socket.fd, POLLOUT, 0});
    const int ready = ::poll(fds.data(), fds.size(), static_cast<int>(timeout.count()));
    if (ready < 0 && errno == EINTR) return;
    const int poll_error = ready < 0 ? errno : 0;
    for (std::size_t i = 0; i < pending.size(); ++i) {
      if (poll_error != 0) {
        pending[i].error = poll_error;
      } else if (fds[i].revents != 0) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) error = errno;
        pending[i].error = error;
      }
    }
  }

  int finish(int fd) {
    return ::fcntl(fd, F_SETFL, 0) == 0 ? 0 : errno;
  }

  void close(int fd) {
    proxy.unregister_fd(fd);
    ::close(fd);
  }

  void record(std::size_t index, int error) {
    char address[NI_MAXHOST]{};
    const auto* ai = addresses[index];
    const int name_error = ::getnameinfo(ai->ai_addr, ai->ai_addrlen, address, sizeof(address), nullptr, 0,
                                       NI_NUMERICHOST);
    failure += "; " + (name_error == 0 ? std::string(address) : std::string("address #") + std::to_string(index)) +
               ": " + std::generic_category().message(error) + " (errno=" + std::to_string(error) + ")";
  }
};

int NetworkProxy::connect_to(const std::string& host, int port, bool& denied, std::string& failure) {
  const auto result = std::make_shared<Resolution>();
  const auto resolvers = resolvers_;
  if (resolvers->fetch_add(1) >= max_connections_) {
    --*resolvers;
    failure = "DNS resolver concurrency limit reached (" + std::to_string(max_connections_) + ")";
    return -1;
  }
  try {
    std::thread([result, resolvers, host, port] {
      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      addrinfo* addresses = nullptr;
      const int error = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses);
      const int system_error = errno;
      {
        std::lock_guard lock(result->mutex);
        result->addresses = addresses;
        result->error = error;
        result->system_error = system_error;
        result->done = true;
      }
      result->ready.notify_all();
      --*resolvers;
    }).detach();
  } catch (const std::system_error& error) {
    --*resolvers;
    failure = "DNS resolver thread creation failed: " + std::string(error.what());
    return -1;
  }
  const auto deadline = std::chrono::steady_clock::now() + detail::kDnsTimeout;
  {
    std::unique_lock lock(result->mutex);
    while (!result->done) {
      if (stopping_) return -1;
      if (std::chrono::steady_clock::now() >= deadline) {
        failure = "DNS resolution timed out (DNS deadline: " +
                  std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(detail::kDnsTimeout).count()) +
                  " ms)";
        return -1;
      }
      result->ready.wait_for(lock, std::chrono::milliseconds(50));
    }
  }
  if (result->error != 0) {
    failure = "DNS resolution failed: " + std::string(::gai_strerror(result->error)) +
              " (getaddrinfo=" + std::to_string(result->error) + ")";
    if (result->error == EAI_SYSTEM)
      failure += ": " + std::generic_category().message(result->system_error) +
                 " (errno=" + std::to_string(result->system_error) + ")";
    return -1;
  }
  failure = "TCP connection failed";
  if (!result->addresses) {
    failure = "DNS resolution returned no addresses";
    return -1;
  }
  const bool exact = host_allowed(host, private_allowed_);
  // Validate every answer before dialing any; connect the checked sockaddr
  // directly, never perform a second DNS lookup.
  if (!exact && !detail::public_addresses(result->addresses)) {
    denied = true;
    return -1;
  }
  const auto addresses = detail::interleave_addresses(result->addresses);
  Connector connector{*this, addresses, failure};
  const auto started = connector.now();
  const int fd = detail::race_connections(addresses.size(), connector);
  if (fd < 0 && !stopping_ && connector.now() - started >= detail::kTcpTimeout)
    failure += "; TCP deadline exceeded (" +
               std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(detail::kTcpTimeout).count()) +
               " ms)";
  return fd;
}

void NetworkProxy::accept_loop() {
  while (!stopping_) {
    pollfd fds[3] = {{listen_fd_, POLLIN, 0}, {listen_v6_fd_, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
    if (::poll(fds, 3, -1) < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (fds[2].revents != 0 || stopping_) return;
    for (const auto& listener : {fds[0], fds[1]}) {
      if (stopping_) return;
      if (!(listener.revents & POLLIN)) continue;
      {
        std::unique_lock lock(mutex_);
        if (workers_ >= max_connections_ && !stopping_) {
          const auto status = status_locked();
          lock.unlock();
          if (on_warning_) {
            on_warning_("Network proxy at capacity (" + std::to_string(max_connections_) + " active connections" +
                        status + "); waiting for a connection slot");
          }
          lock.lock();
          // Leave excess connections in the bounded kernel listen backlog.
          workers_done_.wait(lock, [this] { return stopping_ || workers_ < max_connections_; });
        }
        if (stopping_) return;
      }
      const int client = ::accept(listener.fd, nullptr, nullptr);
      if (client < 0) continue;
      const int flags = ::fcntl(client, F_GETFL, 0);
      if (flags >= 0) ::fcntl(client, F_SETFL, flags & ~O_NONBLOCK);
      configure_socket(client);
      {
        std::lock_guard lock(mutex_);
        ++workers_;
      }
      try {
        std::thread([this, client] {
          register_fd(client);
          try {
            handle(client);
          } catch (...) {
          }
          unregister_fd(client);
          ::close(client);
          std::lock_guard lock(mutex_);
          --workers_;
          workers_done_.notify_all();
        }).detach();
      } catch (...) {
        ::close(client);
        std::lock_guard lock(mutex_);
        --workers_;
        workers_done_.notify_all();
      }
    }
  }
}

void NetworkProxy::handle(int client) {
  std::string head;
  char buf[8192];
  std::size_t end = std::string::npos;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (end == std::string::npos && head.size() < kMaxHeader && !stopping_ &&
         std::chrono::steady_clock::now() < deadline) {
    const auto n = ::recv(client, buf, std::min(sizeof(buf), kMaxHeader - head.size()), 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    head.append(buf, static_cast<std::size_t>(n));
    end = head.find("\r\n\r\n");
  }
  auto bad = [&] { send_all(client, kBadRequest); };
  if (end == std::string::npos) return bad();
  const auto line_end = head.find("\r\n");
  const auto parts = util::split(head.substr(0, line_end), ' ');
  if (parts.size() != 3 || !token(parts[0]) || (parts[2] != "HTTP/1.0" && parts[2] != "HTTP/1.1")) return bad();
  const auto& method = parts[0];
  const auto& target = parts[1];
  for (unsigned char c : target)
    if (c <= 32 || c >= 127 || c == '#' || c == '\\') return bad();
  std::string host;
  int port = 0;
  std::string authority;
  std::string path;
  const bool tunnel = method == "CONNECT";
  if (tunnel) {
    authority = target;
    if (!split_host_port(authority, host, port, 443)) return bad();
  } else {
    if (!util::istarts_with(target, "http://")) return bad();
    const auto rest = std::string_view(target).substr(7);
    const auto slash = rest.find_first_of("/?");
    authority = std::string(rest.substr(0, slash));
    path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
    if (path.front() == '?') path.insert(0, "/");
    if (!split_host_port(authority, host, port, 80)) return bad();
  }
  std::size_t length = 0;
  bool seen_length = false;
  bool seen_host = false;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::string> connection_headers;
  for (std::size_t pos = line_end + 2; pos < end;) {
    const auto next = head.find("\r\n", pos);
    if (next == std::string::npos || next > end) return bad();
    const std::string_view line(head.data() + pos, next - pos);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || !token(line.substr(0, colon))) return bad();
    auto name = util::to_lower(line.substr(0, colon));
    const auto raw_value = line.substr(colon + 1);
    for (unsigned char c : raw_value)
      if ((c < 32 && c != '\t') || c == 127) return bad();
    const auto value = util::trim(raw_value);
    if (name == "transfer-encoding" || name == "upgrade" || name == "expect" || name == "trailer") return bad();
    if (name == "content-length") {
      if (seen_length || !number(value, kMaxBody, length)) return bad();
      seen_length = true;
    }
    if (name == "host") {
      std::string header_host;
      int header_port = 0;
      if (seen_host || !split_host_port(value, header_host, header_port, tunnel ? 443 : 80) ||
          canonical(header_host) != canonical(host) || header_port != port)
        return bad();
      seen_host = true;
    }
    if (name == "connection") {
      for (const auto& item : util::split(value, ',')) {
        const auto field = util::to_lower(util::trim(item));
        if (!token(field) || field == "host" || field == "content-length") return bad();
        connection_headers.push_back(field);
      }
    }
    headers.emplace_back(std::move(name), std::string(value));
    pos = next + 2;
  }
  if ((!tunnel && parts[2] == "HTTP/1.1" && !seen_host) || (tunnel && length != 0)) return bad();
  const auto extra = std::string_view(head).substr(end + 4);
  if (!tunnel && extra.size() > length) return bad();
  auto deny = [&] {
    if (on_deny_) on_deny_(host, port);
    send_all(client, kForbidden);
  };
  if (!host_allowed(host, allowed_)) return deny();
  bool denied = false;
  std::string failure;
  const auto started = std::chrono::steady_clock::now();
  ++connecting_;
  int upstream = -1;
  try {
    upstream = connect_to(host, port, denied, failure);
  } catch (...) {
    --connecting_;
    throw;
  }
  --connecting_;
  if (upstream < 0) {
    if (denied) return deny();
    if (!stopping_ && on_warning_) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started).count();
      std::string status;
      {
        std::lock_guard lock(mutex_);
        status = status_locked();
      }
      on_warning_("Network proxy upstream connection failed for " + host + ":" + std::to_string(port) +
                  " after " + std::to_string(elapsed) + " ms: " + failure + "; returning HTTP 502 (" +
                  status.substr(2) + ")");
    }
    send_all(client, kBadGateway);
    return;
  }
  // Cleanup also runs if forwarding allocation or a callback throws.
  auto cleanup = [this](const int* fd) {
    unregister_fd(*fd);
    ::close(*fd);
  };
  const std::unique_ptr<const int, decltype(cleanup)> owned(&upstream, cleanup);
  bool ok = true;
  if (tunnel) {
    ok = send_all(client, "HTTP/1.1 200 Connection Established\r\n\r\n") && send_all(upstream, extra);
  } else {
    std::string forward = method + " " + path + " " + parts[2] + "\r\nHost: " + authority + "\r\n";
    for (const auto& [name, value] : headers) {
      if (name == "host" || name.starts_with("proxy-") || name == "connection" || name == "keep-alive" ||
          name == "te" ||
          std::find(connection_headers.begin(), connection_headers.end(), name) != connection_headers.end())
        continue;
      forward += name + ": " + value + "\r\n";
    }
    ok = send_all(upstream, forward + "Connection: close\r\n\r\n") && send_all(upstream, extra);
    std::size_t remaining = length - extra.size();
    while (ok && remaining && !stopping_) {
      const auto n = ::recv(client, buf, std::min(sizeof(buf), remaining), 0);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        ok = false;
        break;
      }
      remaining -= static_cast<std::size_t>(n);
      ok = send_all(upstream, std::string_view(buf, static_cast<std::size_t>(n)));
    }
    // HTTP is deliberately one request per connection. relay() never reads
    // client bytes after the framed body. Do not half-close the upstream:
    // some servers drop a request without responding once they see EOF.
  }
  if (ok) relay(client, upstream, tunnel);
  int flags = ::fcntl(client, F_GETFL, 0);
  if (flags >= 0 && ::fcntl(client, F_SETFL, flags | O_NONBLOCK) == 0) {
    std::size_t discarded = 0;
    while (discarded < kMaxHeader) {
      const auto n = ::recv(client, buf, std::min(sizeof(buf), kMaxHeader - discarded), 0);
      if (n > 0) {
        discarded += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    ::fcntl(client, F_SETFL, flags);
  }
}

}  // namespace cpa::sandbox
