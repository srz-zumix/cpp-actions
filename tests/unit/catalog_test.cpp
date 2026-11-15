#include "iutest.hpp"

#include "engine/catalog.hpp"
#include "test_support.hpp"
#include "workflow/parser.hpp"

using namespace cpa;
using namespace cpa::engine;

namespace {

const char* const kCi = R"(name: CI
on: [push, pull_request]
jobs:
  build:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [macos-latest, macos-14]
        cfg: [debug]
    steps: [{run: make}]
  lint:
    name: Lint code
    runs-on: macos-latest
    steps: [{run: lint}]
  test:
    needs: build
    runs-on: [self-hosted, macOS]
    steps: [{run: test}]
  deploy:
    needs: [test, lint]
    if: github.ref == 'refs/heads/main'
    runs-on: macos-latest
    steps: [{run: deploy}]
)";

}  // namespace

IUTEST(CatalogTest, LoadAndSelect) {
  cpa_test::TempDir dir;
  util::write_file(dir / ".github/workflows/ci.yml", kCi);
  util::write_file(dir / ".github/workflows/bad.yaml", "on: push\njobs: {a: {steps: []}}\n");
  util::write_file(dir / ".github/workflows/notes.txt", "ignored");
  const auto files = load_workflow_files(dir.path(), {});
  IUTEST_ASSERT_EQ(2u, files.size());
  IUTEST_EXPECT_EQ(".github/workflows/bad.yaml", files[0].display);
  IUTEST_EXPECT_FALSE(files[0].valid());
  IUTEST_EXPECT_FALSE(files[0].errors.empty());
  IUTEST_EXPECT_TRUE(files[1].valid());

  IUTEST_EXPECT_TRUE(workflow_matches(files[1], "ci.yml"));
  IUTEST_EXPECT_TRUE(workflow_matches(files[1], "ci"));
  IUTEST_EXPECT_TRUE(workflow_matches(files[1], "CI"));
  IUTEST_EXPECT_TRUE(workflow_matches(files[1], ".github/workflows/ci.yml"));
  IUTEST_EXPECT_FALSE(workflow_matches(files[1], "bad"));

  workflow::EventInfo push;
  push.name = "push";
  push.ref = "refs/heads/main";
  const auto all = select_workflows(files, push, {});
  IUTEST_ASSERT_EQ(2u, all.size());
  IUTEST_EXPECT_FALSE(all[0].match.matched);
  IUTEST_EXPECT_TRUE(all[1].match.matched);
  const auto only = select_workflows(files, push, {"CI"});
  IUTEST_ASSERT_EQ(1u, only.size());
  IUTEST_EXPECT_THROW(select_workflows(files, push, {"missing"}), std::runtime_error);

  const auto validation = render_validation(files);
  IUTEST_EXPECT_NE(std::string::npos, validation.find("2 workflow file(s) checked: 1 valid, 1 invalid"));
  IUTEST_EXPECT_NE(std::string::npos, validation.find(".github/workflows/bad.yaml:"));
}

IUTEST(CatalogTest, Levels) {
  const auto wf = workflow::parse_workflow(yaml::load(kCi, "ci.yml"), "ci.yml");
  const auto levels = job_levels(wf);
  IUTEST_ASSERT_EQ(4u, levels.size());
  IUTEST_EXPECT_EQ(1, levels[0]);
  IUTEST_EXPECT_EQ(1, levels[1]);
  IUTEST_EXPECT_EQ(2, levels[2]);
  IUTEST_EXPECT_EQ(3, levels[3]);
}

IUTEST(CatalogTest, RenderGraph) {
  const auto wf = workflow::parse_workflow(yaml::load(kCi, "ci.yml"), "ci.yml");
  IUTEST_EXPECT_EQ(
      "CI (ci.yml)\n"
      "  stage 1:\n"
      "    build\n"
      "    lint (Lint code)\n"
      "  stage 2:\n"
      "    test <- build\n"
      "  stage 3:\n"
      "    deploy <- test, lint\n",
      render_graph_text(wf));
  const auto dot = render_graph_dot({&wf});
  IUTEST_EXPECT_NE(std::string::npos, dot.find("\"ci.yml/build\" -> \"ci.yml/test\";"));
  IUTEST_EXPECT_NE(std::string::npos, dot.find("\"ci.yml/lint\" -> \"ci.yml/deploy\";"));
  IUTEST_EXPECT_NE(std::string::npos, dot.find("label=\"lint (Lint code)\""));
}

IUTEST(CatalogTest, RenderList) {
  cpa_test::TempDir dir;
  util::write_file(dir / ".github/workflows/ci.yml", kCi);
  const auto text = render_list(load_workflow_files(dir.path(), {}));
  IUTEST_EXPECT_NE(std::string::npos, text.find("CI (.github/workflows/ci.yml)\n  on: push, pull_request\n"));
  IUTEST_EXPECT_NE(std::string::npos, text.find("matrix: 2 combination(s)\n        (macos-latest, debug)\n"));
  IUTEST_EXPECT_NE(std::string::npos, text.find("runs-on: self-hosted, macOS\n"));
  IUTEST_EXPECT_NE(std::string::npos, text.find("needs: test, lint\n"));
}
