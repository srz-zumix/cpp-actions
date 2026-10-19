#include "iutest.hpp"

#include "actions/builtin.hpp"
#include "test_support.hpp"

namespace {

namespace fs = std::filesystem;
using cpa::actions::Builtin;
using cpa::actions::BuiltinContext;
using cpa::actions::BuiltinResult;
using Inputs = std::map<std::string, std::string>;

class BuiltinTest : public iutest::Test {
 protected:
  void SetUp() override {
    root = cpa::util::make_temp_dir(".builtin-test-", fs::current_path());
    ctx.workspace = root / "job-a";
    ctx.home = root / "home-a";
    ctx.writable = {ctx.workspace, ctx.home};
    ctx.cache_store = root / "caches";
    ctx.artifact_store = root / "artifacts";
    fs::create_directories(ctx.workspace);
    fs::create_directories(ctx.home);
    ctx.warning = [this](std::string_view message) { warnings.emplace_back(message); };
  }

  void TearDown() override { cpa::util::remove_tree(root); }

  BuiltinResult run(Builtin kind, const Inputs& inputs, bool post = false,
                    const std::map<std::string, std::string>& state = {}) {
    return cpa::actions::run_builtin(kind, post, inputs, state, ctx);
  }

  void next_job() {
    ctx.workspace = root / "job-b";
    ctx.home = root / "home-b";
    ctx.writable = {ctx.workspace, ctx.home};
    fs::create_directories(ctx.workspace);
    fs::create_directories(ctx.home);
  }

  fs::path root;
  BuiltinContext ctx;
  std::vector<std::string> warnings;
};

IUTEST_F(BuiltinTest, CachePostPersistsWorkspaceAndHomeAcrossJobs) {
  const Inputs inputs{{"key", "build-v1"}, {"path", "build\n~/.compiler"}};
  const auto miss = run(Builtin::Cache, inputs);
  IUTEST_ASSERT_TRUE(miss.ok) << miss.error;
  IUTEST_EXPECT_EQ(miss.outputs.end(), miss.outputs.find("cache-hit"));
  cpa::util::write_file(ctx.workspace / "build/result", "compiled");
  cpa::util::write_file(ctx.home / ".compiler/config", "settings");
  IUTEST_ASSERT_TRUE(run(Builtin::Cache, inputs, true, miss.state).ok);
  next_job();
  const auto hit = run(Builtin::CacheRestore, inputs);
  IUTEST_ASSERT_TRUE(hit.ok) << hit.error;
  IUTEST_EXPECT_EQ("true", hit.outputs.at("cache-hit"));
  IUTEST_EXPECT_EQ("build-v1", hit.outputs.at("cache-primary-key"));
  IUTEST_EXPECT_EQ("build-v1", hit.outputs.at("cache-matched-key"));
  IUTEST_EXPECT_EQ("compiled", cpa::util::read_file(ctx.workspace / "build/result"));
  IUTEST_EXPECT_EQ("settings", cpa::util::read_file(ctx.home / ".compiler/config"));
}

IUTEST_F(BuiltinTest, CacheEntriesAreImmutableAndExactHitSkipsPostSave) {
  const Inputs inputs{{"key", "immutable"}, {"path", "data"}};
  cpa::util::write_file(ctx.workspace / "data", "original");
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, inputs).ok);
  const auto hit = run(Builtin::Cache, inputs);
  IUTEST_ASSERT_TRUE(hit.ok);
  cpa::util::write_file(ctx.workspace / "data", "modified");
  IUTEST_ASSERT_TRUE(run(Builtin::Cache, inputs, true, hit.state).ok);
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, inputs).ok);
  fs::remove(ctx.workspace / "data");
  IUTEST_ASSERT_TRUE(run(Builtin::CacheRestore, inputs).ok);
  IUTEST_EXPECT_EQ("original", cpa::util::read_file(ctx.workspace / "data"));
}

IUTEST_F(BuiltinTest, CacheUsesPrimaryPrefixBeforeRestoreKeys) {
  cpa::util::write_file(ctx.workspace / "data", "prefix");
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, {{"key", "linux-v2"}, {"path", "data"}}).ok);
  cpa::util::write_file(ctx.workspace / "data", "fallback");
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, {{"key", "fallback"}, {"path", "data"}}).ok);
  const auto restored =
      run(Builtin::CacheRestore, {{"key", "linux-"}, {"path", "data"}, {"restore-keys", "fallback"}});
  IUTEST_ASSERT_TRUE(restored.ok) << restored.error;
  IUTEST_EXPECT_EQ("false", restored.outputs.at("cache-hit"));
  IUTEST_EXPECT_EQ("linux-v2", restored.outputs.at("cache-matched-key"));
  IUTEST_EXPECT_EQ("prefix", cpa::util::read_file(ctx.workspace / "data"));
}

IUTEST_F(BuiltinTest, CacheRestoreKeysAreOrderedAndVersioned) {
  cpa::util::write_file(ctx.workspace / "data", "first");
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, {{"key", "first-v1"}, {"path", "data"}}).ok);
  cpa::util::write_file(ctx.workspace / "data", "second");
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, {{"key", "second-v1"}, {"path", "data"}}).ok);
  const auto restored =
      run(Builtin::CacheRestore, {{"key", "missing"}, {"path", "data"}, {"restore-keys", "first-\nsecond-"}});
  IUTEST_ASSERT_TRUE(restored.ok);
  IUTEST_EXPECT_EQ("first-v1", restored.outputs.at("cache-matched-key"));
  const auto miss = run(Builtin::CacheRestore,
                        {{"key", "first-v1"}, {"path", "other"}, {"fail-on-cache-miss", "true"}});
  IUTEST_EXPECT_FALSE(miss.ok);
}

IUTEST_F(BuiltinTest, CacheLookupOnlyDoesNotRestoreOrSave) {
  const Inputs inputs{{"key", "lookup"}, {"path", "data"}, {"lookup-only", "true"}};
  cpa::util::write_file(ctx.workspace / "data", "saved");
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, inputs).ok);
  fs::remove(ctx.workspace / "data");
  const auto hit = run(Builtin::Cache, inputs);
  IUTEST_ASSERT_TRUE(hit.ok);
  IUTEST_EXPECT_EQ("true", hit.outputs.at("cache-hit"));
  IUTEST_EXPECT_FALSE(fs::exists(ctx.workspace / "data"));
  const Inputs missing{{"key", "other"}, {"path", "data"}, {"lookup-only", "true"}};
  const auto miss = run(Builtin::Cache, missing);
  cpa::util::write_file(ctx.workspace / "data", "new");
  IUTEST_ASSERT_TRUE(run(Builtin::Cache, missing, true, miss.state).ok);
  IUTEST_EXPECT_FALSE(run(Builtin::CacheRestore,
                          {{"key", "other"}, {"path", "data"}, {"fail-on-cache-miss", "true"}}).ok);
}

IUTEST_F(BuiltinTest, FailedJobDoesNotSavePostCache) {
  const Inputs inputs{{"key", "failed"}, {"path", "data"}};
  const auto miss = run(Builtin::Cache, inputs);
  cpa::util::write_file(ctx.workspace / "data", "incomplete");
  ctx.job_succeeded = false;
  IUTEST_ASSERT_TRUE(run(Builtin::Cache, inputs, true, miss.state).ok);
  IUTEST_EXPECT_FALSE(fs::exists(ctx.cache_store));
}

IUTEST_F(BuiltinTest, CacheDirectoryExclusionsApplyRecursively) {
  const Inputs inputs{{"key", "filtered"}, {"path", "build\n!build/private\n!build/**/*.secret"}};
  cpa::util::write_file(ctx.workspace / "build/good", "public");
  cpa::util::write_file(ctx.workspace / "build/private/token", "excluded");
  cpa::util::write_file(ctx.workspace / "build/nested/key.secret", "excluded");
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, inputs).ok);
  next_job();
  IUTEST_ASSERT_TRUE(run(Builtin::CacheRestore, inputs).ok);
  IUTEST_EXPECT_EQ("public", cpa::util::read_file(ctx.workspace / "build/good"));
  IUTEST_EXPECT_FALSE(fs::exists(ctx.workspace / "build/private/token"));
  IUTEST_EXPECT_FALSE(fs::exists(ctx.workspace / "build/nested/key.secret"));
}

IUTEST_F(BuiltinTest, CacheTreePreservesSymlinkNamesWithoutWritingOutsideStore) {
  cpa::util::write_file(ctx.workspace / "tree/file", "data");
  cpa::util::write_file(ctx.workspace / "outside", "untouched");
  fs::create_symlink("../outside", ctx.workspace / "tree/link");
  const Inputs inputs{{"key", "links"}, {"path", "tree"}};
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, inputs).ok);
  IUTEST_EXPECT_FALSE(fs::exists(ctx.cache_store / "outside"));
  next_job();
  IUTEST_ASSERT_TRUE(run(Builtin::CacheRestore, inputs).ok);
  IUTEST_EXPECT_TRUE(fs::is_symlink(ctx.workspace / "tree/link"));
  IUTEST_EXPECT_EQ(fs::path("../outside"), fs::read_symlink(ctx.workspace / "tree/link"));
}

IUTEST_F(BuiltinTest, CacheRestoreReplacesNestedDestinationSymlinks) {
  cpa::util::write_file(ctx.workspace / "tree/nested/file", "cached");
  const Inputs inputs{{"key", "safe"}, {"path", "tree"}};
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, inputs).ok);
  next_job();
  cpa::util::write_file(root / "outside/file", "untouched");
  fs::create_directories(ctx.workspace / "tree");
  fs::create_directory_symlink(root / "outside", ctx.workspace / "tree/nested");
  IUTEST_ASSERT_TRUE(run(Builtin::CacheRestore, inputs).ok);
  IUTEST_EXPECT_EQ("untouched", cpa::util::read_file(root / "outside/file"));
  IUTEST_EXPECT_EQ("cached", cpa::util::read_file(ctx.workspace / "tree/nested/file"));
}

IUTEST_F(BuiltinTest, CacheRefusesOutsidePathsAndSymlinkedParents) {
  cpa::util::write_file(root / "outside/file", "host");
  fs::create_directory_symlink(root / "outside", ctx.workspace / "escape");
  const Inputs inputs{{"key", "outside"}, {"path", "escape/file"}};
  IUTEST_ASSERT_TRUE(run(Builtin::CacheSave, inputs).ok);
  IUTEST_EXPECT_FALSE(warnings.empty());
  IUTEST_EXPECT_FALSE(fs::exists(ctx.cache_store));
  IUTEST_EXPECT_EQ("host", cpa::util::read_file(root / "outside/file"));
}

IUTEST_F(BuiltinTest, ArtifactsPersistAcrossJobsAndFilterHiddenAndExcludedFiles) {
  cpa::util::write_file(ctx.workspace / "dist/bin/app", "binary");
  cpa::util::write_file(ctx.workspace / "dist/.hidden", "hidden");
  cpa::util::write_file(ctx.workspace / "dist/private/token", "excluded");
  ctx.next_artifact_id = [] { return 7; };
  const auto upload = run(Builtin::UploadArtifact,
                          {{"name", "build"}, {"path", "dist\n!dist/private"}});
  IUTEST_ASSERT_TRUE(upload.ok) << upload.error;
  IUTEST_EXPECT_EQ("7", upload.outputs.at("artifact-id"));
  IUTEST_EXPECT_EQ(64u, upload.outputs.at("artifact-digest").size());
  next_job();
  const auto download = run(Builtin::DownloadArtifact, {{"name", "build"}, {"path", "download"}});
  IUTEST_ASSERT_TRUE(download.ok) << download.error;
  IUTEST_EXPECT_EQ((ctx.workspace / "download").string(), download.outputs.at("download-path"));
  IUTEST_EXPECT_EQ("binary", cpa::util::read_file(ctx.workspace / "download/bin/app"));
  IUTEST_EXPECT_FALSE(fs::exists(ctx.workspace / "download/.hidden"));
  IUTEST_EXPECT_FALSE(fs::exists(ctx.workspace / "download/private/token"));
}

IUTEST_F(BuiltinTest, ArtifactOverwriteAndMultipleDownloadLayout) {
  cpa::util::write_file(ctx.workspace / "file", "one");
  const Inputs upload{{"name", "one"}, {"path", "file"}};
  IUTEST_ASSERT_TRUE(run(Builtin::UploadArtifact, upload).ok);
  IUTEST_EXPECT_FALSE(run(Builtin::UploadArtifact, upload).ok);
  cpa::util::write_file(ctx.workspace / "file", "two");
  IUTEST_ASSERT_TRUE(run(Builtin::UploadArtifact,
                         {{"name", "one"}, {"path", "file"}, {"overwrite", "true"}}).ok);
  IUTEST_ASSERT_TRUE(run(Builtin::UploadArtifact, {{"name", "two"}, {"path", "file"}}).ok);
  next_job();
  IUTEST_ASSERT_TRUE(run(Builtin::DownloadArtifact, {{"path", "all"}}).ok);
  IUTEST_EXPECT_EQ("two", cpa::util::read_file(ctx.workspace / "all/one/file"));
  IUTEST_EXPECT_EQ("two", cpa::util::read_file(ctx.workspace / "all/two/file"));
  IUTEST_ASSERT_TRUE(run(Builtin::DownloadArtifact,
                         {{"path", "merged"}, {"pattern", "t*"}, {"merge-multiple", "true"}}).ok);
  IUTEST_EXPECT_EQ("two", cpa::util::read_file(ctx.workspace / "merged/file"));
  IUTEST_EXPECT_FALSE(fs::exists(ctx.workspace / "merged/two"));
}

IUTEST_F(BuiltinTest, ArtifactNamesCannotAddressStoreOrParent) {
  cpa::util::write_file(ctx.workspace / "file", "data");
  for (const auto* name : {".", "..", "../escape", "dir/name"}) {
    IUTEST_EXPECT_FALSE(run(Builtin::UploadArtifact, {{"name", name}, {"path", "file"}}).ok) << name;
    IUTEST_EXPECT_FALSE(run(Builtin::DownloadArtifact, {{"name", name}}).ok) << name;
  }
  IUTEST_EXPECT_FALSE(fs::exists(ctx.artifact_store));
}

IUTEST_F(BuiltinTest, ArtifactInputAndNoFilePolicies) {
  IUTEST_EXPECT_FALSE(run(Builtin::UploadArtifact, {{"path", "missing"}, {"if-no-files-found", "error"}}).ok);
  IUTEST_EXPECT_TRUE(run(Builtin::UploadArtifact, {{"path", "missing"}, {"if-no-files-found", "ignore"}}).ok);
  cpa::util::write_file(ctx.workspace / "file", "data");
  IUTEST_EXPECT_FALSE(run(Builtin::UploadArtifact, {{"path", "file"}, {"if-no-files-found", "invalid"}}).ok);
  IUTEST_EXPECT_FALSE(run(Builtin::DownloadArtifact, {{"run-id", "123"}}).ok);
  IUTEST_EXPECT_FALSE(run(Builtin::DownloadArtifact, {{"artifact-ids", "1"}}).ok);
}

IUTEST_F(BuiltinTest, ArtifactDownloadPreservesSymlinksAndReplacesDestinationLinks) {
  cpa::util::write_file(ctx.workspace / "dist/nested/file", "artifact");
  fs::create_symlink("nested/file", ctx.workspace / "dist/link");
  IUTEST_ASSERT_TRUE(run(Builtin::UploadArtifact, {{"name", "links"}, {"path", "dist"}}).ok);
  next_job();
  cpa::util::write_file(root / "outside/file", "untouched");
  fs::create_directories(ctx.workspace / "download");
  fs::create_directory_symlink(root / "outside", ctx.workspace / "download/nested");
  IUTEST_ASSERT_TRUE(run(Builtin::DownloadArtifact, {{"name", "links"}, {"path", "download"}}).ok);
  IUTEST_EXPECT_EQ("untouched", cpa::util::read_file(root / "outside/file"));
  IUTEST_EXPECT_TRUE(fs::is_symlink(ctx.workspace / "download/link"));
  IUTEST_EXPECT_EQ(fs::path("nested/file"), fs::read_symlink(ctx.workspace / "download/link"));
}

IUTEST_F(BuiltinTest, ArtifactPathsCannotEscapeWritableRoots) {
  cpa::util::write_file(root / "outside/file", "host");
  fs::create_directory_symlink(root / "outside", ctx.workspace / "escape");
  IUTEST_EXPECT_FALSE(run(Builtin::UploadArtifact, {{"path", "escape/file"}}).ok);
  IUTEST_EXPECT_FALSE(run(Builtin::DownloadArtifact, {{"path", "escape/download"}}).ok);
  IUTEST_EXPECT_FALSE(run(Builtin::DownloadArtifact, {{"path", "../outside"}}).ok);
  IUTEST_EXPECT_EQ("host", cpa::util::read_file(root / "outside/file"));
}

IUTEST_F(BuiltinTest, CheckoutCopiesSnapshotAndCleansWorkspace) {
  cpa::workspace::Snapshot snapshot;
  snapshot.dir = root / "snapshot";
  snapshot.sha = "snapshot-sha";
  cpa::util::write_file(snapshot.dir / "file", "dirty contents");
  cpa::util::write_file(snapshot.dir / "nested/item", "nested");
  fs::create_symlink("file", snapshot.dir / "link");
  cpa::util::write_file(ctx.workspace / "stale", "remove");
  ctx.snapshot = [&snapshot]() -> const auto& { return snapshot; };
  ctx.ref = "refs/heads/main";
  const auto result = run(Builtin::Checkout, {});
  IUTEST_ASSERT_TRUE(result.ok) << result.error;
  IUTEST_EXPECT_EQ("snapshot-sha", result.outputs.at("commit"));
  IUTEST_EXPECT_EQ("refs/heads/main", result.outputs.at("ref"));
  IUTEST_EXPECT_EQ("dirty contents", cpa::util::read_file(ctx.workspace / "file"));
  IUTEST_EXPECT_TRUE(fs::is_symlink(ctx.workspace / "link"));
  IUTEST_EXPECT_FALSE(fs::exists(ctx.workspace / "stale"));
}

IUTEST_F(BuiltinTest, CheckoutSelectsLocalBranchWithoutOriginRemote) {
  const auto repo = root / "repo";
  fs::create_directories(repo);
  cpa_test::init_repo(repo, {{"file", "main"}});
  const auto sha = cpa_test::git(repo, {"rev-parse", "HEAD"});
  cpa_test::git(repo, {"branch", "feature"});
  cpa::workspace::Snapshot snapshot{repo, true, sha, false};
  ctx.snapshot = [&snapshot]() -> const auto& { return snapshot; };
  ctx.ref = "refs/heads/main";
  ctx.sha = sha;
  const auto result = run(Builtin::Checkout, {{"ref", "refs/heads/feature"}, {"path", "nested"}});
  IUTEST_ASSERT_TRUE(result.ok) << result.error;
  IUTEST_EXPECT_EQ("refs/heads/feature", result.outputs.at("ref"));
  IUTEST_EXPECT_EQ(sha, result.outputs.at("commit"));
  IUTEST_EXPECT_EQ("feature", cpa_test::git(ctx.workspace / "nested", {"branch", "--show-current"}));
}

IUTEST_F(BuiltinTest, CheckoutRejectsPathsOutsideWorkspaceBeforeCleaning) {
  cpa::util::write_file(root / "outside/file", "host");
  fs::create_directory_symlink(root / "outside", ctx.workspace / "escape");
  IUTEST_EXPECT_FALSE(run(Builtin::Checkout, {{"path", "../outside"}}).ok);
  IUTEST_EXPECT_FALSE(run(Builtin::Checkout, {{"path", "escape"}}).ok);
  IUTEST_EXPECT_FALSE(run(Builtin::Checkout, {{"path", (root / "outside").string()}}).ok);
  IUTEST_EXPECT_EQ("host", cpa::util::read_file(root / "outside/file"));
}

IUTEST(BuiltinHelpersTest, ClassificationAndInputHelpers) {
  cpa::actions::ActionRef ref;
  ref.kind = cpa::actions::ActionRef::Kind::Remote;
  ref.owner = "Actions";
  ref.repo = "CACHE";
  ref.path = "restore";
  IUTEST_EXPECT_EQ(Builtin::CacheRestore, cpa::actions::find_builtin(ref));
  IUTEST_EXPECT_TRUE(cpa::actions::builtin_has_post(Builtin::Cache));
  IUTEST_EXPECT_FALSE(cpa::actions::builtin_has_post(Builtin::CacheRestore));
  const auto lines = cpa::actions::input_lines(" a \r\n\n b\n");
  IUTEST_ASSERT_EQ(2u, lines.size());
  IUTEST_EXPECT_EQ("a", lines[0]);
  IUTEST_EXPECT_EQ("b", lines[1]);
  IUTEST_EXPECT_EQ(fs::path("/work"), cpa::actions::artifact_root({"/work/a", "/work/b"}));
}

}  // namespace
