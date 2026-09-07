#include "product_session_state.hpp"
#include "../../tests/support/child_process.hpp"

#include <array>
#include <cstdlib>

using omarchy::plugin_runtime::channel::ProductSessionState;
using namespace omarchy::plugin_runtime::test_support;
namespace permissions = omarchy::plugins::permissions;
using State = ProductSessionState;
using Phase = State::Phase;
using Event = State::Event;
using namespace std::chrono_literals;

namespace {
permissions::ActivationBinding binding(std::uint64_t generation) {
  return {.plugin = permissions::PluginId("org.example.lifecycle"),
          .revision = permissions::Digest(std::string(64, 'a')),
          .policy_fingerprint = permissions::Digest(std::string(64, 'b')),
          .generation = generation};
}

void prepare(State &state) {
  state.apply(Event::prepare);
  state.apply(Event::worker_started);
}

void startup_and_publication() {
  State state;
  OMARCHY_CHECK(state.phase() == Phase::opening && state.epoch() == 0);
  state.open(1);
  state.apply(Event::prepare);
  state.apply(Event::preparation_deferred);
  OMARCHY_CHECK(state.phase() == Phase::opening && state.epoch() == 1);
  prepare(state);
  OMARCHY_CHECK(state.phase() == Phase::starting);
  // Allocation/publication may fail before or after provisional publication.
  state.apply(Event::publication_failed);
  OMARCHY_CHECK(state.publish(binding(7)));
  state.apply(Event::publication_failed);
  OMARCHY_CHECK(state.phase() == Phase::starting && state.epoch() == 1);
  OMARCHY_CHECK(state.publish(binding(7)));
  state.apply(Event::running_accepted);
  OMARCHY_CHECK(state.phase() == Phase::running && !state.retry_due());
  OMARCHY_CHECK(!state.publish(binding(7)));
  state.stop(2);
  OMARCHY_CHECK(state.phase() == Phase::stopping && state.epoch() == 2);
  OMARCHY_CHECK(!state.publish(binding(7)));
}

void authority_replacement() {
  State state;
  state.open(1);
  prepare(state);
  OMARCHY_CHECK(state.publish(binding(40)));
  state.apply(Event::running_accepted);
  state.fence(2);
  OMARCHY_CHECK(state.phase() == Phase::permission_changing && state.epoch() == 2);
  OMARCHY_CHECK(!state.publish(binding(40)));
  state.stop(3);
  state.restart(binding(41), 4);
  OMARCHY_CHECK(state.expected_binding() == binding(41) && state.epoch() == 4);
  prepare(state);
  OMARCHY_CHECK(!state.publish(binding(40)) && state.phase() == Phase::starting);
  auto changed = binding(41);
  changed.revision = permissions::Digest(std::string(64, 'c'));
  OMARCHY_CHECK(!state.publish(changed));
  changed = binding(41);
  changed.policy_fingerprint = permissions::Digest(std::string(64, 'c'));
  OMARCHY_CHECK(!state.publish(changed));
  changed = binding(41);
  changed.plugin = permissions::PluginId("org.example.other");
  OMARCHY_CHECK(!state.publish(changed));
  OMARCHY_CHECK(state.publish(binding(41)));
  // The pin survives provisional publication and rollback.
  OMARCHY_CHECK(state.expected_binding() == binding(41));
  state.apply(Event::publication_failed);
  OMARCHY_CHECK(!state.publish(binding(40)));
  OMARCHY_CHECK(state.publish(binding(41)));
  state.apply(Event::running_accepted);
  OMARCHY_CHECK(!state.expected_binding());
  state.fence(5);
  state.stop(6);
  state.apply(Event::disable);
  OMARCHY_CHECK(state.phase() == Phase::permission_disabled &&
                !state.retry_due() && state.retry_attempts() == 0);
}

void bounded_retry() {
  State state;
  const auto now = State::Clock::time_point{};
  constexpr std::array delays{250ms, 500ms, 1000ms, 2000ms, 4000ms,
                              8000ms, 16000ms, 30000ms, 30000ms};
  std::uint64_t epoch = 0;
  for (const auto delay : delays) {
    state.open(++epoch);
    state.stop(++epoch);
    state.apply(Event::failure, now);
    OMARCHY_CHECK(state.phase() == Phase::retry_wait && state.retry_due() == now + delay);
  }
  for (int attempt = 0; attempt < 300; ++attempt) {
    state.open(++epoch);
    OMARCHY_CHECK(!state.retry_due());
    state.stop(++epoch);
    state.apply(Event::failure, now);
  }
  OMARCHY_CHECK(state.retry_attempts() == 255 && state.retry_due() == now + 30000ms);
  state.open(++epoch);
  prepare(state);
  OMARCHY_CHECK(state.publish(binding(1)));
  state.apply(Event::running_accepted);
  OMARCHY_CHECK(state.retry_attempts() == 0);
  state.stop(++epoch);
  state.apply(Event::failure, now);
  OMARCHY_CHECK(state.retry_due() == now + 250ms);
  state.stop(++epoch);
  state.apply(Event::disable);
  OMARCHY_CHECK(!state.retry_due() && state.retry_attempts() == 0);
}

void failed_replacement_never_retries() {
  for (int stage = 0; stage < 4; ++stage) {
    State state;
    state.open(1);
    state.stop(2);
    state.apply(Event::failure);
    OMARCHY_CHECK(state.retry_attempts() == 1);
    state.restart(binding(9), 3);
    if (stage >= 1) state.apply(Event::prepare);
    if (stage >= 2) state.apply(Event::worker_started);
    if (stage >= 3) OMARCHY_CHECK(state.publish(binding(9)));
    state.stop(4);
    state.apply(Event::failure);
    OMARCHY_CHECK(state.phase() == Phase::permission_disabled &&
                  !state.retry_due() && !state.expected_binding() &&
                  state.retry_attempts() == 0 && state.epoch() == 4);
  }
}

void invalid_owner_events_fail_closed() {
  for (const auto event : {Event::preparation_deferred, Event::worker_started,
                           Event::publication_failed, Event::running_accepted,
                           Event::failure}) {
    expect_child_exit(42, [event] {
      std::set_terminate([] { ::_exit(42); });
      State state;
      state.apply(event);
    });
  }
  for (const auto stale : {std::uint64_t{0}, std::uint64_t{1}}) {
    expect_child_exit(42, [stale] {
      std::set_terminate([] { ::_exit(42); });
      State state;
      state.open(1);
      state.stop(stale);
    });
  }
}
} // namespace

int main() {
  return test_main([] {
    startup_and_publication();
    authority_replacement();
    bounded_retry();
    failed_replacement_never_retries();
    invalid_owner_events_fail_closed();
    return 0;
  });
}
