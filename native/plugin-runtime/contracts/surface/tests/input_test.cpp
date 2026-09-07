#include "test.hpp"

#include "omarchy/plugin_runtime/surface/input.hpp"

#include <initializer_list>

using namespace omarchy::plugin_runtime::surface;

struct InputStep {
  std::uint64_t sequence;
  InputPayload payload;
  InputValidation expected;
  const char *message;
  bool active = true;
};

void check_sequence(const TrustedAllocation &allocation,
                    std::initializer_list<InputStep> steps) {
  InputMirror mirror;
  for (const auto &step : steps)
    require(mirror.accept({.surface = allocation.surface,
                           .sequence = step.sequence,
                           .payload = step.payload},
                          allocation, step.active) == step.expected,
            step.message);
}

int main() {
  const auto check_text = [](const std::string &text, InputValidation expected) {
    OMARCHY_CHECK_WITH(require, validate_input_shape(Key{.key = 1, .text = text}) == expected &&
                  validate_input_shape(TextCommit{.text = text}) == expected);
  };
  for (const auto &text : {std::string{}, std::string("a\0b", 3),
                           std::string("\xef\xbf\xbf"), std::string("\xf4\x8f\xbf\xbf"),
                           std::string(kMaximumInputTextBytes, 'x')})
    check_text(text, InputValidation::accepted);
  for (const auto &text : {std::string("\xc0\x80"), std::string("\xed\xa0\x80"),
                           std::string("\xf4\x90\x80\x80"), std::string("\xf0\x90\x80"),
                           std::string("a\0\x80", 3), std::string(kMaximumInputTextBytes + 1, 'x')})
    check_text(text, InputValidation::invalid_text);
  const auto first = make_allocation({.id = 8, .generation = 2}, 100, 50,
                                     200, 100, 2, 1, 4096);
  const auto second = make_allocation({.id = 9, .generation = 2}, 100, 50,
                                      200, 100, 2, 1, 4096);
  OMARCHY_CHECK_WITH(require, first && second);
  InputMirror mirror;

  InputEvent motion{
      .surface = first->surface,
      .sequence = 1,
      .payload = PointerMotion{.position = {.x_q16 = 50U << 16,
                                             .y_q16 = 25U << 16}}};
  OMARCHY_CHECK_WITH(require, mirror.accept(motion, *first, true) == InputValidation::accepted);
  OMARCHY_CHECK_WITH(require, mirror.accept(motion, *first, true) ==
              InputValidation::replayed_sequence);
  std::get<PointerMotion>(motion.payload).position.x_q16 = 100U << 16;
  motion.sequence = 2;
  OMARCHY_CHECK_WITH(require, validate_input(motion, *first, true, false) ==
              InputValidation::coordinate_out_of_bounds);

  InputEvent focus{.surface = first->surface,
                   .sequence = 2,
                   .payload = FocusChanged{.focused = true}};
  OMARCHY_CHECK_WITH(require, mirror.accept(focus, *first, true) == InputValidation::accepted &&
              mirror.focused_surface() == first->surface);
  focus.surface = second->surface;
  focus.sequence = 3;
  OMARCHY_CHECK_WITH(require, mirror.accept(focus, *second, true) ==
              InputValidation::invalid_transition);
  focus.surface = first->surface;
  focus.sequence = 3;
  focus.payload = FocusChanged{.focused = false};
  OMARCHY_CHECK_WITH(require, mirror.accept(focus, *first, true) == InputValidation::accepted);
  focus.surface = second->surface;
  focus.sequence = 4;
  focus.payload = FocusChanged{.focused = true};
  OMARCHY_CHECK_WITH(require, mirror.accept(focus, *second, true) == InputValidation::accepted);

  InputEvent key{.surface = first->surface,
                 .sequence = 5,
                 .payload = Key{.key = 30,
                                .state = ButtonState::pressed,
                                .text = "x"}};
  OMARCHY_CHECK_WITH(require, validate_input(key, *first, true, false) ==
              InputValidation::not_focused);
  key.surface = second->surface;
  OMARCHY_CHECK_WITH(require, mirror.accept(key, *second, true) == InputValidation::accepted);
  std::get<Key>(key.payload).text = std::string("\xc0\x80", 2);
  key.sequence = 6;
  OMARCHY_CHECK_WITH(require, mirror.accept(key, *second, true) == InputValidation::invalid_text);

  struct ButtonStep {
    std::uint64_t sequence;
    std::uint32_t button;
    ButtonState state;
    std::uint32_t buttons;
    InputValidation expected;
  };
  for (const auto &step : {
      ButtonStep{6, 2, ButtonState::pressed, 2, InputValidation::accepted},
      ButtonStep{7, 2, ButtonState::pressed, 2, InputValidation::invalid_transition},
      ButtonStep{7, 2, ButtonState::released, 0, InputValidation::accepted},
      ButtonStep{8, 4, ButtonState::pressed, 4, InputValidation::accepted},
      ButtonStep{9, 8, ButtonState::pressed, 8, InputValidation::invalid_transition},
      ButtonStep{9, 4, ButtonState::released, 0, InputValidation::accepted}}) {
    OMARCHY_CHECK_WITH(require, mirror.accept(
        {.surface = first->surface, .sequence = step.sequence,
         .payload = PointerButton{.position = {}, .button = step.button,
                                  .state = step.state, .buttons = step.buttons}},
        *first, true) == step.expected);
  }

  TouchFrame begin{.phase = TouchFramePhase::begin,
                   .points = {{{.id = 3,
                                .state = TouchPointState::pressed,
                                .position = {1U << 16, 1U << 16}}}},
                   .count = 1};
  InputEvent touch{.surface = first->surface,
                   .sequence = 10,
                   .payload = begin};
  OMARCHY_CHECK_WITH(require, mirror.accept(touch, *first, true) == InputValidation::accepted);
  auto duplicate = begin;
  duplicate.phase = TouchFramePhase::update;
  duplicate.count = 2;
  duplicate.points[1] = duplicate.points[0];
  touch.sequence = 11;
  touch.payload = duplicate;
  OMARCHY_CHECK_WITH(require, mirror.accept(touch, *first, true) == InputValidation::invalid_code);
  begin.phase = TouchFramePhase::end;
  begin.points[0].state = TouchPointState::released;
  touch.payload = begin;
  OMARCHY_CHECK_WITH(require, mirror.accept(touch, *first, true) == InputValidation::accepted);

  check_sequence(*first, {
      {1, TouchFrame{
        .phase = TouchFramePhase::begin,
        .points = {{{.id = 1,
                     .state = TouchPointState::pressed,
                     .position = {1U << 16, 1U << 16}}}},
        .count = 1}, InputValidation::accepted, "complete touch frame begin rejected"},
      {2, TouchFrame{
        .phase = TouchFramePhase::update,
        .points = {{{.id = 2,
                     .state = TouchPointState::pressed,
                     .position = {2U << 16, 2U << 16}}}},
        .count = 1}, InputValidation::invalid_transition, "incomplete touch update accepted"},
      {3, TouchFrame{
        .phase = TouchFramePhase::update,
        .points = {{{.id = 1,
                     .state = TouchPointState::stationary,
                     .position = {1U << 16, 1U << 16}},
                    {.id = 2,
                     .state = TouchPointState::pressed,
                     .position = {2U << 16, 2U << 16}}}},
        .count = 2}, InputValidation::accepted, "complete touch update rejected"},
      {4, TouchFrame{
        .phase = TouchFramePhase::end,
        .points = {{{.id = 1,
                     .state = TouchPointState::released,
                     .position = {1U << 16, 1U << 16}},
                    {.id = 2,
                     .state = TouchPointState::released,
                     .position = {2U << 16, 2U << 16}}}},
        .count = 2}, InputValidation::accepted, "complete touch frame end rejected"},
  });

  InputEvent cancel{.surface = second->surface,
                    .sequence = 12,
                    .payload = Cancel{}};
  OMARCHY_CHECK_WITH(require, mirror.accept(cancel, *second, false) == InputValidation::accepted &&
              !mirror.focused_surface());
  cancel.sequence = 13;
  cancel.surface.generation--;
  OMARCHY_CHECK_WITH(require, mirror.accept(cancel, *second, false) ==
              InputValidation::stale_surface);

  check_sequence(*first, {
      {1, Wheel{.position = {}, .phase = WheelPhase::discrete}, InputValidation::accepted,
       "discrete wheel event rejected"},
      {2, Wheel{.position = {}, .phase = WheelPhase::update}, InputValidation::invalid_transition,
       "wheel update without begin accepted"},
      {3, Wheel{.position = {}, .phase = WheelPhase::begin}, InputValidation::accepted,
       "wheel begin rejected"},
      {4, Wheel{.position = {}, .phase = WheelPhase::discrete}, InputValidation::invalid_transition,
       "discrete wheel event accepted during an active gesture"},
      {5, Wheel{.position = {}, .phase = WheelPhase::update}, InputValidation::accepted,
       "active wheel update rejected"},
      {6, Wheel{.position = {}, .phase = WheelPhase::momentum}, InputValidation::accepted,
       "wheel momentum rejected"},
      {7, Wheel{.position = {}, .phase = WheelPhase::end}, InputValidation::accepted,
       "wheel end rejected"},
      {8, Wheel{.position = {}, .phase = WheelPhase::end}, InputValidation::invalid_transition,
       "duplicate wheel end accepted"},
  });

  const auto physical_key = [](std::uint32_t scan, ButtonState state,
                               bool repeat = false) {
    return Key{.key = 0x01000020, .native_scan_code = scan,
               .state = state, .auto_repeat = repeat, .text = {}};
  };
  check_sequence(*first, {
      {1, FocusChanged{.focused = true}, InputValidation::accepted,
       "key fixture focus rejected"},
      {2, physical_key(42, ButtonState::pressed), InputValidation::accepted,
       "left physical modifier press rejected"},
      {3, physical_key(54, ButtonState::pressed), InputValidation::accepted,
       "right physical modifier was conflated with the left key"},
      {4, physical_key(42, ButtonState::pressed), InputValidation::invalid_transition,
       "duplicate non-repeat key press accepted"},
      {5, physical_key(42, ButtonState::pressed, true), InputValidation::accepted,
       "repeat press for an active key rejected"},
      {6, physical_key(42, ButtonState::released, true), InputValidation::invalid_transition,
       "auto-repeat release changed physical key state"},
      {7, physical_key(42, ButtonState::released), InputValidation::accepted,
       "left physical modifier release rejected"},
      {8, physical_key(54, ButtonState::released), InputValidation::accepted,
       "right physical modifier release rejected"},
      {9, physical_key(54, ButtonState::released), InputValidation::invalid_transition,
       "key release without an active press accepted"},
  });

  const auto touch_frame = [](std::uint32_t id, TouchFramePhase phase,
                              TouchPointState state) {
    return TouchFrame{.phase = phase,
                       .points = {{{.id = id, .state = state, .position = {}}}},
                       .count = 1};
  };
  check_sequence(*first, {
      {1, touch_frame(2, TouchFramePhase::begin, TouchPointState::pressed),
       InputValidation::accepted, "active-cancel fixture touch begin rejected"},
      {2, Cancel{}, InputValidation::accepted,
       "cancel did not clear an active touch", false},
      {3, touch_frame(2, TouchFramePhase::update, TouchPointState::updated),
       InputValidation::invalid_transition, "touch continuation survived cancel"},
      {4, touch_frame(2, TouchFramePhase::begin, TouchPointState::pressed),
       InputValidation::accepted, "fresh touch begin after cancel rejected"},
  });

  check_sequence(*first, {
      {1, FocusChanged{.focused = true}, InputValidation::accepted,
       "focus-loss fixture focus rejected"},
      {2, PointerButton{.position = {}, .button = 1,
                         .state = ButtonState::pressed, .buttons = 1},
       InputValidation::accepted, "focus-loss fixture button press rejected"},
      {3, Key{.key = 65, .native_scan_code = 30,
               .state = ButtonState::pressed, .text = {}},
       InputValidation::accepted, "focus-loss fixture key press rejected"},
      {4, touch_frame(1, TouchFramePhase::begin, TouchPointState::pressed),
       InputValidation::accepted, "focus-loss fixture touch begin rejected"},
      {5, Wheel{.position = {}, .phase = WheelPhase::begin, .buttons = 1},
       InputValidation::accepted, "focus-loss fixture wheel begin rejected"},
      {6, FocusChanged{.focused = false}, InputValidation::accepted,
       "focus loss rejected"},
      {7, FocusChanged{.focused = true}, InputValidation::accepted,
       "refocus after state teardown rejected"},
      {8, PointerMotion{.position = {}, .buttons = 0}, InputValidation::accepted,
       "button state survived focus loss"},
      {9, Key{.key = 65, .native_scan_code = 30,
               .state = ButtonState::released, .text = {}},
       InputValidation::invalid_transition, "key state survived focus loss"},
      {10, touch_frame(1, TouchFramePhase::update, TouchPointState::updated),
       InputValidation::invalid_transition, "touch state survived focus loss"},
      {11, Wheel{.position = {}, .phase = WheelPhase::end, .buttons = 0},
       InputValidation::invalid_transition, "wheel state survived focus loss"},
  });

  {
    InputMirror capacity_mirror;
    for (std::uint64_t index = 0;
         index <= omarchy::plugin::wire::kMaximumPluginSurfaces; ++index) {
      const auto allocation = make_allocation(
          {.id = 100 + index, .generation = 1}, 10, 10, 10, 10, 1, 1, 4096);
      OMARCHY_CHECK_WITH(require, allocation.has_value());
      const InputEvent event{.surface = allocation->surface,
                             .sequence = index + 1,
                             .payload = PointerMotion{}};
      OMARCHY_CHECK_WITH(require, capacity_mirror.accept(event, *allocation, true) ==
                  (index < omarchy::plugin::wire::kMaximumPluginSurfaces
                       ? InputValidation::accepted
                       : InputValidation::invalid_transition));
    }
  }

  {
    InputMirror rollback_mirror;
    const auto focused = make_allocation({.id = 200, .generation = 1}, 10, 10,
                                         10, 10, 1, 1, 4096);
    OMARCHY_CHECK_WITH(require, focused.has_value());
    OMARCHY_CHECK_WITH(require, rollback_mirror.accept({.surface = focused->surface,
                                    .sequence = 1,
                                    .payload = FocusChanged{.focused = true}},
                                   *focused, true) == InputValidation::accepted);
    for (std::uint64_t index = 0; index < 20; ++index) {
      const auto rejected = make_allocation(
          {.id = 201 + index, .generation = 1}, 10, 10, 10, 10, 1, 1, 4096);
      OMARCHY_CHECK_WITH(require, rejected.has_value());
      OMARCHY_CHECK_WITH(require, rollback_mirror.accept(
                  {.surface = rejected->surface,
                   .sequence = index + 2,
                   .payload = FocusChanged{.focused = true}},
                  *rejected, true) == InputValidation::invalid_transition);
    }
    OMARCHY_CHECK_WITH(require, rollback_mirror.accept(
                {.surface = focused->surface,
                 .sequence = 22,
                 .payload = FocusChanged{.focused = false}},
                *focused, true) == InputValidation::accepted);
    const auto after_rejections = make_allocation(
        {.id = 250, .generation = 1}, 10, 10, 10, 10, 1, 1, 4096);
    OMARCHY_CHECK_WITH(require, after_rejections.has_value());
    OMARCHY_CHECK_WITH(require, rollback_mirror.accept(
                {.surface = after_rejections->surface,
                 .sequence = 23,
                 .payload = PointerMotion{}},
                *after_rejections, true) == InputValidation::accepted);
  }
}
