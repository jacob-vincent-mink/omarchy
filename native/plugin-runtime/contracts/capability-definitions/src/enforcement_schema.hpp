#pragma once

#include "capability_definition.hpp"

#include <algorithm>

namespace omarchy::plugins::definitions {

struct EnforcementSchema {
  EnforcementFamily family;
  std::string_view name;
  std::string_view scope;
  std::uint8_t scope_digest_tag;
};

// These tags preserve the v1 definition digest; they are not enum ordinals.
inline constexpr std::array enforcement_schemas{
    EnforcementSchema{EnforcementFamily::network_fetch, "network-fetch", "https-origins-methods", 0},
    EnforcementSchema{EnforcementFamily::external_open_uri, "external-open-uri", "https-origins-gesture", 1},
    EnforcementSchema{EnforcementFamily::system_observe, "system-observe", "named-sanitized-datasets", 2},
    EnforcementSchema{EnforcementFamily::device_observe, "device-observe", "selected-device-fields", 3},
    EnforcementSchema{EnforcementFamily::device_control, "device-control", "selected-device-controls", 4},
    EnforcementSchema{EnforcementFamily::media_play_stream, "media-play-stream", "activation-source-handles-controls", 6},
    EnforcementSchema{EnforcementFamily::cli_harness, "cli-harness", "manifest-command-rules", 7},
    EnforcementSchema{EnforcementFamily::private_storage, "private-storage", "private-storage-quota", 8},
    EnforcementSchema{EnforcementFamily::notifications, "notifications", "notification-categories", 9}};

inline const EnforcementSchema *enforcement_schema(EnforcementFamily family) {
  const auto found = std::ranges::find(enforcement_schemas, family,
                                      &EnforcementSchema::family);
  return found == enforcement_schemas.end() ? nullptr : &*found;
}

} // namespace omarchy::plugins::definitions
