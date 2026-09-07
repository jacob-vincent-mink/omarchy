#include "omarchy/plugin/wire/permission_snapshot.hpp"
#include "omarchy/plugin_runtime/big_endian.hpp"
#include "omarchy/plugin_runtime/canonical_sha256.hpp"

#include <algorithm>
#include <utility>

namespace omarchy::plugin::wire::permission_snapshot {
namespace {

using omarchy::plugin_runtime::big_endian::get;
using omarchy::plugin_runtime::big_endian::put;

bool valid_state(GrantState state) {
  return state >= GrantState::granted && state <= GrantState::revoked;
}

bool valid_permission(const PermissionRow &permission) {
  return valid_state(permission.state) &&
         ((permission.state == GrantState::denied) ==
          (permission.operation_mask == 0));
}

} // namespace

bool valid_manifest_request_fingerprint(std::string_view fingerprint) noexcept {
  static_assert(kManifestRequestFingerprintBytes == 64);
  return omarchy::plugin_runtime::canonical_sha256(fingerprint);
}

std::vector<std::byte> encode(const PermissionSnapshot &snapshot) {
  if (!valid_manifest_request_fingerprint(
          snapshot.manifest_request_fingerprint) ||
      snapshot.permissions.size() > kMaximumManifestRequests ||
      !std::ranges::all_of(snapshot.permissions, valid_permission))
    return {};

  std::vector<std::byte> output(
      kFixedPayloadBytes + snapshot.permissions.size() * kPermissionRowBytes);
  put<std::uint16_t>(output, 0, kCodecVersion);
  std::transform(snapshot.manifest_request_fingerprint.begin(),
                 snapshot.manifest_request_fingerprint.end(),
                 output.begin() + sizeof(std::uint16_t),
                 [](char character) { return std::byte(character); });
  put<std::uint16_t>(output, sizeof(std::uint16_t) + kManifestRequestFingerprintBytes,
        static_cast<std::uint16_t>(snapshot.permissions.size()));
  for (std::size_t index = 0; index < snapshot.permissions.size(); ++index) {
    const auto offset = kFixedPayloadBytes + index * kPermissionRowBytes;
    output[offset] = static_cast<std::byte>(snapshot.permissions[index].state);
    put<std::uint16_t>(output, offset + sizeof(std::uint8_t),
          snapshot.permissions[index].operation_mask);
  }
  return output;
}

bool decode(std::span<const std::byte> payload, PermissionSnapshot &snapshot) {
  if (payload.size() < kFixedPayloadBytes ||
      payload.size() > kMaximumPayloadBytes ||
      get<std::uint16_t>(payload, 0) != kCodecVersion)
    return false;

  PermissionSnapshot decoded;
  decoded.manifest_request_fingerprint.assign(
      reinterpret_cast<const char *>(payload.data() + sizeof(std::uint16_t)),
      kManifestRequestFingerprintBytes);
  if (!valid_manifest_request_fingerprint(decoded.manifest_request_fingerprint))
    return false;

  const auto count =
      get<std::uint16_t>(payload, sizeof(std::uint16_t) + kManifestRequestFingerprintBytes);
  if (count > kMaximumManifestRequests ||
      payload.size() != kFixedPayloadBytes + count * kPermissionRowBytes)
    return false;

  decoded.permissions.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto offset = kFixedPayloadBytes + index * kPermissionRowBytes;
    const auto state =
        static_cast<GrantState>(std::to_integer<std::uint8_t>(payload[offset]));
    const PermissionRow permission{
        .state = state,
        .operation_mask = get<std::uint16_t>(payload, offset + sizeof(std::uint8_t))};
    if (!valid_permission(permission))
      return false;
    decoded.permissions.push_back(permission);
  }
  snapshot = std::move(decoded);
  return true;
}

} // namespace omarchy::plugin::wire::permission_snapshot
