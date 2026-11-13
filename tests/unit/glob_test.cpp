#include "iutest.hpp"

#include <filesystem>

#include "util/fs.hpp"
#include "util/glob.hpp"
#include "util/strings.hpp"

using namespace cpa::util;

IUTEST(GlobTest, FilterPatterns) {
  IUTEST_EXPECT_TRUE(filter_match("main", "main"));
  IUTEST_EXPECT_FALSE(filter_match("main", "main2"));
  IUTEST_EXPECT_TRUE(filter_match("feature/*", "feature/x"));
  IUTEST_EXPECT_FALSE(filter_match("feature/*", "feature/x/y"));
  IUTEST_EXPECT_TRUE(filter_match("feature/**", "feature/x/y"));
  IUTEST_EXPECT_TRUE(filter_match("v[12].[0-9]+.[0-9]+", "v1.10.3"));
  IUTEST_EXPECT_FALSE(filter_match("v[12].[0-9]+.[0-9]+", "v3.1.0"));
  IUTEST_EXPECT_TRUE(filter_match("*.jsx?", "page.js"));
  IUTEST_EXPECT_TRUE(filter_match("*.jsx?", "page.jsx"));
  IUTEST_EXPECT_TRUE(filter_match("docs/\\*", "docs/*"));
  IUTEST_EXPECT_FALSE(filter_match("docs/\\*", "docs/a"));
  IUTEST_EXPECT_TRUE(filter_match("**.js", "a/b/c.js"));
  IUTEST_EXPECT_FALSE(filter_match("a.b", "axb"));
}

IUTEST(GlobTest, FilterList) {
  const std::vector<std::string> patterns = {"releases/**", "!releases/**-alpha"};
  IUTEST_EXPECT_TRUE(filter_list_match(patterns, "releases/10"));
  IUTEST_EXPECT_FALSE(filter_list_match(patterns, "releases/10-alpha"));
  IUTEST_EXPECT_FALSE(filter_list_match(patterns, "main"));
}

IUTEST(GlobTest, GlobMatch) {
  IUTEST_EXPECT_TRUE(glob_match("**/package-lock.json", "package-lock.json"));
  IUTEST_EXPECT_TRUE(glob_match("**/package-lock.json", "a/b/package-lock.json"));
  IUTEST_EXPECT_TRUE(glob_match("src/*.cpp", "src/main.cpp"));
  IUTEST_EXPECT_FALSE(glob_match("src/*.cpp", "src/x/main.cpp"));
  IUTEST_EXPECT_TRUE(glob_match("src/?.c", "src/a.c"));
  IUTEST_EXPECT_FALSE(glob_match("src/?.c", "src/ab.c"));
  IUTEST_EXPECT_TRUE(glob_match("*.[ch]", "x.h"));
  IUTEST_EXPECT_TRUE(glob_match("*", ".hidden"));
  IUTEST_EXPECT_TRUE(glob_match("./a/b", "a/b"));
}

IUTEST(GlobTest, GlobStringMatchesAcrossSlashes) {
  IUTEST_EXPECT_TRUE(glob_match_string("*", "feature/nested/branch"));
  IUTEST_EXPECT_TRUE(glob_match_string("feature/*", "feature/a/b"));
  IUTEST_EXPECT_TRUE(glob_match_string("v*", "v"));
  IUTEST_EXPECT_TRUE(glob_match_string("a**b*c", "ab-c"));
  IUTEST_EXPECT_TRUE(glob_match_string("v+*", "v+1"));
  IUTEST_EXPECT_FALSE(glob_match_string("v+*", "vv1"));
  IUTEST_EXPECT_FALSE(glob_match_string("v1.*", "v1x2"));
  IUTEST_EXPECT_FALSE(glob_match_string("v*", "release/v1"));
}

IUTEST(GlobTest, GlobFiles) {
  const auto root = temp_dir() / ("cpa-glob-" + random_hex(6));
  write_file(root / "a.lock", "1");
  write_file(root / "sub" / "b.lock", "2");
  write_file(root / "sub" / "c.txt", "3");
  write_file(root / "vendor" / "d.lock", "4");
  auto files = glob_files(root, {"**/*.lock", "!vendor"});
  IUTEST_ASSERT_EQ(2u, files.size());
  IUTEST_EXPECT_EQ(root / "a.lock", files[0]);
  IUTEST_EXPECT_EQ(root / "sub" / "b.lock", files[1]);
  files = glob_files(root, {"sub"});
  IUTEST_EXPECT_EQ(2u, files.size());
  remove_tree(root);
}
