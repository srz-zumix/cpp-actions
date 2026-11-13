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
allow-apple-security-for: ['example/action@v1', './local']
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
  IUTEST_ASSERT_EQ(2u, c.allow_apple_security_for.size());
  IUTEST_EXPECT_EQ("example/action@v1", c.allow_apple_security_for[0]);
  IUTEST_EXPECT_EQ("./local", c.allow_apple_security_for[1]);
  IUTEST_EXPECT_EQ(3, c.parallel.value_or(0));
  IUTEST_EXPECT_EQ("1", c.vars.at("A"));
  IUTEST_EXPECT_EQ("prod.env", c.environments.at("prod").secrets_file);
  IUTEST_EXPECT_EQ("https://example.com", c.environments.at("prod").vars.at("URL"));

  util::write_file(dir / "bad.yml", "unknown: 1\n");
  IUTEST_EXPECT_THROW(load_config(dir / "bad.yml"), ConfigError);
  util::write_file(dir / "bad2.yml", "parallel: 0\n");
  IUTEST_EXPECT_THROW(load_config(dir / "bad2.yml"), ConfigError);
  util::write_file(dir / "bad3.yml", "allow-apple-security-for: {unexpected: mapping}\n");
  IUTEST_EXPECT_THROW(load_config(dir / "bad3.yml"), ConfigError);
}

IUTEST(ConfigTest, ExpandUser) {
  IUTEST_EXPECT_EQ(util::home_dir() / "x", expand_user("~/x"));
  IUTEST_EXPECT_EQ(std::filesystem::path("/abs"), expand_user("/abs"));
}

IUTEST(ConfigTest, SetupStepsRetainDocumentAndValidateRunSteps) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", "{}\n");
  IUTEST_EXPECT_TRUE(load_config(dir / "config.yml").setup_steps.empty());
  util::write_file(dir / "config.yml", R"(
setup-steps:
  - id: prepare
    name: Prepare local environment
    run: echo "${{ env.VALUE }}"
    env: {VALUE: configured}
    timeout-minutes: 1
  - run: echo second
    shell: sh -e {0}
    working-directory: ${{ runner.temp }}
    if: runner.os == 'macOS'
)");
  auto config = load_config(dir / "config.yml");
  auto copy = config;
  config = {};
  IUTEST_ASSERT_TRUE(copy.setup_document != nullptr);
  IUTEST_ASSERT_EQ(2u, copy.setup_steps.size());
  IUTEST_EXPECT_EQ("prepare", copy.setup_steps[0].id);
  IUTEST_EXPECT_EQ("Prepare local environment", copy.setup_steps[0].name->text);
  IUTEST_EXPECT_EQ("echo \"${{ env.VALUE }}\"", copy.setup_steps[0].run->text);
  IUTEST_EXPECT_EQ("sh -e {0}", copy.setup_steps[1].shell->text);
  for (const auto* body : {"null", "{}", "true", "[wrong]", "[{}]",
                           "[{uses: 'actions/checkout@v4'}]",
                           "[{run: echo ok, continue-on-error: true}]",
                           "[{run: echo ok, with: {key: value}}]",
                           "[{run: {bad: value}}]", "[{run: echo ok, shell: []}]",
                           "[{run: echo ok, env: []}]", "[{run: echo ok, timeout-minutes: bad}]",
                           "[{run: echo ok, unknown: true}]",
                           "[{id: same, run: echo one}, {id: same, run: echo two}]"}) {
    util::write_file(dir / "bad.yml", std::string("setup-steps: ") + body + "\n");
    IUTEST_EXPECT_THROW(load_config(dir / "bad.yml"), ConfigError) << body;
  }
  util::write_file(dir / "config.yml", "setup-steps: []\n");
  IUTEST_EXPECT_TRUE(load_config(dir / "config.yml").setup_steps.empty());
}

IUTEST(ConfigTest, DockerPermissionLists) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml",
                   "allow-docker-for: ['./local', 'owner/repo@v1']\n"
                   "allow-docker-step: ['.github/workflows/build.yml::build::docker']\n");
  const auto config = load_config(dir / "config.yml");
  IUTEST_ASSERT_EQ(2u, config.allow_docker_for.size());
  IUTEST_EXPECT_EQ("./local", config.allow_docker_for[0]);
  IUTEST_ASSERT_EQ(1u, config.allow_docker_step.size());
  IUTEST_EXPECT_EQ(".github/workflows/build.yml::build::docker", config.allow_docker_step[0]);
  for (const auto* key : {"allow-docker-for", "allow-docker-step"}) {
    util::write_file(dir / "config.yml", std::string(key) + ": {bad: mapping}\n");
    IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError);
  }
}

IUTEST(ConfigTest, MsbuildPermissionLists) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml",
                   "allow-msbuild-for: ['./local', 'owner/repo@v1']\n"
                   "allow-msbuild-step: ['.github/workflows/build.yml::build::restore']\n");
  const auto c = load_config(dir / "config.yml");
  IUTEST_ASSERT_EQ(2u, c.allow_msbuild_for.size());
  IUTEST_EXPECT_EQ("./local", c.allow_msbuild_for[0]);
  IUTEST_ASSERT_EQ(1u, c.allow_msbuild_step.size());
  IUTEST_EXPECT_EQ(".github/workflows/build.yml::build::restore", c.allow_msbuild_step[0]);
  for (const auto* key : {"allow-msbuild-for", "allow-msbuild-step"}) {
    util::write_file(dir / "config.yml", std::string(key) + ": {bad: mapping}\n");
    IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError);
  }
}

IUTEST(ConfigTest, UnityPermissionLists) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", "allow-unity-for: ['./local', 'owner/action@v1']\n"
                                     "allow-unity-step: ['.github/workflows/build.yml::build::unity']\n");
  const auto config = load_config(dir / "config.yml");
  IUTEST_ASSERT_EQ(2u, config.allow_unity_for.size());
  IUTEST_EXPECT_EQ("./local", config.allow_unity_for[0]);
  IUTEST_ASSERT_EQ(1u, config.allow_unity_step.size());
  IUTEST_EXPECT_EQ(".github/workflows/build.yml::build::unity", config.allow_unity_step[0]);
  for (const auto* key : {"allow-unity-for", "allow-unity-step"}) {
    util::write_file(dir / "config.yml", std::string(key) + ": {bad: mapping}\n");
    IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError);
  }
}

IUTEST(ConfigTest, XcodePermissionList) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", "{}\n");
  IUTEST_EXPECT_TRUE(load_config(dir / "config.yml").allow_xcode_for.empty());
  util::write_file(dir / "config.yml", "allow-xcode-for: ['./local', 'owner/action@v1']\n");
  const auto config = load_config(dir / "config.yml");
  IUTEST_ASSERT_EQ(2u, config.allow_xcode_for.size());
  IUTEST_EXPECT_EQ("./local", config.allow_xcode_for[0]);
  IUTEST_EXPECT_EQ("owner/action@v1", config.allow_xcode_for[1]);
  util::write_file(dir / "config.yml", "allow-xcode-for: {bad: mapping}\n");
  IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError);
}

IUTEST(ConfigTest, XcodeSimulatorRequiresBooleanOptIn) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", "{}\n");
  IUTEST_EXPECT_FALSE(load_config(dir / "config.yml").allow_xcode_simulator.has_value());
  for (const bool enabled : {false, true}) {
    util::write_file(dir / "config.yml", std::string("allow-xcode-simulator: ") + (enabled ? "true\n" : "false\n"));
    const auto config = load_config(dir / "config.yml");
    IUTEST_ASSERT_TRUE(config.allow_xcode_simulator.has_value());
    IUTEST_EXPECT_EQ(enabled, *config.allow_xcode_simulator);
  }

  for (const auto* value : {"'true'", "1", "[]", "{}", "null"}) {
    util::write_file(dir / "config.yml", std::string("allow-xcode-simulator: ") + value + "\n");
    IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError);
  }
}

IUTEST(ConfigTest, UnityHostsRequireBooleanOptIn) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", "{}\n");
  IUTEST_EXPECT_FALSE(load_config(dir / "config.yml").allow_unity_hosts.has_value());
  for (const bool enabled : {false, true}) {
    util::write_file(dir / "config.yml", std::string("allow-unity-hosts: ") + (enabled ? "true\n" : "false\n"));
    const auto config = load_config(dir / "config.yml");
    IUTEST_ASSERT_TRUE(config.allow_unity_hosts.has_value());
    IUTEST_EXPECT_EQ(enabled, *config.allow_unity_hosts);
  }
  for (const auto* value : {"'true'", "1", "[]", "{}", "null"}) {
    util::write_file(dir / "config.yml", std::string("allow-unity-hosts: ") + value + "\n");
    IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError);
  }
}

IUTEST(ConfigTest, InheritUnityEditorPrefsRequiresBooleanOptIn) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", "{}\n");
  IUTEST_EXPECT_FALSE(load_config(dir / "config.yml").inherit_unity_editor_prefs.has_value());
  for (const bool enabled : {false, true}) {
    util::write_file(dir / "config.yml",
                     std::string("inherit-unity-editor-prefs: ") + (enabled ? "true\n" : "false\n"));
    const auto config = load_config(dir / "config.yml");
    IUTEST_ASSERT_TRUE(config.inherit_unity_editor_prefs.has_value());
    IUTEST_EXPECT_EQ(enabled, *config.inherit_unity_editor_prefs);
  }
  for (const auto* value : {"'true'", "1", "[]", "{}", "null"}) {
    util::write_file(dir / "config.yml", std::string("inherit-unity-editor-prefs: ") + value + "\n");
    IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError);
  }
}

IUTEST(ConfigTest, IgnoreInteractiveShellRequiresBooleanOptIn) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", "{}\n");
  IUTEST_EXPECT_FALSE(load_config(dir / "config.yml").ignore_interactive_shell.has_value());
  for (const bool enabled : {false, true}) {
    util::write_file(dir / "config.yml",
                     std::string("ignore-interactive-shell: ") + (enabled ? "true\n" : "false\n"));
    const auto config = load_config(dir / "config.yml");
    IUTEST_ASSERT_TRUE(config.ignore_interactive_shell.has_value());
    IUTEST_EXPECT_EQ(enabled, *config.ignore_interactive_shell);
  }
  for (const auto* value : {"'true'", "1", "[]", "{}", "null"}) {
    util::write_file(dir / "config.yml", std::string("ignore-interactive-shell: ") + value + "\n");
    IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError);
  }
}

IUTEST(ConfigTest, ProxyMaxConnectionsRequiresBoundedPositiveInteger) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", "{}\n");
  IUTEST_EXPECT_FALSE(load_config(dir / "config.yml").proxy_max_connections.has_value());
  util::write_file(dir / "config.yml", "proxy-max-connections: 256\n");
  IUTEST_EXPECT_EQ(256, load_config(dir / "config.yml").proxy_max_connections.value_or(0));
  for (const auto* value : {"0", "-1", "4097", "abc", "[]", "1.5"}) {
    util::write_file(dir / "config.yml", std::string("proxy-max-connections: ") + value + "\n");
    IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError) << value;
  }
}
