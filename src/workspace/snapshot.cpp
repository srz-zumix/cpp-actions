#include "workspace/snapshot.hpp"

#include "util/fs.hpp"
#include "util/strings.hpp"

namespace cpa::workspace {

namespace {

const std::string kZeroSha(40, '0');

void must(const fs::path& dir, const std::vector<std::string>& args) {
  // Background repacking must not race with copy_snapshot().
  std::vector<std::string> command = {"-c", "maintenance.auto=false", "-c", "gc.auto=0"};
  command.insert(command.end(), args.begin(), args.end());
  if (!git_ok(dir, command)) throw SnapshotError("git " + util::join(args, " ") + " failed in " + dir.string());
}

void copy_tree(const fs::path& from, const fs::path& to) {
  const auto source = util::real_path(from);
  const auto destination = util::real_path(to);
  if (util::is_subpath(destination, source) || util::is_subpath(source, destination)) {
    throw SnapshotError("snapshot source and destination must not overlap");
  }
  for (auto parent = fs::absolute(to).parent_path(); parent != parent.root_path(); parent = parent.parent_path()) {
    if (fs::is_symlink(fs::symlink_status(parent))) {
      throw SnapshotError("snapshot destination has a symlinked ancestor");
    }
  }
  const auto prepare = [](const fs::path& path, bool directory) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (fs::is_symlink(status) || (fs::exists(status) && fs::is_directory(status) != directory)) {
      util::remove_tree(path);
    } else if (!directory && fs::exists(status)) {
      fs::remove(path);
    }
    if (directory) fs::create_directories(path);
  };
  prepare(to, true);
  for (const auto& entry : fs::recursive_directory_iterator(from)) {
    // relative() resolves symlinks, potentially producing a path outside `to`.
    const auto target = to / entry.path().lexically_relative(from);
    prepare(target, fs::is_directory(entry.symlink_status()));
    if (!fs::is_directory(entry.symlink_status())) util::copy_entry(entry.path(), target);
  }
}

void copy_worktree_files(const fs::path& root, const fs::path& dest) {
  for (const auto& rel : list_worktree_files(root)) {
    bool through_symlink = false;
    for (auto parent = fs::path(rel).parent_path(); !parent.empty(); parent = parent.parent_path()) {
      if (fs::is_symlink(fs::symlink_status(root / parent))) {
        through_symlink = true;
        break;
      }
    }
    if (through_symlink) continue;  // tracked descendants of a directory replaced by a symlink
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
  if (fs::exists(fs::symlink_status(dest, ec))) throw SnapshotError(dest.string() + " already exists");
  if (util::is_subpath(util::real_path(dest), util::real_path(git.root))) {
    throw SnapshotError("snapshot destination must be outside the source repository");
  }
  if (!git.is_repo || git.head_sha.empty()) {
    if (git.is_repo) {
      fs::create_directories(dest);
      copy_worktree_files(git.root, dest);
    } else {
      copy_tree(git.root, dest);
    }
    snap.sha = kZeroSha;
    return snap;
  }
  const auto absolute_dest = fs::absolute(dest);
  fs::create_directories(absolute_dest.parent_path());
  must(absolute_dest.parent_path(),
       {"clone", "--quiet", "--no-checkout", "--local", git.root.string(), absolute_dest.string()});
  snap.is_git = true;
  must(dest, {"reset", "--quiet", git.head_sha});
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
    // Keep the cloned branch refs available for an explicit local checkout.
    must(dest, {"config", "--remove-section", "remote.origin"});
  }
  return snap;
}

void copy_snapshot(const Snapshot& snapshot, const fs::path& dest) { copy_tree(snapshot.dir, dest); }

}  // namespace cpa::workspace
