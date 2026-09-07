#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace omarchy::plugin_runtime::test_support {

// Independent wire fixture: keep the literal package pin and encoding separate
// from production serialization so channel tests detect protocol drift.
inline std::vector<std::byte> notification_request(
    std::string_view category = "complete") {
  std::vector<std::byte> bytes;
  const auto number = [&](std::uint64_t value, unsigned width) {
    for (unsigned i = width; i > 0; --i)
      bytes.push_back(static_cast<std::byte>(value >> ((i - 1) * 8)));
  };
  const auto text = [&](std::string_view value) {
    number(value.size(), 2);
    for (const auto ch : value) bytes.push_back(static_cast<std::byte>(ch));
  };
  for (const auto ch : std::string_view("OMDINVK"))
    bytes.push_back(static_cast<std::byte>(ch));
  bytes.push_back(std::byte{2});
  text("notifications.send");
  number(1, 4);
  text("522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872");
  text("send");
  number(0, 1);
  const auto payload = "{\"body\":\".\",\"category\":\"" + std::string(category) +
                       "\",\"title\":\"Test\"}";
  number(payload.size(), 4);
  for (const auto ch : payload) bytes.push_back(static_cast<std::byte>(ch));
  return bytes;
}

} // namespace omarchy::plugin_runtime::test_support
