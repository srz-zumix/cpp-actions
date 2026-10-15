#include "iutest.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <sstream>
#include <system_error>
#include <thread>

#include "engine/evaluate.hpp"
#include "engine/job.hpp"
#include "engine/runner.hpp"
#include "engine/shell.hpp"
#include "test_support.hpp"
#include "util/sha256.hpp"
#include "workflow/parser.hpp"
#include "workflow/scopes.hpp"

using namespace cpa;
using namespace cpa::engine;

namespace {

class EngineRun {
 public:
  EngineRun() : console(output, false) {
    options.run_dir = dir / "run";
    options.cache_dir = dir / "cache";
    options.host_labels = {"test-host"};
    options.sandbox.enabled = false;
    options.parallel = 4;
    git.root = dir / "repository";
    fs::create_directories(git.root);
    git.github = workspace::GitHubRepo{"octo", "hello"};
    git.head_sha = std::string(40, 'a');
    event.info.name = "push";
    event.info.ref = "refs/heads/main";
    event.sha = git.head_sha;
    event.actor = "octocat";
    event.info.payload = expr::Value::parse_json(R"({"custom":"payload"})");
  }

  void load(const std::string& text) {
    file.display = ".github/workflows/test.yml";
    file.path = git.root / file.display;
    file.workflow = std::make_shared<workflow::Workflow>(
        workflow::parse_workflow(yaml::load(text, file.display), file.display));
  }

  std::vector<Selection> selection() const { return {{&file, {true, "test"}}}; }

  RunReport run(const std::string& text) {
    load(text);
    Runner runner(options, console);
    return runner.run(selection(), event, git);
  }

  cpa_test::TempDir dir;
  RunOptions options;
  workspace::GitInfo git;
  Event event;
  std::ostringstream output;
  Console console;
  WorkflowFile file;
};

const JobReport& job(const RunReport& report, std::string_view id) {
  for (const auto& workflow : report.workflows) {
    for (const auto& item : workflow.jobs) {
      if (item.job_id == id) return item;
    }
  }
  throw std::runtime_error("missing job " + std::string(id));
}

const StepReport& step(const JobReport& report, std::string_view id) {
  const auto found = std::find_if(report.steps.begin(), report.steps.end(),
                                [&](const auto& item) { return item.id == id; });
  if (found == report.steps.end()) throw std::runtime_error("missing step " + std::string(id));
  return *found;
}

bool wait_for_file(const fs::path& path) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fs::exists(path)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

IUTEST(EngineTest, RunContextsEnvironmentAndFileCommands) {
  EngineRun test;
  test.options.vars = {{"GREETING", "hello"}};
  test.options.pass_env = {"PASSED=from-host"};
  const auto report = test.run(R"yaml(
name: Contexts
on: push
env:
  SHARED: workflow
  WORKFLOW_ONLY: workflow-only
jobs:
  run:
    runs-on: test-host
    env:
      SHARED: job
      FROM_VAR: ${{ vars.GREETING }}
    outputs:
      answer: ${{ steps.check.outputs.answer }}
    steps:
      - id: prepare
        env:
          SHARED: step
        run: |
          test "$SHARED" = step
          test "$WORKFLOW_ONLY" = workflow-only
          test "$FROM_VAR" = hello
          test "$PASSED" = from-host
          test "$GITHUB_ACTIONS" = true
          test "$GITHUB_ACTOR" = octocat
          test "$GITHUB_REPOSITORY" = octo/hello
          test "$GITHUB_JOB" = run
          test "$GITHUB_ACTION" = '${{ github.action }}'
          test "$GITHUB_ENV" = '${{ github.env }}'
          test "$GITHUB_PATH" = '${{ github.path }}'
          test "$GITHUB_REF" = refs/heads/main
          test "$GITHUB_SHA" = '${{ github.sha }}'
          test "$GITHUB_WORKSPACE" = '${{ github.workspace }}'
          test "$RUNNER_TEMP" = '${{ runner.temp }}'
          test '${{ github.event.custom }}' = payload
          test '${{ job.status }}' = success
          test -f "$GITHUB_EVENT_PATH"
          printf 'SHARED=persisted\nNEW_VALUE=next\nNODE_OPTIONS=blocked\n' >> "$GITHUB_ENV"
          printf 'multi<<END\nfirst\nsecond\nEND\n' >> "$GITHUB_OUTPUT"
          mkdir -p "$GITHUB_WORKSPACE/bin"
          printf '#!/bin/sh\nprintf path-ok\n' > "$GITHUB_WORKSPACE/bin/custom-tool"
          chmod +x "$GITHUB_WORKSPACE/bin/custom-tool"
          printf '%s\n' "$GITHUB_WORKSPACE/bin" >> "$GITHUB_PATH"
      - id: check
        run: |
          test "$SHARED" = persisted
          test "$NEW_VALUE" = next
          test -z "${NODE_OPTIONS:-}"
          test "$(custom-tool)" = path-ok
          test '${{ steps.prepare.outputs.multi }}' = "$(printf 'first\nsecond')"
          test '${{ steps.prepare.outcome }}' = success
          test '${{ steps.prepare.conclusion }}' = success
          printf 'answer=42\n' >> "$GITHUB_OUTPUT"
)yaml");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  const auto& run = job(report, "run");
  IUTEST_EXPECT_EQ("42", run.outputs.at("answer"));
  IUTEST_EXPECT_EQ("first\nsecond", step(run, "prepare").outputs.at("multi"));
  IUTEST_EXPECT_FALSE(fs::exists(test.options.run_dir / "jobs"));
  IUTEST_EXPECT_TRUE(fs::exists(run.log_file));
  const auto saved = expr::Value::parse_json(util::read_file(test.options.run_dir / "report.json"));
  IUTEST_EXPECT_EQ("success", saved.get("result").to_string());
  IUTEST_EXPECT_EQ("push", saved.get("event").to_string());
}

IUTEST(EngineTest, DispatchInputsAndConfiguredEnvironment) {
  EngineRun test;
  test.event.info.name = "workflow_dispatch";
  test.options.inputs = {{"enabled", "true"}, {"count", "3"}};
  test.options.vars = {{"VALUE", "global"}};
  config::EnvironmentConfig environment;
  environment.vars = {{"VALUE", "environment"}};
  test.options.environments["testing"] = environment;
  const auto report = test.run(R"(
on:
  workflow_dispatch:
    inputs:
      enabled: {type: boolean, default: false}
      count: {type: number, default: 1}
      word: {type: string, default: default-word}
jobs:
  run:
    runs-on: test-host
    environment: {name: testing}
    steps:
      - run: |
          test '${{ inputs.enabled == true }}' = true
          test '${{ inputs.count == 3 }}' = true
          test '${{ inputs.word }}' = default-word
          test '${{ github.event.inputs.enabled }}' = true
          test '${{ vars.VALUE }}' = environment
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
}

IUTEST(EngineTest, WorkflowJobAndStepDefaults) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
defaults:
  run: {shell: bash, working-directory: workflow-dir}
jobs:
  run:
    runs-on: test-host
    defaults:
      run: {working-directory: job-dir}
    steps:
      - run: mkdir -p workflow-dir job-dir step-dir
        working-directory: .
      - run: test "$PWD" = "$GITHUB_WORKSPACE/job-dir"
      - run: test "$PWD" = "$GITHUB_WORKSPACE/step-dir"
        working-directory: step-dir
        shell: sh
  workflow-default:
    runs-on: test-host
    steps:
      - run: mkdir workflow-dir
        working-directory: .
      - run: test "$PWD" = "$GITHUB_WORKSPACE/workflow-dir"
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
}

IUTEST(EngineTest, DefaultBashAndExplicitPipefailAndCustomShell) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: default
        run: false | true
      - id: pipefail
        shell: bash
        continue-on-error: true
        run: false | true
      - id: custom
        shell: sh -e "{0}"
        run: test '${{ steps.pipefail.outcome }}' = failure
      - id: errexit
        continue-on-error: true
        run: |
          false
          exit 0
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  const auto& run = job(report, "run");
  IUTEST_EXPECT_EQ("success", step(run, "default").outcome);
  IUTEST_EXPECT_EQ("failure", step(run, "pipefail").outcome);
  IUTEST_EXPECT_EQ("success", step(run, "pipefail").conclusion);
  IUTEST_EXPECT_EQ("failure", step(run, "errexit").outcome);
}

IUTEST(EngineTest, ConditionsAndFailureStatus) {
  EngineRun test;
  const auto report = test.run(R"yaml(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: disabled
        if: false
        run: exit 99
      - id: failure
        run: exit 7
      - id: implicit
        if: true
        run: exit 99
      - id: on-failure
        if: failure()
        run: |
          test '${{ job.status }}' = failure
          test '${{ steps.failure.outcome }}' = failure
          test '${{ steps.disabled.conclusion }}' = skipped
      - id: always
        if: always()
        run: "true"
      - id: not-cancelled
        if: "!cancelled()"
        run: "true"
)yaml");
  IUTEST_EXPECT_EQ("failure", report.result);
  const auto& run = job(report, "run");
  IUTEST_EXPECT_EQ("skipped", step(run, "disabled").outcome);
  IUTEST_EXPECT_EQ("failure", step(run, "failure").outcome);
  IUTEST_EXPECT_EQ("skipped", step(run, "implicit").outcome);
  IUTEST_EXPECT_EQ("success", step(run, "on-failure").outcome) << test.output.str();
  IUTEST_EXPECT_EQ("success", step(run, "always").outcome);
  IUTEST_EXPECT_EQ("success", step(run, "not-cancelled").outcome);
}

IUTEST(EngineTest, StepContinueOnErrorPreservesOutcome) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: tolerated
        continue-on-error: true
        run: exit 3
      - id: check
        run: |
          test '${{ steps.tolerated.outcome }}' = failure
          test '${{ steps.tolerated.conclusion }}' = success
          test '${{ job.status }}' = success
      - id: failure-only
        if: failure()
        run: exit 99
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ("skipped", step(job(report, "run"), "failure-only").outcome);
}

IUTEST(EngineTest, NeedsOutputsAndAncestorFailure) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  producer:
    runs-on: test-host
    outputs: {value: "${{ steps.emit.outputs.value }}"}
    steps:
      - id: emit
        run: echo value=hello >> "$GITHUB_OUTPUT"
  consumer:
    needs: producer
    runs-on: test-host
    steps:
      - run: |
          test '${{ needs.producer.outputs.value }}' = hello
          test '${{ needs.producer.result }}' = success
  broken:
    needs: consumer
    runs-on: test-host
    steps:
      - run: exit 1
  skipped:
    needs: broken
    runs-on: test-host
    steps:
      - run: exit 99
  recovered:
    needs: skipped
    if: failure()
    runs-on: test-host
    steps:
      - run: test '${{ needs.skipped.result }}' = skipped
)");
  IUTEST_EXPECT_EQ("failure", report.result);
  IUTEST_EXPECT_EQ("success", job(report, "consumer").result) << test.output.str();
  IUTEST_EXPECT_EQ("skipped", job(report, "skipped").result);
  IUTEST_EXPECT_EQ("success", job(report, "recovered").result);
}

IUTEST(EngineTest, JobContinueOnErrorAllowsDependents) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  tolerated:
    runs-on: test-host
    continue-on-error: true
    steps:
      - run: exit 1
  next:
    needs: tolerated
    runs-on: test-host
    steps:
      - run: test '${{ needs.tolerated.result }}' = success
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ("failure", job(report, "tolerated").result);
  IUTEST_EXPECT_TRUE(job(report, "tolerated").continue_on_error);
  IUTEST_EXPECT_EQ("success", job(report, "next").result);
}

IUTEST(EngineTest, SelectedJobsIncludeNeedsUnlessDisabled) {
  for (bool with_needs : {true, false}) {
    EngineRun test;
    test.options.jobs = {"next"};
    test.options.with_needs = with_needs;
    const auto report = test.run(R"(
on: push
jobs:
  first:
    runs-on: test-host
    steps:
      - run: "true"
  next:
    needs: first
    runs-on: test-host
    steps:
      - run: "true"
  unrelated:
    runs-on: test-host
    steps:
      - run: exit 99
)");
    IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
    IUTEST_ASSERT_EQ(1u, report.workflows.size());
    IUTEST_EXPECT_EQ(with_needs ? 2u : 1u, report.workflows[0].jobs.size());
    IUTEST_EXPECT_EQ("success", job(report, "next").result);
  }
}

IUTEST(EngineTest, StepTimeoutAllowsFailureCleanup) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: timeout
        timeout-minutes: 0.001
        run: sleep 10
      - id: skipped
        run: exit 99
      - id: cleanup
        if: failure()
        run: "true"
)");
  IUTEST_EXPECT_EQ("failure", report.result);
  const auto& run = job(report, "run");
  IUTEST_EXPECT_EQ("failure", step(run, "timeout").outcome);
  IUTEST_EXPECT_EQ("The action has timed out.", step(run, "timeout").error);
  IUTEST_EXPECT_EQ("skipped", step(run, "skipped").outcome);
  IUTEST_EXPECT_EQ("success", step(run, "cleanup").outcome) << test.output.str();
}

IUTEST(EngineTest, StepExitKillsBackgroundProcessesBeforeNextStep) {
  for (const auto* exit_code : {"0", "1"}) {
    EngineRun test;
    const auto marker = test.dir / "background-write";
    test.options.pass_env = {"MARKER=" + marker.string()};
    test.options.vars = {{"EXIT", exit_code}};
    const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: background
        continue-on-error: true
        run: |
          (sleep 0.15; echo poisoned > "$MARKER") >/dev/null 2>&1 &
          exit ${{ vars.EXIT }}
      - id: next
        run: |
          sleep 0.3
          test ! -e "$MARKER"
)");
    IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
    IUTEST_EXPECT_EQ("success", step(job(report, "run"), "next").outcome);
    IUTEST_EXPECT_FALSE(fs::exists(marker));
  }
}

IUTEST(EngineTest, JobTimeoutOverridesLongerStepTimeout) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    timeout-minutes: 0.001
    steps:
      - id: timeout
        timeout-minutes: 1
        run: sleep 10
      - id: skipped
        run: exit 99
)");
  IUTEST_EXPECT_EQ("failure", report.result);
  const auto& run = job(report, "run");
  IUTEST_EXPECT_EQ("failure", step(run, "timeout").outcome);
  IUTEST_EXPECT_NE(std::string::npos, step(run, "timeout").error.find("The job has exceeded"));
  IUTEST_EXPECT_EQ("skipped", step(run, "skipped").outcome);
}

IUTEST(EngineTest, CancellationRunsCleanupAndCancelsWaitingJobs) {
  EngineRun test;
  const auto marker = test.dir / "started";
  test.options.pass_env = {"STARTED=" + marker.string()};
  test.load(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: running
        run: |
          touch "$STARTED"
          sleep 20
      - id: skipped
        run: exit 99
      - id: cleanup
        if: cancelled()
        run: test '${{ job.status }}' = cancelled
  waiting:
    needs: run
    runs-on: test-host
    steps:
      - run: exit 99
)");
  Runner runner(test.options, test.console);
  RunReport report;
  std::exception_ptr error;
  std::thread worker([&] {
    try {
      report = runner.run(test.selection(), test.event, test.git);
    } catch (...) {
      error = std::current_exception();
    }
  });
  const bool started = wait_for_file(marker);
  runner.cancel();
  worker.join();
  if (error) std::rethrow_exception(error);
  IUTEST_ASSERT_TRUE(started) << test.output.str();
  IUTEST_EXPECT_EQ("cancelled", report.result);
  const auto& run = job(report, "run");
  IUTEST_EXPECT_EQ("cancelled", step(run, "running").outcome);
  IUTEST_EXPECT_EQ("skipped", step(run, "skipped").outcome);
  IUTEST_EXPECT_EQ("success", step(run, "cleanup").outcome);
  IUTEST_EXPECT_EQ("cancelled", job(report, "waiting").result);
}

IUTEST(EngineTest, SecondCancellationStopsCleanup) {
  EngineRun test;
  const auto running = test.dir / "running";
  const auto cleanup = test.dir / "cleanup";
  test.options.pass_env = {"RUNNING=" + running.string(), "CLEANUP=" + cleanup.string()};
  test.load(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: running
        run: |
          touch "$RUNNING"
          sleep 20
      - id: cleanup
        if: always()
        run: |
          touch "$CLEANUP"
          sleep 20
)");
  Runner runner(test.options, test.console);
  RunReport report;
  std::exception_ptr error;
  std::thread worker([&] {
    try {
      report = runner.run(test.selection(), test.event, test.git);
    } catch (...) {
      error = std::current_exception();
    }
  });
  const bool started = wait_for_file(running);
  runner.cancel();
  const bool cleaning = wait_for_file(cleanup);
  runner.cancel();
  worker.join();
  if (error) std::rethrow_exception(error);
  IUTEST_ASSERT_TRUE(started);
  IUTEST_ASSERT_TRUE(cleaning) << test.output.str();
  IUTEST_EXPECT_EQ("cancelled", step(job(report, "run"), "cleanup").outcome);
}

IUTEST(EngineTest, CancellationBeforeRunAndNoTriggeredWorkflows) {
  EngineRun test;
  test.load(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - run: exit 99
)");
  Runner runner(test.options, test.console);
  runner.cancel();
  const auto report = runner.run(test.selection(), test.event, test.git);
  IUTEST_EXPECT_EQ("cancelled", report.result);
  IUTEST_EXPECT_EQ("cancelled", job(report, "run").result);
  IUTEST_EXPECT_TRUE(job(report, "run").steps.empty());
  test.options.run_dir = test.dir / "empty-run";
  Runner empty(test.options, test.console);
  IUTEST_EXPECT_EQ("skipped", empty.run({}, test.event, test.git).result);
}

IUTEST(EngineTest, MatrixContextsAndMaxParallel) {
  EngineRun test;
  test.options.pass_env = {"LOCK=" + (test.dir / "matrix-lock").string()};
  const auto report = test.run(R"yaml(
on: push
jobs:
  matrix:
    name: build ${{ matrix.version }}
    runs-on: ${{ matrix.runner }}
    strategy:
      max-parallel: 1
      fail-fast: false
      matrix:
        version: [1, 2, 3, 4]
        runner: [test-host]
    steps:
      - id: check
        run: |
          mkdir "$LOCK"
          sleep 0.03
          test '${{ strategy.job-total }}' = 4
          test '${{ strategy.max-parallel }}' = 1
          test '${{ strategy.fail-fast }}' = false
          test '${{ matrix.version }}' = "$(( ${{ strategy.job-index }} + 1 ))"
          echo version=${{ matrix.version }} >> "$GITHUB_OUTPUT"
          rmdir "$LOCK"
)yaml");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_ASSERT_EQ(1u, report.workflows.size());
  IUTEST_ASSERT_EQ(4u, report.workflows[0].jobs.size());
  for (const auto& run : report.workflows[0].jobs) {
    const auto version = run.matrix.get("version").to_string();
    IUTEST_EXPECT_EQ("build " + version, run.name);
    IUTEST_EXPECT_EQ(version, step(run, "check").outputs.at("version"));
  }
}

IUTEST(EngineTest, GlobalParallelLimit) {
  EngineRun test;
  test.options.parallel = 1;
  test.options.pass_env = {"LOCK=" + (test.dir / "global-lock").string()};
  const auto report = test.run(R"(
on: push
jobs:
  one:
    runs-on: test-host
    steps:
      - run: mkdir "$LOCK"; sleep 0.03; rmdir "$LOCK"
  two:
    runs-on: test-host
    steps:
      - run: mkdir "$LOCK"; sleep 0.03; rmdir "$LOCK"
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
}

IUTEST(EngineTest, MatrixFilteringPreservesStrategyIndex) {
  for (const auto* filter : {"2", "9"}) {
    EngineRun test;
    test.options.matrix = {{"version", filter}};
    const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    strategy:
      matrix: {version: [1, 2, 3]}
    steps:
      - run: |
          test '${{ matrix.version }}' = 2
          test '${{ strategy.job-index }}' = 1
          test '${{ strategy.job-total }}' = 3
)");
    IUTEST_EXPECT_EQ(std::string(filter) == "2" ? "success" : "skipped", report.result) << test.output.str();
    IUTEST_ASSERT_EQ(1u, report.workflows[0].jobs.size());
  }
}

IUTEST(EngineTest, MatrixFailFastCancelsOtherInstances) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    strategy:
      matrix: {version: [fail, slow1, slow2]}
    steps:
      - run: |
          if [ '${{ matrix.version }}' = fail ]; then
            sleep 0.03
            exit 1
          fi
          sleep 10
)");
  IUTEST_EXPECT_EQ("failure", report.result);
  IUTEST_ASSERT_EQ(3u, report.workflows[0].jobs.size());
  int failures = 0;
  int cancelled = 0;
  for (const auto& run : report.workflows[0].jobs) {
    failures += run.result == "failure";
    cancelled += run.result == "cancelled";
  }
  IUTEST_EXPECT_EQ(1, failures);
  IUTEST_EXPECT_EQ(2, cancelled) << test.output.str();
}

IUTEST(EngineTest, MatrixFailFastDisabledAndToleratedFailures) {
  for (bool tolerated : {false, true}) {
    EngineRun test;
    test.options.vars = {{"TOLERATE", tolerated ? "true" : "false"}};
    const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    continue-on-error: ${{ matrix.experimental && vars.TOLERATE == 'true' }}
    strategy:
      fail-fast: ${{ vars.TOLERATE == 'true' }}
      max-parallel: 1
      matrix: {experimental: [true, false]}
    steps:
      - run: test '${{ matrix.experimental }}' = false
)");
    IUTEST_EXPECT_EQ(tolerated ? "success" : "failure", report.result) << test.output.str();
    IUTEST_ASSERT_EQ(2u, report.workflows[0].jobs.size());
    for (const auto& run : report.workflows[0].jobs) {
      IUTEST_EXPECT_EQ(run.matrix.get("experimental").truthy() ? "failure" : "success", run.result);
    }
  }
}

IUTEST(EngineTest, CommandsMaskingAnnotationsAndSummaries) {
  EngineRun test;
  test.options.secrets = {{"TOKEN", "test-secret-value"}};
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    outputs:
      public: ${{ steps.emit.outputs.public }}
      secret: ${{ steps.emit.outputs.secret }}
    steps:
      - id: emit
        env:
          TOKEN: ${{ secrets.TOKEN }}
        run: |
          printf '%s\n' "$TOKEN"
          echo "::warning file=a.cpp,line=4,col=2,title=title::$TOKEN"
          echo "::error::annotation only"
          echo "::stop-commands::pause"
          echo "::warning::not an annotation"
          echo "::pause::"
          echo "::notice::resumed"
          echo "::notice file=$TOKEN,title=$TOKEN::masked metadata"
          echo "::set-output name=legacy::old"
          printf 'public=ok\nsecret=%s\n' "$TOKEN" >> "$GITHUB_OUTPUT"
          printf '# First %s\n' "$TOKEN" >> "$GITHUB_STEP_SUMMARY"
      - run: |
          test '${{ steps.emit.outputs.legacy }}' = old
          test '${{ steps.emit.outputs.secret }}' = '${{ secrets.TOKEN }}'
          echo '# Second' >> "$GITHUB_STEP_SUMMARY"
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  const auto& run = job(report, "run");
  IUTEST_EXPECT_EQ("ok", run.outputs.at("public"));
  IUTEST_EXPECT_EQ(0u, run.outputs.count("secret"));
  IUTEST_ASSERT_EQ(4u, run.annotations.size());
  IUTEST_EXPECT_EQ("***", run.annotations[0].message);
  IUTEST_EXPECT_EQ("a.cpp", run.annotations[0].file);
  IUTEST_EXPECT_EQ(4, run.annotations[0].line);
  IUTEST_EXPECT_EQ(2, run.annotations[0].column);
  IUTEST_EXPECT_EQ("title", run.annotations[0].title);
  IUTEST_EXPECT_EQ("***", run.annotations[3].file);
  IUTEST_EXPECT_EQ("***", run.annotations[3].title);
  IUTEST_EXPECT_EQ("# First ***\n# Second\n", run.summary);
  IUTEST_EXPECT_EQ(std::string::npos, test.output.str().find("test-secret-value"));
  IUTEST_EXPECT_EQ(std::string::npos, util::read_file(run.log_file).find("test-secret-value"));
  IUTEST_EXPECT_EQ(std::string::npos, util::read_file(test.options.run_dir / "report.json").find("test-secret-value"));
  IUTEST_EXPECT_NE(std::string::npos, util::read_file(test.options.run_dir / "summary.md").find("# First ***"));
}

IUTEST(EngineTest, DisabledLegacyEnvironmentCommandsFailStep) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: blocked
        continue-on-error: true
        run: echo "::set-env name=BAD::value"
      - id: opted-in
        env: {ACTIONS_ALLOW_UNSECURE_COMMANDS: "true"}
        run: echo "::set-env name=ALLOWED::value"
      - run: |
          test -z "${BAD:-}"
          test "$ALLOWED" = value
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ("failure", step(job(report, "run"), "blocked").outcome);
}

IUTEST(EngineTest, MalformedFileCommandFailsStepAndMissingDirectoryFailsClearly) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: malformed
        continue-on-error: true
        run: echo invalid >> "$GITHUB_OUTPUT"
      - id: missing
        working-directory: nonexistent
        run: "true"
)");
  IUTEST_EXPECT_EQ("failure", report.result);
  const auto& run = job(report, "run");
  IUTEST_EXPECT_EQ("failure", step(run, "malformed").outcome);
  IUTEST_EXPECT_NE(std::string::npos, step(run, "malformed").error.find("Invalid format"));
  IUTEST_EXPECT_NE(std::string::npos, step(run, "missing").error.find("No such file or directory"));
}

IUTEST(EngineTest, UnsupportedRunnerSkipsJob) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: windows-latest
    steps:
      - run: exit 99
)");
  IUTEST_EXPECT_EQ("skipped", report.result);
  IUTEST_EXPECT_TRUE(job(report, "run").steps.empty());
  IUTEST_EXPECT_NE(std::string::npos, job(report, "run").reason.find("not supported"));
}

IUTEST(EngineTest, HashFilesInExpressionsUsesWorkspace) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - run: printf abc > a.txt
      - id: hash
        if: hashFiles('*.txt') != ''
        run: echo 'hash=${{ hashFiles('*.txt') }}' >> "$GITHUB_OUTPUT"
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  const auto inner = [] {
    util::Sha256 h;
    h.update("abc");
    return h.digest();
  }();
  util::Sha256 outer;
  outer.update(inner.data(), inner.size());
  IUTEST_EXPECT_EQ(util::Sha256::hex(outer.digest()), step(job(report, "run"), "hash").outputs.at("hash"));
}

IUTEST(EngineTest, InvalidMatrixConcurrencyFailsWithoutRunningSteps) {
  for (const auto* value : {"0", "-1", "1.5", "1e100", "${{ 'not-a-number' }}"}) {
    EngineRun test;
    const auto report = test.run(
        "on: push\njobs:\n  run:\n    runs-on: test-host\n    strategy:\n      max-parallel: " +
        std::string(value) + "\n      matrix: {value: [1, 2]}\n    steps:\n      - run: exit 99\n");
    IUTEST_EXPECT_EQ("failure", report.result);
    IUTEST_EXPECT_TRUE(job(report, "run").steps.empty());
    IUTEST_EXPECT_NE(std::string::npos, job(report, "run").reason.find("max-parallel"));
  }
}

IUTEST(EngineTest, MatrixInstancesActuallyRunConcurrently) {
  EngineRun test;
  const auto sync = test.dir / "sync";
  fs::create_directories(sync);
  test.options.parallel = 2;
  test.options.pass_env = {"SYNC=" + sync.string()};
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    strategy:
      max-parallel: 2
      matrix: {value: [0, 1]}
    steps:
      - timeout-minutes: 0.05
        run: |
          mkdir "$SYNC/${{ matrix.value }}"
          attempt=0
          while [ "$attempt" -lt 200 ]; do
            if [ -d "$SYNC/0" ] && [ -d "$SYNC/1" ]; then
              exit 0
            fi
            sleep 0.01
            attempt=$((attempt + 1))
          done
          exit 1
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ(2u, report.workflows[0].jobs.size());
}

IUTEST(EngineTest, MatrixKeysWithEquivalentSanitizedNamesHaveSeparateWorkspaces) {
  EngineRun test;
  test.options.keep_workdir = true;
  const auto report = test.run(R"yaml(
on: push
jobs:
  run:
    runs-on: test-host
    strategy:
      matrix: {value: ["a/b", "a?b"]}
    steps:
      - run: |
          printf '%s' '${{ matrix.value }}' > value.txt
          sleep 0.03
          test "$(cat value.txt)" = '${{ matrix.value }}'
)yaml");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  const auto& jobs = report.workflows[0].jobs;
  IUTEST_ASSERT_EQ(2u, jobs.size());
  IUTEST_EXPECT_NE(jobs[0].log_file, jobs[1].log_file);
}

IUTEST(EngineTest, IdenticalJobIdsAcrossWorkflowsHaveSeparateWorkspacesAndLogs) {
  EngineRun test;
  test.options.keep_workdir = true;
  test.load(R"yaml(
name: First
on: push
jobs:
  same:
    runs-on: test-host
    steps:
      - run: |
          echo first > value.txt
          sleep 0.03
          test "$(cat value.txt)" = first
)yaml");
  WorkflowFile other;
  other.display = ".github/workflows/other.yml";
  other.workflow = std::make_shared<workflow::Workflow>(workflow::parse_workflow(yaml::load(R"yaml(
name: Other
on: push
jobs:
  same:
    runs-on: test-host
    steps:
      - run: |
          echo other > value.txt
          sleep 0.03
          test "$(cat value.txt)" = other
)yaml", other.display), other.display));
  auto selection = test.selection();
  selection.push_back({&other, {true, "test"}});
  Runner runner(test.options, test.console);
  const auto report = runner.run(selection, test.event, test.git);
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_ASSERT_EQ(2u, report.workflows.size());
  const auto& first = report.workflows[0].jobs.at(0);
  const auto& second = report.workflows[1].jobs.at(0);
  IUTEST_EXPECT_NE(first.log_file, second.log_file);
  IUTEST_EXPECT_NE(std::string::npos, util::read_file(first.log_file).find("echo first"));
  IUTEST_EXPECT_EQ(std::string::npos, util::read_file(first.log_file).find("echo other"));
  IUTEST_EXPECT_NE(std::string::npos, util::read_file(second.log_file).find("echo other"));
}

IUTEST(EngineTest, DynamicMasksAndRetainedIsolatedWorkspace) {
  EngineRun test;
  test.options.keep_workdir = true;
  util::write_file(test.git.root / "original.txt", "host");
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    outputs:
      hidden: ${{ steps.mask.outputs.hidden }}
    steps:
      - id: mask
        run: |
          test ! -f original.txt
          echo isolated > created.txt
          printf '::add-mask::%s%s\n' dynamic- token
          printf '%s%s\n' dynamic- token
          printf 'hidden=%s%s\n' dynamic- token >> "$GITHUB_OUTPUT"
          printf '%s%s\n' dynamic- token >> "$GITHUB_STEP_SUMMARY"
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  const auto& run = job(report, "run");
  IUTEST_EXPECT_EQ("***", step(run, "mask").outputs.at("hidden"));
  IUTEST_EXPECT_EQ(0u, run.outputs.count("hidden"));
  IUTEST_EXPECT_EQ("***\n", run.summary);
  IUTEST_EXPECT_EQ(std::string::npos, test.output.str().find("dynamic-token"));
  IUTEST_EXPECT_EQ(std::string::npos, util::read_file(test.options.run_dir / "report.json").find("dynamic-token"));
  IUTEST_EXPECT_FALSE(fs::exists(test.git.root / "created.txt"));
  IUTEST_EXPECT_EQ("host", util::read_file(test.git.root / "original.txt"));
  IUTEST_EXPECT_EQ("isolated\n", util::read_file(test.options.run_dir / "jobs" / report.workflows[0].run_id /
                                               sanitize_key(run.key) /
                                               "work" / "hello" / "hello" / "created.txt"));
}

IUTEST(EngineTest, CleansSnapshotsAndJobsButPreservesRunReports) {
  for (bool keep : {false, true}) {
    EngineRun test;
    test.options.keep_workdir = keep;
    util::write_file(test.git.root / "original.txt", "host");
    const auto report = test.run(R"yaml(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - uses: actions/checkout@v4
      - run: |
          test "$(cat original.txt)" = host
          echo '# Summary' >> "$GITHUB_STEP_SUMMARY"
)yaml");
    IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
    IUTEST_EXPECT_EQ(keep, fs::exists(test.options.run_dir / "snapshot"));
    IUTEST_EXPECT_EQ(keep, fs::exists(test.options.run_dir / "jobs"));
    IUTEST_EXPECT_TRUE(fs::exists(test.options.run_dir / "report.json"));
    IUTEST_EXPECT_TRUE(fs::exists(test.options.run_dir / "summary.md"));
    IUTEST_EXPECT_TRUE(fs::exists(job(report, "run").log_file));
    IUTEST_EXPECT_EQ("host", util::read_file(test.git.root / "original.txt"));
  }
}

IUTEST(EngineTest, RefusesExistingRunDirectoriesFilesAndSymlinksWithoutDeletingData) {
  for (const auto* kind : {"directory", "file", "symlink"}) {
    EngineRun test;
    const auto foreign = test.dir / "foreign";
    util::write_file(foreign / "jobs/run/valuable.txt", "keep");
    util::write_file(foreign / "snapshot/valuable.txt", "keep");
    if (std::string_view(kind) == "directory") {
      test.options.run_dir = foreign;
    } else if (std::string_view(kind) == "file") {
      util::write_file(test.options.run_dir, "keep-file");
    } else {
      fs::create_directory_symlink(foreign, test.options.run_dir);
    }
    Runner runner(test.options, test.console);
    IUTEST_EXPECT_THROW(runner.run({}, test.event, test.git), std::system_error);
    IUTEST_EXPECT_EQ("keep", util::read_file(foreign / "jobs/run/valuable.txt"));
    IUTEST_EXPECT_EQ("keep", util::read_file(foreign / "snapshot/valuable.txt"));
    IUTEST_EXPECT_FALSE(fs::exists(foreign / "logs"));
    if (std::string_view(kind) == "file") {
      IUTEST_EXPECT_EQ("keep-file", util::read_file(test.options.run_dir));
    }
  }
}

IUTEST(EngineTest, NewRunCacheAndLogDirectoriesArePrivate) {
  EngineRun test;
  Runner runner(test.options, test.console);
  const auto report = runner.run({}, test.event, test.git);
  IUTEST_EXPECT_EQ("skipped", report.result);
  for (const auto& path : {test.options.run_dir, test.options.cache_dir, test.options.run_dir / "logs"}) {
    IUTEST_EXPECT_TRUE((fs::status(path).permissions() & fs::perms::all) == fs::perms::owner_all);
  }
}

IUTEST(EngineTest, ReportWriteExceptionsDetachCancellationAndCleanSnapshots) {
  EngineRun test;
  util::write_file(test.git.root / "original.txt", "host");
  test.options.pass_env = {"RUN_DIR=" + test.options.run_dir.string()};
  test.load(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - uses: actions/checkout@v4
      - run: mkdir "$RUN_DIR/report.json"
)");
  Runner runner(test.options, test.console);
  IUTEST_EXPECT_THROW(runner.run(test.selection(), test.event, test.git), std::runtime_error);
  IUTEST_EXPECT_FALSE(fs::exists(test.options.run_dir / "snapshot"));
  IUTEST_EXPECT_FALSE(fs::exists(test.options.run_dir / "jobs"));
  runner.cancel();
  runner.cancel();
  util::remove_tree(test.options.run_dir);
  const auto report = runner.run(test.selection(), test.event, test.git);
  IUTEST_EXPECT_EQ("cancelled", report.result);
}

IUTEST(EngineTest, MasksReportMetadataAndConsolePrefixes) {
  EngineRun test;
  test.options.secrets = {{"TOKEN", "private-token"}};
  test.options.vars = {{"TOKEN", "private-token"}};
  test.event.info.ref = "refs/heads/private-token";
  const auto report = test.run(R"(
name: private-token
on: push
jobs:
  run:
    name: ${{ matrix.token }}
    runs-on: test-host
    strategy:
      matrix: {token: ["${{ vars.TOKEN }}"]}
    steps:
      - id: error
        name: ${{ secrets.TOKEN }}
        shell: missing-${{ vars.TOKEN }} {0}
        run: "true"
      - if: always()
        run: echo '# Summary' >> "$GITHUB_STEP_SUMMARY"
  unsupported:
    runs-on: ${{ vars.TOKEN }}
    steps:
      - run: exit 99
)");
  IUTEST_EXPECT_EQ("failure", report.result);
  const auto serialized = report_to_json(report);
  IUTEST_EXPECT_EQ(std::string::npos, serialized.find("private-token"));
  IUTEST_EXPECT_EQ(std::string::npos, test.output.str().find("private-token"));
  IUTEST_EXPECT_EQ("***", report.workflows[0].name);
  IUTEST_EXPECT_EQ("refs/heads/***", report.ref);
  IUTEST_EXPECT_EQ("***", job(report, "run").name);
  IUTEST_EXPECT_EQ("***", job(report, "run").matrix.get("token").to_string());
  IUTEST_EXPECT_EQ("***", step(job(report, "run"), "error").name);
  IUTEST_EXPECT_EQ(std::string::npos, util::read_file(test.options.run_dir / "summary.md").find("private-token"));
  IUTEST_EXPECT_EQ(std::string::npos, util::read_file(test.options.run_dir / "report.json").find("private-token"));
}

IUTEST(EngineTest, FinalReportAppliesMasksRegisteredByLaterSteps) {
  EngineRun test;
  const auto report = test.run(R"yaml(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: early
        run: |
          printf 'secret=%s%s\n' late- token >> "$GITHUB_OUTPUT"
          printf '%s%s\n' late- token >> "$GITHUB_STEP_SUMMARY"
      - run: printf '::add-mask::%s%s\n' late- token
      - run: test '${{ steps.early.outputs.secret }}' = "$(printf '%s%s' late- token)"
)yaml");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ("***", step(job(report, "run"), "early").outputs.at("secret"));
  IUTEST_EXPECT_EQ("***\n", job(report, "run").summary);
  IUTEST_EXPECT_EQ(std::string::npos, report_to_json(report).find("late-token"));
  IUTEST_EXPECT_EQ(std::string::npos, util::read_file(test.options.run_dir / "summary.md").find("late-token"));
}

IUTEST(EngineTest, InvalidTimeoutsAreNotSilentlyDisabled) {
  for (const auto* value : {"0", "-1", "1e100", "${{ 'not-a-number' }}"}) {
    for (bool job_timeout : {true, false}) {
      EngineRun test;
      const auto report = test.run(
          "on: push\njobs:\n  run:\n    runs-on: test-host\n" +
          (job_timeout ? "    timeout-minutes: " + std::string(value) + "\n" : "") +
          "    steps:\n      - id: run\n" +
          (!job_timeout ? "        timeout-minutes: " + std::string(value) + "\n" : "") +
          "        run: 'true'\n");
      IUTEST_EXPECT_EQ("failure", report.result) << value << "\n" << test.output.str();
      const auto& run = job(report, "run");
      IUTEST_EXPECT_NE(std::string::npos,
                       (job_timeout ? run.reason : step(run, "run").error).find("timeout-minutes"));
    }
  }
}

IUTEST(EngineTest, InvalidRuntimeExpressionsFailWithSourceLocation) {
  EngineRun test;
  test.options.vars = {{"JSON", "not-json"}};
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: invalid
        if: fromJSON(vars.JSON)
        run: exit 99
      - id: cleanup
        if: always()
        run: "true"
)");
  IUTEST_EXPECT_EQ("failure", report.result);
  const auto& run = job(report, "run");
  IUTEST_EXPECT_NE(std::string::npos, step(run, "invalid").error.find(".github/workflows/test.yml:"));
  IUTEST_EXPECT_NE(std::string::npos, step(run, "invalid").error.find("Error evaluating 'if'"));
  IUTEST_EXPECT_EQ("success", step(run, "cleanup").outcome);
}

IUTEST(EngineTest, OversizedSummaryIsAnnotatedWithoutFailingStep) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - run: head -c 1048577 /dev/zero > "$GITHUB_STEP_SUMMARY"
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  const auto& run = job(report, "run");
  IUTEST_EXPECT_TRUE(run.summary.empty());
  IUTEST_ASSERT_EQ(1u, run.annotations.size());
  IUTEST_EXPECT_EQ("error", run.annotations[0].level);
  IUTEST_EXPECT_NE(std::string::npos, run.annotations[0].message.find("1024k"));
}

IUTEST(EngineHelpersTest, HashFilesSortedDeduplicatedExcludedAndEmpty) {
  cpa_test::TempDir dir;
  util::write_file(dir / "b.txt", "second");
  util::write_file(dir / "a.txt", "first");
  util::write_file(dir / "nested/c.txt", "excluded");
  const auto first = util::sha256_file(dir / "a.txt");
  const auto second = util::sha256_file(dir / "b.txt");
  util::Sha256 expected;
  expected.update(first.data(), first.size());
  expected.update(second.data(), second.size());
  IUTEST_EXPECT_EQ(util::Sha256::hex(expected.digest()), hash_files(dir.path(), {"**/*.txt", "!nested/**"}));
  IUTEST_EXPECT_EQ(hash_files(dir.path(), {"*.txt"}), hash_files(dir.path(), {"b.txt", "a.txt", "*.txt"}));
  IUTEST_EXPECT_EQ("", hash_files(dir.path(), {"missing/**"}));
  IUTEST_EXPECT_EQ("", hash_files(dir.path(), {}));
  util::write_file(dir / "a.txt", "changed");
  IUTEST_EXPECT_NE(hash_files(dir.path(), {"a.txt"}), hash_files(dir.path(), {"b.txt"}));
}

IUTEST(EngineHelpersTest, HashFilesDoesNotReadSymlinksOutsideWorkspace) {
  cpa_test::TempDir dir;
  const auto root = dir / "workspace";
  fs::create_directories(root);
  util::write_file(dir / "outside.txt", "outside");
  fs::create_symlink(dir / "outside.txt", root / "escape.txt");
  IUTEST_EXPECT_EQ("", hash_files(root, {"*.txt"}));
  util::write_file(root / "inside.txt", "inside");
  IUTEST_EXPECT_EQ(hash_files(root, {"inside.txt"}), hash_files(root, {"*.txt"}));
}

IUTEST(EngineHelpersTest, RunnerLabelsAndHostOverrides) {
  IUTEST_EXPECT_TRUE(runs_on_labels(expr::Value()).empty());
  IUTEST_EXPECT_EQ("macos-latest", runs_on_labels(expr::Value("macos-latest")).at(0));
  const auto labels = runs_on_labels(expr::Value::parse_json(R"({"group":"builders","labels":["self-hosted","macOS"]})"));
  IUTEST_ASSERT_EQ(3u, labels.size());
  IUTEST_EXPECT_EQ("group:builders", labels[0]);
  IUTEST_EXPECT_EQ("self-hosted", labels[1]);
  IUTEST_EXPECT_TRUE(match_runner(labels, {}, true).host);
  IUTEST_EXPECT_FALSE(match_runner(labels, {}, false).host);
  IUTEST_EXPECT_TRUE(match_runner({"macos-14"}, {}, true).host);
  IUTEST_EXPECT_FALSE(match_runner({"ubuntu-latest"}, {}, true).host);
  IUTEST_EXPECT_TRUE(match_runner({"ubuntu-latest"}, {"UBUNTU-LATEST"}, false).host);
  IUTEST_EXPECT_FALSE(match_runner({}, {"test-host"}, false).host);
}

IUTEST(EngineHelpersTest, SanitizedJobKeysRemainSafeBoundedAndDistinct) {
  const std::vector<std::string> keys = {
      "", ".", "..", "/", "a/b", "a?b", "a_b", "a b", std::string(200, 'x'), std::string(199, 'x') + "y"};
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const auto sanitized = sanitize_key(keys[i]);
    IUTEST_EXPECT_LE(sanitized.size(), 100u);
    IUTEST_EXPECT_EQ(std::string::npos, sanitized.find('/'));
    IUTEST_EXPECT_NE(".", sanitized);
    IUTEST_EXPECT_NE("..", sanitized);
    IUTEST_EXPECT_EQ(sanitized, sanitize_key(keys[i]));
    for (std::size_t j = 0; j < i; ++j) IUTEST_EXPECT_NE(sanitize_key(keys[j]), sanitized);
  }
}

IUTEST(EngineHelpersTest, ShellResolutionAndSubstitution) {
  const std::string path = "/usr/bin:/bin";
  const auto implicit = resolve_shell("", path);
  IUTEST_EXPECT_EQ("-e", implicit.args.at(0));
  const auto bash = resolve_shell("bash", path);
  IUTEST_EXPECT_EQ("--noprofile", bash.args.at(0));
  IUTEST_EXPECT_EQ("pipefail", bash.args.at(3));
  IUTEST_EXPECT_EQ(".sh", bash.extension);
  const auto custom = resolve_shell("sh -e \"{0}\"", path);
  IUTEST_EXPECT_EQ("/a script.sh", custom.argv("/a script.sh").back());
  IUTEST_EXPECT_EQ("body", custom.script("body"));
  IUTEST_EXPECT_THROW(resolve_shell("cmd", path), ShellError);
  IUTEST_EXPECT_THROW(resolve_shell("sh -e", path), ShellError);
  IUTEST_EXPECT_THROW(resolve_shell("unknown-shell", path), ShellError);
  IUTEST_EXPECT_THROW(resolve_shell("missing-shell {0}", path), ShellError);
  Shell pwsh;
  pwsh.powershell = true;
  IUTEST_EXPECT_NE(std::string::npos, pwsh.script("Write-Output ok").find("$ErrorActionPreference = 'stop'"));
  IUTEST_EXPECT_NE(std::string::npos, pwsh.script("Write-Output ok").find("exit $LASTEXITCODE"));
}

IUTEST(EngineHelpersTest, EvaluationRetainsTypesAndReportsLocation) {
  auto document = yaml::load(R"(
scalar: ${{ vars.VALUE }}
sequence: [true, 7, "${{ vars.VALUE }}"]
mapping: {key: "${{ vars.VALUE }}"}
object: ${{ fromJSON('{"a":"one","b":2}') }}
bad: ${{ fromJSON('[1]') }}
)", "eval.yml");
  expr::EvalContext context;
  context.contexts.set("vars", to_object({{"VALUE", "evaluated"}}));
  const auto scope = workflow::scope_for(workflow::Site::StepGeneral);
  const auto value = evaluate_token(document->root, context, scope, "eval.yml");
  IUTEST_EXPECT_EQ("evaluated", value.get("scalar").to_string());
  IUTEST_EXPECT_TRUE(value.get("sequence").items()[0].is_bool());
  IUTEST_EXPECT_TRUE(value.get("sequence").items()[1].is_number());
  IUTEST_EXPECT_EQ("evaluated", value.get("mapping").get("key").to_string());
  const auto map = evaluate_string_map(document->root->get("object"), context, scope, "eval.yml");
  IUTEST_ASSERT_EQ(2u, map.size());
  IUTEST_EXPECT_EQ("one", map[0].second);
  IUTEST_EXPECT_EQ("2", map[1].second);
  IUTEST_EXPECT_THROW(evaluate_string_map(document->root->get("bad"), context, scope, "eval.yml"), EvaluationError);
  try {
    (void)evaluate_string(document->root->get("sequence"), context, scope, "eval.yml");
    IUTEST_ADD_FAILURE();
  } catch (const EvaluationError& error) {
    IUTEST_EXPECT_NE(std::string::npos, std::string(error.what()).find("eval.yml:3:"));
  }
  IUTEST_EXPECT_TRUE(evaluate_token(nullptr, context, scope, "eval.yml").is_null());
  IUTEST_EXPECT_TRUE(evaluate_string_map(nullptr, context, scope, "eval.yml").empty());
}

IUTEST(EngineHelpersTest, StatusFunctionsAndResultPrecedence) {
  const auto success = job_status_functions({});
  IUTEST_EXPECT_TRUE(success("success"));
  IUTEST_EXPECT_TRUE(success("always"));
  IUTEST_EXPECT_FALSE(success("failure"));
  const auto cancelled = job_status_functions({false, true, true});
  IUTEST_EXPECT_TRUE(cancelled("cancelled"));
  IUTEST_EXPECT_FALSE(cancelled("success"));
  IUTEST_EXPECT_FALSE(cancelled("failure"));
  IUTEST_EXPECT_EQ("skipped", combine_results({}));
  IUTEST_EXPECT_EQ("success", combine_results({"skipped", "success"}));
  IUTEST_EXPECT_EQ("cancelled", combine_results({"success", "cancelled"}));
  IUTEST_EXPECT_EQ("failure", combine_results({"cancelled", "failure", "success"}));
}
