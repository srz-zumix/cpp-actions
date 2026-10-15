#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "util/fs.hpp"
#include "util/process.hpp"

namespace cpa_test {

namespace fs = std::filesystem;

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
