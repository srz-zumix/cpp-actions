#include "util/fs.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#include <sys/clonefile.h>
#endif

#include "util/strings.hpp"

namespace cpa::util {

namespace {

[[noreturn]] void throw_errno(const std::string& what, const fs::path& path) {
  throw std::system_error(errno, std::generic_category(), what + ": " + path.string());
}

}  // namespace

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open file: " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::optional<std::string> try_read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file(const fs::path& path, std::string_view content) {
  if (path.has_parent_path()) fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot write file: " + path.string());
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) throw std::runtime_error("cannot write file: " + path.string());
}

void append_file(const fs::path& path, std::string_view content) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) throw std::runtime_error("cannot write file: " + path.string());
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void make_executable(const fs::path& path) {
  fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                  fs::perm_options::add);
}

fs::path real_path(const fs::path& path) {
  char buf[PATH_MAX];
  if (::realpath(path.c_str(), buf) != nullptr) return fs::path(buf);
  // Resolve the longest existing prefix so that e.g. /var -> /private/var is
  // still applied for paths that do not exist yet.
  auto abs = fs::absolute(path).lexically_normal();
  fs::path existing = abs;
  fs::path rest;
  while (!existing.empty() && existing != existing.root_path()) {
    if (::realpath(existing.c_str(), buf) != nullptr) {
      return (fs::path(buf) / rest).lexically_normal();
    }
    rest = existing.filename() / rest;
    existing = existing.parent_path();
  }
  return abs;
}

void copy_entry(const fs::path& from, const fs::path& to) {
  struct stat st{};
  if (::lstat(from.c_str(), &st) != 0) throw_errno("lstat", from);
  if (S_ISLNK(st.st_mode)) {
    std::error_code ec;
    fs::remove(to, ec);
    fs::copy_symlink(from, to);
    return;
  }
  if (!S_ISREG(st.st_mode)) return;  // sockets, fifos, devices are skipped
#if defined(__APPLE__)
  {
    std::error_code ec;
    fs::remove(to, ec);
    if (::clonefile(from.c_str(), to.c_str(), CLONE_NOFOLLOW) == 0) return;
  }
#endif
  const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) throw_errno("open", from);
  const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
  if (out < 0) {
    const int saved = errno;
    ::close(in);
    errno = saved;
    throw_errno("open", to);
  }
  char buf[1 << 16];
  while (true) {
    const auto n = ::read(in, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      const int saved = errno;
      ::close(in);
      ::close(out);
      errno = saved;
      throw_errno("read", from);
    }
    if (n == 0) break;
    ssize_t off = 0;
    while (off < n) {
      const auto w = ::write(out, buf + off, static_cast<std::size_t>(n - off));
      if (w < 0) {
        if (errno == EINTR) continue;
        const int saved = errno;
        ::close(in);
        ::close(out);
        errno = saved;
        throw_errno("write", to);
      }
      off += w;
    }
  }
  ::fchmod(out, st.st_mode & 07777);
  ::close(in);
  ::close(out);
}

void copy_tree(const fs::path& from, const fs::path& to) {
  fs::create_directories(to);
  for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::none);
       it != fs::recursive_directory_iterator(); ++it) {
    const auto rel = fs::relative(it->path(), from);
    const auto dest = to / rel;
    const auto status = it->symlink_status();
    if (fs::is_directory(status)) {
      fs::create_directories(dest);
    } else {
      copy_entry(it->path(), dest);
    }
  }
}

void remove_tree(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(fs::symlink_status(path, ec))) return;
  fs::remove_all(path, ec);
  if (!ec) return;
  // Make directories writable and retry (e.g. read-only module caches).
  for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_directory(ec) && !it->is_symlink(ec)) {
      fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
    }
  }
  fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
  ec.clear();
  fs::remove_all(path, ec);
  if (ec) throw std::system_error(ec, "remove " + path.string());
}

std::optional<fs::path> find_executable(std::string_view name, std::string_view path_env) {
  if (name.find('/') != std::string_view::npos) {
    fs::path p(name);
    if (::access(p.c_str(), X_OK) == 0) return p;
    return std::nullopt;
  }
  for (const auto& dir : split(path_env, ':')) {
    if (dir.empty()) continue;
    fs::path candidate = fs::path(dir) / std::string(name);
    struct stat st{};
    if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return std::nullopt;
}

fs::path home_dir() {
  if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
  if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) return fs::path(pw->pw_dir);
  return fs::path("/");
}

fs::path temp_dir() {
  if (const char* t = std::getenv("TMPDIR"); t && *t) return fs::path(t);
  return fs::path("/tmp");
}

fs::path make_temp_dir(std::string_view prefix, const fs::path& parent) {
  const fs::path base = parent.empty() ? temp_dir() : parent;
  fs::create_directories(base);
  std::string templ = (base / (std::string(prefix) + "XXXXXX")).string();
  std::vector<char> buf(templ.begin(), templ.end());
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) {
    throw std::system_error(errno, std::generic_category(), "mkdtemp " + templ);
  }
  return real_path(fs::path(buf.data()));
}

fs::path user_cache_dir() {
#if defined(__APPLE__)
  return home_dir() / "Library" / "Caches";
#else
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return fs::path(x);
  return home_dir() / ".cache";
#endif
}

bool is_subpath(const fs::path& path, const fs::path& base) {
  const auto rel = path.lexically_normal().lexically_relative(base.lexically_normal());
  if (rel.empty()) return false;
  return *rel.begin() != "..";
}

}  // namespace cpa::util
