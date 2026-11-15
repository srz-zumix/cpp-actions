#include "iutest.hpp"

#include <chrono>

#include "util/process.hpp"

using namespace cpa::util;

namespace {

ProcessSpec shell(const std::string& script) {
  ProcessSpec spec;
  spec.argv = {"/bin/sh", "-c", script};
  spec.env = {{"PATH", "/usr/bin:/bin"}, {"FOO", "bar"}};
  return spec;
}

}  // namespace

IUTEST(ProcessTest, CapturesLinesAndExitCode) {
  auto spec = shell("echo out1; echo err1 >&2; printf 'partial'; exit 3");
  std::vector<std::string> out, err;
  spec.on_line = [&](std::string_view line, bool is_err) { (is_err ? err : out).emplace_back(line); };
  const auto r = run_process(spec);
  IUTEST_EXPECT_EQ(3, r.exit_code);
  IUTEST_ASSERT_EQ(2u, out.size());
  IUTEST_EXPECT_EQ("out1", out[0]);
  IUTEST_EXPECT_EQ("partial", out[1]);
  IUTEST_ASSERT_EQ(1u, err.size());
  IUTEST_EXPECT_EQ("err1", err[0]);
}

IUTEST(ProcessTest, EnvironmentAndCwd) {
  auto spec = shell("echo $FOO; pwd");
  spec.cwd = "/";
  std::vector<std::string> out;
  spec.on_line = [&](std::string_view line, bool) { out.emplace_back(line); };
  const auto r = run_process(spec);
  IUTEST_EXPECT_EQ(0, r.exit_code);
  IUTEST_ASSERT_EQ(2u, out.size());
  IUTEST_EXPECT_EQ("bar", out[0]);
  IUTEST_EXPECT_EQ("/", out[1]);
}

IUTEST(ProcessTest, Timeout) {
  auto spec = shell("sleep 30");
  spec.timeout = std::chrono::milliseconds(200);
  const auto start = std::chrono::steady_clock::now();
  const auto r = run_process(spec);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  IUTEST_EXPECT_TRUE(r.timed_out);
  IUTEST_EXPECT_NE(0, r.exit_code);
  IUTEST_EXPECT_LT(elapsed, std::chrono::seconds(5));
}

IUTEST(ProcessTest, HardTimeoutSkipsSignalGracePeriod) {
  auto spec = shell("trap '' INT TERM; echo ready; while :; do sleep 1; done");
  spec.timeout = std::chrono::milliseconds(200);
  spec.hard_timeout = true;
  const auto start = std::chrono::steady_clock::now();
  const auto r = run_process(spec);
  IUTEST_EXPECT_TRUE(r.timed_out);
  IUTEST_EXPECT_FALSE(r.cancelled);
  IUTEST_EXPECT_NE(0, r.exit_code);
  IUTEST_EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
}

IUTEST(ProcessTest, ForceCancellationSkipsSignalGracePeriod) {
  auto spec = shell("trap '' INT TERM; echo ready; while :; do sleep 1; done");
  std::atomic<bool> force{false};
  spec.force_cancel = &force;
  spec.timeout = std::chrono::seconds(5);
  spec.on_line = [&](std::string_view line, bool) {
    if (line == "ready") force = true;
  };
  const auto start = std::chrono::steady_clock::now();
  const auto r = run_process(spec);
  IUTEST_EXPECT_TRUE(r.cancelled);
  IUTEST_EXPECT_FALSE(r.timed_out);
  IUTEST_EXPECT_NE(0, r.exit_code);
  IUTEST_EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
}

IUTEST(ProcessTest, BackgroundChildDoesNotBlock) {
  auto spec = shell("sleep 5 & echo done");
  const auto start = std::chrono::steady_clock::now();
  std::vector<std::string> out;
  pid_t pgid = 0;
  spec.on_spawn = [&](pid_t p) { pgid = p; };
  spec.on_line = [&](std::string_view line, bool) { out.emplace_back(line); };
  const auto r = run_process(spec);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  IUTEST_EXPECT_EQ(0, r.exit_code);
  IUTEST_EXPECT_LT(elapsed, std::chrono::seconds(3));
  IUTEST_ASSERT_EQ(1u, out.size());
  kill_process_group(pgid);
}

IUTEST(ProcessTest, CommandNotFound) {
  ProcessSpec spec;
  spec.argv = {"definitely-not-a-command-xyz"};
  spec.env = {{"PATH", "/usr/bin:/bin"}};
  const auto r = run_process(spec);
  IUTEST_EXPECT_TRUE(r.spawn_failed);
  IUTEST_EXPECT_EQ(127, r.exit_code);
}

IUTEST(ProcessTest, Capture) {
  const auto r = capture({"/bin/sh", "-c", "echo hi"});
  IUTEST_EXPECT_EQ(0, r.exit_code);
  IUTEST_EXPECT_EQ("hi\n", r.out);
}
