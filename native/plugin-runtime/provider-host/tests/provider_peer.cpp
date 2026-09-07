#include "../../tests/support/provider_peer_protocol.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace provider_peer_protocol;

namespace {
std::string argument(int argc, char **argv) {
  return argc >= 2 ? argv[1] : "echo";
}
bool isolated_descriptor_table(int inherited_fd) {
  struct stat null_metadata {};
  if (::stat("/dev/null", &null_metadata) < 0)
    return false;
  for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
    struct stat metadata {};
    if (::fstat(fd, &metadata) < 0 || !S_ISCHR(metadata.st_mode) ||
        metadata.st_rdev != null_metadata.st_rdev)
      return false;
  }
  struct stat channel {};
  errno = 0;
  return ::fstat(3, &channel) == 0 && S_ISSOCK(channel.st_mode) &&
         ::fcntl(inherited_fd, F_GETFD) < 0 && errno == EBADF &&
         ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1;
}
} // namespace

int main(int argc, char **argv) {
  const auto mode = argument(argc, argv);
  pid_t descendant = -1;
  if (mode == "marker" && argc == 3) {
    std::ofstream marker(argv[2]);
    marker << ::getpid() << '\n';
  }
  std::array<std::byte, 1024 * 1024 + 1024> request{};
  while (true) {
    const auto count = ::recv(3, request.data(), request.size(), 0);
    if (count <= 0)
      return 0;
    if (static_cast<std::size_t>(count) < kHeader || u32(request.data()) != kMagic)
      return 2;
    if (mode == "crash")
      return 3;
    if (mode == "late")
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto correlation = u64(request.data() + 8);
    std::string payload;
    if (mode == "pid")
      payload = std::to_string(::getpid());
    else if (mode == "descendant") {
      if (descendant <= 0) {
        descendant = ::fork();
        if (descendant < 0)
          return 5;
        if (descendant == 0) {
          ::close(3);
          while (true)
            ::pause();
        }
      }
      payload = std::to_string(::getpid()) + "|" +
                std::to_string(descendant);
    }
    else if (mode == "environment")
      payload = std::string(::getenv("PATH") ? ::getenv("PATH") : "") + "|" +
                (::getenv("HOME") ? ::getenv("HOME") : "");
    else if (mode == "inherited-environment")
      payload =
          std::string(::getenv("HYPRLAND_INSTANCE_SIGNATURE")
                          ? ::getenv("HYPRLAND_INSTANCE_SIGNATURE")
                          : "") +
          "|" +
          (::getenv("XDG_RUNTIME_DIR") ? ::getenv("XDG_RUNTIME_DIR") : "");
    else if (mode == "isolation" && argc == 3)
      payload = isolated_descriptor_table(std::atoi(argv[2])) ? "isolated"
                                                              : "leaked";
    else
      payload = "ok";
    if (!respond(mode, mode == "wrong-correlation" ? correlation + 1 : correlation,
                 payload))
      return 4;
  }
}
