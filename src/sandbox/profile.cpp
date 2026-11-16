#include "sandbox/profile.hpp"

#include <unistd.h>
#include <pwd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <set>
#include <stdexcept>
#include <system_error>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace cpa::sandbox {

namespace {

void append_paths(std::string& out, const std::vector<fs::path>& paths) {
  for (const auto& p : paths) {
    if (p.empty()) throw std::invalid_argument("sandbox paths must not be empty");
    out += "\n  (subpath " + sbpl_quote(fs::weakly_canonical(fs::absolute(p)).string()) + ")";
  }
}

void append_scoped_paths(std::string& out, const std::vector<fs::path>& paths) {
  for (const auto& p : paths) {
    validate_scoped_path(p);
    out += "\n  (subpath " + sbpl_quote(p.string()) + ")";
  }
}

void append_rsync_ancestor_read(std::string& out, const std::vector<fs::path>& write,
                                const std::vector<fs::path>& scoped_write) {
  std::set<fs::path> ancestors;
  const auto collect = [&ancestors](const fs::path& path) {
    for (auto parent = path.parent_path(); parent != parent.root_path(); parent = parent.parent_path()) {
      ancestors.insert(parent);
    }
  };
  for (const auto& path : write) collect(fs::weakly_canonical(fs::absolute(path)));
  for (const auto& path : scoped_write) {
    validate_scoped_path(path);
    collect(path);
  }
  if (ancestors.empty()) return;

  out += "\n; Homebrew rsync may inspect writable-path ancestors while resolving destinations\n";
  out += "(allow file-read-data\n  (require-all\n    (require-any";
  for (const auto& path : ancestors) out += "\n      (literal " + sbpl_quote(path.string()) + ")";
  out += R"(
    )
    (process-path-regex #"^/(opt/homebrew|usr/local)/Cellar/rsync/[^/]+/bin/rsync$")
  ))
)";
}

fs::path os_user_home() {
  std::vector<char> buffer(16384);
  passwd user{};
  passwd* result = nullptr;
  int error;
  while ((error = ::getpwuid_r(::getuid(), &user, buffer.data(), buffer.size(), &result)) == ERANGE) {
    buffer.resize(buffer.size() * 2);
  }
  if (error != 0) throw std::system_error(error, std::generic_category(), "Resolving OS user home");
  const fs::path home = result != nullptr && user.pw_dir != nullptr ? user.pw_dir : "";
  if (home.empty() || !home.is_absolute() || home == home.root_path()) {
    throw std::runtime_error("Cannot resolve OS user home for sandbox permissions");
  }
  return home;
}

fs::path developer_disk_images() {
  return os_user_home() / "Library/Developer/DeveloperDiskImages";
}

std::string simulator_launch_service() {
#if defined(__APPLE__)
  // Unlike compile-time architecture, this also identifies ARM hosts under Rosetta.
  int arm64 = 0;
  std::size_t size = sizeof(arm64);
  if (::sysctlbyname("hw.optional.arm64", &arm64, &size, nullptr, 0) != 0 && errno != ENOENT) {
    throw std::system_error(errno, std::generic_category(), "Resolving CoreSimulator host architecture");
  }
  if (arm64) return "com.apple.CoreSimulator.SimLaunchHost-arm64";
#endif
  return "com.apple.CoreSimulator.SimLaunchHost-x86_64";
}

std::string regex_literal(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (std::string_view(R"(\.^$|()[]{}*+?)").find(c) != std::string_view::npos) out += '\\';
    out += c;
  }
  return out;
}

std::string guid_regex() {
  std::string guid;
  for (const int length : {8, 4, 4, 4, 12}) {
    if (!guid.empty()) guid += "-";
    for (int i = 0; i < length; ++i) guid += "[0-9a-fA-F]";
  }
  return guid;
}

fs::path xcode_user_directory([[maybe_unused]] int name, const std::string& label) {
#if defined(__APPLE__)
  char buffer[PATH_MAX];
  errno = 0;
  const auto length = ::confstr(name, buffer, sizeof(buffer));
  if (length == 0) {
    throw std::system_error(errno != 0 ? errno : ENOENT, std::generic_category(), "Resolving macOS Xcode " + label);
  }
  if (length > sizeof(buffer)) throw std::runtime_error("macOS Xcode " + label + " path is too long");
  const fs::path dir(buffer);
  if (!dir.is_absolute() || dir == dir.root_path()) {
    throw std::runtime_error("macOS Xcode " + label + " must be an absolute path below the filesystem root");
  }
  std::error_code ec;
  const auto canonical = fs::canonical(dir, ec);
  if (ec) throw std::system_error(ec, "Resolving macOS Xcode " + label);
  if (canonical == canonical.root_path()) {
    throw std::runtime_error("macOS Xcode " + label + " must not be the filesystem root");
  }
  const auto status = fs::status(canonical, ec);
  if (ec) throw std::system_error(ec, "Inspecting macOS Xcode " + label);
  if (!fs::is_directory(status)) throw std::runtime_error("macOS Xcode " + label + " is not a directory");
  return canonical;
#else
  throw std::runtime_error("macOS Xcode " + label + " is unavailable on this platform");
#endif
}

fs::path xcode_temp_directory() {
#if defined(__APPLE__)
  return xcode_user_directory(_CS_DARWIN_USER_TEMP_DIR, "temporary directory");
#else
  return xcode_user_directory(0, "temporary directory");
#endif
}

fs::path xcode_cache_directory() {
#if defined(__APPLE__)
  return xcode_user_directory(_CS_DARWIN_USER_CACHE_DIR, "cache directory");
#else
  return xcode_user_directory(0, "cache directory");
#endif
}

std::string xcode_rules(bool allow_xcode_simulator) {
  const auto temp = xcode_temp_directory();
  const auto temporary_items = temp / "TemporaryItems";
  const auto temp_regex = "^" + regex_literal(temp.string());
  const auto guid = guid_regex();
  const auto distribution_pipeline = temp_regex + "/XcodeDistPipeline[.][^/]+";
  const auto distribution_pipeline_contents = distribution_pipeline + "(/.*)?$";
  const auto simulator_regex =
      temp_regex + "/com[.]apple[.]CoreSimulator[.]SimDevice[.]" + guid + "[.]Standalone[.]" + guid;
  std::string out = "\n; Opt-in Xcode temporary files (shared with host Xcode processes)\n";
  // Xcode keeps the clang module validation session and SDK stat caches in the user cache
  // directory; build settings cannot redirect both into the job.
  const auto cache = xcode_cache_directory();
  const auto clang_cache = cache / "org.llvm.clang";
  const auto developer_tools_regex = "^" + regex_literal((cache / "com.apple.DeveloperTools").string());
  out += "(allow file-read* file-write* (regex " +
         sbpl_quote("^" + regex_literal(clang_cache.string()) + "/ModuleCache[.]noindex(/.*)?$") + "))\n";
  out += "(allow file-read* (literal " + sbpl_quote(clang_cache.string()) + ") (regex " +
         sbpl_quote(developer_tools_regex + "(/[^/]+(/Xcode)?)?$") + "))\n";
  out += "(allow file-write-create\n  (require-all\n    (vnode-type DIRECTORY)\n    (require-any\n      (literal " +
         sbpl_quote(clang_cache.string()) + ")\n      (regex " +
         sbpl_quote(developer_tools_regex + "(/[^/]+(/Xcode)?)?$") + "))))\n";
  out += "(allow file-read* file-write* (regex " +
         sbpl_quote(developer_tools_regex + "/[^/]+/Xcode/SDKStatCaches[.]noindex(/.*)?$") + "))\n";
  out += "(allow file-read* file-write-create (literal " + sbpl_quote(temporary_items.string()) + "))\n";
  out += "(allow file-read* file-write* (regex " +
         sbpl_quote("^" + regex_literal(temporary_items.string()) +
                    "/NSIRD_(xcodebuild|SWBBuildService|ibtool)_[^/]+(/.*)?$") +
         "))\n";
  out += "(allow file-read* file-write* (regex " + sbpl_quote(temp_regex + "/swbuild[.]tmp[.][^/]+(/.*)?$") + "))\n";
  out +=
      "; Xcode distribution pipeline scratch files, limited to platform binaries\n"
      "(allow file-write-create\n"
      "  (require-all\n"
      "    (regex " +
      sbpl_quote(distribution_pipeline + "$") +
      ")\n"
      "    (signing-identifier \"com.apple.dt.xcodebuild\")\n"
      "    (process-attribute is-platform-binary)\n"
      "    (process-path-regex #\"^/.*/xcodebuild$\")\n"
      "  ))\n";
  out +=
      "(allow file-read* file-write*\n"
      "  (require-all\n"
      "    (regex " +
      sbpl_quote(distribution_pipeline_contents) +
      ")\n"
      "    (signing-identifier \"com.apple.dt.xcodebuild\")\n"
      "    (process-attribute is-platform-binary)\n"
      "    (process-path-regex #\"^/.*/xcodebuild$\")\n"
      "  ))\n"
      "(allow file-read* file-write*\n"
      "  (require-all\n"
      "    (regex " +
      sbpl_quote(distribution_pipeline_contents) +
      ")\n"
      "    (process-attribute is-platform-binary)\n"
      "    (process-path-regex #\"^/usr/bin/ditto$\")\n"
      "  ))\n"
      "(allow file-read* file-write*\n"
      "  (require-all\n"
      "    (regex " +
      sbpl_quote(distribution_pipeline_contents) +
      ")\n"
      "    (process-attribute is-platform-binary)\n"
      "    (process-path-regex #\"^/usr/bin/codesign$\")\n"
      "  ))\n";
  out += "(allow file-read* file-write* (regex " + sbpl_quote(temp_regex + "/" + guid + "[.]storyboard$") + "))\n";
  out += "(allow file-read* file-write* (regex " + sbpl_quote(temp_regex + "/ibtoold-[0-9]+(/IB(/.*)?)?$") + "))\n";
  out +=
      "; Asset compilation scratch files, limited to the platform-signed Xcode worker\n"
      "(allow file-read* file-write-create file-write-data file-write-unlink\n"
      "  (require-all\n"
      "    (require-any\n"
      "      (regex " +
      sbpl_quote(temp_regex + "/" + guid + "(/.*)?$") +
      ")\n"
      "      (regex " +
      sbpl_quote(temp_regex + "/actool-sprite-atlas-scratch-" + guid + "(/.*)?$") +
      ")\n"
      "    )\n"
      "    (signing-identifier \"com.apple.ibtool\")\n"
      "    (process-attribute is-platform-binary)\n"
      "    (process-path-regex #\"^/.*/ibtoold$\")\n"
      "  ))\n";
  if (allow_xcode_simulator) {
    // Seatbelt rejects a single regex long enough to contain three expanded UUIDs.
    out +=
        "; Asset catalog Simulator worker scratch files\n"
        "(allow file-read* file-write-create file-write-data file-write-unlink\n"
        "  (require-all\n"
        "    (regex " +
        sbpl_quote(simulator_regex + "/[^/]+(/.*)?$") +
        ")\n"
        "    (require-any\n"
        "      (regex " +
        sbpl_quote(temp_regex + "/[^/]+/" + guid + "(/.*)?$") +
        ")\n"
        "      (regex " +
        sbpl_quote(temp_regex + "/[^/]+/Untitled-[a-zA-Z0-9]+[.]uicatalog(-journal)?$") +
        "))\n"
        "    (signing-identifier \"com.apple.dt.AssetCatalogSimulatorAgent\")\n"
        "    (process-attribute is-platform-binary)\n"
        "    (process-path-regex #\"^/.*/AssetCatalogSimulatorAgent$\")\n"
        "  ))\n";
    out += "(allow file-read* file-write* (regex " + sbpl_quote(simulator_regex + "$") + "))\n";
  }
  return out;
}

std::string unity_rules(const fs::path& job_home) {
  if (job_home.empty() || !job_home.is_absolute()) {
    throw std::invalid_argument("Unity permissions require an absolute job HOME");
  }
  const auto home = fs::weakly_canonical(job_home);
  if (home == home.root_path()) throw std::invalid_argument("Unity job HOME must not be the filesystem root");
  auto encoded_home = home.string();
  std::replace(encoded_home.begin(), encoded_home.end(), '/', '_');
  const auto log_mutex = regex_literal(encoded_home) +
                        "_Library_Logs_Unity_Unity[.](Licensing[.]Client|Entitlements[.]Audit)[.]log_rolling";
  std::string out = "\n; Opt-in Unity temporary files and IPC (shared with host Unity processes)\n";
  out += "; Host Unity editor cache (shared with host Unity processes)\n(allow file-read* file-write*";
  append_paths(out, {os_user_home() / "Library/Caches/com.unity3d.UnityEditor"});
  out += ")\n";
  out += "(allow file-read* file-write* (literal \"/private/tmp/unitypreflock\"))\n";
  // The Licensing Client's machine bindings include the Wi-Fi MAC address via CoreWLAN,
  // and .NET network-interface queries read the resolver configuration.
  out += "(allow mach-lookup (global-name \"com.apple.airportd\"))\n";
  out += "(allow file-read* (literal \"/private/var/run/resolv.conf\"))\n";
  out += "(allow file-read-data (literal \"/private/tmp\"))\n";
  const auto user_dirs = darwin_user_dirs();
  if (user_dirs.empty()) throw std::runtime_error("Cannot resolve macOS Unity temporary directory");
  const auto temporary = fs::weakly_canonical(user_dirs.front()) / "TemporaryItems";
  out += "(allow file-read* file-write-create (literal " + sbpl_quote(temporary.string()) + "))\n";
  out += "(allow file-read* file-write* (regex " +
         sbpl_quote("^" + regex_literal(temporary.string()) + "/NSIRD_Unity_[^/]+(/.*)?$") + "))\n";
  // CoreCLR creates shared-memory directories via mkdtemp + chmod + rename from /private/tmp/.coreclr.XXXXXX.
  out += R"(
(allow file-read* file-write-create file-write-mode file-write-unlink
  (regex #"^/private/tmp/[.]coreclr[.][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]$"))
(allow file-read* file-write-create file-write-mode
  (literal "/private/tmp/.dotnet")
  (literal "/private/tmp/.dotnet/shm")
  (literal "/private/tmp/.dotnet/lockfiles")
  (regex #"^/private/tmp/[.]dotnet/(shm|lockfiles)/(global|session[0-9]+)$"))
(allow file-read* file-write*
  (regex #"^/private/tmp/[.]dotnet/(shm|lockfiles)/global/Unity[.]Licensing[.]Client[.]Pipe[.]Unity-LicenseClient-runner(-[0-9]+[.][0-9]+[.][0-9]+)?$")
)";
  out += "  (regex " + sbpl_quote("^/private/tmp/[.]dotnet/(shm|lockfiles)/session[0-9]+/" + log_mutex + "$") + "))\n";
  // Floating license leases are guarded by a global mutex named after the lease GUID.
  out += "(allow file-read* file-write* (regex #" +
         sbpl_quote("^/private/tmp/[.]dotnet/(shm|lockfiles)/global/" + guid_regex() + "$") + "))\n";
  std::string sockets =
      R"(^/private/tmp/((Unity-LicenseClient-runner(-[0-9]+[.][0-9]+[.][0-9]+)?(-notifications)?|Unity-Upm-[0-9]+|)"
      R"(Unity-ShaderCompilerIPC-[0-9]+-[0-9]+)[.]sock|ilpp[.]sock-)";
  // Seatbelt regexes do not implement counted repetitions.
  for (int i = 0; i < 32; ++i) sockets += "[0-9a-f]";
  sockets += ")$";
  out += "(allow file-read* file-write* (regex #" + sbpl_quote(sockets) + "))\n";
  // The shader compiler IPC listener is guarded by a lock file next to its socket.
  out += "(allow file-read* file-write* (regex #\"^/private/tmp/Unity-ShaderCompilerIPC-[0-9]+-[0-9]+[.]lock$\"))\n";
  out += "(allow network-bind network-inbound network-outbound (local unix-socket (regex #" +
         sbpl_quote(sockets) + ")))\n";
  out += "(allow network-outbound (remote unix-socket (regex #" + sbpl_quote(sockets) + ")))\n";
  // Java/Gradle requires wildcard socket permissions even when advertising loopback.
  out += "(allow network-bind network-inbound (local ip \"*:*\"))\n";
  out += "(allow network-outbound (remote ip \"localhost:*\"))\n";
  return out;
}

constexpr std::string_view kBase = R"((version 1)
(deny default)

; Processes
(allow process-fork)
(allow process-exec)
(allow process-info* (target same-sandbox))
(allow signal (target same-sandbox))
(allow sysctl-read)
(deny sysctl-read
  (sysctl-name "kern.procargs")
  (sysctl-name "kern.procargs2"))
(allow ipc-posix*)
(allow ipc-sysv*)
(allow system-socket)
(allow pseudo-tty)
(allow file-ioctl)
(deny file-write-setugid)

; System services (no keychain, LaunchServices or Apple events)
(allow mach-lookup
  (global-name "com.apple.system.opendirectoryd.libinfo")
  (global-name "com.apple.system.opendirectoryd.membership")
  (global-name "com.apple.system.logger")
  (global-name "com.apple.logd")
  (global-name "com.apple.bsd.dirhelper")
  (global-name "com.apple.trustd")
  (global-name "com.apple.SystemConfiguration.configd"))

; Files: metadata everywhere, contents of the system directories only
(allow file-read-metadata)
(allow file-read*
  (literal "/")
  (subpath "/bin")
  (subpath "/sbin")
  (subpath "/usr")
  (subpath "/System")
  (subpath "/Library")
  (subpath "/Applications")
  (subpath "/opt/homebrew")
  (subpath "/private/etc")
  (subpath "/private/var/select")
  (subpath "/private/var/db/timezone")
  (subpath "/private/var/db/xcode_select_link")
  (subpath "/dev"))
(deny file-read* file-write*
  (subpath "/Library/Keychains")
  (subpath "/private/etc/ssh")
  (subpath "/private/etc/sudoers.d")
  (literal "/private/etc/sudoers")
  (literal "/private/etc/master.passwd"))
(allow file-write*
  (literal "/dev/null")
  (literal "/dev/zero")
  (literal "/dev/tty")
  (literal "/dev/dtracehelper")
  (literal "/dev/ptmx")
  (regex #"^/dev/ttys[0-9]+$")
  (subpath "/dev/fd"))
)";

}  // namespace

std::string sbpl_quote(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      throw std::invalid_argument("sandbox paths must not contain control characters");
    }
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

void validate_scoped_path(const fs::path& path) {
  if (!path.is_absolute() || path == path.root_path() || path != path.lexically_normal()) {
    throw std::invalid_argument("scoped sandbox paths must be normalized absolute paths below the filesystem root");
  }
  (void)sbpl_quote(path.string());
}

std::vector<fs::path> darwin_user_dirs() {
  std::vector<fs::path> dirs;
#if defined(__APPLE__)
  for (int name : {_CS_DARWIN_USER_TEMP_DIR, _CS_DARWIN_USER_CACHE_DIR}) {
    char buf[PATH_MAX];
    const auto n = ::confstr(name, buf, sizeof(buf));
    if (n > 0 && n <= sizeof(buf)) dirs.emplace_back(buf);
  }
#endif
  return dirs;
}

std::string generate_profile(const ProfileOptions& options) {
  if (options.proxy_port < 0 || options.proxy_port > 65535) {
    throw std::invalid_argument("invalid sandbox proxy port");
  }
  std::string out(kBase);
  if (options.allow_unity) out += unity_rules(options.unity_home);
  if (options.allow_xcode) out += xcode_rules(options.allow_xcode_simulator);
  if (options.allow_unity && !options.unity_tmp.empty()) {
    out += "\n; Short job-specific Unity temporary directory\n(allow file-read* file-write*";
    append_paths(out, {options.unity_tmp});
    out += ")\n";
    out += "(allow network-bind network-inbound network-outbound (local unix-socket (subpath " +
           sbpl_quote(fs::weakly_canonical(options.unity_tmp).string()) + ")))\n";
    out += "(allow network-outbound (remote unix-socket (subpath " +
           sbpl_quote(fs::weakly_canonical(options.unity_tmp).string()) + ")))\n";
  }
  if (options.allow_msbuild) {
    out += R"(
; Opt-in MSBuild worker IPC (host MSBuild sockets may also be accessible)
(allow file-read* file-write* (regex #"^/private/tmp/MSBuild[0-9]+$"))
(allow network-bind network-inbound network-outbound
  (local unix-socket (regex #"^/private/tmp/MSBuild[0-9]+$")))
(allow network-outbound
  (remote unix-socket (regex #"^/private/tmp/MSBuild[0-9]+$")))
)";
  }
  if (options.allow_apple_security) {
    out += "\n; Opt-in Apple security services (not limited to TLS)\n"
           "(allow mach-lookup\n"
           "  (global-name \"com.apple.SecurityServer\")\n"
           "  (global-name \"com.apple.trustd.agent\"))\n";
  }
  if (options.allow_xcode_simulator) {
    out += "\n; Opt-in Xcode/Simulator compatibility (permits host Simulator operations)\n"
           "(allow mach-lookup\n"
           "  (global-name \"com.apple.CoreSimulator.CoreSimulatorService\")\n"
           "  (global-name \"com.apple.CoreSimulator.simdiskimaged\")\n"
           "  (global-name " + sbpl_quote(simulator_launch_service()) + "))\n"
           "(allow file-read*";
    append_paths(out, {developer_disk_images()});
    out += ")\n";
  }
  if (!options.read.empty()) {
    out += "\n; Additional readable directories\n(allow file-read*";
    append_paths(out, options.read);
    out += ")\n";
  }
  if (!options.write.empty()) {
    out += "\n; Writable directories\n(allow file-read* file-write*";
    append_paths(out, options.write);
    out += ")\n";
  }
  if (!options.scoped_read.empty()) {
    out += "\n; Invocation-specific read-only directories\n(allow file-read*";
    append_scoped_paths(out, options.scoped_read);
    out += ")\n";
  }
  if (!options.scoped_write.empty()) {
    out += "\n; Invocation-specific writable directories\n(allow file-read* file-write*";
    append_scoped_paths(out, options.scoped_write);
    out += ")\n";
  }
  append_rsync_ancestor_read(out, options.write, options.scoped_write);
  if (!options.docker_socket.empty()) {
    validate_scoped_path(options.docker_socket);
    out += "\n; Opt-in Docker daemon access\n(allow network-outbound (remote unix-socket (literal " +
           sbpl_quote(options.docker_socket.string()) + ")))\n";
  }
  out += "\n; Network\n";
  if (options.proxy_port > 0) {
    out += "(allow network-outbound (remote ip \"localhost:" + std::to_string(options.proxy_port) + "\"))\n";
  }
  if (options.allow_localhost) {
    out += "(allow network-outbound (remote ip \"localhost:*\"))\n";
    out += "(allow network-bind network-inbound (local ip \"localhost:*\"))\n";
  }
  if (!options.extra_rules.empty()) {
    out += "\n; Additional rules\n";
    out += options.extra_rules;
    if (!options.extra_rules.ends_with("\n")) out += "\n";
  }
  return out;
}

}  // namespace cpa::sandbox
