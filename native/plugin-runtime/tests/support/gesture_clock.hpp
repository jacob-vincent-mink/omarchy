#pragma once

#include "gesture_eligibility.hpp"

#include <memory>

namespace omarchy::plugin_runtime::test_support {

template <std::uint64_t Initial = 100>
struct GestureClock final : runtime::GestureEligibilityClock {
  std::uint64_t now = Initial;
  std::uint64_t now_nanoseconds() const override { return now; }
};

template <std::uint64_t Initial>
auto make_gesture_latch() {
  return std::make_shared<runtime::GestureEligibilityLatch>(
      std::make_shared<GestureClock<Initial>>());
}

} // namespace omarchy::plugin_runtime::test_support
