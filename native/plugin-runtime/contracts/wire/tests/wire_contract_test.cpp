#include "../../../tests/support/test_assert.hpp"

#include "omarchy/plugin/wire/common.hpp"
#include "omarchy/plugin/wire/control.hpp"
#include "omarchy/plugin/wire/permission_snapshot.hpp"
#include "omarchy/plugin/wire/role_registry.hpp"
#include "omarchy/plugin/wire/state.hpp"
#include "omarchy/plugin_runtime/big_endian.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using namespace omarchy::plugin::wire;

constexpr std::uint64_t kGeneration = 0x0102030405060708ULL;
constexpr std::uint64_t kCorrelation = 0x1112131415161718ULL;
constexpr std::uint16_t kRequestType = 0x1100;
constexpr std::uint16_t kResponseType = 0x1101;
constexpr std::uint16_t kEventType = 0x1102;

using omarchy::plugin_runtime::test_support::require;

void byte_order_goldens() {
  namespace endian = omarchy::plugin_runtime::big_endian;
  const auto check = []<typename T>(T value, std::array<std::byte, sizeof(T)> golden) {
    std::array<std::byte, sizeof(T) + 2> bytes{};
    bytes.front() = std::byte{0x5a};
    bytes.back() = std::byte{0xa5};
    endian::put<T>(bytes, 1, value);
    OMARCHY_CHECK(bytes.front() == std::byte{0x5a} && bytes.back() == std::byte{0xa5} &&
                std::ranges::equal(std::span(bytes).subspan(1, sizeof(T)), golden));
    OMARCHY_CHECK(endian::get<T>(bytes, 1) == value);
    std::vector<std::byte> appended{std::byte{0x5a}};
    endian::append(appended, value);
    endian::append(appended, value);
    OMARCHY_CHECK(appended.size() == 1 + 2 * sizeof(T) && appended.front() == std::byte{0x5a} &&
                std::ranges::equal(std::span(appended).subspan(1, sizeof(T)), golden) &&
                std::ranges::equal(std::span(appended).subspan(1 + sizeof(T)), golden));
  };
  check(std::uint16_t{0x1234}, {std::byte{0x12}, std::byte{0x34}});
  check(std::numeric_limits<std::int32_t>::min(),
        {std::byte{0x80}, std::byte{0}, std::byte{0}, std::byte{0}});
  check(std::int64_t{-2},
        {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
         std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xfe}});
  check(kGeneration,
        {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
         std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}});
}

std::vector<std::byte> encode(const EnvelopeHeader &header,
                              std::span<const std::byte> payload = {}) {
  std::vector<std::byte> output(kHeaderSize + payload.size());
  const auto result = encode_packet(header, payload, output);
  OMARCHY_CHECK(static_cast<bool>(result) && result.bytes_written == output.size());
  return output;
}

EnvelopeHeader sequenced_header(EndpointRole role, std::uint16_t type,
                                std::uint64_t sequence,
                                std::uint64_t correlation = 0) {
  return {.endpoint_role = role,
          .message_type = type,
          .role_protocol_version = 1,
          .launch_generation = kGeneration,
          .correlation_id = correlation,
          .lane_sequence = sequence};
}

constexpr std::uint64_t lane_value(EndpointRole role, std::uint64_t counter) {
  return (counter << 2U) | static_cast<std::uint16_t>(role);
}

std::string hex(std::span<const std::byte> bytes) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : bytes) {
    output << std::setw(2) << std::to_integer<unsigned int>(byte);
  }
  return output.str();
}

EnvelopeHeader selected_header(EndpointRole role, std::uint16_t type,
                               std::uint64_t correlation = 0) {
  return EnvelopeHeader{.endpoint_role = role,
                        .message_type = type,
                        .role_protocol_version = 1,
                        .launch_generation = kGeneration,
                        .correlation_id = correlation,
                        .lane_sequence =
                            type == static_cast<std::uint16_t>(
                                        CommonMessageType::welcome)
                                ? 0
                                : lane_value(role, 1)};
}

void control_schema_test() {
  const std::array schemas{control_role_schema()};
  OMARCHY_CHECK(RoleSchemaRegistryView(schemas).validate() == FatalReason::none);
  const std::array payload{std::byte{0}};
  for (std::uint32_t type = 0; type <= UINT16_MAX; ++type) {
    const bool snapshot = type == 0x0100 || type == 0x0102 || type == 0x0106;
    const bool acknowledgement = type == 0x0101 || type == 0x0103 || type == 0x0107;
    for (const auto direction : {Direction::host_to_worker, Direction::worker_to_host}) {
      PacketView packet{.header = {.message_type = static_cast<std::uint16_t>(type)},
                        .payload = payload};
      OMARCHY_CHECK(valid_control_packet(packet, direction) ==
          (snapshot && direction == Direction::host_to_worker));
      packet.payload = {};
      OMARCHY_CHECK(valid_control_packet(packet, direction) ==
          (acknowledgement && direction == Direction::worker_to_host));
      packet.header.correlation_id = 9;
      packet.payload = payload;
      OMARCHY_CHECK(valid_control_packet(packet, direction) ==
          ((type == 0x0104 && direction == Direction::worker_to_host) ||
           (type == 0x0105 && direction == Direction::host_to_worker)));
    }
  }
  std::vector<std::byte> data(payload_cap(EndpointRole::control), std::byte{0});
  PacketView snapshot{.header = {.message_type = 0x0100}, .payload = data};
  OMARCHY_CHECK(valid_control_packet(snapshot, Direction::host_to_worker));
  data.push_back(std::byte{0});
  snapshot.payload = data;
  OMARCHY_CHECK(!valid_control_packet(snapshot, Direction::host_to_worker));
  for (unsigned status = 0; status <= 255; ++status) {
    const std::array bytes{static_cast<std::byte>(status)};
    const PacketView result{.header = {.message_type = 0x0105, .correlation_id = 9},
                            .payload = bytes};
    OMARCHY_CHECK(valid_control_packet(result, Direction::host_to_worker) == (status <= 1));
  }
}

void golden_test() {
  const auto hello_payload =
      encode_hello_payload(HelloPayload{VersionRange{1, 1}});
  const auto hello = encode(
      EnvelopeHeader{.endpoint_role = EndpointRole::control}, hello_payload);
  OMARCHY_CHECK(hex(hello) ==
              "4f4d504c00020030000100010000000000000004000000000000000000000000"
              "0000000000000000000000000000000000010001");

  const auto welcome_payload = encode_welcome_payload({4096, 4});
  const auto welcome = encode(
      selected_header(EndpointRole::control,
                      static_cast<std::uint16_t>(CommonMessageType::welcome)),
      welcome_payload);
  OMARCHY_CHECK(hex(welcome) ==
              "4f4d504c00020030000100020001000000000008000000000102030405060708"
              "000000000000000000000000000000000000100000000004");

  const auto failed_payload = encode_negotiation_failed_payload(
      {NegotiationFailure::no_common_role_version, {1, 1}});
  const auto failed =
      encode(EnvelopeHeader{.endpoint_role = EndpointRole::render,
                            .message_type = static_cast<std::uint16_t>(
                                CommonMessageType::negotiation_failed)},
             failed_payload);
  OMARCHY_CHECK(hex(failed) ==
              "4f4d504c00020030000300030000000000000006000000000000000000000000"
              "00000000000000000000000000000000000100010001");

  const std::array<std::byte, 2> reason{std::byte{0}, std::byte{1}};
  const auto check_broker = [&](CommonMessageType type, std::uint64_t correlation,
      std::span<const std::byte> payload, std::string_view golden) {
    OMARCHY_CHECK(hex(encode(selected_header(EndpointRole::broker,
        static_cast<std::uint16_t>(type), correlation), payload)) == golden);
  };
  check_broker(CommonMessageType::typed_error, kCorrelation, reason,
      "4f4d504c00020030000200040001000000000002000000000102030405060708"
      "111213141516171800000000000000060001");
  check_broker(CommonMessageType::cancel, kCorrelation, {},
      "4f4d504c00020030000200050001000000000000000000000102030405060708"
      "11121314151617180000000000000006");
  check_broker(CommonMessageType::cancel_result, kCorrelation, reason,
      "4f4d504c00020030000200060001000000000002000000000102030405060708"
      "111213141516171800000000000000060001");
  check_broker(CommonMessageType::protocol_error, 0, reason,
      "4f4d504c00020030000200070001000000000002000000000102030405060708"
      "000000000000000000000000000000060001");

  const std::array<std::byte, 2> payload{std::byte{0xaa}, std::byte{0x55}};
  const auto sequenced = encode(sequenced_header(
                                    EndpointRole::broker, kRequestType,
                                    0x212223242526272aULL, kCorrelation),
                                payload);
  OMARCHY_CHECK(
      hex(sequenced) ==
          "4f4d504c00020030000211000001000000000002000000000102030405060708"
          "1112131415161718212223242526272aaa55");
}

void envelope_and_sequence_test() {
  static_assert(!std::is_copy_constructible_v<SessionSequence>);
  static_assert(!std::is_move_constructible_v<SessionSequence>);
  const auto valid = encode(sequenced_header(
      EndpointRole::broker, kRequestType,
      lane_value(EndpointRole::broker, 1), kCorrelation));
  const auto decoded = decode_packet(valid, EndpointRole::broker);
  OMARCHY_CHECK(decoded && decoded.packet.header.envelope_version ==
                         kEnvelopeVersion &&
              decoded.packet.header.header_size == kHeaderSize &&
              decoded.packet.header.lane_sequence ==
                  lane_value(EndpointRole::broker, 1));

  auto crossed = sequenced_header(EndpointRole::broker, kRequestType,
                                  lane_value(EndpointRole::broker, 1),
                                  kCorrelation);
  crossed.header_size = 40;
  std::array<std::byte, kHeaderSize> output{};
  OMARCHY_CHECK(encode_packet(crossed, {}, output).error ==
              FatalReason::invalid_header_size);
  auto crossed_bytes = valid;
  crossed_bytes[6] = std::byte{0};
  crossed_bytes[7] = std::byte{40};
  OMARCHY_CHECK(decode_packet(crossed_bytes, EndpointRole::broker).error ==
              FatalReason::invalid_header_size);

  auto unsupported = valid;
  unsupported[4] = std::byte{0};
  unsupported[5] = std::byte{1};
  OMARCHY_CHECK(decode_packet(unsupported, EndpointRole::broker).error ==
              FatalReason::unsupported_envelope_version);

  auto zero =
      sequenced_header(EndpointRole::broker, kRequestType, 0, kCorrelation);
  OMARCHY_CHECK(encode_packet(zero, {}, output).error ==
              FatalReason::invalid_lane_sequence);
  auto wrong_tag = sequenced_header(EndpointRole::control, kRequestType,
                                    lane_value(EndpointRole::broker, 1),
                                    kCorrelation);
  OMARCHY_CHECK(encode_packet(wrong_tag, {}, output).error ==
              FatalReason::invalid_lane_sequence);
  auto sequenced_hello = sequenced_header(
      EndpointRole::control,
      static_cast<std::uint16_t>(CommonMessageType::hello), 1);
  sequenced_hello.role_protocol_version = 0;
  sequenced_hello.launch_generation = 0;
  OMARCHY_CHECK(encode_packet(sequenced_hello, {}, output).error ==
              FatalReason::invalid_lane_sequence);

  SessionSequence sequence;
  const auto control = sequence.take_outbound(EndpointRole::control);
  const auto broker = sequence.take_outbound(EndpointRole::broker);
  const auto render = sequence.take_outbound(EndpointRole::render);
  OMARCHY_CHECK(control && control.value == lane_value(EndpointRole::control, 1) &&
              broker && broker.value == lane_value(EndpointRole::broker, 1) &&
              render && render.value == lane_value(EndpointRole::render, 1));
  OMARCHY_CHECK(sequence.accept_inbound(
              EndpointRole::broker, lane_value(EndpointRole::broker, 1)) ==
              FatalReason::none &&
              sequence.accept_inbound(
                  EndpointRole::broker,
                  lane_value(EndpointRole::broker, 3)) == FatalReason::none);

  SessionSequence replay;
  OMARCHY_CHECK(replay.accept_inbound(
              EndpointRole::render, lane_value(EndpointRole::render, 2)) ==
              FatalReason::none &&
              replay.accept_inbound(
                  EndpointRole::render,
                  lane_value(EndpointRole::render, 2)) ==
                  FatalReason::lane_sequence_replayed &&
              replay.failed());
  SessionSequence lower;
  OMARCHY_CHECK(lower.accept_inbound(
              EndpointRole::control, lane_value(EndpointRole::control, 9)) ==
              FatalReason::none &&
              lower.accept_inbound(
                  EndpointRole::control,
                  lane_value(EndpointRole::control, 8)) ==
                  FatalReason::lane_sequence_replayed);
  SessionSequence zero_sequence;
  OMARCHY_CHECK(zero_sequence.accept_inbound(EndpointRole::control, 0) ==
              FatalReason::invalid_lane_sequence);
  SessionSequence wrong_lane;
  OMARCHY_CHECK(wrong_lane.accept_inbound(
              EndpointRole::control, lane_value(EndpointRole::broker, 1)) ==
              FatalReason::invalid_lane_sequence);

  SessionSequence maximum(std::numeric_limits<std::uint64_t>::max() >> 2U);
  const auto last = maximum.take_outbound(EndpointRole::render);
  OMARCHY_CHECK(maximum.take_outbound(EndpointRole::control).value == last.value - 2 &&
              maximum.take_outbound(EndpointRole::broker).value == last.value - 1);
  const auto exhausted = maximum.take_outbound(EndpointRole::render);
  OMARCHY_CHECK(last && last.value == std::numeric_limits<std::uint64_t>::max() &&
              !exhausted &&
              exhausted.error == FatalReason::lane_sequence_exhausted);
}

void envelope_test() {
  for (const auto role :
       {EndpointRole::control, EndpointRole::broker, EndpointRole::render}) {
    const auto cap = payload_cap(role);
    std::vector<std::byte> payload(cap);
    const auto packet = encode(selected_header(role, kEventType), payload);
    const auto decoded = decode_packet(packet, role);
    OMARCHY_CHECK(static_cast<bool>(decoded) && decoded.packet.payload.size() == cap);
    auto oversized_claim = packet;
    omarchy::plugin_runtime::big_endian::put<std::uint32_t>(oversized_claim, 16, cap + 1);
    OMARCHY_CHECK(decode_packet(oversized_claim, role).error == FatalReason::payload_cap_exceeded);

    std::vector<std::byte> too_large(cap + 1);
    std::vector<std::byte> output(kHeaderSize + too_large.size());
    OMARCHY_CHECK(encode_packet(selected_header(role, kEventType), too_large, output)
                    .error == FatalReason::payload_cap_exceeded);
  }

  auto packet = encode(selected_header(EndpointRole::control, kEventType));
  auto mutation = packet;
  mutation[0] = std::byte{0};
  OMARCHY_CHECK(decode_packet(mutation, EndpointRole::control).error ==
              FatalReason::invalid_magic);
  mutation = packet;
  mutation[14] = std::byte{1};
  OMARCHY_CHECK(decode_packet(mutation, EndpointRole::control).error ==
              FatalReason::nonzero_flags);
  mutation = packet;
  mutation[20] = std::byte{1};
  OMARCHY_CHECK(decode_packet(mutation, EndpointRole::control).error ==
              FatalReason::nonzero_reserved);
  OMARCHY_CHECK(decode_packet(packet, EndpointRole::broker).error ==
              FatalReason::endpoint_role_mismatch);
  packet.push_back(std::byte{0});
  OMARCHY_CHECK(decode_packet(packet, EndpointRole::control).error ==
              FatalReason::packet_length_mismatch);
  OMARCHY_CHECK(decode_packet(
              std::span<const std::byte>(packet).first(kHeaderSize - 1),
              EndpointRole::control)
                  .error == FatalReason::packet_too_short);
}

std::vector<std::byte> encode_negotiation(const NegotiationResult &result) {
  return encode(
      result.header,
      std::span<const std::byte>(result.payload).first(result.payload_size));
}

void negotiation_test() {
  RequiredEndpointReadiness readiness;
  for (const auto role :
       {EndpointRole::control, EndpointRole::broker, EndpointRole::render}) {
    WorkerNegotiator worker(role, {1, 2});
    TrustedNegotiator trusted(role, {1, 1}, kGeneration, payload_cap(role), 4);
    const auto worker_hello = worker.make_hello();
    OMARCHY_CHECK(worker_hello &&
                worker_hello.header.envelope_version == kEnvelopeVersion &&
                worker_hello.header.header_size == kHeaderSize &&
                worker_hello.header.lane_sequence == 0);
    const auto hello_bytes = encode(worker_hello.header, worker_hello.payload);
    const auto hello = decode_packet(hello_bytes, role);
    OMARCHY_CHECK(static_cast<bool>(hello));
    const auto reply = trusted.accept_hello(hello.packet);
    OMARCHY_CHECK(static_cast<bool>(reply) &&
                reply.kind == NegotiationKind::welcome && trusted.selected());
    const auto reply_bytes = encode_negotiation(reply);
    const auto decoded_reply = decode_packet(reply_bytes, role);
    OMARCHY_CHECK(static_cast<bool>(decoded_reply) &&
                worker.accept_reply(decoded_reply.packet) == FatalReason::none);
    OMARCHY_CHECK(readiness.observe(role, worker.launch_generation()) ==
                FatalReason::none);
  }
  bool ready = false;
  OMARCHY_CHECK(readiness.ready(ready) == FatalReason::none && ready);

  WorkerNegotiator duplicate_worker(EndpointRole::control, {1, 1});
  const auto first_hello = duplicate_worker.make_hello();
  OMARCHY_CHECK(static_cast<bool>(first_hello) &&
              duplicate_worker.make_hello().error ==
                  FatalReason::invalid_message_order &&
              duplicate_worker.failed());

  RequiredEndpointReadiness mismatched;
  OMARCHY_CHECK(mismatched.observe(EndpointRole::control, kGeneration) ==
                  FatalReason::none &&
              mismatched.observe(EndpointRole::broker, kGeneration) ==
                  FatalReason::none &&
              mismatched.observe(EndpointRole::render, kGeneration + 1) ==
                  FatalReason::none &&
              mismatched.ready(ready) ==
                  FatalReason::readiness_generation_mismatch);

  for (const auto role :
       {EndpointRole::control, EndpointRole::broker, EndpointRole::render}) {
    WorkerNegotiator worker(role, {2, 3});
    TrustedNegotiator trusted(role, {1, 1}, kGeneration, payload_cap(role), 4);
    const auto worker_hello = worker.make_hello();
    OMARCHY_CHECK(static_cast<bool>(worker_hello));
    const auto hello_bytes = encode(worker_hello.header, worker_hello.payload);
    const auto hello = decode_packet(hello_bytes, role);
    const auto failure = trusted.accept_hello(hello.packet);
    OMARCHY_CHECK(failure.kind == NegotiationKind::negotiation_failed &&
                trusted.failed());
    const auto failure_bytes = encode_negotiation(failure);
    const auto decoded_failure = decode_packet(failure_bytes, role);
    OMARCHY_CHECK(worker.accept_reply(decoded_failure.packet) ==
                FatalReason::version_negotiation_failed);
  }

}

PacketView decode_selected(const std::vector<std::byte> &bytes,
                           EndpointRole role = EndpointRole::broker) {
  const auto result = decode_packet(bytes, role);
  OMARCHY_CHECK(static_cast<bool>(result));
  return result.packet;
}

void state_test() {
  constexpr std::array rules{
      MessageRule{kRequestType, DirectionMask::bidirectional,
                  CorrelationRule::nonzero, MessageSemantic::request, 0, 8},
      MessageRule{kResponseType, DirectionMask::bidirectional,
                  CorrelationRule::nonzero, MessageSemantic::terminal, 0, 8},
      MessageRule{kEventType, DirectionMask::worker_to_host,
                  CorrelationRule::zero, MessageSemantic::event, 0, 4},
  };
  const std::array schemas{
      RoleSchemaView{EndpointRole::broker, 1, rules, 2, 2},
  };
  const RoleSchemaRegistryView registry(schemas);
  OMARCHY_CHECK(registry.validate() == FatalReason::none);

  const auto request =
      encode(selected_header(EndpointRole::broker, kRequestType, kCorrelation));
  const auto response =
      encode(selected_header(EndpointRole::broker, kResponseType, kCorrelation));
  const auto common = [](CommonMessageType type, std::span<const std::byte> payload = {}) {
    return encode(selected_header(EndpointRole::broker,
                                  static_cast<std::uint16_t>(type), kCorrelation),
                  payload);
  };
  const std::array<std::byte, 2> error_payload{std::byte{0}, std::byte{1}};
  const auto typed_error = common(CommonMessageType::typed_error, error_payload);
  const auto unknown_packet =
      encode(selected_header(EndpointRole::broker, 0x1fff, 0));
  const std::array<std::byte, 3> oversized_payload{};
  const auto oversized = encode(
      selected_header(EndpointRole::broker, kEventType), oversized_payload);

  struct Step {
    const std::vector<std::byte> &packet;
    Direction direction;
    SessionAction action = SessionAction::none;
    FatalReason error = FatalReason::none;
  };
  const auto run = [&](std::string_view name, std::initializer_list<Step> steps,
                       std::uint32_t cap = payload_cap(EndpointRole::broker)) {
    SelectedEndpointState<4> state(EndpointRole::broker, 1, kGeneration, cap, 4,
                                  registry);
    std::size_t index = 0;
    for (const auto &step : steps) {
      const auto result = state.accept(decode_selected(step.packet), step.direction);
      require(result.error == step.error && result.action == step.action &&
                  state.failed() == (step.error != FatalReason::none),
              std::string(name) + " step " + std::to_string(index++));
    }
  };
  using enum SessionAction;
  constexpr auto inbound = Direction::worker_to_host;
  constexpr auto outbound = Direction::host_to_worker;
  auto stale_header = selected_header(EndpointRole::broker, kRequestType, kCorrelation);
  ++stale_header.launch_generation;
  const auto stale = encode(stale_header);
  run("stale generation", {{stale, inbound, none, FatalReason::stale_generation}});
  run("invalid direction",
      {{request, static_cast<Direction>(0xff), none, FatalReason::invalid_direction}});
  run("bidirectional correlation and reuse",
      {{request, inbound, request_admitted},
       {request, outbound, request_admitted},
       {request, inbound, none, FatalReason::correlation_reused}});
  run("terminal releases correlation and duplicate terminal is fatal",
      {{request, inbound, request_admitted},
       {response, outbound, terminal_received},
       {request, inbound, request_admitted},
       {response, outbound, terminal_received},
       {response, outbound, none, FatalReason::unmatched_terminal}});
  run("bidirectional terminals release only their initiating lane",
      {{request, inbound, request_admitted}, {request, outbound, request_admitted},
       {response, inbound, terminal_received}, {request, outbound, request_admitted},
       {request, inbound, none, FatalReason::correlation_reused}});
  for (const auto type : {CommonMessageType::cancel, CommonMessageType::cancel_result}) {
    for (const auto direction : {inbound, outbound}) {
      for (const std::vector<std::byte> &payload : {
               std::vector<std::byte>{}, {std::byte{0}},
               {std::byte{0}, std::byte{1}}, {std::byte{0}, std::byte{2}},
               {std::byte{0}, std::byte{3}}, {std::byte{0}, std::byte{4}},
               {std::byte{0}, std::byte{0}}, {std::byte{0xff}, std::byte{0xff}}}) {
        const auto retired = common(type, payload);
        const Step rejected{retired, direction, none, FatalReason::invalid_message_order};
        run("generic cancellation without request is fatal", {rejected});
        run("generic cancellation with request is fatal",
            {{request, inbound, request_admitted}, rejected,
             {request, outbound, none, FatalReason::invalid_message_order}});
        run("generic cancellation after terminal is fatal",
            {{request, inbound, request_admitted},
             {response, outbound, terminal_received}, rejected});
      }
    }
  }
  run("typed error",
      {{request, inbound, request_admitted},
       {typed_error, outbound, recoverable_error_received}});
  run("unknown message",
      {{unknown_packet, inbound, none, FatalReason::unknown_message_type}});
  run("negotiated payload limit",
      {{oversized, inbound, none, FatalReason::payload_cap_exceeded}}, 2);

  SelectedEndpointState<1> bounded(EndpointRole::broker, 1, kGeneration,
                                   payload_cap(EndpointRole::broker), 1,
                                   registry);
  OMARCHY_CHECK(static_cast<bool>(bounded.accept(decode_selected(request),
                                               Direction::worker_to_host)));
  const auto second =
      encode(selected_header(EndpointRole::broker, kRequestType, 2));
  OMARCHY_CHECK(bounded.accept(decode_selected(second), Direction::worker_to_host)
                  .error == FatalReason::maximum_in_flight_exceeded);
}

void canonical_surface_binding_test() {
  const auto first = manifest_surface_binding("bar", 0, 17);
  const auto third = manifest_surface_binding("overlay", 2, 17);
  OMARCHY_CHECK(first && first->id == 1 && first->generation == 17 &&
              third && third->id == 3 && third->generation == 17 &&
              !manifest_surface_binding("bar", 0, 0) &&
              !manifest_surface_binding("bad/name", 0, 17) &&
              !manifest_surface_binding("bar", kMaximumPluginSurfaces, 17));
}

void permission_snapshot_test() {
  namespace snapshot_wire = omarchy::plugin::wire::permission_snapshot;
  using snapshot_wire::GrantState;
  using snapshot_wire::PermissionRow;
  using snapshot_wire::PermissionSnapshot;

  const PermissionSnapshot source{
      .manifest_request_fingerprint = std::string(64, 'a'),
      .permissions = {{GrantState::granted, 0x0005},
                      {GrantState::denied, 0x0000},
                      {GrantState::revoked, 0xabcd}}};
  const auto golden = snapshot_wire::encode(source);
  OMARCHY_CHECK(hex(golden) ==
              "0001616161616161616161616161616161616161616161616161616161616161"
              "6161616161616161616161616161616161616161616161616161616161616161"
              "6161000301000502000003abcd");
  PermissionSnapshot decoded;
  OMARCHY_CHECK(snapshot_wire::decode(golden, decoded) && decoded == source);

  const PermissionSnapshot sentinel{.manifest_request_fingerprint =
                                        std::string(64, 'b'),
                                    .permissions = {
                                        {GrantState::revoked, 0x1234}}};
  const auto rejects = [&](std::span<const std::byte> bytes,
                           std::string_view message) {
    auto output = sentinel;
    require(!snapshot_wire::decode(bytes, output) && output == sentinel,
            message);
  };

  for (std::size_t length = 0; length < golden.size(); ++length)
    rejects(std::span(golden).first(length),
            "permission snapshot truncation was accepted");

  auto malformed = golden;
  malformed[66] = std::byte{1};
  malformed[67] = std::byte{1};
  rejects(malformed, "oversized permission request count was accepted");
  malformed = golden;
  malformed[67] = std::byte{2};
  rejects(malformed, "permission snapshot trailing byte was accepted");

  for (unsigned int value = 0; value <= 0xff; ++value) {
    malformed = golden;
    malformed[68] = static_cast<std::byte>(value);
    if (value == static_cast<unsigned int>(GrantState::denied)) {
      malformed[69] = std::byte{0};
      malformed[70] = std::byte{0};
    }
    auto state = sentinel;
    const bool accepted = snapshot_wire::decode(malformed, state);
    const bool valid =
        value >= static_cast<unsigned int>(GrantState::granted) &&
        value <= static_cast<unsigned int>(GrantState::revoked);
    OMARCHY_CHECK(accepted == valid &&
                (valid ? snapshot_wire::encode(state) == malformed
                       : state == sentinel));
  }

  PermissionSnapshot empty{.manifest_request_fingerprint = std::string(64, '0'),
                           .permissions = {}};
  const auto empty_encoded = snapshot_wire::encode(empty);
  OMARCHY_CHECK(empty_encoded.size() == snapshot_wire::kFixedPayloadBytes &&
              snapshot_wire::decode(empty_encoded, decoded) && decoded == empty);

  PermissionSnapshot maximum{
      .manifest_request_fingerprint = std::string(64, 'f'),
      .permissions = std::vector<PermissionRow>(
          snapshot_wire::kMaximumManifestRequests,
          {GrantState::denied, 0})};
  const auto maximum_encoded = snapshot_wire::encode(maximum);
  OMARCHY_CHECK(maximum_encoded.size() == snapshot_wire::kMaximumPayloadBytes &&
              snapshot_wire::decode(maximum_encoded, decoded) &&
              decoded == maximum);
  maximum.permissions.push_back({GrantState::denied, 0});
  OMARCHY_CHECK(snapshot_wire::encode(maximum).empty());
  auto too_large = maximum_encoded;
  too_large.push_back(std::byte{0});
  rejects(too_large, "decoder exceeded the manifest request bound");

  auto invalid_source = source;
  invalid_source.manifest_request_fingerprint[0] = 'A';
  OMARCHY_CHECK(snapshot_wire::encode(invalid_source).empty());
  invalid_source = source;
  invalid_source.permissions[0].state = static_cast<GrantState>(0);
  OMARCHY_CHECK(snapshot_wire::encode(invalid_source).empty());
  invalid_source = source;
  invalid_source.permissions[1].operation_mask = 1;
  OMARCHY_CHECK(snapshot_wire::encode(invalid_source).empty());
  invalid_source = source;
  invalid_source.permissions[0].operation_mask = 0;
  OMARCHY_CHECK(snapshot_wire::encode(invalid_source).empty());
  invalid_source = source;
  invalid_source.permissions[2].operation_mask = 0;
  OMARCHY_CHECK(snapshot_wire::encode(invalid_source).empty());

  const auto verifies = [&](std::span<const std::byte> bytes,
                            bool expected_valid, std::string_view message) {
    auto output = sentinel;
    const bool accepted = snapshot_wire::decode(bytes, output);
    require(accepted == expected_valid &&
                (accepted
                     ? std::ranges::equal(snapshot_wire::encode(output), bytes)
                     : output == sentinel),
            message);
  };

  for (unsigned version = 0; version <= 0xffff; ++version) {
    auto bytes = golden;
    bytes[0] = static_cast<std::byte>(version >> 8);
    bytes[1] = static_cast<std::byte>(version);
    verifies(bytes, version == 1, "codec version classification changed");
  }
  constexpr std::string_view digits = "0123456789abcdef";
  for (std::size_t index = 0; index < 64; ++index) {
    for (unsigned value = 0; value <= 0xff; ++value) {
      auto bytes = golden;
      bytes[2 + index] = static_cast<std::byte>(value);
      verifies(bytes, digits.find(static_cast<char>(value)) != digits.npos,
               "fingerprint byte classification changed");
    }
  }
  for (unsigned state = 1; state <= 3; ++state) {
    const auto offset = 68 + (state - 1) * 3;
    for (unsigned mask = 0; mask <= 0xffff; ++mask) {
      auto bytes = golden;
      bytes[offset + 1] = static_cast<std::byte>(mask >> 8);
      bytes[offset + 2] = static_cast<std::byte>(mask);
      verifies(bytes, (state == 2) == (mask == 0),
               "permission state/mask classification changed");
    }
  }
  for (std::size_t count = 0;
       count <= snapshot_wire::kMaximumManifestRequests; ++count) {
    auto bytes = golden;
    bytes.resize(snapshot_wire::kFixedPayloadBytes +
                 count * snapshot_wire::kPermissionRowBytes);
    bytes[66] = static_cast<std::byte>(count >> 8);
    bytes[67] = static_cast<std::byte>(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto offset = snapshot_wire::kFixedPayloadBytes +
                          index * snapshot_wire::kPermissionRowBytes;
      const auto state = index % 3 + 1;
      const auto mask = state == 2 ? 0 : (index + 1) * 257;
      bytes[offset] = static_cast<std::byte>(state);
      bytes[offset + 1] = static_cast<std::byte>(mask >> 8);
      bytes[offset + 2] = static_cast<std::byte>(mask);
    }
    verifies(bytes, true, "bounded count did not round trip");
    for (std::size_t index = 0; index < count; ++index) {
      const auto offset = snapshot_wire::kFixedPayloadBytes +
                          index * snapshot_wire::kPermissionRowBytes;
      const auto saved = bytes[offset];
      for (const auto invalid : {std::byte{0}, std::byte{0xff}}) {
        bytes[offset] = invalid;
        verifies(bytes, false, "invalid row state was accepted");
      }
      bytes[offset] = saved;
    }
    auto truncated = bytes;
    truncated.pop_back();
    verifies(truncated, false, "mismatched count truncation was accepted");
    bytes.push_back(std::byte{1});
    verifies(bytes, false, "mismatched count suffix was accepted");
  }
  for (unsigned suffix = 1; suffix <= 16; ++suffix) {
    auto bytes = golden;
    bytes.resize(golden.size() + suffix, std::byte{1});
    verifies(bytes, false, "structured suffix was accepted");
  }
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    byte_order_goldens();
    golden_test();
    control_schema_test();
    envelope_and_sequence_test();
    envelope_test();
    negotiation_test();
    state_test();
    canonical_surface_binding_test();
    permission_snapshot_test();
    std::cout << "plugin wire contract: PASS\n";
    return 0;
  }, "plugin wire contract: ");
}
