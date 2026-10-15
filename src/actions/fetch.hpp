#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::actions {

namespace fs = std::filesystem;

using LogFn = std::function<void(std::string_view)>;

// True for a full 40 character commit SHA.
bool is_commit_sha(std::string_view ref);

// Picks the commit of `ref` from "git ls-remote" output: a peeled tag, then a
// tag, then a branch.
std::optional<std::string> pick_ls_remote_sha(std::string_view output, std::string_view ref);

// Downloads GitHub repositories (actions and reusable workflows) into a cache
// shared by all runs: <root>/<owner>/<repo>/<sha>. Downloads happen outside
// of the sandbox and only from GitHub.
class RepositoryCache {
 public:
  RepositoryCache(fs::path root, std::string token);

  struct Entry {
    fs::path dir;
    std::string sha;
  };
  // Resolves owner/repo@ref and returns the extracted tree. Throws ActionError.
  Entry fetch(const std::string& owner, const std::string& repo, const std::string& ref, const LogFn& log);

 private:
  std::string resolve_sha(const std::string& owner, const std::string& repo, const std::string& ref,
                          const LogFn& log);

  fs::path root_;
  std::string token_;
  std::mutex mutex_;
};

// Provides the Node.js runtime for JavaScript actions: an explicit binary, a
// cached or downloaded build of actions/node-versions, or the host `node`.
class NodeProvider {
 public:
  NodeProvider(fs::path explicit_node, fs::path cache_root);
  // Throws ActionError when no Node.js is available.
  fs::path get(int major, const LogFn& warn);

 private:
  std::optional<fs::path> download(int major, const LogFn& warn);

  fs::path explicit_node_;
  fs::path cache_root_;
  std::mutex mutex_;
  std::map<int, fs::path> resolved_;
};

// Runs a helper command (git, curl, tar) with the host environment plus
// `extra_env`; throws ActionError with the stderr output on failure.
std::string run_tool(const std::vector<std::string>& argv, const fs::path& cwd = {},
                     const std::map<std::string, std::string>& extra_env = {});

// Environment that makes git send the token to github.com (not visible in
// the process arguments).
std::map<std::string, std::string> git_token_env(const std::string& token);

// Writes a curl config file (0600) with the authorization header.
fs::path write_curl_auth_config(const fs::path& dir, const std::string& token);

}  // namespace cpa::actions
