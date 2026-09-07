#pragma once

#include <string_view>

namespace omarchy::plugin_runtime {

// Lexical rule for manifest plugin/capability IDs and selected plugin names.
// Trusted definition names have a different grammar and do not use this check.
[[nodiscard]] constexpr bool
canonical_manifest_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128 ||
      value.front() < 'a' || value.front() > 'z')
    return false;
  bool previous_separator = true;
  for (const unsigned char character : value) {
    const bool alphanumeric = (character >= 'a' && character <= 'z') ||
                              (character >= '0' && character <= '9');
    const bool separator =
        character == '.' || character == '-' || character == '_';
    if ((!alphanumeric && !separator) || (separator && previous_separator))
      return false;
    previous_separator = separator;
  }
  return !previous_separator;
}

} // namespace omarchy::plugin_runtime
