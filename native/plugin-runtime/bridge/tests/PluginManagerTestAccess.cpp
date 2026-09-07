#include "PluginRuntimeController.h"
#include "SurfaceEndpointOwner.h"

#ifdef OMARCHY_PLUGIN_MANAGER_TESTING

#include "plugin_permission_authority_test_access.hpp"

#include <algorithm>
#include <ranges>

namespace omarchy::plugin_runtime::bridge {

bool SurfaceProjectionModelTestAccess::publish(
    PluginManager &manager,
    const plugins::permissions::ActivationBinding &binding,
    std::vector<SurfaceProjectionModel::SurfaceDeclaration> declarations,
    qulonglong revision) {
  return manager.surfaces_.publishSurfaces(binding, std::move(declarations),
                                           revision);
}

bool SurfaceProjectionModelTestAccess::withdraw(
    PluginManager &manager,
    const plugins::permissions::ActivationBinding &binding) {
  return manager.surfaces_.withdrawSurfaces(binding);
}

void PluginManagerTestAccess::installRuntime(
    PluginManager &manager,
    std::unique_ptr<channel::RuntimeBootstrap> bootstrap) {
  manager.runtime_.reset();
  manager.available_ = false;
  if (bootstrap)
    manager.runtime_ = std::unique_ptr<detail::PluginRuntimeController>(
        new detail::PluginRuntimeController(
            manager, std::move(bootstrap),
            detail::PluginRuntimeController::ManualTestTag{}));
}

bool PluginManagerTestAccess::scanRuntime(PluginManager &manager) {
  if (!manager.runtime_)
    return false;
  channel::ActivationCatalogError error{};
  auto candidate = channel::RuntimeBootstrapTestAccess::scan_catalog(
      *manager.runtime_->owner_->bootstrap_, error);
  return manager.runtime_->owner_->acceptScan(std::move(candidate));
}

std::vector<PluginManagerTestAccess::SlotObservation>
PluginManagerTestAccess::runtimeSlots(const PluginManager &manager) {
  std::vector<SlotObservation> result;
  if (!manager.runtime_)
    return result;
  result.reserve(manager.runtime_->owner_->slots_.size());
  for (const auto &slot : manager.runtime_->owner_->slots_)
    result.push_back(
        {.plugin = slot.plugin,
         .epoch = slot.lifecycle.epoch(),
         .retry_attempts = slot.lifecycle.retry_attempts(),
         .retry_wait =
             slot.lifecycle.phase() == channel::RuntimeSessionOwner::Phase::retry_wait,
         .opening =
             slot.lifecycle.phase() == channel::RuntimeSessionOwner::Phase::opening,
         .preparing =
             slot.lifecycle.phase() == channel::RuntimeSessionOwner::Phase::preparing,
         .starting =
             slot.lifecycle.phase() == channel::RuntimeSessionOwner::Phase::starting,
         .running =
             slot.lifecycle.phase() == channel::RuntimeSessionOwner::Phase::running,
         .permission_transaction = slot.permission_transaction != nullptr,
         .permission_changing =
             slot.lifecycle.phase() ==
             channel::RuntimeSessionOwner::Phase::permission_changing,
         .permission_disabled =
             slot.lifecycle.phase() ==
             channel::RuntimeSessionOwner::Phase::permission_disabled,
         .has_runtime_root = slot.root != nullptr,
         .has_endpoint_owner = slot.presentation != nullptr,
         .last_state = slot.last_state,
         .last_error = slot.last_error});
  return result;
}

bool PluginManagerTestAccess::retryRuntime(PluginManager &manager,
                                           std::string_view plugin) {
  if (!manager.runtime_)
    return false;
  const auto found = std::ranges::lower_bound(
      manager.runtime_->owner_->slots_, plugin, {},
      [](const channel::RuntimeSessionOwner::Slot &slot) {
        return slot.plugin;
      });
  if (found == manager.runtime_->owner_->slots_.end() || found->plugin != plugin ||
      found->lifecycle.phase() != channel::RuntimeSessionOwner::Phase::retry_wait)
    return false;
  manager.runtime_->owner_->start(*found);
  manager.runtime_->owner_->requestPreparations();
  return true;
}

bool PluginManagerTestAccess::queueStaleRunningCallback(
    PluginManager &manager, std::string_view plugin) {
  if (!manager.runtime_)
    return false;
  const auto found = std::ranges::lower_bound(
      manager.runtime_->owner_->slots_, plugin, {},
      [](const channel::RuntimeSessionOwner::Slot &slot) {
        return slot.plugin;
      });
  if (found == manager.runtime_->owner_->slots_.end() || found->plugin != plugin)
    return false;
  try {
    auto state = std::make_shared<channel::RuntimeSessionOwner::HookState>(
        found->plugin, found->lifecycle.epoch());
    found->callback_state = state;
    channel::RuntimeSessionOwner::Hook hook(std::move(state));
    hook.state_changed(host_session::SessionState::running,
                       host_session::SessionError::none);
    return true;
  } catch (...) {
    return false;
  }
}

void PluginManagerTestAccess::setJobSubmitter(PluginManager &manager,
                                              JobSubmitter submitter) {
  if (manager.runtime_) {
    if (submitter)
      manager.runtime_->owner_->job_submitter_ =
          [submitter = std::move(submitter)](channel::RuntimeSessionOwner::JobKind kind,
                                             std::function<void()> job) {
            return submitter(static_cast<TestJobKind>(kind), std::move(job));
          };
    else
      manager.runtime_->owner_->job_submitter_ = {};
  }
}

void PluginManagerTestAccess::setJobEntryProbe(PluginManager &manager,
                                               JobEntryProbe probe) {
  if (manager.runtime_) {
    if (probe)
      manager.runtime_->owner_->job_entry_probe_ =
          [probe = std::move(probe)](channel::RuntimeSessionOwner::JobKind kind) {
            probe(static_cast<TestJobKind>(kind));
          };
    else
      manager.runtime_->owner_->job_entry_probe_ = {};
  }
}

void PluginManagerTestAccess::requestAsyncScan(PluginManager &manager) {
  if (manager.runtime_)
    manager.runtime_->owner_->requestScan();
}

void PluginManagerTestAccess::requestPreparations(PluginManager &manager) {
  if (manager.runtime_)
    manager.runtime_->owner_->requestPreparations();
}

void PluginManagerTestAccess::drainRuntime(PluginManager &manager) {
  if (manager.runtime_)
    manager.runtime_->owner_->drainCompletions();
}

std::optional<PluginManagerTestAccess::SurfaceIntentCallback>
PluginManagerTestAccess::surfaceIntentCallback(PluginManager &manager,
                                               std::string_view plugin,
                                               std::uint64_t epoch) {
  if (!manager.runtime_)
    return std::nullopt;
  auto *slot = manager.runtime_->owner_->exact(plugin, epoch);
  if (!slot)
    return std::nullopt;
  try {
    if (!slot->callback_state)
      slot->callback_state =
          std::make_shared<channel::RuntimeSessionOwner::HookState>(
              slot->plugin, slot->lifecycle.epoch());
    const auto state = slot->callback_state;
    return SurfaceIntentCallback{
        .deliver =
            [state, &manager](host_session::AdmittedSurfaceIntent intent) {
              channel::RuntimeSessionOwner::Hook hook(state);
              return hook.accept(std::move(intent));
            },
        .pending =
            [state] {
              std::scoped_lock lock(state->intent_mutex);
              return state->intents.size();
            }};
  } catch (...) {
    return std::nullopt;
  }
}

bool PluginManagerTestAccess::stageRunningSurfaceIntentSlot(
    PluginManager &manager, std::string_view plugin, std::uint64_t epoch,
    const plugins::permissions::ActivationBinding &binding,
    std::vector<SurfaceProjectionModel::SurfaceDeclaration> declarations) {
  if (!manager.runtime_ || binding.plugin.view() != plugin)
    return false;
  auto *slot = manager.runtime_->owner_->exact(plugin, epoch);
  if (!slot || slot->root || slot->presentation ||
      !SurfaceProjectionModelTestAccess::publish(
          manager, binding, std::move(declarations), epoch))
    return false;
  using State = channel::ProductSessionState;
  if (slot->lifecycle.phase() == State::Phase::opening)
    slot->lifecycle.apply(State::Event::prepare);
  if (slot->lifecycle.phase() == State::Phase::preparing)
    slot->lifecycle.apply(State::Event::worker_started);
  if (!slot->lifecycle.publish(binding))
    return false;
  slot->test_running_binding = binding;
  slot->test_surface_endpoint = true;
  return true;
}

bool PluginManagerTestAccess::routeTrustedPointer(
    PluginManager &manager, std::string_view plugin, std::uint64_t epoch,
    QStringView surface_key, bool pressed) {
  if (!manager.runtime_)
    return false;
  auto *slot = manager.runtime_->owner_->exact(plugin, epoch);
  if (!slot || !slot->presentation)
    return false;
  return SurfaceEndpointOwnerTestAccess::route_input(
      dynamic_cast<SurfaceEndpointOwner &>(*slot->presentation), surface_key,
      {.payload = surface::PointerButton{
           .position = {16U << surface::kQ16FractionBits,
                        16U << surface::kQ16FractionBits},
           .button = static_cast<std::uint32_t>(Qt::LeftButton),
           .state = pressed ? surface::ButtonState::pressed
                            : surface::ButtonState::released,
           .buttons = pressed ? static_cast<std::uint32_t>(Qt::LeftButton)
                              : 0U},
       .device = 1,
       .trusted_physical = true});
}

std::uint8_t
PluginManagerTestAccess::preparationCount(const PluginManager &manager) {
  return manager.runtime_
             ? manager.runtime_->owner_->gate_->preparations_in_flight.load()
             : 0;
}

std::uint8_t
PluginManagerTestAccess::executingPermissionJobs(const PluginManager &manager) {
  return manager.runtime_
             ? manager.runtime_->owner_->gate_->permissions_in_flight.load()
             : 0;
}

std::optional<plugins::permissions::DecisionActor>
PluginManagerTestAccess::pendingPermissionActor(const PluginManager &manager,
                                                std::string_view plugin) {
  if (!manager.runtime_)
    return std::nullopt;
  const auto found =
      std::ranges::find(manager.runtime_->owner_->slots_, plugin,
                        &channel::RuntimeSessionOwner::Slot::plugin);
  return found != manager.runtime_->owner_->slots_.end() &&
                 found->permission_transaction &&
                 found->permission_transaction->consent_review
             ? std::optional(found->permission_transaction->confirmation.actor)
             : std::nullopt;
}

bool PluginManagerTestAccess::scanInFlight(const PluginManager &manager) {
  return manager.runtime_ && manager.runtime_->owner_->gate_->scan_in_flight.load();
}

bool PluginManagerTestAccess::revokePermissionImmediatelyForTest(
    PluginManager &manager, std::string_view plugin, std::uint64_t epoch,
    const plugins::definitions::CapabilityReference &capability,
    std::uint64_t expected_sequence) {
  return manager.revokePermissionImmediatelyForTest(plugin, epoch, capability,
                                                    expected_sequence);
}

std::uint8_t PluginManagerTestAccess::occupiedPreparationLanes(
    const PluginManager &manager) {
  if (!manager.runtime_)
    return 0;
  std::scoped_lock lock(manager.runtime_->owner_->gate_->mutex);
  return static_cast<std::uint8_t>(std::ranges::count_if(
      manager.runtime_->owner_->gate_->preparation_results,
      [](const auto &result) { return result != nullptr; }));
}

bool PluginManagerTestAccess::deliverLifecycle(PluginManager &manager,
                                               std::string_view plugin,
                                               std::uint64_t epoch,
                                               std::uint8_t state,
                                               std::uint8_t error) {
  if (!manager.runtime_)
    return false;
  auto *slot = manager.runtime_->owner_->exact(plugin, epoch);
  if (!slot)
    return false;
  try {
    if (!slot->callback_state)
      slot->callback_state =
          std::make_shared<channel::RuntimeSessionOwner::HookState>(
              slot->plugin, slot->lifecycle.epoch());
    channel::RuntimeSessionOwner::Hook hook(slot->callback_state);
    hook.state_changed(static_cast<host_session::SessionState>(state),
                       static_cast<host_session::SessionError>(error));
    return true;
  } catch (...) {
    return false;
  }
}

bool PluginManagerTestAccess::clockIsNondecreasing(PluginManager &manager) {
  if (!manager.runtime_)
    return false;
  const auto first = manager.runtime_->clock_.now_nanoseconds();
  const auto second = manager.runtime_->clock_.now_nanoseconds();
  return second >= first;
}

std::optional<host_session::AuthorityView>
PluginManagerTestAccess::permissionView(PluginManager &manager,
                                        std::string_view plugin,
                                        std::uint64_t epoch) {
  auto *slot = manager.runtime_ ? manager.runtime_->owner_->exact(plugin, epoch) : nullptr;
  return slot && slot->permissions && !slot->permission_transaction
             ? channel::PluginPermissionAuthorityTestAccess::list(*slot->permissions)
             : std::nullopt;
}

std::shared_ptr<const host_session::ConsentReview>
PluginManagerTestAccess::preparePermissionReview(PluginManager &manager,
                                                 std::string_view plugin,
                                                 std::uint64_t epoch) {
  auto *slot = manager.runtime_ ? manager.runtime_->owner_->exact(plugin, epoch) : nullptr;
  return slot && slot->permissions && !slot->permission_transaction
             ? channel::PluginPermissionAuthorityTestAccess::prepare_review(*slot->permissions)
             : nullptr;
}

bool PluginManagerTestAccess::revokePermission(
    PluginManager &manager, std::string_view plugin, std::uint64_t epoch,
    const plugins::definitions::CapabilityReference &definition,
    std::uint64_t expected_sequence) {
  if (!manager.runtime_)
    return false;
  try {
    auto result = std::make_shared<channel::RuntimeSessionOwner::PermissionTransaction>();
    result->dynamic = definition;
    result->expected_sequence = expected_sequence;
    return manager.runtime_->owner_->beginPermissionMutation(plugin, epoch, std::move(result));
  } catch (...) {
    return false;
  }
}

bool PluginManagerTestAccess::applyPermissionReview(
    PluginManager &manager, std::string_view plugin, std::uint64_t epoch,
    std::shared_ptr<const host_session::ConsentReview> review,
    const host_session::ConsentConfirmation &confirmation,
    std::span<const host_session::DynamicConsentDecision> dynamic_decisions) {
  if (!manager.runtime_)
    return false;
  try {
    if (!review)
      return false;
    auto result = std::make_shared<channel::RuntimeSessionOwner::PermissionTransaction>();
    result->consent_review = std::move(review);
    result->confirmation = confirmation;
    result->dynamic_decisions.assign(dynamic_decisions.begin(),
                                     dynamic_decisions.end());
    return manager.runtime_->owner_->beginPermissionMutation(plugin, epoch, std::move(result));
  } catch (...) {
    return false;
  }
}

std::weak_ptr<const void>
PluginManagerTestAccess::deliveryGate(const PluginManager &manager) {
  return manager.runtime_ ? std::weak_ptr<const void>(manager.runtime_->owner_->gate_)
                          : std::weak_ptr<const void>{};
}

bool PluginManager::revokePermissionImmediatelyForTest(
    std::string_view plugin, std::uint64_t epoch,
    const plugins::definitions::CapabilityReference &capability,
    std::uint64_t expected_sequence) {
  if (!runtime_)
    return false;
  auto *slot = runtime_->owner_->exact(plugin, epoch);
  return slot && slot->permissions &&
         slot->permissions->revoke(capability, expected_sequence, nullptr)
                 .status == host_session::AuthorityMutationResult::applied;
}
} // namespace omarchy::plugin_runtime::bridge

#endif
