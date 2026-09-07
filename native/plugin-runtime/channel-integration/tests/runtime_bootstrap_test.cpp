#include "../../tests/support/runtime_bootstrap_fixture.hpp"
#include "../../tests/support/permission_fixture.hpp"
#include "../../tests/support/test_assert.hpp"

#include "runtime_bootstrap.hpp"
#include "runtime_roots_test_access.hpp"
#include "plugin_permission_authority_test_access.hpp"
#include "../../tests/support/child_process.hpp"
#include "../../tests/support/qt_wait.hpp"
#include "omarchy/plugin_runtime/launcher/test_supervisor.h"

#include "capability_definition_loader.hpp"
#include "omarchy/plugin_runtime/Version.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

#include <filesystem>
#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace channel = omarchy::plugin_runtime::channel;
namespace definitions = omarchy::plugins::definitions;
namespace manifest = omarchy::plugins::manifest;
namespace permissions = omarchy::plugins::permissions;
namespace host_session = omarchy::plugin_runtime::host_session;
namespace policy = omarchy::plugin_runtime::policy;

namespace {

using omarchy::plugin_runtime::test_support::require;
using namespace omarchy::plugin_runtime::test_support;

class Fixture final : public omarchy::plugin_runtime::test_support::RuntimeBootstrapTree {
public:
  Fixture() {
    create_authority("example.plugin");
  }

  void create_admin(mode_t mode = 0755) {
    create(admin(), mode);
  }
  void create_authority(std::string_view plugin, mode_t mode = 0700) {
    create(authority(plugin), mode);
  }
  void seed_runtime(std::string_view plugin,
                    std::string_view revision_directory) {
    const auto revision = revisions() / std::string(revision_directory);
    create(revision / "ui", 0755);
    {
      std::ofstream manifest_file(revision / "manifest.json");
      manifest_file
          << "{\n  \"schemaVersion\": 2,\n  \"id\": \"" << plugin
          << "\",\n  \"name\": \"Fixture\",\n"
             "  \"version\": \"1.0.0\",\n"
             "  \"runtime\": {\"apiVersion\": 1, \"qml\": "
             "\"ui/Main.qml\"},\n  \"surfaces\": {\"bar\": {"
             "\"role\": \"bar-embedded\", \"defaultSection\": "
             "\"right\"}},\n  \"permissions\": {\"required\": [], "
             "\"optional\": []}\n}\n";
    }
    std::ofstream(revision / "ui/Main.qml") << "import QtQuick\nItem {}\n";
    const auto verified = omarchy::plugin_runtime::test_support::freeze_revision(
        revision, plugin);
    create(state() / std::string(plugin), 0700);

    const auto record_path = activations() / std::string(plugin);
    std::ofstream(record_path)
        << "format=omarchy-plugin-activation-v2\nplugin=" << plugin
        << "\nrevision-directory=" << revision_directory
        << "\nrevision-sha256=" << verified.tree_sha256
        << "\nstate-directory=" << plugin << "\n";
    OMARCHY_CHECK(::chmod(record_path.c_str(), 0600) == 0);

    auto store = open_authority_store(plugin);
    definitions::TrustedDefinitionRegistry registry;
    const auto snapshot = permission_snapshot(registry, verified.manifest,
                                              verified.tree_sha256);
    OMARCHY_CHECK(store->publish_candidate(verified, snapshot, 0, registry) ==
                    host_session::AuthorityMutationResult::applied &&
                store->promote_candidate(snapshot.binding, 1) ==
                    host_session::AuthorityMutationResult::applied);
  }

};

std::unique_ptr<channel::PreparedPluginSession> prepare_runtime_for_test(
    const channel::RuntimeBootstrap &bootstrap, std::string_view record,
    const permissions::PluginId &plugin) {
  auto authority = channel::RuntimeBootstrapTestAccess::open_permissions(
      bootstrap, record, plugin);
  return channel::RuntimeBootstrapTestAccess::prepare_runtime(bootstrap,
                                                               authority)
      .runtime;
}

definitions::CapabilityDefinition dynamic_definition() {
  definitions::CapabilityDefinition definition{
      .canonical_name = definitions::Name("local.test"),
      .authority_identity = definitions::Name("local.test-v1"),
      .enforcement_family = definitions::EnforcementFamily::network_fetch,
      .display_category_id = definitions::Name("local.testing"),
      .display_category_label = definitions::Label("Local testing"),
      .title = definitions::Label("Test a dynamic adapter"),
      .risk_text = definitions::Label("Exercises a test-only definition"),
      .risk = definitions::RiskLevel::moderate,
      .revocation = definitions::RevocationPolicy::cancel_inflight,
      .adapter = {.adapter_class = definitions::Name("unregistered-adapter"),
                  .contract_digest =
                      definitions::Digest(std::string(64, 'a')),
                  .abi_version = 1},
      .operations = {},
  };
  OMARCHY_CHECK(definition.operations.insert(
              {.name = definitions::Name("read"),
               .label = definitions::Label("Read test data")}));
  return definition;
}

std::string hex(char value) { return std::string(64, value); }

struct Review final {
  host_session::VerifiedRevision verified;
  policy::GrantSnapshot snapshot;
};

Review review(std::string_view plugin, std::uint64_t generation,
              char revision) {
  manifest::ManifestV2 plugin_manifest;
  plugin_manifest.id = std::string(plugin);
  const auto registry = packaged_registry();
  plugin_manifest.requests.push_back(capability_request(
      registry, "notifications.send", R"({"categories":["status"]})"));
  auto snapshot = permission_snapshot(registry, plugin_manifest, hex(revision), generation);
  return {
      .verified = {.manifest = std::move(plugin_manifest),
                   .tree_sha256 = hex(revision),
                   .request_sha256 = std::string(
                       snapshot.binding.policy_fingerprint.view())},
      .snapshot = std::move(snapshot),
  };
}

void empty_package_and_absent_admin_compose_one_shared_context() {
  Fixture fixture;
  fixture.create_authority("second.plugin");
  fixture.seed_runtime("example.plugin", "first-installed");
  fixture.seed_runtime("second.plugin", "second-installed");
  channel::RuntimeBootstrapError error{};
  auto bootstrap = fixture.open_bootstrap(error);
  OMARCHY_CHECK(bootstrap && error == channel::RuntimeBootstrapError::none &&
              channel::RuntimeBootstrapTestAccess::has_fixed_service_context(
                  *bootstrap));

  const permissions::PluginId plugin("example.plugin");
  OMARCHY_CHECK(!prepare_runtime_for_test(
              *bootstrap, "other.plugin", plugin));
  auto first = prepare_runtime_for_test(
      *bootstrap, "example.plugin", plugin);
  auto second = prepare_runtime_for_test(
      *bootstrap, "second.plugin", permissions::PluginId("second.plugin"));
  OMARCHY_CHECK(first && second);
  OMARCHY_CHECK(!prepare_runtime_for_test(
              *bootstrap, "example.plugin", plugin));
}

class AuthorWorkerScope final : public omarchy::plugin_runtime::launcher::test_support::ReadyScope {
public:
  using Deadline = omarchy::plugin_runtime::launcher::Deadline;
  AttachResult attach(std::string_view, pid_t monitor_pid, pid_t worker_pid,
                      const omarchy::plugin_runtime::sandbox::SandboxPlan &plan,
                      Deadline, std::string &) override {
    if (monitor_pid <= 0 || worker_pid <= 0 ||
        plan.worker_descriptors != std::vector<int>({3, 4, 5}))
      return {};
    peer.store(worker_pid);
    ++attachments;
    return {.attached = true, .cleanup_required = true};
  }
  bool terminate_scope_validated(std::string_view, Deadline, std::string &) noexcept override {
    ++terminations;
    return true;
  }
  std::atomic<pid_t> peer{-1};
  std::atomic<int> attachments{0};
  std::atomic<int> terminations{0};
};

class AuthorWorkerHooks final : public channel::PluginRuntimeHooks {
public:
  void state_changed(host_session::SessionState value, host_session::SessionError) override {
    state.store(value);
  }
  void render_rejected(host_session::RouteResult) override {}
  bool accept(host_session::AdmittedSurfaceIntent) override { return false; }
  std::atomic<host_session::SessionState> state{host_session::SessionState::idle};
};

void author_archive_requires_exact_review_before_activation(bool run_worker = false) {
  Fixture fixture;
  for (const auto &definition : definitions::packaged_definitions())
    TemporaryDirectory::write_file(
        fixture.package() / (std::string(definition.canonical_name.view()) + ".capability"),
        definitions::canonical_definition_document(definition, 1),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  const auto source = fixture.path() / "author-source";
  fixture.create(source, 0700);
  constexpr std::string_view author = R"({
    "authoringVersion":1,"id":"org.example.author-install","name":"Author install",
    "version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},
    "surfaces":{"bar":{"role":"bar-embedded","defaultSection":"right"}},
    "permissions":{"required":[{"capability":"storage.private",
      "definitionVersions":{"minimum":1,"maximum":1},"operations":["read","write"],
      "quotaBytes":4096,"itemBytes":1024,"reason":"Restore preferences"}],"optional":[]}
  })";
  TemporaryDirectory::write_file(source / "manifest.author.json", author,
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  TemporaryDirectory::write_file(source / "Main.qml", R"(import QtQuick
Item {
  id: proofRoot
  property bool requested: false
  function writeProof() {
    if (!runtime.brokerReady || requested) return
    requested = true
    runtime.invoke("storage.private", "write", {
      key: "author-proof", value: "resolved-author-runtime"
    })
  }
  Component.onCompleted: writeProof()
  Connections {
    target: runtime
    function onBrokerReadyChanged() { proofRoot.writeProof() }
  }
}
)",
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  const auto archive_path = fixture.path() / "author.tar";
  expect_child_exit(0, [&] {
    ::execlp("tar", "tar", "--format=ustar", "-cf", archive_path.c_str(), "-C",
             source.c_str(), "manifest.author.json", "Main.qml", nullptr);
    ::_exit(127);
  });
  host_session::UniqueFd archive(::open(archive_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  OMARCHY_CHECK(archive);
  channel::RuntimeBootstrapError error{};
  auto bootstrap = fixture.open_bootstrap(error);
  OMARCHY_CHECK(bootstrap && error == channel::RuntimeBootstrapError::none);
  auto scope = std::make_shared<AuthorWorkerScope>();
  if (run_worker)
    channel::RuntimeBootstrapTestAccess::set_supervisor_factory(*bootstrap, [scope] {
      return omarchy::plugin_runtime::launcher::test_support::make_supervisor(
          "/usr/bin/bwrap", QML_WORKER_PATH, scope);
    });
  const auto installed = channel::RuntimeBootstrapTestAccess::stage_revision_for_review(
      *bootstrap, archive.get());
  const permissions::PluginId plugin("org.example.author-install");
  auto authority = channel::RuntimeBootstrapTestAccess::open_permissions(
      *bootstrap, plugin.view(), plugin);
  OMARCHY_CHECK(authority);
  const auto initial = channel::PluginPermissionAuthorityTestAccess::list(*authority);
  OMARCHY_CHECK(initial && !initial->active && initial->authority_slots.sequence == 0);
  OMARCHY_CHECK(!channel::RuntimeBootstrapTestAccess::prepare_runtime(*bootstrap, authority).runtime);
  const auto proof = fixture.state() / std::string(plugin.view()) / "author-proof";
  OMARCHY_CHECK(scope->attachments == 0 && !std::filesystem::exists(proof));
  const auto review = channel::PluginPermissionAuthorityTestAccess::prepare_review(*authority);
  OMARCHY_CHECK(review && review->verified.manifest == installed.verified().manifest &&
              review->verified.tree_sha256 == installed.verified().identity.tree_sha256 &&
              review->dynamic_rows.size() == 1 && review->dynamic_rows[0].requested);
  const auto &request = *review->dynamic_rows[0].requested;
  OMARCHY_CHECK(request.definition.definition_generation == 1 &&
              request.definition.canonical_name.view() == "storage.private");
  const std::array decisions{host_session::DynamicConsentDecision{
      .definition = request.definition, .operations = request.operations,
      .decided_scope = request.scope, .decision = permissions::UserDecision::grant}};
  const host_session::ConsentConfirmation confirmation{
      .review_fingerprint = review->fingerprint,
      .decision_fingerprint = host_session::consent_decision_fingerprint(*review, decisions),
      .actor = permissions::DecisionActor::trusted_ui, .confirmed_wall_seconds = 1};
  const auto applied = channel::PluginPermissionAuthorityTestAccess::apply_review(
      *authority, *review, confirmation, decisions);
  OMARCHY_CHECK(applied.publication == host_session::ConsentResult::applied &&
              applied.promotion == host_session::AuthorityMutationResult::applied && applied.binding);
  auto prepared = channel::RuntimeBootstrapTestAccess::prepare_runtime(*bootstrap, authority).runtime;
  OMARCHY_CHECK(prepared);
  if (run_worker) {
    AuthorWorkerHooks hooks;
    auto root = channel::ReviewedSessionTestAccess::commit(
        std::move(prepared), hooks, *QCoreApplication::instance());
    OMARCHY_CHECK(root);
    const bool completed = awaitFor(std::chrono::seconds(10), [&] {
      return (hooks.state == host_session::SessionState::running && std::filesystem::exists(proof)) ||
             hooks.state == host_session::SessionState::failed;
    });
    require(completed, "author worker proof timeout: state=" +
        std::to_string(static_cast<unsigned>(hooks.state.load())) +
        " attachments=" + std::to_string(scope->attachments.load()) +
        " proof=" + std::to_string(std::filesystem::exists(proof)));
    OMARCHY_CHECK(hooks.state == host_session::SessionState::running && scope->attachments == 1);
    std::ifstream result(proof);
    const std::string value{std::istreambuf_iterator<char>(result), std::istreambuf_iterator<char>()};
    OMARCHY_CHECK(value == "resolved-author-runtime");
    auto &surfaces = channel::ReviewedSessionTestAccess::surface_session(*root);
    const auto bar = surfaces.describe("bar");
    OMARCHY_CHECK(bar && bar->binding == *applied.binding &&
                bar->binding.revision.view() == installed.verified().identity.tree_sha256);
    root.reset();
    OMARCHY_CHECK(awaitFor(std::chrono::seconds(10), [&] {
      return scope->terminations == 1 && scope->peer > 0 &&
             ::kill(scope->peer.load(), 0) < 0 && errno == ESRCH;
    }));
  }
}

void authority_cannot_cross_runtime_service_identity() {
  Fixture fixture;
  fixture.seed_runtime("example.plugin", "installed");
  const auto shared_definitions =
      std::make_shared<const definitions::TrustedDefinitionRegistry>();
  const auto first_services =
      std::make_shared<const channel::RuntimeServices>();
  const auto second_services =
      std::make_shared<const channel::RuntimeServices>();
  auto first = channel::RuntimeBootstrapTestAccess::compose_with_context(
      fixture.roots(), shared_definitions, first_services);
  auto second = channel::RuntimeBootstrapTestAccess::compose_with_context(
      fixture.roots(), shared_definitions, second_services);
  OMARCHY_CHECK(first && second);
  const permissions::PluginId plugin("example.plugin");
  auto authority = channel::RuntimeBootstrapTestAccess::open_permissions(
      *first, plugin.view(), plugin);
  OMARCHY_CHECK(authority != nullptr);
  const auto crossed = channel::RuntimeBootstrapTestAccess::prepare_runtime(
      *second, authority);
  OMARCHY_CHECK(!crossed.runtime && !crossed.permission_disabled);
  OMARCHY_CHECK(channel::RuntimeBootstrapTestAccess::prepare_runtime(*first,
                                                                authority)
                  .runtime != nullptr);
}

void mandatory_package_and_optional_admin_are_exact() {
  const auto reject = [](auto mutate, channel::RuntimeBootstrapError expected) {
    Fixture fixture;
    mutate(fixture);
    channel::RuntimeBootstrapError error{};
    OMARCHY_CHECK(!fixture.open_bootstrap(error) && error == expected);
  };
  {
    Fixture fixture;
    channel::RuntimeBootstrapError error{};
    OMARCHY_CHECK(!channel::RuntimeBootstrapTestAccess::
                 open_from_filesystem_root(
                     fixture.roots(), -1,
                     static_cast<std::uint32_t>(::getuid()), error) &&
                error == channel::RuntimeBootstrapError::
                             package_definitions_untrusted);
  }
  reject([](Fixture &fixture) {
    std::filesystem::remove(fixture.package());
  }, channel::RuntimeBootstrapError::package_definitions_unavailable);
  reject([](Fixture &fixture) {
    OMARCHY_CHECK(::chmod(fixture.package().c_str(), 0775) == 0);
  }, channel::RuntimeBootstrapError::package_definitions_untrusted);
  reject([](Fixture &fixture) {
    const auto package = fixture.package();
    const auto target = package.parent_path() / "alternate-capabilities";
    std::filesystem::remove(package);
    std::filesystem::create_directory(target);
    OMARCHY_CHECK(::chmod(target.c_str(), 0755) == 0);
    std::filesystem::create_directory_symlink(target, package);
  }, channel::RuntimeBootstrapError::package_definitions_untrusted);
  reject([](Fixture &fixture) {
    fixture.create_admin(0775);
  }, channel::RuntimeBootstrapError::admin_definitions_untrusted);
  reject([](Fixture &fixture) {
    fixture.create_admin();
    const auto document = fixture.admin() / "broken.capability";
    {
      std::ofstream output(document, std::ios::binary);
      output << "not a capability definition";
    }
    OMARCHY_CHECK(::chmod(document.c_str(), 0644) == 0);
  }, channel::RuntimeBootstrapError::definition_document_rejected);
}

void authority_children_are_exact_and_never_created() {
  const auto uid = static_cast<std::uint32_t>(::getuid());
  OMARCHY_CHECK(channel::RuntimeBootstrapTestAccess::
                  authority_directory_accepted(uid, S_IFDIR | 0700, uid) &&
              !channel::RuntimeBootstrapTestAccess::
                  authority_directory_accepted(uid + 1, S_IFDIR | 0700, uid) &&
              !channel::RuntimeBootstrapTestAccess::
                  authority_directory_accepted(uid, S_IFDIR | 0750, uid) &&
              !channel::RuntimeBootstrapTestAccess::
                  authority_directory_accepted(uid, S_IFREG | 0700, uid));

  const permissions::PluginId plugin("example.plugin");
  const auto rejects_authority = [&](const Fixture &fixture) {
    channel::RuntimeBootstrapError error{};
    auto bootstrap = fixture.open_bootstrap(error);
    OMARCHY_CHECK(bootstrap &&
                !prepare_runtime_for_test(*bootstrap, plugin.view(), plugin));
  };
  {
    Fixture fixture;
    std::filesystem::remove(fixture.authority("example.plugin"));
    rejects_authority(fixture);
    OMARCHY_CHECK(!std::filesystem::exists(fixture.authority("example.plugin")));
  }
  {
    Fixture fixture;
    OMARCHY_CHECK(::chmod(fixture.authority("example.plugin").c_str(), 0750) == 0);
    rejects_authority(fixture);
  }
  {
    Fixture fixture;
    const auto child = fixture.authority("example.plugin");
    const auto target = fixture.authority("alternate.plugin");
    std::filesystem::remove(child);
    fixture.create_authority("alternate.plugin");
    std::filesystem::create_directory_symlink(target, child);
    rejects_authority(fixture);
  }
  {
    Fixture fixture;
    channel::RuntimeBootstrapError error{};
    auto bootstrap = fixture.open_bootstrap(error);
    const permissions::PluginId path_plugin("../example.plugin");
    OMARCHY_CHECK(bootstrap &&
                !prepare_runtime_for_test(
                    *bootstrap, path_plugin.view(), path_plugin));
  }
}

void authority_stores_are_physically_isolated_per_plugin() {
  Fixture fixture;
  fixture.create_authority("second.plugin");
  const permissions::PluginId first_plugin("example.plugin");
  const permissions::PluginId second_plugin("second.plugin");
  const int first_authority =
      ::open(fixture.authority(first_plugin.view()).c_str(),
             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  const int second_authority =
      ::open(fixture.authority(second_plugin.view()).c_str(),
             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  OMARCHY_CHECK(first_authority >= 0 && second_authority >= 0 &&
              (::fcntl(first_authority, F_GETFD) & FD_CLOEXEC) != 0 &&
              (::fcntl(second_authority, F_GETFD) & FD_CLOEXEC) != 0);

  auto first_store = host_session::AuthorityStore::open(
      first_authority, ::getuid(), first_plugin);
  auto second_store = host_session::AuthorityStore::open(
      second_authority, ::getuid(), second_plugin);
  OMARCHY_CHECK(first_store && second_store);
  OMARCHY_CHECK(!host_session::AuthorityStore::open(
              first_authority, ::getuid(), first_plugin));
  ::close(first_authority);
  ::close(second_authority);

  auto registry = packaged_registry();
  auto first_review = review(first_plugin.view(), 1, 'a');
  auto second_review = review(second_plugin.view(), 1, 'b');
  OMARCHY_CHECK(first_store->publish_candidate(first_review.verified,
                                         first_review.snapshot, 0, registry) ==
                  host_session::AuthorityMutationResult::applied &&
              second_store->publish_candidate(second_review.verified,
                                               second_review.snapshot, 0,
                                               registry) ==
                  host_session::AuthorityMutationResult::applied &&
              first_store->promote_candidate(first_review.snapshot.binding,
                                             1) ==
                  host_session::AuthorityMutationResult::applied &&
              second_store->promote_candidate(second_review.snapshot.binding,
                                              1) ==
                  host_session::AuthorityMutationResult::applied);
  OMARCHY_CHECK(first_store->resolve(first_plugin.view(), hex('a')).status ==
                  host_session::GrantStatus::activatable &&
              second_store->resolve(second_plugin.view(), hex('b')).status ==
                  host_session::GrantStatus::activatable);

  auto replacement = review(first_plugin.view(), 2, 'c');
  OMARCHY_CHECK(first_store->publish_candidate(replacement.verified,
                                         replacement.snapshot, 2, registry) ==
                  host_session::AuthorityMutationResult::applied &&
              first_store->promote_candidate(replacement.snapshot.binding,
                                             3) ==
                  host_session::AuthorityMutationResult::applied &&
              second_store->resolve(second_plugin.view(), hex('b')).status ==
                  host_session::GrantStatus::activatable);
  const auto first_view = first_store->read_authority_view();
  const auto second_view = second_store->read_authority_view();
  OMARCHY_CHECK(first_view && second_view && first_view->active &&
              second_view->active &&
              first_view->active->binding.plugin == first_plugin &&
              second_view->active->binding.plugin == second_plugin);

  first_store.reset();
  second_store.reset();

  Fixture runtime_fixture;
  runtime_fixture.create_authority("second.plugin");
  runtime_fixture.seed_runtime("example.plugin", "first-installed");
  runtime_fixture.seed_runtime("second.plugin", "second-installed");
  channel::RuntimeBootstrapError error{};
  auto bootstrap = runtime_fixture.open_bootstrap(error);
  auto first = prepare_runtime_for_test(
      *bootstrap, first_plugin.view(), first_plugin);
  auto second = prepare_runtime_for_test(
      *bootstrap, second_plugin.view(), second_plugin);
  OMARCHY_CHECK(first && second &&
              !prepare_runtime_for_test(
                  *bootstrap, first_plugin.view(), first_plugin));
  first.reset();
  OMARCHY_CHECK(static_cast<bool>(
              prepare_runtime_for_test(
                  *bootstrap, first_plugin.view(), first_plugin)));
}

void trusted_definition_loads_without_a_provider() {
  Fixture fixture;
  const auto definition = dynamic_definition();
  const auto document = definitions::canonical_definition_document(definition, 1);
  OMARCHY_CHECK(!document.empty());
  const auto file = fixture.package() / "test.capability";
  {
    std::ofstream output(file, std::ios::binary);
    output << document;
  }
  OMARCHY_CHECK(::chmod(file.c_str(), 0644) == 0);
  channel::RuntimeBootstrapError error{};
  auto bootstrap = fixture.open_bootstrap(error);
  const auto loaded = bootstrap
                          ? channel::RuntimeBootstrapTestAccess::definition(
                                *bootstrap, definition.canonical_name.view())
                          : std::nullopt;
  OMARCHY_CHECK(bootstrap && error == channel::RuntimeBootstrapError::none &&
              loaded && loaded->generation == 1 &&
              loaded->digest == definitions::definition_digest(definition) &&
              loaded->definition->adapter == definition.adapter);
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  return omarchy::plugin_runtime::test_support::test_main([&] {
    if (argc == 2 && std::string_view(argv[1]) == "--author-worker-only") {
      author_archive_requires_exact_review_before_activation(true);
      std::cout << "author archive real-worker and broker-effect test passed\n";
      return 0;
    }
    empty_package_and_absent_admin_compose_one_shared_context();
    author_archive_requires_exact_review_before_activation();
    authority_cannot_cross_runtime_service_identity();
    mandatory_package_and_optional_admin_are_exact();
    authority_children_are_exact_and_never_created();
    authority_stores_are_physically_isolated_per_plugin();
    trusted_definition_loads_without_a_provider();
    return 0;
  }, "runtime bootstrap test failed: ");
}
