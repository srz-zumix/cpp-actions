#pragma once

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::util {

using Environment = std::map<std::string, std::string>;

Environment current_environment();

struct ProcessSpec {
  // argv[0] is resolved with the PATH of `env` when it does not contain '/'.
  std::vector<std::string> argv;
  Environment env;
  std::filesystem::path cwd;
  std::optional<std::chrono::milliseconds> timeout;
  // Kill immediately on timeout instead of using the normal signal grace period.
  bool hard_timeout = false;
  const std::atomic<bool>* cancel = nullptr;
  const std::atomic<bool>* force_cancel = nullptr;
  // Called for each complete output line (without the line terminator).
  std::function<void(std::string_view line, bool is_stderr)> on_line;
  // Called for raw output chunks (used when on_line is not set).
  std::function<void(std::string_view chunk, bool is_stderr)> on_output;
  // Called once the child has been spawned.
  std::function<void(pid_t pgid)> on_spawn;
  // Grace period for draining the pipes after the main process exited while
  // background processes still hold them open.
  std::chrono::milliseconds drain_timeout{200};
};

struct ProcessResult {
  int exit_code = -1;  // exit status, or 128 + signal number
  int term_signal = 0;
  bool timed_out = false;
  bool cancelled = false;
  bool spawn_failed = false;
  std::string error;
};

ProcessResult run_process(const ProcessSpec& spec);

struct CaptureResult {
  int exit_code = -1;
  std::string out;
  std::string err;
};

// Runs a command and captures its output (inherits the current environment
// unless `env` is given).
CaptureResult capture(const std::vector<std::string>& argv, const std::filesystem::path& cwd = {},
                      const Environment* env = nullptr);

// Sends SIGKILL to a process group (ignores errors).
void kill_process_group(pid_t pgid);
bool process_group_alive(pid_t pgid);

}  // namespace cpa::util
