#pragma once

#include "capability_fixture.hpp"
#include "activation_snapshot.hpp"

namespace omarchy::plugin_runtime::test_support {
inline plugins::manifest::ManifestV2 manifest_for(const policy::GrantSnapshot &snapshot) {
  plugins::manifest::ManifestV2 result;
  result.id = snapshot.binding.plugin.view();
  for (const auto &grant : snapshot.dynamic_grants) {
    const auto &request = grant.request;
    std::vector<std::string> operations;
    for (const auto &operation : request.operations.values())
      operations.emplace_back(operation.view());
    result.requests.push_back({
        .capability = std::string(request.definition.canonical_name.view()),
        .reason = "Fixture capability", .canonical_scope = std::string(request.scope.view()),
        .definition_generation = request.definition.definition_generation,
        .definition_digest = std::string(request.definition.definition_digest.view()),
        .operations = std::move(operations), .required = request.required});
  }
  return result;
}

inline policy::GrantSnapshot permission_snapshot(
    const definitions::TrustedDefinitionRegistry &registry,
    const plugins::manifest::ManifestV2 &manifest, std::string_view revision,
    std::uint64_t generation = 1,
    permissions::GrantState state = permissions::GrantState::granted) {
  policy::GrantSnapshot result;
  result.binding = {
      .plugin = permissions::PluginId(manifest.id),
      .revision = permissions::Digest(revision),
      .policy_fingerprint = permissions::Digest(
          plugins::manifest::requested_capability_fingerprint(manifest.requests)),
      .generation = generation};
  for (const auto &request : plugins::manifest::canonical_capability_requests(manifest.requests))
    result.dynamic_grants.push_back(
        capability_grant(registry, request, result.binding, state, generation));
  return result;
}
} // namespace omarchy::plugin_runtime::test_support
