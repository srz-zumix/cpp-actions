#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "engine/catalog.hpp"
#include "engine/console.hpp"
#include "engine/event.hpp"
#include "engine/report.hpp"
#include "expr/value.hpp"
#include "workspace/git.hpp"

namespace cpa::engine {

namespace fs = std::filesystem;

struct SandboxOptions {
  bool enabled = true;
  std::vector<std::string> allow_hosts;  // in addition to the GitHub domains
  std::vector<fs::path> allow_read;
  std::vector<fs::path> allow_write;
  bool allow_localhost = false;
};

struct RunOptions {
  fs::path run_dir;    // default: <cache>/runs/<timestamp>-<random>
  fs::path cache_dir;  // shared caches; default: <user cache dir>/cpp-actions
  std::vector<std::string> jobs;              // job ids to run (empty = all)
  bool with_needs = true;                     // also run the jobs needed by the selected jobs
  std::map<std::string, std::string> matrix;  // matrix filters (key=value)
  int parallel = 0;                           // concurrent jobs (0 = number of CPUs)
  std::map<std::string, std::string> secrets;
  std::map<std::string, std::string> vars;
  std::map<std::string, std::string> inputs;  // workflow_dispatch inputs
  std::vector<std::string> pass_env;          // NAME or NAME=VALUE passed to the steps
  std::vector<std::string> host_labels;       // additional runs-on labels executed on the host
  std::map<std::string, config::EnvironmentConfig> environments;
  SandboxOptions sandbox;
  fs::path node;  // node executable for JavaScript actions
  std::string github_token;
  bool keep_workdir = false;
  bool debug = false;
};

class Runner {
 public:
  Runner(RunOptions options, Console& console);
  ~Runner();
  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

  // Runs the triggered workflows of the selection.
  RunReport run(const std::vector<Selection>& selection, const Event& event, const workspace::GitInfo& git);
  // First call: graceful cancellation; next calls: force. Thread-safe.
  void cancel();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Labels of an evaluated `runs-on` (string, sequence or {group, labels}).
std::vector<std::string> runs_on_labels(const expr::Value& runs_on);

struct RunnerMatch {
  bool host = false;
  std::string reason;
};

// Decides whether a job with these labels runs on this host: macos-* labels
// (and self-hosted macOS) on macOS hosts, plus `host_labels`.
RunnerMatch match_runner(const std::vector<std::string>& labels, const std::vector<std::string>& host_labels,
                         bool host_is_macos);

bool host_is_macos();

// The default cache directory (<user cache dir>/cpp-actions).
fs::path default_cache_dir();

}  // namespace cpa::engine
