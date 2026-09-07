#pragma once

#include "MultiSurfaceRouter.h"
#include "activation_snapshot.hpp"
#include "authority_store.hpp"
#include "gesture_intent.hpp"
#include "authenticated_session_channel.hpp"
#include "plugin_permission_authority.hpp"
#include "session_runtime_factory.hpp"
#include "surface_session_port.hpp"
#include "omarchy/plugin/wire/permission_snapshot.hpp"

#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace omarchy::plugin_runtime::channel {

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
class PluginSessionTestAccess;
#endif
class PreparedPluginSession;

class SessionRuntimeFactory;

class PluginSessionEvents {
public:
  virtual ~PluginSessionEvents() = default;
  virtual void state_changed(session::SessionState state,
                             session::SessionError error) = 0;
  virtual void render_rejected(session::RouteResult result) = 0;
  [[nodiscard]] virtual bool update_settings(
      const permissions::ActivationBinding &,
      std::string_view) { return false; }
};

// The shell-side consumer performs the final freshness check immediately
// before publishing the effect. Keeping this interface move-only prevents an
// admitted intent from being copied or replayed between host components.
class SurfaceIntentSink {
public:
  virtual ~SurfaceIntentSink() = default;
  [[nodiscard]] virtual bool
  accept(host_session::AdmittedSurfaceIntent intent) = 0;
};

// Trusted, non-owning integration hook. It outlives the session and queues
// lifecycle/permission work to the next host-loop turn rather than reentering
// the session from a callback. Queued work is fenced before destruction.
class PluginRuntimeHooks : public PluginSessionEvents, public SurfaceIntentSink {
public:
  ~PluginRuntimeHooks() override = default;
};

struct PluginRuntimePreparationResult final {
  std::unique_ptr<PreparedPluginSession> runtime;
  bool permission_disabled = false;
};

enum class PluginSessionCreateError : std::uint8_t {
  none,
  invalid_activation,
  nonce_unavailable,
  runtime_unavailable,
  allocation_failed,
};

enum class SurfaceAttachStatus : std::uint8_t {
  attached,
  session_not_running,
  undeclared_surface,
  already_attached,
  invalid_correlations,
  allocation_failed,
};

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
enum class SurfaceAttachFault : std::uint8_t { none, after_router };
#endif

struct SurfaceAttachResult final {
  SurfaceAttachStatus status = SurfaceAttachStatus::undeclared_surface;
  session::surface::SurfaceKey key{};
  [[nodiscard]] explicit operator bool() const noexcept {
    return status == SurfaceAttachStatus::attached;
  }
};

// Product composition root for one verified plugin activation. One instance
// owns one worker/channel/sandbox/sidecar lifecycle and routes every attached
// surface over that single authenticated render lane.
class PluginSession final : private QObject, private SurfaceSessionPort {
public:
  ~PluginSession() override;
  PluginSession(const PluginSession &) = delete;
  PluginSession &operator=(const PluginSession &) = delete;

  void start();
  [[nodiscard]] bool
  send_render(std::uint16_t message_type, std::uint64_t correlation_id,
              std::vector<std::byte> payload,
              std::vector<session::UniqueFd> descriptors = {});
  void revoke();
  void stop();

  [[nodiscard]] SurfaceAttachResult
  attach(std::string_view declared_surface,
         std::span<const std::uint64_t> correlations,
         session::SurfaceEndpoint &endpoint);
  [[nodiscard]] bool detach(std::string_view declared_surface,
                            const session::SurfaceEndpoint &endpoint) noexcept;
  // Called only after trusted host-input admission accepts physical input. If
  // the input packet cannot be sent, the caller must clear the exact source.
  [[nodiscard]] bool arm_surface_intent(session::surface::SurfaceKey source,
                                        std::uint64_t input_sequence);
  void clear_surface_intent_eligibility(
      session::surface::SurfaceKey source) noexcept;
  [[nodiscard]] std::size_t surface_count() const noexcept;
  [[nodiscard]] session::SessionState state() const noexcept;
  [[nodiscard]] session::SessionError error() const noexcept;
  [[nodiscard]] const permissions::ActivationBinding &binding() const noexcept;
  [[nodiscard]] std::uint64_t session_nonce_value() const noexcept;
  [[nodiscard]] const plugins::manifest::ManifestV2 &manifest() const noexcept;
  [[nodiscard]] const session::policy::GrantSnapshot &grants() const noexcept;

private:
  struct DeclaredSurfaceSet final {
    permissions::ActivationBinding binding;
    std::string plugin_id;
    std::vector<std::string> names;
    std::string canonical_surfaces;
  };
  [[nodiscard]] std::optional<permissions::ActivationBinding> session_binding() const;
  [[nodiscard]] std::optional<DeclaredSurfaceSet> declared_surfaces() const noexcept;
  [[nodiscard]] SurfaceSessionPort &surface_session() noexcept { return *this; }
  [[nodiscard]] std::optional<SurfaceDescription>
  describe(std::string_view declared_surface) const noexcept override;
  [[nodiscard]] bool attach(const SurfaceDescription &expected,
                            session::SurfaceEndpoint &endpoint) noexcept override;
  [[nodiscard]] bool detach(const SurfaceDescription &expected,
                            const session::SurfaceEndpoint &endpoint) noexcept override;
  [[nodiscard]] bool arm_surface_intent(const SurfaceDescription &expected,
                                       std::uint64_t input_sequence) noexcept override;
  void clear_surface_intent_eligibility(const SurfaceDescription &expected) noexcept override;
  [[nodiscard]] bool send_render_packet_impl(const SurfaceDescription &expected,
      const plugin::wire::EnvelopeHeader &header, std::vector<std::byte> payload,
      std::vector<session::UniqueFd> descriptors) noexcept override;
  [[nodiscard]] bool matches(const SurfaceDescription &expected) const noexcept;
  [[nodiscard]] bool on_owner_thread() const noexcept {
    return std::this_thread::get_id() == owner_thread_;
  }
  struct Configuration final {
    std::shared_ptr<PluginPermissionAuthority> permissions;
    std::optional<std::string> settings;
    std::optional<std::string> presentation;
    Limits runtime_limits;
    session::SessionLimits session_limits;
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    std::function<launcher::Supervisor()> test_supervisor_factory;
    void (*test_before_final_fence)(host_session::AuthorityStore &, void *) noexcept = nullptr;
    void *test_before_final_fence_context = nullptr;
#endif
  };
  [[nodiscard]] static PluginRuntimePreparationResult prepare(Configuration &&configuration);
  [[nodiscard]] static std::unique_ptr<PluginSession> commit(
      std::unique_ptr<PreparedPluginSession> prepared,
      PluginRuntimeHooks &hooks, QObject &ui_owner);

  [[nodiscard]] static std::unique_ptr<PreparedPluginSession>
  prepare(launcher::Supervisor supervisor, session::ActivationSnapshot snapshot,
          SessionRuntimeFactory &runtime_factory,
          PluginSessionCreateError &error, session::SessionLimits limits,
          std::optional<std::string> settings = std::nullopt,
          std::optional<std::string> presentation = std::nullopt,
          std::shared_ptr<runtime::GestureEligibilityClock> gesture_clock = {});

  PluginSession(PreparedPluginSession &&prepared, PluginSessionEvents *events,
                SurfaceIntentSink *intent_sink);

  void state_changed(session::SessionState state,
                     session::SessionError error);
  void message_received(session::OwnedMessage message);
  [[nodiscard]] bool send(session::ChannelLane lane, std::uint16_t message_type,
                          std::uint64_t correlation_id, std::vector<std::byte> payload,
                          std::vector<session::UniqueFd> descriptors = {});
  [[nodiscard]] std::optional<session::surface::SurfaceKey>
  find_surface(std::string_view name) const noexcept;

  // Retain authority until every session resource has been destroyed.
  std::shared_ptr<PluginPermissionAuthority> permissions_;
  const std::thread::id owner_thread_ = std::this_thread::get_id();
  bool session_committed_ = false;
  session::SessionToken token_;
  session::UniqueFd activation_record_;
  plugins::manifest::ManifestV2 manifest_;
  session::policy::GrantSnapshot grants_;
  std::shared_ptr<session::LiveGenerationState> live_;
  session::MultiSurfaceRouter router_;
  PluginSessionEvents *events_ = nullptr;
  SurfaceIntentSink *intent_sink_ = nullptr;
  std::shared_ptr<runtime::GestureEligibilityLatch> gesture_eligibility_;
  host_session::GestureIntentAuthority gesture_intents_;
  session::PluginSessionIo io_;

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  SurfaceAttachFault surface_attach_fault_ = SurfaceAttachFault::none;
  friend class PluginSessionTestAccess;
  friend class ReviewedSessionTestAccess;
#endif
  friend class session::PluginSessionIo;
  friend class RuntimeBootstrap;
  friend class RuntimeSessionOwner;
};

// Fully validated and assembled activation state with no QObject ownership or
// effects. It may cross from a lifecycle worker to the UI thread. The runtime
// session requires its authority reservation before constructing and starting it.
class PreparedPluginSession final {
  PreparedPluginSession(
      session::SessionToken token, session::UniqueFd activation_record,
      plugins::manifest::ManifestV2 manifest,
      session::policy::GrantSnapshot grants,
      std::shared_ptr<session::LiveGenerationState> live,
      std::unique_ptr<AuthenticatedSessionChannel> channel,
      std::shared_ptr<runtime::GestureEligibilityLatch> gesture_eligibility,
      session::SessionLimits limits) noexcept
      : token(std::move(token)),
        activation_record(std::move(activation_record)),
        manifest(std::move(manifest)), grants(std::move(grants)),
        live(std::move(live)), channel(std::move(channel)),
        gesture_eligibility(std::move(gesture_eligibility)), limits(limits) {}

  // Retain authority until all assembled session resources are destroyed.
  std::shared_ptr<PluginPermissionAuthority> permissions;
  session::SessionToken token;
  session::UniqueFd activation_record;
  plugins::manifest::ManifestV2 manifest;
  session::policy::GrantSnapshot grants;
  std::shared_ptr<session::LiveGenerationState> live;
  std::unique_ptr<AuthenticatedSessionChannel> channel;
  std::shared_ptr<runtime::GestureEligibilityLatch> gesture_eligibility;
  session::SessionLimits limits;
  std::optional<host_session::PreparedLiveBinding> live_binding;
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  void (*before_final_fence)(host_session::AuthorityStore &, void *) noexcept = nullptr;
  void *before_final_fence_context = nullptr;
#endif

  friend class PluginSession;
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  friend class PluginSessionTestAccess;
#endif
};

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
class PluginSessionTestAccess final {
public:
  [[nodiscard]] static session::PluginSessionIo &io(PluginSession &session) { return session.io_; }

  [[nodiscard]] static std::pair<const session::SessionToken &, AuthenticatedSessionChannel &>
  transport(PreparedPluginSession &prepared) {
    return {prepared.token, *prepared.channel};
  }

  [[nodiscard]] static std::unique_ptr<PreparedPluginSession>
  prepare_from_activation(launcher::Supervisor supervisor,
                          session::ActivationSnapshot snapshot,
                          SessionRuntimeFactory &runtime_factory,
                          PluginSessionCreateError &error,
                          session::SessionLimits limits = {},
                          std::optional<std::string> settings = std::nullopt,
                          std::shared_ptr<runtime::GestureEligibilityClock> gesture_clock = {});
  [[nodiscard]] static std::unique_ptr<PluginSession>
  commit(std::unique_ptr<PreparedPluginSession> prepared,
         PluginSessionCreateError &error, PluginSessionEvents *events = nullptr,
         SurfaceIntentSink *intent_sink = nullptr);
  [[nodiscard]] static int
  activation_record_fd(const PluginSession &session) noexcept;
  [[nodiscard]] static std::shared_ptr<session::LiveGenerationState>
  live_generation(const PluginSession &session) noexcept;
  [[nodiscard]] static bool ui_affine(const PluginSession &session,
                                      const QThread *thread) noexcept;
  static void set_surface_attach_fault(PluginSession &session,
                                       SurfaceAttachFault fault) noexcept;
};
#endif

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
class ReviewedSessionTestAccess final {
public:
  [[nodiscard]] static std::optional<permissions::ActivationBinding>
  session_binding(const PluginSession &root) {
    return root.session_binding();
  }
  [[nodiscard]] static SurfaceSessionPort &
  surface_session(PluginSession &root) noexcept {
    return root.surface_session();
  }
  [[nodiscard]] static PluginRuntimePreparationResult
  prepare_from_parts(
      int activation_root_fd, int revision_root_fd, int state_root_fd,
      host_session::UniqueFd authority_root,
      permissions::PluginId plugin, std::uint32_t trusted_uid,
      std::string activation_record,
      std::shared_ptr<const definitions::TrustedDefinitionRegistry> definitions,
      std::shared_ptr<const RuntimeServices> services,
      Limits runtime_limits, session::SessionLimits session_limits,
      std::function<launcher::Supervisor()> supervisor_factory = {},
      void (*before_final_fence)(host_session::AuthorityStore &,
                                 void *) noexcept = nullptr,
      void *before_final_fence_context = nullptr);
  [[nodiscard]] static std::unique_ptr<PluginSession>
  commit(std::unique_ptr<PreparedPluginSession> prepared,
         PluginRuntimeHooks &hooks, QObject &ui_owner);
  [[nodiscard]] static bool ui_affine(
      const PluginSession &root,
      const QObject &ui_owner) noexcept;
};
#endif

} // namespace omarchy::plugin_runtime::channel
