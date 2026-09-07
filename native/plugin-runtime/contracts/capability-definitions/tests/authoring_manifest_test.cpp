#include "authoring_manifest.hpp"
#include "../../../tests/support/capability_fixture.hpp"

#include <nlohmann/json.hpp>
#include <array>
#include <iostream>
#include <limits>

namespace {
namespace definitions = omarchy::plugins::definitions;
namespace manifest = omarchy::plugins::manifest;
namespace permissions = omarchy::plugins::permissions;
using namespace omarchy::plugin_runtime::test_support;
using Json = nlohmann::json;

constexpr std::string_view author_document = R"({
  "authoringVersion":1,"id":"org.example.authoring","name":"Authoring",
  "version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},
  "permissions":{
    "required":[{"capability":"network.fetch",
      "definitionVersions":{"minimum":1,"maximum":2},"operations":["fetch"],
      "origins":["https://example.com"],"methods":["GET"],"reason":"Fetch status"}],
    "optional":[{"capability":"storage.private",
      "definitionVersions":{"minimum":1,"maximum":1},"operations":["write","read"],
      "quotaBytes":4096,"itemBytes":1024,"reason":"Remember preferences"}]
  }
})";

void resolution_and_identity() {
  const auto registry = packaged_registry();
  const auto resolved = definitions::resolve_authoring_manifest_v1(author_document, registry);
  OMARCHY_CHECK(resolved == manifest::parse_manifest_v2(resolved.canonical_json));
  OMARCHY_CHECK(throws_exception<std::runtime_error>([&] {
    (void)manifest::parse_manifest_v2(author_document);
  }));
  OMARCHY_CHECK(resolved.id == "org.example.authoring" && resolved.requests.size() == 2);
  const auto &network = resolved.requests[0];
  const auto &storage = resolved.requests[1];
  OMARCHY_CHECK(network.required && !storage.required);
  OMARCHY_CHECK(network.capability == "network.fetch" &&
              network.reason == "Fetch status" &&
              network.operations == std::vector<std::string>{"fetch"} &&
              network.canonical_scope == R"({"methods":["GET"],"origins":["https://example.com"]})");
  OMARCHY_CHECK(storage.capability == "storage.private" &&
              storage.operations == std::vector<std::string>({"read", "write"}) &&
              storage.canonical_scope == R"({"itemBytes":1024,"quotaBytes":4096})");
  for (const auto &request : resolved.requests) {
    const auto installed = registry.find(request.capability);
    OMARCHY_CHECK(installed && request.definition_generation == 1 &&
                request.definition_digest == installed->digest.view());
  }
  auto expected = Json::parse(author_document);
  expected.erase("authoringVersion");
  expected["schemaVersion"] = 2;
  for (const auto *kind : {"required", "optional"}) {
    auto &request = expected["permissions"][kind][0];
    request.erase("definitionVersions");
    request["definitionGeneration"] = 1;
    request["definitionDigest"] = registry.find(request["capability"].get<std::string>())->digest.view();
  }
  OMARCHY_CHECK(resolved == manifest::parse_manifest_v2(expected.dump()));
  OMARCHY_CHECK(definitions::resolve_authoring_manifest_v1(
                  Json::parse(author_document).dump(), registry) == resolved);
  OMARCHY_CHECK(throws_exception<std::runtime_error>([&] {
    (void)definitions::resolve_authoring_manifest_v1(resolved.canonical_json, registry);
  }));
  for (const std::uint32_t maximum : {1U, UINT32_MAX}) {
    auto bounded = Json::parse(author_document);
    bounded["permissions"]["required"][0]["definitionVersions"]["maximum"] = maximum;
    OMARCHY_CHECK(definitions::resolve_authoring_manifest_v1(bounded.dump(), registry) == resolved);
  }
  auto no_permissions = Json::parse(author_document);
  no_permissions["permissions"] = {{"required", Json::array()}, {"optional", Json::array()}};
  OMARCHY_CHECK(definitions::resolve_authoring_manifest_v1(no_permissions.dump(), {}).requests.empty());

  // Even an explicitly compatible update produces a different reviewed
  // identity. Old pins and a previously granted operation cannot rebind.
  for (const std::uint32_t replacement_generation : {1U, 2U}) {
    definitions::TrustedDefinitionRegistry replacement;
    for (auto definition : definitions::packaged_definitions()) {
      const bool network_definition = definition.canonical_name.view() == "network.fetch";
      if (network_definition)
        definition.title = definitions::Label("Changed reviewed network definition");
      OMARCHY_CHECK(replacement.install(definition,
                    network_definition ? replacement_generation : 1));
    }
    const auto updated = definitions::resolve_authoring_manifest_v1(author_document, replacement);
    OMARCHY_CHECK(updated.requests[0].definition_generation == replacement_generation &&
                updated.requests[0].definition_digest != network.definition_digest &&
                manifest::requested_capability_fingerprint(updated.requests) !=
                    manifest::requested_capability_fingerprint(resolved.requests));
    const auto old = definitions::dynamic_request_from_manifest(network, registry);
    OMARCHY_CHECK(old && !replacement.resolve(old->definition));
    const definitions::DynamicGrant grant{.operations = old->operations,
        .state = permissions::GrantState::granted, .epoch = 1};
    OMARCHY_CHECK(!definitions::authorize_dynamic_operation(replacement, *old, grant,
        "fetch", replacement.find("network.fetch")->definition->adapter, true).allowed());
  }
}

void malformed_and_incompatible() {
  const auto registry = packaged_registry();
  const auto reject = [&](auto mutation) {
    auto document = Json::parse(author_document);
    mutation(document);
    OMARCHY_CHECK(throws_exception<std::runtime_error>([&] {
      (void)definitions::resolve_authoring_manifest_v1(document.dump(), registry);
    }));
  };
  reject([](auto &d) { d["schemaVersion"] = 2; });
  reject([](auto &d) { d.erase("authoringVersion"); });
  reject([](auto &d) { d["authoringVersion"] = 2; });
  reject([](auto &d) { d["authoringVersion"] = true; });
  reject([](auto &d) { d["unexpected"] = 1; });
  reject([](auto &d) { d["permissions"].erase("optional"); });
  reject([](auto &d) { d["permissions"]["required"] = Json::object(); });
  reject([](auto &d) { d["permissions"]["required"][0]["definitionGeneration"] = 1; });
  reject([](auto &d) { d["permissions"]["required"][0]["definitionDigest"] = std::string(64, '0'); });
  reject([](auto &d) { d["permissions"]["required"][0].erase("definitionVersions"); });
  reject([](auto &d) { d["permissions"]["required"][0]["definitionVersions"]["extra"] = 1; });
  reject([](auto &d) { d["permissions"]["required"][0]["definitionVersions"].erase("maximum"); });
  reject([](auto &d) { d["permissions"]["required"][0]["definitionVersions"] = 1; });
  for (const auto *kind : {"required", "optional"}) {
    reject([&](auto &d) { d["permissions"][kind][0]["capability"] = "unknown.definition"; });
    reject([&](auto &d) { d["permissions"][kind][0]["operations"] = Json::array({"undeclared"}); });
    reject([&](auto &d) { d["permissions"][kind][0]["operations"] = Json::array(); });
    reject([&](auto &d) { d["permissions"][kind][0]["operations"] = Json::array({"read", "read"}); });
    reject([&](auto &d) { d["permissions"][kind][0]["definitionVersions"] = {{"minimum", 2}, {"maximum", 3}}; });
    reject([&](auto &d) { d["permissions"][kind][0]["definitionVersions"] = {{"minimum", 2}, {"maximum", 1}}; });
  }
  const std::array<Json, 9> bad_versions{0, -1, 4294967296ULL,
      std::numeric_limits<std::uint64_t>::max(), true, nullptr, 1.5, "1", Json::object()};
  for (const auto &bad : bad_versions)
    for (const auto *bound : {"minimum", "maximum"})
      reject([&](auto &d) { d["permissions"]["required"][0]["definitionVersions"][bound] = bad; });
  reject([](auto &d) { d["permissions"]["optional"].push_back(d["permissions"]["required"][0]); });
  reject([](auto &d) { d["permissions"]["required"][0]["reason"] = ""; });
  reject([](auto &d) { d["runtime"]["qml"] = "../outside.qml"; });
  for (const std::string &malformed : {
      std::string("{\"authoringVersion\":1,\"authoringVersion\":1}"),
      std::string("{\"authoringVersion\":1,\"\\u0061uthoringVersion\":1}"),
      std::string("\xef\xbb\xbf") + std::string(author_document),
      std::string(1024 * 1024 + 1, ' '), std::string("[]"), std::string("null")})
    OMARCHY_CHECK(throws_exception<std::runtime_error>([&] {
      (void)definitions::resolve_authoring_manifest_v1(malformed, registry);
    }));
  OMARCHY_CHECK(throws_exception<std::runtime_error>([&] {
    (void)definitions::resolve_authoring_manifest_v1(author_document, {});
  }));
}
} // namespace

int main() {
  return test_main([] {
    resolution_and_identity();
    malformed_and_incompatible();
    std::cout << "authoring resolution, exact identity and rejection tests passed\n";
    return 0;
  });
}
