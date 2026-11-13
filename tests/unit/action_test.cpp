#include "iutest.hpp"

#include <sstream>

#include "actions/action.hpp"
#include "actions/fetch.hpp"
#include "engine/runner.hpp"
#include "test_support.hpp"
#include "util/fs.hpp"

using namespace cpa;
using namespace cpa::actions;

IUTEST(ActionTest, References) {
  const auto local = parse_action_ref("./actions/example");
  IUTEST_EXPECT_EQ(ActionRef::Kind::Local, local.kind);
  IUTEST_EXPECT_EQ("./actions/example", local.display());
  const auto remote = parse_action_ref("owner/repo/path/to/action@feature/branch");
  IUTEST_EXPECT_EQ("owner", remote.owner);
  IUTEST_EXPECT_EQ("repo", remote.repo);
  IUTEST_EXPECT_EQ("path/to/action", remote.path);
  IUTEST_EXPECT_EQ("feature/branch", remote.ref);
  IUTEST_EXPECT_EQ("owner/repo/path/to/action@feature/branch", remote.display());
  IUTEST_EXPECT_EQ(ActionRef::Kind::Docker, parse_action_ref("docker://alpine:3").kind);
  for (const auto* bad : {"owner/repo", "owner/repo@", "owner/repo/../escape@v1", "../repo@v1",
                          "owner/repo@-option", "owner/repo@x\n", "owner/repo@x..y"}) {
    IUTEST_EXPECT_THROW(parse_action_ref(bad), ActionError);
  }
}

IUTEST(ActionTest, SetupPythonHostCacheMatchesOnlyTheOfficialRootAction) {
  for (const auto* ref :
       {"actions/setup-python@v7", "Actions/Setup-Python@v6", "actions/setup-python@feature/branch"}) {
    IUTEST_EXPECT_TRUE(is_setup_python_root(parse_action_ref(ref))) << ref;
  }
  for (const auto* ref : {"./setup-python", "example/setup-python@v7", "actions/setup-python-extra@v7",
                          "actions/setup-python/subdir@v7", "docker://actions/setup-python"}) {
    IUTEST_EXPECT_FALSE(is_setup_python_root(parse_action_ref(ref))) << ref;
  }
}

IUTEST(ActionTest, JavaScriptMetadata) {
  cpa_test::TempDir dir;
  util::write_file(dir / "action.yaml", R"(name: Example
inputs:
  greeting:
    description: Message
    required: true
    default: hello
outputs:
  result:
    description: Result
runs:
  using: node24
  pre: pre.js
  pre-if: runner.os == 'macOS'
  main: main.js
  post: post.js
  post-if: always()
)");
  const auto action = load_action(dir.path(), "./example");
  IUTEST_EXPECT_EQ(ActionKind::Node, action.kind);
  IUTEST_EXPECT_EQ("main.js", action.main);
  IUTEST_EXPECT_EQ("pre.js", action.pre);
  IUTEST_EXPECT_EQ("post.js", action.post);
  IUTEST_EXPECT_EQ(24, node_major_version(action.runtime));
  IUTEST_ASSERT_NE(nullptr, action.find_input("GREETING"));
  IUTEST_EXPECT_TRUE(action.find_input("greeting")->required);
  IUTEST_EXPECT_EQ("hello", action.find_input("greeting")->default_value->scalar_string());
  IUTEST_ASSERT_NE(nullptr, action.post_if);
  IUTEST_EXPECT_EQ("always()", action.post_if->scalar_string());
  IUTEST_EXPECT_EQ("./example/action.yaml", action.file);
}

IUTEST(ActionTest, ActionPermissionSelectorsValidateRefWildcardsOnly) {
  for (const auto* selector : {"./local", "./local/", "./local/./", ".", "owner/repo@v1",
                               "owner/repo/subpath@feature/branch", "./name..suffix", "owner/repo@*",
                               "owner/repo@v*", "owner/repo/subpath@feature/*"}) {
    IUTEST_EXPECT_NO_THROW(validate_action_permission_selector(selector)) << selector;
  }
  for (const auto* selector : {"", "*", "./../escape", "./local/../other", "./local\n", "./local/*",
                               "./local@*", "owner/repo", "owner/*@v1", "owner/repo/*@*",
                               "owner/repo@v?", "owner/repo@v[12]",
                               "owner/repo@${{ inputs.ref }}", "docker://alpine:3"}) {
    IUTEST_EXPECT_THROW(validate_action_permission_selector(selector), ActionError) << selector;
  }
}

IUTEST(ActionTest, ActionPermissionSelectorsMatchRefsAndPreserveIdentity) {
  const struct {
    const char* selector;
    const char* uses;
    bool matches;
  } cases[] = {
      {"owner/repo@v1", "owner/repo@v1", true},
      {"owner/repo@v1", "owner/repo@v1.0", false},
      {"owner/repo@*", "owner/repo@v2", true},
      {"owner/repo@*", "owner/repo@feature/nested/branch", true},
      {"owner/repo@*", "owner/repo@0123456789abcdef0123456789abcdef01234567", true},
      {"owner/repo@v*", "owner/repo@v", true},
      {"owner/repo@v*", "owner/repo@v2.1", true},
      {"owner/repo@v*", "owner/repo@main", false},
      {"owner/repo@v1.*", "owner/repo@v1.2", true},
      {"owner/repo@v1.*", "owner/repo@v1x2", false},
      {"owner/repo@v+*", "owner/repo@v+2", true},
      {"owner/repo@v+*", "owner/repo@vv2", false},
      {"owner/repo@a**b*c", "owner/repo@ab-c", true},
      {"owner/repo@a*b*c", "owner/repo@ab-d", false},
      {"owner/repo@release/*", "owner/repo@release/nested/branch", true},
      {"owner/repo@*", "owner/other@v1", false},
      {"owner/repo@*", "other/repo@v1", false},
      {"owner/repo@*", "Owner/repo@v1", false},
      {"owner/repo@*", "owner/repo/subpath@v1", false},
      {"owner/repo/subpath@*", "owner/repo/subpath/nested@v1", false},
      {"owner/repo@*", "./local", false},
      {"owner/repo@*", "docker://alpine:3", false},
      {"./local", "./local", true},
      {"./local", "./local/", false},
      {"./local/./", "./local", false},
  };
  for (const auto& test : cases) {
    IUTEST_EXPECT_EQ(test.matches, matches_action_permission(test.selector, parse_action_ref(test.uses)))
        << test.selector << " vs " << test.uses;
  }
  IUTEST_EXPECT_THROW(matches_action_permission("owner/*@*", parse_action_ref("owner/repo@v1")), ActionError);
  IUTEST_EXPECT_THROW(matches_action_permission("./local/*", parse_action_ref("./local/action")), ActionError);
}

IUTEST(ActionTest, InvalidPermissionFailsBeforeCreatingRunDirectory) {
  for (int permission = 0; permission < 5; ++permission) {
    cpa_test::TempDir dir;
    engine::RunOptions options;
    options.sandbox.enabled = false;
    switch (permission) {
      case 0: options.sandbox.allow_apple_security_for = {"owner/*@v1"}; break;
      case 1: options.sandbox.allow_docker_for = {"owner/*@v1"}; break;
      case 2: options.sandbox.allow_msbuild_for = {"owner/*@v1"}; break;
      case 3: options.sandbox.allow_unity_for = {"owner/*@v1"}; break;
      case 4: options.sandbox.allow_xcode_for = {"owner/*@v1"}; break;
    }
    options.run_dir = dir / "run";
    options.cache_dir = dir / "cache";
    std::ostringstream out;
    engine::Console console(out, false);
    engine::Runner runner(options, console);
    IUTEST_EXPECT_THROW(runner.run({}, {}, {}), ActionError) << permission;
    IUTEST_EXPECT_FALSE(std::filesystem::exists(options.run_dir)) << permission;
  }
}

IUTEST(ActionTest, CompositeMetadata) {
  cpa_test::TempDir dir;
  util::write_file(dir / "action.yml", R"(runs:
  using: composite
  steps:
    - id: inner
      shell: bash
      run: echo "value=ok" >> "$GITHUB_OUTPUT"
outputs:
  result:
    value: ${{ steps.inner.outputs.value }}
)");
  const auto action = load_action(dir.path(), "./example");
  IUTEST_EXPECT_EQ(ActionKind::Composite, action.kind);
  IUTEST_ASSERT_EQ(1u, action.steps.size());
  IUTEST_EXPECT_EQ("inner", action.steps.front().id);
  IUTEST_ASSERT_EQ(1u, action.outputs.size());
  IUTEST_EXPECT_EQ("${{ steps.inner.outputs.value }}", action.outputs.front().value->scalar_string());
}

IUTEST(ActionTest, QuotedRequiredMetadataRemainsCompatible) {
  cpa_test::TempDir dir;
  util::write_file(dir / "action.yml", R"(inputs:
  yes:
    required: 'true'
  no:
    required: 'false'
runs:
  using: node20
  main: main.js
)");
  const auto action = load_action(dir.path(), "./quoted");
  IUTEST_ASSERT_NE(nullptr, action.find_input("yes"));
  IUTEST_ASSERT_NE(nullptr, action.find_input("no"));
  IUTEST_EXPECT_TRUE(action.find_input("yes")->required);
  IUTEST_EXPECT_FALSE(action.find_input("no")->required);
}

IUTEST(ActionTest, InvalidMetadata) {
  cpa_test::TempDir dir;
  for (const auto* text : {
           "[]", "runs: []", "runs: {using: node20}", "runs: {using: unknown, main: main.js}",
           "runs: {using: node20, main: [main.js]}", "runs: {using: [node20], main: main.js}",
           "inputs: {x: value}\nruns: {using: node20, main: main.js}",
           "inputs: {x: {default: [value]}}\nruns: {using: node20, main: main.js}",
           "inputs: {x: {}, X: {}}\nruns: {using: node20, main: main.js}",
           "outputs: {x: value}\nruns: {using: node20, main: main.js}",
           "outputs: {x: {}}\nruns: {using: composite, steps: []}",
           "runs: {using: composite, steps: [{run: echo hi}]}",
           "runs: {using: node20, main: main.js, post-if: [always()]}",
       }) {
    util::write_file(dir / "action.yml", text);
    IUTEST_EXPECT_THROW(load_action(dir.path(), "./invalid"), ActionError);
  }
  util::write_file(dir / "action.yml", "runs: {using: docker, image: Dockerfile}");
  IUTEST_EXPECT_EQ(ActionKind::Docker, load_action(dir.path(), "./docker").kind);
}

IUTEST(ActionTest, MissingMetadata) {
  cpa_test::TempDir dir;
  IUTEST_EXPECT_THROW(load_action(dir.path(), "./missing"), ActionError);
  util::write_file(dir / "Dockerfile", "FROM scratch\n");
  IUTEST_EXPECT_THROW(load_action(dir.path(), "./docker"), ActionError);
}

IUTEST(ActionFetchTest, CommitResolution) {
  const std::string branch(40, 'a');
  const std::string tag(40, 'b');
  const std::string peeled(40, 'c');
  const auto refs = branch + "\trefs/heads/v1\n" + tag + "\trefs/tags/v1\n" + peeled + "\trefs/tags/v1^{}\n";
  IUTEST_EXPECT_EQ(peeled, pick_ls_remote_sha(refs, "v1").value_or(""));
  IUTEST_EXPECT_EQ(peeled, pick_ls_remote_sha(refs, "refs/tags/v1").value_or(""));
  IUTEST_EXPECT_EQ(branch, pick_ls_remote_sha(refs, "refs/heads/v1").value_or(""));
  IUTEST_EXPECT_FALSE(pick_ls_remote_sha(refs, "missing").has_value());
  IUTEST_EXPECT_FALSE(pick_ls_remote_sha("invalid\trefs/tags/v1\n", "v1").has_value());
  IUTEST_EXPECT_TRUE(is_commit_sha(branch));
  IUTEST_EXPECT_FALSE(is_commit_sha(std::string(40, 'g')));
  IUTEST_EXPECT_FALSE(is_commit_sha("abc"));
}

IUTEST(ActionFetchTest, CachedCommit) {
  cpa_test::TempDir dir;
  const std::string sha(40, 'a');
  util::write_file(dir / ("owner/repo/" + sha + "/.cpp-actions-complete"), sha + "\n");
  RepositoryCache cache(dir.path(), "");
  const auto entry = cache.fetch("owner", "repo", std::string(40, 'A'), {});
  IUTEST_EXPECT_EQ(sha, entry.sha);
  IUTEST_EXPECT_EQ(dir / ("owner/repo/" + sha), entry.dir);
}

IUTEST(ActionFetchTest, PrivateCurlConfiguration) {
  cpa_test::TempDir dir;
  const auto path = write_curl_auth_config(dir.path(), "example\"\\value");
  const std::string prefix = "header = \"Authorization: ";
  IUTEST_EXPECT_EQ(prefix + "Bearer " + "example\\\"\\\\value\"\n", util::read_file(path));
  const auto permissions = std::filesystem::status(path).permissions();
  IUTEST_EXPECT_EQ(std::filesystem::perms::none,
                   permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all));
  IUTEST_EXPECT_THROW(write_curl_auth_config(dir.path(), "again"), std::system_error);
  IUTEST_EXPECT_THROW(write_curl_auth_config(dir.path(), "value\noutput=elsewhere"), ActionError);
}

IUTEST(ActionFetchTest, GitHelpersUseRunnerConfigAndExplicitToken) {
  cpa_test::TempDir dir;
  IUTEST_EXPECT_EQ("https://github.com/owner/repo\n",
                   run_tool({"git", "ls-remote", "--get-url", "https://github.com/owner/repo"}, dir.path()));
  IUTEST_EXPECT_EQ("*\n", run_tool({"git", "config", "--global", "--get", "safe.directory"}, dir.path()));
  IUTEST_EXPECT_EQ("git-lfs filter-process\n",
                   run_tool({"git", "config", "--global", "--get", "filter.lfs.process"}, dir.path()));
  const auto token_env = git_token_env("test-token");
  IUTEST_EXPECT_EQ("AUTHORIZATION: basic eC1hY2Nlc3MtdG9rZW46dGVzdC10b2tlbg==",
                   token_env.at("GIT_CONFIG_VALUE_0"));
  IUTEST_EXPECT_EQ(token_env.at("GIT_CONFIG_VALUE_0") + "\n",
                   run_tool({"git", "config", "--get", "http.https://github.com/.extraheader"},
                            dir.path(), token_env));
}

IUTEST(ActionFetchTest, SubmoduleSshMappingIsProcessScopedAndLimitedToGitHub) {
  cpa_test::TempDir dir;
  for (const auto& token : {std::string(), std::string("test-token")}) {
    const auto env = git_submodule_env(token);
    for (const auto* url : {"git@github.com:owner/repo.git", "ssh://git@github.com/owner/repo.git"}) {
      IUTEST_EXPECT_EQ("https://github.com/owner/repo.git\n",
                       run_tool({"git", "ls-remote", "--get-url", url}, dir.path(), env));
      IUTEST_EXPECT_EQ(std::string(url) + "\n",
                       run_tool({"git", "ls-remote", "--get-url", url}, dir.path()));
    }
    for (const auto* url : {"git@github.enterprise.example.test:fixture-owner/repo.git",
                            "ssh://git@github.enterprise.example.test/fixture-owner/repo.git",
                            "git@github.com.evil:owner/repo.git", "ssh://git@github.com.evil/owner/repo.git",
                            "https://github.com/owner/repo.git", "git@other.example:owner/repo.git"}) {
      IUTEST_EXPECT_EQ(std::string(url) + "\n",
                       run_tool({"git", "ls-remote", "--get-url", url}, dir.path(), env));
    }
    if (!token.empty()) {
      IUTEST_EXPECT_EQ(git_token_env(token).at("GIT_CONFIG_VALUE_0") + "\n",
                       run_tool({"git", "config", "--get", "http.https://github.com/.extraheader"},
                                dir.path(), env));
    }
  }
}

IUTEST(ActionFetchTest, ExplicitNode) {
  cpa_test::TempDir dir;
  util::write_file(dir / "node", "#!/bin/sh\nexit 0\n");
  NodeProvider provider(dir / "node", dir / "cache");
  IUTEST_EXPECT_THROW(provider.get(20, {}), ActionError);
  util::make_executable(dir / "node");
  IUTEST_EXPECT_EQ(util::real_path(dir / "node"), provider.get(20, {}));
  NodeProvider missing(dir / "missing", dir / "cache");
  IUTEST_EXPECT_THROW(missing.get(24, {}), ActionError);
}

IUTEST(ActionTest, ExplicitRuntimeDoesNotExposeParentDirectory) {
  if (!engine::host_is_macos()) IUTEST_SKIP() << "Seatbelt isolation requires macOS";
  struct ProtectedFixture {
    const std::filesystem::path root = util::make_temp_dir("cpa-runtime-test-", util::home_dir());
    ~ProtectedFixture() { util::remove_tree(root); }
    std::filesystem::path operator/(const char* path) const { return root / path; }
  } dir;
  const auto node = dir / "private-runtime/runtime";
  util::write_file(node, "#!/bin/sh\nexec /bin/sh \"$@\"\n");
  util::make_executable(node);
  util::write_file(dir / "private-runtime/marker", "host-only");
  std::filesystem::create_directories(dir / "repo");
  cpa_test::init_repo(dir / "repo", {
      {".github/workflows/test.yml", R"(on: push
jobs:
  test:
    runs-on: runtime-test-host
    steps:
      - uses: actions/checkout@v4
      - uses: ./action
        env:
          MARKER: )" + (dir / "private-runtime/marker").string() + "\n"},
      {"action/action.yml", "runs: {using: node20, main: main.js}\n"},
      {"action/main.js", R"(if cat "$MARKER"; then exit 1; fi
echo isolated >> "$GITHUB_STEP_SUMMARY"
)"}});
  const auto git = workspace::read_git_info(dir / "repo");
  const auto event = engine::build_event({}, git);
  const auto files = engine::load_workflow_files(git.root, {});
  engine::RunOptions options;
  options.node = node;
  options.cache_dir = dir / "cache";
  options.run_dir = dir / "run";
  options.host_labels = {"runtime-test-host"};
  std::ostringstream log;
  engine::Console console(log, false);
  engine::Runner runner(options, console);
  const auto report = runner.run(engine::select_workflows(files, event.info, {}), event, git);
  IUTEST_EXPECT_EQ("success", report.result) << log.str();
  IUTEST_ASSERT_EQ(1u, report.workflows.size());
  IUTEST_ASSERT_EQ(1u, report.workflows.front().jobs.size());
  IUTEST_EXPECT_EQ("isolated\n", report.workflows.front().jobs.front().summary);
}
