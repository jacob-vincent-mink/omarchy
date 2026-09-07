#include "../../tests/support/gesture_clock.hpp"
#include "../../tests/support/authenticated_broker_fixture.hpp"
#include "../../tests/support/echo_fixture.hpp"
#include "../../tests/support/test_assert.hpp"
#include "../../tests/support/runtime_bootstrap_fixture.hpp"

#include "runtime_bootstrap.hpp"
#include "runtime_roots_test_access.hpp"

#include "capability_definition_loader.hpp"
#include "omarchy/plugin_runtime/Version.h"
#include "omarchy/plugin_runtime/broker/broker_schema.hpp"
#include "structured_broker.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace channel = omarchy::plugin_runtime::channel;
namespace definitions = omarchy::plugins::definitions;
namespace host = omarchy::plugin_runtime::host_session;
namespace manifest = omarchy::plugins::manifest;
namespace permissions = omarchy::plugins::permissions;
namespace policy = omarchy::plugin_runtime::policy;
namespace runtime = omarchy::plugin_runtime::runtime;
namespace wire = omarchy::plugin::wire;
namespace broker = omarchy::plugin_runtime::broker;

namespace {

using omarchy::plugin_runtime::test_support::require;

std::string repeated(char value) { return std::string(64, value); }

using omarchy::plugin_runtime::test_support::read_file;

bool change_owner_for_test(const std::filesystem::path &path, uid_t uid,
                           gid_t gid) {
  if (::chown(path.c_str(), uid, gid) == 0)
    return true;
  struct stat metadata{};
  return ::getenv("OMARCHY_TEST_FAKE_OWNERSHIP") != nullptr &&
         ::stat(path.c_str(), &metadata) == 0 && metadata.st_uid == uid &&
         metadata.st_gid == gid;
}


using omarchy::plugin_runtime::test_support::make_gesture_latch;

struct Fixture final : omarchy::plugin_runtime::test_support::RuntimeBootstrapTree {
  std::filesystem::path marker;
  std::filesystem::path executable;
  std::filesystem::path profile;
  host::UniqueFd revision_fd;
  host::UniqueFd state_fd;

  explicit Fixture(std::string_view mode = "ok") {
    marker = root_ / "started";
    executable = root_ / "usr/lib/omarchy/plugin-security/provider-peer";
    profile = root_ / "usr/lib/omarchy/plugin-security" /
              std::string(omarchy::plugin_runtime::build_version()) /
              "providers.d/echo.profile";

    create(root_ / "revision", 0700);
    create(root_ / "state", 0700);
    create(profile.parent_path(), 0755);
    std::filesystem::copy_file(PROVIDER_COMPOSITION_PEER_PATH, executable);
    OMARCHY_CHECK(::chmod(executable.c_str(), 0500) == 0);
    write_definition();
    write_profile(mode);

    revision_fd.reset(::open((root_ / "revision").c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    state_fd.reset(::open((root_ / "state").c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    OMARCHY_CHECK(revision_fd && state_fd);
  }

  void write_definition() const {
    const auto document =
        definitions::canonical_definition_document(omarchy::plugin_runtime::test_support::echo_definition(), 1);
    OMARCHY_CHECK(!document.empty());
    const auto path = profile.parent_path().parent_path() /
                      "capabilities.d/service.echo.capability";
    std::ofstream(path, std::ios::binary) << document;
    OMARCHY_CHECK(::chmod(path.c_str(), 0644) == 0);
  }

  void write_profile(std::string_view mode,
                     std::string_view adapter = "service.echo.adapter",
                     std::string_view contract = repeated('d'),
                     std::uint32_t abi = 1,
                     std::string_view executable_path =
                         "/usr/lib/omarchy/plugin-security/provider-peer",
                     std::string_view digest = {}) const {
    std::ofstream output(profile);
    output << "schema=1\n"
           << "adapter-class=" << adapter << "\n"
           << "contract-digest=" << contract << "\n"
           << "abi-version=" << abi << "\n"
           << "group=echo.group\n"
           << "executable=" << executable_path << "\n"
           << "executable-sha256="
           << (digest.empty() ? manifest::sha256_hex(read_file(executable))
                              : std::string(digest))
           << "\narg=" << mode << "\narg=" << marker.string() << "\n";
    output.close();
    OMARCHY_CHECK(::chmod(profile.c_str(), 0644) == 0);
  }

  [[nodiscard]] std::size_t starts() const {
    std::ifstream input(marker);
    std::size_t count = 0;
    std::string line;
    while (std::getline(input, line))
      ++count;
    return count;
  }
};

manifest::ManifestV2
plugin_manifest(const definitions::ResolvedDefinition &resolved,
                bool required = false) {
  return omarchy::plugin_runtime::test_support::echo_manifest(
      resolved, "fixture.plugin", "exercise product provider composition", required);
}

policy::GrantSnapshot snapshot(const definitions::ResolvedDefinition &resolved,
                               permissions::GrantState state,
                               bool required = false,
                               std::uint64_t generation = 7) {
  const auto plugin = plugin_manifest(resolved, required);
  policy::GrantSnapshot value;
  value.binding = {.plugin = permissions::PluginId(plugin.id),
                   .revision = permissions::Digest(repeated('a')),
                   .policy_fingerprint = permissions::Digest(
                       manifest::requested_capability_fingerprint(plugin.requests)),
                   .generation = generation};
  value.dynamic_grants.push_back(omarchy::plugin_runtime::test_support::echo_grant(
      resolved, value.binding, required, state, 7));
  return value;
}

std::vector<std::byte> invocation(const policy::GrantSnapshot &grants) {
  return omarchy::plugin_runtime::test_support::ungestured_invocation(
      grants.dynamic_grants[0].request.definition, "echo");
}

std::unique_ptr<channel::AuthenticatedSessionRuntime> create_runtime(
    const channel::RuntimeBootstrap &bootstrap, const Fixture &fixture,
    const manifest::ManifestV2 &plugin, const policy::GrantSnapshot &grants,
    std::shared_ptr<host::LiveGenerationState> live, std::uint64_t nonce = 41) {
  return channel::RuntimeBootstrapTestAccess::create_session_runtime(
      bootstrap, plugin, grants, fixture.revision_fd.get(), fixture.state_fd.get(), nonce,
      std::move(live), make_gesture_latch<1>());
}

host::BrokerTransaction dispatch(channel::AuthenticatedSessionRuntime &product,
                                 host::AuthenticatedBrokerAdmission &admission,
                                 const policy::GrantSnapshot &grants,
                                 std::uint64_t correlation,
                                 std::span<std::byte> response) {
  const auto payload = invocation(grants);
  auto admitted = omarchy::plugin_runtime::test_support::admit_invocation(
      admission, correlation, payload);
  return product.broker().dispatch(std::move(*admitted.request),
                                   response);
}

void exact_identity_and_pinned_dispatch() {
  Fixture fixture;
  channel::RuntimeBootstrapError error{};
  auto bootstrap = fixture.open_bootstrap(error);
  require(bootstrap && error == channel::RuntimeBootstrapError::none,
          "exact bootstrap rejected: " +
              std::to_string(static_cast<unsigned>(error)));
  const auto resolved = channel::RuntimeBootstrapTestAccess::definition(
      *bootstrap, "service.echo");
  OMARCHY_CHECK(resolved.has_value());
  const auto grants = snapshot(*resolved, permissions::GrantState::granted);
  const auto plugin = plugin_manifest(*resolved);
  const auto projected =
      channel::RuntimeBootstrapTestAccess::project_permissions(*bootstrap,
                                                               plugin, grants);
  OMARCHY_CHECK(projected && projected->permissions.size() == 1 &&
              projected->permissions[0] ==
                  wire::permission_snapshot::PermissionRow{
                      wire::permission_snapshot::GrantState::granted, 1});

  std::filesystem::rename(fixture.executable,
                          fixture.executable.string() + ".pinned");
  std::ofstream(fixture.executable) << "replacement must never execute\n";
  OMARCHY_CHECK(::chmod(fixture.executable.c_str(), 0500) == 0);
  auto live = std::make_shared<host::LiveGenerationState>(grants.binding);
  auto product = create_runtime(*bootstrap, fixture, plugin, grants, live);
  OMARCHY_CHECK(static_cast<bool>(product));
  auto extracted = product->broker().take_admission();
  OMARCHY_CHECK(static_cast<bool>(extracted));
  std::array<std::byte, 128> response{};
  auto result = dispatch(*product, *extracted.admission, grants, 1, response);
  OMARCHY_CHECK(result.state() == host::TransactionState::reply &&
              result.reply_kind() == host::ReplyKind::result &&
              fixture.starts() == 1);
  OMARCHY_CHECK(product->broker().commit_sent(std::move(result)));
}

void unavailable_and_denied_never_launch() {
  for (const auto &mismatch :
       {std::string("class"), std::string("contract"), std::string("abi")}) {
    Fixture fixture;
    if (mismatch == "class")
      fixture.write_profile("ok", "service.other.adapter");
    else if (mismatch == "contract")
      fixture.write_profile("ok", "service.echo.adapter", repeated('e'));
    else
      fixture.write_profile("ok", "service.echo.adapter", repeated('d'), 2);
    channel::RuntimeBootstrapError error{};
    auto bootstrap = fixture.open_bootstrap(error);
    OMARCHY_CHECK(static_cast<bool>(bootstrap));
    const auto resolved = channel::RuntimeBootstrapTestAccess::definition(
        *bootstrap, "service.echo");
    OMARCHY_CHECK(resolved.has_value());
    const auto optional = snapshot(*resolved, permissions::GrantState::granted);
    const auto plugin = plugin_manifest(*resolved);
    const auto projected =
        channel::RuntimeBootstrapTestAccess::project_permissions(
            *bootstrap, plugin, optional);
    auto live = std::make_shared<host::LiveGenerationState>(optional.binding);
    auto product = create_runtime(*bootstrap, fixture, plugin, optional, live);
    OMARCHY_CHECK(projected && projected->permissions[0].operation_mask == 0 &&
                product && fixture.starts() == 0);
    auto admission = product->broker().take_admission();
    std::array<std::byte, 64> response{};
    auto denied =
        dispatch(*product, *admission.admission, optional, 1, response);
    OMARCHY_CHECK(denied.state() == host::TransactionState::reply &&
                denied.reply_kind() == host::ReplyKind::denied &&
                product->broker().commit_sent(std::move(denied)) &&
                fixture.starts() == 0);

    const auto required =
        snapshot(*resolved, permissions::GrantState::granted, true);
    const auto required_plugin = plugin_manifest(*resolved, true);
    auto required_live =
        std::make_shared<host::LiveGenerationState>(required.binding);
    OMARCHY_CHECK(!channel::RuntimeBootstrapTestAccess::project_permissions(
                *bootstrap, required_plugin, required) &&
                !create_runtime(*bootstrap, fixture, required_plugin, required,
                                required_live) &&
                fixture.starts() == 0);
  }

  Fixture denied_fixture;
  channel::RuntimeBootstrapError error{};
  auto bootstrap = denied_fixture.open_bootstrap(error);
  const auto resolved = channel::RuntimeBootstrapTestAccess::definition(
      *bootstrap, "service.echo");
  const auto denied = snapshot(*resolved, permissions::GrantState::denied);
  const auto plugin = plugin_manifest(*resolved);
  const auto projected =
      channel::RuntimeBootstrapTestAccess::project_permissions(*bootstrap,
                                                               plugin, denied);
  auto live = std::make_shared<host::LiveGenerationState>(denied.binding);
  auto product =
      create_runtime(*bootstrap, denied_fixture, plugin, denied, live);
  OMARCHY_CHECK(projected && projected->permissions[0].operation_mask == 0 && product);
  auto admission = product->broker().take_admission();
  std::array<std::byte, 64> response{};
  auto result = dispatch(*product, *admission.admission, denied, 1, response);
  OMARCHY_CHECK(result.state() == host::TransactionState::reply &&
              result.reply_kind() == host::ReplyKind::denied &&
              product->broker().commit_sent(std::move(result)) &&
              denied_fixture.starts() == 0);
}

void bootstrap_rejects_provider_tamper() {
  auto rejected = [](auto mutate, std::string_view message) {
    Fixture fixture;
    mutate(fixture);
    channel::RuntimeBootstrapError error{};
    require(!fixture.open_bootstrap(error) &&
                error ==
                    channel::RuntimeBootstrapError::provider_profiles_untrusted,
            message);
  };
  rejected([](Fixture &fixture) {
    OMARCHY_CHECK(::chmod(fixture.profile.parent_path().c_str(), 0775) == 0);
  }, "group-writable fixed provider root accepted");
  rejected([](Fixture &fixture) {
    OMARCHY_CHECK(::chmod(fixture.profile.c_str(), 0664) == 0);
  }, "group-writable profile accepted");
  rejected([](Fixture &fixture) {
    OMARCHY_CHECK(::chmod(fixture.executable.c_str(), 0520) == 0);
  }, "group-writable executable accepted");
  rejected([](Fixture &fixture) {
    fixture.write_profile("ok", "service.echo.adapter", repeated('d'), 1,
                          "/usr/lib/omarchy/plugin-security/provider-peer",
                          repeated('f'));
  }, "wrong executable hash accepted");
  rejected([](Fixture &fixture) {
    fixture.write_profile(
        "ok", "service.echo.adapter", repeated('d'), 1,
        "/usr/lib/omarchy/plugin-security/../plugin-security/provider-peer");
  }, "noncanonical executable path accepted");
  rejected([](Fixture &fixture) {
    std::filesystem::rename(fixture.executable,
                            fixture.executable.string() + ".real");
    std::filesystem::create_symlink("provider-peer.real", fixture.executable);
  }, "symlink executable accepted");
  rejected([](Fixture &fixture) {
    std::filesystem::rename(fixture.profile,
                            fixture.profile.string() + ".real");
    std::filesystem::create_symlink("echo.profile.real", fixture.profile);
  }, "symlink profile accepted");
  rejected([](Fixture &fixture) {
    const auto directory = fixture.profile.parent_path();
    const auto real = directory.string() + ".real";
    std::filesystem::rename(directory, real);
    std::filesystem::create_symlink(std::filesystem::path(real).filename(),
                                    directory);
  }, "symlink fixed provider root accepted");
  if (::geteuid() == 0 || ::getenv("OMARCHY_TEST_FAKE_OWNERSHIP") != nullptr) {
    rejected([](Fixture &fixture) {
      OMARCHY_CHECK(
          change_owner_for_test(fixture.profile.parent_path(), 65534, 65534));
    }, "wrong-owner fixed provider root accepted");
    rejected([](Fixture &fixture) {
      OMARCHY_CHECK(change_owner_for_test(fixture.profile, 65534, 65534));
    }, "wrong-owner profile accepted");
    rejected([](Fixture &fixture) {
      OMARCHY_CHECK(change_owner_for_test(fixture.executable, 65534, 65534));
    }, "wrong-owner executable accepted");
  }
}

void provider_failures_are_fail_stop_and_reaped() {
  for (const std::string mode :
       {"crash", "timeout", "malformed", "truncated", "oversized", "late"}) {
    Fixture fixture(mode);
    channel::RuntimeBootstrapError error{};
    auto bootstrap = fixture.open_bootstrap(error);
    OMARCHY_CHECK(static_cast<bool>(bootstrap));
    const auto resolved = channel::RuntimeBootstrapTestAccess::definition(
        *bootstrap, "service.echo");
    const auto grants = snapshot(*resolved, permissions::GrantState::granted);
    const auto plugin = plugin_manifest(*resolved);
    auto live = std::make_shared<host::LiveGenerationState>(grants.binding);
    auto product = create_runtime(*bootstrap, fixture, plugin, grants, live);
    OMARCHY_CHECK(static_cast<bool>(product));
    auto admission = product->broker().take_admission();
    std::array<std::byte, 128> response{};
    auto first = dispatch(*product, *admission.admission, grants, 1, response);
    OMARCHY_CHECK(first.state() == host::TransactionState::reply &&
                first.reply_kind() == host::ReplyKind::provider_failed &&
                product->broker().commit_sent(std::move(first)) &&
                fixture.starts() == 1);
    auto second = dispatch(*product, *admission.admission, grants, 2, response);
    OMARCHY_CHECK(second.state() == host::TransactionState::reply &&
                second.reply_kind() == host::ReplyKind::provider_failed &&
                product->broker().commit_sent(std::move(second)) &&
                fixture.starts() == 1);
    errno = 0;
    OMARCHY_CHECK(::waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD);
  }
}

void activation_and_admission_binding() {
  Fixture fixture("pid");
  channel::RuntimeBootstrapError error{};
  auto bootstrap = fixture.open_bootstrap(error);
  const auto resolved = channel::RuntimeBootstrapTestAccess::definition(
      *bootstrap, "service.echo");
  const auto first_grants =
      snapshot(*resolved, permissions::GrantState::granted, false, 7);
  const auto second_grants =
      snapshot(*resolved, permissions::GrantState::granted, false, 8);
  const auto plugin = plugin_manifest(*resolved);
  auto first_live =
      std::make_shared<host::LiveGenerationState>(first_grants.binding);
  auto second_live =
      std::make_shared<host::LiveGenerationState>(second_grants.binding);
  auto first =
      create_runtime(*bootstrap, fixture, plugin, first_grants, first_live, 51);
  auto second = create_runtime(*bootstrap, fixture, plugin, second_grants,
                               second_live, 52);
  OMARCHY_CHECK(first && second);
  auto first_admission = first->broker().take_admission();
  auto second_admission = second->broker().take_admission();
  const auto payload = invocation(first_grants);
  auto foreign = first_admission.admission->admit(
      {.message_type = broker::kDynamicInvokeMessage,
       .correlation_id = 1,
       .payload = payload});
  OMARCHY_CHECK(static_cast<bool>(foreign));
  std::array<std::byte, 128> response{};
  auto rejected =
      second->broker().dispatch(std::move(*foreign.request), response);
  OMARCHY_CHECK(rejected.state() == host::TransactionState::fatal &&
              fixture.starts() == 0);
  auto duplicate = first_admission.admission->admit(
      {.message_type = broker::kDynamicInvokeMessage,
       .correlation_id = 1,
       .payload = payload});
  OMARCHY_CHECK(!duplicate);

  auto valid =
      dispatch(*first, *first_admission.admission, first_grants, 2, response);
  OMARCHY_CHECK(valid.state() == host::TransactionState::reply &&
              valid.reply_kind() == host::ReplyKind::result &&
              first->broker().commit_sent(std::move(valid)) &&
              fixture.starts() == 1);
  const auto marker = read_file(fixture.marker);
  const auto pid = static_cast<pid_t>(std::stol(marker));
  OMARCHY_CHECK(first_live->revoke_and_drain() ==
              host::LiveGenerationRevokeResult::drained);
  auto revoked =
      dispatch(*first, *first_admission.admission, first_grants, 3, response);
  OMARCHY_CHECK(revoked.state() == host::TransactionState::fatal &&
              fixture.starts() == 1);
  first.reset();
  errno = 0;
  OMARCHY_CHECK(::kill(pid, 0) < 0 && errno == ESRCH);
  errno = 0;
  OMARCHY_CHECK(::waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD);
  (void)second_admission;
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    if (::getenv("OMARCHY_TEST_FAKE_OWNERSHIP") != nullptr) {
      // fakeroot deliberately reports uid 0, so the production resource-scope
      // controller would probe /run/user/0 instead of the caller's real user
      // bus. This pass exists only for the ownership mutations below; the
      // ordinary test already covers composition and provider lifecycles.
      bootstrap_rejects_provider_tamper();
      std::cout << "provider composition ownership tests passed\n";
      return 0;
    }
    exact_identity_and_pinned_dispatch();
    unavailable_and_denied_never_launch();
    bootstrap_rejects_provider_tamper();
    provider_failures_are_fail_stop_and_reaped();
    activation_and_admission_binding();
    std::cout << "provider composition tests passed\n";
    return 0;
  }, "provider composition test failed: ");
}
