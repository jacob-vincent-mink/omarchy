#include "../../tests/support/gesture_clock.hpp"
#include "../../tests/support/authenticated_broker_fixture.hpp"
#include "../../tests/support/echo_fixture.hpp"
#include "../../tests/support/permission_fixture.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include "../../tests/support/broker_fixture.hpp"
#include "../../tests/support/temporary_directory.hpp"

#include "session_runtime_factory.hpp"

#include "omarchy/plugin_runtime/broker/broker_schema.hpp"
#include "structured_broker.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include "../../tests/support/blocking_gate.hpp"
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace channel = omarchy::plugin_runtime::channel;
namespace definitions = omarchy::plugins::definitions;
namespace host = omarchy::plugin_runtime::host_session;
namespace manifest = omarchy::plugins::manifest;
namespace permissions = omarchy::plugins::permissions;
namespace policy = omarchy::plugin_runtime::policy;
namespace runtime = omarchy::plugin_runtime::runtime;
namespace wire = omarchy::plugin::wire;
namespace broker = omarchy::plugin_runtime::broker;
namespace test_support = omarchy::plugin_runtime::test_support;

namespace {

using omarchy::plugin_runtime::test_support::require;

using test_support::admit_invocation;
using test_support::extract_admission;

permissions::Digest digest(char value) {
  return permissions::Digest(std::string(64, value));
}

struct Directories final {
  omarchy::plugin_runtime::test_support::TemporaryDirectory directory;
  const std::filesystem::path root = directory.path();
  omarchy::plugin_runtime::UniqueFd state;
  omarchy::plugin_runtime::UniqueFd revision;

  Directories() {
    OMARCHY_CHECK(::mkdir((root / "revision").c_str(), 0700) == 0 &&
                ::mkdir((root / "state").c_str(), 0700) == 0);
    revision.reset(::open((root / "revision").c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    state.reset(::open((root / "state").c_str(),
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    OMARCHY_CHECK(revision && state);
  }

};

using omarchy::plugin_runtime::test_support::make_gesture_latch;

std::shared_ptr<const omarchy::plugin_runtime::provider_host::ProviderCatalog>
empty_provider_catalog(const std::filesystem::path &root) {
  OMARCHY_CHECK(::mkdir((root / "provider-package").c_str(), 0700) == 0 &&
              ::mkdir((root / "provider-admin").c_str(), 0700) == 0);
  const int root_fd =
      ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  OMARCHY_CHECK(root_fd >= 0);
  const std::array<std::string_view, 1> package{"provider-package"};
  const std::array<std::string_view, 1> admin{"provider-admin"};
  omarchy::plugin_runtime::provider_host::CatalogError error{};
  auto catalog =
      omarchy::plugin_runtime::provider_host::ProviderCatalog::load(
          root_fd, package, admin, static_cast<std::uint32_t>(::getuid()), error);
  ::close(root_fd);
  OMARCHY_CHECK(catalog && error ==
                         omarchy::plugin_runtime::provider_host::CatalogError::none);
  return catalog;
}

policy::GrantSnapshot local_snapshot(std::string_view capability, std::string scope) {
  auto registry = test_support::packaged_registry();
  manifest::ManifestV2 manifest;
  manifest.id = "fixture.plugin";
  manifest.requests.push_back(test_support::capability_request(registry, capability, std::move(scope)));
  return test_support::permission_snapshot(registry, manifest, std::string(64, 'a'), 7);
}
policy::GrantSnapshot notification_snapshot() {
  return local_snapshot("notifications.send", "{\"categories\":[\"complete\"]}");
}
policy::GrantSnapshot storage_snapshot(std::uint64_t total, std::uint64_t item) {
  return local_snapshot("storage.private", "{\"itemBytes\":" + std::to_string(item) +
                                          ",\"quotaBytes\":" + std::to_string(total) + "}");
}
std::vector<std::byte> storage_write_request(std::string_view key,
                                            std::span<const std::byte> value) {
  const auto registry = test_support::packaged_registry();
  const auto definition = registry.find("storage.private");
  const auto payload = QJsonDocument(QJsonObject{
      {"key", QString::fromUtf8(key.data(), key.size())},
      {"value", QString::fromUtf8(reinterpret_cast<const char *>(value.data()), value.size())}})
      .toJson(QJsonDocument::Compact);
  return test_support::ungestured_invocation(
      {.canonical_name = definitions::Name("storage.private"),
       .definition_generation = 1, .definition_digest = definition->digest},
      "write", std::as_bytes(std::span(payload)));
}

struct ServiceProbe final {
  std::size_t notification_calls = 0;
  std::size_t dynamic_calls = 0;
};

struct BlockingProbe final {
  test_support::BlockingGate gate;
  std::size_t calls = 0;
};

bool blocking_notification(std::string_view, std::string_view, std::string_view, std::string_view, void *context) noexcept {
  auto &probe = *static_cast<BlockingProbe *>(context);
  probe.gate.arrive_and_wait([&] { ++probe.calls; });
  return true;
}

bool play_notification(std::string_view, std::string_view cue, std::string_view, std::string_view, void *context) noexcept {
  auto &probe = *static_cast<ServiceProbe *>(context);
  ++probe.notification_calls;
  return cue == "complete";
}

bool dynamic_dispatch(const definitions::AuthorizedDynamicRequest &,
                      std::span<std::byte>, std::size_t &written,
                      void *context) noexcept {
  ++static_cast<ServiceProbe *>(context)->dynamic_calls;
  written = 0;
  return true;
}


manifest::ManifestV2
dynamic_manifest(const definitions::ResolvedDefinition &resolved,
                 bool required = false) {
  return test_support::echo_manifest(
      resolved, "fixture.plugin", "exercise an exact trusted provider", required);
}

policy::GrantSnapshot dynamic_snapshot(
    const definitions::ResolvedDefinition &resolved,
    permissions::GrantState state = permissions::GrantState::granted,
    bool required = false) {
  const auto manifest_value = dynamic_manifest(resolved, required);
  policy::GrantSnapshot snapshot;
  snapshot.binding = {
      .plugin = permissions::PluginId("fixture.plugin"),
      .revision = digest('a'),
      .policy_fingerprint = permissions::Digest(
          manifest::requested_capability_fingerprint(manifest_value.requests)),
      .generation = 7};
  snapshot.dynamic_grants.push_back(test_support::echo_grant(
      resolved, snapshot.binding, required, state, 7));
  return snapshot;
}


void provider_completeness_and_effect_fence() {
  Directories directories;
  auto definitions = test_support::packaged_registry();
  auto grants = notification_snapshot();
  auto live = std::make_shared<host::LiveGenerationState>(grants.binding);
  channel::SessionRuntimeFactory missing(definitions, {});
  OMARCHY_CHECK(!missing.create(test_support::manifest_for(grants), grants, directories.revision.get(),
                          directories.state.get(), 41, live, make_gesture_latch<1>()));

  auto probe = std::make_shared<ServiceProbe>();
  std::weak_ptr<ServiceProbe> retained = probe;
  channel::RuntimeServices services{
      .context = probe,
      .notification_send = play_notification};
  auto factory = std::make_unique<channel::SessionRuntimeFactory>(
      definitions, services);
  auto product = factory->create(test_support::manifest_for(grants), grants,
                                 directories.revision.get(), directories.state.get(), 41,
                                 live, make_gesture_latch<1>());
  OMARCHY_CHECK(product && product->broker().accepts(grants.binding, 41));
  probe.reset();
  services.context.reset();
  factory.reset();
  OMARCHY_CHECK(!retained.expired());

  auto admission = extract_admission(product->broker());
  const auto payload = omarchy::plugin_runtime::test_support::notification_request();
  auto first = admit_invocation(admission, 1, payload);
  std::array<std::byte, 64> response{};
  auto allowed = product->broker().dispatch(std::move(*first.request),
                                            response);
  OMARCHY_CHECK(allowed.state() == host::TransactionState::reply &&
              retained.lock()->notification_calls == 1);
  OMARCHY_CHECK(product->broker().commit_sent(std::move(allowed)));
  (void)live->revoke_and_drain();
  auto second = admit_invocation(admission, 2, payload);
  auto fenced = product->broker().dispatch(std::move(*second.request),
                                           response);
  OMARCHY_CHECK(fenced.state() == host::TransactionState::fatal &&
              retained.lock()->notification_calls == 1);
  product.reset();
  OMARCHY_CHECK(retained.expired());
}

void descriptor_quota_and_dynamic_catalog_validation() {
  Directories directories;
  auto empty = test_support::packaged_registry();
  auto storage = storage_snapshot(4096, 1024);
  auto live = std::make_shared<host::LiveGenerationState>(storage.binding);
  channel::SessionRuntimeFactory constrained(
      empty, {}, {.maximum_audit_records = 8,
                  .maximum_storage_bytes = 2048,
                  .maximum_storage_item_bytes = 1024});
  OMARCHY_CHECK(!constrained.create(test_support::manifest_for(storage), storage, directories.revision.get(),
                              directories.state.get(), 42, live, make_gesture_latch<1>()));
  channel::SessionRuntimeFactory exact(
      empty, {}, {.maximum_audit_records = 8,
                  .maximum_storage_bytes = 4096,
                  .maximum_storage_item_bytes = 1024});
  OMARCHY_CHECK(!exact.create(test_support::manifest_for(storage), storage, directories.revision.get(), -1,
                        42, live, make_gesture_latch<1>()));
  auto storage_product = exact.create(test_support::manifest_for(storage), storage,
                                      directories.revision.get(), directories.state.get(),
                                      42, live, make_gesture_latch<1>());
  OMARCHY_CHECK(storage_product != nullptr);
  auto storage_admission = extract_admission(storage_product->broker());
  const std::array value{std::byte{'a'}, std::byte{'b'}};
  const auto storage_payload =
      storage_write_request("proof", value);
  auto storage_request = admit_invocation(storage_admission, 1, storage_payload);
  std::array<std::byte, 64> storage_response{};
  auto storage_result = storage_product->broker().dispatch(
      std::move(*storage_request.request), storage_response);
  OMARCHY_CHECK(storage_result.state() == host::TransactionState::reply &&
              storage_product->broker().commit_sent(
                  std::move(storage_result)));
  const int proof = ::openat(directories.state.get(), "proof",
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  std::array<std::byte, 2> stored{};
  OMARCHY_CHECK(proof >= 0 && ::read(proof, stored.data(), stored.size()) == 2 &&
              stored == value);
  if (proof >= 0)
    ::close(proof);

  definitions::TrustedDefinitionRegistry mutable_definitions;
  const auto definition = test_support::echo_definition("Uses trusted echo service");
  OMARCHY_CHECK(mutable_definitions.install(
              definition, 3));
  const auto resolved = mutable_definitions.find("service.echo");
  OMARCHY_CHECK(resolved.has_value());
  auto dynamic = dynamic_snapshot(*resolved);
  const auto dynamic_plugin = dynamic_manifest(*resolved);
  auto dynamic_live =
      std::make_shared<host::LiveGenerationState>(dynamic.binding);
  auto probe = std::make_shared<ServiceProbe>();
  channel::RuntimeServices missing_route{
      .context = probe,
      .notification_send = nullptr,
      .dynamic_services = {},
      .provider_catalog = empty_provider_catalog(directories.root)};
  channel::SessionRuntimeFactory missing_dynamic(
      mutable_definitions, missing_route);
  auto missing_product = missing_dynamic.create(
      dynamic_plugin, dynamic, directories.revision.get(), directories.state.get(), 43,
      dynamic_live, make_gesture_latch<1>());
  const auto missing_projection =
      missing_dynamic.project_permissions(dynamic_plugin, dynamic);
  OMARCHY_CHECK(missing_product && missing_projection &&
              missing_projection->permissions.size() == 1 &&
              missing_projection->permissions[0].state ==
                  wire::permission_snapshot::GrantState::granted &&
              missing_projection->permissions[0].operation_mask == 0);
  auto required_dynamic =
      dynamic_snapshot(*resolved, permissions::GrantState::granted, true);
  const auto required_plugin = dynamic_manifest(*resolved, true);
  auto required_live =
      std::make_shared<host::LiveGenerationState>(required_dynamic.binding);
  OMARCHY_CHECK(!missing_dynamic.create(required_plugin, required_dynamic,
                                  directories.revision.get(), directories.state.get(), 44,
                                  required_live, make_gesture_latch<1>()) &&
              !missing_dynamic.project_permissions(required_plugin,
                                                   required_dynamic));
  missing_route.provider_catalog.reset();
  auto wrong = definition.adapter;
  wrong.contract_digest = digest('e');
  missing_route.dynamic_services.push_back(
      {.binding = wrong, .dispatch = dynamic_dispatch});
  channel::SessionRuntimeFactory wrong_dynamic(
      mutable_definitions, missing_route);
  OMARCHY_CHECK(wrong_dynamic.create(dynamic_plugin, dynamic, directories.revision.get(),
                               directories.state.get(), 43, dynamic_live,
                               make_gesture_latch<1>()) != nullptr);
  missing_route.dynamic_services[0].binding = definition.adapter;
  missing_route.dynamic_services.push_back(
      {.binding = definition.adapter, .dispatch = dynamic_dispatch});
  channel::SessionRuntimeFactory duplicate_dynamic(
      mutable_definitions, missing_route);
  OMARCHY_CHECK(duplicate_dynamic.create(dynamic_plugin, dynamic,
                                   directories.revision.get(), directories.state.get(), 43,
                                   dynamic_live, make_gesture_latch<1>()) != nullptr);
  missing_route.dynamic_services.pop_back();
  missing_route.dynamic_services[0].binding.abi_version = 2;
  channel::SessionRuntimeFactory wrong_abi_dynamic(
      mutable_definitions, missing_route);
  OMARCHY_CHECK(wrong_abi_dynamic.create(dynamic_plugin, dynamic,
                                   directories.revision.get(), directories.state.get(), 43,
                                   dynamic_live, make_gesture_latch<1>()) != nullptr);
  missing_route.dynamic_services[0].binding = definition.adapter;
  channel::SessionRuntimeFactory exact_dynamic(
      mutable_definitions, missing_route);
  const auto restored_projection =
      exact_dynamic.project_permissions(dynamic_plugin, dynamic);
  OMARCHY_CHECK(restored_projection && restored_projection->permissions.size() == 1 &&
              restored_projection->permissions[0] ==
                  wire::permission_snapshot::PermissionRow{
                      wire::permission_snapshot::GrantState::granted, 0x0001});
  auto changed_definition = test_support::echo_definition("Uses trusted echo service");
  changed_definition.canonical_name = definitions::Name("service.other");
  changed_definition.authority_identity =
      definitions::Name("service.other.authority");
  changed_definition.adapter.adapter_class =
      definitions::Name("service.other.adapter");
  OMARCHY_CHECK(mutable_definitions.install(
              changed_definition,
              4));
  OMARCHY_CHECK(mutable_definitions.find("service.other").has_value() &&
              !exact_dynamic.definitions().find("service.other") &&
              exact_dynamic.definitions().size() == 1);
  auto dynamic_product = exact_dynamic.create(
      dynamic_plugin, dynamic, directories.revision.get(), directories.state.get(), 43,
      dynamic_live, make_gesture_latch<1>());
  OMARCHY_CHECK(dynamic_product != nullptr);
  auto dynamic_admission = extract_admission(dynamic_product->broker());
  const auto dynamic_payload = test_support::ungestured_invocation(
      dynamic.dynamic_grants[0].request.definition, "echo");
  auto missing_admission = extract_admission(missing_product->broker());
  auto missing_request = admit_invocation(missing_admission, 9, dynamic_payload);
  std::array<std::byte, 64> missing_response{};
  auto missing_result = missing_product->broker().dispatch(
      std::move(*missing_request.request), missing_response);
  // The trusted snapshot gives well-behaved QML a zero operation mask. If a
  // compromised worker bypasses that local gate, the broker returns a typed
  // denial for the absent route and never reaches a provider callback. This
  // is nonfatal so a transient provider outage cannot kill unrelated plugin
  // features.
  OMARCHY_CHECK(missing_result.state() == host::TransactionState::reply &&
              missing_result.reply_kind() == host::ReplyKind::denied &&
              probe->dynamic_calls == 0 &&
              missing_product->broker().commit_sent(std::move(missing_result)));
  auto dynamic_request = admit_invocation(dynamic_admission, 1, dynamic_payload);
  std::array<std::byte, 64> dynamic_response{};
  auto dynamic_result = dynamic_product->broker().dispatch(
      std::move(*dynamic_request.request), dynamic_response);
  OMARCHY_CHECK(dynamic_result.state() == host::TransactionState::reply &&
              probe->dynamic_calls == 1 &&
              dynamic_product->broker().commit_sent(
                  std::move(dynamic_result)));

  auto denied = dynamic_snapshot(*resolved, permissions::GrantState::denied);
  auto denied_live =
      std::make_shared<host::LiveGenerationState>(denied.binding);
  channel::SessionRuntimeFactory denied_dynamic(
      mutable_definitions, {});
  OMARCHY_CHECK(denied_dynamic.create(dynamic_plugin, denied,
                                directories.revision.get(), directories.state.get(), 44,
                                denied_live, make_gesture_latch<1>()) != nullptr);

  channel::RuntimeServices provider_appeared{
      .context = probe,
      .notification_send = nullptr,
      .dynamic_services = {
          {.binding = definition.adapter, .dispatch = dynamic_dispatch}},
      .provider_catalog = {}};
  channel::SessionRuntimeFactory still_denied(
      mutable_definitions, provider_appeared);
  const auto denied_projection =
      still_denied.project_permissions(dynamic_plugin, denied);
  auto degraded = still_denied.create(
      dynamic_plugin, denied, directories.revision.get(), directories.state.get(), 45,
      denied_live, make_gesture_latch<1>());
  OMARCHY_CHECK(degraded && denied_projection &&
              denied_projection->permissions.size() == 1 &&
              denied_projection->permissions[0] ==
                  wire::permission_snapshot::PermissionRow{
                      wire::permission_snapshot::GrantState::denied, 0x0000});
  auto degraded_admission = extract_admission(degraded->broker());
  auto denied_request = admit_invocation(degraded_admission, 2, dynamic_payload);
  auto denied_result = degraded->broker().dispatch(
      std::move(*denied_request.request), dynamic_response);
  OMARCHY_CHECK(denied_result.state() == host::TransactionState::reply &&
              denied_result.reply_kind() == host::ReplyKind::denied &&
              probe->dynamic_calls == 1 &&
              degraded->broker().commit_sent(std::move(denied_result)));
}

void synchronous_effects_drain_before_revocation_acknowledges() {
  using namespace std::chrono_literals;
  Directories directories;
  auto definitions = test_support::packaged_registry();
  auto grants = notification_snapshot();

  auto reentrant =
      std::make_shared<host::LiveGenerationState>(grants.binding);
  auto token = reentrant->acquire_effect(grants.binding);
  OMARCHY_CHECK(token &&
              reentrant->revoke_and_drain() ==
                  host::LiveGenerationRevokeResult::reentrant &&
              !reentrant->current(grants.binding));
  auto unrelated =
      std::make_shared<host::LiveGenerationState>(grants.binding);
  OMARCHY_CHECK(unrelated->revoke_and_drain() ==
              host::LiveGenerationRevokeResult::drained);
  token.reset();

  auto transferred =
      std::make_shared<host::LiveGenerationState>(grants.binding);
  auto acquired = transferred->acquire_effect(grants.binding);
  OMARCHY_CHECK(acquired.has_value());
  bool transferred_current = false;
  host::LiveGenerationRevokeResult transferred_revoke{};
  std::thread destination(
      [effect = std::move(*acquired), transferred, &transferred_current,
       &transferred_revoke]() mutable {
        transferred_current = effect.current();
        transferred_revoke = transferred->revoke_and_drain();
      });
  destination.join();
  OMARCHY_CHECK(transferred_current &&
              transferred_revoke ==
                  host::LiveGenerationRevokeResult::reentrant);

  auto live = std::make_shared<host::LiveGenerationState>(grants.binding);
  auto probe = std::make_shared<BlockingProbe>();
  channel::SessionRuntimeFactory factory(
      definitions,
      {.context = probe,
       .notification_send = blocking_notification});
  auto product = factory.create(test_support::manifest_for(grants), grants, directories.revision.get(),
                                directories.state.get(), 45, live, make_gesture_latch<1>());
  OMARCHY_CHECK(product != nullptr);
  auto admission = extract_admission(product->broker());
  const auto payload = omarchy::plugin_runtime::test_support::notification_request();
  auto admitted = admit_invocation(admission, 1, payload);

  std::array<std::byte, 64> response{};
  std::atomic<host::TransactionState> transaction_state{
      host::TransactionState::fatal};
  std::thread effect([&] {
    auto transaction = product->broker().dispatch(std::move(*admitted.request), response);
    transaction_state.store(transaction.state(), std::memory_order_release);
    if (transaction.state() == host::TransactionState::reply)
      (void)product->broker().commit_sent(std::move(transaction));
  });
  OMARCHY_CHECK(probe->gate.wait_entered_for(2s));
  std::atomic<bool> revoked = false;
  host::LiveGenerationRevokeResult revoke_result{};
  std::thread revoker([&] {
    revoke_result = live->revoke_and_drain();
    revoked.store(true, std::memory_order_release);
  });
  for (int attempt = 0; attempt < 200 && live->generation() != 0; ++attempt)
    std::this_thread::sleep_for(1ms);
  OMARCHY_CHECK(live->generation() == 0 &&
              !revoked.load(std::memory_order_acquire));
  probe->gate.release();
  effect.join();
  revoker.join();
  OMARCHY_CHECK(revoke_result == host::LiveGenerationRevokeResult::drained &&
              transaction_state.load(std::memory_order_acquire) ==
                  host::TransactionState::reply &&
              probe->calls == 1);

  auto denied = admit_invocation(admission, 2, payload);
  auto fenced = product->broker().dispatch(std::move(*denied.request),
                                           response);
  OMARCHY_CHECK(fenced.state() == host::TransactionState::fatal &&
              probe->calls == 1);
}

} // namespace

void session_runtime_factory_tests() {
  const channel::RuntimeServices defaults;
  OMARCHY_CHECK(!defaults.context && !defaults.notification_send &&
              defaults.dynamic_services.empty() && !defaults.provider_catalog);
  provider_completeness_and_effect_fence();
  descriptor_quota_and_dynamic_catalog_validation();
  synchronous_effects_drain_before_revocation_acknowledges();
}
