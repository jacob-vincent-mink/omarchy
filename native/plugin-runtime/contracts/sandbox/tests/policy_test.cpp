#include "../../../tests/support/test_assert.hpp"

#include "omarchy/plugin_runtime/sandbox/policy.h"
#include "omarchy/plugin_runtime/sandbox/test_plan.h"
#include "omarchy/plugin_runtime/runtime_paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <linux/sched.h>

using omarchy::plugin_runtime::sandbox::build_plan;
using omarchy::plugin_runtime::sandbox::contains_argument_pair;
using omarchy::plugin_runtime::sandbox::SandboxPlan;
using omarchy::plugin_runtime::sandbox::test_support::build_plan_for_worker;

namespace {

using omarchy::plugin_runtime::test_support::throws_exception;
using omarchy::plugin_runtime::test_support::exit_assertions::fail;
using omarchy::plugin_runtime::test_support::exit_assertions::require;

bool contains(const std::vector<std::string> &values,
              std::string_view expected) {
  return std::ranges::find(values, expected) != values.end();
}

void verify_namespaces(const SandboxPlan &plan) {
  for (std::string_view argument :
       {"--unshare-user", "--unshare-pid", "--unshare-ipc", "--unshare-uts",
        "--unshare-net", "--unshare-cgroup", "--disable-userns",
        "--assert-userns-disabled", "--new-session", "--die-with-parent",
        "--as-pid-1"}) {
    OMARCHY_CHECK_WITH(require, contains(plan.argv, argument));
  }
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--cap-drop", "ALL"));
}

void verify_environment(const SandboxPlan &plan) {
  OMARCHY_CHECK_WITH(require, plan.pre_bwrap_environment ==
              std::vector<std::string>({"PATH=/usr/bin", "PWD=/"}));
  OMARCHY_CHECK_WITH(require, contains(plan.argv, "--clearenv"));
  OMARCHY_CHECK_WITH(require, contains(plan.worker_environment, "QT_QPA_PLATFORM=offscreen"));
  OMARCHY_CHECK_WITH(require, contains(plan.worker_environment, "QT_QUICK_CONTROLS_STYLE=Basic"));
  OMARCHY_CHECK_WITH(require, contains(plan.worker_environment, "QSG_RHI_BACKEND=software"));
  OMARCHY_CHECK_WITH(require, contains(plan.worker_environment, "HOME=/home/plugin"));
  for (std::string_view forbidden :
       {"DISPLAY=", "WAYLAND_DISPLAY=", "DBUS_SESSION_BUS_ADDRESS=",
        "SSH_AUTH_SOCK=", "GNUPGHOME=", "XAUTHORITY="}) {
    OMARCHY_CHECK_WITH(require, std::ranges::none_of(plan.worker_environment,
                                 [forbidden](const std::string &value) {
                                   return value.starts_with(forbidden);
                                 }));
  }
}

void verify_mounts(const SandboxPlan &plan, std::string_view worker) {
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--ro-bind", "/usr/lib"));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--symlink", "usr/lib"));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--ro-bind", worker));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--ro-bind-fd", "9"));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--bind-fd", "10"));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--tmpfs", "/tmp"));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--dir", "/tmp/cache"));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--tmpfs", "/run"));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--tmpfs", "/home"));
  OMARCHY_CHECK_WITH(require, !contains_argument_pair(plan, "--ro-bind", "/usr"));
  OMARCHY_CHECK_WITH(require, !contains_argument_pair(plan, "--bind", "/"));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--tmpfs", "/usr/lib/qt6/qml"));
  std::size_t qml_bind_count = 0;
  for (std::size_t index = 0; index + 2 < plan.argv.size(); ++index) {
    if (plan.argv[index] != "--ro-bind" ||
        !plan.argv[index + 1].starts_with("/usr/lib/qt6/qml/"))
      continue;
    ++qml_bind_count;
    const auto relative = plan.argv[index + 1].substr(
        std::string_view("/usr/lib/qt6/qml/").size());
    OMARCHY_CHECK_WITH(require, plan.argv[index + 2] == "/runtime/qml/" + relative);
    OMARCHY_CHECK_WITH(require, omarchy::plugin_runtime::sandbox::trusted_qml_resource(relative));
  }
  OMARCHY_CHECK_WITH(require, qml_bind_count ==
              omarchy::plugin_runtime::sandbox::trusted_qml_files().size());
  OMARCHY_CHECK_WITH(require, omarchy::plugin_runtime::sandbox::trusted_qml_public_module(
              "QtQuick.Layouts") &&
              omarchy::plugin_runtime::sandbox::trusted_qml_public_module(
                  "QtQuick.Effects") &&
              omarchy::plugin_runtime::sandbox::trusted_qml_public_module(
                  "QtQuick.Controls") &&
              !omarchy::plugin_runtime::sandbox::trusted_qml_public_module(
                  "QtQuick.Dialogs") &&
              !omarchy::plugin_runtime::sandbox::trusted_qml_public_module(
                  "Quickshell"));
  OMARCHY_CHECK_WITH(require, omarchy::plugin_runtime::sandbox::trusted_qml_files().size() == 36 &&
              omarchy::plugin_runtime::sandbox::trusted_qml_resource(
                  "QtQuick/Controls/Basic/Button.qml") &&
              omarchy::plugin_runtime::sandbox::trusted_qml_resource(
                  "QtQuick/Controls/Basic/impl/TextEditingContextMenu.qml") &&
              !omarchy::plugin_runtime::sandbox::trusted_qml_resource(
                  "QtQuick/Dialogs/qmldir") &&
              !omarchy::plugin_runtime::sandbox::trusted_qml_resource(
                  "QtQuick/Controls/Fusion/qmldir"));
  for (std::string_view forbidden :
       {"/run/user", "/home", "/sys", "/dev/dri", "/dev/input", "/etc/ssh"}) {
    OMARCHY_CHECK_WITH(require, std::ranges::none_of(plan.argv,
                                 [forbidden](const std::string &value) {
                                   return value == forbidden &&
                                          forbidden != "/home";
                                 }));
  }
}

void verify_descriptors(const SandboxPlan &plan) {
  OMARCHY_CHECK_WITH(require, plan.descriptors.control == 3 && plan.descriptors.broker == 4 &&
              plan.descriptors.render == 5 && plan.descriptors.status == 6 &&
              plan.descriptors.barrier == 7 && plan.descriptors.seccomp == 8 &&
              plan.descriptors.revision == 9 &&
              plan.descriptors.private_state == 10);
  OMARCHY_CHECK_WITH(require, plan.worker_descriptors == std::vector<int>({3, 4, 5}));
  OMARCHY_CHECK_WITH(require, plan.launcher_descriptors ==
              std::vector<int>({3, 4, 5, 6, 7, 8, 9, 10}));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--json-status-fd", "6"));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--block-fd", "7"));
  OMARCHY_CHECK_WITH(require, contains_argument_pair(plan, "--seccomp", "8"));
}

void verify_resources(const SandboxPlan &plan) {
  OMARCHY_CHECK_WITH(require, plan.resources.memory_high_bytes < plan.resources.memory_max_bytes);
  OMARCHY_CHECK_WITH(require, plan.resources.tasks_max == 16 &&
              plan.resources.cpu_quota_percent == 50);
  OMARCHY_CHECK_WITH(require, plan.resources.open_files_max == 64 &&
              plan.resources.file_size_max_bytes == 64 * 1024 * 1024 &&
              plan.resources.core_size_max_bytes == 0);
  OMARCHY_CHECK_WITH(require, contains(plan.transient_scope_properties, "OOMPolicy=kill"));
  OMARCHY_CHECK_WITH(require, contains(plan.transient_scope_properties, "KillMode=control-group"));
  OMARCHY_CHECK_WITH(require, plan.timeouts.launch_seconds == 5 &&
              plan.timeouts.hello_seconds == 3 &&
              plan.timeouts.graceful_shutdown_seconds == 1 &&
              plan.timeouts.forced_teardown_seconds == 2);
  OMARCHY_CHECK_WITH(require, plan.timeouts.restart_burst == 3 &&
              plan.timeouts.restart_window_seconds == 60 &&
              plan.timeouts.restart_backoff_max_seconds == 30);
  OMARCHY_CHECK_WITH(require, plan.timeouts.host_restart_burst == 5 &&
              plan.timeouts.host_restart_window_seconds == 60);
  OMARCHY_CHECK_WITH(require, plan.resources.output_burst_bytes == 64 * 1024 &&
              plan.resources.output_bytes_per_second == 4096);
  OMARCHY_CHECK_WITH(require, plan.process.standard_input_is_dev_null &&
              plan.process.standard_output_is_bounded_pipe &&
              plan.process.standard_error_is_bounded_pipe);
}

void verify_lifecycle(const SandboxPlan &plan) {
  OMARCHY_CHECK_WITH(require, plan.process.worker_is_pid_one && plan.process.descendants_permitted);
  OMARCHY_CHECK_WITH(require, plan.process.require_no_new_privileges &&
              plan.process.role_descriptors_are_close_on_exec);
  OMARCHY_CHECK_WITH(require, plan.process.bind_reported_pidfd_before_barrier_release &&
              plan.process.poll_pidfd_with_every_receive &&
              plan.process.recheck_pidfd_after_receive &&
              plan.process.signal_only_through_pidfd);
  OMARCHY_CHECK_WITH(require, plan.process.invalidate_generation_before_cleanup &&
              plan.process.kill_complete_generation_cgroup &&
              plan.process.reap_with_bounded_nonblocking_wait);
  OMARCHY_CHECK_WITH(require, plan.process.teardown_order ==
              std::vector<std::string>(
                  {"stop-accepting-messages", "invalidate-generation-handles",
                   "request-graceful-shutdown", "pidfd-sigkill-on-deadline",
                   "kill-generation-cgroup", "bounded-reap",
                   "remove-runtime-scratch"}));
}

void verify_seccomp(const SandboxPlan &plan) {
  OMARCHY_CHECK_WITH(require, plan.seccomp.denied_errno == EPERM);
  OMARCHY_CHECK_WITH(require, plan.seccomp.clone3_errno == ENOSYS);
  for (std::string_view forbidden :
       {"bpf", "io_uring_setup", "keyctl", "memfd_create", "mount",
        "open_by_handle_at", "perf_event_open", "ptrace", "reboot", "socket",
        "swapoff", "swapon", "unshare", "userfaultfd"}) {
    OMARCHY_CHECK_WITH(require, !contains(plan.seccomp.launch_allowlist, forbidden));
    OMARCHY_CHECK_WITH(require, !contains(plan.seccomp.steady_state_allowlist, forbidden));
  }
  OMARCHY_CHECK_WITH(require, contains(plan.seccomp.launch_allowlist, "execve"));
  OMARCHY_CHECK_WITH(require, !contains(plan.seccomp.steady_state_allowlist, "execve") &&
              !contains(plan.seccomp.steady_state_allowlist, "execveat"));
  OMARCHY_CHECK_WITH(require, !contains(plan.seccomp.steady_state_allowlist, "clone3"));
  OMARCHY_CHECK_WITH(require, (plan.seccomp.thread_clone.required_flags &
           (CLONE_VM | CLONE_SIGHAND | CLONE_THREAD)) ==
              (CLONE_VM | CLONE_SIGHAND | CLONE_THREAD));
  OMARCHY_CHECK_WITH(require, (plan.seccomp.thread_clone.forbidden_flags & CLONE_NEWUSER) != 0);
}
} // namespace

int main() {
  constexpr std::string_view worker =
      omarchy::plugin_runtime::kPackagedWorkerPath;
  const SandboxPlan plan = build_plan();
  OMARCHY_CHECK_WITH(require, plan.argv.front() == "/usr/bin/bwrap" &&
              plan.argv.back() == "/runtime/worker");
  verify_namespaces(plan);
  verify_environment(plan);
  verify_mounts(plan, worker);
  verify_descriptors(plan);
  verify_resources(plan);
  verify_lifecycle(plan);
  verify_seccomp(plan);

  constexpr std::string_view test_worker = "/tmp/omarchy-test-worker";
  const SandboxPlan test_plan = build_plan_for_worker(std::string(test_worker));
  OMARCHY_CHECK_WITH(require, test_plan.argv.size() == plan.argv.size());
  std::size_t changed_arguments = 0;
  for (std::size_t index = 0; index < plan.argv.size(); ++index) {
    changed_arguments += plan.argv.at(index) != test_plan.argv.at(index);
  }
  OMARCHY_CHECK_WITH(require, contains_argument_pair(test_plan, "--ro-bind", test_worker) &&
              !contains_argument_pair(test_plan, "--ro-bind", worker) &&
              test_plan.argv.back() == "/runtime/worker" &&
              changed_arguments == 1);

  OMARCHY_CHECK_WITH(require, throws_exception<std::invalid_argument>([&] {
    static_cast<void>(build_plan_for_worker("relative-worker"));
  }));

  OMARCHY_CHECK_WITH(require, throws_exception<std::invalid_argument>([&] {
    static_cast<void>(build_plan_for_worker("/tmp/../worker"));
  }));
  return 0;
}
