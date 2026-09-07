#include "omarchy/plugin_runtime/broker/broker_schema.hpp"

#include <array>

namespace omarchy::plugin_runtime::broker {
namespace wire = omarchy::plugin::wire;
namespace {

using wire::CorrelationRule;
using wire::DirectionMask;
using wire::MessageRule;
using wire::MessageSemantic;

constexpr std::array kRules{
    MessageRule{kDynamicInvokeMessage, DirectionMask::worker_to_host,
                CorrelationRule::nonzero, MessageSemantic::request, 80, 49152},
    MessageRule{kBrokerResultMessage, DirectionMask::host_to_worker,
                CorrelationRule::nonzero, MessageSemantic::terminal, 0, 65536},
};

constexpr bool valid_reason(BrokerErrorReason reason) {
  return reason >= BrokerErrorReason::denied &&
         reason <= BrokerErrorReason::cancelled;
}

constexpr bool valid_decision(permissions::GrantDecisionCode decision) {
  return decision >= permissions::GrantDecisionCode::allowed &&
         decision <= permissions::GrantDecisionCode::gesture_missing;
}

} // namespace

const wire::RoleSchemaRegistryView &broker_schema_registry() {
  static const std::array schemas{wire::RoleSchemaView{
          .role = wire::EndpointRole::broker,
          .version = kBrokerRoleVersion,
          .messages = kRules,
          .typed_error_minimum_payload = kBrokerErrorBytes,
          .typed_error_maximum_payload = kBrokerErrorBytes}};
  static const wire::RoleSchemaRegistryView registry(schemas);
  return registry;
}

std::array<std::byte, kBrokerErrorBytes>
encode_broker_error(const BrokerTypedError &error) {
  std::array<std::byte, kBrokerErrorBytes> output{};
  BrokerErrorLayout::encode(error, output);
  return output;
}

bool decode_broker_error(std::span<const std::byte> bytes,
                         BrokerTypedError &output) {
  if (bytes.size() != kBrokerErrorBytes || bytes[5] != std::byte{0} ||
      bytes[6] != std::byte{0} || bytes[7] != std::byte{0}) {
    return false;
  }
  const auto decoded = BrokerErrorLayout::decode(bytes);
  const auto operation = decoded.failed_operation;
  const auto reason = decoded.reason;
  const auto decision = decoded.decision;
  if (operation != kDynamicInvokeMessage || !valid_reason(reason) ||
      !valid_decision(decision) ||
      (reason == BrokerErrorReason::denied) !=
          (decision != permissions::GrantDecisionCode::allowed)) {
    return false;
  }
  output = decoded;
  return true;
}

} // namespace omarchy::plugin_runtime::broker
