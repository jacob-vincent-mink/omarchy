#include "../../tests/support/child_process.hpp"
#include "../../tests/support/capability_fixture.hpp"
#include "../../tests/support/temporary_directory.hpp"
#include "../../tests/support/test_assert.hpp"
#include "omarchy/plugin_runtime/test_support/test_support.h"

#include "runtime_roots.hpp"
#include "runtime_roots_test_access.hpp"
#include "activation_catalog.hpp"
#include "consent_review.hpp"
#include "revision_verifier_adapter.hpp"
#include "plugin_permission_authority_test_access.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <barrier>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <functional>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace channel = omarchy::plugin_runtime::channel;
namespace host = omarchy::plugin_runtime::host_session;
namespace permissions = omarchy::plugins::permissions;
namespace definitions = omarchy::plugins::definitions;

namespace {

enum class LookupBehavior { mixed_then_success, interrupt_forever, success,
                            wrong_result, wrong_uid, allocation_failure, internal_failure };

LookupBehavior lookup_behavior = LookupBehavior::success;
std::size_t lookup_calls = 0;
uid_t lookup_home_uid = 0;
const char *lookup_home = "/home";
std::filesystem::path provisioning_race_home;

void substitute_first_provisioned_component() {
  const auto local = provisioning_race_home / ".local";
  const auto displaced = provisioning_race_home / "displaced-local";
  std::filesystem::rename(local, displaced);
  std::filesystem::create_directory_symlink(displaced, local);
}

int scripted_lookup(uid_t requested_uid, struct passwd *account, char *,
                    std::size_t, struct passwd **result) {
  ++lookup_calls;
  if (lookup_behavior == LookupBehavior::allocation_failure)
    throw std::bad_alloc();
  if (lookup_behavior == LookupBehavior::internal_failure)
    throw 7;
  if (lookup_behavior == LookupBehavior::interrupt_forever)
    return EINTR;
  if (lookup_behavior == LookupBehavior::mixed_then_success &&
      lookup_calls < 16)
    return lookup_calls % 2 == 0 ? EINTR : ERANGE;
  account->pw_uid = lookup_behavior == LookupBehavior::wrong_uid
                        ? static_cast<uid_t>(requested_uid + 1)
                        : lookup_home_uid;
  account->pw_dir = const_cast<char *>(lookup_home);
  static struct passwd unrelated{};
  *result = lookup_behavior == LookupBehavior::wrong_result ? &unrelated
                                                             : account;
  return 0;
}

using omarchy::plugin_runtime::test_support::require;
using omarchy::plugin_runtime::test_support::expect_child_exit;

void write_file(const std::filesystem::path &path, std::string_view bytes,
                mode_t mode = 0644) {
  omarchy::plugin_runtime::test_support::TemporaryDirectory::write_file(
      path, bytes, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
}

int create_standard_ustar(const std::filesystem::path &home,
                          std::string_view suffix = "base") {
  const auto source = home / ("archive-source-" + std::string(suffix));
  std::filesystem::create_directories(source / "ui");
  constexpr std::string_view manifest = R"({
    "schemaVersion": 2,
    "id": "org.example.ingress",
    "name": "Ingress",
    "version": "1.0.0",
    "runtime": {"apiVersion": 1, "qml": "ui/Main.qml"},
    "surfaces": {"overlay": {"role": "overlay"}},
    "permissions": {"required": [{"capability": "storage.private", "definitionGeneration": 1, "definitionDigest": "8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66", "operations": ["read", "remove", "write"], "itemBytes": 1024, "quotaBytes": 1024, "reason": "state"}], "optional": []}
  })";
  write_file(source / "manifest.json", manifest);
  write_file(source / "ui/Main.qml",
             "import QtQuick\nItem { property string revision: \"" +
                 std::string(suffix) + "\" }\n");
  const auto output = home / ("plugin-" + std::string(suffix) + ".tar");
  expect_child_exit(0, [&] {
    ::execlp("tar", "tar", "--format=ustar", "-cf", output.c_str(), "-C",
             source.c_str(), "ui", "manifest.json", nullptr);
    ::_exit(127);
  });
  const int archive = ::open(output.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  OMARCHY_CHECK(archive >= 0);
  return archive;
}

auto stage_standard_ustar(channel::RuntimeRoots &roots,
                           const std::filesystem::path &home,
                           std::string_view suffix = "base") {
  omarchy::plugin_runtime::UniqueFd archive(create_standard_ustar(home, suffix));
  return roots.stage_revision_for_review(archive.get());
}

class Fixture final {
public:
  explicit Fixture(bool with_roots = true) {
    if (with_roots) {
      create(".local/share/omarchy-plugin-security/v2/revisions");
      create(".local/state/omarchy/plugin-security/v2/activations");
      create(".local/state/omarchy/plugin-security/v2/authority");
      create(".local/state/omarchy/plugin-security/v2/state");
    }
  }

  [[nodiscard]] int open_home() const {
    return ::open(home_.c_str(),
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  }
  [[nodiscard]] const std::filesystem::path &home() const { return home_; }

private:
  void create(const std::filesystem::path &relative) {
    auto current = home_;
    for (const auto &component : relative) {
      current /= component;
      std::filesystem::create_directory(current);
      OMARCHY_CHECK(::chmod(current.c_str(), 0700) == 0);
    }
  }

  omarchy::plugin_runtime::test_support::TemporaryDirectory directory_;
  std::filesystem::path home_ = directory_.path();
};

class ScopedUmask final {
public:
  explicit ScopedUmask(mode_t value) : previous_(::umask(value)) {}
  ~ScopedUmask() { ::umask(previous_); }

private:
  mode_t previous_;
};

std::unique_ptr<channel::RuntimeRoots>
load(const Fixture &fixture, channel::RuntimeRootsError &error,
     std::uint32_t uid = static_cast<std::uint32_t>(::getuid())) {
  const int home = fixture.open_home();
  OMARCHY_CHECK(home >= 0);
  auto result = channel::RuntimeRootsTestAccess::open_from_home_fd(
      home, uid, error);
  ::close(home);
  return result;
}

std::unique_ptr<channel::RuntimeRoots>
provision(const Fixture &fixture, channel::RuntimeRootsError &error,
          std::uint32_t uid = static_cast<std::uint32_t>(::getuid())) {
  const int home = fixture.open_home();
  OMARCHY_CHECK(home >= 0);
  auto result = channel::RuntimeRootsTestAccess::provision_from_home_fd(
      home, uid, error);
  ::close(home);
  return result;
}

using omarchy::plugin_runtime::test_support::open_descriptor_count;

void fresh_home_is_provisioned_privately_and_idempotently() {
  Fixture fixture(false);
  channel::RuntimeRootsError error{};
  std::unique_ptr<channel::RuntimeRoots> roots;
  {
    ScopedUmask hostile(0777);
    roots = provision(fixture, error);
  }
  OMARCHY_CHECK(roots && error == channel::RuntimeRootsError::none);

  const std::array<std::filesystem::path, 12> created{
      ".local",
      ".local/share",
      ".local/share/omarchy-plugin-security",
      ".local/share/omarchy-plugin-security/v2",
      ".local/share/omarchy-plugin-security/v2/revisions",
      ".local/state",
      ".local/state/omarchy",
      ".local/state/omarchy/plugin-security",
      ".local/state/omarchy/plugin-security/v2",
      ".local/state/omarchy/plugin-security/v2/activations",
      ".local/state/omarchy/plugin-security/v2/authority",
      ".local/state/omarchy/plugin-security/v2/state"};
  for (const auto &relative : created) {
    struct stat metadata{};
    const auto path = fixture.home() / relative;
    OMARCHY_CHECK(::lstat(path.c_str(), &metadata) == 0 &&
                S_ISDIR(metadata.st_mode) && metadata.st_uid == ::getuid() &&
                (metadata.st_mode & 07777) == 0700);
  }
  const std::array leaves{
      fixture.home() /
          ".local/share/omarchy-plugin-security/v2/revisions",
      fixture.home() /
          ".local/state/omarchy/plugin-security/v2/activations",
      fixture.home() /
          ".local/state/omarchy/plugin-security/v2/authority",
      fixture.home() / ".local/state/omarchy/plugin-security/v2/state"};
  std::array<struct stat, leaves.size()> before{};
  for (std::size_t index = 0; index < leaves.size(); ++index) {
    OMARCHY_CHECK(std::filesystem::is_empty(leaves[index]) &&
                ::stat(leaves[index].c_str(), &before[index]) == 0);
  }
  roots.reset();
  roots = provision(fixture, error);
  OMARCHY_CHECK(roots && error == channel::RuntimeRootsError::none);
  for (std::size_t index = 0; index < leaves.size(); ++index) {
    struct stat after{};
    OMARCHY_CHECK(::stat(leaves[index].c_str(), &after) == 0 &&
                after.st_dev == before[index].st_dev &&
                after.st_ino == before[index].st_ino &&
                std::filesystem::is_empty(leaves[index]));
  }
}

void provisioning_coexists_with_the_omarchy_compatibility_symlink() {
  Fixture fixture(false);
  const auto local = fixture.home() / ".local";
  const auto share = local / "share";
  const auto compatibility_target = fixture.home() / "system-omarchy";
  std::filesystem::create_directories(share);
  std::filesystem::create_directory(compatibility_target);
  OMARCHY_CHECK(::chmod(local.c_str(), 0755) == 0 &&
              ::chmod(share.c_str(), 0755) == 0 &&
              ::chmod(compatibility_target.c_str(), 0755) == 0);
  write_file(compatibility_target / "sentinel", "Omarchy data tree\n");
  std::filesystem::create_directory_symlink(
      compatibility_target, share / "omarchy");

  channel::RuntimeRootsError error{};
  auto roots = provision(fixture, error);
  OMARCHY_CHECK(roots && error == channel::RuntimeRootsError::none);
  OMARCHY_CHECK(std::filesystem::is_symlink(share / "omarchy") &&
              std::filesystem::exists(compatibility_target / "sentinel") &&
              !std::filesystem::exists(compatibility_target /
                                       "plugin-security"));
  OMARCHY_CHECK(std::filesystem::is_directory(
              share / "omarchy-plugin-security/v2/revisions") &&
              std::filesystem::is_directory(
                  fixture.home() /
                  ".local/state/omarchy/plugin-security/v2/activations"));
}

void provisioning_rejects_untrusted_existing_components() {
  {
    Fixture fixture(false);
    OMARCHY_CHECK(::mkdir((fixture.home() / ".local").c_str(), 0700) == 0 &&
                ::chmod((fixture.home() / ".local").c_str(), 0770) == 0);
    channel::RuntimeRootsError error{};
    OMARCHY_CHECK(!provision(fixture, error) &&
                error == channel::RuntimeRootsError::root_untrusted);
  }
  {
    Fixture fixture(false);
    write_file(fixture.home() / ".local", "not a directory");
    channel::RuntimeRootsError error{};
    OMARCHY_CHECK(!provision(fixture, error));
  }
  {
    Fixture fixture(false);
    const auto target = fixture.home() / "local-target";
    OMARCHY_CHECK(::mkdir(target.c_str(), 0700) == 0);
    std::filesystem::create_directory_symlink(target,
                                               fixture.home() / ".local");
    channel::RuntimeRootsError error{};
    OMARCHY_CHECK(!provision(fixture, error));
  }
  {
    Fixture fixture(false);
    channel::RuntimeRootsError error{};
    OMARCHY_CHECK(!provision(fixture, error,
                       static_cast<std::uint32_t>(::getuid()) + 1) &&
                error == channel::RuntimeRootsError::home_untrusted);
  }
}

void provisioning_substitution_is_rejected_at_each_identity_fence() {
  for (const auto point : {channel::ProvisioningRacePoint::after_mkdir,
                           channel::ProvisioningRacePoint::after_pin,
                           channel::ProvisioningRacePoint::after_named_stat}) {
    Fixture fixture(false);
    provisioning_race_home = fixture.home();
    channel::set_provisioning_race_hook_for_testing(
        point, substitute_first_provisioned_component);
    channel::RuntimeRootsError error{};
    auto roots = provision(fixture, error);
    channel::set_provisioning_race_hook_for_testing(
        channel::ProvisioningRacePoint::none, nullptr);
    OMARCHY_CHECK(!roots && error != channel::RuntimeRootsError::none);
    OMARCHY_CHECK(!std::filesystem::exists(
                fixture.home() /
                ".local/state/omarchy/plugin-security/v2/activations") &&
                !std::filesystem::exists(
                    fixture.home() /
                    ".local/state/omarchy/plugin-security/v2/authority"));
  }
  provisioning_race_home.clear();
}

void concurrent_provisioning_converges_without_leaks() {
  Fixture fixture(false);
  constexpr std::size_t workers = 8;
  std::barrier start(static_cast<std::ptrdiff_t>(workers + 1));
  std::array<std::thread, workers> threads;
  std::atomic<std::size_t> successes{0};
  for (auto &thread : threads) {
    thread = std::thread([&] {
      start.arrive_and_wait();
      channel::RuntimeRootsError error{};
      auto roots = provision(fixture, error);
      if (roots && error == channel::RuntimeRootsError::none)
        ++successes;
    });
  }
  start.arrive_and_wait();
  for (auto &thread : threads)
    thread.join();
  OMARCHY_CHECK(successes == workers);

  const auto activations =
      fixture.home() / ".local/state/omarchy/plugin-security/v2/activations";
  const auto authority =
      fixture.home() / ".local/state/omarchy/plugin-security/v2/authority";
  OMARCHY_CHECK(std::filesystem::is_empty(activations) &&
              std::filesystem::is_empty(authority));

  const auto displaced = fixture.home() / "real-local";
  std::filesystem::rename(fixture.home() / ".local", displaced);
  std::filesystem::create_directory_symlink(displaced,
                                             fixture.home() / ".local");
  const auto descriptors_before = open_descriptor_count();
  for (int attempt = 0; attempt < 128; ++attempt) {
    channel::RuntimeRootsError error{};
    OMARCHY_CHECK(!provision(fixture, error));
  }
  OMARCHY_CHECK(open_descriptor_count() == descriptors_before);
}

void fixed_roots_are_exact_distinct_and_pinned() {
  Fixture fixture;
  channel::RuntimeRootsError error{};
  auto roots = load(fixture, error);
  OMARCHY_CHECK(roots && error == channel::RuntimeRootsError::none &&
              roots->trusted_uid() == static_cast<std::uint32_t>(::getuid()));
  const std::array descriptors{roots->revisions_fd(), roots->activations_fd(),
                               roots->authority_fd(), roots->state_fd()};
  std::array<struct stat, descriptors.size()> metadata{};
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    OMARCHY_CHECK(descriptors[index] >= 0 &&
                (::fcntl(descriptors[index], F_GETFD) & FD_CLOEXEC) != 0 &&
                ::fstat(descriptors[index], &metadata[index]) == 0 &&
                (metadata[index].st_mode & 07777) == 0700);
    for (std::size_t prior = 0; prior < index; ++prior)
      OMARCHY_CHECK(metadata[index].st_dev != metadata[prior].st_dev ||
                  metadata[index].st_ino != metadata[prior].st_ino);
  }

  const auto revisions =
      fixture.home() / ".local/share/omarchy-plugin-security/v2/revisions";
  const auto displaced = fixture.home() / "displaced-revisions";
  std::filesystem::rename(revisions, displaced);
  std::filesystem::create_directory(revisions);
  OMARCHY_CHECK(::chmod(revisions.c_str(), 0700) == 0);
  struct stat pinned{};
  struct stat replacement{};
  OMARCHY_CHECK(::fstat(roots->revisions_fd(), &pinned) == 0 &&
              ::stat(revisions.c_str(), &replacement) == 0 &&
              (pinned.st_dev != replacement.st_dev ||
               pinned.st_ino != replacement.st_ino));
}

void unsafe_home_components_and_roots_fail_closed() {
  struct UnsafeMode {
    std::string_view relative;
    mode_t mode;
    channel::RuntimeRootsError expected;
  };
  for (const auto &[relative, mode, expected] : std::array<UnsafeMode, 3>{{
           {"", 0770, channel::RuntimeRootsError::home_untrusted},
           {".local/share/omarchy-plugin-security", 0770, channel::RuntimeRootsError::root_untrusted},
           {".local/share/omarchy-plugin-security/v2/revisions", 0755, channel::RuntimeRootsError::root_untrusted}}}) {
    Fixture fixture;
    const auto path = relative.empty() ? fixture.home() : fixture.home() / relative;
    OMARCHY_CHECK(::chmod(path.c_str(), mode) == 0);
    channel::RuntimeRootsError error{};
    OMARCHY_CHECK(!load(fixture, error) && error == expected);
  }
  {
    Fixture fixture;
    const auto activations =
        fixture.home() / ".local/state/omarchy/plugin-security/v2/activations";
    const auto moved = fixture.home() / "real-activations";
    std::filesystem::rename(activations, moved);
    std::filesystem::create_directory_symlink(moved, activations);
    channel::RuntimeRootsError error{};
    OMARCHY_CHECK(!load(fixture, error) &&
                error == channel::RuntimeRootsError::path_unavailable);
  }
  {
    Fixture fixture;
    channel::RuntimeRootsError error{};
    OMARCHY_CHECK(!load(fixture, error, static_cast<std::uint32_t>(::getuid()) + 1) &&
                error == channel::RuntimeRootsError::home_untrusted);
  }
  {
    channel::RuntimeRootsError error{};
    auto roots = channel::RuntimeRootsTestAccess::open_from_home_fd(
        -1, static_cast<std::uint32_t>(::getuid()), error);
    OMARCHY_CHECK(!roots &&
                error == channel::RuntimeRootsError::home_untrusted);
  }
}

void absolute_home_walker_rejects_untrusted_paths() {
  struct stat system_home{};
  OMARCHY_CHECK(::stat("/home", &system_home) == 0);
  const auto owner = static_cast<std::uint32_t>(system_home.st_uid);
  channel::RuntimeRootsError error{};
  auto open_home = [&](const char *path, std::uint32_t uid = 0) {
    const int descriptor =
        channel::RuntimeRootsTestAccess::open_absolute_home(
            path, uid == 0 ? owner : uid, error);
    if (descriptor >= 0)
      ::close(descriptor);
    return descriptor;
  };

  OMARCHY_CHECK(open_home("/home") >= 0 &&
              error == channel::RuntimeRootsError::none);
  for (const char *path : {"home", "/home/.", "/home/../home", "/bin"})
    OMARCHY_CHECK(open_home(path) < 0 &&
                error == channel::RuntimeRootsError::home_untrusted);
  const auto *current_account = ::getpwuid(::getuid());
  OMARCHY_CHECK(current_account != nullptr && current_account->pw_dir != nullptr);
  OMARCHY_CHECK(open_home(current_account->pw_dir) < 0 &&
              error == channel::RuntimeRootsError::home_untrusted);

  OMARCHY_CHECK(channel::RuntimeRootsTestAccess::
              absolute_ancestor_is_secure(0, S_IFDIR | 0755, 1000));
  OMARCHY_CHECK(!channel::RuntimeRootsTestAccess::
               absolute_ancestor_is_secure(0, S_IFDIR | 0775, 1000));
}

void account_resolution_is_bounded_and_exact() {
  struct stat system_home{};
  OMARCHY_CHECK(::stat("/home", &system_home) == 0);
  lookup_home_uid = system_home.st_uid;
  channel::RuntimeRootsError error{};
  auto resolve = [&](LookupBehavior behavior) {
    lookup_behavior = behavior;
    lookup_calls = 0;
    const int descriptor =
        channel::RuntimeRootsTestAccess::resolve_account_home(
            static_cast<std::uint32_t>(lookup_home_uid), 128,
            scripted_lookup, error);
    if (descriptor >= 0)
      ::close(descriptor);
    return descriptor;
  };

  OMARCHY_CHECK(resolve(LookupBehavior::mixed_then_success) >= 0 &&
              lookup_calls == 16 &&
              error == channel::RuntimeRootsError::none);
  OMARCHY_CHECK(resolve(LookupBehavior::interrupt_forever) < 0 &&
              lookup_calls == 16 &&
              error == channel::RuntimeRootsError::account_unavailable);
  OMARCHY_CHECK(resolve(LookupBehavior::wrong_result) < 0 &&
              error == channel::RuntimeRootsError::account_unavailable);
  OMARCHY_CHECK(resolve(LookupBehavior::wrong_uid) < 0 &&
              error == channel::RuntimeRootsError::account_unavailable);
  OMARCHY_CHECK(resolve(LookupBehavior::allocation_failure) == -1 &&
              lookup_calls == 1 && error == channel::RuntimeRootsError::resource_exhausted);
  OMARCHY_CHECK(resolve(LookupBehavior::internal_failure) == -1 &&
              lookup_calls == 1 && error == channel::RuntimeRootsError::internal_failure);
}

std::shared_ptr<channel::PluginPermissionAuthority>
open_review_authority(channel::RuntimeRoots &roots, const std::string &plugin) {
  host::UniqueFd authority_fd(::openat(
      roots.authority_fd(), plugin.c_str(),
      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  OMARCHY_CHECK(static_cast<bool>(authority_fd));
  auto registry = std::make_shared<const definitions::TrustedDefinitionRegistry>(
      omarchy::plugin_runtime::test_support::packaged_registry());
  auto services = std::make_shared<const channel::RuntimeServices>();
  return channel::PluginPermissionAuthorityTestAccess::open(
      roots.activations_fd(), roots.revisions_fd(), roots.state_fd(),
      std::move(authority_fd), permissions::PluginId(plugin), roots.trusted_uid(),
      std::move(registry), std::move(services), plugin);
}

void archive_reaches_exact_review_without_fabricating_authority() {
  Fixture fixture;
  channel::RuntimeRootsError error{};
  auto roots = load(fixture, error);
  OMARCHY_CHECK(roots && error == channel::RuntimeRootsError::none);
  auto published = stage_standard_ustar(*roots, fixture.home());
  const auto &plugin = published.verified().manifest.id;
  const auto &digest = published.verified().identity.tree_sha256;

  auto record = host::inspect_activation_record(
      roots->activations_fd(), plugin, roots->trusted_uid());
  OMARCHY_CHECK(record && record->record().plugin_id == plugin &&
              record->record().revision_directory == digest &&
              record->record().revision_sha256 == digest &&
              record->record().state_directory == plugin);
  channel::ActivationCatalogError catalog_error{};
  auto catalog = channel::ActivationCatalog::load(
      roots->activations_fd(), roots->trusted_uid(), catalog_error);
  OMARCHY_CHECK(catalog && catalog_error == channel::ActivationCatalogError::none &&
              catalog->entries().size() == 1 &&
              catalog->entries().front().plugin_id() == plugin);

  auto authority = open_review_authority(*roots, plugin);
  OMARCHY_CHECK(authority != nullptr);
  host::DescriptorRevisionVerifier verifier(roots->trusted_uid());
  const auto verified = verifier.verify_open_revision(published.descriptor());
  OMARCHY_CHECK(verified && verified->tree_sha256 == digest &&
              verified->request_sha256 ==
                  published.verified().identity.request_sha256);
  const auto review =
      channel::PluginPermissionAuthorityTestAccess::prepare_review(*authority);
  OMARCHY_CHECK(review && review->verified.tree_sha256 == digest &&
              review->candidate_binding.revision.view() == digest &&
              review->dynamic_rows.size() == 1);
  const std::array decisions{host::DynamicConsentDecision{
      .definition = review->dynamic_rows.front().requested->definition,
      .operations = review->dynamic_rows.front().requested->operations,
      .decided_scope = review->dynamic_rows.front().requested->scope,
      .decision = permissions::UserDecision::deny}};
  host::ConsentConfirmation confirmation{
      .review_fingerprint = review->fingerprint,
      .decision_fingerprint = host::consent_decision_fingerprint(
          *review, decisions),
      .actor = permissions::DecisionActor::trusted_ui,
      .confirmed_wall_seconds = 1};
  const auto applied = channel::PluginPermissionAuthorityTestAccess::apply_review(
      *authority, *review, confirmation, decisions);
  OMARCHY_CHECK(applied.publication == host::ConsentResult::required_denied);
  const auto view = channel::PluginPermissionAuthorityTestAccess::list(*authority);
  OMARCHY_CHECK(view && !view->authority_slots.active &&
              !view->authority_slots.candidate &&
              view->authority_slots.sequence == 0);
}

void candidate_crashes_recover_to_complete_state(bool replacing) {
  for (const auto point : {channel::CandidateRecordCrashPoint::write,
                           channel::CandidateRecordCrashPoint::file_sync,
                           channel::CandidateRecordCrashPoint::rename,
                           channel::CandidateRecordCrashPoint::directory_sync}) {
    Fixture fixture;
    channel::RuntimeRootsError error{};
    auto roots = load(fixture, error);
    OMARCHY_CHECK(roots != nullptr);
    const int input = create_standard_ustar(fixture.home(), replacing ? "new" : "crash");
    std::string old_digest;
    std::string new_digest;
    if (replacing) {
      const int old_archive = create_standard_ustar(fixture.home(), "old");
      auto old_revision = roots->stage_revision_for_review(old_archive);
      old_digest = old_revision.verified().identity.tree_sha256;
      auto expected_new = roots->stage_revision_for_review(input);
      new_digest = expected_new.verified().identity.tree_sha256;
      OMARCHY_CHECK(::lseek(old_archive, 0, SEEK_SET) == 0);
      auto restored_old = roots->stage_revision_for_review(old_archive);
      OMARCHY_CHECK(restored_old.verified().identity.tree_sha256 == old_digest);
      ::close(old_archive);
      OMARCHY_CHECK(::lseek(input, 0, SEEK_SET) == 0);
    }
    channel::ActivationCatalogError catalog_error{};
    auto before_catalog = channel::ActivationCatalog::load(
        roots->activations_fd(), roots->trusted_uid(), catalog_error);
    OMARCHY_CHECK(before_catalog && before_catalog->unchanged());
    expect_child_exit(90 + static_cast<int>(point), [&] {
      channel::set_candidate_record_crash_point_for_testing(point);
      try {
        auto ignored = roots->stage_revision_for_review(input);
      } catch (...) {
        ::_exit(125);
      }
      ::_exit(126);
    });
    roots.reset();
    roots = load(fixture, error);
    OMARCHY_CHECK(roots != nullptr);
    auto record = host::inspect_activation_record(
        roots->activations_fd(), "org.example.ingress", roots->trusted_uid());
    const bool renamed = point == channel::CandidateRecordCrashPoint::rename ||
                         point == channel::CandidateRecordCrashPoint::directory_sync;
    OMARCHY_CHECK(record.has_value() == (replacing || renamed));
    if (replacing)
      OMARCHY_CHECK(record->record().revision_sha256 == (renamed ? new_digest : old_digest));
    auto catalog = channel::ActivationCatalog::load(
        roots->activations_fd(), roots->trusted_uid(), catalog_error);
    OMARCHY_CHECK(catalog && catalog_error == channel::ActivationCatalogError::none &&
                catalog->entries().size() == (replacing || renamed ? 1U : 0U));
    catalog.reset();
    for (const auto &entry : std::filesystem::directory_iterator(
             fixture.home() /
             ".local/share/omarchy-plugin-security/v2/revisions")) {
      const auto name = entry.path().filename().string();
      if (name.starts_with(".incoming-"))
        continue;
      const int revision = ::open(entry.path().c_str(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                      O_NOFOLLOW);
      OMARCHY_CHECK(revision >= 0);
      const auto verified = omarchy::plugins::discovery::
          discover_open_published_revision(revision, roots->trusted_uid());
      ::close(revision);
      OMARCHY_CHECK(verified.identity.tree_sha256 == name);
    }
    OMARCHY_CHECK(::lseek(input, 0, SEEK_SET) == 0);
    const int retry = replacing ? input : create_standard_ustar(fixture.home(), "retry");
    auto retried = roots->stage_revision_for_review(retry);
    ::close(input);
    if (!replacing)
      ::close(retry);
    if (replacing)
      OMARCHY_CHECK(retried.verified().identity.tree_sha256 == new_digest);
    auto retried_record = host::inspect_activation_record(
        roots->activations_fd(), "org.example.ingress", roots->trusted_uid());
    OMARCHY_CHECK(retried_record &&
                retried_record->record().revision_sha256 == retried.verified().identity.tree_sha256);
    OMARCHY_CHECK(!before_catalog->unchanged());
  }
}

void superseded_product_review_is_stale_without_authority_change() {
  Fixture fixture;
  channel::RuntimeRootsError error{};
  auto roots = load(fixture, error);
  auto old_revision = stage_standard_ustar(*roots, fixture.home(), "review-old");
  const auto plugin = old_revision.verified().manifest.id;
  auto authority = open_review_authority(*roots, plugin);
  OMARCHY_CHECK(authority != nullptr);
  const auto old_review =
      channel::PluginPermissionAuthorityTestAccess::prepare_review(*authority);
  OMARCHY_CHECK(old_review && old_review->verified.tree_sha256 ==
                            old_revision.verified().identity.tree_sha256);

  auto new_revision = stage_standard_ustar(*roots, fixture.home(), "review-new");
  const std::array decisions{host::DynamicConsentDecision{
      .definition = old_review->dynamic_rows.front().requested->definition,
      .operations = old_review->dynamic_rows.front().requested->operations,
      .decided_scope = old_review->dynamic_rows.front().requested->scope,
      .decision = permissions::UserDecision::deny}};
  const host::ConsentConfirmation confirmation{
      .review_fingerprint = old_review->fingerprint,
      .decision_fingerprint = host::consent_decision_fingerprint(
          *old_review, decisions),
      .actor = permissions::DecisionActor::trusted_ui,
      .confirmed_wall_seconds = 1};
  const auto rejected = channel::PluginPermissionAuthorityTestAccess::apply_review(
      *authority, *old_review, confirmation, decisions);
  OMARCHY_CHECK(rejected.publication == host::ConsentResult::invalid_review &&
              !rejected.binding);
  const auto view = channel::PluginPermissionAuthorityTestAccess::list(*authority);
  OMARCHY_CHECK(view && view->authority_slots.sequence == 0 &&
              !view->authority_slots.active && !view->authority_slots.candidate);
  const auto current =
      channel::PluginPermissionAuthorityTestAccess::prepare_review(*authority);
  OMARCHY_CHECK(current && current->verified.tree_sha256 ==
                         new_revision.verified().identity.tree_sha256);
}

void concurrent_candidates_publish_one_exact_record_and_two_revisions() {
  Fixture fixture;
  channel::RuntimeRootsError error{};
  auto roots = load(fixture, error);
  const int left_archive = create_standard_ustar(fixture.home(), "parallel-a");
  const int right_archive = create_standard_ustar(fixture.home(), "parallel-b");
  std::barrier start(3);
  std::atomic<int> failures{0};
  std::string left_digest;
  std::string right_digest;
  auto stage = [&](int archive, std::string &digest) {
    start.arrive_and_wait();
    try {
      auto revision = roots->stage_revision_for_review(archive);
      digest = revision.verified().identity.tree_sha256;
    } catch (...) {
      ++failures;
    }
  };
  std::thread left(stage, left_archive, std::ref(left_digest));
  std::thread right(stage, right_archive, std::ref(right_digest));
  start.arrive_and_wait();
  left.join();
  right.join();
  ::close(left_archive);
  ::close(right_archive);
  OMARCHY_CHECK(failures == 0 && left_digest.size() == 64 &&
              right_digest.size() == 64 && left_digest != right_digest);
  auto record = host::inspect_activation_record(
      roots->activations_fd(), "org.example.ingress", roots->trusted_uid());
  OMARCHY_CHECK(record && (record->record().revision_sha256 == left_digest ||
                     record->record().revision_sha256 == right_digest));
  for (const auto &digest : {left_digest, right_digest}) {
    const int revision = ::openat(roots->revisions_fd(), digest.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                      O_NOFOLLOW);
    OMARCHY_CHECK(revision >= 0);
    const auto verified = omarchy::plugins::discovery::
        discover_open_published_revision(revision, roots->trusted_uid());
    ::close(revision);
    OMARCHY_CHECK(verified.identity.tree_sha256 == digest);
  }
}

void hostile_umask_is_normalized_at_every_published_leaf() {
  Fixture fixture;
  channel::RuntimeRootsError error{};
  auto roots = load(fixture, error);
  const int input = create_standard_ustar(fixture.home(), "umask");
  std::optional<omarchy::plugins::discovery::PublishedRevision> published;
  {
    ScopedUmask hostile(0777);
    published.emplace(roots->stage_revision_for_review(input));
  }
  ::close(input);
  const auto &plugin = published->verified().manifest.id;
  const auto &digest = published->verified().identity.tree_sha256;
  struct stat metadata{};
  OMARCHY_CHECK(::fstat(published->descriptor(), &metadata) == 0 &&
              (metadata.st_mode & 07777) == 0555);
  for (const auto &pair : {std::pair{roots->state_fd(), plugin},
                           std::pair{roots->authority_fd(), plugin}}) {
    const int directory = ::openat(pair.first, pair.second.c_str(),
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                       O_NOFOLLOW);
    OMARCHY_CHECK(directory >= 0 && ::fstat(directory, &metadata) == 0 &&
                (metadata.st_mode & 07777) == 0700);
    ::close(directory);
  }
  const int record = ::openat(roots->activations_fd(), plugin.c_str(),
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  OMARCHY_CHECK(record >= 0 && ::fstat(record, &metadata) == 0 &&
              (metadata.st_mode & 07777) == 0600);
  ::close(record);
  OMARCHY_CHECK(host::inspect_activation_record(roots->activations_fd(), plugin,
                                          roots->trusted_uid())
                  ->record()
                  .revision_sha256 == digest);
}

void live_catalog_does_not_hold_the_transaction_lock() {
  Fixture fixture;
  channel::RuntimeRootsError error{};
  auto roots = load(fixture, error);
  auto first = stage_standard_ustar(*roots, fixture.home(), "catalog-a");
  (void)first;
  channel::ActivationCatalogError catalog_error{};
  auto catalog = channel::ActivationCatalog::load(
      roots->activations_fd(), roots->trusted_uid(), catalog_error);
  OMARCHY_CHECK(catalog && catalog->unchanged());
  auto second = stage_standard_ustar(*roots, fixture.home(), "catalog-b");
  (void)second;
  OMARCHY_CHECK(!catalog->unchanged());
}

void concurrent_catalog_scan_waits_for_an_exact_transaction_epoch() {
  Fixture fixture;
  channel::RuntimeRootsError error{};
  auto roots = load(fixture, error);
  auto initial = stage_standard_ustar(*roots, fixture.home(), "scan-base");
  (void)initial;
  for (int iteration = 0; iteration < 10; ++iteration) {
    const auto suffix = "scan-" + std::to_string(iteration);
    const int update_archive = create_standard_ustar(fixture.home(), suffix);
    std::barrier start(3);
    std::atomic<bool> writer_ok{false};
    std::atomic<bool> reader_ok{false};
    std::thread writer([&] {
      start.arrive_and_wait();
      try {
        auto update = roots->stage_revision_for_review(update_archive);
        (void)update;
        writer_ok = true;
      } catch (...) {
      }
    });
    std::thread reader([&] {
      start.arrive_and_wait();
      channel::ActivationCatalogError catalog_error{};
      auto catalog = channel::ActivationCatalog::load(
          roots->activations_fd(), roots->trusted_uid(), catalog_error);
      reader_ok = catalog &&
                  catalog_error == channel::ActivationCatalogError::none &&
                  catalog->entries().size() == 1;
    });
    start.arrive_and_wait();
    writer.join();
    reader.join();
    ::close(update_archive);
    OMARCHY_CHECK(writer_ok && reader_ok);
  }
}

void product_review_rejects_mutated_published_metadata() {
  Fixture fixture;
  channel::RuntimeRootsError error{};
  auto roots = load(fixture, error);
  auto published = stage_standard_ustar(*roots, fixture.home(), "metadata");
  const auto plugin = published.verified().manifest.id;
  auto authority = open_review_authority(*roots, plugin);
  OMARCHY_CHECK(authority && channel::PluginPermissionAuthorityTestAccess::
                           prepare_review(*authority));
  OMARCHY_CHECK(::fchmodat(published.descriptor(), "manifest.json", 0644, 0) == 0);
  OMARCHY_CHECK(!channel::PluginPermissionAuthorityTestAccess::prepare_review(
              *authority));
  const auto view = channel::PluginPermissionAuthorityTestAccess::list(*authority);
  OMARCHY_CHECK(view && view->authority_slots.sequence == 0 &&
              !view->authority_slots.active && !view->authority_slots.candidate);
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    fresh_home_is_provisioned_privately_and_idempotently();
    provisioning_coexists_with_the_omarchy_compatibility_symlink();
    provisioning_rejects_untrusted_existing_components();
    provisioning_substitution_is_rejected_at_each_identity_fence();
    concurrent_provisioning_converges_without_leaks();
    fixed_roots_are_exact_distinct_and_pinned();
    unsafe_home_components_and_roots_fail_closed();
    absolute_home_walker_rejects_untrusted_paths();
    account_resolution_is_bounded_and_exact();
    archive_reaches_exact_review_without_fabricating_authority();
    candidate_crashes_recover_to_complete_state(false);
    candidate_crashes_recover_to_complete_state(true);
    superseded_product_review_is_stale_without_authority_change();
    concurrent_candidates_publish_one_exact_record_and_two_revisions();
    hostile_umask_is_normalized_at_every_published_leaf();
    live_catalog_does_not_hold_the_transaction_lock();
    concurrent_catalog_scan_waits_for_an_exact_transaction_epoch();
    product_review_rejects_mutated_published_metadata();
    return 0;
  }, "runtime roots test failed: ");
}
