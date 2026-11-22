#include "iutest.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include "util/stoppable_thread.hpp"

using cpa::util::StoppableThread;

namespace {

static_assert(!std::is_copy_constructible_v<StoppableThread>);
static_assert(!std::is_copy_assignable_v<StoppableThread>);
static_assert(!std::is_move_constructible_v<StoppableThread>);
static_assert(!std::is_move_assignable_v<StoppableThread>);

struct StopProbe {
  std::promise<void> started;
  std::future<void> started_future = started.get_future();
  std::atomic<bool> stopped{false};

  void operator()(const std::atomic<bool>& stop) {
    started.set_value();
    while (!stop.load(std::memory_order_relaxed)) std::this_thread::yield();
    stopped.store(true, std::memory_order_relaxed);
  }

  bool wait_until_started() {
    return started_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  }
};

}  // namespace

IUTEST(StoppableThreadTest, DestructionRequestsStopAndJoins) {
  StopProbe probe;
  {
    StoppableThread thread(std::ref(probe));
    IUTEST_ASSERT_TRUE(probe.wait_until_started());
  }
  IUTEST_EXPECT_TRUE(probe.stopped.load(std::memory_order_relaxed));
}

IUTEST(StoppableThreadTest, ExplicitStopAndJoinAreSafeBeforeDestruction) {
  StopProbe probe;
  {
    StoppableThread thread(std::ref(probe));
    IUTEST_ASSERT_TRUE(probe.wait_until_started());
    thread.request_stop();
    thread.request_stop();
    thread.join();
    thread.join();
    IUTEST_EXPECT_TRUE(probe.stopped.load(std::memory_order_relaxed));
  }
}

IUTEST(StoppableThreadTest, DestructionDoesNotRequireTheWorkerToHaveStarted) {
  std::atomic<int> completed{0};
  for (int i = 0; i < 10; ++i) {
    StoppableThread thread([&](const std::atomic<bool>& stop) {
      while (!stop.load(std::memory_order_relaxed)) std::this_thread::yield();
      completed.fetch_add(1, std::memory_order_relaxed);
    });
  }
  IUTEST_EXPECT_EQ(10, completed.load(std::memory_order_relaxed));
}

IUTEST(StoppableThreadTest, ExceptionUnwindingStopsAndJoins) {
  StopProbe probe;
  bool started = false;
  const auto run = [&] {
    StoppableThread thread(std::ref(probe));
    started = probe.wait_until_started();
    throw std::runtime_error("test exception");
  };
  IUTEST_EXPECT_THROW(run(), std::runtime_error);
  IUTEST_EXPECT_TRUE(started);
  IUTEST_EXPECT_TRUE(probe.stopped.load(std::memory_order_relaxed));
}
