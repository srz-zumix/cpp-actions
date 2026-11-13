#include "iutest.hpp"

#include <algorithm>

#include "test_support.hpp"
#include "workspace/git.hpp"

using namespace cpa::workspace;

IUTEST(GitTest, RunnerConfigurationIgnoresInheritedOverrides) {
  cpa_test::TempDir dir;
  cpa::util::write_file(dir / ".gitconfig", "invalid global configuration\n");
  cpa::util::write_file(dir / "git/config", "invalid XDG configuration\n");
  cpa::util::write_file(dir / "system", "invalid system configuration\n");
  const auto env = git_environment({
      {"PATH", cpa::util::current_environment().at("PATH")},
      {"HOME", dir.path().string()},
      {"XDG_CONFIG_HOME", dir.path().string()},
      {"GIT_CONFIG", (dir / "system").string()},
      {"GIT_CONFIG_GLOBAL", (dir / ".gitconfig").string()},
      {"GIT_CONFIG_SYSTEM", (dir / "system").string()},
      {"GIT_CONFIG_NOSYSTEM", "0"},
      {"GIT_CONFIG_COUNT", "1"},
      {"GIT_CONFIG_KEY_0", "filter.lfs.required"},
      {"GIT_CONFIG_VALUE_0", "false"},
      {"GIT_CONFIG_PARAMETERS", "'filter.lfs.required=false'"},
  });
  IUTEST_EXPECT_EQ("1", env.at("GIT_CONFIG_NOSYSTEM"));
  IUTEST_EXPECT_EQ("/dev/null", env.at("GIT_CONFIG_SYSTEM"));
  IUTEST_EXPECT_NE((dir / ".gitconfig").string(), env.at("GIT_CONFIG_GLOBAL"));
  for (const auto* name : {"GIT_CONFIG", "GIT_CONFIG_COUNT", "GIT_CONFIG_KEY_0", "GIT_CONFIG_VALUE_0",
                           "GIT_CONFIG_PARAMETERS"}) {
    IUTEST_EXPECT_EQ(0u, env.count(name)) << name;
  }
  for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
           {"safe.directory", "*"}, {"filter.lfs.clean", "git-lfs clean -- %f"},
           {"filter.lfs.smudge", "git-lfs smudge -- %f"}, {"filter.lfs.process", "git-lfs filter-process"},
           {"filter.lfs.required", "true"}, {"advice.detachedHead", "false"}}) {
    const auto result = cpa::util::capture({"git", "config", "--global", "--get", key}, dir.path(), &env);
    IUTEST_EXPECT_EQ(0, result.exit_code) << result.err;
    IUTEST_EXPECT_EQ(value + "\n", result.out) << key;
  }
}

IUTEST(GitTest, LocalAndCommandLineConfigOverrideRunnerDefaults) {
  cpa_test::TempDir dir;
  cpa_test::init_repo(dir.path(), {{"file", "initial"}});
  cpa_test::git(dir.path(), {"config", "--local", "filter.lfs.required", "false"});
  cpa_test::git(dir.path(), {"config", "--local", "advice.detachedHead", "true"});
  IUTEST_EXPECT_EQ("false", git_output(dir.path(), {"config", "--get", "filter.lfs.required"}).value_or(""));
  IUTEST_EXPECT_EQ("true", git_output(dir.path(), {"config", "--get", "advice.detachedHead"}).value_or(""));
  IUTEST_EXPECT_EQ("true", git_output(dir.path(), {"-c", "filter.lfs.required=true", "config", "--get",
                                                 "filter.lfs.required"}).value_or(""));
}

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
  IUTEST_EXPECT_FALSE(parse_github_remote("https://git.example.test/owner/repo.git").has_value());
  IUTEST_EXPECT_FALSE(parse_github_remote("/local/path/repo").has_value());
  IUTEST_EXPECT_FALSE(parse_github_remote("https://github.com/owner").has_value());
}

IUTEST(GitTest, ReadGitInfo) {
  cpa_test::TempDir dir;
  cpa_test::init_repo(dir.path(), {{"a.txt", "a\n"}});
  cpa_test::git(dir.path(), {"remote", "add", "origin", "git@github.com:fixture-owner/hello.git"});
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
  IUTEST_EXPECT_EQ("fixture-owner/hello", info.github->full_name());
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
