#pragma once

#include "omarchy/plugin/wire/envelope.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace omarchy::plugin::wire {

struct VersionRange {
  std::uint16_t minimum = 0;
  std::uint16_t maximum = 0;
};

struct HelloPayload {
  VersionRange supported;
};

struct WelcomePayload {
  std::uint32_t maximum_payload = 0;
  std::uint32_t maximum_in_flight = 0;
};

enum class NegotiationFailure : std::uint16_t { no_common_role_version = 1 };

struct NegotiationFailedPayload {
  NegotiationFailure reason = NegotiationFailure::no_common_role_version;
  VersionRange trusted_supported;
};

using HelloLayout = omarchy::plugin_runtime::FixedLayout<HelloPayload,
    omarchy::plugin_runtime::MemberPath<&HelloPayload::supported, &VersionRange::minimum>{},
    omarchy::plugin_runtime::MemberPath<&HelloPayload::supported, &VersionRange::maximum>{}>;
using WelcomeLayout = omarchy::plugin_runtime::FixedLayout<WelcomePayload,
    &WelcomePayload::maximum_payload, &WelcomePayload::maximum_in_flight>;
using NegotiationFailedLayout = omarchy::plugin_runtime::FixedLayout<NegotiationFailedPayload,
    &NegotiationFailedPayload::reason,
    omarchy::plugin_runtime::MemberPath<&NegotiationFailedPayload::trusted_supported, &VersionRange::minimum>{},
    omarchy::plugin_runtime::MemberPath<&NegotiationFailedPayload::trusted_supported, &VersionRange::maximum>{}>;
static_assert(HelloLayout::size == 4 && WelcomeLayout::size == 8 &&
              NegotiationFailedLayout::size == 6);

[[nodiscard]] std::array<std::byte, HelloLayout::size>
encode_hello_payload(const HelloPayload &payload);
[[nodiscard]] std::array<std::byte, WelcomeLayout::size>
encode_welcome_payload(const WelcomePayload &payload);
[[nodiscard]] std::array<std::byte, NegotiationFailedLayout::size>
encode_negotiation_failed_payload(const NegotiationFailedPayload &payload);

} // namespace omarchy::plugin::wire
