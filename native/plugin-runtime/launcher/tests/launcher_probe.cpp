#include "../../tests/support/probe_reports.hpp"
#include "../../tests/support/packet_sender.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>

namespace {
using Probe = omarchy::plugin_runtime::test_support::LauncherProbe;

[[noreturn]] void fail(int code) { _exit(code); }

std::uint32_t descriptor_mask() {
  DIR *directory = opendir("/proc/self/fd");
  if (directory == nullptr) {
    fail(100);
  }
  const int enumeration_fd = dirfd(directory);
  std::uint32_t mask = 0;
  while (const dirent *entry = readdir(directory)) {
    char *end = nullptr;
    const long descriptor = std::strtol(entry->d_name, &end, 10);
    if (end != nullptr && *end == '\0' && descriptor >= 0 && descriptor < 32 &&
        descriptor != enumeration_fd) {
      mask |= 1U << descriptor;
    }
  }
  closedir(directory);
  return mask;
}

void inject_descriptor() {
  const int injected = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (injected < 0) {
    fail(103);
  }
  const std::byte payload{0x44};
  if (!omarchy::plugin_runtime::test_support::send_datagram<1>(
          4, std::span(&payload, 1), std::span(&injected, 1))) {
    close(injected);
    fail(104);
  }
  close(injected);
}

void inject_many_descriptors() {
  std::array<int, 24> injected{};
  for (int &descriptor : injected) {
    descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
      fail(107);
  }
  const std::byte payload{0x55};
  if (!omarchy::plugin_runtime::test_support::send_datagram<24>(
          5, std::span(&payload, 1), injected))
    fail(108);
  for (const int descriptor : injected)
    close(descriptor);
}
} // namespace

int main() {
  for (const int descriptor : {3, 4, 5}) {
    if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
      fail(101);
    }
  }
  rlimit open_files{};
  rlimit file_size{};
  rlimit core_size{};
  if (getrlimit(RLIMIT_NOFILE, &open_files) < 0 ||
      getrlimit(RLIMIT_FSIZE, &file_size) < 0 ||
      getrlimit(RLIMIT_CORE, &core_size) < 0) {
    fail(106);
  }
  const Probe probe{.magic = 0x43575037,
                    .pid = static_cast<std::int32_t>(getpid()),
                    .uid = getuid(),
                    .gid = getgid(),
                    .descriptor_mask = descriptor_mask(),
                    .no_new_privileges = static_cast<std::uint32_t>(
                        prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0)),
                    .open_files_max = open_files.rlim_cur,
                    .file_size_max = file_size.rlim_cur,
                    .core_size_max = core_size.rlim_cur};
  if (send(3, &probe, sizeof(probe), MSG_NOSIGNAL) != sizeof(probe)) {
    fail(102);
  }
  inject_descriptor();
  inject_many_descriptors();
  std::byte acknowledgement{};
  if (recv(3, &acknowledgement, sizeof(acknowledgement), 0) != 1) {
    fail(105);
  }
  return 0;
}
