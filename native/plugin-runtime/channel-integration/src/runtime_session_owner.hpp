#pragma once

#include "runtime_host.hpp"
#include "runtime_bootstrap.hpp"
#include "product_session_state.hpp"

#include <QThreadPool>
#include <QTimer>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omarchy::plugin_runtime::bridge {
class PluginManager;
class PluginManagerTestAccess;
namespace detail { class PluginRuntimeController; }
}

namespace omarchy::plugin_runtime::channel {

// Owns the catalog's active sessions. Each slot is the sole product owner of
// its authority, generation-tagged jobs, worker/provider composition, bounded
// callback queue and presentation lifetime. The host port projects results.
class RuntimeSessionOwner final {
public:
  using Clock = std::chrono::steady_clock;

  static std::unique_ptr<RuntimeSessionOwner>
  open(RuntimeHost &host) noexcept;
  RuntimeSessionOwner(RuntimeHost &host,
                          std::unique_ptr<channel::RuntimeBootstrap> bootstrap);
  ~RuntimeSessionOwner() noexcept;

  bool beginPermissionRead(
      std::uint64_t serial, std::string plugin, bool review,
      std::optional<plugins::permissions::Digest> expected_revision) noexcept;
  bool beginInstall(std::uint64_t serial, int archive_fd) noexcept;
  bool beginControlledPermissionApply(
      std::uint64_t serial, std::string_view plugin, std::uint64_t epoch,
      const std::shared_ptr<channel::PluginPermissionAuthority> &authority,
      std::shared_ptr<const host_session::ConsentReview> review,
      const host_session::ConsentConfirmation &confirmation,
      std::span<const host_session::DynamicConsentDecision>
          dynamic_decisions) noexcept;
  bool beginControlledPermissionRevoke(
      std::uint64_t serial, std::string_view plugin, std::uint64_t epoch,
      const std::shared_ptr<channel::PluginPermissionAuthority> &authority,
      const plugins::definitions::CapabilityReference &definition,
      std::uint64_t expected_sequence) noexcept;
  [[nodiscard]] RuntimePresentation *presentation(
      std::string_view plugin, std::uint64_t epoch,
      const permissions::ActivationBinding &binding) noexcept;

private:
  using Phase = channel::ProductSessionState::Phase;
  using Event = channel::ProductSessionState::Event;
  enum class JobKind : std::uint8_t { scan, preparation, permission, install };

  struct HookState final {
    static constexpr std::size_t maximum_pending_surface_intents = 64;
    explicit HookState(std::string plugin, std::uint64_t epoch);

    const std::string plugin;
    const std::uint64_t epoch;
    std::atomic<std::uint16_t> lifecycle = 0;
    std::mutex intent_mutex;
    std::deque<host_session::AdmittedSurfaceIntent> intents;
  };

  struct Hook final : channel::PluginRuntimeHooks {
    Hook(std::shared_ptr<HookState> state, RuntimeHost &host);
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    explicit Hook(std::shared_ptr<HookState> state) : state(std::move(state)) {}
#endif
    void state_changed(host_session::SessionState state,
                       host_session::SessionError error) override;
    void render_rejected(host_session::RouteResult) override;
    bool accept(host_session::AdmittedSurfaceIntent intent) override;
    bool update_settings(
        const plugins::permissions::ActivationBinding &binding,
        std::string_view canonical_entry) override;
    std::shared_ptr<HookState> state;
    RuntimeHost *host = nullptr;
  };

  struct ScanResult final {
    std::unique_ptr<channel::ActivationCatalog> catalog;
  };
  struct PreparationResult final {
    std::string plugin;
    std::uint64_t epoch = 0;
    std::shared_ptr<channel::PluginPermissionAuthority> permissions;
    std::optional<std::string> settings;
    std::optional<std::string> presentation;
    std::unique_ptr<channel::PreparedPluginSession> prepared;
    bool permission_disabled = false;
  };
  struct PermissionReadResult final {
    std::uint64_t serial = 0;
    std::string plugin;
    std::uint64_t epoch = 0;
    std::shared_ptr<channel::PluginPermissionAuthority> authority;
    std::optional<host_session::AuthorityView> view;
    std::shared_ptr<const host_session::ConsentReview> review;
    std::optional<plugins::permissions::Digest> expected_revision;
  };
  struct InstallResult final {
    std::uint64_t serial = 0;
    std::string plugin;
    std::string revision;
    std::string error;
    std::unique_ptr<channel::ActivationCatalog> catalog;
  };
  struct PermissionTransaction final {
    static constexpr std::uint8_t fenced = 1U << 0U;
    static constexpr std::uint8_t complete = 1U << 1U;

    std::atomic<std::uint8_t> delivery = 0;
    std::uint64_t control_serial = 0;
    std::shared_ptr<channel::PluginPermissionAuthority> authority;
    plugins::definitions::CapabilityReference dynamic;
    std::uint64_t expected_sequence = 0;
    // Present only for apply; an absent review identifies revocation.
    std::shared_ptr<const host_session::ConsentReview> consent_review;
    host_session::ConsentConfirmation confirmation;
    std::vector<host_session::DynamicConsentDecision> dynamic_decisions;
    host_session::AuthorityRevocationResult revocation;
    channel::ReviewedPermissionApplyResult review;
  };
  struct Slot final {
    std::string plugin;
    channel::ProductSessionState lifecycle;
    std::shared_ptr<HookState> callback_state;
    std::unique_ptr<Hook> hook;
    std::shared_ptr<channel::PluginPermissionAuthority> permissions;
    std::uint64_t permission_read_serial = 0;
    std::shared_ptr<PermissionTransaction> permission_transaction;
    std::unique_ptr<channel::PluginSession> root;
    std::unique_ptr<RuntimePresentation> presentation;
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    std::optional<plugins::permissions::ActivationBinding> test_running_binding;
    bool test_surface_endpoint = false;
    std::uint8_t last_state = 0;
    std::uint8_t last_error = 0;
#endif
  };
  struct DeliveryGate final {
    std::mutex mutex;
    std::atomic<bool> canceled = false;
    std::atomic<bool> scan_in_flight = false;
    std::atomic<std::uint8_t> preparations_in_flight = 0;
    std::atomic<std::uint8_t> permissions_in_flight = 0;
    std::atomic<std::uint8_t> permission_reads_in_flight = 0;
    std::atomic<bool> install_in_flight = false;
    std::shared_ptr<ScanResult> scan_result;
    std::array<std::shared_ptr<PreparationResult>, 2> preparation_results;
    std::array<std::shared_ptr<PermissionReadResult>, 2>
        permission_read_results;
    std::array<std::shared_ptr<PermissionTransaction>, 2> permission_results;
    std::shared_ptr<InstallResult> install_result;
  };
  struct PermissionFenceObserver final : host_session::AuthorityFenceObserver {
    explicit PermissionFenceObserver(
        std::shared_ptr<PermissionTransaction> transaction);
    void live_generation_closed() noexcept override;
    std::shared_ptr<PermissionTransaction> transaction;
  };

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  struct ManualTestTag final {};
  RuntimeSessionOwner(RuntimeHost &host,
                          std::unique_ptr<channel::RuntimeBootstrap> bootstrap,
                          ManualTestTag);
#endif

  void configureTimers();
  void requestScan() noexcept;
  void drainCompletions() noexcept;
  bool
  reconcile(std::unique_ptr<channel::ActivationCatalog> candidate) noexcept;
  Slot makeSlot(std::string_view plugin);
  bool
  acceptScan(std::unique_ptr<channel::ActivationCatalog> candidate) noexcept;
  void start(Slot &slot) noexcept;
  void requestPreparations() noexcept;
  bool beginPermissionMutation(
      std::string_view plugin, std::uint64_t epoch,
      std::shared_ptr<PermissionTransaction> result) noexcept;
  void fencePermission(Slot &slot) noexcept;
  void completePermission(Slot &slot,
                          const PermissionTransaction &result) noexcept;
  void withdraw(Slot &slot) noexcept;
  void disable(Slot &slot) noexcept;

  template <typename Job> bool submit(JobKind kind, Job &&job) {
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    auto observed = [probe = job_entry_probe_, kind,
                     job = std::forward<Job>(job)]() mutable {
      if (probe)
        probe(kind);
      job();
    };
    if (job_submitter_)
      return job_submitter_(kind, std::move(observed));
    return QThreadPool::globalInstance()->tryStart(std::move(observed));
#else
    (void)kind;
    return QThreadPool::globalInstance()->tryStart(std::forward<Job>(job));
#endif
  }

  void armCompletionTimer() noexcept;
  void stateChanged(std::string_view plugin, std::uint64_t epoch,
                    host_session::SessionState state,
                    host_session::SessionError error) noexcept;
  void drainSurfaceIntents(Slot &slot) noexcept;
  bool publishRunning(std::string_view plugin, std::uint64_t epoch,
                      const plugins::permissions::ActivationBinding &binding,
                      PluginSession::DeclaredSurfaceSet declarations) noexcept;
  Slot *exact(std::string_view plugin, std::uint64_t epoch) noexcept;
  void fail(Slot &slot) noexcept;
  void retryDue() noexcept;
  void armRetryTimer() noexcept;
  void stopRuntime(Slot &slot) noexcept;
  std::uint64_t nextEpoch() noexcept;

  RuntimeHost &host_;
  std::shared_ptr<const channel::RuntimeBootstrap> bootstrap_;
  std::shared_ptr<DeliveryGate> gate_ = std::make_shared<DeliveryGate>();
  std::unique_ptr<channel::ActivationCatalog> catalog_;
  std::vector<Slot> slots_;
  QTimer scan_timer_;
  QTimer retry_timer_;
  QTimer completion_timer_;
  std::uint64_t next_epoch_ = 0;
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  bool manual_test_ = false;
  std::function<bool(JobKind, std::function<void()>)> job_submitter_;
  std::function<void(JobKind)> job_entry_probe_;
#endif

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  friend class RuntimeSessionOwnerTestAccess;
  friend class ::omarchy::plugin_runtime::bridge::PluginManagerTestAccess;
  friend class ::omarchy::plugin_runtime::bridge::PluginManager;
  friend class ::omarchy::plugin_runtime::bridge::detail::PluginRuntimeController;
#endif
};

} // namespace omarchy::plugin_runtime::channel
