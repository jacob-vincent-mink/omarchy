#include "omarchy/plugin_runtime/provider_frame.hpp"
#include "../../tests/support/test_assert.hpp"
#include <array>

using namespace omarchy::plugin_runtime;

int main() {
  return test_support::test_main([] {
    // Independent golden bytes: OPRV, v1 response, correlation 7, two-byte body.
    constexpr std::array<unsigned char, 22> golden{
      0x4f, 0x50, 0x52, 0x56, 1, 2, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 2, 0, 0x61};
    const auto bytes = std::as_bytes(std::span(golden));
    const auto result = provider_frame::decode_response(bytes, 7);
    OMARCHY_CHECK(result && result->size() == 1 && result->front() == std::byte{0x61});
    OMARCHY_CHECK(!provider_frame::decode_response(bytes, 8));
    OMARCHY_CHECK(!provider_frame::decode(bytes, provider_frame::Type::request));
    for (std::size_t size = 0; size < bytes.size(); ++size)
      OMARCHY_CHECK(!provider_frame::decode_response(bytes.first(size), 7));
    for (const auto offset : {0, 4, 5, 6, 7, 15, 19, 20}) {
      std::vector<std::byte> malformed(bytes.begin(), bytes.end());
      malformed[offset] ^= std::byte{1};
      OMARCHY_CHECK(!provider_frame::decode_response(malformed, 7));
    }
    std::vector<std::byte> encoded(bytes.size());
    encoded[21] = std::byte{0x61};
    OMARCHY_CHECK(provider_frame::encode_header(encoded, provider_frame::Type::response, 7));
    OMARCHY_CHECK(std::ranges::equal(encoded, bytes));
    OMARCHY_CHECK(!provider_frame::encode_header(encoded, provider_frame::Type::response, 0));
    encoded.resize(provider_frame::maximum_frame_bytes + 1);
    OMARCHY_CHECK(!provider_frame::encode_header(encoded, provider_frame::Type::response, 7));
    OMARCHY_CHECK(!provider_frame::decode_response(encoded, 7));
    constexpr std::array<unsigned char, 42> request_golden{
      0x4f, 0x50, 0x52, 0x56, 1, 1, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 22,
      0, 1, 'a', 0, 1, 'b', 0, 0, 0, 1,
      0, 1, 'c', 0, 1, 'd', 0, 0, 0, 2, '{', '}'};
    const auto request_bytes = std::as_bytes(std::span(request_golden));
    const auto request = provider_frame::decode_request(request_bytes);
    OMARCHY_CHECK(request && request->adapter == "a" && request->contract == "b" &&
                  request->abi == 1 && request->operation == "c" && request->scope == "d");
    OMARCHY_CHECK(std::ranges::equal(provider_frame::encode_request(*request), request_bytes));
    OMARCHY_CHECK(!provider_frame::decode_request(request_bytes, 1));
    for (std::size_t size = 0; size < request_bytes.size(); ++size)
      OMARCHY_CHECK(!provider_frame::decode_request(request_bytes.first(size)));
    for (const auto offset : {21, 24, 29, 31, 34, 39}) {
      std::vector<std::byte> malformed(request_bytes.begin(), request_bytes.end());
      malformed[offset] = std::byte{0};
      OMARCHY_CHECK(!provider_frame::decode_request(malformed));
    }
    auto invalid = *request;
    invalid.abi = 2;
    OMARCHY_CHECK(provider_frame::encode_request(invalid).empty());
    invalid = *request;
    invalid.scope = std::string_view("x\0y", 3);
    OMARCHY_CHECK(provider_frame::encode_request(invalid).empty());
    std::string oversized(provider_frame::maximum_scope_bytes + 1, 'x');
    invalid.scope = oversized;
    OMARCHY_CHECK(provider_frame::encode_request(invalid).empty());
    // The full body cap includes text and length prefixes, not just payload.
    invalid = *request;
    std::vector<std::byte> payload(provider_frame::maximum_body_bytes - 20);
    invalid.payload = payload;
    const auto maximum = provider_frame::encode_request(invalid);
    OMARCHY_CHECK(maximum.size() == provider_frame::maximum_frame_bytes);
    OMARCHY_CHECK(provider_frame::decode_request(maximum));
    payload.push_back(std::byte{});
    invalid.payload = payload;
    OMARCHY_CHECK(provider_frame::encode_request(invalid).empty());
    return 0;
  });
}
