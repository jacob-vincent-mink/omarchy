#include "../../../tests/support/test_assert.hpp"

#include "permission_contract.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using omarchy::plugin_runtime::test_support::throws_exception;

using namespace omarchy::plugins::permissions;

using omarchy::plugin_runtime::test_support::require;

void reject(const std::function<void()> &operation, std::string_view message) {
  require(throws_exception<std::runtime_error>(operation), message);
}

Digest digest(char value) { return Digest(std::string(64, value)); }

void bounded_collection_contract() {
  BoundedString<96> reassigned("longer-value");
  reassigned.assign("short");
  OMARCHY_CHECK(reassigned == BoundedString<96>("short"));
  FixedVector<int, 2> values;
  values.push_back(1);
  reject([&] { (void)values[1]; }, "fixed vector exposed unused capacity");
  values.push_back(2);
  reject([&] { values.push_back(3); }, "fixed vector exceeded capacity");
  reject([] { BoundedString<96> invalid(""); }, "empty scope token was accepted");
}

void audit_contract() {
  AuditDraft draft{.event = AuditEvent::operation_decided,
                   .outcome = AuditOutcome::denied,
                   .plugin = PluginId("org.example.timer"),
                   .revision = digest('a'),
                   .generation = 9,
                   .correlation = 42,
                   .dynamic_operation = DynamicAuditIdentity{
                       .capability = CapabilityId("storage.private"),
                       .definition_generation = 1,
                       .definition_digest = digest('b'),
                       .operation = BoundedString<128>("write"),
                       .grant_epoch = 4},
                   .dynamic_attempt = std::nullopt,
                   .decision = GrantDecisionCode::outside_scope,
                   .metadata = {}};
  draft.metadata.push_back(
      {.metric = AuditMetric::request_bytes, .value = 128});
  auto invalid_draft = draft;

  invalid_draft.metadata[0].value = -1;
  reject([&] { validate_audit_draft(invalid_draft); },
         "audit accepted a negative metric");
  invalid_draft = draft;
  invalid_draft.metadata.push_back(
      {.metric = AuditMetric::request_bytes, .value = 1});
  reject([&] { validate_audit_draft(invalid_draft); },
         "audit accepted duplicate metrics");
  OMARCHY_CHECK(!valid_audit_producer(static_cast<AuditProducer>(255)));
  AuditDraft worker{.event = AuditEvent::worker_crashed,
                    .outcome = AuditOutcome::failed,
                    .plugin = PluginId("org.example.timer"),
                    .revision = digest('a'),
                    .generation = 9,
                    .correlation = 0,
                    .dynamic_operation = std::nullopt,
                    .dynamic_attempt = std::nullopt,
                    .decision = GrantDecisionCode::ungranted,
                    .metadata = {}};
  worker.metadata.push_back(
      {.metric = AuditMetric::retry_after_seconds, .value = 2});
  validate_audit_draft(worker);
  AuditDraft dynamic{
      .event = AuditEvent::operation_decided,
      .outcome = AuditOutcome::allowed,
      .plugin = PluginId("org.example.radio"),
      .revision = digest('a'),
      .generation = 9,
      .correlation = 43,
      .dynamic_operation =
          DynamicAuditIdentity{.capability = CapabilityId("network.fetch"),
                               .definition_generation = 1,
                               .definition_digest = digest('b'),
                               .operation = BoundedString<128>("fetch"),
                               .grant_epoch = 4},
      .dynamic_attempt = std::nullopt,
      .decision = GrantDecisionCode::allowed,
      .metadata = {}};
  validate_audit_draft(dynamic);
  auto attempt = dynamic;
  attempt.dynamic_operation = std::nullopt;
  attempt.dynamic_attempt =
      DynamicAuditAttemptIdentity{.opaque_digest = digest('e')};
  validate_audit_draft(attempt);
  auto spoofed_attempt = attempt;
  spoofed_attempt.dynamic_operation = dynamic.dynamic_operation;
  reject([&] { validate_audit_draft(spoofed_attempt); },
         "rejected dynamic attempt accepted a plugin-provided identity");
  auto invalid_attempt = attempt;
  invalid_attempt.dynamic_attempt->opaque_digest = Digest();
  reject([&] { validate_audit_draft(invalid_attempt); },
         "dynamic audit attempt accepted an invalid opaque digest");
  auto zero_epoch = dynamic;
  zero_epoch.dynamic_operation->grant_epoch = 0;
  reject([&] { validate_audit_draft(zero_epoch); },
         "dynamic audit accepted a zero grant epoch");
  auto invalid_worker = worker;
  invalid_worker.correlation = 1;
  reject([&] { validate_audit_draft(invalid_worker); },
         "worker audit accepted a broker correlation");
  invalid_worker = worker;
  invalid_worker.dynamic_operation = dynamic.dynamic_operation;
  reject([&] { validate_audit_draft(invalid_worker); },
         "worker audit accepted an operation authority field");
  for (int index = 0; index < 7; ++index)
    draft.metadata.push_back(
        {.metric = AuditMetric::item_count, .value = index});
  reject(
      [&] {
        draft.metadata.push_back(
            {.metric = AuditMetric::item_count, .value = 9});
      },
      "audit metadata exceeded fixed bound");
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    bounded_collection_contract();
    audit_contract();
    std::cout << "bounded collections and audit contract: PASS\n";
    return 0;
  }, "permission-contract-test: ");
}
