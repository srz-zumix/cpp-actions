#include "iutest.hpp"

#include "workflow/events.hpp"
#include "workflow/parser.hpp"

using namespace cpa::workflow;

namespace {

Workflow parse(const std::string& on) {
  const std::string text = "on:\n" + on + "\njobs: {a: {runs-on: x, steps: [{run: a}]}}\n";
  return parse_workflow(cpa::yaml::load(text, "wf.yml"), "wf.yml");
}

EventInfo push(const std::string& ref, std::optional<std::vector<std::string>> files = std::nullopt) {
  EventInfo e;
  e.name = "push";
  e.ref = ref;
  e.changed_files = std::move(files);
  return e;
}

}  // namespace

IUTEST(EventsTest, NotTriggered) {
  const auto wf = parse("  pull_request:");
  IUTEST_EXPECT_FALSE(match_trigger(wf, push("refs/heads/main")).matched);
}

IUTEST(EventsTest, PushBranches) {
  const auto wf = parse("  push:\n    branches: [main, 'releases/**', '!releases/**-alpha']");
  IUTEST_EXPECT_TRUE(match_trigger(wf, push("refs/heads/main")).matched);
  IUTEST_EXPECT_TRUE(match_trigger(wf, push("refs/heads/releases/v1")).matched);
  IUTEST_EXPECT_FALSE(match_trigger(wf, push("refs/heads/releases/v1-alpha")).matched);
  IUTEST_EXPECT_FALSE(match_trigger(wf, push("refs/heads/dev")).matched);
  // Only branch filters: tag pushes do not trigger.
  IUTEST_EXPECT_FALSE(match_trigger(wf, push("refs/tags/v1")).matched);
}

IUTEST(EventsTest, PushTagsAndIgnore) {
  const auto wf = parse("  push:\n    tags: ['v*']\n    branches-ignore: [wip]");
  IUTEST_EXPECT_TRUE(match_trigger(wf, push("refs/tags/v1.0")).matched);
  IUTEST_EXPECT_FALSE(match_trigger(wf, push("refs/tags/x")).matched);
  IUTEST_EXPECT_TRUE(match_trigger(wf, push("refs/heads/main")).matched);
  IUTEST_EXPECT_FALSE(match_trigger(wf, push("refs/heads/wip")).matched);
}

IUTEST(EventsTest, Paths) {
  const auto wf = parse("  push:\n    paths: ['src/**', '!src/docs/**']");
  IUTEST_EXPECT_TRUE(match_trigger(wf, push("refs/heads/main", std::vector<std::string>{"src/a.cpp"})).matched);
  IUTEST_EXPECT_FALSE(match_trigger(wf, push("refs/heads/main", std::vector<std::string>{"src/docs/a.md"})).matched);
  IUTEST_EXPECT_FALSE(match_trigger(wf, push("refs/heads/main", std::vector<std::string>{"README.md"})).matched);
  // Unknown changed files: the filter is not evaluated.
  IUTEST_EXPECT_TRUE(match_trigger(wf, push("refs/heads/main")).matched);

  const auto ignore = parse("  push:\n    paths-ignore: ['**.md']");
  IUTEST_EXPECT_FALSE(match_trigger(ignore, push("refs/heads/main", std::vector<std::string>{"a.md", "b/c.md"})).matched);
  IUTEST_EXPECT_TRUE(match_trigger(ignore, push("refs/heads/main", std::vector<std::string>{"a.md", "b.cpp"})).matched);
}

IUTEST(EventsTest, PullRequest) {
  const auto wf = parse("  pull_request:\n    branches: [main]");
  EventInfo e;
  e.name = "pull_request";
  e.base_ref = "main";
  e.action = "opened";
  IUTEST_EXPECT_TRUE(match_trigger(wf, e).matched);
  e.action = "closed";
  IUTEST_EXPECT_FALSE(match_trigger(wf, e).matched);
  e.action = "synchronize";
  e.base_ref = "dev";
  IUTEST_EXPECT_FALSE(match_trigger(wf, e).matched);

  const auto typed = parse("  pull_request:\n    types: [closed]");
  e.action = "closed";
  IUTEST_EXPECT_TRUE(match_trigger(typed, e).matched);
}

IUTEST(EventsTest, OtherEvents) {
  const auto wf = parse("  issues:\n    types: [opened]\n  workflow_dispatch:");
  EventInfo e;
  e.name = "issues";
  e.action = "opened";
  IUTEST_EXPECT_TRUE(match_trigger(wf, e).matched);
  e.action = "edited";
  IUTEST_EXPECT_FALSE(match_trigger(wf, e).matched);
  e.name = "workflow_dispatch";
  e.action.clear();
  IUTEST_EXPECT_TRUE(match_trigger(wf, e).matched);
}
