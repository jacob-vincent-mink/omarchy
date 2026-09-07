#include "../../tests/support/child_process.hpp"
#include "../../tests/support/test_assert.hpp"
#include "../../tests/support/temporary_directory.hpp"

#include "worker_runtime.hpp"

#include "qt_touch_injector.hpp"
#include "omarchy/plugin_runtime/sandbox/policy.h"
#include "omarchy/plugin_runtime/unique_fd.hpp"

#include <QBuffer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLibraryInfo>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QScreen>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTimer>
#include <QTouchEvent>
#include <QWheelEvent>

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using omarchy::plugin_runtime::test_support::expect_child_exit;
using omarchy::plugin_runtime::test_support::TemporaryDirectory;

namespace surface = omarchy::plugin_runtime::surface;
namespace worker = omarchy::plugin_runtime::worker;

using omarchy::plugin_runtime::test_support::require;

std::filesystem::path fixture(const char *name) {
  return std::filesystem::path(WORKER_FIXTURE_ROOT) / name;
}

class ExactQmlTree : public omarchy::plugin_runtime::test_support::TemporaryDirectory {
public:
  ExactQmlTree() {
    const std::filesystem::path source(
        QLibraryInfo::path(QLibraryInfo::QmlImportsPath).toStdString());
    for (const auto &relative :
         omarchy::plugin_runtime::sandbox::trusted_qml_files()) {
      const auto destination = root_ / relative;
      std::filesystem::create_directories(destination.parent_path());
      std::filesystem::copy_file(source / relative, destination);
    }
  }
  [[nodiscard]] const std::filesystem::path &root() const { return root_; }
};

class Mapping {
public:
  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;
  Mapping(int descriptor, std::size_t size) : size_(size) {
    address_ = static_cast<std::byte *>(
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0));
    OMARCHY_CHECK(address_ != MAP_FAILED);
  }
  ~Mapping() {
    if (address_ != MAP_FAILED)
      munmap(address_, size_);
  }
  [[nodiscard]] std::span<const std::byte> bytes() const {
    return {address_, size_};
  }

private:
  std::byte *address_ = reinterpret_cast<std::byte *>(MAP_FAILED);
  std::size_t size_ = 0;
};

using UniqueFd = omarchy::plugin_runtime::UniqueFd;

auto frame_allocation(surface::SurfaceKey key, std::uint32_t logical_width,
                      std::uint32_t logical_height, std::uint32_t pixel_width,
                      std::uint32_t pixel_height, std::uint32_t dpr_numerator,
                      std::uint32_t dpr_denominator) {
  const auto page_size = sysconf(_SC_PAGESIZE);
  OMARCHY_CHECK(page_size > 0);
  auto allocation = surface::make_allocation(
      key, logical_width, logical_height, pixel_width, pixel_height,
      dpr_numerator, dpr_denominator, static_cast<std::uint64_t>(page_size));
  OMARCHY_CHECK(allocation.has_value());
  return allocation;
}

UniqueFd frame_descriptor(const surface::TrustedAllocation &allocation,
                          const char *name = "worker-frame-test") {
  UniqueFd descriptor(static_cast<int>(
      syscall(SYS_memfd_create, name, MFD_CLOEXEC)));
  OMARCHY_CHECK(descriptor && ftruncate(descriptor.get(),
                                 static_cast<off_t>(allocation.mapping_bytes)) == 0);
  return descriptor;
}

class WorkerMapping final : public Mapping {
  WorkerMapping(UniqueFd descriptor, worker::WorkerRuntime &runtime,
                const surface::TrustedAllocation &allocation)
      : Mapping(descriptor.get(), allocation.mapping_bytes) {
    UniqueFd worker_descriptor(fcntl(descriptor.get(), F_DUPFD_CLOEXEC, 64));
    OMARCHY_CHECK(static_cast<bool>(worker_descriptor));
    const auto result = runtime.allocate(allocation, worker_descriptor.release());
    require(static_cast<bool>(result), "worker allocation failed: " + result.detail);
  }

public:
  WorkerMapping(worker::WorkerRuntime &runtime,
                const surface::TrustedAllocation &allocation)
      : WorkerMapping(frame_descriptor(allocation), runtime, allocation) {}
};

QImage frame_image(const surface::TrustedAllocation &allocation,
                   const surface::FrameConsumer &consumer) {
  return QImage(reinterpret_cast<const uchar *>(consumer.last_frame()->pixels.data()),
                static_cast<int>(allocation.pixel_width),
                static_cast<int>(allocation.pixel_height),
                static_cast<int>(allocation.stride),
                QImage::Format_RGBA8888_Premultiplied);
}

class InputEventProbe final : public QObject {
public:
  QEvent::Type type = QEvent::None;
  QPointF position;
  QPoint pixel_delta;
  QPoint angle_delta;
  Qt::MouseButton button = Qt::NoButton;
  Qt::MouseButtons buttons = Qt::NoButton;
  Qt::KeyboardModifiers modifiers = Qt::NoModifier;
  Qt::ScrollPhase phase = Qt::NoScrollPhase;
  bool inverted = false;
  int key = 0;
  quint32 native_scan_code = 0;
  QString text;
  bool auto_repeat = false;
  int replacement_start = 0;
  int replacement_length = 0;
  QList<QEventPoint> touch_points;
  std::size_t touch_cancel_count = 0;

protected:
  bool eventFilter(QObject *, QEvent *event) override {
    if (const auto *mouse = dynamic_cast<QMouseEvent *>(event)) {
      type = event->type();
      position = mouse->position();
      button = mouse->button();
      buttons = mouse->buttons();
      modifiers = mouse->modifiers();
    } else if (const auto *wheel = dynamic_cast<QWheelEvent *>(event)) {
      type = event->type();
      position = wheel->position();
      pixel_delta = wheel->pixelDelta();
      angle_delta = wheel->angleDelta();
      buttons = wheel->buttons();
      modifiers = wheel->modifiers();
      phase = wheel->phase();
      inverted = wheel->inverted();
    } else if (const auto *key_event = dynamic_cast<QKeyEvent *>(event)) {
      type = event->type();
      key = key_event->key();
      native_scan_code = key_event->nativeScanCode();
      modifiers = key_event->modifiers();
      text = key_event->text();
      auto_repeat = key_event->isAutoRepeat();
    } else if (const auto *commit =
                   dynamic_cast<QInputMethodEvent *>(event)) {
      type = event->type();
      text = commit->commitString();
      replacement_start = commit->replacementStart();
      replacement_length = commit->replacementLength();
    } else if (const auto *touch = dynamic_cast<QTouchEvent *>(event)) {
      type = event->type();
      if (event->type() == QEvent::TouchCancel)
        ++touch_cancel_count;
      modifiers = touch->modifiers();
      touch_points = touch->points();
    }
    return false;
  }
};

bool deliver_input(worker::WorkerRuntime &runtime, surface::SurfaceKey key,
                   std::uint64_t sequence, surface::InputPayload payload) {
  return static_cast<bool>(runtime.input(
      {.surface = key, .sequence = sequence, .payload = std::move(payload)}));
}

void render_and_input() {
  worker::WorkerRuntime runtime(fixture("expressive"));
  const auto prepared = runtime.prepare_trusted_qt_types();
  if (!prepared)
    throw std::runtime_error("trusted Qt type preparation failed: " +
                             prepared.detail);
  OMARCHY_CHECK(static_cast<bool>(runtime.load_surface_entry("barWidget", "Main.qml")));
  OMARCHY_CHECK(runtime.loaded() && runtime.object_count() > 2);
  OMARCHY_CHECK(static_cast<bool>(runtime.select_software_profile(
              surface::software_profile_offer())));
  const auto allocation = frame_allocation(
      {.id = 41, .generation = 9}, 64, 32, 64, 32, 1, 1);
  WorkerMapping mapping(runtime, *allocation);
  OMARCHY_CHECK(runtime.active() && runtime.render_requested());
  const auto published = runtime.render();
  OMARCHY_CHECK(published.has_value() && published->frame_sequence == 1 &&
              published->slot_sequence == 2);
  auto consumer = surface::FrameConsumer::create(*allocation);
  OMARCHY_CHECK(consumer.has_value() &&
              consumer->consume(mapping.bytes(), *published) ==
                  surface::ConsumeResult::accepted);
  const auto *frame = consumer->last_frame();
  OMARCHY_CHECK(frame != nullptr && frame->pixels.size() == allocation->frame_bytes);
  OMARCHY_CHECK(
      std::ranges::any_of(
          frame->pixels, [](std::byte value) { return value != std::byte{0}; }));
  const auto first_pixels = frame->pixels;
  bool animation_changed = false;
  for (int sample = 0; sample < 6 && !animation_changed; ++sample) {
    QEventLoop animation_loop;
    QTimer::singleShot(40, &animation_loop, &QEventLoop::quit);
    animation_loop.exec();
    const auto animated = runtime.render();
    OMARCHY_CHECK(animated.has_value() &&
                consumer->consume(mapping.bytes(), *animated) ==
                    surface::ConsumeResult::accepted);
    animation_changed = consumer->last_frame()->pixels != first_pixels;
  }
  OMARCHY_CHECK(animation_changed);

  OMARCHY_CHECK(deliver_input(runtime, allocation->surface, 1,
                             surface::FocusChanged{.focused = true}));
  surface::InputEvent press{
      .surface = allocation->surface,
      .sequence = 2,
      .payload = surface::PointerButton{
          .position = {.x_q16 = 10U << 16, .y_q16 = 10U << 16},
          .button = static_cast<std::uint32_t>(Qt::LeftButton),
          .state = surface::ButtonState::pressed,
          .buttons = static_cast<std::uint32_t>(Qt::LeftButton)},
  };
  OMARCHY_CHECK(static_cast<bool>(runtime.input(press)));
  OMARCHY_CHECK(!static_cast<bool>(runtime.input(press)));
  auto release = press;
  release.sequence = 3;
  release.payload = surface::PointerButton{
      .position = {.x_q16 = 10U << 16, .y_q16 = 10U << 16},
      .button = static_cast<std::uint32_t>(Qt::LeftButton),
      .state = surface::ButtonState::released,
      .buttons = 0};
  OMARCHY_CHECK(static_cast<bool>(runtime.input(release)));
  surface::InputEvent touch{
      .surface = allocation->surface,
      .sequence = 4,
      .payload = surface::TouchFrame{
          .phase = surface::TouchFramePhase::begin,
          .points = {{surface::TouchPoint{
              .id = 1,
              .state = surface::TouchPointState::pressed,
              .position = {.x_q16 = 10U << 16, .y_q16 = 10U << 16}}}},
          .count = 1},
  };
  OMARCHY_CHECK(static_cast<bool>(runtime.input(touch)));
  touch.sequence = 5;
  touch.payload = surface::TouchFrame{
      .phase = surface::TouchFramePhase::update,
      .points = {{surface::TouchPoint{
          .id = 1,
          .state = surface::TouchPointState::updated,
          .position = {.x_q16 = 11U << 16, .y_q16 = 11U << 16}}}},
      .count = 1};
  OMARCHY_CHECK(static_cast<bool>(runtime.input(touch)));
  touch.sequence = 6;
  touch.payload = surface::TouchFrame{
      .phase = surface::TouchFramePhase::end,
      .points = {{surface::TouchPoint{
          .id = 1,
          .state = surface::TouchPointState::released,
          .position = {.x_q16 = 11U << 16, .y_q16 = 11U << 16}}}},
      .count = 1};
  OMARCHY_CHECK(static_cast<bool>(runtime.input(touch)));
  OMARCHY_CHECK(deliver_input(runtime, allocation->surface, 7,
                             surface::FocusChanged{.focused = false}) &&
              !runtime.focused());
  OMARCHY_CHECK(runtime.render_requested());
  OMARCHY_CHECK(runtime.render().has_value());

  OMARCHY_CHECK(static_cast<bool>(runtime.suspend(allocation->surface)));
  OMARCHY_CHECK(!runtime.render().has_value());
  OMARCHY_CHECK(!runtime.resume({.id = 41, .generation = 8}));
  OMARCHY_CHECK(static_cast<bool>(runtime.resume(allocation->surface)));
  OMARCHY_CHECK(runtime.render().has_value());
  OMARCHY_CHECK(static_cast<bool>(runtime.release(allocation->surface)));
  OMARCHY_CHECK(!runtime.allocated());
}

void headless_entry_has_no_surface_authority() {
  worker::WorkerRuntime runtime(fixture("expressive"));
  OMARCHY_CHECK(static_cast<bool>(runtime.load_entry("Main.qml")) &&
              runtime.loaded() && runtime.object_count() > 2);
  OMARCHY_CHECK(!runtime.surface_key("") &&
              !static_cast<bool>(runtime.bind_surface(
                  "", {.id = 91, .generation = 7})));
  OMARCHY_CHECK(static_cast<bool>(runtime.select_software_profile(
              surface::software_profile_offer())));
  const auto allocation = frame_allocation(
      {.id = 91, .generation = 7}, 32, 16, 32, 16, 1, 1);
  const int descriptor = frame_descriptor(*allocation, "headless-frame-test").release();
  const auto result = runtime.allocate(*allocation, descriptor);
  OMARCHY_CHECK(!result && result.failure == worker::RuntimeFailure::stale_surface &&
              fcntl(descriptor, F_GETFD) < 0 && errno == EBADF &&
              !runtime.allocated() && !runtime.active() &&
              !runtime.render().has_value());
}

void typed_input_projects_exact_qt_events() {
  worker::WorkerRuntime runtime(fixture("expressive"));
  OMARCHY_CHECK(static_cast<bool>(runtime.load_surface_entry("barWidget", "Main.qml")) &&
              static_cast<bool>(runtime.select_software_profile(
                  surface::software_profile_offer())));
  const auto allocation = frame_allocation(
      {.id = 44, .generation = 12}, 64, 32, 128, 64, 2, 1);
  auto descriptor = frame_descriptor(*allocation, "worker-input-test");
  const int worker_descriptor = fcntl(descriptor.get(), F_DUPFD_CLOEXEC, 64);
  descriptor.reset();
  OMARCHY_CHECK(worker_descriptor >= 0 &&
              static_cast<bool>(runtime.allocate(*allocation,
                                                 worker_descriptor)));

  InputEventProbe probe;
  QCoreApplication::instance()->installEventFilter(&probe);
  std::uint64_t sequence = 1;
  const auto input = [&](surface::InputPayload payload) {
    return deliver_input(runtime, allocation->surface, sequence, std::move(payload));
  };
  const auto accepted = [&](surface::InputPayload payload) {
    const bool result = input(std::move(payload));
    ++sequence;
    return result;
  };

  OMARCHY_CHECK(accepted(surface::FocusChanged{.focused = true}) &&
              runtime.focused());
  OMARCHY_CHECK(accepted(surface::PointerMotion{
              .position = {.x_q16 = (3U << 16) + (1U << 15),
                           .y_q16 = (4U << 16) + (1U << 14)},
              .buttons = 0,
              .modifiers = static_cast<std::uint32_t>(Qt::ShiftModifier)}) &&
              probe.type == QEvent::MouseMove &&
              probe.position == QPointF(7.0, 8.5) &&
              probe.buttons == Qt::NoButton &&
              probe.modifiers == Qt::ShiftModifier);
  const auto press = surface::PointerButton{
      .position = {.x_q16 = 5U << 16, .y_q16 = 6U << 16},
      .button = static_cast<std::uint32_t>(Qt::RightButton),
      .state = surface::ButtonState::pressed,
      .buttons = static_cast<std::uint32_t>(Qt::RightButton),
      .modifiers = static_cast<std::uint32_t>(Qt::ControlModifier)};
  OMARCHY_CHECK(accepted(press) && probe.type == QEvent::MouseButtonPress &&
              probe.position == QPointF(10.0, 12.0) &&
              probe.button == Qt::RightButton &&
              probe.buttons == Qt::RightButton &&
              probe.modifiers == Qt::ControlModifier);
  OMARCHY_CHECK(accepted(surface::Wheel{
              .position = {.x_q16 = 5U << 16, .y_q16 = 6U << 16},
              .pixel_delta_x_q16 = (1 << 16) + (1 << 15),
              .pixel_delta_y_q16 = -(2 << 16),
              .angle_delta_x = 120,
              .angle_delta_y = -240,
              .phase = surface::WheelPhase::discrete,
              .buttons = static_cast<std::uint32_t>(Qt::RightButton),
              .modifiers = static_cast<std::uint32_t>(Qt::AltModifier),
              .inverted = true}) &&
              probe.type == QEvent::Wheel &&
              probe.pixel_delta == QPoint(3, -4) &&
              probe.angle_delta == QPoint(120, -240) &&
              probe.phase == Qt::NoScrollPhase && probe.inverted &&
              probe.buttons == Qt::RightButton &&
              probe.modifiers == Qt::AltModifier);
  for (const auto [input_phase, qt_phase] :
       {std::pair{surface::WheelPhase::begin, Qt::ScrollBegin},
        std::pair{surface::WheelPhase::update, Qt::ScrollUpdate},
        std::pair{surface::WheelPhase::momentum, Qt::ScrollMomentum},
        std::pair{surface::WheelPhase::end, Qt::ScrollEnd}}) {
    OMARCHY_CHECK(accepted(surface::Wheel{
                .position = {.x_q16 = 5U << 16, .y_q16 = 6U << 16},
                .phase = input_phase,
                .buttons = static_cast<std::uint32_t>(Qt::RightButton)}) &&
                probe.type == QEvent::Wheel && probe.phase == qt_phase);
  }

  const auto key_press = surface::Key{
      .key = static_cast<std::uint32_t>(Qt::Key_A),
      .native_scan_code = 30,
      .modifiers = static_cast<std::uint32_t>(Qt::ShiftModifier),
      .state = surface::ButtonState::pressed,
      .auto_repeat = false,
      .text = "A"};
  OMARCHY_CHECK(accepted(key_press) && probe.type == QEvent::KeyPress &&
              probe.key == Qt::Key_A && probe.native_scan_code == 30 &&
              probe.modifiers == Qt::ShiftModifier && probe.text == "A" &&
              !probe.auto_repeat);
  auto repeated = key_press;
  repeated.auto_repeat = true;
  OMARCHY_CHECK(accepted(repeated) && probe.type == QEvent::KeyPress &&
              probe.auto_repeat);
  auto invalid_repeat_release = repeated;
  invalid_repeat_release.state = surface::ButtonState::released;
  OMARCHY_CHECK(!input(invalid_repeat_release));
  OMARCHY_CHECK(accepted(surface::TextCommit{.text = std::string("\xc3\xa9", 2),
                                       .replacement_start = -1,
                                       .replacement_length = 1}) &&
              probe.type == QEvent::InputMethod && probe.text == QString(u"é") &&
              probe.replacement_start == -1 &&
              probe.replacement_length == 1);
  auto key_release = key_press;
  key_release.state = surface::ButtonState::released;
  key_release.text.clear();
  OMARCHY_CHECK(accepted(key_release) && probe.type == QEvent::KeyRelease &&
              !probe.auto_repeat);

  surface::TouchFrame touch_begin{
      .phase = surface::TouchFramePhase::begin,
      .points = {{
          {.id = 1,
           .state = surface::TouchPointState::pressed,
           .position = {.x_q16 = 8U << 16, .y_q16 = 9U << 16}},
          {.id = 2,
           .state = surface::TouchPointState::pressed,
           .position = {.x_q16 = 12U << 16, .y_q16 = 13U << 16}},
      }},
      .count = 2,
      .modifiers = static_cast<std::uint32_t>(Qt::MetaModifier)};
  OMARCHY_CHECK(accepted(touch_begin) && probe.type == QEvent::TouchBegin &&
              probe.touch_points.size() == 2 &&
              probe.touch_points[0].id() == 1 &&
              probe.touch_points[0].state() == QEventPoint::State::Pressed &&
              probe.touch_points[1].id() == 2 &&
              probe.touch_points[1].state() == QEventPoint::State::Pressed &&
              probe.modifiers == Qt::MetaModifier);
  auto touch_update = touch_begin;
  touch_update.phase = surface::TouchFramePhase::update;
  touch_update.points[0].state = surface::TouchPointState::updated;
  touch_update.points[0].position.x_q16 = 9U << 16;
  touch_update.points[1].state = surface::TouchPointState::stationary;
  OMARCHY_CHECK(accepted(touch_update) && probe.type == QEvent::TouchUpdate &&
              probe.touch_points.size() == 2 &&
              probe.touch_points[0].state() == QEventPoint::State::Updated &&
              probe.touch_points[1].state() ==
                  QEventPoint::State::Stationary);
  auto touch_end = touch_update;
  touch_end.phase = surface::TouchFramePhase::end;
  touch_end.points[0].state = surface::TouchPointState::released;
  touch_end.points[1].state = surface::TouchPointState::released;
  OMARCHY_CHECK(accepted(touch_end) && probe.type == QEvent::TouchEnd &&
              probe.touch_points.size() == 2);

  auto pointer_release = press;
  pointer_release.state = surface::ButtonState::released;
  pointer_release.buttons = 0;
  OMARCHY_CHECK(accepted(pointer_release));
  OMARCHY_CHECK(accepted(key_press));
  OMARCHY_CHECK(accepted(touch_begin));
  OMARCHY_CHECK(accepted(surface::Cancel{}) && !runtime.focused());
  OMARCHY_CHECK(!input(pointer_release) && !input(key_release));
  OMARCHY_CHECK(accepted(surface::FocusChanged{.focused = true}));
  OMARCHY_CHECK(!input(key_release));
  OMARCHY_CHECK(accepted(key_press) && accepted(press) && accepted(touch_begin));
  OMARCHY_CHECK(static_cast<bool>(runtime.suspend(allocation->surface)) &&
              !runtime.focused() &&
              static_cast<bool>(runtime.resume(allocation->surface)));
  OMARCHY_CHECK(!input(pointer_release) && !input(key_release) && !input(touch_end));
  const auto touch_cancels_before_no_touch_focus_loss =
      probe.touch_cancel_count;
  OMARCHY_CHECK(accepted(surface::FocusChanged{.focused = true}) &&
              !input(key_release) &&
              accepted(surface::FocusChanged{.focused = false}) &&
              probe.touch_cancel_count ==
                  touch_cancels_before_no_touch_focus_loss &&
              static_cast<bool>(runtime.release(allocation->surface)) &&
              !runtime.focused());
  QCoreApplication::instance()->removeEventFilter(&probe);
}

void touch_injector_maps_native_global_coordinates() {
  QQuickRenderControl render_control;
  QQuickWindow window(&render_control);
  window.setGeometry(37, 53, 128, 64);
  window.contentItem()->setSize(QSizeF(128, 64));
  QQuickItem receiver(window.contentItem());
  receiver.setSize(QSizeF(128, 64));
  receiver.setAcceptTouchEvents(true);

  InputEventProbe probe;
  QCoreApplication::instance()->installEventFilter(&probe);
  worker::QtTouchInjector injector;
  injector.deliver(
      window,
      {.phase = surface::TouchFramePhase::begin,
       .points = {{
           {.id = 0,
            .state = surface::TouchPointState::pressed,
            .position = {.x_q16 = 8U << 16, .y_q16 = 6U << 16}},
       }},
       .count = 1},
      2.0);
  OMARCHY_CHECK(probe.type == QEvent::TouchBegin &&
              probe.touch_points.size() == 1 &&
              probe.touch_points[0].position() == QPointF(16, 12) &&
              probe.touch_points[0].globalPosition() == QPointF(53, 65));
  const auto geometry = window.screen()->geometry();
  const auto normalized = probe.touch_points[0].normalizedPosition();
  const auto platform_scale = window.devicePixelRatio();
  const QPointF native_global(
      geometry.x() + (53.0 - geometry.x()) * platform_scale,
      geometry.y() + (65.0 - geometry.y()) * platform_scale);
  QRect native_virtual_geometry;
  for (const QScreen *screen : QGuiApplication::screens()) {
    const auto screen_geometry = screen->geometry();
    const auto scale = screen->devicePixelRatio();
    native_virtual_geometry |=
        QRect(screen_geometry.topLeft(),
              QSize(qRound(screen_geometry.width() * scale),
                    qRound(screen_geometry.height() * scale)));
  }
  OMARCHY_CHECK(normalized.x() > 0.0 && normalized.x() < 1.0 &&
              normalized.y() > 0.0 && normalized.y() < 1.0);
  if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
    const QPointF expected(
        (native_global.x() - native_virtual_geometry.x()) /
            native_virtual_geometry.width(),
        (native_global.y() - native_virtual_geometry.y()) /
            native_virtual_geometry.height());
    OMARCHY_CHECK(qAbs(normalized.x() - expected.x()) < 0.000001 &&
                qAbs(normalized.y() - expected.y()) < 0.000001);
  }
  injector.cancel(window);
  QCoreApplication::instance()->removeEventFilter(&probe);
}

void touch_input_reaches_qml_handlers() {
  worker::WorkerRuntime runtime(fixture("expressive"));
  const auto loaded = runtime.load_surface_entry("touch", "Touch.qml");
  if (!loaded)
    throw std::runtime_error("touch QML did not load: " + loaded.detail);
  OMARCHY_CHECK(static_cast<bool>(runtime.select_software_profile(
              surface::software_profile_offer())));
  const auto allocation = frame_allocation(
      {.id = 47, .generation = 13}, 64, 32, 128, 64, 2, 1);
  WorkerMapping mapping(runtime, *allocation);
  auto consumer = surface::FrameConsumer::create(*allocation);
  OMARCHY_CHECK(consumer.has_value());

  const auto render_color = [&](int x, int y) {
    runtime.request_render();
    const auto frame = runtime.render();
    OMARCHY_CHECK(frame.has_value() &&
                consumer->consume(mapping.bytes(), *frame) ==
                    surface::ConsumeResult::accepted);
    return frame_image(*allocation, *consumer).pixelColor(x, y);
  };

  InputEventProbe probe;
  QCoreApplication::instance()->installEventFilter(&probe);
  std::uint64_t sequence = 1;
  const auto accepted = [&](surface::InputPayload payload) {
    const bool result = deliver_input(runtime, allocation->surface, sequence, std::move(payload));
    ++sequence;
    return result;
  };
  const auto focus = [&] {
    OMARCHY_CHECK(accepted(surface::FocusChanged{.focused = true}));
  };
  const auto two_point_begin = [] {
    return surface::TouchFrame{
        .phase = surface::TouchFramePhase::begin,
        .points = {{
            {.id = 1,
             .state = surface::TouchPointState::pressed,
             .position = {.x_q16 = 8U << 16, .y_q16 = 8U << 16}},
            {.id = 2,
             .state = surface::TouchPointState::pressed,
             .position = {.x_q16 = 20U << 16, .y_q16 = 12U << 16}},
        }},
        .count = 2};
  };

  focus();
  auto begin = two_point_begin();
  const bool begin_accepted = accepted(begin);
  const auto left_begin_color = render_color(16, 16);
  const auto right_begin_color = render_color(96, 32);
  if (!(begin_accepted && probe.type == QEvent::TouchBegin &&
              probe.touch_points.size() == 2 &&
              probe.touch_points[0].position() == QPointF(8, 8) &&
              probe.touch_points[1].position() == QPointF(20, 12) &&
              probe.touch_points[0].pressure() == 1.0 &&
              left_begin_color == QColor("#ff0000") &&
              right_begin_color == QColor("#111111"))) {
    throw std::runtime_error(
        "DPR-scaled two-point begin mismatch: accepted=" +
        std::to_string(begin_accepted) + " type=" +
        std::to_string(probe.type) + " count=" +
        std::to_string(probe.touch_points.size()) + " p0=" +
        std::to_string(probe.touch_points.empty()
                           ? -1.0
                           : probe.touch_points[0].position().x()) +
        "," +
        std::to_string(probe.touch_points.empty()
                           ? -1.0
                           : probe.touch_points[0].position().y()) +
        " colors=" + left_begin_color.name().toStdString() + "," +
        right_begin_color.name().toStdString());
  }
  auto update = begin;
  update.phase = surface::TouchFramePhase::update;
  update.points[0].state = surface::TouchPointState::updated;
  update.points[0].position = {.x_q16 = 10U << 16, .y_q16 = 9U << 16};
  update.points[1].state = surface::TouchPointState::stationary;
  auto partial_update = update;
  partial_update.count = 1;
  OMARCHY_CHECK(!accepted(partial_update) &&
              render_color(16, 16) == QColor("#ff0000"));
  OMARCHY_CHECK(accepted(update) && probe.type == QEvent::TouchUpdate &&
              probe.touch_points.size() == 2 &&
              probe.touch_points[0].state() == QEventPoint::State::Updated &&
              probe.touch_points[1].state() ==
                  QEventPoint::State::Stationary &&
              render_color(16, 16) == QColor("#00ff00"));
  auto end = update;
  end.phase = surface::TouchFramePhase::end;
  end.points[0].state = surface::TouchPointState::released;
  end.points[1].state = surface::TouchPointState::released;
  OMARCHY_CHECK(accepted(end) && probe.type == QEvent::TouchEnd &&
              probe.touch_points.size() == 2 &&
              probe.touch_points[0].pressure() == 0.0 &&
              render_color(16, 16) == QColor("#0000ff"));

  surface::TouchFrame handler_begin{
      .phase = surface::TouchFramePhase::begin,
      .points = {{
          {.id = 3,
           .state = surface::TouchPointState::pressed,
           .position = {.x_q16 = 48U << 16, .y_q16 = 16U << 16}},
      }},
      .count = 1};
  OMARCHY_CHECK(accepted(handler_begin) &&
              render_color(96, 32) == QColor("#ff00ff"));
  auto handler_end = handler_begin;
  handler_end.phase = surface::TouchFramePhase::end;
  handler_end.points[0].state = surface::TouchPointState::released;
  OMARCHY_CHECK(accepted(handler_end) &&
              render_color(96, 32) == QColor("#00ffff"));

  begin = two_point_begin();
  OMARCHY_CHECK(accepted(begin));
  auto cancels = probe.touch_cancel_count;
  OMARCHY_CHECK(accepted(surface::TouchFrame{
              .phase = surface::TouchFramePhase::cancel}) &&
              probe.touch_cancel_count > cancels &&
              render_color(16, 16) == QColor("#ffff00") && runtime.focused());
  OMARCHY_CHECK(accepted(surface::FocusChanged{.focused = false}));

  focus();
  begin = two_point_begin();
  OMARCHY_CHECK(accepted(begin));
  cancels = probe.touch_cancel_count;
  const bool cancel_accepted = accepted(surface::Cancel{});
  const auto cancel_color = render_color(16, 16);
  if (!(cancel_accepted && probe.touch_cancel_count > cancels &&
        cancel_color == QColor("#ffff00")))
    throw std::runtime_error(
        "cancel QML mismatch: accepted=" +
        std::to_string(cancel_accepted) + " count=" +
        std::to_string(probe.touch_cancel_count - cancels) + " color=" +
        cancel_color.name().toStdString());

  focus();
  OMARCHY_CHECK(accepted(two_point_begin()));
  cancels = probe.touch_cancel_count;
  OMARCHY_CHECK(accepted(surface::FocusChanged{.focused = false}) &&
              probe.touch_cancel_count > cancels &&
              render_color(16, 16) == QColor("#ffff00"));

  focus();
  OMARCHY_CHECK(accepted(two_point_begin()));
  cancels = probe.touch_cancel_count;
  OMARCHY_CHECK(static_cast<bool>(runtime.suspend(allocation->surface)) &&
              probe.touch_cancel_count > cancels &&
              static_cast<bool>(runtime.resume(allocation->surface)) &&
              render_color(16, 16) == QColor("#ffff00"));

  focus();
  OMARCHY_CHECK(accepted(two_point_begin()));
  cancels = probe.touch_cancel_count;
  OMARCHY_CHECK(static_cast<bool>(runtime.release(allocation->surface)) &&
              probe.touch_cancel_count > cancels &&
              !runtime.allocated());
  QCoreApplication::instance()->removeEventFilter(&probe);
}

enum class SiblingInteraction { focused_text, pointer_grab };

void sibling_render_preserves_interaction(SiblingInteraction interaction) {
  worker::WorkerRuntime runtime(fixture("expressive"));
  OMARCHY_CHECK(static_cast<bool>(runtime.load_surface_entry(
              "target", "InteractionState.qml")) &&
              static_cast<bool>(runtime.load_surface_entry(
                  "sibling", "InteractionState.qml")) &&
              static_cast<bool>(runtime.select_software_profile(
                  surface::software_profile_offer())));
  const auto target = frame_allocation(
      {.id = 81, .generation = 15}, 64, 32, 64, 32, 1, 1);
  const auto sibling = frame_allocation(
      {.id = 82, .generation = 15}, 64, 32, 64, 32, 1, 1);
  OMARCHY_CHECK(static_cast<bool>(runtime.bind_surface("target",
                                                     target->surface)) &&
              static_cast<bool>(runtime.bind_surface("sibling",
                                                     sibling->surface)));
  auto target_fd = frame_descriptor(*target, "worker-interaction-target");
  auto sibling_fd = frame_descriptor(*sibling, "worker-interaction-sibling");
  OMARCHY_CHECK(static_cast<bool>(runtime.allocate(*target, target_fd.release())) &&
              static_cast<bool>(runtime.allocate(*sibling, sibling_fd.release())));

  std::uint64_t sequence = 1;
  const auto accepted = [&](surface::InputPayload payload) {
    return deliver_input(runtime, target->surface, sequence++, std::move(payload));
  };
  const auto render_sibling = [&] {
    runtime.request_render();
    for (int attempt = 0; attempt < 4; ++attempt) {
      const auto frame = runtime.render();
      OMARCHY_CHECK(frame.has_value());
      if (frame->surface == sibling->surface)
        return;
    }
    throw std::runtime_error("interleaved sibling surface was not rendered");
  };

  OMARCHY_CHECK(accepted(surface::FocusChanged{.focused = true}));
  if (interaction == SiblingInteraction::focused_text) {
    OMARCHY_CHECK(runtime.root_object_name() ==
                "focused=true;keys=0;text=;drag=0");
    render_sibling();
    OMARCHY_CHECK(runtime.root_object_name() ==
                "focused=true;keys=0;text=;drag=0");
    const surface::Key key_press{
        .key = static_cast<std::uint32_t>(Qt::Key_A),
        .native_scan_code = 30,
        .state = surface::ButtonState::pressed,
        .text = "a"};
    auto key_release = key_press;
    key_release.state = surface::ButtonState::released;
    key_release.text.clear();
    OMARCHY_CHECK(accepted(key_press) && accepted(key_release) &&
                accepted(surface::TextCommit{.text = "é"}));
    OMARCHY_CHECK(runtime.root_object_name() ==
                "focused=true;keys=1;text=é;drag=0");
    return;
  }

  const surface::PointerButton press{
      .position = {.x_q16 = 10U << 16, .y_q16 = 24U << 16},
      .button = static_cast<std::uint32_t>(Qt::LeftButton),
      .state = surface::ButtonState::pressed,
      .buttons = static_cast<std::uint32_t>(Qt::LeftButton)};
  OMARCHY_CHECK(accepted(press) &&
              runtime.root_object_name() ==
                  "focused=true;keys=0;text=;drag=1");
  render_sibling();
  const surface::PointerMotion motion{
      .position = {.x_q16 = 20U << 16, .y_q16 = 24U << 16},
      .buttons = static_cast<std::uint32_t>(Qt::LeftButton)};
  auto release = press;
  release.position.x_q16 = 20U << 16;
  release.state = surface::ButtonState::released;
  release.buttons = 0;
  OMARCHY_CHECK(accepted(motion) && accepted(release));
  OMARCHY_CHECK(runtime.root_object_name() ==
              "focused=true;keys=0;text=;drag=3");
}

void mouse_grab_cleanup_is_surface_scoped() {
  worker::WorkerRuntime runtime(fixture("expressive"));
  OMARCHY_CHECK(static_cast<bool>(
              runtime.load_surface_entry("first", "MouseGrab.qml")) &&
              static_cast<bool>(
                  runtime.load_surface_entry("second", "MouseGrab.qml")) &&
              static_cast<bool>(runtime.select_software_profile(
                  surface::software_profile_offer())));
  const auto first = frame_allocation(
      {.id = 45, .generation = 12}, 64, 32, 64, 32, 1, 1);
  const auto second = frame_allocation(
      {.id = 46, .generation = 12}, 64, 32, 64, 32, 1, 1);
  OMARCHY_CHECK(static_cast<bool>(runtime.bind_surface("first", first->surface)) &&
              static_cast<bool>(
                  runtime.bind_surface("second", second->surface)));
  WorkerMapping first_mapping(runtime, *first);
  WorkerMapping second_mapping(runtime, *second);
  auto first_consumer = surface::FrameConsumer::create(*first);
  auto second_consumer = surface::FrameConsumer::create(*second);
  OMARCHY_CHECK(first_consumer && second_consumer);

  const auto render_pixels = [&](surface::SurfaceKey key,
                                 surface::FrameConsumer &consumer,
                                 const Mapping &mapping) {
    runtime.request_render();
    for (int attempt = 0; attempt < 4; ++attempt) {
      const auto frame = runtime.render();
      OMARCHY_CHECK(frame.has_value());
      if (frame->surface != key)
        continue;
      OMARCHY_CHECK(consumer.consume(mapping.bytes(), *frame) ==
                  surface::ConsumeResult::accepted);
      return std::vector<std::byte>(consumer.last_frame()->pixels);
    }
    throw std::runtime_error("mouse-grab surface was not scheduled");
  };
  const auto first_baseline =
      render_pixels(first->surface, *first_consumer, first_mapping);
  auto second_pixels =
      render_pixels(second->surface, *second_consumer, second_mapping);

  InputEventProbe probe;
  QCoreApplication::instance()->installEventFilter(&probe);
  std::uint64_t sequence = 1;
  const auto accepted = [&](surface::SurfaceKey key,
                            surface::InputPayload payload) {
    const bool result = deliver_input(runtime, key, sequence, std::move(payload));
    ++sequence;
    return result;
  };
  const surface::PointerButton press{
      .position = {.x_q16 = 10U << 16, .y_q16 = 10U << 16},
      .button = static_cast<std::uint32_t>(Qt::LeftButton),
      .state = surface::ButtonState::pressed,
      .buttons = static_cast<std::uint32_t>(Qt::LeftButton)};
  auto release = press;
  release.state = surface::ButtonState::released;
  release.buttons = 0;
  const surface::PointerMotion motion{
      .position = {.x_q16 = 11U << 16, .y_q16 = 11U << 16},
      .buttons = static_cast<std::uint32_t>(Qt::LeftButton)};
  const auto exercise_second = [&] {
    OMARCHY_CHECK(accepted(second->surface,
                     surface::FocusChanged{.focused = true}) &&
                accepted(second->surface, press) &&
                accepted(second->surface, motion) &&
                accepted(second->surface, release) &&
                accepted(second->surface,
                         surface::FocusChanged{.focused = false}));
    const auto changed =
        render_pixels(second->surface, *second_consumer, second_mapping);
    OMARCHY_CHECK(changed != second_pixels);
    second_pixels = changed;
  };

  OMARCHY_CHECK(accepted(first->surface,
                   surface::FocusChanged{.focused = true}) &&
              accepted(first->surface, press));
  OMARCHY_CHECK(render_pixels(first->surface, *first_consumer, first_mapping) !=
              first_baseline);
  const auto prior_touch_cancels = probe.touch_cancel_count;
  OMARCHY_CHECK(accepted(first->surface,
                   surface::FocusChanged{.focused = false}) &&
              probe.touch_cancel_count == prior_touch_cancels);
  const auto after_focus_loss =
      render_pixels(first->surface, *first_consumer, first_mapping);
  exercise_second();
  const auto first_after_second =
      render_pixels(first->surface, *first_consumer, first_mapping);
  if (first_after_second != after_focus_loss) {
    throw std::runtime_error(
        "old child changed after focus loss: before=" +
        std::to_string(std::to_integer<unsigned char>(after_focus_loss[0])) +
        "," +
        std::to_string(std::to_integer<unsigned char>(after_focus_loss[1])) +
        "," +
        std::to_string(std::to_integer<unsigned char>(after_focus_loss[2])) +
        " after=" +
        std::to_string(std::to_integer<unsigned char>(first_after_second[0])) +
        "," +
        std::to_string(std::to_integer<unsigned char>(first_after_second[1])) +
        "," +
        std::to_string(std::to_integer<unsigned char>(first_after_second[2])));
  }

  OMARCHY_CHECK(accepted(first->surface,
                   surface::FocusChanged{.focused = true}) &&
              accepted(first->surface, press));
  OMARCHY_CHECK(accepted(first->surface, surface::Cancel{}) &&
              probe.touch_cancel_count == prior_touch_cancels);
  const auto after_cancel =
      render_pixels(first->surface, *first_consumer, first_mapping);
  exercise_second();
  OMARCHY_CHECK(render_pixels(first->surface, *first_consumer, first_mapping) ==
              after_cancel);

  OMARCHY_CHECK(accepted(first->surface,
                   surface::FocusChanged{.focused = true}) &&
              accepted(first->surface, press));
  OMARCHY_CHECK(static_cast<bool>(runtime.suspend(first->surface)) &&
              probe.touch_cancel_count == prior_touch_cancels &&
              static_cast<bool>(runtime.resume(first->surface)));
  const auto after_suspend =
      render_pixels(first->surface, *first_consumer, first_mapping);
  OMARCHY_CHECK(static_cast<bool>(runtime.suspend(first->surface)));
  exercise_second();
  OMARCHY_CHECK(static_cast<bool>(runtime.resume(first->surface)) &&
              render_pixels(first->surface, *first_consumer, first_mapping) ==
                  after_suspend);

  OMARCHY_CHECK(accepted(first->surface,
                   surface::FocusChanged{.focused = true}) &&
              accepted(first->surface, press));
  OMARCHY_CHECK(static_cast<bool>(runtime.release(first->surface)) &&
              probe.touch_cancel_count == prior_touch_cancels);
  exercise_second();
  OMARCHY_CHECK(static_cast<bool>(runtime.release(second->surface)));
  QCoreApplication::instance()->removeEventFilter(&probe);
}

void two_surface_activation() {
  worker::WorkerRuntime runtime(fixture("multi-surface"));
  const auto loaded_bar = runtime.load_surface_entry("bar", "Bar.qml");
  if (!loaded_bar)
    throw std::runtime_error("bar QML did not load: " + loaded_bar.detail);
  const auto loaded_atlas =
      runtime.load_surface_entry("atlas", "Atlas.qml");
  if (!loaded_atlas)
    throw std::runtime_error("atlas QML did not load: " +
                             loaded_atlas.detail);
  OMARCHY_CHECK(runtime.object_count() >= 2);
  OMARCHY_CHECK(static_cast<bool>(runtime.select_software_profile(
              surface::software_profile_offer())));
  const auto bar = frame_allocation(
      {.id = 51, .generation = 12}, 72, 48, 72, 48, 1, 1);
  const auto atlas = frame_allocation(
      {.id = 52, .generation = 12}, 320, 200, 320, 200, 1, 1);
  OMARCHY_CHECK(static_cast<bool>(runtime.bind_surface("bar", bar->surface)) &&
              static_cast<bool>(
                  runtime.bind_surface("atlas", atlas->surface)));
  OMARCHY_CHECK(runtime.surface_key("bar") == bar->surface &&
              runtime.surface_key("atlas") == atlas->surface &&
              !runtime.surface_key("missing"));

  WorkerMapping bar_mapping(runtime, *bar);
  WorkerMapping atlas_mapping(runtime, *atlas);

  const auto first = runtime.render();
  const auto second = runtime.render();
  OMARCHY_CHECK(first && second && first->surface == bar->surface &&
              second->surface == atlas->surface);
  auto bar_consumer = surface::FrameConsumer::create(*bar);
  auto atlas_consumer = surface::FrameConsumer::create(*atlas);
  OMARCHY_CHECK(bar_consumer && atlas_consumer &&
              bar_consumer->consume(bar_mapping.bytes(), *first) ==
                  surface::ConsumeResult::accepted &&
              atlas_consumer->consume(atlas_mapping.bytes(), *second) ==
                  surface::ConsumeResult::accepted &&
              bar_consumer->last_frame()->pixels !=
                  atlas_consumer->last_frame()->pixels);
  const auto bar_before_intent = bar_consumer->last_frame()->pixels;
  const auto atlas_before_intent = atlas_consumer->last_frame()->pixels;
  OMARCHY_CHECK(!runtime.can_deliver_surface_intent("bar") &&
              runtime.can_deliver_surface_intent("atlas") &&
              !runtime.can_deliver_surface_intent("missing") &&
              !runtime.deliver_surface_intent("bar", {}) &&
              runtime.deliver_surface_intent(
                  "atlas", {{QStringLiteral("color"),
                              QStringLiteral("#24733f")}}));
  const auto intent_frame = runtime.render();
  OMARCHY_CHECK(intent_frame && intent_frame->surface == atlas->surface &&
              atlas_consumer->consume(atlas_mapping.bytes(),
                                      *intent_frame) ==
                  surface::ConsumeResult::accepted &&
              atlas_consumer->last_frame()->pixels != atlas_before_intent &&
              bar_consumer->last_frame()->pixels == bar_before_intent);

  OMARCHY_CHECK(deliver_input(runtime, bar->surface, 1,
                             surface::FocusChanged{.focused = true}) &&
              !deliver_input(runtime, atlas->surface, 2,
                              surface::FocusChanged{.focused = true}) &&
              deliver_input(runtime, bar->surface, 2,
                             surface::FocusChanged{.focused = false}) &&
              deliver_input(runtime, atlas->surface, 3,
                             surface::FocusChanged{.focused = true}));
  OMARCHY_CHECK(static_cast<bool>(runtime.release(bar->surface)) &&
              runtime.active() &&
              !static_cast<bool>(runtime.resume(bar->surface)) &&
              runtime.surface_key("bar") == bar->surface);
  runtime.request_render();
  const auto survivor = runtime.render();
  OMARCHY_CHECK(survivor && survivor->surface == atlas->surface);

  const auto stale_bar = frame_allocation(
      {.id = bar->surface.id, .generation = bar->surface.generation + 1}, 72, 48,
      72, 48, 1, 1);
  const int stale_fd = frame_descriptor(*stale_bar, "worker-stale-reattach").release();
  OMARCHY_CHECK(!static_cast<bool>(runtime.allocate(*stale_bar, stale_fd)));
  errno = 0;
  OMARCHY_CHECK(fcntl(stale_fd, F_GETFD) == -1 && errno == EBADF);

  WorkerMapping replacement_mapping(runtime, *bar);
  const auto replacement = runtime.render();
  auto replacement_consumer = surface::FrameConsumer::create(*bar);
  OMARCHY_CHECK(replacement && replacement->surface == bar->surface &&
              replacement_consumer &&
              replacement_consumer->consume(replacement_mapping.bytes(),
                                            *replacement) ==
                  surface::ConsumeResult::accepted);
  OMARCHY_CHECK(static_cast<bool>(runtime.release(atlas->surface)) &&
              static_cast<bool>(runtime.release(bar->surface)) &&
              !runtime.allocated() &&
              runtime.surface_key("bar") == bar->surface &&
              runtime.surface_key("atlas") == atlas->surface);
}

void device_pixel_ratio_scales_scene_pixels() {
  worker::WorkerRuntime runtime(fixture("expressive"));
  OMARCHY_CHECK(static_cast<bool>(runtime.load_surface_entry("barWidget", "Main.qml")) &&
              static_cast<bool>(runtime.select_software_profile(
                  surface::software_profile_offer())));
  const auto allocation = frame_allocation(
      {.id = 42, .generation = 10}, 64, 32, 128, 64, 2, 1);
  WorkerMapping mapping(runtime, *allocation);
  const auto published = runtime.render();
  auto consumer = surface::FrameConsumer::create(*allocation);
  OMARCHY_CHECK(published.has_value() && consumer.has_value() &&
              consumer->consume(mapping.bytes(), *published) ==
                  surface::ConsumeResult::accepted);
  const auto *frame = consumer->last_frame();
  constexpr std::size_t sample_x = 100;
  constexpr std::size_t sample_y = 50;
  const auto alpha_offset = sample_y * allocation->stride + sample_x * 4 + 3;
  OMARCHY_CHECK(frame != nullptr && alpha_offset < frame->pixels.size() &&
              frame->pixels[alpha_offset] != std::byte{0});
}

void asynchronous_scene_change_publishes_distinct_frame() {
  worker::WorkerRuntime runtime(fixture("async-change"));
  OMARCHY_CHECK(static_cast<bool>(runtime.load_surface_entry("proof", "Main.qml")) &&
              static_cast<bool>(runtime.select_software_profile(
                  surface::software_profile_offer())));
  const auto allocation = frame_allocation(
      {.id = 43, .generation = 11}, 64, 32, 64, 32, 1, 1);
  WorkerMapping mapping(runtime, *allocation);
  auto consumer = surface::FrameConsumer::create(*allocation);
  const auto first = runtime.render();
  OMARCHY_CHECK(first.has_value() && consumer.has_value() &&
              consumer->consume(mapping.bytes(), *first) ==
                  surface::ConsumeResult::accepted);
  const QImage first_copy = frame_image(*allocation, *consumer).copy();
  OMARCHY_CHECK(!runtime.render().has_value());
  QEventLoop loop;
  QTimer::singleShot(60, &loop, &QEventLoop::quit);
  loop.exec();
  OMARCHY_CHECK(runtime.render_requested());
  const auto second = runtime.render();
  OMARCHY_CHECK(second.has_value() && second->frame_sequence == 2 &&
              consumer->consume(mapping.bytes(), *second) ==
                  surface::ConsumeResult::accepted);
  const QImage second_image = frame_image(*allocation, *consumer);
  OMARCHY_CHECK(second_image != first_copy);
}

void hostile_loading() {
  OMARCHY_CHECK(!worker::safe_relative_qml_path("../Main.qml") &&
              !worker::safe_relative_qml_path("/plugin/Main.qml") &&
              !worker::safe_relative_qml_path("Main.js") &&
              worker::safe_relative_qml_path("ui/Main.qml"));
  worker::WorkerRuntime window(fixture("window"));
  const auto window_result = window.load_entry("Window.qml");
  OMARCHY_CHECK(!window_result &&
              window_result.failure == worker::RuntimeFailure::root_not_item);

  ExactQmlTree exact_qml;
  worker::WorkerRuntime remote(fixture("remote"), exact_qml.root());
  OMARCHY_CHECK(static_cast<bool>(remote.prepare_trusted_qt_types()) &&
              !static_cast<bool>(remote.load_entry("Remote.qml")));

  worker::WorkerRuntime unknown(fixture("unknown-module"), exact_qml.root());
  OMARCHY_CHECK(static_cast<bool>(unknown.prepare_trusted_qt_types()) &&
              !static_cast<bool>(unknown.load_surface_entry("main", "Main.qml")));

  worker::WorkerRuntime controls_shadow(fixture("controls-shadow"),
                                         exact_qml.root());
  OMARCHY_CHECK(static_cast<bool>(controls_shadow.prepare_trusted_qt_types()) &&
              static_cast<bool>(controls_shadow.load_surface_entry("main", "Main.qml")) &&
              controls_shadow.root_object_name() == "trusted");

  worker::WorkerRuntime host_shell(fixture("host-shell-module"),
                                   exact_qml.root());
  OMARCHY_CHECK(static_cast<bool>(host_shell.prepare_trusted_qt_types()) &&
              !static_cast<bool>(host_shell.load_surface_entry("main", "Main.qml")));

  TemporaryDirectory native_surface_directory;
  const auto &native_surface_root = native_surface_directory.path();
  const auto windows_before = QGuiApplication::topLevelWindows().size();
  const auto reject_native_surface = [&](std::string_view source) {
    std::ofstream(native_surface_root / "Main.qml") << source;
    auto runtime = std::make_unique<worker::WorkerRuntime>(
        native_surface_root, exact_qml.root());
    OMARCHY_CHECK(static_cast<bool>(runtime->prepare_trusted_qt_types()) &&
                !static_cast<bool>(runtime->load_surface_entry("main", "Main.qml")) &&
                QGuiApplication::topLevelWindows().size() == windows_before);
    return runtime;
  };
  const auto quickshell_window = reject_native_surface(
      "import QtQuick\nimport Quickshell\nPanelWindow {}\n");
  const auto wayland = reject_native_surface(
      "import QtQuick\nimport Quickshell.Wayland\nItem {}\n");
  const auto qt_window = reject_native_surface(
      "import QtQuick\nimport QtQuick.Window\nItem { Window { visible: true } }\n");
  std::filesystem::remove_all(native_surface_root);

  worker::WorkerRuntime presentation(fixture("presentation"),
                                     exact_qml.root());
  OMARCHY_CHECK(static_cast<bool>(presentation.prepare_trusted_qt_types()) &&
              !presentation.apply_presentation({{QStringLiteral("hostPath"),
                                                QStringLiteral("/etc/passwd")}}) &&
              presentation.apply_presentation({}) &&
              !presentation.apply_presentation({}) &&
              static_cast<bool>(presentation.load_surface_entry("main", "Main.qml")) &&
              presentation.root_object_name() == "presentation-loaded");

  worker::WorkerRuntime partial_presentation(fixture("presentation"),
                                            exact_qml.root());
  OMARCHY_CHECK(static_cast<bool>(partial_presentation.prepare_trusted_qt_types()) &&
              partial_presentation.apply_presentation(
                  {{QStringLiteral("barSize"), 26}}) &&
              static_cast<bool>(partial_presentation.load_surface_entry("main", "Main.qml")));

  worker::WorkerRuntime themed_presentation(fixture("presentation"),
                                            exact_qml.root());
  QVariantMap theme{
      {QStringLiteral("foreground"), QStringLiteral("#123456")},
      {QStringLiteral("background"), QStringLiteral("#654321")},
      {QStringLiteral("accent"), QStringLiteral("#234567")},
      {QStringLiteral("urgent"), QStringLiteral("#ff0000")},
      {QStringLiteral("barForeground"), QStringLiteral("#123456")},
      {QStringLiteral("barBackground"), QStringLiteral("#654321")},
      {QStringLiteral("fontFamily"), QStringLiteral("monospace")},
      {QStringLiteral("barPosition"), QStringLiteral("bottom")},
      {QStringLiteral("barSize"), 26},
      {QStringLiteral("iconSlot"), 27},
      {QStringLiteral("statusSlot"), 23}};
  OMARCHY_CHECK(static_cast<bool>(themed_presentation.prepare_trusted_qt_types()) &&
              themed_presentation.apply_presentation(theme) &&
              !themed_presentation.apply_presentation(theme) &&
              static_cast<bool>(themed_presentation.load_surface_entry("main", "Main.qml")) &&
              themed_presentation.root_object_name() == "presentation-themed");

  worker::WorkerRuntime shadowed_presentation(fixture("presentation-shadow"),
                                              exact_qml.root());
  OMARCHY_CHECK(static_cast<bool>(shadowed_presentation.prepare_trusted_qt_types()) &&
              static_cast<bool>(shadowed_presentation.load_surface_entry("main", "Main.qml")) &&
              shadowed_presentation.root_object_name() ==
                  "trusted-presentation");

  worker::WorkerRuntime local_module(fixture("local-module"));
  OMARCHY_CHECK(static_cast<bool>(local_module.prepare_trusted_qt_types()) &&
              static_cast<bool>(local_module.load_surface_entry("main", "Main.qml")));

  worker::WorkerRuntime local_native(fixture("local-native"));
  const auto local_native_result = local_native.prepare_trusted_qt_types();
  OMARCHY_CHECK(!local_native_result &&
              local_native_result.detail.find("pure QML") != std::string::npos);

  worker::WorkerRuntime local_redirect(fixture("local-redirect"));
  const auto local_redirect_result = local_redirect.prepare_trusted_qt_types();
  OMARCHY_CHECK(!local_redirect_result &&
              local_redirect_result.detail.find("pure QML") !=
                  std::string::npos);

  worker::WorkerRuntime bomb(fixture("object-bomb"));
  const auto bomb_result = bomb.load_entry("Bomb.qml");
  OMARCHY_CHECK(!bomb_result &&
              bomb_result.failure == worker::RuntimeFailure::object_limit);

  TemporaryDirectory symlink_directory;
  const auto &temporary = symlink_directory.path();
  std::filesystem::create_symlink("/etc/passwd", temporary / "escape.qml");
  worker::WorkerRuntime symlinked(temporary);
  const auto result = symlinked.load_entry("escape.qml");
  std::filesystem::remove_all(temporary);
  OMARCHY_CHECK(!result &&
              result.failure == worker::RuntimeFailure::invalid_source_root);
}

void bounded_image_decoding() {
  QByteArray encoded;
  {
    QImage source(4097, 4097, QImage::Format_RGBA8888);
    OMARCHY_CHECK(!source.isNull());
    source.fill(Qt::transparent);
    QBuffer output(&encoded);
    OMARCHY_CHECK(output.open(QIODevice::WriteOnly) && source.save(&output, "PNG"));
  }
  OMARCHY_CHECK(encoded.size() < 1024 * 1024);

  worker::WorkerRuntime runtime(fixture("expressive"));
  OMARCHY_CHECK(QImageReader::allocationLimit() == worker::kMaximumDecodedImageMiB);

  QBuffer oversized_input(&encoded);
  OMARCHY_CHECK(oversized_input.open(QIODevice::ReadOnly));
  QImageReader oversized(&oversized_input, "PNG");
  OMARCHY_CHECK(oversized.size() == QSize(4097, 4097) && oversized.read().isNull());

  QImage small_source(32, 32, QImage::Format_RGBA8888);
  small_source.fill(Qt::green);
  QByteArray small_encoded;
  QBuffer small_output(&small_encoded);
  OMARCHY_CHECK(small_output.open(QIODevice::WriteOnly) &&
              small_source.save(&small_output, "PNG"));
  QBuffer small_input(&small_encoded);
  OMARCHY_CHECK(small_input.open(QIODevice::ReadOnly));
  QImageReader small(&small_input, "PNG");
  const auto decoded = small.read();
  OMARCHY_CHECK(!decoded.isNull() && decoded.size() == QSize(32, 32));

  const QByteArray truncated = small_encoded.first(small_encoded.size() / 3);
  const QByteArray malformed("not-an-image\0\xff", 14);
  for (int attempt = 0; attempt < 32; ++attempt) {
    for (const auto &bytes : {truncated, malformed}) {
      QBuffer input;
      input.setData(bytes);
      OMARCHY_CHECK(input.open(QIODevice::ReadOnly));
      QImageReader reader(&input);
      OMARCHY_CHECK(reader.read().isNull());
    }
  }
}

void steady_state_denies_exec() {
  expect_child_exit(0, [&] {
    std::string error;
    if (!worker::install_steady_state_seccomp(error))
      _exit(10);
    char executable[] = "/bin/true";
    char *arguments[] = {executable, nullptr};
    char *environment[] = {nullptr};
    errno = 0;
    execve(executable, arguments, environment);
    _exit(errno == EPERM ? 0 : 11);
  });
}

// Each case needs a fresh process: the steady-state filter is irreversible.
void certified_module_loads(const char *fixture_name,
                           std::string_view expected_object_name,
                           const char *surface_name) {
  ExactQmlTree qml_tree;
  const pid_t child = fork();
  require(child >= 0, std::string(fixture_name) + ": seccomp test fork failed");
  if (child == 0) {
    worker::WorkerRuntime runtime(fixture(fixture_name), qml_tree.root());
    if (!runtime.prepare_trusted_qt_types())
      _exit(20);
    std::string error;
    if (!worker::install_steady_state_seccomp(error))
      _exit(21);
    const auto loaded = runtime.load_surface_entry(surface_name, "Main.qml");
    const bool accepted = loaded && runtime.loaded() &&
                          runtime.root_object_name() == expected_object_name;
    if (!accepted) {
      const auto detail = std::string(fixture_name) + " marker=" +
                          runtime.root_object_name() + " objects=" +
                          std::to_string(runtime.object_count()) + " " +
                          loaded.detail + "\n";
      static_cast<void>(write(STDERR_FILENO, detail.data(), detail.size()));
    }
    _exit(accepted ? 0 : 22);
  }
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          std::string(fixture_name) +
              ": certified module failed after steady-state seccomp, status=" +
              std::to_string(status));
}

void certified_modules_load_after_steady_state() {
  struct Case {
    const char *fixture_name;
    std::string_view expected_object_name;
    const char *surface_name = "main";
  };
  // Literal expectations stay independent of the fixture QML sources.
  for (const auto &test : {
           Case{"trusted-shapes", "", "shape"},
           Case{"trusted-layouts", "trusted-layouts"},
           Case{"trusted-effects", "trusted-effects"},
           Case{"trusted-controls", "basic-controls-1"},
           Case{"dynamic-shapes", "shapes-loaded"},
           Case{"dynamic-layouts", "layouts-loaded"},
           Case{"dynamic-effects", "effects-loaded"},
           Case{"dynamic-controls", "dynamic-loaded"},
       })
    certified_module_loads(test.fixture_name, test.expected_object_name, test.surface_name);
}

void dynamic_module_resolution_stays_certified() {
  expect_child_exit(0, [&] {
    QQmlEngine engine;
    QQmlComponent component(&engine,
                            QUrl::fromLocalFile(QString::fromStdString(
                                (fixture("dynamic-module") / "Main.qml")
                                    .string())));
    std::unique_ptr<QObject> root(component.create());
    _exit(root && root->objectName() == QStringLiteral("dialog-loaded") ? 0
                                                                         : 29);
  });

  ExactQmlTree qml_tree;
  expect_child_exit(0, [&] {
    worker::WorkerRuntime runtime(fixture("dynamic-module"), qml_tree.root());
    const auto prepared = runtime.prepare_trusted_qt_types();
    if (!prepared)
      _exit(30);
    std::string error;
    if (!worker::install_steady_state_seccomp(error))
      _exit(31);
    const auto loaded = runtime.load_surface_entry("main", "Main.qml");
    if (!loaded || !runtime.root_object_name().empty()) {
      const auto detail = std::string("dynamic count=") +
                          std::to_string(runtime.object_count()) + " " +
                          loaded.detail + "\n";
      static_cast<void>(write(STDERR_FILENO, detail.data(), detail.size()));
    }
    _exit(loaded && runtime.root_object_name().empty() ? 0 : 32);
  });
}

} // namespace

int main(int argc, char **argv) {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    QGuiApplication application(argc, argv);
    if (argc == 2 && std::string_view(argv[1]) == "--headless-only") {
      headless_entry_has_no_surface_authority();
      std::cout << "plugin worker headless boundary: ok\n";
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--sibling-focus-only") {
      sibling_render_preserves_interaction(SiblingInteraction::focused_text);
      std::cout << "plugin worker sibling focus: ok\n";
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--sibling-grab-only") {
      sibling_render_preserves_interaction(SiblingInteraction::pointer_grab);
      std::cout << "plugin worker sibling grab: ok\n";
      return 0;
    }
    render_and_input();
    headless_entry_has_no_surface_authority();
    typed_input_projects_exact_qt_events();
    touch_injector_maps_native_global_coordinates();
    touch_input_reaches_qml_handlers();
    mouse_grab_cleanup_is_surface_scoped();
    two_surface_activation();
    device_pixel_ratio_scales_scene_pixels();
    asynchronous_scene_change_publishes_distinct_frame();
    hostile_loading();
    bounded_image_decoding();
    steady_state_denies_exec();
    certified_modules_load_after_steady_state();
    dynamic_module_resolution_stays_certified();
    std::cout << "plugin worker runtime: ok\n";
    return 0;
  }, "plugin worker runtime: ");
}
