#include "../../tests/support/capability_fixture.hpp"
#include "../../tests/support/authority_fixture.hpp"
#include "../../tests/support/test_assert.hpp"

#include "consent_review.hpp"

#include "manifest_contract.hpp"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace host = omarchy::plugin_runtime::host_session;
namespace definitions = omarchy::plugins::definitions;
namespace manifest = omarchy::plugins::manifest;
namespace permissions = omarchy::plugins::permissions;

namespace {
constexpr std::string_view kPlugin = "org.example.consent";

using omarchy::plugin_runtime::test_support::require;

std::string hex(char value) { return std::string(64, value); }

definitions::CapabilityDefinition dynamic_definition(char provider) {
  auto definition = omarchy::plugin_runtime::test_support::reviewed_service_definition(
      "service.demo", "Fetch demo data", "Uses a reviewed adapter", provider);
  definition.operations.insert({.name = definitions::Name("read"),
                                .label = definitions::Label("Read demo data")});
  definition.operations.insert({.name = definitions::Name("write"),
                                .label = definitions::Label("Write demo data"),
                                .mutating = true});
  return definition;
}

struct Fixture : omarchy::plugin_runtime::test_support::AuthorityFixture {
  explicit Fixture(bool open_store = true)
      : AuthorityFixture(kPlugin, open_store) {}
};

manifest::CapabilityRequest builtin_request(bool required, std::string_view category) {
  return omarchy::plugin_runtime::test_support::capability_request(
      omarchy::plugin_runtime::test_support::packaged_registry(), "notifications.send",
      "{\"categories\":[\"" + std::string(category) + "\"]}", required);
}

manifest::CapabilityRequest storage_request(bool required, std::uint64_t quota) {
  return omarchy::plugin_runtime::test_support::capability_request(
      omarchy::plugin_runtime::test_support::packaged_registry(), "storage.private",
      "{\"itemBytes\":4096,\"quotaBytes\":" + std::to_string(quota) + "}", required);
}

manifest::CapabilityRequest
dynamic_request(const Fixture &fixture, bool required,
                std::string scope = "wide",
                std::vector<std::string> operations = {"read"}) {
  const auto resolved = fixture.definitions.find("service.demo");
  OMARCHY_CHECK(resolved.has_value());
  return {.capability = "service.demo",
          .reason = "Read demo status",
          .canonical_scope = std::move(scope),
          .definition_generation = resolved->generation,
          .definition_digest = std::string(resolved->digest.view()),
          .operations = std::move(operations),
          .required = required};
}

host::VerifiedRevision
verified(std::vector<manifest::CapabilityRequest> requests, char revision) {
  manifest::ManifestV2 model;
  model.id = kPlugin;
  model.requests = std::move(requests);
  const auto request_sha256 =
      manifest::requested_capability_fingerprint(model.requests);
  return {.manifest = std::move(model),
          .tree_sha256 = hex(revision),
          .request_sha256 = request_sha256};
}

host::DynamicConsentDecision dynamic_choice(const host::ConsentReview &review,
                                            permissions::UserDecision choice) {
  OMARCHY_CHECK(review.dynamic_rows.size() == 1 &&
              review.dynamic_rows.front().requested);
  const auto &request = *review.dynamic_rows.front().requested;
  return {.definition = request.definition,
          .operations = request.operations,
          .decided_scope = request.scope,
          .decision = choice};
}

host::ConsentConfirmation
confirmation(const host::ConsentReview &review,
             std::span<const host::DynamicConsentDecision> dynamic) {
  return {.review_fingerprint = review.fingerprint,
          .decision_fingerprint =
              host::consent_decision_fingerprint(review, dynamic),
          .actor = permissions::DecisionActor::trusted_ui,
          .confirmed_wall_seconds = 1};
}

host::ConsentResult publish_confirmed(
    Fixture &fixture, const host::ConsentReview &review,
    std::span<const host::DynamicConsentDecision> choices) {
  return host::publish_consent_review(
      *fixture.store, review, confirmation(review, choices), choices,
      fixture.definitions);
}

void grant_and_promote(Fixture &fixture, const host::ConsentReview &review) {
  const std::array choices{dynamic_choice(review, permissions::UserDecision::grant)};
  OMARCHY_CHECK(publish_confirmed(fixture, review, choices) == host::ConsentResult::applied);
  const auto slots = fixture.store->read_slots();
  OMARCHY_CHECK(slots && fixture.store->promote_candidate(
                            review.candidate_binding, slots->sequence) ==
                            host::AuthorityMutationResult::applied);
}

void explicit_install_and_denial_guards() {
  Fixture fixture;
  auto candidate = verified({builtin_request(true, "status")}, 'a');
  auto review = host::prepare_consent_review(
      *fixture.store, candidate, fixture.definitions);
  OMARCHY_CHECK(review && review->dynamic_rows.size() == 1 &&
              review->dynamic_rows[0].delta == host::ConsentDeltaKind::added);

  auto denied = dynamic_choice(*review, permissions::UserDecision::deny);
  const std::array denied_choices{denied};
  auto denied_confirmation = confirmation(*review, denied_choices);
  OMARCHY_CHECK(host::publish_consent_review(
              *fixture.store, *review, denied_confirmation, denied_choices, fixture.definitions) == host::ConsentResult::required_denied);
  OMARCHY_CHECK(fixture.store->read_slots()->sequence == 0 &&
              !fixture.store->read_slots()->candidate);

  auto granted = dynamic_choice(*review, permissions::UserDecision::grant);
  const std::array granted_choices{granted};
  auto granted_confirmation = confirmation(*review, granted_choices);
  auto spoofed_tree = *review;
  spoofed_tree.verified.tree_sha256 = hex('f');
  OMARCHY_CHECK(host::publish_consent_review(
              *fixture.store, spoofed_tree, granted_confirmation,
              granted_choices, fixture.definitions) == host::ConsentResult::invalid_review);
  auto tampered = granted;
  tampered.decision = permissions::UserDecision::deny;
  const std::array tampered_choices{tampered};
  OMARCHY_CHECK(host::publish_consent_review(
              *fixture.store, *review, granted_confirmation, tampered_choices, fixture.definitions) == host::ConsentResult::invalid_review);
  OMARCHY_CHECK(host::publish_consent_review(
              *fixture.store, *review, granted_confirmation, granted_choices, fixture.definitions) == host::ConsentResult::applied);
  OMARCHY_CHECK(!host::prepare_consent_review(*fixture.store, candidate,
                                        fixture.definitions));
}

void optional_partial_spoof_and_invalid_enum() {
  Fixture fixture;
  auto candidate = verified({builtin_request(false, "status")}, 'b');
  auto review = host::prepare_consent_review(
      *fixture.store, candidate, fixture.definitions);
  OMARCHY_CHECK(review.has_value());
  auto denied = dynamic_choice(*review, permissions::UserDecision::deny);
  const std::array choices{denied};
  OMARCHY_CHECK(publish_confirmed(fixture, *review, {}) == host::ConsentResult::incomplete_decisions);
  const std::array duplicates{denied, denied};
  OMARCHY_CHECK(publish_confirmed(fixture, *review, duplicates) == host::ConsentResult::incomplete_decisions);
  auto invalid = denied;
  invalid.decision = static_cast<permissions::UserDecision>(77);
  const std::array invalid_choices{invalid};
  OMARCHY_CHECK(publish_confirmed(fixture, *review, invalid_choices) == host::ConsentResult::spoofed_decision);
  auto expanded = denied;
  expanded.decided_scope = definitions::CanonicalScope("{\"categories\":[\"alerts\",\"status\"]}");
  expanded.decision = permissions::UserDecision::grant;
  const std::array expanded_choices{expanded};
  OMARCHY_CHECK(publish_confirmed(fixture, *review, expanded_choices) == host::ConsentResult::spoofed_decision);
  OMARCHY_CHECK(publish_confirmed(fixture, *review, choices) == host::ConsentResult::applied);
}

void dynamic_exactness_and_provider_identity() {
  Fixture fixture;
  OMARCHY_CHECK(fixture.definitions.install(
              dynamic_definition('d'),
              4));
  auto candidate = verified({dynamic_request(fixture, false)}, 'c');
  auto review = host::prepare_consent_review(
      *fixture.store, candidate, fixture.definitions);
  OMARCHY_CHECK(review && review->dynamic_rows.size() == 1 &&
              review->dynamic_rows[0].trusted_definition &&
              review->dynamic_rows[0]
                      .trusted_definition->adapter.contract_digest ==
                  definitions::Digest(hex('d')));
  for (const auto decision : {permissions::UserDecision::deny, permissions::UserDecision::grant}) {
    for (const auto scope : {"narrow", " narrow", "profile=anything"}) {
      auto changed = dynamic_choice(*review, decision);
      changed.decided_scope = definitions::CanonicalScope(scope);
      const std::array choices{changed};
      OMARCHY_CHECK(publish_confirmed(fixture, *review, choices) == host::ConsentResult::spoofed_decision);
    }
  }
  auto changed_operations =
      dynamic_choice(*review, permissions::UserDecision::deny);
  changed_operations.operations = {};
  const std::array changed_denial{changed_operations};
  OMARCHY_CHECK(publish_confirmed(fixture, *review, changed_denial) == host::ConsentResult::spoofed_decision);
  auto invalid = dynamic_choice(*review, permissions::UserDecision::deny);
  invalid.decision = static_cast<permissions::UserDecision>(88);
  const std::array invalid_choices{invalid};
  OMARCHY_CHECK(publish_confirmed(fixture, *review, invalid_choices) == host::ConsentResult::spoofed_decision);
  grant_and_promote(fixture, *review);
  auto narrowed_update =
      verified({dynamic_request(fixture, false, "narrow")}, '6');
  auto narrowed_review = host::prepare_consent_review(
      *fixture.store, narrowed_update, fixture.definitions);
  OMARCHY_CHECK(narrowed_review && narrowed_review->dynamic_rows.size() == 1 &&
              narrowed_review->dynamic_rows[0].delta ==
                  host::ConsentDeltaKind::scope_changed &&
              narrowed_review->dynamic_rows[0].previous_request->scope ==
                  definitions::CanonicalScope("wide") &&
              narrowed_review->dynamic_rows[0].previous_grant->state ==
                  permissions::GrantState::granted);
}

void dynamic_grants_require_nonempty_selected_operations() {
  for (const bool required : {false, true}) {
    Fixture fixture;
    OMARCHY_CHECK(fixture.definitions.install(
                dynamic_definition('b'),
                4));
    auto candidate = verified(
        {dynamic_request(fixture, required, "wide", {"read", "write"})},
        required ? '2' : '1');
    auto review = host::prepare_consent_review(
        *fixture.store, candidate, fixture.definitions);
    OMARCHY_CHECK(review && review->dynamic_rows.size() == 1);
    auto empty = dynamic_choice(*review, permissions::UserDecision::grant);
    empty.operations = {};
    const std::array empty_choice{empty};
    OMARCHY_CHECK(publish_confirmed(fixture, *review, empty_choice) ==
                host::ConsentResult::spoofed_decision);

    auto partial = dynamic_choice(*review, permissions::UserDecision::grant);
    partial.operations = {};
    partial.operations.insert(definitions::Name("read"));
    const std::array partial_choice{partial};
    require(publish_confirmed(fixture, *review, partial_choice) ==
                host::ConsentResult::applied,
            required ? "nonempty partial required grant was rejected"
                     : "nonempty partial optional grant was rejected");
  }
}

void exact_choice_sets_and_ordering() {
  Fixture fixture;
  auto candidate = verified(
      {storage_request(false, 8192), builtin_request(false, "status")}, '7');
  auto review = host::prepare_consent_review(
      *fixture.store, candidate, fixture.definitions);
  OMARCHY_CHECK(review && review->dynamic_rows.size() == 2);
  std::array<host::DynamicConsentDecision, 2> choices;
  for (std::size_t index = 0; index < choices.size(); ++index) {
    const auto &request = *review->dynamic_rows[index].requested;
    choices[index] = {.definition = request.definition,
                      .operations = request.operations,
                      .decided_scope = request.scope,
                      .decision = permissions::UserDecision::grant};
  }
  auto reversed = choices;
  std::ranges::reverse(reversed);
  OMARCHY_CHECK(host::consent_decision_fingerprint(*review, choices) ==
              host::consent_decision_fingerprint(*review, reversed));
  auto duplicate = choices;
  duplicate[1] = duplicate[0];
  OMARCHY_CHECK(publish_confirmed(fixture, *review, duplicate) == host::ConsentResult::spoofed_decision);
}

void replay_and_parallel_review_cas() {
  Fixture fixture;
  auto candidate = verified({builtin_request(false, "status")}, '8');
  auto first = host::prepare_consent_review(
      *fixture.store, candidate, fixture.definitions);
  auto parallel = host::prepare_consent_review(
      *fixture.store, candidate, fixture.definitions);
  auto choice = dynamic_choice(*first, permissions::UserDecision::grant);
  const std::array choices{choice};
  const auto accepted = confirmation(*first, choices);
  OMARCHY_CHECK(host::publish_consent_review(*fixture.store, *first, accepted,
                                       choices, fixture.definitions) ==
              host::ConsentResult::applied);
  OMARCHY_CHECK(host::publish_consent_review(*fixture.store, *parallel, accepted,
                                       choices, fixture.definitions) ==
              host::ConsentResult::stale_authority);
  auto slots = fixture.store->read_slots();
  const auto binding = first->candidate_binding;
  OMARCHY_CHECK(slots && fixture.store->promote_candidate(binding, slots->sequence) ==
                       host::AuthorityMutationResult::applied);
  auto retried = host::prepare_consent_review(
      *fixture.store, candidate, fixture.definitions);
  OMARCHY_CHECK(retried && retried->candidate_binding.generation == 2 &&
              retried->fingerprint != first->fingerprint);
  OMARCHY_CHECK(host::publish_consent_review(*fixture.store, *retried, accepted,
                                       choices, fixture.definitions) ==
              host::ConsentResult::invalid_review);
}

void prior_builtin_grant_and_corrupt_active_fail_closed() {
  Fixture fixture;
  auto first = verified({storage_request(false, 8192)}, '0');
  auto review = host::prepare_consent_review(
      *fixture.store, first, fixture.definitions);
  grant_and_promote(fixture, *review);
  auto update = verified({storage_request(false, 8192)}, 'a');
  auto update_review = host::prepare_consent_review(
      *fixture.store, update, fixture.definitions);
  OMARCHY_CHECK(update_review && update_review->dynamic_rows.size() == 1 &&
              update_review->dynamic_rows[0].previous_request &&
              update_review->dynamic_rows[0].previous_grant &&
              update_review->dynamic_rows[0].previous_request->scope ==
                  review->dynamic_rows[0].requested->scope &&
              update_review->dynamic_rows[0].previous_grant->state ==
                  permissions::GrantState::granted);

  OMARCHY_CHECK(::fchmodat(fixture.root.get(), "authority.db", 0644, 0) == 0);
  OMARCHY_CHECK(!host::prepare_consent_review(*fixture.store, update,
                                        fixture.definitions));
}

void dynamic_update_requires_fresh_operations_decision() {
  Fixture fixture;
  OMARCHY_CHECK(fixture.definitions.install(
              dynamic_definition('e'),
              5));
  auto first = verified({dynamic_request(fixture, false, "narrow")}, 'f');
  auto first_review = host::prepare_consent_review(
      *fixture.store, first, fixture.definitions);
  grant_and_promote(fixture, *first_review);

  const auto expect_delta = [&](manifest::CapabilityRequest request,
                                char revision,
                                host::ConsentDeltaKind expected) {
    auto candidate = verified({std::move(request)}, revision);
    auto review = host::prepare_consent_review(
        *fixture.store, candidate, fixture.definitions);
    OMARCHY_CHECK(review && review->dynamic_rows.size() == 1 &&
                review->dynamic_rows[0].delta == expected);
  };
  expect_delta(dynamic_request(fixture, false, "narrow"), '1',
               host::ConsentDeltaKind::unchanged);
  expect_delta(dynamic_request(fixture, false, "wide"), '2',
               host::ConsentDeltaKind::scope_changed);
  expect_delta(dynamic_request(fixture, false, "other"), '3',
               host::ConsentDeltaKind::scope_changed);
  expect_delta(dynamic_request(fixture, true, "narrow"), '4',
               host::ConsentDeltaKind::requirement_changed);

  auto expanded = verified(
      {dynamic_request(fixture, false, "wide", {"write", "read"})}, '9');
  auto review = host::prepare_consent_review(
      *fixture.store, expanded, fixture.definitions);
  OMARCHY_CHECK(review && review->dynamic_rows.size() == 1 &&
              review->dynamic_rows[0].delta ==
                  host::ConsentDeltaKind::operations_changed &&
              review->dynamic_rows[0].previous_request &&
              review->dynamic_rows[0].previous_grant &&
              review->dynamic_rows[0].requested->operations.size() == 2 &&
              review->dynamic_rows[0].previous_request->operations.size() == 1);

  fixture.definitions = {};
  OMARCHY_CHECK(fixture.definitions.install(
              dynamic_definition('a'),
              6));
  expect_delta(dynamic_request(fixture, false, "narrow"), '5',
               host::ConsentDeltaKind::definition_changed);
}

void update_diff_reorder_stale_and_zero_permission() {
  Fixture fixture;
  auto first = verified({builtin_request(false, "status")}, 'd');
  auto first_review = host::prepare_consent_review(
      *fixture.store, first, fixture.definitions);
  auto first_choice =
      dynamic_choice(*first_review, permissions::UserDecision::grant);
  const std::array first_choices{first_choice};
  OMARCHY_CHECK(publish_confirmed(fixture, *first_review, first_choices) == host::ConsentResult::applied);
  auto slots = fixture.store->read_slots();
  auto snapshot = fixture.store->read_authority_view();
  OMARCHY_CHECK(slots && snapshot && slots->candidate);
  const auto binding = first_review->candidate_binding;
  OMARCHY_CHECK(fixture.store->promote_candidate(binding, slots->sequence) ==
              host::AuthorityMutationResult::applied);

  auto removal = verified({}, 'e');
  auto stale_review = host::prepare_consent_review(
      *fixture.store, removal, fixture.definitions);
  OMARCHY_CHECK(stale_review && stale_review->dynamic_rows.size() == 1 &&
              !stale_review->dynamic_rows[0].requested &&
              stale_review->dynamic_rows[0].previous_request &&
              stale_review->dynamic_rows[0].previous_grant);
  auto zero_confirmation = confirmation(*stale_review, {});
  OMARCHY_CHECK(host::publish_consent_review(
              *fixture.store, *stale_review, zero_confirmation, {}, fixture.definitions) == host::ConsentResult::applied);
  OMARCHY_CHECK(host::publish_consent_review(
              *fixture.store, *stale_review, zero_confirmation, {}, fixture.definitions) == host::ConsentResult::stale_authority);
}

void host_extension_requires_separate_exact_acknowledgement() {
  Fixture fixture;
  auto broad = dynamic_definition('b');
  broad.enforcement_family = definitions::EnforcementFamily::cli_harness;
  broad.title = definitions::Label("Harmless status reader");
  broad.risk = definitions::RiskLevel::low;
  OMARCHY_CHECK(fixture.definitions.install(broad, 1));
  auto candidate = verified({dynamic_request(fixture, false), builtin_request(false, "status")}, 'b');
  auto review = host::prepare_consent_review(*fixture.store, candidate, fixture.definitions);
  OMARCHY_CHECK(review && review->requested_trust_tier == definitions::TrustTier::trusted_host_extension);
  std::vector<host::DynamicConsentDecision> choices;
  for (const auto &row : review->dynamic_rows) {
    const auto &request = *row.requested;
    choices.push_back({request.definition, request.operations, request.scope, permissions::UserDecision::grant});
  }
  auto approved = confirmation(*review, choices);
  OMARCHY_CHECK(host::publish_consent_review(*fixture.store, *review, approved, choices, fixture.definitions) ==
      host::ConsentResult::host_extension_confirmation_required);
  OMARCHY_CHECK(fixture.store->read_slots()->sequence == 0 && !fixture.store->read_slots()->candidate);
  approved.host_extension_acknowledgement = permissions::Digest(hex('f'));
  OMARCHY_CHECK(host::publish_consent_review(*fixture.store, *review, approved, choices, fixture.definitions) ==
      host::ConsentResult::host_extension_confirmation_required);
  approved.host_extension_acknowledgement = review->fingerprint;
  OMARCHY_CHECK(host::publish_consent_review(*fixture.store, *review, approved, choices, fixture.definitions) ==
      host::ConsentResult::applied);
  auto slots = fixture.store->read_slots();
  OMARCHY_CHECK(fixture.store->promote_candidate(review->candidate_binding, slots->sequence) ==
      host::AuthorityMutationResult::applied);
  candidate.tree_sha256 = hex('c');
  auto updated = host::prepare_consent_review(*fixture.store, candidate, fixture.definitions);
  OMARCHY_CHECK(updated && updated->fingerprint != review->fingerprint);
  auto update_confirmation = confirmation(*updated, choices);
  update_confirmation.host_extension_acknowledgement = review->fingerprint;
  OMARCHY_CHECK(host::publish_consent_review(*fixture.store, *updated, update_confirmation, choices, fixture.definitions) ==
      host::ConsentResult::host_extension_confirmation_required);
  for (auto &choice : choices)
    if (choice.definition.canonical_name.view() == "service.demo")
      choice.decision = permissions::UserDecision::deny;
  OMARCHY_CHECK(publish_confirmed(fixture, *updated, choices) == host::ConsentResult::applied);
  OMARCHY_CHECK(definitions::trust_tier(definitions::EnforcementFamily::network_fetch) ==
      definitions::TrustTier::sandboxed_plugin);
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    explicit_install_and_denial_guards();
    optional_partial_spoof_and_invalid_enum();
    dynamic_exactness_and_provider_identity();
    dynamic_grants_require_nonempty_selected_operations();
    exact_choice_sets_and_ordering();
    replay_and_parallel_review_cas();
    prior_builtin_grant_and_corrupt_active_fail_closed();
    dynamic_update_requires_fresh_operations_decision();
    update_diff_reorder_stale_and_zero_permission();
    host_extension_requires_separate_exact_acknowledgement();
    std::cout << "consent review tests passed\n";
    return 0;
  }, "consent review test failed: ");
}
