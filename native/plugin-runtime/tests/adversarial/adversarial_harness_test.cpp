#include "../support/probe_reports.hpp"
#include "../../tests/support/test_assert.hpp"
#include "../../tests/support/temporary_directory.hpp"

#include "omarchy/plugin_runtime/unique_fd.hpp"
#include "omarchy/plugin_runtime/launcher/launcher.h"
#include "omarchy/plugin_runtime/launcher/test_supervisor.h"
#include "omarchy/plugin_runtime/sandbox/policy.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace launcher = omarchy::plugin_runtime::launcher;
namespace sandbox = omarchy::plugin_runtime::sandbox;


using omarchy::plugin_runtime::test_support::require;
using omarchy::plugin_runtime::UniqueFd;

class FakeScope final : public launcher::test_support::ReadyScope {
public:

  AttachResult attach(std::string_view unit, pid_t monitor_pid,
                      pid_t worker_pid, const sandbox::SandboxPlan &plan,
                      launcher::Deadline deadline, std::string &) override {
    OMARCHY_CHECK(unit.starts_with("app-omarchy-plugin-worker-") &&
                monitor_pid > 0 && worker_pid > 0 &&
                plan.worker_descriptors == std::vector<int>({3, 4, 5}) &&
                plan.process.descendants_permitted &&
                deadline > std::chrono::steady_clock::now());
    unit_ = unit;
    attached = true;
    return {.attached = true, .cleanup_required = true};
  }

  bool terminate_scope_validated(std::string_view unit, launcher::Deadline,
                                  std::string &) noexcept override {
    if (unit == unit_) {
      ++terminations;
    }
    return true;
  }

  bool attached = false;
  unsigned terminations = 0;

private:
  std::string unit_;
};

class SandboxTree : public omarchy::plugin_runtime::test_support::TemporaryDirectory {
public:
  SandboxTree() {
    std::filesystem::create_directory(revision());
    std::filesystem::create_directory(state());
    const char *home = std::getenv("HOME");
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    const char *wayland = std::getenv("WAYLAND_DISPLAY");
    OMARCHY_CHECK(home != nullptr && *home != '\0' && runtime != nullptr &&
                *runtime != '\0' && wayland != nullptr && *wayland != '\0');
    host_home_ = home;
    bus_socket_path_ = std::filesystem::path(runtime) / "bus";
    wayland_socket_path_ = std::filesystem::path(runtime) / wayland;
    agent_socket_path_ = root_ / "agent.sock";
    other_plugin_state_path_ = root_ / "other-plugin-state" / "secret";
    std::filesystem::create_directories(other_plugin_state_path_.parent_path());
    std::ofstream(other_plugin_state_path_) << "other-plugin-private-state\n";
    agent_socket_.reset(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    OMARCHY_CHECK(agent_socket_.get() >= 0);
    sockaddr_un agent_address{};
    agent_address.sun_family = AF_UNIX;
    OMARCHY_CHECK(agent_socket_path_.string().size() < sizeof(agent_address.sun_path));
    std::strcpy(agent_address.sun_path, agent_socket_path_.c_str());
    OMARCHY_CHECK(bind(agent_socket_.get(),
                 reinterpret_cast<const sockaddr *>(&agent_address),
                 sizeof(agent_address)) == 0);
    struct stat home_status {};
    struct stat bus_status {};
    struct stat wayland_status {};
    struct stat agent_status {};
    OMARCHY_CHECK(lstat(host_home_.c_str(), &home_status) == 0 &&
                S_ISDIR(home_status.st_mode) &&
                lstat(bus_socket_path_.c_str(), &bus_status) == 0 &&
                S_ISSOCK(bus_status.st_mode) &&
                lstat(wayland_socket_path_.c_str(), &wayland_status) == 0 &&
                S_ISSOCK(wayland_status.st_mode) &&
                lstat(agent_socket_path_.c_str(), &agent_status) == 0 &&
                S_ISSOCK(agent_status.st_mode));
    std::ofstream(revision() / "fixture")
        << host_home_.string() << '\n'
        << bus_socket_path_.string() << '\n'
        << wayland_socket_path_.string() << '\n'
        << agent_socket_path_.string() << '\n'
        << other_plugin_state_path_.string() << '\n';
    const int host_write =
        open((revision() / "fixture").c_str(), O_WRONLY | O_CLOEXEC);
    OMARCHY_CHECK(host_write >= 0);
    close(host_write);
    OMARCHY_CHECK(chmod(revision().c_str(), 0555) == 0);
  }

  [[nodiscard]] std::filesystem::path revision() const {
    return root_ / "revision";
  }
  [[nodiscard]] std::filesystem::path state() const { return root_ / "state"; }

private:
  std::filesystem::path host_home_;
  std::filesystem::path bus_socket_path_;
  std::filesystem::path wayland_socket_path_;
  std::filesystem::path agent_socket_path_;
  std::filesystem::path other_plugin_state_path_;
  UniqueFd agent_socket_;
};

using omarchy::plugin_runtime::test_support::SandboxProbe;

void test_standalone_sandbox() {
  OMARCHY_CHECK(access("/usr/bin/bwrap", X_OK) == 0);
  SandboxTree tree;
  UniqueFd revision(
      open(tree.revision().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  UniqueFd state(open(tree.state().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  OMARCHY_CHECK(revision.get() >= 0 && state.get() >= 0);
  auto scope = std::make_shared<FakeScope>();
  auto supervisor = launcher::test_support::make_supervisor(
      "/usr/bin/bwrap", MALICIOUS_WORKER_PATH, scope);
  const launcher::TrustedLaunchRequest request{
      .plugin_id = "org.omarchy.fixture",
      .revision_sha256 = std::string(64, 'a'),
      .generation = 29,
      .revision_directory_fd = revision.get(),
      .private_state_directory_fd = state.get(),
  };
  auto launched = supervisor.launch(
      request, std::chrono::steady_clock::now() + std::chrono::seconds(4));
  if (!launched) {
    throw std::runtime_error("standalone sandbox launch failed: " +
                             launched.detail);
  }
  OMARCHY_CHECK(scope->attached);
  const auto message = launched.worker->receive_any(
      launcher::PacketSizeLimit{sizeof(SandboxProbe)},
      std::chrono::steady_clock::now() + std::chrono::seconds(2),
      launcher::EndpointMask::control);
  OMARCHY_CHECK(static_cast<bool>(message) && message.payload.size() == sizeof(SandboxProbe));
  SandboxProbe probe{};
  std::memcpy(&probe, message.payload.data(), sizeof(probe));
  OMARCHY_CHECK(probe.magic == 0x53425831 && probe.descriptor_mask == 0x3f &&
              probe.exact_descriptors == 1 && probe.exact_environment == 1 &&
              probe.host_home_absent == 1 &&
              probe.bus_socket_absent == 1 &&
              probe.wayland_socket_absent == 1 &&
              probe.agent_socket_absent == 1 &&
              probe.other_plugin_state_absent == 1 &&
              probe.network_denied == 1 && probe.descendant_denied == 1 &&
              probe.revision_write_denied == 1);
  const std::array acknowledgement{std::byte{1}};
  OMARCHY_CHECK(launched.worker->try_send(
              launcher::EndpointRole::control, acknowledgement,
              launcher::PacketSizeLimit{acknowledgement.size()}) ==
              launcher::SendStatus::complete);
  OMARCHY_CHECK(launched.worker->terminate(std::chrono::steady_clock::now() +
                                     std::chrono::seconds(4)));
  OMARCHY_CHECK(scope->terminations == 1);
}


} // namespace

int main() {
  try {
    test_standalone_sandbox();
  } catch (const std::exception &error) {
    dprintf(STDERR_FILENO, "%s\n", error.what());
    return 1;
  }
  return 0;
}
