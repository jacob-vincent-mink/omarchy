#include "capability_definition.hpp"
#include "omarchy/plugin_runtime/canonical_sha256.hpp"
#include "enforcement_schema.hpp"

#include "manifest_contract.hpp"

#include <algorithm>
#include <string>

namespace omarchy::plugins::definitions {
namespace {

void append(std::string &bytes, std::string_view value) {
  bytes.append(std::to_string(value.size()));
  bytes.push_back(':');
  bytes.append(value);
}

} // namespace

bool canonical_identifier(std::string_view value) {
  if (value.empty() || value.size() > 128)
    return false;
  bool separator = true;
  for (const unsigned char item : value) {
    const bool alphanumeric = (item >= 'a' && item <= 'z') ||
                              (item >= '0' && item <= '9');
    const bool current_separator = item == '.' || item == '-';
    if ((!alphanumeric && !current_separator) ||
        (separator && current_separator))
      return false;
    separator = current_separator;
  }
  return !separator;
}

bool valid_digest(std::string_view digest) {
  return omarchy::plugin_runtime::canonical_sha256(digest);
}

bool valid_digest(const Digest &digest) { return valid_digest(digest.view()); }

bool valid_definition(const CapabilityDefinition &definition) {
  if (!canonical_identifier(definition.canonical_name.view()) ||
      !canonical_identifier(definition.authority_identity.view()) ||
      !canonical_identifier(definition.display_category_id.view()) ||
      definition.display_category_label.size() == 0 ||
      !canonical_identifier(definition.adapter.adapter_class.view()) ||
      !valid_digest(definition.adapter.contract_digest) ||
      definition.adapter.abi_version == 0 || definition.operations.size() == 0 ||
      definition.title.size() == 0 || definition.risk_text.size() == 0)
    return false;
  if (!enforcement_schema(definition.enforcement_family))
    return false;
  if (definition.enforcement_family == EnforcementFamily::external_open_uri &&
      std::any_of(definition.operations.values().begin(),
                  definition.operations.values().end(), [](const auto &op) {
                    return !op.requires_fresh_gesture;
                  }))
    return false;
  return std::all_of(
      definition.operations.values().begin(),
      definition.operations.values().end(), [](const auto &operation) {
        return canonical_identifier(operation.name.view()) &&
               operation.label.size() > 0;
      });
}

Digest semantic_contract_digest(std::string_view descriptor) {
  std::string bytes("OMARCHY-ADAPTER-CONTRACT-V1\0", 28);
  append(bytes, descriptor);
  return Digest(manifest::sha256_hex(bytes));
}

Digest definition_digest(const CapabilityDefinition &definition) {
  const auto *schema = enforcement_schema(definition.enforcement_family);
  if (!schema)
    return {};
  std::string bytes("OMARCHY-CAPABILITY-DEFINITION-V1\0", 33);
  append(bytes, definition.canonical_name.view());
  append(bytes, definition.authority_identity.view());
  bytes.push_back(static_cast<char>(definition.enforcement_family));
  append(bytes, definition.display_category_id.view());
  append(bytes, definition.display_category_label.view());
  bytes.push_back(static_cast<char>(schema->scope_digest_tag));
  append(bytes, definition.title.view());
  append(bytes, definition.risk_text.view());
  bytes.push_back(static_cast<char>(definition.risk));
  bytes.push_back(static_cast<char>(definition.revocation));
  // Mandatory audit policy, retaining its six true bytes in the v1 digest.
  bytes.append(6, char{1});
  append(bytes, definition.adapter.adapter_class.view());
  append(bytes, definition.adapter.contract_digest.view());
  for (int shift = 24; shift >= 0; shift -= 8)
    bytes.push_back(static_cast<char>(definition.adapter.abi_version >> shift));
  for (const auto &operation : definition.operations.values()) {
    append(bytes, operation.name.view());
    append(bytes, operation.label.view());
    bytes.push_back(operation.mutating ? 1 : 0);
    bytes.push_back(operation.requires_fresh_gesture ? 1 : 0);
  }
  return Digest(manifest::sha256_hex(bytes));
}

bool TrustedDefinitionRegistry::install(const CapabilityDefinition &definition,
                                        std::uint32_t generation) {
  if (!valid_definition(definition) || generation == 0 || size_ == entries_.size())
    return false;
  const auto digest = definition_digest(definition);
  for (std::size_t index = 0; index < size_; ++index) {
    const auto &entry = entries_[index];
    if (entry.definition.canonical_name == definition.canonical_name ||
        entry.definition.authority_identity == definition.authority_identity)
      return false;
    if (entry.definition.adapter == definition.adapter &&
        entry.definition.operations == definition.operations)
      return false;
  }
  entries_[size_++] = {.definition = definition,
                       .generation = generation,
                       .digest = digest};
  return true;
}

std::optional<ResolvedDefinition>
TrustedDefinitionRegistry::find(std::string_view canonical) const {
  const auto found = std::find_if(entries_.begin(), entries_.begin() + size_,
                                  [canonical](const auto &entry) {
                                    return entry.definition.canonical_name.view() == canonical;
                                  });
  if (found == entries_.begin() + size_)
    return std::nullopt;
  return ResolvedDefinition{.definition = &found->definition,
                            .generation = found->generation,
                            .digest = found->digest};
}

std::optional<ResolvedDefinition>
TrustedDefinitionRegistry::resolve(const CapabilityReference &reference) const {
  const auto found = find(reference.canonical_name.view());
  if (!found || found->generation != reference.definition_generation ||
      found->digest != reference.definition_digest)
    return std::nullopt;
  return found;
}


permissions::GrantDecisionCode
audit_decision_code(DynamicDecision decision) {
  switch (decision) {
  case DynamicDecision::allowed:
    return permissions::GrantDecisionCode::allowed;
  case DynamicDecision::revoked:
    return permissions::GrantDecisionCode::revoked;
  case DynamicDecision::operation_undeclared:
    return permissions::GrantDecisionCode::capability_undeclared;
  case DynamicDecision::operation_ungranted:
    return permissions::GrantDecisionCode::ungranted;
  case DynamicDecision::gesture_missing:
    return permissions::GrantDecisionCode::gesture_missing;
  case DynamicDecision::denied:
    return permissions::GrantDecisionCode::explicitly_denied;
  case DynamicDecision::unknown_definition:
  case DynamicDecision::stale_definition:
  case DynamicDecision::adapter_mismatch:
    return permissions::GrantDecisionCode::ungranted;
  }
  return permissions::GrantDecisionCode::ungranted;
}

DynamicAuthorization authorize_dynamic_operation(
    const TrustedDefinitionRegistry &registry, const DynamicRequest &request,
    const DynamicGrant &grant, std::string_view operation,
    const AdapterBinding &running_adapter, bool fresh_gesture) {
  const auto by_name = registry.find(request.definition.canonical_name.view());
  if (!by_name)
    return {.decision = DynamicDecision::unknown_definition};
  const auto resolved = registry.resolve(request.definition);
  if (!resolved)
    return {.decision = DynamicDecision::stale_definition};
  if (!std::any_of(request.operations.values().begin(),
                   request.operations.values().end(), [operation](const auto &item) {
                     return item.view() == operation;
                   }))
    return {.decision = DynamicDecision::operation_undeclared,
            .definition = resolved->definition};
  const auto found = std::find_if(
      resolved->definition->operations.values().begin(),
      resolved->definition->operations.values().end(), [operation](const auto &item) {
        return item.name.view() == operation;
      });
  if (found == resolved->definition->operations.values().end())
    return {.decision = DynamicDecision::operation_undeclared,
            .definition = resolved->definition};
  if (grant.state == permissions::GrantState::denied)
    return {.decision = DynamicDecision::denied,
            .definition = resolved->definition, .operation = &*found};
  if (grant.state == permissions::GrantState::revoked || grant.epoch == 0)
    return {.decision = DynamicDecision::revoked,
            .definition = resolved->definition, .operation = &*found};
  if (!std::any_of(grant.operations.values().begin(),
                   grant.operations.values().end(), [operation](const auto &item) {
                     return item.view() == operation;
                   }))
    return {.decision = DynamicDecision::operation_ungranted,
            .definition = resolved->definition, .operation = &*found};
  if (running_adapter != resolved->definition->adapter)
    return {.decision = DynamicDecision::adapter_mismatch,
            .definition = resolved->definition, .operation = &*found};
  if (found->requires_fresh_gesture && !fresh_gesture)
    return {.decision = DynamicDecision::gesture_missing,
            .definition = resolved->definition, .operation = &*found};
  return {.decision = DynamicDecision::allowed,
          .definition = resolved->definition,
          .operation = &*found,
          .grant_epoch = grant.epoch};
}

std::optional<DynamicRequest>
dynamic_request_from_manifest(const manifest::CapabilityRequest &request,
                              const TrustedDefinitionRegistry &registry) {
  try {
    if (request.definition_generation == 0 || request.definition_digest.empty() ||
        request.operations.empty())
      return std::nullopt;
    DynamicRequest result{
        .definition = {.canonical_name = Name(request.capability),
                       .definition_generation = request.definition_generation,
                       .definition_digest = Digest(request.definition_digest)},
        .operations = {},
        .scope = CanonicalScope(request.canonical_scope),
        .required = request.required,
    };
    const auto resolved = registry.resolve(result.definition);
    if (!resolved)
      return std::nullopt;
    for (const auto &operation : request.operations) {
      const Name name(operation);
      if (!std::ranges::any_of(resolved->definition->operations.values(),
                               [&](const auto &defined) {
                                 return defined.name == name;
                               }) ||
          !result.operations.insert(name))
        return std::nullopt;
    }
    return result;
  } catch (const std::runtime_error &) {
    return std::nullopt;
  }
}

std::optional<std::vector<DynamicRequest>>
dynamic_requests_from_manifest(const manifest::ManifestV2 &manifest,
                               const TrustedDefinitionRegistry &registry) {
  std::vector<DynamicRequest> result;
  for (const auto &request : manifest.requests) {
    auto translated = dynamic_request_from_manifest(request, registry);
    if (!translated)
      return std::nullopt;
    result.push_back(std::move(*translated));
  }
  std::ranges::sort(result, {}, [](const auto &request) {
    return request.definition.canonical_name;
  });
  return result;
}

} // namespace omarchy::plugins::definitions
