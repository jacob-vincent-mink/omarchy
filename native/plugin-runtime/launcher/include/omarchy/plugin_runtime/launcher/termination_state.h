#pragma once

#include <poll.h>

namespace omarchy::plugin_runtime::launcher {

[[nodiscard]] constexpr bool pidfd_has_exited(short revents) noexcept {
  return revents == POLLIN || revents == (POLLIN | POLLHUP);
}

} // namespace omarchy::plugin_runtime::launcher
