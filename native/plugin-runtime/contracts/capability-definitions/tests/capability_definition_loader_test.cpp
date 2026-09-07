#include "../../../tests/support/test_assert.hpp"
#include "../../../tests/support/concurrent_mutation.hpp"
#include "../../../tests/support/temporary_directory.hpp"

#include "capability_definition_loader.hpp"

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace omarchy::plugins::definitions;

using omarchy::plugin_runtime::test_support::require;

CapabilityDefinition fixture() {
  CapabilityDefinition definition{
      .canonical_name = Name("local.weather-fetch"),
      .authority_identity = Name("local.weather-fetch-v1"),
      .enforcement_family = EnforcementFamily::network_fetch,
      .display_category_id = Name("local.weather"),
      .display_category_label = Label("Local weather tools"),
      .title = Label("Fetch weather from selected origins"),
      .risk_text =
          Label("Sends bounded requests to explicitly granted weather origins"),
      .risk = RiskLevel::moderate,
      .revocation = RevocationPolicy::cancel_inflight,
      .adapter = {.adapter_class = Name("bounded-https-fetch"),
                  .contract_digest = Digest(std::string(64, 'a')),
                  .abi_version = 1},
      .operations = {},
  };
  definition.operations.insert(
      {.name = Name("forecast.read"), .label = Label("Read a forecast")});
  return definition;
}
} // namespace

void capability_definition_loader_tests() {
  // Independent document vocabulary: each family accepts exactly one schema.
  const std::array schemas{
      std::pair{EnforcementFamily::network_fetch, "https-origins-methods"},
      std::pair{EnforcementFamily::external_open_uri, "https-origins-gesture"},
      std::pair{EnforcementFamily::system_observe, "named-sanitized-datasets"},
      std::pair{EnforcementFamily::device_observe, "selected-device-fields"},
      std::pair{EnforcementFamily::device_control, "selected-device-controls"},
      std::pair{EnforcementFamily::media_play_stream, "activation-source-handles-controls"},
      std::pair{EnforcementFamily::cli_harness, "manifest-command-rules"},
      std::pair{EnforcementFamily::private_storage, "private-storage-quota"},
      std::pair{EnforcementFamily::notifications, "notification-categories"}};
  for (const auto &definition : packaged_definitions()) {
    const auto document = canonical_definition_document(definition, 1);
    const auto expected = std::ranges::find(schemas, definition.enforcement_family,
                                           &decltype(schemas)::value_type::first);
    OMARCHY_CHECK(expected != schemas.end());
    const std::string field = std::string("scope-schema=") + expected->second + '\n';
    const auto offset = document.find(field);
    OMARCHY_CHECK(offset != std::string::npos);
    for (const auto &[family, scope] : schemas) {
      auto candidate = document;
      candidate.replace(offset, field.size(), std::string("scope-schema=") + scope + '\n');
      LoadedDefinition parsed;
      const auto result = parse_definition_document(candidate, parsed);
      OMARCHY_CHECK(result == (family == definition.enforcement_family
                                  ? LoadResult::loaded : LoadResult::invalid_document));
      if (result == LoadResult::loaded)
        OMARCHY_CHECK(parsed.definition == definition && parsed.generation == 1);
    }
    auto unknown = document;
    unknown.replace(offset, field.size(), "scope-schema=unknown\n");
    LoadedDefinition parsed;
    OMARCHY_CHECK(parse_definition_document(unknown, parsed) == LoadResult::invalid_document);
  }
  for (unsigned value = 0; value <= UINT8_MAX; ++value) {
    auto candidate = fixture();
    candidate.enforcement_family = static_cast<EnforcementFamily>(value);
    if (std::ranges::find(schemas, candidate.enforcement_family,
                          &decltype(schemas)::value_type::first) == schemas.end())
      OMARCHY_CHECK(!valid_definition(candidate) && canonical_definition_document(candidate, 1).empty());
  }
  const auto definition = fixture();
  const auto document = canonical_definition_document(definition, 7);
  OMARCHY_CHECK(!document.empty());
  for (const std::string term : {"decision", "duration", "bytes", "redact-payload", "redact-uri", "redact-tokens"}) {
    auto weakened = document;
    const auto field = term == "redact-tokens" ? "," + term : term + ",";
    const auto offset = weakened.find(field, weakened.find("\naudit="));
    OMARCHY_CHECK(offset != std::string::npos);
    weakened.erase(offset, field.size());
    LoadedDefinition rejected;
    OMARCHY_CHECK(parse_definition_document(weakened, rejected) == LoadResult::invalid_document);
  }
  LoadedDefinition parsed;
  OMARCHY_CHECK(parse_definition_document(document, parsed) == LoadResult::loaded &&
              parsed.generation == 7 && parsed.definition == definition);
  auto mutated = document;
  mutated.replace(mutated.find("bounded-https-fetch"),
                  std::string("bounded-https-fetch").size(), "unknown-adapter");
  OMARCHY_CHECK(parse_definition_document(mutated, parsed) == LoadResult::invalid_document);
  auto old_digest_field = document;
  old_digest_field.replace(old_digest_field.find("contract-digest"),
                           std::string("contract-digest").size(),
                           "adapter-digest");
  OMARCHY_CHECK(parse_definition_document(old_digest_field,
                                    parsed) ==
              LoadResult::invalid_document);

  omarchy::plugin_runtime::test_support::TemporaryDirectory directory;
  const auto &root = directory.path();
  directory.write_file(root / "weather.capability", document,
                       O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  chmod((root / "weather.capability").c_str(), 0644);
  chmod(root.c_str(), 0755);
  TrustedDefinitionRegistry registry;
  std::size_t loaded = 0;
  const int root_fd = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  OMARCHY_CHECK(root_fd >= 0);
  OMARCHY_CHECK(load_definition_directory_fd(root_fd, static_cast<std::uint32_t>(getuid()),
                                       registry,
                                       loaded) == LoadResult::loaded &&
              loaded == 1 && registry.find("local.weather-fetch"));

  TrustedDefinitionRegistry wrong_owner_registry;
  OMARCHY_CHECK(load_definition_directory_fd(root_fd, static_cast<std::uint32_t>(getuid()) + 1,
                                       wrong_owner_registry,
                                       loaded) == LoadResult::untrusted_path &&
              loaded == 0 && wrong_owner_registry.size() == 0);

  TrustedDefinitionRegistry invalid_descriptor_registry;
  loaded = 99;
  OMARCHY_CHECK(load_definition_directory_fd(-1, static_cast<std::uint32_t>(getuid()),
                                       invalid_descriptor_registry,
                                       loaded) == LoadResult::untrusted_path &&
              loaded == 0 && invalid_descriptor_registry.size() == 0);
  const int regular_file_fd =
      open((root / "weather.capability").c_str(), O_RDONLY | O_CLOEXEC);
  OMARCHY_CHECK(regular_file_fd >= 0);
  loaded = 99;
  OMARCHY_CHECK(load_definition_directory_fd(
              regular_file_fd, static_cast<std::uint32_t>(getuid()), invalid_descriptor_registry,
              loaded) == LoadResult::untrusted_path &&
              loaded == 0 && invalid_descriptor_registry.size() == 0);
  close(regular_file_fd);
  close(root_fd);

  const auto assert_rejected = [&](std::string_view message) {
    TrustedDefinitionRegistry candidate = registry;
    const auto before = candidate.size();
    const int descriptor =
        open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    OMARCHY_CHECK(descriptor >= 0);
    const auto result = load_definition_directory_fd(
        descriptor, static_cast<std::uint32_t>(getuid()), candidate, loaded);
    close(descriptor);
    require(result != LoadResult::loaded && loaded == 0 &&
                candidate.size() == before &&
                candidate.find("local.weather-fetch"),
            message);
  };

  chmod((root / "weather.capability").c_str(), 0600);
  assert_rejected("mode-0600 definition entered the trust registry");
  chmod((root / "weather.capability").c_str(), 0644);

  chmod(root.c_str(), 0775);
  assert_rejected("mode-0775 definition root entered the trust registry");
  chmod(root.c_str(), 0755);

  std::filesystem::create_hard_link(root / "weather.capability",
                                    root / "alias.capability");
  assert_rejected("hard-linked definition entered the trust registry");
  std::filesystem::remove(root / "alias.capability");

  for (const bool directory_change : {false, true}) {
    const auto path = root / (directory_change ? "churn" : "weather.capability");
    const bool rejected = omarchy::plugin_runtime::test_support::observe_concurrent_mutation(
        [&] {
          if (directory_change) {
            const int descriptor =
                open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
            if (descriptor >= 0) close(descriptor);
            unlink(path.c_str());
          } else {
            chmod(path.c_str(), 0600);
            chmod(path.c_str(), 0644);
          }
        },
        [&] {
          TrustedDefinitionRegistry mutation_registry;
          const int descriptor =
              open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
          OMARCHY_CHECK(descriptor >= 0);
          const auto result = load_definition_directory_fd(
              descriptor, static_cast<std::uint32_t>(getuid()), mutation_registry, loaded);
          close(descriptor);
          if (result == LoadResult::loaded) return false;
          OMARCHY_CHECK(loaded == 0 && mutation_registry.size() == 0);
          return true;
        }, 100, 100);
    if (directory_change)
      std::filesystem::remove(path);
    else
      chmod(path.c_str(), 0644);
    OMARCHY_CHECK(rejected);
  }

  std::filesystem::remove(root / "weather.capability");
  std::filesystem::create_symlink("/etc/passwd", root / "escape.capability");
  TrustedDefinitionRegistry symlink_registry;
  const int symlink_root_fd =
      open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  OMARCHY_CHECK(symlink_root_fd >= 0);
  OMARCHY_CHECK(load_definition_directory_fd(
              symlink_root_fd, static_cast<std::uint32_t>(getuid()), symlink_registry,
              loaded) == LoadResult::untrusted_path &&
              symlink_registry.size() == 0);
  close(symlink_root_fd);
  std::filesystem::remove_all(root);
}
