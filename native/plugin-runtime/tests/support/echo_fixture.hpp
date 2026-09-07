#pragma once

#include "capability_fixture.hpp"

namespace omarchy::plugin_runtime::test_support {

inline definitions::CapabilityDefinition echo_definition(
    std::string_view risk_text = "Uses a trusted echo service") {
  definitions::CapabilityDefinition value{
      .canonical_name = definitions::Name("service.echo"),
      .authority_identity = definitions::Name("service.echo.authority"),
      .enforcement_family = definitions::EnforcementFamily::network_fetch,
      .display_category_id = definitions::Name("developer.services"),
      .display_category_label = definitions::Label("Developer services"),
      .title = definitions::Label("Echo"),
      .risk_text = definitions::Label(risk_text),
      .risk = definitions::RiskLevel::moderate,
      .revocation = definitions::RevocationPolicy::deny_new,
      .adapter = {.adapter_class = definitions::Name("service.echo.adapter"),
                  .contract_digest = digest('d'), .abi_version = 1},
      .operations = {}};
  require(value.operations.insert({.name = definitions::Name("echo"),
                                    .label = definitions::Label("Echo payload")}),
          "cannot define echo fixture operation");
  return value;
}

inline plugins::manifest::ManifestV2 echo_manifest(
    const definitions::ResolvedDefinition &resolved, std::string_view plugin,
    std::string_view reason, bool required) {
  plugins::manifest::ManifestV2 value;
  value.id = std::string(plugin);
  value.requests.push_back(
      {.capability = std::string(resolved.definition->canonical_name.view()),
       .reason = std::string(reason), .canonical_scope = "exact",
       .definition_generation = resolved.generation,
       .definition_digest = std::string(resolved.digest.view()),
       .operations = {"echo"}, .required = required});
  return value;
}

inline definitions::DynamicRevisionGrant echo_grant(
    const definitions::ResolvedDefinition &resolved,
    const permissions::ActivationBinding &binding, bool required,
    permissions::GrantState state, std::uint64_t epoch) {
  definitions::DynamicRevisionGrant value{
      .binding = binding,
      .request = {.definition = {.canonical_name = resolved.definition->canonical_name,
                                 .definition_generation = resolved.generation,
                                 .definition_digest = resolved.digest},
                  .operations = {}, .scope = definitions::CanonicalScope("exact"),
                  .required = required},
      .grant = {}};
  require(value.request.operations.insert(definitions::Name("echo")),
          "cannot request echo fixture operation");
  value.grant = {.operations = value.request.operations,
                 .state = state, .epoch = epoch};
  return value;
}

} // namespace omarchy::plugin_runtime::test_support
