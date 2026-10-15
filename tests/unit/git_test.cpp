#include "iutest.hpp"

#include <algorithm>

#include "test_support.hpp"
#include "workspace/git.hpp"

using namespace cpa::workspace;

IUTEST(GitTest, ParseGitHubRemote) {
  const struct {
    const char* url;
    const char* full;
  } cases[] = {
      {"https://github.com/owner/repo.git", "owner/repo"},
      {"https://github.com/owner/repo", "owner/repo"},
      {"https://user@github.com/owner/repo.git/", "owner/repo"},
      {"http://github.com/o/r", "o/r"},
      {"git@github.com:owner/repo.git", "owner/repo"},
      {"ssh://git@github.com/owner/repo", "owner/repo"},
      {"ssh://git@github.com:22/owner/repo.git", "owner/repo"},
      {"git://github.com/owner/repo.git", "owner/repo"},
  };
  for (const auto& c : cases) {
    const auto r = parse_github_remote(c.url);
    IUTEST_ASSERT_TRUE(r.has_value()) << c.url;
    IUTEST_EXPECT_EQ(c.full, r->full_name()) << c.url;
  }
  IUTEST_EXPECT_FALSE(parse_github_remote("https://gitlab.com/owner/repo.git").has_value());
  IUTEST_EXPECT_FALSE(parse_github_remote("/local/path/repo").has_value());
  IUTEST_EXPECT_FALSE(parse_github_remote("https://github.com/owner").has_value());
}

IUTEST(GitTest, ReadGitInfo) {
  cpa_test::TempDir dir;
  cpa_test::init_repo(dir.path(), {{"a.txt", "a\n"}});
  cpa_test::git(dir.path(), {"remote", "add", "origin", "git@github.com:octo/hello.git"});
  cpa_test::git(dir.path(), {"tag", "v1"});
  cpa::util::write_file(dir / "sub/x.txt", "x");

  const auto info = read_git_info(dir / "sub");
  IUTEST_EXPECT_TRUE(info.is_repo);
  IUTEST_EXPECT_EQ(cpa::util::real_path(dir.path()), info.root);
  IUTEST_EXPECT_EQ(cpa_test::git(dir.path(), {"rev-parse", "HEAD"}), info.head_sha);
  IUTEST_EXPECT_EQ("initial", info.head_message);
  IUTEST_EXPECT_EQ("main", info.branch);
  IUTEST_EXPECT_EQ("main", info.default_branch);
  IUTEST_ASSERT_EQ(1u, info.tags.size());
  IUTEST_EXPECT_EQ("v1", info.tags[0]);
  IUTEST_ASSERT_TRUE(info.github.has_value());
  IUTEST_EXPECT_EQ("octo/hello", info.github->full_name());
}

IUTEST(GitTest, NotARepository) {
  cpa_test::TempDir dir;
  const auto info = read_git_info(dir.path());
  IUTEST_EXPECT_FALSE(info.is_repo);
  IUTEST_EXPECT_EQ("main", info.default_branch);
  IUTEST_EXPECT_FALSE(find_repo_root(dir.path()).has_value());
}

IUTEST(GitTest, ChangedAndWorktreeFiles) {
  cpa_test::TempDir dir;
  cpa_test::init_repo(dir.path(), {{"a.txt", "a\n"}, {"b.txt", "b\n"}, {".gitignore", "*.log\n"}});
  const auto base = cpa_test::git(dir.path(), {"rev-parse", "HEAD"});
  cpa::util::write_file(dir / "a.txt", "changed\n");
  cpa::util::write_file(dir / "new dir/c.txt", "c\n");
  cpa::util::write_file(dir / "ignored.log", "x\n");

  auto changed = changed_files_since(dir.path(), base);
  IUTEST_ASSERT_TRUE(changed.has_value());
  std::sort(changed->begin(), changed->end());
  IUTEST_ASSERT_EQ(2u, changed->size());
  IUTEST_EXPECT_EQ("a.txt", (*changed)[0]);
  IUTEST_EXPECT_EQ("new dir/c.txt", (*changed)[1]);
  IUTEST_EXPECT_FALSE(changed_files_since(dir.path(), "no-such-revision").has_value());

  auto files = list_worktree_files(dir.path());
  std::sort(files.begin(), files.end());
  IUTEST_ASSERT_EQ(4u, files.size());
  IUTEST_EXPECT_EQ(".gitignore", files[0]);
  IUTEST_EXPECT_EQ("a.txt", files[1]);
  IUTEST_EXPECT_EQ("b.txt", files[2]);
  IUTEST_EXPECT_EQ("new dir/c.txt", files[3]);

  auto committed = files_of_commit(dir.path(), "HEAD");
  std::sort(committed.begin(), committed.end());
  IUTEST_ASSERT_EQ(3u, committed.size());
  IUTEST_EXPECT_EQ(".gitignore", committed[0]);
}
