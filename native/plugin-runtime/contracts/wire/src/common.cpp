#include "omarchy/plugin/wire/common.hpp"
#include "omarchy/plugin/wire/state.hpp"

#include <algorithm>

namespace omarchy::plugin::wire {
namespace {


EnvelopeHeader common_header(EndpointRole role, CommonMessageType type) {
  return EnvelopeHeader{.endpoint_role = role,
                        .message_type = static_cast<std::uint16_t>(type)};
}

} // namespace

std::array<std::byte, HelloLayout::size> encode_hello_payload(const HelloPayload &payload) {
  std::array<std::byte, HelloLayout::size> output{};
  HelloLayout::encode(payload, output);
  return output;
}

std::array<std::byte, WelcomeLayout::size> encode_welcome_payload(const WelcomePayload &payload) {
  std::array<std::byte, WelcomeLayout::size> output{};
  WelcomeLayout::encode(payload, output);
  return output;
}

std::array<std::byte, NegotiationFailedLayout::size>
encode_negotiation_failed_payload(const NegotiationFailedPayload &payload) {
  std::array<std::byte, NegotiationFailedLayout::size> output{};
  NegotiationFailedLayout::encode(payload, output);
  return output;
}

NegotiationResult TrustedNegotiator::accept_hello(const PacketView &packet) {
  if (hello_seen_ || selected_ || failed_) {
    failed_ = true;
    return {.error = FatalReason::duplicate_hello};
  }
  hello_seen_ = true;
  if (packet.header.envelope_version != kEnvelopeVersion ||
      packet.header.endpoint_role != role_ ||
      packet.header.message_type !=
          static_cast<std::uint16_t>(CommonMessageType::hello) ||
      packet.header.role_protocol_version != 0 ||
      packet.header.launch_generation != 0 ||
      packet.header.correlation_id != 0 ||
      packet.header.lane_sequence != 0 || supported_.minimum == 0 ||
      supported_.minimum > supported_.maximum || generation_ == 0 ||
      maximum_payload_ == 0 || maximum_payload_ > payload_cap(role_) ||
      maximum_in_flight_ == 0) {
    failed_ = true;
    return {.error = FatalReason::invalid_message_order};
  }
  if (packet.payload.size() != HelloLayout::size) {
    failed_ = true;
    return {.error = FatalReason::invalid_common_payload};
  }
  const auto hello = HelloLayout::decode(packet.payload);
  if (hello.supported.minimum == 0 || hello.supported.minimum > hello.supported.maximum) {
    failed_ = true;
    return {.error = FatalReason::invalid_common_payload};
  }

  const auto minimum = std::max(supported_.minimum, hello.supported.minimum);
  const auto maximum = std::min(supported_.maximum, hello.supported.maximum);
  if (minimum > maximum) {
    failed_ = true;
    NegotiationResult result{};
    result.kind = NegotiationKind::negotiation_failed;
    result.header = common_header(role_, CommonMessageType::negotiation_failed);
    const auto payload = encode_negotiation_failed_payload(
        {NegotiationFailure::no_common_role_version, supported_});
    std::copy(payload.begin(), payload.end(), result.payload.begin());
    result.payload_size = payload.size();
    return result;
  }

  selected_ = true;
  selected_version_ = maximum;
  NegotiationResult result{};
  result.kind = NegotiationKind::welcome;
  result.header = common_header(role_, CommonMessageType::welcome);
  result.header.role_protocol_version = selected_version_;
  result.header.launch_generation = generation_;
  const auto payload =
      encode_welcome_payload({maximum_payload_, maximum_in_flight_});
  std::copy(payload.begin(), payload.end(), result.payload.begin());
  result.payload_size = payload.size();
  return result;
}

WorkerNegotiator::HelloResult WorkerNegotiator::make_hello() {
  if (hello_sent_ || selected_ || failed_ || supported_.minimum == 0 ||
      supported_.minimum > supported_.maximum) {
    failed_ = true;
    return {.error = FatalReason::invalid_message_order};
  }
  hello_sent_ = true;
  return {.header = common_header(role_, CommonMessageType::hello),
          .payload = encode_hello_payload({supported_})};
}

FatalReason WorkerNegotiator::accept_reply(const PacketView &packet) {
  if (selected_) {
    failed_ = true;
    return FatalReason::duplicate_welcome;
  }
  if (!hello_sent_ || failed_) {
    failed_ = true;
    return FatalReason::invalid_message_order;
  }
  if (packet.header.envelope_version != kEnvelopeVersion ||
      packet.header.endpoint_role != role_ ||
      packet.header.correlation_id != 0 ||
      packet.header.lane_sequence != 0) {
    failed_ = true;
    return FatalReason::invalid_welcome;
  }
  if (packet.header.message_type ==
      static_cast<std::uint16_t>(CommonMessageType::negotiation_failed)) {
    if (packet.header.role_protocol_version != 0 ||
        packet.header.launch_generation != 0 ||
        packet.payload.size() != NegotiationFailedLayout::size) {
      failed_ = true;
      return FatalReason::invalid_common_payload;
    }
    const auto failure = NegotiationFailedLayout::decode(packet.payload);
    if (failure.reason != NegotiationFailure::no_common_role_version ||
        failure.trusted_supported.minimum == 0 ||
        failure.trusted_supported.minimum > failure.trusted_supported.maximum) {
      failed_ = true;
      return FatalReason::invalid_common_payload;
    }
    failed_ = true;
    return FatalReason::version_negotiation_failed;
  }
  if (packet.header.message_type !=
          static_cast<std::uint16_t>(CommonMessageType::welcome) ||
      packet.header.role_protocol_version < supported_.minimum ||
      packet.header.role_protocol_version > supported_.maximum ||
      packet.header.launch_generation == 0) {
    failed_ = true;
    return FatalReason::invalid_welcome;
  }
  if (packet.payload.size() != WelcomeLayout::size) {
    failed_ = true;
    return FatalReason::invalid_welcome;
  }
  const auto welcome = WelcomeLayout::decode(packet.payload);
  if (welcome.maximum_payload == 0 || welcome.maximum_in_flight == 0 ||
      welcome.maximum_payload > payload_cap(role_)) {
    failed_ = true;
    return FatalReason::invalid_welcome;
  }
  selected_ = true;
  selected_version_ = packet.header.role_protocol_version;
  generation_ = packet.header.launch_generation;
  maximum_payload_ = welcome.maximum_payload;
  maximum_in_flight_ = welcome.maximum_in_flight;
  return FatalReason::none;
}

FatalReason RequiredEndpointReadiness::observe(EndpointRole role,
                                               std::uint64_t generation) {
  const auto raw = static_cast<std::uint16_t>(role);
  if (raw < 1 || raw > generations_.size() || generation == 0 ||
      generations_[raw - 1] != 0) {
    return FatalReason::invalid_message_order;
  }
  generations_[raw - 1] = generation;
  return FatalReason::none;
}

FatalReason RequiredEndpointReadiness::ready(bool &output) const {
  output = false;
  if (std::any_of(generations_.begin(), generations_.end(),
                  [](std::uint64_t value) { return value == 0; })) {
    return FatalReason::none;
  }
  if (!std::all_of(
          generations_.begin(), generations_.end(),
          [&](std::uint64_t value) { return value == generations_.front(); })) {
    return FatalReason::readiness_generation_mismatch;
  }
  output = true;
  return FatalReason::none;
}

} // namespace omarchy::plugin::wire
