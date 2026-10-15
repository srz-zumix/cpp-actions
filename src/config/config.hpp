#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::config {

namespace fs = std::filesystem;

class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct EnvironmentConfig {
  std::string secrets_file;
  std::string vars_file;
  std::map<std::string, std::string> vars;
};

// User configuration (~/.config/cpp-actions/config.yml). It is never read from
// the repository unless given explicitly, because it relaxes the sandbox.
struct Config {
  std::vector<std::string> allow_hosts;
  std::vector<std::string> allow_read;
  std::vector<std::string> allow_write;
  std::optional<bool> allow_localhost;
  std::vector<std::string> pass_env;
  std::vector<std::string> host_labels;
  std::optional<int> parallel;
  std::string node;
  std::string cache_dir;
  std::string secrets_file;
  std::string vars_file;
  std::map<std::string, std::string> vars;
  std::map<std::string, EnvironmentConfig> environments;
};

// $XDG_CONFIG_HOME/cpp-actions/config.yml or ~/.config/cpp-actions/config.yml.
fs::path default_config_path();
Config load_config(const fs::path& path);

// Parses KEY=VALUE lines (dotenv): comments, `export` prefixes, single and
// double quotes (double quotes support \n, \t, \" and \\ and may span lines).
std::map<std::string, std::string> parse_dotenv(std::string_view text, std::string_view file = {});
std::map<std::string, std::string> load_dotenv(const fs::path& path);

// Expands a leading "~" to the home directory.
fs::path expand_user(std::string_view path);

// "NAME=VALUE" -> {NAME, VALUE}; "NAME" -> {NAME, nullopt}.
std::pair<std::string, std::optional<std::string>> split_assignment(std::string_view text);

}  // namespace cpa::config
