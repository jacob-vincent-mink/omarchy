#include "session_runtime_factory.hpp"

#include "audit_store.hpp"
#include "omarchy/plugin_runtime/providers/private_storage_backend.hpp"
#include "permission_projection.hpp"
#include "structured_broker.hpp"

#include <fcntl.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace omarchy::plugin_runtime::channel {
namespace audit = omarchy::plugins::audit;
namespace permissions = omarchy::plugins::permissions;
namespace policy = omarchy::plugin_runtime::policy;

bool runtime_service_available(
    const definitions::TrustedDefinitionRegistry &definitions,
    const RuntimeServices &services,
    const definitions::CapabilityReference &definition) noexcept {
  try {
    const auto resolved = definitions.resolve(definition);
    if (!resolved)
      return false;
    using Family = definitions::EnforcementFamily;
    if (resolved->definition->enforcement_family == Family::private_storage)
      return true;
    if (resolved->definition->enforcement_family == Family::notifications)
      return services.notification_send != nullptr;
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    if (!services.provider_catalog)
      return std::ranges::count(services.dynamic_services,
                                resolved->definition->adapter,
                                &TrustedDynamicService::binding) == 1 &&
             std::ranges::any_of(
                 services.dynamic_services, [&](const auto &service) {
                   return service.binding == resolved->definition->adapter &&
                          service.dispatch != nullptr;
                 });
#endif
    return services.provider_catalog &&
           services.provider_catalog->available(resolved->definition->adapter);
  } catch (...) {
    return false;
  }
}

namespace {

std::optional<std::vector<runtime::DynamicRoute>> prepare_runtime(
    const policy::GrantSnapshot &grants,
    const definitions::TrustedDefinitionRegistry &registry,
    const RuntimeServices &services, int state_directory,
    const Limits &limits) {
  std::vector<runtime::DynamicRoute> routes;
  std::shared_ptr<provider_host::ProviderActivation> activation;
  for (const auto &grant : grants.dynamic_grants) {
    if (grant.grant.state != permissions::GrantState::granted)
      continue;
    if (grant.binding != grants.binding ||
        !definitions::review_dynamic_grant(registry, grant))
      return std::nullopt;
    if (!runtime_service_available(registry, services,
                                   grant.request.definition)) {
      if (grant.request.required)
        return std::nullopt;
      continue;
    }
    const auto resolved = registry.resolve(grant.request.definition);
    if (!resolved)
      return std::nullopt;
    using Family = definitions::EnforcementFamily;
    const auto family = resolved->definition->enforcement_family;
    if (family == Family::private_storage || family == Family::notifications) {
      auto local = std::make_shared<providers::LocalProvider>(
          grant, family, state_directory, services.notification_send, services.context.get(),
          limits.maximum_storage_bytes, limits.maximum_storage_item_bytes);
      routes.push_back({
          .grant = grant,
          .adapter = {.binding = resolved->definition->adapter,
                      .dispatch = [local = std::move(local), context = services.context](
                          const definitions::AuthorizedDynamicRequest &request,
                          std::span<std::byte> response, std::size_t &written) noexcept {
                        (void)context; // Retain the notification service's borrowed context.
                        return local->dispatch(request, response, written);
                      }}});
      continue;
    }
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    if (!services.provider_catalog) {
      const auto matches = std::ranges::count(
          services.dynamic_services, resolved->definition->adapter,
          &TrustedDynamicService::binding);
      const auto configured = std::ranges::find(
          services.dynamic_services, resolved->definition->adapter,
          &TrustedDynamicService::binding);
      if (matches != 1 || configured == services.dynamic_services.end() ||
          !configured->dispatch)
        return std::nullopt;
      routes.push_back({
          .grant = grant,
          .adapter = {.binding = configured->binding,
                      .dispatch = [dispatch = configured->dispatch,
                                   context = services.context](
                          const definitions::AuthorizedDynamicRequest &request,
                          std::span<std::byte> response, std::size_t &written) noexcept {
                        return dispatch(request, response, written, context.get());
                      }}});
      continue;
    }
#endif
    if (!activation)
      activation = provider_host::ProviderActivation::create(
          services.provider_catalog, grants.binding);
    if (!activation)
      return std::nullopt;
    auto provider_route = activation->route(
        resolved->definition->adapter);
    if (!provider_route)
      return std::nullopt;
    routes.push_back({.grant = grant, .adapter = std::move(*provider_route)});
  }
  return routes;
}

} // namespace

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
SessionRuntimeFactory::SessionRuntimeFactory(
    definitions::TrustedDefinitionRegistry definitions,
    RuntimeServices services, Limits limits)
    : SessionRuntimeFactory(
          std::make_shared<const definitions::TrustedDefinitionRegistry>(
              std::move(definitions)),
          std::make_shared<const RuntimeServices>(
              std::move(services)),
          limits) {}
#endif

SessionRuntimeFactory::SessionRuntimeFactory(
    std::shared_ptr<const definitions::TrustedDefinitionRegistry> definitions,
    std::shared_ptr<const RuntimeServices> services,
    Limits limits)
    : definitions_(std::move(definitions)), services_(std::move(services)),
      limits_(limits) {
  if (!definitions_ || !services_ || limits_.maximum_audit_records == 0 ||
      limits_.maximum_audit_records > audit::kHardMaximumRecords ||
      limits_.maximum_storage_item_bytes == 0 ||
      limits_.maximum_storage_item_bytes > limits_.maximum_storage_bytes)
    throw std::invalid_argument("invalid runtime configuration");
}

const definitions::TrustedDefinitionRegistry &
SessionRuntimeFactory::definitions() const noexcept {
  return *definitions_;
}

std::optional<plugin::wire::permission_snapshot::PermissionSnapshot>
SessionRuntimeFactory::project_permissions(
    const plugins::manifest::ManifestV2 &manifest,
    const session::policy::GrantSnapshot &grants) const {
  auto projected = session::project_permission_snapshot(manifest, grants);
  if (!projected)
    return std::nullopt;
  const auto ordered =
      plugins::manifest::canonical_capability_requests(manifest.requests);
  if (ordered.size() != projected->permissions.size())
    return std::nullopt;
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    const auto &request = ordered[index];
    auto &row = projected->permissions[index];
    if (row.state != plugin::wire::permission_snapshot::GrantState::granted)
      continue;
    bool available = false;
      try {
        available = runtime_service_available(
            *definitions_, *services_,
            {.canonical_name = definitions::Name(request.capability),
             .definition_generation = request.definition_generation,
             .definition_digest =
                 definitions::Digest(request.definition_digest)});
      } catch (...) {
        return std::nullopt;
      }
    if (available)
      continue;
    if (request.required)
      return std::nullopt;
    row.operation_mask = 0;
  }
  return projected;
}

std::unique_ptr<AuthenticatedSessionRuntime>
SessionRuntimeFactory::create(
    const plugins::manifest::ManifestV2 &manifest,
    const session::policy::GrantSnapshot &grants, int revision_directory_fd,
    int private_state_directory_fd, std::uint64_t session_nonce,
    std::shared_ptr<session::LiveGenerationState> live_generation,
    std::shared_ptr<runtime::GestureEligibilityLatch> gesture_eligibility) {
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  if (test_on_create)
    test_on_create(manifest, grants, revision_directory_fd, private_state_directory_fd,
                   live_generation, gesture_eligibility);
#endif
  if (session_nonce == 0 || !live_generation || !gesture_eligibility ||
      manifest.id != grants.binding.plugin.view() ||
      !live_generation->current(grants.binding) ||
      ::fcntl(revision_directory_fd, F_GETFD) < 0 ||
      ::fcntl(private_state_directory_fd, F_GETFD) < 0)
    return {};
  try {
    if (!project_permissions(manifest, grants)) return {};
    auto prepared = prepare_runtime(grants, *definitions_, *services_, private_state_directory_fd, limits_);
    if (!prepared) return {};
    auto runtime = std::unique_ptr<AuthenticatedSessionRuntime>(new AuthenticatedSessionRuntime(
        definitions_, grants.binding, std::move(*prepared),
        session_nonce, std::move(live_generation), limits_.maximum_audit_records));
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    runtime->test_on_destroy = test_on_destroy;
#endif
    return runtime;
  } catch (...) {
    return {};
  }
}

} // namespace omarchy::plugin_runtime::channel
