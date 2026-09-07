#include "../support/probe_reports.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>


extern char **environ;

namespace {

using omarchy::plugin_runtime::test_support::SandboxProbe;

int sandbox_probe() {
  SandboxProbe probe{.magic = 0x53425831};
  DIR *directory = opendir("/proc/self/fd");
  if (directory == nullptr) {
    return 80;
  }
  const int enumeration_fd = dirfd(directory);
  std::set<int> descriptors;
  while (const dirent *entry = readdir(directory)) {
    char *end = nullptr;
    const long value = std::strtol(entry->d_name, &end, 10);
    if (*entry->d_name != '\0' && end != nullptr && *end == '\0' &&
        value != enumeration_fd && value >= 0) {
      descriptors.insert(static_cast<int>(value));
      if (value < 32) {
        probe.descriptor_mask |= 1U << static_cast<unsigned>(value);
      }
    }
  }
  closedir(directory);
  probe.exact_descriptors = descriptors == std::set<int>({0, 1, 2, 3, 4, 5});

  std::vector<std::string> expected = {
      "HOME=/home/plugin",       "LANG=C.UTF-8",
      "LC_ALL=C.UTF-8",         "PATH=/runtime",
      "PWD=/plugin",            "QT_QPA_PLATFORM=offscreen",
      "QT_QUICK_CONTROLS_STYLE=Basic",
      "QSG_RHI_BACKEND=software", "XDG_CACHE_HOME=/tmp/cache",
      "XDG_CONFIG_HOME=/state/config", "XDG_DATA_HOME=/state/data",
      "XDG_RUNTIME_DIR=/run/plugin",
  };
  std::vector<std::string> actual;
  for (char **entry = environ; *entry != nullptr; ++entry) {
    actual.emplace_back(*entry);
  }
  std::sort(expected.begin(), expected.end());
  std::sort(actual.begin(), actual.end());
  probe.exact_environment = actual == expected;

  std::ifstream fixture("/plugin/fixture");
  std::string host_home;
  std::string bus_socket;
  std::string wayland_socket;
  std::string agent_socket;
  std::string other_plugin_state;
  std::getline(fixture, host_home);
  std::getline(fixture, bus_socket);
  std::getline(fixture, wayland_socket);
  std::getline(fixture, agent_socket);
  std::getline(fixture, other_plugin_state);
  errno = 0;
  probe.host_home_absent = !host_home.empty() &&
                           access(host_home.c_str(), F_OK) < 0 &&
                           errno == ENOENT;
  errno = 0;
  probe.bus_socket_absent = !bus_socket.empty() &&
                            access(bus_socket.c_str(), F_OK) < 0 &&
                            errno == ENOENT;
  errno = 0;
  probe.wayland_socket_absent = !wayland_socket.empty() &&
                                access(wayland_socket.c_str(), F_OK) < 0 &&
                                errno == ENOENT;
  errno = 0;
  probe.agent_socket_absent = !agent_socket.empty() &&
                              access(agent_socket.c_str(), F_OK) < 0 &&
                              errno == ENOENT;
  errno = 0;
  probe.other_plugin_state_absent =
      !other_plugin_state.empty() &&
      access(other_plugin_state.c_str(), F_OK) < 0 && errno == ENOENT;

  errno = 0;
  const int network = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  probe.network_denied = network < 0 && errno == EPERM;
  if (network >= 0) {
    close(network);
  }
  errno = 0;
  const pid_t descendant = fork();
  probe.descendant_denied = descendant < 0 && errno == EPERM;
  if (descendant == 0) {
    _exit(81);
  }
  if (descendant > 0) {
    kill(descendant, SIGKILL);
    waitpid(descendant, nullptr, 0);
  }
  errno = 0;
  const int revision_write = open("/plugin/fixture", O_WRONLY | O_CLOEXEC);
  probe.revision_write_denied =
      revision_write < 0 && (errno == EROFS || errno == EACCES);
  if (revision_write >= 0) {
    close(revision_write);
  }
  if (send(3, &probe, sizeof(probe), MSG_NOSIGNAL) != sizeof(probe)) {
    return 82;
  }
  std::byte acknowledgement{};
  return recv(3, &acknowledgement, sizeof(acknowledgement), 0) == 1 ? 0 : 83;
}

} // namespace

int main() { return sandbox_probe(); }
