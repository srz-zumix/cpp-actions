#include "sandbox/sandbox.hpp"

#include <unistd.h>

#include <stdexcept>

#include "util/fs.hpp"

namespace cpa::sandbox {

bool supported(std::string* reason) {
#if defined(__APPLE__)
  if (::access(kSandboxExec, X_OK) != 0) {
    if (reason != nullptr) *reason = std::string(kSandboxExec) + " is not available";
    return false;
  }
  return true;
#else
  if (reason != nullptr) *reason = "the Seatbelt sandbox is only available on macOS";
  return false;
#endif
}

Sandbox::Sandbox(SandboxConfig config, NetworkProxy::DenyFn on_deny) : config_(std::move(config)) {
  std::string reason;
  if (!supported(&reason)) throw std::runtime_error("sandbox unavailable: " + reason);
  if (config_.profile_file.empty()) throw std::invalid_argument("sandbox profile path must not be empty");
  config_.profile_file = fs::weakly_canonical(fs::absolute(config_.profile_file));
  const auto& profile = config_.profile_file;
  for (const auto& path : config_.write) {
    if (util::is_subpath(profile, fs::weakly_canonical(fs::absolute(path)))) {
      throw std::invalid_argument("sandbox profile must be outside writable directories");
    }
  }
  auto hosts = default_allowed_hosts();
  hosts.insert(hosts.end(), config_.allow_hosts.begin(), config_.allow_hosts.end());
  proxy_ = std::make_unique<NetworkProxy>(std::move(hosts), std::move(on_deny));
  proxy_->start();
  util::write_file(config_.profile_file, generate_profile(profile_options()));
}

ProfileOptions Sandbox::profile_options() const {
  ProfileOptions options;
  options.read = config_.read;
  options.write = config_.write;
  options.proxy_port = proxy_->port();
  options.allow_localhost = config_.allow_localhost;
  options.extra_rules = config_.extra_rules;
  return options;
}

Sandbox::~Sandbox() {
  if (proxy_) proxy_->stop();
}

std::vector<std::string> Sandbox::wrap(const std::vector<std::string>& argv, const std::vector<fs::path>& extra_read) {
  fs::path profile = config_.profile_file;
  if (!extra_read.empty()) {
    std::string key;
    for (const auto& p : extra_read) key += p.string() + "\n";
    std::lock_guard lock(mutex_);
    auto it = derived_.find(key);
    if (it == derived_.end()) {
      auto options = profile_options();
      options.read.insert(options.read.end(), extra_read.begin(), extra_read.end());
      const auto file = config_.profile_file.parent_path() /
                        (config_.profile_file.stem().string() + "-" + std::to_string(derived_.size() + 1) + ".sb");
      util::write_file(file, generate_profile(options));
      it = derived_.emplace(key, file).first;
    }
    profile = it->second;
  }
  std::vector<std::string> out = {kSandboxExec, "-f", profile.string()};
  out.insert(out.end(), argv.begin(), argv.end());
  return out;
}

std::map<std::string, std::string> Sandbox::environment() const {
  const auto url = proxy_->url();
  std::map<std::string, std::string> env = {
      {"HTTP_PROXY", url},  {"HTTPS_PROXY", url}, {"http_proxy", url},
      {"https_proxy", url}, {"ALL_PROXY", url},   {"all_proxy", url},
      {"NODE_USE_ENV_PROXY", "1"},
      {"NO_PROXY", ""}, {"no_proxy", ""},
  };
  if (config_.allow_localhost) {
    env["NO_PROXY"] = "localhost,127.0.0.1,::1";
    env["no_proxy"] = "localhost,127.0.0.1,::1";
  }
  return env;
}

}  // namespace cpa::sandbox
