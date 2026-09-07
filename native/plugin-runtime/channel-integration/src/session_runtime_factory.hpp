#pragma once

#include "authenticated_session_channel.hpp"
#include "omarchy/plugin/wire/permission_snapshot.hpp"
#include "dynamic_activation.hpp"
#include "omarchy/plugin_runtime/providers/local_provider.hpp"
#include "omarchy/plugin_runtime/provider_host/provider_host.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace omarchy::plugin_runtime::channel {

namespace definitions = omarchy::plugins::definitions;
namespace providers = omarchy::plugin_runtime::providers;
namespace provider_host = omarchy::plugin_runtime::provider_host;

class PluginSession;
class RuntimeBootstrapTestAccess;

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
struct TrustedDynamicService final {
  definitions::AdapterBinding binding;
  bool (*dispatch)(const definitions::AuthorizedDynamicRequest &,
                   std::span<std::byte>, std::size_t &, void *) noexcept = nullptr;
};
#endif

// All callbacks are trusted, synchronous host services. They must finish every
// effect before returning, must not invoke permission/session lifecycle APIs,
// and must not retain requests, payloads, or authority for later work. The
// shared context is retained for every runtime using it.
struct RuntimeServices final {
  std::shared_ptr<void> context;
  providers::NotificationSend notification_send = nullptr;
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  std::vector<TrustedDynamicService> dynamic_services{};
#endif
  std::shared_ptr<const provider_host::ProviderCatalog> provider_catalog{};
};

// Trusted availability is derived from the frozen service table and exact
// definition identity. It never returns a callback or selects a provider by a
// plugin-supplied alias.
[[nodiscard]] bool runtime_service_available(
    const definitions::TrustedDefinitionRegistry &definitions,
    const RuntimeServices &services,
    const definitions::CapabilityReference &definition) noexcept;
struct Limits final {
  std::size_t maximum_audit_records = 1024;
  std::uint64_t maximum_storage_bytes = 64 * 1024 * 1024;
  std::uint64_t maximum_storage_item_bytes = 1024 * 1024;
};

inline constexpr std::chrono::milliseconds kProviderSettlementMargin{250};
static_assert(provider_host::kMaximumProviderInvocationTimeout +
                  kProviderSettlementMargin <=
              session::SessionLimits::kMaximumIoTimeout);

// Provider dispatch is synchronous at the authenticated channel boundary.
// Runtime sessions therefore retain a bounded transport/scheduling margin
// after the provider's invocation contract, without changing generic sessions.
[[nodiscard]] inline session::SessionLimits
provider_backed_session_limits() noexcept {
  session::SessionLimits limits;
  limits.io_timeout =
      provider_host::kMaximumProviderInvocationTimeout +
      kProviderSettlementMargin;
  return limits;
}

// Sole concrete factory for the runtime authenticated-session path. Plugin
// data supplies no callback, provider pointer, definition, quota, or path.
class SessionRuntimeFactory final {
public:
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  // Test-only value seam. Runtime composition shares one frozen registry
  // and service context from RuntimeBootstrap.
  SessionRuntimeFactory(
      definitions::TrustedDefinitionRegistry definitions,
      RuntimeServices services, Limits limits = {});
  std::function<void(const plugins::manifest::ManifestV2 &,
                     const session::policy::GrantSnapshot &, int, int,
                     const std::shared_ptr<session::LiveGenerationState> &,
                     const std::shared_ptr<runtime::GestureEligibilityLatch> &)>
      test_on_create;
  std::function<void()> test_on_destroy;
#endif
  [[nodiscard]] const definitions::TrustedDefinitionRegistry &
  definitions() const noexcept;
  // Projects only available trusted services; optional operation masks may
  // narrow, but projection never widens durable authority.
  [[nodiscard]] std::optional<
      plugin::wire::permission_snapshot::PermissionSnapshot>
  project_permissions(
      const plugins::manifest::ManifestV2 &manifest,
      const session::policy::GrantSnapshot &grants) const;

  // Assembly owns provider/runtime state but performs no external effects.
  // Descriptors are borrowed only for this call; retained descriptors must be
  // owned, and effect leases consult the exact supplied live generation.
  [[nodiscard]] std::unique_ptr<AuthenticatedSessionRuntime>
  create(const plugins::manifest::ManifestV2 &manifest,
         const session::policy::GrantSnapshot &grants,
         int revision_directory_fd, int private_state_directory_fd,
         std::uint64_t session_nonce,
         std::shared_ptr<session::LiveGenerationState> live_generation,
         std::shared_ptr<runtime::GestureEligibilityLatch>
             gesture_eligibility);

private:
  SessionRuntimeFactory(
      std::shared_ptr<const definitions::TrustedDefinitionRegistry> definitions,
      std::shared_ptr<const RuntimeServices> services,
      Limits limits = {});

  std::shared_ptr<const definitions::TrustedDefinitionRegistry> definitions_;
  std::shared_ptr<const RuntimeServices> services_;
  Limits limits_;

  friend class PluginSession;
  friend class RuntimeBootstrapTestAccess;
};

} // namespace omarchy::plugin_runtime::channel
