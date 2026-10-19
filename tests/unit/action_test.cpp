#include "iutest.hpp"

#include "actions/action.hpp"
#include "actions/fetch.hpp"
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

IUTEST(ActionTest, InvalidMetadata) {
  cpa_test::TempDir dir;
  for (const auto* text : {
           "[]", "runs: []", "runs: {using: node20}", "runs: {using: unknown, main: main.js}",
           "runs: {using: node20, main: [main.js]}", "runs: {using: [node20], main: main.js}",
           "inputs: {x: value}\nruns: {using: node20, main: main.js}",
           "inputs: {x: {required: maybe}}\nruns: {using: node20, main: main.js}",
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
