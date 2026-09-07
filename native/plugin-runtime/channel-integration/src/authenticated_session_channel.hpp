#pragma once

#include "plugin_session_io.hpp"
#include "authenticated_channel.hpp"
#include "permission_contract.hpp"
#include "activation_snapshot.hpp"
#include "structured_broker.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace omarchy::plugin_runtime::runtime {
class GestureEligibilityAuthority;
class GestureEligibilityLatch;
} // namespace omarchy::plugin_runtime::runtime

namespace omarchy::plugin_runtime::channel {

namespace session = omarchy::plugin_runtime::host_session;
namespace permissions = omarchy::plugins::permissions;

struct AuthenticatedSessionLaunch final {
  permissions::ActivationBinding binding;
  session::UniqueFd revision_directory;
  session::UniqueFd private_state_directory;
  // Canonical manifest-indexed permission authority for this generation.
  // The channel sends it exactly once during authenticated startup and does
  // not become ready until the worker acknowledges loading its QML with it.
  std::vector<std::byte> permission_snapshot;
  std::vector<std::byte> settings_snapshot;
  std::vector<std::byte> presentation_snapshot;
};

// Owns the broker and its audit, definitions, providers and exact dispatch
// authority for the complete asynchronous channel lifetime. Only the trusted
// factory constructs this fixed, noncopyable composition.
class AuthenticatedSessionRuntime final : private session::DispatchAuthority,
                                          private GenerationAuthority {
public:
  ~AuthenticatedSessionRuntime() override {
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    if (test_on_destroy) test_on_destroy();
#endif
  }
  [[nodiscard]] session::StructuredBroker &broker() noexcept { return broker_; }

private:
  class Lease final : public session::DispatchAuthorityLease {
  public:
    explicit Lease(session::LiveGenerationState::EffectToken token)
        : token_(std::move(token)) {}
    [[nodiscard]] bool current_at_effect() const noexcept override {
      return token_.current();
    }

  private:
    session::LiveGenerationState::EffectToken token_;
  };

  AuthenticatedSessionRuntime(
      std::shared_ptr<const session::definitions::TrustedDefinitionRegistry> definitions,
      const permissions::ActivationBinding &binding,
      std::vector<runtime::DynamicRoute> routes, std::uint64_t session_nonce,
      std::shared_ptr<session::LiveGenerationState> live,
      std::size_t maximum_audit_records)
      : definitions_(std::move(definitions)), audit_(maximum_audit_records),
        binding_(binding), session_nonce_(session_nonce), live_(std::move(live)),
        broker_(binding, session_nonce, *definitions_,
                std::move(routes), audit_, *this) {}

  [[nodiscard]] bool
  is_current(const launcher::LaunchIdentity &identity) const noexcept override {
    return identity.plugin_id == binding_.plugin.view() &&
           identity.revision_sha256 == binding_.revision.view() &&
           identity.generation == binding_.generation && live_ &&
           live_->current(binding_);
  }


  [[nodiscard]] std::unique_ptr<session::DispatchAuthorityLease>
  acquire(const permissions::ActivationBinding &binding,
          std::uint64_t session_nonce,
          const plugin::wire::PacketView &request) override {
    if (binding != binding_ || session_nonce != session_nonce_ ||
        request.header.launch_generation != binding_.generation || !live_)
      return {};
    auto token = live_->acquire_effect(binding_);
    if (!token)
      return {};
    return std::make_unique<Lease>(std::move(*token));
  }

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  std::function<void()> test_on_destroy;
#endif
  std::shared_ptr<const session::definitions::TrustedDefinitionRegistry> definitions_;
  omarchy::plugins::audit::BoundedAuditLog audit_;
  permissions::ActivationBinding binding_;
  std::uint64_t session_nonce_ = 0;
  std::shared_ptr<session::LiveGenerationState> live_;
  session::StructuredBroker broker_;

  friend class SessionRuntimeFactory;
  friend class AuthenticatedSessionChannel;
};

// Adapts the authenticated v2 transport to PluginSessionIo's semantic message
// contract. Before transfer into PluginSessionIo an unlaunched instance owns
// no QObject, notifier, thread affinity, or installed wake callback and may be
// destroyed on any thread. After transfer, all methods and destruction run on
// the one PluginSessionIo worker thread.
class AuthenticatedSessionChannel final {
public:
  using TimePoint = launcher::Deadline;
  AuthenticatedSessionChannel(
      launcher::Supervisor supervisor, AuthenticatedSessionLaunch launch,
      std::unique_ptr<AuthenticatedSessionRuntime> runtime,
      std::shared_ptr<runtime::GestureEligibilityLatch> gesture_eligibility);
  ~AuthenticatedSessionChannel();
  AuthenticatedSessionChannel(const AuthenticatedSessionChannel &) = delete;
  AuthenticatedSessionChannel &
  operator=(const AuthenticatedSessionChannel &) = delete;

  [[nodiscard]] session::ChannelError launch(const session::SessionToken &token,
                                             TimePoint deadline);
  [[nodiscard]] session::ChannelError handshake(TimePoint deadline);
  [[nodiscard]] session::SendStatus send(const session::OwnedMessage &message,
                                         TimePoint deadline);
  [[nodiscard]] session::ReceiveResult receive(TimePoint deadline);
  [[nodiscard]] bool
  install_wake_handler(session::SessionWakeHandler handler) noexcept;
  void clear_wake_handler() noexcept;
  [[nodiscard]] bool revoke(const session::SessionToken &token,
                            TimePoint deadline) noexcept;
  void terminate(TimePoint deadline) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> implementation_;
};

} // namespace omarchy::plugin_runtime::channel
