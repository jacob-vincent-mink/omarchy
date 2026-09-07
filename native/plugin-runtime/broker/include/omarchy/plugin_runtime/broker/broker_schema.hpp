#pragma once

#include "omarchy/plugin/wire/role_registry.hpp"
#include "permission_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace omarchy::plugin_runtime::broker {

namespace permissions = omarchy::plugins::permissions;

inline constexpr std::uint16_t kBrokerRoleVersion = 1;
inline constexpr std::uint16_t kDynamicInvokeMessage = 0x4f00;
inline constexpr std::uint16_t kBrokerResultMessage = 0x5000;

enum class BrokerErrorReason : std::uint16_t {
  denied = 1,
  malformed_request = 2,
  provider_unavailable = 3,
  provider_failed = 4,
  cancelled = 5,
};

struct BrokerTypedError {
  std::uint16_t failed_operation{};
  BrokerErrorReason reason = BrokerErrorReason::denied;
  permissions::GrantDecisionCode decision =
      permissions::GrantDecisionCode::ungranted;
};

using BrokerErrorLayout = FixedLayout<BrokerTypedError,
    &BrokerTypedError::failed_operation, &BrokerTypedError::reason,
    &BrokerTypedError::decision>;
inline constexpr std::size_t kBrokerErrorBytes = BrokerErrorLayout::size + 3;
static_assert(kBrokerErrorBytes == 8);

[[nodiscard]] const omarchy::plugin::wire::RoleSchemaRegistryView &
broker_schema_registry();

[[nodiscard]] std::array<std::byte, kBrokerErrorBytes>
encode_broker_error(const BrokerTypedError &error);
[[nodiscard]] bool decode_broker_error(std::span<const std::byte> bytes,
                                       BrokerTypedError &output);

} // namespace omarchy::plugin_runtime::broker
