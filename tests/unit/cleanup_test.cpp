#include "iutest.hpp"

#include <sys/stat.h>

#include <filesystem>

#include "engine/cleanup.hpp"
#include "test_support.hpp"
#include "util/fs.hpp"

using namespace cpa;
namespace fs = std::filesystem;

namespace {

struct RetainedRun {
  cpa_test::TempDir temp_root;
  cpa_test::TempDir parent;
  fs::path run;
  fs::path work_dir;
  fs::path unity_dir;

  RetainedRun() {
    run = util::make_temp_dir("run-", parent.path());
    fs::create_directories(run / "logs");
    fs::create_directories(run / "artifacts" / "1");
    fs::create_directories(run / "snapshot" / "repo");
    util::write_file(run / "report.json", "{}\n");
    work_dir = util::make_temp_dir("cpa-w-", temp_root.path());
    fs::create_directories(work_dir / "work" / "repo");
    unity_dir = util::make_temp_dir("cpa-u-", temp_root.path());
    add_job("build", work_dir / "work", unity_dir);
  }

  fs::path add_job(const std::string& key, const fs::path& work, const fs::path& unity = {}) {
    const auto job = run / "jobs" / "1" / key;
    fs::create_directories(job / "home");
    fs::create_directories(run / "control" / "1" / key);
    util::write_file(job / "work-path", work.string() + "\n");
    if (!unity.empty()) util::write_file(job / "unity-tmp-path", unity.string() + "\n");
    return job;
  }

  engine::CleanOptions options() const {
    engine::CleanOptions o;
    o.temp_root = temp_root.path();
    return o;
  }
};

}  // namespace

IUTEST(CleanupTest, RemovesRecordedDirectoriesAndKeepsResults) {
  RetainedRun r;
  IUTEST_ASSERT_TRUE(engine::has_retained_data(r.run));
  const auto report = engine::clean_retained_run(r.run, r.options());
  IUTEST_ASSERT_TRUE(report.errors.empty()) << report.errors.front();
  IUTEST_EXPECT_FALSE(report.skipped);
  IUTEST_EXPECT_FALSE(fs::exists(r.work_dir));
  IUTEST_EXPECT_FALSE(fs::exists(r.unity_dir));
  for (const char* name : {"jobs", "control", "snapshot"}) IUTEST_EXPECT_FALSE(fs::exists(r.run / name)) << name;
  for (const char* name : {"logs", "artifacts/1", "report.json"}) IUTEST_EXPECT_TRUE(fs::exists(r.run / name)) << name;
  IUTEST_EXPECT_FALSE(engine::has_retained_data(r.run));
  const auto again = engine::clean_retained_run(r.run, r.options());
  IUTEST_EXPECT_TRUE(again.skipped);
  IUTEST_EXPECT_TRUE(again.errors.empty());
}

IUTEST(CleanupTest, DryRunRemovesNothing) {
  RetainedRun r;
  auto options = r.options();
  options.dry_run = true;
  const auto report = engine::clean_retained_run(r.run, options);
  IUTEST_ASSERT_TRUE(report.errors.empty());
  IUTEST_EXPECT_FALSE(report.removed.empty());
  IUTEST_EXPECT_TRUE(fs::exists(r.work_dir / "work" / "repo"));
  IUTEST_EXPECT_TRUE(fs::exists(r.unity_dir));
  IUTEST_EXPECT_TRUE(fs::exists(r.run / "jobs" / "1" / "build" / "work-path"));
  IUTEST_EXPECT_TRUE(fs::exists(r.run / "snapshot"));
}

IUTEST(CleanupTest, RefusesUnexpectedRecordedDirectoriesAndKeepsTheirMetadata) {
  RetainedRun r;
  const auto outside = util::make_temp_dir("cpa-w-", r.parent.path());
  const auto other = r.temp_root / "other";
  fs::create_directories(other / "work");
  const auto target = r.temp_root / "target";
  fs::create_directories(target / "work");
  const auto link = r.temp_root / "cpa-w-Linked";
  fs::create_directory_symlink(target, link);
  const std::vector<std::pair<std::string, fs::path>> bad = {
      {"outside", outside / "work"},
      {"unprefixed", other / "work"},
      {"symlink", link / "work"},
      {"relative", fs::path("cpa-w-ABCDEF/work")},
      {"traversal", r.temp_root.path() / "cpa-w-ABCDEF" / ".." / "other" / "work"},
      {"not-work", r.work_dir}};
  for (const auto& [key, path] : bad) r.add_job(key, path);
  const auto manifest_link = r.add_job("manifest-link", r.work_dir / "work");
  fs::remove(manifest_link / "work-path");
  fs::create_symlink(r.run / "jobs" / "1" / "build" / "work-path", manifest_link / "work-path");

  const auto report = engine::clean_retained_run(r.run, r.options());
  IUTEST_EXPECT_EQ(bad.size() + 1, report.errors.size());
  IUTEST_EXPECT_TRUE(fs::exists(outside));
  IUTEST_EXPECT_TRUE(fs::exists(other / "work"));
  IUTEST_EXPECT_TRUE(fs::exists(target / "work"));
  IUTEST_EXPECT_TRUE(fs::is_symlink(link));
  for (const auto& [key, path] : bad) {
    IUTEST_EXPECT_TRUE(fs::exists(r.run / "jobs" / "1" / key / "work-path")) << key;
    IUTEST_EXPECT_TRUE(fs::exists(r.run / "control" / "1" / key)) << key;
  }
  IUTEST_EXPECT_FALSE(fs::exists(r.work_dir));
  IUTEST_EXPECT_FALSE(fs::exists(r.run / "jobs" / "1" / "build"));
  IUTEST_EXPECT_FALSE(fs::exists(r.run / "control" / "1" / "build"));
  IUTEST_EXPECT_FALSE(fs::exists(r.run / "snapshot"));
}

IUTEST(CleanupTest, RequiresForceForRunsWithoutReport) {
  RetainedRun r;
  fs::remove(r.run / "report.json");
  const auto skipped = engine::clean_retained_run(r.run, r.options());
  IUTEST_EXPECT_TRUE(skipped.skipped);
  IUTEST_EXPECT_TRUE(fs::exists(r.work_dir));
  auto options = r.options();
  options.force = true;
  const auto report = engine::clean_retained_run(r.run, options);
  IUTEST_EXPECT_FALSE(report.skipped);
  IUTEST_EXPECT_FALSE(fs::exists(r.work_dir));
}

IUTEST(CleanupTest, NeverCleansARunThatHoldsItsLock) {
  RetainedRun r;
  {
    const engine::RunLock lock(r.run);
    auto options = r.options();
    options.force = true;
    const auto report = engine::clean_retained_run(r.run, options);
    IUTEST_EXPECT_TRUE(report.skipped);
    IUTEST_EXPECT_TRUE(fs::exists(r.work_dir));
    IUTEST_EXPECT_TRUE(fs::exists(r.run / "jobs"));
    IUTEST_EXPECT_THROW(engine::RunLock{r.run}, std::system_error);
  }
  const auto report = engine::clean_retained_run(r.run, r.options());
  IUTEST_EXPECT_FALSE(report.skipped);
  IUTEST_EXPECT_FALSE(fs::exists(r.work_dir));
}

IUTEST(CleanupTest, RejectsDirectoriesThatAreNotRuns) {
  RetainedRun r;
  const auto link = r.parent / "linked-run";
  fs::create_directory_symlink(r.run, link);
  IUTEST_EXPECT_FALSE(engine::clean_retained_run(link, r.options()).errors.empty());
  ::chmod(r.run.c_str(), 0755);
  IUTEST_EXPECT_FALSE(engine::clean_retained_run(r.run, r.options()).errors.empty());
  ::chmod(r.run.c_str(), 0700);
  fs::remove_all(r.run / "logs");
  IUTEST_EXPECT_FALSE(engine::clean_retained_run(r.run, r.options()).errors.empty());
  IUTEST_EXPECT_TRUE(fs::exists(r.work_dir));
  IUTEST_EXPECT_TRUE(fs::exists(r.run / "jobs"));
}
