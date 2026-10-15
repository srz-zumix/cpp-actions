#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::workspace {

namespace fs = std::filesystem;

struct GitHubRepo {
  std::string owner;
  std::string name;
  std::string full_name() const { return owner + "/" + name; }
};

// Parses GitHub remote URLs (https://github.com/o/r.git, git@github.com:o/r.git,
// ssh://git@github.com/o/r). Returns nullopt for other hosts.
std::optional<GitHubRepo> parse_github_remote(std::string_view url);

struct GitInfo {
  bool is_repo = false;
  fs::path root;                  // repository root (or the directory itself)
  std::string head_sha;           // empty when the repository has no commit
  std::string head_message;       // subject of HEAD
  std::string branch;             // current branch, empty when detached
  std::vector<std::string> tags;  // tags pointing at HEAD
  std::string upstream;           // e.g. "origin/main", empty when unknown
  std::string default_branch;     // from origin/HEAD, "main" when unknown
  std::string remote_url;         // origin
  std::optional<GitHubRepo> github;
};

// Runs git in `dir` and returns stdout with the trailing newline removed
// (nullopt when git fails or is not installed).
std::optional<std::string> git_output(const fs::path& dir, const std::vector<std::string>& args);
// Runs git and returns whether it succeeded (output is discarded).
bool git_ok(const fs::path& dir, const std::vector<std::string>& args);

std::optional<fs::path> find_repo_root(const fs::path& start);
GitInfo read_git_info(const fs::path& dir);

// Files (relative, '/' separated) that differ between `base` and the working
// tree, including untracked files that are not ignored. Returns nullopt when
// `base` cannot be resolved.
std::optional<std::vector<std::string>> changed_files_since(const fs::path& root, const std::string& base);
// Files changed by the commit `rev` (all files for a root commit).
std::vector<std::string> files_of_commit(const fs::path& root, const std::string& rev);
// Tracked files and untracked files that are not ignored ("git ls-files").
std::vector<std::string> list_worktree_files(const fs::path& root);

}  // namespace cpa::workspace
