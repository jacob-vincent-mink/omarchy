#include "../../tests/support/test_assert.hpp"

#include "audit_store.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using omarchy::plugin_runtime::test_support::throws_exception;

namespace audit = omarchy::plugins::audit;
namespace permissions = omarchy::plugins::permissions;

using omarchy::plugin_runtime::test_support::require;

permissions::Digest digest(char value) {
  return permissions::Digest(std::string(64, value));
}

permissions::AuditDraft operation(std::uint64_t correlation,
                                  std::string_view plugin = "org.example.one") {
  return {
      .event = permissions::AuditEvent::operation_decided,
      .outcome = permissions::AuditOutcome::allowed,
      .plugin = permissions::PluginId(plugin),
      .revision = digest('a'),
      .generation = 4,
      .correlation = correlation,
      .dynamic_operation = permissions::DynamicAuditIdentity{
          .capability = permissions::CapabilityId("storage.private"),
          .definition_generation = 1,
          .definition_digest = digest('b'),
          .operation = permissions::BoundedString<128>("read"),
          .grant_epoch = 1},
      .dynamic_attempt = std::nullopt,
      .decision = permissions::GrantDecisionCode::allowed,
      .metadata = {},
  };
}

void bounded_append_and_query() {
  audit::BoundedAuditLog log(2);
  const auto first =
      log.append(permissions::AuditProducer::broker, operation(1));
  const auto second =
      log.append(permissions::AuditProducer::broker, operation(2));
  const auto third = log.append(permissions::AuditProducer::broker,
                                operation(3, "org.example.two"));
  OMARCHY_CHECK(first.status.ok() && second.status.ok() && third.status.ok());
  OMARCHY_CHECK(first.record->sequence == 1 && second.record->sequence == 2 &&
              third.record->sequence == 3 &&
              first.record->monotonic_ns < second.record->monotonic_ns &&
              second.record->monotonic_ns < third.record->monotonic_ns);

  const auto retained = log.query();
  OMARCHY_CHECK(retained.status.ok() && retained.records.size() == 2 &&
              retained.records.front().correlation == 2 &&
              retained.records.back().correlation == 3);
  audit::Query filtered;
  filtered.plugin = permissions::PluginId("org.example.two");
  const auto selected = log.query(filtered);
  OMARCHY_CHECK(selected.records.size() == 1 &&
              selected.records.front().correlation == 3);
}

void retains_audit_identity() {
  const auto changed = [](permissions::AuditDraft draft, auto field, auto value) {
    OMARCHY_CHECK(field(draft) != value);
    field(draft) = value;
    audit::BoundedAuditLog log(1);
    const auto appended = log.append(permissions::AuditProducer::broker, draft);
    const auto retained = log.query();
    OMARCHY_CHECK(appended.status.ok() && appended.record &&
                retained.status.ok() && retained.records.size() == 1);
    OMARCHY_CHECK(field(*appended.record) == value &&
                field(retained.records.front()) == value);
  };
  const auto draft = operation(1);
  changed(draft, [](auto &v) -> auto & { return v.dynamic_operation->grant_epoch; },
          std::uint64_t{5});
  changed(draft, [](auto &v) -> auto & { return v.dynamic_operation->capability; },
          permissions::CapabilityId("media.play-stream"));
  changed(draft, [](auto &v) -> auto & { return v.dynamic_operation->definition_generation; },
          std::uint32_t{2});
  changed(draft, [](auto &v) -> auto & { return v.dynamic_operation->definition_digest; },
          digest('c'));
  changed(draft, [](auto &v) -> auto & { return v.dynamic_operation->operation; },
          permissions::BoundedString<128>("other"));
  changed(draft, [](auto &v) -> auto & { return v.generation; }, std::uint64_t{5});
  changed(draft, [](auto &v) -> auto & { return v.revision; }, digest('d'));
  auto attempt = draft;
  attempt.dynamic_operation = std::nullopt;
  attempt.dynamic_attempt = permissions::DynamicAuditAttemptIdentity{digest('e')};
  changed(attempt, [](auto &v) -> auto & { return v.dynamic_attempt->opaque_digest; },
          digest('f'));
}

void rejects_invalid_input() {
  OMARCHY_CHECK(throws_exception<std::invalid_argument>([&] {
    audit::BoundedAuditLog invalid(0);
  }));

  audit::BoundedAuditLog log(1);
  auto invalid = operation(1);
  invalid.correlation = 0;
  OMARCHY_CHECK(!log.append(permissions::AuditProducer::broker, invalid).status.ok());
  OMARCHY_CHECK(
      !log.append(static_cast<permissions::AuditProducer>(255), operation(1))
           .status.ok());
  audit::Query query;
  query.maximum_results = 0;
  OMARCHY_CHECK(!log.query(query).status.ok());
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    bounded_append_and_query();
    retains_audit_identity();
    rejects_invalid_input();
    std::cout << "bounded audit sink: ok\n";
    return 0;
  }, "bounded audit sink: ");
}
