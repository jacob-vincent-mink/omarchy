#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace omarchy::plugin_runtime::launcher::detail {

// Qt overwrites duplicate keys. Inspect the original decoded key events first;
// future metadata is allowed, but these top-level authority keys must be unique.
[[nodiscard]] inline bool unique_authoritative_keys(std::string_view line) {
  unsigned child_pid_count = 0;
  unsigned exit_code_count = 0;
  const auto parsed = nlohmann::json::parse(line,
      [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json &value) {
        if (depth == 1 && event == nlohmann::json::parse_event_t::key) {
          child_pid_count += value == "child-pid";
          exit_code_count += value == "exit-code";
        }
        return true;
      }, false);
  return !parsed.is_discarded() && child_pid_count <= 1 && exit_code_count <= 1;
}

} // namespace omarchy::plugin_runtime::launcher::detail
