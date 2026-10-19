#include "actions/builtin.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

#include "actions/fetch.hpp"
#include "util/fs.hpp"
#include "util/glob.hpp"
#include "util/sha256.hpp"
#include "util/strings.hpp"
#include "workspace/git.hpp"

namespace cpa::actions {

namespace {

using Inputs = std::map<std::string, std::string>;

std::mutex& store_mutex() {
  static std::mutex m;
  return m;
}

std::string input(const Inputs& inputs, const std::string& name, const std::string& fallback = {}) {
  auto it = inputs.find(name);
  if (it == inputs.end() || util::trim(it->second).empty()) return fallback;
  return std::string(util::trim(it->second));
}

bool input_bool(const Inputs& inputs, const std::string& name, bool fallback) {
  const auto v = input(inputs, name);
  if (v.empty()) return fallback;
  return util::iequals(v, "true");
}

BuiltinResult fail(std::string message) {
  BuiltinResult r;
  r.ok = false;
  r.error = std::move(message);
  return r;
}

bool has_glob(std::string_view s) { return s.find_first_of("*?[") != std::string_view::npos; }

// The real path of the deepest existing ancestor joined with the rest.
fs::path resolve_existing(const fs::path& path) {
  fs::path existing = path;
  fs::path rest;
  std::error_code ec;
  while (!existing.empty() && !fs::exists(fs::symlink_status(existing, ec))) {
    rest = rest.empty() ? existing.filename() : existing.filename() / rest;
    existing = existing.parent_path();
  }
  if (existing.empty()) return path;
  auto real = util::real_path(existing);
  return rest.empty() ? real : (real / rest).lexically_normal();
}

bool within(const fs::path& path, const std::vector<fs::path>& roots) {
  const auto real = resolve_existing(path);
  for (const auto& root : roots) {
    if (util::is_subpath(real, root)) return true;
  }
  return false;
}

fs::path expand_path(std::string_view raw, const BuiltinContext& ctx) {
  std::string p(util::trim(raw));
  fs::path path;
  if (p == "~") {
    path = ctx.home;
  } else if (p.starts_with("~/")) {
    path = ctx.home / p.substr(2);
  } else {
    path = p;
    if (path.is_relative()) path = ctx.workspace / path;
  }
  return path.lexically_normal();
}

// Splits an absolute glob pattern into the literal directory prefix and the
// pattern relative to it.
std::pair<fs::path, std::string> split_glob(const fs::path& pattern) {
  fs::path base;
  std::string rest;
  bool globbing = false;
  for (const auto& part : pattern) {
    const auto s = part.string();
    if (!globbing && !has_glob(s)) {
      base /= part;
    } else {
      globbing = true;
      if (!rest.empty()) rest += "/";
      rest += s;
    }
  }
  return {base, rest};
}

bool is_hidden(const fs::path& rel) {
  for (const auto& part : rel) {
    const auto s = part.string();
    if (s.size() > 1 && s[0] == '.' && s != "..") return true;
  }
  return false;
}

// Copies a tree without following symlinks in the destination: symlinked
// directories and files in the destination are replaced, so writes cannot
// escape `to`.
void safe_copy_tree(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  const auto top = fs::symlink_status(to, ec);
  if (fs::is_symlink(top) || (fs::exists(top) && !fs::is_directory(top))) fs::remove(to);
  fs::create_directories(to);
  for (auto it = fs::recursive_directory_iterator(from); it != fs::recursive_directory_iterator(); ++it) {
    const auto dest = to / it->path().lexically_relative(from);
    const auto dest_status = fs::symlink_status(dest, ec);
    if (it->is_directory() && !it->is_symlink()) {
      if (fs::is_symlink(dest_status) || (fs::exists(dest_status) && !fs::is_directory(dest_status))) {
        fs::remove(dest);
      }
      fs::create_directories(dest);
    } else {
      if (fs::is_directory(dest_status) && !fs::is_symlink(dest_status)) {
        util::remove_tree(dest);
      } else if (fs::exists(dest_status) || fs::is_symlink(dest_status)) {
        fs::remove(dest);
      }
      util::copy_entry(it->path(), dest);
    }
  }
}

void safe_copy(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  const auto st = fs::symlink_status(from, ec);
  if (fs::is_directory(st)) {
    safe_copy_tree(from, to);
    return;
  }
  fs::create_directories(to.parent_path());
  const auto dest_status = fs::symlink_status(to, ec);
  if (fs::is_directory(dest_status) && !fs::is_symlink(dest_status)) {
    util::remove_tree(to);
  } else if (fs::exists(dest_status) || fs::is_symlink(dest_status)) {
    fs::remove(to);
  }
  util::copy_entry(from, to);
}

std::string now_iso() {
  const auto now = std::chrono::system_clock::now();
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

// ---------------------------------------------------------------- checkout

std::optional<std::string> rev_parse(const fs::path& dir, const std::string& rev) {
  return workspace::git_output(dir, {"rev-parse", "--verify", "--quiet", rev + "^{commit}"});
}

BuiltinResult checkout(const Inputs& in, const BuiltinContext& ctx) {
  const auto repository = input(in, "repository", ctx.repository);
  const auto ref = input(in, "ref");
  const auto rel = input(in, "path");
  const fs::path target = rel.empty() ? ctx.workspace : (ctx.workspace / rel).lexically_normal();
  if (!within(target, {util::real_path(ctx.workspace)})) {
    return fail("Repository path '" + target.string() + "' is not under '" + ctx.workspace.string() + "'");
  }
  // Clean the target directory.
  std::error_code ec;
  const auto st = fs::symlink_status(target, ec);
  if (fs::is_symlink(st) || (fs::exists(st) && !fs::is_directory(st))) fs::remove(target);
  fs::create_directories(target);
  for (const auto& entry : fs::directory_iterator(target)) util::remove_tree(entry.path());

  BuiltinResult result;
  const bool self = ctx.repository.empty() ? repository.empty() : util::iequals(repository, ctx.repository);
  if (self || repository.empty()) {
    const auto& snapshot = ctx.snapshot();
    safe_copy_tree(snapshot.dir, target);
    if (ctx.info) {
      ctx.info(std::string("Copied the working tree") + (snapshot.dirty ? " (including uncommitted changes)" : "") +
               " to " + target.string());
    }
    std::string commit = snapshot.sha;
    std::string out_ref = ctx.ref;
    if (!ref.empty() && ref != ctx.ref && ref != ctx.sha && ref != snapshot.sha) {
      if (!snapshot.is_git) return fail("'ref' can't be used because the repository is not a git repository");
      std::string name = ref;
      if (name.starts_with("refs/heads/")) name = name.substr(11);
      std::optional<std::string> sha;
      bool branch = false;
      if (!ref.starts_with("refs/")) {
        if ((sha = rev_parse(target, "refs/remotes/origin/" + name))) {
          branch = true;
        } else {
          sha = rev_parse(target, "refs/tags/" + name);
          if (!sha) sha = rev_parse(target, name);
        }
      } else if (ref.starts_with("refs/heads/")) {
        sha = rev_parse(target, "refs/remotes/origin/" + name);
        if (!sha) sha = rev_parse(target, ref);
        branch = sha.has_value();
      } else {
        sha = rev_parse(target, ref);
      }
      if (!sha) return fail("A branch or tag with the name '" + ref + "' could not be found");
      const bool ok = branch ? workspace::git_ok(target, {"checkout", "--quiet", "--force", "-B", name, *sha})
                             : workspace::git_ok(target, {"checkout", "--quiet", "--force", "--detach", *sha});
      if (!ok) return fail("Failed to check out '" + ref + "'");
      if (ctx.warning && snapshot.dirty) {
        ctx.warning("'" + ref + "' was checked out from the local repository; uncommitted changes are not included");
      }
      commit = *sha;
      out_ref = ref;
    }
    if (input_bool(in, "submodules", false) || util::iequals(input(in, "submodules"), "recursive")) {
      if (ctx.warning) ctx.warning("Submodules are not checked out by cpp-actions");
    }
    result.outputs["ref"] = out_ref;
    result.outputs["commit"] = commit;
    return result;
  }

  // Another repository: fetch it from GitHub.
  const auto parts = util::split(repository, '/');
  if (parts.size() != 2 || parts[0].empty() || parts[1].empty() || repository.find("..") != std::string::npos) {
    return fail("Invalid repository '" + repository + "'. Expected format {owner}/{repo}");
  }
  const auto token = input(in, "token", ctx.token);
  const auto env = git_token_env(token);
  const auto depth = util::parse_int(input(in, "fetch-depth", "1")).value_or(1);
  try {
    run_tool({"git", "init", "--quiet", target.string()});
    run_tool({"git", "-C", target.string(), "remote", "add", "origin", "https://github.com/" + repository});
    std::vector<std::string> fetch = {"git", "-C", target.string(), "fetch", "--quiet", "--no-tags"};
    if (depth > 0) fetch.push_back("--depth=" + std::to_string(depth));
    fetch.push_back("origin");
    fetch.push_back(ref.empty() ? "HEAD" : ref);
    if (ctx.info) ctx.info("Fetching " + repository + (ref.empty() ? "" : "@" + ref) + " from GitHub");
    run_tool(fetch, {}, env);
    run_tool({"git", "-C", target.string(), "checkout", "--quiet", "--force", "--detach", "FETCH_HEAD"});
  } catch (const std::exception& e) {
    return fail(e.what());
  }
  result.outputs["ref"] = ref;
  result.outputs["commit"] = workspace::git_output(target, {"rev-parse", "HEAD"}).value_or("");
  return result;
}

// ------------------------------------------------------------------- cache

struct CacheEntry {
  fs::path dir;
  std::string key;
  std::string version;
  std::int64_t created = 0;
};

std::string cache_version(const std::vector<std::string>& paths) {
  util::Sha256 h;
  for (const auto& p : paths) {
    h.update(p);
    h.update("|");
  }
  h.update("cpp-actions-1");
  return util::Sha256::hex(h.digest());
}

std::vector<CacheEntry> list_cache(const fs::path& store) {
  std::vector<CacheEntry> entries;
  std::error_code ec;
  for (const auto& dir : fs::directory_iterator(store, ec)) {
    const auto meta_text = util::try_read_file(dir.path() / "meta.json");
    if (!meta_text) continue;
    try {
      const auto meta = nlohmann::json::parse(*meta_text);
      entries.push_back({dir.path(), meta.value("key", std::string()), meta.value("version", std::string()),
                         meta.value("created", std::int64_t{0})});
    } catch (const std::exception&) {
    }
  }
  return entries;
}

std::optional<CacheEntry> find_cache(const fs::path& store, const std::string& key,
                                     const std::vector<std::string>& restore_keys, const std::string& version) {
  const auto entries = list_cache(store);
  for (const auto& e : entries) {
    if (e.key == key && e.version == version) return e;
  }
  auto prefixes = restore_keys;
  prefixes.insert(prefixes.begin(), key);
  for (const auto& prefix : prefixes) {
    const CacheEntry* best = nullptr;
    for (const auto& e : entries) {
      if (e.version != version || !e.key.starts_with(prefix)) continue;
      if (best == nullptr || e.created > best->created) best = &e;
    }
    if (best != nullptr) return *best;
  }
  return std::nullopt;
}

struct CachePath {
  std::string base;  // workspace, home, absolute
  std::string rel;
};

fs::path cache_target(const CachePath& p, const BuiltinContext& ctx) {
  if (p.base == "workspace") return (ctx.workspace / p.rel).lexically_normal();
  if (p.base == "home") return (ctx.home / p.rel).lexically_normal();
  return fs::path(p.rel).lexically_normal();
}

CachePath cache_path_of(const fs::path& path, const BuiltinContext& ctx) {
  if (util::is_subpath(path, ctx.workspace)) {
    return {"workspace", path.lexically_relative(ctx.workspace).generic_string()};
  }
  if (util::is_subpath(path, ctx.home)) return {"home", path.lexically_relative(ctx.home).generic_string()};
  return {"absolute", path.string()};
}

// Resolves cache / artifact paths (globs and '!' exclusions). Returns the
// matched top-level paths (files or directories).
std::vector<fs::path> resolve_paths(const std::vector<std::string>& lines, const BuiltinContext& ctx,
                                    std::vector<fs::path>* search_roots = nullptr) {
  std::vector<fs::path> result;
  std::vector<std::string> excludes;
  for (const auto& line : lines) {
    if (line.starts_with("!")) {
      excludes.push_back(expand_path(line.substr(1), ctx).generic_string());
      continue;
    }
    const auto path = expand_path(line, ctx);
    std::error_code ec;
    if (has_glob(path.string())) {
      const auto [base, rest] = split_glob(path);
      if (search_roots != nullptr) search_roots->push_back(base);
      for (const auto& f : util::glob_files(base, {rest})) result.push_back(f);
    } else {
      if (search_roots != nullptr) {
        search_roots->push_back(fs::is_directory(fs::symlink_status(path, ec)) ? path : path.parent_path());
      }
      if (fs::exists(fs::symlink_status(path, ec))) result.push_back(path);
    }
  }
  if (!excludes.empty()) {
    std::vector<fs::path> expanded;
    for (const auto& p : result) {
      if (!fs::is_directory(fs::symlink_status(p))) {
        expanded.push_back(p);
        continue;
      }
      if (fs::is_empty(p)) expanded.push_back(p);
      for (const auto& entry : fs::recursive_directory_iterator(p)) {
        const auto st = entry.symlink_status();
        if (!fs::is_directory(st) || fs::is_empty(entry.path())) expanded.push_back(entry.path());
      }
    }
    result = std::move(expanded);
    std::erase_if(result, [&](const fs::path& p) {
      for (const auto& ex : excludes) {
        for (auto ancestor = p; !ancestor.empty(); ancestor = ancestor.parent_path()) {
          if (util::glob_match(ex, ancestor.generic_string())) return true;
          if (ancestor == ancestor.root_path()) break;
        }
      }
      return false;
    });
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

void restore_cache(const CacheEntry& entry, const BuiltinContext& ctx) {
  const auto meta = nlohmann::json::parse(util::read_file(entry.dir / "meta.json"));
  std::size_t i = 0;
  for (const auto& p : meta.at("paths")) {
    const CachePath cp{p.value("base", std::string()), p.value("rel", std::string())};
    const auto target = cache_target(cp, ctx);
    const auto source = entry.dir / "data" / std::to_string(i++);
    if (!within(target, ctx.writable)) {
      if (ctx.warning) ctx.warning("Skipping cached path outside of the writable directories: " + target.string());
      continue;
    }
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(source, ec))) continue;
    safe_copy(source, target);
  }
}

BuiltinResult save_cache(const std::string& key, const std::vector<std::string>& lines, const BuiltinContext& ctx) {
  BuiltinResult result;
  const auto version = cache_version(lines);
  auto paths = resolve_paths(lines, ctx);
  std::erase_if(paths, [&](const fs::path& p) {
    if (within(p, ctx.writable)) return false;
    if (ctx.warning) ctx.warning("Skipping path outside of the writable directories: " + p.string());
    return true;
  });
  if (paths.empty()) {
    if (ctx.warning) {
      ctx.warning(
          "Path Validation Error: Path(s) specified in the action for caching do(es) not exist, hence no cache is "
          "being saved.");
    }
    return result;
  }
  std::lock_guard lock(store_mutex());
  for (const auto& e : list_cache(ctx.cache_store)) {
    if (e.key == key && e.version == version) {
      if (ctx.warning) {
        ctx.warning("Failed to save: Unable to reserve cache with key " + key +
                    ", another job may be creating this cache.");
      }
      return result;
    }
  }
  fs::create_directories(ctx.cache_store);
  const auto tmp = util::make_temp_dir(".save-", ctx.cache_store);
  try {
    nlohmann::json meta;
    meta["key"] = key;
    meta["version"] = version;
    meta["created"] = std::stoll(now_iso());
    meta["paths"] = nlohmann::json::array();
    std::size_t i = 0;
    for (const auto& p : paths) {
      const auto cp = cache_path_of(p, ctx);
      safe_copy(p, tmp / "data" / std::to_string(i++));
      meta["paths"].push_back({{"base", cp.base}, {"rel", cp.rel}});
    }
    util::write_file(tmp / "meta.json", meta.dump(2));
    util::Sha256 id;
    id.update(key);
    id.update("\n");
    id.update(version);
    const auto dest = ctx.cache_store / util::Sha256::hex(id.digest());
    util::remove_tree(dest);
    fs::rename(tmp, dest);
  } catch (...) {
    util::remove_tree(tmp);
    throw;
  }
  if (ctx.info) ctx.info("Cache saved with key: " + key);
  return result;
}

BuiltinResult cache_main(Builtin kind, const Inputs& in, const BuiltinContext& ctx) {
  const auto key = input(in, "key");
  if (key.empty()) return fail("Input required and not supplied: key");
  const auto lines = input_lines(input(in, "path"));
  if (lines.empty()) return fail("Input required and not supplied: path");
  if (kind == Builtin::CacheSave) return save_cache(key, lines, ctx);

  BuiltinResult result;
  const auto restore_keys = input_lines(input(in, "restore-keys"));
  const auto version = cache_version(lines);
  result.state["CACHE_KEY"] = key;
  if (kind == Builtin::CacheRestore) result.outputs["cache-primary-key"] = key;
  std::optional<CacheEntry> entry;
  {
    std::lock_guard lock(store_mutex());
    entry = find_cache(ctx.cache_store, key, restore_keys, version);
  }
  if (!entry) {
    if (input_bool(in, "fail-on-cache-miss", false)) {
      return fail("Failed to restore cache entry. Exiting as fail-on-cache-miss is set. Input key: " + key);
    }
    std::string keys = key;
    for (const auto& k : restore_keys) keys += ", " + k;
    if (ctx.info) ctx.info("Cache not found for input keys: " + keys);
    return result;
  }
  if (!input_bool(in, "lookup-only", false)) {
    restore_cache(*entry, ctx);
    if (ctx.info) ctx.info("Cache restored from key: " + entry->key);
  } else if (ctx.info) {
    ctx.info("Cache found and can be restored from key: " + entry->key);
  }
  result.state["CACHE_RESULT"] = entry->key;
  result.outputs["cache-hit"] = entry->key == key ? "true" : "false";
  if (kind == Builtin::CacheRestore) result.outputs["cache-matched-key"] = entry->key;
  return result;
}

BuiltinResult cache_post(const Inputs& in, const std::map<std::string, std::string>& state,
                         const BuiltinContext& ctx) {
  if (!ctx.job_succeeded) return {};
  auto it = state.find("CACHE_KEY");
  const std::string key = it != state.end() ? it->second : input(in, "key");
  if (key.empty()) return {};
  auto hit = state.find("CACHE_RESULT");
  if (hit != state.end() && hit->second == key) {
    if (ctx.info) ctx.info("Cache hit occurred on the primary key " + key + ", not saving cache.");
    return {};
  }
  if (input_bool(in, "lookup-only", false)) return {};
  return save_cache(key, input_lines(input(in, "path")), ctx);
}

// ---------------------------------------------------------------- artifacts

bool valid_artifact_name(std::string_view name) {
  if (name.empty() || name == "." || name == "..") return false;
  return name.find_first_of("\":<>|*?\r\n\\/") == std::string_view::npos;
}

BuiltinResult upload_artifact(const Inputs& in, const BuiltinContext& ctx) {
  const auto name = input(in, "name", "artifact");
  if (!valid_artifact_name(name)) {
    return fail("The artifact name is not valid: " + name +
                ". Contains the following character: one of \" : < > | * ? \\r \\n \\ /");
  }
  const auto lines = input_lines(input(in, "path"));
  if (lines.empty()) return fail("Input required and not supplied: path");
  const auto if_no_files = util::to_lower(input(in, "if-no-files-found", "warn"));
  if (if_no_files != "warn" && if_no_files != "ignore" && if_no_files != "error") {
    return fail("Unrecognized if-no-files-found input. Provided: " + if_no_files);
  }
  const bool include_hidden = input_bool(in, "include-hidden-files", false);

  std::vector<fs::path> roots;
  const auto matched = resolve_paths(lines, ctx, &roots);
  const auto root = artifact_root(roots);
  std::vector<std::pair<fs::path, fs::path>> files;  // source, relative destination
  for (const auto& p : matched) {
    if (!within(p, ctx.writable)) return fail("Path is outside of the writable directories: " + p.string());
    std::error_code ec;
    if (fs::is_directory(fs::symlink_status(p, ec))) {
      for (auto it = fs::recursive_directory_iterator(p); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory() && !it->is_symlink()) continue;
        files.emplace_back(it->path(), it->path().lexically_relative(root));
      }
    } else {
      files.emplace_back(p, p.lexically_relative(root));
    }
  }
  if (!include_hidden) {
    std::erase_if(files, [](const auto& f) { return is_hidden(f.second); });
  }
  BuiltinResult result;
  if (files.empty()) {
    const std::string message =
        "No files were found with the provided path: " + util::join(lines, "\n") + ". No artifacts will be uploaded.";
    if (if_no_files == "error") return fail(message);
    if (if_no_files == "warn" && ctx.warning) ctx.warning(message);
    return result;
  }
  std::lock_guard lock(store_mutex());
  const auto dest = ctx.artifact_store / name;
  std::error_code ec;
  if (fs::exists(dest, ec)) {
    if (!input_bool(in, "overwrite", false)) {
      return fail(
          "Failed to CreateArtifact: Received non-retryable error: Failed request: (409) Conflict: an artifact with "
          "this name already exists on the workflow run");
    }
    util::remove_tree(dest);
  }
  fs::create_directories(dest);
  util::Sha256 digest;
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  for (const auto& [src, rel] : files) {
    const auto to = dest / rel;
    fs::create_directories(to.parent_path());
    util::copy_entry(src, to);
    digest.update(rel.generic_string());
    digest.update("\n");
    if (fs::is_regular_file(fs::symlink_status(src, ec))) {
      const auto bytes = util::sha256_file(src);
      digest.update(bytes.data(), bytes.size());
    }
  }
  const int id = ctx.next_artifact_id ? ctx.next_artifact_id() : 1;
  if (ctx.info) {
    ctx.info("Artifact " + name + " has been successfully uploaded (" + std::to_string(files.size()) + " files)");
  }
  result.outputs["artifact-id"] = std::to_string(id);
  result.outputs["artifact-url"] = "";
  result.outputs["artifact-digest"] = util::Sha256::hex(digest.digest());
  return result;
}

BuiltinResult download_artifact(const Inputs& in, const BuiltinContext& ctx) {
  if (!input(in, "run-id").empty() || !input(in, "artifact-ids").empty()) {
    return fail("Downloading artifacts of other workflow runs or by id is not supported by cpp-actions");
  }
  const auto name = input(in, "name");
  const auto pattern = input(in, "pattern");
  const bool merge = input_bool(in, "merge-multiple", false);
  const auto path_input = input(in, "path");
  const auto dest = path_input.empty() ? ctx.workspace : expand_path(path_input, ctx);
  if (!within(dest, ctx.writable)) return fail("Path is outside of the writable directories: " + dest.string());
  BuiltinResult result;
  std::lock_guard lock(store_mutex());
  std::error_code ec;
  if (!name.empty()) {
    const auto src = ctx.artifact_store / name;
    if (!valid_artifact_name(name) || !fs::is_directory(fs::symlink_status(src, ec))) {
      return fail("Unable to download artifact(s): Artifact not found for name: " + name);
    }
    safe_copy_tree(src, dest);
    if (ctx.info) ctx.info("Artifact " + name + " was downloaded to " + dest.string());
  } else {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(ctx.artifact_store, ec)) {
      const auto n = entry.path().filename().string();
      if (!fs::is_directory(entry.symlink_status()) || !valid_artifact_name(n)) continue;
      if (pattern.empty() || util::glob_match(pattern, n)) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    for (const auto& n : names) safe_copy_tree(ctx.artifact_store / n, merge ? dest : dest / n);
    if (ctx.info) ctx.info("Downloaded " + std::to_string(names.size()) + " artifact(s) to " + dest.string());
  }
  result.outputs["download-path"] = dest.string();
  return result;
}

}  // namespace

std::vector<std::string> input_lines(std::string_view value) {
  std::vector<std::string> lines;
  for (const auto& line : util::split_lines(value)) {
    const auto t = util::trim(line);
    if (!t.empty()) lines.emplace_back(t);
  }
  return lines;
}

fs::path artifact_root(const std::vector<fs::path>& search_paths) {
  if (search_paths.empty()) return {};
  fs::path common = search_paths.front().lexically_normal();
  for (std::size_t i = 1; i < search_paths.size(); ++i) {
    const auto p = search_paths[i].lexically_normal();
    fs::path next;
    auto a = common.begin();
    auto b = p.begin();
    for (; a != common.end() && b != p.end() && *a == *b; ++a, ++b) next /= *a;
    common = next;
  }
  return common;
}

Builtin find_builtin(const ActionRef& ref) {
  if (ref.kind != ActionRef::Kind::Remote || !util::iequals(ref.owner, "actions")) return Builtin::None;
  const auto name = util::to_lower(ref.repo + (ref.path.empty() ? "" : "/" + ref.path));
  if (name == "checkout") return Builtin::Checkout;
  if (name == "cache") return Builtin::Cache;
  if (name == "cache/restore") return Builtin::CacheRestore;
  if (name == "cache/save") return Builtin::CacheSave;
  if (name == "upload-artifact") return Builtin::UploadArtifact;
  if (name == "download-artifact") return Builtin::DownloadArtifact;
  return Builtin::None;
}

bool builtin_has_post(Builtin builtin) { return builtin == Builtin::Cache; }

BuiltinResult run_builtin(Builtin builtin, bool post, const std::map<std::string, std::string>& inputs,
                          const std::map<std::string, std::string>& state, const BuiltinContext& context) {
  try {
    switch (builtin) {
      case Builtin::Checkout:
        return checkout(inputs, context);
      case Builtin::Cache:
        if (post) return cache_post(inputs, state, context);
        return cache_main(builtin, inputs, context);
      case Builtin::CacheRestore:
      case Builtin::CacheSave:
        return cache_main(builtin, inputs, context);
      case Builtin::UploadArtifact:
        return upload_artifact(inputs, context);
      case Builtin::DownloadArtifact:
        return download_artifact(inputs, context);
      case Builtin::None:
        break;
    }
  } catch (const std::exception& e) {
    return fail(e.what());
  }
  return fail("Unknown builtin action");
}

}  // namespace cpa::actions
