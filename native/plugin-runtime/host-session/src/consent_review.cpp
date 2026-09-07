#include "consent_review.hpp"

#include "manifest_contract.hpp"

#include <algorithm>
#include <ranges>
#include <tuple>
#include <utility>

namespace omarchy::plugin_runtime::host_session {
namespace {

void number(std::string &bytes, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    bytes.push_back(static_cast<char>((value >> shift) & 0xff));
}

void field(std::string &bytes, std::string_view value) {
  number(bytes, value.size());
  bytes.append(value);
}

const definitions::DynamicRevisionGrant *
dynamic_for(const std::optional<policy::GrantSnapshot> &snapshot,
            std::string_view name) {
  if (!snapshot)
    return nullptr;
  const auto found =
      std::ranges::find_if(snapshot->dynamic_grants, [&](const auto &grant) {
        return grant.request.definition.canonical_name.view() == name;
      });
  return found == snapshot->dynamic_grants.end() ? nullptr : &*found;
}

ConsentDeltaKind
dynamic_delta(const definitions::DynamicRequest &next,
              const definitions::DynamicRevisionGrant *prior) {
  if (!prior)
    return ConsentDeltaKind::added;
  if (prior->request.definition != next.definition)
    return ConsentDeltaKind::definition_changed;
  if (prior->request.required != next.required)
    return ConsentDeltaKind::requirement_changed;
  if (prior->request.operations != next.operations)
    return ConsentDeltaKind::operations_changed;
  if (prior->request.scope == next.scope)
    return ConsentDeltaKind::unchanged;
  return ConsentDeltaKind::scope_changed;
}

std::string review_fingerprint(const ConsentReview &review,
                               const AuthorityView &view) {
  std::string bytes = "OMARCHY-PLUGIN-CONSENT-REVIEW-V2\0";
  number(bytes, static_cast<std::uint8_t>(review.requested_trust_tier));
  field(bytes, review.candidate_binding.plugin.view());
  field(bytes, review.candidate_binding.revision.view());
  field(bytes, review.verified.request_sha256);
  field(bytes, review.candidate_binding.policy_fingerprint.view());
  number(bytes, review.expected_sequence);
  number(bytes, review.candidate_binding.generation);
  if (view.authority_slots.active) {
    field(bytes, view.authority_slots.active->snapshot_digest.view());
    number(bytes, view.authority_slots.active->generation);
  } else {
    field(bytes, {});
  }
  return plugins::manifest::sha256_hex(bytes);
}

std::optional<ConsentReview>
build_review(const AuthorityView &view, const VerifiedRevision &verified,
             const definitions::TrustedDefinitionRegistry &registry) {
  try {
    if (view.authority_slots.candidate ||
        view.authority_slots.generation_high_watermark == UINT64_MAX ||
        verified.request_sha256 !=
            plugins::manifest::requested_capability_fingerprint(
                verified.manifest.requests))
      return std::nullopt;
    const auto dynamic = definitions::dynamic_requests_from_manifest(verified.manifest, registry);
    if (!dynamic)
      return std::nullopt;
    ConsentReview review{.verified = verified,
                         .fingerprint = {},
                         .expected_sequence = view.authority_slots.sequence,
                         .candidate_binding =
                             {.plugin = permissions::PluginId(
                                  verified.manifest.id),
                              .revision =
                                  permissions::Digest(verified.tree_sha256),
                              .policy_fingerprint =
                                  permissions::Digest(verified.request_sha256),
                              .generation =
                                  view.authority_slots
                                          .generation_high_watermark +
                                      1},
                         .dynamic_rows = {}};

    for (const auto &request : *dynamic) {
      const auto resolved = registry.resolve(request.definition);
      if (!resolved)
        return std::nullopt;
      if (definitions::trust_tier(resolved->definition->enforcement_family) ==
          definitions::TrustTier::trusted_host_extension)
        review.requested_trust_tier = definitions::TrustTier::trusted_host_extension;
      const auto manifest_request = std::ranges::find(
          verified.manifest.requests, request.definition.canonical_name.view(),
          &plugins::manifest::CapabilityRequest::capability);
      const auto *prior =
          dynamic_for(view.active, request.definition.canonical_name.view());
      DynamicReviewRow row{
          .publisher_reason = manifest_request->reason,
          .requested = request,
          .previous_request =
              prior ? std::optional(prior->request) : std::nullopt,
          .previous_grant = prior ? std::optional(prior->grant) : std::nullopt,
          .trusted_definition = *resolved->definition,
          .delta = dynamic_delta(request, prior)};
      review.dynamic_rows.push_back(std::move(row));
    }
    if (view.active) {
      for (const auto &prior : view.active->dynamic_grants) {
        if (std::ranges::any_of(*dynamic, [&](const auto &request) {
              return request.definition.canonical_name ==
                     prior.request.definition.canonical_name;
            }))
          continue;
        const auto resolved = registry.resolve(prior.request.definition);
        DynamicReviewRow row{.publisher_reason = {},
                             .requested = std::nullopt,
                             .previous_request = prior.request,
                             .previous_grant = prior.grant,
                             .trusted_definition =
                                 resolved ? std::optional(*resolved->definition)
                                          : std::nullopt,
                             .delta = ConsentDeltaKind::removed};
        review.dynamic_rows.push_back(std::move(row));
      }
    }
    std::ranges::sort(review.dynamic_rows, {}, [](const auto &row) {
      const auto &request =
          row.requested ? row.requested : row.previous_request;
      return request->definition.canonical_name;
    });
    review.fingerprint =
        permissions::Digest(review_fingerprint(review, view));
    return review;
  } catch (...) {
    return std::nullopt;
  }
}

permissions::Digest
compute_decision_fingerprint(const ConsentReview &review,
                             std::span<const DynamicConsentDecision> dynamic) {
  std::string bytes = "OMARCHY-PLUGIN-CONSENT-CHOICES-V1\0";
  field(bytes, review.fingerprint.view());
  std::vector<const DynamicConsentDecision *> sorted_dynamic;
  for (const auto &choice : dynamic)
    sorted_dynamic.push_back(&choice);
  std::ranges::sort(sorted_dynamic, {}, [](const auto *choice) {
    return std::tuple(choice->definition.canonical_name.view(),
                      choice->definition.definition_generation,
                      choice->definition.definition_digest.view());
  });
  number(bytes, sorted_dynamic.size());
  for (const auto *choice : sorted_dynamic) {
    field(bytes, choice->definition.canonical_name.view());
    number(bytes, choice->definition.definition_generation);
    field(bytes, choice->definition.definition_digest.view());
    field(bytes, choice->decided_scope.view());
    number(bytes, choice->operations.size());
    for (const auto &operation : choice->operations.values())
      field(bytes, operation.view());
    number(bytes, static_cast<std::uint8_t>(choice->decision));
  }
  return permissions::Digest(plugins::manifest::sha256_hex(bytes));
}

bool confirmed(const ConsentReview &review,
               const ConsentConfirmation &confirmation,
               const permissions::Digest &choices) {
  return confirmation.review_fingerprint == review.fingerprint &&
         confirmation.decision_fingerprint == choices &&
         confirmation.confirmed_wall_seconds > 0 &&
         (confirmation.actor == permissions::DecisionActor::trusted_ui ||
          confirmation.actor == permissions::DecisionActor::interactive_cli);
}

} // namespace

std::optional<ConsentReview> prepare_consent_review(
    AuthorityStore &store, const VerifiedRevision &verified,
    const definitions::TrustedDefinitionRegistry &definitions) {
  const auto view = store.read_authority_view();
  return view ? build_review(*view, verified, definitions)
              : std::nullopt;
}

permissions::Digest consent_decision_fingerprint(
    const ConsentReview &review,
    std::span<const DynamicConsentDecision> dynamic_decisions) {
  return compute_decision_fingerprint(review, dynamic_decisions);
}

ConsentResult publish_consent_review(
    AuthorityStore &store, const ConsentReview &review,
    const ConsentConfirmation &confirmation,
    std::span<const DynamicConsentDecision> dynamic_decisions,
    const definitions::TrustedDefinitionRegistry &definitions) {
  try {
    const auto view = store.read_authority_view();
    if (!view)
      return ConsentResult::authority_error;
    if (view->authority_slots.sequence != review.expected_sequence)
      return ConsentResult::stale_authority;
    const auto exact =
        build_review(*view, review.verified, definitions);
    const auto choices = compute_decision_fingerprint(review, dynamic_decisions);
    if (!exact || exact->fingerprint != review.fingerprint ||
        exact->candidate_binding != review.candidate_binding ||
        !confirmed(*exact, confirmation, choices))
      return ConsentResult::invalid_review;
    auto dynamic = definitions::dynamic_requests_from_manifest(review.verified.manifest, definitions);
    if (!dynamic || dynamic_decisions.size() != dynamic->size())
      return ConsentResult::incomplete_decisions;
    const auto binding = review.candidate_binding;
    bool granting_host_extension = false;
    policy::GrantSnapshot snapshot{
        .binding = binding,
        .dynamic_grants = {}};
    for (const auto &request : *dynamic) {
      if (std::ranges::count(dynamic_decisions, request.definition,
                             &DynamicConsentDecision::definition) != 1)
        return ConsentResult::spoofed_decision;
      const auto &choice =
          *std::ranges::find(dynamic_decisions, request.definition,
                             &DynamicConsentDecision::definition);
      if (choice.decision != permissions::UserDecision::grant &&
          choice.decision != permissions::UserDecision::deny)
        return ConsentResult::spoofed_decision;
      if (choice.decided_scope != request.scope ||
          (choice.decision == permissions::UserDecision::deny &&
           choice.operations != request.operations))
        return ConsentResult::spoofed_decision;
      if (choice.decision == permissions::UserDecision::grant &&
          choice.operations.size() == 0)
        return ConsentResult::spoofed_decision;
      definitions::DynamicGrant grant{
          .operations = choice.operations,
          .state = choice.decision == permissions::UserDecision::grant
                       ? permissions::GrantState::granted
                       : permissions::GrantState::denied,
          .epoch = review.candidate_binding.generation};
      if (request.required &&
          choice.decision != permissions::UserDecision::grant)
        return ConsentResult::required_denied;
      definitions::DynamicRevisionGrant revision{
          .binding = binding, .request = request, .grant = std::move(grant)};
      if (!definitions::review_dynamic_grant(definitions, revision))
        return ConsentResult::spoofed_decision;
      const auto resolved = definitions.resolve(request.definition);
      if (!resolved)
        return ConsentResult::spoofed_decision;
      granting_host_extension = granting_host_extension ||
          (choice.decision == permissions::UserDecision::grant &&
           definitions::trust_tier(resolved->definition->enforcement_family) ==
               definitions::TrustTier::trusted_host_extension);
      snapshot.dynamic_grants.push_back(std::move(revision));
    }
    if (granting_host_extension &&
        (!confirmation.host_extension_acknowledgement ||
         *confirmation.host_extension_acknowledgement != exact->fingerprint))
      return ConsentResult::host_extension_confirmation_required;
    std::ranges::sort(snapshot.dynamic_grants, {}, [](const auto &grant) {
      return grant.request.definition.canonical_name;
    });
    const auto result = store.publish_candidate(review.verified, snapshot,
                                                review.expected_sequence,
                                                definitions);
    if (result == AuthorityMutationResult::applied)
      return ConsentResult::applied;
    if (result == AuthorityMutationResult::stale_sequence)
      return ConsentResult::stale_authority;
    return result == AuthorityMutationResult::invalid
               ? ConsentResult::invalid_review
               : ConsentResult::authority_error;
  } catch (...) {
    return ConsentResult::spoofed_decision;
  }
}

} // namespace omarchy::plugin_runtime::host_session
