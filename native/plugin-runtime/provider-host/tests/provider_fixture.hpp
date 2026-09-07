#pragma once

#include "../../tests/support/test_assert.hpp"
#include "../../tests/support/packet_sender.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace provider_test {
using omarchy::plugin_runtime::test_support::require;

// Independent of the production codec: protocol drift must break these peers.
inline std::vector<std::byte> request_frame(
    std::uint64_t correlation, std::string_view adapter, std::string_view digest,
    std::string_view operation, std::string_view scope, const QByteArray &payload) {
  std::vector<std::byte> bytes;
  const auto number = [&](std::uint64_t value, unsigned width) {
    for (unsigned i = width; i > 0; --i)
      bytes.push_back(static_cast<std::byte>(value >> ((i - 1) * 8)));
  };
  const auto text = [&](std::string_view value) {
    OMARCHY_CHECK(value.size() <= UINT16_MAX);
    number(value.size(), 2);
    for (const auto ch : value) bytes.push_back(static_cast<std::byte>(ch));
  };
  number(0x4f505256, 4);
  number(0x01010000, 4);
  number(correlation, 8);
  const auto body_size = 2 + adapter.size() + 2 + digest.size() + 4 +
                         2 + operation.size() + 2 + scope.size() + 4 + payload.size();
  number(body_size, 4);
  text(adapter);
  text(digest);
  number(1, 4);
  text(operation);
  text(scope);
  number(payload.size(), 4);
  for (const auto ch : payload) bytes.push_back(static_cast<std::byte>(ch));
  return bytes;
}

struct Child { pid_t pid; int channel; };

inline Child start_provider(std::vector<std::string> arguments) {
  OMARCHY_CHECK(!arguments.empty());
  std::vector<char *> argv;
  for (auto &argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);
  int pair[2];
  OMARCHY_CHECK(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
  const auto pid = ::fork();
  OMARCHY_CHECK(pid >= 0);
  if (pid == 0) {
    ::close(pair[0]);
    if (pair[1] != 3 && ::dup2(pair[1], 3) != 3) _exit(126);
    if (pair[1] != 3) ::close(pair[1]);
    if (::fcntl(3, F_SETFD, 0) < 0) _exit(126);
    ::execv(argv[0], argv.data());
    _exit(127);
  }
  ::close(pair[1]);
  return {.pid = pid, .channel = pair[0]};
}

inline QJsonObject roundtrip(int channel, const std::vector<std::byte> &frame) {
  OMARCHY_CHECK(::send(channel, frame.data(), frame.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(frame.size()));
  std::array<std::byte, 70 * 1024> response;
  iovec part{.iov_base = response.data(), .iov_len = response.size()};
  msghdr message{};
  message.msg_iov = &part;
  message.msg_iovlen = 1;
  const auto count = ::recvmsg(channel, &message, 0);
  OMARCHY_CHECK(count > 21 && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
  const auto bytes = std::span(response).first(static_cast<std::size_t>(count));
  std::uint32_t length = 0;
  for (unsigned i = 16; i < 20; ++i)
    length = (length << 8) | std::to_integer<unsigned char>(bytes[i]);
  constexpr std::array prefix{std::byte{0x4f}, std::byte{0x50}, std::byte{0x52},
                              std::byte{0x56}, std::byte{1}, std::byte{2},
                              std::byte{0}, std::byte{0}};
  OMARCHY_CHECK(std::ranges::equal(bytes.first(8), prefix) &&
              frame.size() >= 20 &&
              std::ranges::equal(bytes.subspan(8, 8), std::span(frame).subspan(8, 8)) &&
              length == bytes.size() - 20 && bytes[20] == std::byte{0});
  const auto document = QJsonDocument::fromJson(QByteArray(
      reinterpret_cast<const char *>(bytes.data() + 21), count - 21));
  OMARCHY_CHECK(document.isObject());
  return document.object();
}

inline int finish(Child child) {
  ::close(child.channel);
  int status = 0;
  pid_t result;
  do { result = ::waitpid(child.pid, &status, 0); } while (result < 0 && errno == EINTR);
  OMARCHY_CHECK(result == child.pid);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

inline void reject_descriptors(Child child, const std::vector<std::byte> &frame) {
  const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  OMARCHY_CHECK(descriptor >= 0);
  const bool sent = omarchy::plugin_runtime::test_support::send_datagram<1>(
      child.channel, frame, std::span(&descriptor, 1));
  ::close(descriptor);
  OMARCHY_CHECK(sent);
  OMARCHY_CHECK(finish(child) == 2);
}
} // namespace provider_test
