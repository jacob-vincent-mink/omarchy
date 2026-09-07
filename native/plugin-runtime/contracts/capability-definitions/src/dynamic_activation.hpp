#pragma once

#include "capability_definition.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace omarchy::plugins::definitions {

inline constexpr std::size_t kMaximumDynamicPayloadBytes = 32768;
inline constexpr std::size_t kMaximumDynamicEnvelopeBytes = 49152;

struct DynamicRevisionGrant {
  permissions::ActivationBinding binding;
  DynamicRequest request;
  DynamicGrant grant;
};

// Structural checks only; callers still bind the scope, definition and identity.
[[nodiscard]] bool valid_dynamic_grant_shape(const DynamicRevisionGrant &revision);

[[nodiscard]] bool
review_dynamic_grant(const TrustedDefinitionRegistry &registry,
                     const DynamicRevisionGrant &revision);
[[nodiscard]] bool encode_dynamic_grant(const DynamicRevisionGrant &revision,
                                        std::span<std::byte> output,
                                        std::size_t &written);
[[nodiscard]] bool decode_dynamic_grant(std::span<const std::byte> input,
                                        DynamicRevisionGrant &output);

struct DynamicInvocation {
  struct GestureClaim {
    std::uint64_t surface_id = 0;
    std::uint64_t surface_generation = 0;
    std::uint64_t input_sequence = 0;
    bool operator==(const GestureClaim &) const = default;
  };
  CapabilityReference definition;
  Name operation;
  std::optional<GestureClaim> gesture;
  std::span<const std::byte> payload;
};

[[nodiscard]] bool
encode_dynamic_invocation(const DynamicInvocation &invocation,
                          std::span<std::byte> output, std::size_t &written);
[[nodiscard]] bool decode_dynamic_invocation(std::span<const std::byte> input,
                                             DynamicInvocation &output);

struct DynamicAuthorizationContext {
  permissions::ActivationBinding binding;
  CapabilityReference definition;
  std::uint64_t grant_epoch = 0;
};

struct AuthorizedDynamicRequest {
  DynamicAuthorizationContext authorization;
  std::string_view operation;
  std::string_view demand_scope;
  std::span<const std::byte> payload;
};

struct DynamicAdapter {
  AdapterBinding binding;
  // Host-created callbacks own their retained provider state. Allocation and
  // copying occur during trusted composition, never from a plugin callback.
  std::function<bool(const AuthorizedDynamicRequest &, std::span<std::byte>,
                     std::size_t &)> dispatch;
  bool invoke(const AuthorizedDynamicRequest &request,
              std::span<std::byte> response, std::size_t &written) const noexcept {
    return dispatch(request, response, written);
  }
};
static_assert(noexcept(std::declval<const DynamicAdapter &>().invoke(
    std::declval<const AuthorizedDynamicRequest &>(),
    std::declval<std::span<std::byte>>(), std::declval<std::size_t &>())));

} // namespace omarchy::plugins::definitions
