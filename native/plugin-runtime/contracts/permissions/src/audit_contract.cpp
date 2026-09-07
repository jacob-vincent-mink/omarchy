#include "permission_contract.hpp"

#include "omarchy/plugin_runtime/manifest_identifier.hpp"
#include "omarchy/plugin_runtime/canonical_sha256.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace omarchy::plugins::permissions {
using omarchy::plugin_runtime::canonical_manifest_identifier;
using omarchy::plugin_runtime::canonical_sha256;

namespace {
void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

} // namespace

bool valid_audit_producer(AuditProducer producer) {
  return static_cast<std::uint8_t>(producer) <=
         static_cast<std::uint8_t>(AuditProducer::surface_host);
}

void validate_audit_draft(const AuditDraft &draft) {
  require(canonical_manifest_identifier(draft.plugin.view()) &&
              canonical_sha256(draft.revision.view()) && draft.generation > 0,
          "invalid audit identity");
  require(static_cast<std::uint8_t>(draft.event) <=
                  static_cast<std::uint8_t>(AuditEvent::operation_completed) &&
              static_cast<std::uint8_t>(draft.outcome) <=
                  static_cast<std::uint8_t>(AuditOutcome::failed) &&
              static_cast<std::uint8_t>(draft.decision) <=
                  static_cast<std::uint8_t>(GrantDecisionCode::gesture_missing),
          "invalid audit enumeration");
  if (draft.dynamic_operation.has_value()) {
    const auto &dynamic = *draft.dynamic_operation;
    require(canonical_manifest_identifier(dynamic.capability.view()) &&
                dynamic.definition_generation > 0 &&
                canonical_sha256(dynamic.definition_digest.view()) &&
                canonical_manifest_identifier(dynamic.operation.view()) &&
                dynamic.grant_epoch > 0,
            "invalid dynamic audit identity");
  }
  if (draft.dynamic_attempt.has_value()) {
    require(!draft.dynamic_operation.has_value() &&
                canonical_sha256(draft.dynamic_attempt->opaque_digest.view()),
            "invalid dynamic audit attempt identity");
  }
  switch (draft.event) {
  case AuditEvent::operation_decided:
  case AuditEvent::operation_completed:
    require(
        static_cast<unsigned>(draft.dynamic_operation.has_value()) +
                    static_cast<unsigned>(draft.dynamic_attempt.has_value()) ==
                1 &&
            draft.correlation > 0,
        "operation audit event has invalid fields");
    break;
  case AuditEvent::worker_started:
  case AuditEvent::worker_health:
  case AuditEvent::worker_crashed:
  case AuditEvent::worker_stopped:
  case AuditEvent::worker_disabled:
    require(!draft.dynamic_operation.has_value() &&
                !draft.dynamic_attempt.has_value() && draft.correlation == 0,
            "worker audit event has invalid fields");
    break;
  }
  FixedSet<AuditMetric, 8> metrics;
  for (const auto &metadata : draft.metadata.values()) {
    require(
        static_cast<std::uint8_t>(metadata.metric) <=
                static_cast<std::uint8_t>(AuditMetric::retry_after_seconds) &&
            metadata.value >= 0,
        "invalid audit metric");
    require(metrics.insert(metadata.metric), "duplicate audit metric");
  }
}

} // namespace omarchy::plugins::permissions
