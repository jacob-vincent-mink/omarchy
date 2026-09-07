#pragma once

#include "big_endian.hpp"
#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>

namespace omarchy::plugin_runtime {

struct ByteReader {
  std::span<const std::byte> bytes;
  std::size_t offset = 0;

  bool raw(std::size_t size, std::span<const std::byte> &value) {
    if (size > bytes.size() - std::min(offset, bytes.size()))
      return false;
    value = bytes.subspan(offset, size);
    offset += size;
    return true;
  }
  template <typename T> bool integer(T &value) {
    std::span<const std::byte> encoded;
    if (!raw(sizeof(T), encoded))
      return false;
    value = big_endian::get<T>(encoded, 0);
    return true;
  }
  bool text(std::string_view &value, std::size_t maximum = UINT16_MAX) {
    std::uint16_t size = 0;
    std::span<const std::byte> encoded;
    if (!integer(size) || size == 0 || size > maximum || !raw(size, encoded))
      return false;
    value = {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
    return value.find('\0') == std::string_view::npos;
  }
};

} // namespace omarchy::plugin_runtime
