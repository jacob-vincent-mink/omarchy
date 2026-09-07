#include "../../tests/support/probe_reports.hpp"
#include "../../tests/support/test_assert.hpp"
#include "../../tests/support/blocking_gate.hpp"

#include "omarchy/plugin_runtime/launcher/launcher.h"
#include "omarchy/plugin_runtime/launcher/test_supervisor.h"
#include "omarchy/plugin_runtime/launcher/termination_state.h"
#include "omarchy/plugin_runtime/test_support/test_support.h"
#include "../src/process_cleanup.hpp"
#include "../src/status_json.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace launcher = omarchy::plugin_runtime::launcher;
namespace sandbox = omarchy::plugin_runtime::sandbox;
namespace support = omarchy::plugin_runtime::test_support;

namespace {
using namespace std::chrono_literals;

using Probe = omarchy::plugin_runtime::test_support::LauncherProbe;

using Claim = omarchy::plugin_runtime::test_support::ProcessClaim;

using omarchy::plugin_runtime::test_support::DescriptorReport;

using omarchy::plugin_runtime::test_support::exit_assertions::fail;
using omarchy::plugin_runtime::test_support::exit_assertions::require;

launcher::Deadline deadline_after(std::chrono::milliseconds duration) {
  return std::chrono::steady_clock::now() + duration;
}

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  return predicate();
}

bool await_readable_lanes(launcher::Worker &worker,
                          launcher::EndpointMask expected) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  const auto expected_bits = static_cast<std::uint8_t>(expected);
  std::uint8_t observed = 0;
  while ((observed & expected_bits) != expected_bits &&
         std::chrono::steady_clock::now() < deadline) {
    std::array<epoll_event, 3> events{};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const int timeout = static_cast<int>(
        std::max<std::chrono::milliseconds::rep>(1, remaining.count()));
    const int count =
        epoll_wait(worker.readiness_fd(), events.data(), events.size(), timeout);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    for (int index = 0; index < count; ++index) {
      if ((events[index].events & EPOLLIN) != 0 && events[index].data.u64 < 3) {
        observed |= static_cast<std::uint8_t>(1U << events[index].data.u64);
      }
    }
  }
  return (observed & expected_bits) == expected_bits;
}

using support::open_descriptor_count;

enum class WakeFault { none, interrupt_once, saturated, failed };
std::atomic<WakeFault> wake_fault = WakeFault::none;
std::atomic<unsigned> wake_calls = 0;

ssize_t injected_wake(int descriptor, const void *bytes,
                      std::size_t size) {
  const unsigned call = wake_calls.fetch_add(1);
  if (wake_fault == WakeFault::interrupt_once && call == 0) {
    errno = EINTR;
    return -1;
  }
  if (wake_fault == WakeFault::saturated) {
    errno = EAGAIN;
    return -1;
  }
  if (wake_fault == WakeFault::failed) {
    errno = EIO;
    return -1;
  }
  return write(descriptor, bytes, size);
}

template <typename Value> Value decode(std::span<const std::byte> bytes) {
  OMARCHY_CHECK_WITH(require, bytes.size() == sizeof(Value));
  Value value{};
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

class FakeScope final : public launcher::ResourceScopeController {
public:
  static bool delay_before_deadline(std::chrono::milliseconds delay,
                                    launcher::Deadline deadline,
                                    std::string &error, const char *expired) {
    if (delay > 0ms) {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (remaining <= delay) {
        std::this_thread::sleep_until(deadline);
        error = expired;
        return false;
      }
      std::this_thread::sleep_for(delay);
    }
    return true;
  }

  bool probe(launcher::Deadline deadline, std::string &error) override {
    probe_deadline = deadline;
    if (!delay_before_deadline(probe_delay, deadline, error,
                               "synthetic preflight deadline expired"))
      return false;
    if (!available) error = "synthetic resource controller unavailable";
    return available;
  }

  bool prepare_cleanup(launcher::Deadline deadline,
                       std::string &error) override {
    cleanup_deadline = deadline;
    if (!delay_before_deadline(cleanup_setup_delay, deadline, error,
                               "synthetic cleanup setup deadline expired"))
      return false;
    cleanup_prepared = true;
    return true;
  }

  AttachResult attach_validated(const launcher::ProcessScopeRequest &request,
                                launcher::Deadline deadline,
                                std::string &error) override {
    attach_deadline = deadline;
    if (!attach_succeeds) {
      error = "synthetic scope attachment rejected";
      return {};
    }
    OMARCHY_CHECK_WITH(require, request.unit.starts_with("app-omarchy-plugin-worker-"));
    OMARCHY_CHECK_WITH(require, request.description == "Omarchy sandboxed plugin worker");
    OMARCHY_CHECK_WITH(require, request.pids.size() == 2 && request.pids[0] > 0 &&
                request.pids[1] > 0 && request.pids[0] != request.pids[1]);
    OMARCHY_CHECK_WITH(require, request.resources.memory_high_bytes ==
                    384ULL * 1024ULL * 1024ULL &&
                request.resources.memory_max_bytes ==
                    512ULL * 1024ULL * 1024ULL &&
                request.resources.tasks_max == 16 &&
                request.resources.cpu_quota_per_second_usec == 500000 &&
                request.resources.cpu_weight == 20 &&
                request.resources.io_weight == 10);
    attached = true;
    attached_before_release = true;
    scope.assign(request.unit);
    description.assign(request.description);
    pids.assign(request.pids.begin(), request.pids.end());
    if (!delay_before_deadline(attach_delay, deadline, error,
                               "synthetic attachment deadline expired"))
      return {.attached = false, .cleanup_required = true};
    return {.attached = std::chrono::steady_clock::now() < deadline,
            .cleanup_required = true};
  }

  bool terminate_scope_validated(std::string_view unit,
                                  launcher::Deadline deadline,
                                  std::string &error) noexcept override {
    const bool matching = unit == scope;
    if (matching && remove_delay > 0ms)
      std::this_thread::sleep_until(std::min(
          deadline, std::chrono::steady_clock::now() + remove_delay));
    const bool succeeds = termination_succeeds.load();
    if (matching)
      ++termination_count;
    if (!succeeds)
      error = "synthetic confirmed termination failed";
    return succeeds && std::chrono::steady_clock::now() < deadline;
  }

  bool available = true;
  bool attach_succeeds = true;
  bool attached = false;
  bool attached_before_release = false;
  std::atomic<bool> termination_succeeds = true;
  std::atomic<unsigned> termination_count = 0;
  std::chrono::milliseconds probe_delay{};
  std::chrono::milliseconds cleanup_setup_delay{};
  std::chrono::milliseconds attach_delay{};
  std::chrono::milliseconds remove_delay{};
  std::optional<launcher::Deadline> probe_deadline;
  std::optional<launcher::Deadline> cleanup_deadline;
  std::optional<launcher::Deadline> attach_deadline;
  std::string scope;
  std::string description;
  std::vector<pid_t> pids;
  bool cleanup_prepared = false;
};

class IncompleteController : public launcher::ResourceScopeController {
};

static_assert(std::is_abstract_v<IncompleteController>,
              "incomplete controllers must not enter the launch path");
static_assert(std::is_abstract_v<launcher::test_support::ReadyScope>);
static_assert(std::is_abstract_v<launcher::test_support::AttachedScope>);

class ScopeRequestProbe final : public launcher::test_support::ReadyScope {
public:
  AttachResult attach_validated(const launcher::ProcessScopeRequest &request,
                                launcher::Deadline deadline,
                                std::string &) override {
    ++attach_calls;
    unit.assign(request.unit);
    description.assign(request.description);
    pids.assign(request.pids.begin(), request.pids.end());
    resources = request.resources;
    attach_deadline = deadline;
    return attach_result;
  }
  bool terminate_scope_validated(std::string_view requested,
                                  launcher::Deadline,
                                  std::string &error) noexcept override {
    if (requested == unit) ++termination_calls;
    if (!termination_succeeds)
      error = "synthetic partial scope cleanup";
    return termination_succeeds;
  }

  AttachResult attach_result{.attached = true, .cleanup_required = true};
  unsigned attach_calls = 0;
  unsigned termination_calls = 0;
  bool termination_succeeds = true;
  std::string unit;
  std::string description;
  std::vector<pid_t> pids;
  launcher::ProcessResourceCeilings resources;
  std::optional<launcher::Deadline> attach_deadline;
};

class BlockingCleanupScope final : public launcher::test_support::AttachedScope {
public:
  bool terminate_scope_validated(std::string_view, launcher::Deadline,
                                  std::string &) noexcept override {
    const unsigned current = entered.fetch_add(1) + 1;
    if (current == 1)
      gate.arrive_and_wait();
    completed.fetch_add(1);
    return true;
  }

  omarchy::plugin_runtime::test_support::BlockingGate gate;
  std::atomic<unsigned> entered = 0;
  std::atomic<unsigned> completed = 0;
};

class DeadlineCleanupScope final : public launcher::test_support::AttachedScope {
public:
  bool terminate_scope_validated(std::string_view,
                                  launcher::Deadline deadline,
                                  std::string &error) noexcept override {
    if (calls.fetch_add(1) == 0)
      std::this_thread::sleep_until(
          std::min(deadline, std::chrono::steady_clock::now() + 50ms));
    completed.fetch_add(1);
    if (std::chrono::steady_clock::now() >= deadline) {
      error = "synthetic confirmed termination deadline expired";
      return false;
    }
    return true;
  }
  std::atomic<unsigned> calls = 0;
  std::atomic<unsigned> completed = 0;
};

class FailingCleanupScope final : public launcher::test_support::AttachedScope {
public:
  enum class Failure { bus_loss, partial_cleanup, timeout };
  explicit FailingCleanupScope(Failure selected) : failure(selected) {}
  bool terminate_scope_validated(std::string_view,
                                  launcher::Deadline deadline,
                                  std::string &error) noexcept override {
    ++calls;
    if (recover)
      return true;
    if (failure == Failure::timeout)
      std::this_thread::sleep_until(deadline);
    if (failure == Failure::bus_loss)
      error = "synthetic cleanup bus lost";
    else if (failure == Failure::partial_cleanup)
      error = "synthetic cgroup remained populated";
    else
      error = "synthetic confirmed termination deadline expired";
    return false;
  }
  Failure failure;
  std::atomic<bool> recover = false;
  std::atomic<unsigned> calls = 0;
};

struct LaunchFixture {
  LaunchFixture() {
    const auto revision_path = tree.path() / "revision";
    const auto state_path = tree.path() / "state";
    std::filesystem::create_directory(revision_path);
    std::filesystem::create_directory(state_path);
    support::TemporaryDirectory::write_file(revision_path / "plugin.qml",
        "import QtQml\nQtObject {}\n", O_WRONLY | O_CREAT | O_EXCL, 0644);
    OMARCHY_CHECK_WITH(require, chmod(revision_path.c_str(), 0555) == 0);
    revision.reset(
        open(revision_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    state.reset(
        open(state_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    OMARCHY_CHECK_WITH(require, revision && state);
  }

  [[nodiscard]] launcher::TrustedLaunchRequest request() const {
    return {.plugin_id = "org.omarchy_fixture",
            .revision_sha256 = std::string(64, 'a'),
            .generation = 17,
            .revision_directory_fd = revision.get(),
            .private_state_directory_fd = state.get()};
  }

  support::TemporaryDirectory tree;
  support::UniqueFd revision;
  support::UniqueFd state;
};

void validate_probe(launcher::Worker &worker) {
  const auto message = worker.receive_any(
      launcher::PacketSizeLimit{sizeof(Probe)},
      deadline_after(2s),
      launcher::EndpointMask::control);
  OMARCHY_CHECK_WITH(require, static_cast<bool>(message));
  const Probe probe = decode<Probe>(message.payload);
  OMARCHY_CHECK_WITH(require, probe.magic == 0x43575037 && probe.pid == 1 && probe.uid == 0 &&
              probe.gid == 0 && probe.descriptor_mask == 0x3f &&
              probe.no_new_privileges == 1 && probe.open_files_max == 64 &&
              probe.file_size_max == 64ULL * 1024ULL * 1024ULL &&
              probe.core_size_max == 0);
}

void pidfd_reap_state_test() {
  OMARCHY_CHECK_WITH(require, launcher::pidfd_has_exited(POLLIN) &&
              launcher::pidfd_has_exited(POLLIN | POLLHUP) &&
              !launcher::pidfd_has_exited(POLLHUP) &&
              !launcher::pidfd_has_exited(POLLIN | POLLERR) &&
              !launcher::pidfd_has_exited(POLLIN | POLLNVAL));
}

launcher::ProcessResourceCeilings scope_resources() {
  return {.memory_high_bytes = 1024,
          .memory_max_bytes = 2048,
          .tasks_max = 8,
          .cpu_quota_per_second_usec = 250000,
          .cpu_weight = 50,
          .io_weight = 25};
}

void generic_process_scope_request_test() {
  ScopeRequestProbe scope;
  const std::array<pid_t, 3> processes{101, 202, 303};
  const auto deadline = deadline_after(5s);
  std::string error;
  const launcher::ProcessScopeRequest request{
      .unit = "app-omarchy-plugin-provider-example.scope",
      .description = "Omarchy trusted plugin provider",
      .pids = processes,
      .resources = scope_resources()};
  const auto attached = scope.attach(request, deadline, error);
  OMARCHY_CHECK_WITH(require, attached.attached && attached.cleanup_required && error.empty() &&
              scope.attach_calls == 1 && scope.attach_deadline == deadline &&
              scope.unit == request.unit &&
              scope.description == request.description &&
              scope.pids == std::vector<pid_t>(processes.begin(),
                                               processes.end()) &&
              scope.resources.memory_high_bytes == 1024 &&
              scope.resources.memory_max_bytes == 2048 &&
              scope.resources.tasks_max == 8 &&
              scope.resources.cpu_quota_per_second_usec == 250000 &&
              scope.resources.cpu_weight == 50 &&
              scope.resources.io_weight == 25);
  std::string termination_error = "stale caller text";
  OMARCHY_CHECK_WITH(require, scope.terminate_scope(request.unit, deadline, termination_error) &&
              scope.termination_calls == 1 && termination_error.empty());

  ScopeRequestProbe boundary;
  std::string boundary_error;
  auto boundary_request = request;
  boundary_request.resources.memory_high_bytes =
      launcher::kMaximumProcessScopeMemoryBytes;
  boundary_request.resources.memory_max_bytes =
      launcher::kMaximumProcessScopeMemoryBytes;
  boundary_request.resources.tasks_max = launcher::kMaximumProcessScopeTasks;
  boundary_request.resources.cpu_quota_per_second_usec =
      launcher::kMaximumProcessScopeCpuQuotaUsec;
  OMARCHY_CHECK_WITH(require, boundary.attach(boundary_request, deadline_after(5s),
                          boundary_error)
                  .attached &&
              boundary.attach_calls == 1 && boundary_error.empty());

  const auto rejected_without_call = [&](std::span<const pid_t> pids,
                                         std::string_view expected) {
    ScopeRequestProbe rejected;
    std::string rejected_error;
    auto candidate = request;
    candidate.pids = pids;
    const auto result = rejected.attach(candidate, deadline, rejected_error);
    OMARCHY_CHECK_WITH(require, !result.attached && !result.cleanup_required &&
                rejected.attach_calls == 0 &&
                rejected_error.find(expected) != std::string::npos);
  };
  rejected_without_call({}, "PID count");
  const std::array<pid_t, 2> invalid{101, -1};
  rejected_without_call(invalid, "invalid or duplicate");
  const std::array<pid_t, 2> zero{101, 0};
  rejected_without_call(zero, "invalid or duplicate");
  const std::array<pid_t, 2> duplicate{101, 101};
  rejected_without_call(duplicate, "invalid or duplicate");

  ScopeRequestProbe invalid_resources;
  std::string resource_error;
  auto bad_resources = request;
  bad_resources.resources.memory_high_bytes =
      bad_resources.resources.memory_max_bytes + 1;
  const auto resource_result = invalid_resources.attach(
      bad_resources, deadline_after(5s), resource_error);
  OMARCHY_CHECK_WITH(require, !resource_result.attached && !resource_result.cleanup_required &&
              invalid_resources.attach_calls == 0 &&
              resource_error.find("resource ceilings") != std::string::npos);
  const auto rejects_resource = [&](auto mutation) {
    ScopeRequestProbe rejected;
    std::string rejected_error;
    auto candidate = boundary_request;
    mutation(candidate.resources);
    const auto result = rejected.attach(candidate, deadline_after(5s),
                                        rejected_error);
    OMARCHY_CHECK_WITH(require, !result.attached && !result.cleanup_required &&
                rejected.attach_calls == 0 &&
                rejected_error.find("resource ceilings") != std::string::npos);
  };
  rejects_resource([](auto &resources) {
    resources.memory_max_bytes =
        launcher::kMaximumProcessScopeMemoryBytes + 1;
  });
  rejects_resource([](auto &resources) {
    resources.tasks_max = launcher::kMaximumProcessScopeTasks + 1;
  });
  rejects_resource([](auto &resources) {
    resources.cpu_quota_per_second_usec =
        launcher::kMaximumProcessScopeCpuQuotaUsec + 1;
  });

  ScopeRequestProbe expired;
  std::string expired_error;
  const auto expired_result = expired.attach(
      request, std::chrono::steady_clock::now(), expired_error);
  OMARCHY_CHECK_WITH(require, !expired_result.attached && !expired_result.cleanup_required &&
              expired.attach_calls == 0 &&
              expired_error.find("deadline") != std::string::npos);

  ScopeRequestProbe attempted;
  attempted.attach_result = {.attached = false, .cleanup_required = true};
  std::string attempted_error;
  const auto attempted_result = attempted.attach(
      request, deadline_after(5s), attempted_error);
  OMARCHY_CHECK_WITH(require, !attempted_result.attached && attempted_result.cleanup_required &&
              attempted.attach_calls == 1);
  std::string cleanup_error;
  OMARCHY_CHECK_WITH(require, attempted.terminate_scope(request.unit, deadline_after(5s),
                                    cleanup_error) &&
              attempted.termination_calls == 1);

  ScopeRequestProbe partial_cleanup;
  partial_cleanup.unit = std::string(request.unit);
  partial_cleanup.termination_succeeds = false;
  std::string partial_error;
  OMARCHY_CHECK_WITH(require, !partial_cleanup.terminate_scope(request.unit, deadline_after(5s),
                                           partial_error) &&
              partial_cleanup.termination_calls == 1 &&
              partial_error.find("partial") != std::string::npos);
  ScopeRequestProbe invalid_cleanup;
  std::string invalid_cleanup_error;
  OMARCHY_CHECK_WITH(require, !invalid_cleanup.terminate_scope(
              "ssh.service", deadline_after(5s), invalid_cleanup_error) &&
              invalid_cleanup.termination_calls == 0 &&
              invalid_cleanup_error.find("identity") != std::string::npos);

  FakeScope worker_scope;
  std::string worker_error;
  const auto worker_deadline = deadline_after(5s);
  const auto worker_result = worker_scope.attach(
      "app-omarchy-plugin-worker-test.scope", 404, 505,
      sandbox::build_plan(), worker_deadline, worker_error);
  OMARCHY_CHECK_WITH(require, worker_result.attached && worker_result.cleanup_required &&
              worker_error.empty() &&
              worker_scope.pids == std::vector<pid_t>({404, 505}) &&
              worker_scope.description == "Omarchy sandboxed plugin worker" &&
              worker_scope.attach_deadline == worker_deadline);
}

void deadline_and_async_cleanup_test(LaunchFixture &fixture) {
  auto rejected_scope = std::make_shared<FakeScope>();
  rejected_scope->attach_succeeds = false;
  auto rejected_supervisor = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, PROBE_PATH, rejected_scope);
  const auto pre_call_rejected =
      rejected_supervisor.launch(fixture.request(), deadline_after(5s));
  OMARCHY_CHECK_WITH(require, pre_call_rejected.failure ==
              launcher::LaunchFailure::resource_scope_failed &&
              rejected_scope->termination_count == 0);

  auto scope = std::make_shared<FakeScope>();
  scope->remove_delay = 150ms;
  auto supervisor =
      launcher::test_support::make_supervisor(FAKE_BWRAP_PATH, PROBE_PATH,
                                              scope);
  const auto launch_deadline = std::chrono::steady_clock::now() + 5s;
  auto launched = supervisor.launch(fixture.request(), launch_deadline);
  OMARCHY_CHECK_WITH(require, static_cast<bool>(launched) && scope->probe_deadline == launch_deadline &&
              scope->cleanup_deadline == launch_deadline &&
              scope->cleanup_prepared &&
              scope->attach_deadline == launch_deadline);
  const auto destroy_started = std::chrono::steady_clock::now();
  launched.worker.reset();
  OMARCHY_CHECK_WITH(require, std::chrono::steady_clock::now() - destroy_started < 50ms);
  OMARCHY_CHECK_WITH(require, wait_until([&] { return scope->termination_count.load() == 1; }, 2s));

  auto delayed_setup_scope = std::make_shared<FakeScope>();
  delayed_setup_scope->cleanup_setup_delay = 1s;
  auto delayed_setup = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, PROBE_PATH, delayed_setup_scope);
  const auto setup_deadline = std::chrono::steady_clock::now() + 30ms;
  const auto setup_rejected =
      delayed_setup.launch(fixture.request(), setup_deadline);
  OMARCHY_CHECK_WITH(require, setup_rejected.failure == launcher::LaunchFailure::startup_timeout &&
              !delayed_setup_scope->cleanup_prepared &&
              !delayed_setup_scope->attach_deadline.has_value() &&
              delayed_setup_scope->termination_count == 0);

  auto expiring_scope = std::make_shared<FakeScope>();
  expiring_scope->probe_delay = 10ms;
  expiring_scope->attach_delay = 1s;
  expiring_scope->remove_delay = 150ms;
  auto expiring = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, PROBE_PATH, expiring_scope);
  const auto aggregate_deadline = std::chrono::steady_clock::now() + 40ms;
  const auto launch_started = std::chrono::steady_clock::now();
  const auto rejected = expiring.launch(fixture.request(), aggregate_deadline);
  const auto launch_elapsed = std::chrono::steady_clock::now() - launch_started;
  OMARCHY_CHECK_WITH(require, rejected.failure == launcher::LaunchFailure::startup_timeout &&
              expiring_scope->probe_deadline == aggregate_deadline &&
              expiring_scope->attach_deadline == aggregate_deadline);
  OMARCHY_CHECK_WITH(require, launch_elapsed < 100ms);
  OMARCHY_CHECK_WITH(require, wait_until(
              [&] { return expiring_scope->termination_count.load() == 1; }, 2s));

  auto retry_scope = std::make_shared<FakeScope>();
  retry_scope->termination_succeeds = false;
  auto retry_supervisor = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, PROBE_PATH, retry_scope);
  auto retry_worker =
      retry_supervisor.launch(fixture.request(), deadline_after(5s));
  OMARCHY_CHECK_WITH(require, static_cast<bool>(retry_worker));
  OMARCHY_CHECK_WITH(require, !retry_worker.worker->terminate(deadline_after(2s)) &&
              retry_scope->termination_count == 1);
  retry_scope->termination_succeeds = true;
  OMARCHY_CHECK_WITH(require, retry_worker.worker->terminate(deadline_after(2s)) &&
              retry_worker.worker->terminate(deadline_after(2s)) &&
              retry_scope->termination_count == 2);

  const auto recover_cleanup = [](const auto &failed_scope) {
    OMARCHY_CHECK_WITH(require, wait_until(
        [&] { return failed_scope->termination_count.load() >= 1; }, 2s));
    const auto attempts = failed_scope->termination_count.load();
    failed_scope->termination_succeeds = true;
    return wait_until(
        [&] { return failed_scope->termination_count.load() > attempts; }, 2s);
  };
  auto abandoned_scope = std::make_shared<FakeScope>();
  abandoned_scope->termination_succeeds = false;
  auto abandoned_supervisor = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, PROBE_PATH, abandoned_scope);
  auto abandoned_worker =
      abandoned_supervisor.launch(fixture.request(), deadline_after(5s));
  OMARCHY_CHECK_WITH(require, static_cast<bool>(abandoned_worker));
  abandoned_worker.worker.reset();
  OMARCHY_CHECK_WITH(require, recover_cleanup(abandoned_scope));
  const auto abandoned_clean = abandoned_scope->termination_count.load();
  std::this_thread::sleep_for(150ms);
  OMARCHY_CHECK_WITH(require, abandoned_scope->termination_count == abandoned_clean);

  auto failed_launch_scope = std::make_shared<FakeScope>();
  failed_launch_scope->attach_delay = 100ms;
  failed_launch_scope->termination_succeeds = false;
  auto failed_launch_supervisor = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, PROBE_PATH, failed_launch_scope);
  const auto failed_launch = failed_launch_supervisor.launch(
      fixture.request(), deadline_after(20ms));
  OMARCHY_CHECK_WITH(require, !failed_launch && recover_cleanup(failed_launch_scope));

}

void reaper_wake_and_cleanup_deadline_test() {
  using launcher::detail::CleanupJob;
  using launcher::detail::ProcessScopeReaper;
  const auto exercise = [](WakeFault fault, bool poisons_reaper) {
    wake_fault = fault;
    wake_calls = 0;
    ProcessScopeReaper reaper(false, injected_wake);
    std::string error;
    OMARCHY_CHECK_WITH(require, reaper.start(error));
    auto completion = std::make_shared<launcher::detail::ReapCompletion>();
    auto job = std::make_unique<CleanupJob>();
    job->completion = completion;
    reaper.submit(std::move(job));
    OMARCHY_CHECK_WITH(require, wait_until(
                [&] {
                  std::lock_guard lock(completion->mutex);
                  return completion->completed;
                },
                2s));
    std::string restart_error;
    OMARCHY_CHECK_WITH(require, reaper.start(restart_error) != poisons_reaper);
  };
  exercise(WakeFault::interrupt_once, false);
  OMARCHY_CHECK_WITH(require, wake_calls >= 2);
  exercise(WakeFault::saturated, false);
  exercise(WakeFault::failed, true);
  wake_fault = WakeFault::none;

  auto scope = std::make_shared<DeadlineCleanupScope>();
  ProcessScopeReaper reaper(false);
  std::string error;
  OMARCHY_CHECK_WITH(require, reaper.start(error));
  for (unsigned index = 0; index < 2; ++index) {
    auto job = std::make_unique<CleanupJob>();
    job->resource_scope = scope;
    job->scope = "app-omarchy-plugin-worker-deadline.scope";
    job->scope_attached = true;
    reaper.submit(std::move(job));
  }
  OMARCHY_CHECK_WITH(require, wait_until([&] { return scope->completed == 2; }, 500ms));

  for (const auto failure : {FailingCleanupScope::Failure::bus_loss,
                             FailingCleanupScope::Failure::partial_cleanup,
                             FailingCleanupScope::Failure::timeout}) {
    auto failed_scope = std::make_shared<FailingCleanupScope>(failure);
    auto completion = std::make_shared<launcher::detail::ReapCompletion>();
    auto job = std::make_unique<CleanupJob>();
    job->resource_scope = failed_scope;
    job->scope = "app-omarchy-plugin-worker-failed-cleanup.scope";
    job->scope_attached = true;
    job->completion = completion;
    job->timeouts.forced_teardown_seconds = 1;
    reaper.submit(std::move(job));
    OMARCHY_CHECK_WITH(require, wait_until(
                [&] {
                  std::lock_guard lock(completion->mutex);
                  return completion->failed_attempts >= 1;
                },
                2s));
    {
      std::lock_guard lock(completion->mutex);
      OMARCHY_CHECK_WITH(require, !completion->succeeded && failed_scope->calls == 1);
    }
    failed_scope->recover = true;
    OMARCHY_CHECK_WITH(require, wait_until(
                [&] {
                  std::lock_guard retry_lock(completion->mutex);
                  return completion->succeeded;
                },
                500ms));
    std::lock_guard retry_lock(completion->mutex);
    OMARCHY_CHECK_WITH(require, completion->succeeded && failed_scope->calls == 2);
  }

  auto aggregate_scope = std::make_shared<FailingCleanupScope>(
      FailingCleanupScope::Failure::timeout);
  auto aggregate_completion =
      std::make_shared<launcher::detail::ReapCompletion>();
  const pid_t stuck_monitor = fork();
  OMARCHY_CHECK_WITH(require, stuck_monitor >= 0);
  if (stuck_monitor == 0) {
    for (;;)
      pause();
  }
  const int stuck_pidfd =
      static_cast<int>(syscall(SYS_pidfd_open, stuck_monitor, 0));
  OMARCHY_CHECK_WITH(require, stuck_pidfd >= 0);
  auto aggregate_job = std::make_unique<CleanupJob>();
  aggregate_job->resource_scope = aggregate_scope;
  aggregate_job->scope = "app-omarchy-plugin-worker-aggregate.scope";
  aggregate_job->scope_attached = true;
  aggregate_job->monitor_pid = stuck_monitor;
  aggregate_job->monitor_pidfd = stuck_pidfd;
  aggregate_job->completion = aggregate_completion;
  aggregate_job->timeouts.forced_teardown_seconds = 1;
  const auto aggregate_started = std::chrono::steady_clock::now();
  reaper.submit(std::move(aggregate_job));
  OMARCHY_CHECK_WITH(require, wait_until(
              [&] {
                std::lock_guard lock(aggregate_completion->mutex);
                return aggregate_completion->failed_attempts >= 1;
              },
              1600ms) &&
              std::chrono::steady_clock::now() - aggregate_started < 1500ms);
  aggregate_scope->recover = true;
  OMARCHY_CHECK_WITH(require, wait_until(
              [&] {
                std::lock_guard lock(aggregate_completion->mutex);
                return aggregate_completion->succeeded;
              },
              2s));
}

void nonblocking_bus_connection_test() {
  support::TemporaryDirectory tree;
  const auto socket_path = tree.path() / "hung-bus";
  support::UniqueFd listener(
      socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  OMARCHY_CHECK_WITH(require, static_cast<bool>(listener));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  OMARCHY_CHECK_WITH(require, socket_path.string().size() < sizeof(address.sun_path));
  std::memcpy(address.sun_path, socket_path.c_str(),
              socket_path.string().size() + 1);
  OMARCHY_CHECK_WITH(require, bind(listener.get(), reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) == 0 &&
              listen(listener.get(), 2) == 0);

  std::atomic<unsigned> accepted = 0;
  std::atomic<unsigned> closed = 0;
  std::thread server([&] {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      support::UniqueFd peer(
          accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
      if (!peer)
        return;
      accepted.fetch_add(1);
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (std::chrono::steady_clock::now() < deadline) {
        pollfd event{.fd = peer.get(), .events = POLLIN, .revents = 0};
        if (poll(&event, 1, 50) <= 0)
          continue;
        std::array<std::byte, 64> bytes{};
        const ssize_t received = recv(peer.get(), bytes.data(), bytes.size(), 0);
        if (received == 0) {
          closed.fetch_add(1);
          break;
        }
        if (received < 0 && errno != EINTR)
          break;
      }
    }
  });

  const std::string bus_address = "unix:path=" + socket_path.string();
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    OMARCHY_CHECK_WITH(require, !launcher::test_support::connect_bus(
                bus_address, started + 60ms, error) &&
                std::chrono::steady_clock::now() - started < 150ms &&
                error == "bus connection deadline expired");
  }
  server.join();
  OMARCHY_CHECK_WITH(require, accepted == 2 && closed == 2);
}

void reaper_capacity_and_startup_test(LaunchFixture &fixture) {
  auto failed_scope = std::make_shared<FakeScope>();
  auto failed = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, PROBE_PATH, failed_scope, true);
  const auto rejected = failed.launch(fixture.request(), deadline_after(5s));
  OMARCHY_CHECK_WITH(require, rejected.failure ==
              launcher::LaunchFailure::resource_scope_unavailable &&
              !failed_scope->probe_deadline.has_value());

  using launcher::detail::CleanupJob;
  using launcher::detail::ProcessScopeReaper;
  constexpr std::size_t job_count = 5000;
  auto scope = std::make_shared<BlockingCleanupScope>();
  ProcessScopeReaper reaper(false);
  std::string error;
  OMARCHY_CHECK_WITH(require, reaper.start(error));
  std::vector<std::unique_ptr<CleanupJob>> jobs;
  jobs.reserve(job_count);
  for (std::size_t index = 0; index < job_count; ++index) {
    auto job = std::make_unique<CleanupJob>();
    job->resource_scope = scope;
    job->scope = "app-omarchy-plugin-worker-capacity.scope";
    job->scope_attached = true;
    jobs.push_back(std::move(job));
  }
  reaper.submit(std::move(jobs.front()));
  OMARCHY_CHECK_WITH(require, scope->gate.wait_entered_for(2s) && scope->entered == 1);
  constexpr std::size_t producer_count = 8;
  std::array<std::thread, producer_count> producers;
  for (std::size_t producer = 0; producer < producer_count; ++producer) {
    producers[producer] = std::thread([&, producer] {
      for (std::size_t index = 1 + producer; index < job_count;
           index += producer_count)
        reaper.submit(std::move(jobs[index]));
    });
  }
  for (auto &producer : producers) producer.join();
  scope->gate.release();
  OMARCHY_CHECK_WITH(require, wait_until([&] { return scope->completed == job_count; }, 5s));
}

struct LaunchedProbe {
  std::shared_ptr<FakeScope> scope = std::make_shared<FakeScope>();
  launcher::Supervisor supervisor = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, PROBE_PATH, scope);
  launcher::LaunchResult launched;

  explicit LaunchedProbe(LaunchFixture &fixture)
      : launched(supervisor.launch(fixture.request(), deadline_after(5s))) {}
};

void owned_descriptor_transport_test(LaunchFixture &fixture) {
  auto [scope, supervisor, launched] = LaunchedProbe(fixture);
  OMARCHY_CHECK_WITH(require, static_cast<bool>(launched));
  pollfd readiness{.fd = launched.worker->readiness_fd(),
                   .events = POLLIN,
                   .revents = 0};
  OMARCHY_CHECK_WITH(require, readiness.fd >= 0 && poll(&readiness, 1, 2000) == 1 &&
              (readiness.revents & POLLIN) != 0);
  const auto probe_message = launched.worker->receive_any(
      launcher::PacketSizeLimit{sizeof(Probe)},
      deadline_after(2s),
      launcher::EndpointMask::control);
  OMARCHY_CHECK_WITH(require, probe_message &&
              decode<Probe>(probe_message.payload).pid ==
                  launched.worker->identity().outer_worker_pid);

  auto descriptor_message = launched.worker->receive_any(
      launcher::PacketSizeLimit{16}, deadline_after(2s), launcher::EndpointMask::broker);
  const int flags = descriptor_message.descriptors.size() == 1
      ? fcntl(descriptor_message.descriptors.front().get(), F_GETFD) : -1;
  require(descriptor_message && descriptor_message.role == launcher::EndpointRole::broker &&
              flags >= 0 && (flags & FD_CLOEXEC) != 0,
          "owned broker descriptor: status=" + std::to_string(static_cast<unsigned>(descriptor_message.status)) +
          " failure=" + std::to_string(static_cast<unsigned>(descriptor_message.failure)) +
          " count=" + std::to_string(descriptor_message.descriptors.size()) +
          " flags=" + std::to_string(flags));
  const auto descriptors_before = open_descriptor_count();
  auto malformed = launched.worker->receive_any(
      launcher::PacketSizeLimit{16},
      deadline_after(2s),
      launcher::EndpointMask::render);
  OMARCHY_CHECK_WITH(require, malformed.failure == launcher::ReceiveFailure::truncated &&
              malformed.descriptors.empty() &&
              open_descriptor_count() == descriptors_before);

  const std::array acknowledgement{std::byte{1}};
  std::array<int, launcher::kMaximumTransportDescriptors> exact_descriptors{};
  exact_descriptors.fill(descriptor_message.descriptors.front().get());
  std::array<int, launcher::kMaximumTransportDescriptors + 1>
      oversized_descriptors{};
  oversized_descriptors.fill(descriptor_message.descriptors.front().get());
  OMARCHY_CHECK_WITH(require, launched.worker->try_send(launcher::EndpointRole::broker,
                                    acknowledgement,
                                    launcher::PacketSizeLimit{1},
                                    exact_descriptors) ==
                  launcher::SendStatus::complete &&
              launched.worker->try_send(launcher::EndpointRole::broker,
                                        acknowledgement,
                                        launcher::PacketSizeLimit{1},
                                        oversized_descriptors) ==
                  launcher::SendStatus::fatal);
  OMARCHY_CHECK_WITH(require, launched.worker->try_send(launcher::EndpointRole::control,
                                    acknowledgement,
                                    launcher::PacketSizeLimit{1}) ==
                  launcher::SendStatus::complete &&
              launched.worker->terminate(deadline_after(2s)) &&
              launched.worker->terminate(
                  std::chrono::steady_clock::now()) &&
              launched.worker->terminate(deadline_after(2s)));
}

void pidfd_priority_test(LaunchFixture &fixture) {
  auto [scope, supervisor, launched] = LaunchedProbe(fixture);
  OMARCHY_CHECK_WITH(require, static_cast<bool>(launched));
  support::UniqueFd exit_probe(static_cast<int>(
      syscall(SYS_pidfd_open, launched.worker->identity().outer_worker_pid, 0)));
  OMARCHY_CHECK_WITH(require, static_cast<bool>(exit_probe));
  const std::array acknowledgement{std::byte{1}};
  OMARCHY_CHECK_WITH(require, launched.worker->try_send(launcher::EndpointRole::control,
                                    acknowledgement,
                                    launcher::PacketSizeLimit{1}) ==
              launcher::SendStatus::complete);
  OMARCHY_CHECK_WITH(require, wait_until([&] { return !launched.worker->alive(); }, 2s));
  const auto result = launched.worker->receive_any(
      launcher::PacketSizeLimit{sizeof(Probe)}, deadline_after(2s));
  OMARCHY_CHECK_WITH(require, result.status == launcher::ReceiveStatus::peer_closed &&
              result.failure == launcher::ReceiveFailure::worker_exited &&
              result.payload.empty() && result.descriptors.empty());
  OMARCHY_CHECK_WITH(require, wait_until([&] {
    pollfd reaped{.fd = exit_probe.get(), .events = POLLIN, .revents = 0};
    return poll(&reaped, 1, 0) == 1 && reaped.revents == (POLLIN | POLLHUP);
  }, 2s));
  OMARCHY_CHECK_WITH(require, launched.worker->try_send(launcher::EndpointRole::control,
                                    acknowledgement,
                                    launcher::PacketSizeLimit{1}) ==
              launcher::SendStatus::peer_closed);
  OMARCHY_CHECK_WITH(require, launched.worker->terminate(deadline_after(5s)) &&
              scope->termination_count == 1);
}

void readiness_control_failure_test(LaunchFixture &fixture) {
  auto [scope, supervisor, launched] = LaunchedProbe(fixture);
  OMARCHY_CHECK_WITH(require, static_cast<bool>(launched));
  const int borrowed_readiness = launched.worker->readiness_fd();
  OMARCHY_CHECK_WITH(require, borrowed_readiness >= 0 && close(borrowed_readiness) == 0);
  OMARCHY_CHECK_WITH(require, !launched.worker->set_readiness_interests(
              {.read = launcher::EndpointMask::all,
               .write = launcher::EndpointMask::broker}) &&
              !launched.worker->alive() &&
              launched.worker->terminate(deadline_after(2s)) &&
              scope->termination_count == 1);
}

void fake_bwrap_alias_test() {
  struct stat source{};
  OMARCHY_CHECK_WITH(require, lstat(FAKE_BWRAP_PATH, &source) == 0 &&
              S_ISREG(source.st_mode) && !S_ISLNK(source.st_mode) &&
              source.st_uid == geteuid());
  for (const char *alias : {DUPLICATE_STATUS_BWRAP_PATH,
                            STRING_STATUS_BWRAP_PATH,
                            EXITED_STATUS_BWRAP_PATH}) {
    struct stat metadata{};
    OMARCHY_CHECK_WITH(require, lstat(alias, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
                !S_ISLNK(metadata.st_mode) && metadata.st_dev == source.st_dev &&
                metadata.st_ino == source.st_ino &&
                metadata.st_uid == source.st_uid &&
                metadata.st_mode == source.st_mode);
  }
}

void contract_test() {
  for (const std::string_view status : {
           R"({"child-pid":1,"exit-code":0})",
           R"({"child\u002dpid":1,"exit\u002dcode":0})",
           R"({"future":[{"child-pid":1,"child-pid":2},{"exit-code":0,"exit-code":1}],"child-pid":3})",
           R"({"future":1,"future":2,"text":"\"child-pid\": [{}]","child-pid":3})"})
    OMARCHY_CHECK_WITH(require, launcher::detail::unique_authoritative_keys(status));
  for (const std::string_view status : {
           R"({"child-pid":1,"child-pid":2})",
           R"({"child-pid":1,"child\u002dpid":2})",
           R"({"exit-code":0,"exit-code":1})",
           R"({"exit-code":0,"exit\u002dcode":1})",
           R"({"child-pid":1,"future":"\ud800"})",
           R"({"child-pid":1,"future":"\q"})",
           "{\"child-pid\":1,\"future\":\"\x01\"}",
           R"({"child-pid":1,})", R"({"child-pid":1} {})"})
    OMARCHY_CHECK_WITH(require, !launcher::detail::unique_authoritative_keys(status));
  pidfd_reap_state_test();
  fake_bwrap_alias_test();
  auto scope = std::make_shared<FakeScope>();
  auto supervisor =
      launcher::test_support::make_supervisor(FAKE_BWRAP_PATH, PROBE_PATH,
                                              scope);
  LaunchFixture fixture;

  for (const auto &[plugin, generation] :
       std::array<std::pair<std::string, std::uint64_t>, 3>{{
           {"../forged", fixture.request().generation},
           {"1.invalid", fixture.request().generation},
           {fixture.request().plugin_id, 0}}}) {
    auto invalid = fixture.request();
    invalid.plugin_id = plugin;
    invalid.generation = generation;
    OMARCHY_CHECK_WITH(require, supervisor.launch(invalid, deadline_after(5s)).failure ==
                launcher::LaunchFailure::invalid_trusted_record);
  }

  auto unavailable_scope = std::make_shared<FakeScope>();
  unavailable_scope->available = false;
  auto unavailable = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, PROBE_PATH, unavailable_scope);
  OMARCHY_CHECK_WITH(require, unavailable.launch(fixture.request(), deadline_after(5s)).failure ==
              launcher::LaunchFailure::missing_kernel_prerequisite);

  for (const auto &[path, expected] : {
           std::pair{DUPLICATE_STATUS_BWRAP_PATH, launcher::LaunchFailure::status_protocol_failed},
           std::pair{STRING_STATUS_BWRAP_PATH, launcher::LaunchFailure::status_protocol_failed},
           std::pair{EXITED_STATUS_BWRAP_PATH, launcher::LaunchFailure::worker_exited_early}}) {
    auto invalid_status = launcher::test_support::make_supervisor(
        path, PROBE_PATH, std::make_shared<FakeScope>());
    const auto result = invalid_status.launch(fixture.request(), deadline_after(5s));
    require(result.failure == expected,
            std::string(path) + " failure=" + std::to_string(static_cast<int>(result.failure)) +
                " detail=" + result.detail);
  }

  const auto invalid_executable = fixture.tree.path() / "invalid-bwrap";
  std::ofstream(invalid_executable) << "not an executable format\n";
  OMARCHY_CHECK_WITH(require, chmod(invalid_executable.c_str(), 0700) == 0);
  auto exec_error = launcher::test_support::make_supervisor(
      invalid_executable.string(), PROBE_PATH, std::make_shared<FakeScope>());
  const auto failed_exec =
      exec_error.launch(fixture.request(), deadline_after(5s));
  OMARCHY_CHECK_WITH(require, failed_exec.failure == launcher::LaunchFailure::exec_failed);

  owned_descriptor_transport_test(fixture);
  pidfd_priority_test(fixture);
  readiness_control_failure_test(fixture);
  deadline_and_async_cleanup_test(fixture);
  reaper_wake_and_cleanup_deadline_test();
  reaper_capacity_and_startup_test(fixture);
}

void malicious_test() {
  auto scope = std::make_shared<FakeScope>();
  auto supervisor = launcher::test_support::make_supervisor(
      FAKE_BWRAP_PATH, MALICIOUS_PROBE_PATH, scope);
  LaunchFixture fixture;
  auto launched = supervisor.launch(fixture.request(), deadline_after(5s));
  OMARCHY_CHECK_WITH(require, static_cast<bool>(launched) && scope->attached);
  OMARCHY_CHECK_WITH(require,
      launched.worker
              ->receive_any(
                  launcher::PacketSizeLimit{16},
                  std::chrono::steady_clock::now(),
                  static_cast<launcher::EndpointMask>(99))
              .failure == launcher::ReceiveFailure::invalid_role);

  const auto allowed = launcher::EndpointMask::control |
                       launcher::EndpointMask::render;
  OMARCHY_CHECK_WITH(require, launched.worker->set_readiness_interests(
              {.read = allowed, .write = launcher::EndpointMask::none}));
  OMARCHY_CHECK_WITH(require, await_readable_lanes(*launched.worker, allowed));
  const auto control = launched.worker->try_receive_any(
      launcher::PacketSizeLimit{sizeof(Claim)}, allowed);
  OMARCHY_CHECK_WITH(require, static_cast<bool>(control) &&
              control.role == launcher::EndpointRole::control &&
              decode<Claim>(control.payload).claimed_pid ==
                  launched.worker->identity().outer_worker_pid);
  OMARCHY_CHECK_WITH(require, await_readable_lanes(*launched.worker,
                               launcher::EndpointMask::render));
  const auto render = launched.worker->try_receive_any(
      launcher::PacketSizeLimit{sizeof(Claim)}, allowed);
  OMARCHY_CHECK_WITH(require, static_cast<bool>(render) &&
              render.role == launcher::EndpointRole::render);
  const auto empty = launched.worker->try_receive_any(
      launcher::PacketSizeLimit{sizeof(Claim)}, allowed);
  OMARCHY_CHECK_WITH(require, empty.status == launcher::ReceiveStatus::would_block &&
              empty.failure == launcher::ReceiveFailure::none &&
              empty.payload.empty() && empty.descriptors.empty());

  OMARCHY_CHECK_WITH(require, launched.worker->set_readiness_interests(
              {.read = allowed, .write = launcher::EndpointMask::broker}));
  epoll_event ready{};
  OMARCHY_CHECK_WITH(require, epoll_wait(launched.worker->readiness_fd(), &ready, 1, 1000) == 1 &&
              ready.data.u64 == 1 && (ready.events & EPOLLOUT) != 0 &&
              (ready.events & EPOLLIN) == 0);
  OMARCHY_CHECK_WITH(require, launched.worker->set_readiness_interests(
              {.read = allowed, .write = launcher::EndpointMask::control}));
  ready = {};
  OMARCHY_CHECK_WITH(require, epoll_wait(launched.worker->readiness_fd(), &ready, 1, 1000) == 1 &&
              ready.data.u64 == 0 && (ready.events & EPOLLOUT) != 0);
  OMARCHY_CHECK_WITH(require, launched.worker->set_readiness_interests(
              {.read = allowed, .write = launcher::EndpointMask::none}));
  struct ReceiveCase {
    std::size_t limit;
    launcher::EndpointMask lanes;
    launcher::ReceiveFailure expected;
  };
  const auto check_receive = [&](ReceiveCase test) {
    OMARCHY_CHECK_WITH(require, launched.worker->receive_any(
        launcher::PacketSizeLimit{test.limit}, std::chrono::steady_clock::now(),
        test.lanes).failure == test.expected);
  };
  for (const auto &test : std::array{
           ReceiveCase{0, allowed, launcher::ReceiveFailure::invalid_role},
           ReceiveCase{launcher::kTransportPacketHardLimit + 1, allowed, launcher::ReceiveFailure::invalid_role},
           ReceiveCase{sizeof(Claim), launcher::EndpointMask::broker, launcher::ReceiveFailure::invalid_role}})
    check_receive(test);
  OMARCHY_CHECK_WITH(require, launched.worker->receive_any(
      launcher::PacketSizeLimit{sizeof(Claim)}, std::chrono::steady_clock::now())
      .failure == launcher::ReceiveFailure::timeout);
  const auto masked = launched.worker->receive_any(
      launcher::PacketSizeLimit{sizeof(Claim)},
      deadline_after(10ms),
      launcher::EndpointMask::broker);
  OMARCHY_CHECK_WITH(require, masked.failure == launcher::ReceiveFailure::invalid_role);
  pollfd disabled_broker{.fd = launched.worker->readiness_fd(),
                         .events = POLLIN,
                         .revents = 0};
  OMARCHY_CHECK_WITH(require, poll(&disabled_broker, 1, 0) == 0);
  OMARCHY_CHECK_WITH(require, launched.worker->set_readiness_interests(
              {.read = launcher::EndpointMask::broker,
               .write = launcher::EndpointMask::none}));
  disabled_broker.revents = 0;
  OMARCHY_CHECK_WITH(require, poll(&disabled_broker, 1, 1000) == 1 &&
              (disabled_broker.revents & POLLIN) != 0);
  const auto descendant = launched.worker->receive_any(
      launcher::PacketSizeLimit{sizeof(Claim)},
      deadline_after(2s),
      launcher::EndpointMask::broker);
  OMARCHY_CHECK_WITH(require, descendant.failure == launcher::ReceiveFailure::credential_mismatch);
  constexpr std::size_t probe_broker_datagram_size = 40 + 65536;
  const auto maximum_broker = launched.worker->receive_any(
      launcher::PacketSizeLimit{launcher::kTransportPacketHardLimit},
      deadline_after(2s),
      launcher::EndpointMask::broker);
  OMARCHY_CHECK_WITH(require, static_cast<bool>(maximum_broker) &&
              maximum_broker.payload.size() == probe_broker_datagram_size);
  check_receive({launcher::kTransportPacketHardLimit, launcher::EndpointMask::broker,
                 launcher::ReceiveFailure::timeout});
  check_receive({launcher::kTransportPacketHardLimit + 1, launcher::EndpointMask::broker,
                 launcher::ReceiveFailure::invalid_role});
  std::vector<std::byte> maximum_packet(launcher::kTransportPacketHardLimit,
                                        std::byte{0x32});
  std::vector<std::byte> oversized_packet(
      launcher::kTransportPacketHardLimit + 1, std::byte{0x33});
  OMARCHY_CHECK_WITH(require, launched.worker->try_send(
              launcher::EndpointRole::broker, maximum_packet,
              launcher::PacketSizeLimit{launcher::kTransportPacketHardLimit}) ==
                  launcher::SendStatus::complete);
  struct SendCase { std::span<const std::byte> packet; std::size_t limit; };
  for (const auto &test : std::array{
           SendCase{maximum_packet, launcher::kTransportPacketHardLimit - 1},
           SendCase{oversized_packet, launcher::kTransportPacketHardLimit},
           SendCase{std::span(maximum_packet).first(1), launcher::kTransportPacketHardLimit + 1}})
    OMARCHY_CHECK_WITH(require, launched.worker->try_send(
        launcher::EndpointRole::broker, test.packet, launcher::PacketSizeLimit{test.limit}) ==
        launcher::SendStatus::fatal);
  const std::array acknowledgement{std::byte{1}};
  support::UniqueFd passed(open("/dev/null", O_RDONLY | O_CLOEXEC));
  const std::array one{passed.get()};
  launcher::SendStatus saturated = launcher::SendStatus::complete;
  for (std::size_t attempts = 0;
       attempts < 100000 && saturated == launcher::SendStatus::complete;
       ++attempts) {
    saturated = launched.worker->try_send(launcher::EndpointRole::broker,
                                          acknowledgement,
                                          launcher::PacketSizeLimit{1}, one);
  }
  OMARCHY_CHECK_WITH(require, saturated == launcher::SendStatus::would_block && passed &&
              fcntl(passed.get(), F_GETFD) >= 0);
  OMARCHY_CHECK_WITH(require, launched.worker->try_send(launcher::EndpointRole::control,
                                    acknowledgement,
                                    launcher::PacketSizeLimit{1}) ==
              launcher::SendStatus::complete);
  OMARCHY_CHECK_WITH(require, passed &&
              launched.worker->try_send(launcher::EndpointRole::control,
                                        acknowledgement,
                                        launcher::PacketSizeLimit{1}, one) ==
                  launcher::SendStatus::complete);
  std::array<int, 17> excess{};
  excess.fill(passed.get());
  OMARCHY_CHECK_WITH(require, launched.worker->try_send(launcher::EndpointRole::control,
                                    acknowledgement,
                                    launcher::PacketSizeLimit{1}, excess) ==
                  launcher::SendStatus::fatal &&
              fcntl(passed.get(), F_GETFD) >= 0);
  OMARCHY_CHECK_WITH(require, launched.worker->set_readiness_interests(
              {.read = launcher::EndpointMask::render,
               .write = launcher::EndpointMask::none}));
  const auto descriptor_report = launched.worker->receive_any(
      launcher::PacketSizeLimit{sizeof(DescriptorReport)},
      deadline_after(2s),
      launcher::EndpointMask::render);
  OMARCHY_CHECK_WITH(require, static_cast<bool>(descriptor_report));
  const auto report = decode<DescriptorReport>(descriptor_report.payload);
  OMARCHY_CHECK_WITH(require, report.count == 1 && report.close_on_exec == 1);
  OMARCHY_CHECK_WITH(require, launched.worker->set_readiness_interests(
              {.read = launcher::EndpointMask::none,
               .write = launcher::EndpointMask::none}));
  pollfd exit_readiness{.fd = launched.worker->readiness_fd(),
                        .events = POLLIN,
                        .revents = 0};
  OMARCHY_CHECK_WITH(require, poll(&exit_readiness, 1, 2000) == 1 &&
              (exit_readiness.revents & POLLIN) != 0 &&
              launched.worker
                      ->receive_any(
                          launcher::PacketSizeLimit{1},
                          std::chrono::steady_clock::now() + 2s,
                          launcher::EndpointMask::none)
                      .failure == launcher::ReceiveFailure::worker_exited);

  launcher::Worker active_worker(std::move(*launched.worker));
  OMARCHY_CHECK_WITH(require, launched.worker
                  ->receive_any(launcher::PacketSizeLimit{1},
                                std::chrono::steady_clock::now())
                  .failure == launcher::ReceiveFailure::invalid_role &&
              launched.worker->try_send(launcher::EndpointRole::control,
                                        acknowledgement,
                                        launcher::PacketSizeLimit{1}) ==
                  launcher::SendStatus::peer_closed);
  OMARCHY_CHECK_WITH(require, active_worker.terminate(deadline_after(5s)) &&
              scope->termination_count == 1);
}

void bwrap_test() {
  if (access(BWRAP_PATH, X_OK) < 0) {
    std::cerr << "Bubblewrap unavailable; launcher integration skipped\n";
    std::exit(77);
  }
  auto scope = std::make_shared<FakeScope>();
  auto supervisor = launcher::test_support::make_supervisor(
      BWRAP_PATH, PROBE_PATH, scope);
  LaunchFixture fixture;
  auto launched = supervisor.launch(fixture.request(), deadline_after(5s));
  if (!launched &&
      (launched.detail.find("Operation not permitted") != std::string::npos ||
       launched.detail.find("SO_PASSCRED") != std::string::npos)) {
    std::cerr << "Outer sandbox denied kernel launch proof; skipped\n";
    std::exit(77);
  }
  OMARCHY_CHECK_WITH(require, static_cast<bool>(launched) && scope->attached_before_release);
  validate_probe(*launched.worker);
  const auto injection = launched.worker->receive_any(
      launcher::PacketSizeLimit{16},
      deadline_after(2s),
      launcher::EndpointMask::broker);
  OMARCHY_CHECK_WITH(require, injection && injection.descriptors.size() == 1 &&
              (fcntl(injection.descriptors.front().get(), F_GETFD) &
               FD_CLOEXEC) != 0);
  const auto descriptors_before = open_descriptor_count();
  const auto truncated_ancillary =
      launched.worker->receive_any(
          launcher::PacketSizeLimit{16},
          deadline_after(2s),
          launcher::EndpointMask::render);
  const auto descriptors_after = open_descriptor_count();
  OMARCHY_CHECK_WITH(require, truncated_ancillary.failure == launcher::ReceiveFailure::truncated &&
              descriptors_after == descriptors_before);
  const std::array acknowledgement{std::byte{1}};
  OMARCHY_CHECK_WITH(require, launched.worker->try_send(launcher::EndpointRole::control,
                                    acknowledgement,
                                    launcher::PacketSizeLimit{1}) ==
                  launcher::SendStatus::complete &&
              launched.worker->terminate(deadline_after(5s)) &&
              scope->termination_count == 1);
}

std::string read_one_line(const std::filesystem::path &path) {
  std::ifstream stream(path);
  std::string line;
  std::getline(stream, line);
  if (!stream && !stream.eof()) {
    fail("cannot read enforced cgroup value: " + path.string());
  }
  return line;
}

void systemd_scope_test() {
  if (access(BWRAP_PATH, X_OK) < 0) {
    std::cerr << "Bubblewrap unavailable; systemd scope test skipped\n";
    std::exit(77);
  }
  const auto descriptors_before = support::open_fd_set();
  auto resource_scope = launcher::make_systemd_resource_scope_controller();
  auto supervisor = launcher::test_support::make_supervisor(
      BWRAP_PATH, PROBE_PATH, resource_scope);
  LaunchFixture fixture;
  auto launched = supervisor.launch(fixture.request(), deadline_after(5s));
  if (!launched &&
      (launched.failure == launcher::LaunchFailure::resource_scope_failed ||
       launched.failure ==
           launcher::LaunchFailure::missing_kernel_prerequisite)) {
    std::cerr << "systemd user scope unavailable: " << launched.detail << '\n';
    std::exit(77);
  }
  OMARCHY_CHECK_WITH(require, static_cast<bool>(launched));
  validate_probe(*launched.worker);

  const auto cgroup_file =
      std::filesystem::path("/proc") /
      std::to_string(launched.worker->identity().outer_worker_pid) / "cgroup";
  const std::string cgroup_record = read_one_line(cgroup_file);
  const auto separator = cgroup_record.find("::");
  OMARCHY_CHECK_WITH(require, separator != std::string::npos);
  const std::string cgroup_path = cgroup_record.substr(separator + 2);
  OMARCHY_CHECK_WITH(require, cgroup_path.find("app-omarchy-plugin-worker-") != std::string::npos);
  const std::filesystem::path cgroup_root =
      std::filesystem::path("/sys/fs/cgroup") /
      cgroup_path.substr(cgroup_path.starts_with('/') ? 1 : 0);
  OMARCHY_CHECK_WITH(require, read_one_line(cgroup_root / "memory.high") == "402653184" &&
              read_one_line(cgroup_root / "memory.max") == "536870912" &&
              read_one_line(cgroup_root / "pids.max") == "16" &&
              read_one_line(cgroup_root / "cpu.weight") == "20" &&
              read_one_line(cgroup_root / "cpu.max").starts_with("50000 "));
  const auto io_weight = cgroup_root / "io.weight";
  if (std::filesystem::exists(io_weight)) {
    OMARCHY_CHECK_WITH(require, read_one_line(io_weight) == "default 10");
  } else {
    std::cerr << "io controller is not delegated on this host; IOWeight "
                 "enforcement remains a VM gate\n";
  }

  const auto injection = launched.worker->receive_any(
      launcher::PacketSizeLimit{16},
      deadline_after(2s),
      launcher::EndpointMask::broker);
  OMARCHY_CHECK_WITH(require, injection && injection.descriptors.size() == 1);
  const std::array acknowledgement{std::byte{1}};
  OMARCHY_CHECK_WITH(require, launched.worker->try_send(launcher::EndpointRole::control,
                                    acknowledgement,
                                    launcher::PacketSizeLimit{1}) ==
                  launcher::SendStatus::complete &&
              launched.worker->terminate(deadline_after(5s)));

  std::vector<int> bus_sockets;
  for (const int descriptor : support::open_fd_set()) {
    if (std::ranges::find(descriptors_before, descriptor) !=
        descriptors_before.end())
      continue;
    int type = 0;
    socklen_t size = sizeof(type);
    if (getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &size) == 0 &&
        type == SOCK_STREAM)
      bus_sockets.push_back(descriptor);
  }
  OMARCHY_CHECK_WITH(require, bus_sockets.size() >= 2);
  // probe() opens the primary bus before prepare_cleanup() opens its independent
  // cleanup bus, so the lower exact descriptor is the primary connection.
  OMARCHY_CHECK_WITH(require, shutdown(*std::ranges::min_element(bus_sockets), SHUT_RDWR) == 0);

  auto reconnected =
      supervisor.launch(fixture.request(), deadline_after(5s));
  OMARCHY_CHECK_WITH(require, static_cast<bool>(reconnected));
  validate_probe(*reconnected.worker);
  OMARCHY_CHECK_WITH(require, reconnected.worker->try_send(launcher::EndpointRole::control,
                                       acknowledgement,
                                       launcher::PacketSizeLimit{1}) ==
                  launcher::SendStatus::complete &&
              reconnected.worker->terminate(deadline_after(5s)));
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    return 64;
  }
  const std::string_view mode(argv[1]);
  if (mode == "scope") {
    generic_process_scope_request_test();
    reaper_wake_and_cleanup_deadline_test();
    nonblocking_bus_connection_test();
  } else if (mode == "contract") {
    contract_test();
  } else if (mode == "malicious") {
    malicious_test();
  } else if (mode == "bwrap") {
    bwrap_test();
  } else if (mode == "systemd") {
    systemd_scope_test();
  } else {
    return 64;
  }
  return 0;
}
