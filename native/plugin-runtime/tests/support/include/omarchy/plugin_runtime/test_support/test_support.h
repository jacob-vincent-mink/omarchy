#pragma once

#include "../../../../temporary_directory.hpp"

#include "omarchy/plugin_runtime/unique_fd.hpp"

#include <sys/socket.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace omarchy::plugin_runtime::test_support {

using ::omarchy::plugin_runtime::UniqueFd;

[[nodiscard]] std::vector<int> open_fd_set();
inline std::size_t open_descriptor_count() {
  return open_fd_set().size();
}

struct SeqpacketPair {
  UniqueFd trusted;
  UniqueFd worker;

  [[nodiscard]] static SeqpacketPair create();
};

} // namespace omarchy::plugin_runtime::test_support
