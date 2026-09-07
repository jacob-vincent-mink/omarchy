#pragma once

#include "omarchy/plugin/wire/surface_name.hpp"
#include "omarchy/plugin/wire/role_registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace omarchy::plugin::wire {

inline constexpr std::uint16_t kControlRoleVersion = 1;

inline constexpr std::uint16_t kPermissionSnapshotMessage = 0x0100;
inline constexpr std::uint16_t kPermissionSnapshotAcceptedMessage = 0x0101;
inline constexpr std::uint16_t kSettingsSnapshotMessage = 0x0102;
inline constexpr std::uint16_t kSettingsSnapshotAcceptedMessage = 0x0103;
inline constexpr std::uint16_t kSettingsUpdateMessage = 0x0104;
inline constexpr std::uint16_t kSettingsUpdateResultMessage = 0x0105;
inline constexpr std::uint16_t kPresentationSnapshotMessage = 0x0106;
inline constexpr std::uint16_t kPresentationSnapshotAcceptedMessage = 0x0107;

inline constexpr std::array kControlMessageRules{
    MessageRule{kPermissionSnapshotMessage, DirectionMask::host_to_worker,
        CorrelationRule::zero, MessageSemantic::one_way, 1, payload_cap(EndpointRole::control)},
    MessageRule{kPermissionSnapshotAcceptedMessage, DirectionMask::worker_to_host,
        CorrelationRule::zero, MessageSemantic::one_way, 0, 0},
    MessageRule{kSettingsSnapshotMessage, DirectionMask::host_to_worker,
        CorrelationRule::zero, MessageSemantic::one_way, 1, payload_cap(EndpointRole::control)},
    MessageRule{kSettingsSnapshotAcceptedMessage, DirectionMask::worker_to_host,
        CorrelationRule::zero, MessageSemantic::one_way, 0, 0},
    MessageRule{kSettingsUpdateMessage, DirectionMask::worker_to_host,
        CorrelationRule::nonzero, MessageSemantic::request, 1, payload_cap(EndpointRole::control)},
    MessageRule{kSettingsUpdateResultMessage, DirectionMask::host_to_worker,
        CorrelationRule::nonzero, MessageSemantic::terminal, 1, 1},
    MessageRule{kPresentationSnapshotMessage, DirectionMask::host_to_worker,
        CorrelationRule::zero, MessageSemantic::one_way, 1, payload_cap(EndpointRole::control)},
    MessageRule{kPresentationSnapshotAcceptedMessage, DirectionMask::worker_to_host,
        CorrelationRule::zero, MessageSemantic::one_way, 0, 0},
};

inline constexpr RoleSchemaView control_role_schema() {
  return {.role = EndpointRole::control, .version = kControlRoleVersion,
          .messages = kControlMessageRules};
}

// Structural validation only: readiness order, exact manifest identity and
// settings/presentation contents remain the consuming session's responsibility.
inline bool valid_control_packet(const PacketView &packet, Direction direction) {
  if (packet.header.endpoint_role != EndpointRole::control)
    return false;
  const auto *rule = find_message(control_role_schema(), packet.header.message_type);
  if (!rule || !permits_direction(*rule, direction) ||
      packet.payload.size() < rule->minimum_payload ||
      packet.payload.size() > rule->maximum_payload ||
      (packet.header.correlation_id == 0) != (rule->correlation == CorrelationRule::zero))
    return false;
  return packet.header.message_type != kSettingsUpdateResultMessage ||
         packet.payload.front() == std::byte{0} || packet.payload.front() == std::byte{1};
}

inline constexpr bool is_startup_snapshot(std::uint16_t type) {
  return type == kPermissionSnapshotMessage || type == kSettingsSnapshotMessage ||
         type == kPresentationSnapshotMessage;
}

inline constexpr bool is_startup_acknowledgement(std::uint16_t type) {
  return type == kPermissionSnapshotAcceptedMessage ||
         type == kSettingsSnapshotAcceptedMessage ||
         type == kPresentationSnapshotAcceptedMessage;
}

struct SurfaceBinding {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
};

// Both trusted endpoints derive surface identity from the verified manifest's
// canonical surface_names order. Keeping that rule here prevents either side
// from acquiring an independent name-to-key assignment authority.
inline std::optional<SurfaceBinding>
manifest_surface_binding(std::string_view surface, std::size_t index,
                         std::uint64_t generation) {
  if (!valid_surface_name(surface) || index >= kMaximumPluginSurfaces ||
      generation == 0)
    return std::nullopt;
  return SurfaceBinding{.id = index + 1, .generation = generation};
}

} // namespace omarchy::plugin::wire
