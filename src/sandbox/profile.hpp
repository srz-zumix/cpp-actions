#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::sandbox {

namespace fs = std::filesystem;

struct ProfileOptions {
  std::vector<fs::path> read;         // additional read-only directories
  std::vector<fs::path> write;        // read-write directories
  std::vector<fs::path> scoped_read;  // prevalidated paths; do not resolve symlinks again
  std::vector<fs::path> scoped_write;
  int proxy_port = 0;            // outbound connections to localhost:<port> (0 = none)
  bool allow_localhost = false;  // connect to loopback; listen on any port (all interfaces)
  bool allow_apple_security = false;
  bool allow_msbuild = false;
  bool allow_unity = false;
  bool allow_xcode = false;
  fs::path unity_home;          // job HOME used in Unity log mutex names
  fs::path unity_tmp;
  fs::path docker_socket;       // prevalidated canonical socket for this invocation
  bool allow_xcode_simulator = false;
  std::string extra_rules;       // appended verbatim (SBPL)
};

// Seatbelt (SBPL) profile for a step: everything is denied by default; the
// system directories are readable, only the given directories are writable
// and the network is limited to the local proxy. The real home directory is
// not readable unless it contains an allowed directory.
std::string generate_profile(const ProfileOptions& options);

// Quotes a string for SBPL ("..." with \ and " escaped).
std::string sbpl_quote(std::string_view s);
void validate_scoped_path(const fs::path& path);

// Per-user temporary and cache directories of macOS (confstr), empty elsewhere.
std::vector<fs::path> darwin_user_dirs();

}  // namespace cpa::sandbox
