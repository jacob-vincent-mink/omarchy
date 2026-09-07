#include "../../../tests/support/test_assert.hpp"

#include "capability_definition.hpp"

#include <array>
#include <stdexcept>
#include <string>

namespace {
using namespace omarchy::plugins::definitions;
namespace permissions = omarchy::plugins::permissions;

using omarchy::plugin_runtime::test_support::require;

Digest digest(char value) { return Digest(std::string(64, value)); }

CapabilityDefinition network_fetch() {
  CapabilityDefinition definition{
      .canonical_name = Name("network.fetch"),
      .authority_identity = Name("network.fetch.https-v1"),
      .enforcement_family = EnforcementFamily::network_fetch,
      .display_category_id = Name("developer.services"),
      .display_category_label = Label("Developer services"),
      .title = Label("Fetch data from selected HTTPS origins"),
      .risk_text = Label("Sends bounded requests to explicitly selected origins and methods"),
      .risk = RiskLevel::high,
      .revocation = RevocationPolicy::cancel_inflight,
      .adapter = {.adapter_class = Name("bounded-https-fetch"),
                  .contract_digest = digest('a'),
                  .abi_version = 1},
      .operations = {},
  };
  definition.operations.insert({.name = Name("notifications.list"),
                                .label = Label("List notifications")});
  definition.operations.insert({.name = Name("notifications.mark-read"),
                                .label = Label("Mark a notification read"),
                                .mutating = true});
  return definition;
}

CapabilityDefinition open_uri() {
  CapabilityDefinition definition{
      .canonical_name = Name("external.open-uri.https"),
      .authority_identity = Name("external.open-uri.https-v1"),
      .enforcement_family = EnforcementFamily::external_open_uri,
      .display_category_id = Name("desktop.actions"),
      .display_category_label = Label("Desktop actions"),
      .title = Label("Open selected HTTPS sites"),
      .risk_text = Label("Opens a user-visible HTTPS address after one fresh gesture"),
      .risk = RiskLevel::moderate,
      .revocation = RevocationPolicy::deny_new,
      .adapter = {.adapter_class = Name("desktop-open-uri"),
                  .contract_digest = digest('b'),
                  .abi_version = 1},
      .operations = {},
  };
  definition.operations.insert({.name = Name("open"),
                                .label = Label("Open link"),
                                .mutating = true,
                                .requires_fresh_gesture = true});
  return definition;
}

} // namespace

void capability_definition_loader_tests();
void permissions_extensibility_contract_tests();

int main() {
  for (const std::size_t size : {0, 63, 64, 65, 129})
    OMARCHY_CHECK(valid_digest(std::string_view(std::string(size, '0'))) == (size == 64));
  for (std::size_t position = 0; position < 64; ++position) {
    std::string candidate(64, '0');
    for (unsigned byte = 0; byte < 256; ++byte) {
      candidate[position] = static_cast<char>(byte);
      const bool hexadecimal = std::string_view("0123456789abcdef").find(candidate[position]) !=
                               std::string_view::npos;
      OMARCHY_CHECK(valid_digest(std::string_view(candidate)) == hexadecimal);
    }
  }
  using D = DynamicDecision;
  using A = permissions::GrantDecisionCode;
  for (const auto [decision, expected] : std::array{
           std::pair{D::allowed, A::allowed},
           std::pair{D::revoked, A::revoked},
           std::pair{D::operation_undeclared, A::capability_undeclared},
           std::pair{D::operation_ungranted, A::ungranted},
           std::pair{D::gesture_missing, A::gesture_missing},
           std::pair{D::denied, A::explicitly_denied},
           std::pair{D::unknown_definition, A::ungranted},
           std::pair{D::stale_definition, A::ungranted},
           std::pair{D::adapter_mismatch, A::ungranted},
           std::pair{static_cast<D>(255), A::ungranted}})
    OMARCHY_CHECK(audit_decision_code(decision) == expected);
  TrustedDefinitionRegistry registry;
  const auto fetch_definition = network_fetch();
  OMARCHY_CHECK(registry.install(fetch_definition, 4));
  OMARCHY_CHECK(registry.install(open_uri(), 2));
  OMARCHY_CHECK(registry.size() == 2);

  const auto installed = registry.find("network.fetch");
  OMARCHY_CHECK(installed && installed->generation == 4 &&
              installed->definition->adapter.adapter_class.view() == "bounded-https-fetch");
  OMARCHY_CHECK(registry.resolve({.canonical_name = Name("network.fetch"),
                            .definition_generation = 4,
                            .definition_digest = installed->digest}).has_value());
  OMARCHY_CHECK(!registry.resolve({.canonical_name = Name("network.fetch"),
                             .definition_generation = 3,
                             .definition_digest = installed->digest}).has_value());
  OMARCHY_CHECK(!registry.resolve({.canonical_name = Name("network.fetch"),
                             .definition_generation = 4,
                             .definition_digest = digest('f')}).has_value());
  OMARCHY_CHECK(!registry.find("plugin.proposed-capability"));

  DynamicRequest request{
      .definition = {.canonical_name = Name("network.fetch"),
                     .definition_generation = 4,
                     .definition_digest = installed->digest},
      .operations = {},
      .scope = CanonicalScope("wide"),
      .required = true,
  };
  request.operations.insert(Name("notifications.list"));
  DynamicGrant grant{.operations = {},
                     .state = permissions::GrantState::granted,
                     .epoch = 9};
  grant.operations.insert(Name("notifications.list"));
  OMARCHY_CHECK(authorize_dynamic_operation(
              registry, request, grant, "notifications.list",
              installed->definition->adapter, false).allowed());
  OMARCHY_CHECK(authorize_dynamic_operation(
              registry, request, grant, "notifications.mark-read",
              installed->definition->adapter, false).decision ==
              DynamicDecision::operation_undeclared);
  auto revoked = grant;
  revoked.state = permissions::GrantState::revoked;
  OMARCHY_CHECK(authorize_dynamic_operation(
              registry, request, revoked, "notifications.list",
              installed->definition->adapter, false).decision ==
              DynamicDecision::revoked);
  auto substituted = installed->definition->adapter;
  substituted.contract_digest = digest('e');
  OMARCHY_CHECK(authorize_dynamic_operation(
              registry, request, grant, "notifications.list", substituted, false).decision ==
              DynamicDecision::adapter_mismatch);

  const std::string dynamic_manifest =
      "{\"schemaVersion\":2,\"id\":\"org.example.dynamic\","
      "\"name\":\"Dynamic\",\"version\":\"1\","
      "\"runtime\":{\"apiVersion\":1,\"qml\":\"Main.qml\"},"
      "\"surfaces\":{},\"permissions\":{\"required\":[{"
      "\"capability\":\"network.fetch\",\"definitionGeneration\":4,"
      "\"definitionDigest\":\"" + std::string(installed->digest.view()) +
      "\",\"operations\":[\"notifications.list\"],"
      "\"origins\":[\"https://status.example.com\"],\"reason\":\"status\"}],"
      "\"optional\":[]}}";
  const auto parsed_manifest =
      omarchy::plugins::manifest::parse_manifest_v2(dynamic_manifest);
  const auto parsed_request =
      dynamic_request_from_manifest(parsed_manifest.requests.front(), registry);
  OMARCHY_CHECK(parsed_request && parsed_request->definition.definition_generation == 4 &&
              parsed_request->operations.contains(Name("notifications.list")));
  auto untrusted_operation = parsed_manifest.requests.front();
  untrusted_operation.operations.push_back("admin");
  OMARCHY_CHECK(!dynamic_request_from_manifest(untrusted_operation, registry));
  auto stale_manifest_reference = parsed_manifest.requests.front();
  ++stale_manifest_reference.definition_generation;
  OMARCHY_CHECK(!dynamic_request_from_manifest(stale_manifest_reference, registry));

  auto alias = fetch_definition;
  alias.canonical_name = Name("internet.read-safe");
  OMARCHY_CHECK(!registry.install(alias, 1));
  alias.authority_identity = Name("internet.read-safe-v1");
  OMARCHY_CHECK(!registry.install(alias, 1));

  auto dishonest_open = open_uri();
  dishonest_open.canonical_name = Name("external.open-uri.no-gesture");
  dishonest_open.authority_identity = Name("external.open-uri.no-gesture-v1");
  auto operation = *dishonest_open.operations.values().begin();
  dishonest_open.operations = {};
  operation.requires_fresh_gesture = false;
  dishonest_open.operations.insert(operation);
  OMARCHY_CHECK(!valid_definition(dishonest_open));

  auto device_observe = fetch_definition;
  device_observe.enforcement_family = EnforcementFamily::device_observe;
  OMARCHY_CHECK(valid_definition(device_observe));

  auto media = fetch_definition;
  media.enforcement_family = EnforcementFamily::media_play_stream;
  OMARCHY_CHECK(valid_definition(media));

  const auto contract = semantic_contract_digest("request=a;response=b");
  OMARCHY_CHECK(contract == semantic_contract_digest("request=a;response=b") &&
              contract != semantic_contract_digest("request=a;response=c"));
  OMARCHY_CHECK(canonical_identifier(std::string(128, 'a')) &&
              !canonical_identifier(std::string(129, 'a')));

  capability_definition_loader_tests();
  TrustedDefinitionRegistry command_registry;
  for (const auto &definition : packaged_definitions())
    OMARCHY_CHECK(command_registry.install(definition, 1));
  const auto command = command_registry.find("bash.execute");
  OMARCHY_CHECK(command && command_registry.resolve({Name("bash.execute"), 1, command->digest}) &&
               !command_registry.resolve({Name("bash.execute"), 1,
                   Digest("dabe8499d0a7576316abac765abfcdf03a0a6bb183833fa1b30ce1f420da2150")}));
  permissions_extensibility_contract_tests();
  return 0;
}
