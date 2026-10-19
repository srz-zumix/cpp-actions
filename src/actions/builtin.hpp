#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "actions/action.hpp"
#include "workspace/snapshot.hpp"

namespace cpa::actions {

// Actions that depend on GitHub services or on the repository checkout and
// are emulated by cpp-actions instead of being downloaded.
enum class Builtin { None, Checkout, Cache, CacheRestore, CacheSave, UploadArtifact, DownloadArtifact };

Builtin find_builtin(const ActionRef& ref);
bool builtin_has_post(Builtin builtin);

struct BuiltinContext {
  fs::path workspace;  // GITHUB_WORKSPACE
  fs::path home;       // HOME of the job ("~" in paths)
  std::vector<fs::path> writable;  // roots that may be written (workspace, temp, home, tool cache, ...)
  std::vector<fs::path> readable;  // roots that may be read; empty disables read confinement
  fs::path cache_store;            // actions/cache entries of the repository
  fs::path artifact_store;         // artifacts of the workflow run
  std::string repository;          // owner/name of the workflow repository ("" when unknown)
  std::string ref;                 // event ref
  std::string sha;                 // event sha
  std::string token;
  std::function<const workspace::Snapshot&()> snapshot;
  std::function<void(std::string_view)> info;
  std::function<void(std::string_view)> warning;
  std::function<int()> next_artifact_id;
  bool job_succeeded = true;
};

struct BuiltinResult {
  bool ok = true;
  std::string error;
  std::map<std::string, std::string> outputs;
  std::map<std::string, std::string> state;
};

// `inputs` uses lower-case names. `state` is the state saved by the main step
// (post steps).
BuiltinResult run_builtin(Builtin builtin, bool post, const std::map<std::string, std::string>& inputs,
                          const std::map<std::string, std::string>& state, const BuiltinContext& context);

// Least common ancestor directory of upload search paths (artifact root).
fs::path artifact_root(const std::vector<fs::path>& search_paths);

// Non-empty, trimmed lines of a multi-line input.
std::vector<std::string> input_lines(std::string_view value);

}  // namespace cpa::actions
