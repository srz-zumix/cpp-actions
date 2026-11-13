#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cpa::sandbox {

// Default allowed public hosts for common CI services and dependency downloads.
const std::vector<std::string>& default_allowed_hosts();

// Fixed public Unity services and TLS revocation hosts, enabled only by the trusted opt-in.
const std::vector<std::string>& unity_allowed_hosts();

// Matches a host name against patterns ("example.com" or "*.example.com"),
// case-insensitively. "*" allows every host. Private resolved addresses require
// an exact non-default host allowance; wildcard/default allowances never grant
// private access.
bool host_allowed(std::string_view host, const std::vector<std::string>& patterns);

// Splits "host:port" / "[v6]:port" (port defaults to `default_port`).
bool split_host_port(std::string_view authority, std::string& host, int& port, int default_port);

inline constexpr int kDefaultProxyConnections = 128;
inline constexpr int kMaxProxyConnections = 4096;

// HTTP proxy on IPv4/IPv6 loopback that only connects to allowed hosts: CONNECT
// tunnels (HTTPS) and absolute-form HTTP requests. TLS is not inspected.
class NetworkProxy {
 public:
  using DenyFn = std::function<void(const std::string& host, int port)>;
  // Called concurrently on accept/worker threads; must not throw, block, or call start()/stop().
  using WarningFn = std::function<void(std::string_view)>;

  NetworkProxy(std::vector<std::string> allowed_hosts, DenyFn on_deny, WarningFn on_warning = {},
               bool allow_unity_hosts = false, int max_connections = kDefaultProxyConnections);
  ~NetworkProxy();
  NetworkProxy(const NetworkProxy&) = delete;
  NetworkProxy& operator=(const NetworkProxy&) = delete;

  // Binds to 127.0.0.1 and ::1 on the same port. Throws std::system_error.
  void start();
  void stop();
  int port() const { return port_; }
  // http://localhost:<port>
  std::string url() const;

 private:
  friend struct NetworkProxyTestAccess;
  struct Connector;
  void accept_loop();
  void handle(int client);
  void register_fd(int fd);
  void unregister_fd(int fd);
  int connect_to(const std::string& host, int port, bool& denied, std::string& failure);
  // Requires mutex_. Timestamp plus active/connecting counts for diagnostics.
  std::string status_locked() const;

  std::vector<std::string> allowed_;
  std::vector<std::string> private_allowed_;
  DenyFn on_deny_;
  WarningFn on_warning_;
  int listen_fd_ = -1;
  int listen_v6_fd_ = -1;
  int wake_pipe_[2] = {-1, -1};
  int port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread accept_thread_;
  std::mutex mutex_;
  std::condition_variable workers_done_;
  int workers_ = 0;
  int max_connections_ = kDefaultProxyConnections;
  std::atomic<int> connecting_{0};
  // Shared with detached resolver threads, which may outlive the proxy.
  std::shared_ptr<std::atomic<int>> resolvers_ = std::make_shared<std::atomic<int>>(0);
  std::set<int> active_fds_;
};

}  // namespace cpa::sandbox
