#include "../../tests/support/test_assert.hpp"

#include "surface_host.hpp"

#include "omarchy/plugin_runtime/surface/profile.hpp"
#include "omarchy/plugin_runtime/surface/render_messages.hpp"

#include <QGuiApplication>

#include <cstdlib>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using omarchy::plugin_runtime::test_support::throws_exception;

namespace bridge = omarchy::plugin_runtime::bridge;
namespace host = omarchy::plugin_runtime::surface_host;
namespace permissions = omarchy::plugins::permissions;
namespace render_session = omarchy::plugin_runtime::render_session;
namespace surface = omarchy::plugin_runtime::surface;
namespace wire = omarchy::plugin::wire;

using omarchy::plugin_runtime::test_support::require;

class PacketSender final : public render_session::PacketSender {
public:
  bool send(const wire::EnvelopeHeader &, std::span<const std::byte>,
            std::span<const int>) override {
    return true;
  }
};

class InputSink final : public bridge::RenderPacketSink {
public:
  bool send(const wire::EnvelopeHeader &header,
            std::span<const std::byte> payload) override {
    ++calls;
    last_header = header;
    last_payload.assign(payload.begin(), payload.end());
    surface::InputEvent input;
    if (header.message_type == static_cast<std::uint16_t>(
                                   surface::RenderMessageType::input) &&
        surface::decode_input_event(payload, input))
      inputs.push_back(std::move(input));
    return true;
  }
  std::size_t calls = 0;
  wire::EnvelopeHeader last_header{};
  std::vector<std::byte> last_payload;
  std::vector<surface::InputEvent> inputs;
};

class Clock final : public host::MonotonicClock {
public:
  std::uint64_t now_nanoseconds() const override { return 1'000'000'000; }
};

render_session::AuthenticatedRenderPacket
packet(std::uint16_t type, std::span<const std::byte> payload,
       std::uint64_t correlation) {
  return {.message_type = type,
          .correlation_id = correlation,
          .payload = payload};
}

struct Harness {
  explicit Harness(bool dynamic = true,
                   host::SurfaceRole role = host::SurfaceRole::desktop_overlay,
                   bridge::TrustedInputAuthority *shared_input = nullptr,
                   host::KeyboardFocusPolicy keyboard_focus =
                       host::KeyboardFocusPolicy::none,
                   std::uint64_t id = surface_id)
      : sink(std::make_shared<InputSink>()),
        input_authority(shared_input != nullptr ? *shared_input
                                                : owned_input_authority) {
    const permissions::ActivationBinding binding{
        .plugin = permissions::PluginId("org.example.regions"),
        .revision = permissions::Digest(std::string(64, 'a')),
        .policy_fingerprint = permissions::Digest(std::string(64, 'b')),
        .generation = generation,
    };
    surface_host = host::HostSurface::create(
        {.plugin_id = "org.example.regions",
         .surface_name = "pet",
         .role = role,
         .maximum_width = 64,
         .maximum_height = 32,
         .maximum_frames_per_second = 60,
         .keyboard_focus = keyboard_focus,
         .dynamic_input_regions = dynamic,
         .initially_visible = false},
        binding, id, 64, 32, 1, 1, item, sender, sink,
        input_authority, clock);
    OMARCHY_CHECK(surface_host != nullptr);

    const auto selection = surface::encode_profile_selection(
        {.version = surface::kSoftwareProfileVersion,
         .pixel_format = surface::kRgba8888Premultiplied});
    OMARCHY_CHECK(surface_host->receive_render(
                packet(static_cast<std::uint16_t>(
                           surface::RenderMessageType::profile_select),
                       selection, id * 4 + 1)));
    const auto allocated =
        surface::encode_surface_key(surface_host->allocation().surface);
    OMARCHY_CHECK(surface_host->receive_render(
                packet(static_cast<std::uint16_t>(
                           surface::RenderMessageType::surface_allocated),
                       allocated, id * 4 + 2)));
  }

  surface::InputRegionUpdate update(std::uint64_t region_generation = 1) {
    return {.surface = surface_host->allocation().surface,
            .generation = region_generation,
            .regions = {{{.x = 2, .y = 3, .width = 4, .height = 5}}},
            .count = 1};
  }

  bool receive(const surface::InputRegionUpdate &value) {
    const auto payload = surface::encode_input_region_update(value);
    return surface_host->receive_render(
        packet(static_cast<std::uint16_t>(
                   surface::RenderMessageType::input_regions),
               payload, 0));
  }

  static constexpr std::uint64_t generation = 7;
  static constexpr std::uint64_t surface_id = 71;
  bridge::RemotePluginSurface item;
  PacketSender sender;
  std::shared_ptr<InputSink> sink;
  bridge::TrustedInputAuthority owned_input_authority;
  bridge::TrustedInputAuthority &input_authority;
  Clock clock;
  std::unique_ptr<host::HostSurface> surface_host;
};

void authenticated_input_regions_reach_the_single_policy_decision() {
  Harness accepted;
  OMARCHY_CHECK(accepted.receive(accepted.update()) &&
              accepted.item.inputRegions() ==
                  QList<QRect>{QRect(2, 3, 4, 5)});

  Harness rejected;
  OMARCHY_CHECK(rejected.item.updateInputRegions(rejected.update()));
  const auto rejected_surface = rejected.surface_host->allocation().surface;
  OMARCHY_CHECK(rejected.surface_host->route_input(
              {.payload = surface::PointerButton{
                   .position = {3U << surface::kQ16FractionBits,
                                4U << surface::kQ16FractionBits},
                   .button = static_cast<std::uint32_t>(Qt::LeftButton),
                   .state = surface::ButtonState::pressed,
                   .buttons = static_cast<std::uint32_t>(Qt::LeftButton)},
               .device = 77,
               .trusted_physical = true}) &&
              rejected.input_authority.pointer_captured(rejected_surface, 77));
  auto invalid = rejected.update();
  invalid.regions[0].x = -1;
  surface::InputEvent terminal_input;
  OMARCHY_CHECK(!rejected.receive(invalid) &&
              rejected.surface_host->terminated() &&
              !rejected.item.connected() &&
              rejected.item.inputRegions().isEmpty() &&
              !rejected.input_authority.pointer_captured(rejected_surface, 77) &&
              rejected.sink->calls == 2 &&
              surface::decode_input_event(rejected.sink->last_payload,
                                          terminal_input) &&
              std::holds_alternative<surface::Cancel>(terminal_input.payload));
}

void accepted_regions_are_one_enforcement_and_projection_decision() {
  Harness harness;
  const auto accepted = harness.update();
  OMARCHY_CHECK(harness.item.updateInputRegions(accepted) &&
              harness.item.inputRegions() == QList<QRect>{QRect(2, 3, 4, 5)});

  auto outside = bridge::HostInputEvent{
      .payload = surface::PointerButton{
          .position = {.x_q16 = 1U << surface::kQ16FractionBits,
                       .y_q16 = 1U << surface::kQ16FractionBits},
          .button = static_cast<std::uint32_t>(Qt::LeftButton),
          .state = surface::ButtonState::pressed,
          .buttons = static_cast<std::uint32_t>(Qt::LeftButton)},
      .device = 10,
      .trusted_physical = true};
  OMARCHY_CHECK(!harness.surface_host->route_input(outside));
  auto &button = std::get<surface::PointerButton>(outside.payload);
  button.position.x_q16 = 3U << surface::kQ16FractionBits;
  button.position.y_q16 = 4U << surface::kQ16FractionBits;
  OMARCHY_CHECK(harness.surface_host->route_input(outside) &&
              harness.sink->calls == 1);

  auto drag = bridge::HostInputEvent{
      .payload = surface::PointerMotion{
          .position = {.x_q16 = 1U << surface::kQ16FractionBits,
                       .y_q16 = 1U << surface::kQ16FractionBits},
          .buttons = static_cast<std::uint32_t>(Qt::LeftButton)},
      .device = 10,
      .trusted_physical = true};
  OMARCHY_CHECK(harness.surface_host->route_input(drag) &&
              harness.sink->calls == 2);
  drag.device = 11;
  OMARCHY_CHECK(!harness.surface_host->route_input(drag) &&
              harness.sink->calls == 2);
  auto release = bridge::HostInputEvent{
      .payload = surface::PointerButton{
          .position = {.x_q16 = 1U << surface::kQ16FractionBits,
                       .y_q16 = 1U << surface::kQ16FractionBits},
          .button = static_cast<std::uint32_t>(Qt::LeftButton),
          .state = surface::ButtonState::released,
          .buttons = 0},
      .device = 10,
      .trusted_physical = true};
  OMARCHY_CHECK(harness.surface_host->route_input(release) &&
              harness.sink->calls == 3 &&
              !harness.surface_host->route_input(drag));

  auto wheel = bridge::HostInputEvent{
      .payload = surface::Wheel{
          .position = {.x_q16 = 3U << surface::kQ16FractionBits,
                       .y_q16 = 4U << surface::kQ16FractionBits},
          .angle_delta_y = 120,
          .phase = surface::WheelPhase::begin},
      .device = 13,
      .trusted_physical = true};
  OMARCHY_CHECK(harness.surface_host->route_input(wheel));
  auto &wheel_payload = std::get<surface::Wheel>(wheel.payload);
  wheel_payload.position = {.x_q16 = 1U << surface::kQ16FractionBits,
                            .y_q16 = 1U << surface::kQ16FractionBits};
  wheel_payload.phase = surface::WheelPhase::update;
  OMARCHY_CHECK(harness.surface_host->route_input(wheel));
  wheel.device = 14;
  OMARCHY_CHECK(!harness.surface_host->route_input(wheel));
  wheel.device = 13;
  wheel_payload.phase = surface::WheelPhase::end;
  OMARCHY_CHECK(harness.surface_host->route_input(wheel));
  wheel_payload.phase = surface::WheelPhase::discrete;
  OMARCHY_CHECK(!harness.surface_host->route_input(wheel));

  bridge::HostTouchFrame oversized_touch;
  oversized_touch.phase = surface::TouchFramePhase::begin;
  oversized_touch.count = surface::kMaximumTouchPoints + 1;
  OMARCHY_CHECK(!harness.surface_host->route_input(
              {.payload = oversized_touch,
               .device = 12,
               .trusted_physical = true}));

  bridge::HostTouchFrame touch_begin;
  touch_begin.phase = surface::TouchFramePhase::begin;
  touch_begin.count = 1;
  touch_begin.points[0] = {
      .id = 700,
      .state = surface::TouchPointState::pressed,
      .position = {.x_q16 = 3U << surface::kQ16FractionBits,
                   .y_q16 = 4U << surface::kQ16FractionBits}};
  OMARCHY_CHECK(harness.surface_host->route_input(
              {.payload = touch_begin,
               .device = 15,
               .trusted_physical = true}) &&
              !harness.surface_host->cancel_input(16) &&
              harness.surface_host->cancel_input(15) &&
              !harness.input_authority.touch_captured(
                  harness.surface_host->allocation().surface, 15));

  auto cleared = harness.update(2);
  cleared.count = 0;
  OMARCHY_CHECK(harness.item.updateInputRegions(cleared) &&
              harness.item.inputRegions().isEmpty());
}

bridge::HostInputEvent pointer_button(std::uint64_t device,
                                      bool trusted_physical,
                                      surface::ButtonState state,
                                      std::uint32_t buttons) {
  return {
      .payload = surface::PointerButton{
          .position = {.x_q16 = 1U << surface::kQ16FractionBits,
                       .y_q16 = 1U << surface::kQ16FractionBits},
          .button = static_cast<std::uint32_t>(Qt::LeftButton),
          .state = state,
          .buttons = buttons},
      .device = device,
      .trusted_physical = trusted_physical};
}

void focus_and_sequence_are_owned_across_surfaces() {
  Harness synthetic(false, host::SurfaceRole::desktop_overlay, nullptr,
                    host::KeyboardFocusPolicy::after_gesture, 81);
  const auto synthetic_surface = synthetic.surface_host->allocation().surface;
  OMARCHY_CHECK(synthetic.surface_host->route_input(pointer_button(
              10, false, surface::ButtonState::pressed,
              static_cast<std::uint32_t>(Qt::LeftButton))) &&
              synthetic.input_authority.pointer_captured(synthetic_surface,
                                                         10) &&
              !synthetic.input_authority.surface_has_physical_activation(
                  synthetic_surface) &&
              !synthetic.surface_host->route_input(
                  {.payload = surface::FocusChanged{.focused = true}}) &&
              !synthetic.input_authority.focused_surface() &&
              !synthetic.item.surfaceFocused() &&
              synthetic.sink->inputs.size() == 1 &&
              synthetic.surface_host->cancel_input(10));

  bridge::TrustedInputAuthority shared_input;
  Harness first(false, host::SurfaceRole::desktop_overlay, &shared_input,
                host::KeyboardFocusPolicy::after_gesture, 82);
  Harness second(false, host::SurfaceRole::desktop_overlay, &shared_input,
                 host::KeyboardFocusPolicy::after_gesture, 83);
  const auto first_surface = first.surface_host->allocation().surface;
  const auto second_surface = second.surface_host->allocation().surface;
  OMARCHY_CHECK(first.surface_host->route_input(pointer_button(
              20, true, surface::ButtonState::pressed,
              static_cast<std::uint32_t>(Qt::LeftButton))) &&
              shared_input.surface_has_physical_activation(first_surface) &&
              first.surface_host->route_input(
                  {.payload = surface::FocusChanged{.focused = true}}) &&
              shared_input.focused_surface() == first_surface &&
              first.surface_host->route_input(pointer_button(
                  20, true, surface::ButtonState::released, 0)) &&
              second.surface_host->route_input(pointer_button(
                  21, true, surface::ButtonState::pressed,
                  static_cast<std::uint32_t>(Qt::LeftButton))) &&
              !second.surface_host->route_input(
                  {.payload = surface::FocusChanged{.focused = true}}) &&
              first.surface_host->route_input(
                  {.payload = surface::FocusChanged{.focused = false}}) &&
              second.surface_host->route_input(
                  {.payload = surface::FocusChanged{.focused = true}}) &&
              shared_input.focused_surface() == second_surface);
  OMARCHY_CHECK(first.sink->inputs.size() == 4 &&
              second.sink->inputs.size() == 2 &&
              first.sink->inputs[0].sequence == 1 &&
              first.sink->inputs[1].sequence == 2 &&
              first.sink->inputs[2].sequence == 3 &&
              second.sink->inputs[0].sequence == 4 &&
              first.sink->inputs[3].sequence == 5 &&
              second.sink->inputs[1].sequence == 6);
}

void close_sends_terminal_cancel_and_clears_authority() {
  Harness harness(false);
  const auto key = harness.surface_host->allocation().surface;
  OMARCHY_CHECK(harness.surface_host->route_input(pointer_button(
              30, true, surface::ButtonState::pressed,
              static_cast<std::uint32_t>(Qt::LeftButton))) &&
              harness.input_authority.pointer_captured(key, 30));
  harness.surface_host->close();
  OMARCHY_CHECK(harness.surface_host->terminated() && !harness.item.connected() &&
              !harness.input_authority.pointer_captured(key, 30) &&
              !harness.input_authority.focused_surface() &&
              harness.sink->inputs.size() == 2 &&
              std::holds_alternative<surface::Cancel>(
                  harness.sink->inputs.back().payload));
}

void invalid_regions_preserve_last_accepted_state() {
  Harness harness;
  OMARCHY_CHECK(harness.item.updateInputRegions(harness.update()));
  const auto expected = harness.item.inputRegions();

  auto negative = harness.update(2);
  negative.regions[0].x = -1;
  auto zero = harness.update(2);
  zero.regions[0].width = 0;
  auto outside = harness.update(2);
  outside.regions[0] = {.x = 63, .y = 31, .width = 2, .height = 2};
  auto too_many = harness.update(2);
  too_many.count = surface::kMaximumTransportedInputRegions + 1;
  auto wrong_surface = harness.update(2);
  wrong_surface.surface.id++;
  for (const auto &candidate :
       {negative, zero, outside, too_many, wrong_surface}) {
    OMARCHY_CHECK(!harness.item.updateInputRegions(candidate) &&
                harness.item.inputRegions() == expected);
  }
  OMARCHY_CHECK(!harness.item.updateInputRegions(harness.update()) &&
              harness.item.inputRegions() == expected);
  auto retained_press =
      pointer_button(92, true, surface::ButtonState::pressed,
                     static_cast<std::uint32_t>(Qt::LeftButton));
  auto &retained_position =
      std::get<surface::PointerButton>(retained_press.payload).position;
  retained_position = {.x_q16 = 3U << surface::kQ16FractionBits,
                       .y_q16 = 4U << surface::kQ16FractionBits};
  auto retained_release =
      pointer_button(92, true, surface::ButtonState::released, 0);
  std::get<surface::PointerButton>(retained_release.payload).position =
      retained_position;
  OMARCHY_CHECK(harness.surface_host->route_input(retained_press) &&
              harness.surface_host->route_input(retained_release));
}

void policy_and_lifetime_are_fail_closed() {
  Harness fixed(false);
  auto fixed_press = pointer_button(91, true, surface::ButtonState::pressed,
                                    static_cast<std::uint32_t>(Qt::LeftButton));
  auto fixed_release =
      pointer_button(91, true, surface::ButtonState::released, 0);
  OMARCHY_CHECK(!fixed.item.updateInputRegions(fixed.update()) &&
              fixed.item.inputRegions().isEmpty() &&
              fixed.surface_host->route_input(fixed_press) &&
              fixed.surface_host->route_input(fixed_release));

  Harness bar(true, host::SurfaceRole::bar_embedded);
  OMARCHY_CHECK(bar.item.updateInputRegions(bar.update()) &&
              bar.item.inputRegions() == QList<QRect>{QRect(2, 3, 4, 5)});
  bar.surface_host.reset();
  OMARCHY_CHECK(bar.item.inputRegions().isEmpty() &&
              !bar.item.updateInputRegions(
                  {.surface = {.id = Harness::surface_id,
                               .generation = Harness::generation},
                   .generation = 2,
                   .count = 0}));
}

void initial_visibility_is_overlay_only_and_typed() {
  omarchy::plugins::manifest::ManifestV2 manifest;
  manifest.id = "org.example.visibility";
  manifest.canonical_surfaces = R"JSON({
    "pet": {
      "role": "desktop-overlay",
      "maximumWidth": 64,
      "maximumHeight": 32,
      "maximumFramesPerSecond": 30,
      "initiallyVisible": true
    }
  })JSON";
  const auto overlay = host::parse_named_surface_policy(manifest, "pet");
  OMARCHY_CHECK(overlay.initially_visible);

  manifest.canonical_surfaces = R"JSON({
    "panel": {
      "role": "panel",
      "maximumWidth": 64,
      "maximumHeight": 32,
      "maximumFramesPerSecond": 30,
      "initiallyVisible": true
    }
  })JSON";
  const bool rejected_panel = throws_exception<std::runtime_error>([&] {
    static_cast<void>(host::parse_named_surface_policy(manifest, "panel"));
  });

  manifest.canonical_surfaces = R"JSON({
    "pet": {
      "role": "desktop-overlay",
      "maximumWidth": 64,
      "maximumHeight": 32,
      "maximumFramesPerSecond": 30,
      "initiallyVisible": "yes"
    }
  })JSON";
  const bool rejected_untyped = throws_exception<std::runtime_error>([&] {
    static_cast<void>(host::parse_named_surface_policy(manifest, "pet"));
  });
  OMARCHY_CHECK(rejected_panel && rejected_untyped);
}

} // namespace

int main(int argc, char **argv) {
  QGuiApplication application(argc, argv);
  (void)application;
  try {
    authenticated_input_regions_reach_the_single_policy_decision();
    accepted_regions_are_one_enforcement_and_projection_decision();
    focus_and_sequence_are_owned_across_surfaces();
    close_sends_terminal_cancel_and_clears_authority();
    invalid_regions_preserve_last_accepted_state();
    policy_and_lifetime_are_fail_closed();
    initial_visibility_is_overlay_only_and_typed();
    return EXIT_SUCCESS;
  } catch (const std::exception &failure) {
    std::fprintf(stderr, "surface host test failed: %s\n", failure.what());
    return EXIT_FAILURE;
  }
}
