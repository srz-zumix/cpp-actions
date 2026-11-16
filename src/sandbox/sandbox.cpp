#include "sandbox/sandbox.hpp"

#include <unistd.h>

#include <stdexcept>
#include <system_error>

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

Sandbox::Sandbox(SandboxConfig config, NetworkProxy::DenyFn on_deny, NetworkProxy::WarningFn on_warning)
    : config_(std::move(config)) {
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
  proxy_ = std::make_unique<NetworkProxy>(std::move(hosts), std::move(on_deny), std::move(on_warning),
                                        config_.allow_unity_hosts, config_.proxy_max_connections);
  proxy_->start();
  util::write_file(config_.profile_file, generate_profile(profile_options()));
}

ProfileOptions Sandbox::profile_options() const {
  ProfileOptions options;
  options.read = config_.read;
  options.write = config_.write;
  options.proxy_port = proxy_->port();
  options.allow_localhost = config_.allow_localhost;
  options.allow_xcode = config_.allow_xcode;
  options.allow_xcode_simulator = config_.allow_xcode_simulator;
  options.unity_home = config_.unity_home;
  options.unity_tmp = config_.unity_tmp;
  options.extra_rules = config_.extra_rules;
  return options;
}

Sandbox::~Sandbox() {
  if (proxy_) proxy_->stop();
}

std::vector<std::string> Sandbox::wrap(const std::vector<std::string>& argv, const std::vector<fs::path>& extra_read,
                                       bool allow_apple_security, bool allow_msbuild, bool allow_unity,
                                       const std::vector<fs::path>& scoped_read,
                                       const std::vector<fs::path>& scoped_write, bool allow_docker,
                                       bool allow_xcode) {
  fs::path profile = config_.profile_file;
  for (const auto& path : scoped_read) validate_scoped_path(path);
  for (const auto& path : scoped_write) {
    validate_scoped_path(path);
    if (util::is_subpath(profile, path) || util::is_subpath(path, profile.parent_path())) {
      throw std::invalid_argument("scoped writable path overlaps the sandbox profile directory");
    }
  }
  fs::path docker_socket;
  if (allow_docker) {
    validate_scoped_path(config_.docker_socket);
    std::error_code ec;
    docker_socket = fs::canonical(config_.docker_socket, ec);
    if (ec) {
      throw std::runtime_error("Cannot resolve Docker socket '" + config_.docker_socket.string() +
                               "': " + ec.message() + "; ensure Docker is running");
    }
    validate_scoped_path(docker_socket);
    const auto status = fs::status(docker_socket, ec);
    if (ec) throw std::runtime_error("Cannot inspect Docker socket '" + docker_socket.string() + "': " + ec.message());
    if (!fs::is_socket(status)) {
      throw std::runtime_error("Docker endpoint is not a Unix socket: " + docker_socket.string());
    }
  }
  if (!extra_read.empty() || allow_apple_security || allow_msbuild || allow_unity || allow_docker || allow_xcode ||
      !scoped_read.empty() || !scoped_write.empty()) {
    const DerivedProfileKey key{allow_apple_security, allow_msbuild, allow_unity, allow_xcode, docker_socket,
                                extra_read,           scoped_read,   scoped_write};
    std::lock_guard lock(mutex_);
    if (allow_docker) {
      if (docker_socket_.empty()) {
        docker_socket_ = docker_socket;
      } else if (docker_socket_ != docker_socket) {
        throw std::runtime_error("Docker socket target changed during this job: " + config_.docker_socket.string());
      }
    }
    auto it = derived_.find(key);
    if (it == derived_.end()) {
      auto options = profile_options();
      options.allow_apple_security = allow_apple_security;
      options.allow_msbuild = allow_msbuild;
      options.allow_unity = allow_unity;
      options.allow_xcode = options.allow_xcode || allow_xcode;
      options.docker_socket = docker_socket;
      options.read.insert(options.read.end(), extra_read.begin(), extra_read.end());
      options.scoped_read = scoped_read;
      options.scoped_write = scoped_write;
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

std::string Sandbox::java_proxy_options() const {
  const auto port = std::to_string(proxy_->port());
  return "-Dhttp.proxyHost=127.0.0.1 -Dhttp.proxyPort=" + port +
         " -Dhttps.proxyHost=127.0.0.1 -Dhttps.proxyPort=" + port +
         " -Dhttp.nonProxyHosts=" + (config_.allow_localhost ? "localhost|127.0.0.1|[::1]" : "");
}

}  // namespace cpa::sandbox
