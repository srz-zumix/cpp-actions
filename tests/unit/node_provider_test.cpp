#include "iutest.hpp"

#include <sys/stat.h>
#include <sys/utsname.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>

#include "actions/action.hpp"
#include "actions/fetch.hpp"
#include "engine/runner.hpp"
#include "test_support.hpp"

using namespace cpa;
using namespace cpa::actions;
namespace fs = std::filesystem;

namespace {

class TestEnvironment {
 public:
  void set(const std::string& key, const std::string& value) {
    if (!saved_.contains(key)) {
      const char* previous = std::getenv(key.c_str());
      saved_[key] = previous ? std::optional<std::string>(previous) : std::nullopt;
    }
    if (::setenv(key.c_str(), value.c_str(), 1) != 0) throw std::runtime_error("Cannot set test environment");
  }

  ~TestEnvironment() {
    for (const auto& [key, value] : saved_) {
      if (value)
        ::setenv(key.c_str(), value->c_str(), 1);
      else
        ::unsetenv(key.c_str());
    }
  }

 private:
  std::map<std::string, std::optional<std::string>> saved_;
};

std::string original_path() {
  const char* path = std::getenv("PATH");
  return path ? path : "";
}

void fake_node(const fs::path& path, const std::string& version) {
  util::write_file(path, "#!/bin/sh\nprintf '%s\\n' " + util::shell_quote(version) + "\n");
  util::make_executable(path);
}

fs::path fake_nodenv_shim(const fs::path& root, bool version_succeeds = true) {
  const auto shim = root / "shims/node";
  util::write_file(shim, std::string(R"sh(#!/bin/sh
# nodenv exec "$program"
)sh") + "export NODENV_ROOT=\"" +
                             root.string() + "\"\n" + R"sh(
if [ "$1" = "--version" ]; then
)sh" + std::string(version_succeeds ? "  echo v24.0.0\n  exit 0\n" : "  exit 1\n") +
                             R"sh(fi
action_dir=$(dirname "$1")
if [ -f "$action_dir/.node-version" ]; then
  echo "nodenv: version required by .node-version is not installed" >&2
  exit 42
fi
exit 43
)sh");
  util::make_executable(shim);
  return shim;
}

fs::path installed_node(const fs::path& root, const std::string& directory) {
  return root / "versions" / directory / "bin/node";
}

std::string platform() {
#if defined(__APPLE__)
  return "darwin";
#else
  return "linux";
#endif
}

std::string architecture() {
  struct utsname u{};
  if (::uname(&u) == 0) {
    const std::string machine = u.machine;
    if (machine == "arm64" || machine == "aarch64") return "arm64";
  }
  return "x64";
}

fs::path cached_node_path(const fs::path& root, int major) {
  return root / (std::to_string(major) + "-" + platform() + "-" + architecture()) / "bin/node";
}

void fake_download(const fs::path& dir, const std::string& manifest_version, const std::string& binary_version) {
  const auto tools = dir / "tools";
  const auto manifest = dir / "manifest.json";
  const auto binary = dir / "download-node";
  util::write_file(manifest, "[{\"version\":\"" + manifest_version + "\",\"stable\":true,\"files\":[{\"platform\":\"" +
                                 platform() + "\",\"arch\":\"" + architecture() +
                                 "\",\"download_url\":\"https://github.com/actions/node-versions/test.tar.gz\"}]}]");
  fake_node(binary, binary_version);
  util::write_file(
      tools / "curl",
      "#!/bin/sh\n"
      "if [ \"$6\" = \"https://raw.githubusercontent.com/actions/node-versions/main/versions-manifest.json\" ]; then\n"
      "  cp " +
          util::shell_quote(manifest.string()) +
          " \"$5\"\n"
          "else\n"
          "  : > \"$5\"\n"
          "fi\n");
  util::make_executable(tools / "curl");
  util::write_file(tools / "tar",
                   "#!/bin/sh\n"
                   "mkdir -p \"$4/bin\"\n"
                   "cp -p " +
                       util::shell_quote(binary.string()) + " \"$4/bin/node\"\n");
  util::make_executable(tools / "tar");
}

}  // namespace

IUTEST(NodeProviderTest, SelectsLatestNodenvBinaryByReportedVersionPerMajor) {
  cpa_test::TempDir dir;
  const auto root = dir / "custom-nodenv";
  const auto shim = fake_nodenv_shim(root, false);
  fake_node(installed_node(root, "24.9.9"), "v24.9.9");
  fake_node(installed_node(root, "24.10.2"), "v24.10.2");
  const auto selected = installed_node(root, "24.10.10");
  fake_node(selected, "v24.10.10");
  fake_node(installed_node(root, "24.999.999"), "v20.31.0");
  fake_node(installed_node(root, "25.0.0"), "v25.0.0");

  const auto alias = dir / "alias/node";
  fs::create_directories(alias.parent_path());
  fs::create_symlink(shim, alias);
  const auto other_root = dir / "other-nodenv";
  fake_node(installed_node(other_root, "24.900.0"), "v24.900.0");
  TestEnvironment env;
  env.set("PATH", alias.parent_path().string() + ":" + original_path());
  env.set("NODENV_ROOT", other_root.string());

  NodeProvider provider({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(selected), provider.get(24, {}));
  IUTEST_EXPECT_EQ(fs::canonical(installed_node(root, "24.999.999")), provider.get(20, {}));

  const auto newer = installed_node(root, "new-release");
  fake_node(newer, "v24.100.0");
  IUTEST_EXPECT_EQ(fs::canonical(selected), provider.get(24, {}));
  NodeProvider next_run({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(newer), next_run.get(24, {}));
}

IUTEST(NodeProviderTest, FindsRootRecordedByShimWhenShimsDirectoryIsSymlinked) {
  cpa_test::TempDir dir;
  const auto root = dir / "custom root";
  const auto external_shims = dir / "external/shims";
  fs::create_directories(root);
  fs::create_directories(external_shims);
  fs::create_directory_symlink(external_shims, root / "shims");
  fake_nodenv_shim(root, false);
  const auto node = installed_node(root, "24.22.2");
  fake_node(node, "v24.22.2");
  const auto alias = dir / "alias/node";
  fs::create_directories(alias.parent_path());
  fs::create_symlink(root / "shims/node", alias);
  TestEnvironment env;
  env.set("PATH", alias.parent_path().string() + ":" + original_path());
  env.set("NODENV_ROOT", (dir / "wrong-root").string());
  NodeProvider provider({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(node), provider.get(24, {}));
}

IUTEST(NodeProviderTest, ComparesPrereleasesAndCachedVersions) {
  cpa_test::TempDir dir;
  const auto root = dir / "nodenv";
  fake_nodenv_shim(root);
  fake_node(installed_node(root, "rc2"), "v24.11.0-rc.2");
  const auto rc10 = installed_node(root, "rc10");
  fake_node(rc10, "v24.11.0-rc.10");
  TestEnvironment env;
  env.set("PATH", (root / "shims").string() + ":" + original_path());

  NodeProvider prerelease({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(rc10), prerelease.get(24, {}));
  const auto release = installed_node(root, "release");
  fake_node(release, "v24.11.0");
  NodeProvider stable({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(release), stable.get(24, {}));

  const auto cache = cached_node_path(dir / "cache", 24);
  fake_node(cache, "v24.11.0+build.1");
  NodeProvider equal({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(cache), equal.get(24, {}));
  fake_node(cache, "v24.12.0");
  NodeProvider cache_newer({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(cache), cache_newer.get(24, {}));
  fake_node(cache, "v24.9.999");
  NodeProvider nodenv_newer({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(release), nodenv_newer.get(24, {}));
  fake_node(cache, "v20.99.0");
  std::string warnings;
  NodeProvider wrong_cache({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(release),
                   wrong_cache.get(24, [&](std::string_view text) { warnings += std::string(text) + "\n"; }));
  IUTEST_EXPECT_NE(std::string::npos, warnings.find("does not match required major version 24")) << warnings;
}

IUTEST(NodeProviderTest, FindsNewestUsableBinaryInNestedCacheLayouts) {
  cpa_test::TempDir dir;
  const auto root = dir / "nodenv";
  fake_nodenv_shim(root);
  const auto base = cached_node_path(dir / "cache", 24).parent_path().parent_path();
  fake_node(base / "bin/node", "invalid");
  fake_node(base / "old/bin/node", "v24.9.9");
  const auto latest = base / "new/bin/node";
  fake_node(latest, "v24.10.0");
  TestEnvironment env;
  env.set("PATH", (root / "shims").string() + ":" + original_path());
  std::string warnings;
  NodeProvider provider({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(latest),
                   provider.get(24, [&](std::string_view text) { warnings += std::string(text) + "\n"; }));
  IUTEST_EXPECT_NE(std::string::npos, warnings.find("Ignoring cached Node.js")) << warnings;
}

IUTEST(NodeProviderTest, RejectsCachedSymlinkToNodenvShim) {
  cpa_test::TempDir dir;
  const auto root = dir / "nodenv";
  const auto shim = fake_nodenv_shim(root);
  const auto installed = installed_node(root, "24.0.0");
  fake_node(installed, "v24.0.0");
  const auto cache = cached_node_path(dir / "cache", 24);
  fs::create_directories(cache.parent_path());
  fs::create_symlink(shim, cache);
  TestEnvironment env;
  env.set("PATH", (root / "shims").string() + ":" + original_path());
  std::string warnings;
  NodeProvider provider({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(installed),
                   provider.get(24, [&](std::string_view text) { warnings += std::string(text) + "\n"; }));
  IUTEST_EXPECT_NE(std::string::npos, warnings.find("it is a nodenv shim")) << warnings;
}

IUTEST(NodeProviderTest, SkipsUnusableNodenvCandidates) {
  cpa_test::TempDir dir;
  const auto root = dir / "nodenv";
  const auto shim = fake_nodenv_shim(root, false);
  const auto good = installed_node(root, "good");
  fake_node(good, "v24.3.2");
  fake_node(installed_node(root, "short"), "v24.99");
  fake_node(installed_node(root, "multiline"), "v24.99.0\nunexpected");
  fake_node(installed_node(root, "overflow"), "v24.999999999999999999999999.0");
  fake_node(installed_node(root, "large-output"), std::string(5000, 'x'));
  util::write_file(installed_node(root, "not-executable"), "#!/bin/sh\nexit 0\n");
  const auto failed = installed_node(root, "failed");
  util::write_file(failed, "#!/bin/sh\nexit 7\n");
  util::make_executable(failed);
  const auto hung = installed_node(root, "hung");
  util::write_file(hung, "#!/bin/sh\ntrap '' INT TERM\nwhile :; do sleep 1; done\n");
  util::make_executable(hung);
  const auto recursive = installed_node(root, "shim-link");
  fs::create_directories(recursive.parent_path());
  fs::create_symlink(shim, recursive);
  const auto fifo = installed_node(root, "fifo");
  fs::create_directories(fifo.parent_path());
  IUTEST_ASSERT_EQ(0, ::mkfifo(fifo.c_str(), 0700));

  TestEnvironment env;
  env.set("PATH", (root / "shims").string() + ":" + original_path());
  std::string warnings;
  NodeProvider provider({}, dir / "cache");
  const auto start = std::chrono::steady_clock::now();
  IUTEST_EXPECT_EQ(fs::canonical(good),
                   provider.get(24, [&](std::string_view text) { warnings += std::string(text) + "\n"; }));
  IUTEST_EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(11));
  for (const auto* reason :
       {"invalid version output", "version output is too long", "not an executable file",
        "version check exited with code 7", "version check timed out", "points back to the nodenv shim"}) {
    IUTEST_EXPECT_NE(std::string::npos, warnings.find(reason)) << reason << "\n" << warnings;
  }
  IUTEST_EXPECT_NE(std::string::npos, warnings.find(fifo.string())) << warnings;
}

IUTEST(NodeProviderTest, OrdinaryHostAndExplicitNodeKeepPriority) {
  cpa_test::TempDir dir;
  const auto ordinary = dir / "ordinary/shims/node";
  fake_node(ordinary, "v24.1.0");
  fake_node(installed_node(dir / "ordinary", "24.99.0"), "v24.99.0");
  const auto cache = cached_node_path(dir / "cache", 24);
  fake_node(cache, "v24.100.0");
  TestEnvironment env;
  env.set("PATH", ordinary.parent_path().string() + ":" + original_path());
  NodeProvider host({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(ordinary), host.get(24, {}));

  const auto shim = fake_nodenv_shim(dir / "nodenv", false);
  NodeProvider explicit_node(shim, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(shim), explicit_node.get(24, {}));
  IUTEST_EXPECT_EQ(fs::canonical(shim), explicit_node.get(20, {}));

  fake_node(ordinary, "v20.1.0");
  NodeProvider mismatched_host({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(cache), mismatched_host.get(24, {}));
}

IUTEST(NodeProviderTest, ValidatesAndReplacesInvalidCacheOnDownload) {
  cpa_test::TempDir dir;
  const auto root = dir / "nodenv";
  fake_nodenv_shim(root, false);
  const auto cache = cached_node_path(dir / "cache", 24);
  fake_node(cache, "v20.1.0");
  fake_download(dir.path(), "24.16.0", "v24.16.0");
  TestEnvironment env;
  env.set("PATH", (root / "shims").string() + ":" + (dir / "tools").string() + ":/usr/bin:/bin");
  std::string warnings;
  NodeProvider provider({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(cache),
                   provider.get(24, [&](std::string_view text) { warnings += std::string(text) + "\n"; }));
  IUTEST_EXPECT_NE(std::string::npos, warnings.find("Ignoring cached Node.js")) << warnings;
  IUTEST_EXPECT_NE(std::string::npos, warnings.find("Downloading Node.js 24.16.0")) << warnings;
  IUTEST_EXPECT_EQ("v24.16.0\n", util::capture({cache.string(), "--version"}).out);

  NodeProvider next_run({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(cache), next_run.get(24, {}));
}

IUTEST(NodeProviderTest, RejectsInvalidDownloadAndWarnsBeforeUsingShim) {
  cpa_test::TempDir dir;
  const auto root = dir / "nodenv";
  const auto shim = fake_nodenv_shim(root, false);
  const auto cache = cached_node_path(dir / "cache", 24);
  fake_node(cache, "v20.1.0");
  fake_download(dir.path(), "24.16.0", "v24.15.0");
  TestEnvironment env;
  env.set("PATH", (root / "shims").string() + ":" + (dir / "tools").string() + ":/usr/bin:/bin");
  std::string warnings;
  NodeProvider provider({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(shim),
                   provider.get(24, [&](std::string_view text) { warnings += std::string(text) + "\n"; }));
  IUTEST_EXPECT_NE(std::string::npos, warnings.find("does not match version 24.16.0")) << warnings;
  IUTEST_EXPECT_NE(std::string::npos, warnings.find("falling back to the nodenv shim")) << warnings;
  IUTEST_EXPECT_EQ("v20.1.0\n", util::capture({cache.string(), "--version"}).out);
}

IUTEST(NodeProviderTest, PreservesFallbackAndErrorWhenDownloadFails) {
  cpa_test::TempDir dir;
  const auto tools = dir / "tools";
  util::write_file(tools / "curl", "#!/bin/sh\nexit 22\n");
  util::make_executable(tools / "curl");
  const auto host = dir / "host/node";
  fake_node(host, "v20.0.0");
  TestEnvironment env;
  env.set("PATH", host.parent_path().string() + ":" + tools.string());
  std::string warnings;
  NodeProvider provider({}, dir / "cache");
  IUTEST_EXPECT_EQ(fs::canonical(host),
                   provider.get(24, [&](std::string_view text) { warnings += std::string(text) + "\n"; }));
  IUTEST_EXPECT_NE(std::string::npos, warnings.find("Failed to download Node.js 24")) << warnings;
  IUTEST_EXPECT_NE(std::string::npos, warnings.find("Using the host Node.js")) << warnings;

  env.set("PATH", tools.string());
  NodeProvider missing({}, dir / "other-cache");
  IUTEST_EXPECT_THROW(missing.get(24, {}), ActionError);
}

IUTEST(NodeProviderTest, SandboxedActionBypassesPinnedShimWithoutExposingItsSiblings) {
  if (!engine::host_is_macos()) IUTEST_SKIP() << "Seatbelt isolation requires macOS";
  const auto host = util::find_executable("node", original_path());
  if (!host) IUTEST_SKIP() << "No host Node.js runtime";
  const auto version = util::capture({host->string(), "--version"});
  const auto text = util::trim(version.out);
  const std::string major = text.starts_with("v24.") ? "24" : text.starts_with("v20.") ? "20" : "";
  if (version.exit_code != 0 || major.empty()) IUTEST_SKIP() << "A Node.js 20 or 24 runtime is needed";
  const auto executable = util::capture({host->string(), "-p", "process.execPath"});
  if (executable.exit_code != 0 || util::trim(executable.out).empty()) IUTEST_SKIP() << "Cannot resolve host Node.js";
  const auto native_node = fs::canonical(fs::path(util::trim(executable.out)));

  struct ProtectedFixture {
    const fs::path root = util::make_temp_dir("cpa-nodenv-test-", util::home_dir());
    ~ProtectedFixture() { util::remove_tree(root); }
    fs::path operator/(const std::string& path) const { return root / path; }
  } dir;
  const auto nodenv_root = dir / "nodenv";
  const auto shim = fake_nodenv_shim(nodenv_root);
  const auto node = installed_node(nodenv_root, "installed");
  fs::create_directories(node.parent_path());
  util::copy_entry(native_node, node);
  const auto marker = node.parent_path() / "private-marker";
  util::write_file(marker, "host-only");
  IUTEST_ASSERT_EQ("host-only", util::read_file(marker));
  fs::create_directories(dir / "repo");
  cpa_test::init_repo(
      dir / "repo",
      {{".github/workflows/test.yml", R"(on: push
jobs:
  test:
    runs-on: nodenv-test-host
    steps:
      - uses: actions/checkout@v4
      - uses: ./action
)"},
       {"action/action.yml", "runs:\n  using: node" + major + "\n  pre: pre.js\n  main: main.js\n  post: post.js\n"},
       {"action/.node-version", major + ".0.99999\n"},
       {"action/pre.js", "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'pre\\n');\n"},
       {"action/main.js", R"js(const fs = require('fs');
let denied = false;
try {
  fs.readFileSync(process.env.MARKER);
} catch (error) {
  if (error.code !== 'EACCES' && error.code !== 'EPERM') throw error;
  denied = true;
}
if (!denied) throw Error('The runtime sibling was exposed');
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'main\n');
)js"},
       {"action/post.js", "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'post\\n');\n"}});
  IUTEST_EXPECT_EQ(42, util::capture({shim.string(), (dir / "repo/action/main.js").string()}).exit_code);

  const auto git = workspace::read_git_info(dir / "repo");
  const auto event = engine::build_event({}, git);
  const auto files = engine::load_workflow_files(git.root, {});
  engine::RunOptions options;
  options.sandbox.enabled = true;
  options.cache_dir = dir / "cache";
  options.run_dir = dir / "run";
  options.host_labels = {"nodenv-test-host"};
  options.pass_env = {"MARKER=" + marker.string()};
  options.parallel = 1;
  TestEnvironment env;
  env.set("PATH", (nodenv_root / "shims").string() + ":" + original_path());
  std::ostringstream log;
  engine::Console console(log, false);
  engine::Runner runner(options, console);
  const auto report = runner.run(engine::select_workflows(files, event.info, {}), event, git);
  IUTEST_EXPECT_EQ("success", report.result) << log.str();
  IUTEST_ASSERT_EQ(1u, report.workflows.size());
  IUTEST_ASSERT_EQ(1u, report.workflows.front().jobs.size());
  IUTEST_EXPECT_EQ("pre\nmain\npost\n", report.workflows.front().jobs.front().summary) << log.str();
}
