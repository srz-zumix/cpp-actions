#pragma once

// Internal interfaces shared by the runner (scheduler) and the job runner.

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "actions/fetch.hpp"
#include "engine/console.hpp"
#include "engine/event.hpp"
#include "engine/masker.hpp"
#include "engine/report.hpp"
#include "engine/runner.hpp"
#include "expr/value.hpp"
#include "workflow/model.hpp"
#include "workspace/git.hpp"
#include "workspace/snapshot.hpp"

namespace cpa::engine {

class Semaphore {
 public:
  explicit Semaphore(int count) : count_(count) {}
  void acquire();
  void release();

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  int count_;
};

class SemaphoreGuard {
 public:
  explicit SemaphoreGuard(Semaphore* s) : s_(s) {
    if (s_ != nullptr) s_->acquire();
  }
  ~SemaphoreGuard() {
    if (s_ != nullptr) s_->release();
  }
  SemaphoreGuard(const SemaphoreGuard&) = delete;
  SemaphoreGuard& operator=(const SemaphoreGuard&) = delete;

 private:
  Semaphore* s_;
};

// State shared by all jobs of a run (thread-safe).
class RunState {
 public:
  RunState(RunOptions options, Console& console, Event event, workspace::GitInfo git);

  const RunOptions options;
  Console& console;
  Masker masker;
  const Event event;
  const workspace::GitInfo git;
  const std::string repository;  // owner/name
  std::atomic<bool> cancelled{false};
  std::atomic<bool> force_cancelled{false};
  Semaphore slots;
  actions::RepositoryCache repositories;
  actions::NodeProvider node;
  std::atomic<int> artifact_ids{0};

  // Snapshot of the working tree, created on first use. Throws SnapshotError.
  const workspace::Snapshot& snapshot();

  // Cancel flags (graceful, force) of the running job instances. Flags
  // registered after a cancellation are set immediately.
  void register_cancel(std::atomic<bool>* cancel, std::atomic<bool>* force);
  void unregister_cancel(std::atomic<bool>* cancel);
  void cancel_all(bool force);

 private:
  std::mutex snapshot_mutex_;
  std::optional<workspace::Snapshot> snapshot_;
  std::string snapshot_error_;
  std::mutex flags_mutex_;
  std::vector<std::pair<std::atomic<bool>*, std::atomic<bool>*>> flags_;  // cancel, force
};

// One run of a workflow (a selected workflow or a called reusable workflow).
struct WorkflowRun {
  std::shared_ptr<const workflow::Workflow> workflow;
  std::string file;           // display path
  std::string run_id;
  std::string run_number = "1";
  std::string name;           // github.workflow
  std::string workflow_ref;   // github.workflow_ref
  expr::Value payload;        // github.event
  expr::Value inputs;         // inputs context (object)
  std::map<std::string, std::string> secrets;
  expr::Value vars;           // vars context (object)
  std::string key_prefix;     // "" or "caller / "
  int depth = 0;
  fs::path artifacts;         // artifact store of the top-level run
  fs::path repo_root;         // root for local `uses: ./...` (the snapshot is used for checkouts)
  std::vector<fs::path> call_stack;  // canonical workflow files in this call chain
};

struct JobInstance {
  RunState* run = nullptr;
  const WorkflowRun* workflow = nullptr;
  const workflow::Job* job = nullptr;
  std::string key;           // unique key (prefix + id + matrix suffix)
  std::string name;          // display name
  expr::Value matrix;        // null when the job has no matrix
  expr::Value strategy;      // strategy context
  expr::Value needs;         // needs context
  std::atomic<bool> cancel{false};
  std::atomic<bool> force{false};
};

// Runs the steps of a job instance (jobs without `uses`).
JobReport run_job_instance(JobInstance& instance);

// The github context of a job.
expr::Value github_context(const RunState& run, const WorkflowRun& wr, const std::string& job_id,
                           const fs::path& workspace, const fs::path& event_path);

// hashFiles(): SHA-256 of the SHA-256 digests of the matching files (sorted),
// or "" when no file matches.
std::string hash_files(const fs::path& root, const std::vector<std::string>& patterns);

// File system friendly name for a job key.
std::string sanitize_key(std::string_view key);

// Status functions of a job condition.
struct JobStatus {
  bool success = true;    // all ancestors succeeded
  bool failure = false;   // an ancestor failed
  bool cancelled = false;
};
std::function<bool(std::string_view)> job_status_functions(const JobStatus& status);

// Converts string values to an object value.
expr::Value to_object(const std::map<std::string, std::string>& values);

}  // namespace cpa::engine
