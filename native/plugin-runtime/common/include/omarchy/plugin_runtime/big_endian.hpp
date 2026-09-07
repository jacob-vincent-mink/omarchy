#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

namespace omarchy::plugin_runtime::big_endian {

// Unaligned, fixed-width primitives. Protocol codecs must validate the span
// and field offset first; these functions do not authorize a frame or payload.
template <typename T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
constexpr void put(std::span<std::byte> output, std::size_t offset, T value) {
  auto bits = static_cast<std::make_unsigned_t<T>>(value);
  for (std::size_t index = sizeof(T); index > 0; --index) {
    output[offset + index - 1] = static_cast<std::byte>(bits);
    bits >>= 8;
  }
}

template <typename T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
constexpr T get(std::span<const std::byte> input, std::size_t offset) {
  using Unsigned = std::make_unsigned_t<T>;
  Unsigned bits = 0;
  for (std::size_t index = 0; index < sizeof(T); ++index)
    bits = static_cast<Unsigned>((bits << 8) |
                                 std::to_integer<Unsigned>(input[offset + index]));
  return std::bit_cast<T>(bits);
}

template <typename T>
void append(std::vector<std::byte> &output, T value) {
  std::array<std::byte, sizeof(T)> bytes;
  put(bytes, 0, value);
  output.insert(output.end(), bytes.begin(), bytes.end());
}

} // namespace omarchy::plugin_runtime::big_endian
