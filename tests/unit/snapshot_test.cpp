#include "iutest.hpp"

#include "actions/builtin.hpp"
#include "test_support.hpp"
#include "workspace/snapshot.hpp"

namespace {

namespace fs = std::filesystem;
using cpa::util::read_file;
using cpa::util::write_file;
using namespace cpa::workspace;

GitInfo plain_info(const fs::path& root) {
  GitInfo info;
  info.root = root;
  return info;
}

cpa::actions::BuiltinContext checkout_context(const Snapshot& snapshot, const fs::path& workspace) {
  cpa::actions::BuiltinContext ctx;
  ctx.workspace = workspace;
  ctx.repository = "owner/repo";
  ctx.ref = "refs/heads/main";
  ctx.sha = snapshot.sha;
  ctx.snapshot = [&snapshot]() -> const Snapshot& { return snapshot; };
  return ctx;
}

cpa::actions::BuiltinResult checkout(const cpa::actions::BuiltinContext& ctx,
                                    const std::map<std::string, std::string>& inputs = {}) {
  return cpa::actions::run_builtin(cpa::actions::Builtin::Checkout, false, inputs, {}, ctx);
}

}  // namespace

IUTEST(SnapshotTest, CapturesWorkingTreeWithoutChangingSource) {
  cpa_test::TempDir dir;
  const auto source = dir / "source";
  fs::create_directories(source);
  cpa_test::init_repo(source, {{"modified", "old"}, {"deleted", "old"}, {"removed-dir/file", "old"}, {"staged", "old"},
                               {"ignored-tracked.log", "tracked"}, {".gitignore", "*.log\n"}});
  cpa_test::git(source, {"add", "--force", "ignored-tracked.log"});
  cpa_test::git(source, {"commit", "-q", "--amend", "--no-edit"});
  cpa_test::git(source, {"remote", "add", "origin", "git@github.com:owner/repo.git"});
  write_file(source / "modified", "working");
  write_file(source / "staged", "index");
  cpa_test::git(source, {"add", "staged"});
  write_file(source / "staged", "working after staging");
  fs::remove(source / "deleted");
  fs::remove_all(source / "removed-dir");
  write_file(source / "new dir/untracked", "new");
  write_file(source / "ignored.log", "ignored");
  cpa::util::make_executable(source / "modified");
  const auto head = cpa_test::git(source, {"rev-parse", "HEAD"});
  const auto status = cpa_test::git(source, {"status", "--porcelain=v1", "--untracked-files=all"});
  const auto index = read_file(source / ".git/index");
  const auto config = read_file(source / ".git/config");

  const auto snap = create_snapshot(read_git_info(source), dir / "snapshot", "refs/heads/main");

  IUTEST_EXPECT_TRUE(snap.is_git);
  IUTEST_EXPECT_TRUE(snap.dirty);
  IUTEST_EXPECT_NE(head, snap.sha);
  IUTEST_EXPECT_EQ("working", read_file(snap.dir / "modified"));
  IUTEST_EXPECT_EQ("working after staging", read_file(snap.dir / "staged"));
  IUTEST_EXPECT_EQ("new", read_file(snap.dir / "new dir/untracked"));
  IUTEST_EXPECT_FALSE(fs::exists(snap.dir / "deleted"));
  IUTEST_EXPECT_FALSE(fs::exists(snap.dir / "removed-dir"));
  IUTEST_EXPECT_FALSE(fs::exists(snap.dir / "ignored.log"));
  IUTEST_EXPECT_EQ("tracked", read_file(snap.dir / "ignored-tracked.log"));
  IUTEST_EXPECT_NE(fs::perms::none, fs::status(snap.dir / "modified").permissions() & fs::perms::owner_exec);
  IUTEST_EXPECT_EQ("", cpa_test::git(snap.dir, {"status", "--porcelain"}));
  IUTEST_EXPECT_EQ(snap.sha, cpa_test::git(snap.dir, {"rev-parse", "HEAD"}));
  IUTEST_EXPECT_EQ("refs/heads/main", cpa_test::git(snap.dir, {"symbolic-ref", "HEAD"}));
  IUTEST_EXPECT_EQ("https://github.com/owner/repo", cpa_test::git(snap.dir, {"remote", "get-url", "origin"}));
  IUTEST_EXPECT_EQ("working", cpa_test::git(snap.dir, {"show", "HEAD:modified"}));
  IUTEST_EXPECT_EQ(head, cpa_test::git(source, {"rev-parse", "HEAD"}));
  IUTEST_EXPECT_EQ(status, cpa_test::git(source, {"status", "--porcelain=v1", "--untracked-files=all"}));
  IUTEST_EXPECT_EQ(index, read_file(source / ".git/index"));
  IUTEST_EXPECT_EQ(config, read_file(source / ".git/config"));
}

IUTEST(SnapshotTest, CleanSnapshotAndIndependentWorkspaces) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  cpa_test::init_repo(dir / "source", {{"file", "initial"}});
  const auto info = read_git_info(dir / "source");
  const auto snap = create_snapshot(info, dir / "snapshot", "refs/heads/main");
  IUTEST_EXPECT_FALSE(snap.dirty);
  IUTEST_EXPECT_EQ(info.head_sha, snap.sha);
  copy_snapshot(snap, dir / "first");
  copy_snapshot(snap, dir / "second");
  write_file(dir / "first/file", "first job");
  cpa_test::git(dir / "first", {"add", "file"});
  cpa_test::git(dir / "first", {"commit", "-q", "-m", "job"});
  IUTEST_EXPECT_EQ("initial", read_file(dir / "source/file"));
  IUTEST_EXPECT_EQ("initial", read_file(dir / "snapshot/file"));
  IUTEST_EXPECT_EQ("initial", read_file(dir / "second/file"));
  IUTEST_EXPECT_EQ(snap.sha, cpa_test::git(dir / "second", {"rev-parse", "HEAD"}));
  IUTEST_EXPECT_EQ("", cpa_test::git(dir / "second", {"status", "--porcelain"}));
}

IUTEST(SnapshotTest, RelativeDestinationAndDetachedTag) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  cpa_test::init_repo(dir / "source", {{"file", "initial"}});
  const auto dest = (dir / "snapshot").lexically_relative(fs::current_path());
  const auto snap = create_snapshot(read_git_info(dir / "source"), dest, "refs/tags/event");
  IUTEST_EXPECT_EQ("initial", read_file(snap.dir / "file"));
  IUTEST_EXPECT_EQ(snap.sha, cpa_test::git(snap.dir, {"rev-parse", "refs/tags/event"}));
  IUTEST_EXPECT_FALSE(git_output(snap.dir, {"symbolic-ref", "--quiet", "HEAD"}).has_value());
}

IUTEST(SnapshotTest, LinkedWorktreeUsesItsOwnHeadAndChanges) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  cpa_test::init_repo(dir / "source", {{"file", "main"}});
  cpa_test::git(dir / "source", {"worktree", "add", "-q", "-b", "linked", (dir / "linked").string()});
  write_file(dir / "linked/file", "linked commit");
  cpa_test::git(dir / "linked", {"add", "file"});
  cpa_test::git(dir / "linked", {"commit", "-q", "-m", "linked"});
  const auto linked_head = cpa_test::git(dir / "linked", {"rev-parse", "HEAD"});
  write_file(dir / "linked/file", "linked working tree");
  const auto snap = create_snapshot(read_git_info(dir / "linked"), dir / "snapshot", "refs/heads/linked");
  IUTEST_EXPECT_TRUE(snap.is_git);
  IUTEST_EXPECT_TRUE(snap.dirty);
  IUTEST_EXPECT_EQ(linked_head, cpa_test::git(snap.dir, {"rev-parse", "HEAD^"}));
  IUTEST_EXPECT_EQ("linked working tree", read_file(snap.dir / "file"));
  IUTEST_EXPECT_TRUE(fs::is_directory(snap.dir / ".git"));
  IUTEST_EXPECT_EQ("", cpa_test::git(snap.dir, {"status", "--porcelain"}));
  IUTEST_EXPECT_EQ(linked_head, cpa_test::git(dir / "linked", {"rev-parse", "HEAD"}));
  IUTEST_EXPECT_EQ("main", read_file(dir / "source/file"));
}

IUTEST(SnapshotTest, PlainAndUnbornRepositories) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "plain");
  write_file(dir / "plain/file", "plain");
  fs::create_symlink("missing", dir / "plain/dangling");
  const auto plain = create_snapshot(plain_info(dir / "plain"), dir / "plain-snapshot", "");
  IUTEST_EXPECT_FALSE(plain.is_git);
  IUTEST_EXPECT_EQ(std::string(40, '0'), plain.sha);
  IUTEST_EXPECT_EQ("plain", read_file(plain.dir / "file"));
  IUTEST_EXPECT_TRUE(fs::is_symlink(plain.dir / "dangling"));
  IUTEST_EXPECT_EQ(fs::path("missing"), fs::read_symlink(plain.dir / "dangling"));
  fs::create_directories(dir / "unborn");
  cpa_test::git(dir / "unborn", {"init", "-q", "-b", "main"});
  write_file(dir / "unborn/.gitignore", "*.log\n");
  write_file(dir / "unborn/file", "unborn");
  write_file(dir / "unborn/ignored.log", "ignored");
  const auto unborn = create_snapshot(read_git_info(dir / "unborn"), dir / "unborn-snapshot", "refs/heads/main");
  IUTEST_EXPECT_FALSE(unborn.is_git);
  IUTEST_EXPECT_EQ(std::string(40, '0'), unborn.sha);
  IUTEST_EXPECT_EQ("unborn", read_file(unborn.dir / "file"));
  IUTEST_EXPECT_FALSE(fs::exists(unborn.dir / "ignored.log"));
  IUTEST_EXPECT_FALSE(fs::exists(unborn.dir / ".git"));
}

IUTEST(SnapshotTest, PreservesSymlinksAndReplacesDestinationSymlinks) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  cpa_test::init_repo(dir / "source", {{"file", "file"}, {"nested/file", "nested"}});
  write_file(dir / "outside/original", "original");
  fs::create_symlink(dir / "outside/original", dir / "source/absolute-link");
  fs::create_directory_symlink("../outside", dir / "source/directory-link");
  fs::create_symlink("file", dir / "source/relative-link");
  fs::create_symlink("missing", dir / "source/dangling");
  const auto snap = create_snapshot(read_git_info(dir / "source"), dir / "snapshot", "refs/heads/main");
  fs::create_directories(dir / "destination");
  fs::create_symlink(dir / "outside/original", dir / "destination/file");
  fs::create_directory_symlink(dir / "outside", dir / "destination/nested");
  copy_snapshot(snap, dir / "destination");
  IUTEST_EXPECT_EQ("original", read_file(dir / "outside/original"));
  IUTEST_EXPECT_FALSE(fs::exists(dir / "outside/file"));
  IUTEST_EXPECT_FALSE(fs::is_symlink(dir / "destination/file"));
  IUTEST_EXPECT_FALSE(fs::is_symlink(dir / "destination/nested"));
  IUTEST_EXPECT_EQ("nested", read_file(dir / "destination/nested/file"));
  for (const auto* name : {"absolute-link", "directory-link", "relative-link", "dangling"}) {
    IUTEST_EXPECT_TRUE(fs::is_symlink(snap.dir / name)) << name;
    IUTEST_EXPECT_EQ(fs::read_symlink(dir / "source" / name), fs::read_symlink(dir / "destination" / name));
  }
  IUTEST_EXPECT_EQ("", cpa_test::git(dir / "destination", {"status", "--porcelain"}));
  fs::create_directory_symlink(dir / "outside", dir / "top-link");
  copy_snapshot(snap, dir / "top-link");
  IUTEST_EXPECT_FALSE(fs::is_symlink(dir / "top-link"));
  IUTEST_EXPECT_EQ("file", read_file(dir / "top-link/file"));
  IUTEST_EXPECT_EQ("original", read_file(dir / "outside/original"));
  fs::create_directory_symlink(dir / "outside", dir / "alias");
  IUTEST_EXPECT_THROW(copy_snapshot(snap, dir / "alias/subdirectory"), SnapshotError);
  IUTEST_EXPECT_FALSE(fs::exists(dir / "outside/subdirectory"));
}

IUTEST(SnapshotTest, DirectoryReplacedBySymlinkDoesNotWriteThroughIt) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  cpa_test::init_repo(dir / "source", {{"nested/file", "tracked"}});
  fs::remove_all(dir / "source/nested");
  write_file(dir / "outside/file", "outside");
  fs::create_directory_symlink(dir / "outside", dir / "source/nested");
  const auto snap = create_snapshot(read_git_info(dir / "source"), dir / "snapshot", "refs/heads/main");
  IUTEST_EXPECT_EQ("outside", read_file(dir / "outside/file"));
  IUTEST_EXPECT_TRUE(fs::is_symlink(snap.dir / "nested"));
  IUTEST_EXPECT_EQ(fs::read_symlink(dir / "source/nested"), fs::read_symlink(snap.dir / "nested"));
  IUTEST_EXPECT_EQ("", cpa_test::git(snap.dir, {"status", "--porcelain"}));
}

IUTEST(SnapshotTest, RejectsExistingAndOverlappingDestinations) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  cpa_test::init_repo(dir / "source", {{"file", "original"}});
  const auto info = read_git_info(dir / "source");
  IUTEST_EXPECT_THROW(create_snapshot(info, dir / "source/snapshot", ""), SnapshotError);
  fs::create_symlink("missing", dir / "existing");
  IUTEST_EXPECT_THROW(create_snapshot(info, dir / "existing", ""), SnapshotError);
  const auto snap = create_snapshot(info, dir / "snapshot", "");
  IUTEST_EXPECT_THROW(copy_snapshot(snap, snap.dir), SnapshotError);
  IUTEST_EXPECT_THROW(copy_snapshot(snap, snap.dir / "nested"), SnapshotError);
  IUTEST_EXPECT_THROW(copy_snapshot(snap, dir.path()), SnapshotError);
  IUTEST_EXPECT_EQ("original", read_file(dir / "source/file"));
}

IUTEST(CheckoutTest, DefaultCheckoutUsesSnapshotAndCleansWorkspace) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  cpa_test::init_repo(dir / "source", {{"file", "committed"}});
  const auto head = cpa_test::git(dir / "source", {"rev-parse", "HEAD"});
  write_file(dir / "source/file", "working tree");
  write_file(dir / "source/new", "untracked");
  const auto snap = create_snapshot(read_git_info(dir / "source"), dir / "snapshot", "refs/heads/main");
  auto ctx = checkout_context(snap, dir / "workspace");
  ctx.sha = head;
  write_file(ctx.workspace / "stale", "stale");
  for (const auto& inputs : {std::map<std::string, std::string>{},
                             std::map<std::string, std::string>{{"ref", ctx.ref}},
                             std::map<std::string, std::string>{{"ref", ctx.sha}},
                             std::map<std::string, std::string>{{"ref", snap.sha}}}) {
    const auto result = checkout(ctx, inputs);
    IUTEST_ASSERT_TRUE(result.ok) << result.error;
    IUTEST_EXPECT_EQ(snap.sha, result.outputs.at("commit"));
    IUTEST_EXPECT_EQ(ctx.ref, result.outputs.at("ref"));
    IUTEST_EXPECT_EQ("working tree", read_file(ctx.workspace / "file"));
    IUTEST_EXPECT_EQ("untracked", read_file(ctx.workspace / "new"));
    IUTEST_EXPECT_FALSE(fs::exists(ctx.workspace / "stale"));
    IUTEST_EXPECT_EQ("", cpa_test::git(ctx.workspace, {"status", "--porcelain"}));
  }
  const auto nested = checkout(ctx, {{"path", "nested/repo"}, {"repository", "OWNER/REPO"}});
  IUTEST_ASSERT_TRUE(nested.ok) << nested.error;
  IUTEST_EXPECT_EQ("working tree", read_file(ctx.workspace / "nested/repo/file"));
  IUTEST_EXPECT_EQ(head, cpa_test::git(dir / "source", {"rev-parse", "HEAD"}));
}

IUTEST(CheckoutTest, ExplicitLocalBranchAndTagDiscardUncommittedChanges) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  cpa_test::init_repo(dir / "source", {{"file", "main"}});
  const auto main_sha = cpa_test::git(dir / "source", {"rev-parse", "HEAD"});
  cpa_test::git(dir / "source", {"tag", "v1"});
  cpa_test::git(dir / "source", {"checkout", "-q", "-b", "other"});
  write_file(dir / "source/file", "other");
  cpa_test::git(dir / "source", {"add", "file"});
  cpa_test::git(dir / "source", {"commit", "-q", "-m", "other"});
  const auto other_sha = cpa_test::git(dir / "source", {"rev-parse", "HEAD"});
  cpa_test::git(dir / "source", {"checkout", "-q", "main"});
  write_file(dir / "source/file", "dirty");
  const auto snap = create_snapshot(read_git_info(dir / "source"), dir / "snapshot", "refs/heads/main");
  auto ctx = checkout_context(snap, dir / "workspace");
  int warnings = 0;
  ctx.warning = [&](std::string_view) { ++warnings; };
  for (const auto* ref : {"other", "refs/heads/other"}) {
    const auto result = checkout(ctx, {{"ref", ref}});
    IUTEST_ASSERT_TRUE(result.ok) << result.error;
    IUTEST_EXPECT_EQ(other_sha, result.outputs.at("commit"));
    IUTEST_EXPECT_EQ("other", read_file(ctx.workspace / "file"));
    IUTEST_EXPECT_EQ("refs/heads/other", cpa_test::git(ctx.workspace, {"symbolic-ref", "HEAD"}));
  }
  for (const auto* ref : {"v1", "refs/tags/v1"}) {
    const auto result = checkout(ctx, {{"ref", ref}});
    IUTEST_ASSERT_TRUE(result.ok) << result.error;
    IUTEST_EXPECT_EQ(main_sha, result.outputs.at("commit"));
    IUTEST_EXPECT_EQ("main", read_file(ctx.workspace / "file"));
    IUTEST_EXPECT_FALSE(git_output(ctx.workspace, {"symbolic-ref", "--quiet", "HEAD"}).has_value());
  }
  IUTEST_EXPECT_EQ(4, warnings);
  IUTEST_EXPECT_FALSE(checkout(ctx, {{"ref", "missing"}}).ok);
  IUTEST_EXPECT_EQ("dirty", read_file(snap.dir / "file"));
  IUTEST_EXPECT_EQ(main_sha, cpa_test::git(dir / "source", {"rev-parse", "HEAD"}));
}

IUTEST(CheckoutTest, RejectsEscapingAndSnapshotOverlappingPathsBeforeCleaning) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  write_file(dir / "source/file", "original");
  const auto snap = create_snapshot(plain_info(dir / "source"), dir / "workspace/snapshot", "");
  auto ctx = checkout_context(snap, dir / "workspace");
  write_file(dir / "outside/keep", "outside");
  fs::create_directory_symlink(dir / "outside", ctx.workspace / "escape");
  write_file(ctx.workspace / "keep", "workspace");
  for (const auto& path : {std::string("../outside"), (dir / "outside").string(), std::string("escape"),
                           std::string("escape/nested"), std::string("snapshot"), std::string()}) {
    const auto result = checkout(ctx, {{"path", path}});
    IUTEST_EXPECT_FALSE(result.ok) << path;
    IUTEST_EXPECT_EQ("outside", read_file(dir / "outside/keep"));
    IUTEST_EXPECT_EQ("workspace", read_file(ctx.workspace / "keep"));
    IUTEST_EXPECT_EQ("original", read_file(snap.dir / "file"));
  }
}

IUTEST(CheckoutTest, PlainSnapshotAndInvalidRepository) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  write_file(dir / "source/file", "plain");
  const auto snap = create_snapshot(plain_info(dir / "source"), dir / "snapshot", "");
  const auto ctx = checkout_context(snap, dir / "workspace");
  const auto result = checkout(ctx);
  IUTEST_ASSERT_TRUE(result.ok) << result.error;
  IUTEST_EXPECT_EQ("plain", read_file(ctx.workspace / "file"));
  IUTEST_EXPECT_FALSE(checkout(ctx, {{"ref", "other"}}).ok);
  write_file(ctx.workspace / "keep", "keep");
  IUTEST_EXPECT_FALSE(checkout(ctx, {{"repository", "invalid"}}).ok);
  IUTEST_EXPECT_EQ("keep", read_file(ctx.workspace / "keep"));
}

IUTEST(CheckoutTest, SnapshotFailureAndSymlinkedAncestorsDoNotCleanTargets) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  write_file(dir / "source/file", "plain");
  const auto snap = create_snapshot(plain_info(dir / "source"), dir / "snapshot", "");
  auto ctx = checkout_context(snap, dir / "workspace");
  write_file(ctx.workspace / "keep", "keep");
  ctx.snapshot = []() -> const Snapshot& { throw SnapshotError("unavailable"); };
  IUTEST_EXPECT_FALSE(checkout(ctx).ok);
  IUTEST_EXPECT_EQ("keep", read_file(ctx.workspace / "keep"));
  ctx = checkout_context(snap, ctx.workspace);
  write_file(ctx.workspace / "real/child/keep", "keep child");
  fs::create_directory_symlink("real", ctx.workspace / "alias");
  IUTEST_EXPECT_FALSE(checkout(ctx, {{"path", "alias/child"}}).ok);
  IUTEST_EXPECT_EQ("keep child", read_file(ctx.workspace / "real/child/keep"));
}
