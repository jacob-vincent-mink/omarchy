#include "../../tests/support/test_assert.hpp"

#include "render_session.hpp"

#include "remote_surface.hpp"
#include "render_input_transport.hpp"
#include "worker_runtime.hpp"

#include "omarchy/plugin_runtime/surface/render_messages.hpp"
#include "omarchy/plugin_runtime/unique_fd.hpp"

#include <QEventLoop>
#include <QGuiApplication>
#include <QPainter>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace bridge = omarchy::plugin_runtime::bridge;
namespace session = omarchy::plugin_runtime::render_session;
namespace surface = omarchy::plugin_runtime::surface;
namespace wire = omarchy::plugin::wire;
namespace worker = omarchy::plugin_runtime::worker;

using omarchy::plugin_runtime::test_support::require;

void wait_for_render_request(worker::WorkerRuntime &runtime,
                             int timeout_milliseconds = 100) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_milliseconds);
  while (!runtime.render_requested() &&
         std::chrono::steady_clock::now() < deadline) {
    QEventLoop loop;
    QTimer::singleShot(2, &loop, &QEventLoop::quit);
    loop.exec();
  }
  OMARCHY_CHECK(runtime.render_requested());
}

class NullInputSink final : public bridge::RenderPacketSink {
public:
  bool send(const wire::EnvelopeHeader &, std::span<const std::byte>) override {
    return true;
  }
};

class CapturingSender final : public session::PacketSender {
public:
  bool send(const wire::EnvelopeHeader &header,
            std::span<const std::byte> payload,
            std::span<const int> descriptors) override {
    if (fail_send || descriptors.size() > 1)
      return false;
    headers.push_back(header);
    payloads.emplace_back(payload.begin(), payload.end());
    if (!descriptors.empty()) {
      if (worker_descriptor)
        return false;
      worker_descriptor.reset(fcntl(descriptors.front(), F_DUPFD_CLOEXEC, 64));
      mutation_descriptor.reset(fcntl(descriptors.front(), F_DUPFD_CLOEXEC, 64));
      if (!worker_descriptor || !mutation_descriptor)
        return false;
    }
    return true;
  }

  int take_worker_descriptor() { return worker_descriptor.release(); }

  std::vector<wire::EnvelopeHeader> headers;
  std::vector<std::vector<std::byte>> payloads;
  omarchy::plugin_runtime::UniqueFd mutation_descriptor;
  omarchy::plugin_runtime::UniqueFd worker_descriptor;
  bool fail_send = false;
};

struct Harness {
  explicit Harness(std::uint64_t generation, std::uint32_t logical_width = 64,
                   std::uint32_t logical_height = 32, std::uint32_t dpr = 1,
                   std::string_view fixture_name = "expressive",
                   const char *entry_surface = "barWidget")
      : surface_name(entry_surface),
        runtime(std::filesystem::path(SURFACE_HOST_WORKER_FIXTURE_ROOT) /
                fixture_name),
        input_sink(std::make_shared<NullInputSink>()),
        transport(std::make_shared<bridge::AuthenticatedInputTransport>(
            generation, input_sink)),
        host(generation, item, sender) {
    OMARCHY_CHECK(item.bindTransport(transport));
    const auto page_size = sysconf(_SC_PAGESIZE);
    OMARCHY_CHECK(page_size > 0);
    allocation = surface::make_allocation(
        {.id = generation + 100, .generation = generation}, logical_width,
        logical_height, logical_width * dpr, logical_height * dpr, dpr, 1,
        static_cast<std::uint64_t>(page_size));
    OMARCHY_CHECK(allocation.has_value());
  }

  bool receive(std::uint16_t type, std::span<const std::byte> payload,
               std::uint64_t correlation = 0) {
    return host.receive({.message_type = type,
                         .correlation_id = correlation,
                         .payload = payload});
  }

  void select_profile() {
    OMARCHY_CHECK(static_cast<bool>(runtime.load_surface_entry(surface_name, "Main.qml")) &&
                host.start(*allocation) && sender.headers.size() == 1);
    surface::ProfileOffer offer{};
    OMARCHY_CHECK(surface::decode_profile_offer(sender.payloads.at(0), offer) &&
                static_cast<bool>(runtime.select_software_profile(offer)));
    const auto selected =
        surface::select_software_profile(offer.version);
    OMARCHY_CHECK(selected.has_value());
    const auto selected_payload = surface::encode_profile_selection(*selected);
    OMARCHY_CHECK(receive(static_cast<std::uint16_t>(
                        surface::RenderMessageType::profile_select),
                    selected_payload, 1) &&
                sender.headers.size() == 2 && sender.worker_descriptor);
    surface::TrustedAllocation decoded{};
    const auto page_size = sysconf(_SC_PAGESIZE);
    OMARCHY_CHECK(surface::decode_surface_allocation(
                sender.payloads.at(1), static_cast<std::uint64_t>(page_size),
                decoded) &&
                decoded == *allocation &&
                static_cast<bool>(
                    runtime.allocate(decoded, sender.take_worker_descriptor())));
  }

  void negotiate() {
    select_profile();
    const auto allocated = surface::encode_surface_key(allocation->surface);
    OMARCHY_CHECK(
        receive(static_cast<std::uint16_t>(
                    surface::RenderMessageType::surface_allocated),
                allocated, 2) &&
            host.phase() == session::Phase::active && item.connected());
  }

  surface::FrameReady publish() {
    const auto frame = runtime.render();
    OMARCHY_CHECK(frame.has_value());
    const auto payload = surface::encode_frame_ready(*frame);
    OMARCHY_CHECK(receive(static_cast<std::uint16_t>(
                        surface::RenderMessageType::frame_ready),
                    payload));
    return *frame;
  }

  const char *surface_name;
  worker::WorkerRuntime runtime;
  std::shared_ptr<NullInputSink> input_sink;
  std::shared_ptr<bridge::AuthenticatedInputTransport> transport;
  bridge::RemotePluginSurface item;
  CapturingSender sender;
  session::HostRenderSession host;
  std::optional<surface::TrustedAllocation> allocation;
};

QImage painted_surface(bridge::RemotePluginSurface &item,
                       const surface::TrustedAllocation &allocation) {
  QImage image(static_cast<int>(allocation.pixel_width),
               static_cast<int>(allocation.pixel_height),
               QImage::Format_RGBA8888_Premultiplied);
  image.setDevicePixelRatio(
      static_cast<qreal>(allocation.dpr_numerator) /
      static_cast<qreal>(allocation.dpr_denominator));
  image.fill(Qt::transparent);
  item.setSize(QSizeF(allocation.logical_width, allocation.logical_height));
  QPainter painter(&image);
  item.paint(&painter);
  return image;
}

void asynchronous_change_reaches_host_surface() {
  Harness harness(38, 64, 32, 1, "async-change", "proof");
  harness.negotiate();
  static_cast<void>(harness.publish());
  const QImage first_image = painted_surface(harness.item, *harness.allocation);
  OMARCHY_CHECK(!harness.runtime.render().has_value());

  QEventLoop loop;
  QTimer::singleShot(60, &loop, &QEventLoop::quit);
  loop.exec();
  OMARCHY_CHECK(harness.runtime.render_requested());
  const auto second = harness.publish();
  OMARCHY_CHECK(second.frame_sequence == 2 && harness.item.frameSequence() == 2 &&
              painted_surface(harness.item, *harness.allocation) !=
                  first_image &&
              harness.host.statistics().accepted_frames == 2);
}

void animated_alpha_and_throughput() {
  Harness harness(31);
  harness.negotiate();
  const auto first = harness.publish();
  OMARCHY_CHECK(harness.item.ready() && harness.item.frameSequence() == 1);
  const QImage first_image = painted_surface(harness.item, *harness.allocation);
  const auto pixel = first_image.pixelColor(0, 0);
  OMARCHY_CHECK(pixel.alpha() > 0 && pixel.alpha() < 255);
  bool changed = false;
  const auto started = std::chrono::steady_clock::now();
  for (int frame = 0; frame < 120; ++frame) {
    wait_for_render_request(harness.runtime);
    static_cast<void>(harness.publish());
    changed = changed ||
              painted_surface(harness.item, *harness.allocation) != first_image;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const auto &statistics = harness.host.statistics();
  OMARCHY_CHECK(changed && statistics.accepted_frames == 121 &&
              statistics.copied_bytes ==
                  121 * harness.allocation->frame_bytes &&
              elapsed < std::chrono::seconds(5) &&
              statistics.maximum_copy_time < std::chrono::milliseconds(50));
  std::cout
      << "render_session frames=" << statistics.accepted_frames
      << " copied_bytes=" << statistics.copied_bytes << " wall_us="
      << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()
      << " max_copy_us="
      << std::chrono::duration_cast<std::chrono::microseconds>(
             statistics.maximum_copy_time)
             .count()
      << '\n';

  const auto repeated = surface::encode_frame_ready(first);
  OMARCHY_CHECK(!harness.receive(static_cast<std::uint16_t>(
                               surface::RenderMessageType::frame_ready),
                           repeated) &&
              harness.host.phase() == session::Phase::active &&
              harness.item.ready() &&
              harness.host.statistics().rejected_frames == 1);
}

void resize_and_dpr() {
  Harness harness(32, 48, 24, 2);
  harness.negotiate();
  static_cast<void>(harness.publish());
  const auto painted = painted_surface(harness.item, *harness.allocation);
  OMARCHY_CHECK(painted.width() == 96 && painted.height() == 48 &&
              painted.devicePixelRatio() == 2.0 &&
              painted.pixelColor(95, 47).alpha() != 0 &&
              harness.item.implicitWidth() == 48 &&
              harness.item.implicitHeight() == 24);
}

void graceful_close_releases_worker_mapping() {
  Harness harness(37);
  harness.negotiate();
  static_cast<void>(harness.publish());
  harness.host.close();
  surface::SurfaceKey released{};
  OMARCHY_CHECK(harness.sender.headers.size() == 3 &&
              harness.sender.headers.back().message_type ==
                  static_cast<std::uint16_t>(
                      surface::RenderMessageType::surface_release) &&
              surface::decode_surface_key(harness.sender.payloads.back(),
                                          released) &&
              released == harness.allocation->surface &&
              static_cast<bool>(harness.runtime.release(released)) &&
              !harness.runtime.allocated() && !harness.runtime.active() &&
              !harness.item.connected() && !harness.item.ready());

  Harness failed(38);
  failed.negotiate();
  failed.sender.fail_send = true;
  failed.host.close();
  OMARCHY_CHECK(failed.host.phase() == session::Phase::failed &&
              !failed.item.connected() && !failed.item.ready());
}

void malformed_and_oversized_fail_closed() {
  {
    Harness harness(34);
    harness.negotiate();
    const auto frame = harness.runtime.render();
    OMARCHY_CHECK(frame.has_value());
    const std::byte nonzero{0x7f};
    const auto offset = static_cast<off_t>(
        frame->slot * harness.allocation->slot_extent + 88);
    OMARCHY_CHECK(pwrite(harness.sender.mutation_descriptor.get(), &nonzero, 1, offset) ==
                1);
    const auto payload = surface::encode_frame_ready(*frame);
    OMARCHY_CHECK(!harness.receive(static_cast<std::uint16_t>(
                                 surface::RenderMessageType::frame_ready),
                             payload) &&
                harness.host.phase() == session::Phase::failed &&
                !harness.item.ready());
  }
  {
    Harness harness(39);
    OMARCHY_CHECK(static_cast<bool>(harness.runtime.load_surface_entry(harness.surface_name, "Main.qml")) &&
                harness.host.start(*harness.allocation));
    std::vector<std::byte> oversized(
        wire::payload_cap(wire::EndpointRole::render) + 1, std::byte{0x7f});
    OMARCHY_CHECK(!harness.host.receive(
                {.message_type = static_cast<std::uint16_t>(
                     surface::RenderMessageType::profile_select),
                 .correlation_id = 1,
                 .payload = oversized}) &&
                harness.host.phase() == session::Phase::failed &&
                !harness.item.connected() && !harness.item.ready());
  }
  {
    Harness harness(36);
    OMARCHY_CHECK(static_cast<bool>(harness.runtime.load_surface_entry(harness.surface_name, "Main.qml")) &&
                harness.host.start(*harness.allocation));
    surface::ProfileOffer offer{};
    OMARCHY_CHECK(
        surface::decode_profile_offer(harness.sender.payloads.at(0), offer) &&
            static_cast<bool>(harness.runtime.select_software_profile(offer)));
    const auto selected =
        surface::select_software_profile(offer.version);
    OMARCHY_CHECK(selected.has_value());
    const auto selected_payload = surface::encode_profile_selection(*selected);
    harness.sender.fail_send = true;
    OMARCHY_CHECK(!harness.receive(static_cast<std::uint16_t>(
                                 surface::RenderMessageType::profile_select),
                             selected_payload, 1) &&
                harness.host.phase() == session::Phase::failed &&
                !harness.item.ready() && !harness.item.connected());
  }
}

enum class RenderFailure {
  invalid_correlation,
  invalid_schema,
  unknown_type,
  typed_error,
  stale_surface,
};

void exercise_render_failure(RenderFailure failure) {
  Harness harness(51);
  if (failure == RenderFailure::stale_surface) {
    harness.select_profile();
    auto stale = harness.allocation->surface;
    ++stale.generation;
    const auto payload = surface::encode_surface_key(stale);
    OMARCHY_CHECK(!harness.receive(static_cast<std::uint16_t>(
                                 surface::RenderMessageType::surface_allocated),
                             payload, 2));
  } else {
    OMARCHY_CHECK(static_cast<bool>(harness.runtime.load_surface_entry(harness.surface_name, "Main.qml")) &&
                harness.host.start(*harness.allocation));
    const auto selection = surface::encode_profile_selection(
        {.version = surface::kSoftwareProfileVersion,
         .pixel_format = surface::kRgba8888Premultiplied});
    if (failure == RenderFailure::invalid_correlation) {
      OMARCHY_CHECK(!harness.receive(static_cast<std::uint16_t>(
                                   surface::RenderMessageType::profile_select),
                               selection, 0));
    } else if (failure == RenderFailure::invalid_schema) {
      OMARCHY_CHECK(!harness.receive(static_cast<std::uint16_t>(
                                   surface::RenderMessageType::profile_select),
                               std::span(selection).first(7), 1));
    } else if (failure == RenderFailure::unknown_type) {
      OMARCHY_CHECK(!harness.receive(0x20ff, selection, 1));
    } else {
      const auto error = surface::encode_render_error(
          {.reason = surface::RenderErrorReason::unsupported_profile,
           .failed_message_type = static_cast<std::uint16_t>(
               surface::RenderMessageType::profile_offer),
           .surface = {}});
      OMARCHY_CHECK(!harness.receive(
                  static_cast<std::uint16_t>(wire::CommonMessageType::typed_error),
                  error, 1));
    }
  }
  OMARCHY_CHECK(harness.host.phase() == session::Phase::failed &&
              !harness.item.connected() && !harness.item.ready() &&
              !harness.host.failure_detail().empty());
}

void authenticated_ingress_failures_fail_closed() {
  for (const auto failure : {RenderFailure::invalid_correlation,
                             RenderFailure::invalid_schema,
                             RenderFailure::unknown_type,
                             RenderFailure::typed_error,
                             RenderFailure::stale_surface})
    exercise_render_failure(failure);
}

} // namespace

int main(int argc, char **argv) {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    QGuiApplication application(argc, argv);
    animated_alpha_and_throughput();
    asynchronous_change_reaches_host_surface();
    resize_and_dpr();
    graceful_close_releases_worker_mapping();
    authenticated_ingress_failures_fail_closed();
    malformed_and_oversized_fail_closed();
    std::cout << "plugin render session: ok\n";
    return 0;
  }, "plugin render session: ");
}
