#pragma once

#include <filesystem>
#include <string>

#include "workspace/git.hpp"

namespace cpa::workspace {

struct Snapshot {
  fs::path dir;
  bool is_git = false;
  std::string sha;     // commit of the snapshot (HEAD plus the uncommitted changes)
  bool dirty = false;  // the working tree had uncommitted changes
};

class SnapshotError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Copies the working tree of a repository, including uncommitted changes and
// untracked files that are not ignored, into `dest` (which must not exist).
// For git repositories the snapshot is a clone whose HEAD is `ref` (a branch
// is checked out for refs/heads/*, HEAD is detached otherwise) and whose last
// commit contains the uncommitted changes. Non-git directories are copied.
Snapshot create_snapshot(const GitInfo& git, const fs::path& dest, const std::string& ref);

// Copies a snapshot to a non-overlapping workspace directory (clonefile on
// macOS), replacing destination entries without following their symlinks.
// Symlinked destination ancestors are rejected.
void copy_snapshot(const Snapshot& snapshot, const fs::path& dest);

}  // namespace cpa::workspace
