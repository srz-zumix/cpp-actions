#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpa::engine {

// A workflow command written to stdout/stderr ("::name k=v,k=v::message" or
// the legacy "##[name k=v;]message").
struct WorkflowCommand {
  std::string name;
  std::map<std::string, std::string> properties;  // unescaped
  std::string message;                            // unescaped
};

// Returns nullopt when the line is not a known workflow command.
std::optional<WorkflowCommand> parse_workflow_command(std::string_view line);

std::string unescape_command_data(std::string_view text);
std::string unescape_command_property(std::string_view text);
std::string escape_command_data(std::string_view text);
std::string escape_command_property(std::string_view text);

class FileCommandError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Reads a bounded regular command file without following symlinks in its path.
std::string read_command_file(const std::filesystem::path& path);

// Parses GITHUB_ENV / GITHUB_OUTPUT / GITHUB_STATE contents ("NAME=VALUE" and
// "NAME<<DELIMITER" heredocs). Throws FileCommandError on invalid content.
std::vector<std::pair<std::string, std::string>> parse_key_value_file(std::string_view content);

// GITHUB_PATH entries (non-blank lines).
std::vector<std::string> parse_path_file(std::string_view content);

}  // namespace cpa::engine
