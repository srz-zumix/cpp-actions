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
#include <system_error>

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
constexpr int kMaxWorkers = 32;
constexpr auto kConnectTimeout = std::chrono::seconds(3);

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
  addrinfo* addresses = nullptr;
  ~Resolution() {
    if (addresses) ::freeaddrinfo(addresses);
  }
};
std::atomic<int> resolvers{0};

}  // namespace

const std::vector<std::string>& default_allowed_hosts() {
  static const std::vector<std::string> hosts = {
      "github.com", "*.github.com", "githubusercontent.com", "*.githubusercontent.com", "ghcr.io", "*.ghcr.io",
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

NetworkProxy::NetworkProxy(std::vector<std::string> allowed_hosts, DenyFn on_deny)
    : allowed_(std::move(allowed_hosts)), on_deny_(std::move(on_deny)) {}
NetworkProxy::~NetworkProxy() { stop(); }
std::string NetworkProxy::url() const { return "http://127.0.0.1:" + std::to_string(port_); }

void NetworkProxy::start() {
  if (listen_fd_ >= 0) return;
  stopping_ = false;
  try {
    if (::pipe(wake_pipe_) != 0) throw std::system_error(errno, std::generic_category(), "pipe");
    for (int fd : wake_pipe_) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::system_error(errno, std::generic_category(), "socket");
    configure_socket(listen_fd_);
    ::fcntl(listen_fd_, F_SETFL, O_NONBLOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 32) != 0)
      throw std::system_error(errno, std::generic_category(), "bind proxy");
    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
      throw std::system_error(errno, std::generic_category(), "proxy address");
    port_ = ntohs(addr.sin_port);
    accept_thread_ = std::thread([this] { accept_loop(); });
  } catch (...) {
    stop();
    throw;
  }
}

void NetworkProxy::stop() {
  if (stopping_.exchange(true)) return;
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
  for (int fd : wake_pipe_)
    if (fd >= 0) ::close(fd);
  listen_fd_ = -1;
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

int NetworkProxy::connect_to(const std::string& host, int port, bool& denied) {
  const auto result = std::make_shared<Resolution>();
  if (resolvers.fetch_add(1) >= kMaxWorkers) {
    --resolvers;
    return -1;
  }
  try {
    std::thread([result, host, port] {
      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      addrinfo* addresses = nullptr;
      ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses);
      {
        std::lock_guard lock(result->mutex);
        result->addresses = addresses;
        result->done = true;
      }
      result->ready.notify_all();
      --resolvers;
    }).detach();
  } catch (...) {
    --resolvers;
    return -1;
  }
  const auto deadline = std::chrono::steady_clock::now() + kConnectTimeout;
  {
    std::unique_lock lock(result->mutex);
    while (!result->done) {
      if (stopping_ || std::chrono::steady_clock::now() >= deadline) return -1;
      result->ready.wait_for(lock, std::chrono::milliseconds(50));
    }
  }
  bool exact = false;
  for (const auto& pattern : allowed_) {
    // Built-in GitHub allowances are not permission to follow a poisoned DNS
    // answer into the local network. Explicit local names/literals still work.
    if (canonical(util::trim(pattern)) == canonical(host) && !host_allowed(host, default_allowed_hosts())) exact = true;
  }
  // Validate every answer before dialing any; connect the checked sockaddr
  // directly, never perform a second DNS lookup.
  for (auto* ai = result->addresses; ai; ai = ai->ai_next) {
    if (!exact && private_address(ai->ai_addr)) {
      denied = true;
      return -1;
    }
  }
  for (auto* ai = result->addresses; ai && !stopping_; ai = ai->ai_next) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    configure_socket(fd);
    register_fd(fd);
    ::fcntl(fd, F_SETFL, O_NONBLOCK);
    bool connected = ::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0;
    if (!connected && errno == EINPROGRESS) {
      while (!stopping_ && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{fd, POLLOUT, 0};
        const int ready = ::poll(&pfd, 1, 50);
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) continue;
        int error = 0;
        socklen_t len = sizeof(error);
        connected = ready > 0 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0;
        break;
      }
    }
    if (connected && !stopping_) {
      ::fcntl(fd, F_SETFL, 0);
      return fd;
    }
    unregister_fd(fd);
    ::close(fd);
  }
  return -1;
}

void NetworkProxy::accept_loop() {
  while (!stopping_) {
    pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (fds[1].revents != 0 || stopping_) return;
    if (!(fds[0].revents & POLLIN)) continue;
    const int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) continue;
    configure_socket(client);
    {
      std::lock_guard lock(mutex_);
      if (workers_ >= kMaxWorkers) {
        ::close(client);
        continue;
      }
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
        if (--workers_ == 0) workers_done_.notify_all();
      }).detach();
    } catch (...) {
      ::close(client);
      std::lock_guard lock(mutex_);
      if (--workers_ == 0) workers_done_.notify_all();
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
  const int upstream = connect_to(host, port, denied);
  if (upstream < 0) {
    if (denied) return deny();
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
    // HTTP is deliberately one request per connection. Never relay arbitrary
    // client bytes after its framed body, even if the upstream ignores close.
    ::shutdown(upstream, SHUT_WR);
  }
  if (ok) relay(client, upstream, tunnel);
}

}  // namespace cpa::sandbox
