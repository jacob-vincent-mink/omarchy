#include "../../tests/support/test_assert.hpp"

#include "remote_surface.hpp"
#include "pointer_provenance.hpp"
#include "render_input_transport.hpp"

#include "omarchy/plugin_runtime/surface/render_messages.hpp"
#include "omarchy/plugin_runtime/surface/shared_layout.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QQuickWindow>
#include <QSizeF>
#include <QTest>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace omarchy::plugin_runtime::bridge {
class RemotePluginSurfaceTestAccess final {
public:
  static bool quickRedispatch(RemotePluginSurface &surface,
                              QMouseEvent &event) {
    return surface.childMouseEventFilter(surface.input_proxy_, &event);
  }
  static unsigned claimMismatch(const RemotePluginSurface &surface,
                                const QMouseEvent &event) {
    if (!surface.window_pointer_claim_)
      return 1U << 7;
    const auto &claim = *surface.window_pointer_claim_;
    return (claim.type != event.type() ? 1U : 0U) |
           (claim.timestamp != event.timestamp() ? 1U << 1 : 0U) |
           (claim.device != event.pointingDevice() ? 1U << 2 : 0U) |
           (claim.button != event.button() ? 1U << 3 : 0U) |
           (claim.buttons != event.buttons() ? 1U << 4 : 0U) |
           (claim.modifiers != event.modifiers() ? 1U << 5 : 0U) |
           (claim.global_position != event.globalPosition() ? 1U << 6 : 0U);
  }
  static bool hasClaim(const RemotePluginSurface &surface) {
    return surface.window_pointer_claim_.has_value();
  }
  static bool isBoundTo(const RemotePluginSurface &surface,
                        const QQuickWindow &window) {
    return surface.input_window_ == &window;
  }
};
} // namespace omarchy::plugin_runtime::bridge

namespace {

namespace bridge = omarchy::plugin_runtime::bridge;
namespace surface = omarchy::plugin_runtime::surface;
namespace wire = omarchy::plugin::wire;

using omarchy::plugin_runtime::test_support::require;

QMouseEvent mouse_press(QPointF position,
                        Qt::MouseEventSource source = Qt::MouseEventNotSynthesized) {
  return QMouseEvent(QEvent::MouseButtonPress, position, position, position,
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier, source);
}

class RecordingSink final : public bridge::RenderPacketSink {
public:
  bool send(const wire::EnvelopeHeader &value,
            std::span<const std::byte> bytes) override {
    ++calls;
    header = value;
    payload.assign(bytes.begin(), bytes.end());
    return accept;
  }

  wire::EnvelopeHeader header{};
  std::vector<std::byte> payload;
  std::size_t calls = 0;
  bool accept = true;
};

class RecordingInputRouter final : public bridge::HostInputRouter {
public:
  bool route(bridge::HostInputEvent event) override {
    events.push_back(std::move(event));
    return accept;
  }
  bool cancel(std::uint64_t device) override {
    cancelled_device = device;
    return accept;
  }
  std::vector<bridge::HostInputEvent> events;
  std::uint64_t cancelled_device = 0;
  bool accept = true;
};

class RecordingRegionRouter final : public bridge::HostInputRegionRouter {
public:
  bool apply(const surface::InputRegionUpdate &update) override {
    ++calls;
    last_generation = update.generation;
    return accept;
  }
  std::size_t calls = 0;
  std::uint64_t last_generation = 0;
  bool accept = true;
};

void configure_input_fixture(bridge::RemotePluginSurface &item,
                             RecordingInputRouter &router, std::uint64_t session,
                             surface::SurfaceKey key, std::uint32_t extent = 64) {
  auto transport = std::make_shared<bridge::AuthenticatedInputTransport>(
      session, std::make_shared<RecordingSink>());
  const auto allocation = surface::make_allocation(
      key, extent, extent, extent, extent, 1, 1, 4096);
  OMARCHY_CHECK(allocation && item.bindTransport(transport) &&
              item.configure(*allocation) && item.bindHostInputRouter(router));
}

class WindowInputProbe final : public QObject {
public:
  bool eventFilter(QObject *watched, QEvent *event) override {
    (void)watched;
    if (event->type() == QEvent::MouseButtonPress) {
      auto &mouse = *static_cast<QMouseEvent *>(event);
      saw_press = true;
      press_was_spontaneous = mouse.spontaneous();
      press_source = mouse.source();
      press_device_type = mouse.deviceType();
      press_timestamp = mouse.timestamp();
      press_device = mouse.pointingDevice();
      press_global_position = mouse.globalPosition();
      if (on_press)
        on_press(mouse);
    }
    return false;
  }

  bool saw_press = false;
  bool press_was_spontaneous = false;
  Qt::MouseEventSource press_source = Qt::MouseEventSynthesizedByApplication;
  QInputDevice::DeviceType press_device_type =
      QInputDevice::DeviceType::Unknown;
  ulong press_timestamp = 0;
  const QPointingDevice *press_device = nullptr;
  QPointF press_global_position;
  std::function<void(QMouseEvent &)> on_press;
};

class AbsorbingItem final : public QQuickItem {
public:
  explicit AbsorbingItem(QQuickItem *parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton);
  }

private:
  void mousePressEvent(QMouseEvent *event) override { event->accept(); }
  void mouseReleaseEvent(QMouseEvent *event) override { event->accept(); }
};

void test_qpa_pointer_provenance_is_captured_before_quick_redispatch() {
  QQuickWindow window;
  window.resize(96, 96);
  WindowInputProbe probe;
  window.installEventFilter(&probe);

  bridge::RemotePluginSurface item(window.contentItem());
  item.setPosition({0, 0});
  item.setSize({64, 64});
  RecordingInputRouter router;
  configure_input_fixture(item, router, 18, {.id = 12, .generation = 6});

  window.show();
  QCoreApplication::processEvents();
  router.events.clear();
  QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {12, 18});
  QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {12, 18});
  OMARCHY_CHECK(probe.saw_press);
  OMARCHY_CHECK(probe.press_was_spontaneous);
  OMARCHY_CHECK(probe.press_source == Qt::MouseEventNotSynthesized);
  OMARCHY_CHECK(probe.press_device_type == QInputDevice::DeviceType::Mouse);
  const auto pointer_events = [&] {
    return std::ranges::count_if(router.events, [](const auto &input) {
      return std::holds_alternative<surface::PointerButton>(input.payload);
    });
  };
  OMARCHY_CHECK(pointer_events() == 2);
  const auto press = std::ranges::find_if(router.events, [](const auto &input) {
    const auto *button = std::get_if<surface::PointerButton>(&input.payload);
    return button != nullptr &&
           button->state == surface::ButtonState::pressed;
  });
  const auto release =
      std::ranges::find_if(router.events, [](const auto &input) {
        const auto *button =
            std::get_if<surface::PointerButton>(&input.payload);
        return button != nullptr &&
               button->state == surface::ButtonState::released;
      });
  OMARCHY_CHECK(press != router.events.end() && release != router.events.end() &&
              press->trusted_physical && release->trusted_physical);

  const auto after_physical = pointer_events();
  auto direct_replay = mouse_press({12, 18});
  OMARCHY_CHECK(QCoreApplication::sendEvent(&item, &direct_replay) &&
              pointer_events() == after_physical + 1 &&
              !router.events.back().trusted_physical);

  const auto after_replay = pointer_events();
  auto synthetic_window = mouse_press({12, 18});
  QCoreApplication::sendEvent(&window, &synthetic_window);
  auto application_synthesized =
      mouse_press({12, 18}, Qt::MouseEventSynthesizedByApplication);
  QCoreApplication::sendEvent(&window, &application_synthesized);
  OMARCHY_CHECK(pointer_events() == after_replay);
  item.setWidth(63);
  QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {12, 18});
  QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {12, 18});
  OMARCHY_CHECK(pointer_events() == after_replay);
  item.setWidth(64);

  AbsorbingItem unrelated(&item);
  unrelated.setSize(item.size());
  unrelated.setZ(10);
  QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {12, 18});
  QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {12, 18});
  OMARCHY_CHECK(pointer_events() == after_replay);

  unrelated.setVisible(false);
  auto *touch_device = QTest::createTouchDevice();
  OMARCHY_CHECK(touch_device != nullptr);
  touch_device->setParent(&window);
  QTest::touchEvent(&window, touch_device)
      .press(0, {20, 20}, &window)
      .commit();
  QTest::touchEvent(&window, touch_device)
      .release(0, {20, 20}, &window)
      .commit();
  const auto touch_frames =
      std::ranges::count_if(router.events, [](const auto &input) {
        return std::holds_alternative<bridge::HostTouchFrame>(input.payload);
      });
  const bool touch_was_untrusted =
      std::ranges::all_of(router.events, [](const auto &input) {
        return !std::holds_alternative<bridge::HostTouchFrame>(input.payload) ||
               !input.trusted_physical;
      });
  OMARCHY_CHECK(touch_frames == 2 && touch_was_untrusted &&
              pointer_events() == after_replay);
}

void test_window_provenance_survives_one_quick_redispatch_only() {
  QQuickWindow window;
  window.resize(96, 96);
  WindowInputProbe probe;
  window.installEventFilter(&probe);

  bridge::RemotePluginSurface item(window.contentItem());
  item.setPosition({0, 0});
  item.setSize({64, 64});
  RecordingInputRouter router;
  configure_input_fixture(item, router, 19, {.id = 13, .generation = 7});

  AbsorbingItem blocker(window.contentItem());
  blocker.setPosition({0, 0});
  blocker.setSize({64, 64});
  blocker.setZ(100);
  bool redispatch_routed = false;
  probe.on_press = [&](QMouseEvent &physical) {
    QMouseEvent redispatched(
        QEvent::MouseButtonPress, QPointF(12, 18), QPointF(12, 18),
        physical.globalPosition(), Qt::LeftButton, Qt::LeftButton,
        Qt::NoModifier, Qt::MouseEventNotSynthesized,
        physical.pointingDevice());
    redispatched.setTimestamp(physical.timestamp());
    const auto mismatch =
        bridge::RemotePluginSurfaceTestAccess::claimMismatch(item,
                                                             redispatched);
    redispatch_routed =
        mismatch == 0 && !redispatched.spontaneous() &&
        bridge::RemotePluginSurfaceTestAccess::quickRedispatch(item,
                                                               redispatched) &&
        redispatched.isAccepted();
  };
  window.show();
  QCoreApplication::processEvents();
  OMARCHY_CHECK(bridge::RemotePluginSurfaceTestAccess::isBoundTo(item, window));

  QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {12, 18});
  OMARCHY_CHECK(probe.saw_press && probe.press_was_spontaneous &&
              probe.press_device != nullptr && redispatch_routed &&
              router.events.size() == 1 &&
              router.events.back().trusted_physical);
  probe.on_press = {};

  QMouseEvent replay(
      QEvent::MouseButtonPress, QPointF(12, 18), QPointF(12, 18),
      probe.press_global_position, Qt::LeftButton, Qt::LeftButton,
      Qt::NoModifier, Qt::MouseEventNotSynthesized, probe.press_device);
  replay.setTimestamp(probe.press_timestamp);
  OMARCHY_CHECK(bridge::RemotePluginSurfaceTestAccess::quickRedispatch(item,
                                                                 replay) &&
              !replay.isAccepted() && router.events.size() == 1);

  QMouseEvent application_event(
      QEvent::MouseButtonPress, QPointF(12, 18), QPointF(12, 18),
      probe.press_global_position, Qt::LeftButton, Qt::LeftButton,
      Qt::NoModifier, Qt::MouseEventSynthesizedByApplication,
      probe.press_device);
  application_event.setTimestamp(probe.press_timestamp);
  QCoreApplication::sendEvent(&window, &application_event);
  QMouseEvent application_redispatch(
      QEvent::MouseButtonPress, QPointF(12, 18), QPointF(12, 18),
      probe.press_global_position, Qt::LeftButton, Qt::LeftButton,
      Qt::NoModifier, Qt::MouseEventNotSynthesized, probe.press_device);
  application_redispatch.setTimestamp(probe.press_timestamp);
  OMARCHY_CHECK(bridge::RemotePluginSurfaceTestAccess::quickRedispatch(
              item, application_redispatch) &&
              !application_redispatch.isAccepted() && router.events.size() == 1);

  QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {12, 18});
}

void test_window_provenance_mismatch_and_overlap_expire_fail_closed() {
  QQuickWindow window;
  window.resize(96, 96);
  WindowInputProbe probe;
  window.installEventFilter(&probe);

  bridge::RemotePluginSurface first(window.contentItem());
  bridge::RemotePluginSurface second(window.contentItem());
  for (auto *item : {&first, &second}) {
    item->setPosition({0, 0});
    item->setSize({64, 64});
  }
  RecordingInputRouter first_router;
  RecordingInputRouter second_router;
  configure_input_fixture(first, first_router, 20, {.id = 14, .generation = 8});
  configure_input_fixture(second, second_router, 21, {.id = 15, .generation = 8});

  AbsorbingItem blocker(window.contentItem());
  blocker.setPosition({0, 0});
  blocker.setSize({64, 64});
  blocker.setZ(100);
  window.show();
  QCoreApplication::processEvents();

  const QPointingDevice other_device(
      QStringLiteral("other-mouse"), 55, QInputDevice::DeviceType::Mouse,
      QPointingDevice::PointerType::Generic,
      QInputDevice::Capability::Position, 1, 5);
  struct Mutation {
    QEvent::Type type = QEvent::MouseButtonPress;
    unsigned long timestamp_delta = 0;
    const QPointingDevice *device = nullptr;
    Qt::MouseButton button = Qt::LeftButton;
    Qt::MouseButtons buttons = Qt::LeftButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    QPointF global_delta{};
  };
  const std::array mutations{
      Mutation{.type = QEvent::MouseButtonRelease},
      Mutation{.timestamp_delta = 1},
      Mutation{.device = &other_device},
      Mutation{.button = Qt::RightButton},
      Mutation{.buttons = Qt::NoButton},
      Mutation{.modifiers = Qt::ShiftModifier},
      Mutation{.global_delta = {1, 0}}};

  for (const auto &mutation : mutations) {
    bool mismatch_rejected = false;
    bool consumed_claim_rejected = false;
    bool overlapping_claim_seen = false;
    probe.on_press = [&](QMouseEvent &physical) {
      QMouseEvent mismatch(
          mutation.type, QPointF(12, 18), QPointF(12, 18),
          physical.globalPosition() + mutation.global_delta,
          mutation.button, mutation.buttons, mutation.modifiers,
          Qt::MouseEventNotSynthesized,
          mutation.device ? mutation.device : physical.pointingDevice());
      mismatch.setTimestamp(physical.timestamp() + mutation.timestamp_delta);
      mismatch_rejected =
          bridge::RemotePluginSurfaceTestAccess::quickRedispatch(second,
                                                                 mismatch) &&
          !mismatch.isAccepted();

      QMouseEvent exact(
          QEvent::MouseButtonPress, QPointF(12, 18), QPointF(12, 18),
          physical.globalPosition(), Qt::LeftButton, Qt::LeftButton,
          Qt::NoModifier, Qt::MouseEventNotSynthesized,
          physical.pointingDevice());
      exact.setTimestamp(physical.timestamp());
      consumed_claim_rejected =
          bridge::RemotePluginSurfaceTestAccess::quickRedispatch(second,
                                                                 exact) &&
          !exact.isAccepted();
      overlapping_claim_seen =
          bridge::RemotePluginSurfaceTestAccess::hasClaim(first);
    };

    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, {12, 18});
    probe.on_press = {};
    OMARCHY_CHECK(mismatch_rejected && consumed_claim_rejected &&
                overlapping_claim_seen && first_router.events.empty() &&
                second_router.events.empty() &&
                !bridge::RemotePluginSurfaceTestAccess::hasClaim(second));
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, {12, 18});
    QCoreApplication::processEvents();
    OMARCHY_CHECK(!bridge::RemotePluginSurfaceTestAccess::hasClaim(first) &&
                !bridge::RemotePluginSurfaceTestAccess::hasClaim(second));
  }
}

void test_window_boundary_disconnects_before_base_teardown() {
  QQuickWindow first_window;
  QQuickWindow second_window;
  for (auto *window : {&first_window, &second_window}) {
    window->resize(96, 96);
    window->show();
  }
  QCoreApplication::processEvents();

  auto item =
      std::make_unique<bridge::RemotePluginSurface>(first_window.contentItem());
  item->setSize({64, 64});
  OMARCHY_CHECK(bridge::RemotePluginSurfaceTestAccess::isBoundTo(*item,
                                                            first_window));
  item->setParentItem(second_window.contentItem());
  QCoreApplication::processEvents();
  OMARCHY_CHECK(bridge::RemotePluginSurfaceTestAccess::isBoundTo(*item,
                                                            second_window));

  item.reset();
  QCoreApplication::processEvents();
  QTest::mouseClick(&second_window, Qt::LeftButton, Qt::NoModifier, {12, 18});
}

void test_physical_pointer_device_classification() {
  using bridge::detail::classify_pointer_provenance;

  const QPointingDevice mouse(
      QStringLiteral("mouse"), 1, QInputDevice::DeviceType::Mouse,
      QPointingDevice::PointerType::Generic, QInputDevice::Capability::Position,
      1, 5);
  const QPointingDevice touchpad(
      QStringLiteral("touchpad"), 2, QInputDevice::DeviceType::TouchPad,
      QPointingDevice::PointerType::Finger,
      QInputDevice::Capability::Position, 5, 3);
  const QPointingDevice touchscreen(
      QStringLiteral("touchscreen"), 3, QInputDevice::DeviceType::TouchScreen,
      QPointingDevice::PointerType::Finger,
      QInputDevice::Capability::Position, 10, 0);
  const QPointingDevice unknown(
      QStringLiteral("unknown"), 4, QInputDevice::DeviceType::Unknown,
      QPointingDevice::PointerType::Unknown, QInputDevice::Capability::None, 1,
      0);

  const struct Case {
    bool spontaneous;
    Qt::MouseEventSource source;
    const QPointingDevice *device;
    bool trusted;
  } cases[] = {
      {true, Qt::MouseEventNotSynthesized, &mouse, true},
      {true, Qt::MouseEventNotSynthesized, &touchpad, true},
      {true, Qt::MouseEventSynthesizedBySystem, &touchpad, false},
      {true, Qt::MouseEventSynthesizedByApplication, &mouse, false},
      {true, Qt::MouseEventNotSynthesized, &unknown, false},
      {true, Qt::MouseEventNotSynthesized, &touchscreen, false},
      {true, Qt::MouseEventNotSynthesized, nullptr, false},
      {false, Qt::MouseEventNotSynthesized, &touchpad, false}};
  for (const auto &test : cases)
    OMARCHY_CHECK(classify_pointer_provenance(test.spontaneous, test.source,
                                            test.device).trusted() == test.trusted);
}

void test_quick_item_pointer_delivery() {
  bridge::RemotePluginSurface item;
  RecordingInputRouter router;
  configure_input_fixture(item, router, 1, {.id = 1, .generation = 1});
  auto press = mouse_press({12, 34});
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(12, 34),
                      QPointF(12, 34), QPointF(12, 34), Qt::LeftButton, Qt::NoButton,
                      Qt::NoModifier, Qt::MouseEventNotSynthesized);
  OMARCHY_CHECK(QCoreApplication::sendEvent(&item, &press) &&
              QCoreApplication::sendEvent(&item, &release) &&
              router.events.size() == 2 &&
              std::get<surface::PointerButton>(router.events[0].payload).state ==
                  surface::ButtonState::pressed &&
              std::get<surface::PointerButton>(router.events[1].payload).state ==
                  surface::ButtonState::released &&
              !router.events[0].trusted_physical);

  QMouseEvent outside_release(
      QEvent::MouseButtonRelease, QPointF(65, 1), QPointF(65, 1),
      QPointF(65, 1), Qt::LeftButton, Qt::NoButton, Qt::NoModifier,
      Qt::MouseEventNotSynthesized);
  OMARCHY_CHECK(QCoreApplication::sendEvent(&item, &outside_release) &&
              router.cancelled_device != 0);

  router.cancelled_device = 0;
  QWheelEvent wheel_begin(QPointF(2, 2), QPointF(2, 2), QPoint(),
                          QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                          Qt::ScrollBegin, false);
  QWheelEvent wheel_update(QPointF(2, 2), QPointF(2, 2), QPoint(-3, -4),
                           QPoint(-120, -120), Qt::NoButton, Qt::NoModifier,
                           Qt::ScrollUpdate, false);
  QWheelEvent wheel_end(QPointF(65, 2), QPointF(65, 2), QPoint(), QPoint(),
                        Qt::NoButton, Qt::NoModifier, Qt::ScrollEnd, false);
  OMARCHY_CHECK(QCoreApplication::sendEvent(&item, &wheel_begin) &&
              QCoreApplication::sendEvent(&item, &wheel_update) &&
              std::get<surface::Wheel>(router.events.back().payload)
                      .pixel_delta_x_q16 == -(3 << 16) &&
              std::get<surface::Wheel>(router.events.back().payload)
                      .pixel_delta_y_q16 == -(4 << 16) &&
              QCoreApplication::sendEvent(&item, &wheel_end) &&
              router.cancelled_device != 0);

  auto synthesized = mouse_press({1, 2}, Qt::MouseEventSynthesizedByApplication);
  OMARCHY_CHECK(QCoreApplication::sendEvent(&item, &synthesized) &&
              router.events.size() == 5 &&
              !router.events.back().trusted_physical);

  auto system_synthesized = mouse_press({3, 4}, Qt::MouseEventSynthesizedBySystem);
  OMARCHY_CHECK(QCoreApplication::sendEvent(&item, &system_synthesized) &&
              router.events.size() == 6 &&
              !router.events.back().trusted_physical);
}

void test_router_unbind_is_idempotent_and_identity_checked() {
  bridge::RemotePluginSurface item;
  RecordingInputRouter first;
  RecordingInputRouter unrelated;
  configure_input_fixture(item, first, 2, {.id = 2, .generation = 2}, 16);

  item.unbindHostInputRouter(unrelated);
  auto routed = mouse_press({5, 6});
  OMARCHY_CHECK(QCoreApplication::sendEvent(&item, &routed) &&
              first.events.size() == 1);

  item.unbindHostInputRouter(first);
  item.unbindHostInputRouter(first);
  auto detached = mouse_press({7, 8});
  QCoreApplication::sendEvent(&item, &detached);
  OMARCHY_CHECK(first.events.size() == 1);
}

void test_router_destruction_orders() {
  bridge::RemotePluginSurface item;
  {
    RecordingInputRouter router;
    OMARCHY_CHECK(item.bindHostInputRouter(router));
    item.unbindHostInputRouter(router);
  }
  auto after_router = mouse_press({1, 1});
  QCoreApplication::sendEvent(&item, &after_router);

  auto sink = std::make_shared<RecordingSink>();
  auto transport =
      std::make_shared<bridge::AuthenticatedInputTransport>(16, sink);
  OMARCHY_CHECK(item.bindTransport(transport));
  const auto allocation = surface::make_allocation({.id = 10, .generation = 4},
                                                   2, 2, 2, 2, 1, 1, 4096);
  OMARCHY_CHECK(allocation && item.configure(*allocation));
  const surface::InputRegionUpdate update{
      .surface = allocation->surface,
      .generation = 1,
      .count = 0,
  };
  {
    RecordingRegionRouter router;
    RecordingRegionRouter unrelated;
    OMARCHY_CHECK(item.bindHostInputRegionRouter(router));
    OMARCHY_CHECK(!item.bindHostInputRegionRouter(unrelated));
    item.unbindHostInputRegionRouter(unrelated);
    OMARCHY_CHECK(item.updateInputRegions(update) && router.calls == 1 &&
                router.last_generation == 1);
    item.unbindHostInputRegionRouter(router);
    item.unbindHostInputRegionRouter(router);
  }
  auto newer_update = update;
  newer_update.generation = 2;
  OMARCHY_CHECK(!item.updateInputRegions(newer_update));

  RecordingInputRouter surviving_router;
  {
    auto short_lived_item = std::make_unique<bridge::RemotePluginSurface>();
    OMARCHY_CHECK(short_lived_item->bindHostInputRouter(surviving_router));
  }
  OMARCHY_CHECK(surviving_router.events.empty());
}

void test_input_region_projection_is_post_router_and_stable_on_reject() {
  auto sink = std::make_shared<RecordingSink>();
  auto transport =
      std::make_shared<bridge::AuthenticatedInputTransport>(17, sink);
  bridge::RemotePluginSurface item;
  OMARCHY_CHECK(item.bindTransport(transport));
  const auto allocation = surface::make_allocation({.id = 11, .generation = 5},
                                                   20, 10, 20, 10, 1, 1, 4096);
  OMARCHY_CHECK(allocation && item.configure(*allocation));
  RecordingRegionRouter router;
  OMARCHY_CHECK(item.bindHostInputRegionRouter(router));

  surface::InputRegionUpdate accepted{
      .surface = allocation->surface,
      .generation = 1,
      .regions = {{{.x = 2, .y = 3, .width = 4, .height = 5}}},
      .count = 1,
  };
  OMARCHY_CHECK(item.updateInputRegions(accepted) && router.calls == 1 &&
              item.inputRegions() == QList<QRect>{QRect(2, 3, 4, 5)});

  auto stale = accepted;
  stale.generation = 1;
  stale.regions[0].x = 9;
  OMARCHY_CHECK(!item.updateInputRegions(stale) && router.calls == 1 &&
              item.inputRegions() == QList<QRect>{QRect(2, 3, 4, 5)});

  auto wrong_surface = accepted;
  wrong_surface.surface.generation--;
  wrong_surface.generation = 2;
  OMARCHY_CHECK(!item.updateInputRegions(wrong_surface) && router.calls == 1 &&
              item.inputRegions() == QList<QRect>{QRect(2, 3, 4, 5)});

  router.accept = false;
  auto rejected = accepted;
  rejected.generation = 2;
  rejected.regions[0].x = 8;
  OMARCHY_CHECK(!item.updateInputRegions(rejected) && router.calls == 2 &&
              item.inputRegions() == QList<QRect>{QRect(2, 3, 4, 5)});

  router.accept = true;
  auto cleared = accepted;
  cleared.generation = 3;
  cleared.count = 0;
  OMARCHY_CHECK(item.updateInputRegions(cleared) && item.inputRegions().isEmpty());

  auto restored = accepted;
  restored.generation = 4;
  OMARCHY_CHECK(item.updateInputRegions(restored) && !item.inputRegions().isEmpty());
  const auto calls_before_destroy = router.calls;
  item.disconnect();
  OMARCHY_CHECK(item.inputRegions().isEmpty());
  auto after_destroy = accepted;
  after_destroy.generation = 5;
  OMARCHY_CHECK(!item.updateInputRegions(after_destroy) &&
              router.calls == calls_before_destroy &&
              item.inputRegions().isEmpty());
  item.unbindHostInputRegionRouter(router);
}

surface::InputEvent pointer(surface::SurfaceKey key, std::uint64_t sequence) {
  return {.surface = key,
          .sequence = sequence,
          .payload = surface::PointerButton{
              .position = {1U << surface::kQ16FractionBits,
                           1U << surface::kQ16FractionBits},
              .button = static_cast<std::uint32_t>(Qt::LeftButton),
              .state = surface::ButtonState::pressed,
              .buttons = static_cast<std::uint32_t>(Qt::LeftButton)}};
}

void test_owned_pixels_and_lifecycle() {
  auto sink = std::make_shared<RecordingSink>();
  auto transport =
      std::make_shared<bridge::AuthenticatedInputTransport>(9, sink);
  bridge::RemotePluginSurface item;
  surface::TrustedFrameSink &producer = item;
  OMARCHY_CHECK(item.bindTransport(transport));
  const auto allocation = surface::make_allocation({.id = 7, .generation = 2},
                                                   2, 2, 2, 2, 1, 1, 4096);
  OMARCHY_CHECK(allocation && producer.configure(*allocation) && item.connected() &&
              !item.ready() && item.surfaceId() == 7 &&
              item.surfaceGeneration() == 2);
  std::vector<std::byte> pixels(allocation->frame_bytes, std::byte{0x20});
  pixels[0] = std::byte{0xff};
  OMARCHY_CHECK(producer.present(allocation->surface, 1, pixels) && item.ready() &&
              item.frameSequence() == 1);
  const auto copied_byte = std::to_integer<unsigned char>(pixels[0]);
  pixels[0] = std::byte{0};

  QImage target(4, 4, QImage::Format_RGBA8888_Premultiplied);
  target.fill(Qt::transparent);
  QPainter painter(&target);
  item.setSize(QSizeF(4, 4));
  item.paint(&painter);
  painter.end();
  OMARCHY_CHECK(!target.isNull() && target.constBits()[0] == copied_byte);

  std::vector<std::byte> short_pixels(allocation->frame_bytes - 1);
  OMARCHY_CHECK(!producer.present(allocation->surface, 2, short_pixels) && item.ready() &&
              item.frameSequence() == 1 &&
              item.inspectionState() == QStringLiteral("invalid-pixels"));
  OMARCHY_CHECK(!producer.present(allocation->surface, 1, pixels) && item.ready() &&
              item.frameSequence() == 1);
  OMARCHY_CHECK(!item.present({.id = 7, .generation = 1}, 2, pixels) && item.ready());
  item.disconnect();
  OMARCHY_CHECK(!transport->connected() && !item.connected() && !item.ready() && !item.surfaceFocused() &&
              item.inspectionState() == QStringLiteral("disconnected"));
}

void test_authenticated_focus_and_input() {
  auto sink = std::make_shared<RecordingSink>();
  auto transport =
      std::make_shared<bridge::AuthenticatedInputTransport>(11, sink);
  bridge::RemotePluginSurface item;
  OMARCHY_CHECK(item.bindTransport(transport));
  const auto allocation = surface::make_allocation({.id = 8, .generation = 3},
                                                   4, 4, 4, 4, 1, 1, 4096);
  OMARCHY_CHECK(allocation && item.configure(*allocation));
  const surface::InputEvent focus{
      .surface = allocation->surface,
      .sequence = 1,
      .payload = surface::FocusChanged{.focused = true}};
  OMARCHY_CHECK(
      item.submitInput(focus) && item.surfaceFocused() && sink->calls == 1 &&
          sink->header.endpoint_role == wire::EndpointRole::render &&
          sink->header.launch_generation == 11 &&
          sink->header.role_protocol_version == surface::kRenderRoleVersion &&
          sink->header.flags == 0 &&
          sink->header.payload_length == sink->payload.size() &&
          sink->header.message_type ==
              static_cast<std::uint16_t>(surface::RenderMessageType::input) &&
          sink->header.correlation_id == 0);
  surface::InputEvent decoded_focus{};
  OMARCHY_CHECK(surface::decode_input_event(sink->payload, decoded_focus) &&
              decoded_focus == focus);

  const auto event = pointer(allocation->surface, 2);
  OMARCHY_CHECK(item.submitInput(event) && sink->calls == 2 &&
              sink->header.message_type ==
                  static_cast<std::uint16_t>(surface::RenderMessageType::input));
  surface::InputEvent decoded_input{};
  OMARCHY_CHECK(surface::decode_input_event(sink->payload, decoded_input) &&
              decoded_input.surface == event.surface &&
              decoded_input.sequence == event.sequence);
  OMARCHY_CHECK(!item.submitInput({.surface = {.id = 8, .generation = 2},
                             .sequence = 3,
                             .payload = surface::FocusChanged{.focused = false}}) &&
              sink->calls == 2 && item.surfaceFocused());

  sink->accept = false;
  OMARCHY_CHECK(
      !item.submitInput(
          {.surface = allocation->surface,
           .sequence = 3,
           .payload = surface::FocusChanged{.focused = false}}) &&
          !item.connected() && !item.ready() && transport->failed());
}

void test_invalid_transport_and_allocation() {
  auto sink = std::make_shared<RecordingSink>();
  auto invalid_transport =
      std::make_shared<bridge::AuthenticatedInputTransport>(0, sink);
  OMARCHY_CHECK(!invalid_transport->connected() && invalid_transport->failed());
  bridge::RemotePluginSurface item;
  OMARCHY_CHECK(!item.bindTransport(invalid_transport));
  surface::TrustedAllocation invalid{};
  OMARCHY_CHECK(!item.configure(invalid) && !item.connected() &&
              !invalid_transport->connected() &&
              item.inspectionState() == QStringLiteral("invalid-allocation"));

  auto valid_transport =
      std::make_shared<bridge::AuthenticatedInputTransport>(12, sink);
  bridge::RemotePluginSurface duplicate;
  OMARCHY_CHECK(duplicate.bindTransport(valid_transport));
  const auto allocation = surface::make_allocation({.id = 9, .generation = 1},
                                                   2, 2, 2, 2, 1, 1, 4096);
  OMARCHY_CHECK(allocation && duplicate.configure(*allocation) &&
              !duplicate.configure(*allocation) && !duplicate.connected() &&
              !valid_transport->connected());
  std::vector<std::byte> pixels(allocation->frame_bytes, std::byte{0xff});
  OMARCHY_CHECK(!duplicate.present(allocation->surface, 1, pixels) &&
              !duplicate.ready());

  auto first_transport =
      std::make_shared<bridge::AuthenticatedInputTransport>(14, sink);
  auto replacement_transport =
      std::make_shared<bridge::AuthenticatedInputTransport>(15, sink);
  bridge::RemotePluginSurface rebound;
  OMARCHY_CHECK(rebound.bindTransport(first_transport) &&
              !rebound.bindTransport(replacement_transport) &&
              first_transport->connected() &&
              !replacement_transport->connected());

  std::shared_ptr<bridge::RenderPacketSink> missing_sink;
  bridge::AuthenticatedInputTransport missing_transport(13, missing_sink);
  OMARCHY_CHECK(!missing_transport.connected() && missing_transport.failed());
}

void test_input_authority_sequence_focus_capture_and_touch_identity() {
  bridge::TrustedInputAuthority authority;
  const auto first = surface::make_allocation({.id = 21, .generation = 3},
                                              8, 8, 8, 8, 1, 1, 4096);
  const auto second = surface::make_allocation({.id = 22, .generation = 3},
                                               8, 8, 8, 8, 1, 1, 4096);
  OMARCHY_CHECK(first && second);

  const auto admit_active_physical = [&](const auto &allocation,
                                         std::uint64_t device,
                                         const auto &payload) {
    return authority.admit(allocation,
        {.payload = payload, .device = device, .trusted_physical = true}, true);
  };

  const surface::InputPoint point{.x_q16 = 1U << surface::kQ16FractionBits,
                                  .y_q16 = 1U << surface::kQ16FractionBits};
  auto press = admit_active_physical(*first, 41, surface::PointerButton{
           .position = point,
           .button = static_cast<std::uint32_t>(Qt::LeftButton),
           .state = surface::ButtonState::pressed,
           .buttons = static_cast<std::uint32_t>(Qt::LeftButton)});
  OMARCHY_CHECK(press && press->event.sequence == 1 && press->trusted_gesture &&
              authority.pointer_captured(first->surface, 41) &&
              !authority.pointer_captured(first->surface, 42) &&
              !authority.pointer_captured(second->surface, 41));

  OMARCHY_CHECK(!admit_active_physical(
                  *second, 42, surface::PointerMotion{.position = point, .buttons = 0}) &&
              !admit_active_physical(*first, 42, surface::PointerMotion{
                       .position = point,
                       .buttons = static_cast<std::uint32_t>(Qt::LeftButton)}));
  auto release = admit_active_physical(*first, 41, surface::PointerButton{
           .position = point,
           .button = static_cast<std::uint32_t>(Qt::LeftButton),
           .state = surface::ButtonState::released,
           .buttons = 0});
  OMARCHY_CHECK(release && release->event.sequence == 2 &&
              !authority.pointer_captured(first->surface, 41));

  auto interleaved = admit_active_physical(
      *second, 42, surface::PointerMotion{.position = point, .buttons = 0});
  OMARCHY_CHECK(interleaved && interleaved->event.sequence == 3);

  auto focus_first = authority.admit(
      *first, {.payload = surface::FocusChanged{.focused = true}}, true);
  auto invalid_switch = authority.admit(
      *second, {.payload = surface::FocusChanged{.focused = true}}, true);
  auto blur_first = authority.admit(
      *first, {.payload = surface::FocusChanged{.focused = false}}, true);
  auto focus_second = authority.admit(
      *second, {.payload = surface::FocusChanged{.focused = true}}, true);
  OMARCHY_CHECK(focus_first && focus_first->event.sequence == 4 &&
              !invalid_switch && blur_first && blur_first->event.sequence == 5 &&
              focus_second && focus_second->event.sequence == 6 &&
              authority.focused_surface() == second->surface);

  const surface::Key key_press{.key = static_cast<std::uint32_t>(Qt::Key_Return),
                               .native_scan_code = 28,
                               .state = surface::ButtonState::pressed,
                               .auto_repeat = false,
                               .text = "\r"};
  auto physical_key = admit_active_physical(*second, 43, key_press);
  auto repeated_key = key_press;
  repeated_key.auto_repeat = true;
  auto repeated = admit_active_physical(*second, 43, repeated_key);
  auto synthetic_key_event = key_press;
  synthetic_key_event.key = static_cast<std::uint32_t>(Qt::Key_A);
  synthetic_key_event.native_scan_code = 30;
  synthetic_key_event.text = "a";
  auto synthetic_key = authority.admit(
      *second,
      {.payload = synthetic_key_event,
       .device = 43,
       .trusted_physical = false},
      true);
  OMARCHY_CHECK(physical_key && physical_key->trusted_gesture && repeated &&
              !repeated->trusted_gesture && synthetic_key &&
              !synthetic_key->trusted_gesture);

  bridge::HostTouchFrame begin;
  begin.phase = surface::TouchFramePhase::begin;
  begin.count = 1;
  begin.points[0] = {.id = 400,
                     .state = surface::TouchPointState::pressed,
                     .position = point};
  auto touch_begin = admit_active_physical(*first, 51, begin);
  OMARCHY_CHECK(touch_begin && touch_begin->event.sequence == 10 &&
              std::get<surface::TouchFrame>(touch_begin->event.payload)
                      .points[0]
                      .id == 0 &&
              authority.touch_captured(first->surface, 51));

  bridge::HostTouchFrame update = begin;
  update.phase = surface::TouchFramePhase::update;
  update.points[0].state = surface::TouchPointState::updated;
  OMARCHY_CHECK(!admit_active_physical(*first, 52, update) &&
              !admit_active_physical(*second, 51, update));

  auto omitted = update;
  omitted.points[0] = {.id = 401,
                       .state = surface::TouchPointState::pressed,
                       .position = point};
  OMARCHY_CHECK(!admit_active_physical(*first, 51, omitted));

  auto add_contact = update;
  add_contact.count = 2;
  add_contact.points[0].state = surface::TouchPointState::stationary;
  add_contact.points[1] = {.id = 401,
                           .state = surface::TouchPointState::pressed,
                           .position = point};
  auto added = admit_active_physical(*first, 51, add_contact);
  OMARCHY_CHECK(added && added->event.sequence == 11);

  auto end = add_contact;
  end.phase = surface::TouchFramePhase::end;
  end.points[0].state = surface::TouchPointState::released;
  end.points[1].state = surface::TouchPointState::released;
  auto touch_end = admit_active_physical(*first, 51, end);
  begin.points[0].id = 900;
  auto reused = admit_active_physical(*first, 51, begin);
  OMARCHY_CHECK(touch_end && touch_end->event.sequence == 12 && reused &&
              reused->event.sequence == 13 &&
              std::get<surface::TouchFrame>(reused->event.payload)
                      .points[0]
                      .id == 0);
  const auto cancelled = authority.cancel(*first);
  authority.release(second->surface);
  OMARCHY_CHECK(cancelled && cancelled->sequence == 14 &&
              !authority.touch_captured(first->surface, 51) &&
              !authority.focused_surface());
  begin.points[0].id = 1200;
  auto after_cancel = admit_active_physical(*first, 52, begin);
  OMARCHY_CHECK(after_cancel && after_cancel->event.sequence == 15 &&
              std::get<surface::TouchFrame>(after_cancel->event.payload)
                      .points[0]
                      .id == 0 &&
              authority.touch_captured(first->surface, 52));
  authority.release(first->surface);
}

void test_synthesized_activation_routes_without_privileged_provenance() {
  bridge::TrustedInputAuthority authority;
  const auto allocation = surface::make_allocation(
      {.id = 31, .generation = 4}, 8, 8, 8, 8, 1, 1, 4096);
  OMARCHY_CHECK(allocation.has_value());
  const surface::InputPoint point{.x_q16 = 1U << surface::kQ16FractionBits,
                                  .y_q16 = 1U << surface::kQ16FractionBits};
  auto press = authority.admit(
      *allocation,
      {.payload = surface::PointerButton{
           .position = point,
           .button = static_cast<std::uint32_t>(Qt::LeftButton),
           .state = surface::ButtonState::pressed,
           .buttons = static_cast<std::uint32_t>(Qt::LeftButton)},
       .device = 61,
       .trusted_physical = false},
      true);
  OMARCHY_CHECK(press && !press->trusted_gesture &&
              authority.pointer_captured(allocation->surface, 61) &&
              !authority.surface_has_physical_activation(allocation->surface));
  const auto cancelled = authority.cancel(*allocation);
  OMARCHY_CHECK(cancelled &&
              !authority.pointer_captured(allocation->surface, 61));
}

} // namespace

int main(int argc, char **argv) {
  QGuiApplication application(argc, argv);
  (void)application;
  try {
    test_qpa_pointer_provenance_is_captured_before_quick_redispatch();
    test_window_provenance_survives_one_quick_redispatch_only();
    test_window_provenance_mismatch_and_overlap_expire_fail_closed();
    test_window_boundary_disconnects_before_base_teardown();
    test_physical_pointer_device_classification();
    test_quick_item_pointer_delivery();
    test_router_unbind_is_idempotent_and_identity_checked();
    test_router_destruction_orders();
    test_input_region_projection_is_post_router_and_stable_on_reject();
    test_owned_pixels_and_lifecycle();
    test_authenticated_focus_and_input();
    test_invalid_transport_and_allocation();
    test_input_authority_sequence_focus_capture_and_touch_identity();
    test_synthesized_activation_routes_without_privileged_provenance();
    return EXIT_SUCCESS;
  } catch (const std::exception &failure) {
    std::fprintf(stderr, "trusted bridge test failed: %s\n", failure.what());
    return EXIT_FAILURE;
  }
}
