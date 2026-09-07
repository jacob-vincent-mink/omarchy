#include "omarchy/plugin_runtime/surface/render_messages.hpp"
#include "omarchy/plugin_runtime/big_endian.hpp"

#include <algorithm>

namespace omarchy::plugin_runtime::surface {
namespace {

namespace wire = omarchy::plugin::wire;

constexpr std::array<wire::MessageRule, 11> kWireRules{{
    {static_cast<std::uint16_t>(RenderMessageType::profile_offer),
     wire::DirectionMask::host_to_worker, wire::CorrelationRule::nonzero,
     wire::MessageSemantic::request, ProfileOfferLayout::size, ProfileOfferLayout::size},
    {static_cast<std::uint16_t>(RenderMessageType::profile_select),
     wire::DirectionMask::worker_to_host, wire::CorrelationRule::nonzero,
     wire::MessageSemantic::terminal, ProfileSelectionLayout::size, ProfileSelectionLayout::size},
    {static_cast<std::uint16_t>(RenderMessageType::surface_allocate),
     wire::DirectionMask::host_to_worker, wire::CorrelationRule::nonzero,
     wire::MessageSemantic::request, AllocationLayout::size, AllocationLayout::size},
    {static_cast<std::uint16_t>(RenderMessageType::surface_allocated),
     wire::DirectionMask::worker_to_host, wire::CorrelationRule::nonzero,
     wire::MessageSemantic::terminal, SurfaceKeyLayout::size, SurfaceKeyLayout::size},
    {static_cast<std::uint16_t>(RenderMessageType::surface_release),
     wire::DirectionMask::host_to_worker, wire::CorrelationRule::zero,
     wire::MessageSemantic::one_way, SurfaceKeyLayout::size, SurfaceKeyLayout::size},
    {static_cast<std::uint16_t>(RenderMessageType::surface_suspend),
     wire::DirectionMask::host_to_worker, wire::CorrelationRule::zero,
     wire::MessageSemantic::one_way, SurfaceKeyLayout::size, SurfaceKeyLayout::size},
    {static_cast<std::uint16_t>(RenderMessageType::surface_resume),
     wire::DirectionMask::host_to_worker, wire::CorrelationRule::zero,
     wire::MessageSemantic::one_way, SurfaceKeyLayout::size, SurfaceKeyLayout::size},
    {static_cast<std::uint16_t>(RenderMessageType::frame_ready),
     wire::DirectionMask::worker_to_host, wire::CorrelationRule::zero,
     wire::MessageSemantic::event, FrameReadyLayout::size, FrameReadyLayout::size},
    {static_cast<std::uint16_t>(RenderMessageType::input_regions),
     wire::DirectionMask::worker_to_host, wire::CorrelationRule::zero,
     wire::MessageSemantic::event, kInputRegionUpdateBytes, kInputRegionUpdateBytes},
    {static_cast<std::uint16_t>(RenderMessageType::input),
     wire::DirectionMask::host_to_worker, wire::CorrelationRule::zero,
     wire::MessageSemantic::one_way, InputEventHeaderLayout::size, kMaximumInputEventBytes},
    {static_cast<std::uint16_t>(RenderMessageType::surface_intent),
     wire::DirectionMask::worker_to_host, wire::CorrelationRule::zero,
     wire::MessageSemantic::event, kSurfaceIntentBytes, kSurfaceIntentBytes},
}};

using omarchy::plugin_runtime::big_endian::get;

bool valid_render_error(RenderErrorReason reason) {
  switch (reason) {
  case RenderErrorReason::unsupported_profile:
  case RenderErrorReason::invalid_allocation:
  case RenderErrorReason::resource_limit:
  case RenderErrorReason::stale_surface:
    return true;
  }
  return false;
}

} // namespace

wire::RoleSchemaView render_role_schema() {
  return {.role = wire::EndpointRole::render,
          .version = kRenderRoleVersion,
          .messages = kWireRules,
          .typed_error_minimum_payload = RenderErrorLayout::size,
          .typed_error_maximum_payload = RenderErrorLayout::size};
}

std::optional<std::uint8_t>
render_descriptor_count(std::uint16_t message_type) {
  if (!wire::find_message(render_role_schema(), message_type)) {
    return std::nullopt;
  }
  return message_type == static_cast<std::uint16_t>(RenderMessageType::surface_allocate)
             ? 1 : 0;
}

std::array<std::byte, ProfileOfferLayout::size> encode_profile_offer(const ProfileOffer &payload) {
  std::array<std::byte, ProfileOfferLayout::size> output{};
  ProfileOfferLayout::encode(payload, output);
  return output;
}

bool decode_profile_offer(std::span<const std::byte> bytes,
                          ProfileOffer &output) {
  if (bytes.size() != ProfileOfferLayout::size || !ProfileOfferLayout::matches_constants(bytes)) {
    return false;
  }
  output = ProfileOfferLayout::decode(bytes);
  return output.version == kSoftwareProfileVersion &&
         output.maximum_pixel_dimension <= kMaximumPixelDimension &&
         output.maximum_pixel_dimension != 0 &&
         output.maximum_frame_bytes <= kMaximumFrameBytes &&
         output.maximum_frame_bytes != 0;
}

std::array<std::byte, ProfileSelectionLayout::size>
encode_profile_selection(const ProfileSelection &payload) {
  std::array<std::byte, ProfileSelectionLayout::size> output{};
  ProfileSelectionLayout::encode(payload, output);
  return output;
}

bool decode_profile_selection(std::span<const std::byte> bytes,
                              ProfileSelection &output) {
  if (bytes.size() != ProfileSelectionLayout::size) {
    return false;
  }
  output = ProfileSelectionLayout::decode(bytes);
  return output.version == kSoftwareProfileVersion &&
         output.pixel_format == kRgba8888Premultiplied;
}

std::array<std::byte, AllocationLayout::size>
encode_surface_allocation(const TrustedAllocation &allocation) {
  std::array<std::byte, AllocationLayout::size> output{};
  AllocationLayout::encode(allocation, output);
  return output;
}

bool decode_surface_allocation(std::span<const std::byte> bytes,
                               std::uint64_t trusted_page_size,
                               TrustedAllocation &output) {
  if (bytes.size() != AllocationLayout::size || !AllocationLayout::matches_constants(bytes)) {
    return false;
  }
  const auto decoded = AllocationLayout::decode(bytes);
  if (decoded.pixel_format != kRgba8888Premultiplied)
    return false;
  const auto allocation = make_allocation(
      decoded.surface, decoded.logical_width, decoded.logical_height,
      decoded.pixel_width, decoded.pixel_height, decoded.dpr_numerator, decoded.dpr_denominator,
      trusted_page_size);
  if (!allocation || allocation->stride != decoded.stride ||
      allocation->frame_bytes != decoded.frame_bytes ||
      allocation->slot_extent != decoded.slot_extent ||
      allocation->mapping_bytes != decoded.mapping_bytes) {
    return false;
  }
  output = *allocation;
  return true;
}

std::array<std::byte, SurfaceKeyLayout::size> encode_surface_key(SurfaceKey surface) {
  std::array<std::byte, SurfaceKeyLayout::size> output{};
  SurfaceKeyLayout::encode(surface, output);
  return output;
}

bool decode_surface_key(std::span<const std::byte> bytes, SurfaceKey &output) {
  if (bytes.size() != SurfaceKeyLayout::size) {
    return false;
  }
  output = SurfaceKeyLayout::decode(bytes);
  return output.id != 0 && output.generation != 0;
}

std::array<std::byte, FrameReadyLayout::size> encode_frame_ready(const FrameReady &payload) {
  std::array<std::byte, FrameReadyLayout::size> output{};
  FrameReadyLayout::encode(payload, output);
  return output;
}

bool decode_frame_ready(std::span<const std::byte> bytes, FrameReady &output) {
  if (bytes.size() != FrameReadyLayout::size || !FrameReadyLayout::matches_constants(bytes)) {
    return false;
  }
  output = FrameReadyLayout::decode(bytes);
  return output.surface.id != 0 && output.surface.generation != 0 &&
         output.slot < kSlotCount && output.slot_sequence >= 2 &&
         (output.slot_sequence & 1U) == 0 && output.frame_sequence != 0;
}

std::array<std::byte, kInputRegionUpdateBytes>
encode_input_region_update(const InputRegionUpdate &payload) {
  std::array<std::byte, kInputRegionUpdateBytes> output{};
  InputRegionHeaderLayout::encode(payload, output);
  for (std::size_t index = 0;
       index < std::min<std::size_t>(payload.count, payload.regions.size());
       ++index) {
    const auto offset = InputRegionHeaderLayout::size + index * InputRegionLayout::size;
    InputRegionLayout::encode(payload.regions[index], std::span(output).subspan(offset));
  }
  return output;
}

bool decode_input_region_update(std::span<const std::byte> bytes,
                                InputRegionUpdate &output) {
  if (bytes.size() != kInputRegionUpdateBytes || !InputRegionHeaderLayout::matches_constants(bytes))
    return false;
  auto decoded = InputRegionHeaderLayout::decode(bytes);
  if (decoded.surface.id == 0 || decoded.surface.generation == 0 ||
      decoded.generation == 0 || decoded.count > decoded.regions.size())
    return false;
  for (std::size_t index = 0; index < decoded.regions.size(); ++index) {
    const auto offset = InputRegionHeaderLayout::size + index * InputRegionLayout::size;
    const auto region = InputRegionLayout::decode(bytes.subspan(offset));
    if (index < decoded.count) {
      if (region.width == 0 || region.height == 0) return false;
      decoded.regions[index] = region;
    } else if (region != TransportedInputRegion{}) {
      return false;
    }
  }
  output = decoded;
  return true;
}

namespace {
template <typename... T>
void append(std::vector<std::byte> &output, T... values) {
  (big_endian::append(output, values), ...);
}

void append_point(std::vector<std::byte> &output, InputPoint point) {
  append(output, point.x_q16, point.y_q16);
}

void append_text(std::vector<std::byte> &output, const std::string &text) {
  append(output, static_cast<std::uint32_t>(text.size()));
  const auto *bytes = reinterpret_cast<const std::byte *>(text.data());
  output.insert(output.end(), bytes, bytes + text.size());
}
} // namespace

std::optional<std::vector<std::byte>>
encode_input_event(const InputEvent &payload) {
  if (payload.surface.id == 0 || payload.surface.generation == 0 ||
      payload.sequence == 0 ||
      validate_input_shape(payload.payload) != InputValidation::accepted)
    return std::nullopt;
  std::vector<std::byte> output(InputEventHeaderLayout::size);
  std::visit(
      [&](const auto &event) {
        using Event = std::decay_t<decltype(event)>;
        EncodedInputKind kind;
        if constexpr (std::is_same_v<Event, PointerMotion>) {
          kind = EncodedInputKind::pointer_motion;
          append_point(output, event.position);
          append(output, event.buttons, event.modifiers);
        } else if constexpr (std::is_same_v<Event, PointerButton>) {
          kind = EncodedInputKind::pointer_button;
          append_point(output, event.position);
          append(output, event.button, static_cast<std::uint32_t>(event.state),
                 event.buttons, event.modifiers);
        } else if constexpr (std::is_same_v<Event, Wheel>) {
          kind = EncodedInputKind::wheel;
          append_point(output, event.position);
          append(output, std::bit_cast<std::uint32_t>(event.pixel_delta_x_q16),
                 std::bit_cast<std::uint32_t>(event.pixel_delta_y_q16),
                 std::bit_cast<std::uint32_t>(event.angle_delta_x),
                 std::bit_cast<std::uint32_t>(event.angle_delta_y),
                 static_cast<std::uint32_t>(event.phase), event.buttons,
                 event.modifiers, static_cast<std::uint32_t>(event.inverted));
        } else if constexpr (std::is_same_v<Event, Key>) {
          kind = EncodedInputKind::key;
          append(output, event.key, event.native_scan_code, event.modifiers,
                 static_cast<std::uint32_t>(event.state),
                 static_cast<std::uint32_t>(event.auto_repeat));
          append_text(output, event.text);
        } else if constexpr (std::is_same_v<Event, TextCommit>) {
          kind = EncodedInputKind::text_commit;
          append(output, std::bit_cast<std::uint32_t>(event.replacement_start),
                 event.replacement_length);
          append_text(output, event.text);
        } else if constexpr (std::is_same_v<Event, TouchFrame>) {
          kind = EncodedInputKind::touch_frame;
          append(output, static_cast<std::uint32_t>(event.phase), event.count,
                 event.modifiers);
          for (std::size_t index = 0; index < event.count; ++index) {
            append(output, event.points[index].id,
                   static_cast<std::uint32_t>(event.points[index].state));
            append_point(output, event.points[index].position);
          }
        } else if constexpr (std::is_same_v<Event, FocusChanged>) {
          kind = EncodedInputKind::focus_changed;
          append(output, static_cast<std::uint32_t>(event.focused));
        } else {
          kind = EncodedInputKind::cancel;
        }
        InputEventHeaderLayout::encode({payload.surface, payload.sequence, kind}, output);
      },
      payload.payload);
  return output;
}

bool decode_input_event(std::span<const std::byte> bytes, InputEvent &output) {
  if (bytes.size() < InputEventHeaderLayout::size || bytes.size() > kMaximumInputEventBytes ||
      !InputEventHeaderLayout::matches_constants(bytes))
    return false;
  const auto header = InputEventHeaderLayout::decode(bytes);
  InputEvent decoded{.surface = header.surface,
                     .sequence = header.sequence,
                     .payload = Cancel{}};
  if (decoded.surface.id == 0 || decoded.surface.generation == 0 ||
      decoded.sequence == 0)
    return false;
  const auto kind = header.kind;
  const auto u32 = [&](std::size_t offset) {
    return get<std::uint32_t>(bytes, offset);
  };
  const auto point = [&](std::size_t offset) {
    return InputPoint{.x_q16 = u32(offset), .y_q16 = u32(offset + 4)};
  };
  if (kind == EncodedInputKind::pointer_motion && bytes.size() == 48) {
    decoded.payload = PointerMotion{.position = point(32),
                                    .buttons = u32(40),
                                    .modifiers = u32(44)};
  } else if (kind == EncodedInputKind::pointer_button && bytes.size() == 56) {
    decoded.payload = PointerButton{
        .position = point(32),
        .button = u32(40),
        .state = static_cast<ButtonState>(u32(44)),
        .buttons = u32(48),
        .modifiers = u32(52)};
  } else if (kind == EncodedInputKind::wheel && bytes.size() == 72) {
    decoded.payload = Wheel{
        .position = point(32),
        .pixel_delta_x_q16 = std::bit_cast<std::int32_t>(u32(40)),
        .pixel_delta_y_q16 = std::bit_cast<std::int32_t>(u32(44)),
        .angle_delta_x = std::bit_cast<std::int32_t>(u32(48)),
        .angle_delta_y = std::bit_cast<std::int32_t>(u32(52)),
        .phase = static_cast<WheelPhase>(u32(56)),
        .buttons = u32(60),
        .modifiers = u32(64),
        .inverted = u32(68) == 1};
    if (u32(68) > 1)
      return false;
  } else if (kind == EncodedInputKind::key && bytes.size() >= 56 &&
             bytes.size() == 56 + u32(52) && u32(52) <= kMaximumInputTextBytes &&
             u32(48) <= 1) {
    decoded.payload = Key{
        .key = u32(32),
        .native_scan_code = u32(36),
        .modifiers = u32(40),
        .state = static_cast<ButtonState>(u32(44)),
        .auto_repeat = u32(48) == 1,
        .text = std::string(reinterpret_cast<const char *>(bytes.data() + 56),
                            u32(52))};
  } else if (kind == EncodedInputKind::text_commit && bytes.size() >= 44 &&
             bytes.size() == 44 + u32(40) && u32(40) <= kMaximumInputTextBytes) {
    decoded.payload = TextCommit{
        .text = std::string(reinterpret_cast<const char *>(bytes.data() + 44),
                            u32(40)),
        .replacement_start = std::bit_cast<std::int32_t>(u32(32)),
        .replacement_length = u32(36)};
  } else if (kind == EncodedInputKind::touch_frame && bytes.size() >= 44 &&
             u32(36) <= kMaximumTouchPoints &&
             bytes.size() == 44 + static_cast<std::size_t>(u32(36)) * 16) {
    TouchFrame frame{.phase = static_cast<TouchFramePhase>(u32(32)),
                     .count = u32(36),
                     .modifiers = u32(40)};
    for (std::size_t index = 0; index < frame.count; ++index) {
      const auto offset = 44 + index * 16;
      frame.points[index] = {
          .id = u32(offset),
          .state = static_cast<TouchPointState>(u32(offset + 4)),
          .position = point(offset + 8)};
    }
    decoded.payload = frame;
  } else if (kind == EncodedInputKind::focus_changed && bytes.size() == 36 &&
             u32(32) <= 1) {
    decoded.payload = FocusChanged{.focused = u32(32) == 1};
  } else if (kind == EncodedInputKind::cancel && bytes.size() == 32) {
    decoded.payload = Cancel{};
  } else {
    return false;
  }
  if (validate_input_shape(decoded.payload) != InputValidation::accepted)
    return false;
  output = std::move(decoded);
  return true;
}

std::array<std::byte, kSurfaceIntentBytes>
encode_surface_intent(const SurfaceIntentRequest &payload) {
  std::array<std::byte, kSurfaceIntentBytes> output{};
  if (payload.requested_output.size() > kMaximumRequestedOutputBytes ||
      std::ranges::any_of(payload.requested_output, [](const char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte < 0x21 || byte > 0x7e;
      }))
    return output;
  SurfaceIntentHeaderLayout::encode({payload.source, payload.target, payload.input_sequence,
      payload.action, static_cast<std::uint16_t>(payload.requested_output.size())}, output);
  std::ranges::transform(payload.requested_output, output.begin() + SurfaceIntentHeaderLayout::size,
                         [](const char value) {
                           return static_cast<std::byte>(value);
                         });
  return output;
}

bool decode_surface_intent(std::span<const std::byte> bytes,
                           SurfaceIntentRequest &output) {
  if (bytes.size() != kSurfaceIntentBytes || !SurfaceIntentHeaderLayout::matches_constants(bytes))
    return false;
  const auto header = SurfaceIntentHeaderLayout::decode(bytes);
  const auto output_size = header.output_size;
  if (output_size > kMaximumRequestedOutputBytes ||
      std::ranges::any_of(bytes.subspan(SurfaceIntentHeaderLayout::size, output_size), [](std::byte value) {
        const auto byte = std::to_integer<unsigned char>(value);
        return byte < 0x21 || byte > 0x7e;
      }) ||
      std::ranges::any_of(bytes.subspan(SurfaceIntentHeaderLayout::size + output_size),
                          [](std::byte value) { return value != std::byte{0}; }))
    return false;
  const auto action = header.action;
  if (action != SurfaceIntentAction::open &&
      action != SurfaceIntentAction::toggle &&
      action != SurfaceIntentAction::dismiss)
    return false;
  output = {
      .source = header.source,
      .target = header.target,
      .input_sequence = header.input_sequence,
      .action = action,
      .requested_output = std::string(
          reinterpret_cast<const char *>(bytes.data() + SurfaceIntentHeaderLayout::size), output_size)};
  if (output.source.id == 0 || output.source.generation == 0 ||
      output.target.id == 0 || output.target.generation == 0)
    return false;
  if (action == SurfaceIntentAction::dismiss)
    return output.source == output.target && output.input_sequence == 0 &&
           output.requested_output.empty();
  return output.input_sequence != 0;
}

std::array<std::byte, RenderErrorLayout::size> encode_render_error(const RenderTypedError &payload) {
  std::array<std::byte, RenderErrorLayout::size> output{};
  RenderErrorLayout::encode(payload, output);
  return output;
}

bool decode_render_error(std::span<const std::byte> bytes,
                         RenderTypedError &output) {
  if (bytes.size() != RenderErrorLayout::size || !RenderErrorLayout::matches_constants(bytes)) {
    return false;
  }
  output = RenderErrorLayout::decode(bytes);
  const bool profile_error =
      output.reason == RenderErrorReason::unsupported_profile &&
      output.failed_message_type ==
          static_cast<std::uint16_t>(RenderMessageType::profile_offer) &&
      output.surface.id == 0 && output.surface.generation == 0;
  const bool surface_error =
      output.reason != RenderErrorReason::unsupported_profile &&
      output.failed_message_type ==
          static_cast<std::uint16_t>(RenderMessageType::surface_allocate) &&
      output.surface.id != 0 && output.surface.generation != 0;
  return valid_render_error(output.reason) && (profile_error || surface_error);
}

} // namespace omarchy::plugin_runtime::surface
