#pragma once

#include "omarchy/plugin_runtime/provider_frame.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace omarchy::plugin_runtime::provider_protocol {
inline constexpr auto header_bytes = provider_frame::header_bytes;
inline constexpr auto maximum_frame_bytes = provider_frame::maximum_frame_bytes;

struct ExitCodes {
  int receive = 1;
  int malformed = 2;
  int response = 3;
};

inline bool exact_keys(const QJsonObject &object,
                       std::initializer_list<QStringView> required,
                       std::initializer_list<QStringView> optional = {}) {
  for (const auto key : required)
    if (!object.contains(key))
      return false;
  for (auto it = object.begin(); it != object.end(); ++it) {
    const auto key = QStringView(it.key());
    if (std::ranges::find(required, key) == required.end() &&
        std::ranges::find(optional, key) == optional.end())
      return false;
  }
  return true;
}

// Providers consume descriptor-free datagrams. With no ancillary buffer,
// recvmsg discards supplied descriptors and reports MSG_CTRUNC; reject them
// before decoding or dispatching any provider effect.
template <typename Decode, typename Dispatch>
int serve(Decode decode, Dispatch dispatch, ExitCodes exits = {}, int channel = 3) {
  std::array<std::byte, maximum_frame_bytes + 1> frame{};
  while (true) {
    iovec part{.iov_base = frame.data(), .iov_len = frame.size()};
    msghdr message{};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    const auto count = ::recvmsg(channel, &message, MSG_TRUNC);
    if (count == 0)
      return 0;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return exits.receive;
    }
    if (count > static_cast<ssize_t>(maximum_frame_bytes) ||
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
      return exits.malformed;
    const auto request = decode(std::span(frame.data(), static_cast<std::size_t>(count)));
    if (!request)
      return exits.malformed;
    if (!dispatch(*request))
      return exits.response;
  }
}

// Text views borrow the datagram and must not outlive synchronous dispatch.
struct Request {
  std::uint64_t correlation;
  std::string_view adapter, contract, operation, scope;
  QJsonObject payload;
};

inline std::optional<Request> decode(std::span<const std::byte> frame,
                                     std::size_t maximum_payload = provider_frame::maximum_body_bytes) {
  const auto decoded = provider_frame::decode_request(frame, maximum_payload);
  if (!decoded)
    return std::nullopt;
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(
      QByteArray(reinterpret_cast<const char *>(decoded->payload.data()),
                 static_cast<qsizetype>(decoded->payload.size())), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return std::nullopt;
  return Request{decoded->correlation, decoded->adapter, decoded->contract,
                 decoded->operation, decoded->scope, document.object()};
}

inline std::vector<std::byte> response(std::uint64_t correlation,
                                        const QByteArray &payload) {
  if (correlation == 0 || payload.size() >
          static_cast<qsizetype>(maximum_frame_bytes - header_bytes - 1))
    return {};
  std::vector<std::byte> bytes(header_bytes);
  bytes.reserve(header_bytes + 1 + payload.size());
  bytes.push_back(std::byte{0});
  for (const auto ch : payload) bytes.push_back(static_cast<std::byte>(ch));
  if (!provider_frame::encode_header(bytes, provider_frame::Type::response, correlation))
    return {};
  return bytes;
}

inline bool send_response(std::uint64_t correlation, const QByteArray &payload,
                           int channel = 3) {
  const auto frame = response(correlation, payload);
  if (frame.empty()) return false;
  ssize_t sent;
  do { sent = ::send(channel, frame.data(), frame.size(), MSG_NOSIGNAL); }
  while (sent < 0 && errno == EINTR);
  return sent == static_cast<ssize_t>(frame.size());
}
} // namespace omarchy::plugin_runtime::provider_protocol
