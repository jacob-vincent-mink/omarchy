#include "../../tests/support/child_process.hpp"
#include "omarchy/plugin_runtime/test_support/test_support.h"
#include "../../tests/support/blocking_gate.hpp"
#include "deterministic_jobs.hpp"
#include "../../tests/support/qt_wait.hpp"
#include "../../tests/support/gesture_clock.hpp"
#include "../../tests/support/runtime_bootstrap_fixture.hpp"
#include "../../tests/support/permission_fixture.hpp"
#include "capability_definition_loader.hpp"
#include "../../tests/support/test_assert.hpp"

#include "PluginManager.h"

#include "authority_store.hpp"
#include "desktop_notification_service.hpp"
#include "omarchy/plugin_runtime/Version.h"
#include "omarchy/plugin_runtime/runtime_paths.hpp"
#include "remote_surface.hpp"
#include "revision_verifier_adapter.hpp"
#include "runtime_bootstrap.hpp"
#include "runtime_roots.hpp"
#include "runtime_roots_test_access.hpp"
#include "omarchy/plugin_runtime/launcher/test_supervisor.h"

#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QUrl>
#include <QtQml/qqml.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

bool use_build_worker() {
  return !std::getenv("OMARCHY_REQUIRE_PACKAGED_WORKER_TEST");
}

std::string worker_path() {
  return use_build_worker() ? QML_WORKER_PATH
                           : std::string(omarchy::plugin_runtime::kPackagedWorkerPath);
}

class WorkerTestScope final : public omarchy::plugin_runtime::launcher::test_support::AttachedScope {
  bool terminate_scope_validated(std::string_view,
      omarchy::plugin_runtime::launcher::Deadline, std::string &) noexcept override { return true; }
};

namespace bridge = omarchy::plugin_runtime::bridge;
namespace permissions = omarchy::plugins::permissions;
namespace channel = omarchy::plugin_runtime::channel;
namespace host = omarchy::plugin_runtime::host_session;
namespace policy = omarchy::plugin_runtime::policy;
namespace definitions = omarchy::plugins::definitions;
namespace runtime = omarchy::plugin_runtime::runtime;
namespace surface = omarchy::plugin_runtime::surface;

class SettingsHost final : public QObject {
  Q_OBJECT

public:
  QVariantMap current;
  QString updated_plugin;
  QVariantMap updated_settings;

  Q_INVOKABLE QVariant readSecurePluginSettings(const QVariant &plugin) {
    return plugin.toString() == QStringLiteral("org.example.widget")
               ? QVariant(current)
               : QVariant{};
  }

  Q_INVOKABLE QVariant updateSecurePluginSettings(const QVariant &plugin,
                                                  const QVariant &settings) {
    updated_plugin = plugin.toString();
    updated_settings = settings.toMap();
    return true;
  }
};

using omarchy::plugin_runtime::test_support::require;
using omarchy::plugin_runtime::test_support::expect_child_exit;
using omarchy::plugin_runtime::test_support::awaitFor;
using omarchy::plugin_runtime::test_support::await;

void host_owned_settings_are_read_and_replaced_atomically() {
  auto manager = bridge::PluginManagerTestAccess::create();
  SettingsHost host;
  host.current = {{QStringLiteral("enabled"), true},
                  {QStringLiteral("mode"), QStringLiteral("full")}};
  OMARCHY_CHECK(manager->configureSettingsHost(&host) &&
              !manager->configureSettingsHost(&host));
  const auto current = bridge::PluginManagerTestAccess::currentSettings(
      *manager, "org.example.widget");
  OMARCHY_CHECK(current && *current == R"({"enabled":true,"mode":"full"})" &&
              !bridge::PluginManagerTestAccess::currentSettings(
                  *manager, "org.example.other"));
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::persistSettings(
              *manager, "org.example.widget",
              R"({"enabled":false,"mode":"compact"})") &&
              host.updated_plugin == QStringLiteral("org.example.widget") &&
              host.updated_settings.value(QStringLiteral("enabled")) == false &&
              host.updated_settings.value(QStringLiteral("mode")) ==
                  QStringLiteral("compact") &&
              !bridge::PluginManagerTestAccess::persistSettings(
                  *manager, "org.example.widget", "[]"));
}

void qml_hosts_return_startup_snapshots_as_maps() {
  QQmlEngine engine;
  QQmlComponent component(&engine);
  component.setData(R"QML(
import QtQuick
QtObject {
  function readSecurePluginSettings(pluginId) {
    return pluginId === "org.example.widget" ? { enabled: true } : undefined
  }
  function readSecurePluginPresentation() {
    return { foreground: "#123456", statusSlot: 23 }
  }
}
)QML",
                    QUrl());
  OMARCHY_CHECK(component.isReady());
  std::unique_ptr<QObject> host(component.create());
  OMARCHY_CHECK(host != nullptr);
  auto manager = bridge::PluginManagerTestAccess::create();
  OMARCHY_CHECK(manager->configureSettingsHost(host.get()) &&
              manager->configurePresentationHost(host.get()));
  const auto settings = bridge::PluginManagerTestAccess::currentSettings(
      *manager, "org.example.widget");
  const auto presentation =
      bridge::PluginManagerTestAccess::currentPresentation(*manager);
  OMARCHY_CHECK(settings && *settings == R"({"enabled":true})" && presentation &&
              *presentation ==
                  R"({"foreground":"#123456","statusSlot":23})");
}

std::size_t openDescriptorCount() {
  return omarchy::plugin_runtime::test_support::open_descriptor_count();
}

void process_singleton_factory_is_exact_and_recoverable() {
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::processClaimAvailable());
  QQmlEngine first_engine;
  QQmlEngine second_engine;
  OMARCHY_CHECK(!bridge::PluginManager::create(nullptr, nullptr) &&
              !bridge::PluginManager::create(&first_engine, nullptr) &&
              !bridge::PluginManager::create(nullptr, &first_engine) &&
              !bridge::PluginManager::create(&first_engine, &second_engine) &&
              bridge::PluginManagerTestAccess::processClaimAvailable());

  std::atomic<bridge::PluginManager *> wrong_thread_result = nullptr;
  std::thread wrong_thread([&] {
    wrong_thread_result.store(
        bridge::PluginManager::create(&first_engine, &first_engine),
        std::memory_order_release);
  });
  wrong_thread.join();
  OMARCHY_CHECK(!wrong_thread_result.load(std::memory_order_acquire) &&
              bridge::PluginManagerTestAccess::processClaimAvailable());

  bridge::PluginManagerTestAccess::failNextConstruction();
  OMARCHY_CHECK(!bridge::PluginManager::create(&first_engine, &first_engine) &&
              bridge::PluginManagerTestAccess::processClaimAvailable());

  auto *first = bridge::PluginManager::create(&first_engine, &first_engine);
  OMARCHY_CHECK(first && first->parent() == &first_engine &&
              !bridge::PluginManagerTestAccess::processClaimAvailable() &&
              !bridge::PluginManager::create(&first_engine, &first_engine) &&
              !bridge::PluginManager::create(&second_engine, &second_engine));
  delete first;
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::processClaimAvailable());

  auto *replacement =
      bridge::PluginManager::create(&second_engine, &second_engine);
  OMARCHY_CHECK(replacement && replacement->parent() == &second_engine);
  delete replacement;
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::processClaimAvailable());
}

void concurrent_engines_have_one_process_winner() {
  for (int iteration = 0; iteration < 32; ++iteration) {
    std::barrier enter_factory(2);
    std::barrier hold_winner(2);
    std::atomic<int> successes = 0;
    auto contender = [&] {
      QQmlEngine engine;
      enter_factory.arrive_and_wait();
      auto *manager = bridge::PluginManager::create(&engine, &engine);
      if (manager)
        successes.fetch_add(1, std::memory_order_relaxed);
      hold_winner.arrive_and_wait();
      delete manager;
    };
    std::thread first(contender);
    std::thread second(contender);
    first.join();
    second.join();
    OMARCHY_CHECK(successes.load(std::memory_order_relaxed) == 1 &&
                bridge::PluginManagerTestAccess::processClaimAvailable());
  }
}

template <typename Predicate>
bool awaitSlots(bridge::PluginManager &manager, Predicate predicate) {
  return await([&] {
    bridge::PluginManagerTestAccess::drainRuntime(manager);
    return predicate(bridge::PluginManagerTestAccess::runtimeSlots(manager));
  });
}

bool awaitSingleRunning(bridge::PluginManager &manager) {
  return awaitSlots(manager, [](const auto &observations) {
    return observations.size() == 1 && observations.front().running;
  });
}

permissions::ActivationBinding binding() {
  return {
      .plugin = permissions::PluginId("org.example.singleton"),
      .revision = permissions::Digest(std::string(64, 'a')),
      .policy_fingerprint = permissions::Digest(std::string(64, 'b')),
      .generation = 4,
  };
}

std::string readyActivationRecord(std::string_view plugin,
                                  std::string_view revision_directory,
                                  std::string_view revision_sha256) {
  return "format=omarchy-plugin-activation-v2\nplugin=" + std::string(plugin) +
         "\nrevision-directory=" + std::string(revision_directory) +
         "\nrevision-sha256=" + std::string(revision_sha256) +
         "\nstate-directory=" + std::string(plugin) + "\n";
}

std::string activationRecord(std::string_view plugin, char digest = 'a') {
  return readyActivationRecord(plugin, "revision", std::string(64, digest));
}

class RuntimeFixture final : public omarchy::plugin_runtime::test_support::RuntimeBootstrapTree {
public:
  RuntimeFixture() {
    for (const auto &definition : definitions::packaged_definitions()) {
      const auto path = package() / (std::string(definition.canonical_name.view()) + ".capability");
      std::ofstream(path) << definitions::canonical_definition_document(definition, 1);
      OMARCHY_CHECK(::chmod(path.c_str(), 0644) == 0);
    }
  }

  void put(std::string_view plugin, char digest = 'a') {
    write(plugin, activationRecord(plugin, digest), O_CREAT | O_EXCL);
  }

  void overwrite(std::string_view plugin, char digest) {
    write(plugin, activationRecord(plugin, digest), O_TRUNC);
  }

  void putInvalid() { write("!", "invalid\n", O_CREAT | O_EXCL); }

  permissions::ActivationBinding seedRuntime(
      std::string_view plugin,
      std::string_view qml = "import QtQuick\nItem {}\n",
      std::string_view permission_json = "{\"required\": [], \"optional\": []}",
      bool declares_surface = true,
      std::optional<std::string_view> denied_capability = std::nullopt) {
    const auto binding =
        stageRuntime(plugin, 1, qml, permission_json, declares_surface);
    promoteRuntime(binding, 0, denied_capability);
    activateStaged(binding);
    return binding;
  }

  permissions::ActivationBinding
  seedCheckedFixture(std::string_view plugin,
                     const std::filesystem::path &source) {
    const auto binding = stageCheckedFixture(plugin, 1, source);
    promoteRuntime(binding, 0);
    activateStaged(binding);
    return binding;
  }

  permissions::ActivationBinding
  stageCheckedFixture(std::string_view plugin, std::uint64_t generation,
                      const std::filesystem::path &source) {
    OMARCHY_CHECK(std::filesystem::is_directory(source));
    const auto revision = revisions() / revisionDirectory(plugin, generation);
    create(revision, 0755);
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(source)) {
      const auto destination = revision / entry.path().lexically_relative(source);
      if (entry.is_directory()) {
        create(destination, 0755);
      } else {
        OMARCHY_CHECK(entry.is_regular_file() &&
                    std::filesystem::copy_file(entry.path(), destination));
      }
    }
    return freezeAndVerify(plugin, generation, revision);
  }

  permissions::ActivationBinding stageRuntime(
      std::string_view plugin, std::uint64_t generation, std::string_view qml,
      std::string_view permission_json = "{\"required\": [], \"optional\": []}",
      bool declares_surface = true) {
    const auto revision_name = revisionDirectory(plugin, generation);
    const auto revision = revisions() / revision_name;
    create(revision / "ui", 0755);
    {
      std::ofstream manifest_file(revision / "manifest.json");
      manifest_file << "{\n  \"schemaVersion\": 2,\n  \"id\": \"" << plugin
                    << "\",\n  \"name\": \"Manager fixture\",\n"
                       "  \"version\": \"1.0.0\",\n"
                       "  \"runtime\": {\"apiVersion\": 1, \"qml\": "
                       "\"ui/Main.qml\"},\n  \"surfaces\": ";
      if (declares_surface) {
        manifest_file
            << "{\"bar\": {\"role\": \"bar-embedded\", "
               "\"defaultSection\": \"right\", \"maximumWidth\": 320, "
               "\"maximumHeight\": 64, \"maximumFramesPerSecond\": 60}}";
      } else {
        manifest_file << "{}";
      }
      manifest_file << ",\n  \"permissions\": " << permission_json << "\n}\n";
    }
    std::ofstream(revision / "ui/Main.qml") << qml;
    return freezeAndVerify(plugin, generation, revision);
  }

  std::filesystem::path archive(
      std::string_view plugin, std::string_view suffix,
      std::string_view permission_json =
          "{\"required\": [{\"capability\": \"storage.private\", \"definitionGeneration\":1,\"definitionDigest\":\"8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66\",\"operations\":[\"read\",\"write\",\"remove\"],\"itemBytes\":1024, \"quotaBytes\": 1024, \"reason\": \"state\"}], \"optional\": []}") {
    const auto source = root_ / ("archive-" + std::string(suffix));
    create(source / "ui", 0755);
    std::ofstream manifest(source / "manifest.json");
    manifest << "{\"schemaVersion\":2,\"id\":\"" << plugin
             << "\",\"name\":\"Install fixture\",\"version\":\"1.0.0\","
                "\"runtime\":{\"apiVersion\":1,\"qml\":\"ui/Main.qml\"},"
                "\"surfaces\":{\"overlay\":{\"role\":\"overlay\"}},"
                "\"permissions\":"
             << permission_json << "}";
    manifest.close();
    std::ofstream(source / "ui/Main.qml")
        << "import QtQuick\nItem { property string fixture: \"" << suffix
        << "\" }\n";
    const auto output = root_ / ("plugin-" + std::string(suffix) + ".tar");
    expect_child_exit(0, [&] {
      ::execlp("tar", "tar", "--format=ustar", "-cf", output.c_str(), "-C",
               source.c_str(), "ui", "manifest.json", nullptr);
      ::_exit(127);
    });
    return output;
  }

  void promoteRuntime(
      const permissions::ActivationBinding &binding,
      std::uint64_t expected_sequence,
      std::optional<std::string_view> denied_capability = std::nullopt) {
    auto store = open_authority_store(binding.plugin.view());

    const auto revision = revisions() / revisionDirectory(binding.plugin.view(),
                                                          binding.generation);
    const int revision_fd = ::open(
        revision.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    OMARCHY_CHECK(revision_fd >= 0);
    host::DescriptorRevisionVerifier verifier(::getuid());
    auto verified = verifier.verify_open_revision(revision_fd);
    ::close(revision_fd);
    OMARCHY_CHECK(verified && verified->tree_sha256 == binding.revision.view());
    const auto registry = omarchy::plugin_runtime::test_support::packaged_registry();
    auto snapshot = omarchy::plugin_runtime::test_support::permission_snapshot(
        registry, verified->manifest, verified->tree_sha256, binding.generation);
    for (auto &grant : snapshot.dynamic_grants)
      if (denied_capability && grant.request.definition.canonical_name.view() == *denied_capability)
        grant.grant.state = permissions::GrantState::denied;
    OMARCHY_CHECK(
        store->publish_candidate(*verified, snapshot, expected_sequence,
                                 registry) ==
                host::AuthorityMutationResult::applied &&
            store->promote_candidate(snapshot.binding, expected_sequence + 1) ==
                host::AuthorityMutationResult::applied);
  }

  void selectReplacement(const permissions::ActivationBinding &binding) {
    erase(binding.plugin.view());
    activateStaged(binding);
  }

  void activateStaged(const permissions::ActivationBinding &binding) {
    write(binding.plugin.view(),
          readyActivationRecord(
              binding.plugin.view(),
              revisionDirectory(binding.plugin.view(), binding.generation),
              binding.revision.view()),
          O_CREAT | O_EXCL);
  }

  void erase(std::string_view name) {
    OMARCHY_CHECK(std::filesystem::remove(activations() / std::string(name)));
  }

  std::unique_ptr<channel::RuntimeBootstrap> bootstrap(
      std::optional<channel::RuntimeServices> services = std::nullopt) const {
    channel::RuntimeBootstrapError bootstrap_error{};
    auto result = open_bootstrap(bootstrap_error);
    OMARCHY_CHECK(result && bootstrap_error == channel::RuntimeBootstrapError::none);
    if (services)
      channel::RuntimeBootstrapTestAccess::set_services(*result,
                                                        std::move(*services));
    if (use_build_worker())
      channel::RuntimeBootstrapTestAccess::set_supervisor_factory(*result, [] {
        return omarchy::plugin_runtime::launcher::test_support::make_supervisor(
            "/usr/bin/bwrap", QML_WORKER_PATH,
            std::make_shared<WorkerTestScope>());
      });
    return result;
  }

private:
  permissions::ActivationBinding
  freezeAndVerify(std::string_view plugin, std::uint64_t generation,
                  const std::filesystem::path &revision) {
    if (generation == 1) {
      create(state() / std::string(plugin), 0700);
      create(authority() / std::string(plugin), 0700);
    }
    const auto verified = omarchy::plugin_runtime::test_support::freeze_revision(
        revision, plugin);
    return {
        .plugin = permissions::PluginId(plugin),
        .revision = permissions::Digest(verified.tree_sha256),
        .policy_fingerprint =
            permissions::Digest(verified.request_sha256),
        .generation = generation,
    };
  }

  static std::string revisionDirectory(std::string_view plugin,
                                       std::uint64_t generation) {
    return std::string(plugin) + "-g" + std::to_string(generation);
  }

  void write(std::string_view name, std::string_view bytes, int flags) {
    write_file(activations() / std::string(name), bytes,
               O_WRONLY | O_CLOEXEC | O_NOFOLLOW | flags);
  }

};

using bridge::test_support::DeterministicJobs;

std::unique_ptr<bridge::PluginManager>
createRuntimeManager(RuntimeFixture &fixture,
                      DeterministicJobs *scheduler = nullptr,
                      std::optional<channel::RuntimeServices> services = std::nullopt) {
  auto manager = bridge::PluginManagerTestAccess::create();
  bridge::PluginManagerTestAccess::installRuntime(
      *manager, fixture.bootstrap(std::move(services)));
  if (scheduler)
    scheduler->install(*manager);
  return manager;
}

class BlockingNotifications final {
public:
  static bool send(std::string_view, std::string_view category,
                   std::string_view, std::string_view,
                   void *context) noexcept {
    auto &self = *static_cast<BlockingNotifications *>(context);
    std::unique_lock lock(self.mutex_);
    auto &effect = self.effect(category);
    ++effect.calls;
    effect.entered = true;
    self.changed_.notify_all();
    self.changed_.wait(lock, [&] { return effect.released; });
    return true;
  }

  void hold(std::string_view category) {
    std::scoped_lock lock(mutex_);
    auto &effect = this->effect(category);
    effect.entered = false;
    effect.released = false;
  }

  void release(std::string_view category) {
    {
      std::scoped_lock lock(mutex_);
      effect(category).released = true;
    }
    changed_.notify_all();
  }

  bool awaitEntered(std::string_view category) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(2),
                             [&] { return effect(category).entered; });
  }

  std::size_t calls(std::string_view category) {
    std::scoped_lock lock(mutex_);
    return effect(category).calls;
  }

private:
  struct Effect final {
    std::string_view category;
    std::size_t calls = 0;
    bool entered = false;
    bool released = true;
  };

  Effect &effect(std::string_view category) {
    auto found = std::ranges::find(effects_, category, &Effect::category);
    if (found == effects_.end())
      std::terminate();
    return *found;
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  std::array<Effect, 3> effects_{
      {{.category = "status"}, {.category = "a"}, {.category = "b"}}};
};

class LifecycleNotificationTransport final {
public:
  bool send(const channel::DesktopNotification &notification) noexcept {
    try {
      std::unique_lock lock(mutex_);
      notifications_.push_back(notification);
      entered_ = true;
      changed_.notify_all();
      return changed_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return !held_; });
    } catch (...) {
      return false;
    }
  }

  void hold() {
    std::scoped_lock lock(mutex_);
    held_ = true;
    entered_ = false;
  }

  void release() noexcept {
    try {
      {
        std::scoped_lock lock(mutex_);
        held_ = false;
      }
      changed_.notify_all();
    } catch (...) {
    }
  }

  bool awaitEntered() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(2),
                             [&] { return entered_; });
  }

  std::size_t calls() const {
    std::scoped_lock lock(mutex_);
    return notifications_.size();
  }

  channel::DesktopNotification last() const {
    std::scoped_lock lock(mutex_);
    OMARCHY_CHECK(!notifications_.empty());
    return notifications_.back();
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<channel::DesktopNotification> notifications_;
  bool held_ = false;
  bool entered_ = false;
};

constexpr std::string_view permissionAwareQml = R"QML(import QtQuick
import QtQml
Item {
  width: 64
  height: 64
  readonly property bool notificationsGranted:
    runtime.hasPermission("notifications.send", "send")
  property var notificationCall
  function invokeNotification() {
    if (notificationsGranted)
      notificationCall = runtime.invoke("notifications.send", "send", {
        category: "status",
        title: "Permission generation",
        body: "The optional feature is enabled"
      })
  }
  Timer {
    interval: 1
    running: true
    repeat: false
    onTriggered: parent.invokeNotification()
  }
  Rectangle {
    anchors.fill: parent
    color: parent.notificationsGranted ? "#20c060" : "#d02020"
  }
}
)QML";

std::string permissionAwareQmlFor(std::string_view category) {
  auto qml = std::string(permissionAwareQml);
  const auto marker = qml.find("category: \"status\"");
  OMARCHY_CHECK(marker != std::string::npos);
  qml.replace(marker,
              std::string_view("category: \"").size() +
                  std::string_view("status").size(),
              "category: \"" + std::string(category));
  return qml;
}

const bridge::PluginManagerTestAccess::SlotObservation &
observed(const std::vector<bridge::PluginManagerTestAccess::SlotObservation>
             &observations,
         std::string_view plugin) {
  const auto found = std::ranges::find(
      observations, plugin,
      &bridge::PluginManagerTestAccess::SlotObservation::plugin);
  OMARCHY_CHECK(found != observations.end());
  return *found;
}

QString barSurfaceKey(bridge::PluginManager &manager, std::string_view plugin) {
  using Model = bridge::SurfaceProjectionModel;
  const auto expected = QString::fromUtf8(plugin.data(), plugin.size());
  auto *model = manager.barSurfaces();
  for (int row = 0; row < model->rowCount(); ++row) {
    const auto index = model->index(row, 0);
    if (model->data(index, Model::PluginIdRole).toString() == expected)
      return model->data(index, Model::SurfaceKeyRole).toString();
  }
  return {};
}

QJsonObject permissionOperation(bridge::PermissionControl &control,
                                const QString &operation_id) {
  const auto document =
      QJsonDocument::fromJson(control.poll(operation_id).toUtf8());
  OMARCHY_CHECK(document.isObject());
  return document.object();
}

QJsonArray permissionRows(bridge::PermissionControl &control,
                          const QString &operation_id) {
  const auto operation = permissionOperation(control, operation_id);
  OMARCHY_CHECK(operation.value("state") == "succeeded" &&
              operation.value("result").isObject());
  return operation.value("result").toObject().value("permissions").toArray();
}

QString permissionRow(const QJsonArray &rows, std::string_view name) {
  const auto expected = QString::fromUtf8(name.data(), name.size());
  for (const auto value : rows) {
    const auto row = value.toObject();
    if (row.value("name") == expected)
      return row.value("rowId").toString();
  }
  return {};
}

template <typename Grant>
QString permissionChoices(const QJsonArray &rows, Grant should_grant) {
  QJsonArray choices;
  for (const auto value : rows) {
    const auto row = value.toObject();
    const bool grant = should_grant(row);
    QJsonObject choice{
        {"rowId", row.value("rowId")},
        {"decision", grant ? "grant" : "deny"}};
    if (grant) {
      QJsonArray selected;
      for (const auto operation : row.value("operations").toArray())
        selected.push_back(
            operation.toObject().value("operationId").toString());
      choice.insert("operations", selected);
    }
    choices.push_back(choice);
  }
  return QString::fromUtf8(QJsonDocument(QJsonObject{{"choices", choices}})
                               .toJson(QJsonDocument::Compact));
}

QString grantEveryAvailablePermission(const QJsonArray &rows) {
  return permissionChoices(rows, [](const QJsonObject &row) {
    return row.value("available").toBool();
  });
}

QString decidePermissions(const QJsonArray &rows,
                          std::span<const std::string_view> denied) {
  return permissionChoices(rows, [&](const QJsonObject &row) {
    const auto name = row.value("name").toString().toStdString();
    return std::ranges::find(denied, std::string_view(name)) == denied.end();
  });
}
QImage paintedFrame(bridge::RemotePluginSurface &remote) {
  QImage image(64, 64, QImage::Format_RGBA8888_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  remote.paint(&painter);
  painter.end();
  return image;
}

std::set<std::string>
pluginScopePaths(const permissions::ActivationBinding &binding) {
  const std::string marker = "app-omarchy-plugin-worker-" +
                             std::string(binding.plugin.view()) + "-" +
                             std::string(binding.revision.view().substr(0, 12)) +
                             "-" + std::to_string(binding.generation) + "-m";
  std::set<std::string> scopes;
  std::error_code error;
  for (const auto &entry : std::filesystem::directory_iterator("/proc", error)) {
    if (error)
      break;
    const auto name = entry.path().filename().string();
    if (name.empty() || !std::ranges::all_of(name, [](unsigned char value) {
          return value >= '0' && value <= '9';
        }))
      continue;
    std::ifstream cgroup(entry.path() / "cgroup");
    for (std::string line; std::getline(cgroup, line);) {
      const auto path_start = line.rfind(':');
      if (path_start == std::string::npos)
        continue;
      const auto path = line.substr(path_start + 1);
      if (path.find(marker) != std::string::npos && path.ends_with(".scope"))
        scopes.insert(path);
    }
  }
  return scopes;
}

bool redSignature(const QImage &image) {
  const auto color = image.pixelColor(32, 32);
  return color.alpha() >= 250 && color.red() >= 160 &&
         color.red() >= color.green() + 90 && color.red() >= color.blue() + 80;
}

bool blueSignature(const QImage &image) {
  const auto color = image.pixelColor(32, 32);
  return color.alpha() >= 250 && color.blue() >= 170 &&
         color.blue() >= color.red() + 100 &&
         color.blue() >= color.green() + 90;
}

bool greenSignature(const QImage &image) {
  const auto center = image.pixelColor(32, 32);
  const auto border = image.pixelColor(2, 2);
  return center.alpha() >= 250 && center.green() >= 140 &&
         center.green() >= center.red() + 90 &&
         center.green() >= center.blue() + 100 && border.red() >= 190 &&
         border.green() >= 150 && border.blue() <= 100;
}

constexpr std::string_view animatedRedQml = R"QML(import QtQuick
Item {
  property real phase: 0
  NumberAnimation on phase { from: 0; to: 1; duration: 240; loops: Animation.Infinite }
  Rectangle { anchors.fill: parent; color: Qt.rgba(0.65 + parent.phase * 0.35, 0.05, 0.08, 1) }
}
)QML";

constexpr std::string_view animatedBlueQml = R"QML(import QtQuick
Item {
  property real phase: 0
  NumberAnimation on phase { from: 0; to: 1; duration: 300; loops: Animation.Infinite }
  Rectangle { anchors.fill: parent; color: Qt.rgba(0.04, 0.08 + parent.phase * 0.2, 0.7 + parent.phase * 0.25, 1) }
}
)QML";

constexpr std::string_view animatedGreenQml = R"QML(import QtQuick
Item {
  property real phase: 0
  SequentialAnimation on phase {
    loops: Animation.Infinite
    NumberAnimation { to: 1; duration: 180; easing.type: Easing.InOutQuad }
    NumberAnimation { to: 0; duration: 180; easing.type: Easing.InOutQuad }
  }
  Rectangle {
    anchors.fill: parent
    color: Qt.rgba(0.12, 0.55 + parent.phase * 0.4, 0.06, 1)
    border.width: 6
    border.color: "#ffdc39"
  }
}
)QML";

std::filesystem::path secureBarQmlPath() {
  return std::filesystem::path(__FILE__)
             .parent_path()
             .parent_path()
             .parent_path() /
         "shell/SecureBarSurface.qml";
}

std::unique_ptr<QObject> createSecureBar(QQmlComponent &component,
                                         QObject &service, QString surface_key,
                                         std::uint64_t generation,
                                         QQuickItem &parent) {
  QVariantMap properties{
      {QStringLiteral("surfaceService"),
       QVariant::fromValue(static_cast<QObject *>(&service))},
      {QStringLiteral("surfaceKey"), std::move(surface_key)},
      {QStringLiteral("generation"), QString::number(generation)},
      {QStringLiteral("maximumWidth"), 64},
      {QStringLiteral("maximumHeight"), 64},
  };
  std::unique_ptr<QObject> object(
      component.createWithInitialProperties(properties));
  OMARCHY_CHECK(object != nullptr);
  auto *item = qobject_cast<QQuickItem *>(object.get());
  OMARCHY_CHECK(item != nullptr);
  item->setParentItem(&parent);
  item->setWidth(64);
  item->setHeight(64);
  OMARCHY_CHECK(item->width() == 64 && item->height() == 64);
  return object;
}

void secure_bar_retries_only_on_readiness_events() {
  QQmlEngine engine;
  QQmlComponent component(
      &engine, QUrl::fromLocalFile(QString::fromStdString(secureBarQmlPath())));
  if (!component.isReady()) {
    std::string errors = "secure bar QML component did not load:";
    for (const auto &error : component.errors())
      errors += "\n" + error.toString().toStdString();
    throw std::runtime_error(errors);
  }
  QJSValue service = engine.evaluate(
      "({ attempts: 0, lastKey: '', lastSurface: null, "
      "attach: function(key, surface) { this.attempts += 1; "
      "this.lastKey = key; this.lastSurface = surface; return false; } })");
  OMARCHY_CHECK(!service.isError());
  QVariantMap properties{
      {QStringLiteral("surfaceService"), QVariant::fromValue(service)},
      {QStringLiteral("surfaceKey"), QStringLiteral("first-key")},
      {QStringLiteral("generation"), QStringLiteral("1")},
      {QStringLiteral("maximumWidth"), 0},
      {QStringLiteral("maximumHeight"), 0},
  };
  std::unique_ptr<QObject> object(
      component.createWithInitialProperties(properties));
  OMARCHY_CHECK(object != nullptr);
  auto *item = qobject_cast<QQuickItem *>(object.get());
  auto *remote = object->findChild<bridge::RemotePluginSurface *>();
  OMARCHY_CHECK(item && remote && item->width() == 0 && item->height() == 0 &&
              service.property("attempts").toInt() == 0);

  QQuickWindow window;
  window.resize(128, 64);
  window.show();
  item->setParentItem(window.contentItem());
  QCoreApplication::processEvents();
  OMARCHY_CHECK(remote->window() == &window && remote->width() == 0 &&
              remote->height() == 0 &&
              service.property("attempts").toInt() == 0);
  item->setWidth(64);
  QCoreApplication::processEvents();
  OMARCHY_CHECK(service.property("attempts").toInt() == 0);
  item->setHeight(64);
  QCoreApplication::processEvents();
  OMARCHY_CHECK(service.property("attempts").toInt() == 1 &&
              service.property("lastKey").toString() ==
                  QStringLiteral("first-key") &&
              service.property("lastSurface").toQObject() == remote);

  OMARCHY_CHECK(item->setProperty("surfaceKey", QStringLiteral("replacement-key")) &&
              service.property("attempts").toInt() == 2 &&
              service.property("lastKey").toString() ==
                  QStringLiteral("replacement-key"));
  const int bounded_attempts =
      2 + item->property("maximumAttachAttempts").toInt();
  OMARCHY_CHECK(await([&] {
            return service.property("attempts").toInt() == bounded_attempts;
          }));
  const auto settled_attempts = service.property("attempts").toInt();
  OMARCHY_CHECK(!awaitFor(std::chrono::milliseconds(100), [&] {
            return service.property("attempts").toInt() != settled_attempts;
          }));
}

void secure_bar_cannot_expand_the_host_bar() {
  QQmlEngine engine;
  QQmlComponent bar_component(
      &engine, QUrl::fromLocalFile(QString::fromStdString(secureBarQmlPath())));
  OMARCHY_CHECK(bar_component.isReady());

  QJSValue service = engine.evaluate(
      "({ attach: function(key, surface) { return false; } })");
  OMARCHY_CHECK(!service.isError());
  const QVariantMap properties{
      {QStringLiteral("surfaceService"), QVariant::fromValue(service)},
      {QStringLiteral("surfaceKey"), QStringLiteral("bounded-bar")},
      {QStringLiteral("generation"), QStringLiteral("1")},
      {QStringLiteral("maximumWidth"), 64},
      {QStringLiteral("maximumHeight"), 64},
  };
  std::unique_ptr<QObject> secure_object(
      bar_component.createWithInitialProperties(properties));
  auto *secure_item = qobject_cast<QQuickItem *>(secure_object.get());
  OMARCHY_CHECK(secure_item && secure_item->implicitWidth() == 0 &&
              secure_item->implicitHeight() == 0);
  secure_item->setProperty("bar", QVariant{});
  QCoreApplication::processEvents();
  OMARCHY_CHECK(secure_item->implicitWidth() == 0 &&
              secure_item->implicitHeight() == 0);
  secure_item->setProperty(
      "bar", QVariantMap{{QStringLiteral("vertical"), false}});
  QCoreApplication::processEvents();
  OMARCHY_CHECK(secure_item->implicitWidth() == 0 &&
              secure_item->implicitHeight() == 0);
  secure_item->setProperty(
      "bar", QVariantMap{{QStringLiteral("vertical"), false},
                          {QStringLiteral("barSize"), -1}});
  QCoreApplication::processEvents();
  OMARCHY_CHECK(secure_item->implicitWidth() == 0 &&
              secure_item->implicitHeight() == 0);

  QQmlComponent host_component(&engine);
  host_component.setData(R"QML(
import QtQuick
Item {
  width: 256
  height: 26
  Row {
    objectName: "rowModules"
    anchors.verticalCenter: parent.verticalCenter
    Item { objectName: "ordinaryRow"; implicitWidth: 40; implicitHeight: 26 }
  }
  Column {
    objectName: "columnModules"
    x: 128
    Item { objectName: "ordinaryColumn"; implicitWidth: 32; implicitHeight: 40 }
  }
}
)QML",
                         QUrl());
  OMARCHY_CHECK(host_component.isReady());
  std::unique_ptr<QObject> host_object(host_component.create());
  auto *row = host_object->findChild<QQuickItem *>("rowModules");
  auto *ordinary_row = host_object->findChild<QQuickItem *>("ordinaryRow");
  auto *column = host_object->findChild<QQuickItem *>("columnModules");
  auto *ordinary_column =
      host_object->findChild<QQuickItem *>("ordinaryColumn");
  OMARCHY_CHECK(row && ordinary_row && column && ordinary_column);
  secure_item->setParentItem(row);
  QCoreApplication::processEvents();
  OMARCHY_CHECK(row->implicitHeight() == 26 && row->y() == 0 &&
              ordinary_row->y() == 0);

  const QVariantMap horizontal_bar{{QStringLiteral("vertical"), false},
                                   {QStringLiteral("barSize"), 26},
                                   {QStringLiteral("statusSlot"), 21}};
  secure_item->setProperty("bar", horizontal_bar);
  QCoreApplication::processEvents();

  OMARCHY_CHECK(secure_item->implicitWidth() == 21 &&
              secure_item->implicitHeight() == 26 &&
              row->implicitHeight() == 26 && row->y() == 0 &&
              ordinary_row->y() == 0);

  const QVariantMap vertical_bar{{QStringLiteral("vertical"), true},
                                 {QStringLiteral("barSize"), 32},
                                 {QStringLiteral("statusSlot"), 21}};
  secure_item->setParentItem(column);
  secure_item->setProperty("bar", vertical_bar);
  QCoreApplication::processEvents();
  OMARCHY_CHECK(secure_item->implicitWidth() == 32 &&
              secure_item->implicitHeight() == 21 &&
              column->implicitWidth() == 32 && ordinary_column->x() == 0);
}

void singleton_boundary_is_inert_and_not_configurable() {
  auto manager_owner = bridge::PluginManagerTestAccess::create();
  auto &manager = *manager_owner;
  const auto version = omarchy::plugin_runtime::build_version();
  OMARCHY_CHECK(!manager.available() && manager.count() == 0 &&
              manager.barSurfaces()->rowCount() == 0 &&
              manager.panelSurfaces()->rowCount() == 0 &&
              manager.overlaySurfaces()->rowCount() == 0 &&
              manager.runtimeVersion() ==
                  QString::fromLatin1(version.data(), version.size()));

  const auto *meta = manager.metaObject();
  for (int index = meta->methodOffset(); index < meta->methodCount(); ++index) {
    const auto name = meta->method(index).name();
    OMARCHY_CHECK(name != "publishSurfaces" && name != "withdrawSurfaces" &&
                name != "publishIntent" && name != "bindBackend");
  }
  bridge::RemotePluginSurface remote;
  OMARCHY_CHECK(!manager.attach(QStringLiteral("missing"), &remote));
  QCoreApplication::processEvents();
  OMARCHY_CHECK(!manager.available());
}

void private_projection_seam_preserves_fail_closed_boundary() {
  using Service = bridge::SurfaceProjectionModel;
  auto manager_owner = bridge::PluginManagerTestAccess::create();
  auto &manager = *manager_owner;
  int changes = 0;
  QObject::connect(&manager, &bridge::PluginManager::surfacesChanged,
                   [&changes] { ++changes; });
  std::vector<Service::SurfaceDeclaration> declarations;
  declarations.push_back({.surface_name = "panel",
                          .role = Service::Role::Panel,
                          .initially_visible = false,
                          .maximum_width = 640,
                          .maximum_height = 480,
                          .dynamic_input_regions = true});
  const auto exact = binding();
  OMARCHY_CHECK(bridge::SurfaceProjectionModelTestAccess::publish(
              manager, exact, std::move(declarations), 1) &&
              manager.count() == 1 && changes == 1 && !manager.available());

  const auto key =
      manager.panelSurfaces()
          ->data(manager.panelSurfaces()->index(0, 0), Service::SurfaceKeyRole)
          .toString();
  bridge::RemotePluginSurface remote;
  OMARCHY_CHECK(!manager.attach(key, &remote));

  bool off_thread = true;
  std::thread worker([&] {
    off_thread =
        bridge::SurfaceProjectionModelTestAccess::withdraw(manager, exact);
  });
  worker.join();
  OMARCHY_CHECK(
      !off_thread && manager.count() == 1 &&
          bridge::SurfaceProjectionModelTestAccess::withdraw(manager, exact) &&
          manager.count() == 0 && changes == 2);
}

void manager_policy_is_fixed_and_fail_closed() {
  RuntimeFixture fixture;
  auto manager = createRuntimeManager(fixture);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::clockIsNondecreasing(*manager));
}

void last_good_reconciliation_and_stale_callback_are_fail_closed() {
  RuntimeFixture fixture;
  fixture.put("a.plugin");
  auto manager_owner = createRuntimeManager(fixture);
  auto &manager = *manager_owner;
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(manager) &&
              manager.available() && manager.count() == 0);
  OMARCHY_CHECK(awaitSlots(manager, [&](const auto &observations) {
            return observations.size() == 1 && observations[0].retry_wait;
          }));
  const auto first = bridge::PluginManagerTestAccess::runtimeSlots(manager);
  OMARCHY_CHECK(first.size() == 1 && observed(first, "a.plugin").retry_wait);
  const auto a_epoch = observed(first, "a.plugin").epoch;
  const auto a_attempts = observed(first, "a.plugin").retry_attempts;

  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(manager));
  const auto same = bridge::PluginManagerTestAccess::runtimeSlots(manager);
  OMARCHY_CHECK(observed(same, "a.plugin").epoch == a_epoch &&
              observed(same, "a.plugin").retry_attempts == a_attempts);

  fixture.putInvalid();
  OMARCHY_CHECK(!bridge::PluginManagerTestAccess::scanRuntime(manager));
  const auto failed = bridge::PluginManagerTestAccess::runtimeSlots(manager);
  OMARCHY_CHECK(observed(failed, "a.plugin").epoch == a_epoch && manager.available());
  fixture.erase("!");

  fixture.put("b.plugin");
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(manager));
  OMARCHY_CHECK(awaitSlots(manager, [&](const auto &observations) {
            return observations.size() == 2 &&
                   observed(observations, "b.plugin").retry_wait;
          }));
  const auto added = bridge::PluginManagerTestAccess::runtimeSlots(manager);
  OMARCHY_CHECK(added.size() == 2 && observed(added, "a.plugin").epoch == a_epoch);
  const auto b_epoch = observed(added, "b.plugin").epoch;
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::queueStaleRunningCallback(
              manager, "b.plugin"));
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::retryRuntime(manager, "b.plugin"));
  OMARCHY_CHECK(awaitSlots(manager, [&](const auto &observations) {
            return observed(observations, "b.plugin").retry_wait;
          }));
  const auto retried = bridge::PluginManagerTestAccess::runtimeSlots(manager);
  OMARCHY_CHECK(observed(retried, "a.plugin").epoch == a_epoch &&
              observed(retried, "b.plugin").epoch != b_epoch);

  const auto current_b = observed(retried, "b.plugin").epoch;
  fixture.overwrite("a.plugin", 'b');
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(manager));
  OMARCHY_CHECK(awaitSlots(manager, [&](const auto &observations) {
            return observed(observations, "a.plugin").retry_wait;
          }));
  const auto replaced = bridge::PluginManagerTestAccess::runtimeSlots(manager);
  OMARCHY_CHECK(observed(replaced, "a.plugin").epoch != a_epoch &&
              observed(replaced, "b.plugin").epoch == current_b);

  fixture.erase("a.plugin");
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(manager));
  const auto removed = bridge::PluginManagerTestAccess::runtimeSlots(manager);
  OMARCHY_CHECK(removed.size() == 1 && removed[0].plugin == "b.plugin" &&
              removed[0].epoch == current_b);
  QCoreApplication::processEvents();
  const auto late = bridge::PluginManagerTestAccess::runtimeSlots(manager);
  OMARCHY_CHECK(late[0].epoch == current_b && late[0].retry_wait &&
              manager.count() == 0);
  bridge::RemotePluginSurface remote;
  OMARCHY_CHECK(!manager.attach(QStringLiteral("anything"), &remote));
}

void bounded_mailbox_coalesces_and_recovers_without_backoff() {
  {
    RuntimeFixture fixture;
    DeterministicJobs scheduler;
    auto manager = createRuntimeManager(fixture, &scheduler);
    for (int index = 0; index < 1000; ++index)
      bridge::PluginManagerTestAccess::requestAsyncScan(*manager);
    OMARCHY_CHECK(scheduler.jobs.size() == 1 &&
                scheduler.kinds.front() ==
                    bridge::PluginManagerTestAccess::TestJobKind::scan &&
                bridge::PluginManagerTestAccess::scanInFlight(*manager));
    scheduler.runOne();
    for (int index = 0; index < 1000; ++index)
      bridge::PluginManagerTestAccess::requestAsyncScan(*manager);
    OMARCHY_CHECK(scheduler.jobs.empty() &&
                bridge::PluginManagerTestAccess::scanInFlight(*manager));
    bridge::PluginManagerTestAccess::drainRuntime(*manager);
    OMARCHY_CHECK(!bridge::PluginManagerTestAccess::scanInFlight(*manager) &&
                manager->available());
  }

  {
    RuntimeFixture fixture;
    fixture.put("a.plugin");
    fixture.put("b.plugin");
    fixture.put("c.plugin");
    DeterministicJobs scheduler;
    auto manager = createRuntimeManager(fixture, &scheduler);
    OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
                scheduler.jobs.size() == 2 &&
                std::ranges::all_of(scheduler.kinds,
                                    [](auto kind) {
                                      return kind ==
                                             bridge::PluginManagerTestAccess::
                                                 TestJobKind::preparation;
                                    }) &&
                bridge::PluginManagerTestAccess::preparationCount(*manager) ==
                    2);
    scheduler.runOne();
    scheduler.runOne();
    OMARCHY_CHECK(bridge::PluginManagerTestAccess::preparationCount(*manager) == 2 &&
                bridge::PluginManagerTestAccess::occupiedPreparationLanes(
                    *manager) == 2);
    bridge::PluginManagerTestAccess::drainRuntime(*manager);
    OMARCHY_CHECK(scheduler.jobs.size() == 1 && scheduler.peak == 2 &&
                bridge::PluginManagerTestAccess::preparationCount(*manager) ==
                    1);
  }

  for (const bool inject_throw : {false, true}) {
    RuntimeFixture fixture;
    fixture.put("a.plugin");
    DeterministicJobs scheduler;
    scheduler.refuses = !inject_throw;
    scheduler.throws = inject_throw;
    auto manager = createRuntimeManager(fixture, &scheduler);
    OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager));
    const auto refused =
        bridge::PluginManagerTestAccess::runtimeSlots(*manager);
    OMARCHY_CHECK(refused.size() == 1 && refused[0].opening &&
                refused[0].retry_attempts == 0 &&
                bridge::PluginManagerTestAccess::preparationCount(*manager) ==
                    0);
    scheduler.refuses = false;
    scheduler.throws = false;
    bridge::PluginManagerTestAccess::drainRuntime(*manager);
    OMARCHY_CHECK(scheduler.jobs.size() == 1 &&
                bridge::PluginManagerTestAccess::preparationCount(*manager) ==
                    1);
  }
}

void mailbox_results_are_safe_across_replacement_and_destruction() {
  RuntimeFixture fixture;
  fixture.put("a.plugin");
  DeterministicJobs scheduler;
  auto manager = createRuntimeManager(fixture, &scheduler);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 1);
  const auto old_epoch =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager).front().epoch;
  fixture.overwrite("a.plugin", 'b');
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 2 &&
              bridge::PluginManagerTestAccess::runtimeSlots(*manager)
                      .front()
                      .epoch != old_epoch);
  scheduler.runInlineAndDrain(*manager);
  OMARCHY_CHECK(scheduler.jobs.size() == 1 &&
              bridge::PluginManagerTestAccess::preparationCount(*manager) == 1);
  fixture.erase("a.plugin");
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              bridge::PluginManagerTestAccess::runtimeSlots(*manager).empty());
  scheduler.runInlineAndDrain(*manager);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::runtimeSlots(*manager).empty() &&
              bridge::PluginManagerTestAccess::preparationCount(*manager) == 0);

  bridge::PluginManagerTestAccess::requestAsyncScan(*manager);
  OMARCHY_CHECK(scheduler.jobs.size() == 1);
  const auto completed_gate =
      bridge::PluginManagerTestAccess::deliveryGate(*manager);
  scheduler.runOne();
  manager.reset();
  OMARCHY_CHECK(completed_gate.expired());

  DeterministicJobs blocked_scheduler;
  auto blocked_manager = createRuntimeManager(fixture, &blocked_scheduler);
  omarchy::plugin_runtime::test_support::BlockingGate block;
  bool correct_kind = false;
  bridge::PluginManagerTestAccess::setJobEntryProbe(
      *blocked_manager, [&](auto kind) {
        block.arrive_and_wait([&] {
          correct_kind = kind == bridge::PluginManagerTestAccess::TestJobKind::scan;
        });
      });
  bridge::PluginManagerTestAccess::requestAsyncScan(*blocked_manager);
  const auto blocked_gate =
      bridge::PluginManagerTestAccess::deliveryGate(*blocked_manager);
  auto blocked_job = std::move(blocked_scheduler.jobs.front());
  blocked_scheduler.jobs.clear();
  blocked_scheduler.kinds.clear();
  std::thread worker([&, job = std::move(blocked_job)]() mutable {
    job();
    job = {};
  });
  block.wait_entered();
  OMARCHY_CHECK(correct_kind);
  blocked_manager.reset();
  OMARCHY_CHECK(!blocked_gate.expired());
  block.release();
  worker.join();
  OMARCHY_CHECK(blocked_gate.expired());
}

void lifecycle_mailbox_keeps_latest_exact_terminal_state() {
  RuntimeFixture fixture;
  fixture.put("a.plugin");
  auto manager = createRuntimeManager(fixture);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) && await([&] {
            bridge::PluginManagerTestAccess::drainRuntime(*manager);
            return bridge::PluginManagerTestAccess::runtimeSlots(*manager)
                .front()
                .retry_wait;
          }));
  const auto before = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  const auto epoch = before.front().epoch;
  const auto attempts = before.front().retry_attempts;
  OMARCHY_CHECK(
      bridge::PluginManagerTestAccess::deliverLifecycle(
          *manager, "a.plugin", epoch,
          static_cast<std::uint8_t>(host::SessionState::running),
          static_cast<std::uint8_t>(host::SessionError::none)) &&
          bridge::PluginManagerTestAccess::deliverLifecycle(
              *manager, "a.plugin", epoch,
              static_cast<std::uint8_t>(host::SessionState::failed),
              static_cast<std::uint8_t>(host::SessionError::channel_failed)));
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  const auto terminal =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager).front();
  OMARCHY_CHECK(terminal.retry_wait && terminal.retry_attempts == attempts + 1 &&
              manager->count() == 0);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::retryRuntime(*manager, "a.plugin"));
  OMARCHY_CHECK(!bridge::PluginManagerTestAccess::deliverLifecycle(
              *manager, "a.plugin", epoch,
              static_cast<std::uint8_t>(host::SessionState::running),
              static_cast<std::uint8_t>(host::SessionError::none)) &&
              manager->count() == 0);
}

class IntentFixture {
  using Clock = omarchy::plugin_runtime::test_support::GestureClock<100>;
  std::shared_ptr<Clock> clock = std::make_shared<Clock>();
  runtime::GestureEligibilityLatch eligibility{clock};
  host::GestureIntentAuthority authority;
  surface::SurfaceKey key;

public:
  explicit IntentFixture(const permissions::ActivationBinding &binding)
      : authority(binding, eligibility), key{.id = 1, .generation = binding.generation} {
    OMARCHY_CHECK(authority.declare_surface(key, "bar") ==
                    host::SurfaceDeclarationResult::declared &&
                authority.attach_surface(key));
  }
  host::AdmittedSurfaceIntent admit(std::uint64_t sequence,
                                    surface::SurfaceIntentAction action,
                                    bool arm = true) {
    if (arm)
      OMARCHY_CHECK(authority.arm(key, sequence));
    auto admission = authority.admit({.source = key,
                                      .target = key,
                                      .input_sequence = sequence,
                                      .action = action,
                                      .requested_output = {}});
    OMARCHY_CHECK(admission.intent.has_value());
    return std::move(*admission.intent);
  }
};

host::AdmittedSurfaceIntent admittedIntent(
    const permissions::ActivationBinding &binding, std::uint64_t sequence,
    surface::SurfaceIntentAction action = surface::SurfaceIntentAction::toggle) {
  return IntentFixture(binding).admit(sequence, action);
}

void surface_intent_mailbox_is_bounded_thread_safe_and_inert_when_stale() {
  RuntimeFixture fixture;
  fixture.put("a.plugin");
  DeterministicJobs scheduler;
  auto manager = createRuntimeManager(fixture, &scheduler);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager));
  const auto observation =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager).front();
  auto callback = bridge::PluginManagerTestAccess::surfaceIntentCallback(
      *manager, observation.plugin, observation.epoch);
  OMARCHY_CHECK(callback.has_value());

  const auto exact_binding = permissions::ActivationBinding{
      .plugin = permissions::PluginId(observation.plugin),
      .revision = permissions::Digest(std::string(64, '1')),
      .policy_fingerprint = permissions::Digest(std::string(64, '2')),
      .generation = 1};
  std::atomic_bool accepted_on_worker = true;
  std::thread worker([&] {
    for (std::uint64_t sequence = 1; sequence <= 64; ++sequence)
      if (!callback->deliver(admittedIntent(exact_binding, sequence)))
        accepted_on_worker = false;
  });
  worker.join();
  OMARCHY_CHECK(accepted_on_worker && callback->pending() == 64 &&
              !callback->deliver(admittedIntent(exact_binding, 65)));

  int toggles = 0;
  QObject::connect(manager.get(), &bridge::PluginManager::toggleRequested,
                   [&] { ++toggles; });
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  OMARCHY_CHECK(callback->pending() == 0 && toggles == 0);

  const auto wrong_binding = permissions::ActivationBinding{
      .plugin = permissions::PluginId("other.plugin"),
      .revision = permissions::Digest(std::string(64, '1')),
      .policy_fingerprint = permissions::Digest(std::string(64, '2')),
      .generation = 1};
  OMARCHY_CHECK(!callback->deliver(admittedIntent(wrong_binding, 1)));

  fixture.overwrite(observation.plugin, 'b');
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              bridge::PluginManagerTestAccess::runtimeSlots(*manager)
                      .front()
                      .epoch != observation.epoch &&
              callback->deliver(admittedIntent(exact_binding, 66)));
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  OMARCHY_CHECK(callback->pending() == 1 && toggles == 0);

  manager.reset();
  OMARCHY_CHECK(callback->deliver(admittedIntent(exact_binding, 67)) &&
              callback->pending() == 2);
}

void surface_intent_mailbox_delivers_fifo_for_running_published_slot() {
  constexpr std::string_view plugin = "org.example.intent-fifo";
  RuntimeFixture fixture;
  const auto binding = fixture.seedRuntime(plugin);
  DeterministicJobs scheduler;
  auto manager = createRuntimeManager(fixture, &scheduler);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager));
  const auto slot =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager).front();
  std::vector<bridge::SurfaceProjectionModel::SurfaceDeclaration>
      declarations{{.surface_name = "bar",
                    .role = bridge::SurfaceProjectionModel::Role::Bar,
                    .maximum_width = 320,
                    .maximum_height = 64,
                    .default_bar_section =
                        bridge::SurfaceProjectionModel::BarSection::Right}};
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::stageRunningSurfaceIntentSlot(
              *manager, slot.plugin, slot.epoch, binding,
              std::move(declarations)) &&
              manager->count() == 1 &&
              bridge::PluginManagerTestAccess::runtimeSlots(*manager)
                  .front()
                  .running);

  IntentFixture intents(binding);
  auto open = intents.admit(1, surface::SurfaceIntentAction::open);
  auto first_toggle = intents.admit(2, surface::SurfaceIntentAction::toggle);
  auto second_toggle = intents.admit(3, surface::SurfaceIntentAction::toggle);
  auto dismiss = intents.admit(0, surface::SurfaceIntentAction::dismiss, false);
  auto callback = bridge::PluginManagerTestAccess::surfaceIntentCallback(
      *manager, slot.plugin, slot.epoch);
  OMARCHY_CHECK(callback.has_value());
  auto *bar_model = manager->barSurfaces();
  const auto surface_key =
      bar_model
          ->data(bar_model->index(0, 0),
                 bridge::SurfaceProjectionModel::SurfaceKeyRole)
          .toString();
  const auto generation = QString::number(binding.generation);
  QQmlEngine intent_engine;
  QQmlComponent intent_component(&intent_engine);
  intent_component.setData(R"(
    import QtQml
    QtObject {
      id: root
      required property var service
      required property string targetSurface
      required property string targetGeneration
      property bool opened: false
      property string history: ""
      property string lastInputSequence: ""
      function apply(action, source, target, generation, inputSequence) {
        if (target !== targetSurface || generation !== targetGeneration) return
        history += (history.length === 0 ? "" : ",") + action + "|" + source
          + "|" + target + "|" + generation + "|" + inputSequence
        lastInputSequence = inputSequence
        if (action === "open") opened = true
        else if (action === "toggle") opened = !opened
        else if (action === "dismiss") opened = false
      }
      property Connections serviceConnections: Connections {
        target: root.service
        function onOpenRequested(source, target, generation, inputSequence) {
          root.apply("open", source, target, generation, inputSequence)
        }
        function onToggleRequested(source, target, generation, inputSequence) {
          root.apply("toggle", source, target, generation, inputSequence)
        }
        function onDismissRequested(source, target, generation, inputSequence) {
          root.apply("dismiss", source, target, generation, inputSequence)
        }
      }
    }
  )", QUrl());
  std::unique_ptr<QObject> qml_state(
      intent_component.createWithInitialProperties(
          {{QStringLiteral("service"), QVariant::fromValue(manager.get())},
           {QStringLiteral("targetSurface"), surface_key},
           {QStringLiteral("targetGeneration"), generation}}));
  if (qml_state == nullptr)
    throw std::runtime_error(
        "end-to-end QML intent state fixture did not load: " +
        intent_component.errorString().toStdString());

  auto wrong_binding = binding;
  wrong_binding.plugin = permissions::PluginId("org.example.wrong");
  auto stale_binding = binding;
  ++stale_binding.generation;
  OMARCHY_CHECK(!callback->deliver(admittedIntent(wrong_binding, 1)) &&
              callback->deliver(admittedIntent(stale_binding, 1)) &&
              callback->pending() == 1);
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  OMARCHY_CHECK(callback->pending() == 0 &&
              qml_state->property("history").toString().isEmpty() &&
              !qml_state->property("opened").toBool());
  bool queued = false;
  std::thread worker([&] {
    queued = callback->deliver(std::move(open)) &&
             callback->deliver(std::move(first_toggle)) &&
             callback->deliver(std::move(second_toggle)) &&
             callback->deliver(std::move(dismiss));
  });
  worker.join();
  OMARCHY_CHECK(queued && callback->pending() == 4 &&
              qml_state->property("history").toString().isEmpty());
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  const auto tuple_prefix = surface_key + u'|' + surface_key + u'|' +
                            generation + u'|';
  const auto expected_history =
      QStringLiteral("open|") + tuple_prefix + QStringLiteral("1,toggle|") +
      tuple_prefix + QStringLiteral("2,toggle|") + tuple_prefix +
      QStringLiteral("3,dismiss|") + tuple_prefix + QStringLiteral("0");
  OMARCHY_CHECK(qml_state->property("history").toString() == expected_history &&
              !qml_state->property("opened").toBool() &&
              qml_state->property("lastInputSequence").toString() ==
                  QStringLiteral("0") &&
              callback->pending() == 0);
}

void blocked_replacement_preserves_independent_plugin() {
  RuntimeFixture fixture;
  fixture.put("a.plugin");
  fixture.put("b.plugin");
  DeterministicJobs scheduler;
  auto manager = createRuntimeManager(fixture, &scheduler);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 2);
  const auto original = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  const auto old_a = observed(original, "a.plugin").epoch;
  const auto old_b = observed(original, "b.plugin").epoch;
  fixture.overwrite("a.plugin", 'b');
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 2);
  auto replaced = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  const auto replacement_a = observed(replaced, "a.plugin").epoch;
  OMARCHY_CHECK(replacement_a != old_a &&
              observed(replaced, "b.plugin").epoch == old_b);

  scheduler.runInlineAndDrain(*manager, 1);
  replaced = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  const auto accepted_b = observed(replaced, "b.plugin");
  const auto accepted_b_epoch = accepted_b.epoch;
  OMARCHY_CHECK(accepted_b.retry_wait && accepted_b.retry_attempts == 1 &&
              observed(replaced, "a.plugin").epoch == replacement_a &&
              scheduler.jobs.size() == 2);

  scheduler.runInlineAndDrain(*manager);
  replaced = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  OMARCHY_CHECK(observed(replaced, "a.plugin").epoch == replacement_a &&
              observed(replaced, "b.plugin").epoch == accepted_b_epoch &&
              observed(replaced, "b.plugin").retry_attempts == 1 &&
              scheduler.jobs.size() == 1);

  scheduler.runInlineAndDrain(*manager);
  replaced = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  OMARCHY_CHECK(observed(replaced, "a.plugin").retry_wait &&
              observed(replaced, "a.plugin").retry_attempts == 1 &&
              observed(replaced, "b.plugin").epoch == accepted_b_epoch &&
              observed(replaced, "b.plugin").retry_attempts == 1);
}

void runtime_jobs_enter_off_ui_and_commit_on_ui_drain() {
  RuntimeFixture fixture;
  fixture.put("a.plugin");
  auto manager = createRuntimeManager(fixture);
  const auto ui_thread = std::this_thread::get_id();
  std::mutex observed_mutex;
  std::thread::id scan_thread;
  std::thread::id preparation_thread;
  bridge::PluginManagerTestAccess::setJobEntryProbe(*manager, [&](auto kind) {
    std::scoped_lock lock(observed_mutex);
    if (kind == bridge::PluginManagerTestAccess::TestJobKind::scan)
      scan_thread = std::this_thread::get_id();
    else
      preparation_thread = std::this_thread::get_id();
  });
  bridge::PluginManagerTestAccess::requestAsyncScan(*manager);
  OMARCHY_CHECK(await([&] {
            bridge::PluginManagerTestAccess::drainRuntime(*manager);
            std::scoped_lock lock(observed_mutex);
            return scan_thread != std::thread::id{} &&
                   preparation_thread != std::thread::id{};
          }));
  OMARCHY_CHECK(await([&] {
            bridge::PluginManagerTestAccess::drainRuntime(*manager);
            return bridge::PluginManagerTestAccess::runtimeSlots(*manager)
                .front()
                .retry_wait;
          }));
  std::scoped_lock lock(observed_mutex);
  OMARCHY_CHECK(scan_thread != ui_thread && preparation_thread != ui_thread);
}

void manager_owns_permission_generation_replacement() {
  OMARCHY_CHECK(::access(
              worker_path().c_str(),
              X_OK) == 0);
  constexpr std::string_view plugin = "org.example.permissions";
  constexpr std::string_view permission_json =
      R"({"required":[{"capability":"storage.private","definitionGeneration":1,"definitionDigest":"8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66","operations":["read","write","remove"],"itemBytes":1024,"reason":"state","quotaBytes":4096}],"optional":[{"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"],"reason":"alerts","categories":["status"]}]})";
  const definitions::CapabilityReference notifications{
      .canonical_name = definitions::Name("notifications.send"), .definition_generation = 1,
      .definition_digest = definitions::Digest("522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872")};
  auto absent = notifications;
  absent.definition_generation = 2;
  const definitions::CapabilityReference absent_dynamic{
      .canonical_name = definitions::Name("harness.example"),
      .definition_generation = 1,
      .definition_digest = definitions::Digest(std::string(64, 'd'))};

  RuntimeFixture fixture;
  const auto first_binding = fixture.seedRuntime(
      plugin, permissionAwareQml, permission_json, true);
  DeterministicJobs scheduler;
  auto manager = bridge::PluginManagerTestAccess::create();
  auto notification_backend = std::make_shared<BlockingNotifications>();
  const auto release_notification = qScopeGuard(
      [&] { notification_backend->release("status"); });
  notification_backend->hold("status");
  channel::RuntimeServices services{.context = notification_backend,
                                    .notification_send = BlockingNotifications::send};
  bridge::PluginManagerTestAccess::installRuntime(
      *manager, fixture.bootstrap(std::move(services)));
  scheduler.install(*manager);
  const auto run_preparation = [&] {
    OMARCHY_CHECK(!scheduler.jobs.empty() &&
                scheduler.kinds.front() ==
                    bridge::PluginManagerTestAccess::TestJobKind::preparation);
    scheduler.runAndDrain(*manager);
    if (!awaitSingleRunning(*manager)) {
      const auto observations =
          bridge::PluginManagerTestAccess::runtimeSlots(*manager);
      OMARCHY_CHECK(!observations.empty());
      const auto &failed = observations.front();
      throw std::runtime_error(
          "permission replacement generation did not reach running: state=" +
          std::to_string(failed.last_state) +
          " error=" + std::to_string(failed.last_error));
    }
  };
  const auto current_slot = [&] {
    const auto observations =
        bridge::PluginManagerTestAccess::runtimeSlots(*manager);
    OMARCHY_CHECK(observations.size() == 1);
    return observations.front();
  };
  const auto current_view = [&] {
    const auto slot = current_slot();
    auto view = bridge::PluginManagerTestAccess::permissionView(
        *manager, plugin, slot.epoch);
    OMARCHY_CHECK(view && view->active);
    return *view;
  };
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 1);
  run_preparation();
  OMARCHY_CHECK(notification_backend->awaitEntered("status") &&
              notification_backend->calls("status") == 1);
  auto slot = current_slot();
  OMARCHY_CHECK(current_view().active->binding == first_binding);
  OMARCHY_CHECK(manager->count() == 1);
  const auto stale_surface_key = barSurfaceKey(*manager, plugin);
  OMARCHY_CHECK(!stale_surface_key.isEmpty());
  QQuickWindow permission_window;
  permission_window.resize(64, 64);
  permission_window.show();
  bridge::RemotePluginSurface live_remote(permission_window.contentItem());
  live_remote.setWidth(64);
  live_remote.setHeight(64);
  OMARCHY_CHECK(manager->attach(stale_surface_key, &live_remote) &&
              live_remote.connected());

  auto view = current_view();
  for (const bool inject_throw : {false, true}) {
    scheduler.refuses = !inject_throw;
    scheduler.throws = inject_throw;
    OMARCHY_CHECK(!bridge::PluginManagerTestAccess::revokePermission(
                *manager, plugin, slot.epoch, notifications,
                view.authority_slots.sequence) &&
                !current_slot().permission_transaction &&
                current_slot().epoch == slot.epoch && manager->count() == 1);
  }
  scheduler.refuses = false;
  scheduler.throws = false;

  OMARCHY_CHECK(bridge::PluginManagerTestAccess::revokePermission(
              *manager, plugin, slot.epoch, absent,
              view.authority_slots.sequence) &&
              current_slot().permission_transaction);
  scheduler.runThread();
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::executingPermissionJobs(*manager) ==
                  1 &&
              current_slot().permission_transaction);
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  OMARCHY_CHECK(current_slot().epoch == slot.epoch && current_slot().running &&
              !current_slot().permission_transaction && manager->count() == 1);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::revokePermission(
              *manager, plugin, slot.epoch, absent_dynamic,
              view.authority_slots.sequence));
  scheduler.runAndDrain(*manager);
  OMARCHY_CHECK(current_slot().epoch == slot.epoch && current_slot().running &&
              manager->count() == 1);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::revokePermission(
              *manager, plugin, slot.epoch, notifications,
              view.authority_slots.sequence + 1));
  scheduler.runAndDrain(*manager);
  OMARCHY_CHECK(current_slot().epoch == slot.epoch && current_slot().running &&
              manager->count() == 1);

  auto *control = manager->permissions();
  OMARCHY_CHECK(control != nullptr && control->parent() == manager.get());
  const auto optional_list = scheduler.completeOperation(*manager, control->beginList(QString::fromUtf8(plugin)));
  const auto optional_rows = permissionRows(*control, optional_list);
  OMARCHY_CHECK(std::ranges::none_of(optional_rows,
                               [](const QJsonValue &value) {
                                 return value.toObject().contains("version");
                               }));
  const auto optional_row = permissionRow(optional_rows, "notifications.send");
  OMARCHY_CHECK(!optional_row.isEmpty());
  const auto stale_list = scheduler.completeOperation(*manager, control->beginList(QString::fromUtf8(plugin)));
  const auto stale_optional_row =
      permissionRow(permissionRows(*control, stale_list), "notifications.send");
  const auto optional_revoke = control->revoke(optional_list, optional_row);
  OMARCHY_CHECK(!optional_revoke.isEmpty());
  bool reentrant_attach_attempted = false;
  bool reentrant_attach_succeeded = false;
  QObject::connect(&live_remote,
                   &bridge::RemotePluginSurface::connectionChanged, [&] {
                     if (live_remote.connected())
                       return;
                     reentrant_attach_attempted = true;
                     reentrant_attach_succeeded =
                         manager->attach(stale_surface_key, &live_remote);
                   });
  std::thread mutation([&] { scheduler.runOne(); });
  OMARCHY_CHECK(await([&] {
            bridge::PluginManagerTestAccess::drainRuntime(*manager);
            return current_slot().permission_changing;
          }));
  bridge::RemotePluginSurface stale_remote;
  OMARCHY_CHECK(current_slot().permission_changing &&
              current_slot().permission_transaction &&
              current_slot().has_runtime_root && scheduler.jobs.empty() &&
              bridge::PluginManagerTestAccess::executingPermissionJobs(
                  *manager) == 1 &&
              manager->count() == 0 && !live_remote.connected() &&
              reentrant_attach_attempted && !reentrant_attach_succeeded &&
              !manager->attach(stale_surface_key, &stale_remote));
  notification_backend->release("status");
  mutation.join();
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  OMARCHY_CHECK(permissionOperation(*control, optional_revoke).value("state") ==
                  "succeeded" &&
              control->revoke(optional_list, optional_row).isEmpty());
  OMARCHY_CHECK(current_slot().preparing && scheduler.jobs.size() == 1);
  run_preparation();
  slot = current_slot();
  view = current_view();
  auto expected_binding = first_binding;
  expected_binding.generation = first_binding.generation + 1;
  OMARCHY_CHECK(view.active->binding == expected_binding);
  const auto revoked_optional =
      std::ranges::find_if(view.active->dynamic_grants, [&](const auto &grant) {
        return grant.request.definition == notifications;
      });
  OMARCHY_CHECK(revoked_optional != view.active->dynamic_grants.end() &&
              revoked_optional->grant.state == permissions::GrantState::revoked);
  OMARCHY_CHECK(manager->count() == 1);
  OMARCHY_CHECK(control->revoke(stale_list, stale_optional_row).isEmpty());
  bridge::RemotePluginSurface denied_remote(permission_window.contentItem());
  denied_remote.setWidth(64);
  denied_remote.setHeight(64);
  const auto denied_key = barSurfaceKey(*manager, plugin);
  OMARCHY_CHECK(manager->attach(denied_key, &denied_remote) &&
              await([&] { return denied_remote.ready(); }) &&
              redSignature(paintedFrame(denied_remote)) &&
              notification_backend->calls("status") == 1);

  auto review = bridge::PluginManagerTestAccess::preparePermissionReview(
      *manager, plugin, slot.epoch);
  OMARCHY_CHECK(review != nullptr);
  const auto later_review =
      bridge::PluginManagerTestAccess::preparePermissionReview(*manager, plugin,
                                                               slot.epoch);
  OMARCHY_CHECK(later_review && later_review != review);
  std::vector<host::DynamicConsentDecision> decisions;
  for (const auto &row : review->dynamic_rows) {
    OMARCHY_CHECK(row.requested.has_value());
    decisions.push_back(
        {.definition = row.requested->definition,
         .operations = row.requested->operations,
         .decided_scope = row.requested->scope,
         .decision = permissions::UserDecision::grant});
  }
  host::ConsentConfirmation confirmed{
      .review_fingerprint = review->fingerprint,
      .decision_fingerprint = host::consent_decision_fingerprint(*review, decisions),
      .actor = permissions::DecisionActor::trusted_ui,
      .confirmed_wall_seconds = 1};
  OMARCHY_CHECK(!bridge::PluginManagerTestAccess::applyPermissionReview(
              *manager, plugin, slot.epoch, {}, confirmed, decisions) &&
              current_slot().running);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::applyPermissionReview(
              *manager, plugin, slot.epoch, review, confirmed, decisions));
  scheduler.runAndDrain(*manager);
  OMARCHY_CHECK(current_slot().preparing && scheduler.jobs.size() == 1);
  run_preparation();
  OMARCHY_CHECK(await([&] { return notification_backend->calls("status") == 2; }));
  slot = current_slot();
  view = current_view();

  const auto optional_review = scheduler.completeOperation(*manager, control->beginReview(QString::fromUtf8(plugin)));
  const auto optional_review_rows = permissionRows(*control, optional_review);
  auto extra_key_choices =
      QJsonDocument::fromJson(
          grantEveryAvailablePermission(optional_review_rows).toUtf8())
          .object();
  auto extra_key_array = extra_key_choices.value("choices").toArray();
  auto extra_key_choice = extra_key_array.first().toObject();
  extra_key_choice.insert("operations", QJsonArray{});
  extra_key_array[0] = extra_key_choice;
  extra_key_choices.insert("choices", extra_key_array);
  OMARCHY_CHECK(
      control->apply(optional_review, "{}").isEmpty() &&
          control
              ->applyInteractiveCli(
                  optional_review,
                  grantEveryAvailablePermission(optional_review_rows))
              .isEmpty() &&
          control
              ->apply(optional_review,
                      QStringLiteral("{\"choices\":[{\"rowId\":\"foreign\","
                                     "\"decision\":\"grant\"}]}"))
              .isEmpty() &&
          control
              ->apply(optional_review,
                      QString::fromUtf8(QJsonDocument(extra_key_choices)
                                            .toJson(QJsonDocument::Compact)))
              .isEmpty());
  const auto optional_apply = control->apply(
      optional_review, grantEveryAvailablePermission(optional_review_rows));
  OMARCHY_CHECK(!optional_apply.isEmpty() &&
              control
                  ->apply(optional_review,
                          grantEveryAvailablePermission(optional_review_rows))
                  .isEmpty());
  scheduler.runAndDrain(*manager);
  OMARCHY_CHECK(permissionOperation(*control, optional_apply).value("state") ==
              "succeeded");
  OMARCHY_CHECK(current_slot().preparing && scheduler.jobs.size() == 1);
  notification_backend->hold("status");
  run_preparation();
  OMARCHY_CHECK(notification_backend->awaitEntered("status") &&
              notification_backend->calls("status") == 3);
  notification_backend->release("status");
  slot = current_slot();
  view = current_view();
  expected_binding.generation = first_binding.generation + 3;
  OMARCHY_CHECK(view.active->binding == expected_binding);
  OMARCHY_CHECK(manager->count() == 1);
  bridge::RemotePluginSurface granted_remote(permission_window.contentItem());
  granted_remote.setWidth(64);
  granted_remote.setHeight(64);
  OMARCHY_CHECK(manager->attach(barSurfaceKey(*manager, plugin), &granted_remote) &&
              await([&] { return granted_remote.ready(); }) &&
              paintedFrame(granted_remote).pixelColor(32, 32).green() >= 150);

  const auto required_list = scheduler.completeOperation(*manager, control->beginList(QString::fromUtf8(plugin)));
  const auto required_row =
      permissionRow(permissionRows(*control, required_list), "storage.private");
  const auto required_revoke = scheduler.completeOperation(*manager, control->revoke(required_list, required_row));
  OMARCHY_CHECK(permissionOperation(*control, required_revoke).value("state") ==
              "succeeded");
  slot = current_slot();
  OMARCHY_CHECK(slot.permission_disabled && scheduler.jobs.empty());
  manager.reset();

  services.context = notification_backend;
  services.notification_send = BlockingNotifications::send;
  manager = createRuntimeManager(fixture, &scheduler, std::move(services));
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 1);
  scheduler.runAndDrain(*manager);
  slot = current_slot();
  OMARCHY_CHECK(slot.permission_disabled && scheduler.jobs.empty());
  control = manager->permissions();
  const auto required_review = scheduler.completeOperation(*manager, control->beginReview(QString::fromUtf8(plugin)));
  const auto required_apply = scheduler.completeOperation(
      *manager, control->apply(
      required_review,
      grantEveryAvailablePermission(permissionRows(*control, required_review))));
  OMARCHY_CHECK(permissionOperation(*control, required_apply).value("state") ==
              "succeeded");
  OMARCHY_CHECK(current_slot().preparing && scheduler.jobs.size() == 1);
  notification_backend->hold("status");
  run_preparation();
  OMARCHY_CHECK(notification_backend->awaitEntered("status"));
  view = current_view();
  expected_binding.generation = first_binding.generation + 5;
  OMARCHY_CHECK(view.active->binding == expected_binding);

  slot = current_slot();
  view = current_view();
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::revokePermission(
              *manager, plugin, slot.epoch, notifications,
              view.authority_slots.sequence) &&
              scheduler.jobs.size() == 1);
  auto blocked_job = std::move(scheduler.jobs.front());
  scheduler.jobs.clear();
  scheduler.kinds.clear();
  const auto delivery_gate =
      bridge::PluginManagerTestAccess::deliveryGate(*manager);
  std::thread blocked_worker([job = std::move(blocked_job)]() mutable {
    job();
    job = {};
  });
  OMARCHY_CHECK(await([&] {
            bridge::PluginManagerTestAccess::drainRuntime(*manager);
            return current_slot().permission_changing;
          }) &&
              current_slot().has_runtime_root &&
              bridge::PluginManagerTestAccess::executingPermissionJobs(
                  *manager) == 1);
  std::atomic<bool> destruction_started = false;
  std::thread release_effect([&] {
    while (!destruction_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    notification_backend->release("status");
  });
  destruction_started.store(true, std::memory_order_release);
  manager.reset();
  release_effect.join();
  blocked_worker.join();
  OMARCHY_CHECK(delivery_gate.expired());
}

void public_permission_lifecycle_is_closed_until_exact_consent() {
  OMARCHY_CHECK(::access(
              worker_path().c_str(),
              X_OK) == 0);

  constexpr std::string_view plugin_a = "org.example.lifecycle-a";
  constexpr std::string_view plugin_b = "org.example.lifecycle-b";
  constexpr std::string_view permissions_a =
      R"({"required":[{"capability":"storage.private","definitionGeneration":1,"definitionDigest":"8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66","operations":["read","write","remove"],"itemBytes":1024,"reason":"state","quotaBytes":4096}],"optional":[{"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"],"reason":"alerts","categories":["a"]}]})";
  constexpr std::string_view permissions_b =
      R"({"required":[],"optional":[{"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"],"reason":"alerts","categories":["b"]}]})";

  RuntimeFixture fixture;
  const auto binding_a = fixture.stageRuntime(
      plugin_a, 1, permissionAwareQmlFor("a"), permissions_a);
  const auto binding_b = fixture.stageRuntime(
      plugin_b, 1, permissionAwareQmlFor("b"), permissions_b);
  fixture.activateStaged(binding_a);
  fixture.activateStaged(binding_b);

  auto transport = std::make_unique<LifecycleNotificationTransport>();
  auto *notification_state = transport.get();
  auto notification_service =
      std::make_shared<channel::DesktopNotificationService>(
          [owned = std::shared_ptr<LifecycleNotificationTransport>(std::move(transport))](const auto &notification) {
            return owned->send(notification);
          });
  const auto services = [&] {
    return channel::RuntimeServices{
        .context = notification_service,
        .notification_send = channel::DesktopNotificationService::send};
  };

  DeterministicJobs scheduler;
  auto manager = createRuntimeManager(fixture, &scheduler, services());
  const auto drain = [&] {
    bridge::PluginManagerTestAccess::drainRuntime(*manager);
  };
  const auto observe_slots = [&] {
    return bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  };

  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 2);
  scheduler.runThread(1);
  scheduler.runThread(0);
  drain();
  auto observations = observe_slots();
  OMARCHY_CHECK(observations.size() == 2 &&
              observed(observations, plugin_a).permission_disabled &&
              observed(observations, plugin_b).permission_disabled &&
              manager->count() == 0 && scheduler.jobs.empty());

  auto *control = manager->permissions();
  const auto review_a = control->beginReview(QString::fromUtf8(plugin_a));
  const auto review_b = control->beginReview(QString::fromUtf8(plugin_b));
  OMARCHY_CHECK(!review_a.isEmpty() && !review_b.isEmpty() &&
              scheduler.jobs.size() == 2);
  scheduler.runThread(1);
  scheduler.runThread(0);
  drain();
  const auto rows_a = permissionRows(*control, review_a);
  const auto rows_b = permissionRows(*control, review_b);
  constexpr std::array<std::string_view, 1> deny_required{"storage.private"};
  OMARCHY_CHECK(control->apply(review_a, decidePermissions(rows_a, deny_required))
                  .isEmpty() &&
              scheduler.jobs.empty());
  observations = observe_slots();
  OMARCHY_CHECK(observed(observations, plugin_a).permission_disabled &&
              observed(observations, plugin_b).permission_disabled &&
              manager->count() == 0);

  constexpr std::array<std::string_view, 1> deny_notifications{
      "notifications.send"};
  const auto apply_a =
      control->apply(review_a, decidePermissions(rows_a, deny_notifications));
  const auto apply_b = control->apply(review_b, decidePermissions(rows_b, {}));
  OMARCHY_CHECK(!apply_a.isEmpty() && !apply_b.isEmpty() &&
              scheduler.jobs.size() == 2);
  scheduler.runThread(1);
  scheduler.runThread(0);
  drain();
  OMARCHY_CHECK(permissionOperation(*control, apply_a).value("state") ==
                  "succeeded" &&
              permissionOperation(*control, apply_b).value("state") ==
                  "succeeded" &&
              scheduler.jobs.size() == 2);

  std::jthread revocation;
  // Release the effect before joining revocation, including assertion unwinding.
  const auto release_notification = qScopeGuard([&] { notification_state->release(); });
  notification_state->hold();
  scheduler.runThread(1);
  scheduler.runThread(0);
  drain();
  const bool initial_running = await([&] {
    drain();
    const auto current = observe_slots();
    return observed(current, plugin_a).running &&
           observed(current, plugin_b).running && manager->count() == 2;
  });
  if (!initial_running) {
    const auto current = observe_slots();
    const auto &a = observed(current, plugin_a);
    const auto &b = observed(current, plugin_b);
    throw std::runtime_error(
        "consented runtimes did not start: A state/error=" +
        std::to_string(a.last_state) + "/" + std::to_string(a.last_error) +
        ", B state/error=" + std::to_string(b.last_state) + "/" +
        std::to_string(b.last_error));
  }
  OMARCHY_CHECK(notification_state->awaitEntered());
  const auto first_notification = notification_state->last();
  OMARCHY_CHECK(first_notification.plugin == QString::fromUtf8(plugin_b) &&
              first_notification.category == "b" &&
              notification_state->calls() == 1);

  QQuickWindow window;
  window.resize(128, 64);
  window.show();
  bridge::RemotePluginSurface remote_a(window.contentItem());
  bridge::RemotePluginSurface remote_b(window.contentItem());
  remote_a.setWidth(64);
  remote_a.setHeight(64);
  remote_b.setX(64);
  remote_b.setWidth(64);
  remote_b.setHeight(64);
  const auto key_a = barSurfaceKey(*manager, plugin_a);
  const auto key_b = barSurfaceKey(*manager, plugin_b);
  OMARCHY_CHECK(!key_a.isEmpty() && !key_b.isEmpty() &&
              manager->attach(key_a, &remote_a) &&
              manager->attach(key_b, &remote_b) &&
              await([&] { return remote_a.ready(); }) && remote_b.connected() &&
              redSignature(paintedFrame(remote_a)));

  const auto list_b = scheduler.completeOperation(*manager, control->beginList(QString::fromUtf8(plugin_b)));
  const auto row_b =
      permissionRow(permissionRows(*control, list_b), "notifications.send");
  const auto revoke_b = control->revoke(list_b, row_b);
  OMARCHY_CHECK(!revoke_b.isEmpty() && scheduler.jobs.size() == 1);
  revocation = std::jthread([&] { scheduler.runOne(); });
  OMARCHY_CHECK(await([&] {
            drain();
            return observed(observe_slots(), plugin_b).permission_changing;
          }));
  OMARCHY_CHECK(
      manager->count() == 1 && remote_a.connected() && !remote_b.connected() &&
          bridge::PluginManagerTestAccess::executingPermissionJobs(*manager) ==
              1);
  notification_state->release();
  revocation.join();
  drain();
  OMARCHY_CHECK(permissionOperation(*control, revoke_b).value("state") ==
                  "succeeded" &&
              observed(observe_slots(), plugin_b).preparing &&
              scheduler.jobs.size() == 1);
  scheduler.runThread();
  drain();
  OMARCHY_CHECK(await([&] {
            drain();
            return observed(observe_slots(), plugin_b).running &&
                   manager->count() == 2;
          }));
  bridge::RemotePluginSurface denied_b(window.contentItem());
  denied_b.setWidth(64);
  denied_b.setHeight(64);
  OMARCHY_CHECK(manager->attach(barSurfaceKey(*manager, plugin_b), &denied_b) &&
              await([&] { return denied_b.ready(); }) &&
              redSignature(paintedFrame(denied_b)) &&
              notification_state->calls() == 1);

  const auto list_a = scheduler.completeOperation(*manager, control->beginList(QString::fromUtf8(plugin_a)));
  const auto storage_row =
      permissionRow(permissionRows(*control, list_a), "storage.private");
  const auto revoke_a = scheduler.completeOperation(*manager, control->revoke(list_a, storage_row));
  OMARCHY_CHECK(permissionOperation(*control, revoke_a).value("state") ==
                  "succeeded" &&
              observed(observe_slots(), plugin_a).permission_disabled &&
              manager->count() == 1 && !remote_a.connected() &&
              denied_b.connected());

  manager.reset();
  OMARCHY_CHECK(!denied_b.connected());
  manager = createRuntimeManager(fixture, &scheduler, services());
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 2);
  scheduler.runThread(1);
  scheduler.runThread(0);
  drain();
  OMARCHY_CHECK(await([&] {
            drain();
            const auto current = observe_slots();
            return observed(current, plugin_a).permission_disabled &&
                   observed(current, plugin_b).running && manager->count() == 1;
          }));

  control = manager->permissions();
  const auto regrant_review = scheduler.completeOperation(*manager, control->beginReview(QString::fromUtf8(plugin_a)));
  const auto regrant = scheduler.completeOperation(
      *manager, control->apply(
      regrant_review,
      grantEveryAvailablePermission(permissionRows(*control, regrant_review))));
  OMARCHY_CHECK(permissionOperation(*control, regrant).value("state") ==
                  "succeeded" &&
              observed(observe_slots(), plugin_a).preparing &&
              scheduler.jobs.size() == 1);
  notification_state->hold();
  scheduler.runThread();
  drain();
  OMARCHY_CHECK(await([&] {
            drain();
            return observed(observe_slots(), plugin_a).running &&
                   manager->count() == 2;
          }) &&
              notification_state->awaitEntered());
  const auto recovered_notification = notification_state->last();
  OMARCHY_CHECK(recovered_notification.plugin == QString::fromUtf8(plugin_a) &&
              recovered_notification.category == "a" &&
              notification_state->calls() == 2);
  notification_state->release();
}

void public_archive_install_is_closed_until_exact_consent() {
  for (const bool required_permission : {true, false}) {
    const std::string_view plugin = required_permission
        ? "org.example.secure-install" : "org.example.zero-permission-install";
    RuntimeFixture fixture;
    const auto archive = required_permission
        ? fixture.archive(plugin, "first")
        : fixture.archive(plugin, "zero-permission", R"({"required":[],"optional":[]})");
    const auto archive_path = QString::fromStdString(archive.string());
    const auto descriptor_count = openDescriptorCount();
    {
      auto no_runtime = bridge::PluginManagerTestAccess::create();
      QString embedded_nul = QStringLiteral("/tmp/archive");
      embedded_nul.append(QChar(0));
      embedded_nul.append(QStringLiteral("tail"));
      OMARCHY_CHECK(no_runtime->installer()->begin(archive_path).isEmpty() &&
                    no_runtime->installer()->begin(embedded_nul).isEmpty() &&
                    openDescriptorCount() == descriptor_count);
    }
    DeterministicJobs scheduler;
    auto manager = createRuntimeManager(fixture, &scheduler);
    auto *installer = manager->installer();
    auto *control = manager->permissions();
    const auto poll_install = [&](const QString &operation) {
      return QJsonDocument::fromJson(installer->poll(operation).toUtf8()).object();
    };
    const auto runtime_descriptor_count = openDescriptorCount();
    const auto symlink = archive.parent_path() / "archive-link.tar";
    std::filesystem::create_symlink(archive, symlink);
    OMARCHY_CHECK(installer->begin(QString::fromStdString(symlink.string())).isEmpty() &&
                  scheduler.jobs.empty() && openDescriptorCount() == runtime_descriptor_count);
    for (const bool inject_throw : {false, true}) {
      scheduler.refuses = !inject_throw;
      scheduler.throws = inject_throw;
      OMARCHY_CHECK(installer->begin(archive_path).isEmpty() &&
                    openDescriptorCount() == runtime_descriptor_count);
    }
    scheduler.refuses = scheduler.throws = false;

    auto install = installer->begin(archive_path);
    OMARCHY_CHECK(!install.isEmpty() && scheduler.jobs.size() == 1 &&
                  scheduler.kinds.front() == bridge::PluginManagerTestAccess::TestJobKind::install &&
                  manager->count() == 0);
    scheduler.runAndDrain(*manager);
    const auto installed = poll_install(install);
    OMARCHY_CHECK(installed.value("state") == "succeeded" &&
                  installed.value("result").toObject().value("plugin") == QString::fromUtf8(plugin) &&
                  !installed.value("result").toObject().contains("revision") &&
                  scheduler.jobs.size() == 1 && manager->count() == 0);
    auto observations = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
    OMARCHY_CHECK(observations.size() == 1 && observed(observations, plugin).preparing &&
                  !observed(observations, plugin).has_runtime_root &&
                  !observed(observations, plugin).has_endpoint_owner);
    const auto require_unconsented = [&] {
      const auto current = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
      const auto view = bridge::PluginManagerTestAccess::permissionView(
          *manager, plugin, observed(current, plugin).epoch);
      OMARCHY_CHECK(observed(current, plugin).permission_disabled && view &&
                    view->authority_slots.sequence == 0 && !view->active && manager->count() == 0);
    };
    scheduler.runAndDrain(*manager);
    require_unconsented();

    auto review = scheduler.completeOperation(*manager, installer->beginReview(install));
    const auto reviewed = permissionOperation(*control, review);
    OMARCHY_CHECK(reviewed.value("state") == "succeeded" &&
                  reviewed.value("result").toObject().value("plugin") == QString::fromUtf8(plugin) &&
                  installer->beginReview(install).isEmpty() && manager->count() == 0 &&
                  scheduler.jobs.empty());
    const auto rows = permissionRows(*control, review);
    OMARCHY_CHECK(rows.size() == (required_permission ? 1 : 0));
    const auto stale_choices = grantEveryAvailablePermission(rows);

    if (required_permission) {
      OMARCHY_CHECK(rows.first().toObject().value("required").toBool() &&
                    control->applyInteractiveCli(review, R"({"choices":[]})").isEmpty());
      const std::array<std::string_view, 1> denied{"storage.private"};
      OMARCHY_CHECK(control->applyInteractiveCli(review, decidePermissions(rows, denied)).isEmpty() &&
                    scheduler.jobs.empty());
      require_unconsented();

      const auto invalid_archive = archive.parent_path() / "invalid.tar";
      std::ofstream(invalid_archive) << "not a tar archive";
      const auto rejected = scheduler.completeOperation(
          *manager, installer->begin(QString::fromStdString(invalid_archive.string())));
      const auto rejected_result = poll_install(rejected);
      OMARCHY_CHECK(rejected_result.value("state") == "failed" &&
                    rejected_result.value("error") == "archive-rejected" &&
                    permissionOperation(*control, review).value("state") == "succeeded" &&
                    scheduler.jobs.empty() && manager->count() == 0);

      const auto update_archive = fixture.archive(plugin, "update");
      install = scheduler.completeOperation(
          *manager, installer->begin(QString::fromStdString(update_archive.string())));
      OMARCHY_CHECK(poll_install(install).value("state") == "succeeded" &&
                    control->applyInteractiveCli(review, stale_choices).isEmpty() &&
                    manager->count() == 0 && scheduler.jobs.size() == 1);
      scheduler.runAndDrain(*manager);
      require_unconsented();
      review = scheduler.completeOperation(*manager, installer->beginReview(install));
      OMARCHY_CHECK(permissionOperation(*control, review).value("state") == "succeeded" &&
                    manager->count() == 0);
    }

    observations = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
    const auto exact_review = bridge::PluginManagerTestAccess::preparePermissionReview(
        *manager, plugin, observed(observations, plugin).epoch);
    OMARCHY_CHECK(exact_review && exact_review->dynamic_rows.empty() == !required_permission);
    const auto choices = grantEveryAvailablePermission(permissionRows(*control, review));
    const auto apply = scheduler.completeOperation(
        *manager, control->applyInteractiveCli(review, choices));
    observations = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
    const auto active = bridge::PluginManagerTestAccess::permissionView(
        *manager, plugin, observed(observations, plugin).epoch);
    OMARCHY_CHECK(permissionOperation(*control, apply).value("state") == "succeeded" &&
                  observed(observations, plugin).preparing && active && active->active &&
                  active->active->binding == exact_review->candidate_binding &&
                  active->active->dynamic_grants.empty() == !required_permission &&
                  active->authority_slots.sequence > 0 && manager->count() == 0 &&
                  scheduler.jobs.size() == 1 &&
                  control->applyInteractiveCli(review, stale_choices).isEmpty());
  }
}

void permission_control_is_bounded_and_destruction_safe() {
  constexpr std::string_view plugin = "org.example.bounded-permissions";
  constexpr std::string_view permission_json =
      R"({"required":[],"optional":[]})";
  RuntimeFixture fixture;
  fixture.seedRuntime(plugin, permissionAwareQml, permission_json);
  DeterministicJobs scheduler;
  auto services = channel::RuntimeServices{.context = std::make_shared<int>(0)};
  auto manager = createRuntimeManager(fixture, &scheduler, std::move(services));
  const auto run_job = [&] {
    OMARCHY_CHECK(!scheduler.jobs.empty());
    scheduler.runInlineAndDrain(*manager);
  };

  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 1);
  run_job();
  OMARCHY_CHECK(awaitSingleRunning(*manager));

  auto *control = manager->permissions();
  const auto review =
      control->beginInteractiveCliReview(QString::fromUtf8(plugin));
  OMARCHY_CHECK(!review.isEmpty());
  run_job();
  const auto rows = permissionRows(*control, review);
  OMARCHY_CHECK(
      rows.isEmpty() &&
          control->apply(review, grantEveryAvailablePermission(rows))
              .isEmpty() &&
          control->apply(review, QString(32 + 256 * 1024 + 1, 'x')).isEmpty());

  const auto apply =
      control->applyInteractiveCli(review, grantEveryAvailablePermission(rows));
  OMARCHY_CHECK(!apply.isEmpty());
  OMARCHY_CHECK(!scheduler.jobs.empty());
  scheduler.runOne();
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::pendingPermissionActor(
              *manager, plugin) == permissions::DecisionActor::interactive_cli);
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  OMARCHY_CHECK(permissionOperation(*control, apply).value("state") == "succeeded" &&
              scheduler.jobs.size() == 1);
  run_job();
  OMARCHY_CHECK(awaitSingleRunning(*manager));

  const auto ttl_review = control->beginReview(QString::fromUtf8(plugin));
  OMARCHY_CHECK(!ttl_review.isEmpty());
  run_job();
  bridge::PermissionControlTestAccess::ageOperation(*control, ttl_review,
                                                    std::chrono::minutes(2));
  OMARCHY_CHECK(permissionOperation(*control, ttl_review).value("state") ==
              "succeeded");
  bridge::PermissionControlTestAccess::ageOperation(*control, ttl_review,
                                                    std::chrono::minutes(16));
  OMARCHY_CHECK(control->poll(ttl_review).isEmpty());

  std::size_t accepted = 0;
  for (; accepted < 40; ++accepted) {
    const auto list = control->beginList(QString::fromUtf8(plugin));
    if (list.isEmpty())
      break;
    run_job();
    OMARCHY_CHECK(permissionOperation(*control, list).value("state") == "succeeded");
  }
  OMARCHY_CHECK(accepted > 0 && accepted < 40 &&
              control->beginList(QString::fromUtf8(plugin)).isEmpty());

  manager.reset();
  services = channel::RuntimeServices{.context = std::make_shared<int>(0)};
  manager = createRuntimeManager(fixture, &scheduler, std::move(services));
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 1);
  run_job();
  OMARCHY_CHECK(awaitSingleRunning(*manager));
  omarchy::plugin_runtime::test_support::BlockingGate read;
  bridge::PluginManagerTestAccess::setJobEntryProbe(*manager, [&](auto kind) {
    if (kind != bridge::PluginManagerTestAccess::TestJobKind::permission)
      return;
    read.arrive_and_wait();
  });
  control = manager->permissions();
  OMARCHY_CHECK(!control->beginList(QString::fromUtf8(plugin)).isEmpty() &&
              scheduler.jobs.size() == 1);
  const auto delivery_gate =
      bridge::PluginManagerTestAccess::deliveryGate(*manager);
  auto blocked_read = std::move(scheduler.jobs.front());
  scheduler.jobs.clear();
  scheduler.kinds.clear();
  std::thread read_worker([job = std::move(blocked_read)]() mutable { job(); });
  read.wait_entered();
  manager.reset();
  read.release();
  read_worker.join();
  OMARCHY_CHECK(delivery_gate.expired());
}

void controlled_mutations_settle_after_slot_loss() {
  constexpr std::string_view permission_json =
      R"({"required":[],"optional":[{"capability":"storage.private","definitionGeneration":1,"definitionDigest":"8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66","operations":["read","write","remove"],"itemBytes":1024,"reason":"state","quotaBytes":4096}]})";
  const auto scenario = [](std::string_view plugin,
                           std::string_view permissions, const auto &check) {
    RuntimeFixture fixture;
    fixture.seedRuntime(plugin, "import QtQuick\nItem {}\n", permissions);
    DeterministicJobs scheduler;
    auto manager = createRuntimeManager(fixture, &scheduler);
    OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
                scheduler.jobs.size() == 1);
    scheduler.runOne();
    OMARCHY_CHECK(awaitSingleRunning(*manager));
    check(plugin, fixture, scheduler, manager);
  };

  scenario("org.example.removed-permission", permission_json,
           [&](auto plugin, auto &fixture, auto &scheduler, auto &manager) {
    auto *control = manager->permissions();
    const auto list = control->beginList(QString::fromUtf8(plugin));
    OMARCHY_CHECK(!list.isEmpty());
    scheduler.runInlineAndDrain(*manager);
    const auto row =
        permissionRow(permissionRows(*control, list), "storage.private");
    const auto revoke = control->revoke(list, row);
    OMARCHY_CHECK(!revoke.isEmpty() && scheduler.jobs.size() == 1);
    fixture.erase(plugin);
    OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
                bridge::PluginManagerTestAccess::runtimeSlots(*manager).empty());
    scheduler.runInlineAndDrain(*manager);
    OMARCHY_CHECK(permissionOperation(*control, revoke).value("state") == "succeeded");
  });

  scenario("org.example.replaced-permission", permission_json,
           [&](auto plugin, auto &fixture, auto &scheduler, auto &manager) {
    auto *control = manager->permissions();
    const auto review = control->beginReview(QString::fromUtf8(plugin));
    OMARCHY_CHECK(!review.isEmpty());
    scheduler.runInlineAndDrain(*manager);
    const auto apply = control->apply(
        review,
        grantEveryAvailablePermission(permissionRows(*control, review)));
    OMARCHY_CHECK(!apply.isEmpty() && scheduler.jobs.size() == 1);
    const auto replacement = fixture.stageRuntime(
        plugin, 2, "import QtQuick\nItem {}\n", permission_json);
    fixture.selectReplacement(replacement);
    OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager));
    scheduler.runInlineAndDrain(*manager);
    const auto outcome = permissionOperation(*control, apply);
    OMARCHY_CHECK(outcome.value("state") == "succeeded" &&
                outcome.value("result").toObject().value("applied") == true);
  });

  constexpr std::string_view required_permission_json =
        R"({"required":[{"capability":"storage.private","definitionGeneration":1,"definitionDigest":"8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66","operations":["read","write","remove"],"itemBytes":1024,"reason":"state","quotaBytes":4096}],"optional":[]})";
  scenario("org.example.rejected-permission", required_permission_json,
           [&](auto plugin, auto &fixture, auto &scheduler, auto &manager) {
    auto *control = manager->permissions();
    const auto review = control->beginReview(QString::fromUtf8(plugin));
    OMARCHY_CHECK(!review.isEmpty());
    scheduler.runInlineAndDrain(*manager);
    const auto rows = permissionRows(*control, review);
    OMARCHY_CHECK(rows.size() == 1);
    const auto slot = bridge::PluginManagerTestAccess::runtimeSlots(*manager)
                          .front();
    const auto view = bridge::PluginManagerTestAccess::permissionView(
        *manager, plugin, slot.epoch);
    OMARCHY_CHECK(view.has_value());
    const auto apply = control->apply(
        review, grantEveryAvailablePermission(rows));
    OMARCHY_CHECK(!apply.isEmpty() && scheduler.jobs.size() == 1);
    const definitions::CapabilityReference storage{
        .canonical_name = definitions::Name("storage.private"), .definition_generation = 1,
        .definition_digest = definitions::Digest("8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66")};
    OMARCHY_CHECK(view && bridge::PluginManagerTestAccess::
                        revokePermissionImmediatelyForTest(
                            *manager, plugin, slot.epoch, storage,
                            view->authority_slots.sequence));
    const auto replacement = fixture.stageRuntime(
        plugin, 2, "import QtQuick\nItem {}\n", required_permission_json);
    fixture.selectReplacement(replacement);
    OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager));
    scheduler.runInlineAndDrain(*manager);
    const auto outcome = permissionOperation(*control, apply);
    OMARCHY_CHECK(outcome.value("state") == "failed" &&
                outcome.value("error") == "authority-rejected");
  });
}

void concurrent_permission_fences_route_exactly() {
  constexpr std::string_view plugin_a = "a.permission-plugin";
  constexpr std::string_view plugin_b = "b.permission-plugin";
  constexpr std::string_view permissions_a =
      R"({"required":[],"optional":[{"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"],"reason":"A alerts","categories":["a"]}]})";
  constexpr std::string_view permissions_b =
      R"({"required":[],"optional":[{"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"],"reason":"B alerts","categories":["b"]}]})";
  RuntimeFixture fixture;
  const auto binding_a =
      fixture.seedRuntime(plugin_a, permissionAwareQmlFor("a"), permissions_a);
  const auto binding_b =
      fixture.seedRuntime(plugin_b, permissionAwareQmlFor("b"), permissions_b);
  auto effects = std::make_shared<BlockingNotifications>();
  effects->hold("a");
  effects->hold("b");
  channel::RuntimeServices services{.context = effects,
                                    .notification_send = BlockingNotifications::send};
  DeterministicJobs scheduler;
  auto manager = bridge::PluginManagerTestAccess::create();
  const auto release_effects = qScopeGuard([&] {
    effects->release("a");
    effects->release("b");
  });
  bridge::PluginManagerTestAccess::installRuntime(
      *manager, fixture.bootstrap(std::move(services)));
  scheduler.install(*manager);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 2);
  while (!scheduler.jobs.empty()) {
    scheduler.runAndDrain(*manager);
  }
  OMARCHY_CHECK(awaitSlots(*manager, [&](const auto &observations) {
            return observations.size() == 2 &&
                   observed(observations, plugin_a).running &&
                   observed(observations, plugin_b).running;
          }) &&
              effects->awaitEntered("a") && effects->awaitEntered("b"));

  auto observations = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  OMARCHY_CHECK(manager->count() == 2);
  const auto key_a = barSurfaceKey(*manager, plugin_a);
  const auto key_b = barSurfaceKey(*manager, plugin_b);
  QQuickWindow permission_window;
  permission_window.resize(128, 64);
  permission_window.show();
  bridge::RemotePluginSurface remote_a(permission_window.contentItem());
  bridge::RemotePluginSurface remote_b(permission_window.contentItem());
  remote_a.setWidth(64);
  remote_a.setHeight(64);
  remote_b.setWidth(64);
  remote_b.setHeight(64);
  remote_b.setX(64);
  const bool attached_a = manager->attach(key_a, &remote_a);
  const bool attached_b = manager->attach(key_b, &remote_b);
  require(attached_a && attached_b,
          "two permission generations did not attach independently: A=" +
              std::to_string(attached_a) + " key=" + key_a.toStdString() +
              ", B=" + std::to_string(attached_b) +
              " key=" + key_b.toStdString());
  auto *control = manager->permissions();
  const auto list_a = control->beginList(QString::fromUtf8(plugin_a));
  const auto list_b = control->beginList(QString::fromUtf8(plugin_b));
  OMARCHY_CHECK(!list_a.isEmpty() && !list_b.isEmpty() && scheduler.jobs.size() == 2);
  scheduler.runInlineAndDrain(*manager);
  scheduler.runInlineAndDrain(*manager);
  const auto row_a =
      permissionRow(permissionRows(*control, list_a), "notifications.send");
  const auto row_b =
      permissionRow(permissionRows(*control, list_b), "notifications.send");
  OMARCHY_CHECK(!row_a.isEmpty() && !row_b.isEmpty() && row_a != row_b &&
              control->revoke(list_a, row_b).isEmpty() &&
              control->revoke(list_b, row_a).isEmpty());
  const auto revoke_a = control->revoke(list_a, row_a);
  const auto revoke_b = control->revoke(list_b, row_b);
  OMARCHY_CHECK(!revoke_a.isEmpty() && !revoke_b.isEmpty() &&
              scheduler.jobs.size() == 2);

  auto job_a = std::move(scheduler.jobs.at(0));
  auto job_b = std::move(scheduler.jobs.at(1));
  scheduler.jobs.clear();
  scheduler.kinds.clear();
  std::thread worker_a([job = std::move(job_a)]() mutable { job(); });
  std::thread worker_b([job = std::move(job_b)]() mutable { job(); });
  OMARCHY_CHECK(awaitSlots(*manager, [&](const auto &current) {
            return observed(current, plugin_a).permission_changing &&
                   observed(current, plugin_b).permission_changing;
          }));
  observations = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  bridge::RemotePluginSurface stale_a;
  bridge::RemotePluginSurface stale_b;
  OMARCHY_CHECK(
      manager->count() == 0 && !remote_a.connected() && !remote_b.connected() &&
          observed(observations, plugin_a).has_runtime_root &&
          observed(observations, plugin_b).has_runtime_root &&
          bridge::PluginManagerTestAccess::executingPermissionJobs(*manager) ==
              2 &&
          !manager->attach(key_a, &stale_a) &&
          !manager->attach(key_b, &stale_b));

  effects->release("b");
  worker_b.join();
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  OMARCHY_CHECK(
      permissionOperation(*control, revoke_b).value("state") == "succeeded" &&
          permissionOperation(*control, revoke_a).value("state") == "pending");
  observations = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  OMARCHY_CHECK(observed(observations, plugin_a).permission_changing &&
              observed(observations, plugin_a).has_runtime_root &&
              observed(observations, plugin_b).preparing &&
              scheduler.jobs.size() == 1);
  scheduler.runThread();
  OMARCHY_CHECK(awaitSlots(*manager, [&](const auto &current) {
            return observed(current, plugin_b).running;
          }));
  auto current_b = bridge::PluginManagerTestAccess::permissionView(
      *manager, plugin_b,
      observed(bridge::PluginManagerTestAccess::runtimeSlots(*manager),
               plugin_b)
          .epoch);
  auto expected_b = binding_b;
  ++expected_b.generation;
  OMARCHY_CHECK(current_b && current_b->active &&
              current_b->active->binding == expected_b &&
              effects->calls("b") == 1);

  effects->release("a");
  worker_a.join();
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  OMARCHY_CHECK(permissionOperation(*control, revoke_a).value("state") == "succeeded");
  observations = bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  OMARCHY_CHECK(observed(observations, plugin_b).running &&
              observed(observations, plugin_a).preparing &&
              scheduler.jobs.size() == 1);
  scheduler.runThread();
  OMARCHY_CHECK(awaitSlots(*manager, [&](const auto &current) {
            return observed(current, plugin_a).running &&
                   observed(current, plugin_b).running;
          }));
  const auto final_slots =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  auto current_a = bridge::PluginManagerTestAccess::permissionView(
      *manager, plugin_a, observed(final_slots, plugin_a).epoch);
  auto expected_a = binding_a;
  ++expected_a.generation;
  OMARCHY_CHECK(current_a && current_a->active &&
              current_a->active->binding == expected_a &&
              effects->calls("a") == 1);
}

void real_root_publishes_attaches_and_tears_down_exactly() {
  const bool packaged_worker_available =
      ::access(
          worker_path().c_str(),
          X_OK) == 0;
  OMARCHY_CHECK(packaged_worker_available);
  using Model = bridge::SurfaceProjectionModel;
  constexpr std::string_view plugin = "org.example.status";
  RuntimeFixture fixture;
  const auto exact_binding = fixture.seedRuntime(plugin);
  DeterministicJobs scheduler;
  auto manager = createRuntimeManager(fixture, &scheduler);

  QQuickWindow window;
  window.resize(320, 64);
  window.show();
  bridge::RemotePluginSurface rollback_remote(window.contentItem());
  rollback_remote.setWidth(320);
  rollback_remote.setHeight(64);
  bridge::RemotePluginSurface *attachment_remote = &rollback_remote;
  bool attached_in_signal = false;
  QObject::connect(manager.get(), &bridge::PluginManager::surfacesChanged, [&] {
    if (manager->count() != 1)
      return;
    const auto key =
        manager->barSurfaces()
            ->data(manager->barSurfaces()->index(0, 0), Model::SurfaceKeyRole)
            .toString();
    attached_in_signal = manager->attach(key, attachment_remote);
  });
  const auto throwing_publication = QObject::connect(
      manager.get(), &bridge::PluginManager::surfacesChanged,
      [] { throw std::runtime_error("injected publication signal failure"); });

  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 1 &&
              scheduler.kinds.front() ==
                  bridge::PluginManagerTestAccess::TestJobKind::preparation);
  const auto first_epoch =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager).front().epoch;
  scheduler.runAndDrain(*manager);
  OMARCHY_CHECK(manager->count() == 0);
  const bool rolled_back = awaitSlots(*manager, [&](const auto &observations) {
    return observations.size() == 1 && observations.front().retry_wait;
  });
  if (!rolled_back) {
    const auto observations =
        bridge::PluginManagerTestAccess::runtimeSlots(*manager);
    OMARCHY_CHECK(!observations.empty());
    const auto &failure = observations.front();
    throw std::runtime_error("publication rollback did not settle: state=" +
                             std::to_string(failure.last_state) +
                             " error=" + std::to_string(failure.last_error) +
                             " opening=" + std::to_string(failure.opening) +
                             " starting=" + std::to_string(failure.starting) +
                             " retry=" + std::to_string(failure.retry_wait));
  }
  OMARCHY_CHECK(manager->count() == 0 && attached_in_signal &&
              !rollback_remote.connected() &&
              !bridge::PluginManagerTestAccess::runtimeSlots(*manager)
                   .front()
                   .has_endpoint_owner);
  QObject::disconnect(throwing_publication);

  bridge::RemotePluginSurface remote(window.contentItem());
  remote.setWidth(320);
  remote.setHeight(64);
  attachment_remote = &remote;
  attached_in_signal = false;
  OMARCHY_CHECK(!bridge::PluginManagerTestAccess::deliverLifecycle(
              *manager, plugin, first_epoch,
              static_cast<std::uint8_t>(host::SessionState::running),
              static_cast<std::uint8_t>(host::SessionError::none)) &&
              bridge::PluginManagerTestAccess::retryRuntime(*manager, plugin) &&
              scheduler.jobs.size() == 1);
  scheduler.runThread();
  OMARCHY_CHECK(awaitSlots(*manager, [&](const auto &observations) {
            return observations.size() == 1 && observations.front().running &&
                   manager->count() == 1;
          }) &&
              attached_in_signal && remote.connected());

  IntentFixture intents(exact_binding);
  auto open = intents.admit(1, surface::SurfaceIntentAction::open);
  auto first_toggle = intents.admit(2, surface::SurfaceIntentAction::toggle);
  auto second_toggle = intents.admit(3, surface::SurfaceIntentAction::toggle);
  auto dismiss = intents.admit(0, surface::SurfaceIntentAction::dismiss, false);
  const auto running =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager).front();
  auto callback = bridge::PluginManagerTestAccess::surfaceIntentCallback(
      *manager, running.plugin, running.epoch);
  OMARCHY_CHECK(callback.has_value());
  std::vector<std::string> delivered;
  QObject::connect(manager.get(), &bridge::PluginManager::openRequested,
                   [&] { delivered.emplace_back("open"); });
  QObject::connect(manager.get(), &bridge::PluginManager::toggleRequested,
                   [&] { delivered.emplace_back("toggle"); });
  QObject::connect(manager.get(), &bridge::PluginManager::dismissRequested,
                   [&] { delivered.emplace_back("dismiss"); });
  bool queued = false;
  std::thread intent_worker([&] {
    queued = callback->deliver(std::move(open)) &&
             callback->deliver(std::move(first_toggle)) &&
             callback->deliver(std::move(second_toggle)) &&
             callback->deliver(std::move(dismiss));
  });
  intent_worker.join();
  OMARCHY_CHECK(queued && callback->pending() == 4 && delivered.empty());
  bridge::PluginManagerTestAccess::drainRuntime(*manager);
  OMARCHY_CHECK(delivered ==
              std::vector<std::string>{"open", "toggle", "toggle", "dismiss"} &&
              callback->pending() == 0);

  const auto row = manager->barSurfaces()->index(0, 0);
  OMARCHY_CHECK(
      manager->barSurfaces()->data(row, Model::GenerationRole).toString() ==
              QString::number(exact_binding.generation) &&
          manager->barSurfaces()->data(row, Model::MaximumWidthRole).toUInt() ==
              320 &&
          manager->barSurfaces()
                  ->data(row, Model::MaximumHeightRole)
                  .toUInt() == 64 &&
          manager->barSurfaces()
                  ->data(row, Model::DefaultSectionRole)
                  .toString() == QStringLiteral("right"));

  const auto published_key =
      manager->barSurfaces()
          ->data(manager->barSurfaces()->index(0, 0), Model::SurfaceKeyRole)
          .toString();
  bool detached_before_withdraw_signal = false;
  bool reentrant_old_key_rejected = false;
  QObject::connect(manager.get(), &bridge::PluginManager::surfacesChanged, [&] {
    if (manager->count() != 0)
      return;
    detached_before_withdraw_signal = !remote.connected();
    reentrant_old_key_rejected = !manager->attach(published_key, &remote);
  });
  fixture.erase(plugin);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              manager->count() == 0 && !remote.connected() &&
              bridge::PluginManagerTestAccess::runtimeSlots(*manager).empty() &&
              detached_before_withdraw_signal && reentrant_old_key_rejected);
  manager.reset();
}

void neutral_surfaces_share_one_real_sandbox_and_teardown() {
  OMARCHY_CHECK(std::getenv("OMARCHY_REQUIRE_PACKAGED_WORKER_TEST") != nullptr);
  OMARCHY_CHECK(::access(worker_path().c_str(),
                   X_OK) == 0);

  using Model = bridge::SurfaceProjectionModel;
  constexpr std::string_view plugin =
      "org.omarchy.fixture.neutral-surfaces";
  const std::filesystem::path fixture_source =
      std::filesystem::path(OMARCHY_NEUTRAL_SURFACE_FIXTURE_ROOT);
  RuntimeFixture fixture;
  const auto exact_binding =
      fixture.seedCheckedFixture(plugin, fixture_source);
  DeterministicJobs scheduler;
  auto manager = createRuntimeManager(fixture, &scheduler);

  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 1 &&
              scheduler.kinds.front() ==
                  bridge::PluginManagerTestAccess::TestJobKind::preparation);
  std::exception_ptr preparation_error;
  std::jthread preparation([&] {
    try {
      scheduler.runOne();
    } catch (...) {
      preparation_error = std::current_exception();
    }
  });
  preparation.join();
  if (preparation_error)
    std::rethrow_exception(preparation_error);
  OMARCHY_CHECK(awaitSlots(*manager, [&](const auto &observations) {
            return observations.size() == 1 && observations.front().running &&
                   observations.front().has_runtime_root &&
                   observations.front().has_endpoint_owner &&
                   manager->count() == 3;
          }));

  auto *bar_model = manager->barSurfaces();
  auto *panel_model = manager->panelSurfaces();
  auto *overlay_model = manager->overlaySurfaces();
  OMARCHY_CHECK(bar_model->rowCount() == 1 && panel_model->rowCount() == 1 &&
              overlay_model->rowCount() == 1);
  const auto assert_row = [&](QAbstractItemModel *model,
                              std::string_view surface,
                              Model::Role expected_role,
                              std::uint32_t width, std::uint32_t height) {
    const auto row = model->index(0, 0);
    OMARCHY_CHECK(model->data(row, Model::PluginIdRole).toString() ==
                    QString::fromUtf8(plugin.data(), plugin.size()) &&
                model->data(row, Model::SurfaceNameRole).toString() ==
                    QString::fromUtf8(surface.data(), surface.size()) &&
                model->data(row, Model::SurfaceRoleRole).toInt() ==
                    static_cast<int>(expected_role) &&
                model->data(row, Model::GenerationRole).toString() ==
                    QString::number(exact_binding.generation) &&
                model->data(row, Model::MaximumWidthRole).toUInt() == width &&
                model->data(row, Model::MaximumHeightRole).toUInt() == height);
    const auto key = model->data(row, Model::SurfaceKeyRole).toString();
    OMARCHY_CHECK(!key.isEmpty());
    return key;
  };
  const auto bar_key =
      assert_row(bar_model, "bar", Model::Role::Bar, 280, 64);
  const auto panel_key =
      assert_row(panel_model, "panel", Model::Role::Panel, 360, 720);
  const auto overlay_key = assert_row(overlay_model, "overlay",
                                      Model::Role::Overlay, 480, 320);
  OMARCHY_CHECK(bar_key != panel_key && bar_key != overlay_key &&
              panel_key != overlay_key &&
              bar_model->data(bar_model->index(0, 0),
                              Model::DefaultSectionRole)
                      .toString() == QStringLiteral("right"));

  QQuickWindow window;
  window.resize(208, 64);
  window.show();
  OMARCHY_CHECK(qmlRegisterType<bridge::RemotePluginSurface>(
              "Omarchy.PluginHost", 1, 0, "RemotePluginSurface") >= 0);
  // Measure rejected-install descriptor ownership before other runtimes can
  // leave asynchronous cleanup in flight. Keep the exact leak assertions.
  public_archive_install_is_closed_until_exact_consent();
  QQmlEngine surface_engine;
  QQmlComponent remote_component(&surface_engine);
  remote_component.setData(
      "import QtQuick\nimport Omarchy.PluginHost 1.0\n"
      "Item { width: 64; height: 64; "
      "RemotePluginSurface { anchors.fill: parent } }\n",
      QUrl());
  OMARCHY_CHECK(remote_component.isReady());
  std::array<std::unique_ptr<QObject>, 3> wrappers;
  std::array<bridge::RemotePluginSurface *, 3> remotes{};
  for (std::size_t index = 0; index < remotes.size(); ++index) {
    wrappers[index].reset(remote_component.create());
    auto *root = qobject_cast<QQuickItem *>(wrappers[index].get());
    remotes[index] =
        wrappers[index]->findChild<bridge::RemotePluginSurface *>();
    OMARCHY_CHECK(root && remotes[index]);
    root->setX(static_cast<qreal>(index * 72));
    root->setParentItem(window.contentItem());
  }
  const std::array keys = {bar_key, panel_key, overlay_key};
  for (std::size_t index = 0; index < remotes.size(); ++index) {
    OMARCHY_CHECK(manager->attach(keys[index], remotes[index]));
    if (!awaitFor(std::chrono::seconds(5), [&] {
          return remotes[index]->connected() && remotes[index]->ready() &&
                 remotes[index]->frameSequence() > 0;
        })) {
      const auto after_attach =
          bridge::PluginManagerTestAccess::runtimeSlots(*manager);
      const auto state =
          after_attach.empty() ? 255 : after_attach.front().last_state;
      const auto error =
          after_attach.empty() ? 255 : after_attach.front().last_error;
      throw std::runtime_error(
          "neutral fixture surface did not complete sequential attachment: " +
          std::string(index == 0 ? "bar" : index == 1 ? "panel" : "overlay") +
          " connected=" + std::to_string(remotes[index]->connected()) +
          " ready=" + std::to_string(remotes[index]->ready()) +
          " sequence=" + std::to_string(remotes[index]->frameSequence()) +
          " inspection=" +
          remotes[index]->inspectionState().toStdString() +
          " state=" + std::to_string(state) +
          " error=" + std::to_string(error));
    }
  }
  if (!awaitFor(std::chrono::seconds(10), [&] {
        return std::ranges::all_of(remotes, [](const auto *remote) {
          return remote->connected() && remote->ready() &&
                 remote->frameSequence() > 0;
        });
      })) {
    std::string detail;
    for (const auto *remote : remotes)
      detail += " connected=" + std::to_string(remote->connected()) +
                " ready=" + std::to_string(remote->ready()) +
                " sequence=" + std::to_string(remote->frameSequence()) +
                " inspection=" + remote->inspectionState().toStdString();
    throw std::runtime_error(
        "neutral fixture surfaces did not render authenticated frames:" +
        detail);
  }
  const std::array<qulonglong, 3> expected_surface_ids = {1, 3, 2};
  for (std::size_t index = 0; index < remotes.size(); ++index)
    OMARCHY_CHECK(remotes[index]->surfaceId() == expected_surface_ids[index] &&
                remotes[index]->surfaceGeneration() ==
                    exact_binding.generation);

  const std::array expected = {QColor(QStringLiteral("#52677a")),
                               QColor(QStringLiteral("#7a6652")),
                               QColor(QStringLiteral("#647052"))};
  std::array<QColor, 3> actual;
  for (std::size_t index = 0; index < remotes.size(); ++index) {
    actual[index] = paintedFrame(*remotes[index]).pixelColor(32, 32);
    OMARCHY_CHECK(actual[index].alpha() == 255 && actual[index] == expected[index]);
  }
  OMARCHY_CHECK(actual[0] != actual[1] && actual[0] != actual[2] &&
              actual[1] != actual[2]);

  const auto running_epoch =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager).front().epoch;
  QString intent_source;
  QString intent_target;
  QString intent_generation;
  QString intent_input_sequence;
  QObject::connect(manager.get(), &bridge::PluginManager::toggleRequested,
                   [&](const QString &source, const QString &target,
                       const QString &generation, const QString &input_sequence) {
                     intent_source = source;
                     intent_target = target;
                     intent_generation = generation;
                     intent_input_sequence = input_sequence;
                   });
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::routeTrustedPointer(
              *manager, plugin, running_epoch, bar_key, true));
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::routeTrustedPointer(
              *manager, plugin, running_epoch, bar_key, false));
  OMARCHY_CHECK(awaitFor(std::chrono::seconds(5), [&] {
            bridge::PluginManagerTestAccess::drainRuntime(*manager);
            return !intent_target.isEmpty();
          }) &&
              intent_source == bar_key && intent_target == panel_key &&
              intent_generation == QString::number(exact_binding.generation) &&
              intent_input_sequence == QStringLiteral("1"));
  OMARCHY_CHECK(await([&] { return pluginScopePaths(exact_binding).size() == 1; }));

  manager.reset();
  OMARCHY_CHECK(std::ranges::none_of(remotes, [](const auto *remote) {
            return remote->connected();
          }));
  OMARCHY_CHECK(awaitFor(std::chrono::seconds(5),
                   [&] { return pluginScopePaths(exact_binding).empty(); }));
}

void zero_surface_runtime_has_no_publication_authority() {
  constexpr std::string_view plugin = "org.example.sidecar-only";
  RuntimeFixture fixture;
  static_cast<void>(fixture.seedRuntime(
      plugin, "import QtQuick\nItem { objectName: \"no-surface\" }\n",
      "{\"required\": [], \"optional\": []}", false));
  DeterministicJobs scheduler;
  auto manager = createRuntimeManager(fixture, &scheduler);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 1);
  scheduler.runThread();
  const bool running = awaitSingleRunning(*manager);
  if (!running) {
    const auto observation =
        bridge::PluginManagerTestAccess::runtimeSlots(*manager).front();
    throw std::runtime_error(
        "zero-surface startup failed: state=" +
        std::to_string(observation.last_state) +
        " error=" + std::to_string(observation.last_error) +
        " retry=" + std::to_string(observation.retry_wait));
  }
  const auto observation =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager).front();
  bridge::RemotePluginSurface remote;
  OMARCHY_CHECK(manager->count() == 0 && !observation.has_endpoint_owner &&
              !manager->attach(QStringLiteral("anything"), &remote));
}

void reentrant_publication_replacement_rechecks_exact_epoch() {
  constexpr std::string_view plugin = "org.example.reentrant";
  RuntimeFixture fixture;
  const auto first = fixture.seedRuntime(plugin, animatedRedQml);
  const auto replacement = fixture.stageRuntime(plugin, 2, animatedGreenQml);
  DeterministicJobs scheduler;
  auto manager = createRuntimeManager(fixture, &scheduler);

  bool replacement_started_in_publication = false;
  bool replacement_scan_succeeded = false;
  QObject::connect(manager.get(), &bridge::PluginManager::surfacesChanged, [&] {
    if (replacement_started_in_publication || manager->count() != 1)
      return;
    replacement_started_in_publication = true;
    fixture.selectReplacement(replacement);
    replacement_scan_succeeded =
        bridge::PluginManagerTestAccess::scanRuntime(*manager);
    if (replacement_scan_succeeded)
      fixture.promoteRuntime(replacement, 2);
  });

  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 1);
  const auto first_epoch =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager).front().epoch;
  scheduler.runThread();
  OMARCHY_CHECK(awaitSlots(*manager, [&](const auto &observations) {
            return replacement_started_in_publication &&
                   replacement_scan_succeeded && observations.size() == 1 &&
                   observations.front().epoch != first_epoch &&
                   scheduler.jobs.size() == 1;
          }) &&
              manager->count() == 0);
  OMARCHY_CHECK(!bridge::PluginManagerTestAccess::deliverLifecycle(
              *manager, plugin, first_epoch,
              static_cast<std::uint8_t>(host::SessionState::running),
              static_cast<std::uint8_t>(host::SessionError::none)));

  scheduler.runThread();
  OMARCHY_CHECK(awaitSlots(*manager, [&](const auto &observations) {
            return observations.size() == 1 && observations.front().running &&
                   manager->count() == 1;
          }));
  const auto row = manager->barSurfaces()->index(0, 0);
  OMARCHY_CHECK(
      manager->barSurfaces()
                  ->data(row, bridge::SurfaceProjectionModel::GenerationRole)
                  .toString() == QString::number(replacement.generation) &&
          replacement.generation != first.generation);
}

void joined_runtimes_replace_and_render_without_cross_routing() {
  OMARCHY_CHECK(::access(
              worker_path().c_str(),
              X_OK) == 0);
  constexpr std::string_view plugin_a = "a.plugin";
  constexpr std::string_view plugin_b = "b.plugin";

  RuntimeFixture fixture;
  const auto first_a = fixture.seedRuntime(plugin_a, animatedRedQml);
  const auto binding_b = fixture.seedRuntime(plugin_b, animatedBlueQml);
  const auto replacement_a =
      fixture.stageRuntime(plugin_a, 2, animatedGreenQml);
  OMARCHY_CHECK(first_a.revision != replacement_a.revision &&
              first_a.generation != replacement_a.generation);

  DeterministicJobs scheduler;
  auto manager = createRuntimeManager(fixture, &scheduler);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 2);
  while (!scheduler.jobs.empty()) {
    scheduler.runAndDrain(*manager);
  }
  const bool both_running = awaitSlots(*manager, [&](const auto &observations) {
    return observations.size() == 2 &&
           observed(observations, plugin_a).running &&
           observed(observations, plugin_b).running && manager->count() == 2;
  });
  if (!both_running) {
    const auto observations =
        bridge::PluginManagerTestAccess::runtimeSlots(*manager);
    const auto &a = observed(observations, plugin_a);
    const auto &b = observed(observations, plugin_b);
    throw std::runtime_error(
        "two packaged workers did not coexist: A state/error=" +
        std::to_string(a.last_state) + "/" + std::to_string(a.last_error) +
        ", B state/error=" + std::to_string(b.last_state) + "/" +
        std::to_string(b.last_error));
  }
  const auto initial_slots =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  const auto first_a_epoch = observed(initial_slots, plugin_a).epoch;
  const auto b_epoch = observed(initial_slots, plugin_b).epoch;
  OMARCHY_CHECK(manager->count() == 2);

  QQmlEngine shell_engine;
  QQmlComponent bar_component(
      &shell_engine,
      QUrl::fromLocalFile(QString::fromStdString(secureBarQmlPath())));
  OMARCHY_CHECK(bar_component.isReady());
  QQuickWindow window;
  window.resize(128, 64);
  window.show();
  const auto first_a_key = barSurfaceKey(*manager, plugin_a);
  const auto b_key = barSurfaceKey(*manager, plugin_b);
  OMARCHY_CHECK(!first_a_key.isEmpty() && !b_key.isEmpty());
  auto bar_a = createSecureBar(bar_component, *manager, first_a_key,
                               first_a.generation, *window.contentItem());
  auto bar_b = createSecureBar(bar_component, *manager, b_key,
                               binding_b.generation, *window.contentItem());
  auto *stale_a = bar_a->findChild<bridge::RemotePluginSurface *>();
  auto *remote_b = bar_b->findChild<bridge::RemotePluginSurface *>();
  OMARCHY_CHECK(stale_a && remote_b);
  qobject_cast<QQuickItem *>(bar_b.get())->setX(64);
  OMARCHY_CHECK(await([&] {
            return stale_a->ready() && remote_b->ready() &&
                   stale_a->frameSequence() >= 2 &&
                   remote_b->frameSequence() >= 2;
          }));
  const auto first_a_image = paintedFrame(*stale_a);
  const auto first_a_sequence = stale_a->frameSequence();
  const auto first_b_image = paintedFrame(*remote_b);
  const auto first_b_sequence = remote_b->frameSequence();
  OMARCHY_CHECK(await([&] {
            return stale_a->frameSequence() > first_a_sequence &&
                   remote_b->frameSequence() > first_b_sequence &&
                   paintedFrame(*stale_a) != first_a_image &&
                   paintedFrame(*remote_b) != first_b_image;
          }));
  OMARCHY_CHECK(redSignature(paintedFrame(*stale_a)) &&
              blueSignature(paintedFrame(*remote_b)));

  const auto b_surface_id = remote_b->surfaceId();
  const auto b_surface_generation = remote_b->surfaceGeneration();
  const auto b_before_replacement = remote_b->frameSequence();
  fixture.selectReplacement(replacement_a);
  OMARCHY_CHECK(bridge::PluginManagerTestAccess::scanRuntime(*manager) &&
              scheduler.jobs.size() == 1 && manager->count() == 1 &&
              !stale_a->connected() && remote_b->connected() &&
              barSurfaceKey(*manager, plugin_b) == b_key &&
              remote_b->surfaceId() == b_surface_id &&
              remote_b->surfaceGeneration() == b_surface_generation);
  const auto replacement_slots =
      bridge::PluginManagerTestAccess::runtimeSlots(*manager);
  const auto replacement_a_epoch = observed(replacement_slots, plugin_a).epoch;
  OMARCHY_CHECK(replacement_a_epoch != first_a_epoch &&
              observed(replacement_slots, plugin_b).epoch == b_epoch &&
              !manager->attach(first_a_key, stale_a));

  // The stopped root released its authority lock. Install the already staged,
  // immutable generation before allowing its bounded preparation to execute.
  fixture.promoteRuntime(replacement_a, 2);
  scheduler.runThread();
  OMARCHY_CHECK(awaitSlots(*manager, [&](const auto &observations) {
            return observed(observations, plugin_a).running &&
                   observed(observations, plugin_b).running &&
                   manager->count() == 2;
          }));
  const auto replacement_a_key = barSurfaceKey(*manager, plugin_a);
  OMARCHY_CHECK(!replacement_a_key.isEmpty() && replacement_a_key != first_a_key &&
              !manager->attach(replacement_a_key, stale_a));

  bar_a.reset();
  auto replacement_bar_a =
      createSecureBar(bar_component, *manager, replacement_a_key,
                      replacement_a.generation, *window.contentItem());
  auto *remote_a =
      replacement_bar_a->findChild<bridge::RemotePluginSurface *>();
  OMARCHY_CHECK(remote_a && await([&] { return remote_a->ready(); }));
  replacement_bar_a.reset();
  OMARCHY_CHECK(remote_b->connected() && await([&] {
            return remote_b->frameSequence() > b_before_replacement;
          }));

  auto fresh_bar_a =
      createSecureBar(bar_component, *manager, replacement_a_key,
                      replacement_a.generation, *window.contentItem());
  auto *fresh_a = fresh_bar_a->findChild<bridge::RemotePluginSurface *>();
  OMARCHY_CHECK(fresh_a && await([&] { return fresh_a->ready(); }) &&
              remote_b->connected());
  const auto fresh_sequence = fresh_a->frameSequence();
  const auto fresh_image = paintedFrame(*fresh_a);
  const auto b_before_comparison = remote_b->frameSequence();
  OMARCHY_CHECK(fresh_sequence != 0 && await([&] {
            return fresh_a->frameSequence() > fresh_sequence &&
                   paintedFrame(*fresh_a) != fresh_image &&
                   remote_b->frameSequence() > b_before_comparison;
          }));
  const auto a2_image = paintedFrame(*fresh_a);
  const auto continued_b_image = paintedFrame(*remote_b);
  OMARCHY_CHECK(greenSignature(a2_image) && blueSignature(continued_b_image) &&
              a2_image != continued_b_image &&
              remote_b->surfaceId() == b_surface_id &&
              remote_b->surfaceGeneration() == b_surface_generation &&
              remote_b->frameSequence() > b_before_replacement);

  manager.reset();
  OMARCHY_CHECK(!fresh_a->connected() && !remote_b->connected());
}

} // namespace

void run_plugin_manager_tests() {
  OMARCHY_CHECK(qmlRegisterType<bridge::RemotePluginSurface>(
              "Omarchy.PluginHost", 1, 0, "RemotePluginSurface") >= 0);
  secure_bar_retries_only_on_readiness_events();
  secure_bar_cannot_expand_the_host_bar();
  host_owned_settings_are_read_and_replaced_atomically();
  qml_hosts_return_startup_snapshots_as_maps();
  process_singleton_factory_is_exact_and_recoverable();
  concurrent_engines_have_one_process_winner();
  singleton_boundary_is_inert_and_not_configurable();
  private_projection_seam_preserves_fail_closed_boundary();
  manager_policy_is_fixed_and_fail_closed();
  last_good_reconciliation_and_stale_callback_are_fail_closed();
  bounded_mailbox_coalesces_and_recovers_without_backoff();
  mailbox_results_are_safe_across_replacement_and_destruction();
  lifecycle_mailbox_keeps_latest_exact_terminal_state();
  surface_intent_mailbox_is_bounded_thread_safe_and_inert_when_stale();
  surface_intent_mailbox_delivers_fifo_for_running_published_slot();
  blocked_replacement_preserves_independent_plugin();
  runtime_jobs_enter_off_ui_and_commit_on_ui_drain();
  manager_owns_permission_generation_replacement();
  permission_control_is_bounded_and_destruction_safe();
  controlled_mutations_settle_after_slot_loss();
  concurrent_permission_fences_route_exactly();
  real_root_publishes_attaches_and_tears_down_exactly();
  zero_surface_runtime_has_no_publication_authority();
  reentrant_publication_replacement_rechecks_exact_epoch();
  joined_runtimes_replace_and_render_without_cross_routing();
}

void run_public_permission_lifecycle_test() {
  public_permission_lifecycle_is_closed_until_exact_consent();
}

void run_neutral_surfaces_real_bwrap_test() {
  neutral_surfaces_share_one_real_sandbox_and_teardown();
}

#include "PluginManager_test.moc"
