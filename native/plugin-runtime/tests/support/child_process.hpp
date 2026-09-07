#pragma once

#include "test_assert.hpp"

#include <sys/wait.h>
#include <unistd.h>

namespace omarchy::plugin_runtime::test_support {

// The probe must exit explicitly; returning must never continue the parent test
// in a second process. Keep exact exit-status expectations at each call site.
template <typename Probe>
void expect_child_exit(int expected, Probe probe) {
  const pid_t child = ::fork();
  OMARCHY_CHECK(child >= 0);
  if (child == 0) {
    probe();
    ::_exit(255);
  }
  int status = 0;
  OMARCHY_CHECK(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == expected);
}

} // namespace omarchy::plugin_runtime::test_support
