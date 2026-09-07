#pragma once

#include "test_assert.hpp"
#include "structured_broker.hpp"

namespace omarchy::plugin_runtime::test_support {
using namespace omarchy::plugin_runtime::host_session;

inline auto admit_invocation(AuthenticatedBrokerAdmission &admission,
                              std::uint64_t correlation,
                              std::span<const std::byte> payload) {
  auto result = admission.admit({.message_type = broker::kDynamicInvokeMessage,
                                 .correlation_id = correlation,
                                 .payload = payload});
  OMARCHY_CHECK(static_cast<bool>(result));
  return result;
}

class TestAuthority final : public DispatchAuthority {
public:
  TestAuthority(permissions::ActivationBinding binding,
                std::uint64_t session_nonce)
      : binding_(std::move(binding)), session_nonce_(session_nonce) {}

  class Lease final : public DispatchAuthorityLease {
  public:
    bool current_at_effect() const noexcept override { return true; }
  };

  std::unique_ptr<DispatchAuthorityLease>
  acquire(const permissions::ActivationBinding &binding,
          std::uint64_t session_nonce, const wire::PacketView &) override {
    if (throw_on_acquire_)
      throw std::runtime_error("injected authority failure");
    if (binding != binding_ || session_nonce != session_nonce_)
      return {};
    return std::make_unique<Lease>();
  }

  void throw_on_acquire(bool value) noexcept { throw_on_acquire_ = value; }

private:
  permissions::ActivationBinding binding_;
  std::uint64_t session_nonce_ = 0;
  bool throw_on_acquire_ = false;
};

inline AuthenticatedBrokerAdmission extract_admission(StructuredBroker &broker) {
  auto extracted = broker.take_admission();
  OMARCHY_CHECK(extracted);
  return std::move(*extracted.admission);
}

// Exercises the public authenticated path, including exact authority and
// settlement, without giving tests a second raw-packet dispatch API.
struct AdmittedBrokerFixture {
  static constexpr std::uint64_t nonce = 19;
  omarchy::plugins::audit::BoundedAuditLog audit;
  TestAuthority authority;
  StructuredBroker broker;
  AuthenticatedBrokerAdmission admission;
  std::uint64_t correlation = 0;

  AdmittedBrokerFixture(const permissions::ActivationBinding &binding,
                        const definitions::TrustedDefinitionRegistry &registry,
                        std::vector<runtime::DynamicRoute> routes)
      : authority(binding, nonce),
        broker(binding, nonce, registry, std::move(routes), audit, authority),
        admission(extract_admission(broker)) {}

  BrokerTransaction dispatch(std::span<const std::byte> payload,
                             std::span<std::byte> response,
                             runtime::GestureEligibilityAuthority *gesture = nullptr) {
    auto admitted = admit_invocation(admission, ++correlation, payload);
    return broker.dispatch(std::move(*admitted.request), response, gesture);
  }
};

} // namespace omarchy::plugin_runtime::test_support
