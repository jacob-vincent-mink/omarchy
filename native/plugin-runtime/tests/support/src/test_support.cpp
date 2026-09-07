#include "omarchy/plugin_runtime/test_support/test_support.h"

#include "omarchy/plugin_runtime/unique_directory.hpp"
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <stdexcept>

namespace omarchy::plugin_runtime::test_support {
std::vector<int> open_fd_set() {
  UniqueFd directory_fd(
      open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory_fd) {
    throw std::runtime_error("cannot open /proc/self/fd");
  }
  auto directory = take_directory(std::move(directory_fd));
  if (directory == nullptr) {
    throw std::runtime_error("cannot enumerate /proc/self/fd");
  }
  const int enumeration_fd = dirfd(directory.get());
  std::vector<int> output;
  while (const dirent *entry = readdir(directory.get())) {
    int value = -1;
    const std::string_view name(entry->d_name);
    const auto result =
        std::from_chars(name.data(), name.data() + name.size(), value);
    if (result.ec == std::errc{} && result.ptr == name.data() + name.size() &&
        value != enumeration_fd) {
      output.push_back(value);
    }
  }
  directory.reset();
  std::ranges::sort(output);
  return output;
}

SeqpacketPair SeqpacketPair::create() {
  std::array<int, 2> descriptors{};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                 descriptors.data()) < 0) {
    throw std::runtime_error("socketpair failed");
  }
  return {.trusted = UniqueFd(descriptors.at(0)),
          .worker = UniqueFd(descriptors.at(1))};
}

} // namespace omarchy::plugin_runtime::test_support
