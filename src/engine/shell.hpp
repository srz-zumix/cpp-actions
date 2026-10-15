#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::engine {

class ShellError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// How a `run:` script is executed.
struct Shell {
  std::string display;            // e.g. "/bin/bash -e {0}"
  std::string program;            // resolved executable path
  std::vector<std::string> args;  // arguments, "{0}" = script path
  std::string extension;          // script file extension (".sh")
  bool powershell = false;

  std::vector<std::string> argv(const std::filesystem::path& script) const;
  // Script contents (adds the error handling prologue/epilogue of pwsh).
  std::string script(std::string_view body) const;
};

// Resolves a `shell:` value (empty = default) with the PATH of the step.
Shell resolve_shell(std::string_view shell, std::string_view path_env);

}  // namespace cpa::engine
