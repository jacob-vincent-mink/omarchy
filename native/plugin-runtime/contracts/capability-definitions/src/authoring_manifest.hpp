#pragma once

#include "capability_definition.hpp"

namespace omarchy::plugins::definitions {

// Installation-time resolution against an already trusted registry. The output
// is an exact schema-v2 manifest, not a consent decision or a live grant. Callers
// must hash, publish and review this artifact before any activation uses it.
// Runtime readers must continue to call parse_manifest_v2, never this resolver.
[[nodiscard]] manifest::ManifestV2 resolve_authoring_manifest_v1(
    std::string_view bytes, const TrustedDefinitionRegistry &registry);

} // namespace omarchy::plugins::definitions
