#include "authoring_manifest.hpp"
#include "manifest_json.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace omarchy::plugins::definitions {
namespace {
void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

std::uint32_t version(const nlohmann::json &value) {
  require(value.is_number_integer(), "definition version must be an integer");
  const auto number = value.get<std::int64_t>();
  require(number > 0 && number <= UINT32_MAX, "definition version is out of range");
  return static_cast<std::uint32_t>(number);
}
} // namespace

manifest::ManifestV2 resolve_authoring_manifest_v1(
    std::string_view bytes, const TrustedDefinitionRegistry &registry) {
  auto document = manifest::detail::parse_manifest_json(bytes);
  try {
    require(document.is_object() && document.contains("authoringVersion") &&
                !document.contains("schemaVersion") &&
                version(document.at("authoringVersion")) == 1,
            "expected authoringVersion 1 without a runtime schemaVersion");
    auto &permissions = document.at("permissions");
    require(permissions.is_object(), "permissions must be an object");
    for (const auto *kind : {"required", "optional"}) {
      auto &requests = permissions.at(kind);
      require(requests.is_array() && requests.size() <= 128,
              "invalid authoring capability request list");
      for (auto &request : requests) {
        require(request.is_object() && !request.contains("definitionGeneration") &&
                    !request.contains("definitionDigest"),
                "authoring requests cannot contain runtime definition pins");
        const auto &range = request.at("definitionVersions");
        require(range.is_object() && range.size() == 2 &&
                    range.contains("minimum") && range.contains("maximum"),
                "definitionVersions requires only minimum and maximum");
        const auto minimum = version(range.at("minimum"));
        const auto maximum = version(range.at("maximum"));
        require(minimum <= maximum, "definition version interval is reversed");
        const auto &name = request.at("capability").get_ref<const std::string &>();
        const auto selected = registry.find(name);
        require(selected && selected->generation >= minimum &&
                    selected->generation <= maximum,
                "no trusted definition satisfies the requested version interval");
        request.erase("definitionVersions");
        request["definitionGeneration"] = selected->generation;
        request["definitionDigest"] = selected->digest.view();
      }
    }
    document.erase("authoringVersion");
    document["schemaVersion"] = 2;
    auto resolved = manifest::parse_manifest_v2(document.dump());
    require(dynamic_requests_from_manifest(resolved, registry).has_value(),
            "resolved capability requests do not match trusted definitions");
    return resolved;
  } catch (const nlohmann::json::exception &) {
    throw std::runtime_error("invalid authoring manifest structure");
  }
}

} // namespace omarchy::plugins::definitions
