#pragma once

namespace omarchy::plugin_runtime::channel::detail {

// Keep transport, wire and session roles distinct; map their names, not values.
template <typename Target, typename Source>
constexpr Target role_as(Source role, Target fallback = Target::control) noexcept {
  switch (role) {
  case Source::control: return Target::control;
  case Source::broker: return Target::broker;
  case Source::render: return Target::render;
  }
  return fallback;
}

template <typename Role>
constexpr bool valid_role(Role role) noexcept {
  return role == Role::control || role == Role::broker || role == Role::render;
}

} // namespace omarchy::plugin_runtime::channel::detail
