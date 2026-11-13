#include "sandbox/hosted_python.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>

#include "sandbox/profile.hpp"
#include "util/fs.hpp"
#include "util/strings.hpp"

namespace cpa::sandbox {

namespace {

struct Descriptor {
  int fd = -1;
  ~Descriptor() {
    if (fd >= 0) ::close(fd);
  }
};

int directory_open_flags() {
#if defined(O_SEARCH)
  constexpr int access = O_SEARCH;
#elif defined(O_PATH)
  constexpr int access = O_PATH;
#else
  constexpr int access = O_RDONLY;
#endif
  return access | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
}

[[noreturn]] void cache_directory_error(std::string_view operation, const fs::path& path, int error) {
  throw std::runtime_error("Unable to " + std::string(operation) + " setup-python host tool cache directory '" +
                           path.string() + "': " + std::error_code(error, std::generic_category()).message() +
                           "; provision it with appropriate permissions outside cpp-actions. " +
                           hosted_python_provisioning_hint());
}

void reject_symlink(int parent, const fs::path& name, const fs::path& path) {
  struct stat status{};
  if (::fstatat(parent, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(status.st_mode)) {
    throw std::runtime_error("setup-python host tool cache must not contain symlinked path components: " +
                             path.string());
  }
}

}  // namespace

std::string hosted_python_provisioning_hint(std::string_view version, std::string_view architecture) {
  const auto parts = util::split(version, '.');
  const bool numeric =
      (parts.size() == 2 || parts.size() == 3) && parts.front() == "3" &&
      std::all_of(parts.begin(), parts.end(), [](const auto& part) {
        return !part.empty() && std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; });
      });
  auto command = "bash scripts/install-hosted-python.sh --version " +
                 util::shell_quote(numeric ? version : std::string_view("<VERSION>"));
  if (architecture == "arm64" || architecture == "x64") {
    command += " --architecture " + std::string(architecture);
  }
  command += " --dry-run";
  return "Pre-provision macOS Python from a host terminal, not setup-steps. From a cpp-actions checkout: " + command +
         ". Review --help before --accept-system-changes: the script requires administrator privileges, "
         "changes the host and refuses existing conflicts. It supports stable CPython only. "
         "Installed copies are under <prefix>/share/cpp-actions/scripts/. "
         "Script: https://github.com/srz-zumix/cpp-actions/blob/main/scripts/install-hosted-python.sh";
}

void require_disjoint_from_writable(const fs::path& path, const std::vector<fs::path>& writable,
                                    std::string_view description) {
  const auto protected_path = util::real_path(path);
  for (const auto& root : writable) {
    const auto allowed = util::real_path(root);
    if (util::is_subpath(protected_path, allowed) || util::is_subpath(allowed, protected_path)) {
      throw std::runtime_error(std::string(description) + " overlaps a writable directory: " + allowed.string());
    }
  }
}

void validate_hosted_python_cache_path(const fs::path& cache) {
  if (!cache.is_absolute() || cache == cache.root_path() || !cache.has_filename() ||
      cache != cache.lexically_normal()) {
    throw std::invalid_argument("setup-python host tool cache must be a normalized absolute directory");
  }
  validate_scoped_path(cache);
}

bool ensure_hosted_python_cache(const fs::path& cache, const std::vector<fs::path>& writable) {
  validate_hosted_python_cache_path(cache);
  require_disjoint_from_writable(cache, writable, "setup-python host tool cache");

  Descriptor directory;
  directory.fd = ::open(cache.root_path().c_str(), directory_open_flags());
  if (directory.fd < 0) cache_directory_error("open", cache.root_path(), errno);
  bool created = false;
  auto current = cache.root_path();
  for (const auto& component : cache.relative_path()) {
    current /= component;
    int next = ::openat(directory.fd, component.c_str(), directory_open_flags());
    if (next < 0) {
      const int error = errno;
      reject_symlink(directory.fd, component, current);
      if (error != ENOENT) cache_directory_error("open", current, error);
      if (::mkdirat(directory.fd, component.c_str(), 0777) == 0) {
        created = true;
      } else if (errno != EEXIST) {
        cache_directory_error("create", current, errno);
      }
      next = ::openat(directory.fd, component.c_str(), directory_open_flags());
      if (next < 0) {
        const int reopen_error = errno;
        reject_symlink(directory.fd, component, current);
        cache_directory_error("open", current, reopen_error);
      }
    }
    ::close(directory.fd);
    directory.fd = next;
  }
  validate_hosted_python_cache(cache, writable);
  return created;
}

void validate_hosted_python_cache(const fs::path& cache, const std::vector<fs::path>& writable) {
  validate_hosted_python_cache_path(cache);
  std::error_code ec;
  if (!fs::is_directory(cache, ec)) {
    throw std::runtime_error("setup-python host tool cache is not available: " + cache.string() +
                             "; provision the directory and required Python versions before running. " +
                             hosted_python_provisioning_hint());
  }
  const auto canonical = fs::canonical(cache, ec);
  if (ec || canonical != cache) {
    throw std::runtime_error("setup-python host tool cache must not contain symlinked path components: " +
                             cache.string());
  }
  if (::access(cache.c_str(), R_OK | W_OK | X_OK) != 0) {
    const auto error = std::error_code(errno, std::generic_category());
    throw std::runtime_error("setup-python host tool cache must be readable and writable by the runner: " +
                             cache.string() + ": " + error.message() + ". " + hosted_python_provisioning_hint());
  }
  require_disjoint_from_writable(cache, writable, "setup-python host tool cache");
}

std::optional<fs::path> hosted_python_installation(const fs::path& cache, const fs::path& reported) {
  if (!reported.is_absolute()) return std::nullopt;
  const auto path = reported.lexically_normal();
  if (!util::is_subpath(path, cache)) return std::nullopt;

  const auto relative = path.lexically_relative(cache);
  auto part = relative.begin();
  if (part == relative.end() || (*part != "Python" && *part != "PyPy" && *part != "GraalPy")) {
    throw std::runtime_error("setup-python reported an invalid host tool cache path: " + path.string());
  }
  auto installation = cache / *part++;
  if (part == relative.end()) {
    throw std::runtime_error("setup-python reported an incomplete host tool cache path: " + path.string());
  }
  installation /= *part++;
  if (part == relative.end()) {
    throw std::runtime_error("setup-python reported an incomplete host tool cache path: " + path.string());
  }
  installation /= *part;

  std::error_code ec;
  if (!fs::is_directory(installation, ec)) {
    throw std::runtime_error("setup-python reported a missing Python installation: " + installation.string());
  }
  const auto canonical = fs::canonical(installation, ec);
  if (ec || canonical != installation) {
    throw std::runtime_error("setup-python Python installation must not have symlinked directory components: " +
                             installation.string());
  }
  return installation;
}

}  // namespace cpa::sandbox
