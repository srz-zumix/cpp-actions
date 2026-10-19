#include "iutest.hpp"

#include <functional>
#include <sstream>

#include "engine/runner.hpp"
#include "sandbox/profile.hpp"
#include "sandbox/sandbox.hpp"
#include "test_support.hpp"

using namespace cpa;
using namespace cpa::engine;

namespace {

class ActionFixture {
 public:
  cpa_test::TempDir dir;
  std::string log;

  void file(const std::string& name, const std::string& content) {
    util::write_file(dir / ("repo/" + name), content);
  }

  JobReport run(const std::string& body, const std::filesystem::path& node = {},
                const std::string& settings = {},
                const std::function<void(RunOptions&)>& configure = {}) {
    file(".github/workflows/test.yml",
         "on: push\njobs:\n  test:\n    runs-on: action-test-host\n" + settings +
             "    steps:\n      - uses: actions/checkout@v4\n" + body);
    cpa_test::init_repo(dir / "repo", {});
    const auto git = workspace::read_git_info(dir / "repo");
    const auto event = build_event({}, git);
    const auto files = load_workflow_files(git.root, {});
    if (files.size() != 1 || !files.front().valid()) {
      throw std::runtime_error(render_validation(files));
    }
    RunOptions options;
    options.run_dir = dir / "run";
    options.cache_dir = dir / "cache";
    options.host_labels = {"action-test-host"};
    options.sandbox.enabled = false;
    options.node = node;
    options.parallel = 1;
    if (configure) configure(options);
    std::ostringstream out;
    Console console(out, false);
    Runner runner(options, console);
    RunReport report;
    try {
      report = runner.run(select_workflows(files, event.info, {}), event, git);
    } catch (...) {
      log = out.str();
      throw;
    }
    log = out.str();
    if (report.workflows.size() != 1 || report.workflows.front().jobs.size() != 1) {
      throw std::runtime_error("Unexpected Runner report: " + log);
    }
    return report.workflows.front().jobs.front();
  }
};

std::optional<std::filesystem::path> host_node() {
  const auto env = util::current_environment();
  const auto it = env.find("PATH");
  return util::find_executable("node", it == env.end() ? "" : it->second);
}

std::vector<StepReport> posts(const JobReport& job) {
  std::vector<StepReport> result;
  for (const auto& step : job.steps) {
    if (step.post) result.push_back(step);
  }
  return result;
}

}  // namespace

IUTEST(ActionRunnerTest, JavascriptStateEnvironmentInputsOutputsAndLifoPosts) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  f.file("js/action.yml", R"(name: lifecycle
inputs:
  label:
    default: default-label
runs:
  using: node20
  pre: pre.js
  main: main.js
  post: post.js
)");
  f.file("js/pre.js", R"(const fs = require('fs');
const label = process.env.INPUT_LABEL;
if (process.env.STATE_saved) throw Error('state leaked between action instances');
fs.appendFileSync(process.env.GITHUB_STATE, `saved=pre-${label}\n`);
fs.appendFileSync(process.env.GITHUB_ENV, `LIFECYCLE=pre-${label}\nPIN=changed\n`);
)");
  f.file("js/main.js", R"(const fs = require('fs');
const label = process.env.INPUT_LABEL;
if (process.env.STATE_saved !== `pre-${label}` ||
    process.env.LIFECYCLE !== `pre-${label}` || process.env.PIN !== 'pinned')
  throw Error('missing pre state or environment');
if (!process.env.GITHUB_ACTION_PATH.endsWith('/js')) throw Error('action path');
fs.appendFileSync(process.env.GITHUB_STATE, `saved=main-${label}\n`);
fs.appendFileSync(process.env.GITHUB_OUTPUT, `answer=${label}\n`);
)");
  f.file("js/post.js", R"(const fs = require('fs');
const label = process.env.INPUT_LABEL;
if (process.env.STATE_saved !== `main-${label}` ||
    process.env.LIFECYCLE !== 'after' || process.env.PIN !== 'pinned')
  throw Error('missing post state or environment');
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, `post-${label}\n`);
)");
  const auto job = f.run(R"(      - uses: ./js
        id: first
        env: {PIN: pinned}
        with: {LABEL: A}
      - uses: ./js
        id: second
        env: {PIN: pinned}
      - run: |
          test '${{ steps.first.outputs.answer }}' = A
          test '${{ steps.second.outputs.answer }}' = default-label
          echo LIFECYCLE=after >> "$GITHUB_ENV"
          echo PIN=after >> "$GITHUB_ENV"
)", *node, "    env: {PIN: pinned}\n");
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ("post-default-label\npost-A\n", job.summary);
  const auto cleanup = posts(job);
  IUTEST_ASSERT_EQ(2u, cleanup.size());
  IUTEST_EXPECT_EQ("second", cleanup[0].id);
  IUTEST_EXPECT_EQ("first", cleanup[1].id);
}

IUTEST(ActionRunnerTest, DefaultAlwaysPreAndPostCleanUpMainAndPreFailures) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  for (const auto& name : {"main-fails", "pre-fails"}) {
    f.file(std::string(name) + "/action.yml",
           "name: failing\nruns:\n  using: node20\n  pre: pre.js\n  main: main.js\n  post: post.js\n");
    f.file(std::string(name) + "/pre.js",
           "const fs = require('fs');\n"
           "fs.appendFileSync(process.env.GITHUB_STATE, 'saved=pre\\n');\n"
           "if (process.env.GITHUB_ACTION_PATH.endsWith('/pre-fails')) process.exit(1);\n");
    f.file(std::string(name) + "/main.js",
           "require('fs').appendFileSync(process.env.GITHUB_STATE, 'saved=main\\n'); process.exit(1);\n");
    f.file(std::string(name) + "/post.js",
           "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, process.env.STATE_saved + '\\n');\n");
  }
  const auto job = f.run(R"(      - uses: ./main-fails
        id: main
      - uses: ./pre-fails
        if: always()
        id: pre
)", *node);
  IUTEST_EXPECT_EQ("failure", job.result) << f.log;
  IUTEST_EXPECT_EQ("pre\nmain\n", job.summary);
  const auto cleanup = posts(job);
  IUTEST_ASSERT_EQ(2u, cleanup.size());
  IUTEST_EXPECT_EQ("success", cleanup[0].outcome);
  IUTEST_EXPECT_EQ("success", cleanup[1].outcome);
}

IUTEST(ActionRunnerTest, ExplicitFalsePreAndPostConditions) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  f.file("js/action.yml", R"(name: conditions
runs:
  using: node20
  pre: must-not-run.js
  pre-if: 'false'
  main: main.js
  post: must-not-run.js
  post-if: 'false'
)");
  f.file("js/must-not-run.js", "process.exit(1);\n");
  f.file("js/main.js", "if (process.env.STATE_saved) process.exit(1);\n");
  const auto job = f.run("      - uses: ./js\n", *node);
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  const auto cleanup = posts(job);
  IUTEST_ASSERT_EQ(1u, cleanup.size());
  IUTEST_EXPECT_EQ("skipped", cleanup[0].outcome);
}

IUTEST(ActionRunnerTest, NestedCompositeInputsOutputsActionPathsAndEnvironment) {
  ActionFixture f;
  f.file("outer/action.yml", R"(name: outer
inputs:
  message:
    default: default-message
outputs:
  answer:
    value: ${{ steps.inner.outputs.answer }}
runs:
  using: composite
  steps:
    - uses: ./inner
      id: inner
      with:
        message: ${{ inputs.message }}
      env:
        OUTER: ${{ github.action_path }}
    - shell: bash
      run: test '${{ github.action_path }}' = "$GITHUB_ACTION_PATH"
)");
  f.file("inner/action.yml", R"yaml(name: inner
inputs:
  message:
    default: missing
outputs:
  answer:
    value: ${{ steps.emit.outputs.answer }}
runs:
  using: composite
  steps:
    - id: emit
      shell: bash
      run: |
        test '${{ github.action_path }}' = "$GITHUB_ACTION_PATH"
        test "$(basename "$OUTER")" = outer
        test "$(basename "$GITHUB_ACTION_PATH")" = inner
        test "$PIN" = pinned
        echo 'answer=${{ inputs.message }}' >> "$GITHUB_OUTPUT"
        echo PIN=changed >> "$GITHUB_ENV"
    - shell: bash
      run: test "$PIN" = pinned
)yaml");
  const auto job = f.run(R"(      - uses: ./outer
        id: default
        env: {PIN: pinned}
      - uses: ./outer
        id: supplied
        env: {PIN: pinned}
        with: {message: supplied-message}
      - run: |
          test '${{ steps.default.outputs.answer }}' = default-message
          test '${{ steps.supplied.outputs.answer }}' = supplied-message
          test "$PIN" = changed
)", {}, "    env: {PIN: pinned}\n");
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
}

IUTEST(ActionRunnerTest, NestedCompositePostKeepsInputsStepsPathAndFailureStatus) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  f.file("outer/action.yml", R"(name: outer
inputs:
  enabled:
    default: 'yes'
runs:
  using: composite
  steps:
    - uses: ./js
      id: child
    - id: later
      shell: bash
      run: exit 1
)");
  f.file("js/action.yml", R"(name: post-context
runs:
  using: node20
  main: main.js
  post: post.js
  post-if: failure() && inputs.enabled == 'yes' && steps.later.outcome == 'failure' && github.action_status == 'failure' && endsWith(github.action_path, '/outer')
)");
  f.file("js/main.js", "");
  f.file("js/post.js", R"(if (!process.env.GITHUB_ACTION_PATH.endsWith('/js')) process.exit(1);
require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'nested cleanup\n');
)");
  const auto job = f.run(R"(      - uses: ./outer
        continue-on-error: true
)", *node);
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ("nested cleanup\n", job.summary);
  const auto cleanup = posts(job);
  IUTEST_ASSERT_EQ(1u, cleanup.size());
  IUTEST_EXPECT_EQ("success", cleanup[0].outcome);
}

IUTEST(ActionRunnerTest, CompositeDepthLimitRejectsRecursion) {
  ActionFixture f;
  f.file("recursive/action.yml", R"(name: recursive
runs:
  using: composite
  steps:
    - uses: ./recursive
)");
  const auto job = f.run("      - uses: ./recursive\n");
  IUTEST_EXPECT_EQ("failure", job.result);
  IUTEST_EXPECT_NE(std::string::npos, f.log.find("nested up to 10 levels")) << f.log;
  IUTEST_EXPECT_EQ(12u, job.steps.size());
}

IUTEST(ActionRunnerTest, RejectsDockerAndUnsupportedRuntimesWithoutFetching) {
  for (const auto& runtime : {"docker", "node99", "python"}) {
    ActionFixture f;
    const std::string runs = std::string(runtime) == "docker"
                                 ? "  image: Dockerfile\n"
                                 : "  main: main.js\n";
    f.file("action/action.yml", "name: unsupported\nruns:\n  using: " + std::string(runtime) + "\n" + runs);
    const auto job = f.run("      - uses: ./action\n");
    IUTEST_EXPECT_EQ("failure", job.result) << runtime << ": " << f.log;
    IUTEST_EXPECT_NE(std::string::npos, f.log.find(std::string(runtime) == "docker" ? "Docker" : "using"))
        << f.log;
  }
  ActionFixture f;
  const auto job = f.run("      - uses: docker://alpine:3\n");
  IUTEST_EXPECT_EQ("failure", job.result);
  IUTEST_EXPECT_NE(std::string::npos, f.log.find("Docker container actions are not supported"));
}

IUTEST(ActionRunnerTest, RejectsLocalActionSymlinkOutsideWorkspace) {
  ActionFixture f;
  f.file("placeholder", "");
  util::write_file(f.dir / "outside/action.yml", "name: escaped\nruns:\n  using: composite\n  steps: []\n");
  std::filesystem::create_directory_symlink(f.dir / "outside", f.dir / "repo/escape");
  const auto job = f.run("      - uses: ./escape\n");
  IUTEST_EXPECT_EQ("failure", job.result);
  IUTEST_EXPECT_NE(std::string::npos, f.log.find("outside of the workspace")) << f.log;
}

IUTEST(ActionRunnerTest, EntryPointExceptionStillRunsPostAndRejectsSymlinkEscape) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  f.file("js/action.yml",
         "name: invalid main\nruns:\n  using: node20\n  pre: pre.js\n  main: escape.js\n  post: post.js\n");
  f.file("js/pre.js", "require('fs').appendFileSync(process.env.GITHUB_STATE, 'saved=pre\\n');\n");
  f.file("js/post.js",
         "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, process.env.STATE_saved + '\\n');\n");
  f.file("outside.js", "throw Error('escaped entry point executed');\n");
  std::filesystem::create_symlink("../outside.js", f.dir / "repo/js/escape.js");
  const auto job = f.run("      - uses: ./js\n", *node);
  IUTEST_EXPECT_EQ("failure", job.result);
  IUTEST_EXPECT_EQ("pre\n", job.summary);
  IUTEST_EXPECT_NE(std::string::npos, f.log.find("Invalid action entry point")) << f.log;
  const auto cleanup = posts(job);
  IUTEST_ASSERT_EQ(1u, cleanup.size());
  IUTEST_EXPECT_EQ("success", cleanup[0].outcome);
}

IUTEST(ActionRunnerTest, JobTimeoutDoesNotConsumePostCleanupBudget) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  f.file("js/action.yml",
         "name: timeout\nruns:\n  using: node20\n  main: main.js\n  post: post.js\n");
  f.file("js/main.js", "setTimeout(() => {}, 10000);\n");
  f.file("js/post.js",
         "setTimeout(() => require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'cleanup\\n'), 50);\n");
  const auto job = f.run("      - uses: ./js\n", *node, "    timeout-minutes: 0.025\n");
  IUTEST_EXPECT_EQ("failure", job.result) << f.log;
  IUTEST_EXPECT_EQ("cleanup\n", job.summary);
  IUTEST_EXPECT_NE(std::string::npos, job.reason.find("maximum execution time"));
  const auto cleanup = posts(job);
  IUTEST_ASSERT_EQ(1u, cleanup.size());
  IUTEST_EXPECT_EQ("success", cleanup[0].outcome);
}

IUTEST(ActionRunnerTest, BuiltinDownloadsCannotWriteProtectedJobControlDirectory) {
  for (const auto& target : {"../..", "../../..", "../../../../../control"}) {
    ActionFixture f;
    const auto job = f.run(R"(      - run: |
          mkdir payload
          echo data > payload/file
      - uses: actions/upload-artifact@v4
        with: {name: payload, path: payload}
      - uses: actions/download-artifact@v4
        with: {name: payload, path: ')" + std::string(target) + "'}\n");
    IUTEST_EXPECT_EQ("failure", job.result) << f.log;
    IUTEST_EXPECT_NE(std::string::npos, f.log.find("outside of the writable directories")) << f.log;
  }
}

IUTEST(ActionRunnerTest, SandboxProfilesUseSeparateProtectedControlDirectory) {
  if (!sandbox::supported()) IUTEST_SKIP() << "Seatbelt requires macOS";
  ActionFixture f;
  for (const auto& root : sandbox::darwin_user_dirs()) {
    if (util::is_subpath(util::real_path(f.dir.path()), util::real_path(root))) {
      IUTEST_SKIP() << "Fixture root overlaps a system-writable sandbox directory";
    }
  }
  const auto job = f.run("      - run: true\n", {}, {}, [](RunOptions& options) {
    options.sandbox.enabled = true;
    options.keep_workdir = true;
  });
  (void)job;  // Linux can generate the profile but cannot execute sandbox-exec.
  const auto control = util::real_path(f.dir / "run/control");
  IUTEST_ASSERT_TRUE(std::filesystem::is_directory(control));
  std::size_t profiles = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(control)) {
    if (entry.path().extension() != ".sb") continue;
    ++profiles;
    const auto profile = util::read_file(entry.path());
    const auto begin = profile.find("; Writable directories");
    const auto end = profile.find("; Network", begin);
    IUTEST_ASSERT_NE(std::string::npos, begin);
    IUTEST_ASSERT_NE(std::string::npos, end);
    const auto grants = profile.substr(begin, end - begin);
    IUTEST_EXPECT_EQ(std::string::npos, grants.find(control.string()));
    IUTEST_EXPECT_EQ(std::string::npos,
                     grants.find("/jobs/" + entry.path().parent_path().filename().string() + "\")"));
    IUTEST_EXPECT_NE(std::string::npos, profile.find(
        "(deny file-read* file-write* (subpath " + sandbox::sbpl_quote(control.string()) + "))"));
  }
  IUTEST_EXPECT_EQ(1u, profiles);
}

IUTEST(ActionRunnerTest, RejectsWritableRootsOverlappingProtectedControls) {
  for (const bool sandbox_enabled : {false, true}) {
    if (sandbox_enabled && !sandbox::supported()) continue;
    ActionFixture f;
    const auto job = f.run("      - run: true\n", {}, {}, [sandbox_enabled](RunOptions& options) {
      options.sandbox.enabled = sandbox_enabled;
      options.sandbox.allow_write.push_back(options.run_dir);
    });
    IUTEST_EXPECT_EQ("failure", job.result);
    IUTEST_EXPECT_NE(std::string::npos, f.log.find("overlaps the protected job control directory")) << f.log;
  }
}

IUTEST(ActionRunnerTest, RejectsCacheAllocationsOverlappingProtectedControls) {
  ActionFixture f;
  const auto job = f.run("      - run: true\n", {}, {}, [](RunOptions& options) {
    options.run_dir = options.cache_dir / "toolcache" / "execution";
  });
  IUTEST_EXPECT_EQ("failure", job.result);
  IUTEST_EXPECT_NE(std::string::npos,
                   f.log.find("Allocated action directory overlaps the protected job control directory")) << f.log;
}
