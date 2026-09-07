#pragma once

#include "permission_contract.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace omarchy::plugin_runtime::channel {

// Product lifecycle, serialized by the host owner thread. The delivery epoch
// invalidates asynchronous work; it is NOT an authorization generation or a
// wire nonce. Exact reviewed bindings remain independent of that epoch.
// Transport startup/protocol progress is subordinate and reports events here.
class ProductSessionState final {
public:
  using Clock = std::chrono::steady_clock;
  enum class Phase : std::uint8_t {
    opening, preparing, starting, running, permission_changing,
    permission_disabled, retry_wait, stopping,
  };
  enum class Event : std::uint8_t {
    prepare, preparation_deferred, worker_started, publication_failed,
    running_accepted, disable, failure,
  };

  [[nodiscard]] Phase phase() const noexcept { return phase_; }
  [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
  [[nodiscard]] std::uint8_t retry_attempts() const noexcept { return retry_attempts_; }
  [[nodiscard]] const std::optional<Clock::time_point> &retry_due() const noexcept {
    return retry_due_;
  }
  [[nodiscard]] const std::optional<plugins::permissions::ActivationBinding> &
  expected_binding() const noexcept { return expected_binding_; }

  void open(std::uint64_t epoch) noexcept {
    advance(epoch);
    phase_ = Phase::opening;
    retry_due_.reset();
  }
  void stop(std::uint64_t epoch) noexcept {
    advance(epoch);
    phase_ = Phase::stopping;
    retry_due_.reset();
  }
  void fence(std::uint64_t epoch) noexcept {
    advance(epoch);
    phase_ = Phase::permission_changing;
  }
  void restart(plugins::permissions::ActivationBinding binding,
               std::uint64_t epoch) noexcept {
    expected_binding_ = std::move(binding);
    open(epoch);
  }
  [[nodiscard]] bool publish(
      const plugins::permissions::ActivationBinding &binding) noexcept {
    if (phase_ != Phase::starting ||
        (expected_binding_ && *expected_binding_ != binding))
      return false;
    phase_ = Phase::running;
    return true;
  }

  // No writable phase/counter projection is exposed to the Qt adapter.
  // Invalid event order is an internal owner violation, not a plugin event.
  void apply(Event event, Clock::time_point now = Clock::now()) noexcept {
    switch (event) {
    case Event::prepare:
      require(Phase::opening);
      phase_ = Phase::preparing;
      break;
    case Event::preparation_deferred:
      require(Phase::preparing);
      phase_ = Phase::opening;
      break;
    case Event::worker_started:
      require(Phase::preparing);
      phase_ = Phase::starting;
      break;
    case Event::publication_failed:
      if (phase_ != Phase::running && phase_ != Phase::starting)
        std::terminate();
      phase_ = Phase::starting;
      break;
    case Event::running_accepted:
      require(Phase::running);
      expected_binding_.reset();
      retry_attempts_ = 0;
      break;
    case Event::failure: {
      require(Phase::stopping);
      if (!expected_binding_) {
        if (retry_attempts_ < std::numeric_limits<std::uint8_t>::max())
          ++retry_attempts_;
        const auto exponent = std::min<int>(retry_attempts_ - 1, 7);
        const auto delay = std::min(250 * (1 << exponent), 30000);
        phase_ = Phase::retry_wait;
        retry_due_ = now + std::chrono::milliseconds(delay);
        break;
      }
      // A failed exact authority replacement must not silently retry.
      [[fallthrough]];
    }
    case Event::disable:
      phase_ = Phase::permission_disabled;
      expected_binding_.reset();
      retry_attempts_ = 0;
      retry_due_.reset();
      break;
    }
  }

private:
  void advance(std::uint64_t epoch) noexcept {
    if (epoch <= epoch_)
      std::terminate();
    epoch_ = epoch;
  }
  void require(Phase expected) const noexcept {
    if (phase_ != expected)
      std::terminate();
  }
  Phase phase_ = Phase::opening;
  std::uint64_t epoch_ = 0;
  std::uint8_t retry_attempts_ = 0;
  std::optional<Clock::time_point> retry_due_;
  std::optional<plugins::permissions::ActivationBinding> expected_binding_;
};

} // namespace omarchy::plugin_runtime::channel
