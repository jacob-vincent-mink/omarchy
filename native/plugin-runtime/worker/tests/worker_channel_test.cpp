#include "../../tests/support/child_process.hpp"
#include "../../tests/support/test_assert.hpp"
#include "omarchy/plugin_runtime/test_support/test_support.h"
#include "../../tests/support/packet_sender.hpp"

#include "worker_channel.hpp"
#include "startup_state.hpp"

#include "omarchy/plugin/wire/common.hpp"
#include "omarchy/plugin_runtime/surface/render_messages.hpp"

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using omarchy::plugin_runtime::test_support::expect_child_exit;

namespace surface = omarchy::plugin_runtime::surface;
namespace worker = omarchy::plugin_runtime::worker;
namespace wire = omarchy::plugin::wire;

using omarchy::plugin_runtime::test_support::require;

void startup_state_is_one_way() {
  worker::StartupState loaded;
  OMARCHY_CHECK(!loaded.loading() && !loaded.loaded() && !loaded.terminal());
  OMARCHY_CHECK(loaded.begin_loading() && loaded.loading() &&
              !loaded.begin_loading());
  OMARCHY_CHECK(loaded.finish_loading() && loaded.loaded() &&
              !loaded.finish_loading() && !loaded.begin_loading());
  OMARCHY_CHECK(loaded.terminate() && loaded.terminal() && !loaded.terminate());

  worker::StartupState failed_load;
  OMARCHY_CHECK(failed_load.begin_loading() && failed_load.terminate() &&
              failed_load.terminal() && !failed_load.finish_loading() &&
              !failed_load.begin_loading());
}

using omarchy::plugin_runtime::test_support::SeqpacketPair;

bool send_packet(int descriptor, const wire::EnvelopeHeader &header,
                 std::span<const std::byte> payload,
                 std::optional<int> passed_descriptor = std::nullopt) {
  std::vector<std::byte> packet(wire::kHeaderSize + payload.size());
  const auto encoded = wire::encode_packet(header, payload, packet);
  if (!encoded)
    return false;
  const auto descriptors = passed_descriptor ? std::span(&*passed_descriptor, 1) : std::span<int>{};
  return omarchy::plugin_runtime::test_support::send_datagram<1>(
      descriptor, std::span(packet).first(encoded.bytes_written), descriptors);
}

wire::EnvelopeHeader welcome_header(wire::EndpointRole role) {
  return {.endpoint_role = role,
          .message_type =
              static_cast<std::uint16_t>(wire::CommonMessageType::welcome),
          .role_protocol_version = 1,
          .payload_length = 8,
          .launch_generation = 77,
          .correlation_id = 0};
}

void handshake(worker::WorkerEndpoint &endpoint, int host) {
  if (!endpoint.valid())
    throw std::runtime_error("worker endpoint baseline failed: " +
                             endpoint.last_error());
  if (!endpoint.send_hello())
    throw std::runtime_error("worker HELLO failed: " + endpoint.last_error());
  std::array<std::byte, wire::kHeaderSize + 4> hello{};
  const auto received = recv(host, hello.data(), hello.size(), 0);
  OMARCHY_CHECK(received == static_cast<ssize_t>(hello.size()));
  const auto decoded = wire::decode_packet(
      std::span<const std::byte>(hello).first(static_cast<std::size_t>(received)),
      endpoint.role());
  OMARCHY_CHECK(decoded &&
              decoded.packet.header.message_type ==
                  static_cast<std::uint16_t>(wire::CommonMessageType::hello));
  const auto payload = wire::encode_welcome_payload(
      {.maximum_payload = wire::payload_cap(endpoint.role()),
       .maximum_in_flight = 8});
  OMARCHY_CHECK(send_packet(host, welcome_header(endpoint.role()), payload));
  const auto welcome = endpoint.receive();
  OMARCHY_CHECK(static_cast<bool>(welcome) && endpoint.accept_welcome(welcome) &&
              endpoint.selected() && endpoint.generation() == 77 &&
              endpoint.maximum_in_flight() == 8);
}

struct SelectedEndpoint {
  SeqpacketPair pair = SeqpacketPair::create();
  wire::SessionSequence sequence;
  worker::WorkerEndpoint endpoint;

  explicit SelectedEndpoint(wire::EndpointRole role, std::uint64_t first = 1)
      : sequence(first), endpoint(pair.worker.get(), role, 1, sequence) {
    handshake(endpoint, pair.trusted.get());
  }
};

constexpr std::uint64_t lane_value(wire::EndpointRole role,
                                   std::uint64_t counter) {
  return (counter << 2U) | static_cast<std::uint16_t>(role);
}

wire::EnvelopeHeader data_header(
    wire::EndpointRole role, std::uint64_t sequence,
    std::uint64_t correlation = 0,
    std::uint16_t type = static_cast<std::uint16_t>(wire::CommonMessageType::protocol_error),
    std::size_t payload_size = 0) {
  return {.endpoint_role = role,
          .message_type = type,
          .role_protocol_version = 1,
          .payload_length = static_cast<std::uint32_t>(payload_size),
          .launch_generation = 77,
          .correlation_id = correlation,
          .lane_sequence = sequence};
}

bool send_sequence_packet(int host, wire::EndpointRole role, std::uint64_t counter) {
  return send_packet(host, data_header(role, lane_value(role, counter)), {});
}

void valid_and_descriptor_paths() {
  auto [pair, sequence, endpoint] = SelectedEndpoint(wire::EndpointRole::render);
  const auto offer =
      surface::encode_profile_offer(surface::software_profile_offer());
  const auto offer_header = data_header(
      wire::EndpointRole::render, lane_value(wire::EndpointRole::render, 1), 1,
      static_cast<std::uint16_t>(surface::RenderMessageType::profile_offer),
      offer.size());
  OMARCHY_CHECK(send_packet(pair.trusted.get(), offer_header, offer));
  auto received = endpoint.receive();
  OMARCHY_CHECK(static_cast<bool>(received) &&
              received.payload.size() == offer.size() &&
              received.descriptors.empty());

  const auto page_size = sysconf(_SC_PAGESIZE);
  const auto allocation =
      surface::make_allocation({.id = 5, .generation = 77}, 16, 16, 16, 16, 1,
                               1, static_cast<std::uint64_t>(page_size));
  OMARCHY_CHECK(allocation.has_value());
  const auto allocation_payload =
      surface::encode_surface_allocation(*allocation);
  const auto allocation_header = data_header(
      wire::EndpointRole::render, lane_value(wire::EndpointRole::render, 2), 2,
      static_cast<std::uint16_t>(surface::RenderMessageType::surface_allocate),
      allocation_payload.size());
  const int memory =
      static_cast<int>(syscall(SYS_memfd_create, "channel-test", MFD_CLOEXEC));
  OMARCHY_CHECK(memory >= 0 && send_packet(pair.trusted.get(), allocation_header,
                                     allocation_payload, memory));
  auto allocated = endpoint.receive();
  OMARCHY_CHECK(static_cast<bool>(allocated) && allocated.descriptors.size() == 1);
  auto moved = std::move(allocated);
  const int displaced = dup(memory);
  OMARCHY_CHECK(displaced >= 0);
  received.descriptors.emplace_back(displaced);
  received = std::move(moved);
  errno = 0;
  OMARCHY_CHECK(fcntl(displaced, F_GETFD) < 0 && errno == EBADF);
  const int transferred = received.take_only_descriptor();
  OMARCHY_CHECK(transferred >= 0 && fcntl(transferred, F_GETFD) >= 0 &&
              received.descriptors.empty() &&
              received.take_only_descriptor() == -1 &&
              allocated.descriptors.empty() && moved.descriptors.empty());
  close(transferred);
  close(memory);
}

void injected_descriptor_cleanup() {
  auto [pair, sequence, endpoint] = SelectedEndpoint(wire::EndpointRole::render);
  const auto offer =
      surface::encode_profile_offer(surface::software_profile_offer());
  const auto header = data_header(
      wire::EndpointRole::render, lane_value(wire::EndpointRole::render, 1), 1,
      static_cast<std::uint16_t>(surface::RenderMessageType::profile_offer),
      offer.size());
  const int memory =
      static_cast<int>(syscall(SYS_memfd_create, "injected-test", MFD_CLOEXEC));
  OMARCHY_CHECK(memory >= 0 && send_packet(pair.trusted.get(), header, offer, memory));
  int quarantined = -1;
  {
    auto rejected = endpoint.receive();
    OMARCHY_CHECK(!rejected &&
                rejected.failure ==
                    worker::ChannelFailure::descriptor_mismatch &&
                rejected.descriptors.size() == 1);
    quarantined = rejected.descriptors.front().get();
  }
  errno = 0;
  OMARCHY_CHECK(fcntl(quarantined, F_GETFD) < 0 && errno == EBADF);
  close(memory);
}

void role_and_credential_rejection() {
  for (const auto failure : {worker::ChannelFailure::malformed_envelope,
                             worker::ChannelFailure::credential_mismatch}) {
    SeqpacketPair pair = SeqpacketPair::create();
    wire::SessionSequence sequence;
    worker::WorkerEndpoint endpoint(pair.worker.get(), wire::EndpointRole::render, 1,
                                    sequence);
    OMARCHY_CHECK(endpoint.send_hello());
    std::array<std::byte, wire::kHeaderSize + 4> hello{};
    OMARCHY_CHECK(recv(pair.trusted.get(), hello.data(), hello.size(), 0) ==
                static_cast<ssize_t>(hello.size()));
    const auto payload = wire::encode_welcome_payload(
        {.maximum_payload = wire::payload_cap(wire::EndpointRole::render),
         .maximum_in_flight = 8});
    if (failure == worker::ChannelFailure::malformed_envelope) {
      const auto wrong = welcome_header(wire::EndpointRole::control);
      OMARCHY_CHECK(send_packet(pair.trusted.get(), wrong, payload));
    } else {
      const auto header = welcome_header(wire::EndpointRole::render);
      expect_child_exit(0, [&] {
        _exit(send_packet(pair.trusted.get(), header, payload) ? 0 : 10);
      });
    }
    const auto rejected = endpoint.receive();
    OMARCHY_CHECK(!rejected && rejected.failure == failure);
  }
}

void oversized_datagram_rejection() {
  auto [pair, sequence, endpoint] = SelectedEndpoint(wire::EndpointRole::render);
  std::vector<std::byte> oversized(
      wire::kHeaderSize + wire::payload_cap(wire::EndpointRole::render) + 1,
      std::byte{0x5a});
  OMARCHY_CHECK(send(pair.trusted.get(), oversized.data(), oversized.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(oversized.size()));
  const auto rejected = endpoint.receive();
  OMARCHY_CHECK(!rejected && rejected.failure == worker::ChannelFailure::truncated);
}

void pending_input_probe() {
  auto [pair, sequence, endpoint] = SelectedEndpoint(wire::EndpointRole::broker);
  OMARCHY_CHECK(!endpoint.has_pending_input());
  wire::EnvelopeHeader header{
      .endpoint_role = wire::EndpointRole::broker,
      .message_type = 0x5000,
      .role_protocol_version = 1,
      .payload_length = 0,
      .launch_generation = 77,
      .correlation_id = 9,
      .lane_sequence = lane_value(wire::EndpointRole::broker, 1)};
  OMARCHY_CHECK(send_packet(pair.trusted.get(), header, {}) && endpoint.has_pending_input());
  const auto received = endpoint.receive();
  OMARCHY_CHECK(received && received.header.correlation_id == 9 &&
              !endpoint.has_pending_input());
}

wire::EnvelopeHeader receive_header(int descriptor, wire::EndpointRole role) {
  std::array<std::byte, wire::kHeaderSize> packet{};
  const auto received = recv(descriptor, packet.data(), packet.size(), 0);
  OMARCHY_CHECK(received == static_cast<ssize_t>(packet.size()));
  const auto decoded = wire::decode_packet(packet, role);
  OMARCHY_CHECK(static_cast<bool>(decoded));
  return decoded.packet.header;
}

void lane_tagged_sequence_paths() {
  SeqpacketPair control_pair = SeqpacketPair::create();
  SeqpacketPair broker_pair = SeqpacketPair::create();
  SeqpacketPair render_pair = SeqpacketPair::create();
  wire::SessionSequence sequence;
  worker::WorkerEndpoint control(control_pair.worker.get(),
                                 wire::EndpointRole::control, 1, sequence);
  worker::WorkerEndpoint broker(broker_pair.worker.get(), wire::EndpointRole::broker,
                                1, sequence);
  worker::WorkerEndpoint render(render_pair.worker.get(), wire::EndpointRole::render,
                                1, sequence);
  handshake(control, control_pair.trusted.get());
  handshake(broker, broker_pair.trusted.get());
  handshake(render, render_pair.trusted.get());

  const auto type = static_cast<std::uint16_t>(
      wire::CommonMessageType::protocol_error);
  OMARCHY_CHECK(control.send(type, {}, 1) && broker.send(type, {}, 2) &&
              render.send(type, {}, 3));
  OMARCHY_CHECK(receive_header(control_pair.trusted.get(), wire::EndpointRole::control)
                  .lane_sequence ==
              lane_value(wire::EndpointRole::control, 1) &&
              receive_header(broker_pair.trusted.get(), wire::EndpointRole::broker)
                      .lane_sequence ==
                  lane_value(wire::EndpointRole::broker, 1) &&
              receive_header(render_pair.trusted.get(), wire::EndpointRole::render)
                      .lane_sequence ==
                  lane_value(wire::EndpointRole::render, 1));

  OMARCHY_CHECK(send_sequence_packet(render_pair.trusted.get(), wire::EndpointRole::render, 2) &&
              static_cast<bool>(render.receive()) &&
              send_sequence_packet(broker_pair.trusted.get(), wire::EndpointRole::broker, 1) &&
              static_cast<bool>(broker.receive()));
  OMARCHY_CHECK(send_sequence_packet(control_pair.trusted.get(), wire::EndpointRole::control, 3) &&
              static_cast<bool>(control.receive()));
  OMARCHY_CHECK(send_sequence_packet(broker_pair.trusted.get(), wire::EndpointRole::broker, 1));
  const auto replayed = broker.receive();
  OMARCHY_CHECK(!replayed &&
              replayed.failure == worker::ChannelFailure::sequence_failed);

  auto [lower_pair, lower_sequence, lower] = SelectedEndpoint(wire::EndpointRole::render);
  OMARCHY_CHECK(send_sequence_packet(lower_pair.trusted.get(), wire::EndpointRole::render, 3) &&
              static_cast<bool>(lower.receive()) &&
              send_sequence_packet(lower_pair.trusted.get(), wire::EndpointRole::render, 2));
  const auto lowered = lower.receive();
  OMARCHY_CHECK(!lowered &&
              lowered.failure == worker::ChannelFailure::sequence_failed);

  auto [transplant_pair, transplant_sequence, transplant] =
      SelectedEndpoint(wire::EndpointRole::control);
  OMARCHY_CHECK(send_sequence_packet(transplant_pair.trusted.get(), wire::EndpointRole::broker, 1));
  const auto role_rejected = transplant.receive();
  OMARCHY_CHECK(!role_rejected && role_rejected.failure ==
                                worker::ChannelFailure::malformed_envelope);

  auto [wrong_tag_pair, wrong_tag_sequence, wrong_tag] =
      SelectedEndpoint(wire::EndpointRole::control);
  auto wrong_tag_header = data_header(
      wire::EndpointRole::broker,
      lane_value(wire::EndpointRole::broker, 1));
  std::array<std::byte, wire::kHeaderSize> wrong_tag_packet{};
  OMARCHY_CHECK(static_cast<bool>(
              wire::encode_packet(wrong_tag_header, {}, wrong_tag_packet)));
  wrong_tag_packet[9] = std::byte{1};
  OMARCHY_CHECK(send(wrong_tag_pair.trusted.get(), wrong_tag_packet.data(),
               wrong_tag_packet.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(wrong_tag_packet.size()));
  const auto tag_rejected = wrong_tag.receive();
  OMARCHY_CHECK(!tag_rejected &&
              tag_rejected.failure ==
                  worker::ChannelFailure::malformed_envelope);
}

void transport_send_failure_consumes_only_its_lane() {
  wire::SessionSequence sequence;
  SeqpacketPair failed_pair = SeqpacketPair::create();
  worker::WorkerEndpoint failed(failed_pair.worker.get(),
                                wire::EndpointRole::control, 1, sequence);
  handshake(failed, failed_pair.trusted.get());
  OMARCHY_CHECK(close(failed_pair.trusted.release()) == 0);
  const auto type = static_cast<std::uint16_t>(
      wire::CommonMessageType::protocol_error);
  OMARCHY_CHECK(!failed.send(type, {}, 1));

  SeqpacketPair control_pair = SeqpacketPair::create();
  worker::WorkerEndpoint control(control_pair.worker.get(),
                                 wire::EndpointRole::control, 1, sequence);
  handshake(control, control_pair.trusted.get());
  OMARCHY_CHECK(control.send(type, {}, 2) &&
              receive_header(control_pair.trusted.get(), wire::EndpointRole::control)
                      .lane_sequence ==
                  lane_value(wire::EndpointRole::control, 2));

  SeqpacketPair broker_pair = SeqpacketPair::create();
  worker::WorkerEndpoint broker(broker_pair.worker.get(), wire::EndpointRole::broker,
                                1, sequence);
  handshake(broker, broker_pair.trusted.get());
  OMARCHY_CHECK(broker.send(type, {}, 3) &&
              receive_header(broker_pair.trusted.get(), wire::EndpointRole::broker)
                      .lane_sequence ==
                  lane_value(wire::EndpointRole::broker, 1));
}

void zero_max_and_fd_cleanup() {
  {
    auto [pair, sequence, endpoint] = SelectedEndpoint(wire::EndpointRole::broker);
    auto header = data_header(wire::EndpointRole::broker,
                              lane_value(wire::EndpointRole::broker, 1));
    std::array<std::byte, wire::kHeaderSize> packet{};
    OMARCHY_CHECK(static_cast<bool>(wire::encode_packet(header, {}, packet)));
    std::fill(packet.begin() + 40, packet.end(), std::byte{0});
    OMARCHY_CHECK(send(pair.trusted.get(), packet.data(), packet.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(packet.size()));
    const auto rejected = endpoint.receive();
    OMARCHY_CHECK(!rejected &&
                rejected.failure == worker::ChannelFailure::malformed_envelope);
  }
  {
    auto [pair, maximum, endpoint] = SelectedEndpoint(
        wire::EndpointRole::render, std::numeric_limits<std::uint64_t>::max() >> 2U);
    const auto type = static_cast<std::uint16_t>(
        wire::CommonMessageType::protocol_error);
    OMARCHY_CHECK(endpoint.send(type, {}, 0) &&
                receive_header(pair.trusted.get(), wire::EndpointRole::render)
                        .lane_sequence ==
                    std::numeric_limits<std::uint64_t>::max() &&
                !endpoint.send(type, {}, 0));
  }
  {
    auto [pair, sequence, endpoint] = SelectedEndpoint(wire::EndpointRole::render);
    OMARCHY_CHECK(send_sequence_packet(pair.trusted.get(), wire::EndpointRole::render, 2) &&
                static_cast<bool>(endpoint.receive()));
    auto allocation = data_header(
        wire::EndpointRole::render,
        lane_value(wire::EndpointRole::render, 1));
    allocation.message_type = static_cast<std::uint16_t>(
        surface::RenderMessageType::surface_allocate);
    const int memory = static_cast<int>(
        syscall(SYS_memfd_create, "sequence-fd-test", MFD_CLOEXEC));
    OMARCHY_CHECK(memory >= 0 && send_packet(pair.trusted.get(), allocation, {}, memory));
    int quarantined = -1;
    {
      auto rejected = endpoint.receive();
      OMARCHY_CHECK(!rejected &&
                  rejected.failure == worker::ChannelFailure::sequence_failed &&
                  rejected.descriptors.size() == 1);
      quarantined = rejected.descriptors.front().get();
    }
    errno = 0;
    OMARCHY_CHECK(fcntl(quarantined, F_GETFD) < 0 && errno == EBADF);
    close(memory);
  }
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    startup_state_is_one_way();
    valid_and_descriptor_paths();
    injected_descriptor_cleanup();
    role_and_credential_rejection();
    oversized_datagram_rejection();
    pending_input_probe();
    lane_tagged_sequence_paths();
    transport_send_failure_consumes_only_its_lane();
    zero_max_and_fd_cleanup();
    std::cout << "plugin worker channel: ok\n";
    return 0;
  }, "plugin worker channel: ");
}
