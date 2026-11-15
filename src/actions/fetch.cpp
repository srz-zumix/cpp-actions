#include "actions/fetch.hpp"

#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

#include "actions/action.hpp"
#include "util/fs.hpp"
#include "util/process.hpp"
#include "util/sha256.hpp"
#include "util/strings.hpp"
#include "workspace/git.hpp"

namespace cpa::actions {

namespace {

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

fs::path node_cache_dir(const fs::path& root, int major) {
  return root / (std::to_string(major) + "-" + node_platform() + "-" + node_arch());
}

std::optional<fs::path> find_node_binary(const fs::path& root) {
  for (const auto& candidate : {root / "bin" / "node", root / "node"}) {
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (!entry.is_directory(ec)) continue;
    const auto candidate = entry.path() / "bin" / "node";
    if (fs::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return std::nullopt;
}

struct NodeVersion {
  std::array<std::uint64_t, 3> release{};
  std::vector<std::string> prerelease;
};

bool numeric_identifier(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool valid_version_identifiers(std::string_view text, bool prerelease) {
  for (const auto& part : util::split(text, '.')) {
    if (part.empty()) return false;
    for (char c : part) {
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-')) {
        return false;
      }
    }
    if (prerelease && part.size() > 1 && part.front() == '0' && numeric_identifier(part)) return false;
  }
  return true;
}

std::optional<NodeVersion> parse_node_version(std::string_view text) {
  text = util::trim(text);
  if (text.starts_with("v")) text.remove_prefix(1);
  const auto plus = text.find('+');
  if (plus != std::string_view::npos) {
    if (!valid_version_identifiers(text.substr(plus + 1), false)) return std::nullopt;
    text = text.substr(0, plus);
  }

  NodeVersion version;
  const auto dash = text.find('-');
  if (dash != std::string_view::npos) {
    const auto prerelease = text.substr(dash + 1);
    if (!valid_version_identifiers(prerelease, true)) return std::nullopt;
    version.prerelease = util::split(prerelease, '.');
    text = text.substr(0, dash);
  }
  const auto parts = util::split(text, '.');
  if (parts.size() != version.release.size()) return std::nullopt;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto& part = parts[i];
    if (!numeric_identifier(part) || (part.size() > 1 && part.front() == '0')) return std::nullopt;
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), version.release[i]);
    if (error != std::errc{} || end != part.data() + part.size()) return std::nullopt;
  }
  return version;
}

bool node_version_less(const NodeVersion& lhs, const NodeVersion& rhs) {
  if (lhs.release != rhs.release) return lhs.release < rhs.release;
  if (lhs.prerelease.empty() || rhs.prerelease.empty()) {
    return !lhs.prerelease.empty() && rhs.prerelease.empty();
  }
  const auto count = std::min(lhs.prerelease.size(), rhs.prerelease.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto& a = lhs.prerelease[i];
    const auto& b = rhs.prerelease[i];
    if (a == b) continue;
    const bool a_numeric = numeric_identifier(a);
    const bool b_numeric = numeric_identifier(b);
    if (a_numeric != b_numeric) return a_numeric;
    if (a_numeric && a.size() != b.size()) return a.size() < b.size();
    return a < b;
  }
  return lhs.prerelease.size() < rhs.prerelease.size();
}

struct NodeProbe {
  std::optional<NodeVersion> version;
  std::string error;
};

NodeProbe probe_node(const fs::path& node, const util::Environment& env) {
  constexpr std::size_t kMaxVersionOutput = 4096;
  std::string output;
  std::atomic<bool> output_too_long{false};
  util::ProcessSpec spec;
  spec.argv = {node.string(), "--version"};
  spec.env = env;
  spec.timeout = std::chrono::seconds(3);
  spec.hard_timeout = true;
  spec.force_cancel = &output_too_long;
  spec.on_output = [&](std::string_view chunk, bool is_stderr) {
    if (is_stderr || output_too_long.load()) return;
    if (chunk.size() > kMaxVersionOutput - output.size()) {
      output_too_long.store(true);
    } else {
      output.append(chunk);
    }
  };
  const auto result = util::run_process(spec);
  if (output_too_long.load()) return {std::nullopt, "version output is too long"};
  if (result.timed_out) return {std::nullopt, "version check timed out"};
  if (result.spawn_failed) return {std::nullopt, result.error};
  if (result.exit_code != 0) {
    return {std::nullopt, "version check exited with code " + std::to_string(result.exit_code)};
  }
  auto version = parse_node_version(output);
  if (!version) return {std::nullopt, "invalid version output"};
  return {std::move(version), {}};
}

struct NodeCandidate {
  fs::path path;
  NodeVersion version;
};

struct NodenvShim {
  fs::path path;
  fs::path root;
};

std::optional<NodenvShim> nodenv_shim(const fs::path& path);

std::optional<NodeCandidate> node_candidate(const fs::path& path, int major, const util::Environment& env,
                                            const LogFn& warn, std::string_view source, bool warn_mismatch,
                                            const fs::path& shim = {}) {
  auto ignore = [&](std::string_view reason) {
    if (warn) warn("Ignoring " + std::string(source) + " Node.js " + path.string() + ": " + std::string(reason));
  };
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) return std::nullopt;
  if (ec) {
    ignore(ec.message());
    return std::nullopt;
  }
  const auto real = fs::canonical(path, ec);
  if (ec) {
    ignore(ec.message());
    return std::nullopt;
  }
  if (real == shim) {
    ignore("it points back to the nodenv shim");
    return std::nullopt;
  }
  if (!fs::is_regular_file(real, ec) || ::access(real.c_str(), X_OK) != 0) {
    ignore(ec ? ec.message() : "not an executable file");
    return std::nullopt;
  }
  if (nodenv_shim(real)) {
    ignore("it is a nodenv shim rather than a Node.js executable");
    return std::nullopt;
  }
  auto probe = probe_node(real, env);
  if (!probe.version) {
    ignore(probe.error);
    return std::nullopt;
  }
  if (major < 0 || probe.version->release[0] != static_cast<std::uint64_t>(major)) {
    if (warn_mismatch) ignore("does not match required major version " + std::to_string(major));
    return std::nullopt;
  }
  return NodeCandidate{real, std::move(*probe.version)};
}

std::optional<NodenvShim> nodenv_shim(const fs::path& path) {
  std::error_code ec;
  const auto real = fs::canonical(path, ec);
  if (ec) return std::nullopt;
  std::ifstream in(real, std::ios::binary);
  if (!in) return std::nullopt;
  std::array<char, 8192> buffer{};
  in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  const std::string_view contents(buffer.data(), static_cast<std::size_t>(in.gcount()));
  if (!contents.starts_with("#!") || contents.find("NODENV_ROOT") == std::string_view::npos ||
      contents.find("nodenv") == std::string_view::npos ||
      contents.find(" exec \"$program\"") == std::string_view::npos) {
    return std::nullopt;
  }

  // Generated shims embed their root; do not evaluate shell syntax to read it.
  constexpr std::string_view root_prefix = "\nexport NODENV_ROOT=\"";
  const auto root_line = contents.find(root_prefix);
  if (root_line != std::string_view::npos) {
    const auto start = root_line + root_prefix.size();
    const auto end = contents.find('"', start);
    if (end != std::string_view::npos && end + 1 < contents.size() && contents[end + 1] == '\n') {
      const auto root = contents.substr(start, end - start);
      if (root.find_first_of("\\$`") == std::string_view::npos && fs::path(root).is_absolute()) {
        return NodenvShim{real, util::real_path(fs::path(root))};
      }
    }
  }
  auto shims = real.parent_path();
  if (shims.filename() != "shims") {
    shims = fs::absolute(path, ec).lexically_normal().parent_path();
    if (ec || shims.filename() != "shims") return std::nullopt;
  }
  const auto root = fs::canonical(shims.parent_path(), ec);
  if (ec) return std::nullopt;
  return NodenvShim{real, root};
}

void keep_latest_node(std::optional<NodeCandidate>& best, std::optional<NodeCandidate> candidate) {
  if (!candidate) return;
  if (!best || node_version_less(best->version, candidate->version) ||
      (!node_version_less(candidate->version, best->version) && !node_version_less(best->version, candidate->version) &&
       candidate->path < best->path)) {
    best = std::move(candidate);
  }
}

std::optional<NodeCandidate> cached_node(const fs::path& cache_root, int major, const util::Environment& env,
                                         const LogFn& warn) {
  const auto base = node_cache_dir(cache_root, major);
  std::optional<NodeCandidate> best;
  for (const auto& path : {base / "bin" / "node", base / "node"}) {
    keep_latest_node(best, node_candidate(path, major, env, warn, "cached", true));
  }
  std::error_code ec;
  fs::directory_iterator it(base, ec), end;
  if (ec) {
    if (ec != std::errc::no_such_file_or_directory && warn) {
      warn("Cannot inspect cached Node.js in " + base.string() + ": " + ec.message());
    }
    return best;
  }
  for (; it != end; it.increment(ec)) {
    std::error_code entry_error;
    if (!it->is_directory(entry_error)) {
      if (entry_error && warn)
        warn("Cannot inspect cached Node.js in " + it->path().string() + ": " + entry_error.message());
      continue;
    }
    keep_latest_node(best, node_candidate(it->path() / "bin" / "node", major, env, warn, "cached", true));
  }
  if (ec && warn) warn("Cannot inspect cached Node.js in " + base.string() + ": " + ec.message());
  return best;
}

std::optional<NodeCandidate> latest_nodenv_node(const NodenvShim& shim, int major, const util::Environment& env,
                                                const LogFn& warn) {
  const auto versions = shim.root / "versions";
  std::error_code ec;
  fs::directory_iterator it(versions, ec), end;
  if (ec) {
    if (ec != std::errc::no_such_file_or_directory && warn) {
      warn("Cannot inspect nodenv versions in " + versions.string() + ": " + ec.message());
    }
    return std::nullopt;
  }
  std::optional<NodeCandidate> best;
  for (; it != end; it.increment(ec)) {
    std::error_code entry_error;
    if (!it->is_directory(entry_error)) {
      if (entry_error && warn)
        warn("Cannot inspect nodenv installation " + it->path().string() + ": " + entry_error.message());
      continue;
    }
    keep_latest_node(best, node_candidate(it->path() / "bin" / "node", major, env, warn, "nodenv", false, shim.path));
  }
  if (ec && warn) warn("Cannot inspect nodenv versions in " + versions.string() + ": " + ec.message());
  return best;
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
  for (const auto& name : {"refs/tags/" + r + "^{}", "refs/tags/" + r, "refs/heads/" + r, r + "^{}", r}) {
    auto it = refs.find(name);
    if (it != refs.end() && is_commit_sha(it->second)) return it->second;
  }
  return std::nullopt;
}

std::string run_tool(const std::vector<std::string>& argv, const fs::path& cwd,
                     const std::map<std::string, std::string>& extra_env) {
  auto env = !argv.empty() && fs::path(argv.front()).filename() == "git" ? workspace::git_environment()
                                                                      : util::current_environment();
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
          {"GIT_CONFIG_VALUE_0", "AUTHORIZATION: basic " + util::base64_encode("x-access-token:" + token)}};
}

std::map<std::string, std::string> git_submodule_env(const std::string& token) {
  auto env = git_token_env(token);
  int count = token.empty() ? 0 : 1;
  for (const auto* prefix : {"git@github.com:", "ssh://git@github.com/"}) {
    const auto index = std::to_string(count++);
    env["GIT_CONFIG_KEY_" + index] = "url.https://github.com/.insteadOf";
    env["GIT_CONFIG_VALUE_" + index] = prefix;
  }
  env["GIT_CONFIG_COUNT"] = std::to_string(count);
  return env;
}

fs::path write_curl_auth_config(const fs::path& dir, const std::string& token) {
  std::string escaped;
  for (char c : token) {
    if (std::iscntrl(static_cast<unsigned char>(c))) throw ActionError("Invalid character in GitHub token");
    if (c == '"' || c == '\\') escaped += '\\';
    escaped += c;
  }
  const auto path = dir / "curl-auth.conf";
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "Creating curl auth config");
  const auto content = "header = \"Authorization: Bearer " + escaped + "\"\n";
  std::size_t written = 0;
  while (written < content.size()) {
    const auto count = ::write(fd, content.data() + written, content.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      const int error = count < 0 ? errno : EIO;
      ::close(fd);
      fs::remove(path);
      throw std::system_error(error, std::generic_category(), "Writing curl auth config");
    }
    written += static_cast<std::size_t>(count);
  }
  ::close(fd);
  return path;
}

RepositoryCache::RepositoryCache(fs::path root, std::string token) : root_(std::move(root)), token_(std::move(token)) {}

std::string RepositoryCache::resolve_sha(const std::string& owner, const std::string& repo, const std::string& ref,
                                         const LogFn& log) {
  if (is_commit_sha(ref)) return util::to_lower(ref);
  const auto record = root_ / owner / repo / "refs" / util::sha256_hex(ref);
  const std::string url = "https://github.com/" + owner + "/" + repo;
  try {
    std::vector<std::string> argv = {"git", "ls-remote", "--", url};
    if (ref.starts_with("refs/")) {
      argv.insert(argv.end(), {ref, ref + "^{}"});
    } else {
      argv.insert(argv.end(), {"refs/tags/" + ref, "refs/tags/" + ref + "^{}", "refs/heads/" + ref});
    }
    const auto out = run_tool(argv, {}, git_token_env(token_));
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
  const auto base = node_cache_dir(cache_root_, major);
  fs::create_directories(cache_root_);
  const auto tmp = util::make_temp_dir(".download-", cache_root_);
  fs::path result;
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
    const auto node = find_node_binary(extracted);
    if (!node) throw ActionError("The Node.js archive does not contain an executable bin/node");
    const auto expected = parse_node_version(version);
    const auto candidate = node_candidate(*node, major, util::current_environment(), {}, "downloaded", true);
    if (!expected || !candidate || node_version_less(*expected, candidate->version) ||
        node_version_less(candidate->version, *expected)) {
      throw ActionError("The downloaded Node.js executable does not match version " + version);
    }
    const auto relative = node->lexically_relative(extracted);
    util::remove_tree(base);
    fs::rename(extracted, base);
    result = fs::canonical(base / relative);
  } catch (const std::exception& e) {
    util::remove_tree(tmp);
    if (warn) warn(std::string("Failed to download Node.js ") + std::to_string(major) + ": " + e.what());
    return std::nullopt;
  }
  util::remove_tree(tmp);
  return result;
}

fs::path NodeProvider::get(int major, const LogFn& warn) {
  if (!explicit_node_.empty()) {
    auto path = explicit_node_;
    if (!path.has_parent_path()) {
      const auto env = util::current_environment();
      const auto it = env.find("PATH");
      const auto executable = util::find_executable(path.string(), it == env.end() ? "" : it->second);
      if (!executable) throw ActionError("Node.js executable not found: " + path.string());
      path = *executable;
    }
    if (::access(path.c_str(), X_OK) != 0 || !fs::is_regular_file(path)) {
      throw ActionError("Node.js executable is not executable: " + path.string());
    }
    return util::real_path(path);
  }
  std::lock_guard lock(mutex_);
  if (auto it = resolved_.find(major); it != resolved_.end()) return it->second;
  const auto env = util::current_environment();
  const auto path_it = env.find("PATH");
  const auto host = util::find_executable("node", path_it != env.end() ? path_it->second : "");
  const auto shim = host ? nodenv_shim(*host) : std::nullopt;
  std::optional<NodeCandidate> cached;
  fs::path result;
  if (shim) {
    cached = cached_node(cache_root_, major, env, warn);
    const auto installed = latest_nodenv_node(*shim, major, env, warn);
    if (installed && (!cached || node_version_less(cached->version, installed->version))) {
      result = installed->path;
    } else if (cached) {
      result = cached->path;
    }
  } else if (host) {
    const auto probe = probe_node(*host, env);
    if (probe.version && major >= 0 && probe.version->release[0] == static_cast<std::uint64_t>(major)) {
      result = util::real_path(*host);
    }
  }
  if (result.empty()) {
    if (!shim) cached = cached_node(cache_root_, major, env, warn);
    if (cached) {
      result = cached->path;
    } else if (auto downloaded = download(major, warn)) {
      result = *downloaded;
    } else if (host) {
      if (warn) {
        if (shim) {
          warn("No usable Node.js " + std::to_string(major) + " was found; falling back to the nodenv shim (" +
               host->string() + "), which may fail on an action's .node-version");
        } else {
          warn("Using the host Node.js (" + host->string() + ") instead of Node.js " + std::to_string(major));
        }
      }
      result = util::real_path(*host);
    } else {
      throw ActionError("Node.js " + std::to_string(major) +
                        " is required to run JavaScript actions. Install node or use --node <path>");
    }
  }
  resolved_[major] = result;
  return result;
}

}  // namespace cpa::actions
