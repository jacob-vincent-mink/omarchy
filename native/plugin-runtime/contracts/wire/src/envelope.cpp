#include "omarchy/plugin/wire/envelope.hpp"
#include "omarchy/plugin_runtime/big_endian.hpp"

#include <cstring>
#include <limits>

namespace omarchy::plugin::wire {
namespace {

bool negotiation_message(std::uint16_t message_type) {
  return message_type == static_cast<std::uint16_t>(CommonMessageType::hello) ||
         message_type ==
             static_cast<std::uint16_t>(CommonMessageType::welcome) ||
         message_type == static_cast<std::uint16_t>(
                             CommonMessageType::negotiation_failed);
}

bool valid_sequence(const EnvelopeHeader &header) {
  if (negotiation_message(header.message_type))
    return header.lane_sequence == 0;
  const auto tag = static_cast<std::uint16_t>(header.endpoint_role);
  return tag >= 1 && tag <= 3 && (header.lane_sequence >> 2U) != 0 &&
         (header.lane_sequence & 0x3U) == tag;
}

using omarchy::plugin_runtime::big_endian::get;

} // namespace

EncodeResult encode_packet(const EnvelopeHeader &header,
                           std::span<const std::byte> payload,
                           std::span<std::byte> output) {
  if (header.magic != kMagic) {
    return {0, FatalReason::invalid_magic};
  }
  if (header.envelope_version != kEnvelopeVersion) {
    return {0, FatalReason::unsupported_envelope_version};
  }
  if (header.header_size != kHeaderSize) {
    return {0, FatalReason::invalid_header_size};
  }
  if (!valid_sequence(header))
    return {0, FatalReason::invalid_lane_sequence};
  if (header.flags != 0) {
    return {0, FatalReason::nonzero_flags};
  }
  if (header.reserved != 0) {
    return {0, FatalReason::nonzero_reserved};
  }
  const auto cap = payload_cap(header.endpoint_role);
  if (cap == 0 || payload.size() > cap ||
      payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {0, FatalReason::payload_cap_exceeded};
  }
  const auto packet_size = kHeaderSize + payload.size();
  if (output.size() < packet_size) {
    return {0, FatalReason::output_too_small};
  }

  auto encoded_header = header;
  encoded_header.payload_length = static_cast<std::uint32_t>(payload.size());
  EnvelopeLayout::encode(encoded_header, output);
  if (!payload.empty()) {
    std::memcpy(output.data() + kHeaderSize, payload.data(), payload.size());
  }
  return {packet_size, FatalReason::none};
}

DecodeResult decode_packet(std::span<const std::byte> packet,
                           EndpointRole trusted_role) {
  if (packet.size() < 8) {
    return {{}, FatalReason::packet_too_short};
  }

  if (get<std::uint32_t>(packet, 0) != kMagic)
    return {{}, FatalReason::invalid_magic};
  const auto envelope_version = get<std::uint16_t>(packet, 4);
  if (envelope_version != kEnvelopeVersion)
    return {{}, FatalReason::unsupported_envelope_version};
  if (get<std::uint16_t>(packet, 6) != kHeaderSize)
    return {{}, FatalReason::invalid_header_size};
  if (packet.size() < kHeaderSize)
    return {{}, FatalReason::packet_too_short};

  const auto header = EnvelopeLayout::decode(packet);

  if (!valid_sequence(header))
    return {{}, FatalReason::invalid_lane_sequence};
  if (header.flags != 0) {
    return {{}, FatalReason::nonzero_flags};
  }
  if (header.reserved != 0) {
    return {{}, FatalReason::nonzero_reserved};
  }
  if (header.endpoint_role != trusted_role) {
    return {{}, FatalReason::endpoint_role_mismatch};
  }
  const auto cap = payload_cap(trusted_role);
  if (cap == 0 || header.payload_length > cap) {
    return {{}, FatalReason::payload_cap_exceeded};
  }
  if (packet.size() - kHeaderSize != header.payload_length) {
    return {{}, FatalReason::packet_length_mismatch};
  }

  return {PacketView{header, packet.subspan(kHeaderSize)},
          FatalReason::none};
}

} // namespace omarchy::plugin::wire
