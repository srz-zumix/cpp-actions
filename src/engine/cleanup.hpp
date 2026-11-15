#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cpa::engine {

namespace fs = std::filesystem;

// Name of the lock file held exclusively by `run` for the lifetime of a run.
inline constexpr const char* kRunLockFile = "run.lock";

// Holds an exclusive advisory lock on <run>/run.lock until destruction.
class RunLock {
 public:
  explicit RunLock(const fs::path& run_dir);
  ~RunLock();
  RunLock(const RunLock&) = delete;
  RunLock& operator=(const RunLock&) = delete;

 private:
  int fd_ = -1;
};

struct CleanOptions {
  bool dry_run = false;
  // Clean runs without report.json (interrupted runs). Never overrides a held run lock.
  bool force = false;
  // Parent of the cpa-w-* / cpa-u-* directories; empty selects the platform default.
  fs::path temp_root;
};

struct CleanReport {
  fs::path run_dir;
  bool skipped = false;
  std::string skip_reason;
  std::vector<fs::path> removed;  // paths removed (or that would be removed with dry_run)
  std::vector<std::string> errors;
};

fs::path default_retained_temp_root();

// True when the run still has job or snapshot data left by --keep-workdir.
bool has_retained_data(const fs::path& run_dir);

// Removes the work directories and Unity temporary directories recorded by a
// run executed with --keep-workdir, then its jobs/control/snapshot directories.
// Logs, report.json, summary.md and artifacts are kept. Job metadata whose
// directories could not be validated or removed is preserved for a retry.
CleanReport clean_retained_run(const fs::path& run_dir, const CleanOptions& options);

}  // namespace cpa::engine
