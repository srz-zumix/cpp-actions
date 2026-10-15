#include "actions/fetch.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <cctype>
#include <nlohmann/json.hpp>

#include "actions/action.hpp"
#include "util/fs.hpp"
#include "util/process.hpp"
#include "util/strings.hpp"

namespace cpa::actions {

namespace {

std::string base64(std::string_view in) {
  static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  while (i + 2 < in.size()) {
    const auto n = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                   static_cast<unsigned char>(in[i + 2]);
    out += kTable[(n >> 18) & 63];
    out += kTable[(n >> 12) & 63];
    out += kTable[(n >> 6) & 63];
    out += kTable[n & 63];
    i += 3;
  }
  if (i + 1 == in.size()) {
    const auto n = static_cast<unsigned char>(in[i]) << 16;
    out += kTable[(n >> 18) & 63];
    out += kTable[(n >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const auto n = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
    out += kTable[(n >> 18) & 63];
    out += kTable[(n >> 12) & 63];
    out += kTable[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

std::string sanitize_ref(std::string_view ref) {
  std::string out;
  for (char c : ref) {
    out += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_' ? c : '_';
  }
  return out;
}

std::string node_platform() {
#if defined(__APPLE__)
  return "darwin";
#else
  return "linux";
#endif
}

std::string node_arch() {
  struct utsname u{};
  if (::uname(&u) == 0) {
    const std::string m = u.machine;
    if (m == "arm64" || m == "aarch64") return "arm64";
  }
  return "x64";
}

std::optional<fs::path> find_node_binary(const fs::path& root) {
  for (const auto& candidate : {root / "bin" / "node", root / "node"}) {
    if (::access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (!entry.is_directory(ec)) continue;
    const auto candidate = entry.path() / "bin" / "node";
    if (::access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return std::nullopt;
}

int host_node_major(const fs::path& node) {
  const auto r = util::capture({node.string(), "--version"});
  if (r.exit_code != 0) return 0;
  std::string_view v = util::trim(r.out);
  if (v.starts_with("v")) v.remove_prefix(1);
  const auto dot = v.find('.');
  return static_cast<int>(util::parse_int(v.substr(0, dot)).value_or(0));
}

}  // namespace

bool is_commit_sha(std::string_view ref) {
  if (ref.size() != 40) return false;
  for (char c : ref) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

std::optional<std::string> pick_ls_remote_sha(std::string_view output, std::string_view ref) {
  std::map<std::string, std::string> refs;
  for (const auto& line : util::split_lines(output)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    refs[line.substr(tab + 1)] = line.substr(0, tab);
  }
  const std::string r(ref);
  for (const auto& name : {"refs/tags/" + r + "^{}", "refs/tags/" + r, "refs/heads/" + r, r}) {
    auto it = refs.find(name);
    if (it != refs.end() && is_commit_sha(it->second)) return it->second;
  }
  return std::nullopt;
}

std::string run_tool(const std::vector<std::string>& argv, const fs::path& cwd,
                     const std::map<std::string, std::string>& extra_env) {
  auto env = util::current_environment();
  env["GIT_TERMINAL_PROMPT"] = "0";
  for (const auto& [k, v] : extra_env) env[k] = v;
  const auto r = util::capture(argv, cwd, &env);
  if (r.exit_code != 0) {
    std::string message = "'" + argv.front() + "' failed (exit code " + std::to_string(r.exit_code) + ")";
    const auto err = std::string(util::trim(r.err));
    if (!err.empty()) message += ": " + err;
    throw ActionError(message);
  }
  return r.out;
}

std::map<std::string, std::string> git_token_env(const std::string& token) {
  if (token.empty()) return {};
  return {{"GIT_CONFIG_COUNT", "1"},
          {"GIT_CONFIG_KEY_0", "http.https://github.com/.extraheader"},
          {"GIT_CONFIG_VALUE_0", "AUTHORIZATION: basic " + base64("x-access-token:" + token)}};
}

fs::path write_curl_auth_config(const fs::path& dir, const std::string& token) {
  const auto path = dir / "curl-auth.conf";
  util::write_file(path, "header = \"Authorization: Bearer " + token + "\"\n");
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
  return path;
}

RepositoryCache::RepositoryCache(fs::path root, std::string token) : root_(std::move(root)), token_(std::move(token)) {}

std::string RepositoryCache::resolve_sha(const std::string& owner, const std::string& repo, const std::string& ref,
                                         const LogFn& log) {
  if (is_commit_sha(ref)) return util::to_lower(ref);
  const auto record = root_ / owner / repo / "refs" / sanitize_ref(ref);
  const std::string url = "https://github.com/" + owner + "/" + repo;
  try {
    const auto out = run_tool({"git", "ls-remote", "--", url, "refs/tags/" + ref, "refs/tags/" + ref + "^{}",
                               "refs/heads/" + ref},
                              {}, git_token_env(token_));
    auto sha = pick_ls_remote_sha(out, ref);
    if (!sha) {
      throw ActionError("Unable to resolve action '" + owner + "/" + repo + "@" + ref +
                        "', unable to find version '" + ref + "'");
    }
    fs::create_directories(record.parent_path());
    util::write_file(record, *sha + "\n");
    return *sha;
  } catch (const ActionError& e) {
    if (auto cached = util::try_read_file(record)) {
      const std::string sha(util::trim(*cached));
      if (is_commit_sha(sha)) {
        if (log) log("Using the cached commit " + sha + " of " + owner + "/" + repo + "@" + ref + " (" + e.what() + ")");
        return sha;
      }
    }
    throw;
  }
}

RepositoryCache::Entry RepositoryCache::fetch(const std::string& owner, const std::string& repo,
                                              const std::string& ref, const LogFn& log) {
  std::lock_guard lock(mutex_);
  Entry entry;
  entry.sha = resolve_sha(owner, repo, ref, log);
  entry.dir = root_ / owner / repo / entry.sha;
  std::error_code ec;
  if (fs::exists(entry.dir / ".cpp-actions-complete", ec)) return entry;
  if (log) log("Download action repository '" + owner + "/" + repo + "@" + ref + "' (SHA:" + entry.sha + ")");
  fs::create_directories(entry.dir.parent_path());
  const auto tmp = util::make_temp_dir(".download-", entry.dir.parent_path());
  try {
    const auto archive = tmp / "archive.tar.gz";
    std::vector<std::string> curl = {"curl", "-fsSL", "--retry", "3", "-o", archive.string()};
    std::string url = "https://codeload.github.com/" + owner + "/" + repo + "/tar.gz/" + entry.sha;
    if (!token_.empty()) {
      curl.push_back("-K");
      curl.push_back(write_curl_auth_config(tmp, token_).string());
      url = "https://api.github.com/repos/" + owner + "/" + repo + "/tarball/" + entry.sha;
    }
    curl.push_back(url);
    run_tool(curl);
    const auto extracted = tmp / "tree";
    fs::create_directories(extracted);
    run_tool({"tar", "-xzf", archive.string(), "-C", extracted.string(), "--strip-components=1"});
    util::write_file(extracted / ".cpp-actions-complete", entry.sha + "\n");
    util::remove_tree(entry.dir);
    fs::rename(extracted, entry.dir);
  } catch (...) {
    util::remove_tree(tmp);
    throw;
  }
  util::remove_tree(tmp);
  return entry;
}

NodeProvider::NodeProvider(fs::path explicit_node, fs::path cache_root)
    : explicit_node_(std::move(explicit_node)), cache_root_(std::move(cache_root)) {}

std::optional<fs::path> NodeProvider::download(int major, const LogFn& warn) {
  const auto platform = node_platform();
  const auto arch = node_arch();
  const auto base = cache_root_ / (std::to_string(major) + "-" + platform + "-" + arch);
  if (auto node = find_node_binary(base)) return node;
  fs::create_directories(cache_root_);
  const auto tmp = util::make_temp_dir(".download-", cache_root_);
  try {
    const auto manifest_file = tmp / "manifest.json";
    run_tool({"curl", "-fsSL", "--retry", "3", "-o", manifest_file.string(),
              "https://raw.githubusercontent.com/actions/node-versions/main/versions-manifest.json"});
    const auto manifest = nlohmann::json::parse(util::read_file(manifest_file));
    std::string url;
    std::string version;
    const std::string prefix = std::to_string(major) + ".";
    for (const auto& release : manifest) {
      const auto v = release.value("version", std::string());
      if (!v.starts_with(prefix) || !release.value("stable", false)) continue;
      for (const auto& file : release.value("files", nlohmann::json::array())) {
        if (file.value("platform", std::string()) == platform && file.value("arch", std::string()) == arch) {
          url = file.value("download_url", std::string());
          break;
        }
      }
      if (!url.empty()) {
        version = v;
        break;
      }
    }
    if (url.empty() || !url.starts_with("https://github.com/")) {
      throw ActionError("Node.js " + std::to_string(major) + " is not available for " + platform + "-" + arch);
    }
    if (warn) warn("Downloading Node.js " + version + " (" + platform + "-" + arch + ")");
    const auto archive = tmp / "node.tar.gz";
    run_tool({"curl", "-fsSL", "--retry", "3", "-o", archive.string(), url});
    const auto extracted = tmp / "node";
    fs::create_directories(extracted);
    run_tool({"tar", "-xzf", archive.string(), "-C", extracted.string()});
    if (!find_node_binary(extracted)) throw ActionError("The Node.js archive does not contain bin/node");
    util::remove_tree(base);
    fs::rename(extracted, base);
  } catch (const std::exception& e) {
    util::remove_tree(tmp);
    if (warn) warn(std::string("Failed to download Node.js ") + std::to_string(major) + ": " + e.what());
    return std::nullopt;
  }
  util::remove_tree(tmp);
  return find_node_binary(base);
}

fs::path NodeProvider::get(int major, const LogFn& warn) {
  if (!explicit_node_.empty()) return explicit_node_;
  std::lock_guard lock(mutex_);
  if (auto it = resolved_.find(major); it != resolved_.end()) return it->second;
  const auto env = util::current_environment();
  const auto path_it = env.find("PATH");
  const auto host = util::find_executable("node", path_it != env.end() ? path_it->second : "");
  fs::path result;
  if (host && host_node_major(*host) == major) {
    result = util::real_path(*host);
  } else if (auto downloaded = download(major, warn)) {
    result = *downloaded;
  } else if (host) {
    if (warn) {
      warn("Using the host Node.js (" + host->string() + ") instead of Node.js " + std::to_string(major));
    }
    result = util::real_path(*host);
  } else {
    throw ActionError("Node.js " + std::to_string(major) +
                      " is required to run JavaScript actions. Install node or use --node <path>");
  }
  resolved_[major] = result;
  return result;
}

}  // namespace cpa::actions
