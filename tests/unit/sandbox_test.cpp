#include "iutest.hpp"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <sstream>

#include "engine/runner.hpp"
#include "engine/commands.hpp"
#include "engine/job.hpp"
#include "sandbox/profile.hpp"
#include "sandbox/sandbox.hpp"
#include "test_support.hpp"
#include "util/fs.hpp"
#include "util/process.hpp"

#if defined(__APPLE__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <unistd.h>
#include <pwd.h>
#endif

using namespace cpa;

IUTEST(SandboxProfileTest, DefaultDenyAndProxyOnly) {
  sandbox::ProfileOptions options;
  options.proxy_port = 12345;
  const auto profile = sandbox::generate_profile(options);
  IUTEST_EXPECT_NE(std::string::npos, profile.find("(deny default)"));
  IUTEST_EXPECT_NE(std::string::npos, profile.find("localhost:12345"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("localhost:*"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("(subpath \"/Users\")"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("(subpath \"/opt\")"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("(global-name \"com.apple.SecurityServer\")"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("(global-name \"com.apple.trustd.agent\")"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("com.apple.CoreSimulator."));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("DeveloperDiskImages"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("MSBuild[0-9]"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("com.unity3d.UnityEditor"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("; Opt-in Docker daemon access"));
}

IUTEST(SandboxProfileTest, DockerGrantOnlyAllowsTheSpecifiedSocketConnection) {
  sandbox::ProfileOptions options;
  options.docker_socket = "/private/tmp/docker-test.sock";
  const auto profile = sandbox::generate_profile(options);
  const auto begin = profile.find("; Opt-in Docker daemon access");
  IUTEST_ASSERT_NE(std::string::npos, begin);
  const auto end = profile.find("; Network", begin);
  IUTEST_ASSERT_NE(std::string::npos, end);
  const auto section = profile.substr(begin, end - begin);
  IUTEST_EXPECT_NE(std::string::npos, section.find(
      "(allow network-outbound (remote unix-socket (literal \"/private/tmp/docker-test.sock\")))"));
  IUTEST_EXPECT_EQ(std::string::npos, section.find("file-read"));
  IUTEST_EXPECT_EQ(std::string::npos, section.find("file-write"));
  IUTEST_EXPECT_EQ(std::string::npos, section.find("network-inbound"));
  IUTEST_EXPECT_EQ(std::string::npos, section.find("(subpath "));
  options.docker_socket = "relative/socket";
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::invalid_argument);
  options.docker_socket = "/";
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::invalid_argument);
}

IUTEST(SandboxProfileTest, UnityRequiresAnExplicitJobHome) {
  sandbox::ProfileOptions options;
  IUTEST_EXPECT_EQ(std::string::npos, sandbox::generate_profile(options).find("unitypreflock"));
  IUTEST_EXPECT_EQ(std::string::npos, sandbox::generate_profile(options).find("ilpp[.]sock-"));
  options.allow_unity = true;
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::invalid_argument);
  options.unity_home = "/";
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::invalid_argument);
  options.unity_home = "relative/home";
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::invalid_argument);
}

IUTEST(SandboxProfileTest, MsbuildDoesNotGrantGeneralTmpOrNetworkAccess) {
  sandbox::ProfileOptions options;
  options.allow_msbuild = true;
  const auto profile = sandbox::generate_profile(options);
  IUTEST_EXPECT_NE(std::string::npos, profile.find("^/private/tmp/MSBuild[0-9]+$"));
  IUTEST_EXPECT_NE(std::string::npos, profile.find("(remote unix-socket"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("(subpath \"/private/tmp\")"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("(allow network-outbound)"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("(global-name \"com.apple.SecurityServer\")"));
}

IUTEST(SandboxProfileTest, XcodeTemporaryFilesRequireExplicitOptIn) {
  sandbox::ProfileOptions options;
  IUTEST_EXPECT_EQ(std::string::npos, sandbox::generate_profile(options).find("; Opt-in Xcode temporary files"));
  IUTEST_EXPECT_EQ(std::string::npos, sandbox::generate_profile(options).find("XcodeDistPipeline"));
  IUTEST_EXPECT_EQ(std::string::npos, sandbox::generate_profile(options).find("ModuleCache"));
  IUTEST_EXPECT_EQ(std::string::npos, sandbox::generate_profile(options).find("SDKStatCaches"));
  options.allow_xcode = true;
#if defined(__APPLE__)
  const auto profile = sandbox::generate_profile(options);
  const auto begin = profile.find("; Opt-in Xcode temporary files");
  IUTEST_ASSERT_NE(std::string::npos, begin);
  const auto end = profile.find("; Network", begin);
  IUTEST_ASSERT_NE(std::string::npos, end);
  const auto section = profile.substr(begin, end - begin);
  const auto pipeline_begin = section.find("; Xcode distribution pipeline scratch files");
  IUTEST_ASSERT_NE(std::string::npos, pipeline_begin);
  const auto pipeline_end = section.find("; Asset compilation scratch files", pipeline_begin);
  IUTEST_ASSERT_NE(std::string::npos, pipeline_end);
  const auto pipeline = section.substr(pipeline_begin, pipeline_end - pipeline_begin);
  IUTEST_EXPECT_NE(std::string::npos, pipeline.find("XcodeDistPipeline[.][^/]+$"));
  IUTEST_EXPECT_NE(std::string::npos, pipeline.find("XcodeDistPipeline[.][^/]+(/.*)?$"));
  IUTEST_EXPECT_NE(std::string::npos, pipeline.find(
      "(signing-identifier \"com.apple.dt.xcodebuild\")\n"
      "    (process-attribute is-platform-binary)\n"
      "    (process-path-regex #\"^/.*/xcodebuild$\")"));
  IUTEST_EXPECT_NE(std::string::npos, pipeline.find(
      "(process-attribute is-platform-binary)\n"
      "    (process-path-regex #\"^/usr/bin/ditto$\")"));
  IUTEST_EXPECT_NE(std::string::npos, pipeline.find(
      "(process-attribute is-platform-binary)\n"
      "    (process-path-regex #\"^/usr/bin/codesign$\")"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("NSIRD_(xcodebuild|SWBBuildService|ibtool)_"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("swbuild[.]tmp[.]"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("[.]storyboard$"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("ibtoold-[0-9]+(/IB(/.*)?)?$"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("actool-sprite-atlas-scratch-"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("(signing-identifier \"com.apple.ibtool\")"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("(process-attribute is-platform-binary)"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("(process-path-regex #\"^/.*/ibtoold$\")"));
  IUTEST_EXPECT_EQ(std::string::npos, section.find("AssetCatalogSimulatorAgent"));
  IUTEST_EXPECT_EQ(std::string::npos, section.find("CoreSimulator[.]SimDevice[.]"));
  IUTEST_EXPECT_EQ(std::string::npos, section.find("(subpath "));
  IUTEST_EXPECT_NE(std::string::npos, section.find("clang/ModuleCache[.]noindex(/.*)?$"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("/Xcode/SDKStatCaches[.]noindex(/.*)?$"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("(vnode-type DIRECTORY)"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("xcrun_db"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("com.apple.CoreSimulator."));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("(global-name \"com.apple.SecurityServer\")"));
  options.allow_xcode_simulator = true;
  const auto combined = sandbox::generate_profile(options);
  IUTEST_EXPECT_NE(std::string::npos, combined.find("CoreSimulator[.]SimDevice[.]"));
  IUTEST_EXPECT_NE(std::string::npos,
                   combined.find("(signing-identifier \"com.apple.dt.AssetCatalogSimulatorAgent\")"));
  IUTEST_EXPECT_NE(std::string::npos, combined.find("Untitled-[a-zA-Z0-9]+[.]uicatalog(-journal)?$"));
  IUTEST_EXPECT_NE(std::string::npos, combined.find("(global-name \"com.apple.CoreSimulator.CoreSimulatorService\")"));
  options.allow_xcode = false;
  const auto simulator_only = sandbox::generate_profile(options);
  IUTEST_EXPECT_EQ(std::string::npos, simulator_only.find("XcodeDistPipeline"));
  IUTEST_EXPECT_EQ(std::string::npos, simulator_only.find("CoreSimulator[.]SimDevice[.]"));
  IUTEST_EXPECT_EQ(std::string::npos, simulator_only.find("AssetCatalogSimulatorAgent"));
  IUTEST_EXPECT_EQ(std::string::npos, simulator_only.find("(signing-identifier \"com.apple.ibtool\")"));
#else
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::runtime_error);
#endif
}

IUTEST(SandboxProfileTest, XcodeSimulatorGrantsAreNarrowAndIndependent) {
  sandbox::ProfileOptions options;
  options.allow_xcode_simulator = true;
  const auto profile = sandbox::generate_profile(options);
  for (const auto* service : {"com.apple.CoreSimulator.CoreSimulatorService",
                              "com.apple.CoreSimulator.simdiskimaged"}) {
    IUTEST_EXPECT_NE(std::string::npos, profile.find("(global-name \"" + std::string(service) + "\")"));
  }
  IUTEST_EXPECT_NE(std::string::npos, profile.find("(global-name \"com.apple.CoreSimulator.SimLaunchHost-"));
  IUTEST_EXPECT_NE(std::string::npos, profile.find("/Library/Developer/DeveloperDiskImages\")"));
  const auto section = profile.substr(profile.find("; Opt-in Xcode/Simulator compatibility"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("(allow file-read*"));
  IUTEST_EXPECT_EQ(std::string::npos, section.find("file-write"));
  for (const auto* forbidden : {"(allow mach-lookup)", "(global-name \"com.apple.SecurityServer\")",
                                "(global-name \"com.apple.trustd.agent\")", "com.apple.FSEvents",
                                "com.apple.lsd.", "XcodeDistPipeline", "xcrun_db", "(subpath \"/Users\")"}) {
    IUTEST_EXPECT_EQ(std::string::npos, profile.find(forbidden)) << forbidden;
  }
  options.allow_apple_security = true;
  const auto combined = sandbox::generate_profile(options);
  IUTEST_EXPECT_NE(std::string::npos, combined.find("(global-name \"com.apple.SecurityServer\")"));
  IUTEST_EXPECT_NE(std::string::npos, combined.find("(global-name \"com.apple.CoreSimulator.CoreSimulatorService\")"));
}

IUTEST(SandboxProfileTest, AppleSecurityServicesRequireExplicitOptIn) {
  sandbox::ProfileOptions options;
  options.allow_apple_security = true;
  const auto profile = sandbox::generate_profile(options);
  IUTEST_EXPECT_NE(std::string::npos, profile.find("(global-name \"com.apple.SecurityServer\")"));
  IUTEST_EXPECT_NE(std::string::npos, profile.find("(global-name \"com.apple.trustd.agent\")"));
  IUTEST_EXPECT_NE(std::string::npos, profile.find("(subpath \"/Library/Keychains\")"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("(allow mach-lookup)"));
}

IUTEST(SandboxProfileTest, ExplicitPathsAreCanonicalAndQuoted) {
  cpa_test::TempDir dir;
  const auto read = dir / "read\"\\path";
  const auto write = dir / "write";
  std::filesystem::create_directories(read);
  std::filesystem::create_directories(write);
  sandbox::ProfileOptions options;
  options.read = {read};
  options.write = {write};
  const auto profile = sandbox::generate_profile(options);
  IUTEST_EXPECT_NE(std::string::npos, profile.find("(subpath " + sandbox::sbpl_quote(read.string()) + ")"));
  IUTEST_EXPECT_NE(std::string::npos, profile.find("(subpath " + sandbox::sbpl_quote(write.string()) + ")"));
  IUTEST_EXPECT_EQ("\"a\\\"\\\\b\"", sandbox::sbpl_quote("a\"\\b"));
}

IUTEST(SandboxProfileTest, RsyncCanReadOnlyWritablePathAncestors) {
  cpa_test::TempDir dir;
  const auto writable = dir / "write";
  std::filesystem::create_directories(writable);
  sandbox::ProfileOptions options;
  options.write = {writable, writable};
  options.scoped_write = {writable};
  const auto profile = sandbox::generate_profile(options);
  const auto section = profile.substr(profile.find("; Homebrew rsync may inspect"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("(process-path-regex #\"^/(opt/homebrew|usr/local)/Cellar/rsync/"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("(require-all"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("(require-any"));
  IUTEST_EXPECT_NE(std::string::npos, section.find("(literal " + sandbox::sbpl_quote(writable.parent_path().string()) + ")"));
  IUTEST_EXPECT_EQ(std::string::npos, section.find("(subpath "));
  IUTEST_EXPECT_EQ(std::string::npos, section.find("file-write"));
  const auto ancestor = "(literal " + sandbox::sbpl_quote(writable.parent_path().string()) + ")";
  IUTEST_EXPECT_EQ(section.find(ancestor), section.rfind(ancestor));
}

IUTEST(SandboxProfileTest, ScopedPathsRemainLiteralAndRejectInvalidRoots) {
  cpa_test::TempDir dir;
  const auto target = dir / "target";
  const auto alias = dir / "alias";
  std::filesystem::create_directories(target);
  std::filesystem::create_directory_symlink(target, alias);
  sandbox::ProfileOptions options;
  options.scoped_read = {alias};
  options.scoped_write = {dir / "write"};
  const auto profile = sandbox::generate_profile(options);
  IUTEST_EXPECT_NE(std::string::npos, profile.find("(subpath " + sandbox::sbpl_quote(alias.string()) + ")"));
  IUTEST_EXPECT_EQ(std::string::npos, profile.find("(subpath " + sandbox::sbpl_quote(target.string()) + ")"));
  options.scoped_read = {"relative"};
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::invalid_argument);
  options.scoped_read = {"/"};
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::invalid_argument);
}

IUTEST(SandboxProfileTest, RejectsInvalidPathsAndPorts) {
  IUTEST_EXPECT_THROW(sandbox::sbpl_quote(std::string("a\0b", 3)), std::invalid_argument);
  IUTEST_EXPECT_THROW(sandbox::sbpl_quote("a\nb"), std::invalid_argument);
  sandbox::ProfileOptions options;
  options.write = {std::filesystem::path{}};
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::invalid_argument);
  options.write.clear();
  options.proxy_port = -1;
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::invalid_argument);
  options.proxy_port = 65536;
  IUTEST_EXPECT_THROW(sandbox::generate_profile(options), std::invalid_argument);
}

IUTEST(SandboxCommandsTest, ReadsOnlyRegularFilesWithoutLinks) {
  cpa_test::TempDir dir;
  const auto file = dir / "command";
  util::write_file(file, "NAME=value\n");
  IUTEST_EXPECT_EQ("NAME=value\n", engine::read_command_file(file));
  std::filesystem::create_symlink(file, dir / "link");
  IUTEST_EXPECT_THROW(engine::read_command_file(dir / "link"), engine::FileCommandError);
  std::filesystem::create_directory_symlink(dir.path(), dir / "parent");
  IUTEST_EXPECT_THROW(engine::read_command_file(dir / "parent/command"), engine::FileCommandError);
  IUTEST_EXPECT_THROW(engine::read_command_file(dir.path()), engine::FileCommandError);
  IUTEST_EXPECT_THROW(engine::read_command_file(dir / "missing"), engine::FileCommandError);
  std::filesystem::create_hard_link(file, dir / "hardlink");
  IUTEST_EXPECT_THROW(engine::read_command_file(file), engine::FileCommandError);
}

IUTEST(SandboxCommandsTest, RejectsOversizedFiles) {
  cpa_test::TempDir dir;
  const auto file = dir / "command";
  util::write_file(file, "");
  std::filesystem::resize_file(file, 16 * 1024 * 1024 + 1);
  IUTEST_EXPECT_THROW(engine::read_command_file(file), engine::FileCommandError);
}

IUTEST(SandboxCommandsTest, HashFilesDoesNotFollowLinksOutsideWorkspace) {
  cpa_test::TempDir dir, outside;
  util::write_file(dir / "file", "workspace");
  util::write_file(outside / "private", "private");
  const auto expected = engine::hash_files(dir.path(), {"**"});
  IUTEST_ASSERT_FALSE(expected.empty());
  std::filesystem::create_symlink(outside / "private", dir / "link");
  IUTEST_EXPECT_EQ("", engine::hash_files(dir.path(), {"link"}));
  IUTEST_EXPECT_EQ(expected, engine::hash_files(dir.path(), {"**"}));
  std::filesystem::create_directory_symlink(outside.path(), dir / "redirected");
  IUTEST_EXPECT_EQ("", engine::hash_files(dir / "redirected", {"**"}));
}

#if !defined(__APPLE__)
IUTEST(SandboxTest, UnsupportedPlatformsFailClosed) {
  std::string reason;
  IUTEST_EXPECT_FALSE(sandbox::supported(&reason));
  IUTEST_EXPECT_FALSE(reason.empty());
  sandbox::SandboxConfig config;
  IUTEST_EXPECT_THROW((sandbox::Sandbox(config, {})), std::runtime_error);
  std::ostringstream output;
  engine::Console console(output, false);
  engine::Runner runner({}, console);
  IUTEST_EXPECT_THROW(runner.run({}, {}, {}), std::runtime_error);
}
#else
namespace {

struct ScopedHostPaths {
  std::vector<std::filesystem::path> paths;

  ~ScopedHostPaths() {
    for (const auto& path : paths) util::remove_tree(path);
  }

  void create_file(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "Creating " + path.string());
    paths.push_back(path);
    if (::close(fd) != 0) throw std::system_error(errno, std::generic_category(), "Closing " + path.string());
  }

  void create_directory(const std::filesystem::path& path) {
    if (!std::filesystem::create_directory(path)) throw std::runtime_error("Could not create " + path.string());
    paths.push_back(path);
  }
};

std::string asset_worker_predicates(const std::string& identifier, const std::string& executable) {
  return "(signing-identifier " + sandbox::sbpl_quote(identifier) +
         ")\n"
         "    (process-attribute is-platform-binary)\n"
         "    (process-path-regex #\"^/.*/" +
         executable + "$\")";
}

std::string replace_asset_worker(std::string profile, const std::string& identifier, const std::string& executable,
                                 const std::string& replacement) {
  const auto original = asset_worker_predicates(identifier, executable);
  const auto position = profile.find(original);
  if (position == std::string::npos || profile.find(original, position + original.size()) != std::string::npos) {
    throw std::runtime_error("Expected exactly one asset worker predicate for " + executable);
  }
  profile.replace(position, original.size(), replacement);
  return profile;
}

std::filesystem::path asset_profile_for_perl(const std::filesystem::path& original, const std::filesystem::path& output,
                                             const std::string& identifier, const std::string& executable) {
  // Keep the platform-binary check while exercising the same path rules with a controllable Apple-signed process.
  const auto profile = replace_asset_worker(util::read_file(original), identifier, executable,
                                            "(signing-identifier \"com.apple.perl\")\n"
                                            "    (process-attribute is-platform-binary)\n"
                                            "    (process-path \"/usr/bin/perl\")");
  util::write_file(output, profile);
  return output;
}

util::CaptureResult asset_path_probe(const std::filesystem::path& profile, const std::string& operation, bool allowed,
                                     const std::filesystem::path& path, const std::filesystem::path& other = {}) {
  static const std::string script = R"perl(
use strict;
use warnings;
use Fcntl qw(O_WRONLY O_CREAT O_EXCL);
my ($operation, $expected, $path, $other) = @ARGV;
my ($ok, $handle);
if ($operation eq 'read') {
  $ok = open($handle, '<', $path);
  $ok = close($handle) if $ok;
} elsif ($operation eq 'modify') {
  $ok = open($handle, '>>', $path);
  if ($ok) {
    $ok = print $handle 'x';
    $ok = close($handle) && $ok;
  }
} elsif ($operation eq 'create') {
  $ok = sysopen($handle, $path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  $ok = close($handle) if $ok;
} elsif ($operation eq 'mkdir') {
  $ok = mkdir($path, 0700);
} elsif ($operation eq 'rename') {
  $ok = rename($path, $other);
} elsif ($operation eq 'unlink') {
  $ok = unlink($path);
} elsif ($operation eq 'chmod') {
  $ok = chmod(0600, $path);
} elsif ($operation eq 'list') {
  $ok = opendir($handle, $path);
  $ok = closedir($handle) if $ok;
} else {
  die "unknown probe operation: $operation\n";
}
my $error = 0 + $!;
print "operation=$operation ok=", ($ok ? 1 : 0), " errno=$error\n";
exit($expected eq 'allow' ? ($ok ? 0 : 1) : (!$ok && ($error == 1 || $error == 13) ? 0 : 1));
)perl";
  return util::capture({sandbox::kSandboxExec, "-f", profile.string(), "/usr/bin/perl", "-e", script, operation,
                        allowed ? "allow" : "deny", path.string(), other.string()});
}

class SeatbeltTest : public ::iutest::Test {
 protected:
  void SetUp() override {
    work = dir / "work";
    read = dir / "read";
    denied = outside / "private";
    std::filesystem::create_directories(work);
    std::filesystem::create_directories(read);
    util::write_file(read / "data", "readable");
    util::write_file(denied, "private");
    sandbox::SandboxConfig config;
    config.write = {work};
    config.read = {read};
    config.profile_file = dir / "profile.sb";
    box = std::make_unique<sandbox::Sandbox>(config, sandbox::NetworkProxy::DenyFn{});
  }

  util::ProcessResult run(const std::string& script) {
    util::ProcessSpec spec;
    spec.argv = box->wrap({"/bin/bash", "--noprofile", "--norc", "-c", script});
    spec.cwd = work;
    spec.env = box->environment();
    spec.env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin";
    spec.env["HOME"] = (work / "home").string();
    spec.env["TMPDIR"] = (work / "tmp").string();
    spec.env["WORK"] = work.string();
    spec.env["READ"] = read.string();
    spec.env["DENIED"] = denied.string();
    spec.env["PROFILE"] = box->profile_file().string();
    spec.env["REAL_HOME"] = util::home_dir().string();
    spec.timeout = std::chrono::seconds(10);
    return util::run_process(spec);
  }

  cpa_test::TempDir dir, outside;
  std::filesystem::path work, read, denied;
  std::unique_ptr<sandbox::Sandbox> box;
};

}  // namespace

IUTEST_F(SeatbeltTest, AllowsWorkspaceAndReadOnlyPaths) {
  IUTEST_EXPECT_EQ(0, run("mkdir -p \"$HOME\" \"$TMPDIR\"; cat \"$READ/data\"; "
                        "echo ok > \"$WORK/result\"; echo ok > \"$HOME/file\"; echo ok > \"$TMPDIR/file\"").exit_code);
  IUTEST_EXPECT_EQ("ok\n", util::read_file(work / "result"));
  IUTEST_EXPECT_NE(0, run("echo forbidden > \"$READ/data\"").exit_code);
  IUTEST_EXPECT_EQ("readable", util::read_file(read / "data"));
}

IUTEST_F(SeatbeltTest, DeniesOutsideAndSymlinkAccess) {
  IUTEST_EXPECT_NE(0, run("cat \"$DENIED\"").exit_code);
  IUTEST_EXPECT_NE(0, run("echo forbidden > \"$DENIED\"").exit_code);
  std::filesystem::create_symlink(denied, work / "link");
  IUTEST_EXPECT_NE(0, run("cat \"$WORK/link\"").exit_code);
  IUTEST_EXPECT_NE(0, run("echo forbidden > \"$WORK/link\"").exit_code);
  IUTEST_EXPECT_EQ("private", util::read_file(denied));
}

IUTEST_F(SeatbeltTest, HomebrewRsyncCanResolveWritableDestinations) {
  namespace fs = std::filesystem;
  fs::path rsync;
  for (const auto& candidate : {fs::path("/opt/homebrew/bin/rsync"), fs::path("/usr/local/bin/rsync")}) {
    if (fs::exists(candidate)) {
      const auto resolved = fs::canonical(candidate);
      if (resolved.string().find("/Cellar/rsync/") != std::string::npos) {
        rsync = candidate;
        break;
      }
    }
  }
  if (rsync.empty()) return;

  fs::create_directories(work / "source");
  fs::create_directories(work / "destination");
  util::write_file(work / "source/data", "copied");
  IUTEST_EXPECT_EQ(0, run("\"" + rsync.string() + "\" -a \"$WORK/source/\" \"$WORK/destination/\"").exit_code);
  IUTEST_EXPECT_EQ("copied", util::read_file(work / "destination/data"));
  IUTEST_EXPECT_NE(0, run("/bin/ls /private/tmp").exit_code);
  IUTEST_EXPECT_NE(0, run("cat \"$DENIED\"").exit_code);
}

IUTEST_F(SeatbeltTest, DeniesRealHomeAndProfileModification) {
  IUTEST_EXPECT_NE(0, run("/bin/ls \"$REAL_HOME\"").exit_code);
  const auto profile = util::read_file(box->profile_file());
  IUTEST_EXPECT_NE(0, run("echo '(version 1)(allow default)' > \"$PROFILE\"").exit_code);
  IUTEST_EXPECT_EQ(profile, util::read_file(box->profile_file()));
  IUTEST_EXPECT_NE(0, run("cat \"$DENIED\"").exit_code);
}

IUTEST_F(SeatbeltTest, DeniesDirectNetworkAndUnapprovedProxyHosts) {
  struct Listener {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ~Listener() { if (fd >= 0) ::close(fd); }
  } listener;
  IUTEST_ASSERT_GE(listener.fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  IUTEST_ASSERT_EQ(0, ::bind(listener.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  IUTEST_ASSERT_EQ(0, ::listen(listener.fd, 8));
  socklen_t len = sizeof(addr);
  IUTEST_ASSERT_EQ(0, ::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&addr), &len));
  const auto port = std::to_string(ntohs(addr.sin_port));
  IUTEST_ASSERT_EQ(0, util::capture({"/usr/bin/nc", "-z", "-w", "1", "127.0.0.1", port}).exit_code);
  IUTEST_EXPECT_NE(0, run("/usr/bin/nc -z -w 1 127.0.0.1 " + port).exit_code);
  IUTEST_EXPECT_EQ(0, run("test \"$(/usr/bin/curl --noproxy '' --silent --connect-timeout 2 "
                        "--output /dev/null --write-out '%{http_code}' http://blocked.invalid)\" = 403").exit_code);
}

IUTEST_F(SeatbeltTest, AllowsIPv6ProxyButDeniesOtherIPv6LoopbackPorts) {
  struct Listener {
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    ~Listener() { if (fd >= 0) ::close(fd); }
  } listener;
  IUTEST_ASSERT_GE(listener.fd, 0);
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_loopback;
  IUTEST_ASSERT_EQ(0, ::bind(listener.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  IUTEST_ASSERT_EQ(0, ::listen(listener.fd, 8));
  socklen_t len = sizeof(addr);
  IUTEST_ASSERT_EQ(0, ::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&addr), &len));
  const auto other_port = std::to_string(ntohs(addr.sin6_port));
  IUTEST_ASSERT_EQ(0, util::capture({"/usr/bin/nc", "-6", "-z", "-w", "1", "::1", other_port}).exit_code);
  const auto url = box->environment().at("HTTP_PROXY");
  const auto proxy_port = url.substr(url.rfind(':') + 1);
  IUTEST_EXPECT_EQ(0, run("/usr/bin/nc -6 -z -w 1 ::1 " + proxy_port).exit_code);
  IUTEST_EXPECT_NE(0, run("/usr/bin/nc -6 -z -w 1 ::1 " + other_port).exit_code);
  IUTEST_EXPECT_EQ(0, run("test \"$(/usr/bin/curl --proxy 'http://[::1]:" + proxy_port +
                        "' --noproxy '' --silent --connect-timeout 2 --output /dev/null "
                        "--write-out '%{http_code}' http://blocked.invalid)\" = 403").exit_code);
}

IUTEST(SandboxTest, ProfileCannotBeWritable) {
  cpa_test::TempDir dir;
  sandbox::SandboxConfig config;
  config.write = {dir.path()};
  config.profile_file = dir / "profile.sb";
  IUTEST_EXPECT_THROW((sandbox::Sandbox(config, {})), std::invalid_argument);
  std::filesystem::create_directory_symlink(dir.path(), dir / "alias");
  config.profile_file = dir / "alias/new/profile.sb";
  IUTEST_EXPECT_THROW((sandbox::Sandbox(config, {})), std::invalid_argument);
}

IUTEST(SandboxTest, ScopedPythonCachePermissionsDoNotLeakOrFollowReplacedRoots) {
  namespace fs = std::filesystem;
  cpa_test::TempDir dir;
  const auto cache = dir / "hostedtoolcache";
  const auto selected = cache / "Python/3.11.9/arm64";
  const auto other = cache / "Python/3.12.0/arm64";
  const auto secret = dir / "private/secret";
  util::write_file(selected / "file", "selected");
  util::write_file(other / "file", "other");
  util::write_file(secret, "private");
  sandbox::SandboxConfig config;
  config.profile_file = dir / "control/profile.sb";
  sandbox::Sandbox box(config, {});
  const auto invoke = [&](const std::string& script, const fs::path& path, const std::vector<fs::path>& reads,
                          const std::vector<fs::path>& writes) {
    return util::capture(
               box.wrap({"/bin/sh", "-c", script, "sh", path.string()}, {}, false, false, false, reads, writes))
        .exit_code;
  };
  IUTEST_EXPECT_NE(0, invoke("cat \"$1\" >/dev/null", selected / "file", {}, {}));
  IUTEST_EXPECT_EQ(0, invoke("cat \"$1\" >/dev/null", selected / "file", {}, {cache}));
  IUTEST_EXPECT_EQ(0, invoke("echo writable > \"$1\"", cache / "created", {}, {cache}));
  IUTEST_EXPECT_NE(0, invoke("echo forbidden > \"$1\"", secret, {}, {cache}));
  IUTEST_EXPECT_EQ(0, invoke("cat \"$1\" >/dev/null", selected / "file", {selected}, {}));
  IUTEST_EXPECT_NE(0, invoke("echo forbidden > \"$1\"", selected / "file", {selected}, {}));
  IUTEST_EXPECT_NE(0, invoke("cat \"$1\" >/dev/null", other / "file", {selected}, {}));

  const auto allowed = box.wrap({"/usr/bin/true"}, {}, false, false, false, {}, {cache});
  const auto read_only = box.wrap({"/usr/bin/true"}, {}, false, false, false, {selected}, {});
  IUTEST_EXPECT_NE(allowed[2], read_only[2]);
  IUTEST_EXPECT_EQ(allowed[2], box.wrap({"/usr/bin/true"}, {}, false, false, false, {}, {cache})[2]);
  IUTEST_EXPECT_THROW(box.wrap({"/usr/bin/true"}, {}, false, false, false, {}, {dir / "control"}),
                      std::invalid_argument);
  box.wrap({"/usr/bin/true"}, {}, false, false, false, {selected}, {cache});
  const fs::path read_collision(selected.string() + "\nscoped-write:" + cache.string());
  IUTEST_EXPECT_THROW(box.wrap({"/usr/bin/true"}, {}, false, false, false, {read_collision}, {}),
                      std::invalid_argument);
  const auto first = dir / "read-first";
  const auto second = dir / "read-second";
  box.wrap({"/usr/bin/true"}, {first, second});
  const fs::path extra_read_collision(first.string() + "\nread:" + second.string());
  IUTEST_EXPECT_THROW(box.wrap({"/usr/bin/true"}, {extra_read_collision}), std::invalid_argument);

  fs::rename(selected, selected.parent_path() / "original");
  fs::create_directory_symlink(secret.parent_path(), selected);
  IUTEST_EXPECT_NE(0, invoke("cat \"$1\" >/dev/null", selected / "secret", {selected}, {}));
  IUTEST_EXPECT_EQ("private", util::read_file(secret));
}

IUTEST(SandboxTest, AppleSecurityDerivedProfilesDoNotLeakPermissions) {
  cpa_test::TempDir dir;
  sandbox::SandboxConfig config;
  config.profile_file = dir / "profile.sb";
  config.read = {CPA_APPLE_SECURITY_PROBE};
  sandbox::Sandbox box(config, {});
  for (const auto* service : {"com.apple.SecurityServer", "com.apple.trustd.agent"}) {
    IUTEST_ASSERT_EQ(0, util::capture({CPA_APPLE_SECURITY_PROBE, service, "allow"}).exit_code);
    for (const bool extra_read : {false, true}) {
      const std::vector<std::filesystem::path> read = extra_read
          ? std::vector<std::filesystem::path>{CPA_APPLE_SECURITY_PROBE}
          : std::vector<std::filesystem::path>{};
      auto allowed = box.wrap({"/usr/bin/true"}, read, true);
      auto denied = box.wrap({"/usr/bin/true"}, read);
      IUTEST_EXPECT_NE(allowed[2], denied[2]);
      IUTEST_EXPECT_EQ(allowed[2], box.wrap({"/usr/bin/true"}, read, true)[2]);
      for (const auto& profile : {allowed[2], denied[2], allowed[2]}) {
        const bool grant = profile == allowed[2];
        const auto argv = box.wrap({CPA_APPLE_SECURITY_PROBE, service, grant ? "allow" : "deny"},
                                   read, grant);
        IUTEST_EXPECT_EQ(0, util::capture(argv).exit_code);
      }

    }
  }
}

IUTEST(SandboxTest, DockerSocketGrantIsScopedAndPinsTheResolvedTarget) {
  namespace fs = std::filesystem;
  cpa_test::TempDir dir;
  cpa_test::UnixSocketListener docker(dir / "docker.sock");
  cpa_test::UnixSocketListener other(dir / "other.sock");
  const auto alias = dir / "alias.sock";
  fs::create_symlink(docker.path(), alias);
  util::write_file(dir / "private", "private");
  sandbox::SandboxConfig config;
  config.profile_file = dir / "control/profile.sb";
  config.read = {CPA_MSBUILD_SOCKET_PROBE};
  config.docker_socket = alias;
  sandbox::Sandbox box(config, {});
  const auto probe = [&](const std::string& mode, const fs::path& path,
                         const std::vector<fs::path>& read, bool allow) {
    return box.wrap({CPA_MSBUILD_SOCKET_PROBE, mode, path.string()}, read, false, false, false, {}, {}, allow);
  };
  for (const bool extra_read : {false, true}) {
    const std::vector<fs::path> read = extra_read ? std::vector<fs::path>{CPA_MSBUILD_SOCKET_PROBE}
                                                   : std::vector<fs::path>{};
    const auto denied = probe("connect-deny", alias, read, false);
    const auto allowed = probe("connect-allow", alias, read, true);
    IUTEST_EXPECT_NE(denied[2], allowed[2]);
    IUTEST_EXPECT_EQ(0, util::capture(denied).exit_code);
    IUTEST_EXPECT_EQ(0, util::capture(allowed).exit_code);
    IUTEST_EXPECT_EQ(0, util::capture(probe("connect-allow", fs::canonical(docker.path()), read, true)).exit_code);
    IUTEST_EXPECT_EQ(0, util::capture(probe("connect-deny", other.path(), read, true)).exit_code);
    IUTEST_EXPECT_EQ(0, util::capture(probe("read-deny", dir / "private", read, true)).exit_code);
    IUTEST_EXPECT_EQ(allowed[2], probe("connect-allow", alias, read, true)[2]);
    IUTEST_EXPECT_EQ(denied[2], probe("connect-deny", alias, read, false)[2]);
  }
  fs::remove(alias);
  fs::create_symlink(other.path(), alias);
  IUTEST_EXPECT_THROW(probe("connect-allow", alias, {}, true), std::runtime_error);
  IUTEST_EXPECT_EQ(0, util::capture(probe("connect-deny", alias, {}, false)).exit_code);
}

IUTEST(SandboxTest, DockerSocketMustExistAndBeAUnixSocketOnlyWhenGranted) {
  namespace fs = std::filesystem;
  cpa_test::TempDir dir;
  const auto endpoint = dir / "docker.sock";
  sandbox::SandboxConfig config;
  config.profile_file = dir / "control/profile.sb";
  config.docker_socket = endpoint;
  sandbox::Sandbox box(config, {});
  IUTEST_EXPECT_EQ(0, util::capture(box.wrap({"/usr/bin/true"})).exit_code);
  IUTEST_EXPECT_THROW(box.wrap({"/usr/bin/true"}, {}, false, false, false, {}, {}, true), std::runtime_error);
  util::write_file(endpoint, "not a socket");
  IUTEST_EXPECT_THROW(box.wrap({"/usr/bin/true"}, {}, false, false, false, {}, {}, true), std::runtime_error);
}

IUTEST(SandboxTest, MsbuildDerivedProfilesAreScopedAndIndependent) {
  cpa_test::TempDir dir;
  sandbox::SandboxConfig config;
  config.read = {CPA_MSBUILD_SOCKET_PROBE};
  config.profile_file = dir / "profile.sb";
  sandbox::Sandbox box(config, {});
  struct Listener {
    std::string path = "/private/tmp/MSBuild" + std::to_string(::getpid()) + ".extra";
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ~Listener() {
      if (fd != -1) ::close(fd);
      if (bound) ::unlink(path.c_str());
    }
    bool bound = false;
  } listener;
  IUTEST_ASSERT_NE(-1, listener.fd);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, listener.path.c_str(), listener.path.size() + 1);
  const int bound = ::bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  listener.bound = bound == 0;
  IUTEST_ASSERT_EQ(0, bound);
  IUTEST_ASSERT_EQ(0, ::listen(listener.fd, 8));
  for (const bool apple : {false, true}) {
    for (const bool allow : {false, true, false}) {
      const auto argv = box.wrap({CPA_MSBUILD_SOCKET_PROBE, allow ? "allow" : "deny", ""}, {}, apple, allow);
      IUTEST_EXPECT_EQ(0, util::capture(argv).exit_code);
      IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "deny", ".extra"},
                                               {}, apple, allow)).exit_code);
      IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "connect-deny", listener.path},
                                               {}, apple, allow)).exit_code);
      const auto profile = util::read_file(argv[2]);
      IUTEST_EXPECT_EQ(allow, profile.find("MSBuild[0-9]") != std::string::npos);
      IUTEST_EXPECT_EQ(apple, profile.find("(global-name \"com.apple.SecurityServer\")") != std::string::npos);
      IUTEST_EXPECT_EQ(argv[2], box.wrap({"/usr/bin/true"}, {}, apple, allow)[2]);
    }
  }
}

IUTEST(SandboxTest, UnityPermissionsAreNarrowAndIndependent) {
  namespace fs = std::filesystem;
  cpa_test::TempDir dir;
  sandbox::SandboxConfig config;
  config.profile_file = dir / "profile.sb";
  config.unity_home = dir / "home.with+[regex]\"quote";
  config.read = {CPA_MSBUILD_SOCKET_PROBE, CPA_APPLE_SECURITY_PROBE};
  fs::create_directories(config.unity_home);
  sandbox::Sandbox box(config, {});
  auto encoded_home = util::real_path(config.unity_home).string();
  std::replace(encoded_home.begin(), encoded_home.end(), '/', '_');
  const auto session = fs::path("/private/tmp/.dotnet/shm") / ("session" + std::to_string(::getpid()));
  fs::create_directories(session);
  struct Cleanup {
    fs::path path;
    ~Cleanup() { fs::remove(path); }
  } cleanup{session};
  const auto log_mutex = session / (encoded_home + "_Library_Logs_Unity_Unity.Licensing.Client.log_rolling");
  const auto temporary = util::real_path(sandbox::darwin_user_dirs().front()) / "TemporaryItems";
  fs::create_directories(temporary);
  const auto unity_temp = util::make_temp_dir("NSIRD_Unity_cpa-test-", temporary);
  struct TemporaryCleanup {
    fs::path path;
    ~TemporaryCleanup() { util::remove_tree(path); }
  } temp_cleanup{unity_temp};
  const auto unrelated = util::make_temp_dir("cpa-unrelated-unity-", "/private/tmp");
  TemporaryCleanup unrelated_cleanup{unrelated};
  util::write_file(unrelated / "private", "not readable by Unity");
  const auto* user = ::getpwuid(::getuid());
  IUTEST_ASSERT_NE(nullptr, user);
  IUTEST_ASSERT_NE(nullptr, user->pw_dir);
  const auto cache_root = fs::path(user->pw_dir) / "Library/Caches/com.unity3d.UnityEditor";
  fs::create_directories(cache_root);
  const auto cache = util::make_temp_dir("cpp-actions-test-", cache_root);
  TemporaryCleanup cache_cleanup{cache};
  util::write_file(cache / "existing", "Unity cache read probe");
  const auto near_socket = "/private/tmp/Unity-Upm-" + std::to_string(::getpid()) + ".sock.extra";
  struct SocketCleanup {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    std::string path;
    bool bound = false;
    ~SocketCleanup() {
      if (fd != -1) ::close(fd);
      if (bound) ::unlink(path.c_str());
    }
  } listener;
  listener.path = near_socket;
  IUTEST_ASSERT_NE(-1, listener.fd);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, near_socket.c_str(), near_socket.size() + 1);
  const int bound = ::bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  listener.bound = bound == 0;
  IUTEST_ASSERT_EQ(0, bound);
  IUTEST_ASSERT_EQ(0, ::listen(listener.fd, 4));
  for (const bool apple : {false, true}) {
    for (const bool msbuild : {false, true}) {
      for (const bool unity : {false, true, false}) {
        const auto cache_read = util::capture(box.wrap(
            {CPA_MSBUILD_SOCKET_PROBE, unity ? "read-allow" : "read-deny", (cache / "existing").string()},
            {}, apple, msbuild, unity));
        IUTEST_EXPECT_EQ(0, cache_read.exit_code) << cache_read.out << cache_read.err;
        const auto cache_write = util::capture(box.wrap(
            {CPA_MSBUILD_SOCKET_PROBE, unity ? "file-allow" : "file-deny", (cache / "new-file").string()},
            {}, apple, msbuild, unity));
        IUTEST_EXPECT_EQ(0, cache_write.exit_code) << cache_write.out << cache_write.err;
        const auto argv = box.wrap({CPA_MSBUILD_SOCKET_PROBE, unity ? "unity-allow" : "unity-deny", ""},
                                   {}, apple, msbuild, unity);
        const auto result = util::capture(argv);
        IUTEST_EXPECT_EQ(0, result.exit_code) << result.out << result.err;
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "unity-deny", ".extra"},
                                                 {}, apple, msbuild, unity)).exit_code);
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "connect-deny", near_socket},
                                                 {}, apple, msbuild, unity)).exit_code);
        for (const auto* temp_path : {"/tmp", "/private/tmp"}) {
          const auto listed = util::capture(box.wrap(
              {CPA_MSBUILD_SOCKET_PROBE, unity ? "list-allow" : "list-deny", temp_path}, {},
              apple, msbuild, unity));
          IUTEST_EXPECT_EQ(0, listed.exit_code) << listed.out << listed.err;
        }
        const auto ilpp = util::capture(box.wrap(
            {CPA_MSBUILD_SOCKET_PROBE, unity ? "ilpp-allow" : "ilpp-deny", ""}, {}, apple, msbuild, unity));
        IUTEST_EXPECT_EQ(0, ilpp.exit_code) << ilpp.out << ilpp.err;
        const auto coreclr_target = fs::path("/private/tmp/.dotnet/lockfiles") /
                                    ("session" + std::to_string(::getpid()) + "9");
        fs::create_directories(coreclr_target.parent_path());
        fs::remove(coreclr_target);
        const auto coreclr = util::capture(box.wrap(
            {CPA_MSBUILD_SOCKET_PROBE, unity ? "coreclr-allow" : "coreclr-deny", coreclr_target.string()}, {},
            apple, msbuild, unity));
        fs::remove(coreclr_target);
        IUTEST_EXPECT_EQ(0, coreclr.exit_code) << coreclr.out << coreclr.err;
        const auto exact_coreclr = util::capture(box.wrap(
            {CPA_MSBUILD_SOCKET_PROBE, unity ? "mkdir-allow" : "mkdir-deny", "/private/tmp/.coreclr.Ab12Z9"}, {},
            apple, msbuild, unity));
        IUTEST_EXPECT_EQ(0, exact_coreclr.exit_code) << exact_coreclr.out << exact_coreclr.err;
        for (const auto* near : {"/private/tmp/.coreclr.Ab12Z", "/private/tmp/.coreclr.Ab12Z9x",
                                 "/private/tmp/.coreclr.Ab12_9", "/private/tmp/coreclr.Ab12Z9",
                                 "/private/tmp/.coreclrXAb12Z9"}) {
          const auto denied = util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "mkdir-deny", near}, {},
                                                     apple, msbuild, unity));
          IUTEST_EXPECT_EQ(0, denied.exit_code) << near << denied.out << denied.err;
        }
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "ilpp-deny", ".extra"},
                                                 {}, apple, msbuild, unity)).exit_code);
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "ilpp-deny", "0"},
                                                 {}, apple, msbuild, unity)).exit_code);
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap(
            {CPA_MSBUILD_SOCKET_PROBE, "read-deny", (unrelated / "private").string()}, {},
            apple, msbuild, unity)).exit_code);
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap(
            {CPA_MSBUILD_SOCKET_PROBE, "file-deny", (unrelated / "new-file").string()}, {},
            apple, msbuild, unity)).exit_code);
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap(
            {CPA_MSBUILD_SOCKET_PROBE, "list-deny", unrelated.string()}, {},
            apple, msbuild, unity)).exit_code);
        const auto file = util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, unity ? "file-allow" : "file-deny",
                                                 log_mutex.string()}, {}, apple, msbuild, unity));
        IUTEST_EXPECT_EQ(0, file.exit_code) << file.out << file.err;
        const auto temp_file = util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE,
                                                       unity ? "file-allow" : "file-deny",
                                                       (unity_temp / "probe").string()},
                                                      {}, apple, msbuild, unity));
        IUTEST_EXPECT_EQ(0, temp_file.exit_code) << temp_file.out << temp_file.err;
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "file-deny",
                                                   log_mutex.string() + ".extra"},
                                                 {}, apple, msbuild, unity)).exit_code);
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "file-deny",
                                                   (session / "unrelated-mutex").string()},
                                                 {}, apple, msbuild, unity)).exit_code);
        auto other_home_mutex = log_mutex.string();
        other_home_mutex[other_home_mutex.find("home.with")] = 'X';
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "file-deny", other_home_mutex},
                                                 {}, apple, msbuild, unity)).exit_code);
        const auto global = fs::path("/private/tmp/.dotnet/shm/global");
        fs::create_directories(global);
        for (const auto* guid : {"01234567-89ab-4cde-8fab-0123456789ab",
                                 "01234567-89AB-4CDE-8FAB-0123456789AB"}) {
          const auto lease = (global / guid).string();
          const auto lease_result = util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE,
                                                            unity ? "file-allow" : "file-deny", lease},
                                                           {}, apple, msbuild, unity));
          IUTEST_EXPECT_EQ(0, lease_result.exit_code) << lease_result.out << lease_result.err;
        }
        for (const auto* near : {"01234567-89ab-4cde-8fab-0123456789a", "01234567-89ab-4cde-8fab-0123456789abx",
                                 "0123456789ab4cde8fab0123456789ab", "zz234567-89ab-4cde-8fab-0123456789ab"}) {
          IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "file-deny",
                                                     (global / near).string()},
                                                    {}, apple, msbuild, unity)).exit_code) << near;
        }
        const auto resolv = util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE,
                                                    unity ? "read-allow" : "read-deny",
                                                    "/private/var/run/resolv.conf"},
                                                   {}, apple, msbuild, unity));
        if (fs::exists("/private/var/run/resolv.conf"))
          IUTEST_EXPECT_EQ(0, resolv.exit_code) << resolv.out << resolv.err;
        if (util::capture({CPA_APPLE_SECURITY_PROBE, "com.apple.airportd", "allow"}).exit_code == 0) {
          const auto airport = util::capture(box.wrap({CPA_APPLE_SECURITY_PROBE, "com.apple.airportd",
                                                       unity ? "allow" : "deny"},
                                                      {}, apple, msbuild, unity));
          IUTEST_EXPECT_EQ(0, airport.exit_code) << airport.out << airport.err;
        }
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_APPLE_SECURITY_PROBE, "com.apple.wifi.manager", "deny"},
                                                  {}, apple, msbuild, unity)).exit_code);
        const auto shader_ipc = "/private/tmp/Unity-ShaderCompilerIPC-" + std::to_string(::getpid()) + "-1234567890";
        const auto shader_socket = util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE,
                                                           unity ? "socket-allow" : "socket-deny",
                                                           shader_ipc + ".sock"},
                                                          {}, apple, msbuild, unity));
        IUTEST_EXPECT_EQ(0, shader_socket.exit_code) << shader_socket.out << shader_socket.err;
        const auto shader_lock = util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE,
                                                         unity ? "file-allow" : "file-deny", shader_ipc + ".lock"},
                                                        {}, apple, msbuild, unity));
        IUTEST_EXPECT_EQ(0, shader_lock.exit_code) << shader_lock.out << shader_lock.err;
        for (const auto& near : {shader_ipc + ".sock.extra", std::string("/private/tmp/Unity-ShaderCompilerIPC-x-1.sock"),
                                 std::string("/private/tmp/Unity-Other-1.sock")}) {
          IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "socket-deny", near},
                                                    {}, apple, msbuild, unity)).exit_code) << near;
        }
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, "file-deny", shader_ipc + ".locked"},
                                                  {}, apple, msbuild, unity)).exit_code);
        const auto loopback = util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE,
                                                      unity ? "tcp-loopback-allow" : "tcp-loopback-deny", "-"},
                                                     {}, apple, msbuild, unity));
        IUTEST_EXPECT_EQ(0, loopback.exit_code) << loopback.out << loopback.err;
        // Seatbelt cannot restrict listening to loopback interfaces.
        for (const auto* mode : {unity ? "tcp-any-allow" : "tcp-any-deny", "tcp-remote-deny"}) {
          const auto tcp_result = util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, mode, "-"}, {}, apple, msbuild, unity));
          IUTEST_EXPECT_EQ(0, tcp_result.exit_code) << mode << ": " << tcp_result.out << tcp_result.err;
        }
        const auto profile = util::read_file(argv[2]);
        IUTEST_EXPECT_EQ(unity, profile.find("unitypreflock") != std::string::npos);
        IUTEST_EXPECT_EQ(unity, profile.find("(local ip \"*:*\"))") != std::string::npos);
        IUTEST_EXPECT_EQ(unity, profile.find("com.apple.airportd") != std::string::npos);
        IUTEST_EXPECT_EQ(apple, profile.find("com.apple.SecurityServer") != std::string::npos);
        IUTEST_EXPECT_EQ(msbuild, profile.find("MSBuild[0-9]") != std::string::npos);
        IUTEST_EXPECT_EQ(argv[2], box.wrap({"/usr/bin/true"}, {}, apple, msbuild, unity)[2]);
        IUTEST_EXPECT_EQ(std::string::npos, profile.find("CoreBusinessMetrics"));
        IUTEST_EXPECT_EQ(std::string::npos, profile.find("remote ip \"10."));
      }
    }
  }
}

IUTEST(SandboxTest, UnityShortTemporaryDirectoriesAreIsolatedBetweenJobs) {
  cpa_test::TempDir dir;
  const auto first = util::make_temp_dir("cpa-u-", "/private/tmp");
  const auto second = util::make_temp_dir("cpa-u-", "/private/tmp");
  struct Cleanup {
    std::filesystem::path first, second;
    ~Cleanup() {
      util::remove_tree(first);
      util::remove_tree(second);
    }
  } cleanup{first, second};
  for (const auto& own : {first, second}) {
    const auto other = own == first ? second : first;
    sandbox::SandboxConfig config;
    config.profile_file = dir / (own.filename().string() + ".sb");
    config.unity_home = dir / "home";
    config.unity_tmp = own;
    config.read = {CPA_MSBUILD_SOCKET_PROBE};
    std::filesystem::create_directories(config.unity_home);
    sandbox::Sandbox box(config, {});
    for (const bool unity : {false, true, false}) {
      const auto result = util::capture(box.wrap(
          {CPA_MSBUILD_SOCKET_PROBE, unity ? "socket-allow" : "socket-deny", (own / "ipc").string()},
          {}, false, false, unity));
      IUTEST_EXPECT_EQ(0, result.exit_code) << result.out << result.err;
      IUTEST_EXPECT_EQ(0, util::capture(box.wrap(
          {CPA_MSBUILD_SOCKET_PROBE, "socket-deny", (other / "ipc").string()},
          {}, false, false, unity)).exit_code);
      IUTEST_EXPECT_EQ(0, util::capture(box.wrap(
          {CPA_MSBUILD_SOCKET_PROBE, "list-deny", other.string()},
          {}, false, false, unity)).exit_code);
    }
  }
}

IUTEST(SandboxTest, XcodeTemporaryPermissionsAreScopedAndCachedSeparately) {
  namespace fs = std::filesystem;
  cpa_test::TempDir dir;
  const auto user_dirs = sandbox::darwin_user_dirs();
  IUTEST_ASSERT_FALSE(user_dirs.empty());
  const auto temp = util::real_path(user_dirs.front());
  const auto items = temp / "TemporaryItems";
  fs::create_directories(items);
  const auto host_list = util::capture({CPA_MSBUILD_SOCKET_PROBE, "list-allow", items.string()});
  const bool host_can_list_items = host_list.exit_code == 0;
  if (!host_can_list_items) {
    const auto host_denied = util::capture({CPA_MSBUILD_SOCKET_PROBE, "list-deny", items.string()});
    IUTEST_ASSERT_EQ(0, host_denied.exit_code) << host_list.out << host_list.err << host_denied.out << host_denied.err;
  }
  ScopedHostPaths cleanup;
  const auto make_dir = [&](const std::string& prefix, const fs::path& parent) {
    auto path = util::make_temp_dir(prefix, parent);
    cleanup.paths.push_back(path);
    return path;
  };
  const auto xcode = make_dir("NSIRD_xcodebuild_cpa-test-", items);
  const auto service = make_dir("NSIRD_SWBBuildService_cpa-test-", items);
  const auto ibtool = make_dir("NSIRD_ibtool_cpa-test-", items);
  fs::create_directory(ibtool / "nested");
  const auto swbuild = make_dir("swbuild.tmp.cpa-test-", temp);
  const auto near_xcode = make_dir("NSIRD_xcodebuildX_cpa-test-", items);
  const auto near_ibtool = make_dir("NSIRD_ibtoolX_cpa-test-", items);
  const auto near_ibtoold = make_dir("NSIRD_ibtoold_cpa-test-", items);
  const auto ibtool_elsewhere = make_dir("NSIRD_ibtool_cpa-test-", temp);
  const auto near_swbuild = make_dir("swbuild.tmpX.cpa-test-", temp);
  const auto unrelated = make_dir("cpa-xcode-unrelated-", temp);
  for (const auto& path : {xcode, service, ibtool, ibtool / "nested", swbuild, near_xcode, near_ibtool,
                           near_ibtoold, ibtool_elsewhere, near_swbuild, unrelated}) {
    util::write_file(path / "existing", "probe");
  }
  fs::create_directory_symlink(unrelated, xcode / "redirect");
  fs::create_directory_symlink(unrelated, ibtool / "redirect");
  for (const auto& path : {ibtool, ibtool / "nested", near_ibtool, near_ibtoold, ibtool_elsewhere,
                           ibtool / "redirect"}) {
    const auto host_read = util::capture({CPA_MSBUILD_SOCKET_PROBE, "read-allow", (path / "existing").string()});
    const auto host_write = util::capture({CPA_MSBUILD_SOCKET_PROBE, "file-allow", (path / "host-new").string()});
    IUTEST_ASSERT_EQ(0, host_read.exit_code) << path.string() << ": " << host_read.out << host_read.err;
    IUTEST_ASSERT_EQ(0, host_write.exit_code) << path.string() << ": " << host_write.out << host_write.err;
  }
  const auto new_xcode = make_dir("NSIRD_xcodebuild_cpa-create-", items);
  const auto new_service = make_dir("NSIRD_SWBBuildService_cpa-create-", items);
  const auto new_ibtool = make_dir("NSIRD_ibtool_cpa-create-", items);
  const auto new_swbuild = make_dir("swbuild.tmp.cpa-create-", temp);
  for (const auto& path : {new_xcode, new_service, new_ibtool, new_swbuild}) fs::remove(path);
  const auto host_create = util::capture({CPA_MSBUILD_SOCKET_PROBE, "mkdir-allow", new_ibtool.string()});
  IUTEST_ASSERT_EQ(0, host_create.exit_code) << host_create.out << host_create.err;

  const auto uuid = util::random_uuid();
  auto uppercase_uuid = util::random_uuid();
  std::transform(uppercase_uuid.begin(), uppercase_uuid.end(), uppercase_uuid.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  const auto storyboard = temp / (uuid + ".storyboard");
  const auto uppercase_storyboard = temp / (uppercase_uuid + ".storyboard");
  const auto invalid_storyboard = temp / ("g" + uuid.substr(1) + ".storyboard");
  auto misplaced_hyphen_uuid = uuid;
  std::swap(misplaced_hyphen_uuid[7], misplaced_hyphen_uuid[8]);
  const auto invalid_hyphen_storyboard = temp / (misplaced_hyphen_uuid + ".storyboard");
  for (const auto& path : {storyboard, uppercase_storyboard, invalid_storyboard, invalid_hyphen_storyboard}) {
    cleanup.create_file(path);
  }
  const bool case_insensitive = fs::exists(temp / (uuid + ".STORYBOARD"));
  const auto new_storyboard = temp / (util::random_uuid() + ".storyboard");
  const auto uppercase_extension = temp / (util::random_uuid() + ".STORYBOARD");
  const auto storyboard_dir = temp / (util::random_uuid() + ".storyboard");
  cleanup.create_directory(storyboard_dir);
  const auto storyboard_redirect = temp / (util::random_uuid() + ".storyboard");
  fs::create_symlink(unrelated / "existing", storyboard_redirect);
  cleanup.paths.push_back(storyboard_redirect);
  const auto storyboard_alias = temp / ("cpa-" + util::random_uuid() + ".storyboard");
  fs::create_symlink(storyboard, storyboard_alias);
  cleanup.paths.push_back(storyboard_alias);

  auto numeric_suffix = util::random_uuid();
  numeric_suffix.erase(std::remove_if(numeric_suffix.begin(), numeric_suffix.end(),
                                      [](unsigned char c) { return !std::isdigit(c); }),
                       numeric_suffix.end());
  const auto ib_name = "ibtoold-" + std::to_string(::getpid()) + numeric_suffix;
  const auto ibtoold = temp / ib_name;
  const auto ibtoold_near = temp / (ib_name + "x");
  const auto ibtoold_no_ib = temp / (ib_name + "1");
  const auto ibtoold_new = temp / (ib_name + "2");
  for (const auto& path : {ibtoold, ibtoold_near, ibtoold_no_ib, ibtoold_new}) {
    cleanup.create_directory(path);
  }
  IUTEST_ASSERT_TRUE(fs::remove(ibtoold_new));
  for (const auto& path : {ibtoold / "IB", ibtoold / "IB/nested", ibtoold / "IB-extra",
                           ibtoold / "other", ibtoold_near / "IB"}) {
    fs::create_directories(path);
    util::write_file(path / "existing", "probe");
  }
  fs::create_directory_symlink(unrelated, ibtoold / "IB/redirect");

  sandbox::SandboxConfig config;
  config.profile_file = dir / "profile.sb";
  config.read = {CPA_MSBUILD_SOCKET_PROBE};
  sandbox::Sandbox box(config, {});
  const auto wrap = [&](const std::vector<std::string>& argv, bool apple, bool allow_xcode) {
    return box.wrap(argv, {}, apple, false, false, {}, {}, false, allow_xcode);
  };
  for (const bool apple : {false, true}) {
    std::string denied_profile;
    std::string allowed_profile;
    for (const bool allow_xcode : {false, true, false}) {
      const auto argv = wrap({"/usr/bin/true"}, apple, allow_xcode);
      const auto profile = util::read_file(argv[2]);
      IUTEST_EXPECT_EQ(allow_xcode, profile.find("; Opt-in Xcode temporary files") != std::string::npos);
      IUTEST_EXPECT_EQ(apple, profile.find("(global-name \"com.apple.SecurityServer\")") != std::string::npos);
      IUTEST_EXPECT_EQ(std::string::npos, profile.find("com.apple.CoreSimulator."));
      if (allow_xcode) {
        allowed_profile = argv[2];
      } else if (denied_profile.empty()) {
        denied_profile = argv[2];
      } else {
        IUTEST_EXPECT_EQ(denied_profile, argv[2]);
      }
      for (const auto& path : {temp, items}) {
        const auto list = util::capture(wrap(
            {CPA_MSBUILD_SOCKET_PROBE,
             allow_xcode && path == items && host_can_list_items ? "list-allow" : "list-deny", path.string()},
            apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, list.exit_code) << path.string() << ": " << list.out << list.err;
      }
      for (const auto& path : {xcode, service, ibtool, ibtool / "nested", swbuild}) {
        const auto read = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "read-allow" : "read-deny",
                                              (path / "existing").string()}, apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, read.exit_code) << read.out << read.err;
        const auto modify = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "modify-allow" : "modify-deny",
                                                (path / "existing").string()}, apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, modify.exit_code) << modify.out << modify.err;
        const auto write = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "file-allow" : "file-deny",
                                               (path / "new").string()}, apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, write.exit_code) << write.out << write.err;
      }
      for (const auto& path : {near_xcode, near_ibtool, near_ibtoold, ibtool_elsewhere, near_swbuild, unrelated,
                               xcode / "redirect", ibtool / "redirect"}) {
        const auto read = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "read-deny",
                                              (path / "existing").string()}, apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, read.exit_code) << read.out << read.err;
        const auto modify = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "modify-deny",
                                                (path / "existing").string()}, apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, modify.exit_code) << modify.out << modify.err;
        const auto write = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "file-deny",
                                               (path / "new").string()}, apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, write.exit_code) << write.out << write.err;
      }
      const auto cache = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "file-deny",
                                             (temp / ("xcrun_db-cpa-test-" + std::to_string(::getpid()))).string()},
                                            apple, allow_xcode));
      IUTEST_EXPECT_EQ(0, cache.exit_code) << cache.out << cache.err;
      const auto ibtool_rename_from = ibtool / "nested" / ("rename-from-" + util::random_uuid());
      const auto ibtool_rename_to = ibtool / "nested" / ("rename-to-" + util::random_uuid());
      util::write_file(ibtool_rename_from, "from");
      util::write_file(ibtool_rename_to, "to");
      const auto ibtool_rename = util::capture(wrap(
          {CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "rename-allow" : "rename-deny",
           ibtool_rename_from.string(), ibtool_rename_to.string()}, apple, allow_xcode));
      IUTEST_EXPECT_EQ(0, ibtool_rename.exit_code) << ibtool_rename.out << ibtool_rename.err;
      IUTEST_EXPECT_EQ(!allow_xcode, fs::exists(ibtool_rename_from));
      IUTEST_EXPECT_EQ(allow_xcode ? "from" : "to", util::read_file(ibtool_rename_to));
      const auto ibtool_cross_source = ibtool / ("rename-cross-" + util::random_uuid());
      const auto ibtool_cross_destination = unrelated / ("rename-cross-" + util::random_uuid());
      util::write_file(ibtool_cross_source, "from");
      util::write_file(ibtool_cross_destination, "to");
      const auto ibtool_cross_rename = util::capture(wrap(
          {CPA_MSBUILD_SOCKET_PROBE, "rename-deny", ibtool_cross_source.string(), ibtool_cross_destination.string()},
          apple, allow_xcode));
      IUTEST_EXPECT_EQ(0, ibtool_cross_rename.exit_code) << ibtool_cross_rename.out << ibtool_cross_rename.err;
      IUTEST_EXPECT_TRUE(fs::exists(ibtool_cross_source));
      IUTEST_EXPECT_EQ("to", util::read_file(ibtool_cross_destination));
      for (const auto& path : {storyboard, uppercase_storyboard, storyboard_alias}) {
        const auto read = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "read-allow" : "read-deny",
                                              path.string()}, apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, read.exit_code) << path.string() << ": " << read.out << read.err;
        const auto modify = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "modify-allow" : "modify-deny",
                                                path.string()}, apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, modify.exit_code) << path.string() << ": " << modify.out << modify.err;
      }
      for (const auto& path : {invalid_storyboard, invalid_hyphen_storyboard, storyboard_redirect}) {
        const auto read = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "read-deny", path.string()},
                                              apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, read.exit_code) << path.string() << ": " << read.out << read.err;
        const auto modify = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "modify-deny", path.string()},
                                                apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, modify.exit_code) << path.string() << ": " << modify.out << modify.err;
      }
      const auto create_storyboard = util::capture(
          wrap({CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "file-allow" : "file-deny", new_storyboard.string()},
               apple, allow_xcode));
      IUTEST_EXPECT_EQ(0, create_storyboard.exit_code) << create_storyboard.out << create_storyboard.err;
      const auto create_uppercase = util::capture(
          wrap({CPA_MSBUILD_SOCKET_PROBE, allow_xcode && case_insensitive ? "file-allow" : "file-deny",
                uppercase_extension.string()}, apple, allow_xcode));
      IUTEST_EXPECT_EQ(0, create_uppercase.exit_code) << create_uppercase.out << create_uppercase.err;
      const auto rename_from = temp / (util::random_uuid() + ".storyboard");
      const auto rename_to = temp / (util::random_uuid() + ".storyboard");
      cleanup.create_file(rename_from);
      cleanup.create_file(rename_to);
      const auto rename = util::capture(wrap(
          {CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "rename-allow" : "rename-deny",
           rename_from.string(), rename_to.string()}, apple, allow_xcode));
      IUTEST_EXPECT_EQ(0, rename.exit_code) << rename.out << rename.err;
      IUTEST_EXPECT_EQ(!allow_xcode, fs::exists(rename_from));
      IUTEST_EXPECT_TRUE(fs::exists(rename_to));
      const auto denied_destination = unrelated / ("rename-" + util::random_uuid());
      util::write_file(denied_destination, "probe");
      const auto denied_source = temp / (util::random_uuid() + ".storyboard");
      cleanup.create_file(denied_source);
      const auto cross_rename = util::capture(wrap(
          {CPA_MSBUILD_SOCKET_PROBE, "rename-deny", denied_source.string(), denied_destination.string()},
          apple, allow_xcode));
      IUTEST_EXPECT_EQ(0, cross_rename.exit_code) << cross_rename.out << cross_rename.err;
      IUTEST_EXPECT_TRUE(fs::exists(denied_source));
      IUTEST_EXPECT_EQ("probe", util::read_file(denied_destination));
      for (const auto& path : {temp / (uuid.substr(1) + ".storyboard"), temp / ("x" + uuid + ".storyboard"),
                               temp / (uuid + ".storyboardc"), temp / (uuid + ".storyboard.tmp"),
                               storyboard_dir / "child"}) {
        const auto create = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "file-deny", path.string()},
                                               apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, create.exit_code) << path.string() << ": " << create.out << create.err;
      }
      for (const auto& path : {ibtoold / "IB", ibtoold / "IB/nested"}) {
        const auto read = util::capture(wrap(
            {CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "read-allow" : "read-deny", (path / "existing").string()},
            apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, read.exit_code) << path.string() << ": " << read.out << read.err;
        const auto modify = util::capture(wrap(
            {CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "modify-allow" : "modify-deny", (path / "existing").string()},
            apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, modify.exit_code) << path.string() << ": " << modify.out << modify.err;
        for (const auto* operation : {"file", "fifo"}) {
          const auto probe = util::capture(wrap(
              {CPA_MSBUILD_SOCKET_PROBE, std::string(operation) + (allow_xcode ? "-allow" : "-deny"),
               (path / (std::string("new-") + operation)).string()}, apple, allow_xcode));
          IUTEST_EXPECT_EQ(0, probe.exit_code) << path.string() << ": " << probe.out << probe.err;
        }
      }
      for (const auto& path : {ibtoold / "IB-extra", ibtoold / "other", ibtoold_near / "IB",
                               ibtoold / "IB/redirect"}) {
        const auto read = util::capture(wrap(
            {CPA_MSBUILD_SOCKET_PROBE, "read-deny", (path / "existing").string()}, apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, read.exit_code) << path.string() << ": " << read.out << read.err;
        const auto modify = util::capture(wrap(
            {CPA_MSBUILD_SOCKET_PROBE, "modify-deny", (path / "existing").string()}, apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, modify.exit_code) << path.string() << ": " << modify.out << modify.err;
        for (const auto* operation : {"file", "fifo"}) {
          const auto probe = util::capture(wrap(
              {CPA_MSBUILD_SOCKET_PROBE, std::string(operation) + "-deny",
               (path / (std::string("new-") + operation)).string()}, apple, allow_xcode));
          IUTEST_EXPECT_EQ(0, probe.exit_code) << path.string() << ": " << probe.out << probe.err;
        }
      }
      const auto ib_rename_from = ibtoold / "IB" / ("rename-from-" + util::random_uuid());
      const auto ib_rename_to = ibtoold / "IB" / ("rename-to-" + util::random_uuid());
      cleanup.create_file(ib_rename_from);
      cleanup.create_file(ib_rename_to);
      const auto ib_rename = util::capture(wrap(
          {CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "rename-allow" : "rename-deny",
           ib_rename_from.string(), ib_rename_to.string()}, apple, allow_xcode));
      IUTEST_EXPECT_EQ(0, ib_rename.exit_code) << ib_rename.out << ib_rename.err;
      IUTEST_EXPECT_EQ(!allow_xcode, fs::exists(ib_rename_from));
      IUTEST_EXPECT_TRUE(fs::exists(ib_rename_to));
      for (const auto& path : {ibtoold_new, ibtoold_no_ib / "IB", ibtoold / "IB/nested/new"}) {
        const auto create = util::capture(wrap(
            {CPA_MSBUILD_SOCKET_PROBE, allow_xcode ? "mkdir-allow" : "mkdir-deny", path.string()},
            apple, allow_xcode));
        IUTEST_EXPECT_EQ(0, create.exit_code) << path.string() << ": " << create.out << create.err;
      }
      for (const auto& path : {new_xcode, new_service, new_ibtool, new_swbuild}) {
        if (allow_xcode) {
          const auto create = util::capture(wrap({"/bin/mkdir", path.string()}, apple, allow_xcode));
          IUTEST_EXPECT_EQ(0, create.exit_code) << create.out << create.err;
          if (create.exit_code == 0) {
            const auto write = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "file-allow", (path / "new").string()},
                                                  apple, allow_xcode));
            IUTEST_EXPECT_EQ(0, write.exit_code) << write.out << write.err;
            const auto remove = util::capture(wrap({"/bin/rmdir", path.string()}, apple, allow_xcode));
            IUTEST_EXPECT_EQ(0, remove.exit_code) << remove.out << remove.err;
          }
        } else {
          const auto denied = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "mkdir-deny", path.string()},
                                                apple, allow_xcode));
          IUTEST_EXPECT_EQ(0, denied.exit_code) << denied.out << denied.err;
          IUTEST_EXPECT_FALSE(fs::exists(path));
        }
      }
    }
    IUTEST_EXPECT_NE(denied_profile, allowed_profile);
    IUTEST_EXPECT_EQ(allowed_profile, wrap({"/usr/bin/true"}, apple, true)[2]);
  }
}

IUTEST(SandboxTest, XcodeSimulatorTemporaryEntriesRequireBothPermissions) {
  namespace fs = std::filesystem;
  cpa_test::TempDir dir, outside;
  const auto user_dirs = sandbox::darwin_user_dirs();
  IUTEST_ASSERT_FALSE(user_dirs.empty());
  const auto temp = util::real_path(user_dirs.front());
  const auto standalone_name = [] {
    return "com.apple.CoreSimulator.SimDevice." + util::random_uuid() + ".Standalone." + util::random_uuid();
  };
  ScopedHostPaths cleanup;
  const auto existing = temp / standalone_name();
  auto uppercase_uuid = util::random_uuid();
  std::transform(uppercase_uuid.begin(), uppercase_uuid.end(), uppercase_uuid.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  const auto uppercase = temp / ("com.apple.CoreSimulator.SimDevice." + uppercase_uuid +
                                 ".Standalone." + uppercase_uuid);
  const auto invalid = temp / (standalone_name() + ".extra");
  auto invalid_guid = util::random_uuid();
  invalid_guid[0] = 'g';
  const auto invalid_device = temp / ("com.apple.CoreSimulator.SimDevice." + invalid_guid +
                                      ".Standalone." + util::random_uuid());
  const auto invalid_standalone = temp / ("com.apple.CoreSimulator.SimDevice." + util::random_uuid() +
                                          ".Standalone." + invalid_guid);
  const auto nested = temp / standalone_name();
  const auto redirect = temp / standalone_name();
  for (const auto& path : {existing, uppercase, invalid, invalid_device, invalid_standalone}) {
    cleanup.create_file(path);
  }
  cleanup.create_directory(nested);
  util::write_file(nested / "existing", "private");
  util::write_file(outside / "secret", "private");
  fs::create_symlink(outside / "secret", redirect);
  cleanup.paths.push_back(redirect);

  for (const bool allow_simulator : {false, true}) {
    sandbox::SandboxConfig config;
    config.profile_file = dir / (allow_simulator ? "simulator.sb" : "default.sb");
    config.read = {CPA_MSBUILD_SOCKET_PROBE};
    config.allow_xcode_simulator = allow_simulator;
    sandbox::Sandbox box(config, {});
    for (const bool allow_xcode : {false, true}) {
      const bool allowed = allow_simulator && allow_xcode;
      const auto wrap = [&](const std::vector<std::string>& argv) {
        return box.wrap(argv, {}, false, false, false, {}, {}, false, allow_xcode);
      };
      const auto profile = util::read_file(wrap({"/usr/bin/true"})[2]);
      IUTEST_EXPECT_EQ(allow_simulator,
                       profile.find("(global-name \"com.apple.CoreSimulator.CoreSimulatorService\")") !=
                           std::string::npos);
      IUTEST_EXPECT_EQ(allowed, profile.find("CoreSimulator[.]SimDevice[.]") != std::string::npos);
      for (const auto& path : {existing, uppercase}) {
        const auto read = util::capture(wrap(
            {CPA_MSBUILD_SOCKET_PROBE, allowed ? "read-allow" : "read-deny", path.string()}));
        IUTEST_EXPECT_EQ(0, read.exit_code) << path.string() << ": " << read.out << read.err;
        const auto modify = util::capture(wrap(
            {CPA_MSBUILD_SOCKET_PROBE, allowed ? "modify-allow" : "modify-deny", path.string()}));
        IUTEST_EXPECT_EQ(0, modify.exit_code) << path.string() << ": " << modify.out << modify.err;
      }
      const auto create = util::capture(wrap(
          {CPA_MSBUILD_SOCKET_PROBE, allowed ? "file-allow" : "file-deny", (temp / standalone_name()).string()}));
      IUTEST_EXPECT_EQ(0, create.exit_code) << create.out << create.err;
      const auto mkdir = util::capture(wrap(
          {CPA_MSBUILD_SOCKET_PROBE, allowed ? "mkdir-allow" : "mkdir-deny", (temp / standalone_name()).string()}));
      IUTEST_EXPECT_EQ(0, mkdir.exit_code) << mkdir.out << mkdir.err;
      for (const auto& path : {invalid, invalid_device, invalid_standalone, redirect, nested / "existing"}) {
        const auto denied = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "read-deny", path.string()}));
        IUTEST_EXPECT_EQ(0, denied.exit_code) << path.string() << ": " << denied.out << denied.err;
        const auto denied_modify = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "modify-deny", path.string()}));
        IUTEST_EXPECT_EQ(0, denied_modify.exit_code) << path.string() << ": " << denied_modify.out << denied_modify.err;
      }
      for (const auto& path : {nested / "child", temp / (standalone_name() + ".extra")}) {
        const auto denied = util::capture(wrap({CPA_MSBUILD_SOCKET_PROBE, "file-deny", path.string()}));
        IUTEST_EXPECT_EQ(0, denied.exit_code) << path.string() << ": " << denied.out << denied.err;
      }
      const auto rename_from = temp / standalone_name();
      const auto rename_to = temp / standalone_name();
      cleanup.create_file(rename_from);
      cleanup.create_file(rename_to);
      const auto rename = util::capture(wrap(
          {CPA_MSBUILD_SOCKET_PROBE, allowed ? "rename-allow" : "rename-deny",
           rename_from.string(), rename_to.string()}));
      IUTEST_EXPECT_EQ(0, rename.exit_code) << rename.out << rename.err;
      IUTEST_EXPECT_EQ(!allowed, fs::exists(rename_from));
      IUTEST_EXPECT_TRUE(fs::exists(rename_to));
    }
  }
}

IUTEST(SandboxTest, XcodeAssetScratchPathsAreScopedToTheWorker) {
  namespace fs = std::filesystem;
  cpa_test::TempDir dir;
  const auto user_dirs = sandbox::darwin_user_dirs();
  IUTEST_ASSERT_FALSE(user_dirs.empty());
  const auto temp = util::real_path(user_dirs.front());
  ScopedHostPaths cleanup;
  const auto bare = temp / util::random_uuid();
  const auto sprite = temp / ("actool-sprite-atlas-scratch-" + util::random_uuid());
  auto uppercase_uuid = util::random_uuid();
  std::transform(uppercase_uuid.begin(), uppercase_uuid.end(), uppercase_uuid.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  const auto uppercase = temp / uppercase_uuid;
  for (const auto& path : {bare, sprite, uppercase}) {
    cleanup.create_directory(path);
    fs::create_directory(path / "nested");
    util::write_file(path / "existing", "private");
    util::write_file(path / "nested/existing", "private");
  }
  const auto unrelated = util::make_temp_dir("cpa-asset-unrelated-", temp);
  cleanup.paths.push_back(unrelated);
  util::write_file(unrelated / "existing", "private");
  fs::create_directory_symlink(unrelated, bare / "redirect");
  const auto alias = temp / ("cpa-asset-alias-" + util::random_uuid());
  fs::create_directory_symlink(bare, alias);
  cleanup.paths.push_back(alias);
  const auto redirect = temp / util::random_uuid();
  fs::create_symlink(unrelated / "existing", redirect);
  cleanup.paths.push_back(redirect);

  auto invalid_guid = util::random_uuid();
  invalid_guid[0] = 'g';
  auto misplaced_hyphen = util::random_uuid();
  std::swap(misplaced_hyphen[7], misplaced_hyphen[8]);
  const std::vector<fs::path> invalid = {
      temp / invalid_guid,
      temp / misplaced_hyphen,
      temp / (util::random_uuid() + ".extra"),
      temp / ("actool-sprite-atlas-scratch-" + invalid_guid),
      temp / ("actool-sprite-atlas-scratch-" + util::random_uuid() + ".extra"),
      temp / ("actool-sprite-atlas-scratchX-" + util::random_uuid()),
      unrelated / util::random_uuid(),
  };
  for (const auto& path : invalid) {
    if (path.parent_path() == temp) cleanup.create_file(path);
    util::write_file(path, "private");
  }

  sandbox::SandboxConfig config;
  config.profile_file = dir / "profile.sb";
  sandbox::Sandbox box(config, {});
  const auto wrap = [&](bool allow_xcode) {
    return box.wrap({"/usr/bin/true"}, {}, false, false, false, {}, {}, false, allow_xcode);
  };
  const auto denied_profile = fs::path(wrap(false)[2]);
  const auto actual_profile = fs::path(wrap(true)[2]);
  IUTEST_EXPECT_NE(denied_profile, actual_profile);
  IUTEST_EXPECT_EQ(denied_profile.string(), wrap(false)[2]);
  IUTEST_EXPECT_EQ(actual_profile.string(), wrap(true)[2]);
  const auto worker_profile =
      asset_profile_for_perl(actual_profile, dir / "ibtoold-perl.sb", "com.apple.ibtool", "ibtoold");
  const auto probe = [&](const fs::path& profile, const std::string& operation, bool allowed, const fs::path& path,
                         const fs::path& other = {}) {
    const auto result = asset_path_probe(profile, operation, allowed, path, other);
    IUTEST_EXPECT_EQ(0, result.exit_code) << operation << " " << path.string() << ": " << result.out << result.err;
  };
  for (const bool enabled : {false, true}) {
    const auto& profile = enabled ? worker_profile : denied_profile;
    for (const auto& path : {bare / "existing", bare / "nested/existing", sprite / "existing",
                             sprite / "nested/existing", uppercase / "existing", alias / "existing"}) {
      probe(profile, "read", enabled, path);
      probe(profile, "modify", enabled, path);
    }
    for (const auto& path : invalid) {
      probe(profile, "read", false, path);
      probe(profile, "modify", false, path);
    }
    for (const auto& path : {bare / "redirect/existing", redirect}) {
      probe(profile, "read", false, path);
      probe(profile, "modify", false, path);
    }
    probe(profile, "create", false, bare / "redirect/new");
    probe(profile, "list", false, temp);
    for (const auto& path : {temp / util::random_uuid(), temp / ("actool-sprite-atlas-scratch-" + util::random_uuid()),
                             bare / ("new-" + util::random_uuid())}) {
      IUTEST_ASSERT_FALSE(fs::exists(path));
      probe(profile, "create", enabled, path);
      if (fs::exists(path)) cleanup.paths.push_back(path);
      if (enabled && fs::exists(path)) {
        probe(profile, "unlink", true, path);
        IUTEST_EXPECT_FALSE(fs::exists(path));
      }
    }
    const auto new_dir = temp / util::random_uuid();
    IUTEST_ASSERT_FALSE(fs::exists(new_dir));
    probe(profile, "mkdir", enabled, new_dir);
    if (fs::exists(new_dir)) cleanup.paths.push_back(new_dir);
  }
  for (const auto& path : {bare / "existing", sprite / "existing"}) {
    probe(actual_profile, "read", false, path);
    probe(actual_profile, "modify", false, path);
    probe(worker_profile, "chmod", false, path);
  }
  const auto late = temp / util::random_uuid();
  cleanup.create_file(late);
  util::write_file(late, "created after the profile was cached");
  IUTEST_EXPECT_EQ(actual_profile.string(), wrap(true)[2]);
  probe(worker_profile, "read", true, late);
  probe(actual_profile, "read", false, late);

  const auto source = bare / "rename-from";
  const auto destination = sprite / "rename-to";
  util::write_file(source, "source");
  util::write_file(destination, "destination");
  probe(worker_profile, "rename", true, source, destination);
  IUTEST_EXPECT_FALSE(fs::exists(source));
  IUTEST_EXPECT_EQ("source", util::read_file(destination));
  const auto cross_source = bare / "cross-from";
  const auto cross_destination = unrelated / "cross-to";
  util::write_file(cross_source, "source");
  util::write_file(cross_destination, "destination");
  probe(worker_profile, "rename", false, cross_source, cross_destination);
  IUTEST_EXPECT_EQ("source", util::read_file(cross_source));
  IUTEST_EXPECT_EQ("destination", util::read_file(cross_destination));
  probe(worker_profile, "rename", false, cross_destination, cross_source);
  IUTEST_EXPECT_EQ("source", util::read_file(cross_source));
  IUTEST_EXPECT_EQ("destination", util::read_file(cross_destination));
}

IUTEST(SandboxTest, XcodeAssetSimulatorWorkerRequiresBothPermissions) {
  namespace fs = std::filesystem;
  cpa_test::TempDir dir;
  const auto user_dirs = sandbox::darwin_user_dirs();
  IUTEST_ASSERT_FALSE(user_dirs.empty());
  const auto temp = util::real_path(user_dirs.front());
  const auto simulator_name = [](const std::string& device, const std::string& standalone) {
    return "com.apple.CoreSimulator.SimDevice." + device + ".Standalone." + standalone;
  };
  ScopedHostPaths cleanup;
  const auto simulator = temp / simulator_name(util::random_uuid(), util::random_uuid());
  cleanup.create_directory(simulator);
  const auto uuid = simulator / util::random_uuid();
  fs::create_directory(uuid);
  fs::create_directory(uuid / "nested");
  util::write_file(uuid / "nested/existing", "private");
  const auto catalog = simulator / "Untitled-Ab12.uicatalog";
  const auto journal = simulator / "Untitled-Ab12.uicatalog-journal";
  util::write_file(catalog, "private");
  util::write_file(journal, "private");
  const auto unrelated = util::make_temp_dir("cpa-asset-sim-unrelated-", temp);
  cleanup.paths.push_back(unrelated);
  util::write_file(unrelated / "existing", "private");
  fs::create_directory_symlink(unrelated, uuid / "redirect");
  const auto catalog_dir = simulator / "Untitled-Dir.uicatalog";
  fs::create_directory(catalog_dir);
  util::write_file(catalog_dir / "existing", "private");

  auto invalid_guid = util::random_uuid();
  invalid_guid[0] = 'g';
  const auto invalid_device = temp / simulator_name(invalid_guid, util::random_uuid());
  const auto invalid_standalone = temp / simulator_name(util::random_uuid(), invalid_guid);
  for (const auto& path : {invalid_device, invalid_standalone}) {
    cleanup.create_directory(path);
    util::write_file(path / uuid.filename(), "private");
  }
  const std::vector<fs::path> invalid = {
      simulator / invalid_guid,
      simulator / (util::random_uuid() + ".extra"),
      simulator / "Untitled-.uicatalog",
      simulator / "Untitled-Ab12.uicatalog-wal",
      simulator / "Untitled-Ab12.uicatalog-shm",
      simulator / "Untitled-Ab12.uicatalog-journal-extra",
      catalog_dir / "existing",
      invalid_device / uuid.filename(),
      invalid_standalone / uuid.filename(),
      unrelated / uuid.filename(),
      unrelated / "Untitled-Ab12.uicatalog",
  };
  for (const auto& path : invalid) {
    if (!fs::exists(path)) util::write_file(path, "private");
  }

  for (const bool allow_simulator : {false, true}) {
    sandbox::SandboxConfig config;
    config.profile_file = dir / (allow_simulator ? "simulator.sb" : "no-simulator.sb");
    config.allow_xcode_simulator = allow_simulator;
    sandbox::Sandbox box(config, {});
    const auto wrap = [&](bool allow_xcode) {
      return box.wrap({"/usr/bin/true"}, {}, false, false, false, {}, {}, false, allow_xcode);
    };
    const auto denied_profile = fs::path(wrap(false)[2]);
    const auto actual_profile = fs::path(wrap(true)[2]);
    IUTEST_EXPECT_EQ(denied_profile.string(), wrap(false)[2]);
    IUTEST_EXPECT_EQ(actual_profile.string(), wrap(true)[2]);
    const auto agent_profile = allow_simulator ? asset_profile_for_perl(actual_profile, dir / "simulator-perl.sb",
                                                                        "com.apple.dt.AssetCatalogSimulatorAgent",
                                                                        "AssetCatalogSimulatorAgent")
                                               : actual_profile;
    const auto probe = [&](const fs::path& profile, const std::string& operation, bool allowed, const fs::path& path,
                           const fs::path& other = {}) {
      const auto result = asset_path_probe(profile, operation, allowed, path, other);
      IUTEST_EXPECT_EQ(0, result.exit_code) << operation << " " << path.string() << ": " << result.out << result.err;
    };
    for (const bool allow_xcode : {false, true}) {
      const bool allowed = allow_simulator && allow_xcode;
      const auto& profile = allowed ? agent_profile : (allow_xcode ? actual_profile : denied_profile);
      for (const auto& path : {uuid / "nested/existing", catalog, journal}) {
        probe(profile, "read", allowed, path);
        probe(profile, "modify", allowed, path);
      }
      if (!allowed) continue;
      for (const auto& path : invalid) {
        probe(profile, "read", false, path);
        probe(profile, "modify", false, path);
      }
      probe(profile, "read", false, uuid / "redirect/existing");
      probe(profile, "create", false, uuid / "redirect/new");
      probe(profile, "chmod", false, catalog);
      for (const auto& path :
           {uuid / "new", simulator / "Untitled-New1.uicatalog", simulator / "Untitled-New1.uicatalog-journal"}) {
        IUTEST_ASSERT_FALSE(fs::exists(path));
        probe(profile, "create", true, path);
        if (fs::exists(path)) {
          probe(profile, "unlink", true, path);
          IUTEST_EXPECT_FALSE(fs::exists(path));
        }
      }
      const auto source = simulator / "Untitled-From.uicatalog";
      const auto destination = simulator / "Untitled-To.uicatalog";
      util::write_file(source, "source");
      util::write_file(destination, "destination");
      probe(profile, "rename", true, source, destination);
      IUTEST_EXPECT_FALSE(fs::exists(source));
      IUTEST_EXPECT_EQ("source", util::read_file(destination));
      const auto cross_source = simulator / "Untitled-Cross.uicatalog";
      const auto cross_destination = unrelated / "cross-destination";
      util::write_file(cross_source, "source");
      util::write_file(cross_destination, "destination");
      probe(profile, "rename", false, cross_source, cross_destination);
      IUTEST_EXPECT_EQ("source", util::read_file(cross_source));
      IUTEST_EXPECT_EQ("destination", util::read_file(cross_destination));
    }
    for (const auto& path : {uuid / "nested/existing", catalog, journal}) {
      probe(actual_profile, "read", false, path);
      probe(actual_profile, "modify", false, path);
    }
  }
}

IUTEST(SandboxTest, XcodeAssetWorkersRejectSpoofedSigningIdentifiers) {
  namespace fs = std::filesystem;
  cpa_test::TempDir dir;
  const auto user_dirs = sandbox::darwin_user_dirs();
  IUTEST_ASSERT_FALSE(user_dirs.empty());
  const auto temp = util::real_path(user_dirs.front());
  ScopedHostPaths cleanup;
  const auto direct = temp / util::random_uuid();
  cleanup.create_file(direct);
  util::write_file(direct, "private");
  const auto simulator =
      temp / ("com.apple.CoreSimulator.SimDevice." + util::random_uuid() + ".Standalone." + util::random_uuid());
  cleanup.create_directory(simulator);
  const auto nested = simulator / util::random_uuid();
  util::write_file(nested, "private");

  sandbox::SandboxConfig config;
  config.profile_file = dir / "profile.sb";
  config.read = {dir.path()};
  config.allow_xcode_simulator = true;
  sandbox::Sandbox box(config, {});
  const auto wrap = [&](const std::vector<std::string>& argv, bool allow_xcode) {
    return box.wrap(argv, {}, false, false, false, {}, {}, false, allow_xcode);
  };
  const auto actual_profile = fs::path(wrap({"/usr/bin/true"}, true)[2]);
  struct Worker {
    std::string executable;
    std::string identifier;
    fs::path existing;
    fs::path fresh;
  };
  const std::vector<Worker> workers = {
      {"ibtoold", "com.apple.ibtool", direct, temp / util::random_uuid()},
      {"AssetCatalogSimulatorAgent", "com.apple.dt.AssetCatalogSimulatorAgent", nested,
       simulator / util::random_uuid()},
  };
  for (const auto& worker : workers) {
    const auto fake = dir / worker.executable;
    fs::copy_file(CPA_MSBUILD_SOCKET_PROBE, fake);
    const auto without_platform = "(signing-identifier " + sandbox::sbpl_quote(worker.identifier) +
                                  ")\n"
                                  "    (process-path-regex #\"^/.*/" +
                                  worker.executable + "$\")";
    const auto control_profile = dir / (worker.executable + "-identity-only.sb");
    util::write_file(control_profile, replace_asset_worker(util::read_file(actual_profile), worker.identifier,
                                                           worker.executable, without_platform));
    const auto control = [&](const fs::path& executable, const std::string& operation, const fs::path& path) {
      return util::capture(
          {sandbox::kSandboxExec, "-f", control_profile.string(), executable.string(), operation, path.string()});
    };
    auto result = control(fake, "read-deny", worker.existing);
    IUTEST_ASSERT_EQ(0, result.exit_code) << result.out << result.err;
    const auto sign = util::capture(
        {"/usr/bin/codesign", "--force", "--sign", "-", "--identifier", worker.identifier, fake.string()});
    IUTEST_ASSERT_EQ(0, sign.exit_code) << sign.out << sign.err;
    result = util::capture({fake.string(), "read-allow", worker.existing.string()});
    IUTEST_ASSERT_EQ(0, result.exit_code) << result.out << result.err;
    result = control(fake, "read-allow", worker.existing);
    IUTEST_ASSERT_EQ(0, result.exit_code) << result.out << result.err;
    result = control(fake, "file-allow", worker.fresh);
    IUTEST_ASSERT_EQ(0, result.exit_code) << result.out << result.err;
    const auto wrong_name = dir / (worker.executable + "-other");
    fs::copy_file(fake, wrong_name);
    result = control(wrong_name, "read-deny", worker.existing);
    IUTEST_EXPECT_EQ(0, result.exit_code) << result.out << result.err;
    for (const bool allow_xcode : {false, true}) {
      for (const auto& operation : {"read-deny", "modify-deny"}) {
        result = util::capture(wrap({fake.string(), operation, worker.existing.string()}, allow_xcode));
        IUTEST_EXPECT_EQ(0, result.exit_code) << worker.executable << ": " << result.out << result.err;
      }
      result = util::capture(wrap({fake.string(), "file-deny", worker.fresh.string()}, allow_xcode));
      IUTEST_EXPECT_EQ(0, result.exit_code) << worker.executable << ": " << result.out << result.err;
    }
  }
}

IUTEST(SandboxTest, XcodeCompilerCachesAreGrantedJobWideOrPerInvocation) {
  namespace fs = std::filesystem;
  cpa_test::TempDir dir;
  const auto user_dirs = sandbox::darwin_user_dirs();
  IUTEST_ASSERT_EQ(2u, user_dirs.size());
  const auto cache = util::real_path(user_dirs[1]);
  const auto tag = "cpa-test-" + util::random_uuid();
  const auto module_cache = cache / "org.llvm.clang/ModuleCache.noindex";
  const auto tools = cache / "com.apple.DeveloperTools" / tag;
  const auto stat_cache = tools / "Xcode/SDKStatCaches.noindex";
  ScopedHostPaths cleanup;
  fs::create_directories(module_cache);
  fs::create_directories(stat_cache);
  cleanup.paths.push_back(tools);
  const auto unrelated_tools = cache / "com.apple.DeveloperTools" / (tag + "-other");
  const auto new_tools = cache / "com.apple.DeveloperTools" / (tag + "-new");
  cleanup.paths.push_back(new_tools);
  for (const bool job_wide : {false, true}) {
    sandbox::SandboxConfig config;
    config.profile_file = dir / (job_wide ? "job.sb" : "base.sb");
    config.allow_xcode = job_wide;
    sandbox::Sandbox box(config, {});
    for (const bool invocation : {false, true}) {
      const bool allowed = job_wide || invocation;
      const auto probe = [&](const std::string& mode, const fs::path& path) {
        return util::capture(box.wrap({CPA_MSBUILD_SOCKET_PROBE, mode, path.string()}, {}, false, false, false, {}, {},
                                      false, invocation));
      };
      const auto allow = allowed ? "file-allow" : "file-deny";
      for (const auto& path : {module_cache / (tag + ".pcm"), stat_cache / (tag + ".sdkstatcache")}) {
        const auto result = probe(allow, path);
        IUTEST_EXPECT_EQ(0, result.exit_code) << path.string() << ": " << result.out << result.err;
      }
      for (const auto& path : {cache / tag, cache / "org.llvm.clang" / tag, tools / tag, tools / "Xcode" / tag,
                               unrelated_tools}) {
        const auto result = probe("file-deny", path);
        IUTEST_EXPECT_EQ(0, result.exit_code) << path.string() << ": " << result.out << result.err;
      }
      const auto created = util::capture(box.wrap({"/bin/mkdir", new_tools.string()}, {}, false, false, false, {}, {},
                                                   false, invocation));
      IUTEST_EXPECT_EQ(allowed, created.exit_code == 0) << created.out << created.err;
      std::error_code ec;
      fs::remove(new_tools, ec);
    }
  }
}

IUTEST(SandboxTest, XcodeSimulatorOptionIsInheritedByDerivedProfiles) {
  cpa_test::TempDir dir;
  const std::string service = "com.apple.CoreSimulator.CoreSimulatorService";
  const bool available = util::capture({CPA_APPLE_SECURITY_PROBE, service, "allow"}).exit_code == 0;
  for (const bool enabled : {false, true}) {
    sandbox::SandboxConfig config;
    config.profile_file = dir / (enabled ? "enabled.sb" : "disabled.sb");
    config.read = {CPA_APPLE_SECURITY_PROBE};
    config.allow_xcode_simulator = enabled;
    sandbox::Sandbox box(config, {});
    for (const bool extra_read : {false, true}) {
      for (const bool apple_security : {false, true}) {
        const std::vector<std::filesystem::path> read = extra_read
            ? std::vector<std::filesystem::path>{CPA_APPLE_SECURITY_PROBE}
            : std::vector<std::filesystem::path>{};
        const auto argv = box.wrap({CPA_APPLE_SECURITY_PROBE, service, enabled ? "allow" : "deny"},
                                    read, apple_security);
        const auto profile = util::read_file(argv[2]);
        IUTEST_EXPECT_EQ(enabled, profile.find("(global-name \"" + service + "\")") != std::string::npos);
        IUTEST_EXPECT_EQ(enabled, profile.find("DeveloperDiskImages") != std::string::npos);
        IUTEST_EXPECT_EQ(apple_security,
                         profile.find("(global-name \"com.apple.SecurityServer\")") != std::string::npos);
        IUTEST_EXPECT_EQ(argv[2], box.wrap({"/usr/bin/true"}, read, apple_security)[2]);
        IUTEST_EXPECT_EQ(0, util::capture(box.wrap({"/usr/bin/true"}, read, apple_security)).exit_code);
        if (available) IUTEST_EXPECT_EQ(0, util::capture(argv).exit_code);
      }
    }
  }
}
#endif
