#include "capability_definition.hpp"

#include <array>
#include <stdexcept>

namespace omarchy::plugins::definitions {
namespace {
struct OperationSpec {
  std::string_view name;
  std::string_view label;
  bool mutating = false;
  bool gesture = false;
};

struct CatalogSpec {
  std::string_view name;
  std::string_view authority;
  EnforcementFamily family;
  std::string_view category_id;
  std::string_view category_label;
  std::string_view title;
  std::string_view risk_text;
  RiskLevel risk;
  RevocationPolicy revocation;
  std::string_view adapter_class;
  std::string_view contract;
  std::span<const OperationSpec> operations;
};

constexpr std::array network_operations{
    OperationSpec{"fetch", "Fetch bounded data"}};
constexpr std::array open_operations{
    OperationSpec{"open", "Open a link", true, true}};
constexpr std::array observe_operations{
    OperationSpec{"observe", "Observe selected data"}};
constexpr std::array control_operations{
    OperationSpec{"control", "Change selected controls", true, true}};
constexpr std::array media_operations{
    OperationSpec{"play", "Play an approved source", true},
    OperationSpec{"control", "Control approved playback", true}};
constexpr std::array execute_operations{
    OperationSpec{"run", "Run an approved command", true}};

constexpr std::array storage_operations{
    OperationSpec{"read", "Read private state"},
    OperationSpec{"write", "Write private state", true},
    OperationSpec{"remove", "Remove private state", true}};
constexpr std::array notification_operations{
    OperationSpec{"send", "Send a notification", true}};

constexpr std::array catalog{
    CatalogSpec{
        "storage.private", "storage.private-v1", EnforcementFamily::private_storage,
        "local.storage", "Private storage",
        "Store private plugin state",
        "Reads and writes bounded state only in this plugin's private directory",
        RiskLevel::low, RevocationPolicy::restart_worker,
        "private-storage",
        "private-storage-v1;request=key,value;response=found,value;"
        "scope=quota-bytes,item-bytes;key=flat-ascii;value=utf8;"
        "directory=activation-bound;links=reject;writes=atomic",
        storage_operations},
    CatalogSpec{
        "notifications.send", "notifications.send-v1", EnforcementFamily::notifications,
        "desktop.notifications", "Desktop notifications",
        "Send selected notifications",
        "Sends bounded notifications only in approved categories",
        RiskLevel::moderate, RevocationPolicy::restart_worker,
        "desktop-notifications",
        "desktop-notifications-v1;request=category,title,body;response=result;"
        "scope=notification-categories;plugin-label=host-bound;"
        "actions=none;urgency=low;timeout=bounded",
        notification_operations},
    CatalogSpec{
        "network.fetch", "network.fetch-v1", EnforcementFamily::network_fetch,
        "network.access", "Network access",
        "Fetch from selected HTTPS origins",
        "Sends bounded requests only to the selected origins and methods",
        RiskLevel::high, RevocationPolicy::cancel_inflight,
        "bounded-network-fetch",
        "bounded-network-fetch-v1;request=method,origin,path,headers,body,"
        "response-type,media-json-pointers;response=status,content-type,"
        "body-or-json,source-handles;"
        "scope=https-origins-methods;redirects=reject;"
        "resolved-addresses=public-only;limits=provider-fixed",
        network_operations},
    CatalogSpec{"external.open-uri.https", "external.open-uri.https-v1",
                EnforcementFamily::external_open_uri, "desktop.actions",
                "Desktop actions",
                "Open selected HTTPS sites",
                "Opens a user-visible HTTPS address after one fresh gesture",
                RiskLevel::moderate, RevocationPolicy::deny_new,
                "desktop-open-uri",
                "desktop-open-uri-v1;request=url,presentation;response=result;"
                "scope=https-origins-gesture;gesture=fresh;scheme=https;"
                "origin=revalidated-by-provider;launch=trusted-desktop",
                open_operations},
    CatalogSpec{
        "system.observe", "system.observe-v1",
        EnforcementFamily::system_observe, "system.observation",
        "System observation",
        "Observe selected system data",
        "Reads only selected bounded datasets after provider sanitization",
        RiskLevel::moderate, RevocationPolicy::cancel_inflight,
        "sanitized-system-observe",
        "sanitized-system-observe-v1;request=dataset;response=bounded-records;"
        "scope=named-sanitized-datasets;identifiers=opaque;"
        "raw-system-metadata=forbidden",
        observe_operations},
    CatalogSpec{
        "device.observe", "device.observe-v1",
        EnforcementFamily::device_observe, "hardware.devices",
        "Hardware devices",
        "Observe a selected device",
        "Reads only the selected fields from one explicitly selected device",
        RiskLevel::moderate, RevocationPolicy::cancel_inflight,
        "selected-device-observe",
        "selected-device-observe-v1;request=status;response=selected-fields;"
        "scope=selected-device-fields;device=provider-bound;"
        "hardware-identifiers=redacted",
        observe_operations},
    CatalogSpec{
        "device.control", "device.control-v1",
        EnforcementFamily::device_control, "hardware.devices",
        "Hardware devices",
        "Control a selected device",
        "Changes only selected controls on one explicitly selected device",
        RiskLevel::high, RevocationPolicy::cancel_inflight,
        "selected-device-control",
        "selected-device-control-v1;request=control,value;response=result;"
        "scope=selected-device-controls;device=provider-bound;"
        "gesture=fresh",
        control_operations},
    CatalogSpec{
        "media.play-stream", "media.play-stream-v1",
        EnforcementFamily::media_play_stream, "media.playback",
        "Media playback",
        "Play approved media sources",
        "Plays and controls only sources approved during this activation",
        RiskLevel::moderate, RevocationPolicy::cancel_inflight,
        "activation-media-stream",
        "activation-media-stream-v1;request=source-handle,control,value;"
        "response=playback-state;scope=activation-source-handles-controls;"
        "source=provider-bound;teardown=revocation",
        media_operations},
    CatalogSpec{
        "bash.execute", "bash.execute-v2", EnforcementFamily::cli_harness,
        "local.automation", "Local automation",
        "Run approved host commands",
        "Runs approved argv outside the sandbox. Interpreters, scripts and broad rules can access your files and accounts.",
        RiskLevel::critical, RevocationPolicy::cancel_inflight,
        "bounded-command-execute",
        "bounded-command-execute-v2;request=command,arguments;"
        "response=exit-code,stdout,stderr;scope=manifest-command-rules;"
        "shell-parsing=none;environment=reviewed-fixed;"
        "executable=root-owned-descriptor-pinned;limits=bounded-manifest",
        execute_operations},
};

CapabilityDefinition make_definition(const CatalogSpec &spec) {
  CapabilityDefinition definition{
      .canonical_name = Name(spec.name),
      .authority_identity = Name(spec.authority),
      .enforcement_family = spec.family,
      .display_category_id = Name(spec.category_id),
      .display_category_label = Label(spec.category_label),
      .title = Label(spec.title),
      .risk_text = Label(spec.risk_text),
      .risk = spec.risk,
      .revocation = spec.revocation,
      .adapter = {.adapter_class = Name(spec.adapter_class),
                  .contract_digest = semantic_contract_digest(spec.contract),
                  .abi_version = 1},
      .operations = {},
  };
  for (const auto &operation : spec.operations) {
    if (!definition.operations.insert(
            {.name = Name(operation.name),
             .label = Label(operation.label),
             .mutating = operation.mutating,
             .requires_fresh_gesture = operation.gesture}))
      throw std::runtime_error("duplicate catalog operation");
  }
  if (!valid_definition(definition))
    throw std::runtime_error("invalid built-in definition: " +
                             std::string(spec.name));
  return definition;
}

} // namespace

std::vector<CapabilityDefinition> packaged_definitions() {
  std::vector<CapabilityDefinition> result;
  for (const auto &spec : catalog) result.push_back(make_definition(spec));
  return result;
}

} // namespace omarchy::plugins::definitions
