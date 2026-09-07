#include "../../tests/support/gesture_clock.hpp"
#include "../../tests/support/test_assert.hpp"

#include "gesture_eligibility.hpp"

#include <atomic>
#include <barrier>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace runtime = omarchy::plugin_runtime::runtime;
namespace definitions = omarchy::plugins::definitions;
namespace permissions = omarchy::plugins::permissions;

namespace {
using Clock = omarchy::plugin_runtime::test_support::GestureClock<100>;

using omarchy::plugin_runtime::test_support::require;

permissions::Digest digest(char value) {
  return permissions::Digest(std::string(64, value));
}
} // namespace

int main() {
  auto clock = std::make_shared<Clock>();
  runtime::GestureEligibilityLatch latch(clock);
  const permissions::ActivationBinding binding{
      .plugin = permissions::PluginId("fixture.gesture"),
      .revision = digest('1'),
      .policy_fingerprint = digest('2'),
      .generation = 7};
  const definitions::DynamicInvocation::GestureClaim claim{
      .surface_id = 3, .surface_generation = 7, .input_sequence = 11};
  OMARCHY_CHECK(latch.arm(binding, claim));
  auto other_plugin = binding;
  other_plugin.plugin = permissions::PluginId("fixture.other");
  OMARCHY_CHECK(!latch.consume(other_plugin, claim));
  OMARCHY_CHECK(!latch.consume(binding, claim));
  auto stale_sequence = claim;
  --stale_sequence.input_sequence;
  OMARCHY_CHECK(latch.arm(binding, claim) && !latch.arm(binding, stale_sequence));
  latch.clear();
  OMARCHY_CHECK(latch.arm(binding, claim));
  auto wrong = claim;
  ++wrong.surface_id;
  OMARCHY_CHECK(!latch.consume(binding, wrong));
  OMARCHY_CHECK(!latch.consume(binding, claim));
  OMARCHY_CHECK(latch.arm(binding, claim));
  OMARCHY_CHECK(latch.consume(binding, claim).has_value());
  OMARCHY_CHECK(!latch.consume(binding, claim));
  OMARCHY_CHECK(latch.arm(binding, claim));
  clock->now += 5'000'000'000ULL;
  OMARCHY_CHECK(!latch.consume(binding, claim));
  auto stale_generation = claim;
  --stale_generation.surface_generation;
  OMARCHY_CHECK(!latch.arm(binding, stale_generation));

  auto other_surface = claim;
  ++other_surface.surface_id;
  other_surface.input_sequence = 12;
  OMARCHY_CHECK(latch.arm(binding, other_surface));
  latch.clear_surface(binding, claim.surface_id, claim.surface_generation);
  OMARCHY_CHECK(latch.consume(binding, other_surface).has_value());

  auto other_binding = binding;
  other_binding.revision = digest('3');
  other_surface.input_sequence = 13;
  OMARCHY_CHECK(latch.arm(binding, other_surface));
  latch.clear_surface(other_binding, other_surface.surface_id,
                      other_surface.surface_generation);
  OMARCHY_CHECK(latch.consume(binding, other_surface).has_value());

  other_surface.input_sequence = 14;
  OMARCHY_CHECK(latch.arm(binding, other_surface));
  latch.clear_surface(binding, other_surface.surface_id,
                      other_surface.surface_generation);
  OMARCHY_CHECK(!latch.consume(binding, other_surface));

  other_surface.input_sequence = 15;
  OMARCHY_CHECK(latch.arm(binding, other_surface));
  latch.clear();
  OMARCHY_CHECK(!latch.consume(binding, other_surface));

  constexpr int kRaceIterations = 500;
  definitions::DynamicInvocation::GestureClaim race_claim = claim;
  const auto race_consumers = [&](std::uint64_t first_sequence, auto prepare,
                                  auto mutate) {
    std::barrier phase(4);
    std::atomic<int> successes = 0;
    const auto consume = [&] {
      for (int iteration = 0; iteration < kRaceIterations; ++iteration) {
        phase.arrive_and_wait();
        if (latch.consume(binding, race_claim))
          successes.fetch_add(1, std::memory_order_relaxed);
        phase.arrive_and_wait();
      }
    };
    std::thread first_consumer(consume);
    std::thread second_consumer(consume);
    std::thread mutator([&] {
      for (int iteration = 0; iteration < kRaceIterations; ++iteration) {
        phase.arrive_and_wait();
        mutate();
        phase.arrive_and_wait();
      }
    });
    for (int iteration = 0; iteration < kRaceIterations; ++iteration) {
      successes.store(0, std::memory_order_relaxed);
      race_claim.input_sequence = first_sequence + static_cast<std::uint64_t>(iteration);
      prepare();
      phase.arrive_and_wait();
      phase.arrive_and_wait();
      OMARCHY_CHECK(successes.load(std::memory_order_relaxed) <= 1);
    }
    mutator.join();
    first_consumer.join();
    second_consumer.join();
  };
  race_consumers(100, [&] { latch.clear(); }, [&] {
    static_cast<void>(latch.arm(binding, race_claim));
  });
  race_consumers(1'000, [&] {
    OMARCHY_CHECK(latch.arm(binding, race_claim));
  }, [&] { latch.clear(); });
}
