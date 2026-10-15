#include "workspace/snapshot.hpp"

#include "util/fs.hpp"
#include "util/strings.hpp"

namespace cpa::workspace {

namespace {

const std::string kZeroSha(40, '0');

void must(const fs::path& dir, const std::vector<std::string>& args) {
  if (!git_ok(dir, args)) throw SnapshotError("git " + util::join(args, " ") + " failed in " + dir.string());
}

void copy_worktree_files(const fs::path& root, const fs::path& dest) {
  for (const auto& rel : list_worktree_files(root)) {
    const auto from = root / rel;
    const auto to = dest / rel;
    std::error_code ec;
    const auto st = fs::symlink_status(from, ec);
    if (ec || !fs::exists(st)) continue;  // deleted in the working tree
    if (fs::is_directory(st)) {
      fs::create_directories(to);  // submodule (gitlink)
      continue;
    }
    fs::create_directories(to.parent_path());
    util::copy_entry(from, to);
  }
}

}  // namespace

Snapshot create_snapshot(const GitInfo& git, const fs::path& dest, const std::string& ref) {
  Snapshot snap;
  snap.dir = dest;
  std::error_code ec;
  if (fs::exists(dest, ec)) throw SnapshotError(dest.string() + " already exists");
  if (!git.is_repo || git.head_sha.empty()) {
    if (git.is_repo) {
      fs::create_directories(dest);
      copy_worktree_files(git.root, dest);
    } else {
      util::copy_tree(git.root, dest);
    }
    snap.sha = kZeroSha;
    return snap;
  }
  fs::create_directories(dest.parent_path());
  must(dest.parent_path(), {"clone", "--quiet", "--no-checkout", "--local", git.root.string(), dest.string()});
  snap.is_git = true;
  must(dest, {"reset", "--quiet"});
  copy_worktree_files(git.root, dest);
  must(dest, {"-c", "core.autocrlf=false", "-c", "core.safecrlf=false", "add", "--all"});
  snap.sha = git.head_sha;
  if (!git_ok(dest, {"diff", "--cached", "--quiet"})) {
    must(dest, {"-c", "user.name=cpp-actions", "-c", "user.email=cpp-actions@localhost", "-c", "commit.gpgsign=false",
                "-c", "core.hooksPath=/dev/null", "commit", "--quiet", "--no-verify", "--no-gpg-sign", "-m",
                "cpp-actions: uncommitted changes"});
    const auto sha = git_output(dest, {"rev-parse", "HEAD"});
    if (!sha) throw SnapshotError("cannot resolve the snapshot commit");
    snap.sha = *sha;
    snap.dirty = true;
  }
  if (ref.starts_with("refs/heads/")) {
    must(dest, {"update-ref", ref, snap.sha});
    must(dest, {"symbolic-ref", "HEAD", ref});
  } else {
    must(dest, {"update-ref", "--no-deref", "HEAD", snap.sha});
    if (ref.starts_with("refs/tags/")) must(dest, {"update-ref", ref, snap.sha});
  }
  if (git.github) {
    must(dest, {"remote", "set-url", "origin", "https://github.com/" + git.github->full_name()});
  } else {
    must(dest, {"remote", "remove", "origin"});
  }
  return snap;
}

void copy_snapshot(const Snapshot& snapshot, const fs::path& dest) { util::copy_tree(snapshot.dir, dest); }

}  // namespace cpa::workspace
