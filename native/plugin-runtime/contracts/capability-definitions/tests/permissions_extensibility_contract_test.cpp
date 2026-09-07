#include "../../../tests/support/test_assert.hpp"

#include "capability_definition.hpp"

#include <stdexcept>
#include <string>

namespace {
using namespace omarchy::plugins::definitions;
namespace permissions = omarchy::plugins::permissions;

using omarchy::plugin_runtime::test_support::require;

Digest digest(char value) { return Digest(std::string(64, value)); }

CapabilityDefinition bounded_harness() {
  CapabilityDefinition definition{
      .canonical_name = Name("bash.my-harness"),
      .authority_identity = Name("local.my-harness-v1"),
      .enforcement_family = EnforcementFamily::cli_harness,
      .display_category_id = Name("local.automation"),
      .display_category_label = Label("Local automation"),
      .title = Label("Use My Harness"),
      .risk_text = Label("Runs selected fake harness operations with bounded arguments"),
      .risk = RiskLevel::high,
      .revocation = RevocationPolicy::cancel_inflight,
      .adapter = {.adapter_class = Name("fake-bounded-harness"),
                  .contract_digest = digest('d'),
                  .abi_version = 1},
      .operations = {},
  };
  definition.operations.insert({.name = Name("status"),
                                .label = Label("Read harness status")});
  definition.operations.insert({.name = Name("drive"),
                                .label = Label("Drive the harness"),
                                .mutating = true,
                                .requires_fresh_gesture = true});
  return definition;
}
} // namespace

void permissions_extensibility_contract_tests() {
  TrustedDefinitionRegistry registry;

  // A plugin-authored name has no authority until a trusted administrator has
  // independently installed a definition and its reviewed adapter exists.
  OMARCHY_CHECK(!registry.find("bash.my-harness"));
  const auto unknown_manifest = omarchy::plugins::manifest::parse_manifest_v2(
      "{\"schemaVersion\":2,\"id\":\"org.example.proposal\","
      "\"name\":\"Proposal\",\"version\":\"1\",\"runtime\":{"
      "\"apiVersion\":1,\"qml\":\"Main.qml\"},\"surfaces\":{},"
      "\"permissions\":{\"required\":[],\"optional\":[{"
      "\"capability\":\"bash.my-harness\",\"definitionGeneration\":1,"
      "\"definitionDigest\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
      "\"operations\":[\"status\"],\"profile\":\"my-harness-v1\","
      "\"reason\":\"Show status when the administrator installs the integration\"}]}}"
  );
  OMARCHY_CHECK(!dynamic_request_from_manifest(unknown_manifest.requests.front(),
                                         registry));
  const auto definition = bounded_harness();
  OMARCHY_CHECK(registry.install(definition, 1));
  const auto installed = registry.find("bash.my-harness");
  OMARCHY_CHECK(installed.has_value());

  DynamicRequest optional{
      .definition = {.canonical_name = Name("bash.my-harness"),
                     .definition_generation = 1,
                     .definition_digest = installed->digest},
      .operations = {},
      .scope = CanonicalScope("profile=my-harness-v1"),
      .required = false,
  };
  optional.operations.insert(Name("status"));
  DynamicGrant grant{.operations = {},
                     .state = permissions::GrantState::denied,
                     .epoch = 1};
  grant.operations.insert(Name("status"));

  const auto decide = [&](const DynamicRequest &request,
                          const DynamicGrant &candidate,
                          std::string_view operation) {
    return authorize_dynamic_operation(registry, request, candidate, operation,
                                       definition.adapter, false)
        .decision;
  };

  // This is the same host-derived state QML uses to hide/disable an optional
  // feature. It is deliberately not an authorization token.
  OMARCHY_CHECK(decide(optional, grant, "status") ==
              DynamicDecision::denied);
  grant.state = permissions::GrantState::granted;
  ++grant.epoch;
  OMARCHY_CHECK(decide(optional, grant, "status") ==
              DynamicDecision::allowed);
  grant.state = permissions::GrantState::revoked;
  ++grant.epoch;
  OMARCHY_CHECK(decide(optional, grant, "status") ==
              DynamicDecision::revoked);

  auto operation_expansion = optional;
  operation_expansion.operations.insert(Name("drive"));
  grant.state = permissions::GrantState::granted;
  OMARCHY_CHECK(decide(operation_expansion, grant, "drive") ==
              DynamicDecision::operation_ungranted);

  auto required = optional;
  required.required = true;
  grant.state = permissions::GrantState::revoked;
  OMARCHY_CHECK(decide(required, grant, "status") ==
              DynamicDecision::revoked);

  // Definition upgrades are review boundaries too. A plugin cannot silently
  // follow generation 2 or a different adapter digest using its generation-1
  // manifest reference and grant.
  OMARCHY_CHECK(!registry.resolve({.canonical_name = Name("bash.my-harness"),
                             .definition_generation = 2,
                             .definition_digest = installed->digest}));
  auto substituted = definition.adapter;
  substituted.contract_digest = digest('x');
  grant.state = permissions::GrantState::granted;
  OMARCHY_CHECK(authorize_dynamic_operation(
              registry, optional, grant, "status", substituted, false)
              .decision == DynamicDecision::adapter_mismatch);
}
