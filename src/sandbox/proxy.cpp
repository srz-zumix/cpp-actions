#include "sandbox/proxy.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>

#include "util/strings.hpp"

namespace cpa::sandbox {

namespace {

#if defined(MSG_NOSIGNAL)
constexpr int MSG_NOSIGNAL_COMPAT = MSG_NOSIGNAL;
#else
constexpr int MSG_NOSIGNAL_COMPAT = 0;  // SO_NOSIGPIPE is set on the sockets
#endif

constexpr std::size_t kMaxHeader = 64 * 1024;

bool send_all(int fd, std::string_view data) {
  while (!data.empty()) {
    const auto n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL_COMPAT);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(n));
  }
  return true;
}

int connect_to(const std::string& host, int port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const auto service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0) return -1;
  int fd = -1;
  for (auto* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
#if defined(SO_NOSIGPIPE)
  if (fd >= 0) {
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif
  return fd;
}

// Copies data in both directions until both sides are closed.
void relay(int a, int b) {
  char buf[64 * 1024];
  pollfd fds[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
  while (fds[0].fd >= 0 || fds[1].fd >= 0) {
    const int r = ::poll(fds, 2, -1);
    if (r < 0) {
      if (errno == EINTR) continue;
      return;
    }
    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd < 0 || fds[i].revents == 0) continue;
      const int from = i == 0 ? a : b;
      const int to = i == 0 ? b : a;
      const auto n = ::recv(from, buf, sizeof(buf), 0);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        fds[i].fd = -1;  // ignored by poll
        ::shutdown(to, SHUT_WR);
        if (n < 0) return;
        continue;
      }
      if (!send_all(to, std::string_view(buf, static_cast<std::size_t>(n)))) return;
    }
  }
}

std::string forbidden(const std::string& host) {
  const std::string body = "cpp-actions: connections to '" + host +
                           "' are not allowed in the sandbox. Use --allow-host " + host + " to allow them.\n";
  return "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}

const std::string kBadRequest = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
const std::string kBadGateway = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

}  // namespace

const std::vector<std::string>& default_allowed_hosts() {
  static const std::vector<std::string> hosts = {
      "github.com", "*.github.com", "githubusercontent.com", "*.githubusercontent.com", "ghcr.io", "*.ghcr.io",
  };
  return hosts;
}

bool host_allowed(std::string_view host, const std::vector<std::string>& patterns) {
  std::string h = util::to_lower(host);
  while (!h.empty() && h.back() == '.') h.pop_back();
  if (h.empty()) return false;
  for (const auto& raw : patterns) {
    const auto p = util::to_lower(util::trim(raw));
    if (p == "*") return true;
    if (p.starts_with("*.")) {
      const auto suffix = p.substr(1);  // ".example.com"
      if (h.size() > suffix.size() && h.ends_with(suffix)) return true;
    } else if (h == p) {
      return true;
    }
  }
  return false;
}

bool split_host_port(std::string_view authority, std::string& host, int& port, int default_port) {
  port = default_port;
  if (authority.starts_with("[")) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos) return false;
    host = std::string(authority.substr(1, close - 1));
    authority.remove_prefix(close + 1);
    if (authority.empty()) return !host.empty();
    if (authority[0] != ':') return false;
    authority.remove_prefix(1);
  } else {
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
      host = std::string(authority);
      return !host.empty();
    }
    host = std::string(authority.substr(0, colon));
    authority.remove_prefix(colon + 1);
  }
  const auto p = util::parse_int(authority);
  if (!p || *p <= 0 || *p > 65535) return false;
  port = static_cast<int>(*p);
  return !host.empty();
}

NetworkProxy::NetworkProxy(std::vector<std::string> allowed_hosts, DenyFn on_deny)
    : allowed_(std::move(allowed_hosts)), on_deny_(std::move(on_deny)) {}

NetworkProxy::~NetworkProxy() { stop(); }

std::string NetworkProxy::url() const { return "http://127.0.0.1:" + std::to_string(port_); }

void NetworkProxy::start() {
  if (::pipe(wake_pipe_) != 0) throw std::system_error(errno, std::generic_category(), "pipe");
  for (int fd : wake_pipe_) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) throw std::system_error(errno, std::generic_category(), "socket");
  ::fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 64) != 0) {
    const int e = errno;
    ::close(listen_fd_);
    listen_fd_ = -1;
    throw std::system_error(e, std::generic_category(), "bind proxy");
  }
  socklen_t len = sizeof(addr);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  accept_thread_ = std::thread([this] { accept_loop(); });
}

void NetworkProxy::stop() {
  if (stopping_.exchange(true)) return;
  if (wake_pipe_[1] >= 0) {
    const char c = 0;
    [[maybe_unused]] const auto n = ::write(wake_pipe_[1], &c, 1);
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  {
    std::lock_guard lock(mutex_);
    for (int fd : active_fds_) ::shutdown(fd, SHUT_RDWR);
  }
  {
    std::unique_lock lock(mutex_);
    workers_done_.wait(lock, [this] { return workers_ == 0; });
  }
  if (listen_fd_ >= 0) ::close(listen_fd_);
  for (int fd : wake_pipe_) {
    if (fd >= 0) ::close(fd);
  }
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

void NetworkProxy::accept_loop() {
  while (!stopping_) {
    pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
    if (::poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (fds[1].revents != 0 || stopping_) return;
    if ((fds[0].revents & POLLIN) == 0) continue;
    const int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) continue;
    ::fcntl(client, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    {
      std::lock_guard lock(mutex_);
      ++workers_;
    }
    std::thread([this, client] {
      handle(client);
      std::lock_guard lock(mutex_);
      if (--workers_ == 0) workers_done_.notify_all();
    }).detach();
  }
}

void NetworkProxy::handle(int client) {
  register_fd(client);
  std::string head;
  char buf[8192];
  std::size_t end = std::string::npos;
  while (end == std::string::npos && head.size() < kMaxHeader) {
    const auto n = ::recv(client, buf, sizeof(buf), 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    head.append(buf, static_cast<std::size_t>(n));
    end = head.find("\r\n\r\n");
  }
  auto finish = [&] {
    unregister_fd(client);
    ::close(client);
  };
  if (end == std::string::npos) {
    send_all(client, kBadRequest);
    finish();
    return;
  }
  const auto line_end = head.find("\r\n");
  const auto request_line = head.substr(0, line_end);
  const auto parts = util::split(request_line, ' ');
  if (parts.size() != 3 || !parts[2].starts_with("HTTP/")) {
    send_all(client, kBadRequest);
    finish();
    return;
  }
  const auto& method = parts[0];
  const auto& target = parts[1];
  std::string host;
  int port = 0;
  std::string forward;  // request to send upstream (plain HTTP)
  if (method == "CONNECT") {
    if (!split_host_port(target, host, port, 443)) {
      send_all(client, kBadRequest);
      finish();
      return;
    }
  } else {
    if (!util::istarts_with(target, "http://")) {
      send_all(client, kBadRequest);
      finish();
      return;
    }
    const auto rest = std::string_view(target).substr(7);
    const auto slash = rest.find('/');
    const auto authority = rest.substr(0, slash);
    const std::string path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
    const auto at = authority.rfind('@');
    if (!split_host_port(at == std::string_view::npos ? authority : authority.substr(at + 1), host, port, 80)) {
      send_all(client, kBadRequest);
      finish();
      return;
    }
    forward = method + " " + path + " " + parts[2] + "\r\n";
    // Drop proxy headers and force a single request per connection.
    for (const auto& line : util::split(head.substr(line_end + 2, end - line_end - 2), '\n')) {
      std::string_view l = util::trim_right(line);
      if (l.empty()) continue;
      if (util::istarts_with(l, "proxy-") || util::istarts_with(l, "connection:") ||
          util::istarts_with(l, "keep-alive:")) {
        continue;
      }
      forward += std::string(l) + "\r\n";
    }
    forward += "Connection: close\r\n\r\n";
    forward += head.substr(end + 4);
  }
  if (!host_allowed(host, allowed_)) {
    if (on_deny_) on_deny_(host, port);
    send_all(client, forbidden(host));
    finish();
    return;
  }
  const int upstream = connect_to(host, port);
  if (upstream < 0) {
    send_all(client, kBadGateway);
    finish();
    return;
  }
  register_fd(upstream);
  bool ok = true;
  if (method == "CONNECT") {
    ok = send_all(client, "HTTP/1.1 200 Connection Established\r\n\r\n");
    const auto extra = head.substr(end + 4);
    if (ok && !extra.empty()) ok = send_all(upstream, extra);
  } else {
    ok = send_all(upstream, forward);
  }
  if (ok) relay(client, upstream);
  unregister_fd(upstream);
  ::close(upstream);
  finish();
}

}  // namespace cpa::sandbox
