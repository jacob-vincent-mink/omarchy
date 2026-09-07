#pragma once

#include "dynamic_activation.hpp"
#include "permission_contract.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace omarchy::plugin_runtime::policy {

namespace definitions = omarchy::plugins::definitions;
namespace permissions = omarchy::plugins::permissions;

struct GrantSnapshot {
  permissions::ActivationBinding binding;
  std::vector<definitions::DynamicRevisionGrant> dynamic_grants;
};

} // namespace omarchy::plugin_runtime::policy
