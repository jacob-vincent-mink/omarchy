#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace omarchy::plugin_runtime::test_support {

// One-shot rendezvous; callers retain ownership and joining of their threads.
class BlockingGate {
public:
  template <typename Enter> void arrive_and_wait(Enter on_enter) {
    std::unique_lock lock(mutex_);
    on_enter();
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
  }
  void arrive_and_wait() { arrive_and_wait([] {}); }

  void wait_entered() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return entered_; });
  }
  template <typename Rep, typename Period>
  bool wait_entered_for(std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return entered_; });
  }
  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false;
  bool released_ = false;
};

} // namespace omarchy::plugin_runtime::test_support
