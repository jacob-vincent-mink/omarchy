#pragma once

#include <cstdint>

namespace omarchy::plugin_runtime::test_support {

struct LauncherProbe {
  std::uint32_t magic;
  std::int32_t pid;
  std::uint32_t uid;
  std::uint32_t gid;
  std::uint32_t descriptor_mask;
  std::uint32_t no_new_privileges;
  std::uint64_t open_files_max;
  std::uint64_t file_size_max;
  std::uint64_t core_size_max;
};

struct ProcessClaim {
  std::uint32_t magic;
  std::int32_t claimed_pid;
};

struct DescriptorReport {
  std::uint32_t count;
  std::uint32_t close_on_exec;
};

struct SandboxProbe {
  std::uint32_t magic = 0;
  std::uint32_t descriptor_mask = 0;
  std::uint32_t exact_descriptors = 0;
  std::uint32_t exact_environment = 0;
  std::uint32_t host_home_absent = 0;
  std::uint32_t bus_socket_absent = 0;
  std::uint32_t wayland_socket_absent = 0;
  std::uint32_t agent_socket_absent = 0;
  std::uint32_t other_plugin_state_absent = 0;
  std::uint32_t network_denied = 0;
  std::uint32_t descendant_denied = 0;
  std::uint32_t revision_write_denied = 0;
};

} // namespace omarchy::plugin_runtime::test_support
