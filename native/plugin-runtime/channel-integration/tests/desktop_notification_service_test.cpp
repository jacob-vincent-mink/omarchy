#include "../../tests/support/capability_fixture.hpp"
#include "../../tests/support/blocking_gate.hpp"
#include "../../tests/support/test_assert.hpp"

#include "desktop_notification_service.hpp"

#include <QCoreApplication>
#include <QDBusError>
#include <QMetaType>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace channel = omarchy::plugin_runtime::channel;

namespace {

using omarchy::plugin_runtime::test_support::require;

struct Probe final {
  bool result = true;
  unsigned calls = 0;
  channel::DesktopNotification last;
};

struct HoldingProbe final {
  omarchy::plugin_runtime::test_support::BlockingGate gate;
  unsigned calls = 0;
};

void callback_forwards_only_bounded_fields() {
  auto transport = std::make_unique<Probe>();
  auto *probe = transport.get();
  channel::DesktopNotificationService service(
      [owned = std::shared_ptr<Probe>(std::move(transport))](const auto &notification) noexcept {
        ++owned->calls;
        owned->last = notification;
        return owned->result;
      });
  OMARCHY_CHECK(channel::DesktopNotificationService::send("org.example.clock",
                                                    "timer", "Tea is ready",
                                                    "Five minutes", &service));
  OMARCHY_CHECK(probe->calls == 1 && probe->last.plugin == "org.example.clock" &&
              probe->last.category == "timer" &&
              probe->last.title == "Tea is ready" &&
              probe->last.body == "Five minutes");

  probe->result = false;
  OMARCHY_CHECK(!channel::DesktopNotificationService::send(
              "org.example.clock", "timer", "Tea is ready", "Five minutes",
              &service) &&
              probe->calls == 2);
  OMARCHY_CHECK(!channel::DesktopNotificationService::send("org.example.clock",
                                                     "timer", "Tea is ready",
                                                     "Five minutes", nullptr));
  channel::DesktopNotificationService empty({});
  OMARCHY_CHECK(!channel::DesktopNotificationService::send("", "", "", "", &empty));
  channel::DesktopNotificationService throwing([](const auto &) -> bool {
    throw std::runtime_error("transport failure");
  });
  OMARCHY_CHECK(!channel::DesktopNotificationService::send("", "", "", "", &throwing));
}

void dbus_message_has_no_plugin_control_surface() {
  const auto message = channel::desktop_notification_message(
      {.plugin = QStringLiteral("org.example.clock"),
       .category = QStringLiteral("timer"),
       .title = QStringLiteral("Tea is ready"),
       .body = QStringLiteral("Five minutes")});
  OMARCHY_CHECK(message.service() == "org.freedesktop.Notifications" &&
              message.path() == "/org/freedesktop/Notifications" &&
              message.interface() == "org.freedesktop.Notifications" &&
              message.member() == "Notify");
  const auto arguments = message.arguments();
  OMARCHY_CHECK(arguments.size() == 8);
  OMARCHY_CHECK(arguments[0].toString() ==
                  QStringLiteral("Omarchy Plugin · org.example.clock") &&
              arguments[1].metaType() == QMetaType::fromType<uint>() &&
              arguments[1].toUInt() == 0 && arguments[2].toString().isEmpty() &&
              arguments[3].toString() == "Tea is ready" &&
              arguments[4].toString() == "Five minutes" &&
              arguments[5].toStringList().isEmpty() &&
              arguments[7].toInt() == 5000);
  const auto hints = arguments[6].toMap();
  OMARCHY_CHECK(hints.size() == 2 && hints.value("category").toString() == "timer" &&
              hints.value("urgency").metaType() ==
                  QMetaType::fromType<uchar>() &&
              hints.value("urgency").value<uchar>() == 0);
}

void dbus_reply_requires_only_the_specified_type() {
  const auto call = QDBusMessage::createMethodCall(
      QStringLiteral("org.freedesktop.Notifications"),
      QStringLiteral("/org/freedesktop/Notifications"),
      QStringLiteral("org.freedesktop.Notifications"),
      QStringLiteral("Notify"));
  OMARCHY_CHECK(channel::desktop_notification_reply_accepted(
              call.createReply(QVariant::fromValue(uint{0}))) &&
              channel::desktop_notification_reply_accepted(
                  call.createReply(QVariant::fromValue(uint{42}))));
  OMARCHY_CHECK(!channel::desktop_notification_reply_accepted(
              call.createReply(QVariant::fromValue(int{42}))) &&
              !channel::desktop_notification_reply_accepted(
                  call.createReply(QVariantList{})) &&
              !channel::desktop_notification_reply_accepted(
                  call.createErrorReply(QDBusError::Failed,
                                        QStringLiteral("failed"))));
}

void busy_shared_transport_fails_closed_promptly() {
  auto transport = std::make_unique<HoldingProbe>();
  auto *probe = transport.get();
  channel::DesktopNotificationService service(
      [owned = std::shared_ptr<HoldingProbe>(std::move(transport))](const auto &) noexcept {
        owned->gate.arrive_and_wait([&] { ++owned->calls; });
        return true;
      });
  std::atomic<bool> first_succeeded = false;
  std::thread first([&] {
    first_succeeded = channel::DesktopNotificationService::send(
        "org.example.clock", "timer", "Tea is ready", "Five minutes",
        &service);
  });
  const bool entered = probe->gate.wait_entered_for(std::chrono::seconds(1));
  if (!entered) {
    probe->gate.release();
    first.join();
    OMARCHY_CHECK(false);
  }
  const auto started = std::chrono::steady_clock::now();
  const bool second = channel::DesktopNotificationService::send(
      "org.example.clock", "timer", "Second", "Must not wait", &service);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  probe->gate.release();
  first.join();
  OMARCHY_CHECK(!second && elapsed < std::chrono::milliseconds(100) &&
              first_succeeded && probe->calls == 1);
}

void runtime_services_enable_only_implemented_adapters() {
  const auto services = channel::make_runtime_services();
  OMARCHY_CHECK(services && services->context && services->notification_send &&
              !services->provider_catalog);
  const auto registry = omarchy::plugin_runtime::test_support::packaged_registry();
  for (const auto name : {"storage.private", "notifications.send", "network.fetch"}) {
    const auto resolved = registry.find(name);
    OMARCHY_CHECK(resolved.has_value());
    const bool available = channel::runtime_service_available(registry, *services,
        {.canonical_name=omarchy::plugins::definitions::Name(name),
         .definition_generation=resolved->generation,.definition_digest=resolved->digest});
    OMARCHY_CHECK(available == (std::string_view(name) != "network.fetch"));
  }
  OMARCHY_CHECK(!channel::runtime_service_available(registry, *services,
      {.canonical_name=omarchy::plugins::definitions::Name("unknown.effect"),
       .definition_generation=1,.definition_digest=omarchy::plugin_runtime::test_support::digest('a')}));
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  return omarchy::plugin_runtime::test_support::test_main([&] {
    callback_forwards_only_bounded_fields();
    dbus_message_has_no_plugin_control_surface();
    dbus_reply_requires_only_the_specified_type();
    busy_shared_transport_fails_closed_promptly();
    runtime_services_enable_only_implemented_adapters();
    return 0;
  }, "desktop notification service test failed: ");
}
