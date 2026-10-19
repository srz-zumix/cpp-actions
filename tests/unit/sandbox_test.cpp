#include "iutest.hpp"

#include <chrono>
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
#include <sys/socket.h>
#include <unistd.h>
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
#endif
