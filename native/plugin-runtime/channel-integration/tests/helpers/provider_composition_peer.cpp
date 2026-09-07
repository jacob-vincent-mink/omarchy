#include "../../../tests/support/provider_peer_protocol.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace provider_peer_protocol;

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "ok";
  if (argc > 2) {
    std::ofstream marker(argv[2], std::ios::app);
    marker << ::getpid() << '\n';
  }
  std::array<std::byte, 1024 * 1024 + 1024> request{};
  while (true) {
    const auto received = ::recv(3, request.data(), request.size(), 0);
    if (received <= 0)
      return 0;
    if (static_cast<std::size_t>(received) < kHeader ||
        u32(request.data()) != kMagic)
      return 2;
    if (mode == "crash")
      return 3;
    if (mode == "delayed-pid")
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (mode == "timeout" || mode == "late")
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    const auto correlation = u64(request.data() + 8);
    const std::string payload = (mode == "pid" || mode == "delayed-pid")
                                    ? std::to_string(::getpid())
                                    : "composition-ok";
    if (!respond(mode, correlation, payload))
      return 4;
    if (mode == "late")
      return 0;
  }
}
