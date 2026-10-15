#include "iutest.hpp"

#include <nlohmann/json.hpp>

#include "actions/builtin.hpp"
#include "util/fs.hpp"

using namespace cpa;
namespace fs = std::filesystem;

namespace {

class BuiltinSandboxTest : public ::iutest::Test {
 protected:
  void SetUp() override {
    root = util::make_temp_dir("builtin-sandbox-", fs::current_path());
    ctx.workspace = root / "work";
    ctx.home = root / "home";
    ctx.writable = {ctx.workspace, ctx.home, root / "tmp", root / "tools"};
    ctx.readable = ctx.writable;
    ctx.cache_store = root / "cache";
    ctx.artifact_store = root / "artifacts";
    for (const auto& path : ctx.writable) fs::create_directories(path);
    fs::create_directories(ctx.artifact_store);
    profile = root / "sandbox.sb";
    secret = root / "private" / "secret";
    util::write_file(profile, "trusted profile");
    util::write_file(secret, "host secret");
    util::write_file(ctx.workspace / "file", "workspace content");
    ctx.warning = [&](std::string_view text) { warnings.emplace_back(text); };
  }

  void TearDown() override { util::remove_tree(root); }

  actions::BuiltinResult run(actions::Builtin kind, const std::map<std::string, std::string>& inputs) {
    return actions::run_builtin(kind, false, inputs, {}, ctx);
  }

  void expect_read_denied(const fs::path& path) {
    for (const auto kind : {actions::Builtin::UploadArtifact, actions::Builtin::CacheSave}) {
      const auto result = run(kind, {{"path", path.string()}, {"key", "denied"}, {"name", "denied"}});
      IUTEST_EXPECT_FALSE(result.ok) << path.string();
      IUTEST_EXPECT_NE(std::string::npos, result.error.find("Read denied")) << result.error;
    }
    IUTEST_EXPECT_FALSE(fs::exists(ctx.cache_store));
    IUTEST_EXPECT_TRUE(fs::is_empty(ctx.artifact_store));
  }

  fs::path cache_entry() { return fs::directory_iterator(ctx.cache_store)->path(); }

  fs::path root, profile, secret;
  actions::BuiltinContext ctx;
  std::vector<std::string> warnings;
};

}  // namespace

IUTEST_F(BuiltinSandboxTest, DeniesHostFilesAndGlobBases) {
  expect_read_denied(secret);
  expect_read_denied(secret.parent_path() / "*");
  expect_read_denied(root / "**" / "secret");
  expect_read_denied(root / "absent");
}

IUTEST_F(BuiltinSandboxTest, CachePostCannotReadHostFiles) {
  const auto result = actions::run_builtin(actions::Builtin::Cache, true,
                                           {{"path", secret.string()}, {"key", "post"}}, {}, ctx);
  IUTEST_EXPECT_FALSE(result.ok);
  IUTEST_EXPECT_NE(std::string::npos, result.error.find("Read denied"));
  IUTEST_EXPECT_FALSE(fs::exists(ctx.cache_store));
}

IUTEST_F(BuiltinSandboxTest, DeniesSymlinkFilesAndGlobMatches) {
  fs::create_symlink(secret, ctx.workspace / "link");
  expect_read_denied(ctx.workspace / "link");
  expect_read_denied(ctx.workspace / "*");
}

IUTEST_F(BuiltinSandboxTest, DeniesSymlinkDirectoriesAndNestedSymlinks) {
  fs::create_directory_symlink(secret.parent_path(), ctx.workspace / "link");
  expect_read_denied(ctx.workspace / "link" / "*");
  fs::create_directories(ctx.workspace / "tree");
  fs::create_symlink(secret, ctx.workspace / "tree" / "nested");
  expect_read_denied(ctx.workspace / "tree");
}

IUTEST_F(BuiltinSandboxTest, AllowsExplicitReadOnlyRoots) {
  ctx.readable.push_back(secret);
  const auto single = run(actions::Builtin::UploadArtifact,
                          {{"path", secret.string()}, {"name", "single"}});
  IUTEST_ASSERT_TRUE(single.ok) << single.error;
  ctx.readable.push_back(secret.parent_path());
  const auto upload = run(actions::Builtin::UploadArtifact,
                          {{"path", (secret.parent_path() / "*").string()}, {"name", "allowed"}});
  IUTEST_ASSERT_TRUE(upload.ok) << upload.error;
  IUTEST_EXPECT_EQ("host secret", util::read_file(ctx.artifact_store / "allowed" / "secret"));
  const auto save = run(actions::Builtin::CacheSave, {{"path", secret.string()}, {"key", "allowed"}});
  IUTEST_ASSERT_TRUE(save.ok) << save.error;
  IUTEST_EXPECT_EQ("host secret", util::read_file(cache_entry() / "data" / "0"));
}

IUTEST_F(BuiltinSandboxTest, EmptyReadableLeavesDebugReadsUnconfined) {
  ctx.readable.clear();
  const auto upload = run(actions::Builtin::UploadArtifact, {{"path", secret.string()}});
  IUTEST_ASSERT_TRUE(upload.ok) << upload.error;
  const auto save = run(actions::Builtin::CacheSave, {{"path", secret.string()}, {"key", "debug"}});
  IUTEST_ASSERT_TRUE(save.ok) << save.error;
  IUTEST_EXPECT_EQ("host secret", util::read_file(cache_entry() / "data" / "0"));
}

IUTEST_F(BuiltinSandboxTest, DownloadRejectsProfileAndSymlinkDestinations) {
  util::write_file(ctx.artifact_store / "bundle" / "sandbox.sb", "malicious profile");
  const auto outside = run(actions::Builtin::DownloadArtifact,
                           {{"name", "bundle"}, {"path", root.string()}});
  IUTEST_EXPECT_FALSE(outside.ok);
  fs::create_directory_symlink(root, ctx.workspace / "escape");
  const auto linked = run(actions::Builtin::DownloadArtifact,
                          {{"name", "bundle"}, {"path", (ctx.workspace / "escape").string()}});
  IUTEST_EXPECT_FALSE(linked.ok);
  IUTEST_EXPECT_EQ("trusted profile", util::read_file(profile));
}

IUTEST_F(BuiltinSandboxTest, DownloadReplacesNestedDestinationSymlinks) {
  util::write_file(ctx.artifact_store / "bundle" / "nested" / "secret", "downloaded");
  fs::create_directory_symlink(secret.parent_path(), ctx.workspace / "nested");
  const auto result = run(actions::Builtin::DownloadArtifact, {{"name", "bundle"}});
  IUTEST_ASSERT_TRUE(result.ok) << result.error;
  IUTEST_EXPECT_EQ("host secret", util::read_file(secret));
  IUTEST_EXPECT_FALSE(fs::is_symlink(ctx.workspace / "nested"));
  IUTEST_EXPECT_EQ("downloaded", util::read_file(ctx.workspace / "nested" / "secret"));
}

IUTEST_F(BuiltinSandboxTest, SourceSymlinksCannotRedirectTreeCopyDestinations) {
  const auto protected_file = root / "private" / "sandbox.sb";
  util::write_file(protected_file, "protected");
  fs::create_directories(ctx.artifact_store / "bundle");
  fs::create_symlink(protected_file, ctx.artifact_store / "bundle" / "link");
  const auto result = run(actions::Builtin::DownloadArtifact,
                          {{"name", "bundle"}, {"path", (ctx.workspace / "download").string()}});
  IUTEST_ASSERT_TRUE(result.ok) << result.error;
  IUTEST_EXPECT_FALSE(fs::is_symlink(protected_file));
  IUTEST_EXPECT_EQ("protected", util::read_file(protected_file));
  IUTEST_EXPECT_TRUE(fs::is_symlink(ctx.workspace / "download" / "link"));
}

IUTEST_F(BuiltinSandboxTest, DownloadDoesNotFollowArtifactDirectorySymlinks) {
  fs::create_directory_symlink(secret.parent_path(), ctx.artifact_store / "escape");
  const auto named = run(actions::Builtin::DownloadArtifact, {{"name", "escape"}});
  IUTEST_EXPECT_FALSE(named.ok);
  const auto all = run(actions::Builtin::DownloadArtifact, {});
  IUTEST_ASSERT_TRUE(all.ok) << all.error;
  IUTEST_EXPECT_FALSE(fs::exists(ctx.workspace / "escape"));
}

IUTEST_F(BuiltinSandboxTest, CacheMetadataCannotRestoreOutsideWritableRoots) {
  const std::map<std::string, std::string> inputs{{"path", "file"}, {"key", "entry"}};
  const auto saved = run(actions::Builtin::CacheSave, inputs);
  IUTEST_ASSERT_TRUE(saved.ok) << saved.error;
  const auto entry = cache_entry();
  auto meta = nlohmann::json::parse(util::read_file(entry / "meta.json"));
  util::write_file(entry / "data" / "0", "malicious profile");
  for (const auto& target : std::vector<nlohmann::json>{
           {{"base", "absolute"}, {"rel", profile.string()}},
           {{"base", "workspace"}, {"rel", "../sandbox.sb"}},
           {{"base", "home"}, {"rel", "../sandbox.sb"}}}) {
    meta["paths"] = nlohmann::json::array({target});
    util::write_file(entry / "meta.json", meta.dump());
    const auto result = run(actions::Builtin::CacheRestore, inputs);
    IUTEST_EXPECT_TRUE(result.ok) << result.error;
    IUTEST_EXPECT_EQ("trusted profile", util::read_file(profile));
  }
  IUTEST_EXPECT_EQ(3u, warnings.size());
}

IUTEST_F(BuiltinSandboxTest, CacheRestoreRejectsSymlinkDestinationAncestors) {
  const std::map<std::string, std::string> inputs{{"path", "file"}, {"key", "entry"}};
  IUTEST_ASSERT_TRUE(run(actions::Builtin::CacheSave, inputs).ok);
  const auto entry = cache_entry();
  auto meta = nlohmann::json::parse(util::read_file(entry / "meta.json"));
  fs::create_directory_symlink(root, ctx.workspace / "escape");
  meta["paths"] = nlohmann::json::array({{{"base", "workspace"}, {"rel", "escape/sandbox.sb"}}});
  util::write_file(entry / "meta.json", meta.dump());
  const auto restored = run(actions::Builtin::CacheRestore, inputs);
  IUTEST_EXPECT_TRUE(restored.ok) << restored.error;
  IUTEST_EXPECT_EQ("trusted profile", util::read_file(profile));
  IUTEST_EXPECT_EQ(1u, warnings.size());
}

IUTEST_F(BuiltinSandboxTest, CacheRestoreReplacesNestedDestinationSymlinks) {
  util::write_file(ctx.workspace / "tree" / "nested" / "secret", "cached");
  const std::map<std::string, std::string> inputs{{"path", "tree"}, {"key", "entry"}};
  IUTEST_ASSERT_TRUE(run(actions::Builtin::CacheSave, inputs).ok);
  util::remove_tree(ctx.workspace / "tree" / "nested");
  fs::create_directory_symlink(secret.parent_path(), ctx.workspace / "tree" / "nested");
  const auto restored = run(actions::Builtin::CacheRestore, inputs);
  IUTEST_ASSERT_TRUE(restored.ok) << restored.error;
  IUTEST_EXPECT_EQ("host secret", util::read_file(secret));
  IUTEST_EXPECT_EQ("cached", util::read_file(ctx.workspace / "tree" / "nested" / "secret"));
}

IUTEST_F(BuiltinSandboxTest, RejectsArtifactTraversalNames) {
  for (const auto* name : {".", ".."}) {
    IUTEST_EXPECT_FALSE(run(actions::Builtin::UploadArtifact, {{"path", "file"}, {"name", name}}).ok);
    IUTEST_EXPECT_FALSE(run(actions::Builtin::DownloadArtifact, {{"name", name}}).ok);
  }
}
