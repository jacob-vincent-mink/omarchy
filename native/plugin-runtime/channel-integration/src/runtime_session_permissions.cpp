#include "runtime_session_owner.hpp"

#include <algorithm>
#include <ranges>

namespace omarchy::plugin_runtime::channel {

RuntimeSessionOwner::PermissionFenceObserver::PermissionFenceObserver(
    std::shared_ptr<PermissionTransaction> transaction)
    : transaction(std::move(transaction)) {}

void RuntimeSessionOwner::PermissionFenceObserver::
    live_generation_closed() noexcept {
  transaction->delivery.fetch_or(PermissionTransaction::fenced,
                                 std::memory_order_release);
}

bool RuntimeSessionOwner::beginPermissionRead(
    std::uint64_t serial, std::string plugin, bool review,
    std::optional<plugins::permissions::Digest> expected_revision) noexcept {
  constexpr std::uint8_t kMaximumConcurrentPermissionReads = 2;
  if (serial == 0 || gate_->permission_reads_in_flight.load() >=
                         kMaximumConcurrentPermissionReads)
    return false;
  const auto found = std::ranges::lower_bound(
      slots_, plugin, {}, [](const Slot &slot) { return slot.plugin; });
  if (found == slots_.end() || found->plugin != plugin || !found->permissions ||
      found->permission_transaction || found->permission_read_serial != 0 ||
      (found->lifecycle.phase() != Phase::running &&
       found->lifecycle.phase() != Phase::permission_disabled))
    return false;
  bool counted = false;
  try {
    auto result = std::make_shared<PermissionReadResult>();
    result->serial = serial;
    result->plugin = std::move(plugin);
    result->epoch = found->lifecycle.epoch();
    result->authority = found->permissions;
    result->expected_revision = std::move(expected_revision);
    found->permission_read_serial = serial;
    ++gate_->permission_reads_in_flight;
    counted = true;
    const auto gate = gate_;
    const bool started = submit(JobKind::permission, [gate, result, review] {
      try {
        result->view = result->authority->list();
        if (review)
          result->review = result->authority->prepare_review();
        if (result->review && result->expected_revision &&
            result->review->candidate_binding.revision !=
                *result->expected_revision)
          result->review.reset();
      } catch (...) {
        result->view.reset();
        result->review.reset();
      }
      std::scoped_lock lock(gate->mutex);
      if (gate->canceled.load(std::memory_order_acquire)) {
        --gate->permission_reads_in_flight;
        return;
      }
      const auto destination =
          std::ranges::find(gate->permission_read_results,
                            std::shared_ptr<PermissionReadResult>{});
      if (destination == gate->permission_read_results.end()) {
        --gate->permission_reads_in_flight;
        return;
      }
      *destination = std::move(result);
    });
    if (!started) {
      --gate_->permission_reads_in_flight;
      counted = false;
      found->permission_read_serial = 0;
      return false;
    }
    armCompletionTimer();
    return true;
  } catch (...) {
    if (counted)
      --gate_->permission_reads_in_flight;
    if (found->permission_read_serial == serial)
      found->permission_read_serial = 0;
    return false;
  }
}

bool RuntimeSessionOwner::beginControlledPermissionApply(
    std::uint64_t serial, std::string_view plugin, std::uint64_t epoch,
    const std::shared_ptr<channel::PluginPermissionAuthority> &authority,
    std::shared_ptr<const host_session::ConsentReview> review,
    const host_session::ConsentConfirmation &confirmation,
    std::span<const host_session::DynamicConsentDecision>
        dynamic_decisions) noexcept {
  auto *slot = exact(plugin, epoch);
  if (serial == 0 || !review || !slot || slot->permissions != authority)
    return false;
  try {
    auto result = std::make_shared<PermissionTransaction>();
    result->control_serial = serial;
    result->consent_review = std::move(review);
    result->confirmation = confirmation;
    result->dynamic_decisions.assign(dynamic_decisions.begin(),
                                     dynamic_decisions.end());
    return beginPermissionMutation(plugin, epoch, std::move(result));
  } catch (...) {
    return false;
  }
}

bool RuntimeSessionOwner::beginControlledPermissionRevoke(
    std::uint64_t serial, std::string_view plugin, std::uint64_t epoch,
    const std::shared_ptr<channel::PluginPermissionAuthority> &authority,
    const plugins::definitions::CapabilityReference &definition,
    std::uint64_t expected_sequence) noexcept {
  auto *slot = exact(plugin, epoch);
  if (serial == 0 || !slot || slot->permissions != authority)
    return false;
  try {
    auto result = std::make_shared<PermissionTransaction>();
    result->control_serial = serial;
    result->expected_sequence = expected_sequence;
    result->dynamic = definition;
    return beginPermissionMutation(plugin, epoch, std::move(result));
  } catch (...) {
    return false;
  }
}

bool RuntimeSessionOwner::beginPermissionMutation(
    std::string_view plugin, std::uint64_t epoch,
    std::shared_ptr<PermissionTransaction> result) noexcept {
  constexpr std::uint8_t kMaximumConcurrentPermissionMutations = 2;
  if (!result || gate_->permissions_in_flight.load() >=
                     kMaximumConcurrentPermissionMutations)
    return false;
  auto *slot = exact(plugin, epoch);
  const bool running = slot && slot->lifecycle.phase() == Phase::running;
  const bool review = static_cast<bool>(result->consent_review);
  if (!slot || !slot->permissions || slot->permission_transaction ||
      slot->permission_read_serial != 0 ||
      (review ? !(running || slot->lifecycle.phase() == Phase::permission_disabled)
              : !running))
    return false;

  result->authority = slot->permissions;
  slot->permission_transaction = result;

  ++gate_->permissions_in_flight;
  const auto gate = gate_;
  bool started = false;
  try {
    started = submit(JobKind::permission, [gate, result] {
      try {
        PermissionFenceObserver observer(result);
        if (result->consent_review) {
          result->review = result->authority->apply_review(
              *result->consent_review, result->confirmation,
              result->dynamic_decisions, &observer);
        } else {
          result->revocation = result->authority->revoke(
              result->dynamic, result->expected_sequence, &observer);
        }
      } catch (...) {
        result->revocation.status =
            host_session::AuthorityMutationResult::io_error;
      }
      result->delivery.fetch_or(PermissionTransaction::complete,
                                std::memory_order_release);
      try {
        std::scoped_lock lock(gate->mutex);
        if (gate->canceled.load(std::memory_order_acquire)) {
          --gate->permissions_in_flight;
          return;
        }
        const auto destination = std::ranges::find(
            gate->permission_results, std::shared_ptr<PermissionTransaction>{});
        if (destination == gate->permission_results.end())
          std::terminate();
        *destination = result;
      } catch (...) {
        std::terminate();
      }
    });
  } catch (...) {
  }
  if (!started) {
    --gate_->permissions_in_flight;
    if (slot->permission_transaction == result)
      slot->permission_transaction.reset();
    return false;
  }
  armCompletionTimer();
  return true;
}

void RuntimeSessionOwner::fencePermission(Slot &slot) noexcept {
  slot.lifecycle.fence(nextEpoch());
  withdraw(slot);
}

void RuntimeSessionOwner::completePermission(
    Slot &slot, const PermissionTransaction &result) noexcept {
  std::optional<plugins::permissions::ActivationBinding> binding;
  bool applied = false;
  if (result.consent_review) {
    if (result.review.publication != host_session::ConsentResult::applied) {
      if (slot.lifecycle.phase() == Phase::permission_changing)
        disable(slot);
      return;
    }
    applied = result.review.promotion ==
              host_session::AuthorityMutationResult::applied;
    binding = result.review.binding;
  } else {
    applied = result.revocation.status ==
              host_session::AuthorityMutationResult::applied;
    if (result.revocation.status ==
            host_session::AuthorityMutationResult::invalid ||
        result.revocation.status ==
            host_session::AuthorityMutationResult::stale_sequence) {
      if (slot.lifecycle.phase() == Phase::permission_changing)
        disable(slot);
      return;
    }
    if (result.revocation.activatable)
      binding = result.revocation.binding;
  }
  if (!applied || !binding) {
    disable(slot);
    return;
  }
  stopRuntime(slot);
  slot.lifecycle.restart(std::move(*binding), nextEpoch());
}

} // namespace omarchy::plugin_runtime::channel
