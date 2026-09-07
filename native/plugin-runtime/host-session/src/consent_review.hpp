#pragma once

#include "authority_store.hpp"

#include <span>
#include <string>
#include <vector>

namespace omarchy::plugin_runtime::host_session {

enum class ConsentDeltaKind : std::uint8_t {
  unchanged,
  scope_changed,
  added,
  removed,
  requirement_changed,
  definition_changed,
  operations_changed,
};

// Owned presentation only. Publisher reason is untrusted explanatory text;
// trusted labels and adapter identity are copied from the definition registry.
struct DynamicReviewRow {
  std::string publisher_reason;
  std::optional<definitions::DynamicRequest> requested;
  std::optional<definitions::DynamicRequest> previous_request;
  std::optional<definitions::DynamicGrant> previous_grant;
  std::optional<definitions::CapabilityDefinition> trusted_definition;
  ConsentDeltaKind delta = ConsentDeltaKind::added;
};

// The fingerprint binds exact candidate identities, authority sequence/new
// generation, and the immutable active snapshot reference. Rows are derived
// presentation; publication rebuilds them from AuthorityStore and the verified
// revision, so callers cannot mutate presentation into authority.
struct ConsentReview {
  VerifiedRevision verified;
  permissions::Digest fingerprint;
  std::uint64_t expected_sequence = 0;
  permissions::ActivationBinding candidate_binding;
  std::vector<DynamicReviewRow> dynamic_rows;
  definitions::TrustTier requested_trust_tier = definitions::TrustTier::sandboxed_plugin;
};

struct ConsentConfirmation {
  permissions::Digest review_fingerprint;
  permissions::Digest decision_fingerprint;
  permissions::DecisionActor actor = permissions::DecisionActor::trusted_ui;
  std::uint64_t confirmed_wall_seconds = 0;
  // Supplied only after a separate trusted-host-extension acknowledgement.
  // Binding to the review prevents reusing it for another revision or epoch.
  std::optional<permissions::Digest> host_extension_acknowledgement = std::nullopt;
};

struct DynamicConsentDecision {
  definitions::CapabilityReference definition;
  permissions::FixedSet<definitions::Name, 16> operations;
  definitions::CanonicalScope decided_scope;
  permissions::UserDecision decision = permissions::UserDecision::deny;
};

enum class ConsentResult : std::uint8_t {
  applied,
  invalid_review,
  incomplete_decisions,
  spoofed_decision,
  required_denied,
  stale_authority,
  authority_error,
  host_extension_confirmation_required,
};

// Trusted-host API only: fingerprints and actor values bind context but do not
// authenticate an IPC peer. The trusted activation owner supplies a
// descriptor-verified revision and choices from trusted UI or interactive CLI;
// plugins never call these functions or provide these structs.
[[nodiscard]] std::optional<ConsentReview> prepare_consent_review(
    AuthorityStore &store, const VerifiedRevision &verified,
    const definitions::TrustedDefinitionRegistry &definitions);

// Trusted-host-only context binding, not a signature or plugin IPC token.
[[nodiscard]] permissions::Digest consent_decision_fingerprint(
    const ConsentReview &review,
    std::span<const DynamicConsentDecision> dynamic_decisions);

[[nodiscard]] ConsentResult publish_consent_review(
    AuthorityStore &store, const ConsentReview &review,
    const ConsentConfirmation &confirmation,
    std::span<const DynamicConsentDecision> dynamic_decisions,
    const definitions::TrustedDefinitionRegistry &definitions);

} // namespace omarchy::plugin_runtime::host_session
