#pragma once

#include "omarchy/plugin_runtime/surface/input.hpp"

#include <optional>
#include <variant>

namespace omarchy::plugin_runtime::bridge {

struct HostTouchPoint final {
  std::int64_t id = 0;
  surface::TouchPointState state = surface::TouchPointState::stationary;
  surface::InputPoint position;
};
struct HostTouchFrame final {
  surface::TouchFramePhase phase = surface::TouchFramePhase::update;
  std::array<HostTouchPoint, surface::kMaximumTouchPoints> points{};
  std::uint32_t count = 0;
  std::uint32_t modifiers = 0;
};
using HostInputPayload =
    std::variant<surface::PointerMotion, surface::PointerButton,
                 surface::Wheel, surface::Key, surface::TextCommit,
                 HostTouchFrame, surface::FocusChanged>;
struct HostInputEvent final {
  HostInputPayload payload;
  std::uint64_t device = 0;
  bool trusted_physical = false;
};

enum class InputActivationKind { pointer, touch, key };

// Classifies payload shape only; provenance and admission remain caller checks.
template <typename Touch = surface::TouchFrame, typename Payload>
std::optional<InputActivationKind> input_activation_kind(const Payload &payload) noexcept {
  if (const auto *event = std::get_if<surface::PointerButton>(&payload);
      event && event->state == surface::ButtonState::pressed)
    return InputActivationKind::pointer;
  if (const auto *event = std::get_if<Touch>(&payload);
      event && event->phase == surface::TouchFramePhase::begin)
    return InputActivationKind::touch;
  if (const auto *event = std::get_if<surface::Key>(&payload);
      event && event->state == surface::ButtonState::pressed && !event->auto_repeat)
    return InputActivationKind::key;
  return std::nullopt;
}

class TrustedInputAuthority final {
public:
  struct Admission final {
    surface::InputEvent event;
    bool trusted_gesture = false;
  };

  [[nodiscard]] std::optional<Admission>
  admit(const surface::TrustedAllocation &allocation,
        HostInputEvent input, bool active);
  [[nodiscard]] std::optional<surface::InputEvent>
  cancel(const surface::TrustedAllocation &allocation) noexcept;
  void release(surface::SurfaceKey surface) noexcept;

  [[nodiscard]] std::optional<surface::SurfaceKey>
  focused_surface() const noexcept;
  [[nodiscard]] bool pointer_captured(surface::SurfaceKey surface,
                                      std::uint64_t device) const noexcept;
  [[nodiscard]] bool touch_captured(surface::SurfaceKey surface,
                                    std::uint64_t device) const noexcept;
  [[nodiscard]] bool wheel_captured(surface::SurfaceKey surface,
                                    std::uint64_t device) const noexcept;
  [[nodiscard]] bool
  surface_has_physical_activation(surface::SurfaceKey surface) const noexcept;

private:
  struct Contact final {
    std::int64_t raw_id = 0;
    bool active = false;
  };
  [[nodiscard]] std::optional<surface::TouchFrame>
  normalize_touch(const HostTouchFrame &frame, std::uint64_t device) const;
  void commit_touch(const HostTouchFrame &frame,
                    const surface::TouchFrame &normalized) noexcept;
  void clear_surface_state(surface::SurfaceKey surface) noexcept;
  void clear_all_state() noexcept;

  surface::InputMirror mirror_;
  std::uint64_t sequence_ = 0;
  surface::SurfaceKey pointer_surface_{};
  std::uint64_t pointer_device_ = 0;
  std::uint32_t captured_buttons_ = 0;
  std::uint32_t trusted_buttons_ = 0;
  surface::SurfaceKey touch_surface_{};
  std::uint64_t touch_device_ = 0;
  bool trusted_touch_ = false;
  surface::SurfaceKey wheel_surface_{};
  std::uint64_t wheel_device_ = 0;
  std::array<Contact, surface::kMaximumTouchPoints> contacts_{};
};

} // namespace omarchy::plugin_runtime::bridge
