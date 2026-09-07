#include "deterministic_jobs.hpp"
#include "../../tests/support/qt_wait.hpp"
#include "../../tests/support/runtime_bootstrap_fixture.hpp"
#include "../../tests/support/permission_fixture.hpp"

#include "PermissionControl.h"
#include "PluginManager.h"

#include "authority_store.hpp"
#include "authority_store_test_access.hpp"
#include "capability_definition.hpp"
#include "omarchy/plugin_runtime/Version.h"
#include "revision_verifier_adapter.hpp"
#include "runtime_bootstrap.hpp"
#include "runtime_roots.hpp"
#include "runtime_roots_test_access.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace bridge = omarchy::plugin_runtime::bridge;
namespace channel = omarchy::plugin_runtime::channel;
namespace definitions = omarchy::plugins::definitions;
namespace host = omarchy::plugin_runtime::host_session;
namespace permissions = omarchy::plugins::permissions;
namespace policy = omarchy::plugin_runtime::policy;

using omarchy::plugin_runtime::test_support::require;
using omarchy::plugin_runtime::test_support::await;
using namespace omarchy::plugin_runtime::test_support;

using Jobs = bridge::test_support::DeterministicJobs;

definitions::CapabilityDefinition collisionDefinition() {
  definitions::CapabilityDefinition definition{
      .canonical_name = definitions::Name("write"),
      .authority_identity = definitions::Name("test.write-v1"),
      .enforcement_family = definitions::EnforcementFamily::cli_harness,
      .display_category_id = definitions::Name("test.contract"),
      .display_category_label = definitions::Label("Contract tests"),
      .title = definitions::Label("Run the contract harness"),
      .risk_text = definitions::Label("Exercises an inert test adapter"),
      .risk = definitions::RiskLevel::moderate,
      .revocation = definitions::RevocationPolicy::deny_new,
      .adapter = {.adapter_class = definitions::Name("contract-adapter"),
                  .contract_digest =
                      definitions::Digest(std::string(64, 'c')),
                  .abi_version = 1},
      .operations = {}};
  OMARCHY_CHECK(definition.operations.insert(
              {.name = definitions::Name("remove"),
               .label = definitions::Label("Remove through harness")}) &&
              definition.operations.insert(
                  {.name = definitions::Name("storage.private"),
                   .label = definitions::Label("Inspect private storage")}));
  return definition;
}

bool inertDispatch(const definitions::AuthorizedDynamicRequest &,
                   std::span<std::byte>, std::size_t &written,
                   void *) noexcept {
  written = 0;
  return true;
}

class RuntimeFixture final : public omarchy::plugin_runtime::test_support::RuntimeBootstrapTree {
public:
  RuntimeFixture() {
    auto registry = std::make_shared<definitions::TrustedDefinitionRegistry>(packaged_registry());
    const auto definition = collisionDefinition();
    OMARCHY_CHECK(registry->install(
                definition, 3));
    const auto installed = registry->find("write");
    OMARCHY_CHECK(installed.has_value());
    definition_digest_ = std::string(installed->digest.view());
    registry_ = std::move(registry);
  }

  void seed(std::string_view plugin, bool start_running = true, bool dependencies = false) {
    const auto revision_name = std::string(plugin) + "-g1";
    const auto revision = revisions() / revision_name;
    create(revision / "ui", 0755);
    const std::string storage =
        "{\"capability\":\"storage.private\",\"reason\":\"storage row\","
        "\"definitionGeneration\":1,\"definitionDigest\":\""
        "8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66\","
        "\"operations\":[\"read\",\"write\",\"remove\"],\"itemBytes\":4096,\"quotaBytes\":8192}";
    const std::string dynamic =
        "{\"capability\":\"write\",\"reason\":\"dynamic row\","
        "\"definitionGeneration\":3,\"definitionDigest\":\"" +
        definition_digest_ +
        "\",\"operations\":[\"remove\",\"storage.private\"],"
        "\"profile\":\"contract\"}";
    const auto permissions_json =
        start_running
            ? "{\"required\":[],\"optional\":[" + storage + ',' + dynamic + "]}"
            : "{\"required\":[" + storage + "],\"optional\":[" + dynamic + "]}";
    {
      std::ofstream manifest(revision / "manifest.json");
      manifest << "{\"schemaVersion\":2,\"id\":\"" << plugin
               << "\",\"name\":\"Permission contract\",\"version\":\"1\","
                  "\"runtime\":{\"apiVersion\":1,\"qml\":\"ui/Main.qml\"},"
                  "\"surfaces\":{},\"permissions\":"
               << permissions_json;
      if (dependencies)
        manifest << R"(,"dependencies":{"aur":["example-git"]})";
      manifest << "}";
    }
    std::ofstream(revision / "ui/Main.qml") << "import QtQuick\nItem {}\n";
    create(state() / std::string(plugin), 0700);
    create(authority() / std::string(plugin), 0700);
    const auto verified = omarchy::plugin_runtime::test_support::freeze_revision(revision, plugin);

    const auto snapshot = permission_snapshot(*registry_, verified.manifest, verified.tree_sha256);
    OMARCHY_CHECK(snapshot.dynamic_grants.size() == 2);

    auto store = open_authority_store(plugin);
    OMARCHY_CHECK(store &&
                store->publish_candidate(verified, snapshot, 0, *registry_) ==
                    host::AuthorityMutationResult::applied &&
                store->promote_candidate(snapshot.binding, 1) ==
                    host::AuthorityMutationResult::applied);
    if (!start_running) {
      const auto view = store->read_authority_view();
      OMARCHY_CHECK(view && view->active &&
                  host::AuthorityStoreTestAccess::revoke_active(
                      *store, snapshot.dynamic_grants.front().request.definition,
                      view->authority_slots.sequence)
                          .status == host::AuthorityMutationResult::applied);
    }
    writeActivation(plugin, revision_name, verified.tree_sha256);
  }

  std::unique_ptr<channel::RuntimeBootstrap>
  bootstrap(bool dynamic_provider = true) const {
    auto services = std::make_shared<channel::RuntimeServices>();
    services->context = std::make_shared<int>(0);
    if (dynamic_provider) {
      services->dynamic_services.push_back(
          {.binding = collisionDefinition().adapter,
           .dispatch = inertDispatch});
    }
    return channel::RuntimeBootstrapTestAccess::compose_with_context(
        roots(), registry_, std::move(services));
  }

private:
  void writeActivation(std::string_view plugin, std::string_view revision,
                       std::string_view digest) {
    const auto path = activations() / std::string(plugin);
    {
      std::ofstream output(path);
      output << "format=omarchy-plugin-activation-v2\nplugin=" << plugin
             << "\nrevision-directory=" << revision
             << "\nrevision-sha256=" << digest << "\nstate-directory=" << plugin
             << "\n";
    }
    OMARCHY_CHECK(::chmod(path.c_str(), 0600) == 0);
  }

  std::shared_ptr<const definitions::TrustedDefinitionRegistry> registry_;
  std::string definition_digest_;
};

QJsonObject poll(bridge::PermissionControl &control, const QString &id) {
  const auto document = QJsonDocument::fromJson(control.poll(id).toUtf8());
  OMARCHY_CHECK(document.isObject());
  return document.object();
}

void runPermission(Jobs &jobs, bridge::PluginManager &manager) {
  OMARCHY_CHECK(!jobs.jobs.empty() &&
              jobs.kinds.front() ==
                  bridge::PluginManagerTestAccess::TestJobKind::permission);
  jobs.runOne();
  bridge::PluginManagerTestAccess::drainRuntime(manager);
}

QJsonArray rows(const QJsonObject &operation) {
  OMARCHY_CHECK(operation.value("state") == "succeeded" &&
              operation.value("result").isObject());
  return operation.value("result").toObject().value("permissions").toArray();
}

QJsonObject rowNamed(const QJsonArray &values, std::string_view name) {
  const auto expected = QString::fromUtf8(name);
  for (const auto &value : values) {
    const auto row = value.toObject();
    if (row.value("name") == expected)
      return row;
  }
  return {};
}

QJsonObject choice(const QJsonObject &row, bool grant,
                   QJsonArray operations = {}) {
  QJsonObject result{{"rowId", row.value("rowId")},
                     {"decision", grant ? "grant" : "deny"}};
  if (grant) {
    if (operations.empty() && row.value("name") == "storage.private")
      for (const auto &value : row.value("operations").toArray())
        operations.push_back(value.toObject().value("operationId"));
    result.insert("operations", operations);
  }
  return result;
}

QString choices(const QJsonObject &builtin, const QJsonObject &dynamic,
                const QJsonArray &operations, bool acknowledge_host = false) {
  QJsonObject document{{"choices", QJsonArray{choice(builtin, true),
      choice(dynamic, true, operations)}}};
  if (acknowledge_host)
    document.insert("hostExtensionAcknowledged", true);
  return QString::fromUtf8(
      QJsonDocument(document)
          .toJson(QJsonDocument::Compact));
}

void requireKeys(const QJsonObject &object,
                 std::initializer_list<std::string_view> expected,
                 std::string_view context) {
  std::vector<std::string> actual;
  for (auto it = object.begin(); it != object.end(); ++it)
    actual.push_back(it.key().toStdString());
  std::vector<std::string> wanted;
  for (const auto key : expected)
    wanted.emplace_back(key);
  std::ranges::sort(actual);
  std::ranges::sort(wanted);
  require(actual == wanted, std::string(context) + " JSON keys changed");
}

void requireNoAuthorityMetadata(const QJsonValue &value) {
  constexpr std::string_view forbidden[] = {"epoch",
                                            "sequence",
                                            "fingerprint",
                                            "version",
                                            "definitionGeneration",
                                            "definitionDigest",
                                            "adapter",
                                            "actor",
                                            "confirmedWallSeconds",
                                            "slotEpoch",
                                            "authoritySequence",
                                            "binding",
                                            "generation"};
  if (value.isArray()) {
    for (const auto &entry : value.toArray())
      requireNoAuthorityMetadata(entry);
    return;
  }
  if (!value.isObject())
    return;
  const auto object = value.toObject();
  for (auto it = object.begin(); it != object.end(); ++it) {
    OMARCHY_CHECK(std::ranges::find(forbidden, it.key().toStdString()) ==
                std::end(forbidden));
    requireNoAuthorityMetadata(it.value());
  }
}

void publicPermissionContract() {
  constexpr std::string_view plugin_a = "a.permission-contract";
  constexpr std::string_view plugin_b = "b.permission-contract";
  RuntimeFixture fixture;
  fixture.seed(plugin_a, true, true);
  fixture.seed(plugin_b);
  Jobs jobs;
  auto manager = bridge::PluginManagerTestAccess::create();
  bridge::PluginManagerTestAccess::installRuntime(*manager,
                                                  fixture.bootstrap());
  jobs.install(*manager);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              jobs.jobs.size() == 2);
  while (!jobs.jobs.empty()) {
    jobs.runOne();
    bridge::PluginManagerTestAccess::drainRuntime(*manager);
  }
  const bool both_running = await([&] {
    bridge::PluginManagerTestAccess::drainRuntime(*manager);
    const auto observations =
        bridge::PluginManagerTestAccess::runtimeSlots(*manager);
    return observations.size() == 2 &&
           std::ranges::all_of(observations,
                               [](const auto &slot) { return slot.running; });
  });
  if (!both_running) {
    const auto observations =
        bridge::PluginManagerTestAccess::runtimeSlots(*manager);
    std::string detail = "permission contract runtimes did not reach running";
    for (const auto &entry : observations)
      detail += " [" + entry.plugin +
                " opening=" + std::to_string(entry.opening) +
                " preparing=" + std::to_string(entry.preparing) +
                " starting=" + std::to_string(entry.starting) +
                " retry=" + std::to_string(entry.retry_wait) +
                " state=" + std::to_string(entry.last_state) +
                " error=" + std::to_string(entry.last_error) + "]";
    throw std::runtime_error(detail);
  }

  auto &control = *manager->permissions();
  const auto pending_id = control.beginReview(QString::fromUtf8(plugin_a));
  OMARCHY_CHECK(!pending_id.isEmpty());
  const auto pending = poll(control, pending_id);
  requireKeys(pending, {"operationId", "kind", "state"}, "pending operation");
  requireNoAuthorityMetadata(pending);
  runPermission(jobs, *manager);
  const auto review_a = poll(control, pending_id);
  requireKeys(review_a, {"operationId", "kind", "state", "result"},
              "successful review");
  requireKeys(review_a.value("result").toObject(), {"plugin", "permissions", "dependencies"},
              "review result");
  OMARCHY_CHECK(review_a.value("result").toObject().value("dependencies").toObject()
                   .value("aur").toArray() == QJsonArray{QStringLiteral("example-git")});
  requireNoAuthorityMetadata(review_a);
  const auto review_rows_a = rows(review_a);
  const auto builtin_a = rowNamed(review_rows_a, "storage.private");
  const auto dynamic_a = rowNamed(review_rows_a, "write");
  OMARCHY_CHECK(!builtin_a.isEmpty() && !dynamic_a.isEmpty());
  requireKeys(builtin_a,
              {"rowId", "name", "required", "available", "state",
               "scope", "operations", "title", "category", "delta", "reason", "trustTier"},
              "builtin review row");
  requireKeys(dynamic_a,
              {"rowId", "name", "required", "available", "state",
               "scope", "operations", "title", "category", "delta", "reason", "trustTier"},
              "dynamic review row");
  const auto operation_rows = dynamic_a.value("operations").toArray();
  OMARCHY_CHECK(operation_rows.size() == 2);
  for (const auto &value : operation_rows)
    requireKeys(value.toObject(), {"operationId", "name", "label"},
                "dynamic operation");

  QString chosen_id;
  QString foreign_operation_id;
  for (const auto &value : operation_rows) {
    const auto operation = value.toObject();
    if (operation.value("name") == "storage.private")
      chosen_id = operation.value("operationId").toString();
    else
      foreign_operation_id = operation.value("operationId").toString();
  }
  OMARCHY_CHECK(!chosen_id.isEmpty() && !foreign_operation_id.isEmpty() &&
              chosen_id != "storage.private");
  const QJsonArray chosen_only{QJsonValue(chosen_id)};
  const QJsonArray chosen_duplicate{QJsonValue(chosen_id),
                                    QJsonValue(chosen_id)};

  const auto review_b_id = control.beginReview(QString::fromUtf8(plugin_b));
  OMARCHY_CHECK(!review_b_id.isEmpty());
  runPermission(jobs, *manager);
  const auto review_b = poll(control, review_b_id);
  const auto review_rows_b = rows(review_b);
  const auto builtin_b = rowNamed(review_rows_b, "storage.private");
  const auto dynamic_b = rowNamed(review_rows_b, "write");
  const auto operation_b = dynamic_b.value("operations")
                               .toArray()
                               .at(0)
                               .toObject()
                               .value("operationId")
                               .toString();
  const QJsonArray operation_b_only{QJsonValue(operation_b)};
  OMARCHY_CHECK(
      control.revoke(pending_id, builtin_b.value("rowId").toString())
              .isEmpty() &&
          control.revoke(review_b_id, builtin_a.value("rowId").toString())
              .isEmpty() &&
          control
              .apply(review_b_id, choices(builtin_a, dynamic_a,
                                          QJsonArray{QJsonValue(chosen_id)}))
              .isEmpty());

  const auto badChoices = [&](const QJsonArray &selected) {
    return choices(builtin_a, dynamic_a, selected);
  };
  OMARCHY_CHECK(
      control.apply(pending_id, badChoices(QJsonArray{"storage.private"}))
              .isEmpty() &&
          control.apply(pending_id, badChoices(QJsonArray{"foreign"}))
              .isEmpty() &&
          control.apply(pending_id, badChoices(chosen_duplicate)).isEmpty() &&
          control.apply(pending_id, badChoices(operation_b_only)).isEmpty() &&
          control
              .apply(
                  pending_id,
                  QString::fromUtf8(
                      QJsonDocument(
                          QJsonObject{
                              {"choices",
                               QJsonArray{
                                   choice(builtin_a, true),
                                   choice(dynamic_a, true, chosen_only),
                                   choice(dynamic_b, true, operation_b_only)}}})
                          .toJson(QJsonDocument::Compact)))
              .isEmpty());

  const auto cli_review =
      control.beginInteractiveCliReview(QString::fromUtf8(plugin_b));
  OMARCHY_CHECK(!cli_review.isEmpty());
  runPermission(jobs, *manager);
  const auto cli_rows = rows(poll(control, cli_review));
  const auto cli_builtin = rowNamed(cli_rows, "storage.private");
  const auto cli_dynamic = rowNamed(cli_rows, "write");
  QJsonArray cli_operations;
  for (const auto &value : cli_dynamic.value("operations").toArray())
    cli_operations.push_back(value.toObject().value("operationId"));
  const auto cli_choices = choices(cli_builtin, cli_dynamic, cli_operations);
  QJsonArray all_a_operations;
  for (const auto &value : operation_rows)
    all_a_operations.push_back(value.toObject().value("operationId"));
  OMARCHY_CHECK(control.apply(cli_review, cli_choices).isEmpty() &&
              control
                  .applyInteractiveCli(pending_id, choices(builtin_a, dynamic_a,
                                                           all_a_operations))
                  .isEmpty());

  const auto apply_id =
      control.apply(pending_id, choices(builtin_a, dynamic_a, chosen_only, true));
  OMARCHY_CHECK(
      !apply_id.isEmpty() &&
          control.apply(pending_id, choices(builtin_a, dynamic_a, chosen_only))
              .isEmpty() &&
          control.revoke(pending_id, builtin_a.value("rowId").toString())
              .isEmpty());
  runPermission(jobs, *manager);
  const auto applied = poll(control, apply_id);
  requireKeys(applied, {"operationId", "kind", "state", "result"},
              "successful apply");
  requireKeys(applied.value("result").toObject(), {"applied"}, "apply result");
  OMARCHY_CHECK(applied.value("state") == "succeeded" &&
              applied.value("result").toObject().value("applied") == true);
  requireNoAuthorityMetadata(applied);

  OMARCHY_CHECK(!jobs.jobs.empty() &&
              jobs.kinds.front() ==
                  bridge::PluginManagerTestAccess::TestJobKind::preparation);
  jobs.runOne();
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  OMARCHY_CHECK(await([&] {
            bridge::PluginManagerTestAccess::drainRuntime(*manager);
            const auto observations =
                bridge::PluginManagerTestAccess::runtimeSlots(*manager);
            return std::ranges::any_of(observations, [](const auto &slot) {
              return slot.plugin == plugin_a && slot.running;
            });
          }));

  const auto effective_id = control.beginList(QString::fromUtf8(plugin_a));
  OMARCHY_CHECK(!effective_id.isEmpty());
  runPermission(jobs, *manager);
  const auto effective = poll(control, effective_id);
  const auto effective_dynamic = rowNamed(rows(effective), "write");
  requireKeys(effective_dynamic,
              {"rowId", "name", "required", "available", "state",
               "scope", "operations", "title", "category", "trustTier"},
              "dynamic effective row");
  OMARCHY_CHECK(effective_dynamic.value("operations").toArray() ==
              QJsonArray{"storage.private"});
  requireNoAuthorityMetadata(effective);

  const auto requested_id = control.beginReview(QString::fromUtf8(plugin_a));
  OMARCHY_CHECK(!requested_id.isEmpty());
  runPermission(jobs, *manager);
  const auto requested = poll(control, requested_id);
  const auto requested_dynamic = rowNamed(rows(requested), "write");
  OMARCHY_CHECK(requested_dynamic.value("operations").toArray().size() == 2);
  requireNoAuthorityMetadata(requested);
  QString requested_chosen_id;
  for (const auto &value : requested_dynamic.value("operations").toArray()) {
    const auto operation = value.toObject();
    if (operation.value("name") == "storage.private")
      requested_chosen_id = operation.value("operationId").toString();
  }
  OMARCHY_CHECK(!requested_chosen_id.isEmpty());

  const auto revoke_id = control.revoke(
      effective_id,
      rowNamed(rows(effective), "storage.private").value("rowId").toString());
  OMARCHY_CHECK(!revoke_id.isEmpty());
  runPermission(jobs, *manager);
  OMARCHY_CHECK(poll(control, revoke_id).value("state") == "succeeded" &&
              control
                  .apply(requested_id,
                         choices(rowNamed(rows(requested), "storage.private"),
                                 requested_dynamic,
                                 QJsonArray{QJsonValue(requested_chosen_id)}))
                  .isEmpty());
}

void hostTrustContract() {
  constexpr std::string_view plugin = "host.permission-contract";
  RuntimeFixture fixture;
  fixture.seed(plugin, false);
  Jobs jobs;
  auto manager = bridge::PluginManagerTestAccess::create();
  bridge::PluginManagerTestAccess::installRuntime(*manager, fixture.bootstrap());
  jobs.install(*manager);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager));
  jobs.runOne();
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  auto &control = *manager->permissions();
  const auto before_slots = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  OMARCHY_CHECK(before_slots.size() == 1 && before_slots.front().permission_disabled);
  const auto before = bridge::PluginManagerTestAccess::permissionView(
      *manager, plugin, before_slots.front().epoch);
  OMARCHY_CHECK(before);
  for (const auto acknowledgement : {0, 1, 2}) {
    const bool acknowledge = acknowledgement == 2;
    const auto id = control.beginInteractiveCliReview(QString::fromUtf8(plugin));
    runPermission(jobs, *manager);
    const auto review_rows = rows(poll(control, id));
    const auto bounded = rowNamed(review_rows, "storage.private");
    const auto host = rowNamed(review_rows, "write");
    OMARCHY_CHECK(bounded.value("trustTier") == "sandboxed-plugin" &&
                host.value("trustTier") == "trusted-host-extension");
    const QJsonArray selected{host.value("operations").toArray().at(0)
        .toObject().value("operationId")};
    auto malformed = QJsonDocument::fromJson(choices(bounded, host, selected).toUtf8()).object();
    malformed.insert("hostExtensionAcknowledged", "true");
    OMARCHY_CHECK(control.applyInteractiveCli(id, QString::fromUtf8(
        QJsonDocument(malformed).toJson(QJsonDocument::Compact))).isEmpty());
    auto choice_document = QJsonDocument::fromJson(
        choices(bounded, host, selected, acknowledge).toUtf8()).object();
    if (acknowledgement == 1)
      choice_document.insert("hostExtensionAcknowledged", false);
    const auto document = QString::fromUtf8(
        QJsonDocument(choice_document).toJson(QJsonDocument::Compact));
    OMARCHY_CHECK(control.apply(id, document).isEmpty());
    const auto apply_id = control.applyInteractiveCli(id, document);
    OMARCHY_CHECK(!apply_id.isEmpty());
    runPermission(jobs, *manager);
    OMARCHY_CHECK(poll(control, apply_id).value("state") ==
        (acknowledge ? "succeeded" : "failed"));
    if (!acknowledge) {
      const auto after_slots = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
      const auto after = bridge::PluginManagerTestAccess::permissionView(
          *manager, plugin, after_slots.front().epoch);
      OMARCHY_CHECK(after && after->authority_slots.sequence == before->authority_slots.sequence &&
                  after_slots.front().permission_disabled && jobs.jobs.empty());
    }
  }
}

void unavailableProviderReviewContract() {
  constexpr std::string_view plugin = "unavailable.permission-contract";
  RuntimeFixture fixture;
  fixture.seed(plugin, false);
  Jobs jobs;
  auto manager = bridge::PluginManagerTestAccess::create();
  bridge::PluginManagerTestAccess::installRuntime(*manager,
                                                  fixture.bootstrap(false));
  jobs.install(*manager);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              jobs.jobs.size() == 1);
  jobs.runOne();
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  const auto initial = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  if (initial.size() != 1 || !initial.front().permission_disabled) {
    std::string detail =
        "permission-disabled authority did not retain a reviewable slot";
    for (const auto &entry : initial)
      detail += " [preparing=" + std::to_string(entry.preparing) +
                " retry=" + std::to_string(entry.retry_wait) +
                " state=" + std::to_string(entry.last_state) +
                " error=" + std::to_string(entry.last_error) + "]";
    throw std::runtime_error(detail);
  }

  auto &control = *manager->permissions();
  const auto review_id = control.beginReview(QString::fromUtf8(plugin));
  OMARCHY_CHECK(!review_id.isEmpty());
  runPermission(jobs, *manager);
  const auto review_rows = rows(poll(control, review_id));
  const auto builtin = rowNamed(review_rows, "storage.private");
  const auto dynamic = rowNamed(review_rows, "write");
  OMARCHY_CHECK(!builtin.isEmpty() && !dynamic.isEmpty() &&
              dynamic.value("state") == "granted" &&
              dynamic.value("available") == false);
  QJsonArray dynamic_operations;
  for (const auto &value : dynamic.value("operations").toArray())
    dynamic_operations.push_back(value.toObject().value("operationId"));
  OMARCHY_CHECK(
      control.apply(review_id, choices(builtin, dynamic, dynamic_operations))
          .isEmpty());
  const auto deny_choices = QString::fromUtf8(
      QJsonDocument(
          QJsonObject{{"choices", QJsonArray{choice(builtin, true),
                                             choice(dynamic, false)}}})
          .toJson(QJsonDocument::Compact));
  const auto apply_id = control.apply(review_id, deny_choices);
  OMARCHY_CHECK(!apply_id.isEmpty());
  runPermission(jobs, *manager);
  OMARCHY_CHECK(poll(control, apply_id).value("state") == "succeeded" &&
              jobs.jobs.size() == 1);
  const auto promoted = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  OMARCHY_CHECK(promoted.size() == 1 && promoted.front().preparing);
  const auto authority = bridge::PluginManagerTestAccess::permissionView(
      *manager, plugin, promoted.front().epoch);
  OMARCHY_CHECK(authority && authority->active &&
              authority->active->dynamic_grants.size() == 2 &&
              std::ranges::any_of(authority->active->dynamic_grants, [](const auto &grant) {
                return grant.request.definition.canonical_name.view() == "write" &&
                       grant.grant.state == permissions::GrantState::denied;
              }));
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    hostTrustContract();
    unavailableProviderReviewContract();
    if (std::getenv("OMARCHY_REQUIRE_PACKAGED_WORKER_TEST") != nullptr)
      publicPermissionContract();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "permission control contract test failed: %s\n",
                 error.what());
    return 1;
  }
  return 0;
}
