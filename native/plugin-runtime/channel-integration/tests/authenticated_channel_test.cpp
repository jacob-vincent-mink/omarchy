#include "../../tests/support/test_assert.hpp"
#include "omarchy/plugin_runtime/test_support/test_support.h"

#include "omarchy/plugin_runtime/unique_fd.hpp"
#include "../../tests/support/broker_fixture.hpp"
#include "../../tests/support/temporary_directory.hpp"
#include "authenticated_channel.hpp"
#include "omarchy/plugin_runtime/broker/broker_schema.hpp"
#include "omarchy/plugin_runtime/launcher/test_supervisor.h"
#include "omarchy/plugin_runtime/surface/render_messages.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace channel = omarchy::plugin_runtime::channel;
namespace broker = omarchy::plugin_runtime::broker;
namespace permissions = omarchy::plugins::permissions;
namespace launcher = omarchy::plugin_runtime::launcher;
namespace sandbox = omarchy::plugin_runtime::sandbox;
namespace surface = omarchy::plugin_runtime::surface;
namespace wire = omarchy::plugin::wire;

namespace {
using namespace std::chrono_literals;

using omarchy::plugin_runtime::test_support::exit_assertions::fail;
using omarchy::plugin_runtime::test_support::exit_assertions::require;

launcher::Deadline deadline_after(std::chrono::milliseconds delay) {
  return std::chrono::steady_clock::now() + delay;
}

using omarchy::plugin_runtime::UniqueFd;

class Scope final : public launcher::test_support::ReadyScope {
public:
  AttachResult attach(std::string_view unit, pid_t monitor_pid,
                      pid_t worker_pid, const sandbox::SandboxPlan &plan,
                      launcher::Deadline deadline, std::string &) override {
    OMARCHY_CHECK_WITH(require, monitor_pid > 0 && worker_pid > 0 &&
                deadline > std::chrono::steady_clock::now());
    OMARCHY_CHECK_WITH(require, plan.worker_descriptors == std::vector<int>({3, 4, 5}));
    name = unit;
    attached = true;
    return {.attached = true, .cleanup_required = true};
  }
  bool terminate_scope_validated(std::string_view unit, launcher::Deadline,
                                  std::string &) noexcept override {
    if (unit == name) {
      ++terminations;
    }
    return true;
  }

  std::string name;
  bool attached = false;
  std::atomic<unsigned> terminations = 0;
};

bool eventually_removed(const std::shared_ptr<Scope> &scope) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (scope->terminations.load() == 0 &&
         std::chrono::steady_clock::now() < deadline)
    usleep(1000);
  return scope->terminations.load() == 1;
}

bool await_channel_readable(channel::AuthenticatedBrokerChannel &channel) {
  pollfd ready{.fd = channel.readiness_fd(), .events = POLLIN, .revents = 0};
  return poll(&ready, 1, 2000) == 1 && (ready.revents & POLLIN) != 0;
}

// Wait only in the test driver; production has one readiness-driven receive path.
channel::AuthenticatedReceiveResult receive_authenticated(
    channel::AuthenticatedBrokerChannel &connection, launcher::EndpointMask lanes,
    launcher::Deadline deadline) {
  for (;;) {
    auto result = connection.try_receive_authenticated(lanes);
    if (result.status != channel::AuthenticatedReceiveStatus::would_block ||
        std::chrono::steady_clock::now() >= deadline)
      return result;
    usleep(1000);
  }
}

class Authority final : public channel::GenerationAuthority {
public:
  void revoke_after_successful_checks(unsigned allowed_checks) noexcept {
    checks_before_revocation = static_cast<int>(allowed_checks);
  }

  bool
  is_current(const launcher::LaunchIdentity &identity) const noexcept override {
    const auto check = checks.fetch_add(1) + 1;
    if (check == sleep_on_check.load())
      usleep(sleep_microseconds.load());
    int remaining = checks_before_revocation.load();
    while (remaining > 0 && !checks_before_revocation.compare_exchange_weak(
                                remaining, remaining - 1)) {
    }
    const bool semantically_current = remaining != 0;
    return current && identity.generation == generation &&
           (revoke_on_check == 0 || check < revoke_on_check) &&
           semantically_current;
  }

  std::atomic<bool> current = true;
  std::atomic<std::uint64_t> generation = 47;
  mutable std::atomic<unsigned> checks = 0;
  std::atomic<unsigned> revoke_on_check = 0;
  std::atomic<unsigned> sleep_on_check = 0;
  std::atomic<unsigned> sleep_microseconds = 0;
  mutable std::atomic<int> checks_before_revocation = -1;
};

class Fixture : public omarchy::plugin_runtime::test_support::TemporaryDirectory {
public:
  explicit Fixture(std::string_view mode) {
    revision_ = root_ / "revision";
    state_ = root_ / "state";
    std::filesystem::create_directories(revision_);
    std::filesystem::create_directories(state_);
    std::ofstream(revision_ / "worker-mode") << mode << '\n';
    OMARCHY_CHECK_WITH(require, chmod((revision_ / "worker-mode").c_str(), 0444) == 0 &&
                chmod(revision_.c_str(), 0555) == 0);
    revision_fd_.reset(
        open(revision_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    state_fd_.reset(
        open(state_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    OMARCHY_CHECK_WITH(require, revision_fd_ && state_fd_);
  }

  launcher::TrustedLaunchRequest request() const {
    return {.plugin_id = "org.omarchy_d1",
            .revision_sha256 = std::string(64, 'd'),
            .generation = 47,
            .revision_directory_fd = revision_fd_.get(),
            .private_state_directory_fd = state_fd_.get()};
  }

private:
  std::filesystem::path revision_;
  std::filesystem::path state_;
  UniqueFd revision_fd_;
  UniqueFd state_fd_;
};

std::unique_ptr<launcher::Worker>
launch_transport(Fixture &fixture, std::shared_ptr<Scope> scope) {
  auto supervisor = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, CHANNEL_PEER_PATH, std::move(scope));
  auto launched = supervisor.launch(fixture.request(), deadline_after(4s));
  if (!launched)
    std::cerr << "transport launch failure="
              << static_cast<int>(launched.failure)
              << " detail=" << launched.detail << '\n';
  OMARCHY_CHECK_WITH(require, static_cast<bool>(launched));
  return std::move(launched.worker);
}

void transport_suite() {
  {
    Fixture fixture("transport-max");
    auto worker = launch_transport(fixture, std::make_shared<Scope>());
    std::vector<std::byte> maximum(
        omarchy::plugin::wire::kHeaderSize +
        omarchy::plugin::wire::payload_cap(
            omarchy::plugin::wire::EndpointRole::broker));
    OMARCHY_CHECK_WITH(require, worker->try_send(launcher::EndpointRole::broker, maximum,
                             launcher::PacketSizeLimit{maximum.size()}) ==
                launcher::SendStatus::complete);
    const auto acknowledgement =
        worker->receive_any(
            launcher::PacketSizeLimit{1},
            deadline_after(2s),
            launcher::EndpointMask::control);
    OMARCHY_CHECK_WITH(require, acknowledgement && acknowledgement.payload.size() == 1 &&
                acknowledgement.payload.front() == std::byte{0x5a});
    maximum.push_back(std::byte{});
    OMARCHY_CHECK_WITH(require, worker->try_send(launcher::EndpointRole::broker, maximum,
                             launcher::PacketSizeLimit{maximum.size()}) !=
                launcher::SendStatus::complete);
    OMARCHY_CHECK_WITH(require, worker->terminate(deadline_after(4s)));
  }

  {
    Fixture fixture("transport-saturation");
    auto worker = launch_transport(fixture, std::make_shared<Scope>());
    const auto saturate = [&worker](launcher::EndpointRole role,
                                    std::size_t size) {
      std::vector<std::byte> datagram(size);
      const auto started = std::chrono::steady_clock::now();
      bool refused = false;
      for (unsigned attempt = 0; attempt < 10'000; ++attempt) {
        if (worker->try_send(role, datagram,
                             launcher::PacketSizeLimit{datagram.size()}) !=
            launcher::SendStatus::complete) {
          refused = true;
          break;
        }
      }
      OMARCHY_CHECK_WITH(require, refused && std::chrono::steady_clock::now() - started < 1s);
    };
    saturate(launcher::EndpointRole::broker,
             omarchy::plugin::wire::kHeaderSize +
                 omarchy::plugin::wire::payload_cap(
                     omarchy::plugin::wire::EndpointRole::broker));
    saturate(launcher::EndpointRole::render,
             omarchy::plugin::wire::kHeaderSize +
                 omarchy::plugin::wire::payload_cap(
                     omarchy::plugin::wire::EndpointRole::render));
    OMARCHY_CHECK_WITH(require, worker->terminate(deadline_after(4s)));
  }
}

std::size_t descriptor_count() {
  return omarchy::plugin_runtime::test_support::open_descriptor_count();
}

bool eventually_descriptor_count_at_most(std::size_t expected) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (descriptor_count() > expected &&
         std::chrono::steady_clock::now() < deadline)
    usleep(1000);
  return descriptor_count() <= expected;
}

struct ChannelFixture {
  Fixture fixture;
  std::shared_ptr<Scope> scope = std::make_shared<Scope>();
  std::shared_ptr<Authority> authority = std::make_shared<Authority>();
  launcher::Supervisor supervisor;
  ChannelFixture(std::string_view mode, std::string bwrap)
      : fixture(mode),
        supervisor(launcher::test_support::make_supervisor(
            std::move(bwrap), CHANNEL_PEER_PATH, scope)) {}
};

struct Session : ChannelFixture {
  channel::OpenResult opened;

  void negotiate_and_request_peer_exit() {
    OMARCHY_CHECK_WITH(require, opened.channel->negotiate(deadline_after(2s)) &&
                kill(opened.channel->identity().outer_worker_pid, SIGUSR1) == 0);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (opened.channel->alive() && std::chrono::steady_clock::now() < deadline)
      usleep(1000);
  }

  Session(std::string_view mode, std::string bwrap)
      : ChannelFixture(mode, std::move(bwrap)),
        opened(channel::AuthenticatedBrokerChannel::open(
            supervisor, fixture.request(), authority, deadline_after(2s))) {
    if (!opened) {
      std::cerr << "launch failure=" << static_cast<int>(opened.launch_failure)
                << " detail=" << opened.detail << '\n';
      fail("authenticated launch failed");
    }
    const auto &identity = opened.channel->identity();
    OMARCHY_CHECK_WITH(require, identity.plugin_id == "org.omarchy_d1" &&
                identity.revision_sha256 == std::string(64, 'd') &&
                identity.generation == 47 && identity.outer_worker_pid > 0 &&
                identity.outer_uid == getuid() &&
                identity.outer_gid == getgid());
    OMARCHY_CHECK_WITH(require, scope->attached);
  }
};

bool is_notification_request(const channel::AuthenticatedReceiveResult &result) {
  return result && result.message->role == wire::EndpointRole::broker &&
         result.message->message_type ==
             broker::kDynamicInvokeMessage &&
         result.message->correlation_id == 1 &&
         result.message->payload == omarchy::plugin_runtime::test_support::notification_request() &&
         result.message->descriptors.empty();
}

bool is_render_frame(const channel::AuthenticatedReceiveResult &result) {
  return result && result.message->role == wire::EndpointRole::render &&
         result.message->message_type ==
             static_cast<std::uint16_t>(surface::RenderMessageType::frame_ready) &&
         result.message->correlation_id == 0 && result.message->payload.size() == 40 &&
         result.message->descriptors.empty();
}

void fake_suite() {
  transport_suite();
  {
    auto [fixture, scope, authority, supervisor] =
        ChannelFixture("valid", FAKE_BWRAP_PATH);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    auto opened = channel::AuthenticatedBrokerChannel::open(
        supervisor, fixture.request(), authority, deadline);
    channel::AuthenticatedReceiveResult request;
    OMARCHY_CHECK_WITH(require, opened && opened.channel->negotiate(deadline) &&
                (request = receive_authenticated(*opened.channel,
                     launcher::EndpointMask::broker, deadline_after(2s))) &&
                is_notification_request(request));
  }
  {
    Fixture fixture("valid");
    auto supervisor = launcher::test_support::make_supervisor(
        FAKE_BWRAP_PATH, CHANNEL_PEER_PATH, std::make_shared<Scope>());
    auto opened = channel::AuthenticatedBrokerChannel::open(
        supervisor, fixture.request(), std::make_shared<Authority>(),
        std::chrono::steady_clock::now());
    OMARCHY_CHECK_WITH(require, !opened && opened.failure == channel::ChannelFailure::launch_failed);
  }
  {
    Fixture fixture("valid");
    auto scope = std::make_shared<Scope>();
    auto authority = std::make_shared<Authority>();
    authority->sleep_on_check = 1;
    authority->sleep_microseconds = 300'000;
    auto supervisor = launcher::test_support::make_supervisor(
        FAKE_BWRAP_PATH, CHANNEL_PEER_PATH, scope);
    auto opened = channel::AuthenticatedBrokerChannel::open(
        supervisor, fixture.request(), authority, deadline_after(200ms));
    OMARCHY_CHECK_WITH(require, !opened &&
                opened.failure == channel::ChannelFailure::deadline_expired &&
                authority->checks.load() == 1 && eventually_removed(scope));
  }
  {
    auto [fixture, scope, authority, supervisor] =
        ChannelFixture("valid", FAKE_BWRAP_PATH);
    const auto opening_deadline = std::chrono::steady_clock::now() + 200ms;
    auto opened = channel::AuthenticatedBrokerChannel::open(
        supervisor, fixture.request(), authority, opening_deadline);
    OMARCHY_CHECK_WITH(require, static_cast<bool>(opened));
    const auto checks_after_open = authority->checks.load();
    while (std::chrono::steady_clock::now() <= opening_deadline)
      usleep(1000);
    OMARCHY_CHECK_WITH(require, !opened.channel->negotiate(deadline_after(2s)) &&
                authority->checks.load() == checks_after_open &&
                opened.channel->failure() ==
                    channel::ChannelFailure::negotiation_failed);
  }
  for (const unsigned check_offset : {1U, 4U}) {
    auto [fixture, scope, authority, supervisor] =
        ChannelFixture("valid", FAKE_BWRAP_PATH);
    auto opened = channel::AuthenticatedBrokerChannel::open(
        supervisor, fixture.request(), authority, deadline_after(2s));
    OMARCHY_CHECK_WITH(require, static_cast<bool>(opened));
    OMARCHY_CHECK_WITH(require, opened.channel->arm_readiness(launcher::EndpointMask::all,
                                          launcher::EndpointMask::none) &&
                await_channel_readable(*opened.channel));
    const auto delayed_check = authority->checks.load() + check_offset;
    authority->sleep_on_check = delayed_check;
    authority->sleep_microseconds = 20'000;
    OMARCHY_CHECK_WITH(require, !opened.channel->negotiate(
                deadline_after(check_offset == 1 ? 5ms : 10ms)) &&
                authority->checks.load() >= delayed_check &&
                !opened.channel->ready());
    if (check_offset == 1)
      OMARCHY_CHECK_WITH(require, opened.channel->failure() ==
                    channel::ChannelFailure::negotiation_failed &&
                opened.channel->detail() ==
                    "endpoint negotiation deadline elapsed before WELCOME");
  }
  {
    Session reversed("reverse-order", FAKE_BWRAP_PATH);
    channel::AuthenticatedReceiveResult request;
    OMARCHY_CHECK_WITH(require, reversed.opened.channel->negotiate(deadline_after(2s)) &&
                (request = receive_authenticated(*reversed.opened.channel,
                     launcher::EndpointMask::broker, deadline_after(2s))) &&
                is_notification_request(request));
  }
  {
    Session multi("multi-lane", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require,
        multi.opened.channel->negotiate(deadline_after(2s)) &&
            multi.opened.channel->arm_readiness(
                launcher::EndpointMask::all, launcher::EndpointMask::control));
    auto render = receive_authenticated(*multi.opened.channel,
        launcher::EndpointMask::render, deadline_after(2s));
    OMARCHY_CHECK_WITH(require, is_render_frame(render));
    auto broker_message = receive_authenticated(*multi.opened.channel,
        launcher::EndpointMask::broker, deadline_after(2s));
    OMARCHY_CHECK_WITH(require, is_notification_request(broker_message));
    auto owned = std::move(*broker_message.message);
    OMARCHY_CHECK_WITH(require, multi.opened.channel->terminate(deadline_after(2s)) &&
                owned.payload == omarchy::plugin_runtime::test_support::notification_request() && owned.correlation_id == 1);
  }
  {
    Session empty("host-saturation", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, empty.opened.channel->negotiate(deadline_after(2s)));
    const auto first = empty.opened.channel->try_receive_authenticated(
        launcher::EndpointMask::all);
    const auto second = empty.opened.channel->try_receive_authenticated(
        launcher::EndpointMask::all);
    OMARCHY_CHECK_WITH(require, first.status == channel::AuthenticatedReceiveStatus::would_block &&
                !first.message &&
                second.status ==
                    channel::AuthenticatedReceiveStatus::would_block &&
                !second.message && !empty.opened.channel->failed());
  }
  {
    Session multi("multi-lane", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, multi.opened.channel->negotiate(deadline_after(2s)) &&
                await_channel_readable(*multi.opened.channel));
    const auto broker_message = multi.opened.channel->try_receive_authenticated(
        launcher::EndpointMask::all);
    OMARCHY_CHECK_WITH(require, is_notification_request(broker_message));
    OMARCHY_CHECK_WITH(require, await_channel_readable(*multi.opened.channel));
    const auto render_message = multi.opened.channel->try_receive_authenticated(
        launcher::EndpointMask::all);
    const auto empty = multi.opened.channel->try_receive_authenticated(
        launcher::EndpointMask::all);
    OMARCHY_CHECK_WITH(require,
        is_render_frame(render_message) &&
            empty.status == channel::AuthenticatedReceiveStatus::would_block &&
            !empty.message && !multi.opened.channel->failed());
  }
  {
    Session error("render-typed-error", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, error.opened.channel->negotiate(deadline_after(2s)));
    const auto received = receive_authenticated(*error.opened.channel,
        launcher::EndpointMask::render, deadline_after(2s));
    surface::RenderTypedError decoded{};
    OMARCHY_CHECK_WITH(require, received &&
                received.message->role == wire::EndpointRole::render &&
                received.message->message_type ==
                    static_cast<std::uint16_t>(
                        wire::CommonMessageType::typed_error) &&
                received.message->descriptors.empty() &&
                surface::decode_render_error(received.message->payload,
                                             decoded) &&
                decoded.reason ==
                    surface::RenderErrorReason::invalid_allocation &&
                !error.opened.channel->failed());
  }
  for (const std::string_view mode : {"replay", "wrong-sequence-tag", "multi-lane"}) {
    Session session(mode, FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, session.opened.channel->negotiate(deadline_after(2s)) &&
                await_channel_readable(*session.opened.channel));
    channel::AuthenticatedReceiveResult first;
    if (mode == "replay") {
      first = session.opened.channel->try_receive_authenticated(
          launcher::EndpointMask::broker);
      OMARCHY_CHECK_WITH(require, first && await_channel_readable(*session.opened.channel));
    } else if (mode == "multi-lane") {
      session.authority->revoke_after_successful_checks(1);
    }
    const auto result = session.opened.channel->try_receive_authenticated(
        launcher::EndpointMask::broker);
    OMARCHY_CHECK_WITH(require, result.status == channel::AuthenticatedReceiveStatus::fatal &&
                !result.message && session.opened.channel->failed());
  }
  {
    Session exited("ready-loss", FAKE_BWRAP_PATH);
    exited.negotiate_and_request_peer_exit();
    const auto result = exited.opened.channel->try_receive_authenticated(
        launcher::EndpointMask::broker);
    OMARCHY_CHECK_WITH(require,
        (result.status == channel::AuthenticatedReceiveStatus::peer_closed ||
         result.status == channel::AuthenticatedReceiveStatus::fatal) &&
            !result.message && exited.opened.channel->failed() &&
            exited.opened.channel->failure() ==
                channel::ChannelFailure::peer_failure);
  }
  {
    Session exited("stderr-ready-loss", FAKE_BWRAP_PATH);
    exited.negotiate_and_request_peer_exit();
    std::array<int, 2> diagnostic_pipe{};
    OMARCHY_CHECK_WITH(require, pipe2(diagnostic_pipe.data(), O_CLOEXEC) == 0);
    UniqueFd diagnostic_read(diagnostic_pipe[0]);
    UniqueFd diagnostic_write(diagnostic_pipe[1]);
    UniqueFd saved_standard_error(dup(STDERR_FILENO));
    OMARCHY_CHECK_WITH(require, saved_standard_error.get() >= 0 &&
                dup2(diagnostic_write.get(), STDERR_FILENO) >= 0);
    const auto result = exited.opened.channel->try_receive_authenticated(
        launcher::EndpointMask::broker);
    OMARCHY_CHECK_WITH(require, dup2(saved_standard_error.get(), STDERR_FILENO) >= 0);
    std::array<char, 512> diagnostic{};
    const auto diagnostic_size =
        read(diagnostic_read.get(), diagnostic.data(), diagnostic.size());
    const std::string_view diagnostic_text(
        diagnostic.data(), diagnostic_size > 0
                               ? static_cast<std::size_t>(diagnostic_size)
                               : 0);
    OMARCHY_CHECK_WITH(require, (result.status ==
                 channel::AuthenticatedReceiveStatus::peer_closed ||
             result.status == channel::AuthenticatedReceiveStatus::fatal) &&
                diagnostic_text.find("untrusted-stderr-bytes=8192") !=
                    std::string_view::npos &&
                diagnostic_text.find("forged") == std::string_view::npos &&
                diagnostic_text.find("secret") == std::string_view::npos &&
                diagnostic_text.find("Service.qml") ==
                    std::string_view::npos);
  }
  {
    Session revoked("multi-lane", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, revoked.opened.channel->negotiate(deadline_after(2s)));
    revoked.authority->revoke_on_check = revoked.authority->checks.load() + 3;
    auto result = receive_authenticated(*revoked.opened.channel,
        launcher::EndpointMask::render, deadline_after(2s));
    OMARCHY_CHECK_WITH(require, result.status == channel::AuthenticatedReceiveStatus::fatal &&
                !result.message && revoked.opened.channel->failed());
  }
  {
    Session first("valid", FAKE_BWRAP_PATH);
    Session second("valid", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, first.opened.channel->negotiate(deadline_after(2s)) &&
                second.opened.channel->negotiate(deadline_after(2s)));
    auto prepared = first.opened.channel->prepare_send(
        wire::EndpointRole::broker, broker::kBrokerResultMessage, 1, {});
    OMARCHY_CHECK_WITH(require, prepared.has_value());
    channel::PreparedSend moved(std::move(*prepared));
    OMARCHY_CHECK_WITH(require, first.opened.channel->try_send(*prepared, deadline_after(2s)) ==
                    channel::ChannelSendStatus::fatal &&
                second.opened.channel->try_send(moved, deadline_after(2s)) ==
                    channel::ChannelSendStatus::fatal &&
                moved.pending() &&
                first.opened.channel->try_send(moved, deadline_after(2s)) ==
                    channel::ChannelSendStatus::complete &&
                !moved.pending());
  }
  {
    Session stale("valid", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, stale.opened.channel->negotiate(deadline_after(2s)));
    stale.authority->generation = 48;
    OMARCHY_CHECK_WITH(require, !stale.opened.channel->prepare_send(
                wire::EndpointRole::broker, broker::kBrokerResultMessage, 1,
                {}) &&
                stale.opened.channel->failed());
  }
  {
    Session stale("valid", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, stale.opened.channel->negotiate(deadline_after(2s)));
    auto prepared = stale.opened.channel->prepare_send(
        wire::EndpointRole::broker, broker::kBrokerResultMessage, 1, {});
    stale.authority->generation = 48;
    OMARCHY_CHECK_WITH(require,
        prepared &&
            stale.opened.channel->try_send(*prepared, deadline_after(2s)) ==
                channel::ChannelSendStatus::not_ready &&
            !prepared->pending() && stale.opened.channel->failed() &&
            eventually_removed(stale.scope));
  }
  {
    Session session("valid", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, session.opened.channel->negotiate(deadline_after(2s)));
    session.authority->generation = 48;
    const auto received = receive_authenticated(*session.opened.channel,
        launcher::EndpointMask::broker, deadline_after(2s));
    OMARCHY_CHECK_WITH(require, received.status == channel::AuthenticatedReceiveStatus::fatal &&
                session.opened.channel->failure() ==
                    channel::ChannelFailure::stale_generation &&
                eventually_removed(session.scope));
  }
  {
    Session session("valid", FAKE_BWRAP_PATH);
    const auto received = receive_authenticated(*session.opened.channel,
        launcher::EndpointMask::broker, std::chrono::steady_clock::now());
    OMARCHY_CHECK_WITH(require, received.status == channel::AuthenticatedReceiveStatus::not_ready &&
                session.opened.channel->failed() &&
                eventually_removed(session.scope));
    OMARCHY_CHECK_WITH(require, session.opened.channel->terminate(deadline_after(6s)));
  }
  {
    Session session("valid", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, session.opened.channel->negotiate(deadline_after(2s)) &&
                session.opened.channel->ready());
    const auto request = receive_authenticated(*session.opened.channel,
        launcher::EndpointMask::broker, deadline_after(2s));
    const auto empty = session.opened.channel->try_receive_authenticated(
        launcher::EndpointMask::broker);
    OMARCHY_CHECK_WITH(require, is_notification_request(request) &&
                empty.status ==
                    channel::AuthenticatedReceiveStatus::would_block &&
                !empty.message);
    OMARCHY_CHECK_WITH(require, session.opened.channel->terminate(deadline_after(6s)) &&
                eventually_removed(session.scope));
  }
  for (const std::string_view mode :
       {"pre-ready", "wrong-role", "bad-version", "unsupported-envelope-version",
        "oversized-control-hello", "descendant", "peer-loss"}) {
    Session session(mode, FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, !session.opened.channel->negotiate(deadline_after(2s)) &&
                session.opened.channel->failed() &&
                eventually_removed(session.scope));
  }

  for (const std::string_view mode : {"descriptor", "descriptor-flood"}) {
    const auto descriptors_before = descriptor_count();
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
      Session session(mode, FAKE_BWRAP_PATH);
      OMARCHY_CHECK_WITH(require, !session.opened.channel->negotiate(deadline_after(2s)));
    }
    OMARCHY_CHECK_WITH(require, eventually_descriptor_count_at_most(descriptors_before));
  }
  {
    const auto descriptors_before = descriptor_count();
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
      Session injected("post-ready-descriptor", FAKE_BWRAP_PATH);
      OMARCHY_CHECK_WITH(require, injected.opened.channel->negotiate(deadline_after(2s)));
      const auto received = receive_authenticated(*injected.opened.channel,
          launcher::EndpointMask::broker, deadline_after(2s));
      OMARCHY_CHECK_WITH(require, received.status == channel::AuthenticatedReceiveStatus::fatal &&
                  !received.message);
    }
    OMARCHY_CHECK_WITH(require, eventually_descriptor_count_at_most(descriptors_before));
  }

  for (const std::string_view mode :
       {"stale", "bad-role-version",
        "wrong-sequence-tag", "unsupported-envelope-version-after-ready",
        "unknown-message", "wrong-direction", "inbound-typed-error",
        "inbound-cancel", "inbound-cancel-result",
        "short-payload", "zero-correlation", "post-ready-descriptor",
        "bad-magic", "bad-header-size", "bad-envelope-role",
        "bad-payload-length", "nonzero-flags", "nonzero-reserved"}) {
    Session session(mode, FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, session.opened.channel->negotiate(deadline_after(2s)));
    const auto received = receive_authenticated(*session.opened.channel,
        launcher::EndpointMask::broker, deadline_after(2s));
    OMARCHY_CHECK_WITH(require, received.status == channel::AuthenticatedReceiveStatus::fatal &&
                !received.message && session.opened.channel->failed() &&
                eventually_removed(session.scope));
  }
  {
    Session descriptors("valid", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, descriptors.opened.channel->negotiate(deadline_after(2s)));
    auto prepared = descriptors.opened.channel->prepare_send(
        wire::EndpointRole::broker, broker::kBrokerResultMessage, 9, {});
    UniqueFd injected(open("/dev/null", O_RDONLY | O_CLOEXEC));
    const std::array borrowed{injected.get()};
    OMARCHY_CHECK_WITH(require, prepared && injected.get() >= 0 &&
                descriptors.opened.channel->try_send(
                    *prepared, deadline_after(2s), borrowed) ==
                    channel::ChannelSendStatus::fatal &&
                !prepared->pending() && descriptors.opened.channel->failed());
  }
  {
    Session descriptors("valid", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, descriptors.opened.channel->negotiate(deadline_after(2s)));
    const std::array<std::byte, 96> allocation{};
    auto prepared = descriptors.opened.channel->prepare_send(
        wire::EndpointRole::render,
        static_cast<std::uint16_t>(
            surface::RenderMessageType::surface_allocate),
        10, allocation);
    OMARCHY_CHECK_WITH(require, prepared &&
                descriptors.opened.channel->try_send(*prepared,
                                                     deadline_after(2s)) ==
                    channel::ChannelSendStatus::fatal &&
                descriptors.opened.channel->failed());
  }
  for (const auto role : {wire::EndpointRole::control, wire::EndpointRole::broker,
                          wire::EndpointRole::render})
    for (const auto type : {wire::CommonMessageType::cancel, wire::CommonMessageType::cancel_result}) {
      Session common("valid", FAKE_BWRAP_PATH);
      OMARCHY_CHECK_WITH(require, common.opened.channel->negotiate(deadline_after(2s)));
      const std::array payload{std::byte{0}, std::byte{1}};
      OMARCHY_CHECK_WITH(require, !common.opened.channel->prepare_send(
          role, static_cast<std::uint16_t>(type), 10,
          type == wire::CommonMessageType::cancel ? std::span<const std::byte>{} : payload) &&
          common.opened.channel->failed() && eventually_removed(common.scope));
    }
  {
    Session saturated("host-saturation", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, saturated.opened.channel->negotiate(deadline_after(2s)));
    const auto timed_receive = receive_authenticated(*saturated.opened.channel,
        launcher::EndpointMask::render, deadline_after(5ms));
    OMARCHY_CHECK_WITH(require, timed_receive.status ==
                    channel::AuthenticatedReceiveStatus::would_block &&
                !timed_receive.message && !saturated.opened.channel->failed());
    std::vector<std::byte> payload(65536);
    std::optional<channel::PreparedSend> blocked;
    for (unsigned attempt = 0; attempt < 10'000 && !blocked; ++attempt) {
      auto prepared = saturated.opened.channel->prepare_send(
          wire::EndpointRole::broker, broker::kBrokerResultMessage,
          attempt + 1, payload);
      OMARCHY_CHECK_WITH(require, prepared.has_value());
      const auto status =
          saturated.opened.channel->try_send(*prepared, deadline_after(2s));
      if (status == channel::ChannelSendStatus::would_block)
        blocked.emplace(std::move(*prepared));
      else
        OMARCHY_CHECK_WITH(require, status == channel::ChannelSendStatus::complete);
    }
    OMARCHY_CHECK_WITH(require, blocked.has_value());
    OMARCHY_CHECK_WITH(require,
        saturated.opened.channel->try_send(*blocked, deadline_after(2s)) ==
                channel::ChannelSendStatus::would_block &&
            saturated.opened.channel->arm_readiness(
                launcher::EndpointMask::none, launcher::EndpointMask::broker));
    const auto invalid_receive =
        receive_authenticated(*saturated.opened.channel,
            launcher::EndpointMask::render, deadline_after(20ms));
    OMARCHY_CHECK_WITH(require, invalid_receive.status ==
                    channel::AuthenticatedReceiveStatus::not_ready &&
                !invalid_receive.message && !saturated.opened.channel->failed());
    pollfd readiness{.fd = saturated.opened.channel->readiness_fd(),
                     .events = POLLIN,
                     .revents = 0};
    OMARCHY_CHECK_WITH(require, poll(&readiness, 1, 20) == 0);
    OMARCHY_CHECK_WITH(require,
        kill(saturated.opened.channel->identity().outer_worker_pid, SIGUSR1) ==
                0 &&
            poll(&readiness, 1, 2000) == 1 &&
            saturated.opened.channel->try_send(*blocked, deadline_after(2s)) ==
                channel::ChannelSendStatus::complete &&
            !blocked->pending());
  }

  {
    Session bounded("host-saturation", FAKE_BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, bounded.opened.channel->negotiate(deadline_after(2s)));
    const auto started = std::chrono::steady_clock::now();
    const auto first = bounded.opened.channel->terminate(started);
    const auto repeated = bounded.opened.channel->terminate(deadline_after(2s));
    OMARCHY_CHECK_WITH(require, first == repeated &&
                std::chrono::steady_clock::now() - started < 100ms);
    OMARCHY_CHECK_WITH(require, eventually_removed(bounded.scope));
  }

  for (const auto mode : {"ready-loss", "ready-crash"}) {
    Session session(mode, FAKE_BWRAP_PATH);
    session.negotiate_and_request_peer_exit();
    const auto terminal = receive_authenticated(*session.opened.channel,
        launcher::EndpointMask::broker, deadline_after(2s));
    OMARCHY_CHECK_WITH(require,
        !session.opened.channel->alive() &&
            (terminal.status ==
                 channel::AuthenticatedReceiveStatus::peer_closed ||
             terminal.status == channel::AuthenticatedReceiveStatus::fatal) &&
            !terminal.message &&
            session.opened.channel->failure() ==
                channel::ChannelFailure::peer_failure &&
            session.opened.channel->terminate(deadline_after(6s)) &&
            eventually_removed(session.scope));
  }
}

int bwrap_suite() {
  if (access(BWRAP_PATH, X_OK) < 0) {
    return 77;
  }
  try {
    Session valid("valid", BWRAP_PATH);
    if (!valid.opened.channel->negotiate(deadline_after(4s))) {
      return 77;
    }
    const auto request = receive_authenticated(*valid.opened.channel,
        launcher::EndpointMask::broker, deadline_after(4s));
    OMARCHY_CHECK_WITH(require, is_notification_request(request));
    OMARCHY_CHECK_WITH(require, valid.opened.channel->terminate(deadline_after(6s)));

    Session stale("stale", BWRAP_PATH);
    OMARCHY_CHECK_WITH(require, stale.opened.channel->negotiate(deadline_after(4s)));
    const auto rejected = receive_authenticated(*stale.opened.channel,
        launcher::EndpointMask::broker, deadline_after(4s));
    OMARCHY_CHECK_WITH(require, rejected.status == channel::AuthenticatedReceiveStatus::fatal);
  } catch (...) {
    return 77;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  OMARCHY_CHECK_WITH(require, argc == 2);
  if (std::string_view(argv[1]) == "fake") {
    fake_suite();
    return 0;
  }
  if (std::string_view(argv[1]) == "bwrap") {
    return bwrap_suite();
  }
  fail("unknown suite selector");
}
