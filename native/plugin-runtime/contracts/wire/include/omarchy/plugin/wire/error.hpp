#pragma once

#include <cstdint>

namespace omarchy::plugin::wire {

enum class FatalReason : std::uint16_t {
  none = 0,
  output_too_small,
  payload_not_representable,
  packet_too_short,
  invalid_magic,
  unsupported_envelope_version,
  invalid_header_size,
  invalid_lane_sequence,
  lane_sequence_replayed,
  lane_sequence_exhausted,
  nonzero_flags,
  nonzero_reserved,
  endpoint_role_mismatch,
  payload_cap_exceeded,
  packet_length_mismatch,
  invalid_common_payload,
  invalid_message_order,
  duplicate_hello,
  version_negotiation_failed,
  invalid_welcome,
  duplicate_welcome,
  readiness_generation_mismatch,
  unsupported_role_version,
  stale_generation,
  unknown_message_type,
  invalid_direction,
  invalid_correlation,
  correlation_reused,
  maximum_in_flight_exceeded,
  unmatched_terminal,
  unmatched_cancel_result,
  duplicate_terminal,
  duplicate_cancel_result,
  invalid_role_schema,
};

} // namespace omarchy::plugin::wire
