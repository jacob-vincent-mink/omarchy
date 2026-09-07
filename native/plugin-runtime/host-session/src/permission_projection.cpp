#include "permission_projection.hpp"

#include "permission_contract.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ranges>

namespace omarchy::plugin_runtime::host_session {
namespace {

namespace definitions = plugins::definitions;
namespace manifest_contract = plugins::manifest;
namespace permissions = plugins::permissions;
namespace snapshot_wire = plugin::wire::permission_snapshot;

std::optional<snapshot_wire::GrantState>
wire_state(permissions::GrantState state) {
  switch (state) {
  case permissions::GrantState::granted:
    return snapshot_wire::GrantState::granted;
  case permissions::GrantState::denied:
    return snapshot_wire::GrantState::denied;
  case permissions::GrantState::revoked:
    return snapshot_wire::GrantState::revoked;
  }
  return std::nullopt;
}

bool same_dynamic_request(const manifest_contract::CapabilityRequest &manifest,
                          const definitions::DynamicRequest &request) {
  if (request.definition.canonical_name.view() != manifest.capability ||
      request.definition.definition_generation !=
          manifest.definition_generation ||
      request.definition.definition_digest.view() !=
          manifest.definition_digest ||
      request.scope.view() != manifest.canonical_scope ||
      request.required != manifest.required ||
      request.operations.size() != manifest.operations.size())
    return false;
  return std::ranges::equal(
      request.operations.values(), manifest.operations,
      [](const auto &left, const auto &right) { return left.view() == right; });
}

std::optional<snapshot_wire::PermissionSnapshot>
validated_projection(const manifest_contract::ManifestV2 &manifest,
                            const policy::GrantSnapshot &snapshot) noexcept {
  try {
    if (snapshot.binding.plugin.view() != manifest.id ||
        snapshot.binding.generation == 0 ||
        snapshot.binding.policy_fingerprint.view() !=
            manifest_contract::requested_capability_fingerprint(
                manifest.requests))
      return std::nullopt;

    if (manifest.requests.size() != snapshot.dynamic_grants.size())
      return std::nullopt;
    snapshot_wire::PermissionSnapshot result{
        .manifest_request_fingerprint =
            std::string(snapshot.binding.policy_fingerprint.view()),
        .permissions = {}};
    const auto ordered =
        manifest_contract::canonical_capability_requests(manifest.requests);
    result.permissions.reserve(ordered.size());
    for (const auto &request : ordered) {
      const auto revision = std::ranges::find_if(snapshot.dynamic_grants, [&](const auto &grant) {
        return grant.request.definition.canonical_name.view() == request.capability;
      });
      if (revision == snapshot.dynamic_grants.end() || revision->binding != snapshot.binding ||
          !same_dynamic_request(request, revision->request) ||
          revision->request.scope.view().empty() ||
          !definitions::valid_dynamic_grant_shape(*revision))
        return std::nullopt;
      const auto state = wire_state(revision->grant.state);
      std::uint16_t operation_mask = 0;
      if (revision->grant.state != permissions::GrantState::denied)
        for (std::size_t index = 0; index < request.operations.size(); ++index)
          if (revision->grant.operations.contains(definitions::Name(request.operations[index])))
            operation_mask |= static_cast<std::uint16_t>(1U << index);
      if (!state)
        return std::nullopt;
      result.permissions.push_back(
          {.state = *state, .operation_mask = operation_mask});
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

} // namespace

bool permission_snapshot_matches_manifest(
    const manifest_contract::ManifestV2 &manifest,
    const policy::GrantSnapshot &snapshot) noexcept {
  return validated_projection(manifest, snapshot).has_value();
}

std::optional<snapshot_wire::PermissionSnapshot>
project_permission_snapshot(const manifest_contract::ManifestV2 &manifest,
                            const policy::GrantSnapshot &snapshot) noexcept {
  if (std::ranges::any_of(snapshot.dynamic_grants, [](const auto &revision) {
        return revision.request.required &&
               revision.grant.state != permissions::GrantState::granted;
      }))
    return std::nullopt;
  return validated_projection(manifest, snapshot);
}

} // namespace omarchy::plugin_runtime::host_session
