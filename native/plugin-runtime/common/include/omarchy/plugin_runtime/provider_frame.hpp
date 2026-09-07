#pragma once

#include "byte_reader.hpp"
#include <optional>

namespace omarchy::plugin_runtime::provider_frame {

// Descriptor-free provider transport, distinct from the worker's role envelope.
inline constexpr std::uint32_t magic = 0x4f505256;
inline constexpr std::uint8_t version = 1;
inline constexpr std::size_t header_bytes = 20;
inline constexpr std::size_t maximum_body_bytes = 64 * 1024;
inline constexpr std::size_t maximum_frame_bytes = header_bytes + maximum_body_bytes;
enum class Type : std::uint8_t { request = 1, response = 2 };
inline constexpr std::uint32_t adapter_abi = 1;
inline constexpr std::size_t maximum_adapter_bytes = 128;
inline constexpr std::size_t maximum_contract_bytes = 64;
inline constexpr std::size_t maximum_operation_bytes = 128;
inline constexpr std::size_t maximum_scope_bytes = 4096;

// Borrowed views: valid only while the backing datagram remains alive.
struct Request {
  std::uint64_t correlation;
  std::string_view adapter, contract;
  std::uint32_t abi;
  std::string_view operation, scope;
  std::span<const std::byte> payload;
};

struct Frame {
  std::uint64_t correlation;
  std::span<const std::byte> body;
};

// This validates structure only. The caller owns expected correlation,
// authorization, deadlines, descriptor rejection, and payload semantics.
inline std::optional<Frame> decode(std::span<const std::byte> bytes, Type type) {
  ByteReader reader{bytes};
  std::uint32_t signature, size;
  std::uint8_t revision, kind;
  std::uint16_t reserved;
  Frame frame{};
  if (bytes.size() > maximum_frame_bytes || !reader.integer(signature) ||
      signature != magic || !reader.integer(revision) || revision != version ||
      !reader.integer(kind) || kind != static_cast<std::uint8_t>(type) ||
      !reader.integer(reserved) || reserved != 0 ||
      !reader.integer(frame.correlation) || frame.correlation == 0 ||
      !reader.integer(size) || size != bytes.size() - reader.offset ||
      !reader.raw(size, frame.body))
    return std::nullopt;
  return frame;
}

inline bool encode_header(std::span<std::byte> bytes, Type type,
                          std::uint64_t correlation) {
  if (bytes.size() < header_bytes || bytes.size() > maximum_frame_bytes ||
      correlation == 0 || (type != Type::request && type != Type::response))
    return false;
  big_endian::put(bytes, 0, magic);
  big_endian::put(bytes, 4, version);
  big_endian::put(bytes, 5, static_cast<std::uint8_t>(type));
  big_endian::put(bytes, 6, std::uint16_t{0});
  big_endian::put(bytes, 8, correlation);
  big_endian::put(bytes, 16, static_cast<std::uint32_t>(bytes.size() - header_bytes));
  return true;
}

inline std::optional<std::span<const std::byte>>
decode_response(std::span<const std::byte> bytes, std::uint64_t correlation) {
  const auto frame = decode(bytes, Type::response);
  if (!frame || frame->correlation != correlation || frame->body.empty() ||
      frame->body.front() != std::byte{0})
    return std::nullopt;
  return frame->body.subspan(1);
}

inline std::optional<Request> decode_request(
    std::span<const std::byte> bytes,
    std::size_t maximum_payload = maximum_body_bytes) {
  const auto frame = decode(bytes, Type::request);
  if (!frame)
    return std::nullopt;
  ByteReader reader{frame->body};
  Request request{};
  request.correlation = frame->correlation;
  std::uint32_t size;
  if (!reader.text(request.adapter, maximum_adapter_bytes) ||
      !reader.text(request.contract, maximum_contract_bytes) ||
      !reader.integer(request.abi) || request.abi != adapter_abi ||
      !reader.text(request.operation, maximum_operation_bytes) ||
      !reader.text(request.scope, maximum_scope_bytes) ||
      !reader.integer(size) || size > maximum_payload ||
      size != frame->body.size() - reader.offset || !reader.raw(size, request.payload))
    return std::nullopt;
  return request;
}

inline std::vector<std::byte> encode_request(const Request &request) {
  const auto valid_text = [](std::string_view text, std::size_t maximum) {
    return !text.empty() && text.size() <= maximum &&
           text.find('\0') == std::string_view::npos;
  };
  if (request.correlation == 0 || request.abi != adapter_abi ||
      !valid_text(request.adapter, maximum_adapter_bytes) ||
      !valid_text(request.contract, maximum_contract_bytes) ||
      !valid_text(request.operation, maximum_operation_bytes) ||
      !valid_text(request.scope, maximum_scope_bytes) ||
      request.payload.size() > maximum_body_bytes)
    return {};
  const auto body_size = 4 * sizeof(std::uint16_t) + 2 * sizeof(std::uint32_t) +
      request.adapter.size() + request.contract.size() + request.operation.size() +
      request.scope.size() + request.payload.size();
  if (body_size > maximum_body_bytes)
    return {};
  std::vector<std::byte> bytes(header_bytes);
  bytes.reserve(header_bytes + body_size);
  const auto text = [&bytes](std::string_view value) {
    big_endian::append(bytes, static_cast<std::uint16_t>(value.size()));
    const auto raw = std::as_bytes(std::span(value.data(), value.size()));
    bytes.insert(bytes.end(), raw.begin(), raw.end());
  };
  text(request.adapter);
  text(request.contract);
  big_endian::append(bytes, request.abi);
  text(request.operation);
  text(request.scope);
  big_endian::append(bytes, static_cast<std::uint32_t>(request.payload.size()));
  bytes.insert(bytes.end(), request.payload.begin(), request.payload.end());
  if (!encode_header(bytes, Type::request, request.correlation))
    return {};
  return bytes;
}

} // namespace omarchy::plugin_runtime::provider_frame
