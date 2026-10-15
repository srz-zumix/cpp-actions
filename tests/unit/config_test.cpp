#include "iutest.hpp"

#include "config/config.hpp"
#include "test_support.hpp"
#include "util/fs.hpp"

using namespace cpa;
using namespace cpa::config;

IUTEST(ConfigTest, Dotenv) {
  const auto env = parse_dotenv(
      "# comment\n"
      "A=1\n"
      "export B = two words  # trailing comment\n"
      "\n"
      "C='single # kept'\n"
      "D=\"line1\\nline2\"\n"
      "E=\"multi\n"
      "line\"\n"
      "F=\n"
      "G=a=b\r\n");
  IUTEST_ASSERT_EQ(7u, env.size());
  IUTEST_EXPECT_EQ("1", env.at("A"));
  IUTEST_EXPECT_EQ("two words", env.at("B"));
  IUTEST_EXPECT_EQ("single # kept", env.at("C"));
  IUTEST_EXPECT_EQ("line1\nline2", env.at("D"));
  IUTEST_EXPECT_EQ("multi\nline", env.at("E"));
  IUTEST_EXPECT_EQ("", env.at("F"));
  IUTEST_EXPECT_EQ("a=b", env.at("G"));

  IUTEST_EXPECT_THROW(parse_dotenv("NOVALUE\n"), ConfigError);
  IUTEST_EXPECT_THROW(parse_dotenv("1A=x\n"), ConfigError);
  IUTEST_EXPECT_THROW(parse_dotenv("A=\"open\n"), ConfigError);
  IUTEST_EXPECT_THROW(parse_dotenv("A='x' y\n"), ConfigError);
}

IUTEST(ConfigTest, SplitAssignment) {
  const auto [n1, v1] = split_assignment("A=b=c");
  IUTEST_EXPECT_EQ("A", n1);
  IUTEST_ASSERT_TRUE(v1.has_value());
  IUTEST_EXPECT_EQ("b=c", *v1);
  const auto [n2, v2] = split_assignment("TOKEN");
  IUTEST_EXPECT_EQ("TOKEN", n2);
  IUTEST_EXPECT_FALSE(v2.has_value());
}

IUTEST(ConfigTest, LoadConfig) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", R"(allow-hosts: [registry.npmjs.org, '*.pypi.org']
allow-read: ~/sdk
allow-localhost: true
parallel: 3
host-labels: [ubuntu-latest]
vars: {A: 1}
environments:
  prod:
    secrets-file: prod.env
    vars: {URL: https://example.com}
)");
  const auto c = load_config(dir / "config.yml");
  IUTEST_ASSERT_EQ(2u, c.allow_hosts.size());
  IUTEST_EXPECT_EQ("*.pypi.org", c.allow_hosts[1]);
  IUTEST_ASSERT_EQ(1u, c.allow_read.size());
  IUTEST_EXPECT_EQ("~/sdk", c.allow_read[0]);
  IUTEST_EXPECT_TRUE(c.allow_localhost.value_or(false));
  IUTEST_EXPECT_EQ(3, c.parallel.value_or(0));
  IUTEST_EXPECT_EQ("1", c.vars.at("A"));
  IUTEST_EXPECT_EQ("prod.env", c.environments.at("prod").secrets_file);
  IUTEST_EXPECT_EQ("https://example.com", c.environments.at("prod").vars.at("URL"));

  util::write_file(dir / "bad.yml", "unknown: 1\n");
  IUTEST_EXPECT_THROW(load_config(dir / "bad.yml"), ConfigError);
  util::write_file(dir / "bad2.yml", "parallel: 0\n");
  IUTEST_EXPECT_THROW(load_config(dir / "bad2.yml"), ConfigError);
}

IUTEST(ConfigTest, ExpandUser) {
  IUTEST_EXPECT_EQ(util::home_dir() / "x", expand_user("~/x"));
  IUTEST_EXPECT_EQ(std::filesystem::path("/abs"), expand_user("/abs"));
}
