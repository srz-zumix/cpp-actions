#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "sandbox/profile.hpp"
#include "sandbox/proxy.hpp"

namespace cpa::sandbox {

constexpr const char* kSandboxExec = "/usr/bin/sandbox-exec";

// Whether Seatbelt can be used on this host (macOS with sandbox-exec).
bool supported(std::string* reason = nullptr);

struct SandboxConfig {
  std::vector<fs::path> read;
  std::vector<fs::path> write;
  std::vector<std::string> allow_hosts;  // in addition to default_allowed_hosts()
  bool allow_localhost = false;
  bool allow_xcode = false;  // Xcode temporary/cache grants for every invocation
  bool allow_xcode_simulator = false;
  bool allow_unity_hosts = false;
  int proxy_max_connections = kDefaultProxyConnections;
  std::string extra_rules;
  fs::path profile_file;  // where the generated profile is written
  fs::path unity_home;    // job HOME, fixed for this sandbox's lifetime
  fs::path unity_tmp;     // short job-specific directory, granted only to Unity invocations
  fs::path docker_socket;
};

// A sandbox for the steps of one job: the generated profile and the network
// proxy, which runs until the object is destroyed.
class Sandbox {
 public:
  Sandbox(SandboxConfig config, NetworkProxy::DenyFn on_deny, NetworkProxy::WarningFn on_warning = {});
  ~Sandbox();

  // {sandbox-exec, -f, profile, argv...}. `extra_read` directories are
  // added to a derived profile (e.g. the Node.js installation). Apple security
  // service grants apply only to this invocation and its child processes.
  // Scoped paths are prevalidated and must not be resolved again when used.
  std::vector<std::string> wrap(const std::vector<std::string>& argv, const std::vector<fs::path>& extra_read = {},
                                bool allow_apple_security = false, bool allow_msbuild = false, bool allow_unity = false,
                                const std::vector<fs::path>& scoped_read = {},
                                const std::vector<fs::path>& scoped_write = {}, bool allow_docker = false,
                                bool allow_xcode = false);
  // Proxy variables for the step environment.
  std::map<std::string, std::string> environment() const;
  std::string java_proxy_options() const;
  const fs::path& profile_file() const { return config_.profile_file; }

 private:
  ProfileOptions profile_options() const;

  SandboxConfig config_;
  std::unique_ptr<NetworkProxy> proxy_;
  std::mutex mutex_;
  fs::path docker_socket_;
  using DerivedProfileKey =
      std::tuple<bool, bool, bool, bool, fs::path, std::vector<fs::path>, std::vector<fs::path>,
                 std::vector<fs::path>>;
  std::map<DerivedProfileKey, fs::path> derived_;
};

}  // namespace cpa::sandbox
