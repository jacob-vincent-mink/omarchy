#include "../../tests/support/test_assert.hpp"

#include "../../tests/support/temporary_directory.hpp"
#include "omarchy/plugin_runtime/provider_host/provider_host.hpp"

#include "manifest_contract.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace host = omarchy::plugin_runtime::provider_host;
namespace definitions = omarchy::plugins::definitions;
namespace permissions = omarchy::plugins::permissions;
namespace launcher = omarchy::plugin_runtime::launcher;

namespace {

using omarchy::plugin_runtime::test_support::exit_assertions::require;

class RecordingScope final : public launcher::ResourceScopeController {
public:
  bool probe(launcher::Deadline, std::string &) override {
    ++probe_count;
    return probe_succeeds;
  }
  bool prepare_cleanup(launcher::Deadline, std::string &) override {
    ++prepare_count;
    return prepare_succeeds;
  }
  bool terminate_scope_validated(std::string_view unit,
                                  launcher::Deadline deadline,
                                  std::string &error) noexcept override {
    {
      std::lock_guard lock(termination_mutex);
      terminated.emplace_back(unit);
    }
    termination_changed.notify_all();
    if (termination_delay.count() > 0)
      std::this_thread::sleep_until(std::min(
          deadline, std::chrono::steady_clock::now() + termination_delay));
    if (!termination_succeeds)
      error = "synthetic provider scope termination failed";
    return termination_succeeds;
  }

  [[nodiscard]] std::size_t termination_count() const {
    std::lock_guard lock(termination_mutex);
    return terminated.size();
  }
  [[nodiscard]] bool wait_for_termination_count(
      std::size_t count, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(termination_mutex);
    return termination_changed.wait_until(
        lock, deadline, [&] { return terminated.size() >= count; });
  }
  [[nodiscard]] bool exact_termination_retried() const {
    std::lock_guard lock(termination_mutex);
    return !terminated.empty() &&
           std::ranges::all_of(terminated, [&](const auto &unit) {
             return unit == terminated.front();
           });
  }

  bool probe_succeeds = true;
  bool prepare_succeeds = true;
  bool attach_succeeds = true;
  bool cleanup_required = true;
  std::atomic_bool termination_succeeds = true;
  bool kill_child_during_attach = false;
  std::chrono::milliseconds attachment_delay{};
  std::chrono::milliseconds termination_delay{};
  std::atomic_int attachment_count = 0;
  int probe_count = 0;
  int prepare_count = 0;
  std::vector<std::string> units;
  std::vector<std::string> descriptions;
  std::vector<std::vector<pid_t>> attached_pids;
  std::vector<launcher::ProcessResourceCeilings> resources;
  std::vector<std::string> terminated;
  mutable std::mutex termination_mutex;
  std::condition_variable termination_changed;

protected:
  AttachResult attach_validated(const launcher::ProcessScopeRequest &request,
                                launcher::Deadline deadline,
                                std::string &) override {
    ++attachment_count;
    units.emplace_back(request.unit);
    descriptions.emplace_back(request.description);
    attached_pids.emplace_back(request.pids.begin(), request.pids.end());
    resources.push_back(request.resources);
    if (attachment_delay.count() > 0)
      std::this_thread::sleep_until(std::min(
          deadline, std::chrono::steady_clock::now() + attachment_delay));
    if (kill_child_during_attach)
      (void)::kill(request.pids.front(), SIGKILL);
    return {.attached = attach_succeeds,
            .cleanup_required = cleanup_required};
  }
};

std::shared_ptr<RecordingScope> recording_scope() {
  return std::make_shared<RecordingScope>();
}

using omarchy::plugin_runtime::test_support::read_file;

struct Fixture final {
  omarchy::plugin_runtime::test_support::TemporaryDirectory directory;
  std::filesystem::path root = directory.path();
  std::string executable_digest;

  Fixture() {
    std::filesystem::create_directories(root / "pkg");
    std::filesystem::create_directories(root / "admin");
    std::filesystem::create_directories(root / "bin");
    std::filesystem::copy_file(PROVIDER_PEER_PATH, root / "bin/provider-peer");
    std::filesystem::permissions(root / "bin/provider-peer",
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_exec);
    executable_digest = omarchy::plugins::manifest::sha256_hex(
        read_file(root / "bin/provider-peer"));
  }

  std::filesystem::path profile_path(std::string_view name,
                                     bool admin = false) const {
    return root / (admin ? "admin" : "pkg") /
           (std::string(name) + ".profile");
  }
  void profile(std::string_view name, std::string_view adapter,
               std::string_view contract, std::string_view group,
               std::string_view mode = "pid", bool admin = false,
               std::string_view executable = "/bin/provider-peer",
               std::string_view digest = {},
               std::span<const std::string_view> extra_arguments = {},
               std::string_view invocation_timeout = {}) const {
    std::ofstream output(profile_path(name, admin));
    output << "schema=1\n"
           << "adapter-class=" << adapter << "\n"
           << "contract-digest=" << contract << "\n"
           << "abi-version=1\n"
           << "group=" << group << "\n"
           << "executable=" << executable << "\n"
           << "executable-sha256="
           << (digest.empty() ? executable_digest : digest) << "\n"
           << "arg=" << mode << "\n";
    for (const auto argument : extra_arguments)
      output << "arg=" << argument << "\n";
    if (!invocation_timeout.empty())
      output << "invocation-timeout-ms=" << invocation_timeout << "\n";
    output.close();
    std::filesystem::permissions(profile_path(name, admin),
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write);
  }
};

std::string digest(char value) { return std::string(64, value); }

definitions::AdapterBinding binding(std::string_view adapter, char value = 'a') {
  return {.adapter_class = definitions::Name(adapter),
          .contract_digest = definitions::Digest(digest(value)),
          .abi_version = 1};
}

permissions::ActivationBinding activation(std::uint64_t generation) {
  return {.plugin = permissions::PluginId("test.provider"),
          .revision = permissions::Digest(digest('b')),
          .policy_fingerprint = permissions::Digest(digest('c')),
          .generation = generation};
}

std::shared_ptr<const host::ProviderCatalog> load(
    const Fixture &fixture, host::CatalogError &error) {
  omarchy::plugin_runtime::UniqueFd root(
      ::open(fixture.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  OMARCHY_CHECK_WITH(require, root);
  const std::array<std::string_view, 1> package{"pkg"};
  const std::array<std::string_view, 1> admin{"admin"};
  return host::ProviderCatalog::load(
      root.get(), package, admin, static_cast<std::uint32_t>(::getuid()), error);
}

bool dispatch(const std::optional<definitions::DynamicAdapter> &route,
              const permissions::ActivationBinding &active,
              std::span<const std::byte> payload, std::size_t response_capacity,
              std::string &result) {
  std::vector<std::byte> response(response_capacity);
  std::size_t written = 0;
  const definitions::AuthorizedDynamicRequest request{
      .authorization = {.binding = active,
                        .definition = {.canonical_name =
                                           definitions::Name("test.capability"),
                                       .definition_generation = 1,
                                       .definition_digest =
                                           definitions::Digest(digest('d'))},
                        .grant_epoch = 1},
      .operation = "observe",
      .demand_scope = "{}",
      .payload = payload};
  const bool ok = route && route->invoke(request, response, written);
  if (written == 0)
    result.clear();
  else
    result.assign(reinterpret_cast<const char *>(response.data()), written);
  return ok;
}

bool invoke(const std::optional<definitions::DynamicAdapter> &route,
            const permissions::ActivationBinding &active, std::string &result) {
  const std::array<std::byte, 1> payload{std::byte{7}};
  return dispatch(route, active, payload, 256, result);
}

void catalog_security() {
  Fixture fixture;
  fixture.profile("good", "test.adapter", digest('a'), "test.group");
  host::CatalogError error{};
  const auto catalog = load(fixture, error);
  OMARCHY_CHECK_WITH(require, catalog && error == host::CatalogError::none && catalog->size() == 1);
  OMARCHY_CHECK_WITH(require, catalog->available(binding("test.adapter")));
  OMARCHY_CHECK_WITH(require, !catalog->available(binding("test.adapter", 'e')));

  for (const auto timeout : {"0", "30001"}) {
    Fixture invalid_timeout;
    invalid_timeout.profile("timeout", "test.adapter", digest('a'),
                            "test.group", "pid", false,
                            "/bin/provider-peer", {}, {}, timeout);
    OMARCHY_CHECK_WITH(require, !load(invalid_timeout, error) &&
                error == host::CatalogError::profile_rejected);
  }

  Fixture bad_hash;
  bad_hash.profile("bad", "test.adapter", digest('a'), "test.group", "pid",
                   false, "/bin/provider-peer", digest('f'));
  OMARCHY_CHECK_WITH(require, !load(bad_hash, error) && error == host::CatalogError::executable_rejected);

  Fixture writable;
  writable.profile("bad", "test.adapter", digest('a'), "test.group");
  std::filesystem::permissions(
      writable.profile_path("bad"), std::filesystem::perms::group_write,
      std::filesystem::perm_options::add);
  OMARCHY_CHECK_WITH(require, !load(writable, error) && error == host::CatalogError::profile_rejected);

  Fixture profile_link;
  profile_link.profile("real", "test.adapter", digest('a'), "test.group");
  std::filesystem::rename(profile_link.profile_path("real"),
                          profile_link.root / "pkg/real.data");
  std::filesystem::create_symlink("real.data",
                                  profile_link.profile_path("linked"));
  OMARCHY_CHECK_WITH(require, !load(profile_link, error) &&
              error == host::CatalogError::profile_rejected);

  Fixture symlink;
  std::filesystem::create_symlink("/bin/provider-peer",
                                  symlink.root / "bin/provider-link");
  symlink.profile("bad", "test.adapter", digest('a'), "test.group", "pid",
                  false, "/bin/provider-link");
  OMARCHY_CHECK_WITH(require, !load(symlink, error) && error == host::CatalogError::executable_rejected);

  std::string nul_path = "/bin/provider-peer";
  nul_path.push_back('\0');
  nul_path.append("-ignored");
  for (const auto path : {std::string_view("/bin/../bin/provider-peer"),
                          std::string_view(nul_path)}) {
    Fixture invalid_path;
    invalid_path.profile("bad", "test.adapter", digest('a'), "test.group",
                          "pid", false, path);
    OMARCHY_CHECK_WITH(require, !load(invalid_path, error) &&
                error == host::CatalogError::executable_rejected);
  }

  for (const auto permission : {std::filesystem::perms::set_uid,
                                std::filesystem::perms::set_gid}) {
    Fixture privileged;
    privileged.profile("bad", "test.adapter", digest('a'), "test.group");
    std::filesystem::permissions(privileged.root / "bin/provider-peer",
                                 permission, std::filesystem::perm_options::add);
    OMARCHY_CHECK_WITH(require, !load(privileged, error) &&
                error == host::CatalogError::executable_rejected);
  }

  Fixture duplicate;
  duplicate.profile("one", "test.adapter", digest('a'), "test.group");
  duplicate.profile("two", "test.adapter", digest('a'), "test.group", "pid",
                    true);
  OMARCHY_CHECK_WITH(require, !load(duplicate, error) && error == host::CatalogError::duplicate_binding);
}

void protocol_and_lifecycle() {
  Fixture fixture;
  fixture.profile("one", "test.adapter", digest('a'), "shared.group");
  fixture.profile("two", "test.second", digest('e'), "shared.group");
  host::CatalogError error{};
  auto catalog = load(fixture, error);
  OMARCHY_CHECK_WITH(require, catalog && catalog->size() == 2);
  std::filesystem::rename(fixture.root / "bin/provider-peer",
                          fixture.root / "bin/provider-peer-pinned");
  {
    std::ofstream replacement(fixture.root / "bin/provider-peer");
    replacement << "not the reviewed executable\n";
  }
  std::filesystem::permissions(fixture.root / "bin/provider-peer",
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_exec);
  const auto active = activation(4);
  auto scope = recording_scope();
  auto runtime = host::ProviderActivation::create(catalog, active, scope);
  auto first = runtime->route(binding("test.adapter"));
  auto second = runtime->route(binding("test.second", 'e'));
  OMARCHY_CHECK_WITH(require, first && second && !runtime->route(binding("test.missing")));
  std::string first_pid, second_pid;
  OMARCHY_CHECK_WITH(require, invoke(first, active, first_pid));
  OMARCHY_CHECK_WITH(require, invoke(second, active, second_pid));
  OMARCHY_CHECK_WITH(require, first_pid == second_pid);
  std::string rejected;
  OMARCHY_CHECK_WITH(require, !invoke(first, activation(5), rejected));
  OMARCHY_CHECK_WITH(require, scope->units.size() == 1 && scope->attached_pids.size() == 1 &&
              scope->attached_pids.front().size() == 1 &&
              scope->descriptions.front() == "Omarchy trusted plugin provider");
  const auto &limits = scope->resources.front();
  OMARCHY_CHECK_WITH(require, limits.memory_high_bytes == 96U * 1024U * 1024U &&
              limits.memory_max_bytes == 128U * 1024U * 1024U &&
              limits.tasks_max == 8 &&
              limits.cpu_quota_per_second_usec == 250000 &&
              limits.cpu_weight == 10 && limits.io_weight == 10);

  auto other_scope = recording_scope();
  auto other =
      host::ProviderActivation::create(catalog, activation(5), other_scope);
  auto other_route = other->route(binding("test.adapter"));
  std::string other_pid;
  OMARCHY_CHECK_WITH(require, invoke(other_route, activation(5), other_pid) && other_pid != first_pid);
  const auto pid = static_cast<pid_t>(std::stoi(other_pid));
  OMARCHY_CHECK_WITH(require, other->cancel() && !other->cleanup_pending() &&
              ::kill(pid, 0) < 0 && errno == ESRCH);
  OMARCHY_CHECK_WITH(require, other_scope->terminated == other_scope->units);
  std::string cancelled;
  OMARCHY_CHECK_WITH(require, !invoke(other_route, activation(5), cancelled));
  // A copied adapter owns the activation after its factory handle is gone.
  auto owned = host::ProviderActivation::create(catalog, activation(6), recording_scope());
  std::weak_ptr<host::ProviderActivation> lifetime = owned;
  auto original = owned->route(binding("test.adapter"));
  auto copy = original;
  owned.reset();
  original.reset();
  OMARCHY_CHECK_WITH(require, !lifetime.expired() && invoke(copy, activation(6), cancelled));
  const auto owned_pid = static_cast<pid_t>(std::stoi(cancelled));
  copy.reset();
  OMARCHY_CHECK_WITH(require, lifetime.expired() && ::kill(owned_pid, 0) < 0 && errno == ESRCH);
}

void failure_modes() {
  for (const auto mode : {"crash", "malformed", "wrong-correlation", "late",
                          "truncated", "oversized"}) {
    Fixture fixture;
    fixture.profile("bad", "test.adapter", digest('a'), "bad.group", mode);
    host::CatalogError error{};
    auto catalog = load(fixture, error);
    OMARCHY_CHECK_WITH(require, catalog != nullptr);
    auto scope = recording_scope();
    auto runtime = host::ProviderActivation::create(
        catalog, activation(8), scope, std::chrono::milliseconds(50));
    auto route = runtime->route(binding("test.adapter"));
    std::string output;
    OMARCHY_CHECK_WITH(require, !invoke(route, activation(8), output));
    OMARCHY_CHECK_WITH(require, !invoke(route, activation(8), output));
    OMARCHY_CHECK_WITH(require, scope->units.size() == 1 && scope->terminated == scope->units);
    errno = 0;
    OMARCHY_CHECK_WITH(require, ::waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD);
  }

  Fixture small_response;
  small_response.profile("small", "test.adapter", digest('a'), "small.group",
                         "echo");
  host::CatalogError error{};
  auto catalog = load(small_response, error);
  auto runtime = host::ProviderActivation::create(
      catalog, activation(9), recording_scope());
  auto route = runtime->route(binding("test.adapter"));
  const std::array<std::byte, 1> payload{std::byte{7}};
  std::string output;
  OMARCHY_CHECK_WITH(require, !dispatch(route, activation(9), payload, 0, output));
  OMARCHY_CHECK_WITH(require, !invoke(route, activation(9), output));
  errno = 0;
  OMARCHY_CHECK_WITH(require, ::waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD);

  Fixture environment;
  environment.profile("env", "test.adapter", digest('a'), "env.group",
                      "environment");
  error = {};
  catalog = load(environment, error);
  runtime = host::ProviderActivation::create(
      catalog, activation(10), recording_scope());
  route = runtime->route(binding("test.adapter"));
  output.clear();
  OMARCHY_CHECK_WITH(require, invoke(route, activation(10), output) &&
              output == "/usr/bin|/nonexistent");
  OMARCHY_CHECK_WITH(require, runtime->cancel());

  Fixture inherited_environment;
  inherited_environment.profile("env", "test.adapter", digest('a'),
                                "env.group", "inherited-environment");
  {
    std::ofstream profile(inherited_environment.profile_path("env"),
                          std::ios::app);
    profile << "inherit-environment=HYPRLAND_INSTANCE_SIGNATURE\n"
            << "inherit-environment=XDG_RUNTIME_DIR\n";
  }
  OMARCHY_CHECK_WITH(require, ::setenv("HYPRLAND_INSTANCE_SIGNATURE", "test-instance", 1) == 0 &&
              ::setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1) == 0);
  error = {};
  catalog = load(inherited_environment, error);
  OMARCHY_CHECK_WITH(require, catalog != nullptr);
  runtime = host::ProviderActivation::create(
      catalog, activation(11), recording_scope());
  route = runtime->route(binding("test.adapter"));
  output.clear();
  OMARCHY_CHECK_WITH(require, invoke(route, activation(11), output) &&
              output == "test-instance|/run/user/1000");
  OMARCHY_CHECK_WITH(require, runtime->cancel());
  ::unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
  ::unsetenv("XDG_RUNTIME_DIR");

  Fixture rejected_environment;
  rejected_environment.profile("env", "test.adapter", digest('a'),
                               "env.group", "environment");
  {
    std::ofstream profile(rejected_environment.profile_path("env"),
                          std::ios::app);
    profile << "inherit-environment=HOME\n";
  }
  error = {};
  OMARCHY_CHECK_WITH(require, !load(rejected_environment, error) &&
              error == host::CatalogError::profile_rejected);
}

void aggregate_invocation_deadline_bounds_cancel_wait() {
  Fixture fixture;
  fixture.profile("bounded", "test.adapter", digest('a'), "bounded.group",
                  "late");
  host::CatalogError error{};
  auto catalog = load(fixture, error);
  OMARCHY_CHECK_WITH(require, catalog != nullptr);
  auto scope = recording_scope();
  scope->attachment_delay = std::chrono::milliseconds(250);
  constexpr auto timeout = std::chrono::milliseconds(400);
  auto runtime =
      host::ProviderActivation::create(catalog, activation(13), scope, timeout);
  auto route = runtime->route(binding("test.adapter"));
  OMARCHY_CHECK_WITH(require, route.has_value());

  bool invoked = true;
  std::string output;
  const auto started = std::chrono::steady_clock::now();
  std::thread invocation(
      [&] { invoked = invoke(route, activation(13), output); });
  const auto attach_deadline = started + std::chrono::seconds(1);
  while (scope->attachment_count.load() == 0 &&
         std::chrono::steady_clock::now() < attach_deadline)
    std::this_thread::yield();
  OMARCHY_CHECK_WITH(require, scope->attachment_count.load() == 1);
  const bool cancelled = runtime->cancel();
  invocation.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  OMARCHY_CHECK_WITH(require, !invoked && cancelled && !runtime->cleanup_pending());
  OMARCHY_CHECK_WITH(require, elapsed < timeout + std::chrono::milliseconds(150));
  OMARCHY_CHECK_WITH(require, scope->units.size() == 1 && scope->terminated == scope->units);
  OMARCHY_CHECK_WITH(require, !invoke(route, activation(13), output) &&
              scope->units.size() == 1);
  errno = 0;
  OMARCHY_CHECK_WITH(require, ::waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD);
}

void profile_invocation_timeout() {
  Fixture short_fixture;
  short_fixture.profile("short", "test.adapter", digest('a'), "short.group",
                        "late", false, "/bin/provider-peer", {}, {}, "150");
  host::CatalogError error{};
  auto catalog = load(short_fixture, error);
  auto scope = recording_scope();
  auto runtime = host::ProviderActivation::create(catalog, activation(14), scope);
  auto route = runtime->route(binding("test.adapter"));
  std::string output;
  OMARCHY_CHECK_WITH(require, !invoke(route, activation(14), output) &&
              scope->termination_count() == 1);

  Fixture long_fixture;
  long_fixture.profile("long", "test.adapter", digest('a'), "long.group",
                       "late", false, "/bin/provider-peer", {}, {}, "1000");
  catalog = load(long_fixture, error);
  scope = recording_scope();
  runtime = host::ProviderActivation::create(catalog, activation(15), scope);
  route = runtime->route(binding("test.adapter"));
  OMARCHY_CHECK_WITH(require, invoke(route, activation(15), output) && output == "ok");
  OMARCHY_CHECK_WITH(require, runtime->cancel());
}

void launch_boundary() {
  Fixture lazy;
  const auto marker = lazy.root / "provider-started";
  const std::string marker_text = marker.string();
  const std::array<std::string_view, 1> marker_argument{marker_text};
  lazy.profile("lazy", "test.adapter", digest('a'), "lazy.group", "marker",
               false, "/bin/provider-peer", {}, marker_argument);
  host::CatalogError error{};
  auto catalog = load(lazy, error);
  auto scope = recording_scope();
  auto runtime =
      host::ProviderActivation::create(catalog, activation(11), scope);
  auto route = runtime->route(binding("test.adapter"));
  OMARCHY_CHECK_WITH(require, route && !std::filesystem::exists(marker));
  std::string output;
  std::vector<std::byte> oversized(host::kMaximumProviderPayload + 1,
                                   std::byte{0});
  OMARCHY_CHECK_WITH(require, !dispatch(route, activation(11), oversized, 256, output) &&
              !std::filesystem::exists(marker));
  OMARCHY_CHECK_WITH(require, invoke(route, activation(11), output) &&
              std::filesystem::exists(marker));
  OMARCHY_CHECK_WITH(require, scope->units.size() == 1);

  Fixture descriptors;
  const int opened = ::open((descriptors.root / "inherited").c_str(),
                            O_RDWR | O_CREAT | O_TRUNC, 0600);
  OMARCHY_CHECK_WITH(require, opened >= 0);
  const int inherited = ::fcntl(opened, F_DUPFD, 10);
  ::close(opened);
  OMARCHY_CHECK_WITH(require, inherited >= 10);
  const int flags = ::fcntl(inherited, F_GETFD);
  OMARCHY_CHECK_WITH(require, flags >= 0 && ::fcntl(inherited, F_SETFD, flags & ~FD_CLOEXEC) == 0);
  const std::string inherited_text = std::to_string(inherited);
  const std::array<std::string_view, 1> inherited_argument{inherited_text};
  descriptors.profile("isolated", "test.adapter", digest('a'),
                      "isolated.group", "isolation", false,
                      "/bin/provider-peer", {}, inherited_argument);
  catalog = load(descriptors, error);
  runtime = host::ProviderActivation::create(
      catalog, activation(12), recording_scope());
  route = runtime->route(binding("test.adapter"));
  output.clear();
  OMARCHY_CHECK_WITH(require, invoke(route, activation(12), output) && output == "isolated");
  ::close(inherited);
}

void scope_fail_closed() {
  Fixture fixture;
  const auto marker = fixture.root / "provider-started";
  const std::string marker_text = marker.string();
  const std::array<std::string_view, 1> marker_argument{marker_text};
  fixture.profile("scoped", "test.adapter", digest('a'), "scoped.group",
                  "marker", false, "/bin/provider-peer", {}, marker_argument);
  host::CatalogError error{};
  const auto catalog = load(fixture, error);
  OMARCHY_CHECK_WITH(require, catalog != nullptr);

  auto unavailable = recording_scope();
  unavailable->probe_succeeds = false;
  OMARCHY_CHECK_WITH(require, !host::ProviderActivation::create(catalog, activation(20),
                                            unavailable) &&
              unavailable->probe_count == 1 &&
              unavailable->prepare_count == 0 &&
              !std::filesystem::exists(marker));

  auto unprepared = recording_scope();
  unprepared->prepare_succeeds = false;
  OMARCHY_CHECK_WITH(require, !host::ProviderActivation::create(catalog, activation(21),
                                            unprepared) &&
              unprepared->probe_count == 1 && unprepared->prepare_count == 1 &&
              !std::filesystem::exists(marker));

  auto rejected = recording_scope();
  rejected->attach_succeeds = false;
  auto runtime =
      host::ProviderActivation::create(catalog, activation(22), rejected);
  OMARCHY_CHECK_WITH(require, runtime != nullptr);
  auto route = runtime->route(binding("test.adapter"));
  std::string output;
  OMARCHY_CHECK_WITH(require, !invoke(route, activation(22), output) &&
              !std::filesystem::exists(marker));
  OMARCHY_CHECK_WITH(require, rejected->units.size() == 1 && rejected->terminated == rejected->units);
  OMARCHY_CHECK_WITH(require, !invoke(route, activation(22), output) &&
              rejected->units.size() == 1);
  errno = 0;
  OMARCHY_CHECK_WITH(require, ::waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD);

  Fixture cleanup_failure;
  cleanup_failure.profile("cleanup", "test.adapter", digest('a'),
                          "cleanup.group", "crash");
  const auto cleanup_catalog = load(cleanup_failure, error);
  auto failing_cleanup = recording_scope();
  failing_cleanup->termination_succeeds = false;
  auto cleanup_runtime = host::ProviderActivation::create(
      cleanup_catalog, activation(23), failing_cleanup);
  auto cleanup_route = cleanup_runtime->route(binding("test.adapter"));
  OMARCHY_CHECK_WITH(require, !invoke(cleanup_route, activation(23), output) &&
              failing_cleanup->units.size() == 1 &&
              failing_cleanup->terminated == failing_cleanup->units &&
              cleanup_runtime->cleanup_pending());
  OMARCHY_CHECK_WITH(require, !cleanup_runtime->cancel() && cleanup_runtime->cleanup_pending() &&
              failing_cleanup->terminated.size() == 2);
  OMARCHY_CHECK_WITH(require, !invoke(cleanup_route, activation(23), output) &&
              failing_cleanup->units.size() == 1 &&
              failing_cleanup->terminated.size() == 2);
  failing_cleanup->termination_succeeds = true;
  OMARCHY_CHECK_WITH(require, cleanup_runtime->cancel() && !cleanup_runtime->cleanup_pending() &&
              failing_cleanup->terminated.size() == 3 &&
              failing_cleanup->terminated[0] == failing_cleanup->terminated[1] &&
              failing_cleanup->terminated[1] == failing_cleanup->terminated[2]);

  Fixture bounded;
  bounded.profile("bounded", "test.adapter", digest('a'), "bounded.group");
  const auto bounded_catalog = load(bounded, error);
  auto bounded_scope = recording_scope();
  auto bounded_runtime = host::ProviderActivation::create(
      bounded_catalog, activation(24), bounded_scope,
      std::chrono::milliseconds(40));
  auto bounded_route = bounded_runtime->route(binding("test.adapter"));
  OMARCHY_CHECK_WITH(require, invoke(bounded_route, activation(24), output));
  bounded_scope->termination_succeeds = false;
  bounded_scope->termination_delay = std::chrono::milliseconds(250);
  const auto cancel_started = std::chrono::steady_clock::now();
  OMARCHY_CHECK_WITH(require, !bounded_runtime->cancel() && bounded_runtime->cleanup_pending() &&
              std::chrono::steady_clock::now() - cancel_started <
                  std::chrono::milliseconds(150));
  bounded_scope->termination_succeeds = true;
  bounded_scope->termination_delay = {};
  OMARCHY_CHECK_WITH(require, bounded_runtime->cancel() && !bounded_runtime->cleanup_pending());

  Fixture abandoned;
  abandoned.profile("abandoned", "test.adapter", digest('a'),
                    "abandoned.group", "crash");
  const auto abandoned_catalog = load(abandoned, error);
  auto abandoned_scope = recording_scope();
  abandoned_scope->termination_succeeds = false;
  {
    auto abandoned_runtime = host::ProviderActivation::create(
        abandoned_catalog, activation(25), abandoned_scope,
        std::chrono::milliseconds(40));
    auto abandoned_route =
        abandoned_runtime->route(binding("test.adapter"));
    OMARCHY_CHECK_WITH(require, !invoke(abandoned_route, activation(25), output) &&
                abandoned_runtime->cleanup_pending());
    abandoned_route.reset();
  }
  const auto abandoned_attempts = abandoned_scope->termination_count();
  abandoned_scope->termination_succeeds = true;
  OMARCHY_CHECK_WITH(require, abandoned_scope->wait_for_termination_count(
              abandoned_attempts + 1,
              std::chrono::steady_clock::now() + std::chrono::seconds(2)) &&
              abandoned_scope->exact_termination_retried());
  const auto cleaned_attempts = abandoned_scope->termination_count();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  OMARCHY_CHECK_WITH(require, abandoned_scope->termination_count() == cleaned_attempts);

  const pid_t regression = ::fork();
  OMARCHY_CHECK_WITH(require, regression >= 0);
  if (regression == 0) {
    Fixture release;
    release.profile("release", "test.adapter", digest('a'), "release.group");
    host::CatalogError child_error{};
    const auto release_catalog = load(release, child_error);
    auto release_scope = recording_scope();
    release_scope->kill_child_during_attach = true;
    auto release_runtime = host::ProviderActivation::create(
        release_catalog, activation(26), release_scope,
        std::chrono::milliseconds(100));
    auto release_route = release_runtime->route(binding("test.adapter"));
    std::string child_output;
    const bool safe = release_runtime && release_route &&
                      !invoke(release_route, activation(26), child_output) &&
                      !invoke(release_route, activation(26), child_output) &&
                      !release_runtime->cleanup_pending();
    ::_exit(safe ? 0 : 1);
  }
  int regression_status = 0;
  pid_t regression_waited = 0;
  const auto regression_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (regression_waited == 0 &&
         std::chrono::steady_clock::now() < regression_deadline) {
    regression_waited = ::waitpid(regression, &regression_status, WNOHANG);
    if (regression_waited == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  OMARCHY_CHECK_WITH(require, regression_waited == regression &&
              WIFEXITED(regression_status) && WEXITSTATUS(regression_status) == 0);
}

std::filesystem::path process_cgroup(pid_t pid) {
  std::ifstream input(std::filesystem::path("/proc") / std::to_string(pid) /
                      "cgroup");
  std::string record;
  while (std::getline(input, record)) {
    const auto separator = record.find("::");
    if (separator != std::string::npos)
      return record.substr(separator + 2);
  }
  return {};
}

bool terminated(pid_t pid) {
  std::ifstream input(std::filesystem::path("/proc") / std::to_string(pid) /
                      "stat");
  std::string stat;
  if (!std::getline(input, stat))
    return true;
  const auto state = stat.find(") ");
  return state != std::string::npos && state + 2 < stat.size() &&
         stat[state + 2] == 'Z';
}

bool systemd_descendant_scope() {
  auto scope = launcher::make_systemd_resource_scope_controller();
  std::string preflight_error;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(750);
  if (!scope->probe(deadline, preflight_error) ||
      !scope->prepare_cleanup(deadline, preflight_error)) {
    std::cerr << "SKIP: " << preflight_error << '\n';
    return false;
  }

  Fixture fixture;
  fixture.profile("descendant", "test.adapter", digest('a'),
                  "descendant.group", "descendant");
  host::CatalogError error{};
  const auto catalog = load(fixture, error);
  OMARCHY_CHECK_WITH(require, catalog != nullptr);
  const auto active = activation(30);
  auto runtime = host::ProviderActivation::create(
      catalog, active, scope, std::chrono::milliseconds(1500));
  OMARCHY_CHECK_WITH(require, runtime != nullptr);
  auto route = runtime->route(binding("test.adapter"));
  std::string output;
  OMARCHY_CHECK_WITH(require, invoke(route, active, output));
  const auto separator = output.find('|');
  OMARCHY_CHECK_WITH(require, separator != std::string::npos);
  const auto direct = static_cast<pid_t>(std::stoi(output.substr(0, separator)));
  const auto descendant =
      static_cast<pid_t>(std::stoi(output.substr(separator + 1)));
  const auto direct_cgroup = process_cgroup(direct);
  const auto descendant_cgroup = process_cgroup(descendant);
  OMARCHY_CHECK_WITH(require, !direct_cgroup.empty() && direct_cgroup == descendant_cgroup &&
              direct_cgroup.string().find(
                  "app-omarchy-plugin-provider-test.provider-") !=
                  std::string::npos &&
              direct_cgroup.extension() == ".scope");

  const auto cgroup_root = std::filesystem::path("/sys/fs/cgroup") /
                           direct_cgroup.relative_path();
  OMARCHY_CHECK_WITH(require, read_file(cgroup_root / "memory.high") == "100663296\n" &&
              read_file(cgroup_root / "memory.max") == "134217728\n" &&
              read_file(cgroup_root / "pids.max") == "8\n" &&
              read_file(cgroup_root / "cpu.weight") == "10\n" &&
              read_file(cgroup_root / "cpu.max").starts_with("25000 "));

  OMARCHY_CHECK_WITH(require, runtime->cancel() && !runtime->cleanup_pending());
  const auto reap_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < reap_deadline &&
         (!terminated(direct) || !terminated(descendant) ||
          std::filesystem::exists(cgroup_root)))
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  OMARCHY_CHECK_WITH(require, terminated(direct));
  OMARCHY_CHECK_WITH(require, terminated(descendant));
  OMARCHY_CHECK_WITH(require, !std::filesystem::exists(cgroup_root));
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "systemd")
    return systemd_descendant_scope() ? 0 : 77;
  catalog_security();
  protocol_and_lifecycle();
  failure_modes();
  aggregate_invocation_deadline_bounds_cancel_wait();
  profile_invocation_timeout();
  launch_boundary();
  scope_fail_closed();
  std::cout << "provider host tests passed\n";
}
