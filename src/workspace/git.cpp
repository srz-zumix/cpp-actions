#include "workspace/git.hpp"

#include <algorithm>
#include <set>

#include "util/fs.hpp"
#include "util/process.hpp"
#include "util/strings.hpp"

namespace cpa::workspace {

namespace {

util::Environment git_environment() {
  auto env = util::current_environment();
  env["GIT_TERMINAL_PROMPT"] = "0";
  env["GIT_OPTIONAL_LOCKS"] = "0";
  env["LC_ALL"] = "C";
  return env;
}

std::optional<std::string> run_git(const fs::path& dir, const std::vector<std::string>& args, bool trim_newline) {
  std::vector<std::string> argv = {"git"};
  argv.insert(argv.end(), args.begin(), args.end());
  const auto env = git_environment();
  const auto r = util::capture(argv, dir, &env);
  if (r.exit_code != 0) return std::nullopt;
  std::string out = r.out;
  if (trim_newline) {
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  }
  return out;
}

std::vector<std::string> split_nul(std::string_view s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < s.size()) {
    auto end = s.find('\0', start);
    if (end == std::string_view::npos) end = s.size();
    if (end > start) out.emplace_back(s.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

std::string strip_git_suffix(std::string_view s) {
  while (!s.empty() && s.back() == '/') s.remove_suffix(1);
  if (s.ends_with(".git")) s.remove_suffix(4);
  return std::string(s);
}

std::optional<GitHubRepo> split_owner_repo(std::string_view path) {
  while (!path.empty() && path.front() == '/') path.remove_prefix(1);
  const auto parts = util::split(strip_git_suffix(path), '/');
  if (parts.size() != 2 || parts[0].empty() || parts[1].empty()) return std::nullopt;
  return GitHubRepo{parts[0], parts[1]};
}

}  // namespace

std::optional<GitHubRepo> parse_github_remote(std::string_view url) {
  url = util::trim(url);
  // scp-like syntax: [user@]github.com:owner/repo(.git)
  if (url.find("://") == std::string_view::npos) {
    const auto colon = url.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    auto host = url.substr(0, colon);
    if (const auto at = host.find('@'); at != std::string_view::npos) host = host.substr(at + 1);
    if (!util::iequals(host, "github.com")) return std::nullopt;
    return split_owner_repo(url.substr(colon + 1));
  }
  const auto scheme_end = url.find("://");
  const auto scheme = url.substr(0, scheme_end);
  if (!util::iequals(scheme, "https") && !util::iequals(scheme, "http") && !util::iequals(scheme, "ssh") &&
      !util::iequals(scheme, "git")) {
    return std::nullopt;
  }
  auto rest = url.substr(scheme_end + 3);
  const auto slash = rest.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  auto host = rest.substr(0, slash);
  if (const auto at = host.rfind('@'); at != std::string_view::npos) host = host.substr(at + 1);
  if (const auto colon = host.find(':'); colon != std::string_view::npos) host = host.substr(0, colon);
  if (!util::iequals(host, "github.com") && !util::iequals(host, "www.github.com")) return std::nullopt;
  return split_owner_repo(rest.substr(slash));
}

std::optional<std::string> git_output(const fs::path& dir, const std::vector<std::string>& args) {
  return run_git(dir, args, true);
}

bool git_ok(const fs::path& dir, const std::vector<std::string>& args) { return run_git(dir, args, false).has_value(); }

std::optional<fs::path> find_repo_root(const fs::path& start) {
  const auto out = git_output(start, {"rev-parse", "--show-toplevel"});
  if (!out || out->empty()) return std::nullopt;
  return util::real_path(*out);
}

GitInfo read_git_info(const fs::path& dir) {
  GitInfo info;
  const auto root = find_repo_root(dir);
  if (!root) {
    info.root = util::real_path(dir);
    info.default_branch = "main";
    return info;
  }
  info.is_repo = true;
  info.root = *root;
  const auto& r = info.root;
  if (auto sha = git_output(r, {"rev-parse", "--verify", "--quiet", "HEAD"})) info.head_sha = *sha;
  if (!info.head_sha.empty()) {
    if (auto msg = git_output(r, {"log", "-1", "--format=%s"})) info.head_message = *msg;
    if (auto tags = git_output(r, {"tag", "--points-at", "HEAD"})) {
      for (auto& t : util::split_lines(*tags)) {
        if (!t.empty()) info.tags.push_back(t);
      }
    }
  }
  if (auto b = git_output(r, {"symbolic-ref", "--quiet", "--short", "HEAD"})) info.branch = *b;
  if (auto u = git_output(r, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"})) {
    info.upstream = *u;
  }
  if (auto d = git_output(r, {"symbolic-ref", "--quiet", "--short", "refs/remotes/origin/HEAD"})) {
    std::string_view v = *d;
    if (v.starts_with("origin/")) v.remove_prefix(7);
    info.default_branch = std::string(v);
  }
  if (info.default_branch.empty()) {
    for (const char* candidate : {"main", "master"}) {
      if (git_ok(r, {"rev-parse", "--verify", "--quiet", std::string("refs/remotes/origin/") + candidate}) ||
          git_ok(r, {"rev-parse", "--verify", "--quiet", std::string("refs/heads/") + candidate})) {
        info.default_branch = candidate;
        break;
      }
    }
  }
  if (info.default_branch.empty()) info.default_branch = info.branch.empty() ? "main" : info.branch;
  if (auto url = git_output(r, {"config", "--get", "remote.origin.url"})) {
    info.remote_url = *url;
  } else if (auto remotes = git_output(r, {"remote"})) {
    const auto names = util::split_lines(*remotes);
    if (!names.empty() && !names[0].empty()) {
      if (auto first = git_output(r, {"config", "--get", "remote." + names[0] + ".url"})) info.remote_url = *first;
    }
  }
  if (!info.remote_url.empty()) info.github = parse_github_remote(info.remote_url);
  return info;
}

std::optional<std::vector<std::string>> changed_files_since(const fs::path& root, const std::string& base) {
  if (!git_ok(root, {"rev-parse", "--verify", "--quiet", base + "^{commit}"})) return std::nullopt;
  std::set<std::string> files;
  if (auto diff = run_git(root, {"diff", "--name-only", "-z", base, "--"}, false)) {
    for (auto& f : split_nul(*diff)) files.insert(std::move(f));
  } else {
    return std::nullopt;
  }
  if (auto untracked = run_git(root, {"ls-files", "-z", "--others", "--exclude-standard"}, false)) {
    for (auto& f : split_nul(*untracked)) files.insert(std::move(f));
  }
  return std::vector<std::string>(files.begin(), files.end());
}

std::vector<std::string> files_of_commit(const fs::path& root, const std::string& rev) {
  std::vector<std::string> files;
  if (auto out = run_git(root, {"diff-tree", "--no-commit-id", "--name-only", "-r", "-z", "--root", rev}, false)) {
    files = split_nul(*out);
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<std::string> list_worktree_files(const fs::path& root) {
  std::vector<std::string> files;
  if (auto out = run_git(root, {"ls-files", "-z", "--cached", "--others", "--exclude-standard"}, false)) {
    files = split_nul(*out);
  }
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  return files;
}

}  // namespace cpa::workspace
