#include "../../tests/support/test_assert.hpp"
#include "../../tests/support/capability_fixture.hpp"
#include "omarchy/plugin_runtime/providers/local_provider.hpp"
#include "../../tests/support/broker_fixture.hpp"
#include "../../tests/support/authenticated_broker_fixture.hpp"

#include "structured_broker.hpp"

#include "audit_store.hpp"
#include "omarchy/plugin_runtime/broker/broker_schema.hpp"

#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace omarchy::plugin_runtime;
using namespace omarchy::plugin_runtime::host_session;
namespace audit = omarchy::plugins::audit;
namespace broker = omarchy::plugin_runtime::broker;
namespace definitions = omarchy::plugins::definitions;
namespace permissions = omarchy::plugins::permissions;
namespace providers = omarchy::plugin_runtime::providers;
namespace runtime = omarchy::plugin_runtime::runtime;
namespace wire = omarchy::plugin::wire;

namespace {

using omarchy::plugin_runtime::test_support::throws_exception;

static_assert(!std::is_copy_constructible_v<AdmittedBrokerRequest>);
static_assert(!std::is_copy_assignable_v<AdmittedBrokerRequest>);
static_assert(std::is_move_constructible_v<AdmittedBrokerRequest>);
static_assert(!std::is_copy_constructible_v<BrokerTransaction>);
static_assert(!std::is_copy_assignable_v<BrokerTransaction>);
static_assert(std::is_move_constructible_v<BrokerTransaction>);
static_assert(!std::is_move_assignable_v<BrokerTransaction>);
static_assert(!std::is_copy_constructible_v<StructuredBroker>);
static_assert(!std::is_copy_assignable_v<StructuredBroker>);
static_assert(!std::is_move_constructible_v<StructuredBroker>);
static_assert(!std::is_move_assignable_v<StructuredBroker>);
static_assert(!std::is_default_constructible_v<AuthenticatedBrokerAdmission>);

using omarchy::plugin_runtime::test_support::require;

using test_support::extract_admission;

permissions::Digest digest(char value) {
  return permissions::Digest(std::string(64, value));
}

permissions::ActivationBinding binding() {
  return {.plugin = permissions::PluginId("fixture.plugin"),
          .revision = digest('a'), .policy_fingerprint = digest('b'), .generation = 7};
}

AuthenticatedBrokerRequestView
request(std::uint16_t type, std::uint64_t correlation,
        std::span<const std::byte> payload) {
  return {.message_type = type,
          .correlation_id = correlation,
          .payload = payload};
}

struct NotificationProbe {
  std::size_t calls = 0;
  std::function<void()> reenter;
};

bool play(std::string_view, std::string_view cue, std::string_view, std::string_view, void *context) noexcept {
  auto &probe = *static_cast<NotificationProbe *>(context);
  ++probe.calls;
  const auto reenter = probe.reenter;
  if (reenter)
    reenter();
  return cue == "complete";
}

struct DynamicProbe {
  std::size_t calls = 0;
  bool oversize = false;
};

bool dynamic_dispatch(const definitions::AuthorizedDynamicRequest &request,
                      std::span<std::byte> response, std::size_t &written,
                      void *context) noexcept {
  auto &probe = *static_cast<DynamicProbe *>(context);
  ++probe.calls;
  if (probe.oversize) {
    written = response.size() + 1;
    return true;
  }
  if (response.size() < request.payload.size())
    return false;
  std::ranges::copy(request.payload, response.begin());
  written = request.payload.size();
  return true;
}

struct DynamicFixture {
  definitions::TrustedDefinitionRegistry registry = test_support::packaged_registry();
  definitions::DynamicRevisionGrant grant;
  runtime::DynamicRoute route;
  DynamicProbe probe;

  explicit DynamicFixture(definitions::RevocationPolicy revocation =
                              definitions::RevocationPolicy::cancel_inflight) {
    definitions::CapabilityDefinition definition{
        .canonical_name = definitions::Name("fixture.echo"),
        .authority_identity = definitions::Name("fixture.echo-v1"),
        .enforcement_family = definitions::EnforcementFamily::network_fetch,
        .display_category_id = definitions::Name("fixture"),
        .display_category_label = definitions::Label("Fixture"),
        .title = definitions::Label("Echo"),
        .risk_text = definitions::Label("Bounded echo"),
        .risk = definitions::RiskLevel::low,
        .revocation = revocation,
        .adapter = {.adapter_class = definitions::Name("fixture-adapter"),
                    .contract_digest = digest('d'),
                    .abi_version = 1},
        .operations = {}};
    OMARCHY_CHECK(definition.operations.insert({.name = definitions::Name("echo"),
                                          .label = definitions::Label("Echo")}));
    OMARCHY_CHECK(registry.install(definition,
                             1));
    const auto resolved = registry.find("fixture.echo");
    OMARCHY_CHECK(resolved.has_value());
    const definitions::CapabilityReference reference{
        .canonical_name = definitions::Name("fixture.echo"),
        .definition_generation = 1,
        .definition_digest = resolved->digest};
    grant = {.binding = binding(),
             .request = {.definition = reference,
                         .operations = {},
                         .scope = definitions::CanonicalScope("exact"),
                         .required = true},
             .grant = {.operations = {},
                       .state = permissions::GrantState::granted,
                       .epoch = 6}};
    OMARCHY_CHECK(grant.request.operations.insert(definitions::Name("echo")) &&
                grant.grant.operations.insert(definitions::Name("echo")));
    route = {.grant = grant,
             .adapter = {.binding = definition.adapter,
                         .dispatch = [this](const auto &request, auto response,
                                            std::size_t &written) noexcept {
                           return dynamic_dispatch(request, response, written, &probe);
                         }}};
  }

  std::vector<std::byte> invocation(std::span<const std::byte> body,
                                    std::string_view operation = "echo") const {
    return test_support::ungestured_invocation(grant.request.definition, operation, body);
  }
};

using test_support::TestAuthority;

class FaultAudit final : public audit::AuditSink {
public:
  explicit FaultAudit(std::size_t throw_on) : throw_on_(throw_on) {}
  bool reject = false;
  std::function<void()> callback;
  std::vector<permissions::AuditDraft> attempts;

  audit::AppendResult append(permissions::AuditProducer producer,
                             permissions::AuditDraft draft) override {
    attempts.push_back(draft);
    if (callback) {
      auto once = std::move(callback);
      callback = {};
      once();
    }
    if (++calls_ == throw_on_) {
      if (reject)
        return {{audit::ErrorCode::invalid_argument, "injected audit rejection"},
                std::nullopt};
      throw std::runtime_error("injected audit failure");
    }
    return backing_.append(producer, std::move(draft));
  }

private:
  std::size_t throw_on_ = 0;
  std::size_t calls_ = 0;
  audit::BoundedAuditLog backing_;
};

template <typename Audit = audit::BoundedAuditLog> struct BasicRuntimeFixture {
  static constexpr std::uint64_t session_nonce = 19;
  Audit audit_log;
  NotificationProbe notification_probe;
  DynamicFixture dynamic_fixture;
  definitions::DynamicRevisionGrant notification_grant =
      test_support::capability_grant(dynamic_fixture.registry,
          test_support::capability_request(dynamic_fixture.registry, "notifications.send",
                                           "{\"categories\":[\"complete\"]}"),
          binding());
  providers::LocalProvider notifications{notification_grant,
      definitions::EnforcementFamily::notifications, -1, play, &notification_probe};
  TestAuthority authority{binding(), session_nonce};
  StructuredBroker broker_mux;
  AuthenticatedBrokerAdmission admission = extract_admission(broker_mux);

  explicit BasicRuntimeFixture(std::size_t audit_argument = 1024)
      : audit_log(audit_argument),
        broker_mux(binding(), session_nonce, dynamic_fixture.registry,
                {dynamic_fixture.route,
                 {.grant = notification_grant,
                  .adapter = {.binding = dynamic_fixture.registry.find("notifications.send")->definition->adapter,
                              .dispatch = [this](const auto &request, auto response,
                                                 std::size_t &written) noexcept {
                                return notifications.dispatch(request, response, written);
                              }}}},
                audit_log, authority) {}

  BrokerTransaction dispatch(AuthenticatedBrokerRequestView value,
                             std::span<std::byte> response) {
    auto admitted = admission.admit(value);
    OMARCHY_CHECK(static_cast<bool>(admitted));
    return broker_mux.dispatch(std::move(*admitted.request), response);
  }
};
using RuntimeFixture = BasicRuntimeFixture<>;
using FaultRuntimeFixture = BasicRuntimeFixture<FaultAudit>;

void test_constructor_validates_dynamic_binding() {
  audit::BoundedAuditLog audit_log;
  DynamicFixture fixture;
  TestAuthority authority(binding(), 41);
  StructuredBroker broker(binding(), 41, fixture.registry, {}, audit_log, authority);
  OMARCHY_CHECK(static_cast<bool>(broker.take_admission()));
  auto foreign = fixture.route;
  ++foreign.grant.binding.generation;
  OMARCHY_CHECK(throws_exception<std::runtime_error>([&] {
    StructuredBroker mismatched(binding(), 41, fixture.registry, {foreign}, audit_log, authority);
  }));
  OMARCHY_CHECK(throws_exception<std::invalid_argument>([&] {
    StructuredBroker unbound(binding(), 0, fixture.registry, {}, audit_log, authority);
  }));
}

void test_admission_is_exact_and_destructive() {
  RuntimeFixture fixture;
  const auto second = fixture.broker_mux.take_admission();
  OMARCHY_CHECK(!second &&
              second.failure == AdmissionExtractionFailure::already_extracted);
  const auto denied_payload =
      omarchy::plugin_runtime::test_support::notification_request("timer");
  const auto value = request(
      broker::kDynamicInvokeMessage,
      1, denied_payload);
  auto admitted = fixture.admission.admit(value);
  OMARCHY_CHECK(static_cast<bool>(admitted));
  const std::array dynamic_body{std::byte{0x23}};
  const auto dynamic_payload = fixture.dynamic_fixture.invocation(dynamic_body);
  OMARCHY_CHECK(
      fixture.admission
              .admit(request(broker::kDynamicInvokeMessage, 1, dynamic_payload))
              .failure == AdmissionFailure::replay);
  OMARCHY_CHECK(fixture.admission.admit(value).failure == AdmissionFailure::replay);
  std::array<std::byte, 64> response{};
  auto first =
      fixture.broker_mux.dispatch(std::move(*admitted.request), response);
  OMARCHY_CHECK(first.state() == TransactionState::reply &&
              first.reply_kind() == ReplyKind::provider_failed);
  auto moved_replay =
      fixture.broker_mux.dispatch(std::move(*admitted.request), response);
  OMARCHY_CHECK(moved_replay.state() == TransactionState::fatal &&
              moved_replay.fatal() == DispatchFatal::admission_reused &&
              moved_replay.provider_response_bytes() == 0);
  OMARCHY_CHECK(!fixture.broker_mux.abort_send(std::move(first)));

  RuntimeFixture foreign;
  auto crossed = omarchy::plugin_runtime::test_support::admit_invocation(
      foreign.admission, 2, denied_payload);
  auto rejected =
      fixture.broker_mux.dispatch(std::move(*crossed.request), response);
  OMARCHY_CHECK(rejected.fatal() == DispatchFatal::identity_mismatch);
  auto accepted =
      foreign.broker_mux.dispatch(std::move(*crossed.request), response);
  OMARCHY_CHECK(accepted.state() == TransactionState::reply &&
              foreign.broker_mux.commit_sent(std::move(accepted)));

}

void test_authenticated_semantics_are_privately_stamped() {
  RuntimeFixture fixture;
  OMARCHY_CHECK(fixture.broker_mux.accepts(binding(), 19) &&
              !fixture.broker_mux.accepts(binding(), 20));
  auto foreign = binding();
  ++foreign.generation;
  OMARCHY_CHECK(!fixture.broker_mux.accepts(foreign, 19));

  const auto payload =
      omarchy::plugin_runtime::test_support::notification_request("timer");
  auto admitted = fixture.admission.admit(
      {.message_type = broker::kDynamicInvokeMessage,
       .correlation_id = 1,
       .payload = payload});
  OMARCHY_CHECK(admitted &&
              fixture.admission
                      .admit(request(broker::kDynamicInvokeMessage, 1, {}))
                      .failure == AdmissionFailure::replay);
  std::array<std::byte, 64> response{};
  auto transaction =
      fixture.broker_mux.dispatch(std::move(*admitted.request), response);
  OMARCHY_CHECK(transaction.state() == TransactionState::reply &&
              transaction.reply_kind() == ReplyKind::provider_failed &&
              !fixture.broker_mux.abort_send(std::move(transaction)));

  RuntimeFixture invalid;
  std::vector<std::byte> oversized(
      wire::payload_cap(wire::EndpointRole::broker) + 1);
  OMARCHY_CHECK(
      invalid.admission
                  .admit(
                      {.message_type = 0, .correlation_id = 1, .payload = {}})
                  .failure == AdmissionFailure::invalid_message_type &&
          invalid.admission
                  .admit(
                      {.message_type = broker::kDynamicInvokeMessage,
                       .correlation_id = 0,
                       .payload = {}})
                  .failure == AdmissionFailure::invalid_correlation &&
          invalid.admission
                  .admit(
                      {.message_type = broker::kDynamicInvokeMessage,
                       .correlation_id = 1,
                       .payload = oversized})
                  .failure == AdmissionFailure::malformed_length);

  RuntimeFixture failed;
  failed.authority.throw_on_acquire(true);
  auto failed_request = failed.admission.admit(
      {.message_type = broker::kDynamicInvokeMessage,
       .correlation_id = 1,
       .payload = {}});
  auto fatal =
      failed.broker_mux.dispatch(std::move(*failed_request.request),
                                 response);
  OMARCHY_CHECK(fatal.state() == TransactionState::fatal &&
              !failed.broker_mux.accepts(binding(),
                                         RuntimeFixture::session_nonce));
}

void test_reply_is_owned_and_committed_only_after_send() {
  RuntimeFixture fixture;
  std::array<std::byte, 64> response{};
  const auto allowed_payload =
      omarchy::plugin_runtime::test_support::notification_request("complete");
  auto transaction =
      fixture.dispatch(request(broker::kDynamicInvokeMessage,
                              1, allowed_payload),
                       response);
  OMARCHY_CHECK(transaction.state() == TransactionState::reply &&
              transaction.reply_kind() == ReplyKind::result &&
              transaction.message_type() == broker::kBrokerResultMessage &&
              transaction.provider_response_bytes() == 2 &&
              !transaction.settled() && fixture.notification_probe.calls == 1);
  response.fill(std::byte{0xff});
  OMARCHY_CHECK(transaction.wire_payload().size() == 2 && transaction.wire_payload()[0] == std::byte{'{'} && transaction.wire_payload()[1] == std::byte{'}'});
  OMARCHY_CHECK(fixture.broker_mux.commit_sent(std::move(transaction)) &&
              transaction.settled() &&
              !fixture.broker_mux.commit_sent(std::move(transaction)));
}

void test_request_move_owns_payload_and_invalidates_source() {
  RuntimeFixture fixture;
  std::array<std::byte, 64> response{};
  auto request_payload =
      omarchy::plugin_runtime::test_support::notification_request("complete");
  auto admitted = omarchy::plugin_runtime::test_support::admit_invocation(
      fixture.admission, 1, request_payload);
  AdmittedBrokerRequest moved(std::move(*admitted.request));
  request_payload.assign(request_payload.size(), std::byte{0xff});
  const auto moved_from =
      fixture.broker_mux.dispatch(std::move(*admitted.request), response);
  OMARCHY_CHECK(moved_from.fatal() == DispatchFatal::admission_reused);
  auto transaction =
      fixture.broker_mux.dispatch(std::move(moved), response);
  OMARCHY_CHECK(transaction.state() == TransactionState::reply &&
              transaction.reply_kind() == ReplyKind::result &&
              fixture.broker_mux.commit_sent(std::move(transaction)));
}

void test_transaction_move_and_foreign_settlement_are_destructive() {
  {
    RuntimeFixture fixture;
    std::array<std::byte, 64> response{};
    const auto payload =
        omarchy::plugin_runtime::test_support::notification_request("complete");
    auto original =
        fixture.dispatch(request(broker::kDynamicInvokeMessage,
                                1, payload),
                         response);
    BrokerTransaction moved(std::move(original));
    OMARCHY_CHECK(original.settled() &&
                !fixture.broker_mux.commit_sent(std::move(original)) &&
                fixture.broker_mux.commit_sent(std::move(moved)));
  }

  const auto payload =
      omarchy::plugin_runtime::test_support::notification_request("complete");
  for (const auto settle : {&StructuredBroker::commit_sent,
                            &StructuredBroker::abort_send}) {
    RuntimeFixture owner;
    RuntimeFixture foreign;
    std::array<std::byte, 64> owner_response{};
    std::array<std::byte, 64> foreign_response{};
    auto transaction =
        owner.dispatch(request(broker::kDynamicInvokeMessage,
                              1, payload),
                       owner_response);
    OMARCHY_CHECK(!(foreign.broker_mux.*settle)(std::move(transaction)) &&
                !transaction.settled() && owner.broker_mux.accepts(binding(), RuntimeFixture::session_nonce) &&
                foreign.broker_mux.accepts(binding(), RuntimeFixture::session_nonce));
    OMARCHY_CHECK(owner.broker_mux.commit_sent(std::move(transaction)));
    auto foreign_transaction =
        foreign.dispatch(request(broker::kDynamicInvokeMessage,
                                1, payload),
                         foreign_response);
    OMARCHY_CHECK(foreign_transaction.state() == TransactionState::reply &&
                foreign.broker_mux.commit_sent(std::move(foreign_transaction)));
  }
}

void test_dynamic_operation_and_oversize_are_bounded() {
  RuntimeFixture fixture;
  const std::array body{std::byte{0x45}};
  auto invocation = fixture.dynamic_fixture.invocation(body);
  std::array<std::byte, 8> response{};
  auto result = fixture.dispatch(
      request(broker::kDynamicInvokeMessage, 1, invocation), response);
  OMARCHY_CHECK(result.state() == TransactionState::reply &&
              result.reply_kind() == ReplyKind::result &&
              result.provider_response_bytes() == 1 &&
              result.wire_payload().size() == 1 &&
              result.wire_payload()[0] == body[0]);
  OMARCHY_CHECK(fixture.broker_mux.commit_sent(std::move(result)));

  fixture.dynamic_fixture.probe.oversize = true;
  invocation = fixture.dynamic_fixture.invocation(body);
  auto oversized_result = fixture.dispatch(
      request(broker::kDynamicInvokeMessage, 2, invocation), response);
  OMARCHY_CHECK(oversized_result.state() == TransactionState::reply &&
              oversized_result.reply_kind() == ReplyKind::provider_failed &&
              oversized_result.provider_response_bytes() == 0 &&
              oversized_result.wire_payload().size() ==
                  broker::kBrokerErrorBytes);
}

void test_dynamic_send_failure_fail_stops_without_later_effect() {
  RuntimeFixture fixture;
  const std::array body{std::byte{0x46}};
  const auto invocation = fixture.dynamic_fixture.invocation(body);
  std::array<std::byte, 8> response{};
  auto transaction = fixture.dispatch(
      request(broker::kDynamicInvokeMessage, 1, invocation), response);
  OMARCHY_CHECK(transaction.state() == TransactionState::reply &&
              fixture.dynamic_fixture.probe.calls == 1 &&
              !fixture.broker_mux.abort_send(std::move(transaction)) &&
              transaction.settled() &&
              !fixture.broker_mux.accepts(binding(),
                                          RuntimeFixture::session_nonce));
  auto after_abort = fixture.admission.admit(
      request(broker::kDynamicInvokeMessage, 2, invocation));
  OMARCHY_CHECK(static_cast<bool>(after_abort));
  const auto failed = fixture.broker_mux.dispatch(
      std::move(*after_abort.request), response);
  OMARCHY_CHECK(failed.fatal() == DispatchFatal::runtime_failed &&
              fixture.dynamic_fixture.probe.calls == 1);
}

void test_runtime_audit_failures_fail_closed() {
  for (const bool reject : {false, true})
    for (std::size_t throw_on : {1, 2}) {
      FaultRuntimeFixture fixture(throw_on);
      fixture.audit_log.reject = reject;
      std::array<std::byte, 64> response{};
      const auto payload = test_support::notification_request("complete");
      auto failed = fixture.dispatch(request(broker::kDynamicInvokeMessage, 1, payload), response);
      OMARCHY_CHECK(failed.fatal() == DispatchFatal::runtime_failed &&
                  fixture.notification_probe.calls == throw_on - 1 &&
                  fixture.audit_log.attempts.size() == throw_on &&
                  fixture.audit_log.attempts.front().event ==
                      permissions::AuditEvent::operation_decided &&
                  !fixture.broker_mux.accepts(binding(), RuntimeFixture::session_nonce));
      auto replay = fixture.dispatch(request(broker::kDynamicInvokeMessage, 2, payload), response);
      OMARCHY_CHECK(replay.fatal() == DispatchFatal::runtime_failed &&
                  fixture.notification_probe.calls == throw_on - 1 &&
                  fixture.audit_log.attempts.size() == throw_on);
    }
}

void test_authority_exception_fails_closed_before_provider_effect() {
  RuntimeFixture fixture;
  fixture.authority.throw_on_acquire(true);
  std::array<std::byte, 64> response{};
  const auto payload =
      omarchy::plugin_runtime::test_support::notification_request("complete");
  const auto failed =
      fixture.dispatch(request(broker::kDynamicInvokeMessage,
                              1, payload),
                       response);
  OMARCHY_CHECK(failed.fatal() == DispatchFatal::runtime_failed &&
              fixture.notification_probe.calls == 0 && !fixture.broker_mux.accepts(binding(), RuntimeFixture::session_nonce));
  fixture.authority.throw_on_acquire(false);
  const auto after_failure =
      fixture.dispatch(request(broker::kDynamicInvokeMessage,
                              2, payload),
                       response);
  OMARCHY_CHECK(after_failure.fatal() == DispatchFatal::runtime_failed &&
              fixture.notification_probe.calls == 0);
}

void test_invalid_authority_is_rejected() {
  DynamicFixture fixture;
  audit::BoundedAuditLog audit;
  TestAuthority authority(binding(), 19);
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto route = fixture.route;
    auto routes = std::vector{route};
    if (mutation == 0) routes.front().grant.grant.epoch = 0;
    if (mutation == 1) routes.push_back(route);
    if (mutation == 2) routes.front().grant.grant.operations.insert(definitions::Name("unreviewed"));
    OMARCHY_CHECK(throws_exception<std::runtime_error>([&] {
      StructuredBroker invalid(binding(), 19, fixture.registry, routes, audit, authority);
    }));
  }
}

void test_provider_reentry_fails_without_deadlock() {
  for (const bool during_audit : {true, false}) {
    FaultRuntimeFixture fixture(0);
    std::array<std::byte, 64> outer_response{};
    std::array<std::byte, 64> inner_response{};
    const auto payload =
        omarchy::plugin_runtime::test_support::notification_request("complete");
    auto outer_admitted = fixture.admission.admit(request(
        broker::kDynamicInvokeMessage, 1,
        payload));
    auto inner_admitted = fixture.admission.admit(request(
        broker::kDynamicInvokeMessage, 2,
        payload));
    OMARCHY_CHECK(static_cast<bool>(outer_admitted) &&
                static_cast<bool>(inner_admitted));
    std::optional<BrokerTransaction> inner_result;
    const auto reenter = [&] {
      inner_result.emplace(fixture.broker_mux.dispatch(
          std::move(*inner_admitted.request), inner_response));
    };
    if (during_audit)
      fixture.audit_log.callback = reenter;
    else
      fixture.notification_probe.reenter = reenter;
    auto outer = fixture.broker_mux.dispatch(std::move(*outer_admitted.request),
                                             outer_response);
    OMARCHY_CHECK(inner_result.has_value() &&
                inner_result->fatal() == DispatchFatal::runtime_failed &&
                outer.fatal() == DispatchFatal::runtime_failed &&
                fixture.notification_probe.calls == (during_audit ? 0 : 1));
    OMARCHY_CHECK(!fixture.broker_mux.commit_sent(std::move(outer)));
  }
}

void test_denials_keep_exact_or_opaque_audit_identity() {
  for (int scenario = 0; scenario < 4; ++scenario) {
    DynamicFixture fixture;
    if (scenario == 3) {
      fixture.route.grant.grant.state = permissions::GrantState::revoked;
      ++fixture.route.grant.grant.epoch;
    }
    test_support::AdmittedBrokerFixture runtime(
        binding(), fixture.registry,
        scenario == 0 ? std::vector<runtime::DynamicRoute>{}
                      : std::vector{fixture.route});
    if (scenario == 1) {
      fixture.grant.request.definition.canonical_name =
          definitions::Name("plugin.supplied-name");
      fixture.grant.request.definition.definition_digest = digest('f');
    }
    const std::array body{std::byte{7}};
    const auto bytes = fixture.invocation(
        body, scenario == 2 ? "plugin.spoofed-operation" : "echo");
    std::array<std::byte, 8> response{};
    const auto expected = scenario < 2
        ? definitions::DynamicDecision::unknown_definition
        : (scenario == 2 ? definitions::DynamicDecision::operation_undeclared
                         : definitions::DynamicDecision::revoked);
    // Retain the repeated-revocation case: denial does not poison a session.
    for (int attempt = 0; attempt < (scenario == 3 ? 67 : 1); ++attempt) {
      auto result = runtime.dispatch(bytes, response);
      broker::BrokerTypedError error;
      OMARCHY_CHECK(result.state() == TransactionState::reply &&
                  result.reply_kind() == ReplyKind::denied &&
                  broker::decode_broker_error(result.wire_payload(), error) &&
                  error.decision == definitions::audit_decision_code(expected) &&
                  runtime.broker.commit_sent(std::move(result)) &&
                  fixture.probe.calls == 0);
      const auto records = runtime.audit.query({}).records;
      OMARCHY_CHECK(records.size() == 2 * static_cast<std::size_t>(attempt + 1));
      const auto &decided = records[records.size() - 2];
      const auto &completed = records.back();
      OMARCHY_CHECK(decided.event == permissions::AuditEvent::operation_decided &&
                  completed.event == permissions::AuditEvent::operation_completed &&
                  decided.decision == error.decision && completed.decision == error.decision &&
                  decided.dynamic_operation.has_value() == (scenario == 3) &&
                  completed.dynamic_operation.has_value() == (scenario == 3) &&
                  decided.dynamic_attempt.has_value() == (scenario != 3) &&
                  completed.dynamic_attempt.has_value() == (scenario != 3));
    }
  }
}

void test_dispatch_order_and_malformed_payloads() {
  RuntimeFixture fixture;
  const std::array body{std::byte{7}};
  const auto payload = fixture.dynamic_fixture.invocation(body);
  std::array<std::byte, 8> response{};
  for (std::size_t size = 0; size < payload.size(); ++size) {
    auto result = fixture.dispatch(
        request(broker::kDynamicInvokeMessage, size + 1,
                std::span(payload).first(size)), response);
    OMARCHY_CHECK(result.fatal() == DispatchFatal::malformed &&
                fixture.dynamic_fixture.probe.calls == 0 &&
                fixture.audit_log.query({}).records.empty());
  }
  auto unknown = fixture.dispatch(request(0xffff, payload.size() + 1, payload), response);
  OMARCHY_CHECK(unknown.fatal() == DispatchFatal::malformed &&
              fixture.audit_log.query({}).records.empty());
  auto first = fixture.admission.admit(
      request(broker::kDynamicInvokeMessage, payload.size() + 2, payload));
  auto second = fixture.admission.admit(
      request(broker::kDynamicInvokeMessage, payload.size() + 3, payload));
  OMARCHY_CHECK(first && second);
  auto accepted = fixture.broker_mux.dispatch(std::move(*second.request), response);
  OMARCHY_CHECK(accepted.reply_kind() == ReplyKind::result &&
              fixture.broker_mux.commit_sent(std::move(accepted)) &&
              response[0] == body[0]);
  auto backwards = fixture.broker_mux.dispatch(std::move(*first.request), response);
  const auto records = fixture.audit_log.query({}).records;
  OMARCHY_CHECK(backwards.fatal() == DispatchFatal::malformed &&
              fixture.dynamic_fixture.probe.calls == 1 && records.size() == 2 &&
              records[0].event == permissions::AuditEvent::operation_decided &&
              records[1].event == permissions::AuditEvent::operation_completed);
}

} // namespace

void dynamic_activation_tests();

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    dynamic_activation_tests();
    test_constructor_validates_dynamic_binding();
    test_admission_is_exact_and_destructive();
    test_authenticated_semantics_are_privately_stamped();
    test_reply_is_owned_and_committed_only_after_send();
    test_request_move_owns_payload_and_invalidates_source();
    test_transaction_move_and_foreign_settlement_are_destructive();
    test_dynamic_operation_and_oversize_are_bounded();
    test_dynamic_send_failure_fail_stops_without_later_effect();
    test_runtime_audit_failures_fail_closed();
    test_authority_exception_fails_closed_before_provider_effect();
    test_invalid_authority_is_rejected();
    test_provider_reentry_fails_without_deadlock();
    test_denials_keep_exact_or_opaque_audit_identity();
    test_dispatch_order_and_malformed_payloads();
    std::cout << "structured broker composition tests passed\n";
    return EXIT_SUCCESS;
  }, "structured broker composition test failed: ");
}
