#include "iutest.hpp"

#include "engine/event.hpp"
#include "test_support.hpp"
#include "workflow/parser.hpp"

using namespace cpa;
using namespace cpa::engine;

namespace {

workflow::Workflow parse(const std::string& text) {
  return workflow::parse_workflow(yaml::load(text, "wf.yml"), "wf.yml");
}

workspace::GitInfo make_repo(const cpa_test::TempDir& dir) {
  cpa_test::init_repo(dir.path(), {{"a.txt", "a\n"}, {"src/b.cpp", "b\n"}});
  cpa_test::git(dir.path(), {"remote", "add", "origin", "https://github.com/octo/hello.git"});
  return workspace::read_git_info(dir.path());
}

}  // namespace

IUTEST(EventTest, Refs) {
  IUTEST_EXPECT_EQ("main", ref_short_name("refs/heads/main"));
  IUTEST_EXPECT_EQ("v1.0", ref_short_name("refs/tags/v1.0"));
  IUTEST_EXPECT_EQ("1/merge", ref_short_name("refs/pull/1/merge"));
  IUTEST_EXPECT_EQ("branch", ref_type("refs/heads/a/b"));
  IUTEST_EXPECT_EQ("tag", ref_type("refs/tags/v1"));
  IUTEST_EXPECT_EQ("", ref_type("refs/pull/1/merge"));
  IUTEST_EXPECT_EQ("refs/heads/feature/x", normalize_ref("feature/x"));
  IUTEST_EXPECT_EQ("refs/tags/v1", normalize_ref("refs/tags/v1"));
}

IUTEST(EventTest, UnknownEvent) {
  workspace::GitInfo git;
  EventOptions o;
  o.name = "no_such_event";
  IUTEST_EXPECT_THROW(build_event(o, git), EventError);
}

IUTEST(EventTest, Push) {
  cpa_test::TempDir dir;
  const auto git = make_repo(dir);
  EventOptions o;
  o.actor = "octocat";
  auto ev = build_event(o, git);
  IUTEST_EXPECT_EQ("push", ev.info.name);
  IUTEST_EXPECT_EQ("refs/heads/main", ev.info.ref);
  IUTEST_EXPECT_EQ(git.head_sha, ev.sha);
  IUTEST_EXPECT_EQ("octocat", ev.actor);
  IUTEST_EXPECT_EQ("refs/heads/main", ev.info.payload.get("ref").as_string());
  IUTEST_EXPECT_EQ(git.head_sha, ev.info.payload.get("after").as_string());
  IUTEST_EXPECT_EQ("octo/hello", ev.info.payload.get("repository").get("full_name").as_string());
  IUTEST_EXPECT_EQ("octocat", ev.info.payload.get("sender").get("login").as_string());
  // No pending changes: the files of the HEAD commit.
  IUTEST_ASSERT_TRUE(ev.info.changed_files.has_value());
  IUTEST_EXPECT_EQ(2u, ev.info.changed_files->size());

  // Uncommitted changes are the changed files.
  util::write_file(dir / "src/b.cpp", "changed\n");
  ev = build_event(o, git);
  IUTEST_ASSERT_TRUE(ev.info.changed_files.has_value());
  IUTEST_ASSERT_EQ(1u, ev.info.changed_files->size());
  IUTEST_EXPECT_EQ("src/b.cpp", (*ev.info.changed_files)[0]);

  o.ref = "release/v1";
  o.changes_base = "HEAD";
  ev = build_event(o, git);
  IUTEST_EXPECT_EQ("refs/heads/release/v1", ev.info.ref);
  o.changes_base = "no-such-revision";
  IUTEST_EXPECT_THROW(build_event(o, git), EventError);
}

IUTEST(EventTest, PullRequest) {
  cpa_test::TempDir dir;
  const auto git = make_repo(dir);
  cpa_test::git(dir.path(), {"checkout", "-q", "-b", "feature"});
  util::write_file(dir / "c.txt", "c\n");
  cpa_test::git(dir.path(), {"add", "c.txt"});
  cpa_test::git(dir.path(), {"commit", "-q", "-m", "add c"});
  const auto feature = workspace::read_git_info(dir.path());

  EventOptions o;
  o.name = "pull_request";
  const auto ev = build_event(o, feature);
  IUTEST_EXPECT_EQ("refs/pull/1/merge", ev.info.ref);
  IUTEST_EXPECT_EQ("main", ev.info.base_ref);
  IUTEST_EXPECT_EQ("feature", ev.head_ref);
  IUTEST_EXPECT_EQ("opened", ev.info.action);
  IUTEST_EXPECT_EQ("opened", ev.info.payload.get("action").as_string());
  IUTEST_EXPECT_EQ("main", ev.info.payload.get("pull_request").get("base").get("ref").as_string());
  IUTEST_EXPECT_EQ("feature", ev.info.payload.get("pull_request").get("head").get("ref").as_string());
  // Changes since the merge base with main.
  IUTEST_ASSERT_TRUE(ev.info.changed_files.has_value());
  IUTEST_ASSERT_EQ(1u, ev.info.changed_files->size());
  IUTEST_EXPECT_EQ("c.txt", (*ev.info.changed_files)[0]);

  o.name = "pull_request_target";
  o.activity_type = "synchronize";
  const auto target = build_event(o, feature);
  IUTEST_EXPECT_EQ("refs/heads/main", target.info.ref);
  IUTEST_EXPECT_EQ("synchronize", target.info.action);
}

IUTEST(EventTest, PayloadFile) {
  cpa_test::TempDir dir;
  util::write_file(dir / "event.json", R"({"pull_request": {"number": 42, "base": {"ref": "dev"}}})");
  workspace::GitInfo git;
  EventOptions o;
  o.name = "pull_request";
  o.payload_file = dir / "event.json";
  const auto ev = build_event(o, git);
  IUTEST_EXPECT_EQ("refs/pull/42/merge", ev.info.ref);
  IUTEST_EXPECT_EQ("dev", ev.info.base_ref);
  IUTEST_EXPECT_FALSE(ev.info.changed_files.has_value());

  util::write_file(dir / "bad.json", "[1]");
  o.payload_file = dir / "bad.json";
  IUTEST_EXPECT_THROW(build_event(o, git), EventError);
}

IUTEST(EventTest, DispatchInputs) {
  const auto wf = parse(R"(on:
  workflow_dispatch:
    inputs:
      name: {required: true}
      debug: {type: boolean, default: false}
      level: {type: choice, options: [low, high], default: low}
      count: {type: number}
jobs: {a: {runs-on: x, steps: [{run: a}]}}
)");
  auto in = dispatch_inputs(wf, {{"name", "x"}, {"DEBUG", "true"}, {"count", "3"}});
  IUTEST_EXPECT_EQ("x", in.get("name").as_string());
  IUTEST_EXPECT_TRUE(in.get("debug").is_bool());
  IUTEST_EXPECT_TRUE(in.get("debug").as_bool());
  IUTEST_EXPECT_EQ("low", in.get("level").as_string());
  IUTEST_EXPECT_EQ(3.0, in.get("count").as_number());
  const auto ev = dispatch_event_inputs(in);
  IUTEST_EXPECT_EQ("true", ev.get("debug").as_string());
  IUTEST_EXPECT_EQ("3", ev.get("count").as_string());

  IUTEST_EXPECT_THROW(dispatch_inputs(wf, {}), EventError);
  IUTEST_EXPECT_THROW(dispatch_inputs(wf, {{"name", "x"}, {"other", "1"}}), EventError);
  IUTEST_EXPECT_THROW(dispatch_inputs(wf, {{"name", "x"}, {"debug", "yes"}}), EventError);
  IUTEST_EXPECT_THROW(dispatch_inputs(wf, {{"name", "x"}, {"level", "mid"}}), EventError);
  IUTEST_EXPECT_THROW(dispatch_inputs(wf, {{"name", "x"}, {"count", "abc"}}), EventError);
}
