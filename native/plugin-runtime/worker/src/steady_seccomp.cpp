#include "worker_runtime.hpp"

#include "omarchy/plugin_runtime/sandbox/seccomp_rule.hpp"

#include <QScopeGuard>

#include <linux/seccomp.h>
#include <seccomp.h>
#include <sys/prctl.h>

#include <cerrno>

namespace omarchy::plugin_runtime::worker {

bool install_steady_state_seccomp(std::string &error) {
  const auto policy = sandbox::build_plan().seccomp;
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    error = "cannot retain no-new-privileges before steady-state filter";
    return false;
  }
  scmp_filter_ctx context = seccomp_init(SCMP_ACT_ERRNO(policy.denied_errno));
  if (context == nullptr) {
    error = "cannot create steady-state seccomp filter";
    return false;
  }
  const auto release = qScopeGuard([context]() { seccomp_release(context); });
  for (const auto &name : policy.steady_state_allowlist) {
    const int syscall_number = seccomp_syscall_resolve_name(name.c_str());
    if (syscall_number == __NR_SCMP_ERROR) {
      error = "steady-state syscall unavailable: " + name;
      return false;
    }
    if (sandbox::allow_syscall(context, syscall_number, name, policy.thread_clone) < 0) {
      error = "cannot add steady-state syscall: " + name;
      return false;
    }
  }
  const int clone3 = seccomp_syscall_resolve_name("clone3");
  if (clone3 != __NR_SCMP_ERROR &&
      seccomp_rule_add(context, SCMP_ACT_ERRNO(policy.clone3_errno), clone3,
                       0) < 0) {
    error = "cannot add clone3 denial";
    return false;
  }
  if (seccomp_load(context) < 0) {
    error = "cannot install steady-state seccomp filter";
    return false;
  }
  return true;
}

} // namespace omarchy::plugin_runtime::worker
