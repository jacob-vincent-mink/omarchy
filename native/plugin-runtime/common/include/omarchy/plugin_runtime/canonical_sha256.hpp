#pragma once

#include <string_view>

namespace omarchy::plugin_runtime {

[[nodiscard]] constexpr bool canonical_sha256(std::string_view value) noexcept {
  if (value.size() != 64)
    return false;
  for (const unsigned char byte : value)
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return false;
  return true;
}

} // namespace omarchy::plugin_runtime
