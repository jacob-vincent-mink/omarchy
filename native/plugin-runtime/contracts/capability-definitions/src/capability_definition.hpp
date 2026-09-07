#pragma once

#include "manifest_contract.hpp"
#include "permission_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

namespace omarchy::plugins::definitions {

using Name = permissions::BoundedString<128>;
using Label = permissions::BoundedString<160>;
using Digest = permissions::Digest;
using CanonicalScope = permissions::BoundedString<4096>;

enum class EnforcementFamily : std::uint8_t {
  network_fetch,
  external_open_uri,
  system_observe,
  device_observe,
  device_control,
  media_play_stream,
  cli_harness = 8,
  private_storage,
  notifications,
};
enum class RiskLevel : std::uint8_t { low, moderate, high, critical };

enum class TrustTier : std::uint8_t { sandboxed_plugin, trusted_host_extension };

// This is enforcement meaning, not publisher-controlled presentation. Even
// exact argv rules invoke host executables outside the worker sandbox; command
// names and friendly risk labels cannot turn that into a bounded provider.
[[nodiscard]] constexpr TrustTier trust_tier(EnforcementFamily family) {
  switch (family) {
  case EnforcementFamily::network_fetch:
  case EnforcementFamily::external_open_uri:
  case EnforcementFamily::system_observe:
  case EnforcementFamily::device_observe:
  case EnforcementFamily::device_control:
  case EnforcementFamily::media_play_stream:
  case EnforcementFamily::private_storage:
  case EnforcementFamily::notifications:
    return TrustTier::sandboxed_plugin;
  case EnforcementFamily::cli_harness:
    return TrustTier::trusted_host_extension;
  }
  return TrustTier::trusted_host_extension;
}

[[nodiscard]] constexpr std::string_view trust_tier_name(TrustTier tier) {
  return tier == TrustTier::sandboxed_plugin ? "sandboxed-plugin" : "trusted-host-extension";
}
enum class RevocationPolicy : std::uint8_t {
  deny_new,
  cancel_inflight,
  restart_worker,
};

struct OperationDefinition {
  Name name;
  Label label;
  bool mutating = false;
  bool requires_fresh_gesture = false;
  auto operator<=>(const OperationDefinition &) const = default;
};

struct AdapterBinding {
  Name adapter_class;
  // Identifies the provider protocol and its bounded semantics. Executable
  // identity belongs to the trusted provider profile, not this definition.
  Digest contract_digest;
  std::uint32_t abi_version = 0;
  bool operator==(const AdapterBinding &) const = default;
};

struct CapabilityDefinition {
  Name canonical_name;
  Name authority_identity;
  EnforcementFamily enforcement_family = EnforcementFamily::network_fetch;
  Name display_category_id;
  Label display_category_label;
  Label title;
  Label risk_text;
  RiskLevel risk = RiskLevel::critical;
  RevocationPolicy revocation = RevocationPolicy::deny_new;
  AdapterBinding adapter;
  permissions::FixedSet<OperationDefinition, 16> operations;
  bool operator==(const CapabilityDefinition &) const = default;
};

// Single source for the package generator and test fixture definitions.
std::vector<CapabilityDefinition> packaged_definitions();

struct CapabilityReference {
  Name canonical_name;
  std::uint32_t definition_generation = 0;
  Digest definition_digest;
  bool operator==(const CapabilityReference &) const = default;
};

struct ResolvedDefinition {
  const CapabilityDefinition *definition = nullptr;
  std::uint32_t generation = 0;
  Digest digest;
};

class TrustedDefinitionRegistry {
public:
  bool install(const CapabilityDefinition &definition,
               std::uint32_t generation);
  [[nodiscard]] std::optional<ResolvedDefinition>
  resolve(const CapabilityReference &reference) const;
  [[nodiscard]] std::optional<ResolvedDefinition>
  find(std::string_view canonical_name) const;
  [[nodiscard]] std::size_t size() const { return size_; }

private:
  struct Entry {
    CapabilityDefinition definition;
    std::uint32_t generation = 0;
    Digest digest;
  };
  std::array<Entry, 128> entries_{};
  std::size_t size_ = 0;
};

[[nodiscard]] bool valid_definition(const CapabilityDefinition &definition);
[[nodiscard]] bool canonical_identifier(std::string_view value);
[[nodiscard]] bool valid_digest(std::string_view digest);
[[nodiscard]] bool valid_digest(const Digest &digest);
[[nodiscard]] Digest semantic_contract_digest(std::string_view descriptor);
[[nodiscard]] Digest definition_digest(const CapabilityDefinition &definition);

struct DynamicRequest {
  CapabilityReference definition;
  permissions::FixedSet<Name, 16> operations;
  CanonicalScope scope;
  bool required = false;
  bool operator==(const DynamicRequest &) const = default;
};

struct DynamicGrant {
  permissions::FixedSet<Name, 16> operations;
  permissions::GrantState state = permissions::GrantState::denied;
  std::uint64_t epoch = 0;
};

enum class DynamicDecision : std::uint8_t {
  allowed,
  unknown_definition,
  stale_definition,
  operation_undeclared,
  operation_ungranted,
  denied,
  revoked,
  adapter_mismatch,
  gesture_missing,
};

[[nodiscard]] permissions::GrantDecisionCode audit_decision_code(DynamicDecision decision);

struct DynamicAuthorization {
  DynamicDecision decision = DynamicDecision::denied;
  const CapabilityDefinition *definition = nullptr;
  const OperationDefinition *operation = nullptr;
  std::uint64_t grant_epoch = 0;
  [[nodiscard]] bool allowed() const {
    return decision == DynamicDecision::allowed;
  }
};

[[nodiscard]] DynamicAuthorization authorize_dynamic_operation(
    const TrustedDefinitionRegistry &registry, const DynamicRequest &request,
    const DynamicGrant &grant, std::string_view operation,
    const AdapterBinding &running_adapter, bool fresh_gesture);

[[nodiscard]] std::optional<DynamicRequest>
dynamic_request_from_manifest(const manifest::CapabilityRequest &request,
                              const TrustedDefinitionRegistry &registry);
[[nodiscard]] std::optional<std::vector<DynamicRequest>>
dynamic_requests_from_manifest(const manifest::ManifestV2 &manifest,
                               const TrustedDefinitionRegistry &registry);

} // namespace omarchy::plugins::definitions
