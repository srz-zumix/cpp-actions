#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cpa::sandbox {

namespace fs = std::filesystem;

struct ProfileOptions {
  std::vector<fs::path> read;   // additional read-only directories
  std::vector<fs::path> write;  // read-write directories
  int proxy_port = 0;           // outbound connections to 127.0.0.1:<port> (0 = none)
  bool allow_localhost = false;  // bind and connect on localhost (any port)
  std::string extra_rules;       // appended verbatim (SBPL)
};

// Seatbelt (SBPL) profile for a step: everything is denied by default; the
// system directories are readable, only the given directories are writable
// and the network is limited to the local proxy. The real home directory is
// not readable unless it contains an allowed directory.
std::string generate_profile(const ProfileOptions& options);

// Quotes a string for SBPL ("..." with \ and " escaped).
std::string sbpl_quote(std::string_view s);

// Per-user temporary and cache directories of macOS (confstr), empty elsewhere.
std::vector<fs::path> darwin_user_dirs();

}  // namespace cpa::sandbox
