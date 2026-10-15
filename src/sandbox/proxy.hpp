#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cpa::sandbox {

// Hosts that are always reachable from the sandbox (GitHub).
const std::vector<std::string>& default_allowed_hosts();

// Matches a host name against patterns ("example.com" or "*.example.com"),
// case-insensitively. "*" allows every host.
bool host_allowed(std::string_view host, const std::vector<std::string>& patterns);

// Splits "host:port" / "[v6]:port" (port defaults to `default_port`).
bool split_host_port(std::string_view authority, std::string& host, int& port, int default_port);

// HTTP proxy on 127.0.0.1 that only connects to allowed hosts: CONNECT
// tunnels (HTTPS) and absolute-form HTTP requests. TLS is not inspected.
class NetworkProxy {
 public:
  using DenyFn = std::function<void(const std::string& host, int port)>;

  NetworkProxy(std::vector<std::string> allowed_hosts, DenyFn on_deny);
  ~NetworkProxy();
  NetworkProxy(const NetworkProxy&) = delete;
  NetworkProxy& operator=(const NetworkProxy&) = delete;

  // Binds to 127.0.0.1:0 and starts accepting. Throws std::system_error.
  void start();
  void stop();
  int port() const { return port_; }
  // http://127.0.0.1:<port>
  std::string url() const;

 private:
  void accept_loop();
  void handle(int client);
  void register_fd(int fd);
  void unregister_fd(int fd);

  std::vector<std::string> allowed_;
  DenyFn on_deny_;
  int listen_fd_ = -1;
  int wake_pipe_[2] = {-1, -1};
  int port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread accept_thread_;
  std::mutex mutex_;
  std::condition_variable workers_done_;
  int workers_ = 0;
  std::set<int> active_fds_;
};

}  // namespace cpa::sandbox
