#include "util/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "util/fs.hpp"

extern char** environ;

namespace cpa::util {

Environment current_environment() {
  Environment env;
  for (char** e = environ; e && *e; ++e) {
    std::string_view kv(*e);
    const auto pos = kv.find('=');
    if (pos == std::string_view::npos || pos == 0) continue;
    env.emplace(std::string(kv.substr(0, pos)), std::string(kv.substr(pos + 1)));
  }
  return env;
}

namespace {

bool make_pipe(int fds[2]) {
#if defined(__linux__)
  return ::pipe2(fds, O_CLOEXEC) == 0;
#else
  if (::pipe(fds) != 0) return false;
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return true;
#endif
}

class LineSplitter {
 public:
  template <class Emit>
  void feed(std::string_view data, Emit&& emit) {
    buf_.append(data);
    std::size_t start = 0;
    while (true) {
      const auto pos = buf_.find('\n', start);
      if (pos == std::string::npos) break;
      std::string_view line(buf_.data() + start, pos - start);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      emit(line);
      start = pos + 1;
    }
    buf_.erase(0, start);
    if (buf_.size() > (1u << 20)) {  // guard against unbounded lines
      emit(std::string_view(buf_));
      buf_.clear();
    }
  }
  template <class Emit>
  void flush(Emit&& emit) {
    if (!buf_.empty()) {
      std::string_view line(buf_);
      if (line.back() == '\r') line.remove_suffix(1);
      emit(line);
      buf_.clear();
    }
  }

 private:
  std::string buf_;
};

}  // namespace

ProcessResult run_process(const ProcessSpec& spec) {
  using clock = std::chrono::steady_clock;
  ProcessResult result;
  if (spec.argv.empty()) {
    result.spawn_failed = true;
    result.error = "empty command";
    return result;
  }

  std::string exe = spec.argv[0];
  if (exe.find('/') == std::string::npos) {
    const auto it = spec.env.find("PATH");
    const char* host_path = std::getenv("PATH");
    const std::string path = it != spec.env.end() ? it->second : (host_path ? host_path : "/usr/bin:/bin");
    const auto found = find_executable(exe, path);
    if (!found) {
      result.spawn_failed = true;
      result.exit_code = 127;
      result.error = "command not found: " + exe;
      return result;
    }
    exe = found->string();
  }

  std::vector<std::string> env_strings;
  env_strings.reserve(spec.env.size());
  for (const auto& [k, v] : spec.env) env_strings.push_back(k + "=" + v);
  std::vector<char*> envp;
  envp.reserve(env_strings.size() + 1);
  for (auto& s : env_strings) envp.push_back(s.data());
  envp.push_back(nullptr);
  std::vector<std::string> args = spec.argv;
  std::vector<char*> argvp;
  argvp.reserve(args.size() + 1);
  for (auto& s : args) argvp.push_back(s.data());
  argvp.push_back(nullptr);

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (!make_pipe(out_pipe) || !make_pipe(err_pipe)) {
    result.spawn_failed = true;
    result.error = std::string("pipe: ") + std::strerror(errno);
    for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
      if (fd >= 0) ::close(fd);
    }
    return result;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
  if (!spec.cwd.empty()) {
#if defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
    // macOS 26 deprecates the _np variant in favor of the POSIX.1-2024 function
    // (which is only available from macOS 26).
    posix_spawn_file_actions_addchdir(&actions, spec.cwd.c_str());
#else
    posix_spawn_file_actions_addchdir_np(&actions, spec.cwd.c_str());
#endif
  }

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&attr, flags);
  posix_spawnattr_setpgroup(&attr, 0);
  sigset_t mask;
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
  sigset_t defaults;
  sigemptyset(&defaults);
  for (int sig : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCHLD, SIGUSR1, SIGUSR2, SIGALRM}) {
    sigaddset(&defaults, sig);
  }
  posix_spawnattr_setsigdefault(&attr, &defaults);

  pid_t pid = -1;
  const int rc = ::posix_spawn(&pid, exe.c_str(), &actions, &attr, argvp.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  if (rc != 0) {
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    result.spawn_failed = true;
    result.exit_code = 127;
    result.error = "failed to start '" + exe + "': " + std::strerror(rc);
    return result;
  }
  if (spec.on_spawn) spec.on_spawn(pid);

  int fds[2] = {out_pipe[0], err_pipe[0]};
  bool open[2] = {true, true};
  LineSplitter splitters[2];
  auto deliver = [&](int idx, std::string_view data) {
    const bool is_err = idx == 1;
    if (spec.on_line) {
      splitters[idx].feed(data, [&](std::string_view line) { spec.on_line(line, is_err); });
    } else if (spec.on_output) {
      spec.on_output(data, is_err);
    }
  };
  auto close_stream = [&](int idx) {
    if (!open[idx]) return;
    open[idx] = false;
    ::close(fds[idx]);
    if (spec.on_line) {
      const bool is_err = idx == 1;
      splitters[idx].flush([&](std::string_view line) { spec.on_line(line, is_err); });
    }
  };

  const auto start = clock::now();
  std::optional<clock::time_point> deadline;
  if (spec.timeout) deadline = start + *spec.timeout;
  int kill_stage = 0;
  clock::time_point next_kill{};
  bool exited = false;
  int status = 0;
  clock::time_point drain_deadline{};
  char buf[1 << 16];

  while (true) {
    const auto now = clock::now();
    if (!exited) {
      const pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        exited = true;
        drain_deadline = now + spec.drain_timeout;
      }
    }
    if (exited && !open[0] && !open[1]) break;
    if (exited && now >= drain_deadline) break;

    if (!exited) {
      bool want_kill = false;
      if (spec.force_cancel && spec.force_cancel->load()) {
        if (!result.timed_out) result.cancelled = true;
        ::killpg(pid, SIGKILL);
        kill_stage = 3;
      }
      if (spec.cancel && spec.cancel->load()) {
        if (!result.timed_out) result.cancelled = true;
        want_kill = true;
      }
      if (deadline && now >= *deadline) {
        if (!result.cancelled) result.timed_out = true;
        if (spec.hard_timeout) {
          ::killpg(pid, SIGKILL);
          kill_stage = 3;
        } else {
          want_kill = true;
        }
      }
      if (want_kill) {
        if (kill_stage == 0) {
          ::killpg(pid, SIGINT);
          kill_stage = 1;
          next_kill = now + std::chrono::milliseconds(7500);
        } else if (kill_stage == 1 && now >= next_kill) {
          ::killpg(pid, SIGTERM);
          kill_stage = 2;
          next_kill = now + std::chrono::milliseconds(2500);
        } else if (kill_stage == 2 && now >= next_kill) {
          ::killpg(pid, SIGKILL);
          kill_stage = 3;
        }
      }
    }

    pollfd pfds[2];
    int idx_map[2];
    nfds_t n = 0;
    for (int i = 0; i < 2; ++i) {
      if (open[i]) {
        pfds[n].fd = fds[i];
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        idx_map[n] = i;
        ++n;
      }
    }
    int wait_ms = n == 0 ? 10 : 100;
    if (exited) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(drain_deadline - now).count();
      wait_ms = static_cast<int>(left < wait_ms ? (left < 0 ? 0 : left) : wait_ms);
    }
    const int pr = ::poll(n ? pfds : nullptr, n, wait_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (nfds_t i = 0; i < n; ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
      const int idx = idx_map[i];
      const auto r = ::read(fds[idx], buf, sizeof(buf));
      if (r > 0) {
        deliver(idx, std::string_view(buf, static_cast<std::size_t>(r)));
      } else if (r == 0 || (errno != EINTR && errno != EAGAIN)) {
        close_stream(idx);
      }
    }
  }
  close_stream(0);
  close_stream(1);
  if (!exited) ::waitpid(pid, &status, 0);
  if (result.timed_out || result.cancelled) ::killpg(pid, SIGKILL);

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.term_signal = WTERMSIG(status);
    result.exit_code = 128 + result.term_signal;
  }
  return result;
}

CaptureResult capture(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                      const Environment* env) {
  ProcessSpec spec;
  spec.argv = argv;
  spec.cwd = cwd;
  spec.env = env ? *env : current_environment();
  CaptureResult out;
  spec.on_output = [&](std::string_view chunk, bool is_err) { (is_err ? out.err : out.out).append(chunk); };
  const auto r = run_process(spec);
  out.exit_code = r.exit_code;
  if (r.spawn_failed) out.err += r.error;
  return out;
}

void kill_process_group(pid_t pgid) {
  if (pgid > 0) ::killpg(pgid, SIGKILL);
}

bool process_group_alive(pid_t pgid) { return pgid > 0 && ::killpg(pgid, 0) == 0; }

}  // namespace cpa::util
