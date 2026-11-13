#include "iutest.hpp"

#include <unistd.h>

#include <cerrno>
#include <future>
#include <system_error>

#include "engine/runner.hpp"
#include "sandbox/hosted_python.hpp"
#include "test_support.hpp"

using namespace cpa;
namespace fs = std::filesystem;

IUTEST(HostedPythonTest, UsesTheFixedMacOSCacheByDefault) {
  IUTEST_EXPECT_EQ(fs::path("/Users/runner/hostedtoolcache"), engine::SandboxOptions{}.hosted_python_tool_cache);
}

IUTEST(HostedPythonTest, ProvisioningHintUsesOnlySupportedCommandArguments) {
  for (const auto& version : {"3.11", "3.11.9", "3.12.0"}) {
    const auto hint = sandbox::hosted_python_provisioning_hint(version, "arm64");
    IUTEST_EXPECT_NE(std::string::npos, hint.find("scripts/install-hosted-python.sh --version " + std::string(version) +
                                                  " --architecture arm64 --dry-run"));
    IUTEST_EXPECT_NE(std::string::npos, hint.find("not setup-steps"));
    IUTEST_EXPECT_NE(std::string::npos, hint.find("--accept-system-changes"));
  }
  for (const auto& version : {"", "pypy3.10", "3.13t", "3.11\n3.12", "3.11; touch /tmp/unsafe", "3..11", "2.7"}) {
    const auto hint = sandbox::hosted_python_provisioning_hint(version, "arm64; unsafe");
    IUTEST_EXPECT_NE(std::string::npos, hint.find("--version '<VERSION>' --dry-run")) << hint;
    IUTEST_EXPECT_EQ(std::string::npos, hint.find("unsafe")) << hint;
    IUTEST_EXPECT_EQ(std::string::npos, hint.find("--architecture")) << hint;
  }
}

IUTEST(HostedPythonTest, MissingCacheExplainsHostProvisioning) {
  cpa_test::TempDir dir;
  try {
    sandbox::validate_hosted_python_cache(dir / "missing-cache", {});
    IUTEST_FAIL() << "Missing cache was accepted";
  } catch (const std::runtime_error& error) {
    IUTEST_EXPECT_NE(std::string::npos, std::string(error.what()).find("scripts/install-hosted-python.sh"));
    IUTEST_EXPECT_NE(std::string::npos, std::string(error.what()).find("host terminal"));
  }
}

IUTEST(HostedPythonTest, ValidatesAnExistingDisjointCache) {
  cpa_test::TempDir dir;
  const auto cache = dir / "runner/hostedtoolcache";
  IUTEST_EXPECT_THROW(sandbox::validate_hosted_python_cache(cache, {}), std::runtime_error);
  fs::create_directories(cache);
  IUTEST_EXPECT_NO_THROW(sandbox::validate_hosted_python_cache(cache, {dir / "other"}));
  for (const auto& writable : {cache.parent_path(), cache, cache / "Python"}) {
    IUTEST_EXPECT_THROW(sandbox::validate_hosted_python_cache(cache, {writable}), std::runtime_error)
        << writable.string();
  }
  IUTEST_EXPECT_THROW(sandbox::validate_hosted_python_cache("/", {}), std::invalid_argument);
  IUTEST_EXPECT_THROW(sandbox::validate_hosted_python_cache("relative/cache", {}), std::invalid_argument);

  fs::create_directory_symlink(cache.parent_path(), dir / "runner-link");
  IUTEST_EXPECT_THROW(sandbox::validate_hosted_python_cache(dir / "runner-link/hostedtoolcache", {}),
                      std::runtime_error);
  if (::geteuid() != 0) {
    fs::permissions(cache, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    IUTEST_EXPECT_THROW(sandbox::validate_hosted_python_cache(cache, {}), std::runtime_error);
    fs::permissions(cache, fs::perms::owner_all, fs::perm_options::replace);
  }
}

IUTEST(HostedPythonTest, CreatesMissingDirectoriesAndPreservesExistingCache) {
  cpa_test::TempDir dir;
  const auto cache = dir / "runner/hostedtoolcache";
  IUTEST_EXPECT_TRUE(sandbox::ensure_hosted_python_cache(cache, {}));
  IUTEST_ASSERT_TRUE(fs::is_directory(cache));
  util::write_file(cache / "existing", "retained");
  const auto permissions = fs::status(cache).permissions();
  IUTEST_EXPECT_FALSE(sandbox::ensure_hosted_python_cache(cache, {}));
  IUTEST_EXPECT_EQ("retained", util::read_file(cache / "existing"));
  IUTEST_EXPECT_EQ(permissions, fs::status(cache).permissions());
  IUTEST_EXPECT_TRUE(sandbox::ensure_hosted_python_cache(cache.parent_path() / "another-cache", {}));
}

IUTEST(HostedPythonTest, RejectsInvalidOrOverlappingPathsBeforeCreatingDirectories) {
  cpa_test::TempDir dir;
  const auto parent = dir / "writable";
  const auto cache = parent / "hostedtoolcache";
  fs::create_directories(parent);
  IUTEST_EXPECT_THROW(sandbox::ensure_hosted_python_cache(cache, {parent}), std::runtime_error);
  IUTEST_EXPECT_FALSE(fs::exists(cache));
  IUTEST_EXPECT_THROW(sandbox::ensure_hosted_python_cache("/", {}), std::invalid_argument);
  IUTEST_EXPECT_THROW(sandbox::ensure_hosted_python_cache("relative/cache", {}), std::invalid_argument);
  IUTEST_EXPECT_THROW(sandbox::ensure_hosted_python_cache(dir / "missing/../cache", {}), std::invalid_argument);
  IUTEST_EXPECT_THROW(sandbox::ensure_hosted_python_cache(dir / "bad\nname/cache", {}), std::invalid_argument);
  IUTEST_EXPECT_FALSE(fs::exists(dir / "missing"));
  IUTEST_EXPECT_FALSE(fs::exists(dir / "bad\nname"));

  util::write_file(dir / "file", "not a directory");
  IUTEST_EXPECT_THROW(sandbox::ensure_hosted_python_cache(dir / "file/cache", {}), std::runtime_error);
  IUTEST_EXPECT_EQ("not a directory", util::read_file(dir / "file"));
}

IUTEST(HostedPythonTest, DoesNotCreateThroughSymlinkedAncestors) {
  cpa_test::TempDir dir;
  const auto target = dir / "target";
  fs::create_directories(target);
  fs::create_directory_symlink(target, dir / "alias");
  IUTEST_EXPECT_THROW(sandbox::ensure_hosted_python_cache(dir / "alias/hostedtoolcache", {}), std::runtime_error);
  IUTEST_EXPECT_FALSE(fs::exists(target / "hostedtoolcache"));
  fs::create_directory_symlink(dir / "missing", dir / "dangling");
  IUTEST_EXPECT_THROW(sandbox::ensure_hosted_python_cache(dir / "dangling/hostedtoolcache", {}), std::runtime_error);
  IUTEST_EXPECT_FALSE(fs::exists(dir / "missing"));
}

IUTEST(HostedPythonTest, ReportsTheComponentWhenCreationIsDenied) {
  if (::geteuid() == 0) IUTEST_SKIP() << "Root bypasses the fixture's directory permissions";
  cpa_test::TempDir dir;
  const auto parent = dir / "no-write";
  const auto failing_component = parent / "runner";
  const auto cache = failing_component / "hostedtoolcache";
  fs::create_directories(parent);
  fs::permissions(parent, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
  std::string error;
  try {
    sandbox::ensure_hosted_python_cache(cache, {});
  } catch (const std::exception& e) {
    error = e.what();
  }
  fs::permissions(parent, fs::perms::owner_all, fs::perm_options::replace);
  IUTEST_EXPECT_NE(std::string::npos, error.find("Unable to create setup-python host tool cache directory")) << error;
  IUTEST_EXPECT_NE(std::string::npos, error.find(failing_component.string())) << error;
  IUTEST_EXPECT_NE(std::string::npos, error.find(std::error_code(EACCES, std::generic_category()).message())) << error;
  IUTEST_EXPECT_NE(std::string::npos, error.find("scripts/install-hosted-python.sh")) << error;
  IUTEST_EXPECT_FALSE(fs::exists(failing_component));
}

IUTEST(HostedPythonTest, CreatesUnderWritableSearchOnlyParent) {
  if (::geteuid() == 0) IUTEST_SKIP() << "Root bypasses the fixture's directory permissions";
  cpa_test::TempDir dir;
  const auto parent = dir / "search-only";
  const auto cache = parent / "hostedtoolcache";
  fs::create_directories(parent);
  fs::permissions(parent, fs::perms::owner_write | fs::perms::owner_exec, fs::perm_options::replace);
  if (::access(parent.c_str(), R_OK) == 0) {
    fs::permissions(parent, fs::perms::owner_all, fs::perm_options::replace);
    IUTEST_SKIP() << "The filesystem grants read access despite the fixture permissions";
  }
  bool created = false;
  std::string error;
  try {
    created = sandbox::ensure_hosted_python_cache(cache, {});
  } catch (const std::exception& e) {
    error = e.what();
  }
  fs::permissions(parent, fs::perms::owner_all, fs::perm_options::replace);
  IUTEST_EXPECT_TRUE(created) << error;
  IUTEST_EXPECT_TRUE(fs::is_directory(cache));
}

IUTEST(HostedPythonTest, ConcurrentCreationIsIdempotent) {
  cpa_test::TempDir dir;
  const auto cache = dir / "runner/hostedtoolcache";
  std::vector<std::future<bool>> creators;
  for (int i = 0; i < 8; ++i) {
    creators.push_back(std::async(std::launch::async, [&] { return sandbox::ensure_hosted_python_cache(cache, {}); }));
  }
  for (auto& creator : creators) IUTEST_EXPECT_NO_THROW(creator.get());
  IUTEST_EXPECT_TRUE(fs::is_directory(cache));
  IUTEST_EXPECT_FALSE(sandbox::ensure_hosted_python_cache(cache, {}));
}

IUTEST(HostedPythonTest, ResolvesOnlySelectedInstallationDirectories) {
  cpa_test::TempDir dir;
  const auto cache = dir / "hostedtoolcache";
  const auto python = cache / "Python/3.11.9/arm64";
  const auto pypy = cache / "PyPy/3.10.0/arm64";
  const auto graalpy = cache / "GraalPy/24.0.0/arm64";
  for (const auto& installation : {python, pypy, graalpy}) {
    fs::create_directories(installation / "bin");
    for (const auto& reported : {installation, installation / "bin", installation / "bin/python"}) {
      const auto result = sandbox::hosted_python_installation(cache, reported);
      IUTEST_ASSERT_TRUE(result.has_value()) << reported.string();
      IUTEST_EXPECT_EQ(installation, *result);
    }
  }
  IUTEST_EXPECT_FALSE(sandbox::hosted_python_installation(cache, dir / "elsewhere").has_value());
  IUTEST_EXPECT_FALSE(sandbox::hosted_python_installation(cache, "relative/python").has_value());
  IUTEST_EXPECT_THROW(sandbox::hosted_python_installation(cache, cache / "Python"), std::runtime_error);
  IUTEST_EXPECT_THROW(sandbox::hosted_python_installation(cache, cache / "Other/1.0/arm64"), std::runtime_error);
  IUTEST_EXPECT_THROW(sandbox::hosted_python_installation(cache, cache / "Python/3.99.0/arm64"), std::runtime_error);

  fs::create_directories(cache / "Python/3.99.0");
  fs::create_directory_symlink(dir.path(), cache / "Python/3.99.0/arm64");
  IUTEST_EXPECT_THROW(sandbox::hosted_python_installation(cache, cache / "Python/3.99.0/arm64/bin/python"),
                      std::runtime_error);
}
