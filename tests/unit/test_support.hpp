#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <system_error>
#endif

#include "util/fs.hpp"
#include "util/process.hpp"
#include "util/strings.hpp"

namespace cpa_test {

namespace fs = std::filesystem;

inline void remove_retained_workspaces(const fs::path& run) {
  const auto jobs = run / "jobs";
  if (!fs::exists(jobs)) return;
  for (const auto& entry : fs::recursive_directory_iterator(jobs)) {
    if (entry.path().filename() != "work-path" || !entry.is_regular_file()) continue;
    const fs::path work(cpa::util::trim(cpa::util::read_file(entry.path())));
    const auto root = work.parent_path();
    if (work.filename() != "work" || root.filename().string().find("cpa-w-") != 0
        || root.parent_path() != cpa::util::real_path("/tmp"))
      throw std::runtime_error("Unexpected retained work directory: " + work.string());
    cpa::util::remove_tree(root);
  }
}

// Temporary directory removed at the end of the scope.
class TempDir {
 public:
  TempDir() : path_(cpa::util::make_temp_dir("cpa-test-")) {}
  ~TempDir() { cpa::util::remove_tree(path_); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }
  fs::path operator/(const std::string& rel) const { return path_ / rel; }

 private:
  fs::path path_;
};

#if defined(__APPLE__)
class UnixSocketListener {
 public:
  explicit UnixSocketListener(fs::path path) : path_(std::move(path)) {
    sockaddr_un address{};
    const auto name = path_.string();
    if (name.size() >= sizeof(address.sun_path)) throw std::invalid_argument("Unix socket path is too long");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ == -1) throw std::system_error(errno, std::generic_category(), "socket");
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      const int error = errno;
      ::close(fd_);
      throw std::system_error(error, std::generic_category(), "bind " + name);
    }
    if (::listen(fd_, 128) != 0) {
      const int error = errno;
      ::close(fd_);
      ::unlink(name.c_str());
      throw std::system_error(error, std::generic_category(), "listen " + name);
    }
  }
  ~UnixSocketListener() {
    ::close(fd_);
    ::unlink(path_.c_str());
  }
  UnixSocketListener(const UnixSocketListener&) = delete;
  UnixSocketListener& operator=(const UnixSocketListener&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
  int fd_ = -1;
};
#endif

// Environment for git commands of the tests (no user configuration).
inline cpa::util::Environment git_env() {
  auto env = cpa::util::current_environment();
  env["GIT_CONFIG_NOSYSTEM"] = "1";
  env["GIT_CONFIG_GLOBAL"] = "/dev/null";
  env["GIT_AUTHOR_NAME"] = "test";
  env["GIT_AUTHOR_EMAIL"] = "test@example.com";
  env["GIT_COMMITTER_NAME"] = "test";
  env["GIT_COMMITTER_EMAIL"] = "test@example.com";
  env["GIT_TERMINAL_PROMPT"] = "0";
  return env;
}

// Runs git and returns stdout without the trailing newline; throws on failure.
inline std::string git(const fs::path& dir, std::vector<std::string> args) {
  args.insert(args.begin(), {"git", "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false"});
  const auto env = git_env();
  auto r = cpa::util::capture(args, dir, &env);
  if (r.exit_code != 0) throw std::runtime_error("git failed: " + r.err);
  while (!r.out.empty() && r.out.back() == '\n') r.out.pop_back();
  return r.out;
}

// Creates a repository on branch "main" with one commit containing `files`.
inline void init_repo(const fs::path& dir, const std::vector<std::pair<std::string, std::string>>& files) {
  git(dir, {"init", "-q", "-b", "main"});
  for (const auto& [name, content] : files) cpa::util::write_file(dir / name, content);
  git(dir, {"add", "-A"});
  git(dir, {"commit", "-q", "-m", "initial"});
}

}  // namespace cpa_test
