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
  IUTEST_EXPECT_EQ("prod.env", c.environments.at("prod").secrets_file.value_or(""));
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

IUTEST(ConfigTest, MergeListsScalarsAndSourceRelativePaths) {
  cpa_test::TempDir dir;
  const auto user = dir / "user/config.yml";
  const auto repo = dir / "repo/.cpp-actions.yml";
  std::string user_config = R"(
allow-read: [sdk, ~/shared-sdk]
allow-write: output
allow-localhost: true
allow-xcode: true
allow-xcode-simulator: true
allow-unity-hosts: true
ignore-interactive-shell: true
inherit-unity-editor-prefs: true
parallel: 3
proxy-max-connections: 128
node: bin/node
cache-dir: cache
secrets-file: secrets.env
vars-file: vars.env
vars: {USER_ONLY: kept, BOTH: user}
environments:
  Prod:
    secrets-file: prod.env
    vars-file: prod-vars.env
    vars: {USER_ONLY: kept, BOTH: user}
)";
  std::string repo_config = R"(
allow-read: repo-sdk
allow-write: repo-output
allow-localhost: false
allow-xcode: false
allow-xcode-simulator: false
allow-unity-hosts: false
ignore-interactive-shell: false
inherit-unity-editor-prefs: false
parallel: 2
proxy-max-connections: 64
node: repo-node
cache-dir: repo-cache
secrets-file: repo-secrets.env
vars-file: repo-vars.env
vars: {REPO_ONLY: added, BOTH: repository}
environments:
  prod:
    secrets-file: repo-prod.env
    vars: {REPO_ONLY: added, BOTH: repository}
)";
  const std::vector<std::pair<std::string, std::vector<std::string> Config::*>> lists = {
      {"allow-hosts", &Config::allow_hosts},
      {"allow-apple-security-for", &Config::allow_apple_security_for},
      {"allow-docker-for", &Config::allow_docker_for},
      {"allow-docker-step", &Config::allow_docker_step},
      {"allow-msbuild-for", &Config::allow_msbuild_for},
      {"allow-msbuild-step", &Config::allow_msbuild_step},
      {"allow-unity-for", &Config::allow_unity_for},
      {"allow-unity-step", &Config::allow_unity_step},
      {"allow-xcode-for", &Config::allow_xcode_for},
      {"allow-xcode-simulator-job", &Config::allow_xcode_simulator_job},
      {"env-pass", &Config::pass_env},
      {"host-labels", &Config::host_labels}};
  for (const auto& [key, member] : lists) {
    (void)member;
    user_config += key + ": [user]\n";
    repo_config += key + ": [repository]\n";
  }
  util::write_file(user, user_config);
  util::write_file(repo, repo_config);
  const auto merged = load_configs({user, repo});
  for (const auto& [key, member] : lists) {
    IUTEST_EXPECT_EQ((std::vector<std::string>{"user", "repository"}), merged.*member) << key;
  }
  IUTEST_EXPECT_EQ((std::vector<std::string>{(dir / "user/sdk").string(),
                                            (util::home_dir() / "shared-sdk").string(),
                                            (dir / "repo/repo-sdk").string()}), merged.allow_read);
  IUTEST_EXPECT_EQ((std::vector<std::string>{(dir / "user/output").string(),
                                            (dir / "repo/repo-output").string()}), merged.allow_write);
  IUTEST_EXPECT_EQ(2, merged.parallel.value_or(0));
  IUTEST_EXPECT_EQ(64, merged.proxy_max_connections.value_or(0));
  IUTEST_EXPECT_FALSE(merged.allow_localhost.value_or(true));
  IUTEST_EXPECT_FALSE(merged.allow_xcode.value_or(true));
  IUTEST_EXPECT_FALSE(merged.allow_xcode_simulator.value_or(true));
  IUTEST_EXPECT_FALSE(merged.allow_unity_hosts.value_or(true));
  IUTEST_EXPECT_FALSE(merged.ignore_interactive_shell.value_or(true));
  IUTEST_EXPECT_FALSE(merged.inherit_unity_editor_prefs.value_or(true));
  IUTEST_EXPECT_EQ((dir / "repo/repo-node").string(), merged.node.value_or(""));
  IUTEST_EXPECT_EQ((dir / "repo/repo-cache").string(), merged.cache_dir.value_or(""));
  IUTEST_EXPECT_EQ((dir / "repo/repo-secrets.env").string(), merged.secrets_file.value_or(""));
  IUTEST_EXPECT_EQ((dir / "repo/repo-vars.env").string(), merged.vars_file.value_or(""));
  IUTEST_EXPECT_EQ("kept", merged.vars.at("USER_ONLY"));
  IUTEST_EXPECT_EQ("added", merged.vars.at("REPO_ONLY"));
  IUTEST_EXPECT_EQ("repository", merged.vars.at("BOTH"));
  IUTEST_ASSERT_EQ(1u, merged.environments.size());
  const auto& environment = merged.environments.at("prod");
  IUTEST_EXPECT_EQ((dir / "repo/repo-prod.env").string(), environment.secrets_file.value_or(""));
  IUTEST_EXPECT_EQ((dir / "user/prod-vars.env").string(), environment.vars_file.value_or(""));
  IUTEST_EXPECT_EQ("kept", environment.vars.at("USER_ONLY"));
  IUTEST_EXPECT_EQ("added", environment.vars.at("REPO_ONLY"));
  IUTEST_EXPECT_EQ("repository", environment.vars.at("BOTH"));
}

IUTEST(ConfigTest, EmptyScalarsClearEarlierPathsAndAbsentValuesAreRetained) {
  cpa_test::TempDir dir;
  const auto user = dir / "user.yml";
  const auto repo = dir / "repo.yml";
  util::write_file(user, R"(
node: user-node
cache-dir: user-cache
secrets-file: user-secrets.env
vars-file: user-vars.env
parallel: 3
environments:
  prod:
    secrets-file: prod.env
    vars-file: prod-vars.env
)");
  util::write_file(repo, R"(
node: ""
cache-dir: ""
secrets-file: ""
vars-file: ""
environments:
  prod:
    secrets-file: ""
    vars-file: ""
)");
  const auto merged = load_configs({user, repo});
  for (const auto* value : {&merged.node, &merged.cache_dir, &merged.secrets_file, &merged.vars_file,
                            &merged.environments.at("prod").secrets_file,
                            &merged.environments.at("prod").vars_file}) {
    IUTEST_ASSERT_TRUE(value->has_value());
    IUTEST_EXPECT_TRUE(value->value().empty());
  }
  IUTEST_EXPECT_EQ(3, merged.parallel.value_or(0));
  IUTEST_EXPECT_FALSE(load_configs({}).parallel.has_value());
}

IUTEST(ConfigTest, MergedSetupStepsRetainSourcesAndHaveUniqueIds) {
  cpa_test::TempDir dir;
  const auto user = dir / "user.yml";
  const auto repo = dir / "repo.yml";
  util::write_file(user, "setup-steps:\n  - id: __run\n    run: echo explicit\n  - run: echo user\n");
  util::write_file(repo, "setup-steps:\n  - run: echo repository\n  - run: echo last\n");
  auto merged = load_configs({user, repo});
  auto copy = merged;
  merged = {};
  IUTEST_ASSERT_EQ(2u, copy.setup_steps.size());
  const auto& first = copy.setup_steps[0];
  const auto& second = copy.setup_steps[1];
  IUTEST_EXPECT_EQ(user.string(), first.document->file);
  IUTEST_EXPECT_EQ(repo.string(), second.document->file);
  IUTEST_ASSERT_EQ(2u, first.steps.size());
  IUTEST_ASSERT_EQ(2u, second.steps.size());
  IUTEST_EXPECT_EQ("__run", first.steps[0].id);
  IUTEST_EXPECT_EQ("__run_2", first.steps[1].id);
  IUTEST_EXPECT_EQ("__run_3", second.steps[0].id);
  IUTEST_EXPECT_EQ("__run_4", second.steps[1].id);
  IUTEST_EXPECT_EQ(2, second.steps[0].index);
  IUTEST_EXPECT_EQ("echo repository", second.steps[0].run->text);
  util::write_file(repo, "setup-steps:\n  - id: __run\n    run: echo duplicate\n");
  IUTEST_EXPECT_THROW(load_configs({user, repo}), ConfigError);
}

IUTEST(ConfigTest, ExplicitConfigurationBypassesAutomaticDiscovery) {
  cpa_test::TempDir dir;
  fs::create_directories(dir / ".cpp-actions.yml");
  const auto explicit_path = dir / "explicit.yml";
  util::write_file(explicit_path, "vars: {EXPLICIT: selected}\n");
  const auto paths = configuration_paths(dir.path(), explicit_path);
  IUTEST_EXPECT_EQ((std::vector<fs::path>{explicit_path}), paths);
  IUTEST_EXPECT_EQ("selected", load_configs(paths).vars.at("EXPLICIT"));
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
  IUTEST_ASSERT_EQ(1u, copy.setup_steps.size());
  IUTEST_ASSERT_TRUE(copy.setup_steps[0].document != nullptr);
  const auto& steps = copy.setup_steps[0].steps;
  IUTEST_ASSERT_EQ(2u, steps.size());
  IUTEST_EXPECT_EQ("prepare", steps[0].id);
  IUTEST_EXPECT_EQ("Prepare local environment", steps[0].name->text);
  IUTEST_EXPECT_EQ("echo \"${{ env.VALUE }}\"", steps[0].run->text);
  IUTEST_EXPECT_EQ("sh -e {0}", steps[1].shell->text);
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

IUTEST(ConfigTest, XcodeRequiresBooleanOptIn) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", "{}\n");
  IUTEST_EXPECT_FALSE(load_config(dir / "config.yml").allow_xcode.has_value());
  for (const bool enabled : {false, true}) {
    util::write_file(dir / "config.yml", std::string("allow-xcode: ") + (enabled ? "true\n" : "false\n"));
    const auto config = load_config(dir / "config.yml");
    IUTEST_ASSERT_TRUE(config.allow_xcode.has_value());
    IUTEST_EXPECT_EQ(enabled, *config.allow_xcode);
  }

  for (const auto* value : {"'true'", "1", "[]", "{}", "null"}) {
    util::write_file(dir / "config.yml", std::string("allow-xcode: ") + value + "\n");
    IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError);
  }
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

IUTEST(ConfigTest, XcodeSimulatorJobPermissionList) {
  cpa_test::TempDir dir;
  util::write_file(dir / "config.yml", "{}\n");
  IUTEST_EXPECT_TRUE(load_config(dir / "config.yml").allow_xcode_simulator_job.empty());
  util::write_file(dir / "config.yml",
                   "allow-xcode-simulator-job: ['.github/workflows/build.yml::ios', "
                   "'.github/workflows/other.yaml::simulator']\n");
  const auto config = load_config(dir / "config.yml");
  IUTEST_EXPECT_EQ((std::vector<std::string>{".github/workflows/build.yml::ios",
                                            ".github/workflows/other.yaml::simulator"}),
                   config.allow_xcode_simulator_job);
  util::write_file(dir / "config.yml", "allow-xcode-simulator-job: {bad: mapping}\n");
  IUTEST_EXPECT_THROW(load_config(dir / "config.yml"), ConfigError);
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
