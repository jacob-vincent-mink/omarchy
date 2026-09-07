#include "runtime_session_owner.hpp"

#include <QThread>
#include <algorithm>
#include <chrono>
#include <ranges>
#include <stdexcept>

namespace omarchy::plugin_runtime::channel {
namespace {

constexpr int kCatalogScanIntervalMilliseconds = 2000;
constexpr int kActiveCompletionIntervalMilliseconds = 10;
constexpr int kIdleCompletionIntervalMilliseconds = 200;

} // namespace

std::unique_ptr<RuntimeSessionOwner>
RuntimeSessionOwner::open(RuntimeHost &host) noexcept {
  try {
    channel::RuntimeBootstrapError error{};
    auto bootstrap = channel::RuntimeBootstrap::open(error);
    if (!bootstrap)
      return {};
    return std::unique_ptr<RuntimeSessionOwner>(
        new RuntimeSessionOwner(host, std::move(bootstrap)));
  } catch (...) {
    return {};
  }
}

RuntimeSessionOwner::RuntimeSessionOwner(
    RuntimeHost &host,
    std::unique_ptr<channel::RuntimeBootstrap> bootstrap)
    : host_(host), bootstrap_(std::move(bootstrap)) {
  configureTimers();
  scan_timer_.start();
  QTimer::singleShot(0, &scan_timer_, [this] { requestScan(); });
}

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
RuntimeSessionOwner::RuntimeSessionOwner(
    RuntimeHost &host,
    std::unique_ptr<channel::RuntimeBootstrap> bootstrap, ManualTestTag)
    : host_(host), bootstrap_(std::move(bootstrap)), manual_test_(true) {
  configureTimers();
}
#endif

void RuntimeSessionOwner::configureTimers() {
  if (QThread::currentThread() != host_.eventOwner().thread())
    throw std::invalid_argument("runtime owner must share the host event thread");
  // A fixed bounded poll keeps catalog observation simple and
  // non-overlapping; each scan still proves freshness before reconciliation.
  scan_timer_.setInterval(kCatalogScanIntervalMilliseconds);
  QObject::connect(&scan_timer_, &QTimer::timeout, &host_.eventOwner(),
                   [this] { requestScan(); });
  QObject::connect(&retry_timer_, &QTimer::timeout, &host_.eventOwner(),
                   [this] { retryDue(); });
  QObject::connect(&completion_timer_, &QTimer::timeout, &host_.eventOwner(),
                   [this] { drainCompletions(); });
  retry_timer_.setSingleShot(true);
}

RuntimeSessionOwner::~RuntimeSessionOwner() noexcept {
  if (QThread::currentThread() != scan_timer_.thread())
    std::terminate();
  scan_timer_.stop();
  retry_timer_.stop();
  completion_timer_.stop();
  gate_->canceled.store(true, std::memory_order_release);
  for (auto &slot : slots_)
    stopRuntime(slot);
}

void RuntimeSessionOwner::drainCompletions() noexcept {
  std::shared_ptr<ScanResult> scan;
  std::array<std::shared_ptr<PreparationResult>, 2> preparations;
  std::array<std::shared_ptr<PermissionReadResult>, 2> permission_reads;
  std::array<std::shared_ptr<PermissionTransaction>, 2> permission_results;
  std::shared_ptr<InstallResult> install;
  {
    std::scoped_lock lock(gate_->mutex);
    scan = std::move(gate_->scan_result);
    preparations = std::move(gate_->preparation_results);
    permission_reads = std::move(gate_->permission_read_results);
    permission_results = std::move(gate_->permission_results);
    install = std::move(gate_->install_result);
  }
  if (scan) {
    gate_->scan_in_flight.store(false);
    acceptScan(std::move(scan->catalog));
  }
  if (install) {
    gate_->install_in_flight.store(false);
    if (install->error.empty()) {
      host_.invalidatePlugin(install->plugin);
      if (!install->catalog || !acceptScan(std::move(install->catalog)))
        install->error = "catalog-rejected";
    }
    host_.completeInstall(install->serial, std::move(install->plugin),
                             std::move(install->revision),
                             std::move(install->error));
  }
  for (auto &result : preparations) {
    if (!result)
      continue;
    --gate_->preparations_in_flight;
    auto *slot = exact(result->plugin, result->epoch);
    if (slot == nullptr || slot->lifecycle.phase() != Phase::preparing)
      continue;
    if (!result->permissions ||
        (slot->permissions && slot->permissions != result->permissions)) {
      fail(*slot);
      continue;
    }
    slot->permissions = std::move(result->permissions);
    if (result->permission_disabled) {
      slot->lifecycle.apply(Event::disable);
      continue;
    }
    try {
      auto callback_state =
          std::make_shared<HookState>(slot->plugin, slot->lifecycle.epoch());
      auto hook = std::make_unique<Hook>(callback_state, host_);
      auto root = channel::PluginSession::commit(
          std::move(result->prepared), *hook, host_.eventOwner());
      if (!root) {
        fail(*slot);
      } else {
        slot->callback_state = std::move(callback_state);
        slot->hook = std::move(hook);
        slot->root = std::move(root);
        slot->lifecycle.apply(Event::worker_started);
      }
    } catch (...) {
      fail(*slot);
    }
  }
  for (auto &result : permission_reads) {
    if (!result)
      continue;
    --gate_->permission_reads_in_flight;
    auto *slot = exact(result->plugin, result->epoch);
    if (!slot || slot->permissions != result->authority ||
        slot->permission_transaction ||
        slot->permission_read_serial != result->serial) {
      host_.failPermissionControl(result->serial, "stale-context");
      continue;
    }
    slot->permission_read_serial = 0;
    host_.completePermissionRead(result->serial, std::move(result->plugin),
                                    result->epoch, std::move(result->authority),
                                    std::move(result->view),
                                    std::move(result->review));
  }
  for (auto &slot : slots_) {
    const auto transaction = slot.permission_transaction;
    if (!transaction)
      continue;
    const auto delivery = transaction->delivery.load(std::memory_order_acquire);
    if ((delivery & PermissionTransaction::fenced) != 0 &&
        slot.lifecycle.phase() != Phase::permission_changing)
      fencePermission(slot);
    if ((delivery & PermissionTransaction::complete) != 0) {
      completePermission(slot, *transaction);
      if (slot.permission_transaction == transaction)
        slot.permission_transaction.reset();
    }
  }
  for (auto &result : permission_results) {
    if (!result)
      continue;
    --gate_->permissions_in_flight;
    if (result->control_serial == 0)
      continue;
    const bool applied =
        result->consent_review
            ? result->review.publication ==
                      host_session::ConsentResult::applied &&
                  result->review.promotion ==
                      host_session::AuthorityMutationResult::applied &&
                  result->review.binding.has_value()
            : result->revocation.status ==
                  host_session::AuthorityMutationResult::applied;
    host_.completePermissionMutation(
        result->control_serial, applied,
        applied ? std::string{} : std::string("authority-rejected"));
  }
  for (auto &slot : slots_) {
    const auto state = slot.callback_state;
    if (!state)
      continue;
    const auto encoded =
        state->lifecycle.exchange(0, std::memory_order_acq_rel);
    if (encoded == 0)
      continue;
    const auto packed = static_cast<std::uint16_t>(encoded - 1U);
    stateChanged(state->plugin, state->epoch,
                 static_cast<host_session::SessionState>(packed & 0xffU),
                 static_cast<host_session::SessionError>(packed >> 8U));
  }
  for (auto &slot : slots_)
    drainSurfaceIntents(slot);
  requestPreparations();
  armCompletionTimer();
}

void RuntimeSessionOwner::disable(Slot &slot) noexcept {
  stopRuntime(slot);
  slot.lifecycle.apply(Event::disable);
}
void RuntimeSessionOwner::armCompletionTimer() noexcept {
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  if (manual_test_)
    return;
#endif
  // A finished worker leaves its transaction attached until one UI drain
  // consumes it, so job accounting alone cannot decide timer liveness.
  const bool active = gate_->scan_in_flight.load() != 0 ||
                      gate_->preparations_in_flight.load() != 0 ||
                      gate_->permissions_in_flight.load() != 0 ||
                      gate_->permission_reads_in_flight.load() != 0 ||
                      gate_->install_in_flight.load() ||
                      std::ranges::any_of(slots_, [](const Slot &slot) {
                        return slot.lifecycle.phase() == Phase::opening ||
                               slot.lifecycle.phase() == Phase::preparing ||
                               slot.lifecycle.phase() == Phase::starting ||
                               slot.permission_transaction != nullptr;
                      });
  const bool monitoring = std::ranges::any_of(
      slots_, [](const Slot &slot) { return slot.callback_state != nullptr; });
  if (!active && !monitoring) {
    completion_timer_.stop();
    return;
  }
  const int interval = active ? kActiveCompletionIntervalMilliseconds
                              : kIdleCompletionIntervalMilliseconds;
  if (!completion_timer_.isActive() || completion_timer_.interval() != interval)
    completion_timer_.start(interval);
}

RuntimeSessionOwner::Slot *
RuntimeSessionOwner::exact(std::string_view plugin,
                               std::uint64_t epoch) noexcept {
  const auto found = std::ranges::lower_bound(
      slots_, plugin, {}, [](const Slot &slot) { return slot.plugin; });
  return found != slots_.end() && found->plugin == plugin &&
                 found->lifecycle.epoch() == epoch
             ? &*found
             : nullptr;
}

void RuntimeSessionOwner::fail(Slot &slot) noexcept {
  stopRuntime(slot);
  slot.lifecycle.apply(Event::failure);
  armRetryTimer();
}

void RuntimeSessionOwner::stopRuntime(Slot &slot) noexcept {
  slot.lifecycle.stop(nextEpoch());
  slot.permission_transaction.reset();
  slot.permission_read_serial = 0;
  withdraw(slot);
  slot.root.reset();
  slot.hook.reset();
  slot.callback_state.reset();
}

std::uint64_t RuntimeSessionOwner::nextEpoch() noexcept {
  if (next_epoch_ == std::numeric_limits<std::uint64_t>::max())
    std::terminate();
  return ++next_epoch_;
}

} // namespace omarchy::plugin_runtime::channel
