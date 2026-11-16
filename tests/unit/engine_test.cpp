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
#include "util/strings.hpp"
#include "workflow/parser.hpp"
#include "workflow/scopes.hpp"

using namespace cpa;
using namespace cpa::engine;

namespace {

class EngineRun {
 public:
  ~EngineRun() { cpa_test::remove_retained_workspaces(options.run_dir); }
  EngineRun() : console(output, false) {
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
    event.info.payload = expr::Value::parse_json(R"({"custom":"payload"})");
  }

  void load(const std::string& text) {
    file.display = ".github/workflows/test.yml";
    file.path = git.root / file.display;
    file.workflow = std::make_shared<workflow::Workflow>(
        workflow::parse_workflow(yaml::load(text, file.display), file.display));
  }

  std::vector<Selection> selection() const { return {{&file, {true, "test"}}}; }

  void setup_steps(const std::string& text) {
    const auto path = dir / "config.yml";
    util::write_file(path, text);
    const auto config = config::load_config(path);
    options.setup_steps = config.setup_steps;
  }

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

IUTEST(EngineTest, ArtifactDirectoryRejectsProtectedPathsAndFiles) {
  for (const auto* name : {"jobs/nested", "control", "snapshot", "invalid-file"}) {
    EngineRun test;
    test.options.artifact_dir = test.options.run_dir / name;
    if (std::string(name) == "invalid-file") {
      test.options.artifact_dir = test.dir / name;
      util::write_file(test.options.artifact_dir, "preserve");
    }
    IUTEST_EXPECT_THROW(test.run(R"(
on: push
jobs:
  test:
    runs-on: test-host
    steps:
      - run: "true"
)"), std::exception);
    if (std::string(name) == "invalid-file") {
      IUTEST_EXPECT_EQ("preserve", util::read_file(test.options.artifact_dir));
    }
  }
}

IUTEST(EngineTest, CustomArtifactDirectorySupportsUploadDownloadAndPersistsAfterFailure) {
  for (const bool custom : {false, true}) {
    EngineRun test;
    const auto root = custom ? test.dir / "export" : test.options.run_dir / "artifacts";
    if (custom) {
      test.options.artifact_dir = root;
      fs::create_directories(root);
      util::write_file(root / "existing", "preserve");
    }
    const auto report = test.run(R"yaml(
on: push
jobs:
  upload:
    runs-on: test-host
    steps:
      - run: echo payload > output.txt
      - uses: actions/upload-artifact@v4
        with:
          name: build
          path: output.txt
  download:
    needs: upload
    runs-on: test-host
    steps:
      - uses: actions/download-artifact@v4
        with:
          name: build
          path: received
      - run: test "$(cat received/output.txt)" = payload
      - run: exit 7
)yaml");
    IUTEST_EXPECT_EQ("failure", report.result) << test.output.str();
    const auto file = root / report.workflows.front().run_id / "build/output.txt";
    IUTEST_EXPECT_EQ("payload\n", util::read_file(file));
    IUTEST_EXPECT_FALSE(fs::exists(test.options.run_dir / "jobs"));
    IUTEST_EXPECT_NE(std::string::npos, test.output.str().find("Artifact saved to: " + file.parent_path().string()));
    if (custom) {
      IUTEST_EXPECT_EQ("preserve", util::read_file(root / "existing"));
      IUTEST_EXPECT_FALSE(fs::exists(test.options.run_dir / "artifacts"));
    }
  }
}

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

IUTEST(EngineTest, GradleHomeDefaultsToEachJobHome) {
  EngineRun test;
  test.setup_steps(R"yaml(
setup-steps:
  - run: test "$GRADLE_USER_HOME" = "$HOME/.gradle"
)yaml");
  const auto report = test.run(R"yaml(
on: push
jobs:
  test:
    runs-on: test-host
    strategy:
      matrix:
        instance: [first, second]
    steps:
      - run: |
          test "$GRADLE_USER_HOME" = "$HOME/.gradle"
          test ! -e "$GRADLE_USER_HOME/marker"
          mkdir -p "$GRADLE_USER_HOME"
          echo '${{ matrix.instance }}' > "$GRADLE_USER_HOME/marker"
      - run: test "$(cat "$GRADLE_USER_HOME/marker")" = '${{ matrix.instance }}'
)yaml");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
}

#if defined(__APPLE__)
IUTEST(EngineTest, CreatesMacOSJobPreferencesDirectory) {
  EngineRun test;
  const auto report = test.run(R"yaml(
on: push
jobs:
  test:
    runs-on: test-host
    steps:
      - run: test -d "$HOME/Library/Preferences"
)yaml");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
}
#endif

IUTEST(EngineTest, GradleHomeExplicitOverridesArePreserved) {
  EngineRun test;
  test.options.pass_env = {"GRADLE_USER_HOME=/passed/gradle"};
  const auto report = test.run(R"yaml(
on: push
jobs:
  passed:
    runs-on: test-host
    steps:
      - run: test "$GRADLE_USER_HOME" = /passed/gradle
  layered:
    runs-on: test-host
    env:
      GRADLE_USER_HOME: /job/gradle
    steps:
      - run: test "$GRADLE_USER_HOME" = /job/gradle
      - env:
          GRADLE_USER_HOME: /step/gradle
        run: test "$GRADLE_USER_HOME" = /step/gradle
      - run: echo "GRADLE_USER_HOME=$HOME/custom-gradle" >> "$GITHUB_ENV"
      - run: test "$GRADLE_USER_HOME" = "$HOME/custom-gradle"
)yaml");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();

  EngineRun workflow_test;
  workflow_test.options.pass_env = {"GRADLE_USER_HOME=/passed/gradle"};
  const auto workflow_report = workflow_test.run(R"yaml(
on: push
env:
  GRADLE_USER_HOME: /workflow/gradle
jobs:
  test:
    runs-on: test-host
    steps:
      - run: test "$GRADLE_USER_HOME" = /workflow/gradle
)yaml");
  IUTEST_EXPECT_EQ("success", workflow_report.result) << workflow_test.output.str();
}

IUTEST(EngineTest, JavaHomeOptionsAreAppendedAfterEnvironmentOverrides) {
  EngineRun test;
  test.options.pass_env = {"JDK_JAVA_OPTIONS=-Dprobe=passed",
                          "JAVA_TOOL_OPTIONS=-Dtool=preserved"};
  test.setup_steps(R"yaml(
setup-steps:
  - env:
      JDK_JAVA_OPTIONS: -Dprobe=setup
    run: |
      expected="-Dprobe=setup \"-Duser.home=$HOME\" \"-Dmaven.repo.local=$HOME/.m2/repository\" \"-Djava.io.tmpdir=$TMPDIR\""
      test "$JDK_JAVA_OPTIONS" = "$expected"
)yaml");
  const auto report = test.run(R"yaml(
on: push
jobs:
  passed:
    runs-on: test-host
    steps:
      - run: |
          expected="-Dprobe=passed \"-Duser.home=$HOME\" \"-Dmaven.repo.local=$HOME/.m2/repository\" \"-Djava.io.tmpdir=$TMPDIR\""
          test "$JDK_JAVA_OPTIONS" = "$expected"
          test "$JAVA_TOOL_OPTIONS" = "-Dtool=preserved"
  layered:
    runs-on: test-host
    env:
      JDK_JAVA_OPTIONS: -Dprobe=job
    steps:
      - run: |
          expected="-Dprobe=job \"-Duser.home=$HOME\" \"-Dmaven.repo.local=$HOME/.m2/repository\" \"-Djava.io.tmpdir=$TMPDIR\""
          test "$JDK_JAVA_OPTIONS" = "$expected"
      - env:
          JDK_JAVA_OPTIONS: -Dprobe=step
        run: |
          expected="-Dprobe=step \"-Duser.home=$HOME\" \"-Dmaven.repo.local=$HOME/.m2/repository\" \"-Djava.io.tmpdir=$TMPDIR\""
          test "$JDK_JAVA_OPTIONS" = "$expected"
      - run: echo 'JDK_JAVA_OPTIONS=-Dprobe=file' >> "$GITHUB_ENV"
      - run: |
          expected="-Dprobe=file \"-Duser.home=$HOME\" \"-Dmaven.repo.local=$HOME/.m2/repository\" \"-Djava.io.tmpdir=$TMPDIR\""
          test "$JDK_JAVA_OPTIONS" = "$expected"
)yaml");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();

  EngineRun workflow_test;
  const auto workflow_report = workflow_test.run(R"yaml(
on: push
env:
  JDK_JAVA_OPTIONS: -Dprobe=workflow
jobs:
  test:
    runs-on: test-host
    steps:
      - run: |
          expected="-Dprobe=workflow \"-Duser.home=$HOME\" \"-Dmaven.repo.local=$HOME/.m2/repository\" \"-Djava.io.tmpdir=$TMPDIR\""
          test "$JDK_JAVA_OPTIONS" = "$expected"
)yaml");
  IUTEST_EXPECT_EQ("success", workflow_report.result) << workflow_test.output.str();
}

IUTEST(EngineTest, JavaAndMavenHomesAndTempAreIsolatedForEachJob) {
  const auto host = util::current_environment();
  const auto path = host.find("PATH");
  auto java = util::find_executable("java", path == host.end() ? "" : path->second);
#if defined(__APPLE__)
  const auto java_home = util::capture({"/usr/libexec/java_home"});
  if (java_home.exit_code == 0) java = fs::path(util::trim(java_home.out)) / "bin/java";
#endif
  if (!java || util::capture({java->string(), "--source", "11", "-version"}).exit_code != 0)
    IUTEST_SKIP() << "No host Java 11+ source launcher";
  for (const bool sandbox : {false, true}) {
    if (sandbox && !host_is_macos()) continue;
    EngineRun test;
    test.options.sandbox.enabled = sandbox;
    if (sandbox) test.options.sandbox.allow_read.push_back(java->parent_path().parent_path());
    test.options.sandbox.allow_unity_step = {".github/workflows/test.yml::test::unity"};
    test.options.run_dir = test.dir / "java home 'and \"quotes\"";
    test.options.pass_env = {"JAVA=" + java->string(),
                            "JDK_JAVA_OPTIONS=-Dprobe=preserved -Duser.home=/host/java "
                            "-Dmaven.repo.local=/host/maven -Djava.io.tmpdir=/host/tmp"};
    test.setup_steps(R"yaml(
setup-steps:
  - run: |
      cat > Probe.java <<'JAVA'
      import java.nio.file.*;
      class Probe {
        public static void main(String[] args) throws Exception {
          Path home = Path.of(System.getenv("HOME"));
          Path repository = Path.of(System.getProperty("maven.repo.local"));
          Path temporary = Path.of(System.getenv("TMPDIR"));
          if (!System.getProperty("user.home").equals(home.toString())
              || !repository.equals(home.resolve(".m2/repository"))
              || !Path.of(System.getProperty("java.io.tmpdir")).equals(temporary)
              || !System.getProperty("probe").equals("preserved")) {
            throw new AssertionError("Java/Maven home and temp properties were not isolated");
          }
          Path tempDirectory = Files.createTempDirectory("java-probe-");
          Path tempFile = Files.createTempFile("java-probe-", ".tmp");
          if (!Files.isSameFile(temporary, tempDirectory.getParent())
              || !Files.isSameFile(temporary, tempFile.getParent())) {
            throw new AssertionError("Java temporary files escaped the job directory");
          }
          Files.delete(tempFile);
          Files.delete(tempDirectory);
          Path marker = repository.resolve("marker");
          if (args[0].equals("write")) {
            Files.createDirectories(repository);
            Files.writeString(marker, args[1], StandardOpenOption.CREATE_NEW);
          } else if (!Files.readString(marker).equals(args[1])) {
            throw new AssertionError("Maven repository was not shared within the job");
          }
        }
      }
      JAVA
)yaml");
    const auto report = test.run(R"yaml(
on: push
jobs:
  test:
    runs-on: test-host
    strategy:
      matrix:
        instance: [first, second]
    steps:
      - run: '"$JAVA" Probe.java write ${{ matrix.instance }}'
      - run: '"$JAVA" Probe.java read ${{ matrix.instance }}'
      - id: unity
        run: '"$JAVA" Probe.java read ${{ matrix.instance }}'
)yaml");
    IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  }
}

IUTEST(EngineTest, JobsHaveIndependentWritableGitConfiguration) {
  EngineRun test;
  test.options.pass_env = {"GIT_CONFIG_GLOBAL=/host/global", "GIT_CONFIG_SYSTEM=/host/system",
                          "GIT_CONFIG_NOSYSTEM=0", "GIT_CONFIG=/host/config"};
  const auto report = test.run(R"yaml(
on: push
jobs:
  first:
    runs-on: test-host
    env:
      GIT_CONFIG_GLOBAL: /workflow/global
      GIT_CONFIG_SYSTEM: /workflow/system
      GIT_CONFIG_NOSYSTEM: "0"
    steps:
      - run: |
          test "$GIT_CONFIG_GLOBAL" = "$HOME/.gitconfig"
          test "$GIT_CONFIG_SYSTEM" = /dev/null
          test "$GIT_CONFIG_NOSYSTEM" = 1
          test "${GIT_CONFIG-unset}" = unset
          test "$(git config --global --get safe.directory)" = '*'
          test "$(git config --global --get filter.lfs.process)" = 'git-lfs filter-process'
          git config --global runner.job first
          git config --global user.name workflow-author
      - run: |
          test "$(git config --global --get runner.job)" = first
          test "$(git config --global --get user.name)" = workflow-author
          test "$(git -c runner.job=explicit config --get runner.job)" = explicit
      - env:
          GIT_CONFIG_COUNT: "1"
          GIT_CONFIG_KEY_0: runner.job
          GIT_CONFIG_VALUE_0: workflow-explicit
        run: test "$(git config --get runner.job)" = workflow-explicit
  second:
    needs: first
    runs-on: test-host
    steps:
      - run: |
          test "$(git config --global --get filter.lfs.required)" = true
          if git config --global --get runner.job; then exit 1; fi
          if git config --global --get user.name; then exit 1; fi
          git config --global runner.job second
)yaml");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
}

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
          CPP_ACTIONS: overridden
        run: |
          test "$SHARED" = step
          test "$WORKFLOW_ONLY" = workflow-only
          test "$FROM_VAR" = hello
          test "$PASSED" = from-host
          test "$GITHUB_ACTIONS" = true
          test "$CPP_ACTIONS" = true
          test -n "$CPP_ACTIONS_VERSION"
          test "$GITHUB_ACTOR" = fixture-user
          test "$GITHUB_REPOSITORY" = fixture-owner/hello
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

IUTEST(EngineTest, EnvironmentOverridesSecretsAndVariablesInJobAndWorkflowEnv) {
  EngineRun test;
  test.options.vars = {{"VALUE", "repository"}, {"ENVIRONMENT", "Testing"}};
  test.options.secrets = {{"TOKEN", "repository-value"}};
  const auto secret_file = test.dir / "environment.env";
  const auto vars_file = test.dir / "variables.env";
  util::write_file(secret_file, "TOKEN=environment-value\n");
  util::write_file(vars_file, "VALUE=from-file\nFILE_ONLY=present\n");
  config::EnvironmentConfig environment;
  environment.secrets_file = secret_file.string();
  environment.vars_file = vars_file.string();
  environment.vars = {{"VALUE", "inline"}};
  test.options.environments["testing"] = environment;
  const auto report = test.run(R"(
on: push
env:
  WORKFLOW_VALUE: ${{ vars.VALUE }}
  WORKFLOW_TOKEN: ${{ secrets.TOKEN }}
jobs:
  scoped:
    runs-on: test-host
    environment:
      name: ${{ vars.ENVIRONMENT }}
      url: ${{ steps.deploy.outputs.url }}
    env:
      VALUE: ${{ vars.VALUE }}
      TOKEN: ${{ secrets.TOKEN }}
    steps:
      - run: |
          test "$VALUE" = inline
          test "$WORKFLOW_VALUE" = inline
          test "$TOKEN" = "$WORKFLOW_TOKEN"
          test "${#TOKEN}" = 17
          test '${{ vars.FILE_ONLY }}' = present
          printf '%s\n' "$TOKEN"
  unscoped:
    runs-on: test-host
    steps:
      - run: |
          test '${{ vars.VALUE }}' = repository
          test '${{ vars.FILE_ONLY }}' = ''
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_EQ(std::string::npos, test.output.str().find("environment-value"));
}

IUTEST(EngineTest, ConcurrencyAndPermissionsAreDiagnosticOnly) {
  EngineRun test;
  const auto report = test.run(R"(
on: push
permissions: {}
concurrency:
  group: ${{ github.workflow }}-global
  cancel-in-progress: true
jobs:
  scoped:
    runs-on: test-host
    permissions: {contents: read}
    concurrency:
      group: build-${{ matrix.target }}
      cancel-in-progress: ${{ matrix.cancel }}
    strategy:
      matrix:
        include:
          - {target: one, cancel: true}
          - {target: two, cancel: false}
    steps:
      - run: 'true'
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_NE(std::string::npos, test.output.str().find("group=build-one, cancel-in-progress=true"));
  IUTEST_EXPECT_NE(std::string::npos, test.output.str().find("group=build-two, cancel-in-progress=false"));
  IUTEST_EXPECT_NE(std::string::npos, test.output.str().find("(display only; not enforced)"));
  IUTEST_EXPECT_NE(std::string::npos, test.output.str().find("'permissions' are not emulated"));
  IUTEST_EXPECT_NE(std::string::npos, test.output.str().find("tokens retain their original permissions"));
  IUTEST_EXPECT_EQ(2U, report.workflows.front().jobs.size());
}

IUTEST(EngineTest, DynamicMatrixAndRunnerLabelsUseNeedsOutputs) {
  EngineRun test;
  test.options.host_labels.push_back("custom-builder");
  const auto report = test.run(R"(
on: push
jobs:
  prepare:
    runs-on: test-host
    outputs:
      matrix: ${{ steps.set.outputs.matrix }}
      runner: ${{ steps.set.outputs.runner }}
    steps:
      - id: set
        run: |
          echo 'matrix={"include":[{"target":"one"},{"target":"two"}]}' >> "$GITHUB_OUTPUT"
          echo 'runner=["self-hosted","custom-builder"]' >> "$GITHUB_OUTPUT"
  build:
    needs: prepare
    strategy:
      matrix: ${{ fromJSON(needs.prepare.outputs.matrix) }}
    runs-on: ${{ fromJSON(needs.prepare.outputs.runner) }}
    outputs:
      target: ${{ matrix.target }}
    steps:
      - run: test '${{ strategy.job-total }}' = 2
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  std::vector<std::string> targets;
  for (const auto& item : report.workflows.front().jobs) {
    if (item.job_id == "build") targets.push_back(item.outputs.at("target"));
  }
  std::sort(targets.begin(), targets.end());
  IUTEST_EXPECT_EQ((std::vector<std::string>{"one", "two"}), targets);
}

IUTEST(EngineTest, InvalidDynamicMatrixFailsBeforeRunningSteps) {
  for (const auto& invalid : {std::string("not-json"), std::string("[]")}) {
    EngineRun test;
    test.options.vars = {{"MATRIX", invalid}};
    const auto report = test.run(R"(
on: push
jobs:
  build:
    strategy:
      matrix: ${{ fromJSON(vars.MATRIX) }}
    runs-on: test-host
    steps:
      - run: echo should-not-run
)");
    IUTEST_EXPECT_EQ("failure", report.result);
    IUTEST_EXPECT_TRUE(job(report, "build").steps.empty());
  }
}

IUTEST(EngineTest, ConcurrencyExpressionsAreTyped) {
  const auto document = yaml::load(R"(
group: build-${{ vars.BRANCH }}
cancel-in-progress: ${{ vars.CANCEL == 'yes' }}
)", "test.yml");
  expr::EvalContext context;
  context.contexts = expr::Value::parse_json(R"({"vars":{"BRANCH":"main","CANCEL":"yes"}})");
  IUTEST_EXPECT_EQ("Concurrency: group=build-main, cancel-in-progress=true (display only; not enforced)",
                   describe_concurrency(document->root, context, workflow::scope_for(workflow::Site::JobGeneral),
                                        "test.yml"));
  for (const auto& text : {"group: ''", "group: build\ncancel-in-progress: 'false'", "group: [build]"}) {
    const auto invalid = yaml::load(text, "test.yml");
    IUTEST_EXPECT_THROW(
        describe_concurrency(invalid->root, context, workflow::scope_for(workflow::Site::JobGeneral), "test.yml"),
        EvaluationError);
  }
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

IUTEST(EngineTest, IgnoreInteractiveShellInDefaultsAndNestedComposites) {
  EngineRun test;
  IUTEST_EXPECT_FALSE(test.options.ignore_interactive_shell);
  test.options.ignore_interactive_shell = true;
  util::write_file(test.git.root / ".github/actions/outer/action.yml", R"(
name: outer
description: outer
runs:
  using: composite
  steps:
    - shell: bash --noprofile --norc --interactive -e {0}
      run: |
        case "$-" in *i*) exit 11 ;; esac
        case "$-" in *e*) ;; *) exit 12 ;; esac
    - uses: ./.github/actions/inner
)");
  util::write_file(test.git.root / ".github/actions/inner/action.yml", R"yaml(
name: inner
description: inner
runs:
  using: composite
  steps:
    - shell: bash --noprofile --norc -ieo pipefail {0}
      run: |
        case "$-" in *i*) exit 13 ;; esac
        case "$-" in *e*) ;; *) exit 14 ;; esac
        test "$(set -o | awk '$1 == "pipefail" {print $2}')" = on
)yaml");
  const auto report = test.run(R"(
on: push
defaults:
  run:
    shell: bash --noprofile --norc -ieo pipefail {0}
jobs:
  workflow-default:
    runs-on: test-host
    steps:
      - run: |
          case "$-" in *i*) exit 15 ;; esac
          case "$-" in *e*) ;; *) exit 16 ;; esac
      - uses: actions/checkout@v4
      - uses: ./.github/actions/outer
      - shell: bash
        run: echo NON_INTERACTIVE_UNCHANGED
  job-default:
    runs-on: test-host
    defaults:
      run:
        shell: sh -ie {0}
    steps:
      - run: |
          case "$-" in *i*) exit 17 ;; esac
          case "$-" in *e*) ;; *) exit 18 ;; esac
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_EXPECT_NE(std::string::npos, test.output.str().find("Interactive shell options ignored"));
  IUTEST_EXPECT_NE(std::string::npos, test.output.str().find("NON_INTERACTIVE_UNCHANGED"));
}

IUTEST(EngineTest, SetupStepsRunPerJobAndPersistEnvironmentPathAndLoginProfile) {
  EngineRun test;
  test.setup_steps(R"yaml(
setup-steps:
  - id: prepare
    name: Configure local tools
    env: {SETUP_VALUE: ready}
    run: |
      test ! -e "$HOME/setup-once"
      touch "$HOME/setup-once"
      test "$PWD" = "$GITHUB_WORKSPACE"
      test "$SETUP_VALUE" = ready
      case "$-" in *i*) exit 31 ;; esac
      test "$(set -o | awk '$1 == "pipefail" {print $2}')" = on
      mkdir -p "$HOME/bin"
      printf '#!/bin/sh\necho LOCAL_TOOL_READY\n' > "$HOME/bin/setup-tool"
      chmod +x "$HOME/bin/setup-tool"
      echo "$HOME/bin" >> "$GITHUB_PATH"
      echo SETUP_RESULT=ready >> "$GITHUB_ENV"
      printf 'export PATH="$HOME/bin:$PATH"\n' > "$HOME/.bash_profile"
      echo 'value=setup-output' >> "$GITHUB_OUTPUT"
      echo '# Setup summary' >> "$GITHUB_STEP_SUMMARY"
  - name: Verify setup state
    run: |
      test "$SETUP_RESULT" = ready
      test "${{ steps.prepare.outputs.value }}" = setup-output
      setup-tool
)yaml");
  const auto report = test.run(R"yaml(
on: push
defaults:
  run:
    shell: missing-shell {0}
    working-directory: nonexistent
jobs:
  build:
    runs-on: test-host
    strategy:
      matrix:
        variant: [one, two]
    steps:
      - id: prepare
        shell: bash -leo pipefail {0}
        working-directory: ${{ github.workspace }}
        run: |
          test "$SETUP_RESULT" = ready
          test -e "$HOME/setup-once"
          test -z "${{ steps.prepare.outputs.value }}"
          setup-tool
)yaml");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  IUTEST_ASSERT_EQ(2u, report.workflows.front().jobs.size());
  for (const auto& run : report.workflows.front().jobs) {
    IUTEST_ASSERT_EQ(3u, run.steps.size());
    IUTEST_EXPECT_EQ("cpp-actions setup / Configure local tools", run.steps.front().name);
    IUTEST_EXPECT_EQ("__cpp_actions_setup_prepare", run.steps.front().id);
    IUTEST_EXPECT_EQ("success", step(run, "prepare").outcome);
    IUTEST_EXPECT_NE(std::string::npos, run.summary.find("# Setup summary"));
  }
}

IUTEST(EngineTest, SetupFailureSkipsWorkflowEvenAlwaysStepsAndRecordsFailure) {
  EngineRun test;
  test.setup_steps(R"(
setup-steps:
  - name: Failing setup
    run: exit 7
  - name: Unreachable setup
    run: echo SHOULD_NOT_EXECUTE_SETUP
)");
  const auto report = test.run(R"(
on: push
jobs:
  build:
    runs-on: test-host
    steps:
      - id: normal
        name: Normal workflow step
        run: echo SHOULD_NOT_EXECUTE_WORKFLOW
      - id: cleanup
        name: Always workflow step
        if: always()
        run: echo SHOULD_NOT_EXECUTE_CLEANUP
)");
  IUTEST_EXPECT_EQ("failure", report.result) << test.output.str();
  const auto& run = job(report, "build");
  IUTEST_ASSERT_EQ(4u, run.steps.size());
  IUTEST_EXPECT_EQ("failure", run.steps.front().outcome);
  IUTEST_EXPECT_EQ("skipped", run.steps[1].outcome);
  IUTEST_EXPECT_EQ("skipped", step(run, "normal").outcome);
  IUTEST_EXPECT_EQ("skipped", step(run, "cleanup").outcome);
  IUTEST_EXPECT_EQ(std::string::npos, test.output.str().find("SHOULD_NOT_EXECUTE"));
}

IUTEST(EngineTest, SetupStepsHonorTimeoutAndSkippedJobs) {
  EngineRun test;
  test.setup_steps(R"(
setup-steps:
  - name: Setup timeout
    timeout-minutes: 0.001
    run: sleep 10
)");
  const auto report = test.run(R"(
on: push
jobs:
  build:
    runs-on: test-host
    steps:
      - id: normal
        run: echo SHOULD_NOT_EXECUTE
  skipped:
    if: false
    runs-on: test-host
    steps:
      - run: echo SHOULD_NOT_EXECUTE
)");
  IUTEST_EXPECT_EQ("failure", report.result) << test.output.str();
  const auto& run = job(report, "build");
  IUTEST_ASSERT_EQ(2u, run.steps.size());
  IUTEST_EXPECT_EQ("failure", run.steps.front().outcome);
  IUTEST_EXPECT_EQ("skipped", step(run, "normal").outcome);
  IUTEST_EXPECT_EQ("skipped", job(report, "skipped").result);
  IUTEST_EXPECT_TRUE(job(report, "skipped").steps.empty());
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
          printf '%s\n' "$GITHUB_WORKSPACE" > "$STARTED"
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
  const fs::path workspace(util::trim(util::read_file(marker)));
  IUTEST_EXPECT_FALSE(fs::exists(workspace.parent_path().parent_path().parent_path()));
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

IUTEST(EngineTest, Base64SecretsAreMaskedAcrossLogsAndReports) {
  EngineRun test;
  test.options.secrets = {{"TOKEN", "gho_dummy_token"}};
  test.options.github_token = "github-dummy-token";
  test.options.debug = true;
  const auto report = test.run(R"YAML(
on: push
jobs:
  run:
    runs-on: test-host
    env:
      TOKEN: ${{ secrets.TOKEN }}
      GH_TOKEN: ${{ github.token }}
    outputs:
      public: ${{ steps.emit.outputs.public }}
      encoded: ${{ steps.emit.outputs.encoded }}
      basic: ${{ steps.emit.outputs.basic }}
    steps:
      - id: emit
        run: |
          encoded="$(printf '%s' "$TOKEN" | base64 | tr -d '\r\n')"
          basic="$(printf 'build-user:%s' "$TOKEN" | base64 | tr -d '\r\n')"
          github_encoded="$(printf '%s' "$GH_TOKEN" | base64 | tr -d '\r\n')"
          dynamic="$(printf '%s%s' runtime- secret-value)"
          echo "::add-mask::$dynamic"
          dynamic_encoded="$(printf '%s' "$dynamic" | base64 | tr -d '\r\n')"
          printf 'Authorization: Basic %s\n' "$basic"
          printf '%s\n' "$encoded" "$dynamic_encoded"
          printf '%s\n' "$github_encoded" >&2
          echo "::debug::$encoded"
          echo "::warning file=$encoded,title=$github_encoded::$dynamic_encoded"
          printf 'public=ok\nencoded=%s\nbasic=%s\n' "$encoded" "$basic" >> "$GITHUB_OUTPUT"
          printf '# Encoded %s / %s / %s / %s\n' "$encoded" "$basic" "$github_encoded" "$dynamic_encoded" >> "$GITHUB_STEP_SUMMARY"
      - run: |
          expected="$(printf '%s' "$TOKEN" | base64 | tr -d '\r\n')"
          test '${{ steps.emit.outputs.encoded }}' = "$expected"
          expected="$(printf 'build-user:%s' "$TOKEN" | base64 | tr -d '\r\n')"
          test '${{ steps.emit.outputs.basic }}' = "$expected"
)YAML");
  IUTEST_ASSERT_EQ("success", report.result) << test.output.str();
  const auto& run = job(report, "run");
  IUTEST_EXPECT_EQ("ok", run.outputs.at("public"));
  IUTEST_EXPECT_EQ(0u, run.outputs.count("encoded"));
  IUTEST_EXPECT_EQ(0u, run.outputs.count("basic"));
  IUTEST_EXPECT_EQ("***", step(run, "emit").outputs.at("encoded"));
  IUTEST_EXPECT_EQ("YnVpbGQtdXNlcjpn***", step(run, "emit").outputs.at("basic"));
  IUTEST_ASSERT_EQ(1u, run.annotations.size());
  IUTEST_EXPECT_EQ("***", run.annotations[0].message);
  IUTEST_EXPECT_EQ("***", run.annotations[0].title);
  IUTEST_EXPECT_EQ("***", run.annotations[0].file);
  IUTEST_EXPECT_EQ("# Encoded *** / YnVpbGQtdXNlcjpn*** / *** / ***\n", run.summary);
  const auto log = util::read_file(run.log_file);
  IUTEST_EXPECT_NE(std::string::npos, log.find("Authorization: Basic YnVpbGQtdXNlcjpn***"));
  const auto persisted_report = util::read_file(test.options.run_dir / "report.json");
  const auto summary = util::read_file(test.options.run_dir / "summary.md");
  for (const auto* secret : {"gho_dummy_token", "github-dummy-token", "runtime-secret-value",
                             "Z2hvX2R1bW15X3Rva2Vu", "YnVpbGQtdXNlcjpnaG9fZHVtbXlfdG9rZW4=",
                             "Z2l0aHViLWR1bW15LXRva2Vu", "cnVudGltZS1zZWNyZXQtdmFsdWU="}) {
    IUTEST_EXPECT_EQ(std::string::npos, test.output.str().find(secret));
    IUTEST_EXPECT_EQ(std::string::npos, log.find(secret));
    IUTEST_EXPECT_EQ(std::string::npos, persisted_report.find(secret));
    IUTEST_EXPECT_EQ(std::string::npos, summary.find(secret));
  }
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

#if defined(__APPLE__)
IUTEST(EngineTest, FoundationUsesTheIsolatedJobHome) {
  for (const bool sandbox : {false, true}) {
    EngineRun test;
    test.options.sandbox.enabled = sandbox;
    test.options.pass_env = {"CPA_HOME_PROBE=" CPA_HOME_PROBE};
    if (sandbox) test.options.sandbox.allow_read = {CPA_HOME_PROBE};
    const auto report = test.run(sandbox ? R"yaml(
on: push
jobs:
  test:
    runs-on: test-host
    env:
      HOME: /workflow-home
      CFFIXED_USER_HOME: /workflow-fixed-home
    steps:
      - run: echo "CFFIXED_USER_HOME=/github-env-home" >> "$GITHUB_ENV"
      - env:
          HOME: /step-home
          CFFIXED_USER_HOME: /step-fixed-home
        run: |
          "$CPA_HOME_PROBE" "$HOME"
      - run: |
          "$CPA_HOME_PROBE" "$HOME"
)yaml"
                                                : R"yaml(
on: push
jobs:
  test:
    runs-on: test-host
    steps:
      - run: |
          "$CPA_HOME_PROBE" "$HOME"
)yaml");
    IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  }
}
#endif

IUTEST(EngineTest, ShortWorkDirectoriesArePrivateUniqueAndCleanedOrRetained) {
  for (const bool keep : {false, true}) {
    for (const bool fail : {false, true}) {
      for (const bool sandbox : {false, true}) {
        if (sandbox && !host_is_macos()) continue;
        EngineRun test;
        test.options.keep_workdir = keep;
        test.options.sandbox.enabled = sandbox;
        test.options.run_dir = test.dir / std::string(120, 'r');
        test.git.github = workspace::GitHubRepo{"demo", "example-build-workflow"};
        const auto report = test.run(R"yaml(
on: push
jobs:
  test:
    runs-on: test-host
    strategy:
      fail-fast: false
      matrix:
        instance: [first, second]
    outputs:
      workspace: ${{ steps.path.outputs.workspace }}
      temp: ${{ steps.path.outputs.temp }}
    steps:
      - id: path
        run: |
          test "$PWD" = "$GITHUB_WORKSPACE"
          test "$RUNNER_WORKSPACE" = "$(dirname "$GITHUB_WORKSPACE")"
          echo "workspace=$GITHUB_WORKSPACE" >> "$GITHUB_OUTPUT"
          echo "temp=$RUNNER_TEMP" >> "$GITHUB_OUTPUT"
          test ! -f marker
          echo '${{ matrix.instance }}' > marker
          mkdir -p example-client/build
          touch example-client/build/Android-test.symbols.zip
          find "$GITHUB_WORKSPACE/example-client/build" -name 'Android-*.symbols.zip' | head -n 1 | xargs -I{} mv {} ./symbols.zip
          test -f symbols.zip
      - run: test "$(cat marker)" = '${{ matrix.instance }}'
)yaml" + std::string(fail ? "      - run: exit 7\n" : ""));
        IUTEST_ASSERT_EQ(fail ? "failure" : "success", report.result) << test.output.str();
        const auto& jobs = report.workflows.front().jobs;
        IUTEST_ASSERT_EQ(2u, jobs.size());
        std::vector<fs::path> roots;
        for (const auto& item : jobs) {
          const fs::path workspace(step(item, "path").outputs.at("workspace"));
          const fs::path temp(step(item, "path").outputs.at("temp"));
          const auto root = workspace.parent_path().parent_path().parent_path();
          roots.push_back(root);
          IUTEST_EXPECT_EQ(util::real_path("/tmp"), root.parent_path());
          IUTEST_EXPECT_EQ(0u, root.filename().string().find("cpa-w-"));
          IUTEST_EXPECT_LT((workspace / "example-client/build").string().size(), 100u);
          IUTEST_EXPECT_LT((workspace / "example-client/build/Android-test.symbols.zip").string().size(), 255u);
          IUTEST_EXPECT_EQ(root / "work/_temp", temp);
          IUTEST_EXPECT_EQ(keep, fs::exists(root));
          IUTEST_EXPECT_TRUE(fs::exists(item.log_file));
          if (keep) {
            IUTEST_EXPECT_EQ(fs::perms::owner_all, fs::status(root).permissions() & fs::perms::all);
            const auto marker = test.options.run_dir / "jobs" / report.workflows.front().run_id /
                                sanitize_key(item.key) / "work-path";
            IUTEST_EXPECT_EQ((root / "work").string(), util::trim(util::read_file(marker)));
            IUTEST_EXPECT_FALSE(fs::is_symlink(root));
          }
        }
        IUTEST_EXPECT_NE(roots[0], roots[1]);
      }
    }
  }
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
  const fs::path work(util::trim(util::read_file(test.options.run_dir / "jobs" / report.workflows[0].run_id /
                                                sanitize_key(run.key) / "work-path")));
  IUTEST_EXPECT_EQ("isolated\n", util::read_file(work / "hello/hello/created.txt"));
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

IUTEST(EngineTest, SecretValuesMatchingSchemaKeysDoNotCorruptReportStructure) {
  EngineRun test;
  test.options.secrets = {{"TOKEN", "outputs"}, {"OTHER", "result"}};
  const auto report = test.run(R"(
on: push
jobs:
  run:
    runs-on: test-host
    steps:
      - id: emit
        run: echo 'public=${{ secrets.TOKEN }}' >> "$GITHUB_OUTPUT"
)");
  IUTEST_EXPECT_EQ("success", report.result) << test.output.str();
  const auto saved = expr::Value::parse_json(util::read_file(test.options.run_dir / "report.json"));
  IUTEST_EXPECT_EQ("success", saved.get("result").to_string());
  const auto saved_job = saved.get("workflows").items()[0].get("jobs").items()[0];
  IUTEST_EXPECT_EQ("success", saved_job.get("result").to_string());
  IUTEST_EXPECT_EQ("***", saved_job.get("steps").items()[0].get("outputs").get("public").to_string());
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

IUTEST(EngineHelpersTest, InteractiveShellOptionsAreUnchangedByDefault) {
  const auto shell = resolve_shell("bash -ileo pipefail {0}", "/usr/bin:/bin");
  IUTEST_EXPECT_EQ((std::vector<std::string>{"-ileo", "pipefail", "{0}"}), shell.args);
  IUTEST_EXPECT_NE(std::string::npos, shell.display.find("-ileo"));
}

IUTEST(EngineHelpersTest, IgnoreInteractiveShellPreservesOtherOptionsAndArguments) {
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
      {"bash -ileo pipefail {0}", {"-leo", "pipefail", "{0}"}},
      {"bash -i -l -e -o pipefail {0}", {"-l", "-e", "-o", "pipefail", "{0}"}},
      {"bash --interactive --login -e {0}", {"--login", "-e", "{0}"}},
      {"bash -ii --interactive {0} -i --interactive", {"{0}", "-i", "--interactive"}},
      {"sh -ie {0}", {"-e", "{0}"}},
      {"bash --init-file -i -i {0}", {"--init-file", "-i", "{0}"}},
      {"bash --rcfile --interactive -i {0}", {"--rcfile", "--interactive", "{0}"}},
      {"bash -i -o ignoreeof -O extglob +o history {0}", {"-o", "ignoreeof", "-O", "extglob", "+o", "history", "{0}"}},
      {"bash -i -- {0} -i", {"--", "{0}", "-i"}},
      {"bash -ic \"echo -i; . {0}\" -i", {"-c", "echo -i; . {0}", "-i"}},
      {"bash -i +i {0}", {"+i", "{0}"}},
  };
  for (const auto& [command, expected] : cases) {
    auto shell = resolve_shell(command, "/usr/bin:/bin");
    IUTEST_EXPECT_TRUE(shell.ignore_interactive()) << command;
    IUTEST_EXPECT_EQ(expected, shell.args) << command;
    IUTEST_EXPECT_FALSE(shell.ignore_interactive()) << command;
    const auto argv = shell.argv("/path with spaces/script.sh");
    IUTEST_EXPECT_EQ(shell.program, argv.front());
    IUTEST_EXPECT_EQ(expected.size() + 1, argv.size());
  }
  auto shell = resolve_shell("bash -ileo pipefail {0}", "/usr/bin:/bin");
  IUTEST_ASSERT_TRUE(shell.ignore_interactive());
  IUTEST_EXPECT_NE(std::string::npos, shell.display.find("-leo pipefail"));
  IUTEST_EXPECT_EQ("/a script.sh", shell.argv("/a script.sh").back());
}

IUTEST(EngineHelpersTest, IgnoreInteractiveShellLeavesOperandsAndOtherExecutablesAlone) {
  for (const auto* command : {"bash", "bash +i {0}", "bash {0} -i", "bash -- {0} -i",
                              "bash -o -i {0}", "bash --rcfile -i {0}",
                              "bash -c \"echo -i; . {0}\" -i"}) {
    auto shell = resolve_shell(command, "/usr/bin:/bin");
    const auto args = shell.args;
    const auto display = shell.display;
    IUTEST_EXPECT_FALSE(shell.ignore_interactive()) << command;
    IUTEST_EXPECT_EQ(args, shell.args);
    IUTEST_EXPECT_EQ(display, shell.display);
  }
  for (const auto* program : {"/usr/bin/python3", "/usr/bin/node", "/usr/bin/pwsh", "/usr/bin/custom-shell"}) {
    Shell shell;
    shell.program = program;
    shell.args = {"-i", "--interactive", "{0}"};
    const auto args = shell.args;
    IUTEST_EXPECT_FALSE(shell.ignore_interactive()) << program;
    IUTEST_EXPECT_EQ(args, shell.args);
  }
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
