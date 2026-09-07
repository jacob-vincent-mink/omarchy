#include "plugin_session.hpp"

#include <QThread>

#include <algorithm>
#include <utility>

namespace omarchy::plugin_runtime::channel {

bool PluginSession::matches(const SurfaceDescription &expected) const noexcept {
  if (binding() != expected.binding ||
      session_nonce_value() != expected.session_nonce ||
      expected.key.generation != expected.binding.generation ||
      expected.plugin_id != manifest_.id ||
      expected.plugin_id != expected.binding.plugin.view())
    return false;
  const auto &names = manifest_.surface_names;
  const auto found = std::find(names.begin(), names.end(), expected.surface_name);
  return found != names.end() &&
         expected.key.id == static_cast<std::uint64_t>(found - names.begin()) + 1;
}

std::optional<SurfaceDescription> PluginSession::describe(
    std::string_view declared_surface) const noexcept {
  if (!on_owner_thread() || state() != session::SessionState::running)
    return {};
  try {
    const auto &names = manifest_.surface_names;
    const auto found = std::find(names.begin(), names.end(), declared_surface);
    if (found == names.end())
      return {};
    const auto index = static_cast<std::uint64_t>(found - names.begin());
    return SurfaceDescription{
        .binding = binding(),
        .key = {.id = index + 1, .generation = binding().generation},
        .session_nonce = session_nonce_value(),
        .plugin_id = manifest_.id,
        .surface_name = std::string(declared_surface),
        .canonical_surfaces = manifest_.canonical_surfaces,
    };
  } catch (...) {
    return {};
  }
}

bool PluginSession::attach(const SurfaceDescription &expected,
                           session::SurfaceEndpoint &endpoint) noexcept {
  if (!on_owner_thread() || state() != session::SessionState::running || !matches(expected))
    return false;
  try {
    const auto correlations = session::surface::render_correlations(expected.key);
    const auto result = attach(expected.surface_name, correlations, endpoint);
    return result && result.key == expected.key;
  } catch (...) {
    // Surface attachment is transactional: exceptions never publish.
    return false;
  }
}

bool PluginSession::detach(const SurfaceDescription &expected,
                           const session::SurfaceEndpoint &endpoint) noexcept {
  if (!on_owner_thread())
    std::terminate();
  // A stopped or replacement session has already fenced this exact endpoint.
  return state() != session::SessionState::running || !matches(expected) ||
         detach(expected.surface_name, endpoint);
}

bool PluginSession::send_render_packet_impl(
    const SurfaceDescription &expected,
    const plugin::wire::EnvelopeHeader &header, std::vector<std::byte> payload,
    std::vector<session::UniqueFd> descriptors) noexcept {
  if (!on_owner_thread())
    return false;
  try {
    return state() == session::SessionState::running && matches(expected) &&
           header.endpoint_role == plugin::wire::EndpointRole::render &&
           header.launch_generation == binding().generation &&
           header.role_protocol_version == session::surface::kRenderRoleVersion &&
           header.flags == 0 && header.payload_length == payload.size() &&
           send_render(header.message_type, header.correlation_id,
                       std::move(payload), std::move(descriptors));
  } catch (...) {
    return false;
  }
}

bool PluginSession::arm_surface_intent(
    const SurfaceDescription &expected, std::uint64_t input_sequence) noexcept {
  if (!on_owner_thread())
    return false;
  try {
    return state() == session::SessionState::running && matches(expected) &&
           arm_surface_intent(expected.key, input_sequence);
  } catch (...) {
    return false;
  }
}

void PluginSession::clear_surface_intent_eligibility(
    const SurfaceDescription &expected) noexcept {
  if (!on_owner_thread())
    std::terminate();
  if (state() == session::SessionState::running && matches(expected))
    clear_surface_intent_eligibility(expected.key);
}

PluginRuntimePreparationResult PluginSession::prepare(
    Configuration &&configuration) {
  try {
    if (!configuration.permissions)
      return {};
    auto &permissions = configuration.permissions;
    SessionRuntimeFactory runtime_factory(permissions->definitions_, permissions->services_,
                                          configuration.runtime_limits);
    auto loaded = permissions->load_activation();
    if (!loaded.snapshot) {
      if (loaded.error != host_session::ActivationError::grant_unavailable)
        return {};
      // No active grant resolved for the selected revision. Keep the exact
      // authority reachable only when it can build a coherent immutable
      // review. Torn/corrupt or candidate-only authority remains unavailable.
      // No plugin-controlled runtime is assembled before review promotion.
      const auto review = permissions->prepare_review();
      return {.runtime = {}, .permission_disabled = review != nullptr};
    }
    auto &snapshot = *loaded.snapshot;
    const auto binding = snapshot.grants.binding;
    const auto live = snapshot.live;
    if (snapshot.record.plugin_id !=
        permissions->expected_plugin_.view())
      return {};
    if (loaded.grant_status ==
        host_session::GrantStatus::permission_disabled)
      return {.runtime = {}, .permission_disabled = true};
    if (loaded.grant_status != host_session::GrantStatus::activatable)
      return {};
    auto live_binding =
        permissions->prepare_live_activation(binding, live);
    if (!live_binding)
      return {};

    // Bind before construction so an already-stale active revision cannot
    // enter even the side-effect-free runtime assembly phase.
    PluginSessionCreateError create_error = PluginSessionCreateError::none;
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    auto supervisor = configuration.test_supervisor_factory
                          ? configuration.test_supervisor_factory()
                          : launcher::Supervisor::packaged();
#else
    auto supervisor = launcher::Supervisor::packaged();
#endif
    auto prepared = PluginSession::prepare(
        std::move(supervisor), std::move(snapshot), runtime_factory,
        create_error, configuration.session_limits, std::move(configuration.settings),
        std::move(configuration.presentation));
    if (!prepared)
      return {};
    prepared->permissions = std::move(permissions);
    prepared->live_binding = std::move(live_binding);
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    prepared->before_final_fence = configuration.test_before_final_fence;
    prepared->before_final_fence_context = configuration.test_before_final_fence_context;
#endif
    return {.runtime = std::move(prepared)};
  } catch (...) {
    return {};
  }
}

std::unique_ptr<PluginSession>
PluginSession::commit(
    std::unique_ptr<PreparedPluginSession> prepared,
    PluginRuntimeHooks &hooks, QObject &ui_owner) {
  if (!prepared || !prepared->permissions ||
      !prepared->live_binding || QThread::currentThread() != ui_owner.thread())
    return {};
  try {
    const auto expected_binding = prepared->grants.binding;
    const auto expected_live = prepared->live;
    auto root = std::unique_ptr<PluginSession>(new PluginSession(
        std::move(*prepared), &hooks, &hooks));
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
    if (prepared->before_final_fence)
      prepared->before_final_fence(root->permissions_->authority_for_test(),
                                  prepared->before_final_fence_context);
#endif
    if (!root->permissions_->commit_live_activation(
            std::move(*prepared->live_binding), expected_binding,
            expected_live))
      return {};
    root->start();
    root->session_committed_ = true;
    return root;
  } catch (...) {
    return {};
  }
}

std::optional<permissions::ActivationBinding>
PluginSession::session_binding() const {
  return state() == session::SessionState::running ? std::optional(binding()) : std::nullopt;
}

std::optional<PluginSession::DeclaredSurfaceSet>
PluginSession::declared_surfaces() const noexcept {
  try {
    if (state() != session::SessionState::running)
      return std::nullopt;
    return DeclaredSurfaceSet{
        .binding = binding(),
        .plugin_id = manifest_.id,
        .names = manifest_.surface_names,
        .canonical_surfaces = manifest_.canonical_surfaces,
    };
  } catch (...) {
    return std::nullopt;
  }
}

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
PluginRuntimePreparationResult
ReviewedSessionTestAccess::prepare_from_parts(
    int activation_root_fd, int revision_root_fd, int state_root_fd,
    host_session::UniqueFd authority_root,
    permissions::PluginId plugin, std::uint32_t trusted_uid,
    std::string activation_record,
    std::shared_ptr<const definitions::TrustedDefinitionRegistry> definitions,
    std::shared_ptr<const RuntimeServices> services, Limits runtime_limits,
    session::SessionLimits session_limits,
    std::function<launcher::Supervisor()> supervisor_factory,
    void (*before_final_fence)(host_session::AuthorityStore &, void *) noexcept,
    void *before_final_fence_context) {
  return PluginSession::prepare({
      .permissions = PluginPermissionAuthority::open(
          activation_root_fd, revision_root_fd, state_root_fd,
          std::move(authority_root), std::move(plugin), trusted_uid,
          std::move(definitions), std::move(services), activation_record),
      .settings = std::nullopt,
      .presentation = std::nullopt,
      .runtime_limits = runtime_limits,
      .session_limits = session_limits,
      .test_supervisor_factory = std::move(supervisor_factory),
      .test_before_final_fence = before_final_fence,
      .test_before_final_fence_context = before_final_fence_context,
  });
}

std::unique_ptr<PluginSession>
ReviewedSessionTestAccess::commit(
    std::unique_ptr<PreparedPluginSession> prepared,
    PluginRuntimeHooks &hooks, QObject &ui_owner) {
  return PluginSession::commit(std::move(prepared), hooks,
                                             ui_owner);
}

bool ReviewedSessionTestAccess::ui_affine(
    const PluginSession &root,
    const QObject &ui_owner) noexcept {
  return PluginSessionTestAccess::ui_affine(root, ui_owner.thread());
}

#endif

} // namespace omarchy::plugin_runtime::channel
