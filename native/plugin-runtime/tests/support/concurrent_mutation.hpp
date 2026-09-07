#pragma once

#include <atomic>
#include <cstddef>
#include <thread>

namespace omarchy::plugin_runtime::test_support {

// Finish a known number of mutations before probing; keep mutating during each
// probe. jthread joins before captured state dies, including when a probe throws.
template <typename Mutation, typename Probe>
bool observe_concurrent_mutation(Mutation mutate, Probe probe,
                                 std::size_t warmup, std::size_t attempts) {
  std::atomic<std::size_t> mutations{0};
  std::jthread mutator([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      mutate();
      mutations.fetch_add(1, std::memory_order_release);
    }
  });
  while (mutations.load(std::memory_order_acquire) < warmup) {
  }
  for (std::size_t attempt = 0; attempt < attempts; ++attempt)
    if (probe()) return true;
  return false;
}

} // namespace omarchy::plugin_runtime::test_support
