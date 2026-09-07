#include "../../../tests/support/test_assert.hpp"

#include "manifest_contract.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using omarchy::plugin_runtime::test_support::throws_exception;

using omarchy::plugin_runtime::test_support::require;

void expect_rejected(const std::function<void()> &operation,
                     std::string_view message) {
  require(throws_exception<std::runtime_error>(operation), message);
}

void expect_manifest_rejected(std::string_view input, std::string_view message) {
  expect_rejected([&] {
    (void)omarchy::plugins::manifest::parse_manifest_v2(input);
  }, message);
}

std::string read(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  OMARCHY_CHECK(input.good());
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void parser_contract(const std::filesystem::path &fixtures) {
  OMARCHY_CHECK(omarchy::plugins::manifest::canonical_settings_entry({
      {"a", INT64_MIN}, {"b", true},
      {"text", std::string("\"\\/\b\f\n\r\t\0", 9) + "é🙂"}, {"z", INT64_MAX}}) ==
      R"({"a":-9223372036854775808,"b":true,"text":"\"\\/\b\f\n\r\t\u0000é🙂","z":9223372036854775807})");
  const auto valid_root = fixtures / "valid-minimal";
  const auto manifest = omarchy::plugins::manifest::parse_manifest_v2(
      read(valid_root / "manifest.json"));
  OMARCHY_CHECK(manifest.id == "org.example.status");
  OMARCHY_CHECK(manifest.runtime.api_version == 1 &&
              manifest.runtime.qml == "ui/Status.qml");
  OMARCHY_CHECK(manifest.surface_names == std::vector<std::string>{"barWidget"});
  OMARCHY_CHECK(manifest.requests.size() == 2 && manifest.requests[0].required &&
              !manifest.requests[1].required);
  OMARCHY_CHECK(manifest.requests[0].canonical_scope == "{\"itemBytes\":4096,\"quotaBytes\":1048576}");

  // The standard parser owns syntax; our adapter must retain the strict
  // manifest dialect, Unicode checks, and resource bounds.
  const auto valid_json = read(valid_root / "manifest.json");
  const auto with_dependencies = [&](std::string_view dependencies) {
    return omarchy::plugins::manifest::parse_manifest_v2(
        "{\"dependencies\":" + std::string(dependencies) + "," + valid_json.substr(1));
  };
  const auto dependent = with_dependencies(R"({"aur":["example-git","lib32-example+1"]})");
  OMARCHY_CHECK(dependent.aur_dependencies ==
               (std::vector<std::string>{"example-git", "lib32-example+1"}));
  OMARCHY_CHECK(dependent.canonical_json != manifest.canonical_json);
  OMARCHY_CHECK(manifest.aur_dependencies.empty());
  OMARCHY_CHECK(with_dependencies(R"({"aur":[]})").aur_dependencies.empty());
  for (const auto invalid : {R"(null)", R"([])", R"({})", R"({"aur":"pkg"})",
       R"({"aur":[1]})", R"({"aur":[""]})", R"({"aur":["--needed"]})",
       R"({"aur":["aur/pkg"]})", R"({"aur":["pkg>=1"]})", R"({"aur":["pkg\n"]})",
       R"({"aur":["pkg","pkg"]})", R"({"aur":[],"install":"command"})"}) {
    expect_rejected([&] { (void)with_dependencies(invalid); }, "invalid dependency accepted");
  }
  expect_rejected([&] { (void)with_dependencies("{\"aur\":[\"" + std::string(129, 'a') + "\"]}"); },
                  "unbounded dependency name accepted");
  std::string many_dependencies = "{\"aur\":[";
  for (int index = 0; index < 33; ++index)
    many_dependencies += (index ? "," : "") + std::string("\"pkg") + std::to_string(index) + "\"";
  many_dependencies += "]}";
  expect_rejected([&] { (void)with_dependencies(many_dependencies); }, "too many dependencies accepted");
  for (const auto &invalid : {
           std::string("\xef\xbb\xbf") + valid_json,
           valid_json + "true",
           std::string(R"({"schemaVersion":2.0})"),
           std::string(R"({"schemaVersion":2e0})"),
           std::string(R"({"schemaVersion":18446744073709551616})"),
           std::string(R"({"name":"\ud800"})"),
           std::string("{\"name\":\"") + char(0xff) + "\"}",
           std::string(34, '[') + std::string(34, ']'),
           std::string("{\"name\":\"") + std::string(16385, 'x') + "\"}",
           std::string(1024 * 1024 + 1, ' ')}) {
    expect_manifest_rejected(invalid,
        "standard JSON adapter accepted invalid or unbounded input");
  }

  const auto product_settings =
      omarchy::plugins::manifest::parse_manifest_v2(
          R"({"schemaVersion":2,"id":"robzolkos.github","name":"GitHub","version":"0.4.0","author":"Rob Zolkos","license":"MIT","homepage":"https://example.test/plugin","repository":"https://example.test/repository","keywords":["github","inbox"],"runtime":{"apiVersion":1,"qml":"Panel.qml"},"surfaces":{},"settings":{"defaults":{"enabled":false,"mode":"Owned","refreshIntervalSec":900},"schema":[{"key":"refreshIntervalSec","type":"integer","label":"Refresh interval","min":60,"max":3600,"step":60,"defaultValue":900},{"key":"mode","type":"enum","label":"Scope","options":["Owned","All"],"defaultValue":"Owned"},{"key":"enabled","type":"boolean","label":"Enabled","defaultValue":false}]},"permissions":{"required":[],"optional":[]}})");
  OMARCHY_CHECK(product_settings.author == "Rob Zolkos" &&
              product_settings.license == "MIT" &&
              product_settings.homepage == "https://example.test/plugin" &&
              product_settings.repository ==
                  "https://example.test/repository" &&
              product_settings.keywords ==
                  std::vector<std::string>{"github", "inbox"});
  OMARCHY_CHECK(product_settings.settings.schema.size() == 3 &&
              product_settings.settings.canonical_defaults ==
                  R"({"enabled":false,"mode":"Owned","refreshIntervalSec":900})" &&
              omarchy::plugins::manifest::validate_settings_entry(
                  product_settings, product_settings.settings.defaults));
  const auto check_setting = [&](const char *key,
      omarchy::plugins::manifest::SettingValue value, bool expected) {
    auto settings = product_settings.settings.defaults;
    settings[key] = std::move(value);
    OMARCHY_CHECK(omarchy::plugins::manifest::validate_settings_entry(
        product_settings, settings) == expected);
  };
  check_setting("refreshIntervalSec", std::int64_t{30}, false);
  check_setting("refreshIntervalSec", std::int64_t{901}, true);
  check_setting("mode", std::string("Other"), false);
  check_setting("enabled", std::string("false"), false);
  check_setting("unknown", true, false);
  auto invalid_settings = product_settings.settings.defaults;
  invalid_settings.erase("enabled");
  OMARCHY_CHECK(!omarchy::plugins::manifest::validate_settings_entry(
              product_settings, invalid_settings));

  const auto off_step_default =
      omarchy::plugins::manifest::parse_manifest_v2(
          R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"settings":{"defaults":{"value":2},"schema":[{"key":"value","type":"integer","label":"Value","min":1,"max":5,"step":2,"defaultValue":2}]},"permissions":{"required":[],"optional":[]}})");
  OMARCHY_CHECK(off_step_default.settings.defaults.at("value") ==
              omarchy::plugins::manifest::SettingValue(std::int64_t{2}));

  for (const auto malformed : {
           R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"settings":{"defaults":{"mode":"Unknown"},"schema":[{"key":"mode","type":"enum","label":"Mode","options":["Known"],"defaultValue":"Unknown"}]},"permissions":{"required":[],"optional":[]}})",
           R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"settings":{"defaults":{"enabled":false},"schema":[{"key":"enabled","type":"boolean","label":"Enabled","defaultValue":false,"typo":true}]},"permissions":{"required":[],"optional":[]}})"}) {
    expect_manifest_rejected(malformed,
        "malformed settings schema was accepted");
  }

  const auto dynamic = omarchy::plugins::manifest::parse_manifest_v2(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"permissions":{"required":[{"capability":"local.status","definitionGeneration":7,"definitionDigest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","operations":["status.read"],"resource":4,"reason":"status"}],"optional":[]}})");
  OMARCHY_CHECK(dynamic.requests.size() == 1 &&
              dynamic.requests[0].definition_generation == 7 &&
              dynamic.requests[0].definition_digest == std::string(64, 'a') &&
              dynamic.requests[0].operations ==
                  std::vector<std::string>{"status.read"} &&
              dynamic.requests[0].canonical_scope == "{\"resource\":4}");

  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"permissions":{"required":[{"capability":"service.fake-status","resourceIds":[1],"operations":["list","acknowledge"],"reason":"status"}],"optional":[]}})",
      "unpinned capability acquired manifest authority");

  const auto multi_surface = omarchy::plugins::manifest::parse_manifest_v2(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml","surfaceQml":{"atlas":"Atlas.qml","barWidget":"BarWidget.qml"}},"surfaces":{"atlas":{},"barWidget":{}},"permissions":{"required":[],"optional":[]}})");
  OMARCHY_CHECK(multi_surface.runtime.surface_qml.size() == 2 &&
              multi_surface.surface_names ==
                  std::vector<std::string>{"atlas", "barWidget"} &&
              multi_surface.runtime.surface_qml[0].surface == "atlas" &&
              multi_surface.runtime.surface_qml[0].qml == "Atlas.qml" &&
              multi_surface.runtime.surface_qml[1].surface == "barWidget" &&
              multi_surface.runtime.surface_qml[1].qml == "BarWidget.qml");
  const std::string maximum_surface_name(64, 'X');
  const auto maximum_surface =
      omarchy::plugins::manifest::parse_manifest_v2(
          std::string(R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{")") +
          maximum_surface_name +
          R"(":{}},"permissions":{"required":[],"optional":[]}})");
  OMARCHY_CHECK(maximum_surface.surface_names ==
              std::vector<std::string>{maximum_surface_name});
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml","surfaceQml":{"missing":"Other.qml"}},"surfaces":{"atlas":{}},"permissions":{"required":[],"optional":[]}})",
      "QML entry for an undeclared surface was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml","surfaceQml":{"atlas":"Atlas.qml"}},"surfaces":{"atlas":{},"barWidget":{}},"permissions":{"required":[],"optional":[]}})",
      "partial per-surface QML mapping was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{"Panel.Widget":{}},"permissions":{"required":[],"optional":[]}})",
      "dotted surface name outside the wire contract was accepted");
  expect_rejected(
      [] {
        const std::string name(65, 'X');
        (void)omarchy::plugins::manifest::parse_manifest_v2(
            std::string(R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{")") +
            name +
            R"(":{}},"permissions":{"required":[],"optional":[]}})");
      },
      "65-byte surface name outside the wire contract was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{"bad\u0000name":{}},"permissions":{"required":[],"optional":[]}})",
      "NUL surface name outside the wire contract was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{"a":{},"b":{},"c":{},"d":{},"e":{},"f":{},"g":{},"h":{},"i":{}},"permissions":{"required":[],"optional":[]}})",
      "more than eight declared surfaces were accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml","surfaceQml":{"atlas":"../Other.qml"}},"surfaces":{"atlas":{}},"permissions":{"required":[],"optional":[]}})",
      "escaping per-surface QML entry was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml","sidecars":[{"name":"escape","command":["../host-tool"]}]},"surfaces":{},"permissions":{"required":[],"optional":[]}})",
      "removed sidecar field was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml","worker":["bin/one"]},"surfaces":{},"permissions":{"required":[],"optional":[]}})",
      "removed custom worker field was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"permissions":{"required":[{"capability":"local.status","definitionGeneration":7,"operations":["status.read"],"reason":"status"}],"optional":[]}})",
      "incomplete dynamic definition reference was accepted");

  expect_rejected(
      [&] {
        (void)omarchy::plugins::manifest::parse_manifest_v2(
            read(fixtures / "invalid-duplicate/manifest.json"));
      },
      "duplicate manifest key was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","i\u0064":"a.c","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"permissions":{"required":[],"optional":[]}})",
      "escaped duplicate manifest key was accepted");
  expect_rejected(
      [&] {
        (void)omarchy::plugins::manifest::parse_manifest_v2(
            read(fixtures / "invalid-entrypoint/manifest.json"));
      },
      "escaping entry point was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"permissions":{"required":[],"optional":[]},"typo":true})",
      "unknown manifest field was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{"scale":1.5},"permissions":{"required":[],"optional":[]}})",
      "non-integer scope number was accepted");
  expect_manifest_rejected(
      R"({"schemaVersion":2,"id":"A.B","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"permissions":{"required":[],"optional":[]}})",
      "noncanonical plugin id was accepted");
}

void digest_contract(const std::filesystem::path &fixtures) {
  using omarchy::plugins::manifest::identify_tree_contents;
  using omarchy::plugins::manifest::parse_manifest_v2;
  using omarchy::plugins::manifest::sha256_hex;

  const auto make_contents = [](std::string manifest_bytes,
                                bool qml_executable = false) {
    omarchy::plugins::manifest::TreeContents contents;
    contents.add({.relative = "manifest.json",
                  .bytes = std::move(manifest_bytes)});
    contents.add({.relative = "ui/Status.qml",
                  .bytes = "import QtQuick\n\nItem { }\n",
                  .executable = qml_executable});
    return contents;
  };

  OMARCHY_CHECK(
      sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  OMARCHY_CHECK(
      sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  OMARCHY_CHECK(
      sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  const auto root = fixtures / "valid-minimal";
  const auto bytes = read(root / "manifest.json");
  const auto manifest = parse_manifest_v2(bytes);
  const auto identity = identify_tree_contents(make_contents(bytes), manifest);
  require(identity.tree_sha256 == TREE_SHA256_GOLDEN,
          "tree SHA-256 golden mismatch: " + identity.tree_sha256);
  require(identity.manifest_sha256 == MANIFEST_SHA256_GOLDEN,
          "manifest SHA-256 golden mismatch: " + identity.manifest_sha256);
  require(identity.request_sha256 == REQUEST_SHA256_GOLDEN,
          "request SHA-256 golden mismatch: " + identity.request_sha256);

  const auto reordered = parse_manifest_v2(
      R"({"permissions":{"optional":[{"reason":"different words","categories":["timer"],"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"]}],"required":[{"reason":"also different","quotaBytes":1048576,"capability":"storage.private","definitionGeneration":1,"definitionDigest":"8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66","operations":["read","remove","write"],"itemBytes":4096}]},"surfaces":{"barWidget":{"defaultSection":"right","role":"bar-embedded"}},"runtime":{"qml":"ui/Status.qml","apiVersion":1},"version":"2.0.0","name":"Example Status","id":"org.example.status","schemaVersion":2})");
  expect_rejected(
      [&] { (void)identify_tree_contents(make_contents(bytes), reordered); },
      "stale manifest model was accepted for a different tree manifest");
  OMARCHY_CHECK(omarchy::plugins::manifest::requested_capability_fingerprint(
              reordered.requests) == identity.request_sha256);
  const auto expanded = parse_manifest_v2(
      R"({"schemaVersion":2,"id":"org.example.status","name":"Example Status","version":"2.0.0","runtime":{"apiVersion":1,"qml":"ui/Status.qml"},"surfaces":{},"permissions":{"required":[{"capability":"storage.private","definitionGeneration":1,"definitionDigest":"8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66","operations":["read","remove","write"],"itemBytes":4096,"quotaBytes":2097152,"reason":"Save"}],"optional":[{"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"],"categories":["timer"],"reason":"Notify"}]}})");
  OMARCHY_CHECK(omarchy::plugins::manifest::requested_capability_fingerprint(
              expanded.requests) != identity.request_sha256);

  OMARCHY_CHECK(identify_tree_contents(make_contents(bytes, true), manifest)
                  .tree_sha256 != identity.tree_sha256);

  expect_rejected(
      [&] {
        auto contents = make_contents(bytes);
        contents.add({.relative = ".git/config", .bytes = "metadata"});
      },
      ".git content was accepted by canonical tree contents");
  expect_rejected(
      [&] {
        auto contents = make_contents(bytes);
        contents.add({.relative = "../escape", .bytes = "content"});
      },
      "escaping content path was accepted");

  const std::string multi_surface_manifest_bytes =
      R"({"schemaVersion":2,"id":"org.example.status","name":"Example Status","version":"2.0.0","runtime":{"apiVersion":1,"qml":"ui/Status.qml","surfaceQml":{"barWidget":"ui/BarWidget.qml"}},"surfaces":{"barWidget":{"role":"bar-embedded"}},"permissions":{"required":[],"optional":[]}})";
  const auto multi_surface_manifest =
      parse_manifest_v2(multi_surface_manifest_bytes);
  auto multi_surface_contents = make_contents(multi_surface_manifest_bytes);
  multi_surface_contents.add(
      {.relative = "ui/BarWidget.qml", .bytes = "import QtQuick\nItem {}\n"});
  (void)identify_tree_contents(std::move(multi_surface_contents),
                               multi_surface_manifest);
  expect_rejected(
      [&] {
        (void)identify_tree_contents(
            make_contents(multi_surface_manifest_bytes),
            multi_surface_manifest);
      },
      "missing per-surface QML entry was accepted");

}

void request_fingerprint_v2_contract() {
  using omarchy::plugins::manifest::canonical_capability_requests;
  using omarchy::plugins::manifest::parse_manifest_v2;
  using omarchy::plugins::manifest::requested_capability_fingerprint;

  const auto original = parse_manifest_v2(
      R"({"schemaVersion":2,"id":"a.b","name":"x","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"permissions":{"required":[{"capability":"local.status","definitionGeneration":7,"definitionDigest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","operations":["status.write","status.read"],"resource":4,"reason":"status"}],"optional":[{"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"],"categories":["timer"],"reason":"notify"}]}})");
  const auto equivalent = parse_manifest_v2(
      R"({"permissions":{"optional":[{"reason":"different display text","categories":["timer"],"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"]}],"required":[{"reason":"also different","resource":4,"operations":["status.read","status.write"],"definitionDigest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","definitionGeneration":7,"capability":"local.status"}]},"surfaces":{},"runtime":{"qml":"Main.qml","apiVersion":1},"version":"1","name":"x","id":"a.b","schemaVersion":2})");

  const auto fingerprint = requested_capability_fingerprint(original.requests);
  OMARCHY_CHECK(requested_capability_fingerprint(original.requests) == fingerprint);
  OMARCHY_CHECK(requested_capability_fingerprint(equivalent.requests) == fingerprint);
  auto reordered_requests = original.requests;
  std::reverse(reordered_requests.begin(), reordered_requests.end());
  OMARCHY_CHECK(requested_capability_fingerprint(reordered_requests) == fingerprint);
  const auto canonical =
      canonical_capability_requests(std::move(reordered_requests));
  OMARCHY_CHECK(canonical.size() == 2 &&
              canonical[0].capability == "local.status" &&
              canonical[0].operations ==
                  std::vector<std::string>{"status.read", "status.write"} &&
              canonical[1].capability == "notifications.send");

  auto changed_generation = original.requests;
  ++changed_generation.front().definition_generation;
  OMARCHY_CHECK(requested_capability_fingerprint(changed_generation) != fingerprint);

  auto changed_digest = original.requests;
  changed_digest.front().definition_digest = std::string(64, 'b');
  OMARCHY_CHECK(requested_capability_fingerprint(changed_digest) != fingerprint);

  auto changed_operations = original.requests;
  changed_operations.front().operations.back() = "status.watch";
  OMARCHY_CHECK(requested_capability_fingerprint(changed_operations) != fingerprint);
}

void mutation_contract(const std::filesystem::path &fixtures) {
  const auto seed = read(fixtures / "valid-minimal/manifest.json");
  std::size_t accepted = 0;
  std::size_t rejected = 0;
  for (std::size_t iteration = 0; iteration < 2048; ++iteration) {
    std::string candidate = seed;
    const std::size_t offset =
        (iteration * 2654435761ULL + 17ULL) % candidate.size();
    candidate[offset] = static_cast<char>(
        static_cast<unsigned char>(candidate[offset]) ^
        static_cast<unsigned char>(1U << (iteration % 8)));
    try {
      const auto parsed =
          omarchy::plugins::manifest::parse_manifest_v2(candidate);
      const auto reparsed = omarchy::plugins::manifest::parse_manifest_v2(
          parsed.canonical_json);
      OMARCHY_CHECK(parsed == reparsed);
      ++accepted;
    } catch (const std::runtime_error &) {
      ++rejected;
    }
  }
  OMARCHY_CHECK(accepted > 0 && rejected > 0);
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    const std::filesystem::path fixtures = MANIFEST_FIXTURE_ROOT;
    parser_contract(fixtures);
    digest_contract(fixtures);
    request_fingerprint_v2_contract();
    mutation_contract(fixtures);
    std::cout << "manifest v2 contract: PASS\n";
    return 0;
  }, "manifest-contract-test: ");
}
