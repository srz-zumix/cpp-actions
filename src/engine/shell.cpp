#include "engine/shell.hpp"

#include "util/fs.hpp"
#include "util/strings.hpp"

namespace cpa::engine {

namespace {

struct Builtin {
  std::string_view name;
  std::string_view args;
  std::string_view extension;
};

constexpr Builtin kBuiltins[] = {
    {"bash", "--noprofile --norc -eo pipefail {0}", ".sh"},
    {"sh", "-e {0}", ".sh"},
    {"python", "{0}", ".py"},
    {"pwsh", "-command \". '{0}'\"", ".ps1"},
    {"powershell", "-command \". '{0}'\"", ".ps1"},
};

const Builtin* find_builtin(std::string_view name) {
  for (const auto& b : kBuiltins) {
    if (b.name == name) return &b;
  }
  return nullptr;
}

std::string locate(std::string_view program, std::string_view path_env) {
  if (program.find('/') != std::string_view::npos) {
    std::error_code ec;
    if (std::filesystem::exists(std::string(program), ec)) return std::string(program);
  } else if (auto found = util::find_executable(program, path_env)) {
    return found->string();
  }
  throw ShellError("Unable to locate executable file: " + std::string(program) +
                   ". Please verify either the file path exists or the file can be found within a directory "
                   "specified by the PATH environment variable.");
}

std::string base_name(std::string_view program) {
  const auto slash = program.rfind('/');
  return std::string(slash == std::string_view::npos ? program : program.substr(slash + 1));
}

}  // namespace

std::vector<std::string> Shell::argv(const std::filesystem::path& script_path) const {
  std::vector<std::string> out{program};
  for (const auto& a : args) out.push_back(util::replace_all(a, "{0}", script_path.string()));
  return out;
}

bool Shell::ignore_interactive() {
  const auto name = base_name(program);
  if (name != "bash" && name != "sh") return false;
  std::vector<std::string> filtered;
  bool options = true;
  bool operand = false;
  bool changed = false;
  for (const auto& arg : args) {
    if (!options || operand) {
      filtered.push_back(arg);
      operand = false;
      continue;
    }
    if (arg.empty() || arg == "-" || arg == "+" || arg == "--" ||
        (arg.front() != '-' && arg.front() != '+')) {
      options = false;
      filtered.push_back(arg);
      continue;
    }
    if (arg == "--interactive") {
      changed = true;
      continue;
    }
    if (arg.starts_with("--")) {
      operand = arg == "--rcfile" || arg == "--init-file";
      filtered.push_back(arg);
      continue;
    }
    std::string flags(1, arg.front());
    for (std::size_t i = 1; i < arg.size(); ++i) {
      const char flag = arg[i];
      if (flag == 'i' && arg.front() == '-') {
        changed = true;
      } else {
        flags += flag;
      }
      // Option names and command strings are operands, not shell switches.
      if (flag == 'o' || flag == 'O') operand = true;
      if (flag == 'c') options = false;
    }
    if (flags.size() > 1) filtered.push_back(std::move(flags));
  }
  if (!changed) return false;
  args = std::move(filtered);
  display = util::shell_quote(program);
  for (const auto& arg : args) display += " " + util::shell_quote(arg);
  return true;
}

std::string Shell::script(std::string_view body) const {
  if (!powershell) return std::string(body);
  return "$ErrorActionPreference = 'stop'\n" + std::string(body) +
         "\nif ((Test-Path -LiteralPath variable:\\LASTEXITCODE)) { exit $LASTEXITCODE }";
}

Shell resolve_shell(std::string_view shell, std::string_view path_env) {
  Shell s;
  const auto trimmed = util::trim(shell);
  if (trimmed.empty()) {
    // Default: bash -e {0}, or sh -e {0} when bash is not available.
    if (auto bash = util::find_executable("bash", path_env)) {
      s.program = bash->string();
    } else {
      s.program = locate("sh", path_env);
    }
    s.args = {"-e", "{0}"};
    s.extension = ".sh";
    s.display = s.program + " -e {0}";
    return s;
  }
  const auto space = trimmed.find_first_of(" \t");
  const std::string command(trimmed.substr(0, space));
  const std::string arg_format(space == std::string_view::npos ? std::string_view{} : util::trim(trimmed.substr(space)));
  if (command == "cmd") throw ShellError("The 'cmd' shell is only supported on Windows");
  const Builtin* builtin = find_builtin(base_name(command));
  std::string args = arg_format;
  if (args.empty()) {
    if (builtin == nullptr) {
      throw ShellError("Invalid shell option '" + std::string(trimmed) +
                       "'. The shell must be bash, sh, python, pwsh, powershell or a command line containing '{0}'");
    }
    args = std::string(builtin->args);
  } else if (args.find("{0}") == std::string::npos) {
    throw ShellError("Invalid shell option '" + std::string(trimmed) +
                     "'. A custom shell must contain '{0}' (the script path)");
  }
  if (command == "python" && !util::find_executable("python", path_env)) {
    // Local machines often only have python3.
    s.program = locate("python3", path_env);
  } else {
    s.program = locate(command, path_env);
  }
  s.args = util::split_command_line(args);
  s.extension = builtin != nullptr ? std::string(builtin->extension) : std::string();
  s.powershell = builtin != nullptr && builtin->extension == ".ps1";
  s.display = s.program + " " + args;
  return s;
}

}  // namespace cpa::engine
