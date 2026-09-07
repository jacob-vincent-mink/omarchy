#pragma once

#include <sys/socket.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>

namespace omarchy::plugin_runtime::test_support {

// Test-only datagram construction. Descriptors stay borrowed; callers choose
// their bound and retain all ownership, failure handling, and peer policy.
template <std::size_t MaximumDescriptors>
bool send_datagram(int channel, std::span<const std::byte> bytes,
                   std::span<const int> descriptors = {}) {
  static_assert(MaximumDescriptors > 0);
  if (descriptors.size() > MaximumDescriptors)
    return false;
  iovec part{.iov_base = const_cast<std::byte *>(bytes.data()), .iov_len = bytes.size()};
  alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(MaximumDescriptors * sizeof(int))> control{};
  msghdr message{};
  message.msg_iov = &part;
  message.msg_iovlen = 1;
  if (!descriptors.empty()) {
    message.msg_control = control.data();
    message.msg_controllen = CMSG_SPACE(descriptors.size_bytes());
    auto *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(descriptors.size_bytes());
    std::memcpy(CMSG_DATA(header), descriptors.data(), descriptors.size_bytes());
  }
  return ::sendmsg(channel, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
}

} // namespace omarchy::plugin_runtime::test_support
