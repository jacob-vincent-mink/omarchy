#include "channel_roles.hpp"
#include "authenticated_session_channel.hpp"
#include "broker_session_settlement_p.hpp"

#include <QSocketNotifier>

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <optional>
#include <utility>

#include <poll.h>

namespace omarchy::plugin_runtime::channel {

namespace {

using detail::role_as;
using detail::valid_role;

launcher::EndpointMask lane_mask(wire::EndpointRole role) noexcept {
  return role_as<launcher::EndpointMask>(role, launcher::EndpointMask::none);
}

launcher::EndpointMask intersect(launcher::EndpointMask left,
                                 launcher::EndpointMask right) noexcept {
  return static_cast<launcher::EndpointMask>(static_cast<std::uint8_t>(left) &
                                             static_cast<std::uint8_t>(right));
}

session::SendStatus map_send(ChannelSendStatus status) noexcept {
  switch (status) {
  case ChannelSendStatus::complete:
    return session::SendStatus::complete;
  case ChannelSendStatus::would_block:
    return session::SendStatus::would_block;
  case ChannelSendStatus::peer_closed:
    return session::SendStatus::peer_closed;
  case ChannelSendStatus::fatal:
  case ChannelSendStatus::not_ready:
    return session::SendStatus::fatal;
  }
  return session::SendStatus::fatal;
}

bool wait_for_channel(auto &backend,
                      launcher::EndpointMask reads,
                      launcher::EndpointMask writes,
                      launcher::Deadline deadline) noexcept {
  if (std::chrono::steady_clock::now() >= deadline ||
      !backend.arm(reads, writes))
    return false;
  pollfd event{.fd = backend.readiness_fd(), .events = POLLIN, .revents = 0};
  if (event.fd < 0)
    return false;
  int ready = -1;
  do {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return false;
    const auto remaining = deadline - now;
    const auto timeout = static_cast<int>(std::min<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                .count() +
            1,
        std::numeric_limits<int>::max()));
    ready = ::poll(&event, 1, timeout);
  } while (ready < 0 && errno == EINTR);
  return ready == 1 && (event.revents & POLLIN) != 0 &&
         (event.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0 &&
         std::chrono::steady_clock::now() < deadline;
}

bool establish_startup_snapshot(auto &backend,
                                std::uint16_t message_type,
                                std::uint16_t accepted_type,
                                std::span<const std::byte> snapshot,
                                launcher::Deadline deadline) {
  if (snapshot.empty() ||
      snapshot.size() > wire::payload_cap(wire::EndpointRole::control) ||
      std::chrono::steady_clock::now() >= deadline ||
      !backend.prepare(wire::EndpointRole::control,
                       message_type, 0, snapshot))
    return false;

  for (;;) {
    const auto sent = backend.try_send({}, deadline);
    if (sent == session::SendStatus::complete)
      break;
    if (sent != session::SendStatus::would_block ||
        !wait_for_channel(backend, launcher::EndpointMask::none,
                          launcher::EndpointMask::control, deadline))
      return false;
  }

  for (;;) {
    if (std::chrono::steady_clock::now() >= deadline ||
        !backend.arm(launcher::EndpointMask::control,
                     launcher::EndpointMask::none))
      return false;
    auto reply = backend.receive(launcher::EndpointMask::control, deadline);
    if (reply.status == AuthenticatedReceiveStatus::message && reply.message) {
      return reply.message->role == wire::EndpointRole::control &&
             reply.message->message_type ==
                 accepted_type &&
             reply.message->correlation_id == 0 &&
             reply.message->payload.empty() &&
             reply.message->descriptors.empty() &&
             std::chrono::steady_clock::now() < deadline;
    }
    if (reply.status != AuthenticatedReceiveStatus::would_block ||
        !wait_for_channel(backend, launcher::EndpointMask::control,
                          launcher::EndpointMask::none, deadline))
      return false;
  }
}


} // namespace

struct AuthenticatedSessionChannel::Impl final {
  launcher::Supervisor supervisor_;
  AuthenticatedSessionLaunch launch_;
  std::shared_ptr<AuthenticatedSessionRuntime> runtime_;
  std::shared_ptr<runtime::GestureEligibilityLatch> gesture_eligibility_;
  std::unique_ptr<AuthenticatedBrokerChannel> channel_;
  std::optional<BrokerSessionSettlement> settlement_;
  std::optional<PreparedSend> prepared_;

  Impl(
      launcher::Supervisor supervisor, AuthenticatedSessionLaunch launch,
      std::unique_ptr<AuthenticatedSessionRuntime> runtime,
      std::shared_ptr<runtime::GestureEligibilityLatch> gesture_eligibility)
      : supervisor_(std::move(supervisor)), launch_(std::move(launch)),
        runtime_(std::move(runtime)),
        gesture_eligibility_(std::move(gesture_eligibility)) {}


  ~Impl() { terminate(std::chrono::steady_clock::now()); }

  session::ChannelError launch(const session::SessionToken &token,
                               launcher::Deadline deadline) {
    if (channel_ || token.plugin_id != launch_.binding.plugin.view() ||
        token.revision_sha256 != launch_.binding.revision.view() ||
        token.generation != launch_.binding.generation ||
        !launch_.revision_directory || !launch_.private_state_directory ||
        !runtime_ ||
        !runtime_->broker().accepts(launch_.binding, token.session_nonce))
      return session::ChannelError::launch_failed;
    const launcher::TrustedLaunchRequest request{
        .plugin_id = std::string(launch_.binding.plugin.view()),
        .revision_sha256 = std::string(launch_.binding.revision.view()),
        .generation = launch_.binding.generation,
        .revision_directory_fd = launch_.revision_directory.get(),
        .private_state_directory_fd = launch_.private_state_directory.get()};
    auto opened = AuthenticatedBrokerChannel::open(
        supervisor_, request, std::shared_ptr<const GenerationAuthority>(
            runtime_, static_cast<const GenerationAuthority *>(runtime_.get())), deadline);
    if (!opened)
      return session::ChannelError::launch_failed;
    channel_ = std::move(opened.channel);
    auto extracted = runtime_->broker().take_admission();
    if (!extracted) {
      (void)channel_->terminate(deadline);
      channel_.reset();
      return session::ChannelError::launch_failed;
    }
    try {
      settlement_.emplace(ChannelBrokerReplyTransport(*channel_), runtime_->broker(),
                          std::move(*extracted.admission),
                          gesture_eligibility_.get());
    } catch (...) {
      (void)channel_->terminate(deadline);
      channel_.reset();
      return session::ChannelError::launch_failed;
    }
    launch_.revision_directory.reset();
    launch_.private_state_directory.reset();
    return session::ChannelError::none;
  }

  session::ChannelError handshake(launcher::Deadline deadline) {
    if (!channel_ || !channel_->negotiate(deadline))
      return session::ChannelError::handshake_failed;
    return session::ChannelError::none;
  }

  bool prepare(wire::EndpointRole role, std::uint16_t message_type,
               std::uint64_t correlation_id, std::span<const std::byte> payload) {
    if (!channel_ || prepared_ || role == wire::EndpointRole::broker)
      return false;
    auto prepared =
        channel_->prepare_send(role, message_type, correlation_id, payload);
    if (!prepared)
      return false;
    prepared_.emplace(std::move(*prepared));
    return true;
  }

  session::SendStatus try_send(std::span<const int> descriptors,
                               launcher::Deadline deadline) {
    if (!channel_ || !prepared_)
      return session::SendStatus::fatal;
    const auto status =
        map_send(channel_->try_send(*prepared_, deadline, descriptors));
    if (status != session::SendStatus::would_block)
      prepared_.reset();
    return status;
  }

  AuthenticatedReceiveResult receive(launcher::EndpointMask allowed_lanes,
                                     launcher::Deadline deadline) {
    if (!channel_ || !settlement_)
      return {};
    if (settlement_->pending()) {
      const auto flushed = settlement_->flush(deadline);
      if (flushed == BrokerSettlementStatus::fatal)
        return {.status = AuthenticatedReceiveStatus::fatal,
                .message = std::nullopt};
      // Completion expands the readable lanes. Yield so PluginSessionIo can
      // re-arm that mask before this readiness-driven backend receives again.
      if (flushed == BrokerSettlementStatus::complete)
        return {.status = AuthenticatedReceiveStatus::would_block,
                .message = std::nullopt};
    }
    const auto effective = intersect(allowed_lanes, settlement_->read_lanes());
    if (effective == launcher::EndpointMask::none)
      return {.status = AuthenticatedReceiveStatus::would_block,
              .message = std::nullopt};
    auto received = channel_->try_receive_authenticated(effective);
    if (!received || received.message->role != wire::EndpointRole::broker)
      return received;
    const auto settled =
        settlement_->dispatch(std::move(*received.message), deadline);
    if (settled == BrokerSettlementStatus::fatal)
      return {.status = AuthenticatedReceiveStatus::fatal,
              .message = std::nullopt};
    return {.status = AuthenticatedReceiveStatus::would_block,
            .message = std::nullopt};
  }

  int readiness_fd() const noexcept {
    return channel_ ? channel_->readiness_fd() : -1;
  }

  bool arm(launcher::EndpointMask reads,
           launcher::EndpointMask writes) noexcept {
    if (!channel_ || !settlement_)
      return false;
    return channel_->arm_readiness(intersect(reads, settlement_->read_lanes()),
                                   writes | settlement_->write_lanes());
  }


  [[nodiscard]] bool arm_wait() noexcept {
    const auto writes = prepared_ ? lane_mask(prepared_->role())
                                  : launcher::EndpointMask::none;
    if (!arm(launcher::EndpointMask::all, writes))
      return false;
    if (notifier)
      notifier->setEnabled(true);
    return true;
  }

  void terminate(launcher::Deadline deadline) noexcept {
    if (terminal)
      return;
    terminal = true;
    clear_wake();
    launch_.permission_snapshot.clear();
    launch_.settings_snapshot.clear();
    launch_.presentation_snapshot.clear();
    if (settlement_)
      (void)settlement_->abort();
    settlement_.reset();
    prepared_.reset();
    if (channel_)
      (void)channel_->terminate(deadline);
    channel_.reset();
    launch_.revision_directory.reset();
    launch_.private_state_directory.reset();
    runtime_.reset();
    gesture_eligibility_.reset();
    token.reset();
  }

  void clear_wake() noexcept {
    ++wake_generation;
    wake_pending = false;
    if (notifier)
      notifier->setEnabled(false);
    notifier.reset();
    wake = {};
  }

  std::optional<session::SessionToken> token;
  std::optional<launcher::Deadline> startup_deadline;
  std::unique_ptr<QSocketNotifier> notifier;
  std::unique_ptr<QObject> wake_context;
  session::SessionWakeHandler wake;
  std::uint64_t wake_generation = 0;
  bool wake_pending = false;
  bool launched = false;
  bool ready = false;
  bool terminal = false;
};

AuthenticatedSessionChannel::AuthenticatedSessionChannel(
    launcher::Supervisor supervisor, AuthenticatedSessionLaunch launch,
    std::unique_ptr<AuthenticatedSessionRuntime> runtime,
    std::shared_ptr<runtime::GestureEligibilityLatch> gesture_eligibility) {
  implementation_ = std::make_unique<Impl>(
      std::move(supervisor), std::move(launch),
      std::move(runtime), std::move(gesture_eligibility));
}


AuthenticatedSessionChannel::~AuthenticatedSessionChannel() = default;

session::ChannelError
AuthenticatedSessionChannel::launch(const session::SessionToken &token,
                                    TimePoint deadline) {
  auto &value = *implementation_;
  if (value.launched || value.terminal ||
      value.launch_.permission_snapshot.empty() || value.launch_.settings_snapshot.empty() ||
      value.launch_.presentation_snapshot.empty() ||
      value.launch_.permission_snapshot.size() >
          wire::payload_cap(wire::EndpointRole::control) ||
      value.launch_.settings_snapshot.size() >
          wire::payload_cap(wire::EndpointRole::control) ||
      value.launch_.presentation_snapshot.size() >
          wire::payload_cap(wire::EndpointRole::control) ||
      token.plugin_id.empty() || token.revision_sha256.empty() ||
      token.generation == 0 || token.session_nonce == 0 ||
      std::chrono::steady_clock::now() >= deadline)
    return session::ChannelError::launch_failed;
  const auto result = value.launch(token, deadline);
  if (result != session::ChannelError::none) {
    value.terminate(deadline);
    return result;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    value.terminate(deadline);
    return session::ChannelError::launch_failed;
  }
  value.token = token;
  value.startup_deadline = deadline;
  value.launched = true;
  return session::ChannelError::none;
}

session::ChannelError
AuthenticatedSessionChannel::handshake(TimePoint deadline) {
  auto &value = *implementation_;
  if (!value.launched || value.ready || value.terminal ||
      !value.startup_deadline || deadline != *value.startup_deadline)
    return session::ChannelError::handshake_failed;
  const auto result = value.handshake(deadline);
  if (result != session::ChannelError::none) {
    value.terminate(deadline);
    return result;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    value.terminate(deadline);
    return session::ChannelError::handshake_failed;
  }
  if (!establish_startup_snapshot(
          value, wire::kSettingsSnapshotMessage,
          wire::kSettingsSnapshotAcceptedMessage, value.launch_.settings_snapshot,
          deadline) ||
      !establish_startup_snapshot(
          value, wire::kPresentationSnapshotMessage,
          wire::kPresentationSnapshotAcceptedMessage,
          value.launch_.presentation_snapshot, deadline) ||
      !establish_startup_snapshot(
          value, wire::kPermissionSnapshotMessage,
          wire::kPermissionSnapshotAcceptedMessage,
          value.launch_.permission_snapshot, deadline)) {
    value.terminate(deadline);
    return session::ChannelError::protocol_failed;
  }
  value.launch_.permission_snapshot.clear();
  value.launch_.settings_snapshot.clear();
  value.launch_.presentation_snapshot.clear();
  value.ready = true;
  return session::ChannelError::none;
}

session::SendStatus
AuthenticatedSessionChannel::send(const session::OwnedMessage &message,
                                  TimePoint deadline) {
  auto &value = *implementation_;
  try {
    const auto fail = [&value, deadline] {
      value.terminate(deadline);
      return session::SendStatus::fatal;
    };
    if (!value.ready || value.terminal || !value.token ||
        !valid_role(message.lane) ||
        message.lane == session::ChannelLane::broker ||
        (message.lane == session::ChannelLane::control &&
         wire::is_startup_snapshot(message.message_type)) ||
        message.message_type == 0 ||
        message.descriptors.size() > launcher::kMaximumTransportDescriptors ||
        message.payload.size() > wire::payload_cap(role_as<wire::EndpointRole>(message.lane)) ||
        std::chrono::steady_clock::now() >= deadline)
      return fail();

    std::array<int, launcher::kMaximumTransportDescriptors> descriptors;
    for (std::size_t index = 0; index < message.descriptors.size(); ++index) {
      if (!message.descriptors[index])
        return fail();
      descriptors[index] = message.descriptors[index].get();
    }
    if (value.prepared_) {
      if (!value.prepared_->matches(role_as<wire::EndpointRole>(message.lane),
                                    message.message_type, message.correlation_id,
                                    message.payload, message.descriptors.size()))
        return fail();
    } else {
      if (!value.prepare(role_as<wire::EndpointRole>(message.lane), message.message_type,
                          message.correlation_id, message.payload))
        return fail();
    }
    const auto status = value.try_send(
        std::span(descriptors).first(message.descriptors.size()), deadline);
    if (std::chrono::steady_clock::now() >= deadline) {
      value.terminate(deadline);
      return session::SendStatus::fatal;
    }
    if (status == session::SendStatus::would_block) {
      if (!value.arm_wait())
        return fail();
      if (std::chrono::steady_clock::now() >= deadline)
        return fail();
      return status;
    }
    if (status != session::SendStatus::complete)
      value.terminate(deadline);
    return status;
  } catch (...) {
    value.terminate(deadline);
    return session::SendStatus::fatal;
  }
}

session::ReceiveResult
AuthenticatedSessionChannel::receive(TimePoint deadline) {
  auto &value = *implementation_;
  try {
    const auto fail = [&value, deadline] {
      value.terminate(deadline);
      return session::ReceiveResult{.status = session::ReceiveStatus::fatal,
                                    .message = {}};
    };
    if (!value.ready || value.terminal || !value.token ||
        std::chrono::steady_clock::now() >= deadline)
      return fail();
    if (!value.arm_wait())
      return fail();
    if (std::chrono::steady_clock::now() >= deadline)
      return fail();
    auto result = value.receive(launcher::EndpointMask::all, deadline);
    if (result.status == AuthenticatedReceiveStatus::would_block) {
      if (!value.arm_wait())
        return fail();
      if (std::chrono::steady_clock::now() >= deadline)
        return fail();
      return {.status = session::ReceiveStatus::would_block, .message = {}};
    }
    if (result.status != AuthenticatedReceiveStatus::message ||
        !result.message) {
      const auto status =
          result.status == AuthenticatedReceiveStatus::peer_closed
              ? session::ReceiveStatus::peer_closed
              : session::ReceiveStatus::fatal;
      if (status != session::ReceiveStatus::would_block)
        value.terminate(deadline);
      return {.status = status, .message = {}};
    }
    if (!valid_role(result.message->role) ||
        result.message->role == wire::EndpointRole::broker ||
        (result.message->role == wire::EndpointRole::control &&
         wire::is_startup_acknowledgement(result.message->message_type)) ||
        result.message->descriptors.size() >
            launcher::kMaximumTransportDescriptors)
      return fail();

    session::OwnedMessage message;
    message.lane = role_as<session::ChannelLane>(result.message->role);
    message.message_type = result.message->message_type;
    message.correlation_id = result.message->correlation_id;
    message.payload = std::move(result.message->payload);
    message.descriptors = std::move(result.message->descriptors);
    if (std::chrono::steady_clock::now() >= deadline) {
      return fail();
    }
    return {.status = session::ReceiveStatus::message,
            .message = std::move(message)};
  } catch (...) {
    value.terminate(deadline);
    return {.status = session::ReceiveStatus::fatal, .message = {}};
  }
}

bool AuthenticatedSessionChannel::install_wake_handler(
    session::SessionWakeHandler handler) noexcept {
  auto &value = *implementation_;
  if (!handler || value.wake || value.notifier || !value.ready ||
      value.terminal)
    return false;
  const int descriptor = value.readiness_fd();
  if (descriptor < 0)
    return false;
  try {
    if (!value.wake_context)
      value.wake_context = std::make_unique<QObject>();
    value.wake = handler;
    ++value.wake_generation;
    value.notifier =
        std::make_unique<QSocketNotifier>(descriptor, QSocketNotifier::Read);
    value.notifier->setEnabled(false);
    auto *impl = &value;
    QObject::connect(value.notifier.get(), &QSocketNotifier::activated,
                     [impl](QSocketDescriptor, QSocketNotifier::Type) {
                       if (!impl->notifier || !impl->wake || impl->wake_pending)
                         return;
                       impl->notifier->setEnabled(false);
                       impl->wake_pending = true;
                       const auto generation = impl->wake_generation;
                       bool queued = false;
                       try {
                         queued = QMetaObject::invokeMethod(
                             impl->wake_context.get(),
                             [impl, generation] {
                               if (generation != impl->wake_generation)
                                 return;
                               impl->wake_pending = false;
                               if (impl->wake)
                                 impl->wake.invoke();
                             },
                             Qt::QueuedConnection);
                       } catch (...) {
                       }
                       if (!queued) {
                         impl->wake_pending = false;
                         // The relay could not be queued. Keep the currently
                         // executing notifier alive through signal return,
                         // then wake the Worker and terminally fence transport
                         // authority. The queued pump observes fatal state.
                         auto *active = impl->notifier.release();
                         active->disconnect();
                         active->setParent(impl->wake_context.get());
                         active->deleteLater();
                         impl->wake.invoke();
                         impl->terminate(std::chrono::steady_clock::now());
                       }
                     });
    return true;
  } catch (...) {
    value.clear_wake();
    return false;
  }
}

void AuthenticatedSessionChannel::clear_wake_handler() noexcept {
  implementation_->clear_wake();
}

bool AuthenticatedSessionChannel::revoke(const session::SessionToken &token,
                                         TimePoint deadline) noexcept {
  auto &value = *implementation_;
  if (!value.token || !(token == *value.token) || value.terminal)
    return false;
  value.terminate(deadline);
  return true;
}

void AuthenticatedSessionChannel::terminate(TimePoint deadline) noexcept {
  implementation_->terminate(deadline);
}

} // namespace omarchy::plugin_runtime::channel
