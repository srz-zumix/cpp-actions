#include "iutest.hpp"

#include <cstdlib>
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
  ~ActionFixture() { cpa_test::remove_retained_workspaces(dir / "run"); }
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

void seed_setup_python(ActionFixture& f, const std::string& sha) {
  const auto cached = f.dir / ("cache/actions/actions/setup-python/" + sha);
  util::write_file(cached / ".cpp-actions-complete", "");
  util::write_file(cached / "action.yml", R"yaml(inputs:
  python-version:
    default: '3.11.9'
  update-environment:
    default: 'true'
runs:
  using: node20
  pre: pre.js
  main: main.js
  post: post.js
)yaml");
  util::write_file(cached / "pre.js", R"js(const fs = require('fs');
fs.writeFileSync(process.env.HOSTED_CACHE + '/pre', 'pre');
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'pre\n');
)js");
  util::write_file(cached / "main.js", R"js(const fs = require('fs');
const path = require('path');
const {execFileSync} = require('child_process');
try {
  fs.writeFileSync(__filename, 'tampered');
  throw Error('setup-python source was writable');
} catch (e) {
  if (e.code !== 'EPERM' && e.code !== 'EACCES') throw e;
}
const version = process.env['INPUT_PYTHON-VERSION'];
const install = path.join(process.env.HOSTED_CACHE, 'Python', version, 'arm64');
fs.mkdirSync(path.join(install, 'bin'), {recursive: true});
fs.writeFileSync(path.join(install, 'released'), 'selected');
fs.writeFileSync(path.join(install, 'bin', 'python'), '#!/bin/sh\necho stub-' + version + '\n', {mode: 0o755});
execFileSync('/bin/sh', ['-c', 'printf child > "$HOSTED_CACHE/child"']);
if (process.env['INPUT_UPDATE-ENVIRONMENT'] !== 'false') {
  fs.appendFileSync(process.env.GITHUB_ENV, `pythonLocation=${install}\n`);
  fs.appendFileSync(process.env.GITHUB_PATH, `${install}\n${install}/bin\n`);
}
fs.appendFileSync(process.env.GITHUB_OUTPUT, `python-path=${install}/bin/python\n`);
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'main\n');
)js");
  util::write_file(cached / "post.js", R"js(const fs = require('fs');
fs.writeFileSync(process.env.HOSTED_CACHE + '/post', 'post');
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'post\n');
)js");
}

}  // namespace

IUTEST(ActionRunnerTest, SetupPythonCreatesMissingCacheBeforeLifecycle) {
  if (!sandbox::supported()) IUTEST_SKIP() << "Seatbelt is unavailable";
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  const std::string sha(40, 'a');
  const auto cache = f.dir / "new-runner/hostedtoolcache";
  seed_setup_python(f, sha);
  const auto body = "      - uses: actions/setup-python@" + sha + R"yaml(
      - run: |
          test "$(python)" = stub-3.11.9
          if (echo forbidden > "$HOSTED_CACHE/other-write") 2>/dev/null; then exit 1; fi
)yaml";
  const auto job = f.run(body, *node, {}, [&](RunOptions& options) {
    options.sandbox.enabled = true;
    options.sandbox.hosted_python_tool_cache = cache;
    options.pass_env = {"HOSTED_CACHE=" + cache.string()};
  });
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ("pre\nmain\npost\n", job.summary);
  IUTEST_EXPECT_NE(std::string::npos, f.log.find("Created setup-python host tool cache: " + cache.string())) << f.log;
  IUTEST_EXPECT_TRUE(std::filesystem::is_directory(cache / "Python/3.11.9/arm64"));
  IUTEST_EXPECT_FALSE(std::filesystem::exists(cache / "other-write"));
  IUTEST_EXPECT_EQ(std::string::npos, f.log.find("install-hosted-python.sh")) << f.log;
}

IUTEST(ActionRunnerTest, FailedSetupPythonLifecycleSuggestsHostProvisioning) {
  if (!sandbox::supported()) IUTEST_SKIP() << "Seatbelt is unavailable";
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  for (const auto& phase : {"pre.js", "main.js", "post.js"}) {
    ActionFixture f;
    const std::string sha(40, 'f');
    const auto cache = f.dir / "hostedtoolcache";
    const auto cached = f.dir / ("cache/actions/actions/setup-python/" + sha);
    seed_setup_python(f, sha);
    util::write_file(cached / phase, "console.error('intentional lifecycle failure'); process.exit(1);\n");
    f.file("wrapper/action.yml", "runs:\n  using: composite\n  steps:\n    - uses: actions/setup-python@" + sha + "\n");
    const auto body = std::string(phase) == "main.js" ? "      - uses: ./wrapper\n"
                                                      : "      - uses: actions/setup-python@" + sha + "\n";
    const auto job = f.run(body, *node, {}, [&](RunOptions& options) {
      options.sandbox.enabled = true;
      options.sandbox.hosted_python_tool_cache = cache;
      options.pass_env = {"HOSTED_CACHE=" + cache.string()};
    });
    IUTEST_EXPECT_EQ("failure", job.result) << phase << '\n' << f.log;
    IUTEST_EXPECT_NE(std::string::npos, f.log.find("intentional lifecycle failure")) << f.log;
    IUTEST_EXPECT_NE(std::string::npos, f.log.find("scripts/install-hosted-python.sh --version 3.11.9 --dry-run"))
        << f.log;
    IUTEST_EXPECT_NE(std::string::npos, f.log.find("not setup-steps")) << f.log;
  }
}

IUTEST(ActionRunnerTest, UnrelatedActionFailureDoesNotSuggestPythonProvisioning) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  f.file("unrelated/action.yml", "runs:\n  using: node20\n  main: main.js\n");
  f.file("unrelated/main.js", "process.exit(1);\n");
  const auto job = f.run("      - uses: ./unrelated\n", *node);
  IUTEST_EXPECT_EQ("failure", job.result) << f.log;
  IUTEST_EXPECT_EQ(std::string::npos, f.log.find("install-hosted-python.sh")) << f.log;
}

IUTEST(ActionRunnerTest, FreeThreadedFailureUsesGenericHintOnlyWhenSandboxed) {
  if (!sandbox::supported()) IUTEST_SKIP() << "Seatbelt is unavailable";
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  for (const auto enabled : {false, true}) {
    ActionFixture f;
    const std::string sha(40, 'f');
    const auto cached = f.dir / ("cache/actions/actions/setup-python/" + sha);
    seed_setup_python(f, sha);
    util::write_file(cached / "action.yml", R"yaml(inputs:
  python-version:
    default: '3.13'
  architecture:
    default: arm64
  freethreaded:
    default: 'true'
runs:
  using: node20
  main: main.js
)yaml");
    util::write_file(cached / "main.js", "process.exit(1);\n");
    const auto job = f.run("      - uses: actions/setup-python@" + sha + "\n", *node, {}, [&](RunOptions& options) {
      options.sandbox.enabled = enabled;
      options.sandbox.hosted_python_tool_cache = f.dir / "hostedtoolcache";
    });
    IUTEST_EXPECT_EQ("failure", job.result) << f.log;
    if (enabled) {
      IUTEST_EXPECT_NE(std::string::npos, f.log.find("--version '<VERSION>' --architecture arm64 --dry-run")) << f.log;
      IUTEST_EXPECT_EQ(std::string::npos, f.log.find("--version 3.13")) << f.log;
    } else {
      IUTEST_EXPECT_EQ(std::string::npos, f.log.find("install-hosted-python.sh")) << f.log;
    }
  }
}

IUTEST(ActionRunnerTest, SkippedOrDifferentActionsDoNotCreateHostedPythonCache) {
  if (!sandbox::supported()) IUTEST_SKIP() << "Seatbelt is unavailable";
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  const std::string sha(40, 'a');
  const auto cache = f.dir / "new-runner/hostedtoolcache";
  const auto cached = f.dir / ("cache/actions/actions/setup-python/" + sha);
  seed_setup_python(f, sha);
  util::write_file(cached / "other/action.yml", "runs:\n  using: node20\n  main: main.js\n");
  util::write_file(cached / "other/main.js", "console.log('subpath');\n");
  f.file("setup-python/action.yml", "runs:\n  using: node20\n  main: main.js\n");
  f.file("setup-python/main.js", "console.log('local action');\n");
  const auto body =
      "      - uses: ./setup-python\n"
      "      - uses: actions/setup-python/other@" +
      sha +
      "\n"
      "      - uses: actions/setup-python@" +
      sha +
      "\n"
      "        if: false\n";
  const auto job = f.run(body, *node, {}, [&](RunOptions& options) {
    options.sandbox.enabled = true;
    options.sandbox.hosted_python_tool_cache = cache;
  });
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_FALSE(std::filesystem::exists(cache.parent_path()));
}

IUTEST(ActionRunnerTest, SetupPythonScopesCacheThroughNestedActionsAndLifecycle) {
  if (!sandbox::supported()) IUTEST_SKIP() << "Seatbelt is unavailable";
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  const std::string sha(40, 'b');
  const auto cache = f.dir / "hostedtoolcache";
  const auto cached = f.dir / ("cache/actions/actions/setup-python/" + sha);
  seed_setup_python(f, sha);
  util::write_file(cache / "Python/3.12.0/arm64/secret", "unselected");
  util::write_file(cached / "other/action.yml", "runs:\n  using: node20\n  main: main.js\n");
  util::write_file(cached / "other/main.js", R"js(const fs = require('fs');
const path = require('path');
try {
  fs.writeFileSync(process.env.HOSTED_CACHE + '/other-write', 'forbidden');
  throw Error('subpath received setup-python write access');
} catch (e) {
  if (e.code !== 'EPERM' && e.code !== 'EACCES') throw e;
}
fs.writeFileSync(path.join(__dirname, '..', 'main.js'), 'throw Error("tampered copy");\n');
)js");
  f.file("wrapper/action.yml", "runs:\n  using: composite\n  steps:\n    - uses: actions/setup-python@" + sha +
                                   R"yaml(
    - shell: bash
      run: |
        test "$(cat "$pythonLocation/released")" = selected
        if (echo forbidden > "$pythonLocation/forbidden") 2>/dev/null; then exit 1; fi
)yaml");
  f.file("unrelated/action.yml", "runs:\n  using: node20\n  main: main.js\n");
  f.file("unrelated/main.js", R"js(const fs = require('fs');
if (fs.readFileSync(process.env.pythonLocation + '/released', 'utf8') !== 'selected') throw Error('missing read');
try {
  fs.writeFileSync(process.env.HOSTED_CACHE + '/unrelated-write', 'forbidden');
  throw Error('unrelated action received setup-python write access');
} catch (e) {
  if (e.code !== 'EPERM' && e.code !== 'EACCES') throw e;
}
)js");
  const auto body = R"yaml(      - run: |
          if cat "$HOSTED_CACHE/Python/3.12.0/arm64/secret" >/dev/null 2>&1; then exit 1; fi
          if (echo forbidden > "$HOSTED_CACHE/before") 2>/dev/null; then exit 1; fi
      - uses: actions/setup-python/other@)yaml" +
                    sha + R"yaml(
      - uses: ./wrapper
      - uses: ./unrelated
      - uses: actions/upload-artifact@v4
        with:
          name: selected-python
          path: ${{ env.pythonLocation }}/released
      - run: |
          test "$(cat "$pythonLocation/released")" = selected
          test "$(python)" = stub-3.11.9
          if (echo forbidden > "$pythonLocation/forbidden") 2>/dev/null; then exit 1; fi
          if cat "$HOSTED_CACHE/Python/3.12.0/arm64/secret" >/dev/null 2>&1; then exit 1; fi
          if cat "$HOSTED_CACHE/pre" >/dev/null 2>&1; then exit 1; fi
)yaml";
  const auto job = f.run(body, *node, {}, [&](RunOptions& options) {
    options.sandbox.enabled = true;
    options.sandbox.hosted_python_tool_cache = cache;
    options.pass_env = {"HOSTED_CACHE=" + cache.string()};
  });
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ("pre\nmain\npost\n", job.summary);
  IUTEST_EXPECT_EQ("pre", util::read_file(cache / "pre"));
  IUTEST_EXPECT_EQ("child", util::read_file(cache / "child"));
  IUTEST_EXPECT_EQ("post", util::read_file(cache / "post"));
  IUTEST_EXPECT_FALSE(std::filesystem::exists(cache / "other-write"));
  IUTEST_EXPECT_FALSE(std::filesystem::exists(cache / "unrelated-write"));
  IUTEST_EXPECT_NE(std::string::npos, util::read_file(cached / "main.js").find("setup-python source was writable"));
}

IUTEST(ActionRunnerTest, SetupPythonTracksMultipleAndOutputOnlyInstallations) {
  if (!sandbox::supported()) IUTEST_SKIP() << "Seatbelt is unavailable";
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  const std::string sha(40, 'c');
  const auto cache = f.dir / "hostedtoolcache";
  seed_setup_python(f, sha);
  util::write_file(cache / "Python/3.12.0/arm64/secret", "unselected");
  const auto uses = "actions/setup-python@" + sha;
  const auto body = "      - uses: " + uses + R"yaml(
        with: {python-version: '3.11.9'}
      - uses: )yaml" +
                    uses + R"yaml(
        with: {python-version: '3.10.0'}
      - uses: )yaml" +
                    uses + R"yaml(
        id: output-only
        with: {python-version: '3.9.0', update-environment: 'false'}
      - env:
          OUTPUT_PYTHON: ${{ steps.output-only.outputs.python-path }}
        run: |
          test "$(python)" = stub-3.10.0
          test "$("$HOSTED_CACHE/Python/3.11.9/arm64/bin/python")" = stub-3.11.9
          test "$("$OUTPUT_PYTHON")" = stub-3.9.0
          test "$pythonLocation" = "$HOSTED_CACHE/Python/3.10.0/arm64"
          if (echo forbidden > "$OUTPUT_PYTHON") 2>/dev/null; then exit 1; fi
          if cat "$HOSTED_CACHE/Python/3.12.0/arm64/secret" >/dev/null 2>&1; then exit 1; fi
)yaml";
  const auto job = f.run(body, *node, {}, [&](RunOptions& options) {
    options.sandbox.enabled = true;
    options.sandbox.hosted_python_tool_cache = cache;
    options.pass_env = {"HOSTED_CACHE=" + cache.string()};
  });
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ(3u, posts(job).size());
}

IUTEST(ActionRunnerTest, SetupPythonRejectsUnavailableOrUnprotectedExecutablePaths) {
  if (!sandbox::supported()) IUTEST_SKIP() << "Seatbelt is unavailable";
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  for (const int scenario : {0, 1, 2, 3, 4, 5, 6, 7}) {
    ActionFixture f;
    const std::string sha(40, 'd');
    seed_setup_python(f, sha);
    auto cache = f.dir / "hostedtoolcache";
    if (scenario == 2) cache = f.dir / "cache/actions";
    if (scenario == 7) cache = f.dir / "run/control/new-cache";
    if (scenario == 0) {
      util::write_file(f.dir / "not-a-directory", "file");
      cache = f.dir / "not-a-directory/hostedtoolcache";
    }
    if (scenario == 2 || scenario == 3) std::filesystem::create_directories(cache);
    if (scenario == 6) {
      util::write_file(f.dir / ("cache/actions/actions/setup-python/" + sha + "/action.yml"),
                       "runs:\n  using: composite\n  steps:\n    - shell: bash\n      run: true\n");
    }
    auto executable = *node;
    if (scenario == 3 || scenario == 5) {
      executable = scenario == 3 ? cache / "node" : f.dir / "writable-node/node";
      util::write_file(executable, "#!/bin/sh\nexit 0\n");
      std::filesystem::permissions(executable, std::filesystem::perms::owner_all);
    }
    const auto job =
        f.run("      - uses: actions/setup-python@" + sha + "\n", executable, {}, [&](RunOptions& options) {
          options.sandbox.enabled = true;
          options.sandbox.hosted_python_tool_cache = cache;
          options.pass_env = {"HOSTED_CACHE=" + cache.string()};
          if (scenario == 1) options.sandbox.allow_write.push_back(cache);
          if (scenario == 4) options.sandbox.allow_write.push_back(f.dir / "cache/actions");
          if (scenario == 5) options.sandbox.allow_write.push_back(executable.parent_path());
        });
    IUTEST_EXPECT_EQ("failure", job.result) << "scenario " << scenario << ": " << f.log;
    const std::vector<std::string> errors = {
        "Unable to open setup-python host tool cache directory",
        "setup-python host tool cache overlaps",
        "setup-python cached action overlaps",
        "setup-python Node.js executable overlaps",
        "setup-python cached action overlaps",
        "setup-python Node.js executable overlaps",
        "Hosted Python tool cache access requires a JavaScript setup-python action",
        "setup-python host tool cache overlaps",
    };
    IUTEST_EXPECT_NE(std::string::npos, f.log.find(errors[scenario])) << "scenario " << scenario << ": " << f.log;
    if (scenario == 1 || scenario >= 4) {
      IUTEST_EXPECT_FALSE(std::filesystem::exists(cache)) << scenario;
    }
  }
}

IUTEST(ActionRunnerTest, SetupPythonKeepsNormalWritableCopyWithoutSandbox) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  const std::string sha(40, 'f');
  const auto cached = f.dir / ("cache/actions/actions/setup-python/" + sha);
  seed_setup_python(f, sha);
  util::write_file(cached / "action.yml", "runs:\n  using: node20\n  main: main.js\n");
  const std::string source = R"js(const fs = require('fs');
fs.writeFileSync(__filename, 'changed copy');
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'normal-copy\n');
)js";
  util::write_file(cached / "main.js", source);
  const auto job = f.run("      - uses: actions/setup-python@" + sha + "\n", *node);
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ("normal-copy\n", job.summary);
  IUTEST_EXPECT_EQ(source, util::read_file(cached / "main.js"));
  IUTEST_EXPECT_EQ(std::string::npos, f.log.find("Host Python tool cache read/write enabled"));
}

IUTEST(ActionRunnerTest, SetupPythonDoesNotGrantPartialReadsForInvalidReportedPaths) {
  if (!sandbox::supported()) IUTEST_SKIP() << "Seatbelt is unavailable";
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  const std::string sha(40, 'e');
  const auto cache = f.dir / "hostedtoolcache";
  const auto cached = f.dir / ("cache/actions/actions/setup-python/" + sha);
  seed_setup_python(f, sha);
  std::filesystem::create_directories(cache);
  util::write_file(cached / "main.js", R"js(const fs = require('fs');
const path = require('path');
const install = path.join(process.env.HOSTED_CACHE, 'Python/3.11.9/arm64');
fs.mkdirSync(install + '/bin', {recursive: true});
fs.writeFileSync(install + '/file', 'selected');
fs.appendFileSync(process.env.GITHUB_PATH, install + '/bin\n');
fs.appendFileSync(process.env.GITHUB_OUTPUT, 'python-path=' + process.env.HOSTED_CACHE + '/Invalid/version/arm64\n');
)js");
  const auto body = "      - uses: actions/setup-python@" + sha + R"yaml(
      - if: always()
        run: |
          if cat "$HOSTED_CACHE/Python/3.11.9/arm64/file" >/dev/null 2>&1; then exit 1; fi
          echo no-leak >> "$GITHUB_STEP_SUMMARY"
)yaml";
  const auto job = f.run(body, *node, {}, [&](RunOptions& options) {
    options.sandbox.enabled = true;
    options.sandbox.hosted_python_tool_cache = cache;
    options.pass_env = {"HOSTED_CACHE=" + cache.string()};
  });
  IUTEST_EXPECT_EQ("failure", job.result) << f.log;
  IUTEST_EXPECT_NE(std::string::npos, f.log.find("setup-python reported an invalid host tool cache path")) << f.log;
  IUTEST_EXPECT_EQ("pre\nno-leak\npost\n", job.summary);
}

IUTEST(ActionRunnerTest, GitConfigurationIsSharedWithinJobIncludingNodeLifecycle) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  f.file("composite/action.yml", R"yaml(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        test "$(git config --global --get safe.directory)" = '*'
        git config --global runner.composite configured
)yaml");
  f.file("js/action.yml", R"(runs:
  using: node20
  pre: pre.js
  main: main.js
  post: post.js
)");
  f.file("js/pre.js", R"(const {execFileSync} = require('child_process');
if (execFileSync('git', ['config', '--global', '--get', 'runner.composite'], {encoding: 'utf8'}).trim() !== 'configured') {
  throw Error('composite global config was lost');
}
execFileSync('git', ['config', '--global', 'runner.node', 'pre']);
)");
  f.file("js/main.js", R"(const {execFileSync} = require('child_process');
if (execFileSync('git', ['config', '--global', '--get', 'runner.node'], {encoding: 'utf8'}).trim() !== 'pre') {
  throw Error('pre global config was lost');
}
execFileSync('git', ['config', '--global', 'runner.node', 'main']);
)");
  f.file("js/post.js", R"(const {execFileSync} = require('child_process');
if (execFileSync('git', ['config', '--global', '--get', 'runner.node'], {encoding: 'utf8'}).trim() !== 'main') {
  throw Error('main global config was lost');
}
)");
  const auto report = f.run(R"yaml(      - uses: ./composite
      - uses: ./js
      - run: test "$(git config --global --get runner.node)" = main
)yaml", *node, {}, [](RunOptions& options) { options.sandbox.enabled = host_is_macos(); });
  IUTEST_EXPECT_EQ("success", report.result) << f.log;
}

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

IUTEST(ActionRunnerTest, RemoteCompositeUsesWritableJobCopyWithoutChangingCache) {
  for (const bool sandbox_enabled : {false, true}) {
    if (sandbox_enabled && !sandbox::supported()) continue;
    ActionFixture f;
    const std::string sha(40, 'a');
    const auto cached = f.dir / ("cache/actions/example/writable/" + sha);
    util::write_file(cached / ".cpp-actions-complete", "");
    util::write_file(cached / "shared.txt", "original\n");
    util::write_file(cached / "nested/action.yml", R"yaml(name: writable
outputs:
  count:
    value: ${{ steps.write.outputs.count }}
  path:
    value: ${{ steps.write.outputs.path }}
runs:
  using: composite
  steps:
    - id: write
      shell: bash
      working-directory: ${{ github.action_path }}
      run: |
        test '${{ github.action_path }}' = "$GITHUB_ACTION_PATH"
        test -L shared-link
        count=0
        if test -f nuget.config; then count=$(cat nuget.config); fi
        if test "$count" = 0; then
          test "$(cat shared-link)" = original
        else
          test "$(cat shared-link)" = changed
        fi
        count=$((count + 1))
        echo "$count" > nuget.config
        echo changed > ../shared.txt
        echo "count=$count" >> "$GITHUB_OUTPUT"
        echo "path=$GITHUB_ACTION_PATH" >> "$GITHUB_OUTPUT"
)yaml");
    std::filesystem::create_symlink("../shared.txt", cached / "nested/shared-link");
    std::string previous_path;
    for (int job_index = 0; job_index < 2; ++job_index) {
      f.file("iteration", std::to_string(job_index));
      const auto job = f.run(
          "      - uses: example/writable/nested@" + sha + "\n        id: first\n" +
          "      - uses: example/writable/nested@" + sha + "\n        id: second\n" +
          R"(      - run: |
          test '${{ steps.first.outputs.count }}' = 1
          test '${{ steps.second.outputs.count }}' = 2
          test '${{ steps.first.outputs.path }}' = '${{ steps.second.outputs.path }}'
          echo '${{ steps.first.outputs.path }}' >> "$GITHUB_STEP_SUMMARY"
)" + (sandbox_enabled
          ? "      - run: |\n"
            "          if (echo corrupted > '" + (cached / "shared.txt").string() +
            "') 2>/dev/null; then exit 1; fi\n"
          : ""), {}, {}, [sandbox_enabled, job_index](RunOptions& options) {
            options.sandbox.enabled = sandbox_enabled;
            options.run_dir += "-" + std::to_string(job_index);
          });
      IUTEST_ASSERT_EQ("success", job.result) << f.log;
      IUTEST_EXPECT_NE(std::string::npos, job.summary.find("/work/_temp/action-"));
      IUTEST_EXPECT_EQ(std::string::npos, job.summary.find("/cache/actions/"));
      IUTEST_EXPECT_NE(previous_path, job.summary);
      previous_path = job.summary;
      IUTEST_EXPECT_FALSE(std::filesystem::exists(cached / "nested/nuget.config"));
      IUTEST_EXPECT_EQ("original\n", util::read_file(cached / "shared.txt"));
      IUTEST_EXPECT_TRUE(std::filesystem::is_symlink(cached / "nested/shared-link"));
    }
  }
}

IUTEST(ActionRunnerTest, RemoteJavascriptPreMainAndPostShareWritableCopy) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  const std::string sha(40, 'b');
  const auto cached = f.dir / ("cache/actions/example/lifecycle/" + sha);
  util::write_file(cached / ".cpp-actions-complete", "");
  util::write_file(cached / "nested/action.yml",
                   "name: lifecycle\nruns:\n  using: node20\n  pre: pre.js\n  main: main.js\n  post: post.js\n");
  util::write_file(cached / "nested/pre.js",
                   "require('fs').writeFileSync(__dirname + '/generated', 'pre');\n");
  util::write_file(cached / "nested/main.js", R"(const fs = require('fs');
if (__dirname !== process.env.GITHUB_ACTION_PATH) throw Error('action path mismatch');
if (fs.readFileSync(__dirname + '/generated', 'utf8') !== 'pre') throw Error('missing pre file');
fs.writeFileSync(__dirname + '/generated', 'main');
)");
  util::write_file(cached / "nested/post.js", R"(const fs = require('fs');
if (fs.readFileSync(__dirname + '/generated', 'utf8') !== 'main') throw Error('missing main file');
fs.unlinkSync(__dirname + '/generated');
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'cleanup\n');
)");
  const auto job = f.run("      - uses: example/lifecycle/nested@" + sha + "\n", *node);
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ("cleanup\n", job.summary);
  IUTEST_EXPECT_FALSE(std::filesystem::exists(cached / "nested/generated"));
}

IUTEST(ActionRunnerTest, JavascriptPackageScopeStopsAtJobBoundaryAndPreservesActionTypes) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  struct ProtectedFixture {
    const std::filesystem::path root = util::make_temp_dir("cpa-node-scope-test-", util::home_dir());
    ~ProtectedFixture() { util::remove_tree(root); }
  };
  for (const bool sandbox_enabled : {false, true}) {
    if (sandbox_enabled && !sandbox::supported()) continue;
    ProtectedFixture protected_dir;
    ActionFixture f;
    util::write_file(protected_dir.root / "package.json", "{\"type\":\"module\"}\n");
    const std::string sha(40, 'b');
    std::string body;
    std::string expected;
    std::vector<std::string> labels;
    for (const auto* type : {"bare", "commonjs", "module"}) {
      for (const bool remote : {false, true}) {
        const auto label = std::string(remote ? "remote-" : "local-") + type;
        const auto root = remote ? f.dir / ("cache/actions/example/" + label + "/" + sha)
                                 : f.dir / ("repo/" + label);
        if (remote) util::write_file(root / ".cpp-actions-complete", "");
        if (std::string(type) != "bare") {
          util::write_file(root / "package.json", "{\"type\":\"" + std::string(type) + "\"}\n");
        }
        util::write_file(root / "dist/action.yml",
                         "runs:\n  using: node24\n  pre: pre.js\n  main: main.js\n  post: post.js\n");
        for (const auto* phase : {"pre", "main", "post"}) {
          const std::string load_fs = std::string(type) == "module" ? "import fs from 'node:fs';\n"
                                                                  : "const fs = require('node:fs');\n";
          util::write_file(root / ("dist/" + std::string(phase) + ".js"),
                           load_fs + "fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, '" +
                               label + "-" + phase + "\\n');\n");
        }
        body += "      - uses: " +
                (remote ? "example/" + label + "/dist@" + sha : "./" + label + "/dist") + "\n";
        expected += label + "-pre\n" + label + "-main\n";
        labels.push_back(label);
      }
    }
    for (auto it = labels.rbegin(); it != labels.rend(); ++it) expected += *it + "-post\n";
    const auto report = f.run(body, *node, {}, [&](RunOptions& options) {
      options.run_dir = protected_dir.root / "run";
      options.sandbox.enabled = sandbox_enabled;
    });
    IUTEST_EXPECT_EQ("success", report.result) << f.log;
    IUTEST_EXPECT_EQ(expected, report.summary) << f.log;
    IUTEST_ASSERT_EQ(6u, posts(report).size());
    for (const auto& step : posts(report)) IUTEST_EXPECT_EQ("success", step.outcome) << f.log;
  }
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

IUTEST(ActionRunnerTest, CppActionsMarkerInCompositeExpressions) {
  ActionFixture f;
  f.file("outer/action.yml", R"yaml(name: report
inputs:
  default_marker:
    default: ${{ env.CPP_ACTIONS }}
  passed_marker:
    description: marker supplied by the caller
outputs:
  marker:
    value: ${{ env.CPP_ACTIONS }}
  nested_marker:
    value: ${{ steps.nested.outputs.marker }}
  verified:
    value: ${{ steps.verify.outputs.executed }}
runs:
  using: composite
  steps:
    - name: Test Report
      id: report
      uses: ./forbidden
      if: ${{ env.CPP_ACTIONS != 'true' }}
    - name: Test Summary
      id: summary
      if: ${{ env.CPP_ACTIONS != 'true' }}
      shell: bash
      run: exit 1
    - id: verify
      if: ${{ env.CPP_ACTIONS == 'true' }}
      env:
        CPP_ACTIONS: nested-step-override
      shell: bash
      run: |
        test "$CPP_ACTIONS" = true
        test '${{ env.CPP_ACTIONS }}' = true
        test '${{ inputs.default_marker }}' = true
        test '${{ inputs.passed_marker }}' = true
        echo executed=yes >> "$GITHUB_OUTPUT"
    - id: nested
      uses: ./inner
      env:
        CPP_ACTIONS: nested-action-override
)yaml");
  f.file("inner/action.yml", R"yaml(name: inner
outputs:
  marker:
    value: ${{ env.CPP_ACTIONS }}
runs:
  using: composite
  steps:
    - shell: bash
      if: ${{ env.CPP_ACTIONS == 'true' }}
      run: test '${{ env.CPP_ACTIONS }}' = "$CPP_ACTIONS"
)yaml");
  f.file("forbidden/action.yml", R"yaml(name: forbidden
runs:
  using: composite
  steps:
    - shell: bash
      run: exit 1
)yaml");
  const auto job = f.run(R"yaml(      - uses: ./outer
        id: outer
        env:
          CPP_ACTIONS: action-override
        with:
          passed_marker: ${{ env.CPP_ACTIONS }}
      - run: |
          test '${{ steps.outer.outputs.marker }}' = true
          test '${{ steps.outer.outputs.nested_marker }}' = true
          test '${{ steps.outer.outputs.verified }}' = yes
)yaml");
  IUTEST_ASSERT_EQ("success", job.result) << f.log;
  int skipped = 0;
  for (const auto& step : job.steps) {
    if (step.id == "report" || step.id == "summary") {
      ++skipped;
      IUTEST_EXPECT_EQ("skipped", step.outcome) << f.log;
    }
  }
  IUTEST_EXPECT_EQ(2, skipped) << f.log;
}

IUTEST(ActionRunnerTest, CppActionsMarkerInNodeLifecycleExpressions) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  f.file("js/action.yml", R"yaml(name: marker-lifecycle
inputs:
  marker:
    default: ${{ env.CPP_ACTIONS }}
runs:
  using: node20
  pre: pre.js
  pre-if: env.CPP_ACTIONS == 'true'
  main: main.js
  post: post.js
  post-if: env.CPP_ACTIONS == 'true'
)yaml");
  f.file("js/pre.js", R"js(require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'pre\n');
)js");
  f.file("js/main.js",
         R"js(if (process.env.CPP_ACTIONS !== 'true' || process.env.INPUT_MARKER !== 'true') process.exit(1);
require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'main\n');
)js");
  f.file("js/post.js", R"js(if (process.env.CPP_ACTIONS !== 'true') process.exit(1);
require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'post\n');
)js");
  const auto job = f.run(R"yaml(      - uses: ./js
        env:
          CPP_ACTIONS: action-override
      - run: echo CPP_ACTIONS=file-override >> "$GITHUB_ENV"
)yaml",
                         *node);
  IUTEST_ASSERT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ("pre\nmain\npost\n", job.summary);
  const auto cleanup = posts(job);
  IUTEST_ASSERT_EQ(1u, cleanup.size());
  IUTEST_EXPECT_EQ("success", cleanup[0].outcome);
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
  util::write_file(f.dir / "outside.js", "throw Error('escaped entry point executed');\n");
  std::filesystem::create_symlink(f.dir / "outside.js", f.dir / "repo/js/escape.js");
  const auto job = f.run("      - uses: ./js\n", *node);
  IUTEST_EXPECT_EQ("failure", job.result);
  IUTEST_EXPECT_EQ("pre\n", job.summary);
  IUTEST_EXPECT_NE(std::string::npos, f.log.find("Invalid action entry point")) << f.log;
  const auto cleanup = posts(job);
  IUTEST_ASSERT_EQ(1u, cleanup.size());
  IUTEST_EXPECT_EQ("success", cleanup[0].outcome);
}

IUTEST(ActionRunnerTest, SharedJavascriptEntrypointsSupportLocalAndRemoteActions) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  for (const bool remote : {false, true}) {
    for (const bool sandbox_enabled : {false, true}) {
      if (sandbox_enabled && !sandbox::supported()) continue;
      ActionFixture f;
      const std::string sha(40, 'c');
      const auto root = remote ? f.dir / ("cache/actions/example/shared/" + sha) : f.dir / "repo";
      if (remote) util::write_file(root / ".cpp-actions-complete", "");
      util::write_file(root / "nested/action.yml",
                       "runs:\n  using: node20\n  pre: ../lib/pre.js\n  main: " +
                           std::string(remote ? "../lib/main.js" : "main-link.js") +
                           "\n  post: ../lib/cleanup.js\n");
      util::write_file(root / "lib/pre.js", R"js(const fs = require('fs');
fs.appendFileSync(process.env.GITHUB_STATE, 'saved=shared\n');
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'pre\n');
)js");
      util::write_file(root / "lib/main.js", R"js(const fs = require('fs');
const path = require('path');
if (path.basename(process.env.GITHUB_ACTION_PATH) !== 'nested') throw Error('action path changed');
if (path.dirname(process.env.GITHUB_ACTION_PATH) !== path.dirname(__dirname)) throw Error('wrong repository');
if (process.env.STATE_saved !== 'shared') throw Error('missing pre state');
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'main\n');
)js");
      util::write_file(root / "lib/cleanup.js", R"js(const fs = require('fs');
if (process.env.STATE_saved !== 'shared') throw Error('missing post state');
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'post\n');
)js");
      if (!remote) std::filesystem::create_symlink("../lib/main.js", root / "nested/main-link.js");
      const auto uses = remote ? "example/shared/nested@" + sha : "./nested";
      const auto report = f.run("      - uses: " + uses + "\n", *node, {},
                                [sandbox_enabled](RunOptions& options) { options.sandbox.enabled = sandbox_enabled; });
      IUTEST_ASSERT_EQ("success", report.result) << f.log;
      IUTEST_EXPECT_EQ("pre\nmain\npost\n", report.summary);
      const auto cleanup = posts(report);
      IUTEST_ASSERT_EQ(1u, cleanup.size());
      IUTEST_EXPECT_EQ("success", cleanup[0].outcome);
    }
  }
}

IUTEST(ActionRunnerTest, RemoteJavascriptLifecycleRejectsEntrypointsOutsideRepository) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  for (const std::string phase : {"pre", "main", "post"}) {
    ActionFixture f;
    const std::string sha(40, 'd');
    const auto root = f.dir / ("cache/actions/example/escape/" + sha);
    util::write_file(root / ".cpp-actions-complete", "");
    util::write_file(f.dir / "outside.js",
                     "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'escaped\\n');\n");
    util::write_file(root / "nested/action.yml",
                     "runs:\n  using: node20\n  pre: " +
                         std::string(phase == "pre" ? "../../outside.js" : "../lib/pre.js") +
                         "\n  main: " + (phase == "main" ? "../escape.js" : "../lib/main.js") +
                         "\n  post: " + (phase == "post" ? (f.dir / "outside.js").string() : "../lib/cleanup.js") +
                         "\n");
    for (const auto* file : {"pre.js", "main.js", "cleanup.js"}) util::write_file(root / "lib" / file, "");
    std::filesystem::create_symlink(f.dir / "outside.js", root / "escape.js");
    const auto report = f.run("      - uses: example/escape/nested@" + sha + "\n", *node);
    IUTEST_EXPECT_EQ("failure", report.result) << phase << ": " << f.log;
    IUTEST_EXPECT_NE(std::string::npos, f.log.find("Invalid action entry point")) << phase << ": " << f.log;
    IUTEST_EXPECT_EQ(std::string::npos, report.summary.find("escaped")) << phase;
  }
}

IUTEST(ActionRunnerTest, RunnerWorkspaceSupportsReplacementCheckoutAndRestoration) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  for (const bool sandbox_enabled : {false, true}) {
    if (sandbox_enabled && !sandbox::supported()) continue;
    ActionFixture f;
    const std::string sha(40, 'e');
    const auto cached = f.dir / ("cache/actions/example/workspace/" + sha);
    util::write_file(cached / ".cpp-actions-complete", "");
    util::write_file(cached / "action.yml",
                     "runs:\n  using: node20\n  main: main.js\n  post: post.js\n");
    util::write_file(cached / "main.js", R"js(const fs = require('fs');
const path = require('path');
const workspace = process.env.GITHUB_WORKSPACE;
const shared = path.join(process.env.RUNNER_WORKSPACE, 'shared');
fs.renameSync(workspace, workspace + '.bak');
fs.mkdirSync(shared);
fs.symlinkSync(shared, workspace, 'dir');
)js");
    util::write_file(cached / "post.js", R"js(const fs = require('fs');
const workspace = process.env.GITHUB_WORKSPACE;
if (!fs.lstatSync(workspace).isSymbolicLink()) throw Error('workspace link was lost');
if (!fs.existsSync(workspace + '/nested-checkout/marker')) throw Error('nested checkout failed');
fs.unlinkSync(workspace);
fs.renameSync(workspace + '.bak', workspace);
if (!fs.existsSync(workspace + '/marker')) throw Error('original checkout was lost');
fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'restored\n');
)js");
    f.file("marker", "original\n");
    f.file("local/action.yml", R"yaml(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        test "$(cat marker)" = original
        echo local >> "$GITHUB_STEP_SUMMARY"
)yaml");
    f.file("js/action.yml", "runs:\n  using: node20\n  main: main.js\n");
    f.file("js/main.js", "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, 'node\\n');\n");
    const auto report = f.run("      - uses: example/workspace@" + sha + R"yaml(
      - uses: actions/checkout@v4
      - uses: actions/checkout@v4
        with: {path: nested-checkout}
      - uses: ./local
      - uses: ./js
      - uses: actions/upload-artifact@v4
        with: {name: marker, path: marker}
      - uses: actions/download-artifact@v4
        with:
          name: marker
          path: ${{ github.workspace }}/../downloaded
      - run: |
          test -L "$GITHUB_WORKSPACE"
          test "$(cat "$RUNNER_WORKSPACE/downloaded/marker")" = original
)yaml" + (sandbox_enabled
          ? R"yaml(      - run: |
          if touch "$RUNNER_WORKSPACE/../outside-workspace" 2>/dev/null; then exit 1; fi
)yaml"
          : ""), *node, {}, [sandbox_enabled](RunOptions& options) { options.sandbox.enabled = sandbox_enabled; });
    IUTEST_EXPECT_EQ("success", report.result) << f.log;
    IUTEST_EXPECT_EQ("local\nnode\nrestored\n", report.summary);
  }
}

IUTEST(ActionRunnerTest, WorkspaceSymlinksCannotRedirectLocalActionsOrCheckoutOutsideJob) {
  for (const bool local_action : {false, true}) {
    ActionFixture f;
    util::write_file(f.dir / "outside/marker", "keep\n");
    util::write_file(f.dir / "outside/local/action.yml",
                     "runs:\n  using: composite\n  steps: []\n");
    f.file("marker", "original\n");
    const auto report = f.run(R"yaml(      - run: |
          mv "$GITHUB_WORKSPACE" "$GITHUB_WORKSPACE.bak"
          ln -s ')yaml" + (f.dir / "outside").string() + R"yaml(' "$GITHUB_WORKSPACE"
)yaml" + (local_action ? "      - uses: ./local\n" : "      - uses: actions/checkout@v4\n"));
    IUTEST_EXPECT_EQ("failure", report.result) << f.log;
    IUTEST_EXPECT_NE(std::string::npos,
                     f.log.find(local_action ? "outside of the workspace" : "outside of the writable directories"))
        << f.log;
    IUTEST_EXPECT_EQ("keep\n", util::read_file(f.dir / "outside/marker"));
  }
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

IUTEST(ActionRunnerTest, SandboxedMktempUsesJobTempAndPreservesExplicitTemplates) {
  if (!sandbox::supported()) IUTEST_SKIP() << "Seatbelt requires macOS";
  ActionFixture f;
  const auto job = f.run(R"sh(      - run: |
          for command in 'mktemp' 'mktemp -d' 'mktemp -dq' 'mktemp -t probe' 'mktemp -d -t probe' 'mktemp -dt probe' 'mktemp -dtprobe' 'mktemp --directory'; do
            path=$(sh -c "$command")
            case "$path" in "$TMPDIR"/*) ;; *) exit 1 ;; esac
            test -e "$path"
            if test -d "$path"; then
              echo writable > "$path/testfile"
              rm "$path/testfile"
              rmdir "$path"
            else
              rm "$path"
            fi
          done
          path=$(mktemp -d relative.XXXXXX)
          test "$(dirname "$path")" = .
          test -d "$path"
          rmdir "$path"
          path=$(mktemp -d "$RUNNER_TEMP/absolute.XXXXXX")
          test "$(dirname "$path")" = "$RUNNER_TEMP"
          rmdir "$path"
          path=$(mktemp -d -p "$RUNNER_TEMP")
          test "$(dirname "$path")" = "$RUNNER_TEMP"
          rmdir "$path"
          path=$(mktemp -d -- 'with spaces.XXXXXX')
          test -d "$path"
          rmdir "$path"
          if mktemp -t >/dev/null 2>&1; then exit 1; fi
          if (echo overwrite > "$(command -v mktemp)") 2>/dev/null; then exit 1; fi
      - run: |
          path=$(mktemp -d -t next-step)
          case "$path" in "$TMPDIR"/*) ;; *) exit 1 ;; esac
          rmdir "$path"
        env:
          TMPDIR: /not-the-job-temp
)sh", {}, {}, [](RunOptions& options) { options.sandbox.enabled = true; });
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
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

#if defined(__APPLE__)
IUTEST(ActionRunnerTest, XcodeSimulatorRunWideGrantCoversRunCompositeAndNodeLifecycle) {
#if !defined(__APPLE__)
  IUTEST_SKIP() << "Seatbelt requires macOS";
#else
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  const std::string service = "com.apple.CoreSimulator.CoreSimulatorService";
  if (util::capture({CPA_APPLE_SECURITY_PROBE, service, "allow"}).exit_code != 0) {
    IUTEST_SKIP() << "CoreSimulator service is not available outside the sandbox";
  }
  for (const bool enabled : {false, true}) {
    ActionFixture f;
    f.file("outer/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: '"$SIM_PROBE" com.apple.CoreSimulator.CoreSimulatorService "$SIM_EXPECT"'
    - uses: ./inner
)");
    f.file("inner/action.yml", "runs:\n  using: composite\n  steps:\n    - uses: ./js\n");
    f.file("js/action.yml", "runs:\n  using: node24\n  pre: pre.js\n  main: main.js\n  post: post.js\n");
    for (const auto* phase : {"pre", "main", "post"}) {
      f.file("js/" + std::string(phase) + ".js",
             "require('child_process').execFileSync(process.env.SIM_PROBE, "
             "['com.apple.CoreSimulator.CoreSimulatorService', process.env.SIM_EXPECT]);\n"
             "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, '" + std::string(phase) + "\\n');\n");
    }
    const auto job = f.run(R"(      - run: |
          "$SIM_PROBE" com.apple.CoreSimulator.CoreSimulatorService "$SIM_EXPECT"
          "$SIM_PROBE" com.apple.SecurityServer deny
      - uses: ./outer
      - run: '"$SIM_PROBE" com.apple.CoreSimulator.CoreSimulatorService "$SIM_EXPECT"'
)", *node, {}, [enabled](RunOptions& options) {
      options.sandbox.enabled = true;
      options.sandbox.allow_xcode_simulator = enabled;
      options.sandbox.allow_read = {CPA_APPLE_SECURITY_PROBE};
      options.pass_env = {std::string("SIM_PROBE=") + CPA_APPLE_SECURITY_PROBE,
                          std::string("SIM_EXPECT=") + (enabled ? "allow" : "deny")};
    });
    IUTEST_EXPECT_EQ("success", job.result) << f.log;
    IUTEST_EXPECT_EQ("pre\nmain\npost\n", job.summary) << f.log;
    IUTEST_EXPECT_EQ(enabled, f.log.find("Xcode/Simulator compatibility enabled") != std::string::npos);
  }
#endif
}

IUTEST(ActionRunnerTest, DockerActionAndStepGrantsDoNotLeak) {
#if !defined(__APPLE__)
  IUTEST_SKIP() << "Seatbelt requires macOS";
#else
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  for (const bool grant : {false, true}) {
    ActionFixture f;
    cpa_test::UnixSocketListener docker(f.dir / "docker.sock");
    f.file("outer/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: '"$PROBE" "connect-$EXPECT" "$DOCKER_SOCKET"'
    - uses: ./inner
    - uses: ./js
    - uses: ./unlisted
)");
    f.file("inner/action.yml", R"(runs:
  using: composite
  steps:
    - id: permitted
      shell: bash
      run: '"$PROBE" connect-deny "$DOCKER_SOCKET"'
)");
    f.file("js/action.yml", "runs:\n  using: node24\n  pre: pre.js\n  main: main.js\n  post: post.js\n");
    f.file("unlisted/action.yml", "runs:\n  using: node24\n  main: main.js\n");
    f.file("unlisted/main.js",
           "require('child_process').execFileSync(process.env.PROBE, ['connect-deny', process.env.DOCKER_SOCKET]);\n");
    for (const auto* phase : {"pre", "main", "post"}) {
      f.file("js/" + std::string(phase) + ".js",
             "require('child_process').execFileSync(process.env.PROBE, "
             "['connect-' + process.env.EXPECT, process.env.DOCKER_SOCKET]);\n"
             "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, '" + std::string(phase) + "\\n');\n");
    }
    const auto report = f.run(R"(      - id: before
        run: '"$PROBE" connect-deny "$DOCKER_SOCKET"'
      - run: '"$PROBE" connect-deny "$DOCKER_SOCKET"'
      - id: permitted
        run: '"$PROBE" "connect-$EXPECT" "$DOCKER_SOCKET"'
      - uses: ./outer
      - id: after
        run: '"$PROBE" connect-deny "$DOCKER_SOCKET"'
)", *node, {}, [&](RunOptions& options) {
      options.sandbox.enabled = true;
      options.sandbox.docker_socket = docker.path();
      options.sandbox.allow_read = {CPA_MSBUILD_SOCKET_PROBE};
      options.pass_env = {std::string("PROBE=") + CPA_MSBUILD_SOCKET_PROBE,
                          "DOCKER_SOCKET=" + docker.path().string(),
                          std::string("EXPECT=") + (grant ? "allow" : "deny")};
      if (grant) {
        options.sandbox.allow_docker_for = {"./outer", "./js"};
        options.sandbox.allow_docker_step = {".github/workflows/test.yml::test::permitted",
                                             ".github/workflows/test.yml::test::__run",
                                             ".github/workflows/other.yml::test::before",
                                             ".github/workflows/test.yml::other::before"};
      }
    });
    IUTEST_EXPECT_EQ("success", report.result) << f.log;
    IUTEST_EXPECT_EQ("pre\nmain\npost\n", report.summary) << f.log;
    IUTEST_EXPECT_EQ(grant, f.log.find("Docker daemon access enabled") != std::string::npos);
  }
#endif
}

IUTEST(ActionRunnerTest, MsbuildActionAndStepGrantsDoNotLeak) {
#if !defined(__APPLE__)
  IUTEST_SKIP() << "Seatbelt requires macOS";
#else
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  for (const bool grant : {false, true}) {
    ActionFixture f;
    f.file("outer/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: '"$PROBE" "$EXPECT" ""'
    - uses: ./inner
    - uses: ./js
    - uses: ./unlisted
)");
    f.file("inner/action.yml", R"(runs:
  using: composite
  steps:
    - id: permitted
      shell: bash
      run: '"$PROBE" deny ""'
)");
    f.file("js/action.yml", "runs:\n  using: node24\n  pre: pre.js\n  main: main.js\n  post: post.js\n");
    f.file("unlisted/action.yml", "runs:\n  using: node24\n  main: main.js\n");
    f.file("unlisted/main.js",
           "require('child_process').execFileSync(process.env.PROBE, ['deny', '']);\n");
    for (const auto* phase : {"pre", "main", "post"}) {
      f.file("js/" + std::string(phase) + ".js",
             "require('child_process').execFileSync(process.env.PROBE, [process.env.EXPECT, '']);\n"
             "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, '" + std::string(phase) + "\\n');\n");
    }
    const auto report = f.run(R"(      - id: before
        run: '"$PROBE" deny ""'
      - run: '"$PROBE" deny ""'
      - id: permitted
        run: '"$PROBE" "$EXPECT" ""'
      - uses: ./outer
      - id: after
        run: |
          "$PROBE" deny ""
          "$PROBE" deny ".extra"
)", *node, {}, [grant](RunOptions& options) {
      options.sandbox.enabled = true;
      options.sandbox.allow_read = {CPA_MSBUILD_SOCKET_PROBE};
      options.pass_env = {std::string("PROBE=") + CPA_MSBUILD_SOCKET_PROBE,
                          std::string("EXPECT=") + (grant ? "allow" : "deny")};
      if (grant) {
        options.sandbox.allow_msbuild_for = {"./outer", "./js"};
        options.sandbox.allow_msbuild_step = {".github/workflows/test.yml::test::permitted",
                                              ".github/workflows/test.yml::test::__run",
                                              ".github/workflows/other.yml::test::before",
                                              ".github/workflows/test.yml::other::before",
                                              ".github/workflows/test.yml::test::unknown"};
      }
    });
    IUTEST_EXPECT_EQ("success", report.result) << f.log;
    IUTEST_EXPECT_EQ("pre\nmain\npost\n", report.summary) << f.log;
    IUTEST_EXPECT_EQ(grant, f.log.find("MSBuild worker IPC enabled") != std::string::npos);
  }
#endif
}

IUTEST(ActionRunnerTest, UnityActionAndStepGrantsDoNotLeak) {
#if !defined(__APPLE__)
  IUTEST_SKIP() << "Seatbelt requires macOS";
#else
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  for (const bool grant : {false, true}) {
    for (const bool grant_node : {false, true}) {
      ActionFixture f;
      f.file("check-java.sh", R"(expected="$1"
expected="$expected \"-Duser.home=$HOME\" \"-Dmaven.repo.local=$HOME/.m2/repository\""
if [ "$2" = unity-allow ]; then
  port="${HTTPS_PROXY##*:}"
  expected="$expected -Djava.net.preferIPv4Stack=true -Dhttp.proxyHost=127.0.0.1 -Dhttp.proxyPort=$port -Dhttps.proxyHost=127.0.0.1 -Dhttps.proxyPort=$port -Dhttp.nonProxyHosts="
fi
expected="$expected \"-Djava.io.tmpdir=$TMPDIR\""
test "$JDK_JAVA_OPTIONS" = "$expected"
)");
      f.file("outer/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        test "$DOTNET_USE_POLLING_FILE_WATCHER" = "$EXPECT_POLLING"
        bash "$GITHUB_WORKSPACE/check-java.sh" "$EXPECT_JAVA" "$EXPECT"
        test "$JAVA_TOOL_OPTIONS" = "-Djava.tool.options=preserved"
        "$PROBE" "$EXPECT" ""
    - uses: ./inner
    - uses: ./js
)");
      f.file("inner/action.yml", R"(runs:
  using: composite
  steps:
    - id: permitted
      shell: bash
      run: |
        test "$DOTNET_USE_POLLING_FILE_WATCHER" = original
        bash "$GITHUB_WORKSPACE/check-java.sh" "$ORIGINAL_JAVA" unity-deny
        "$PROBE" unity-deny ""
)");
      f.file("js/action.yml", "runs:\n  using: node24\n  pre: pre.js\n  main: main.js\n  post: post.js\n");
      for (const auto* phase : {"pre", "main", "post"}) {
        f.file("js/" + std::string(phase) + ".js",
               "const tmp = process.env.TMPDIR;\n"
               "if ((process.env.NODE_EXPECT === 'unity-allow') !== tmp.startsWith('/private/tmp/cpa-u-')) "
               "throw Error('short TMPDIR escaped its Unity scope');\n"
               "if (process.env.DOTNET_USE_POLLING_FILE_WATCHER !== process.env.NODE_POLLING) "
               "throw Error('polling environment escaped its Unity scope');\n"
               "const port = new URL(process.env.HTTPS_PROXY).port;\n"
               "const homes = ` \"-Duser.home=${process.env.HOME}\""
               " \"-Dmaven.repo.local=${process.env.HOME}/.m2/repository\"`;\n"
               "const proxy = process.env.NODE_EXPECT === 'unity-allow' ? "
               "` -Djava.net.preferIPv4Stack=true -Dhttp.proxyHost=127.0.0.1 -Dhttp.proxyPort=${port}"
               " -Dhttps.proxyHost=127.0.0.1 -Dhttps.proxyPort=${port} -Dhttp.nonProxyHosts=` : '';\n"
               "const temp = ` \"-Djava.io.tmpdir=${tmp}\"`;\n"
               "if (process.env.JDK_JAVA_OPTIONS !== process.env.NODE_JAVA + homes + proxy + temp) "
               "throw Error('Java options escaped their Unity scope');\n"
               "if (process.env.JAVA_TOOL_OPTIONS !== '-Djava.tool.options=preserved') "
               "throw Error('JAVA_TOOL_OPTIONS was modified');\n"
               "require('child_process').execFileSync(process.env.PROBE, [process.env.NODE_EXPECT, '']);\n"
               "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, '" + std::string(phase) + "\\n');\n");
      }
      const auto report = f.run(R"(      - id: before
        run: |
          test "$DOTNET_USE_POLLING_FILE_WATCHER" = original
          bash "$GITHUB_WORKSPACE/check-java.sh" "$ORIGINAL_JAVA" unity-deny
          "$PROBE" unity-deny ""
      - run: |
          test "$DOTNET_USE_POLLING_FILE_WATCHER" = original
          bash "$GITHUB_WORKSPACE/check-java.sh" "$ORIGINAL_JAVA" unity-deny
          "$PROBE" unity-deny ""
      - id: permitted
        run: |
          test "$DOTNET_USE_POLLING_FILE_WATCHER" = "$EXPECT_POLLING"
          bash "$GITHUB_WORKSPACE/check-java.sh" "$EXPECT_JAVA" "$EXPECT"
          test "$JAVA_TOOL_OPTIONS" = "-Djava.tool.options=preserved"
          "$PROBE" "$EXPECT" ""
      - uses: ./outer
      - id: after
        run: |
          test "$DOTNET_USE_POLLING_FILE_WATCHER" = original
          bash "$GITHUB_WORKSPACE/check-java.sh" "$ORIGINAL_JAVA" unity-deny
          "$PROBE" unity-deny ""
)", *node, {}, [grant, grant_node](RunOptions& options) {
        options.sandbox.enabled = true;
        options.sandbox.allow_read = {CPA_MSBUILD_SOCKET_PROBE};
        const std::string original_java =
            "-Dprobe=preserved -Djava.net.preferIPv4Stack=false -Dhttps.proxyHost=wrong.invalid "
            "-Dhttps.proxyPort=1 -Dhttp.proxyHost=wrong.invalid -Dhttp.proxyPort=1 -Dhttp.nonProxyHosts=*";
        options.pass_env = {std::string("PROBE=") + CPA_MSBUILD_SOCKET_PROBE,
                            "JDK_JAVA_OPTIONS=" + original_java,
                            "JAVA_TOOL_OPTIONS=-Djava.tool.options=preserved",
                            "ORIGINAL_JAVA=" + original_java,
                            "EXPECT_JAVA=" + original_java,
                            "NODE_JAVA=" + original_java,
                            "DOTNET_USE_POLLING_FILE_WATCHER=original",
                            std::string("EXPECT_POLLING=") + (grant ? "1" : "original"),
                            std::string("NODE_POLLING=") + (grant_node ? "1" : "original"),
                            std::string("EXPECT=") + (grant ? "unity-allow" : "unity-deny"),
                            std::string("NODE_EXPECT=") + (grant_node ? "unity-allow" : "unity-deny")};
        if (grant) {
          options.sandbox.allow_unity_for = {"./outer"};
          options.sandbox.allow_unity_step = {".github/workflows/test.yml::test::permitted",
                                              ".github/workflows/test.yml::test::__run",
                                              ".github/workflows/other.yml::test::before",
                                              ".github/workflows/test.yml::other::after"};
        }
        if (grant_node) options.sandbox.allow_unity_for.push_back("./js");
      });
      IUTEST_EXPECT_EQ("success", report.result) << f.log;
      IUTEST_EXPECT_EQ("pre\nmain\npost\n", report.summary) << f.log;
      if (grant || grant_node) {
        const auto prefix = std::string("Unity TMPDIR: ");
        const auto position = f.log.find(prefix);
        IUTEST_ASSERT_NE(std::string::npos, position);
        const auto begin = position + prefix.size();
        const auto path = f.log.substr(begin, f.log.find('\n', begin) - begin);
        IUTEST_EXPECT_FALSE(std::filesystem::exists(path)) << path;
      }
      IUTEST_EXPECT_EQ(grant || grant_node, f.log.find("Unity compatibility enabled") != std::string::npos);
    }
  }
#endif
}

IUTEST(ActionRunnerTest, PollingFileWatcherDefaultsForSandboxedProcesses) {
#if !defined(__APPLE__)
  IUTEST_SKIP() << "Seatbelt requires macOS";
#else
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  f.file("composite/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: test "$DOTNET_USE_POLLING_FILE_WATCHER" = 1
)");
  f.file("node/action.yml", "runs:\n  using: node24\n  main: main.js\n");
  f.file("node/main.js", R"js(
if (process.env.DOTNET_USE_POLLING_FILE_WATCHER !== '1') {
  throw Error('Polling watcher default missing in Node action');
}
)js");
  const auto job = f.run(R"(      - run: test "$DOTNET_USE_POLLING_FILE_WATCHER" = 1
      - uses: ./composite
      - uses: ./node
      - env:
          DOTNET_USE_POLLING_FILE_WATCHER: '0'
        run: test "$DOTNET_USE_POLLING_FILE_WATCHER" = 0
)", *node, {}, [](RunOptions& options) { options.sandbox.enabled = true; });
  IUTEST_EXPECT_EQ("success", job.result) << f.log;

  ActionFixture unsandboxed;
  const auto unsandboxed_job = unsandboxed.run(R"(      - run: test -z "${DOTNET_USE_POLLING_FILE_WATCHER+x}"
)");
  IUTEST_EXPECT_EQ("success", unsandboxed_job.result) << unsandboxed.log;
#endif
}

IUTEST(ActionRunnerTest, UnityJavaProxyDefaultsRespectSandboxAndLocalhostPolicy) {
#if !defined(__APPLE__)
  IUTEST_SKIP() << "Seatbelt requires macOS";
#else
  for (const bool sandbox : {false, true}) {
    for (const bool localhost : {false, true}) {
      ActionFixture f;
      const auto report = f.run(R"(      - id: unity
        run: |
          expected="\"-Duser.home=$HOME\" \"-Dmaven.repo.local=$HOME/.m2/repository\""
          if [ "$EXPECT_SANDBOX" = true ]; then
            port="${HTTPS_PROXY##*:}"
            expected="$expected -Djava.net.preferIPv4Stack=true -Dhttp.proxyHost=127.0.0.1 -Dhttp.proxyPort=$port -Dhttps.proxyHost=127.0.0.1 -Dhttps.proxyPort=$port -Dhttp.nonProxyHosts=$EXPECT_BYPASS"
          fi
          expected="$expected \"-Djava.io.tmpdir=$TMPDIR\""
          test "$JDK_JAVA_OPTIONS" = "$expected"
)", {}, {}, [sandbox, localhost](RunOptions& options) {
        options.sandbox.enabled = sandbox;
        options.sandbox.allow_localhost = localhost;
        options.sandbox.allow_unity_step = {".github/workflows/test.yml::test::unity"};
        options.pass_env = {std::string("EXPECT_SANDBOX=") + (sandbox ? "true" : "false"),
                            std::string("EXPECT_BYPASS=") + (localhost ? "localhost|127.0.0.1|[::1]" : "")};
      });
      IUTEST_EXPECT_EQ("success", report.result) << f.log;
    }
  }
#endif
}

IUTEST(ActionRunnerTest, UnityShortTmpIsScopedPrivateAndRetainedWhenRequested) {
#if !defined(__APPLE__)
  IUTEST_SKIP() << "Seatbelt requires macOS";
#else
  for (const int mode : {0, 1, 2, 3}) {
    const bool keep = (mode & 1) != 0;
    const bool fail = (mode & 2) != 0;
    ActionFixture f;
    const auto report = f.run(R"yaml(      - id: before
        run: |
          case "$TMPDIR" in */jobs/*/tmp) ;; *) exit 1 ;; esac
      - id: unity
        run: |
          case "$TMPDIR" in /private/tmp/cpa-u-*/) ;; *) exit 2 ;; esac
          test "$(stat -f %Lp "$TMPDIR")" = 700
          echo "$TMPDIR" > "$GITHUB_WORKSPACE/unity-path"
          mkdir "$TMPDIR/9hvazisp.dsn"
          "$PROBE" socket-allow "$TMPDIR/9hvazisp.dsn/ipc_26048_htc_0"
      - id: after
        run: |
          case "$TMPDIR" in */jobs/*/tmp) ;; *) exit 3 ;; esac
          short=$(cat "$GITHUB_WORKSPACE/unity-path")
          "$PROBE" file-deny "$short/unrelated"
          "$PROBE" list-deny "$short"
          "$PROBE" socket-deny "$short/other.sock"
          exit "$FINAL_EXIT"
)yaml", {}, {}, [keep, fail](RunOptions& options) {
      options.sandbox.enabled = true;
      options.keep_workdir = keep;
      options.sandbox.allow_read = {CPA_MSBUILD_SOCKET_PROBE};
      options.pass_env = {std::string("PROBE=") + CPA_MSBUILD_SOCKET_PROBE,
                          std::string("FINAL_EXIT=") + (fail ? "7" : "0")};
      options.sandbox.allow_unity_step = {".github/workflows/test.yml::test::unity"};
    });
    IUTEST_EXPECT_EQ(fail ? "failure" : "success", report.result) << f.log;
    const auto prefix = std::string("Unity TMPDIR: ");
    const auto position = f.log.find(prefix);
    IUTEST_ASSERT_NE(std::string::npos, position);
    const auto begin = position + prefix.size();
    const auto path = f.log.substr(begin, f.log.find('\n', begin) - begin);
    IUTEST_EXPECT_EQ(keep, std::filesystem::exists(path));
    if (keep) {
      IUTEST_EXPECT_NE(std::string::npos, f.log.find("Retained Unity TMPDIR: " + path));
      bool found = false;
      for (const auto& entry : std::filesystem::recursive_directory_iterator(f.dir / "run/jobs")) {
        if (entry.path().filename() == "unity-tmp-path") {
          found = true;
          IUTEST_EXPECT_EQ(path + "\n", util::read_file(entry.path()));
        }
      }
      IUTEST_EXPECT_TRUE(found);
      util::remove_tree(path);
    }
  }
#endif
}

IUTEST(ActionRunnerTest, XcodeCompositePermissionsAreScopedAndNotInherited) {
  const auto user_dirs = sandbox::darwin_user_dirs();
  IUTEST_ASSERT_FALSE(user_dirs.empty());
  const auto temp = util::real_path(user_dirs.front());
  const auto path = temp / ("swbuild.tmp.cpa-action-" + util::random_uuid());
  const auto storyboard = temp / (util::random_uuid() + ".storyboard");
  const auto asset_uuid = temp / util::random_uuid();
  const auto asset_scratch = temp / ("actool-sprite-atlas-scratch-" + util::random_uuid());
  const auto ibtoold = temp / ("ibtoold-" + std::to_string(::getpid()) +
                               std::to_string(std::hash<std::string>{}(util::random_uuid())));
  std::filesystem::create_directories(temp / "TemporaryItems");
  const auto ibtool_items = temp / "TemporaryItems" / ("NSIRD_ibtool_cpa-action-" + util::random_uuid());
  const auto simulator = temp / ("com.apple.CoreSimulator.SimDevice." + util::random_uuid() +
                                 ".Standalone." + util::random_uuid());
  for (const bool allow_simulator : {false, true}) {
    for (const bool allow_outer : {false, true}) {
      for (const bool allow_inner : {false, true}) {
        ActionFixture f;
        f.file("outer/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        "$PROBE" "file-$OUTER_EXPECT" "$XCODE_PATH"
        "$PROBE" "file-$OUTER_EXPECT" "$STORYBOARD_PATH"
        "$PROBE" file-deny "$ASSET_UUID_PATH"
        "$PROBE" file-deny "$ASSET_SCRATCH_PATH"
        "$PROBE" "mkdir-$OUTER_EXPECT" "$IBTOOLD_PATH"
        "$PROBE" "mkdir-$OUTER_EXPECT" "$IBTOOL_ITEMS_PATH"
        "$PROBE" "file-$OUTER_SIM_EXPECT" "$SIMULATOR_PATH"
    - uses: ./inner
    - shell: bash
      run: |
        "$PROBE" "file-$OUTER_EXPECT" "$XCODE_PATH"
        "$PROBE" "file-$OUTER_EXPECT" "$STORYBOARD_PATH"
        "$PROBE" file-deny "$ASSET_UUID_PATH"
        "$PROBE" file-deny "$ASSET_SCRATCH_PATH"
        "$PROBE" "mkdir-$OUTER_EXPECT" "$IBTOOLD_PATH"
        "$PROBE" "mkdir-$OUTER_EXPECT" "$IBTOOL_ITEMS_PATH"
        "$PROBE" "file-$OUTER_SIM_EXPECT" "$SIMULATOR_PATH"
)");
        f.file("inner/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        "$PROBE" "file-$INNER_EXPECT" "$XCODE_PATH"
        "$PROBE" "file-$INNER_EXPECT" "$STORYBOARD_PATH"
        "$PROBE" file-deny "$ASSET_UUID_PATH"
        "$PROBE" file-deny "$ASSET_SCRATCH_PATH"
        "$PROBE" "mkdir-$INNER_EXPECT" "$IBTOOLD_PATH"
        "$PROBE" "mkdir-$INNER_EXPECT" "$IBTOOL_ITEMS_PATH"
        "$PROBE" "file-$INNER_SIM_EXPECT" "$SIMULATOR_PATH"
)");
        const auto job = f.run(R"(      - run: |
          "$PROBE" file-deny "$XCODE_PATH"
          "$PROBE" file-deny "$STORYBOARD_PATH"
          "$PROBE" file-deny "$ASSET_UUID_PATH"
          "$PROBE" file-deny "$ASSET_SCRATCH_PATH"
          "$PROBE" mkdir-deny "$IBTOOLD_PATH"
          "$PROBE" mkdir-deny "$IBTOOL_ITEMS_PATH"
          "$PROBE" file-deny "$SIMULATOR_PATH"
          echo ALLOW_XCODE_FOR=true >> "$GITHUB_ENV"
      - uses: ./outer
      - run: |
          "$PROBE" file-deny "$XCODE_PATH"
          "$PROBE" file-deny "$STORYBOARD_PATH"
          "$PROBE" file-deny "$ASSET_UUID_PATH"
          "$PROBE" file-deny "$ASSET_SCRATCH_PATH"
          "$PROBE" mkdir-deny "$IBTOOLD_PATH"
          "$PROBE" mkdir-deny "$IBTOOL_ITEMS_PATH"
          "$PROBE" file-deny "$SIMULATOR_PATH"
)", {}, {}, [&](RunOptions& options) {
          options.sandbox.enabled = true;
          options.sandbox.allow_xcode_simulator = allow_simulator;
          options.sandbox.allow_read = {CPA_MSBUILD_SOCKET_PROBE};
          if (allow_outer) options.sandbox.allow_xcode_for.push_back("./outer");
          if (allow_inner) options.sandbox.allow_xcode_for.push_back("./inner");
          options.pass_env = {std::string("PROBE=") + CPA_MSBUILD_SOCKET_PROBE,
                              "XCODE_PATH=" + path.string(),
                              "STORYBOARD_PATH=" + storyboard.string(),
                              "ASSET_UUID_PATH=" + asset_uuid.string(),
                              "ASSET_SCRATCH_PATH=" + asset_scratch.string(),
                              "IBTOOLD_PATH=" + ibtoold.string(),
                              "IBTOOL_ITEMS_PATH=" + ibtool_items.string(),
                              "SIMULATOR_PATH=" + simulator.string(),
                              std::string("OUTER_EXPECT=") + (allow_outer ? "allow" : "deny"),
                              std::string("INNER_EXPECT=") + (allow_inner ? "allow" : "deny"),
                              std::string("OUTER_SIM_EXPECT=") + (allow_outer && allow_simulator ? "allow" : "deny"),
                              std::string("INNER_SIM_EXPECT=") + (allow_inner && allow_simulator ? "allow" : "deny")};
        });
        IUTEST_EXPECT_EQ("success", job.result) << f.log;
        IUTEST_EXPECT_EQ(allow_outer || allow_inner,
                         f.log.find("Xcode temporary-file access enabled") != std::string::npos);
        if (allow_outer || allow_inner) {
          const auto warning = allow_simulator ? "matching host Xcode/Simulator temporary entries"
                                               : "matching host Xcode temporary entries";
          IUTEST_EXPECT_NE(std::string::npos, f.log.find(warning)) << f.log;
          IUTEST_EXPECT_NE(std::string::npos,
                           f.log.find("including unrelated distribution-pipeline and UUID-named entries"))
              << f.log;
        }
      }
    }
  }
}

IUTEST(ActionRunnerTest, XcodeAssetSymbolsGeneratedInSandbox) {
  const char* enabled = std::getenv("CPA_TEST_XCODE_ASSET_SYMBOLS");
  if (enabled == nullptr || std::string(enabled) != "1") {
    IUTEST_SKIP() << "Set CPA_TEST_XCODE_ASSET_SYMBOLS=1 to run the Xcode asset integration test";
  }
  const auto sdks = util::capture({"/usr/bin/xcodebuild", "-showsdks"});
  const auto runtimes = util::capture({"/usr/bin/xcrun", "simctl", "list", "runtimes", "available"});
  if (sdks.exit_code != 0 || sdks.out.find("-sdk iphoneos") == std::string::npos ||
      runtimes.exit_code != 0 || runtimes.out.find("iOS ") == std::string::npos) {
    IUTEST_SKIP() << "Xcode, an iOS SDK, and an iOS Simulator runtime are required";
  }
  ActionFixture f;
  f.file("Images.xcassets/Contents.json", R"json({"info":{"author":"xcode","version":1}})json");
  f.file("Images.xcassets/AccentColor.colorset/Contents.json", R"json({
  "colors": [
    {
      "idiom": "universal",
      "color": {
        "color-space": "srgb",
        "components": {"red": "1.0", "green": "0.0", "blue": "0.0", "alpha": "1.0"}
      }
    }
  ],
  "info": {"author": "xcode", "version": 1}
})json");
  f.file("xcode/action.yml", R"yaml(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        mkdir -p "$RUNNER_TEMP/DerivedSources" "$RUNNER_TEMP/AssetOutput"
        xcrun actool "$GITHUB_WORKSPACE/Images.xcassets" \
          --compile "$RUNNER_TEMP/AssetOutput" \
          --output-format human-readable-text --notices --warnings \
          --export-dependency-info "$RUNNER_TEMP/assetcatalog_dependencies" \
          --output-partial-info-plist "$RUNNER_TEMP/assetcatalog_generated_info.plist" \
          --target-device iphone --target-device ipad --minimum-deployment-target 15.0 \
          --platform iphoneos --bundle-identifier local.cpa.assetprobe \
          --generate-objc-asset-symbols "$RUNNER_TEMP/DerivedSources/GeneratedAssetSymbols.h" \
          --generate-asset-symbol-index "$RUNNER_TEMP/DerivedSources/GeneratedAssetSymbols-Index.plist"
        test -s "$RUNNER_TEMP/DerivedSources/GeneratedAssetSymbols.h"
        grep -Fq AccentColor "$RUNNER_TEMP/DerivedSources/GeneratedAssetSymbols.h"
        test -s "$RUNNER_TEMP/DerivedSources/GeneratedAssetSymbols-Index.plist"
        plutil -lint "$RUNNER_TEMP/DerivedSources/GeneratedAssetSymbols-Index.plist"
        plutil -p "$RUNNER_TEMP/DerivedSources/GeneratedAssetSymbols-Index.plist" | grep -Fq AccentColor
        echo verified >> "$GITHUB_STEP_SUMMARY"
)yaml");
  const auto job = f.run("      - uses: ./xcode\n", {}, {}, [&](RunOptions& options) {
    options.sandbox.enabled = true;
    options.sandbox.allow_xcode_simulator = true;
    options.sandbox.allow_xcode_for = {"./xcode"};
    options.sandbox.allow_unity_for = {"./xcode"};
    const auto environment = util::current_environment();
    const auto developer = environment.find("DEVELOPER_DIR");
    if (developer != environment.end() && !developer->second.empty()) {
      options.pass_env.push_back("DEVELOPER_DIR=" + developer->second);
      options.sandbox.allow_read.push_back(developer->second);
    }
  });
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ("verified\n", job.summary) << f.log;
}

IUTEST(ActionRunnerTest, XcodeAssetCatalogCompilesInSandbox) {
  const char* enabled = std::getenv("CPA_TEST_XCODE_ASSET_CATALOG");
  if (enabled == nullptr || std::string(enabled) != "1") {
    IUTEST_SKIP() << "Set CPA_TEST_XCODE_ASSET_CATALOG=1 to run the Xcode asset catalog integration test";
  }
  const auto sdks = util::capture({"/usr/bin/xcodebuild", "-showsdks"});
  const auto runtimes = util::capture({"/usr/bin/xcrun", "simctl", "list", "runtimes", "available"});
  if (sdks.exit_code != 0 || sdks.out.find("-sdk iphoneos") == std::string::npos || runtimes.exit_code != 0 ||
      runtimes.out.find("iOS ") == std::string::npos) {
    IUTEST_SKIP() << "Xcode, an iOS SDK, and an iOS Simulator runtime are required";
  }
  const std::string pixel(
      "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
      "\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90\x77\x53"
      "\xde\x00\x00\x00\x0c\x49\x44\x41\x54\x78\x9c\x63\xf8\xcf\xc0\x00"
      "\x00\x03\x01\x01\x00\xc9\xfe\x92\xef\x00\x00\x00\x00\x49\x45\x4e"
      "\x44\xae\x42\x60\x82",
      69);
  for (const int mode : {0, 1, 2}) {
    const bool use_sandbox = mode != 0;
    const bool allow_xcode = mode == 2;
    ActionFixture f;
    f.file("Images.xcassets/Contents.json", R"json({"info":{"author":"xcode","version":1}})json");
    f.file("Images.xcassets/AppIcon.appiconset/Contents.json", R"json({
  "images": [{"filename":"AppIcon.png","idiom":"universal","platform":"ios","size":"1024x1024"}],
  "info": {"author":"xcode","version":1}
})json");
    f.file("Images.xcassets/AppIcon.appiconset/AppIcon.png", pixel);
    f.file("Images.xcassets/TestImage.imageset/Contents.json", R"json({
  "images": [{"filename":"TestImage.png","idiom":"universal","scale":"1x"}],
  "info": {"author":"xcode","version":1}
})json");
    f.file("Images.xcassets/TestImage.imageset/TestImage.png", pixel);
    const auto icon = f.dir / "repo/Images.xcassets/AppIcon.appiconset/AppIcon.png";
    const auto resized = util::capture({"/usr/bin/sips", "-z", "1024", "1024", icon.string()});
    IUTEST_ASSERT_EQ(0, resized.exit_code) << resized.out << resized.err;
    f.file("xcode/action.yml", R"yaml(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        mkdir -p "$RUNNER_TEMP/AssetOutput"
        xcrun actool "$GITHUB_WORKSPACE/Images.xcassets" \
          --compile "$RUNNER_TEMP/AssetOutput" \
          --output-format human-readable-text --notices --warnings \
          --export-dependency-info "$RUNNER_TEMP/assetcatalog_dependencies" \
          --output-partial-info-plist "$RUNNER_TEMP/assetcatalog_generated_info.plist" \
          --app-icon AppIcon --compress-pngs --enable-on-demand-resources NO \
          --development-region English --target-device iphone --target-device ipad \
          --minimum-deployment-target 15.0 --platform iphoneos
        test -s "$RUNNER_TEMP/AssetOutput/Assets.car"
        test -s "$RUNNER_TEMP/AssetOutput/AppIcon60x60@2x.png"
        xcrun assetutil --info "$RUNNER_TEMP/AssetOutput/Assets.car" > "$RUNNER_TEMP/assetutil.json"
        grep -Fq TestImage "$RUNNER_TEMP/assetutil.json"
        test -s "$RUNNER_TEMP/assetcatalog_generated_info.plist"
        plutil -lint "$RUNNER_TEMP/assetcatalog_generated_info.plist"
        plutil -p "$RUNNER_TEMP/assetcatalog_generated_info.plist" | grep -Fq CFBundleIcons
        echo verified >> "$GITHUB_STEP_SUMMARY"
)yaml");
    const auto job = f.run("      - uses: ./xcode\n", {}, {}, [&](RunOptions& options) {
      options.sandbox.enabled = use_sandbox;
      options.sandbox.allow_xcode_simulator = true;
      if (allow_xcode) options.sandbox.allow_xcode_for = {"./xcode"};
      const auto environment = util::current_environment();
      const auto developer = environment.find("DEVELOPER_DIR");
      if (developer != environment.end() && !developer->second.empty()) {
        options.pass_env.push_back("DEVELOPER_DIR=" + developer->second);
        options.sandbox.allow_read.push_back(developer->second);
      }
    });
    IUTEST_ASSERT_EQ(mode == 1 ? "failure" : "success", job.result) << "mode=" << mode << "\n" << f.log;
    if (mode == 1) {
      IUTEST_EXPECT_NE(std::string::npos, f.log.find("com.apple.actool.errors")) << f.log;
      IUTEST_EXPECT_TRUE(job.summary.empty()) << f.log;
    } else {
      IUTEST_EXPECT_EQ("verified\n", job.summary) << f.log;
    }
  }
}

IUTEST(ActionRunnerTest, XcodeStoryboardCompilesInSandbox) {
  const char* enabled = std::getenv("CPA_TEST_XCODE_STORYBOARD");
  if (enabled == nullptr || std::string(enabled) != "1") {
    IUTEST_SKIP() << "Set CPA_TEST_XCODE_STORYBOARD=1 to run the host Simulator integration test";
  }
  const auto sdks = util::capture({"/usr/bin/xcodebuild", "-showsdks"});
  const auto runtimes = util::capture({"/usr/bin/xcrun", "simctl", "list", "runtimes", "available"});
  if (sdks.exit_code != 0 || sdks.out.find("-sdk iphoneos") == std::string::npos ||
      runtimes.exit_code != 0 || runtimes.out.find("iOS ") == std::string::npos) {
    IUTEST_SKIP() << "Xcode, an iOS SDK, and an iOS Simulator runtime are required";
  }
  ActionFixture f;
  const std::string storyboard = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<document type="com.apple.InterfaceBuilder3.CocoaTouch.Storyboard.XIB" version="3.0" toolsVersion="23094" targetRuntime="iOS.CocoaTouch" useAutolayout="YES" launchScreen="YES" useTraitCollections="YES" initialViewController="cpa-vc-001">
  <device id="retina6_12" orientation="portrait" appearance="light"/>
  <dependencies>
    <deployment identifier="iOS"/>
    <plugIn identifier="com.apple.InterfaceBuilder.IBCocoaTouchPlugin" version="23084"/>
  </dependencies>
  <scenes>
    <scene sceneID="cpa-scene-001">
      <objects>
        <viewController id="cpa-vc-001" sceneMemberID="viewController">
          <view key="view" contentMode="scaleToFill" id="cpa-view-001">
            <rect key="frame" x="0.0" y="0.0" width="393" height="852"/>
            <color key="backgroundColor" red="1" green="1" blue="1" alpha="1" colorSpace="custom" customColorSpace="sRGB"/>
          </view>
        </viewController>
        <placeholder placeholderIdentifier="IBFirstResponder" id="cpa-first-001" sceneMemberID="firstResponder"/>
      </objects>
    </scene>
  </scenes>
</document>
)xml";
  f.file("LaunchScreen-iPhone.storyboard", storyboard);
  f.file("LaunchScreen-iPad.storyboard", storyboard);
  f.file("xcode/action.yml", R"yaml(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        mkdir -p "$RUNNER_TEMP/Compiled"
        for name in LaunchScreen-iPhone LaunchScreen-iPad; do
          xcrun ibtool --errors --warnings --notices --module CpaStoryboardProbe \
            --output-partial-info-plist "$RUNNER_TEMP/$name-SBPartialInfo.plist" \
            --auto-activate-custom-fonts --target-device iphone --target-device ipad \
            --minimum-deployment-target 15.0 --output-format human-readable-text \
            "$GITHUB_WORKSPACE/$name.storyboard" --compilation-directory "$RUNNER_TEMP/Compiled"
          test -s "$RUNNER_TEMP/$name-SBPartialInfo.plist"
          test -s "$RUNNER_TEMP/Compiled/$name.storyboardc/Info.plist"
          test -n "$(find "$RUNNER_TEMP/Compiled/$name.storyboardc" -type f -name '*.nib' -size +0c -print -quit)"
        done
        echo verified >> "$GITHUB_STEP_SUMMARY"
)yaml");
  const auto job = f.run("      - uses: ./xcode\n", {}, {}, [&](RunOptions& options) {
    options.sandbox.enabled = true;
    options.sandbox.allow_xcode_simulator = true;
    options.sandbox.allow_xcode_for = {"./xcode"};
    options.sandbox.allow_unity_for = {"./xcode"};
    const auto environment = util::current_environment();
    const auto developer = environment.find("DEVELOPER_DIR");
    if (developer != environment.end() && !developer->second.empty()) {
      options.pass_env.push_back("DEVELOPER_DIR=" + developer->second);
      options.sandbox.allow_read.push_back(developer->second);
    }
  });
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ("verified\n", job.summary) << f.log;
}

IUTEST(ActionRunnerTest, XcodeArchiveUsesScopedTemporaryFiles) {
  const auto sdks = util::capture({"/usr/bin/xcodebuild", "-showsdks"});
  if (sdks.exit_code != 0 || sdks.out.find("-sdk iphoneos") == std::string::npos) {
    IUTEST_SKIP() << "Xcode and an iOS SDK are required";
  }
  cpa_test::TempDir project;
  util::write_file(project / "CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.24)
project(CpaXcodeProbe LANGUAGES C)
add_executable(CpaXcodeProbe MACOSX_BUNDLE main.c)
set_target_properties(CpaXcodeProbe PROPERTIES
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER local.cpa-xcode-probe
  XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE YES)
)");
  util::write_file(project / "main.c", "int main(void) { return 0; }\n");
  const auto generated = util::capture(
      {CPA_CMAKE_COMMAND, "-S", project.path().string(), "-B", (project / "build").string(),
       "-G", "Xcode", "-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_OSX_SYSROOT=iphoneos",
       "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY", "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO",
       "-DCMAKE_SUPPRESS_REGENERATION=ON"});
  IUTEST_ASSERT_EQ(0, generated.exit_code) << generated.out << generated.err;
  for (const bool allow_xcode : {false, true}) {
    ActionFixture f;
    f.file("xcode/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        xcodebuild -project "$PROJECT_PATH" -scheme CpaXcodeProbe \
          -sdk iphoneos -configuration Release archive \
          -archivePath "$RUNNER_TEMP/CpaXcodeProbe.xcarchive" \
          -derivedDataPath "$RUNNER_TEMP/DerivedData" \
          -IDEBuildOperationMaxNumberOfConcurrentCompileTasks=2 \
          CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
        test -f "$RUNNER_TEMP/DerivedData/info.plist"
        test -d "$RUNNER_TEMP/CpaXcodeProbe.xcarchive"
        echo verified >> "$GITHUB_STEP_SUMMARY"
)");
    const auto job = f.run("      - uses: ./xcode\n", {}, {}, [&](RunOptions& options) {
      options.sandbox.enabled = true;
      options.sandbox.allow_write = {project.path()};
      options.pass_env = {"PROJECT_PATH=" + (project / "build/CpaXcodeProbe.xcodeproj").string()};
      const auto environment = util::current_environment();
      const auto developer = environment.find("DEVELOPER_DIR");
      if (developer != environment.end() && !developer->second.empty()) {
        options.pass_env.push_back("DEVELOPER_DIR=" + developer->second);
        options.sandbox.allow_read.push_back(developer->second);
      }
      if (allow_xcode) options.sandbox.allow_xcode_for = {"./xcode"};
    });
    if (allow_xcode) {
      IUTEST_EXPECT_EQ("success", job.result) << f.log;
      IUTEST_EXPECT_EQ("verified\n", job.summary) << f.log;
      IUTEST_EXPECT_NE(std::string::npos, f.log.find("Xcode temporary-file access enabled"));
    } else if (job.result != "success") {
      IUTEST_EXPECT_EQ("failure", job.result) << f.log;
      IUTEST_EXPECT_NE(std::string::npos, f.log.find("Couldn't create workspace arena folder")) << f.log;
      IUTEST_EXPECT_NE(std::string::npos, f.log.find("Unable to write to info file")) << f.log;
    }
    if (!allow_xcode) {
      IUTEST_EXPECT_EQ(std::string::npos, f.log.find("Xcode temporary-file access enabled"));
    }
  }
}

IUTEST(ActionRunnerTest, XcodeNodeLifecycleMatchesItsOwnActionNotItsParent) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  const auto user_dirs = sandbox::darwin_user_dirs();
  IUTEST_ASSERT_FALSE(user_dirs.empty());
  const auto path = util::real_path(user_dirs.front()) / ("swbuild.tmp.cpa-node-" + util::random_uuid());
  for (const bool allow_node : {false, true}) {
    ActionFixture f;
    f.file("outer/action.yml", R"(runs:
  using: composite
  steps:
    - uses: ./js
)");
    f.file("js/action.yml", "runs:\n  using: node20\n  pre: pre.js\n  main: main.js\n  post: post.js\n");
    const std::string probe = R"(const {spawnSync} = require('child_process');
const result = spawnSync(process.env.PROBE, ['file-' + process.env.NODE_EXPECT, process.env.XCODE_PATH]);
if (result.status !== 0) throw Error('unexpected Xcode permission: ' + result.stdout + result.stderr);
)";
    for (const auto* phase : {"pre", "main", "post"}) {
      f.file(std::string("js/") + phase + ".js", probe +
             "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, '" + phase + "\\n');\n");
    }
    const auto job = f.run(R"(      - uses: ./outer
      - run: '"$PROBE" file-deny "$XCODE_PATH"'
)", *node, {}, [&](RunOptions& options) {
      options.sandbox.enabled = true;
      options.sandbox.allow_read = {CPA_MSBUILD_SOCKET_PROBE};
      options.sandbox.allow_xcode_for = {allow_node ? "./js" : "./outer"};
      options.pass_env = {std::string("PROBE=") + CPA_MSBUILD_SOCKET_PROBE,
                          "XCODE_PATH=" + path.string(),
                          std::string("NODE_EXPECT=") + (allow_node ? "allow" : "deny")};
    });
    IUTEST_EXPECT_EQ("success", job.result) << f.log;
    IUTEST_EXPECT_EQ("pre\nmain\npost\n", job.summary) << f.log;
  }
}

IUTEST(ActionRunnerTest, AppleSecurityCompositePermissionsAreScopedAndNotInherited) {
  for (const bool allow_outer : {false, true}) {
    for (const bool allow_inner : {false, true}) {
      ActionFixture f;
      f.file("outer/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        "$APPLE_SECURITY_PROBE" com.apple.SecurityServer "$OUTER_EXPECT"
        "$APPLE_SECURITY_PROBE" com.apple.trustd.agent "$OUTER_EXPECT"
    - uses: ./inner
    - shell: bash
      run: |
        "$APPLE_SECURITY_PROBE" com.apple.SecurityServer "$OUTER_EXPECT"
        "$APPLE_SECURITY_PROBE" com.apple.trustd.agent "$OUTER_EXPECT"
)");
      f.file("inner/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        "$APPLE_SECURITY_PROBE" com.apple.SecurityServer "$INNER_EXPECT"
        "$APPLE_SECURITY_PROBE" com.apple.trustd.agent "$INNER_EXPECT"
)");
      const auto job = f.run(R"(      - run: |
          "$APPLE_SECURITY_PROBE" com.apple.SecurityServer deny
          echo ALLOW_APPLE_SECURITY=true >> "$GITHUB_ENV"
      - uses: ./outer
      - run: |
          "$APPLE_SECURITY_PROBE" com.apple.SecurityServer deny
          "$APPLE_SECURITY_PROBE" com.apple.trustd.agent deny
)", {}, {}, [allow_outer, allow_inner](RunOptions& options) {
        options.sandbox.enabled = true;
        options.sandbox.allow_read = {CPA_APPLE_SECURITY_PROBE};
        if (allow_outer) options.sandbox.allow_apple_security_for.push_back("./outer");
        if (allow_inner) options.sandbox.allow_apple_security_for.push_back("./inner");
        options.pass_env = {std::string("APPLE_SECURITY_PROBE=") + CPA_APPLE_SECURITY_PROBE,
                            std::string("OUTER_EXPECT=") + (allow_outer ? "allow" : "deny"),
                            std::string("INNER_EXPECT=") + (allow_inner ? "allow" : "deny")};
      });
      IUTEST_EXPECT_EQ("success", job.result) << f.log;
      IUTEST_EXPECT_EQ(allow_outer || allow_inner,
                        f.log.find("Apple security services enabled") != std::string::npos);
    }
  }
}

IUTEST(ActionRunnerTest, AppleSecurityNodeLifecycleMatchesItsOwnActionNotItsParent) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  for (const bool allow_node : {false, true}) {
    ActionFixture f;
    f.file("outer/action.yml", R"(runs:
  using: composite
  steps:
    - uses: ./js
)");
    f.file("js/action.yml",
           "runs:\n  using: node20\n  pre: pre.js\n  main: main.js\n  post: post.js\n");
    const std::string probe = R"(const {spawnSync} = require('child_process');
for (const service of ['com.apple.SecurityServer', 'com.apple.trustd.agent']) {
  const result = spawnSync(process.env.APPLE_SECURITY_PROBE, [service, process.env.NODE_EXPECT]);
  if (result.status !== 0) throw Error('unexpected Apple security permission: ' + result.stdout + result.stderr);
}
)";
    for (const auto* phase : {"pre", "main", "post"}) {
      f.file(std::string("js/") + phase + ".js", probe +
             "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, '" + phase + "\\n');\n");
    }
    const auto job = f.run(R"(      - uses: ./outer
      - run: |
          "$APPLE_SECURITY_PROBE" com.apple.SecurityServer deny
          "$APPLE_SECURITY_PROBE" com.apple.trustd.agent deny
)", *node, {}, [allow_node](RunOptions& options) {
      options.sandbox.enabled = true;
      options.sandbox.allow_read = {CPA_APPLE_SECURITY_PROBE};
      options.sandbox.allow_apple_security_for = {allow_node ? "./js" : "./outer"};
      options.pass_env = {std::string("APPLE_SECURITY_PROBE=") + CPA_APPLE_SECURITY_PROBE,
                          std::string("NODE_EXPECT=") + (allow_node ? "allow" : "deny")};
    });
    IUTEST_EXPECT_EQ("success", job.result) << f.log;
    IUTEST_EXPECT_EQ("pre\nmain\npost\n", job.summary);
  }
}

IUTEST(ActionRunnerTest, AppleSecurityRemoteSelectorsMatchExactPathAndRef) {
  const std::string sha(40, 'c');
  const std::string exact = "example/secure/nested@" + sha;
  for (const auto& selector : {exact, "example/secure@" + sha,
                               "example/secure/nested@" + std::string(40, 'd')}) {
    ActionFixture f;
    const auto cached = f.dir / ("cache/actions/example/secure/" + sha);
    util::write_file(cached / ".cpp-actions-complete", "");
    util::write_file(cached / "nested/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        "$APPLE_SECURITY_PROBE" com.apple.SecurityServer "$EXPECTED"
        "$APPLE_SECURITY_PROBE" com.apple.trustd.agent "$EXPECTED"
)");
    const auto job = f.run("      - uses: " + exact + "\n", {}, {},
                           [&](RunOptions& options) {
      options.sandbox.enabled = true;
      options.sandbox.allow_read = {CPA_APPLE_SECURITY_PROBE};
      options.sandbox.allow_apple_security_for = {selector};
      options.pass_env = {std::string("APPLE_SECURITY_PROBE=") + CPA_APPLE_SECURITY_PROBE,
                          std::string("EXPECTED=") + (selector == exact ? "allow" : "deny")};
    });
    IUTEST_EXPECT_EQ("success", job.result) << f.log;
  }
}

IUTEST(ActionRunnerTest, WildcardRemoteActionPermissionsAreIndependentAndScoped) {
  const std::string sha(40, 'c');
  const auto user_dirs = sandbox::darwin_user_dirs();
  IUTEST_ASSERT_FALSE(user_dirs.empty());
  const auto xcode_path = util::real_path(user_dirs.front()) / ("swbuild.tmp.cpa-remote-" + util::random_uuid());
  for (int permission = 0; permission < 6; ++permission) {
    ActionFixture f;
    cpa_test::UnixSocketListener docker(f.dir / "docker.sock");
    const auto cached = f.dir / ("cache/actions/example/secure/" + sha);
    util::write_file(cached / ".cpp-actions-complete", "");
    util::write_file(cached / "action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        "$SOCKET_PROBE" "connect-$DOCKER_EXPECT" "$DOCKER_SOCKET"
        "$SOCKET_PROBE" "$MSBUILD_EXPECT" ""
        "$SOCKET_PROBE" "unity-$UNITY_EXPECT" ""
        "$SOCKET_PROBE" "file-$XCODE_EXPECT" "$XCODE_PATH"
        "$APPLE_PROBE" com.apple.SecurityServer "$APPLE_EXPECT"
    - uses: ./inner
    - shell: bash
      run: |
        "$SOCKET_PROBE" "connect-$DOCKER_EXPECT" "$DOCKER_SOCKET"
        "$SOCKET_PROBE" "$MSBUILD_EXPECT" ""
        "$SOCKET_PROBE" "unity-$UNITY_EXPECT" ""
        "$SOCKET_PROBE" "file-$XCODE_EXPECT" "$XCODE_PATH"
        "$APPLE_PROBE" com.apple.SecurityServer "$APPLE_EXPECT"
)");
    f.file("inner/action.yml", R"(runs:
  using: composite
  steps:
    - shell: bash
      run: |
        "$SOCKET_PROBE" connect-deny "$DOCKER_SOCKET"
        "$SOCKET_PROBE" deny ""
        "$SOCKET_PROBE" unity-deny ""
        "$SOCKET_PROBE" file-deny "$XCODE_PATH"
        "$APPLE_PROBE" com.apple.SecurityServer deny
)");
    const auto job = f.run(R"(      - run: |
          "$SOCKET_PROBE" connect-deny "$DOCKER_SOCKET"
          "$SOCKET_PROBE" deny ""
          "$SOCKET_PROBE" unity-deny ""
          "$SOCKET_PROBE" file-deny "$XCODE_PATH"
          "$APPLE_PROBE" com.apple.SecurityServer deny
      - uses: example/secure@)" + sha + R"(
      - run: |
          "$SOCKET_PROBE" connect-deny "$DOCKER_SOCKET"
          "$SOCKET_PROBE" deny ""
          "$SOCKET_PROBE" unity-deny ""
          "$SOCKET_PROBE" file-deny "$XCODE_PATH"
          "$APPLE_PROBE" com.apple.SecurityServer deny
)", {}, {}, [&](RunOptions& options) {
      options.sandbox.enabled = true;
      options.sandbox.docker_socket = docker.path();
      options.sandbox.allow_read = {CPA_MSBUILD_SOCKET_PROBE, CPA_APPLE_SECURITY_PROBE};
      options.pass_env = {
          std::string("SOCKET_PROBE=") + CPA_MSBUILD_SOCKET_PROBE,
          std::string("APPLE_PROBE=") + CPA_APPLE_SECURITY_PROBE,
          "DOCKER_SOCKET=" + docker.path().string(),
          "XCODE_PATH=" + xcode_path.string(),
          std::string("DOCKER_EXPECT=") + (permission == 1 ? "allow" : "deny"),
          std::string("APPLE_EXPECT=") + (permission == 2 ? "allow" : "deny"),
          std::string("MSBUILD_EXPECT=") + (permission == 3 ? "allow" : "deny"),
          std::string("UNITY_EXPECT=") + (permission == 4 ? "allow" : "deny"),
          std::string("XCODE_EXPECT=") + (permission == 5 ? "allow" : "deny")};
      switch (permission) {
        case 0:
          options.sandbox.allow_docker_for = {"example/secure/subpath@*"};
          options.sandbox.allow_apple_security_for = {"Example/secure@*"};
          options.sandbox.allow_msbuild_for = {"example/secure@d*"};
          options.sandbox.allow_unity_for = {"other/secure@*"};
          options.sandbox.allow_xcode_for = {"example/secure/subpath@*"};
          break;
        case 1: options.sandbox.allow_docker_for = {"example/secure@c*"}; break;
        case 2: options.sandbox.allow_apple_security_for = {"example/secure@*"}; break;
        case 3: options.sandbox.allow_msbuild_for = {"example/secure@c*"}; break;
        case 4: options.sandbox.allow_unity_for = {"example/secure@*"}; break;
        case 5: options.sandbox.allow_xcode_for = {"example/secure@c*"}; break;
      }
    });
    IUTEST_EXPECT_EQ("success", job.result) << permission << "\n" << f.log;
    IUTEST_EXPECT_EQ(permission == 1, f.log.find("Docker daemon access enabled") != std::string::npos) << f.log;
    IUTEST_EXPECT_EQ(permission == 2, f.log.find("Apple security services enabled") != std::string::npos) << f.log;
    IUTEST_EXPECT_EQ(permission == 3, f.log.find("MSBuild worker IPC enabled") != std::string::npos) << f.log;
    IUTEST_EXPECT_EQ(permission == 4, f.log.find("Unity compatibility enabled") != std::string::npos) << f.log;
    IUTEST_EXPECT_EQ(permission == 5, f.log.find("Xcode temporary-file access enabled") != std::string::npos) << f.log;
  }
}

IUTEST(ActionRunnerTest, WildcardRemoteNodePermissionCoversPreMainAndPost) {
  const auto node = host_node();
  if (!node) IUTEST_SKIP() << "No host Node.js runtime";
  ActionFixture f;
  const std::string sha(40, 'd');
  const auto cached = f.dir / ("cache/actions/example/node/" + sha);
  util::write_file(cached / ".cpp-actions-complete", "");
  util::write_file(cached / "action.yml", "runs:\n  using: node24\n  pre: pre.js\n  main: main.js\n  post: post.js\n");
  for (const auto* phase : {"pre", "main", "post"}) {
    util::write_file(cached / (std::string(phase) + ".js"),
                     "require('child_process').execFileSync(process.env.APPLE_PROBE, "
                     "['com.apple.SecurityServer', 'allow']);\n"
                     "require('fs').appendFileSync(process.env.GITHUB_STEP_SUMMARY, '" + std::string(phase) + "\\n');\n");
  }
  const auto job = f.run("      - uses: example/node@" + sha + R"(
      - run: '"$APPLE_PROBE" com.apple.SecurityServer deny'
)", *node, {}, [&](RunOptions& options) {
    options.sandbox.enabled = true;
    options.sandbox.allow_read = {CPA_APPLE_SECURITY_PROBE};
    options.sandbox.allow_apple_security_for = {"example/node@*"};
    options.pass_env = {std::string("APPLE_PROBE=") + CPA_APPLE_SECURITY_PROBE};
  });
  IUTEST_EXPECT_EQ("success", job.result) << f.log;
  IUTEST_EXPECT_EQ("pre\nmain\npost\n", job.summary) << f.log;
}
#endif
