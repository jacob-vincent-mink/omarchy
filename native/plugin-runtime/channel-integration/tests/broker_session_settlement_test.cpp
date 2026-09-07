#include "../../tests/support/test_assert.hpp"

#include "../../tests/support/broker_fixture.hpp"
#include "audit_store.hpp"
#include "broker_session_settlement_p.hpp"
#include "../../tests/support/capability_fixture.hpp"
#include "omarchy/plugin_runtime/providers/local_provider.hpp"

#include <deque>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace channel = omarchy::plugin_runtime::channel;
namespace session = omarchy::plugin_runtime::host_session;
namespace runtime = omarchy::plugin_runtime::runtime;
namespace permissions = omarchy::plugins::permissions;
namespace definitions = omarchy::plugins::definitions;
namespace providers = omarchy::plugin_runtime::providers;
namespace broker = omarchy::plugin_runtime::broker;
namespace audit = omarchy::plugins::audit;
namespace wire = omarchy::plugin::wire;
namespace launcher = omarchy::plugin_runtime::launcher;

namespace {
using namespace std::chrono_literals;
using omarchy::plugin_runtime::test_support::require;
permissions::Digest digest(char value) {
  return permissions::Digest(std::string(64, value));
}
permissions::ActivationBinding binding() {
  return {.plugin=permissions::PluginId("settlement.fixture"),.revision=digest('a'),
          .policy_fingerprint=digest('b'),.generation=7};
}
bool play(std::string_view, std::string_view category, std::string_view,
          std::string_view, void *) noexcept { return category == "timer"; }
class Authority final : public session::DispatchAuthority {
  class Lease final : public session::DispatchAuthorityLease {
    bool current_at_effect() const noexcept override { return true; }
  };
  std::unique_ptr<session::DispatchAuthorityLease>
  acquire(const permissions::ActivationBinding &, std::uint64_t,
          const wire::PacketView &) override {
    return std::make_unique<Lease>();
  }
};
struct TransportState {
  std::deque<channel::ChannelSendStatus> sends{};
  std::vector<std::byte> bytes{};
  std::uint16_t message_type = 0;
  std::uint64_t correlation = 0;
  unsigned prepares = 0;
  unsigned attempts = 0;
  bool throw_prepare = false;
  bool reject_prepare = false;
  bool throw_send = false;
  bool prepared = false;
  unsigned sleep_microseconds = 0;
  unsigned prepare_sleep_microseconds = 0;
};
class FakeTransport final {
public:
  explicit FakeTransport(std::shared_ptr<TransportState> state)
      : state_(std::move(state)) {}
  bool prepare(std::uint16_t message_type, std::uint64_t correlation,
               std::span<const std::byte> bytes) {
    ++state_->prepares;
    if (state_->throw_prepare)
      throw std::runtime_error("prepare");
    if (state_->prepare_sleep_microseconds != 0)
      usleep(state_->prepare_sleep_microseconds);
    if (state_->reject_prepare)
      return false;
    if (state_->prepared)
      return false;
    state_->prepared = true;
    state_->message_type = message_type;
    state_->correlation = correlation;
    state_->bytes.assign(bytes.begin(), bytes.end());
    return true;
  }
  channel::ChannelSendStatus try_send(launcher::Deadline) {
    ++state_->attempts;
    if (state_->throw_send)
      throw std::runtime_error("send");
    if (state_->sleep_microseconds != 0)
      usleep(state_->sleep_microseconds);
    const auto result = state_->sends.empty()
                            ? channel::ChannelSendStatus::complete
                            : state_->sends.front();
    if (!state_->sends.empty())
      state_->sends.pop_front();
    if (result != channel::ChannelSendStatus::would_block)
      state_->prepared = false;
    return result;
  }
  void clear() noexcept { state_->prepared = false; }

private:
  std::shared_ptr<TransportState> state_;
};
struct Fixture {
  audit::BoundedAuditLog log;
  definitions::TrustedDefinitionRegistry registry = omarchy::plugin_runtime::test_support::packaged_registry();
  definitions::DynamicRevisionGrant grant;
  providers::LocalProvider local;
  Authority authority;
  session::StructuredBroker broker;
  std::shared_ptr<TransportState> transport =
      std::make_shared<TransportState>();
  channel::BrokerSessionSettlementFor<FakeTransport> settlement;
  explicit Fixture(permissions::GrantState state =
                       permissions::GrantState::granted)
      : grant(omarchy::plugin_runtime::test_support::capability_grant(registry,
                  omarchy::plugin_runtime::test_support::capability_request(registry,
                      "notifications.send", "{\"categories\":[\"timer\"]}", false), binding(), state)),
        local(grant, definitions::EnforcementFamily::notifications, -1, play, nullptr),
        broker(binding(), 19, registry, {{.grant=grant,
                 .adapter={.binding=registry.find("notifications.send")->definition->adapter,
                           .dispatch=[this](const auto &request, auto response,
                                            std::size_t &written) noexcept {
                             return local.dispatch(request, response, written);
                           }}}}, log, authority),
        settlement(FakeTransport(transport), broker,
                   std::move(*broker.take_admission().admission)) {}
  channel::AuthenticatedMessage message(std::uint64_t correlation = 1) {
    channel::AuthenticatedMessage value;
    value.role = wire::EndpointRole::broker;
    value.message_type =
        broker::kDynamicInvokeMessage;
    value.correlation_id = correlation;
    value.payload = omarchy::plugin_runtime::test_support::notification_request("timer");
    return value;
  }
};
template <typename Function> void with_fixture(Function &&run) {
  auto fixture = std::make_unique<Fixture>();
  run(*fixture);
}
std::size_t completed_count(Fixture &fixture) {
  const auto completed =
      fixture.log.query({.plugin = std::nullopt,
                         .event = permissions::AuditEvent::operation_completed,
                         .maximum_results = audit::kHardMaximumRecords});
  OMARCHY_CHECK(completed.status.ok());
  return completed.records.size();
}
permissions::AuditOutcome only_completed_outcome(Fixture &fixture) {
  const auto completed =
      fixture.log.query({.plugin = std::nullopt,
                         .event = permissions::AuditEvent::operation_completed,
                         .maximum_results = audit::kHardMaximumRecords});
  OMARCHY_CHECK(completed.status.ok() && completed.records.size() == 1);
  return completed.records.front().outcome;
}
void test_retry_then_commit_once() {
  with_fixture([](Fixture &fixture) {
    fixture.transport->sends = {channel::ChannelSendStatus::would_block,
                                channel::ChannelSendStatus::complete};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    OMARCHY_CHECK(fixture.settlement.dispatch(fixture.message(), deadline) ==
                channel::BrokerSettlementStatus::would_block);
    const auto exact = fixture.transport->bytes;
    OMARCHY_CHECK(fixture.settlement.pending() &&
                fixture.transport->message_type ==
                    broker::kBrokerResultMessage &&
                fixture.transport->correlation == 1 &&
                fixture.transport->bytes == std::vector{std::byte{'{'}, std::byte{'}'}} &&
                fixture.settlement.read_lanes() ==
                    (launcher::EndpointMask::control |
                     launcher::EndpointMask::render) &&
                fixture.settlement.write_lanes() ==
                    launcher::EndpointMask::broker &&
                fixture.settlement.flush(deadline) ==
                    channel::BrokerSettlementStatus::complete &&
                fixture.transport->prepares == 1 &&
                fixture.transport->attempts == 2 &&
                fixture.transport->bytes == exact);
    OMARCHY_CHECK(completed_count(fixture) == 1);
    OMARCHY_CHECK(only_completed_outcome(fixture) ==
                permissions::AuditOutcome::allowed);
    OMARCHY_CHECK(fixture.settlement.flush(deadline) ==
                    channel::BrokerSettlementStatus::complete &&
                fixture.settlement.abort() &&
                fixture.settlement.flush(deadline) ==
                    channel::BrokerSettlementStatus::fatal &&
                fixture.settlement.dispatch(fixture.message(2), deadline) ==
                    channel::BrokerSettlementStatus::fatal &&
                fixture.transport->prepares == 1 &&
                fixture.transport->attempts == 2 &&
                completed_count(fixture) == 1);
  });
}
void test_denied_reply_keeps_request_identity() {
  auto fixture = std::make_unique<Fixture>(permissions::GrantState::denied);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  OMARCHY_CHECK(fixture->settlement.dispatch(fixture->message(29), deadline) ==
              channel::BrokerSettlementStatus::complete &&
              fixture->transport->prepares == 1 &&
              fixture->transport->attempts == 1 &&
              fixture->transport->message_type ==
                  static_cast<std::uint16_t>(
                      wire::CommonMessageType::typed_error) &&
              fixture->transport->correlation == 29);
  broker::BrokerTypedError error{};
  OMARCHY_CHECK(broker::decode_broker_error(fixture->transport->bytes, error));
  OMARCHY_CHECK(error.failed_operation ==
                  broker::kDynamicInvokeMessage &&
              error.reason == broker::BrokerErrorReason::denied &&
              error.decision ==
                  permissions::GrantDecisionCode::explicitly_denied);
}
void test_abort_paths_are_terminal() {
  for (const auto failure : {channel::ChannelSendStatus::peer_closed,
                             channel::ChannelSendStatus::fatal}) {
    with_fixture([failure](Fixture &fixture) {
      fixture.transport->sends = {failure};
      OMARCHY_CHECK(
          fixture.settlement.dispatch(fixture.message(),
                                      std::chrono::steady_clock::now() + 2s) ==
                  channel::BrokerSettlementStatus::fatal &&
              fixture.settlement.failed() &&
              fixture.settlement.read_lanes() == launcher::EndpointMask::none &&
              completed_count(fixture) == 1 && !fixture.settlement.abort() &&
              completed_count(fixture) == 1 &&
              only_completed_outcome(fixture) ==
                  permissions::AuditOutcome::allowed);
    });
  }
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  with_fixture([deadline](Fixture &duplicate) {
    duplicate.transport->sends = {channel::ChannelSendStatus::would_block};
    OMARCHY_CHECK(duplicate.settlement.dispatch(duplicate.message(), deadline) ==
                    channel::BrokerSettlementStatus::would_block &&
                duplicate.settlement.dispatch(duplicate.message(2), deadline) ==
                    channel::BrokerSettlementStatus::fatal &&
                !duplicate.settlement.pending() &&
                duplicate.settlement.flush(deadline) ==
                    channel::BrokerSettlementStatus::fatal &&
                completed_count(duplicate) == 1);
    OMARCHY_CHECK(only_completed_outcome(duplicate) ==
                permissions::AuditOutcome::allowed);
  });

  with_fixture([deadline](Fixture &clean) {
    clean.transport->sends = {channel::ChannelSendStatus::would_block};
    OMARCHY_CHECK(clean.settlement.dispatch(clean.message(), deadline) ==
                    channel::BrokerSettlementStatus::would_block &&
                !clean.settlement.abort() && !clean.settlement.pending() &&
                clean.settlement.flush(deadline) ==
                    channel::BrokerSettlementStatus::fatal &&
                clean.settlement.dispatch(clean.message(2), deadline) ==
                    channel::BrokerSettlementStatus::fatal &&
                completed_count(clean) == 1);
    OMARCHY_CHECK(only_completed_outcome(clean) ==
                permissions::AuditOutcome::allowed);
  });
}
void test_deadline_and_exceptions_fail_closed() {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  with_fixture([&](Fixture &fixture) {
    fixture.transport->sends = {channel::ChannelSendStatus::would_block};
    OMARCHY_CHECK(fixture.settlement.dispatch(fixture.message(), deadline) ==
                    channel::BrokerSettlementStatus::would_block &&
                fixture.settlement.flush(std::chrono::steady_clock::now()) ==
                    channel::BrokerSettlementStatus::fatal &&
                fixture.settlement.failed() &&
                only_completed_outcome(fixture) ==
                    permissions::AuditOutcome::allowed);
  });
  for (const auto &failure : {
           TransportState{.throw_prepare = true},
           TransportState{.reject_prepare = true},
           TransportState{.prepare_sleep_microseconds = 20'000},
           TransportState{.throw_send = true},
           TransportState{.sleep_microseconds = 20'000},
       }) {
    with_fixture([&](Fixture &fixture) {
      *fixture.transport = failure;
      const auto dispatch_deadline =
          failure.prepare_sleep_microseconds || failure.sleep_microseconds
              ? std::chrono::steady_clock::now() + 5ms : deadline;
      OMARCHY_CHECK(fixture.settlement.dispatch(fixture.message(), dispatch_deadline) ==
                      channel::BrokerSettlementStatus::fatal &&
                  fixture.settlement.failed() && !fixture.transport->prepared &&
                  only_completed_outcome(fixture) == permissions::AuditOutcome::allowed);
    });
  }
  with_fixture([&](Fixture &fixture) {
    auto message = fixture.message();
    message.descriptors.emplace_back(open("/dev/null", O_RDONLY | O_CLOEXEC));
    OMARCHY_CHECK(message.descriptors.front() &&
                fixture.settlement.dispatch(std::move(message), deadline) ==
                    channel::BrokerSettlementStatus::fatal &&
                fixture.transport->prepares == 0 &&
                completed_count(fixture) == 0);
  });
}
} // namespace
int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    test_retry_then_commit_once();
    test_denied_reply_keeps_request_identity();
    test_abort_paths_are_terminal();
    test_deadline_and_exceptions_fail_closed();
    return 0;
  });
}
