#pragma once

#include "test_assert.hpp"
#include "dynamic_activation.hpp"

#include <string>
#include <vector>

namespace omarchy::plugin_runtime::test_support {
namespace definitions = plugins::definitions;
namespace permissions = plugins::permissions;

inline permissions::Digest digest(char digit) {
  return permissions::Digest(std::string(64, digit));
}

inline definitions::CapabilityDefinition reviewed_service_definition(
    std::string_view name, std::string_view title, std::string_view risk_text,
    char adapter_digest) {
  return {.canonical_name = definitions::Name(name),
          .authority_identity = definitions::Name(std::string(name) + ".authority"),
          .enforcement_family = definitions::EnforcementFamily::network_fetch,
          .display_category_id = definitions::Name("developer.services"),
          .display_category_label = definitions::Label("Developer services"),
          .title = definitions::Label(title),
          .risk_text = definitions::Label(risk_text),
          .risk = definitions::RiskLevel::high,
          .revocation = definitions::RevocationPolicy::cancel_inflight,
          .adapter = {.adapter_class = definitions::Name(std::string(name) + ".adapter"),
                      .contract_digest = digest(adapter_digest), .abi_version = 1},
          .operations = {}};
}

inline plugins::manifest::ManifestV2 sixteen_operation_manifest(
    std::uint32_t generation, char digest_digit) {
  plugins::manifest::ManifestV2 manifest;
  manifest.id = "org.example.sixteen";
  plugins::manifest::CapabilityRequest request{
      .capability = "org.example.sixteen-operations",
      .reason = "exercise the complete operation mask",
      .canonical_scope = "{}",
      .definition_generation = generation,
      .definition_digest = std::string(64, digest_digit),
      .operations = {},
      .required = false};
  for (int index = 0; index < 16; ++index)
    request.operations.push_back(
        std::string(index < 10 ? "op-0" : "op-") + std::to_string(index));
  manifest.requests.push_back(std::move(request));
  return manifest;
}

inline std::vector<std::byte> ungestured_invocation(
    const definitions::CapabilityReference &reference, std::string_view operation,
    std::span<const std::byte> payload = {}) {
  const definitions::DynamicInvocation invocation{
      .definition = reference, .operation = definitions::Name(operation),
      .gesture = std::nullopt, .payload = payload};
  std::vector<std::byte> encoded(definitions::kMaximumDynamicEnvelopeBytes);
  std::size_t written = 0;
  OMARCHY_CHECK(definitions::encode_dynamic_invocation(invocation, encoded, written));
  encoded.resize(written);
  return encoded;
}

inline definitions::TrustedDefinitionRegistry packaged_registry() {
  definitions::TrustedDefinitionRegistry registry;
  for (const auto &definition : definitions::packaged_definitions())
    require(registry.install(definition, 1),
            "cannot install packaged test definition");
  return registry;
}

inline plugins::manifest::CapabilityRequest capability_request(
    const definitions::TrustedDefinitionRegistry &registry, std::string_view name,
    std::string scope, bool required = true,
    std::vector<std::string> operations = {}) {
  const auto resolved = registry.find(name);
  require(resolved.has_value(), "test capability has no trusted definition");
  if (operations.empty())
    for (const auto &operation : resolved->definition->operations.values())
      operations.emplace_back(operation.name.view());
  return {.capability = std::string(name), .reason = "Fixture capability",
          .canonical_scope = std::move(scope),
          .definition_generation = resolved->generation,
          .definition_digest = std::string(resolved->digest.view()),
          .operations = std::move(operations), .required = required};
}

inline definitions::DynamicRevisionGrant capability_grant(
    const definitions::TrustedDefinitionRegistry &registry,
    const plugins::manifest::CapabilityRequest &manifest,
    const permissions::ActivationBinding &binding,
    permissions::GrantState state = permissions::GrantState::granted,
    std::uint64_t epoch = 1) {
  const auto request = definitions::dynamic_request_from_manifest(manifest, registry);
  require(request.has_value(), "cannot resolve fixture request");
  return {.binding = binding, .request = *request,
          .grant = {.operations = request->operations,
                    .state = state, .epoch = epoch}};
}
} // namespace omarchy::plugin_runtime::test_support
