#include "iutest.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <sstream>
#include <thread>

#include "engine/runner.hpp"
#include "test_support.hpp"
#include "workflow/parser.hpp"

using namespace cpa;
using namespace cpa::engine;

namespace {

class ReusableRun {
 public:
  ReusableRun() : console(output, false) {
    options.run_dir = dir / "run";
    options.cache_dir = dir / "cache";
    options.host_labels = {"test-host"};
    options.sandbox.enabled = false;
    options.parallel = 4;
    git.root = dir / "repository";
    fs::create_directories(git.root);
    git.github = workspace::GitHubRepo{"fixture-owner", "hello"};
    git.head_sha = std::string(40, 'a');
    event.info.name = "push";
    event.info.ref = "refs/heads/main";
    event.sha = git.head_sha;
    event.actor = "fixture-user";
    event.info.payload = expr::Value::object();
  }

  void workflow(const std::string& name, const std::string& text) {
    util::write_file(git.root / ".github/workflows" / name, text);
  }

  void load(const std::string& text) {
    file.display = ".github/workflows/main.yml";
    file.path = git.root / file.display;
    util::write_file(file.path, text);
    file.workflow = std::make_shared<workflow::Workflow>(
        workflow::parse_workflow(yaml::load(text, file.display), file.display));
  }

  std::vector<Selection> selection() const { return {{&file, {true, "test"}}}; }

  RunReport run(const std::string& text) {
    load(text);
    Runner runner(options, console);
    return runner.run(selection(), event, git);
  }

  fs::path cache(const std::string& sha) {
    const auto root = options.cache_dir / "actions/fixture-owner/library" / sha;
    fs::create_directories(root);
    util::write_file(root / ".cpp-actions-complete", sha + "\n");
    return root;
  }

  cpa_test::TempDir dir;
  RunOptions options;
  workspace::GitInfo git;
  Event event;
  std::ostringstream output;
  Console console;
  WorkflowFile file;
};

const JobReport& reusable_job(const RunReport& report, std::string_view key) {
  for (const auto& workflow : report.workflows) {
    for (const auto& job : workflow.jobs) {
      if (job.key == key) return job;
    }
  }
  throw std::runtime_error("missing job " + std::string(key));
}

const std::string kSimple = R"(
on: workflow_call
jobs:
  leaf:
    runs-on: test-host
    steps:
      - run: echo called
)";

std::string call(const std::string& uses, const std::string& extra = "") {
  return "on: push\njobs:\n  call:\n    uses: " + uses + "\n" + extra;
}

bool wait_for_marker(const fs::path& path) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fs::exists(path)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

IUTEST(ReusableWorkflowTest, LocalTypedInputsDefaultsSecretsAndOutputs) {
  ReusableRun test;
  test.options.secrets = {{"SOURCE", "reusable-source-token"}};
  test.workflow("typed.yml", R"(
on:
  workflow_call:
    inputs:
      enabled: {type: boolean, required: true}
      count: {type: number, required: true}
      text: {type: string, required: true}
      fallback: {type: string, default: default-value}
      off: {type: boolean}
      zero: {type: number}
      empty: {type: string}
    secrets:
      TOKEN: {required: true}
    outputs:
      answer:
        value: ${{ jobs.leaf.outputs.answer }}
jobs:
  leaf:
    runs-on: test-host
    outputs:
      answer: ${{ steps.make.outputs.answer }}
    steps:
      - id: make
        env:
          TOKEN: ${{ secrets.TOKEN }}
        run: |
          test '${{ inputs.enabled == true }}' = true
          test '${{ inputs.count == 7 }}' = true
          test '${{ inputs.text }}' = hello
          test '${{ inputs.fallback }}' = default-value
          test '${{ inputs.off == false }}' = true
          test '${{ inputs.zero == 0 }}' = true
          test '${{ inputs.empty }}' = ''
          test "$TOKEN" = reusable-source-token
          test '${{ secrets.SOURCE }}' = ''
          printf 'answer=typed-ok\n' >> "$GITHUB_OUTPUT"
)");
  const auto report = test.run(call("./.github/workflows/typed.yml", R"(
    with:
      enabled: true
      count: ${{ 7 }}
      text: hello
    secrets:
      TOKEN: ${{ secrets.SOURCE }}
  verify:
    needs: call
    runs-on: test-host
    steps:
      - run: test '${{ needs.call.outputs.answer }}' = typed-ok
)"));
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ("typed-ok", reusable_job(report, "call").outputs.at("answer"));
  IUTEST_EXPECT_EQ("success", reusable_job(report, "call / leaf").result);
  IUTEST_EXPECT_EQ("success", reusable_job(report, "verify").result);
}

IUTEST(ReusableWorkflowTest, NestedLocalOutputsAndInheritAllowsUndeclaredSecrets) {
  ReusableRun test;
  test.options.secrets = {{"TOKEN", "inherited-token"}, {"EXTRA", "extra-token"}};
  test.workflow("middle.yml", R"(
on:
  workflow_call:
    inputs:
      word: {type: string}
    secrets:
      TOKEN: {required: true}
    outputs:
      answer:
        value: ${{ jobs.nested.outputs.answer }}
jobs:
  nested:
    uses: ./.github/workflows/leaf.yml
    with:
      word: ${{ inputs.word }}
    secrets: inherit
)");
  test.workflow("leaf.yml", R"(
on:
  workflow_call:
    inputs:
      word: {type: string}
    outputs:
      answer:
        value: ${{ jobs.leaf.outputs.answer }}
jobs:
  leaf:
    runs-on: test-host
    outputs:
      answer: ${{ steps.make.outputs.answer }}
    steps:
      - id: make
        env:
          TOKEN: ${{ secrets.TOKEN }}
          EXTRA: ${{ secrets.EXTRA }}
        run: |
          test "$TOKEN" = inherited-token
          test "$EXTRA" = extra-token
          printf 'answer=%s\n' '${{ inputs.word }}' >> "$GITHUB_OUTPUT"
)");
  const auto report = test.run(call("./.github/workflows/middle.yml",
                                  "    with: {word: nested-ok}\n    secrets: inherit\n"));
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ("nested-ok", reusable_job(report, "call").outputs.at("answer"));
  IUTEST_EXPECT_EQ("success", reusable_job(report, "call / nested / leaf").result);
}

IUTEST(ReusableWorkflowTest, NullStringInputsBecomeEmptyStrings) {
  ReusableRun test;
  test.workflow("typed.yml", R"(
on:
  workflow_call:
    inputs:
      literal: {type: string, required: true}
      blank: {type: string}
      tilde: {type: string}
      expression: {type: string}
      missing: {type: string}
      overridden: {type: string, default: fallback}
      default_null: {type: string, default: null}
      default_expression: {type: string, default: '${{ null }}'}
      quoted: {type: string}
jobs:
  leaf:
    runs-on: test-host
    steps:
      - run: |
          test '${{ toJSON(inputs.literal) }}' = '""'
          test '${{ toJSON(inputs.blank) }}' = '""'
          test '${{ toJSON(inputs.tilde) }}' = '""'
          test '${{ toJSON(inputs.expression) }}' = '""'
          test '${{ toJSON(inputs.missing) }}' = '""'
          test '${{ toJSON(inputs.overridden) }}' = '""'
          test '${{ toJSON(inputs.default_null) }}' = '""'
          test '${{ toJSON(inputs.default_expression) }}' = '""'
          test '${{ inputs.quoted }}' = null
)");
  const auto report = test.run(call("./.github/workflows/typed.yml", R"(
    with:
      literal: null
      blank:
      tilde: ~
      expression: ${{ null }}
      missing: ${{ inputs.missing }}
      overridden: null
      quoted: 'null'
)"));
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ("success", reusable_job(report, "call / leaf").result);
}

IUTEST(ReusableWorkflowTest, RejectsCoercedInputTypes) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"boolean", "'true'"}, {"boolean", "'false'"}, {"number", "'12'"}, {"number", "true"},
      {"string", "12"}, {"string", "false"},
      {"boolean", "null"}, {"number", "null"}, {"boolean", "${{ null }}"}, {"number", "${{ null }}"},
      {"boolean", "${{ 'true' }}"}, {"number", "${{ '12' }}"}};
  for (const auto& [type, value] : cases) {
    ReusableRun test;
    test.workflow("typed.yml", "on:\n  workflow_call:\n    inputs:\n      value: {type: " + type +
                                   "}\njobs:\n  leaf:\n    runs-on: test-host\n    steps:\n      - run: echo ok\n");
    const auto report = test.run(call("./.github/workflows/typed.yml", "    with:\n      value: " + value + "\n"));
    IUTEST_EXPECT_EQ("failure", report.result) << type << ": " << value;
    IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("must be a " + type));
    IUTEST_EXPECT_EQ(1u, report.workflows.front().jobs.size());
  }
}

IUTEST(ReusableWorkflowTest, RejectsUndeclaredExplicitSecrets) {
  ReusableRun test;
  test.workflow("leaf.yml", kSimple);
  const auto report = test.run(call("./.github/workflows/leaf.yml", "    secrets:\n      EXTRA: explicit-secret\n"));
  IUTEST_EXPECT_EQ("failure", report.result);
  IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("invalid secret, EXTRA"));
  IUTEST_EXPECT_EQ(1u, report.workflows.front().jobs.size());
}

IUTEST(ReusableWorkflowTest, RequiredSecretNamesAreCaseInsensitive) {
  ReusableRun test;
  test.workflow("leaf.yml", R"(
on:
  workflow_call:
    secrets:
      TOKEN: {required: true}
jobs:
  leaf:
    runs-on: test-host
    steps:
      - run: test '${{ secrets.TOKEN }}' = mixed-case-token
)");
  const auto report = test.run(call("./.github/workflows/leaf.yml",
                                  "    secrets:\n      token: mixed-case-token\n"));
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
}

IUTEST(ReusableWorkflowTest, SecretsAreNotImplicitlyInherited) {
  ReusableRun test;
  test.options.secrets = {{"TOKEN", "caller-only-token"}};
  test.workflow("leaf.yml", R"(
on:
  workflow_call:
    secrets:
      TOKEN: {required: true}
jobs:
  leaf:
    runs-on: test-host
    steps:
      - run: exit 1
)");
  const auto report = test.run(call("./.github/workflows/leaf.yml"));
  IUTEST_EXPECT_EQ("failure", report.result);
  IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("secret required and not supplied"));
  IUTEST_EXPECT_EQ(1u, report.workflows.front().jobs.size());
}

IUTEST(ReusableWorkflowTest, InheritedRequiredSecretNamesAreCaseInsensitiveAndStillRequired) {
  for (bool supplied : {false, true}) {
    ReusableRun test;
    test.options.secrets = supplied ? std::map<std::string, std::string>{{"token", "inherited-case-token"}}
                                   : std::map<std::string, std::string>{{"OTHER", "unrelated-token"}};
    test.workflow("leaf.yml", R"(
on:
  workflow_call:
    secrets:
      TOKEN: {required: true}
jobs:
  leaf:
    runs-on: test-host
    steps:
      - run: test '${{ secrets.TOKEN }}' = inherited-case-token
)");
    const auto report = test.run(call("./.github/workflows/leaf.yml", "    secrets: inherit\n"));
    IUTEST_EXPECT_EQ(supplied ? "success" : "failure", report.result) << test.output.str();
    if (!supplied) {
      IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("secret required and not supplied"));
      IUTEST_EXPECT_EQ(1u, report.workflows.front().jobs.size());
    }
  }
}

IUTEST(ReusableWorkflowTest, MissingAndUnknownArgumentsFailBeforeJobsStart) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"    inputs:\n      value: {type: string, required: true}\n", "input required and not supplied: value"},
      {"    secrets:\n      TOKEN: {required: true}\n", "secret required and not supplied: TOKEN"}};
  for (const auto& [declaration, reason] : cases) {
    ReusableRun test;
    test.workflow("leaf.yml", "on:\n  workflow_call:\n" + declaration +
                                  "jobs:\n  leaf:\n    runs-on: test-host\n    steps:\n      - run: echo ok\n");
    const auto report = test.run(call("./.github/workflows/leaf.yml"));
    IUTEST_EXPECT_EQ("failure", report.result);
    IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find(reason));
    IUTEST_EXPECT_EQ(1u, report.workflows.front().jobs.size());
  }
  ReusableRun test;
  test.workflow("leaf.yml", kSimple);
  const auto report = test.run(call("./.github/workflows/leaf.yml", "    with: {unknown: value}\n"));
  IUTEST_EXPECT_EQ("failure", report.result);
  IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("invalid input, unknown"));
}

IUTEST(ReusableWorkflowTest, RequiresWorkflowCallTrigger) {
  ReusableRun test;
  test.workflow("leaf.yml", "on: push\njobs:\n  leaf:\n    runs-on: test-host\n    steps:\n      - run: echo ok\n");
  const auto report = test.run(call("./.github/workflows/leaf.yml"));
  IUTEST_EXPECT_EQ("failure", report.result);
  IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("no 'workflow_call' trigger"));
}

IUTEST(ReusableWorkflowTest, MasksNewLiteralAndDerivedSecretsEverywhere) {
  ReusableRun test;
  test.options.secrets = {{"BASE", "base-secret"}};
  test.workflow("leaf.yml", R"(
on:
  workflow_call:
    secrets:
      TOKEN: {required: true}
      LITERAL: {required: true}
jobs:
  leaf:
    runs-on: test-host
    steps:
      - run: |
          echo '${{ secrets.TOKEN }}'
          echo '${{ secrets.LITERAL }}'
          printf '%s\n' '${{ secrets.TOKEN }}' >> "$GITHUB_STEP_SUMMARY"
)");
  const auto report = test.run(call("./.github/workflows/leaf.yml", R"(
    secrets:
      TOKEN: ${{ format('prefix-{0}-suffix', secrets.BASE) }}
      LITERAL: newly-passed-literal
)"));
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  const auto& leaf = reusable_job(report, "call / leaf");
  for (const auto& text : {test.output.str(), util::read_file(leaf.log_file),
                           util::read_file(test.options.run_dir / "report.json"),
                           util::read_file(test.options.run_dir / "summary.md")}) {
    IUTEST_EXPECT_EQ(std::string::npos, text.find("prefix-base-secret-suffix"));
    IUTEST_EXPECT_EQ(std::string::npos, text.find("prefix-***-suffix"));
    IUTEST_EXPECT_EQ(std::string::npos, text.find("newly-passed-literal"));
    IUTEST_EXPECT_NE(std::string::npos, text.find("***"));
  }
}

IUTEST(ReusableWorkflowTest, ValidatesLocalAndRemoteReferencesBeforeFetching) {
  const std::vector<std::string> references = {
      "./leaf.yml", "./.github/workflows/sub/leaf.yml", "./.github/workflows/../leaf.yml",
      "./.github/workflows/leaf.txt", "./.github/workflows/leaf.yml@main",
      "fixture-owner/library/.github/workflows/leaf.yml", "fixture-owner/library/.github/workflows/leaf.yml@",
      "../library/.github/workflows/leaf.yml@main", "fixture-owner/../.github/workflows/leaf.yml@main",
      "fixture-owner/library/other/workflows/leaf.yml@main", "fixture-owner/library/.github/workflows/sub/leaf.yml@main",
      "fixture-owner/library/.github/workflows/leaf.yml@../main", "fixture-owner/library/.github/workflows/leaf.yml@-main",
      "fixture-owner/library/.github/workflows/leaf.yml@refs//main"};
  for (const auto& reference : references) {
    ReusableRun test;
    test.load(call("./.github/workflows/placeholder.yml"));
    std::const_pointer_cast<workflow::Workflow>(test.file.workflow)->jobs.front().uses = reference;
    Runner runner(test.options, test.console);
    const auto report = runner.run(test.selection(), test.event, test.git);
    IUTEST_EXPECT_EQ("failure", report.result) << reference;
    IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("workflow")) << reference;
    IUTEST_EXPECT_EQ(std::string::npos, test.output.str().find("Download action")) << reference;
  }
}

IUTEST(ReusableWorkflowTest, RejectsLocalSymlinkEscapes) {
  for (bool directory : {false, true}) {
    ReusableRun test;
    util::write_file(test.dir / "outside/leaf.yml", kSimple);
    fs::create_directories(test.git.root / ".github/workflows");
    if (directory) {
      fs::remove(test.git.root / ".github/workflows");
      fs::create_directory_symlink(test.dir / "outside", test.git.root / ".github/workflows");
    } else {
      fs::create_symlink(test.dir / "outside/leaf.yml", test.git.root / ".github/workflows/leaf.yml");
    }
    const auto report = test.run(call("./.github/workflows/leaf.yml"));
    IUTEST_EXPECT_EQ("failure", report.result);
    IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("invalid workflow path"));
  }
}

IUTEST(ReusableWorkflowTest, RemoteNestedLocalCallsUseCalledRepositoryAndCommit) {
  ReusableRun test;
  const std::string sha(40, 'b');
  const auto root = test.cache(sha);
  util::write_file(root / ".github/workflows/middle.yml", R"(
on:
  workflow_call:
    outputs:
      answer:
        value: ${{ jobs.nested.outputs.answer }}
jobs:
  nested:
    uses: ./.github/workflows/leaf.yml
)");
  util::write_file(root / ".github/workflows/leaf.yml", R"(
on:
  workflow_call:
    outputs:
      answer:
        value: ${{ jobs.leaf.outputs.answer }}
jobs:
  leaf:
    runs-on: test-host
    outputs:
      answer: ${{ steps.make.outputs.answer }}
    steps:
      - id: make
        run: |
          test "$GITHUB_REPOSITORY" = fixture-owner/hello
          printf 'answer=remote-commit-ok\n' >> "$GITHUB_OUTPUT"
)");
  test.workflow("leaf.yml", "on: workflow_call\njobs:\n  wrong:\n    runs-on: test-host\n"
                             "    steps:\n      - run: exit 1\n");
  const auto report = test.run(call("fixture-owner/library/.github/workflows/middle.yml@" + sha));
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ("remote-commit-ok", reusable_job(report, "call").outputs.at("answer"));
  IUTEST_EXPECT_EQ("success", reusable_job(report, "call / nested / leaf").result);
}

IUTEST(ReusableWorkflowTest, RejectsRemoteCachedSymlinkEscapes) {
  ReusableRun test;
  const std::string sha(40, 'c');
  const auto root = test.cache(sha);
  util::write_file(test.dir / "outside.yml", kSimple);
  fs::create_directories(root / ".github/workflows");
  fs::create_symlink(test.dir / "outside.yml", root / ".github/workflows/leaf.yml");
  const auto report = test.run(call("fixture-owner/library/.github/workflows/leaf.yml@" + sha));
  IUTEST_EXPECT_EQ("failure", report.result);
  IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("invalid workflow path"));
}

IUTEST(ReusableWorkflowTest, DetectsCyclesIncludingSymlinkAliases) {
  for (bool alias : {false, true}) {
    ReusableRun test;
    test.workflow("loop.yml", "on: workflow_call\njobs:\n  recurse:\n    uses: ./.github/workflows/" +
                                  std::string(alias ? "alias.yml" : "loop.yml") + "\n");
    if (alias) fs::create_symlink("loop.yml", test.git.root / ".github/workflows/alias.yml");
    const auto report = test.run(call("./.github/workflows/loop.yml"));
    IUTEST_EXPECT_EQ("failure", report.result);
    IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call / recurse").reason.find("call cycle"));
    IUTEST_EXPECT_EQ(2u, report.workflows.front().jobs.size());
  }
}

IUTEST(ReusableWorkflowTest, EnforcesTenLevelLimit) {
  for (int levels : {9, 10}) {
    ReusableRun test;
    for (int i = 0; i < levels; ++i) {
      test.workflow("level" + std::to_string(i) + ".yml",
                    i + 1 == levels ? kSimple
                                    : "on: workflow_call\njobs:\n  next:\n    uses: ./.github/workflows/level" +
                                          std::to_string(i + 1) + ".yml\n");
    }
    const auto report = test.run(call("./.github/workflows/level0.yml"));
    IUTEST_EXPECT_EQ(levels == 9 ? "success" : "failure", report.result) << test.output.str();
    if (levels == 10) {
      IUTEST_EXPECT_NE(std::string::npos, test.output.str().find("nested up to 10 levels"));
    }
  }
}

IUTEST(ReusableWorkflowTest, MatrixFailFastCancelsAlreadyRunningNestedJobs) {
  ReusableRun test;
  const auto marker = test.dir / "running";
  test.workflow("leaf.yml", R"(
on:
  workflow_call:
    inputs:
      fail: {type: boolean}
jobs:
  leaf:
    runs-on: test-host
    steps:
      - run: |
          if [ '${{ inputs.fail }}' = true ]; then
            for i in $(seq 1 200); do
              test -f ')" + marker.string() + R"(' && exit 1
              sleep 0.02
            done
            exit 1
          fi
          touch ')" + marker.string() + R"('
          sleep 30
)");
  const auto started = std::chrono::steady_clock::now();
  const auto report = test.run(call("./.github/workflows/leaf.yml", R"(
    strategy:
      matrix:
        fail: [false, true]
    with:
      fail: ${{ matrix.fail }}
)"));
  IUTEST_EXPECT_EQ("failure", report.result) << test.output.str();
  const auto cancelled = std::count_if(report.workflows.front().jobs.begin(), report.workflows.front().jobs.end(),
                                       [](const auto& job) { return job.result == "cancelled"; });
  IUTEST_EXPECT_GE(cancelled, 2);
  IUTEST_EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(15));
}

IUTEST(ReusableWorkflowTest, RunCancellationReachesNestedJobsAndDependents) {
  ReusableRun test;
  const auto marker = test.dir / "running";
  test.workflow("leaf.yml", R"(
on: workflow_call
jobs:
  leaf:
    runs-on: test-host
    steps:
      - run: |
          touch ')" + marker.string() + R"('
          sleep 30
  later:
    needs: leaf
    runs-on: test-host
    steps:
      - run: exit 1
)");
  test.workflow("middle.yml", "on: workflow_call\njobs:\n  nested:\n    uses: ./.github/workflows/leaf.yml\n");
  test.load(call("./.github/workflows/middle.yml"));
  Runner runner(test.options, test.console);
  RunReport report;
  std::exception_ptr failure;
  std::thread worker([&] {
    try {
      report = runner.run(test.selection(), test.event, test.git);
    } catch (...) {
      failure = std::current_exception();
    }
  });
  const bool started = wait_for_marker(marker);
  runner.cancel();
  worker.join();
  if (failure) std::rethrow_exception(failure);
  IUTEST_EXPECT_TRUE(started);
  IUTEST_EXPECT_EQ("cancelled", report.result) << test.output.str();
  IUTEST_EXPECT_EQ("cancelled", reusable_job(report, "call / nested / leaf").result);
  IUTEST_EXPECT_EQ("cancelled", reusable_job(report, "call / nested / later").result);
}

IUTEST(ReusableWorkflowTest, WorkflowAndReusableJobConcurrencyAreMaskedDisplayOnlyDiagnostics) {
  ReusableRun test;
  test.event.info.name = "workflow_dispatch";
  test.options.vars = {{"GROUP", "private-group-name"}};
  test.options.secrets = {{"MASK", "private-group-name"}};
  test.workflow("leaf.yml", R"(
on:
  workflow_call:
    inputs:
      label: {type: string}
concurrency:
  group: nested-${{ github.repository }}-${{ inputs.label }}-${{ vars.GROUP }}
  cancel-in-progress: true
jobs:
  leaf:
    runs-on: test-host
    steps:
      - run: echo completed
)");
  const auto report = test.run(R"(
on:
  workflow_dispatch:
    inputs:
      label: {type: string, default: top}
concurrency:
  group: root-${{ github.repository }}-${{ inputs.label }}-${{ vars.GROUP }}
  cancel-in-progress: true
jobs:
  prepare:
    runs-on: test-host
    outputs:
      group: ${{ steps.make.outputs.group }}
    steps:
      - id: make
        run: echo group=ready >> "$GITHUB_OUTPUT"
  call:
    needs: prepare
    uses: ./.github/workflows/leaf.yml
    permissions: {contents: read}
    strategy:
      matrix:
        target: [one, two]
    concurrency:
      group: caller-${{ needs.prepare.outputs.group }}-${{ matrix.target }}-${{ strategy.job-index }}-${{ vars.GROUP }}
      cancel-in-progress: ${{ matrix.target == 'one' }}
    with:
      label: ${{ matrix.target }}
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ(5u, report.workflows.front().jobs.size());
  const auto output = test.output.str();
  for (const auto& expected : {
           "group=root-fixture-owner/hello-top-***, cancel-in-progress=true (display only; not enforced)",
           "group=nested-fixture-owner/hello-one-***, cancel-in-progress=true (display only; not enforced)",
           "group=nested-fixture-owner/hello-two-***, cancel-in-progress=true (display only; not enforced)",
           "group=caller-ready-one-0-***, cancel-in-progress=true (display only; not enforced)",
           "group=caller-ready-two-1-***, cancel-in-progress=false (display only; not enforced)",
           "'permissions' are not emulated; explicitly supplied tokens retain their original permissions"}) {
    IUTEST_EXPECT_NE(std::string::npos, output.find(expected)) << output;
  }
  IUTEST_EXPECT_EQ(std::string::npos, output.find("private-group-name"));
}

IUTEST(ReusableWorkflowTest, InvalidWorkflowAndReusableCallerConcurrencyFailBeforeExecution) {
  for (const auto& concurrency : {"concurrency:\n  group: ${{ fromJSON(vars.BAD) }}\n",
                                  "concurrency:\n  group: valid\n  cancel-in-progress: ${{ vars.BAD }}\n"}) {
    ReusableRun test;
    test.options.vars = {{"BAD", "12"}};
    test.workflow("leaf.yml", kSimple);
    const auto report = test.run(std::string(concurrency) + call("./.github/workflows/leaf.yml"));
    IUTEST_EXPECT_EQ("failure", report.result);
    IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "(workflow)").reason.find("concurrency."));
    IUTEST_EXPECT_EQ(1u, report.workflows.front().jobs.size());
  }
  ReusableRun test;
  test.options.vars = {{"BAD", "12"}};
  test.workflow("leaf.yml", kSimple);
  const auto report = test.run(call("./.github/workflows/leaf.yml",
                                  "    concurrency:\n      group: ${{ fromJSON(vars.BAD) }}\n"));
  IUTEST_EXPECT_EQ("failure", report.result);
  IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("concurrency.group"));
  IUTEST_EXPECT_EQ(1u, report.workflows.front().jobs.size());
}

IUTEST(ReusableWorkflowTest, InvalidNestedWorkflowConcurrencyFailsCaller) {
  ReusableRun test;
  test.workflow("leaf.yml", R"(
on:
  workflow_call:
    inputs:
      group: {type: string}
concurrency: ${{ inputs.group }}
jobs:
  leaf:
    runs-on: test-host
    steps:
      - run: exit 1
)");
  const auto report = test.run(call("./.github/workflows/leaf.yml", "    with: {group: ''}\n"));
  IUTEST_EXPECT_EQ("failure", report.result);
  IUTEST_EXPECT_NE(std::string::npos, reusable_job(report, "call").reason.find("concurrency.group"));
  IUTEST_EXPECT_EQ(1u, report.workflows.front().jobs.size());
}

IUTEST(ReusableWorkflowTest, MatrixOutputsUseLastSuccessfulNonEmptyValue) {
  ReusableRun test;
  test.workflow("leaf.yml", R"(
on:
  workflow_call:
    inputs:
      mode: {type: string}
    outputs:
      answer:
        value: ${{ jobs.leaf.outputs.answer }}
jobs:
  leaf:
    runs-on: test-host
    outputs:
      answer: ${{ steps.make.outputs.answer }}
    steps:
      - id: make
        run: |
          case '${{ inputs.mode }}' in
            first) echo answer=kept >> "$GITHUB_OUTPUT" ;;
            second) sleep 0.15; echo answer=kept >> "$GITHUB_OUTPUT" ;;
            empty) sleep 0.3; echo answer= >> "$GITHUB_OUTPUT" ;;
            failed) sleep 0.45; echo answer=failed >> "$GITHUB_OUTPUT"; exit 1 ;;
          esac
)");
  const auto report = test.run(call("./.github/workflows/leaf.yml", R"(
    strategy:
      fail-fast: false
      matrix:
        mode: [first, second, empty, failed]
    with:
      mode: ${{ matrix.mode }}
  verify:
    if: always()
    needs: call
    runs-on: test-host
    steps:
      - run: test '${{ needs.call.outputs.answer }}' = kept
)"));
  IUTEST_EXPECT_EQ("failure", report.result) << test.output.str();
  IUTEST_EXPECT_EQ("success", reusable_job(report, "verify").result) << test.output.str();
}
