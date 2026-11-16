#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "workflow/model.hpp"

namespace cpa::config {

namespace fs = std::filesystem;

class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct EnvironmentConfig {
  std::optional<std::string> secrets_file;
  std::optional<std::string> vars_file;
  std::map<std::string, std::string> vars;
};

struct SetupSteps {
  std::shared_ptr<yaml::Document> document;
  std::vector<workflow::Step> steps;
};

struct Config {
  std::vector<SetupSteps> setup_steps;
  std::vector<std::string> allow_hosts;
  std::vector<std::string> allow_apple_security_for;
  std::vector<std::string> allow_docker_for;
  std::vector<std::string> allow_docker_step;
  std::vector<std::string> allow_msbuild_for;
  std::vector<std::string> allow_msbuild_step;
  std::vector<std::string> allow_unity_for;
  std::vector<std::string> allow_unity_step;
  std::vector<std::string> allow_xcode_for;
  std::vector<std::string> allow_read;
  std::vector<std::string> allow_write;
  std::optional<bool> allow_localhost;
  std::optional<bool> allow_xcode;
  std::optional<bool> allow_xcode_simulator;
  std::optional<bool> allow_unity_hosts;
  std::optional<bool> ignore_interactive_shell;
  std::optional<bool> inherit_unity_editor_prefs;
  std::optional<int> proxy_max_connections;
  std::vector<std::string> pass_env;
  std::vector<std::string> host_labels;
  std::optional<int> parallel;
  std::optional<std::string> node;
  std::optional<std::string> cache_dir;
  std::optional<std::string> secrets_file;
  std::optional<std::string> vars_file;
  std::map<std::string, std::string> vars;
  std::map<std::string, EnvironmentConfig> environments;
};

// $XDG_CONFIG_HOME/cpp-actions/config.yml or ~/.config/cpp-actions/config.yml.
fs::path default_config_path();
Config load_config(const fs::path& path);
std::vector<fs::path> configuration_paths(const fs::path& repo_root, const fs::path& explicit_path = {});
// Resolves paths against their source files, then merges in order.
Config load_configs(const std::vector<fs::path>& paths);

// Parses KEY=VALUE lines (dotenv): comments, `export` prefixes, single and
// double quotes (double quotes support \n, \t, \" and \\ and may span lines).
std::map<std::string, std::string> parse_dotenv(std::string_view text, std::string_view file = {});
std::map<std::string, std::string> load_dotenv(const fs::path& path);

// Expands a leading "~" to the home directory.
fs::path expand_user(std::string_view path);

// "NAME=VALUE" -> {NAME, VALUE}; "NAME" -> {NAME, nullopt}.
std::pair<std::string, std::optional<std::string>> split_assignment(std::string_view text);

}  // namespace cpa::config
