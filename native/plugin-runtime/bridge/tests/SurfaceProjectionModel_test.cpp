#include "../../tests/support/gesture_clock.hpp"
#include "../../tests/support/test_assert.hpp"

#include "PluginManager.h"
#include "gesture_intent.hpp"

#include <QPersistentModelIndex>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

void run_surface_endpoint_tests();
void run_surface_endpoint_owner_tests();
void run_plugin_manager_tests();
void run_public_permission_lifecycle_test();
void run_neutral_surfaces_real_bwrap_test();
int run_t07_live_test();

namespace {

namespace bridge = omarchy::plugin_runtime::bridge;
namespace host = omarchy::plugin_runtime::host_session;
namespace permissions = omarchy::plugins::permissions;
namespace runtime = omarchy::plugin_runtime::runtime;
namespace surface = omarchy::plugin_runtime::surface;

using omarchy::plugin_runtime::test_support::require;

using Clock = omarchy::plugin_runtime::test_support::GestureClock<100>;

auto admitted_panel_toggle(host::GestureIntentAuthority &authority,
                            surface::SurfaceKey panel, std::string name) {
  OMARCHY_CHECK(authority.declare_surface(panel, std::move(name)) ==
                  host::SurfaceDeclarationResult::declared &&
              authority.attach_surface(panel) && authority.arm(panel, 1));
  return authority.admit({.source = panel,
                          .target = panel,
                          .input_sequence = 1,
                          .action = surface::SurfaceIntentAction::toggle,
                          .requested_output = {}});
}

class ModelSignalSpy final : public QObject {
public:
  explicit ModelSignalSpy(QAbstractItemModel &model) {
    QObject::connect(&model, &QAbstractItemModel::modelReset, this,
                     [this] { ++resets; });
    QObject::connect(
        &model, &QAbstractItemModel::rowsInserted, this,
        [this](const QModelIndex &, int first, int last) {
          inserted.emplace_back(first, last);
        });
    QObject::connect(
        &model, &QAbstractItemModel::rowsRemoved, this,
        [this](const QModelIndex &, int first, int last) {
          removed.emplace_back(first, last);
        });
  }

  int resets = 0;
  std::vector<std::pair<int, int>> inserted;
  std::vector<std::pair<int, int>> removed;
};

permissions::Digest digest(char value) {
  return permissions::Digest(std::string(64, value));
}

permissions::ActivationBinding binding(std::string_view plugin,
                                       std::uint64_t generation = 7) {
  return {.plugin = permissions::PluginId(plugin),
          .revision = digest('1'),
          .policy_fingerprint = digest('2'),
          .generation = generation};
}

bridge::SurfaceProjectionModel::SurfaceDeclaration declaration(
    std::string name, bridge::SurfaceProjectionModel::Role role) {
  using Service = bridge::SurfaceProjectionModel;
  const bool bar = role == Service::Role::Bar;
  return {.surface_name = std::move(name),
          .role = role,
          .initially_visible = true,
          .maximum_width = bar ? 280U : 640U,
          .maximum_height = bar ? 64U : 480U,
          .dynamic_input_regions = role == Service::Role::Overlay,
          .default_bar_section =
              bar ? Service::BarSection::Left
                  : Service::BarSection::Unspecified};
}

QString value(const bridge::SurfaceProjectionModel &service, int row, int role) {
  return service.data(service.index(row), role).toString();
}

void run() {
  using Service = bridge::SurfaceProjectionModel;
  auto service_owner = bridge::PluginManagerTestAccess::create();
  auto &service_manager = *service_owner;
  auto &service = bridge::PluginManagerTestAccess::model(service_manager);
  OMARCHY_CHECK(service.count() == 0 && service.rowCount() == 0);

  const QString maximum_name(64, QChar(u'X'));
  std::vector<Service::SurfaceDeclaration> surfaces;
  surfaces.push_back(declaration("bar", Service::Role::Bar));
  surfaces.push_back(declaration("PanelWidget", Service::Role::Panel));
  surfaces.push_back(declaration("overlay", Service::Role::Overlay));
  surfaces.push_back(declaration(maximum_name.toStdString(), Service::Role::Panel));
  const auto fixture_binding = binding("org.omarchy.fixture");
  OMARCHY_CHECK(bridge::SurfaceProjectionModelTestAccess::publish(
              service_manager, fixture_binding, surfaces, 2) &&
              service.count() == 4 &&
              service.barSurfaces()->rowCount() == 1 &&
              service.panelSurfaces()->rowCount() == 2 &&
              service.overlaySurfaces()->rowCount() == 1 &&
              !bridge::SurfaceProjectionModelTestAccess::publish(
                  service_manager, fixture_binding, surfaces, 2));
  OMARCHY_CHECK(value(service, 0, Service::SurfaceKeyRole) ==
                  QStringLiteral("v2.19.org.omarchy.fixture.7.bar") &&
              value(service, 0, Service::PluginIdRole) ==
                  QStringLiteral("org.omarchy.fixture") &&
              value(service, 1, Service::SurfaceNameRole) ==
                  QStringLiteral("PanelWidget") &&
              service.data(service.index(1), Service::GenerationRole)
                      .toString() == QStringLiteral("7") &&
              service.data(service.index(1), Service::PublicationRevisionRole)
                      .toString() == QStringLiteral("2") &&
              service.data(service.index(0), Service::DefaultSectionRole)
                      .toString() == QStringLiteral("left") &&
              !service
                   .data(service.index(1), Service::DynamicInputRegionsRole)
                   .toBool() &&
              service
                  .data(service.index(2), Service::DynamicInputRegionsRole)
                  .toBool());

  QQmlEngine qml_engine;
  qml_engine.rootContext()->setContextProperty(QStringLiteral("panelSurfaces"),
                                               service.panelSurfaces());
  qml_engine.rootContext()->setContextProperty(QStringLiteral("overlaySurfaces"),
                                               service.overlaySurfaces());
  QQmlComponent qml_component(&qml_engine);
  qml_component.setData(R"QML(
import QtQml
import QtQml.Models

QtObject {
  property alias panelCount: panels.count
  property alias overlayCount: overlays.count
  property string firstPanelKey: panels.count > 0 ? panels.objectAt(0).surfaceKey : ""
  property bool overlayUsesDynamicInput: overlays.count > 0 && overlays.objectAt(0).dynamicInputRegions

  property Instantiator panelInstances: Instantiator {
    id: panels
    model: panelSurfaces
    delegate: QtObject {
      required property string surfaceKey
      required property string generation
      required property bool initiallyVisible
      required property int maximumWidth
      required property int maximumHeight
      required property bool dynamicInputRegions
    }
  }

  property Instantiator overlayInstances: Instantiator {
    id: overlays
    model: overlaySurfaces
    delegate: QtObject {
      required property string surfaceKey
      required property string generation
      required property bool initiallyVisible
      required property int maximumWidth
      required property int maximumHeight
      required property bool dynamicInputRegions
    }
  }
}
)QML",
                        QUrl());
  std::unique_ptr<QObject> qml_projection(qml_component.create());
  if (!qml_projection) {
    for (const QQmlError &error : qml_component.errors())
      std::cerr << error.toString().toStdString() << '\n';
  } else if (qml_projection->property("panelCount").toInt() != 2 ||
             qml_projection->property("overlayCount").toInt() != 1 ||
             qml_projection->property("firstPanelKey").toString() !=
                 QStringLiteral(
                     "v2.19.org.omarchy.fixture.7.PanelWidget") ||
             !qml_projection->property("overlayUsesDynamicInput").toBool()) {
    std::cerr << "QML projection: panels="
              << qml_projection->property("panelCount").toInt()
              << " overlays="
              << qml_projection->property("overlayCount").toInt()
              << " firstPanelKey="
              << qml_projection->property("firstPanelKey")
                     .toString()
                     .toStdString()
              << " overlayDynamic="
              << qml_projection->property("overlayUsesDynamicInput").toBool()
              << '\n';
  }
  OMARCHY_CHECK(qml_projection &&
              qml_projection->property("panelCount").toInt() == 2 &&
              qml_projection->property("overlayCount").toInt() == 1 &&
              qml_projection->property("firstPanelKey").toString() ==
                  QStringLiteral(
                      "v2.19.org.omarchy.fixture.7.PanelWidget") &&
              qml_projection->property("overlayUsesDynamicInput").toBool());

  const auto exact_key = value(service, 0, Service::SurfaceKeyRole);
  const auto resolved = bridge::SurfaceProjectionModelTestAccess::resolve(
      service, exact_key);
  OMARCHY_CHECK(resolved && resolved->key == exact_key &&
              resolved->binding == fixture_binding &&
              resolved->surface_name == "bar" && resolved->revision == 2 &&
              !bridge::SurfaceProjectionModelTestAccess::resolve(service, {}) &&
              !bridge::SurfaceProjectionModelTestAccess::resolve(
                  service, exact_key + u'x') &&
              !bridge::SurfaceProjectionModelTestAccess::resolve(
                  service, QString(513, u'x')));
  bool off_thread_resolved = true;
  std::thread resolve_worker([&] {
    off_thread_resolved = bridge::SurfaceProjectionModelTestAccess::resolve(
                              service, exact_key)
                              .has_value();
  });
  resolve_worker.join();
  OMARCHY_CHECK(!off_thread_resolved);

  auto invalid = declaration("duplicate", Service::Role::Panel);
  OMARCHY_CHECK(!bridge::SurfaceProjectionModelTestAccess::publish(
              service_manager, fixture_binding, {invalid, invalid}, 3));

  std::vector<Service::SurfaceDeclaration> overflow;
  for (int index = 0; index <= 8; ++index)
    overflow.push_back(
        declaration("surface" + std::to_string(index), Service::Role::Panel));
  OMARCHY_CHECK(!bridge::SurfaceProjectionModelTestAccess::publish(
              service_manager, fixture_binding, std::move(overflow), 3));

  auto multi_owner = bridge::PluginManagerTestAccess::create();
  auto &multi_manager = *multi_owner;
  auto &multi_service = bridge::PluginManagerTestAccess::model(multi_manager);
  const auto binding_a = binding("a.plugin", 4);
  const auto binding_b = binding("b.plugin", 9);
  OMARCHY_CHECK(bridge::SurfaceProjectionModelTestAccess::publish(
              multi_manager, binding_a,
              {declaration("PanelA", Service::Role::Panel)}, 5));
  QPersistentModelIndex persistent_a(multi_service.index(0));
  ModelSignalSpy multi_spy(multi_service);
  QPersistentModelIndex persistent_panel_a(
      multi_service.panelSurfaces()->index(0, 0));
  ModelSignalSpy panel_spy(*multi_service.panelSurfaces());
  OMARCHY_CHECK(bridge::SurfaceProjectionModelTestAccess::publish(
              multi_manager, binding_b,
              {declaration("PanelB", Service::Role::Panel)}, 1) &&
              multi_service.count() == 2 &&
              value(multi_service, 0, Service::PluginIdRole) ==
                  QStringLiteral("a.plugin") &&
              value(multi_service, 1, Service::PluginIdRole) ==
                  QStringLiteral("b.plugin") &&
              multi_service
                      .data(multi_service.index(0),
                            Service::PublicationRevisionRole)
                      .toString() == QStringLiteral("5") &&
              multi_service
                      .data(multi_service.index(1),
                            Service::PublicationRevisionRole)
                      .toString() == QStringLiteral("1"));
  OMARCHY_CHECK(bridge::SurfaceProjectionModelTestAccess::publish(
              multi_manager, binding_b,
              {declaration("PanelB", Service::Role::Panel),
               declaration("OverlayB", Service::Role::Overlay)},
              2) &&
              bridge::SurfaceProjectionModelTestAccess::withdraw(multi_manager,
                                                                 binding_b) &&
              multi_spy.resets == 0 && panel_spy.resets == 0 &&
              persistent_a.isValid() && persistent_panel_a.isValid() &&
              persistent_a.row() == 0 &&
              persistent_panel_a.row() == 0 &&
              multi_service.data(persistent_a, Service::PluginIdRole)
                      .toString() == QStringLiteral("a.plugin") &&
              multi_service.panelSurfaces()
                      ->data(persistent_panel_a, Service::PluginIdRole)
                      .toString() == QStringLiteral("a.plugin") &&
              std::ranges::all_of(
                  multi_spy.removed,
                  [](const auto &range) { return range.first > 0; }) &&
              std::ranges::all_of(
                  multi_spy.inserted,
                  [](const auto &range) { return range.first > 0; }) &&
              std::ranges::all_of(
                  panel_spy.removed,
                  [](const auto &range) { return range.first > 0; }) &&
              std::ranges::all_of(
                  panel_spy.inserted,
                  [](const auto &range) { return range.first > 0; }) &&
              multi_service.count() == 1);
  OMARCHY_CHECK(bridge::SurfaceProjectionModelTestAccess::publish(
              multi_manager, binding_b,
              {declaration("PanelB", Service::Role::Panel)}, 1));
  int cross_plugin_toggles = 0;
  QObject::connect(&multi_service, &Service::toggleRequested,
                   [&cross_plugin_toggles] { ++cross_plugin_toggles; });
  auto cross_clock = std::make_shared<Clock>();
  runtime::GestureEligibilityLatch cross_eligibility(cross_clock);
  host::GestureIntentAuthority cross_authority(binding_a, cross_eligibility);
  const surface::SurfaceKey cross_panel{.id = 2, .generation = 4};
  auto cross_intent = admitted_panel_toggle(cross_authority, cross_panel, "PanelB");
  OMARCHY_CHECK(cross_intent.intent &&
              !bridge::PluginManagerTestAccess::publishIntent(
                  multi_manager, std::move(*cross_intent.intent)) &&
              cross_plugin_toggles == 0);
  const auto newer_a = binding("a.plugin", 5);
  const QString stale_a_key =
      value(multi_service, 0, Service::SurfaceKeyRole);
  int stale_toggles = 0;
  QObject::connect(&multi_service, &Service::toggleRequested,
                   [&stale_toggles] { ++stale_toggles; });
  auto stale_clock = std::make_shared<Clock>();
  runtime::GestureEligibilityLatch stale_eligibility(stale_clock);
  host::GestureIntentAuthority stale_authority(binding_a, stale_eligibility);
  const surface::SurfaceKey stale_panel{.id = 1, .generation = 4};
  auto stale_intent = admitted_panel_toggle(stale_authority, stale_panel, "PanelA");
  OMARCHY_CHECK(bridge::SurfaceProjectionModelTestAccess::publish(
              multi_manager, newer_a,
              {declaration("PanelA", Service::Role::Panel)}, 6) &&
              multi_service.count() == 2 &&
              value(multi_service, 0, Service::SurfaceKeyRole) != stale_a_key &&
              value(multi_service, 0, Service::SurfaceKeyRole) ==
                  QStringLiteral("v2.8.a.plugin.5.PanelA") &&
              stale_intent.intent &&
              !bridge::PluginManagerTestAccess::publishIntent(
                  multi_manager, std::move(*stale_intent.intent)) &&
              stale_toggles == 0 &&
              !bridge::SurfaceProjectionModelTestAccess::withdraw(multi_manager,
                                                                  binding_a) &&
              multi_service.count() == 2 &&
              bridge::SurfaceProjectionModelTestAccess::withdraw(multi_manager,
                                                                 newer_a) &&
              multi_service.count() == 1 &&
              value(multi_service, 0, Service::PluginIdRole) ==
                  QStringLiteral("b.plugin"));

  auto thread_owner = bridge::PluginManagerTestAccess::create();
  auto &thread_manager = *thread_owner;
  auto &thread_service = bridge::PluginManagerTestAccess::model(thread_manager);
  bool off_thread_result = true;
  const auto thread_binding = binding("thread.plugin", 3);
  std::thread off_thread_publish([&] {
    off_thread_result = bridge::SurfaceProjectionModelTestAccess::publish(
        thread_manager, thread_binding,
        {declaration("Panel", Service::Role::Panel)}, 1);
  });
  off_thread_publish.join();
  OMARCHY_CHECK(!off_thread_result && thread_service.count() == 0 &&
              bridge::SurfaceProjectionModelTestAccess::publish(
                  thread_manager, thread_binding,
                  {declaration("Panel", Service::Role::Panel)}, 1));
  std::thread off_thread_withdraw([&] {
    off_thread_result = bridge::SurfaceProjectionModelTestAccess::withdraw(
        thread_manager, thread_binding);
  });
  off_thread_withdraw.join();
  OMARCHY_CHECK(!off_thread_result && thread_service.count() == 1);

  auto thread_clock = std::make_shared<Clock>();
  runtime::GestureEligibilityLatch thread_eligibility(thread_clock);
  host::GestureIntentAuthority thread_authority(thread_binding,
                                                 thread_eligibility);
  const surface::SurfaceKey thread_panel{.id = 1, .generation = 3};
  auto thread_intent = admitted_panel_toggle(thread_authority, thread_panel, "Panel");
  OMARCHY_CHECK(thread_intent.intent.has_value());
  std::thread off_thread_intent(
      [&thread_manager, &off_thread_result,
       intent = std::move(*thread_intent.intent)]() mutable {
        off_thread_result = bridge::PluginManagerTestAccess::publishIntent(
            thread_manager, std::move(intent));
      });
  off_thread_intent.join();
  OMARCHY_CHECK(!off_thread_result && thread_service.count() == 1);
  auto collision_owner = bridge::PluginManagerTestAccess::create();
  auto &collision_manager = *collision_owner;
  auto &collision_service =
      bridge::PluginManagerTestAccess::model(collision_manager);
  OMARCHY_CHECK(bridge::SurfaceProjectionModelTestAccess::publish(
              collision_manager, binding("a.b"),
              {declaration("c", Service::Role::Panel)}, 1));
  const QString first_key =
      value(collision_service, 0, Service::SurfaceKeyRole);
  auto second_collision_owner = bridge::PluginManagerTestAccess::create();
  auto &second_collision_manager = *second_collision_owner;
  auto &second_collision_service =
      bridge::PluginManagerTestAccess::model(second_collision_manager);
  OMARCHY_CHECK(bridge::SurfaceProjectionModelTestAccess::publish(
              second_collision_manager, binding("a"),
              {declaration("bc", Service::Role::Panel)}, 1) &&
              first_key !=
                  value(second_collision_service, 0,
                        Service::SurfaceKeyRole));

  int toggles = 0;
  QString last_target;
  QString last_requested_output;
  QObject::connect(&service, &Service::toggleRequested,
                   [&toggles, &last_target, &last_requested_output](
                       const QString &, const QString &target, const QString &,
                       const QString &, const QString &requested_output) {
                     ++toggles;
                     last_target = target;
                     last_requested_output = requested_output;
                   });
  auto clock = std::make_shared<Clock>();
  runtime::GestureEligibilityLatch eligibility(clock);
  host::GestureIntentAuthority authority(fixture_binding, eligibility);
  const surface::SurfaceKey bar{.id = 1, .generation = 7};
  const surface::SurfaceKey panel{.id = 2, .generation = 7};
  const surface::SurfaceKey maximum{.id = 3, .generation = 7};
  const std::string maximum_name_bytes(64, 'X');
  OMARCHY_CHECK(authority.declare_surface(bar, "bar") ==
                  host::SurfaceDeclarationResult::declared &&
              authority.declare_surface(panel, "PanelWidget") ==
                  host::SurfaceDeclarationResult::declared &&
              authority.declare_surface(maximum, maximum_name_bytes) ==
                  host::SurfaceDeclarationResult::declared &&
              authority.attach_surface(bar) &&
              authority.attach_surface(panel) &&
              authority.arm(bar, 11));
  auto admitted = authority.admit(
      {.source = bar,
       .target = panel,
       .input_sequence = 11,
       .action = surface::SurfaceIntentAction::toggle,
       .requested_output = "DP-1"});
  OMARCHY_CHECK(admitted.intent &&
              bridge::PluginManagerTestAccess::publishIntent(
                  service_manager, std::move(*admitted.intent)) &&
              toggles == 1 &&
              last_requested_output == QStringLiteral("DP-1") &&
              last_target ==
                  QStringLiteral("v2.19.org.omarchy.fixture.7.PanelWidget"));
  OMARCHY_CHECK(authority.arm(bar, 15));
  auto maximum_name_intent = authority.admit(
      {.source = bar,
       .target = maximum,
       .input_sequence = 15,
       .action = surface::SurfaceIntentAction::toggle,
       .requested_output = {}});
  OMARCHY_CHECK(maximum_name_intent.intent &&
              bridge::PluginManagerTestAccess::publishIntent(
                  service_manager, std::move(*maximum_name_intent.intent)) &&
              toggles == 2 &&
              last_requested_output.isEmpty() &&
              last_target == QStringLiteral("v2.19.org.omarchy.fixture.7.") +
                                 maximum_name);
  OMARCHY_CHECK(authority.arm(bar, 12));
  auto delayed = authority.admit(
      {.source = bar,
       .target = panel,
       .input_sequence = 12,
       .action = surface::SurfaceIntentAction::toggle,
       .requested_output = {}});
  OMARCHY_CHECK(delayed.intent && delayed.intent->available());
  clock->now += 5'000'000'000ULL;
  OMARCHY_CHECK(!bridge::PluginManagerTestAccess::publishIntent(
              service_manager, std::move(*delayed.intent)) &&
              toggles == 2);
  OMARCHY_CHECK(authority.arm(bar, 13));
  auto detached_token = authority.admit(
      {.source = bar,
       .target = panel,
       .input_sequence = 13,
       .action = surface::SurfaceIntentAction::toggle,
       .requested_output = {}});
  OMARCHY_CHECK(detached_token.intent && authority.detach_surface(panel) &&
              !bridge::PluginManagerTestAccess::publishIntent(
                  service_manager, std::move(*detached_token.intent)) &&
              toggles == 2);
  OMARCHY_CHECK(authority.attach_surface(panel) &&
              authority.arm(bar, 14));
  auto revoked_token = authority.admit(
      {.source = bar,
       .target = panel,
       .input_sequence = 14,
       .action = surface::SurfaceIntentAction::toggle,
       .requested_output = {}});
  OMARCHY_CHECK(revoked_token.intent.has_value());
  authority.revoke();
  OMARCHY_CHECK(!bridge::PluginManagerTestAccess::publishIntent(
              service_manager, std::move(*revoked_token.intent)) &&
              toggles == 2);
}

} // namespace

int main(int argc, char **argv) {
  QGuiApplication application(argc, argv);
  (void)application;
  return omarchy::plugin_runtime::test_support::test_main([&] {
    if (argc == 2 &&
        std::string_view(argv[1]) == "--public-permission-lifecycle-only") {
      run_public_permission_lifecycle_test();
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--neutral-surfaces-real-bwrap-only") {
      if (std::getenv("OMARCHY_REQUIRE_PACKAGED_WORKER_TEST") == nullptr)
        return 77;
      run_neutral_surfaces_real_bwrap_test();
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--t07-live-only")
      return run_t07_live_test();
    run();
    run_plugin_manager_tests();
    run_surface_endpoint_tests();
    run_surface_endpoint_owner_tests();
    return 0;
  });
}
