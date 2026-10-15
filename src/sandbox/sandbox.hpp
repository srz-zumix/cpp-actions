#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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
  std::string extra_rules;
  fs::path profile_file;  // where the generated profile is written
};

// A sandbox for the steps of one job: the generated profile and the network
// proxy, which runs until the object is destroyed.
class Sandbox {
 public:
  Sandbox(SandboxConfig config, NetworkProxy::DenyFn on_deny);
  ~Sandbox();

  // {sandbox-exec, -f, profile, argv...}. `extra_read` directories are
  // added to a derived profile (e.g. the Node.js installation).
  std::vector<std::string> wrap(const std::vector<std::string>& argv, const std::vector<fs::path>& extra_read = {});
  // Proxy variables for the step environment.
  std::map<std::string, std::string> environment() const;
  const fs::path& profile_file() const { return config_.profile_file; }

 private:
  ProfileOptions profile_options() const;

  SandboxConfig config_;
  std::unique_ptr<NetworkProxy> proxy_;
  std::mutex mutex_;
  std::map<std::string, fs::path> derived_;
};

}  // namespace cpa::sandbox
