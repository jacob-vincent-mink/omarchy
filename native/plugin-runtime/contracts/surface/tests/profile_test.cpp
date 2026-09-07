#include "test.hpp"

#include "omarchy/plugin/wire/state.hpp"
#include "omarchy/plugin_runtime/surface/profile.hpp"
#include "omarchy/plugin_runtime/surface/render_messages.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace omarchy::plugin_runtime::surface;

int main() {
  const auto check_golden = [](std::span<const std::byte> bytes, std::string_view expected) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string actual;
    for (const auto byte : bytes) {
      const auto value = std::to_integer<unsigned>(byte);
      actual += digits[value >> 4];
      actual += digits[value & 15];
    }
    OMARCHY_CHECK_WITH(require, actual == expected);
  };
  // Independent packed-wire oracle, including signed coordinates. Do not
  // derive expected bytes through the production codec.
  constexpr std::array<unsigned char, 16> region_golden{
      0xff, 0xff, 0xff, 0xff, 0x80, 0, 0, 0,
      0, 0, 0, 1, 0, 0, 1, 0};
  const TransportedInputRegion signed_region{-1, INT32_MIN, 1, 256};
  std::array<std::byte, InputRegionLayout::size> signed_bytes{};
  InputRegionLayout::encode(signed_region, signed_bytes);
  OMARCHY_CHECK_WITH(require,
      std::ranges::equal(signed_bytes, std::as_bytes(std::span(region_golden))));
  OMARCHY_CHECK_WITH(require,
      InputRegionLayout::decode(std::as_bytes(std::span(region_golden))) == signed_region);
  constexpr std::array<unsigned char, 16> surface_golden{
      1, 2, 3, 4, 5, 6, 7, 8, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
  const SurfaceKey golden_key{0x0102030405060708, 0x1112131415161718};
  OMARCHY_CHECK_WITH(require, std::ranges::equal(encode_surface_key(golden_key),
      std::as_bytes(std::span(surface_golden))));
  SurfaceKey decoded_key{};
  OMARCHY_CHECK_WITH(require, decode_surface_key(std::as_bytes(std::span(surface_golden)), decoded_key) &&
      decoded_key == golden_key);
  OMARCHY_CHECK_WITH(require, valid_surface_name("barWidget") &&
              valid_surface_name(std::string(64, 'X')) &&
              !valid_surface_name("") &&
              !valid_surface_name("Panel.Widget") &&
              !valid_surface_name(std::string("bad\0name", 8)) &&
              !valid_surface_name(std::string(65, 'a')));
  const auto offer = software_profile_offer();
  const auto selection = select_software_profile(kSoftwareProfileVersion);
  OMARCHY_CHECK_WITH(require, selection && selection->version == kSoftwareProfileVersion &&
              selection->pixel_format == kRgba8888Premultiplied);
  for (const auto version : {0U, 2U, 3U, 9U, UINT32_MAX})
    OMARCHY_CHECK_WITH(require, !select_software_profile(version));

  namespace wire = omarchy::plugin::wire;
  const auto schema = render_role_schema();
  const std::array schemas{schema};
  const wire::RoleSchemaRegistryView registry(schemas);
  OMARCHY_CHECK_WITH(require, registry.validate() == wire::FatalReason::none);
  constexpr std::array known_types{0x2000, 0x2001, 0x2010, 0x2011, 0x2012,
                                   0x2013, 0x2014, 0x2020, 0x2021, 0x2030, 0x2040};
  constexpr std::array<std::uint32_t, 11> rule_minimum_bytes{24, 8, 96, 16, 16, 16, 16, 40, 288, 32, 176};
  constexpr std::array<std::uint32_t, 11> rule_maximum_bytes{24, 8, 96, 16, 16, 16, 16, 40, 288, 204, 176};
  for (std::size_t index = 0; index < known_types.size(); ++index) {
    const auto *rule = wire::find_message(schema, known_types[index]);
    OMARCHY_CHECK_WITH(require, rule && rule->minimum_payload == rule_minimum_bytes[index] &&
        rule->maximum_payload == rule_maximum_bytes[index]);
  }
  for (std::uint32_t type = 0; type <= UINT16_MAX; ++type) {
    std::optional<std::uint8_t> expected;
    if (std::ranges::find(known_types, type) != known_types.end())
      expected = type == 0x2010 ? 1 : 0;
    OMARCHY_CHECK_WITH(require, render_descriptor_count(static_cast<std::uint16_t>(type)) == expected);
  }
  const auto *allocation_rule = wire::find_message(
      schema, static_cast<std::uint16_t>(RenderMessageType::surface_allocate));
  OMARCHY_CHECK_WITH(require, allocation_rule &&
              allocation_rule->semantic == wire::MessageSemantic::request &&
              allocation_rule->correlation == wire::CorrelationRule::nonzero &&
              render_descriptor_count(allocation_rule->message_type) == 1);
  const auto *frame_rule = wire::find_message(
      schema, static_cast<std::uint16_t>(RenderMessageType::frame_ready));
  OMARCHY_CHECK_WITH(require, frame_rule && frame_rule->semantic == wire::MessageSemantic::event &&
              frame_rule->correlation == wire::CorrelationRule::zero &&
              render_descriptor_count(frame_rule->message_type) == 0);
  const auto *intent_rule = wire::find_message(
      schema, static_cast<std::uint16_t>(RenderMessageType::surface_intent));
  OMARCHY_CHECK_WITH(require, intent_rule &&
              intent_rule->directions == wire::DirectionMask::worker_to_host &&
              intent_rule->semantic == wire::MessageSemantic::event &&
              intent_rule->correlation == wire::CorrelationRule::zero &&
              render_descriptor_count(intent_rule->message_type) == 0);

  const auto offer_bytes = encode_profile_offer(offer);
  check_golden(offer_bytes, "000000010000000100001000000000030000000004000000");
  OMARCHY_CHECK_WITH(require, offer_bytes[0] == std::byte{0} && offer_bytes[3] == std::byte{1} &&
              offer_bytes[15] == std::byte{3});
  ProfileOffer decoded_offer{};
  OMARCHY_CHECK_WITH(require, decode_profile_offer(offer_bytes, decoded_offer) &&
              decoded_offer.version == offer.version);
  for (unsigned bit = 0; bit < 32; ++bit) {
    auto bad_offer = offer_bytes;
    bad_offer[12 + bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
    OMARCHY_CHECK_WITH(require, !decode_profile_offer(bad_offer, decoded_offer));
  }

  const auto allocation =
      make_allocation({.id = 22, .generation = 3}, 8, 4, 8, 4, 1, 1, 4096);
  OMARCHY_CHECK_WITH(require, allocation.has_value());
  const auto allocation_bytes = encode_surface_allocation(*allocation);
  check_golden(allocation_bytes,
      "0000000000000016000000000000000300000001000000010000000800000004"
      "0000000800000004000000010000000100000020000000020000008000000000"
      "0000000000000080000000000000100000000000000020000000000000004000");
  TrustedAllocation decoded_allocation{};
  OMARCHY_CHECK_WITH(require,
      decode_surface_allocation(allocation_bytes, 4096, decoded_allocation) &&
          decoded_allocation == *allocation);
  auto bad_allocation = allocation_bytes;
  bad_allocation[63] = std::byte{1};
  OMARCHY_CHECK_WITH(require, !decode_surface_allocation(bad_allocation, 4096, decoded_allocation));
  // Mutate every byte of each fixed field, reserved field and derived size.
  for (const auto start : {16, 20, 48, 52, 56, 60, 64, 72, 80, 88}) {
    const auto width = start >= 64 ? 8 : 4;
    for (int index = 0; index < width; ++index) {
      auto malformed = allocation_bytes;
      malformed[start + index] ^= std::byte{1};
      OMARCHY_CHECK_WITH(require, !decode_surface_allocation(malformed, 4096, decoded_allocation));
    }
  }

  const FrameReady frame{.surface = allocation->surface,
                         .slot = 1,
                         .slot_sequence = 4,
                         .frame_sequence = 9};
  const auto frame_bytes = encode_frame_ready(frame);
  check_golden(frame_bytes,
      "0000000000000016000000000000000300000001000000000000000000000004"
      "0000000000000009");
  FrameReady decoded_frame{};
  OMARCHY_CHECK_WITH(require, decode_frame_ready(frame_bytes, decoded_frame) &&
              decoded_frame.surface == frame.surface &&
              decoded_frame.slot == frame.slot &&
              decoded_frame.slot_sequence == frame.slot_sequence &&
              decoded_frame.frame_sequence == frame.frame_sequence);
  for (int index = 20; index < 24; ++index) {
    auto malformed = frame_bytes;
    malformed[index] = std::byte{1};
    OMARCHY_CHECK_WITH(require, !decode_frame_ready(malformed, decoded_frame));
  }

  InputRegionUpdate regions{.surface = allocation->surface,
                            .generation = 3,
                            .regions = {{{.x = 20, .y = 30,
                                          .width = 80, .height = 40}}},
                            .count = 1};
  const auto region_bytes = encode_input_region_update(regions);
  InputRegionUpdate decoded_regions{};
  OMARCHY_CHECK_WITH(require, decode_input_region_update(region_bytes, decoded_regions) &&
              decoded_regions.surface == regions.surface &&
              decoded_regions.generation == 3 &&
              decoded_regions.count == 1 &&
              decoded_regions.regions[0] == regions.regions[0]);
  auto malformed_regions = region_bytes;
  malformed_regions[31] = std::byte{1};
  OMARCHY_CHECK_WITH(require, !decode_input_region_update(malformed_regions, decoded_regions));
  regions.count = kMaximumTransportedInputRegions + 1;
  OMARCHY_CHECK_WITH(require, !decode_input_region_update(encode_input_region_update(regions),
                                      decoded_regions));

  const InputEvent input{.surface = allocation->surface,
                         .sequence = 10,
                         .payload = PointerMotion{
                             .position = {.x_q16 = 1U << 16,
                                          .y_q16 = 2U << 16}}};
  const auto input_bytes = encode_input_event(input);
  InputEvent decoded_input{};
  OMARCHY_CHECK_WITH(require, input_bytes && decode_input_event(*input_bytes, decoded_input) &&
              decoded_input == input);
  check_golden(std::span(*input_bytes).first(32),
      "00000000000000160000000000000003000000000000000a0000000100000000");
  for (int index = 28; index < 32; ++index) {
    auto malformed = *input_bytes;
    malformed[index] = std::byte{1};
    OMARCHY_CHECK_WITH(require, !decode_input_event(malformed, decoded_input));
  }
  auto bad_input = input_bytes;
  (*bad_input)[27] = std::byte{99};
  OMARCHY_CHECK_WITH(require, !decode_input_event(*bad_input, decoded_input));

  const InputEvent focus{.surface = allocation->surface,
                         .sequence = 11,
                         .payload = FocusChanged{.focused = true}};
  const auto focus_bytes = encode_input_event(focus);
  InputEvent decoded_focus{};
  OMARCHY_CHECK_WITH(require, focus_bytes && decode_input_event(*focus_bytes, decoded_focus) &&
              decoded_focus == focus);
  auto bad_focus = focus_bytes;
  (*bad_focus)[35] = std::byte{2};
  OMARCHY_CHECK_WITH(require, !decode_input_event(*bad_focus, decoded_focus));

  InputEvent key{.surface = allocation->surface,
                 .sequence = 12,
                 .payload = Key{.key = 65,
                                .native_scan_code = 30,
                                .state = ButtonState::pressed,
                                .text = "a"}};
  auto key_bytes = encode_input_event(key);
  OMARCHY_CHECK_WITH(require, key_bytes && decode_input_event(*key_bytes, decoded_input) &&
              decoded_input == key);
  key_bytes->back() = std::byte{0xc0};
  OMARCHY_CHECK_WITH(require, !decode_input_event(*key_bytes, decoded_input));
  std::get<Key>(key.payload).text = std::string("\xed\xa0\x80", 3);
  OMARCHY_CHECK_WITH(require, !encode_input_event(key));

  InputEvent invalid_mask{
      .surface = allocation->surface,
      .sequence = 13,
      .payload = PointerMotion{.position = {}, .buttons = 0x20}};
  OMARCHY_CHECK_WITH(require, !encode_input_event(invalid_mask));
  auto malformed_mask = input_bytes;
  (*malformed_mask)[43] = std::byte{0x20};
  OMARCHY_CHECK_WITH(require, !decode_input_event(*malformed_mask, decoded_input));

  TouchFrame maximum{.phase = TouchFramePhase::begin,
                     .count = kMaximumTouchPoints};
  for (std::uint32_t index = 0; index < maximum.count; ++index)
    maximum.points[index] = {.id = index,
                             .state = TouchPointState::pressed,
                             .position = {index << 16, index << 16}};
  const InputEvent maximum_touch{.surface = allocation->surface,
                                 .sequence = 14,
                                 .payload = maximum};
  const auto maximum_bytes = encode_input_event(maximum_touch);
  OMARCHY_CHECK_WITH(require, maximum_bytes && maximum_bytes->size() == 204 &&
              decode_input_event(*maximum_bytes, decoded_input) &&
              decoded_input == maximum_touch);
  maximum.points[1].id = maximum.points[0].id;
  OMARCHY_CHECK_WITH(require, !encode_input_event({.surface = allocation->surface,
                               .sequence = 15,
                               .payload = maximum}));

  const InputEvent invalid_replacement{
      .surface = allocation->surface,
      .sequence = 16,
      .payload = TextCommit{
          .text = "x",
          .replacement_start = kMaximumTextReplacementOffset + 1}};
  OMARCHY_CHECK_WITH(require, !encode_input_event(invalid_replacement));

  const SurfaceIntentRequest intent{
      .source = allocation->surface,
      .target = {.id = 23, .generation = allocation->surface.generation},
      .input_sequence = 12,
      .action = SurfaceIntentAction::toggle,
      .requested_output = "DP-1"};
  const auto intent_bytes = encode_surface_intent(intent);
  check_golden(std::span(intent_bytes).first(52),
      "0000000000000016000000000000000300000000000000170000000000000003"
      "000000000000000c000000020004000044502d31");
  SurfaceIntentRequest decoded_intent{};
  OMARCHY_CHECK_WITH(require, decode_surface_intent(intent_bytes, decoded_intent) &&
              decoded_intent == intent);
  auto bad_intent = intent_bytes;
  bad_intent[47] = std::byte{1};
  OMARCHY_CHECK_WITH(require, !decode_surface_intent(bad_intent, decoded_intent));
  bad_intent = intent_bytes;
  bad_intent[43] = std::byte{9};
  OMARCHY_CHECK_WITH(require, !decode_surface_intent(bad_intent, decoded_intent));
  auto invalid_output_intent = intent;
  invalid_output_intent.requested_output = std::string(129, 'x');
  OMARCHY_CHECK_WITH(require, !decode_surface_intent(encode_surface_intent(invalid_output_intent),
                                 decoded_intent));
  const SurfaceIntentRequest dismiss_intent{
      .source = allocation->surface,
      .target = allocation->surface,
      .input_sequence = 0,
      .action = SurfaceIntentAction::dismiss,
      .requested_output = {}};
  const auto dismiss_bytes = encode_surface_intent(dismiss_intent);
  OMARCHY_CHECK_WITH(require, decode_surface_intent(dismiss_bytes, decoded_intent) &&
              decoded_intent == dismiss_intent);
  auto forged_dismiss = dismiss_bytes;
  forged_dismiss[32] = std::byte{1};
  OMARCHY_CHECK_WITH(require, !decode_surface_intent(forged_dismiss, decoded_intent));

  const RenderTypedError error{
      .reason = RenderErrorReason::invalid_allocation,
      .failed_message_type =
          static_cast<std::uint16_t>(RenderMessageType::surface_allocate),
      .surface = allocation->surface};
  const auto error_bytes = encode_render_error(error);
  check_golden(error_bytes, "000220100000000000000000000000160000000000000003");
  RenderTypedError decoded_error{};
  OMARCHY_CHECK_WITH(require, decode_render_error(error_bytes, decoded_error) &&
              decoded_error.reason == error.reason);
  for (int index = 4; index < 8; ++index) {
    auto malformed = error_bytes;
    malformed[index] = std::byte{1};
    OMARCHY_CHECK_WITH(require, !decode_render_error(malformed, decoded_error));
  }
  auto nonrequest_error = error_bytes;
  nonrequest_error[2] = std::byte{0x20};
  nonrequest_error[3] = std::byte{0x20};
  OMARCHY_CHECK_WITH(require, !decode_render_error(nonrequest_error, decoded_error));

  wire::SelectedEndpointState<4> endpoint(
      wire::EndpointRole::render, kRenderRoleVersion, 7,
      wire::payload_cap(wire::EndpointRole::render), 4, registry);
  wire::PacketView offer_packet{
      .header = {.endpoint_role = wire::EndpointRole::render,
                 .message_type = static_cast<std::uint16_t>(
                     RenderMessageType::profile_offer),
                 .role_protocol_version = kRenderRoleVersion,
                 .payload_length = offer_bytes.size(),
                 .launch_generation = 7,
                 .correlation_id = 55},
      .payload = offer_bytes};
  OMARCHY_CHECK_WITH(require,
      endpoint.accept(offer_packet, wire::Direction::host_to_worker).action ==
          wire::SessionAction::request_admitted);
  const auto selection_bytes = encode_profile_selection(*selection);
  auto selection_packet = offer_packet;
  selection_packet.header.message_type =
      static_cast<std::uint16_t>(RenderMessageType::profile_select);
  selection_packet.header.payload_length = selection_bytes.size();
  selection_packet.payload = selection_bytes;
  OMARCHY_CHECK_WITH(require, endpoint.accept(selection_packet, wire::Direction::worker_to_host)
                  .action == wire::SessionAction::terminal_received);
}
