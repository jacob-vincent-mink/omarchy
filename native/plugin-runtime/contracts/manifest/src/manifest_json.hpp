#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace omarchy::plugins::manifest::detail {

// Shared syntax/size boundary for authoring, runtime manifests and settings.
// Parsing does not resolve definitions or authorize any requested capability.
nlohmann::json parse_manifest_json(std::string_view input);

} // namespace omarchy::plugins::manifest::detail
