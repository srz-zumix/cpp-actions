#include "engine/cleanup.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <optional>
#include <system_error>
#include <tuple>

#include "util/fs.hpp"
#include "util/strings.hpp"

namespace cpa::engine {

namespace {

constexpr std::size_t kTempSuffixLength = 6;  // mkdtemp's XXXXXX

std::optional<struct stat> lstat_path(const fs::path& path) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) return std::nullopt;
  return st;
}

bool is_real_directory(const fs::path& path) {
  const auto st = lstat_path(path);
  return st && S_ISDIR(st->st_mode);
}

class Fd {
 public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

// Reads a small regular file without following a symlink in its last component.
std::optional<std::string> read_manifest(const fs::path& path, std::string& error) {
  Fd fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (fd.get() < 0) {
    if (errno == ENOENT) return std::nullopt;
    error = "cannot open " + path.string() + ": " + std::strerror(errno);
    return std::nullopt;
  }
  struct stat st {};
  if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() || st.st_size > 4096) {
    error = "unexpected manifest file: " + path.string();
    return std::nullopt;
  }
  std::string content(static_cast<std::size_t>(st.st_size), '\0');
  std::size_t off = 0;
  while (off < content.size()) {
    const auto n = ::read(fd.get(), content.data() + off, content.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    off += static_cast<std::size_t>(n);
  }
  content.resize(off);
  return std::string(util::trim(content));
}

struct Target {
  fs::path path;
  bool exists = false;
};

// Validates a recorded directory created by mkdtemp("<prefix>XXXXXX") directly
// beneath temp_root. With nested, the recorded path is its "work" child.
std::optional<Target> resolve_recorded(const std::string& recorded, const fs::path& temp_root,
                                       std::string_view prefix, bool nested, std::string& error) {
  const fs::path recorded_path(recorded);
  const auto invalid = [&] {
    error = "refusing unexpected recorded directory '" + recorded + "'";
    return std::nullopt;
  };
  if (recorded.empty() || !recorded_path.is_absolute() || recorded_path != recorded_path.lexically_normal()) {
    return invalid();
  }
  fs::path dir = recorded_path;
  if (nested) {
    if (recorded_path.filename() != "work") return invalid();
    dir = recorded_path.parent_path();
  }
  const auto name = dir.filename().string();
  if (dir.parent_path() != temp_root || name.size() != prefix.size() + kTempSuffixLength ||
      !name.starts_with(prefix) ||
      !std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
                   [](unsigned char c) { return std::isalnum(c) != 0; })) {
    return invalid();
  }
  const auto st = lstat_path(dir);
  if (!st) {
    if (errno == ENOENT) return Target{dir, false};
    error = "cannot inspect " + dir.string() + ": " + std::strerror(errno);
    return std::nullopt;
  }
  if (!S_ISDIR(st->st_mode) || st->st_uid != ::getuid()) {
    error = "refusing " + dir.string() + ": not a directory owned by the current user";
    return std::nullopt;
  }
  return Target{dir, true};
}

std::vector<fs::path> real_subdirectories(const fs::path& dir) {
  std::vector<fs::path> out;
  if (!is_real_directory(dir)) return out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (is_real_directory(it->path())) out.push_back(it->path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

RunLock::RunLock(const fs::path& run_dir) {
  const auto path = run_dir / kRunLockFile;
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "open " + path.string());
  if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    const int error = errno;
    ::close(fd_);
    fd_ = -1;
    throw std::system_error(error, std::generic_category(), "lock " + path.string());
  }
}

RunLock::~RunLock() {
  if (fd_ >= 0) ::close(fd_);
}

fs::path default_retained_temp_root() {
#if defined(__APPLE__)
  return util::real_path("/private/tmp");
#else
  return util::real_path("/tmp");
#endif
}

bool has_retained_data(const fs::path& run_dir) {
  for (const char* name : {"jobs", "snapshot"}) {
    if (is_real_directory(run_dir / name)) return true;
  }
  return false;
}

CleanReport clean_retained_run(const fs::path& run_dir, const CleanOptions& options) {
  CleanReport report;
  report.run_dir = run_dir;
  const auto skip = [&](std::string reason) {
    report.skipped = true;
    report.skip_reason = std::move(reason);
    return report;
  };

  const auto st = lstat_path(run_dir);
  if (!st || !S_ISDIR(st->st_mode) || st->st_uid != ::getuid() || (st->st_mode & 077) != 0 ||
      !is_real_directory(run_dir / "logs")) {
    report.errors.push_back("not a cpp-actions run directory: " + run_dir.string());
    return report;
  }
  const auto rd = util::real_path(run_dir);
  report.run_dir = rd;

  Fd lock(::open((rd / kRunLockFile).c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC));
  if (lock.get() >= 0) {
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK) return skip("the run is still in progress");
      report.errors.push_back("cannot lock " + (rd / kRunLockFile).string() + ": " + std::strerror(errno));
      return report;
    }
  } else if (errno != ENOENT) {
    report.errors.push_back("cannot open " + (rd / kRunLockFile).string() + ": " + std::strerror(errno));
    return report;
  }
  if (!options.force && !lstat_path(rd / "report.json")) {
    return skip("report.json is missing (the run may be in progress or was interrupted; use --force)");
  }
  if (!has_retained_data(rd)) return skip("no retained work directories");

  const auto temp_root = options.temp_root.empty() ? default_retained_temp_root() : util::real_path(options.temp_root);
  const auto remove = [&](const fs::path& path) {
    if (!options.dry_run) util::remove_tree(path);
    report.removed.push_back(path);
  };

  std::vector<fs::path> removed_jobs;
  bool failed = false;
  for (const auto& workflow_dir : real_subdirectories(rd / "jobs")) {
    for (const auto& job_dir : real_subdirectories(workflow_dir)) {
      std::vector<Target> targets;
      bool job_ok = true;
      for (const auto& [manifest, prefix, nested] :
           {std::tuple{"work-path", "cpa-w-", true}, std::tuple{"unity-tmp-path", "cpa-u-", false}}) {
        std::string error;
        const auto recorded = read_manifest(job_dir / manifest, error);
        if (!recorded) {
          if (!error.empty()) {
            report.errors.push_back(error);
            job_ok = false;
          }
          continue;
        }
        auto target = resolve_recorded(*recorded, temp_root, prefix, nested, error);
        if (!target) {
          report.errors.push_back((job_dir / manifest).string() + ": " + error);
          job_ok = false;
        } else if (target->exists) {
          targets.push_back(std::move(*target));
        }
      }
      for (const auto& target : targets) {
        if (!job_ok) break;
        try {
          remove(target.path);
        } catch (const std::exception& e) {
          report.errors.push_back(e.what());
          job_ok = false;
        }
      }
      if (!job_ok) {
        failed = true;
        continue;
      }
      removed_jobs.push_back(job_dir);
    }
  }

  const auto remove_top = [&](const char* name) {
    if (!is_real_directory(rd / name)) return;
    try {
      remove(rd / name);
    } catch (const std::exception& e) {
      report.errors.push_back(e.what());
    }
  };
  remove_top("snapshot");
  if (!failed) {
    remove_top("jobs");
    remove_top("control");
    return report;
  }
  // Keep the metadata of failed jobs so that the cleanup can be retried.
  for (const auto& job_dir : removed_jobs) {
    const auto control = rd / "control" / job_dir.parent_path().filename() / job_dir.filename();
    try {
      if (is_real_directory(control)) remove(control);
      remove(job_dir);
    } catch (const std::exception& e) {
      report.errors.push_back(e.what());
    }
  }
  return report;
}

}  // namespace cpa::engine
