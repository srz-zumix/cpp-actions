#include "iutest.hpp"

#include <cstdlib>
#include <future>

#include "actions/builtin.hpp"
#include "actions/fetch.hpp"
#include "test_support.hpp"
#include "util/strings.hpp"
#include "util/sha256.hpp"
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

IUTEST(SnapshotTest, LfsFiltersPreserveContentAndGitPointers) {
  const auto env = cpa::util::current_environment();
  const auto path = env.find("PATH");
  if (path == env.end() || !cpa::util::find_executable("git-lfs", path->second)) {
    IUTEST_SKIP() << "No Git LFS executable";
  }
  cpa_test::TempDir dir;
  const auto source = dir / "source";
  fs::create_directories(source);
  cpa_test::init_repo(source, {{".gitattributes", "file filter=lfs diff=lfs merge=lfs -text\n"}});
  write_file(source / "file", "LFS file contents\n");
  IUTEST_ASSERT_TRUE(git_ok(source, {"add", "--all"}));
  cpa_test::git(source, {"commit", "-q", "-m", "LFS file"});
  const auto snap = create_snapshot(read_git_info(source), dir / "snapshot", "refs/heads/main");
  IUTEST_EXPECT_FALSE(snap.dirty);
  IUTEST_EXPECT_EQ("LFS file contents\n", read_file(snap.dir / "file"));
  const auto pointer = cpa_test::git(snap.dir, {"show", "HEAD:file"});
  IUTEST_EXPECT_EQ(0u, pointer.find("version https://git-lfs.github.com/spec/v1\n"));
  copy_snapshot(snap, dir / "workspace");
  write_file(dir / "workspace/file", "modified\n");
  IUTEST_ASSERT_TRUE(git_ok(dir / "workspace", {"checkout", "--force", "HEAD", "--", "file"}));
  IUTEST_EXPECT_EQ("LFS file contents\n", read_file(dir / "workspace/file"));
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

IUTEST(CheckoutTest, RemoteCheckoutPassesTokenToSmudgeFilters) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  cpa_test::init_repo(dir / "source", {{".gitattributes", "file filter=auth\n"}, {"file", "filtered contents\n"}});
  cpa::actions::BuiltinContext ctx;
  ctx.workspace = dir / "workspace";
  ctx.repository = "owner/workflow";
  ctx.token = "default-test-token";
  ctx.info = [&](std::string_view) {
    // Keep the remote checkout offline and require auth in its smudge child.
    cpa_test::git(ctx.workspace, {"remote", "set-url", "origin", (dir / "source").string()});
    cpa_test::git(ctx.workspace, {"config", "--local", "filter.auth.required", "true"});
    cpa_test::git(ctx.workspace, {"config", "--local", "filter.auth.smudge",
                                 "sh " + cpa::util::shell_quote((dir / "smudge.sh").string())});
  };
  for (const auto& token : {std::string(), std::string("explicit-test-token")}) {
    std::map<std::string, std::string> inputs{{"repository", "owner/other"}, {"ref", "main"}, {"lfs", "true"}};
    if (!token.empty()) inputs["token"] = token;
    const auto header = cpa::actions::git_token_env(token.empty() ? ctx.token : token).at("GIT_CONFIG_VALUE_0");
    write_file(dir / "smudge.sh",
               "#!/bin/sh\nset -eu\n"
               "header=$(git config --get http.https://github.com/.extraheader)\n"
               "[ \"$header\" = " + cpa::util::shell_quote(header) + " ]\ncat\n");
    const auto result = checkout(ctx, inputs);
    IUTEST_ASSERT_TRUE(result.ok) << result.error;
    IUTEST_EXPECT_EQ("filtered contents\n", read_file(ctx.workspace / "file"));
    IUTEST_EXPECT_EQ(cpa_test::git(dir / "source", {"rev-parse", "HEAD"}), result.outputs.at("commit"));
    IUTEST_EXPECT_EQ(std::string::npos, read_file(ctx.workspace / ".git/config").find(header));
    const auto env = cpa_test::git_env();
    const auto persisted = cpa::util::capture(
        {"git", "config", "--local", "--get", "http.https://github.com/.extraheader"}, ctx.workspace, &env);
    IUTEST_EXPECT_EQ(1, persisted.exit_code) << persisted.err;
    IUTEST_EXPECT_TRUE(persisted.out.empty());
  }
}

IUTEST(CheckoutTest, LfsCachePersistsAcrossJobsAndIsolatesRepositoriesAndObjects) {
  if (cpa::util::capture({"git", "lfs", "version"}).exit_code != 0) IUTEST_SKIP() << "Git LFS is unavailable";
  cpa_test::TempDir dir;
  const auto source = dir / "source";
  fs::create_directories(source);
  const std::string contents = "large asset contents\n";
  const auto oid = cpa::util::sha256_hex(contents);
  const auto object = fs::path("objects") / oid.substr(0, 2) / oid.substr(2, 2) / oid;
  const auto pointer = "version https://git-lfs.github.com/spec/v1\noid sha256:" + oid +
                       "\nsize " + std::to_string(contents.size()) + "\n";
  const std::string filename = "asset\nwith-newline.bin";
  cpa_test::init_repo(source, {{".gitattributes", "*.bin filter=lfs diff=lfs merge=lfs -text\n"},
                              {filename, pointer}});
  write_file(source / ".git/lfs" / object, contents);
  const auto adapter = dir / "transfer.sh";
  write_file(adapter,
             "#!/bin/sh\nset -eu\n"
             "[ \"$(git config --get http.https://github.com/.extraheader)\" = " +
                 cpa::util::shell_quote(cpa::actions::git_token_env("test-lfs-token").at("GIT_CONFIG_VALUE_0")) + " ]\n"
             "source=" + cpa::util::shell_quote((source / ".git/lfs/objects").string()) + "\n"
             "while IFS= read -r line; do\n"
             "  case \"$line\" in\n"
             "    *'\"event\":\"init\"'*) printf '{}\\n' ;;\n"
             "    *'\"event\":\"terminate\"'*) exit 0 ;;\n"
             "    *'\"event\":\"download\"'*)\n"
             "      oid=$(printf '%s' \"$line\" | sed -n 's/.*\"oid\":\"\\([0-9a-f]*\\)\".*/\\1/p')\n"
             "      a=$(printf '%s' \"$oid\" | cut -c1-2)\n"
             "      b=$(printf '%s' \"$oid\" | cut -c3-4)\n"
             "      if [ -f \"$source/$a/$b/$oid\" ]; then\n"
             "        tmp=$(mktemp)\n"
             "        cp \"$source/$a/$b/$oid\" \"$tmp\"\n"
             "        printf '{\"event\":\"complete\",\"oid\":\"%s\",\"path\":\"%s\"}\\n' \"$oid\" \"$tmp\"\n"
             "      else\n"
             "        printf '{\"event\":\"complete\",\"oid\":\"%s\",\"error\":{\"code\":404,\"message\":\"offline\"}}\\n' \"$oid\"\n"
             "      fi ;;\n"
             "  esac\n"
             "done\n");
  cpa::util::make_executable(adapter);
  const auto revision = cpa_test::git(source, {"rev-parse", "HEAD"});
  const std::map<std::string, std::string> inputs{
      {"repository", "OWNER/Assets"}, {"ref", revision}, {"lfs", "true"}};
  const auto context = [&](const std::string& job) {
    cpa::actions::BuiltinContext ctx;
    ctx.workspace = dir / job;
    ctx.repository = "owner/workflow";
    ctx.token = "test-lfs-token";
    ctx.lfs_store = dir / "cache/lfs/github.com";
    const auto workspace = ctx.workspace;
    ctx.info = [workspace, source, adapter](std::string_view message) {
      if (message.starts_with("Fetching ")) {
        cpa_test::git(workspace, {"remote", "set-url", "origin", "file://" + source.string()});
        cpa_test::git(workspace, {"config", "--local", "lfs.standalonetransferagent", "test"});
        cpa_test::git(workspace, {"config", "--local", "lfs.customtransfer.test.path", adapter.string()});
      }
    };
    return ctx;
  };
  const auto cached = dir / "cache/lfs/github.com/owner/assets" / object;
  {
    const auto first = context("first");
    const auto parallel = context("parallel");
    auto other = std::async(std::launch::async, [&] { return checkout(parallel, inputs); });
    const auto result = checkout(first, inputs);
    const auto concurrent_result = other.get();
    IUTEST_ASSERT_TRUE(result.ok) << result.error;
    IUTEST_ASSERT_TRUE(concurrent_result.ok) << concurrent_result.error;
    IUTEST_EXPECT_EQ(contents, read_file(first.workspace / filename));
    IUTEST_EXPECT_EQ(contents, read_file(parallel.workspace / filename));
    IUTEST_EXPECT_EQ(contents, read_file(cached));
    IUTEST_EXPECT_EQ(fs::perms::owner_all, fs::status(cached.parent_path().parent_path().parent_path().parent_path()).permissions());
    IUTEST_EXPECT_EQ(contents, read_file(first.workspace / ".git/lfs" / object));
    const auto env = cpa_test::git_env();
    const auto persisted = cpa::util::capture({"git", "config", "--local", "--get", "lfs.storage"},
                                             first.workspace, &env);
    IUTEST_EXPECT_EQ(1, persisted.exit_code);
    IUTEST_EXPECT_TRUE(persisted.out.empty());
    IUTEST_EXPECT_EQ(std::string::npos, read_file(first.workspace / ".git/config").find("AUTHORIZATION"));
    // Modifying a job-local object must not poison the persistent store.
    write_file(first.workspace / ".git/lfs" / object, "modified by job");
    IUTEST_EXPECT_EQ(contents, read_file(cached));
  }
  fs::remove(source / ".git/lfs" / object);
  const auto second = context("second");
  const auto hit = checkout(second, inputs);
  IUTEST_ASSERT_TRUE(hit.ok) << hit.error;
  IUTEST_EXPECT_EQ(contents, read_file(second.workspace / filename));
  IUTEST_EXPECT_EQ(contents, read_file(second.workspace / ".git/lfs" / object));
  // Normal job Git uses its independent object store, not a persisted cache path.
  fs::remove(second.workspace / filename);
  cpa_test::git(second.workspace, {"-c", "filter.lfs.process=git-lfs filter-process",
                                  "-c", "filter.lfs.required=true", "checkout", "--", filename});
  IUTEST_EXPECT_EQ(contents, read_file(second.workspace / filename));
  IUTEST_EXPECT_EQ("true", cpa_test::git(second.workspace, {"rev-parse", "--is-shallow-repository"}));
  auto other_repository = inputs;
  other_repository["repository"] = "owner/different-assets";
  const auto separated = checkout(context("separated"), other_repository);
  IUTEST_EXPECT_FALSE(separated.ok);
  IUTEST_EXPECT_NE(std::string::npos, separated.error.find("git"));

  // A different Git revision with the same LFS object still hits.
  cpa_test::git(source, {"commit", "--allow-empty", "-qm", "same asset"});
  auto next = inputs;
  next["ref"] = cpa_test::git(source, {"rev-parse", "HEAD"});
  const auto same_object = checkout(context("same-object"), next);
  IUTEST_ASSERT_TRUE(same_object.ok) << same_object.error;
  const auto new_oid = cpa::util::sha256_hex("not available from LFS");
  write_file(source / filename, "version https://git-lfs.github.com/spec/v1\noid sha256:" + new_oid + "\nsize 22\n");
  cpa_test::git(source, {"add", filename});
  cpa_test::git(source, {"commit", "-qm", "new missing asset"});
  next["ref"] = cpa_test::git(source, {"rev-parse", "HEAD"});
  const auto miss = checkout(context("miss"), next);
  IUTEST_EXPECT_FALSE(miss.ok);
  IUTEST_EXPECT_NE(std::string::npos, miss.error.find("git"));
}

IUTEST(CheckoutTest, LfsCacheIsNotUsedWithoutExplicitLfsInput) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / "source");
  cpa_test::init_repo(dir / "source", {{"file", "plain"}});
  cpa::actions::BuiltinContext ctx;
  ctx.workspace = dir / "job";
  ctx.repository = "owner/workflow";
  ctx.lfs_store = dir / "cache";
  ctx.info = [&](std::string_view message) {
    if (message.starts_with("Fetching ")) {
      cpa_test::git(ctx.workspace, {"remote", "set-url", "origin", (dir / "source").string()});
    }
  };
  const auto result = checkout(ctx, {{"repository", "owner/plain"}});
  IUTEST_ASSERT_TRUE(result.ok) << result.error;
  IUTEST_EXPECT_FALSE(fs::exists(ctx.lfs_store));
  IUTEST_EXPECT_EQ("plain", read_file(ctx.workspace / "file"));
}

IUTEST(CheckoutTest, SubmodulesSupportDirectRecursiveDisabledAndFetchFailures) {
  cpa_test::TempDir dir;
  for (const auto* name : {"leaf", "child", "source"}) fs::create_directories(dir / name);
  cpa_test::init_repo(dir / "leaf", {{"file", "nested contents\n"}});
  const auto leaf_sha = cpa_test::git(dir / "leaf", {"rev-parse", "HEAD"});
  const auto modules = [](const std::string& name, const fs::path& url) {
    return "[submodule \"" + name + "\"]\n\tpath = " + name + "\n\turl = file://" + url.string() + "\n";
  };
  cpa_test::init_repo(dir / "child",
                     {{"file", "direct contents\n"}, {".gitmodules", modules("nested", dir / "leaf")}});
  cpa_test::git(dir / "child", {"update-index", "--add", "--cacheinfo", "160000," + leaf_sha + ",nested"});
  cpa_test::git(dir / "child", {"commit", "-qm", "nested submodule"});
  const auto child_sha = cpa_test::git(dir / "child", {"rev-parse", "HEAD"});
  cpa_test::init_repo(dir / "source", {{".gitmodules", modules("child", dir / "child")}});
  cpa_test::git(dir / "source", {"update-index", "--add", "--cacheinfo", "160000," + child_sha + ",child"});
  cpa_test::git(dir / "source", {"commit", "-qm", "direct submodule"});
  fs::create_directories(dir / "source/child");
  const auto snap = create_snapshot(read_git_info(dir / "source"), dir / "snapshot", "refs/heads/main");

  struct TestEnvironment {
    std::map<std::string, std::optional<std::string>> saved;
    void set(const std::string& key, const std::string& value) {
      const char* previous = std::getenv(key.c_str());
      saved.emplace(key, previous ? std::optional<std::string>(previous) : std::nullopt);
      if (::setenv(key.c_str(), value.c_str(), 1) != 0) throw std::runtime_error("Cannot set test environment");
    }
    ~TestEnvironment() {
      for (const auto& [key, value] : saved) {
        if (value) ::setenv(key.c_str(), value->c_str(), 1);
        else ::unsetenv(key.c_str());
      }
    }
  } environment;
  environment.set("GIT_ALLOW_PROTOCOL", "file");
  environment.set("GIT_TEMPLATE_DIR", (dir / "template").string());
  for (const bool remote : {false, true}) {
    for (const auto* mode : {"", "false", "true", "recursive"}) {
      auto ctx = checkout_context(snap, dir / "workspace");
      ctx.token = "default-test-token";
      std::map<std::string, std::string> inputs{{"submodules", mode}, {"fetch-depth", remote ? "1" : "0"}};
      if (remote) {
        inputs["repository"] = "owner/other";
        inputs["ref"] = "main";
        inputs["token"] = "explicit-test-token";
        ctx.info = [&](std::string_view message) {
          if (message.starts_with("Fetching ")) {
            cpa_test::git(ctx.workspace, {"remote", "set-url", "origin", (dir / "source").string()});
          }
        };
      }
      const auto header = cpa::actions::git_token_env(remote ? inputs.at("token") : ctx.token).at("GIT_CONFIG_VALUE_0");
      write_file(dir / "template/hooks/post-checkout",
                 "#!/bin/sh\nset -eu\n"
                 "header=$(git config --get http.https://github.com/.extraheader)\n"
                 "[ \"$header\" = " + cpa::util::shell_quote(header) + " ]\n"
                 "if test -f .git; then\n"
                 "  test \"$(git ls-remote --get-url git@github.com:owner/repo.git)\" = https://github.com/owner/repo.git\n"
                 "  test \"$(git ls-remote --get-url git@github.enterprise.example.test:fixture-owner/repo.git)\" = "
                 "git@github.enterprise.example.test:fixture-owner/repo.git\n"
                 "  printf mapped > \"$(git rev-parse --git-path rewrite-seen)\"\n"
                 "fi\n"
                 "printf authenticated > \"$(git rev-parse --git-path auth-seen)\"\n");
      cpa::util::make_executable(dir / "template/hooks/post-checkout");
      const auto result = checkout(ctx, inputs);
      IUTEST_ASSERT_TRUE(result.ok) << mode << ": " << result.error;
      const bool enabled = std::string(mode) == "true" || std::string(mode) == "recursive";
      IUTEST_EXPECT_EQ(enabled, fs::exists(ctx.workspace / "child/file"));
      IUTEST_EXPECT_EQ(std::string(mode) == "recursive", fs::exists(ctx.workspace / "child/nested/file"));
      if (enabled) {
        IUTEST_EXPECT_EQ("direct contents\n", read_file(ctx.workspace / "child/file"));
        IUTEST_EXPECT_EQ(child_sha, cpa_test::git(ctx.workspace / "child", {"rev-parse", "HEAD"}));
        IUTEST_EXPECT_EQ("authenticated", read_file(ctx.workspace / ".git/modules/child/auth-seen"));
        IUTEST_EXPECT_EQ("mapped", read_file(ctx.workspace / ".git/modules/child/rewrite-seen"));
        IUTEST_EXPECT_EQ(std::string::npos, read_file(ctx.workspace / ".git/modules/child/config").find(header));
        IUTEST_EXPECT_EQ(std::string::npos, read_file(ctx.workspace / ".git/modules/child/config").find("insteadOf"));
        IUTEST_EXPECT_EQ(read_file(dir / "source/.gitmodules"), read_file(ctx.workspace / ".gitmodules"));
        IUTEST_EXPECT_EQ(remote ? "true" : "false",
                         cpa_test::git(ctx.workspace / "child", {"rev-parse", "--is-shallow-repository"}));
        IUTEST_EXPECT_EQ("file://" + (dir / "child").string(),
                         cpa_test::git(ctx.workspace / "child", {"remote", "get-url", "origin"}));
      }
      if (std::string(mode) == "recursive") {
        IUTEST_EXPECT_EQ("nested contents\n", read_file(ctx.workspace / "child/nested/file"));
        IUTEST_EXPECT_EQ(leaf_sha, cpa_test::git(ctx.workspace / "child/nested", {"rev-parse", "HEAD"}));
        IUTEST_EXPECT_EQ("authenticated", read_file(ctx.workspace / ".git/modules/child/modules/nested/auth-seen"));
        IUTEST_EXPECT_EQ("mapped", read_file(ctx.workspace / ".git/modules/child/modules/nested/rewrite-seen"));
        IUTEST_EXPECT_EQ(std::string::npos,
                         read_file(ctx.workspace / ".git/modules/child/modules/nested/config").find("insteadOf"));
        IUTEST_EXPECT_EQ(remote ? "true" : "false",
                         cpa_test::git(ctx.workspace / "child/nested", {"rev-parse", "--is-shallow-repository"}));
      }
    }
  }

  // A missing submodule must fail checkout rather than leave a success-shaped result.
  fs::remove(dir / "template/hooks/post-checkout");
  write_file(dir / "source/.gitmodules", modules("child", dir / "missing"));
  cpa_test::git(dir / "source", {"add", ".gitmodules"});
  cpa_test::git(dir / "source", {"commit", "-qm", "unavailable submodule"});
  const auto broken = create_snapshot(read_git_info(dir / "source"), dir / "broken", "refs/heads/main");
  for (const bool remote : {false, true}) {
    auto ctx = checkout_context(broken, dir / "workspace");
    ctx.token = "default-test-token";
    std::map<std::string, std::string> inputs{{"submodules", "recursive"}};
    if (remote) {
      inputs["repository"] = "owner/other";
      ctx.info = [&](std::string_view message) {
        if (message.starts_with("Fetching ")) {
          cpa_test::git(ctx.workspace, {"remote", "set-url", "origin", (dir / "source").string()});
        }
      };
    }
    const auto result = checkout(ctx, inputs);
    IUTEST_EXPECT_FALSE(result.ok);
    IUTEST_EXPECT_NE(std::string::npos, result.error.find("missing")) << result.error;
    IUTEST_EXPECT_TRUE(result.outputs.empty());
  }
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
