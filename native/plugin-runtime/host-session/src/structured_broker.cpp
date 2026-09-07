#include "structured_broker.hpp"
#include "manifest_contract.hpp"

#include "omarchy/plugin_runtime/broker/broker_schema.hpp"

#include <algorithm>
#include <stdexcept>

namespace omarchy::plugin_runtime::host_session {
class BrokerInstanceOrigin final {
  BrokerInstanceOrigin() = default;
  friend class StructuredBroker;
};


BrokerAuthorityStamp::BrokerAuthorityStamp(
    permissions::ActivationBinding binding, std::uint64_t session_nonce,
    std::shared_ptr<const BrokerInstanceOrigin> origin)
    : binding_(std::move(binding)), session_nonce_(session_nonce),
      origin_(std::move(origin)) {}

bool BrokerAuthorityStamp::valid() const noexcept {
  return session_nonce_ != 0 && origin_ != nullptr;
}

bool BrokerAuthorityStamp::exactly_matches(
    const BrokerAuthorityStamp &other) const noexcept {
  return valid() && other.valid() && origin_ == other.origin_ &&
         binding_ == other.binding_ && session_nonce_ == other.session_nonce_;
}

AdmittedBrokerRequest::AdmittedBrokerRequest(
    wire::PacketView packet, const BrokerAuthorityStamp &authority)
    : header_(packet.header), payload_size_(packet.payload.size()),
      authority_(authority), available_(true) {
  if (payload_size_ > payload_.size() || !authority_.valid())
    throw std::invalid_argument("admitted broker request is not bounded");
  std::ranges::copy(packet.payload, payload_.begin());
}

AdmittedBrokerRequest::AdmittedBrokerRequest(
    AdmittedBrokerRequest &&other) noexcept
    : header_(other.header_), payload_size_(other.payload_size_),
      authority_(std::move(other.authority_)), available_(other.available_) {
  std::copy_n(other.payload_.begin(), payload_size_, payload_.begin());
  other.consume();
}

wire::PacketView AdmittedBrokerRequest::packet() const noexcept {
  return {.header = header_,
          .payload = std::span(payload_).first(payload_size_)};
}

void AdmittedBrokerRequest::consume() noexcept {
  header_ = {};
  payload_size_ = 0;
  authority_ = {};
  available_ = false;
}

AuthenticatedBrokerAdmission::AuthenticatedBrokerAdmission(
    const BrokerAuthorityStamp &authority) noexcept
    : authority_(authority) {}

AdmissionResult AuthenticatedBrokerAdmission::admit(
    AuthenticatedBrokerRequestView request) {
  if (!authority_.valid())
    return {.request = std::nullopt,
            .failure = AdmissionFailure::stale_binding};
  if (request.payload.size() > wire::payload_cap(wire::EndpointRole::broker))
    return {.request = std::nullopt,
            .failure = AdmissionFailure::malformed_length};
  if (request.message_type == 0)
    return {.request = std::nullopt,
            .failure = AdmissionFailure::invalid_message_type};
  if (request.correlation_id == 0)
    return {.request = std::nullopt,
            .failure = AdmissionFailure::invalid_correlation};
  if (request.correlation_id <= last_correlation_)
    return {.request = std::nullopt, .failure = AdmissionFailure::replay};
  last_correlation_ = request.correlation_id;
  const wire::PacketView packet{
      .header = {.endpoint_role = wire::EndpointRole::broker,
                 .message_type = request.message_type,
                 .role_protocol_version = broker::kBrokerRoleVersion,
                 .payload_length =
                     static_cast<std::uint32_t>(request.payload.size()),
                 .launch_generation = authority_.binding_.generation,
                 .correlation_id = request.correlation_id},
      .payload = request.payload};
  return {.request = AdmittedBrokerRequest(packet, authority_),
          .failure = AdmissionFailure::none};
}

BrokerTransaction::BrokerTransaction(BrokerTransaction &&other) noexcept
    : state_(other.state_), reply_kind_(other.reply_kind_),
      fatal_(other.fatal_),
      correlation_(other.correlation_), message_type_(other.message_type_),
      payload_size_(other.payload_size_),
      provider_response_bytes_(other.provider_response_bytes_),
      authority_(std::move(other.authority_)), settled_(other.settled_) {
  std::copy_n(other.payload_.begin(), payload_size_, payload_.begin());
  other.consume();
}

void BrokerTransaction::consume() noexcept {
  state_ = TransactionState::fatal;
  reply_kind_ = ReplyKind::provider_failed;
  fatal_ = DispatchFatal::runtime_failed;
  correlation_ = 0;
  message_type_ = 0;
  payload_size_ = 0;
  provider_response_bytes_ = 0;
  authority_ = {};
  settled_ = true;
}

BrokerTransaction
BrokerTransaction::fatal_result(DispatchFatal fatal,
                                std::uint64_t correlation) noexcept {
  BrokerTransaction result;
  result.state_ = TransactionState::fatal;
  result.fatal_ = fatal;
  result.correlation_ = correlation;
  result.message_type_ = 0;
  result.payload_size_ = 0;
  result.provider_response_bytes_ = 0;
  result.settled_ = true;
  return result;
}

BrokerTransaction BrokerTransaction::reply_result(
    ReplyKind kind, std::uint64_t correlation,
    std::uint16_t message_type, std::span<const std::byte> wire_payload,
    std::size_t provider_response_bytes,
    const BrokerAuthorityStamp &authority) {
  if (correlation == 0 || message_type == 0 ||
      wire_payload.size() > kMaximumOwnedBrokerReplyBytes ||
      (kind != ReplyKind::result && provider_response_bytes != 0) ||
      !authority.valid())
    return fatal_result(DispatchFatal::runtime_failed, correlation);
  BrokerTransaction result;
  result.state_ = TransactionState::reply;
  result.reply_kind_ = kind;
  result.fatal_ = DispatchFatal::none;
  result.correlation_ = correlation;
  result.message_type_ = message_type;
  std::ranges::copy(wire_payload, result.payload_.begin());
  result.payload_size_ = wire_payload.size();
  result.provider_response_bytes_ = provider_response_bytes;
  result.authority_ = authority;
  result.settled_ = false;
  return result;
}

StructuredBroker::StructuredBroker(permissions::ActivationBinding binding,
                                   std::uint64_t session_nonce,
                                   const definitions::TrustedDefinitionRegistry &registry,
                                   std::vector<runtime::DynamicRoute> routes,
                                   omarchy::plugins::audit::AuditSink &audit,
                                   DispatchAuthority &authority)
    : authority_stamp_(std::move(binding), session_nonce,
                       std::shared_ptr<const BrokerInstanceOrigin>(
                           new BrokerInstanceOrigin)),
      registry_(registry), routes_(std::move(routes)), audit_(audit),
      authority_(authority) {
  if (!authority_stamp_.valid())
    throw std::invalid_argument("broker admission requires a nonzero session nonce");
  const auto &activation = authority_stamp_.binding_;
  if (activation.plugin.view().empty() || activation.generation == 0 ||
      !definitions::valid_digest(activation.revision) ||
      !definitions::valid_digest(activation.policy_fingerprint))
    throw std::invalid_argument("invalid broker activation binding");
  for (std::size_t index = 0; index < routes_.size(); ++index) {
    const auto &route = routes_[index];
    if (!definitions::review_dynamic_grant(registry_, route.grant) ||
        route.adapter.dispatch == nullptr)
      throw std::runtime_error(
          "dynamic route was not reconstructed from an exact grant");
    if (activation != route.grant.binding)
      throw std::runtime_error("dynamic routes mix activation bindings");
    for (std::size_t previous = 0; previous < index; ++previous)
      if (routes_[previous].grant.request.definition ==
          route.grant.request.definition)
        throw std::runtime_error("duplicate exact dynamic route");
  }
}

AdmissionExtractionResult StructuredBroker::take_admission() {
  if (admission_extracted_.exchange(true))
    return {.admission = std::nullopt,
            .failure = AdmissionExtractionFailure::already_extracted};
  return {.admission = AuthenticatedBrokerAdmission(authority_stamp_),
          .failure = AdmissionExtractionFailure::none};
}

bool StructuredBroker::accepts(const permissions::ActivationBinding &binding,
                               std::uint64_t session_nonce) const noexcept {
  return !failed_.load() && authority_stamp_.valid() &&
         authority_stamp_.binding_ == binding &&
         authority_stamp_.session_nonce_ == session_nonce;
}

bool StructuredBroker::owns(
    const BrokerTransaction &transaction) const noexcept {
  return authority_stamp_.exactly_matches(transaction.authority_);
}

BrokerTransaction
StructuredBroker::dispatch(AdmittedBrokerRequest &&request,
                           std::span<std::byte> provider_response,
                           runtime::GestureEligibilityAuthority *dynamic_gesture) {
  if (!request.available_)
    return BrokerTransaction::fatal_result(DispatchFatal::admission_reused);
  if (!authority_stamp_.exactly_matches(request.authority_))
    return BrokerTransaction::fatal_result(DispatchFatal::identity_mismatch);
  if (dispatching_.exchange(true)) {
    fail_closed();
    return BrokerTransaction::fatal_result(DispatchFatal::runtime_failed);
  }
  struct DispatchGuard {
    std::atomic<bool> &active;
    ~DispatchGuard() { active.store(false); }
  } guard{dispatching_};
  const auto packet = request.packet();
  const auto admitted_authority = request.authority_;
  request.consume();
  if (failed_.load())
    return BrokerTransaction::fatal_result(DispatchFatal::runtime_failed,
                                           packet.header.correlation_id);
  if (!authority_stamp_.exactly_matches(admitted_authority) ||
      packet.header.launch_generation != authority_stamp_.binding_.generation)
    return BrokerTransaction::fatal_result(DispatchFatal::identity_mismatch,
                                           packet.header.correlation_id);

  std::unique_ptr<DispatchAuthorityLease> lease;
  try {
    lease = authority_.acquire(authority_stamp_.binding_,
                               authority_stamp_.session_nonce_, packet);
  } catch (...) {
    fail_closed();
    return BrokerTransaction::fatal_result(DispatchFatal::runtime_failed,
                                           packet.header.correlation_id);
  }
  if (!lease || !lease->current_at_effect())
    return BrokerTransaction::fatal_result(DispatchFatal::authority_stale,
                                           packet.header.correlation_id);
  if (failed_.load())
    return BrokerTransaction::fatal_result(DispatchFatal::runtime_failed,
                                           packet.header.correlation_id);
  try {
    return dispatch_dynamic(packet, provider_response, dynamic_gesture);
  } catch (...) {
    fail_closed();
    return BrokerTransaction::fatal_result(DispatchFatal::runtime_failed,
                                           packet.header.correlation_id);
  }
}

BrokerTransaction
StructuredBroker::dispatch_dynamic(const wire::PacketView &packet,
                                   std::span<std::byte> response,
                                   runtime::GestureEligibilityAuthority *gesture) {
  const auto correlation = packet.header.correlation_id;
  if (packet.header.message_type != broker::kDynamicInvokeMessage ||
      correlation == 0 || correlation <= last_dispatch_correlation_)
    return BrokerTransaction::fatal_result(DispatchFatal::malformed, correlation);
  definitions::DynamicInvocation invocation;
  if (!definitions::decode_dynamic_invocation(packet.payload, invocation))
    return BrokerTransaction::fatal_result(DispatchFatal::malformed, correlation);

  using permissions::AuditEvent;
  using permissions::AuditMetric;
  using permissions::AuditOutcome;
  using permissions::DynamicAuditIdentity;
  const auto &binding = authority_stamp_.binding_;
  const auto attempt_identity = permissions::DynamicAuditAttemptIdentity{
      .opaque_digest = permissions::Digest(
          omarchy::plugins::manifest::sha256_hex(packet.payload))};
  const auto append = [&](AuditEvent event, AuditOutcome outcome,
                          definitions::DynamicDecision decision,
                          const std::optional<DynamicAuditIdentity> &identity,
                          std::size_t response_bytes) {
    permissions::AuditDraft draft{
        .event = event,
        .outcome = outcome,
        .plugin = binding.plugin,
        .revision = binding.revision,
        .generation = binding.generation,
        .correlation = correlation,
        .dynamic_operation = identity,
        .dynamic_attempt =
            identity ? std::nullopt : std::optional(attempt_identity),
        .decision = definitions::audit_decision_code(decision),
        .metadata = {}};
    draft.metadata.push_back(
        {AuditMetric::request_bytes,
         static_cast<std::int64_t>(packet.payload.size())});
    if (response_bytes > 0)
      draft.metadata.push_back({AuditMetric::response_bytes,
                                static_cast<std::int64_t>(response_bytes)});
    return append_audit(std::move(draft));
  };
  const auto route = std::ranges::find_if(routes_, [&](const auto &candidate) {
    return candidate.grant.request.definition == invocation.definition;
  });
  auto decision = definitions::DynamicDecision::unknown_definition;
  bool authorized = false;
  std::optional<DynamicAuditIdentity> identity;
  if (route != routes_.end()) {
    auto authorization = definitions::authorize_dynamic_operation(
        registry_, route->grant.request, route->grant.grant,
        invocation.operation.view(), route->adapter.binding, false);
    if (authorization.decision == definitions::DynamicDecision::gesture_missing &&
        invocation.gesture && gesture != nullptr &&
        gesture->consume(binding, *invocation.gesture).has_value())
      authorization = definitions::authorize_dynamic_operation(
          registry_, route->grant.request, route->grant.grant,
          invocation.operation.view(), route->adapter.binding, true);
    decision = authorization.decision;
    authorized = authorization.allowed();
    const auto resolved = registry_.resolve(route->grant.request.definition);
    if (resolved) {
      const auto &operations = resolved->definition->operations.values();
      const auto operation = std::ranges::find_if(operations, [&](const auto &value) {
        return value.name.view() == invocation.operation.view();
      });
      if (operation != operations.end())
        identity = DynamicAuditIdentity{
            .capability = permissions::CapabilityId(
                route->grant.request.definition.canonical_name.view()),
            .definition_generation =
                route->grant.request.definition.definition_generation,
            .definition_digest = route->grant.request.definition.definition_digest,
            .operation = permissions::BoundedString<128>(operation->name.view()),
            .grant_epoch = route->grant.grant.epoch};
    }
  }
  if (!append(AuditEvent::operation_decided,
              authorized ? AuditOutcome::allowed : AuditOutcome::denied,
              decision, identity, 0))
    return BrokerTransaction::fatal_result(DispatchFatal::runtime_failed, correlation);
  last_dispatch_correlation_ = correlation;
  std::size_t written = 0;
  bool succeeded = false;
  if (authorized) {
    const definitions::AuthorizedDynamicRequest request{
        .authorization = {.binding = binding,
                          .definition = route->grant.request.definition,
                          .grant_epoch = route->grant.grant.epoch},
        .operation = invocation.operation.view(),
        .demand_scope = route->grant.request.scope.view(),
        .payload = invocation.payload};
    succeeded = route->adapter.invoke(request, response, written) &&
                !failed_.load() && written <= response.size();
  }
  const auto completed_bytes = succeeded ? written : 0;
  if (!append(AuditEvent::operation_completed,
              succeeded
                  ? AuditOutcome::allowed
                  : (authorized ? AuditOutcome::failed : AuditOutcome::denied),
              decision, identity, completed_bytes))
    return BrokerTransaction::fatal_result(DispatchFatal::runtime_failed, correlation);
  if (succeeded)
    return BrokerTransaction::reply_result(
        ReplyKind::result, correlation, broker::kBrokerResultMessage,
        std::span<const std::byte>(response).first(written), written,
        authority_stamp_);
  return typed_error_reply(
      packet, authorized ? ReplyKind::provider_failed : ReplyKind::denied,
      definitions::audit_decision_code(decision));
}

bool StructuredBroker::append_audit(permissions::AuditDraft draft) noexcept {
  try {
    if (audit_.append(permissions::AuditProducer::broker, std::move(draft))
            .status.ok() && !failed_.load())
      return true;
  } catch (...) {
  }
  fail_closed();
  return false;
}

bool StructuredBroker::commit_sent(BrokerTransaction &&transaction) {
  if (!owns(transaction) || transaction.state_ != TransactionState::reply ||
      transaction.settled_)
    return false;
  if (failed_.load()) {
    transaction.consume();
    return false;
  }
  transaction.consume();
  return true;
}

bool StructuredBroker::abort_send(BrokerTransaction &&transaction) {
  if (!owns(transaction) || transaction.state_ != TransactionState::reply ||
      transaction.settled_)
    return false;
  if (failed_.load()) {
    transaction.consume();
    return false;
  }
  transaction.consume();
  // A dispatched dynamic reply cannot be canceled independently. Fail-stop
  // the broker so the session owner terminates the channel before more work.
  failed_.store(true);
  return false;
}

void StructuredBroker::fail_closed() noexcept {
  failed_.store(true);
}

BrokerTransaction StructuredBroker::typed_error_reply(
    const wire::PacketView &request,
    ReplyKind kind, permissions::GrantDecisionCode decision) {
  const auto payload = typed_error(request, kind, decision);
  return BrokerTransaction::reply_result(
      kind, request.header.correlation_id,
      static_cast<std::uint16_t>(wire::CommonMessageType::typed_error), payload,
      0, authority_stamp_);
}

std::array<std::byte, broker::kBrokerErrorBytes>
StructuredBroker::typed_error(const wire::PacketView &request, ReplyKind kind,
                              permissions::GrantDecisionCode decision) {
  auto reason = broker::BrokerErrorReason::denied;
  if (kind == ReplyKind::provider_unavailable)
    reason = broker::BrokerErrorReason::provider_unavailable;
  else if (kind == ReplyKind::provider_failed)
    reason = broker::BrokerErrorReason::provider_failed;
  return broker::encode_broker_error(
      {.failed_operation =
           request.header.message_type,
       .reason = reason,
       .decision = decision});
}

} // namespace omarchy::plugin_runtime::host_session
