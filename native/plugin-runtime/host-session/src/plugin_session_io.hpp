#pragma once

#include "omarchy/plugin_runtime/unique_fd.hpp"

#include <QObject>
#include <QThread>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omarchy::plugin_runtime::channel {
class AuthenticatedSessionChannel;
class PluginSession;
}

namespace omarchy::plugin_runtime::host_session {

using ::omarchy::plugin_runtime::UniqueFd;

struct SessionToken {
  std::string plugin_id;
  std::string revision_sha256;
  std::uint64_t generation = 0;
  std::uint64_t session_nonce = 0;
  bool operator==(const SessionToken &) const = default;
};

enum class ChannelLane : std::uint8_t { control, broker, render };

struct OwnedMessage {
  // Messages belong to their one owning session queue. Wire authentication
  // and replay checks live in the channel; queue epochs fence late delivery.
  ChannelLane lane = ChannelLane::render;
  std::uint16_t message_type = 0;
  // Semantic request/reply correlation. The authenticated transport remains
  // the sole encoder of wire headers and lane sequence numbers.
  std::uint64_t correlation_id = 0;
  std::vector<std::byte> payload;
  std::vector<UniqueFd> descriptors;
};

enum class ChannelError : std::uint8_t {
  none,
  launch_failed,
  handshake_failed,
  protocol_failed,
};
enum class SendStatus : std::uint8_t {
  complete,
  would_block,
  peer_closed,
  fatal,
};
enum class ReceiveStatus : std::uint8_t {
  would_block,
  message,
  peer_closed,
  fatal,
};

struct ReceiveResult {
  ReceiveStatus status = ReceiveStatus::would_block;
  OwnedMessage message;
};

// Allocation-free readiness callback representation installed and invoked
// only on the PluginSessionIo worker thread. The callback may queue Qt work.
// AuthenticatedSessionChannel::clear_wake_handler() synchronously fences future callbacks
// before terminal work continues.
struct SessionWakeHandler {
  using Function = void (*)(void *context) noexcept;

  Function function = nullptr;
  void *context = nullptr;

  [[nodiscard]] explicit operator bool() const noexcept {
    return function != nullptr;
  }
  void invoke() const noexcept {
    if (function != nullptr)
      function(context);
  }
};

enum class SessionState : std::uint8_t {
  idle,
  starting,
  running,
  revoking,
  revoked,
  stopping,
  stopped,
  failed,
};
enum class SessionError : std::uint8_t {
  none,
  invalid_configuration,
  invalid_token,
  startup_deadline_expired,
  io_deadline_expired,
  launch_failed,
  handshake_failed,
  channel_failed,
  queue_limit,
};

struct SessionLimits {
  static constexpr std::size_t kMaximumMessages = 1024;
  static constexpr std::size_t kMaximumBytes = 16ULL * 1024 * 1024;
  static constexpr std::size_t kMaximumDescriptors = 256;
  static constexpr std::size_t kMaximumPumpBatch = 256;
  static constexpr std::chrono::milliseconds kMaximumStartupTimeout{30500};
  static constexpr std::chrono::milliseconds kMaximumIoTimeout{30500};

  std::size_t maximum_queued_messages = 64;
  std::size_t maximum_queued_bytes = 1024ULL * 1024;
  std::size_t maximum_descriptors_per_message = 8;
  std::size_t maximum_queued_descriptors = 64;
  std::size_t maximum_pump_batch = 32;
  std::chrono::milliseconds startup_timeout{5000};
  std::chrono::milliseconds io_timeout{10};
};

struct PluginSessionSharedState;
struct PluginSessionRuntimeOwner;
#ifdef OMARCHY_PLUGIN_SESSION_TESTING
class PluginSessionIoTestAccess;
#endif

// Owns one asynchronous channel lifecycle for one plugin activation. Public
// teardown closes admission and queues bounded worker-thread termination; it
// never waits for launch, I/O or QThread shutdown on the UI thread.
class PluginSessionIo final : public QObject {
  // Constructed only as its recipient's private member on the same thread.
  friend class channel::PluginSession;
  PluginSessionIo(SessionToken token, std::unique_ptr<channel::AuthenticatedSessionChannel> channel,
                  channel::PluginSession &owner, SessionLimits limits);

public:
  ~PluginSessionIo() override;
  PluginSessionIo(const PluginSessionIo &) = delete;
  PluginSessionIo &operator=(const PluginSessionIo &) = delete;

  void start();
  [[nodiscard]] bool enqueue(OwnedMessage message);
  void wake();
  void revoke();
  void stop();
  [[nodiscard]] SessionState state() const noexcept;
  [[nodiscard]] SessionError error() const noexcept;

private:
  enum class TerminationIntent : std::uint8_t { revoke, stop, destroy };
  class Worker;
  void fence_and_queue(TerminationIntent intent);
  [[nodiscard]] bool schedule_pump_locked(std::uint64_t epoch);
  template <typename Operation>
  [[nodiscard]] bool queue_worker(Operation operation);
  void invocation_failed_locked() noexcept;

  static bool queue_state(const std::shared_ptr<PluginSessionSharedState> &shared,
                           std::uint64_t epoch, SessionState state, SessionError error);
  static bool queue_delivery(const std::shared_ptr<PluginSessionSharedState> &shared,
                              std::uint64_t epoch, OwnedMessage message);

  std::shared_ptr<PluginSessionSharedState> shared_;
  std::shared_ptr<PluginSessionRuntimeOwner> runtime_;
  channel::PluginSession &owner_;

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
  friend class PluginSessionIoTestAccess;
#endif
};

#ifdef OMARCHY_PLUGIN_SESSION_TESTING
class PluginSessionIoTestAccess final {
public:
  [[nodiscard]] static std::size_t pending_deliveries(PluginSessionIo &session);
  [[nodiscard]] static std::size_t pending_outbound(PluginSessionIo &session);
};
#endif

} // namespace omarchy::plugin_runtime::host_session
