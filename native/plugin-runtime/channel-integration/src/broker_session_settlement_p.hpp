#pragma once
#include "structured_broker.hpp"
#include "authenticated_channel.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <utility>

namespace omarchy::plugin_runtime::channel {

namespace broker_session = omarchy::plugin_runtime::host_session;
namespace gesture_runtime = omarchy::plugin_runtime::runtime;

enum class BrokerSettlementStatus : std::uint8_t {
  complete,
  would_block,
  fatal,
};

class ChannelBrokerReplyTransport final {
public:
  explicit ChannelBrokerReplyTransport(AuthenticatedBrokerChannel &channel)
      : channel_(channel) {}

  bool prepare(std::uint16_t message_type, std::uint64_t correlation,
               std::span<const std::byte> payload) {
    if (prepared_)
      return false;
    auto prepared = channel_.prepare_send(wire::EndpointRole::broker,
                                          message_type, correlation, payload);
    if (!prepared)
      return false;
    prepared_.emplace(std::move(*prepared));
    return true;
  }

  ChannelSendStatus try_send(launcher::Deadline deadline) {
    if (!prepared_)
      return ChannelSendStatus::fatal;
    const auto status = channel_.try_send(*prepared_, deadline);
    if (status != ChannelSendStatus::would_block)
      prepared_.reset();
    return status;
  }
  void clear() noexcept { prepared_.reset(); }

private:
  AuthenticatedBrokerChannel &channel_;
  std::optional<PreparedSend> prepared_;
};

// Owns the one unsettled broker effect and its byte-identical, descriptor-free
// reply. This class is used only on the authenticated session worker thread.
template <typename Transport>
class BrokerSessionSettlementFor final {
public:
  BrokerSessionSettlementFor(
      Transport transport, broker_session::StructuredBroker &broker,
      broker_session::AuthenticatedBrokerAdmission admission,
      gesture_runtime::GestureEligibilityAuthority *gesture_authority = nullptr)
      : transport_(std::move(transport)), broker_(broker),
        admission_(std::move(admission)), gesture_authority_(gesture_authority) {}
  ~BrokerSessionSettlementFor() { (void)abort(); }
  BrokerSessionSettlementFor(const BrokerSessionSettlementFor &) = delete;
  BrokerSessionSettlementFor &operator=(const BrokerSessionSettlementFor &) = delete;

  [[nodiscard]] BrokerSettlementStatus dispatch(AuthenticatedMessage message,
                                                launcher::Deadline deadline) {
    if (pending_) {
      return fail_pending();
    }
    if (terminal_ || failed_ || message.role != wire::EndpointRole::broker ||
        !message.descriptors.empty() ||
        std::chrono::steady_clock::now() >= deadline) {
      return fail_terminal();
    }
    auto admitted = admission_.admit({.message_type = message.message_type,
                                      .correlation_id = message.correlation_id,
                                      .payload = message.payload});
    if (!admitted) {
      return fail_terminal();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return fail_terminal();
    }
    try {
      auto transaction =
          broker_.dispatch(std::move(*admitted.request),
                           provider_response_, gesture_authority_);
      if (transaction.state() != broker_session::TransactionState::reply) {
        return fail_terminal();
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        (void)abort_transaction(std::move(transaction));
        return fail_terminal();
      }
      try {
        if (!transport_.prepare(transaction.message_type(),
                                 transaction.correlation(),
                                 transaction.wire_payload())) {
          (void)abort_transaction(std::move(transaction));
          return fail_terminal();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          transport_.clear();
          (void)abort_transaction(std::move(transaction));
          return fail_terminal();
        }
        pending_.emplace(std::move(transaction));
      } catch (...) {
        transport_.clear();
        (void)abort_transaction(std::move(transaction));
        return fail_terminal();
      }
      return flush(deadline);
    } catch (...) {
      return fail_terminal();
    }
  }

  [[nodiscard]] BrokerSettlementStatus flush(launcher::Deadline deadline) {
    if (terminal_ || failed_)
      return BrokerSettlementStatus::fatal;
    if (!pending_)
      return BrokerSettlementStatus::complete;
    if (std::chrono::steady_clock::now() >= deadline) {
      return fail_pending();
    }
    ChannelSendStatus status = ChannelSendStatus::fatal;
    try {
      status = transport_.try_send(deadline);
    } catch (...) {
      return fail_pending();
    }
    if (status == ChannelSendStatus::would_block) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return fail_pending();
      }
      return BrokerSettlementStatus::would_block;
    }
    if (status != ChannelSendStatus::complete) {
      return fail_pending();
    }

    auto transaction = std::move(*pending_);
    pending_.reset();
    bool committed = false;
    try {
      committed = broker_.commit_sent(std::move(transaction));
    } catch (...) {
      committed = false;
    }
    const bool expired = std::chrono::steady_clock::now() >= deadline;
    if (!committed || expired) {
      return fail_terminal();
    }
    return BrokerSettlementStatus::complete;
  }

  [[nodiscard]] bool abort() noexcept {
    if (terminal_ && !pending_) {
      transport_.clear();
      return !failed_;
    }
    terminal_ = true;
    if (!pending_) {
      transport_.clear();
      return true;
    }
    auto transaction = std::move(*pending_);
    pending_.reset();
    transport_.clear();
    const bool aborted = abort_transaction(std::move(transaction));
    if (!aborted)
      failed_ = true;
    return aborted;
  }

  [[nodiscard]] bool pending() const noexcept {
    return pending_.has_value();
  }

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] launcher::EndpointMask read_lanes() const noexcept {
    if (terminal_ || failed_)
      return launcher::EndpointMask::none;
    return pending_
               ? launcher::EndpointMask::control | launcher::EndpointMask::render
               : launcher::EndpointMask::all;
  }

  [[nodiscard]] launcher::EndpointMask write_lanes() const noexcept {
    if (terminal_ || failed_)
      return launcher::EndpointMask::none;
    return pending_ ? launcher::EndpointMask::broker
                    : launcher::EndpointMask::none;
  }

private:
  [[nodiscard]] BrokerSettlementStatus fail_terminal() noexcept {
    failed_ = true;
    terminal_ = true;
    return BrokerSettlementStatus::fatal;
  }

  [[nodiscard]] BrokerSettlementStatus fail_pending() noexcept {
    failed_ = true;
    (void)abort();
    return BrokerSettlementStatus::fatal;
  }

  [[nodiscard]] bool
  abort_transaction(broker_session::BrokerTransaction &&transaction) noexcept {
    try {
      return broker_.abort_send(std::move(transaction));
    } catch (...) {
      return false;
    }
  }

  Transport transport_;
  broker_session::StructuredBroker &broker_;
  broker_session::AuthenticatedBrokerAdmission admission_;
  gesture_runtime::GestureEligibilityAuthority *gesture_authority_ = nullptr;
  std::optional<broker_session::BrokerTransaction> pending_;
  std::array<std::byte, broker_session::kMaximumOwnedBrokerReplyBytes>
      provider_response_{};
  bool failed_ = false;
  bool terminal_ = false;
};

using BrokerSessionSettlement = BrokerSessionSettlementFor<ChannelBrokerReplyTransport>;

} // namespace omarchy::plugin_runtime::channel
