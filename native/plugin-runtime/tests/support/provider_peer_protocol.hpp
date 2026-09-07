#pragma once

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Test-only framing: keep this independent of the production provider codec.
namespace provider_peer_protocol {
constexpr std::uint32_t kMagic = 0x4f505256;
constexpr std::size_t kHeader = 20;

inline std::uint32_t u32(const std::byte *bytes) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index)
    value = (value << 8) | std::to_integer<unsigned char>(bytes[index]);
  return value;
}
inline std::uint64_t u64(const std::byte *bytes) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index)
    value = (value << 8) | std::to_integer<unsigned char>(bytes[index]);
  return value;
}
inline void put32(std::vector<std::byte> &bytes, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    bytes.push_back(static_cast<std::byte>(value >> shift));
}
inline void put64(std::vector<std::byte> &bytes, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    bytes.push_back(static_cast<std::byte>(value >> shift));
}

inline bool respond(std::string_view mode, std::uint64_t correlation,
                    std::string_view payload) {
  std::vector<std::byte> response;
  put32(response, mode == "malformed" ? 0U : kMagic);
  response.insert(response.end(),
                  {std::byte{1}, std::byte{2}, std::byte{0}, std::byte{0}});
  put64(response, correlation);
  put32(response, static_cast<std::uint32_t>(payload.size() + 1));
  response.push_back(std::byte{0});
  const auto raw = std::as_bytes(std::span(payload.data(), payload.size()));
  response.insert(response.end(), raw.begin(), raw.end());
  if (mode == "truncated")
    response.resize(10);
  if (mode == "oversized")
    response.resize(1024 * 1024, std::byte{0});
  return ::send(3, response.data(), response.size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(response.size());
}
} // namespace provider_peer_protocol
