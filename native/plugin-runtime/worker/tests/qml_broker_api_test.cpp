#include "../../tests/support/test_assert.hpp"
#include "omarchy/plugin_runtime/test_support/test_support.h"

#include "qml_broker_api.hpp"
#include "worker_runtime.hpp"

#include "omarchy/plugin/wire/common.hpp"
#include "../../tests/support/authenticated_broker_fixture.hpp"
#include "../../tests/support/capability_fixture.hpp"
#include "omarchy/plugin_runtime/broker/broker_schema.hpp"

#include <fcntl.h>
#include <QGuiApplication>
#include <QEventLoop>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QTemporaryDir>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace broker = omarchy::plugin_runtime::broker;
namespace definitions = omarchy::plugins::definitions;
namespace manifest = omarchy::plugins::manifest;
namespace permissions = omarchy::plugins::permissions;
namespace surface = omarchy::plugin_runtime::surface;
namespace worker = omarchy::plugin_runtime::worker;
namespace wire = omarchy::plugin::wire;

static_assert(std::is_final_v<worker::QmlBrokerApi>);
static_assert(!std::is_invocable_v<decltype(&worker::WorkerRuntime::bind_runtime_api),
                                   worker::WorkerRuntime &, QObject &>);

class IntentSink final : public worker::SurfaceIntentSink {
public:
  bool request_surface_intent(
      std::optional<definitions::DynamicInvocation::GestureClaim> source,
      std::string_view target,
      surface::SurfaceIntentAction action, const QVariantMap &data) override {
    ++calls;
    last_source = source;
    last_target = target;
    last_action = action;
    last_data = data;
    return accept && target == declared_target;
  }

  int calls = 0;
  bool accept = true;
  std::string declared_target = "PanelWidget";
  std::optional<definitions::DynamicInvocation::GestureClaim> last_source;
  std::string last_target;
  surface::SurfaceIntentAction last_action = surface::SurfaceIntentAction::open;
  QVariantMap last_data;
};

class FixtureIntentSink final : public worker::SurfaceIntentSink {
public:
  bool request_surface_intent(
      std::optional<definitions::DynamicInvocation::GestureClaim> source,
      std::string_view target,
      surface::SurfaceIntentAction action, const QVariantMap &) override {
    sources.push_back(source);
    targets.emplace_back(target);
    actions.push_back(action);
    return (target == "panel" || target == "overlay") &&
           action == surface::SurfaceIntentAction::toggle;
  }

  std::vector<std::optional<definitions::DynamicInvocation::GestureClaim>>
      sources;
  std::vector<std::string> targets;
  std::vector<surface::SurfaceIntentAction> actions;
};

using omarchy::plugin_runtime::test_support::require;

std::vector<std::byte> granted_snapshot(const manifest::ManifestV2 &manifest,
                                       const std::vector<std::uint16_t> &masks) {
  std::vector<wire::permission_snapshot::PermissionRow> rows;
  for (const auto mask : masks)
    rows.push_back({wire::permission_snapshot::GrantState::granted, mask});
  return wire::permission_snapshot::encode({
      .manifest_request_fingerprint =
          manifest::requested_capability_fingerprint(manifest.requests),
      .permissions = std::move(rows)});
}

void drain_events() {
  for (int pass = 0; pass < 3; ++pass)
    QCoreApplication::processEvents();
}
using omarchy::plugin_runtime::test_support::SeqpacketPair;
bool send_packet(int fd, wire::EnvelopeHeader header,
                 std::span<const std::byte> payload) {
  header.payload_length = static_cast<std::uint32_t>(payload.size());
  std::vector<std::byte> packet(wire::kHeaderSize + payload.size());
  const auto encoded = wire::encode_packet(header, payload, packet);
  return encoded && send(fd, packet.data(), encoded.bytes_written, MSG_NOSIGNAL) ==
                        static_cast<ssize_t>(encoded.bytes_written);
}
void handshake(worker::WorkerEndpoint &endpoint, int host) {
  if (!endpoint.valid() || !endpoint.send_hello())
    throw std::runtime_error("HELLO failed: " + endpoint.last_error());
  std::array<std::byte, wire::kHeaderSize + 4> hello{};
  OMARCHY_CHECK(recv(host, hello.data(), hello.size(), 0) ==
              static_cast<ssize_t>(hello.size()));
  const auto welcome = wire::encode_welcome_payload(
      {.maximum_payload = wire::payload_cap(wire::EndpointRole::broker),
       .maximum_in_flight = 32});
  OMARCHY_CHECK(send_packet(host,
      {.endpoint_role = wire::EndpointRole::broker,
       .message_type = static_cast<std::uint16_t>(wire::CommonMessageType::welcome),
       .role_protocol_version = broker::kBrokerRoleVersion,
       .launch_generation = 77}, welcome));
  auto packet = endpoint.receive();
  OMARCHY_CHECK(packet && endpoint.accept_welcome(packet));
}
std::vector<std::byte> receive_packet(int fd) {
  std::vector<std::byte> packet(wire::kHeaderSize +
      wire::payload_cap(wire::EndpointRole::broker));
  const auto bytes = recv(fd, packet.data(), packet.size(), 0);
  OMARCHY_CHECK(bytes > 0);
  packet.resize(static_cast<std::size_t>(bytes));
  return packet;
}
void finish(worker::QmlBrokerApi &api, worker::WorkerEndpoint &endpoint,
            int host, wire::SessionSequence &host_sequence,
            std::uint64_t correlation, std::uint16_t type,
            std::span<const std::byte> payload) {
  const auto sequence =
      host_sequence.take_outbound(wire::EndpointRole::broker);
  OMARCHY_CHECK(static_cast<bool>(sequence));
  OMARCHY_CHECK(send_packet(host,
      {.endpoint_role = wire::EndpointRole::broker,
       .message_type = type,
       .role_protocol_version = broker::kBrokerRoleVersion,
       .launch_generation = 77,
       .correlation_id = correlation,
       .lane_sequence = sequence.value}, payload));
  OMARCHY_CHECK(api.receive(endpoint.receive()));
}
definitions::Digest repeated(char value) {
  return definitions::Digest(std::string(64, value));
}
bool fake_dynamic_dispatch(const definitions::AuthorizedDynamicRequest &request,
                           std::span<std::byte> response,
                           std::size_t &written, void *context) noexcept {
  auto &calls = *static_cast<int *>(context);
  if (request.operation != "read" ||
      request.demand_scope != "{\"dataset\":\"status\"}" ||
      request.payload.empty() || response.empty() ||
      request.authorization.binding.plugin.view() != "org.example.dynamic" ||
      request.authorization.definition.canonical_name.view() !=
          "service.status" ||
      request.authorization.grant_epoch != 1)
    return false;
  ++calls;
  response[0] = std::byte{0x2a};
  written = 1;
  return true;
}

void capability_qualified_collision() {
  const std::string alpha_digest(64, 'a');
  const std::string beta_digest(64, 'b');
  const std::string document =
      "{\"schemaVersion\":2,\"id\":\"org.example.collision\","
      "\"name\":\"Collision\",\"version\":\"1\",\"runtime\":{"
      "\"apiVersion\":1,\"qml\":\"Main.qml\"},\"surfaces\":{},"
      "\"permissions\":{\"required\":[{\"capability\":\"service.alpha\","
      "\"definitionGeneration\":3,\"definitionDigest\":\"" + alpha_digest +
      "\",\"operations\":[\"read\",\"control\"],\"reason\":\"alpha\"},{"
      "\"capability\":\"service.beta\",\"definitionGeneration\":7,"
      "\"definitionDigest\":\"" + beta_digest +
      "\",\"operations\":[\"read\",\"control\"],\"reason\":\"beta\"}],"
      "\"optional\":[]}}";
  const auto parsed = manifest::parse_manifest_v2(document);
  worker::ManifestInvokeEncoder encoder(parsed);
  const QVariantMap arguments{{QStringLiteral("resource"), 7}};

  const auto alpha_read = encoder.encode("service.alpha", "read", arguments);
  const auto beta_read = encoder.encode("service.beta", "read", arguments);
  const auto beta_control =
      encoder.encode("service.beta", "control", arguments);
  definitions::DynamicInvocation alpha_invocation;
  definitions::DynamicInvocation beta_read_invocation;
  definitions::DynamicInvocation beta_control_invocation;
  OMARCHY_CHECK(alpha_read && beta_read && beta_control &&
              definitions::decode_dynamic_invocation(*alpha_read,
                                                     alpha_invocation) &&
              definitions::decode_dynamic_invocation(*beta_read,
                                                     beta_read_invocation) &&
              definitions::decode_dynamic_invocation(*beta_control,
                                                     beta_control_invocation) &&
              alpha_invocation.definition.canonical_name.view() ==
                  "service.alpha" &&
              alpha_invocation.definition.definition_generation == 3 &&
              alpha_invocation.definition.definition_digest.view() ==
                  alpha_digest &&
              alpha_invocation.operation.view() == "read" &&
              beta_read_invocation.definition.canonical_name.view() ==
                  "service.beta" &&
              beta_read_invocation.definition.definition_generation == 7 &&
              beta_read_invocation.definition.definition_digest.view() ==
                  beta_digest &&
              beta_read_invocation.operation.view() == "read" &&
              beta_control_invocation.definition ==
                  beta_read_invocation.definition &&
              beta_control_invocation.operation.view() == "control");
  OMARCHY_CHECK(!encoder.encode("service.old-alpha", "read", arguments) &&
              !encoder.encode("service.alpha", "delete", arguments) &&
              !encoder.encode("storage.private", "read", arguments));

  auto duplicate_capability = parsed;
  duplicate_capability.requests.push_back(parsed.requests.front());
  worker::ManifestInvokeEncoder duplicate_encoder(duplicate_capability);
  OMARCHY_CHECK(!duplicate_encoder.encode("service.alpha", "read", arguments));
  auto duplicate_operation = parsed;
  duplicate_operation.requests.front().operations.push_back("read");
  worker::ManifestInvokeEncoder duplicate_operation_encoder(
      duplicate_operation);
  OMARCHY_CHECK(!duplicate_operation_encoder.encode("service.alpha", "read",
                                               arguments));
  auto malformed_reference = parsed;
  malformed_reference.requests.front().definition_digest =
      std::string(64, 'A');
  worker::ManifestInvokeEncoder malformed_encoder(malformed_reference);
  OMARCHY_CHECK(!malformed_encoder.encode("service.alpha", "read", arguments));

  const std::string overlong_name(129, 'a');
  auto overlong_capability = parsed;
  overlong_capability.requests.front().capability = overlong_name;
  worker::ManifestInvokeEncoder overlong_capability_encoder(
      overlong_capability);
  OMARCHY_CHECK(!overlong_capability_encoder.encode(overlong_name, "read",
                                              arguments) &&
              !overlong_capability_encoder.encode("service.beta", "read",
                                                  arguments));

  auto overlong_operation = parsed;
  overlong_operation.requests.front().operations.front() = overlong_name;
  worker::ManifestInvokeEncoder overlong_operation_encoder(overlong_operation);
  OMARCHY_CHECK(!overlong_operation_encoder.encode("service.alpha", overlong_name,
                                             arguments) &&
              !overlong_operation_encoder.encode("service.beta", "read",
                                                 arguments));
}

void dynamic_qml_to_adapter() {
  definitions::CapabilityDefinition definition{
      .canonical_name = definitions::Name("service.status"),
      .authority_identity = definitions::Name("service.status-v1"),
      .enforcement_family = definitions::EnforcementFamily::network_fetch,
      .display_category_id = definitions::Name("services"),
      .display_category_label = definitions::Label("Services"),
      .title = definitions::Label("Read selected status"),
      .risk_text = definitions::Label("Reads one selected bounded dataset"),
      .risk = definitions::RiskLevel::moderate,
      .revocation = definitions::RevocationPolicy::cancel_inflight,
      .adapter = {.adapter_class = definitions::Name("status-adapter"),
                  .contract_digest = repeated('a'),
                  .abi_version = 1},
      .operations = {}};
  definition.operations.insert(
      {.name = definitions::Name("read"),
       .label = definitions::Label("Read status")});
  definitions::TrustedDefinitionRegistry registry;
  OMARCHY_CHECK(registry.install(definition,
                           3));
  const auto resolved = registry.find("service.status");
  OMARCHY_CHECK(resolved.has_value());
  const std::string document =
      "{\"schemaVersion\":2,\"id\":\"org.example.dynamic\",\"name\":\"Dynamic\","
      "\"version\":\"1\",\"runtime\":{\"apiVersion\":1,\"qml\":\"Main.qml\"},"
      "\"surfaces\":{},\"permissions\":{\"required\":[{\"capability\":"
      "\"service.status\",\"definitionGeneration\":3,\"definitionDigest\":\"" +
      std::string(resolved->digest.view()) +
      "\",\"operations\":[\"read\"],\"dataset\":\"status\",\"reason\":\"test\"}],"
      "\"optional\":[]}}";
  const auto parsed = manifest::parse_manifest_v2(document);
  worker::ManifestInvokeEncoder encoder(parsed);
  const auto encoded = encoder.encode(
      "service.status", "read", {{QStringLiteral("resource"), 7}});
  OMARCHY_CHECK(encoded);

  definitions::DynamicRevisionGrant revision{
      .binding = {.plugin = permissions::PluginId("org.example.dynamic"),
                  .revision = repeated('b'),
                  .policy_fingerprint = repeated('c'),
                  .generation = 5},
      .request = {.definition = {.canonical_name =
                                      definitions::Name("service.status"),
                                  .definition_generation = 3,
                                  .definition_digest = resolved->digest},
                  .operations = {},
                  .scope = definitions::CanonicalScope(
                      "{\"dataset\":\"status\"}"),
                  .required = true},
      .grant = {.operations = {},
                .state = permissions::GrantState::granted,
                .epoch = 1}};
  revision.request.operations.insert(definitions::Name("read"));
  revision.grant.operations.insert(definitions::Name("read"));
  int calls = 0;
  definitions::DynamicAdapter adapter{
      .binding = definition.adapter,
      .dispatch = [&calls](const auto &request, auto response,
                           std::size_t &written) noexcept {
        return fake_dynamic_dispatch(request, response, written, &calls);
      }};
  std::array<std::byte, 8> response{};
  omarchy::plugin_runtime::test_support::AdmittedBrokerFixture fixture(
      revision.binding, registry, {{.grant=revision, .adapter=adapter}});
  auto result = fixture.dispatch(*encoded, response);
  OMARCHY_CHECK(result.state() == omarchy::plugin_runtime::host_session::TransactionState::reply &&
              result.reply_kind() == omarchy::plugin_runtime::host_session::ReplyKind::result &&
              calls == 1 && result.provider_response_bytes() == 1 &&
              fixture.broker.commit_sent(std::move(result)));
}

void structured_command_execution() {
  SeqpacketPair pair = SeqpacketPair::create();
  wire::SessionSequence worker_sequence;
  wire::SessionSequence host_sequence;
  worker::WorkerEndpoint endpoint(pair.worker.get(),
                                  wire::EndpointRole::broker,
                                  broker::kBrokerRoleVersion, worker_sequence);
  handshake(endpoint, pair.trusted.get());
  const std::string digest(64, 'd');
  const auto parsed = manifest::parse_manifest_v2(
      "{\"schemaVersion\":2,\"id\":\"org.example.command\","
      "\"name\":\"Command\",\"version\":\"1\",\"runtime\":{"
      "\"apiVersion\":1,\"qml\":\"Main.qml\"},\"surfaces\":{},"
      "\"permissions\":{\"required\":[{\"capability\":\"bash.execute\","
      "\"definitionGeneration\":1,\"definitionDigest\":\"" + digest +
      "\",\"operations\":[\"run\"],"
      R"("commands":[{"command":"gh","executable":"/usr/bin/gh","timeoutMs":20000,"stdoutBytes":49152,"stderrBytes":4096,"accountHome":true,"environment":{},"rules":[[{"exact":"api"},{"exact":"/notifications"}]]}],)"
      "\"reason\":\"Read GitHub data\"}],\"optional\":[]}}");
  worker::QmlBrokerApi api(
      endpoint, parsed, 77);
  const auto snapshot = granted_snapshot(parsed, {0x0001});
  OMARCHY_CHECK(api.applyPermissionSnapshot(77, snapshot) && api.markBrokerReady());

  auto *generic = qobject_cast<worker::BrokerCall *>(
      api.invoke(QStringLiteral("bash.execute"), QStringLiteral("run"),
                 {{QStringLiteral("command"), QStringLiteral("gh")}})
          .value<QObject *>());
  OMARCHY_CHECK(generic != nullptr && generic->finished() && !generic->ok() &&
              generic->correlation() == 0);

  auto *call = qobject_cast<worker::BrokerCall *>(
      api.execute(QStringLiteral("bash"), QStringLiteral("gh"),
                  {QStringLiteral("api"), QStringLiteral("/notifications")})
          .value<QObject *>());
  OMARCHY_CHECK(call != nullptr && !call->finished() && call->correlation() != 0);
  const auto packet_bytes = receive_packet(pair.trusted.get());
  const auto packet =
      wire::decode_packet(packet_bytes, wire::EndpointRole::broker);
  definitions::DynamicInvocation invocation;
  OMARCHY_CHECK(packet && packet.packet.header.message_type ==
                        broker::kDynamicInvokeMessage &&
              definitions::decode_dynamic_invocation(packet.packet.payload,
                                                     invocation) &&
              invocation.definition.canonical_name.view() == "bash.execute" &&
              invocation.operation.view() == "run" &&
              invocation.definition.definition_digest.view() == digest);
  const QByteArray payload(reinterpret_cast<const char *>(invocation.payload.data()),
                           static_cast<qsizetype>(invocation.payload.size()));
  const auto object = QJsonDocument::fromJson(payload).object();
  OMARCHY_CHECK(object.size() == 2 && object.value(QStringLiteral("command")).toString() ==
                  QStringLiteral("gh") &&
              object.value(QStringLiteral("arguments")).toArray().size() == 2 &&
              object.value(QStringLiteral("arguments")).toArray().at(0).toString() ==
                  QStringLiteral("api"));

  const auto rejected = [&](const QString &runner, const QString &command,
                            const QStringList &arguments) {
    auto *candidate = qobject_cast<worker::BrokerCall *>(
        api.execute(runner, command, arguments).value<QObject *>());
    return candidate != nullptr && candidate->finished() && !candidate->ok() &&
           candidate->correlation() == 0;
  };
  OMARCHY_CHECK(rejected(QStringLiteral("shell"), QStringLiteral("gh"), {}) &&
              rejected(QStringLiteral("bash"), QStringLiteral("../gh"), {}));

  const std::string result =
      R"({"exitCode":0,"stdout":"{}","stderr":""})";
  finish(api, endpoint, pair.trusted.get(), host_sequence,
         call->correlation(), broker::kBrokerResultMessage,
         std::as_bytes(std::span(result)));
  OMARCHY_CHECK(call->finished() && call->ok());
  auto *shell = qobject_cast<worker::BrokerCall *>(
      api.execute(QStringLiteral("bash"), QStringLiteral("sh"),
                  {QStringLiteral("-c"), QStringLiteral("printf approved")}).value<QObject *>());
  OMARCHY_CHECK(shell && !shell->finished() && shell->correlation() != 0);
  const auto shell_bytes = receive_packet(pair.trusted.get());
  const auto shell_packet = wire::decode_packet(shell_bytes, wire::EndpointRole::broker);
  OMARCHY_CHECK(shell_packet && definitions::decode_dynamic_invocation(shell_packet.packet.payload, invocation));
  const auto shell_payload = QJsonDocument::fromJson(QByteArray(
      reinterpret_cast<const char *>(invocation.payload.data()), invocation.payload.size())).object();
  OMARCHY_CHECK(shell_payload.size() == 2 && shell_payload.value("command") == "sh" &&
               !shell_payload.contains("commands"));
  // The worker only forwards argv: the broker/provider retains the decision.
  finish(api, endpoint, pair.trusted.get(), host_sequence, shell->correlation(),
         broker::kBrokerResultMessage, std::as_bytes(std::span(result)));
  OMARCHY_CHECK(shell->finished());
}
void permission_awareness(worker::WorkerEndpoint &endpoint, int host,
                          wire::SessionSequence &host_sequence) {
  const std::string document =
      R"({"schemaVersion":2,"id":"org.example.widget","name":"Widget","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"settings":{"defaults":{"enabled":true,"mode":"compact"},"schema":[{"key":"enabled","type":"boolean","label":"Enabled","defaultValue":true},{"key":"mode","type":"enum","label":"Mode","options":["compact","full"],"defaultValue":"compact"}]},"permissions":{"required":[{"capability":"storage.private","definitionGeneration":1,"definitionDigest":"8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66","operations":["read","remove","write"],"itemBytes":4096,"reason":"save","quotaBytes":1024}],"optional":[{"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"],"reason":"alerts","categories":["status"]}]}})";
  const auto parsed = manifest::parse_manifest_v2(document);
  worker::QmlBrokerApi api(
      endpoint, parsed, 77);
  QTemporaryDir packaged_assets;
  OMARCHY_CHECK(packaged_assets.isValid());
  QFile asset(packaged_assets.filePath("countries.json"));
  OMARCHY_CHECK(asset.open(QIODevice::WriteOnly) &&
              asset.write("{\"country\":\"Japan\"}") == 19);
  asset.close();
  api.setPackagedAssetRoot(packaged_assets.path().toStdString());
  OMARCHY_CHECK(api.readPackagedText("countries.json", 128) ==
              QStringLiteral("{\"country\":\"Japan\"}") &&
              api.readPackagedText("../countries.json", 128).isEmpty() &&
              api.readPackagedText("countries.json", 8).isEmpty() &&
              api.readPackagedText("/etc/passwd", 512 * 1024).isEmpty());
  worker::WorkerRuntime runtime_binding_probe("/not-loaded-in-this-test");
  OMARCHY_CHECK(static_cast<bool>(runtime_binding_probe.bind_runtime_api(api)));
  OMARCHY_CHECK(api.permissionState("storage.private", "read") == "unavailable" &&
              !api.hasPermission("notifications.send", "send"));
  OMARCHY_CHECK(!static_cast<bool>(runtime_binding_probe.bind_runtime_api(api)));
  const auto settings_document =
      manifest::canonical_settings_entry(parsed.settings.defaults);
  const auto settings_bytes = std::as_bytes(std::span(settings_document));
  OMARCHY_CHECK(api.applySettingsSnapshot(77, settings_bytes));
  OMARCHY_CHECK(api.settings().value(QStringLiteral("id")).toString() ==
              QStringLiteral("org.example.widget"));
  OMARCHY_CHECK(api.settings().value(QStringLiteral("enabled")).toBool());
  OMARCHY_CHECK(api.settings().value(QStringLiteral("mode")).toString() ==
              QStringLiteral("compact"));
  OMARCHY_CHECK(!api.applySettingsSnapshot(77, settings_bytes) &&
              !api.applySettingsSnapshot(78, settings_bytes));
  const std::string presentation_document =
      R"({"accent":"#112233","background":"#010203","barBackground":"#040506","barForeground":"#f0f1f2","barPosition":"top","barSize":26,"fontFamily":"monospace","foreground":"#ffffff","iconSlot":27,"statusSlot":21,"urgent":"#ff0000"})";
  const auto presentation_bytes =
      std::as_bytes(std::span(presentation_document));
  OMARCHY_CHECK(api.applyPresentationSnapshot(77, presentation_bytes) &&
              api.presentation().value(QStringLiteral("statusSlot")).toInt() ==
                  21 &&
              api.presentation()
                      .value(QStringLiteral("barForeground"))
                      .toString() == QStringLiteral("#f0f1f2"));
  const std::string presentation_escape =
      R"({"foreground":"#ffffff","hostObject":"shell"})";
  OMARCHY_CHECK(!api.applyPresentationSnapshot(
              78, std::as_bytes(std::span(presentation_escape))));
  // Canonical manifest tuple order is notifications.send, storage.private.
  const auto payload = granted_snapshot(parsed, {0x0001, 0x0007});
  OMARCHY_CHECK(api.applyPermissionSnapshot(77, payload));
  OMARCHY_CHECK(api.hasPermission("storage.private", "read") &&
              api.hasPermission("notifications.send", "send") &&
              api.permissionState("notifications.send", "send") == "granted" &&
              api.permissions()
                      .value(QStringLiteral("notifications.send"))
                      .toMap()
                      .value(QStringLiteral("state")) ==
                  QStringLiteral("granted") &&
              api.permissions()
                      .value(QStringLiteral("notifications.send"))
                      .toMap()
                      .value(QStringLiteral("operations"))
                      .toMap()
                      .value(QStringLiteral("send")) ==
                  QStringLiteral("granted"));
  const auto before = api.permissions();
  OMARCHY_CHECK(!api.applyPermissionSnapshot(77, payload) &&
              api.permissions() == before);
  OMARCHY_CHECK(!api.brokerReady() && api.markBrokerReady() && api.brokerReady());

  const auto fresh_api = [&] {
    return std::make_unique<worker::QmlBrokerApi>(
        endpoint, parsed, 77);
  };
  {
    auto reordered = parsed;
    std::ranges::reverse(reordered.requests);
    worker::QmlBrokerApi candidate(
        endpoint, reordered, 77);
    OMARCHY_CHECK(candidate.applyPermissionSnapshot(77, payload) &&
                candidate.hasPermission("notifications.send", "send") &&
                candidate.hasPermission("storage.private", "write"));
  }
  const auto rejected_snapshot = [&](std::span<const std::byte> invalid,
                                      const char *message,
                                      std::uint64_t generation = 77) {
    auto candidate = fresh_api();
    require(!candidate->applyPermissionSnapshot(generation, invalid) &&
                candidate->permissionState("storage.private", "read") ==
                    "unavailable" &&
                candidate->applyPermissionSnapshot(77, payload),
            message);
  };
  // Mutate actual wire rows, not encode(invalid): the encoder rejects invalid
  // state/mask combinations and would otherwise test only an empty packet.
  OMARCHY_CHECK(payload.size() == 74);
  struct InvalidRows {
    std::array<std::byte, 6> rows;
    const char *message;
  };
  for (const auto &test : {
           InvalidRows{{std::byte{1}, std::byte{0}, std::byte{2},
                        std::byte{1}, std::byte{0}, std::byte{7}},
                       "excess operation mask reached QML projection"},
           InvalidRows{{std::byte{1}, std::byte{0}, std::byte{0},
                        std::byte{1}, std::byte{0}, std::byte{7}},
                       "empty granted wire row reached QML projection"},
           InvalidRows{{std::byte{2}, std::byte{0}, std::byte{1},
                        std::byte{1}, std::byte{0}, std::byte{7}},
                       "denied permission row retained an operation mask"},
           InvalidRows{{std::byte{1}, std::byte{0}, std::byte{1},
                        std::byte{2}, std::byte{0}, std::byte{0}},
                       "denied required request was not rejected transactionally"},
           InvalidRows{{std::byte{3}, std::byte{0}, std::byte{0},
                        std::byte{1}, std::byte{0}, std::byte{7}},
                       "empty revoked wire row reached QML projection"},
       }) {
    auto invalid = payload;
    for (std::size_t index = 0; index < test.rows.size(); ++index)
      invalid.at(68 + index) = test.rows[index];
    rejected_snapshot(invalid, test.message);
  }
  auto wrong_fingerprint = payload;
  wrong_fingerprint[2] = static_cast<std::byte>(
      wrong_fingerprint[2] == std::byte{'0'} ? '1' : '0');
  rejected_snapshot(wrong_fingerprint,
                    "fingerprint mismatch was not rejected transactionally");
  rejected_snapshot(std::span(payload).first(payload.size() - 1),
                    "request-count mismatch was not rejected transactionally");
  rejected_snapshot(payload,
                    "wrong activation generation was not rejected transactionally", 78);
  {
    auto candidate = fresh_api();
    auto revoked = payload;
    revoked[68] = std::byte{3};
    OMARCHY_CHECK(candidate->applyPermissionSnapshot(77, revoked) &&
                !candidate->hasPermission("notifications.send", "send") &&
                candidate->permissionState("notifications.send", "send") ==
                    "revoked");
  }
  {
    auto sixteen = omarchy::plugin_runtime::test_support::sixteen_operation_manifest(1, 'c');
    worker::QmlBrokerApi high_bit(
        endpoint, sixteen, 77);
    const auto high_bit_payload = granted_snapshot(sixteen, {0x8000});
    OMARCHY_CHECK(high_bit.applyPermissionSnapshot(77, high_bit_payload) &&
                high_bit.permissionState(
                    "org.example.sixteen-operations", "op-15") == "granted" &&
                high_bit.permissionState(
                    "org.example.sixteen-operations", "op-14") == "denied" &&
                high_bit.permissions()
                        .value(QStringLiteral(
                            "org.example.sixteen-operations"))
                        .toMap()
                        .value(QStringLiteral("state")) ==
                    QStringLiteral("partial"));

    worker::QmlBrokerApi full(
        endpoint, sixteen, 77);
    const auto full_payload = granted_snapshot(sixteen, {0xffff});
    OMARCHY_CHECK(full.applyPermissionSnapshot(77, full_payload) &&
                full.hasPermission("org.example.sixteen-operations", "op-00") &&
                full.hasPermission("org.example.sixteen-operations", "op-15"));

    sixteen.requests.front().operations.pop_back();
    worker::QmlBrokerApi fifteen(
        endpoint, sixteen, 77);
    const auto excess_payload = granted_snapshot(sixteen, {0x8000});
    OMARCHY_CHECK(!fifteen.applyPermissionSnapshot(77, excess_payload));
  }
  {
    manifest::ManifestV2 colliding;
    colliding.id = "org.example.colliding";
    colliding.requests.push_back(
        {.capability = "org.example.reserved-operations",
         .reason = "prove structural namespacing",
         .canonical_scope = "{}",
         .definition_generation = 1,
         .definition_digest = std::string(64, 'b'),
         .operations = {"required", "state"},
         .required = false});
    for (const bool required : {false, true}) {
      colliding.requests.front().required = required;
      worker::QmlBrokerApi candidate(endpoint, colliding, 77);
      const auto collision_payload = granted_snapshot(colliding, {0x0001});
      OMARCHY_CHECK(candidate.applyPermissionSnapshot(77, collision_payload));
      const auto capability =
          candidate.permissions()
              .value(QStringLiteral("org.example.reserved-operations"))
              .toMap();
      const auto operations = capability.value(QStringLiteral("operations")).toMap();
      OMARCHY_CHECK(capability.value(QStringLiteral("required")).toBool() == required &&
                  capability.value(QStringLiteral("state")).toString() ==
                      QStringLiteral("partial") &&
                  operations.value(QStringLiteral("required")).toString() ==
                      QStringLiteral("granted") &&
                  operations.value(QStringLiteral("state")).toString() ==
                      QStringLiteral("denied") &&
                  candidate.hasPermission("org.example.reserved-operations", "required") &&
                  !candidate.hasPermission("org.example.reserved-operations", "state"));
    }
  }
  {
    manifest::ManifestV2 many;
    many.id = "org.example.many";
    for (std::size_t index = 0; index < 65; ++index) {
      many.requests.push_back(
          {.capability = "org.example.capability-" + std::to_string(index),
           .reason = "bounded projection",
           .canonical_scope = "{}",
           .definition_generation = 1,
           .definition_digest = std::string(64, 'a'),
           .operations = {"read"},
           .required = false});
    }
    worker::QmlBrokerApi candidate(
        endpoint, many,
        77);
    const auto many_payload = granted_snapshot(
        many, std::vector<std::uint16_t>(many.requests.size(), 0x0001));
    OMARCHY_CHECK(candidate.applyPermissionSnapshot(77, many_payload) &&
                candidate.hasPermission("org.example.capability-64", "read"));
  }

  QQmlEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("runtime"), &api);
  QQmlComponent component(&engine);
  component.setData(R"(
    import QtQml
    QtObject {
      id: root
      readonly property bool notificationsAvailable:
        runtime.hasPermission("notifications.send", "send")
    })", QUrl());
  std::unique_ptr<QObject> qml(component.create());
  OMARCHY_CHECK(qml != nullptr && qml->property("notificationsAvailable").toBool());
  auto *still_checked = qobject_cast<worker::BrokerCall *>(
      api.invoke(QStringLiteral("storage.private"), QStringLiteral("read"),
                 {{QStringLiteral("key"), QStringLiteral("widget-state")}})
          .value<QObject *>());
  static_cast<void>(receive_packet(host));
  const auto broker_denial = broker::encode_broker_error({
      .failed_operation = broker::kDynamicInvokeMessage,
      .reason = broker::BrokerErrorReason::denied,
      .decision = permissions::GrantDecisionCode::explicitly_denied});
  finish(api, endpoint, host, host_sequence, still_checked->correlation(),
         static_cast<std::uint16_t>(wire::CommonMessageType::typed_error),
         broker_denial);
  OMARCHY_CHECK(still_checked->finished() && !still_checked->ok());
  OMARCHY_CHECK(qml->property("notificationsAvailable").toBool());
}
void neutral_surface_trusted_input() {
  const std::filesystem::path fixture =
      std::filesystem::path(OMARCHY_NEUTRAL_SURFACE_FIXTURE_ROOT);
  QFile manifest_file(
      QString::fromStdString((fixture / "manifest.json").string()));
  OMARCHY_CHECK(manifest_file.open(QIODevice::ReadOnly));
  const auto parsed = manifest::parse_manifest_v2(
      manifest_file.readAll().toStdString());
  OMARCHY_CHECK(parsed.requests.empty());

  SeqpacketPair pair = SeqpacketPair::create();
  wire::SessionSequence worker_sequence;
  worker::WorkerEndpoint endpoint(pair.worker.get(),
                                  wire::EndpointRole::broker,
                                  broker::kBrokerRoleVersion,
                                  worker_sequence);
  handshake(endpoint, pair.trusted.get());
  worker::QmlBrokerApi api(
      endpoint, parsed, 77);
  FixtureIntentSink sink;
  OMARCHY_CHECK(api.bindSurfaceIntentSink(sink));

  const auto page_size = sysconf(_SC_PAGESIZE);
  OMARCHY_CHECK(page_size > 0);
  const auto allocation = surface::make_allocation(
      {.id = 71, .generation = 9}, 252, 48, 252, 48, 1, 1,
      static_cast<std::uint64_t>(page_size));
  const auto panel_allocation = surface::make_allocation(
      {.id = 72, .generation = 9}, 320, 480, 320, 480, 1, 1,
      static_cast<std::uint64_t>(page_size));
  OMARCHY_CHECK(allocation.has_value() && panel_allocation.has_value());

  worker::WorkerRuntime runtime(fixture);
  OMARCHY_CHECK(static_cast<bool>(runtime.bind_runtime_api(api)));
  OMARCHY_CHECK(static_cast<bool>(runtime.load_surface_entry("bar", "ui/Bar.qml")));
  OMARCHY_CHECK(
      static_cast<bool>(runtime.load_surface_entry("panel", "ui/Panel.qml")));
  OMARCHY_CHECK(static_cast<bool>(runtime.select_software_profile(
              surface::software_profile_offer())));
  OMARCHY_CHECK(static_cast<bool>(runtime.bind_surface("bar", allocation->surface)));
  OMARCHY_CHECK(static_cast<bool>(
              runtime.bind_surface("panel", panel_allocation->surface)));
  const int descriptor = static_cast<int>(
      syscall(SYS_memfd_create, "neutral-input-frame", MFD_CLOEXEC));
  const int panel_descriptor = static_cast<int>(
      syscall(SYS_memfd_create, "neutral-panel-frame", MFD_CLOEXEC));
  OMARCHY_CHECK(descriptor >= 0 &&
              ftruncate(descriptor,
                        static_cast<off_t>(allocation->mapping_bytes)) == 0 &&
              panel_descriptor >= 0 &&
              ftruncate(panel_descriptor,
                        static_cast<off_t>(panel_allocation->mapping_bytes)) ==
                  0 &&
              static_cast<bool>(runtime.allocate(*allocation, descriptor)) &&
              static_cast<bool>(
                  runtime.allocate(*panel_allocation, panel_descriptor)));

  const auto pointer = [&](std::uint64_t sequence, std::uint32_t x,
                           surface::ButtonState state, std::uint32_t buttons) {
    const surface::InputEvent event{
        .surface = allocation->surface,
        .sequence = sequence,
        .payload = surface::PointerButton{
            .position = {.x_q16 = x << 16, .y_q16 = 24U << 16},
            .button = static_cast<std::uint32_t>(Qt::LeftButton),
            .state = state,
            .buttons = buttons}};
    OMARCHY_CHECK(api.beginTrustedGestureForInput(event) &&
                static_cast<bool>(runtime.input(event)));
    api.endTrustedGesture();
  };
  pointer(1, 63, surface::ButtonState::pressed,
          static_cast<std::uint32_t>(Qt::LeftButton));
  runtime.request_render();
  bool sibling_rendered = false;
  for (int attempt = 0; attempt < 4 && !sibling_rendered; ++attempt) {
    const auto frame = runtime.render();
    OMARCHY_CHECK(frame.has_value());
    sibling_rendered = frame->surface == panel_allocation->surface;
  }
  OMARCHY_CHECK(sibling_rendered);
  drain_events();
  pointer(2, 63, surface::ButtonState::released, 0);
  OMARCHY_CHECK(sink.targets.size() == 1 && sink.targets[0] == "panel" &&
              sink.actions[0] == surface::SurfaceIntentAction::toggle &&
              sink.sources[0] &&
              sink.sources[0]->surface_id == allocation->surface.id &&
              sink.sources[0]->surface_generation ==
                  allocation->surface.generation &&
              sink.sources[0]->input_sequence == 1);
  sink.sources.clear();
  sink.targets.clear();
  sink.actions.clear();

  OMARCHY_CHECK(static_cast<bool>(runtime.input(
              {.surface = allocation->surface,
               .sequence = 3,
               .payload = surface::FocusChanged{.focused = true}})) &&
              sink.targets.empty());

  pointer(4, 63, surface::ButtonState::pressed,
          static_cast<std::uint32_t>(Qt::LeftButton));
  bool deferred_callback_accepted = true;
  QTimer::singleShot(0, [&] {
    deferred_callback_accepted = api.requestSurfaceIntent(
        QStringLiteral("panel"), QStringLiteral("toggle"));
  });
  drain_events();
  OMARCHY_CHECK(sink.targets.empty() && !deferred_callback_accepted);

  pointer(5, 63, surface::ButtonState::released, 0);
  OMARCHY_CHECK(sink.targets.size() == 1 && sink.targets[0] == "panel" &&
              sink.actions[0] == surface::SurfaceIntentAction::toggle &&
              sink.sources[0] &&
              sink.sources[0]->surface_id == allocation->surface.id &&
              sink.sources[0]->surface_generation ==
                  allocation->surface.generation &&
              sink.sources[0]->input_sequence == 4);
  OMARCHY_CHECK(!api.requestSurfaceIntent(QStringLiteral("panel"),
                                    QStringLiteral("toggle")));

  pointer(6, 189, surface::ButtonState::pressed,
          static_cast<std::uint32_t>(Qt::LeftButton));
  OMARCHY_CHECK(sink.targets.size() == 1);
  pointer(7, 189, surface::ButtonState::released, 0);
  OMARCHY_CHECK(sink.targets.size() == 2 && sink.targets[1] == "overlay" &&
              sink.actions[1] == surface::SurfaceIntentAction::toggle &&
              sink.sources[1] && sink.sources[1]->input_sequence == 6);
}

void run() {
  neutral_surface_trusted_input();
  structured_command_execution();
  worker::BrokerCall text_call(1);
  text_call.resolve(QByteArray("{\"station\":\"M\xC3\xBCnchen\"}"));
  OMARCHY_CHECK(text_call.utf8Text() == QString::fromUtf8("{\"station\":\"M\xC3\xBCnchen\"}"));
  worker::BrokerCall binary_call(2);
  binary_call.resolve(QByteArray("\xff", 1));
  OMARCHY_CHECK(binary_call.utf8Text().isEmpty());
  SeqpacketPair pair = SeqpacketPair::create();
  wire::SessionSequence worker_sequence;
  wire::SessionSequence host_sequence;
  worker::WorkerEndpoint endpoint(pair.worker.get(), wire::EndpointRole::broker,
                                  broker::kBrokerRoleVersion, worker_sequence);
  handshake(endpoint, pair.trusted.get());
  const auto readiness_manifest = manifest::parse_manifest_v2(
      R"({"schemaVersion":2,"id":"org.example.readiness","name":"Readiness","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"permissions":{"required":[],"optional":[{"capability":"storage.private","definitionGeneration":1,"definitionDigest":"8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66","operations":["read","remove","write"],"itemBytes":4096,"reason":"state","quotaBytes":65536}]}})");
  worker::QmlBrokerApi api(
      endpoint,
      readiness_manifest, 77);
  QQmlEngine readiness_engine;
  readiness_engine.rootContext()->setContextProperty(
      QStringLiteral("runtime"), &api);
  QQmlComponent readiness_component(&readiness_engine);
  readiness_component.setData(R"(
    import QtQml
    QtObject {
      id: root
      property bool observedReady: runtime.brokerReady
      property int transitions: 0
      property var startupCall
      property var readyCall
      function requestWrite() {
        return runtime.invoke("storage.private", "write", {
          key: "startup-state", value: "saved",
          quotaBytes: 65536, itemBytes: 4096
        })
      }
      Component.onCompleted: startupCall = requestWrite()
      property Connections readiness: Connections {
        target: runtime
        function onBrokerReadyChanged() {
          root.transitions += 1
          if (runtime.brokerReady)
            root.readyCall = root.requestWrite()
        }
      }
    })", QUrl());
  std::unique_ptr<QObject> readiness(readiness_component.create());
  OMARCHY_CHECK(readiness != nullptr && !api.brokerReady() &&
              !readiness->property("observedReady").toBool());
  OMARCHY_CHECK(fcntl(pair.trusted.get(), F_SETFL, O_NONBLOCK) == 0);
  auto *early = qobject_cast<worker::BrokerCall *>(
      readiness->property("startupCall").value<QObject *>());
  std::byte early_byte{};
  errno = 0;
  OMARCHY_CHECK(early != nullptr && early->finished() && !early->ok() &&
              early->correlation() == 0 &&
              early->error() == QStringLiteral("broker-not-ready") &&
              recv(pair.trusted.get(), &early_byte, 1, 0) < 0 &&
              (errno == EAGAIN || errno == EWOULDBLOCK));
  OMARCHY_CHECK(!api.markBrokerReady() && !api.brokerReady());
  const auto readiness_snapshot = granted_snapshot(readiness_manifest, {0x0004});
  OMARCHY_CHECK(api.applyPermissionSnapshot(77, readiness_snapshot) &&
              api.hasPermission("storage.private", "write") &&
              !api.hasPermission("storage.private", "read") &&
              !api.brokerReady() &&
              !readiness->property("observedReady").toBool());
  OMARCHY_CHECK(api.markBrokerReady() && api.brokerReady() &&
              !api.markBrokerReady());
  drain_events();
  OMARCHY_CHECK(readiness->property("observedReady").toBool() &&
              readiness->property("transitions").toInt() == 1);
  OMARCHY_CHECK(fcntl(pair.trusted.get(), F_SETFL, 0) == 0);
  auto *ready_call = qobject_cast<worker::BrokerCall *>(
      readiness->property("readyCall").value<QObject *>());
  OMARCHY_CHECK(ready_call != nullptr && !ready_call->finished() &&
              ready_call->correlation() != 0);
  const auto ready_request_bytes = receive_packet(pair.trusted.get());
  const auto ready_request = wire::decode_packet(
      ready_request_bytes, wire::EndpointRole::broker);
  definitions::DynamicInvocation ready_decoded{};
  OMARCHY_CHECK(ready_request &&
              definitions::decode_dynamic_invocation(ready_request.packet.payload, ready_decoded) &&
              ready_decoded.operation.view() == "write" &&
              ready_decoded.definition.canonical_name.view() == "storage.private");
  finish(api, endpoint, pair.trusted.get(), host_sequence,
         ready_call->correlation(), broker::kBrokerResultMessage, {});
  OMARCHY_CHECK(ready_call->finished() && ready_call->ok());
  IntentSink intent_sink;
  IntentSink second_sink;
  OMARCHY_CHECK(api.bindSurfaceIntentSink(intent_sink) &&
              !api.bindSurfaceIntentSink(second_sink) &&
              !api.requestSurfaceIntent(QStringLiteral("panel"),
                                        QStringLiteral("toggle")) &&
              intent_sink.calls == 0);
  OMARCHY_CHECK(api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                   QStringLiteral("dismiss")) &&
              intent_sink.calls == 1 && !intent_sink.last_source &&
              intent_sink.last_target == "PanelWidget" &&
              intent_sink.last_action == surface::SurfaceIntentAction::dismiss);
  intent_sink.calls = 0;
  intent_sink.last_source.reset();
  api.beginTrustedGesture(3, 77, 9);
  OMARCHY_CHECK(!api.requestSurfaceIntent(QString(), QStringLiteral("toggle")) &&
              !api.requestSurfaceIntent(QStringLiteral("Panel.Widget"),
                                        QStringLiteral("toggle")) &&
              !api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                        QStringLiteral("execute")) &&
              !api.requestSurfaceIntent(QStringLiteral("MissingWidget"),
                                       QStringLiteral("toggle")) &&
              intent_sink.calls == 1 &&
              !api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                        QStringLiteral("toggle")));
  api.beginTrustedGesture(3, 77, 10);
  OMARCHY_CHECK(api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                   QStringLiteral("toggle")) &&
              intent_sink.calls == 2 &&
              intent_sink.last_source &&
              intent_sink.last_source->surface_id == 3 &&
              intent_sink.last_source->surface_generation == 77 &&
              intent_sink.last_source->input_sequence == 10 &&
              intent_sink.last_target == "PanelWidget" &&
              intent_sink.last_action == surface::SurfaceIntentAction::toggle &&
              !api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                        QStringLiteral("toggle")));
  api.beginTrustedGesture(3, 77, 11);
  const QVariantMap intent_data{
      {QStringLiteral("screen"), QStringLiteral("DP-1")},
      {QStringLiteral("resume"), true}};
  OMARCHY_CHECK(api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                   QStringLiteral("open"), intent_data) &&
              intent_sink.calls == 3 && intent_sink.last_source &&
              intent_sink.last_source->input_sequence == 11 &&
              intent_sink.last_data == intent_data);
  api.beginTrustedGesture(3, 77, 12);
  OMARCHY_CHECK(!api.requestSurfaceIntent(
              QStringLiteral("PanelWidget"), QStringLiteral("open"),
              {{QStringLiteral("oversized"), QString(4097, QLatin1Char('x'))}}) &&
              intent_sink.calls == 3 &&
              api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                       QStringLiteral("open")) &&
              intent_sink.calls == 4 && intent_sink.last_source &&
              intent_sink.last_source->input_sequence == 12);
  api.endTrustedGesture();

  const surface::SurfaceKey gesture_surface{.id = 3, .generation = 77};
  const auto pointer = [&](std::uint64_t sequence, std::uint32_t button,
                           surface::ButtonState state) {
    return surface::InputEvent{
        .surface = gesture_surface,
        .sequence = sequence,
        .payload = surface::PointerButton{
            .position = {.x_q16 = 1U << 16, .y_q16 = 1U << 16},
            .button = button,
            .state = state,
            .buttons = state == surface::ButtonState::pressed ? button : 0U}};
  };
  const auto left = static_cast<std::uint32_t>(Qt::LeftButton);
  const auto right = static_cast<std::uint32_t>(Qt::RightButton);

  const auto pressed_once = pointer(11, left, surface::ButtonState::pressed);
  OMARCHY_CHECK(api.beginTrustedGestureForInput(pressed_once) &&
              api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                       QStringLiteral("toggle")));
  api.endTrustedGesture();
  OMARCHY_CHECK(!api.beginTrustedGestureForInput(
              pointer(12, left, surface::ButtonState::released)) &&
              !api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                        QStringLiteral("toggle")));

  intent_sink.accept = false;
  OMARCHY_CHECK(api.beginTrustedGestureForInput(
              pointer(13, left, surface::ButtonState::pressed)) &&
              !api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                        QStringLiteral("toggle")));
  api.endTrustedGesture();
  OMARCHY_CHECK(!api.beginTrustedGestureForInput(
              pointer(14, left, surface::ButtonState::released)));
  intent_sink.accept = true;

  OMARCHY_CHECK(api.beginTrustedGestureForInput(
              pointer(15, left, surface::ButtonState::pressed)));
  api.endTrustedGesture();
  OMARCHY_CHECK(!api.beginTrustedGestureForInput(
              pointer(16, right, surface::ButtonState::released)) &&
              !api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                        QStringLiteral("toggle")));

  OMARCHY_CHECK(api.beginTrustedGestureForInput(
              pointer(17, left, surface::ButtonState::pressed)));
  api.endTrustedGesture();
  OMARCHY_CHECK(!api.beginTrustedGestureForInput(
              {.surface = gesture_surface,
               .sequence = 18,
               .payload = surface::Cancel{}}) &&
              !api.beginTrustedGestureForInput(
                  pointer(19, left, surface::ButtonState::released)));

  OMARCHY_CHECK(api.beginTrustedGestureForInput(
              pointer(20, left, surface::ButtonState::pressed)));
  api.endTrustedGesture();
  OMARCHY_CHECK(!api.beginTrustedGestureForInput(
              {.surface = gesture_surface,
               .sequence = 21,
               .payload = surface::FocusChanged{.focused = false}}) &&
              !api.beginTrustedGestureForInput(
                  pointer(22, left, surface::ButtonState::released)));

  OMARCHY_CHECK(api.beginTrustedGestureForInput(
              pointer(23, left, surface::ButtonState::pressed)));
  api.endTrustedGesture();
  OMARCHY_CHECK(api.beginTrustedGestureForInput(
              pointer(24, right, surface::ButtonState::pressed)));
  api.endTrustedGesture();
  OMARCHY_CHECK(api.beginTrustedGestureForInput(
              pointer(25, right, surface::ButtonState::released)) &&
              api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                       QStringLiteral("toggle")) &&
              intent_sink.last_source &&
              intent_sink.last_source->input_sequence == 24);
  api.endTrustedGesture();

  const surface::InputEvent touch_begin{
      .surface = gesture_surface,
      .sequence = 26,
      .payload = surface::TouchFrame{
          .phase = surface::TouchFramePhase::begin}};
  OMARCHY_CHECK(api.beginTrustedGestureForInput(touch_begin));
  bool touch_callback_accepted = true;
  QTimer::singleShot(0, [&] {
    touch_callback_accepted = api.requestSurfaceIntent(
        QStringLiteral("PanelWidget"), QStringLiteral("toggle"));
  });
  api.endTrustedGesture();
  drain_events();
  OMARCHY_CHECK(!touch_callback_accepted);
  const surface::InputEvent touch_end{
      .surface = gesture_surface,
      .sequence = 27,
      .payload = surface::TouchFrame{
          .phase = surface::TouchFramePhase::end}};
  OMARCHY_CHECK(api.beginTrustedGestureForInput(touch_end) &&
              api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                       QStringLiteral("toggle")) &&
              intent_sink.last_source &&
              intent_sink.last_source->input_sequence == 26 &&
              !api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                        QStringLiteral("toggle")));
  api.endTrustedGesture();

  OMARCHY_CHECK(api.beginTrustedGestureForInput(
              {.surface = gesture_surface,
               .sequence = 28,
               .payload = surface::TouchFrame{
                   .phase = surface::TouchFramePhase::begin}}));
  api.endTrustedGesture();
  OMARCHY_CHECK(!api.beginTrustedGestureForInput(
              {.surface = gesture_surface,
               .sequence = 29,
               .payload = surface::TouchFrame{
                   .phase = surface::TouchFramePhase::cancel}}) &&
              !api.beginTrustedGestureForInput(
                  {.surface = gesture_surface,
                   .sequence = 30,
                   .payload = surface::TouchFrame{
                       .phase = surface::TouchFramePhase::end}}));

  const auto key = [&](std::uint64_t sequence, surface::ButtonState state,
                       bool repeat) {
    return surface::InputEvent{
        .surface = gesture_surface,
        .sequence = sequence,
        .payload = surface::Key{
            .key = static_cast<std::uint32_t>(Qt::Key_Return),
            .native_scan_code = 28,
            .state = state,
            .auto_repeat = repeat,
            .text = "\r"}};
  };
  OMARCHY_CHECK(api.beginTrustedGestureForInput(
              key(31, surface::ButtonState::pressed, false)) &&
              api.requestSurfaceIntent(QStringLiteral("PanelWidget"),
                                       QStringLiteral("toggle")) &&
              intent_sink.last_source &&
              intent_sink.last_source->input_sequence == 31);
  api.endTrustedGesture();
  OMARCHY_CHECK(!api.beginTrustedGestureForInput(
              key(32, surface::ButtonState::pressed, true)) &&
              !api.beginTrustedGestureForInput(
                  key(33, surface::ButtonState::released, false)));

  QVariantMap arguments{{QStringLiteral("key"), QStringLiteral("timer-state")},
                        {QStringLiteral("value"), QByteArray("saved")}};
  auto *allowed = qobject_cast<worker::BrokerCall *>(
      api.invoke(QStringLiteral("storage.private"), QStringLiteral("write"),
                 arguments).value<QObject *>());
  OMARCHY_CHECK(allowed != nullptr && !allowed->finished() && allowed->correlation() != 0);
  const auto request_bytes = receive_packet(pair.trusted.get());
  const auto request = wire::decode_packet(request_bytes, wire::EndpointRole::broker);
  definitions::DynamicInvocation decoded{};
  OMARCHY_CHECK(request && definitions::decode_dynamic_invocation(request.packet.payload, decoded) &&
              decoded.operation.view() == "write" &&
              decoded.definition.canonical_name.view() == "storage.private");
  finish(api, endpoint, pair.trusted.get(), host_sequence,
         allowed->correlation(),
         broker::kBrokerResultMessage, {});
  OMARCHY_CHECK(allowed->finished() && allowed->ok());

  QQmlEngine completion_engine;
  completion_engine.rootContext()->setContextProperty(
      QStringLiteral("runtime"), &api);
  QQmlComponent completion_component(&completion_engine);
  completion_component.setData(R"(
    import QtQml
    QtObject {
      id: root
      property var call: null
      property string phase: "idle"
      function start() {
        call = runtime.invoke("storage.private", "write", {
          key: "qml-completion", value: "saved",
          quotaBytes: 65536, itemBytes: 4096
        })
        phase = "waiting"
      }
      property Connections completion: Connections {
        target: runtime
        function onCallFinished(call) {
          if (call && root.call && call.finished &&
              call.correlation === root.call.correlation)
            root.phase = call.ok ? "allowed" : "denied"
        }
      }
    })", QUrl());
  std::unique_ptr<QObject> completion(completion_component.create());
  OMARCHY_CHECK(completion != nullptr);
  const auto complete_qml_call = [&](std::uint16_t type,
                                     std::span<const std::byte> payload,
                                     QString expected_phase) {
    OMARCHY_CHECK(QMetaObject::invokeMethod(completion.get(), "start"));
    const auto request = receive_packet(pair.trusted.get());
    const auto decoded = wire::decode_packet(request, wire::EndpointRole::broker);
    OMARCHY_CHECK(decoded && decoded.packet.header.message_type ==
                                broker::kDynamicInvokeMessage);
    finish(api, endpoint, pair.trusted.get(), host_sequence,
           decoded.packet.header.correlation_id, type, payload);
    drain_events();
    OMARCHY_CHECK(completion->property("phase") == expected_phase);
  };
  complete_qml_call(broker::kBrokerResultMessage, {}, QStringLiteral("allowed"));
  const auto qml_denial = broker::encode_broker_error({
      .failed_operation = broker::kDynamicInvokeMessage,
      .reason = broker::BrokerErrorReason::denied,
      .decision = permissions::GrantDecisionCode::explicitly_denied});
  complete_qml_call(static_cast<std::uint16_t>(wire::CommonMessageType::typed_error),
                    qml_denial, QStringLiteral("denied"));

  for (const auto decision : {permissions::GrantDecisionCode::explicitly_denied,
                              permissions::GrantDecisionCode::outside_scope}) {
    auto *call = qobject_cast<worker::BrokerCall *>(
        api.invoke(QStringLiteral("storage.private"), QStringLiteral("write"),
                   arguments).value<QObject *>());
    OMARCHY_CHECK(call != nullptr);
    static_cast<void>(receive_packet(pair.trusted.get()));
    const auto denial = broker::encode_broker_error({
        .failed_operation = broker::kDynamicInvokeMessage,
        .reason = broker::BrokerErrorReason::denied,
        .decision = decision});
    finish(api, endpoint, pair.trusted.get(), host_sequence,
           call->correlation(),
           static_cast<std::uint16_t>(wire::CommonMessageType::typed_error), denial);
    OMARCHY_CHECK(call->finished() && !call->ok());
    if (decision == permissions::GrantDecisionCode::explicitly_denied)
      OMARCHY_CHECK(call->error() == "denied");
  }

  OMARCHY_CHECK(fcntl(pair.trusted.get(), F_SETFL, O_NONBLOCK) == 0);
  auto *unknown = qobject_cast<worker::BrokerCall *>(
      api.invoke(QStringLiteral("shell.exec"), QStringLiteral("execute"), {})
          .value<QObject *>());
  auto *wrong_capability = qobject_cast<worker::BrokerCall *>(
      api.invoke(QStringLiteral("notifications.send"),
                 QStringLiteral("send"), {})
          .value<QObject *>());
  std::byte byte{};
  errno = 0;
  OMARCHY_CHECK(unknown != nullptr && unknown->finished() && !unknown->ok() &&
              unknown->error() == "request-undeclared" &&
              wrong_capability != nullptr && wrong_capability->finished() &&
              !wrong_capability->ok() &&
              wrong_capability->error() == "request-undeclared" &&
              recv(pair.trusted.get(), &byte, 1, 0) < 0 &&
              (errno == EAGAIN || errno == EWOULDBLOCK));
  dynamic_qml_to_adapter();
  capability_qualified_collision();
  permission_awareness(endpoint, pair.trusted.get(), host_sequence);
  OMARCHY_CHECK(api.beginTrustedGestureForInput(
              pointer(31, left, surface::ButtonState::pressed)));
  api.endTrustedGesture();
  api.disconnect(QStringLiteral("test-disconnect"));
  OMARCHY_CHECK(!api.beginTrustedGestureForInput(
              pointer(32, left, surface::ButtonState::released)));
  drain_events();
  auto *disconnected = qobject_cast<worker::BrokerCall *>(
      api.invoke(QStringLiteral("storage.private"), QStringLiteral("write"),
                 arguments).value<QObject *>());
  errno = 0;
  OMARCHY_CHECK(!api.brokerReady() &&
              !readiness->property("observedReady").toBool() &&
              readiness->property("transitions").toInt() == 2 &&
              disconnected != nullptr && disconnected->finished() &&
              !disconnected->ok() && disconnected->correlation() == 0 &&
              disconnected->error() == QStringLiteral("broker-unavailable") &&
              recv(pair.trusted.get(), &byte, 1, 0) < 0 &&
              (errno == EAGAIN || errno == EWOULDBLOCK));
}
} // namespace


int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  qputenv("QSG_RHI_BACKEND", QByteArrayLiteral("software"));
  QGuiApplication application(argc, argv);
  try { run(); std::cout << "QML broker API: PASS\n"; return 0; }
  catch (const std::exception &error) {
    if (std::string_view(error.what()).find("SO_PEERCRED baseline: Operation not permitted") != std::string_view::npos) {
      std::cout << "QML broker API: SKIP (SO_PEERCRED blocked by test namespace)\n";
      return 77;
    }
    std::cerr << "QML broker API: " << error.what() << '\n'; return 1;
  }
}
