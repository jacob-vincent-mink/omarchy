#include "../../tests/support/test_assert.hpp"
#include "omarchy/plugin_runtime/broker/broker_schema.hpp"
#include <algorithm>

using namespace omarchy::plugin_runtime;

int main() {
  return test_support::test_main([] {
    // Literal wire oracle: invoke, denied, ungranted, three reserved zeros.
    constexpr std::array<unsigned char, 8> golden{0x4f, 0, 0, 1, 3, 0, 0, 0};
    const auto bytes = std::as_bytes(std::span(golden));
    broker::BrokerTypedError decoded;
    OMARCHY_CHECK(broker::decode_broker_error(bytes, decoded));
    OMARCHY_CHECK(decoded.failed_operation == 0x4f00 &&
        decoded.reason == broker::BrokerErrorReason::denied &&
        decoded.decision == broker::permissions::GrantDecisionCode::ungranted);
    OMARCHY_CHECK(std::ranges::equal(broker::encode_broker_error(decoded), bytes));
    for (std::size_t size = 0; size < bytes.size(); ++size)
      OMARCHY_CHECK(!broker::decode_broker_error(bytes.first(size), decoded));
    for (const auto offset : {0, 1, 5, 6, 7}) {
      auto malformed = golden;
      malformed[offset] ^= 1;
      OMARCHY_CHECK(!broker::decode_broker_error(std::as_bytes(std::span(malformed)), decoded));
    }
    // Independent semantic oracle: only reasons 1..5, decisions 0..8;
    // denial requires a nonzero decision, all other reasons require allowed.
    for (unsigned reason = 0; reason <= 255; ++reason) {
      for (unsigned decision = 0; decision <= 255; ++decision) {
        auto candidate = golden;
        candidate[3] = static_cast<unsigned char>(reason);
        candidate[4] = static_cast<unsigned char>(decision);
        const bool expected = reason >= 1 && reason <= 5 && decision <= 8 &&
            ((reason == 1) == (decision != 0));
        OMARCHY_CHECK(broker::decode_broker_error(std::as_bytes(std::span(candidate)), decoded) == expected);
      }
    }
    return 0;
  });
}
