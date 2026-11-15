#include "iutest.hpp"

#include "workflow/parser.hpp"
#include "workflow/scopes.hpp"

using namespace cpa::workflow;

namespace {

Workflow parse(const char* text) { return parse_workflow(cpa::yaml::load(text, "wf.yml"), "wf.yml"); }

// Returns the error messages (empty when the workflow is valid).
std::vector<std::string> errors_of(const char* text) {
  try {
    parse(text);
  } catch (const WorkflowError& e) {
    std::vector<std::string> out;
    for (const auto& d : e.diagnostics()) out.push_back(d.message);
    return out;
  }
  return {};
}

bool has_error(const std::vector<std::string>& errors, std::string_view needle) {
  for (const auto& e : errors) {
    if (e.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

IUTEST(ParserTest, Basic) {
  const auto wf = parse(R"(
name: CI
on:
  push:
    branches: [main]
  workflow_dispatch:
    inputs:
      level:
        type: choice
        options: [info, debug]
        default: info
env:
  A: 1
jobs:
  build:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      - run: echo hi
      - id: second
        run: |
          echo a
          echo b
      - run: echo c
  test:
    needs: build
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [macos-14, macos-15]
    steps:
      - run: echo ${{ matrix.os }}
)");
  IUTEST_EXPECT_EQ("CI", wf.display_name());
  IUTEST_ASSERT_EQ(2u, wf.triggers.size());
  IUTEST_EXPECT_EQ("push", wf.triggers[0].event);
  IUTEST_ASSERT_EQ(1u, wf.dispatch_inputs.size());
  IUTEST_EXPECT_EQ("choice", wf.dispatch_inputs[0].type);
  IUTEST_ASSERT_EQ(2u, wf.jobs.size());
  const auto& build = wf.jobs[0];
  IUTEST_ASSERT_EQ(4u, build.steps.size());
  IUTEST_EXPECT_EQ("__actions_checkout", build.steps[0].id);
  IUTEST_EXPECT_EQ("__run", build.steps[1].id);
  IUTEST_EXPECT_EQ("second", build.steps[2].id);
  IUTEST_EXPECT_EQ("__run_2", build.steps[3].id);
  IUTEST_EXPECT_EQ("Run actions/checkout@v4", build.steps[0].default_display_name());
  IUTEST_EXPECT_EQ("Run echo a", build.steps[2].default_display_name());
  IUTEST_ASSERT_NOTNULL(wf.find_job("test"));
  IUTEST_ASSERT_EQ(1u, wf.find_job("test")->needs.size());
  IUTEST_EXPECT_EQ("build", wf.find_job("test")->needs[0]);
}

IUTEST(ParserTest, OnForms) {
  IUTEST_EXPECT_EQ(1u, parse("on: push\njobs: {a: {runs-on: x, steps: [{run: a}]}}").triggers.size());
  IUTEST_EXPECT_EQ(2u, parse("on: [push, pull_request]\njobs: {a: {runs-on: x, steps: [{run: a}]}}").triggers.size());
  IUTEST_EXPECT_TRUE(has_error(errors_of("on: pushh\njobs: {a: {runs-on: x, steps: [{run: a}]}}"),
                               "Unexpected value 'pushh'"));
  IUTEST_EXPECT_TRUE(has_error(errors_of("jobs: {a: {runs-on: x, steps: [{run: a}]}}"),
                               "Required property is missing: on"));
  IUTEST_EXPECT_TRUE(has_error(
      errors_of("on: {push: {branches: [a], branches-ignore: [b]}}\njobs: {a: {runs-on: x, steps: [{run: a}]}}"),
      "cannot be used together"));
  IUTEST_EXPECT_TRUE(has_error(errors_of("on: {schedule: [{cronn: x}]}\njobs: {a: {runs-on: x, steps: [{run: a}]}}"),
                               "Required property is missing: cron"));
}

IUTEST(ParserTest, StructuralErrors) {
  const auto errors = errors_of(R"(
on: push
foo: bar
jobs:
  1bad:
    runs-on: x
    steps: [{run: a}]
  nosteps:
    runs-on: x
  noruns:
    steps: [{run: a}]
  both:
    runs-on: x
    steps:
      - run: a
        uses: actions/checkout@v4
      - name: nothing
      - run: a
        with: {x: 1}
      - uses: bad-format
      - uses: actions/checkout@v4
        shell: bash
  dup:
    runs-on: x
    steps:
      - {id: s, run: a}
      - {id: s, run: b}
)");
  IUTEST_EXPECT_TRUE(has_error(errors, "Unexpected value 'foo'"));
  IUTEST_EXPECT_TRUE(has_error(errors, "The identifier '1bad' is invalid"));
  IUTEST_EXPECT_TRUE(has_error(errors, "Required property is missing: steps"));
  IUTEST_EXPECT_TRUE(has_error(errors, "Required property is missing: runs-on"));
  IUTEST_EXPECT_TRUE(has_error(errors, "cannot have both"));
  IUTEST_EXPECT_TRUE(has_error(errors, "Every step must define"));
  IUTEST_EXPECT_TRUE(has_error(errors, "Unexpected value 'with'"));
  IUTEST_EXPECT_TRUE(has_error(errors, "Expected format {org}/{repo}[/path]@ref. Actual 'bad-format'"));
  IUTEST_EXPECT_TRUE(has_error(errors, "Unexpected value 'shell'"));
  IUTEST_EXPECT_TRUE(has_error(errors, "may not be used more than once"));
}

IUTEST(ParserTest, Needs) {
  IUTEST_EXPECT_TRUE(has_error(errors_of(R"(
on: push
jobs:
  a: {runs-on: x, needs: missing, steps: [{run: a}]}
)"),
                               "Job 'a' depends on unknown job 'missing'."));
  IUTEST_EXPECT_TRUE(has_error(errors_of(R"(
on: push
jobs:
  a: {runs-on: x, needs: c, steps: [{run: a}]}
  b: {runs-on: x, needs: a, steps: [{run: a}]}
  c: {runs-on: x, needs: [b], steps: [{run: a}]}
)"),
                               "creates a cycle"));
}

IUTEST(ParserTest, ExpressionContexts) {
  const auto errors = errors_of(R"(
on: push
run-name: ${{ env.X }}
env:
  A: ${{ matrix.x }}
jobs:
  a:
    if: ${{ steps.x.outputs.y }}
    runs-on: ${{ secrets.RUNNER }}
    steps:
      - if: secrets.X
        run: echo ${{ secrets.OK }} ${{ hashFiles('**') }}
      - run: echo ${{ github.event_name
      - uses: ${{ matrix.action }}
)");
  IUTEST_EXPECT_TRUE(has_error(errors, "Unrecognized named-value: 'env'"));
  IUTEST_EXPECT_TRUE(has_error(errors, "Unrecognized named-value: 'matrix'"));
  IUTEST_EXPECT_TRUE(has_error(errors, "Unrecognized named-value: 'steps'"));
  IUTEST_EXPECT_TRUE(has_error(errors, "Unrecognized named-value: 'secrets'"));
  IUTEST_EXPECT_TRUE(has_error(errors, "An expression was not closed"));
  IUTEST_EXPECT_TRUE(has_error(errors, "A template expression is not allowed in 'uses'"));
  // secrets and hashFiles are fine in `run`.
  IUTEST_EXPECT_EQ(7u, errors.size());
}

IUTEST(ParserTest, Diagnostics) {
  try {
    parse("on: push\njobs:\n  a:\n    runs-on: x\n    steps:\n      - run: ${{ foo }}\n");
    IUTEST_FAIL() << "expected an error";
  } catch (const WorkflowError& e) {
    IUTEST_ASSERT_EQ(1u, e.diagnostics().size());
    IUTEST_EXPECT_EQ(6, e.diagnostics()[0].mark.line);
    IUTEST_EXPECT_EQ(14, e.diagnostics()[0].mark.column);
    IUTEST_EXPECT_STRIN("wf.yml:6:14: error: Unrecognized named-value: 'foo'", e.diagnostics()[0].to_string());
  }
}

IUTEST(ParserTest, Conditions) {
  const auto scope = scope_for(Site::StepIf);
  IUTEST_EXPECT_EQ("success()", condition_expression(nullptr, scope));
  const auto doc = cpa::yaml::load(R"(
a: github.ref == 'x'
b: ${{ always() }}
c: ${{ a }} && ${{ b }}
d: true
e: ''
f: failure() || cancelled()
)",
                                   "c.yml");
  const auto& r = *doc->root;
  IUTEST_EXPECT_EQ("success() && (github.ref == 'x')", condition_expression(r.get("a"), scope));
  IUTEST_EXPECT_EQ("always()", condition_expression(r.get("b"), scope));
  IUTEST_EXPECT_THROW(condition_expression(r.get("c"), scope), cpa::expr::ParseError);  // `a` is unknown
  IUTEST_EXPECT_EQ("success() && (true)", condition_expression(r.get("d"), scope));
  IUTEST_EXPECT_EQ("success()", condition_expression(r.get("e"), scope));
  IUTEST_EXPECT_EQ("failure() || cancelled()", condition_expression(r.get("f"), scope));
}

IUTEST(ParserTest, ReusableWorkflowJob) {
  const auto wf = parse(R"(
on: push
jobs:
  call:
    uses: fixture-owner/repo/.github/workflows/x.yml@main
    with:
      a: 1
)");
  IUTEST_EXPECT_EQ("fixture-owner/repo/.github/workflows/x.yml@main", wf.jobs[0].uses);
}

IUTEST(ParserTest, ReusableWorkflowReferences) {
  for (const auto* uses : {"./.github/workflows/build.yml", "./.github/workflows/build.yaml",
                           "fixture-owner/repo/.github/workflows/build.yaml@feature/build",
                           "fixture-owner/repo/.github/workflows/build.yml@0123456789"}) {
    const auto text = std::string("on: push\njobs:\n  call:\n    uses: ") + uses + "\n";
    IUTEST_EXPECT_TRUE(errors_of(text.c_str()).empty()) << uses;
  }
  for (const auto* uses : {"''", "null", "true", "[]", "{}", "actions/checkout@v4",
                           "./other/build.yml", "./.github/workflows/nested/build.yml",
                           "./.github/workflows/../build.yml", "./.github/workflows/build.yml@main",
                           "fixture-owner/repo/.github/workflows/build.yml",
                           "fixture-owner/repo/.github/workflows/build.yml@",
                           "fixture-owner/repo/.github/workflows/nested/build.yml@main",
                           "../repo/.github/workflows/build.yml@main",
                           "fixture-owner/repo/.github/workflows/build.txt@main",
                           "${{ inputs.workflow }}", "./.github/workflows/${{ inputs.file }}.yml"}) {
    const auto text = std::string("on: push\njobs:\n  call:\n    uses: ") + uses + "\n";
    IUTEST_EXPECT_FALSE(errors_of(text.c_str()).empty()) << uses;
  }
}

IUTEST(ParserTest, ReusableWorkflowArguments) {
  IUTEST_EXPECT_TRUE(errors_of(R"(
on: push
jobs:
  call:
    uses: ./.github/workflows/build.yml
    with:
      text: hello
      boolean: true
      number: 42
      expression: ${{ false }}
      numeric-expression: ${{ fromJSON('42') }}
      null-literal: null
      null-blank:
      null-tilde: ~
      null-expression: ${{ null }}
      contexts: ${{ github.ref }}-${{ needs.build.outputs.x }}-${{ strategy.job-index }}-${{ matrix.os }}-${{ inputs.x }}-${{ vars.X }}
    secrets:
      token: ${{ secrets.TOKEN }}
      literal: text
      expression: ${{ 'secret' }}
      contexts: ${{ github.ref }}-${{ needs.build.outputs.x }}-${{ strategy.job-index }}-${{ matrix.os }}-${{ inputs.x }}-${{ vars.X }}
)").empty());
  IUTEST_EXPECT_TRUE(errors_of(R"(
on: push
jobs:
  call:
    uses: ./.github/workflows/build.yml
    secrets: inherit
)").empty());
  for (const auto* value : {"[]", "null", "hello", "${{ inputs.values }}", "{a: []}", "{a: {x: y}}",
                            "{a: '${{ fromJSON(\"[]\") }}'}",
                            "{a: '${{ github }}'}", "{a: '${{ secrets.TOKEN }}'}", "{a: '${{ env.X }}'}",
                            "{a: '${{ steps.x.outputs.y }}'}", "{'${{ inputs.key }}': text}"}) {
    const auto text = std::string("on: push\njobs:\n  call:\n    uses: ./.github/workflows/build.yml\n    with: ") +
                      value + "\n";
    IUTEST_EXPECT_FALSE(errors_of(text.c_str()).empty()) << value;
  }
}

IUTEST(ParserTest, ReusableWorkflowSecretValidation) {
  for (const auto* value : {"[]", "null", "other", "${{ secrets }}", "{token: true}", "{token: 1}",
                            "{token: null}", "{token: []}", "{token: {x: y}}",
                            "{token: '${{ secrets }}'}", "{token: '${{ true }}'}",
                            "{token: '${{ fromJSON(\"[]\") }}'}",
                            "{token: '${{ env.X }}'}", "{token: '${{ steps.x.outputs.y }}'}",
                            "{token: '${{ runner.os }}'}", "{token: '${{ hashFiles(\"x\") }}'}",
                            "{'${{ inputs.key }}': text}", "{token: '${{ secrets.TOKEN'}"}) {
    const auto text = std::string("on: push\njobs:\n  call:\n    uses: ./.github/workflows/build.yml\n    secrets: ") +
                      value + "\n";
    IUTEST_EXPECT_FALSE(errors_of(text.c_str()).empty()) << value;
  }
}

IUTEST(ParserTest, WorkflowCallDeclarations) {
  const auto wf = parse(R"(
on:
  workflow_call:
    inputs:
      text: {type: string, default: hello}
      flag: {type: boolean, default: false}
      count: {type: number, default: 2.5}
      expression: {type: boolean, default: '${{ true }}'}
      optional: {type: string}
      null-literal: {type: string, default: null}
      null-blank: {type: string, default:}
      null-expression: {type: string, default: '${{ null }}'}
    outputs:
      result: {description: Result, value: '${{ jobs.build.outputs.result }}'}
    secrets:
      token: {required: true}
      optional:
jobs:
  build: {runs-on: x, steps: [{run: a}]}
)");
  IUTEST_EXPECT_EQ(8u, wf.call_inputs.size());
  IUTEST_EXPECT_EQ(1u, wf.call_outputs.size());
  IUTEST_EXPECT_EQ(2u, wf.call_secrets.size());
  for (const auto* definition : {"{type: string, default: true}", "{type: string, default: 1}",
                                 "{type: boolean, default: 'false'}", "{type: number, default: '2'}",
                                 "{type: boolean, default: null}", "{type: number, default: null}",
                                 "{type: boolean, default: '${{ null }}'}",
                                 "{type: number, default: '${{ null }}'}", "{type: string, default: []}",
                                 "{type: string, default: {x: y}}",
                                 "{type: boolean, default: '${{ 1 }}'}",
                                 "{type: number, default: '${{ fromJSON(\"[]\") }}'}",
                                 "{type: choice}", "{required: true}", "{type: string, required: yes}"}) {
    const auto text = std::string("on:\n  workflow_call:\n    inputs:\n      x: ") + definition +
                      "\njobs:\n  build: {runs-on: x, steps: [{run: a}]}\n";
    IUTEST_EXPECT_FALSE(errors_of(text.c_str()).empty()) << definition;
  }
  for (const auto* config : {"inputs: []", "inputs: {x: true}", "outputs: []",
                             "outputs: {x: {description: missing-value}}", "secrets: []",
                             "secrets: {x: true}", "secrets: {x: {required: 'true'}}"}) {
    const auto text = std::string("on:\n  workflow_call:\n    ") + config +
                      "\njobs:\n  build: {runs-on: x, steps: [{run: a}]}\n";
    IUTEST_EXPECT_FALSE(errors_of(text.c_str()).empty()) << config;
  }
}

IUTEST(ParserTest, EnvironmentMappingRequiresName) {
  IUTEST_EXPECT_TRUE(has_error(errors_of(R"(
on: push
jobs:
  build:
    runs-on: x
    environment: {url: 'https://example.com'}
    steps: [{run: a}]
)"), "Required property is missing: name"));
  IUTEST_EXPECT_TRUE(errors_of(R"(
on: push
jobs:
  build:
    runs-on: x
    env: {TOKEN: '${{ secrets.TOKEN }}'}
    environment: {name: '${{ matrix.environment }}', url: '${{ steps.deploy.outputs.url }}'}
    steps: [{id: deploy, run: a}]
)").empty());
}
