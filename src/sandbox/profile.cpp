#include "sandbox/profile.hpp"

#include <unistd.h>

#include <climits>

#include "util/fs.hpp"

namespace cpa::sandbox {

namespace {

void append_paths(std::string& out, const std::vector<fs::path>& paths) {
  for (const auto& p : paths) {
    out += "\n  (subpath " + sbpl_quote(util::real_path(p).string()) + ")";
  }
}

constexpr std::string_view kBase = R"((version 1)
(deny default)

; Processes
(allow process-fork)
(allow process-exec)
(allow process-info*)
(allow signal (target same-sandbox))
(allow sysctl-read)
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
  (global-name "com.apple.system.notification_center")
  (global-name "com.apple.system.logger")
  (global-name "com.apple.logd")
  (global-name "com.apple.diagnosticd")
  (global-name "com.apple.analyticsd")
  (global-name "com.apple.bsd.dirhelper")
  (global-name "com.apple.cfprefsd.daemon")
  (global-name "com.apple.cfprefsd.agent")
  (global-name "com.apple.trustd")
  (global-name "com.apple.trustd.agent")
  (global-name "com.apple.SystemConfiguration.configd")
  (global-name "com.apple.CoreServices.coreservicesd")
  (global-name "com.apple.lsd.mapdb")
  (global-name "com.apple.xpc.activity.unmanaged"))

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
  (subpath "/opt")
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
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  out += '"';
  return out;
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
  std::string out(kBase);
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
