#pragma once

#include "omarchy/plugin/wire/role_registry.hpp"
#include "omarchy/plugin/wire/surface_name.hpp"
#include "omarchy/plugin_runtime/surface/input.hpp"
#include "omarchy/plugin_runtime/surface/profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace omarchy::plugin_runtime::surface {

using omarchy::plugin::wire::kMaximumSurfaceNameBytes;
using omarchy::plugin::wire::valid_surface_name;

inline constexpr std::uint16_t kRenderRoleVersion = 2;

enum class RenderMessageType : std::uint16_t {
  profile_offer = 0x2000,
  profile_select = 0x2001,
  surface_allocate = 0x2010,
  surface_allocated = 0x2011,
  surface_release = 0x2012,
  surface_suspend = 0x2013,
  surface_resume = 0x2014,
  frame_ready = 0x2020,
  input_regions = 0x2021,
  input = 0x2030,
  surface_intent = 0x2040,
};

// One collision-free correlation pair is fixed by the trusted surface key.
// Both the session router and HostRenderSession use this single derivation.
[[nodiscard]] inline constexpr std::uint64_t
render_correlation_base(SurfaceKey surface) noexcept {
  return surface.id * 4;
}
[[nodiscard]] inline constexpr std::array<std::uint64_t, 2>
render_correlations(SurfaceKey surface) noexcept {
  return {render_correlation_base(surface) + 1,
          render_correlation_base(surface) + 2};
}

enum class RenderErrorReason : std::uint16_t {
  unsupported_profile = 1,
  invalid_allocation = 2,
  resource_limit = 3,
  stale_surface = 4,
};

struct FrameReady {
  SurfaceKey surface;
  std::uint32_t slot;
  std::uint64_t slot_sequence;
  std::uint64_t frame_sequence;
};

inline constexpr std::size_t kMaximumTransportedInputRegions = 16;
struct TransportedInputRegion {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  constexpr bool operator==(const TransportedInputRegion &) const = default;
};
using SurfaceKeyLayout = FixedLayout<SurfaceKey, &SurfaceKey::id, &SurfaceKey::generation>;
using ProfileSelectionLayout = FixedLayout<ProfileSelection,
    &ProfileSelection::version, &ProfileSelection::pixel_format>;
using InputRegionLayout = FixedLayout<TransportedInputRegion,
    &TransportedInputRegion::x, &TransportedInputRegion::y,
    &TransportedInputRegion::width, &TransportedInputRegion::height>;
static_assert(SurfaceKeyLayout::size == 16 && ProfileSelectionLayout::size == 8 &&
              InputRegionLayout::size == 16);
struct InputRegionUpdate {
  SurfaceKey surface;
  std::uint64_t generation = 0;
  std::array<TransportedInputRegion, kMaximumTransportedInputRegions> regions{};
  std::uint32_t count = 0;
};
using InputRegionHeaderLayout = FixedLayout<InputRegionUpdate,
    MemberPath<&InputRegionUpdate::surface, &SurfaceKey::id>{},
    MemberPath<&InputRegionUpdate::surface, &SurfaceKey::generation>{},
    &InputRegionUpdate::generation, &InputRegionUpdate::count,
    ConstantField<std::uint32_t, 0>{}>;
inline constexpr std::size_t kInputRegionUpdateBytes = InputRegionHeaderLayout::size +
    kMaximumTransportedInputRegions * InputRegionLayout::size;
static_assert(kInputRegionUpdateBytes == 288);

enum class SurfaceIntentAction : std::uint32_t {
  open = 1,
  toggle = 2,
  dismiss = 3,
};

struct SurfaceIntentRequest {
  SurfaceKey source;
  SurfaceKey target;
  std::uint64_t input_sequence = 0;
  SurfaceIntentAction action = SurfaceIntentAction::open;
  std::string requested_output;

  constexpr bool operator==(const SurfaceIntentRequest &) const = default;
};

// Wire-only prefixes carry lengths/tags without giving those fields authority
// over the domain payload or its input provenance.
struct SurfaceIntentHeader {
  SurfaceKey source, target;
  std::uint64_t input_sequence;
  SurfaceIntentAction action;
  std::uint16_t output_size;
};
using SurfaceIntentHeaderLayout = FixedLayout<SurfaceIntentHeader,
    MemberPath<&SurfaceIntentHeader::source, &SurfaceKey::id>{},
    MemberPath<&SurfaceIntentHeader::source, &SurfaceKey::generation>{},
    MemberPath<&SurfaceIntentHeader::target, &SurfaceKey::id>{},
    MemberPath<&SurfaceIntentHeader::target, &SurfaceKey::generation>{},
    &SurfaceIntentHeader::input_sequence, &SurfaceIntentHeader::action,
    &SurfaceIntentHeader::output_size, ConstantField<std::uint16_t, 0>{}>;
inline constexpr std::size_t kMaximumRequestedOutputBytes = 128;
inline constexpr std::size_t kSurfaceIntentBytes = SurfaceIntentHeaderLayout::size + kMaximumRequestedOutputBytes;

enum class EncodedInputKind : std::uint32_t {
  pointer_motion = 1,
  pointer_button = 2,
  wheel = 3,
  key = 4,
  text_commit = 5,
  touch_frame = 6,
  focus_changed = 7,
  cancel = 8,
};
struct InputEventHeader {
  SurfaceKey surface;
  std::uint64_t sequence;
  EncodedInputKind kind;
};
using InputEventHeaderLayout = FixedLayout<InputEventHeader,
    MemberPath<&InputEventHeader::surface, &SurfaceKey::id>{},
    MemberPath<&InputEventHeader::surface, &SurfaceKey::generation>{},
    &InputEventHeader::sequence, &InputEventHeader::kind, ConstantField<std::uint32_t, 0>{}>;
// The largest current input body is a touch frame: three u32 fields followed
// by bounded four-u32 points. Key and text bodies must fit this transport cap.
inline constexpr std::size_t kMaximumInputEventBytes = InputEventHeaderLayout::size +
    3 * sizeof(std::uint32_t) + kMaximumTouchPoints * 4 * sizeof(std::uint32_t);
static_assert(SurfaceIntentHeaderLayout::size == 48 && kSurfaceIntentBytes == 176 &&
              InputEventHeaderLayout::size == 32 && kMaximumInputEventBytes == 204);
static_assert(InputEventHeaderLayout::size + 6 * sizeof(std::uint32_t) +
              kMaximumInputTextBytes <= kMaximumInputEventBytes);

struct RenderTypedError {
  RenderErrorReason reason;
  std::uint16_t failed_message_type;
  SurfaceKey surface;
};

inline constexpr std::uint32_t kFullFrameOnly = 1U << 0U;
inline constexpr std::uint32_t kSoftwareSceneGraph = 1U << 1U;
inline constexpr std::uint32_t kRequiredProfileFlags = kFullFrameOnly | kSoftwareSceneGraph;
using ProfileOfferLayout = FixedLayout<ProfileOffer, &ProfileOffer::version,
    ConstantField<std::uint32_t, kRgba8888Premultiplied>{},
    &ProfileOffer::maximum_pixel_dimension,
    ConstantField<std::uint32_t, kRequiredProfileFlags>{}, &ProfileOffer::maximum_frame_bytes>;
using AllocationLayout = FixedLayout<TrustedAllocation,
    MemberPath<&TrustedAllocation::surface, &SurfaceKey::id>{},
    MemberPath<&TrustedAllocation::surface, &SurfaceKey::generation>{},
    ConstantField<std::uint32_t, kSoftwareProfileVersion>{},
    &TrustedAllocation::pixel_format, &TrustedAllocation::logical_width,
    &TrustedAllocation::logical_height, &TrustedAllocation::pixel_width,
    &TrustedAllocation::pixel_height, &TrustedAllocation::dpr_numerator,
    &TrustedAllocation::dpr_denominator, &TrustedAllocation::stride,
    ConstantField<std::uint32_t, kSlotCount>{},
    ConstantField<std::uint32_t, kSlotHeaderSize>{}, ConstantField<std::uint32_t, 0>{},
    &TrustedAllocation::frame_bytes, ConstantField<std::uint64_t, kSlotPixelOffset>{},
    &TrustedAllocation::slot_extent, &TrustedAllocation::mapping_bytes>;
using FrameReadyLayout = FixedLayout<FrameReady,
    MemberPath<&FrameReady::surface, &SurfaceKey::id>{},
    MemberPath<&FrameReady::surface, &SurfaceKey::generation>{},
    &FrameReady::slot, ConstantField<std::uint32_t, 0>{},
    &FrameReady::slot_sequence, &FrameReady::frame_sequence>;
using RenderErrorLayout = FixedLayout<RenderTypedError,
    &RenderTypedError::reason, &RenderTypedError::failed_message_type,
    ConstantField<std::uint32_t, 0>{},
    MemberPath<&RenderTypedError::surface, &SurfaceKey::id>{},
    MemberPath<&RenderTypedError::surface, &SurfaceKey::generation>{}>;
static_assert(ProfileOfferLayout::size == 24 && AllocationLayout::size == 96 &&
              FrameReadyLayout::size == 40 && RenderErrorLayout::size == 24);

[[nodiscard]] omarchy::plugin::wire::RoleSchemaView render_role_schema();
[[nodiscard]] std::optional<std::uint8_t>
render_descriptor_count(std::uint16_t message_type);

[[nodiscard]] std::array<std::byte, ProfileOfferLayout::size>
encode_profile_offer(const ProfileOffer &payload);
[[nodiscard]] bool decode_profile_offer(std::span<const std::byte> bytes,
                                        ProfileOffer &output);
[[nodiscard]] std::array<std::byte, ProfileSelectionLayout::size>
encode_profile_selection(const ProfileSelection &payload);
[[nodiscard]] bool decode_profile_selection(std::span<const std::byte> bytes,
                                            ProfileSelection &output);

[[nodiscard]] std::array<std::byte, AllocationLayout::size>
encode_surface_allocation(const TrustedAllocation &allocation);
[[nodiscard]] bool decode_surface_allocation(std::span<const std::byte> bytes,
                                             std::uint64_t trusted_page_size,
                                             TrustedAllocation &output);
[[nodiscard]] std::array<std::byte, SurfaceKeyLayout::size> encode_surface_key(SurfaceKey surface);
[[nodiscard]] bool decode_surface_key(std::span<const std::byte> bytes,
                                      SurfaceKey &output);

[[nodiscard]] std::array<std::byte, FrameReadyLayout::size>
encode_frame_ready(const FrameReady &payload);
[[nodiscard]] bool decode_frame_ready(std::span<const std::byte> bytes,
                                      FrameReady &output);
[[nodiscard]] std::array<std::byte, kInputRegionUpdateBytes>
encode_input_region_update(const InputRegionUpdate &payload);
[[nodiscard]] bool decode_input_region_update(std::span<const std::byte> bytes,
                                              InputRegionUpdate &output);
[[nodiscard]] std::optional<std::vector<std::byte>>
encode_input_event(const InputEvent &payload);
[[nodiscard]] bool decode_input_event(std::span<const std::byte> bytes,
                                      InputEvent &output);
[[nodiscard]] std::array<std::byte, kSurfaceIntentBytes>
encode_surface_intent(const SurfaceIntentRequest &payload);
[[nodiscard]] bool decode_surface_intent(std::span<const std::byte> bytes,
                                         SurfaceIntentRequest &output);
[[nodiscard]] std::array<std::byte, RenderErrorLayout::size>
encode_render_error(const RenderTypedError &payload);
[[nodiscard]] bool decode_render_error(std::span<const std::byte> bytes,
                                       RenderTypedError &output);

} // namespace omarchy::plugin_runtime::surface
