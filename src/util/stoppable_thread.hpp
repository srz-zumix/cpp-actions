#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <utility>

namespace cpa::util {

// Xcode 16.4's libc++ does not expose std::jthread by default.
class StoppableThread {
 public:
  template <typename Function>
  explicit StoppableThread(Function&& function)
      : thread_(std::forward<Function>(function), std::cref(stop_requested_)) {}

  ~StoppableThread() {
    request_stop();
    join();
  }

  StoppableThread(const StoppableThread&) = delete;
  StoppableThread& operator=(const StoppableThread&) = delete;
  StoppableThread(StoppableThread&&) = delete;
  StoppableThread& operator=(StoppableThread&&) = delete;

  void request_stop() noexcept { stop_requested_.store(true, std::memory_order_relaxed); }

  void join() {
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::atomic<bool> stop_requested_{false};
  std::thread thread_;
};

}  // namespace cpa::util
