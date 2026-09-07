#include "../../tests/support/temporary_directory.hpp"
#include "omarchy/plugin_runtime/providers/local_provider.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <string>

namespace definitions = omarchy::plugins::definitions;
namespace permissions = omarchy::plugins::permissions;
namespace providers = omarchy::plugin_runtime::providers;
using omarchy::plugin_runtime::test_support::require;
using omarchy::plugin_runtime::test_support::TemporaryDirectory;

namespace {
definitions::DynamicRevisionGrant grant(std::string_view capability,
                                        std::string_view scope) {
  definitions::TrustedDefinitionRegistry registry;
  for (const auto &definition : definitions::packaged_definitions())
    OMARCHY_CHECK(registry.install(definition, 1));
  const auto definition = registry.find(capability);
  OMARCHY_CHECK(definition.has_value());
  definitions::DynamicRevisionGrant result{
      .binding = {.plugin = permissions::PluginId("fixture.local"),
                  .revision = permissions::Digest(std::string(64, 'a')),
                  .policy_fingerprint = permissions::Digest(std::string(64, 'b')),
                  .generation = 7},
      .request = {.definition = {.canonical_name = definitions::Name(capability),
                                 .definition_generation = 1,
                                 .definition_digest = definition->digest},
                  .operations = {}, .scope = definitions::CanonicalScope(scope),
                  .required = true},
      .grant = {}};
  for (const auto &operation : definition->definition->operations.values())
    result.request.operations.insert(operation.name);
  result.grant = {.operations = result.request.operations,
                  .state = permissions::GrantState::granted, .epoch = 4};
  OMARCHY_CHECK(definitions::review_dynamic_grant(registry, result));
  return result;
}

struct Probe {
  int calls = 0;
  bool succeeds = true;
  std::string last;
  static bool send(std::string_view plugin, std::string_view category,
                   std::string_view title, std::string_view body, void *context) noexcept {
    auto &self = *static_cast<Probe *>(context);
    ++self.calls;
    self.last = std::string(plugin) + ":" + std::string(category) + ":" +
                std::string(title) + ":" + std::string(body);
    return self.succeeds;
  }
};

struct Call {
  std::array<std::byte, 8192> response{};
  std::size_t written = 0;
  bool run(providers::LocalProvider &provider,
           const definitions::DynamicRevisionGrant &grant,
           std::string_view operation, std::string_view payload,
           std::size_t capacity = 8192) {
    return provider.dispatch(
        {.authorization = {.binding = grant.binding,
                           .definition = grant.request.definition,
                           .grant_epoch = grant.grant.epoch},
         .operation = operation, .demand_scope = grant.request.scope.view(),
         .payload = std::as_bytes(std::span(payload.data(), payload.size()))},
        std::span(response).first(capacity), written);
  }
  std::string text() const {
    return {reinterpret_cast<const char *>(response.data()), written};
  }
};
} // namespace

int main() {
  TemporaryDirectory directory;
  const int fd = open(directory.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  OMARCHY_CHECK(fd >= 0);
  const auto storage_grant = grant("storage.private", R"({"itemBytes":6,"quotaBytes":8})");
  providers::LocalProvider storage(storage_grant,
      definitions::EnforcementFamily::private_storage, fd, nullptr, nullptr);
  close(fd);
  Call call;
  OMARCHY_CHECK(call.run(storage, storage_grant, "read", R"({"key":"state"})") &&
              call.text() == R"({"found":false,"value":""})");
  OMARCHY_CHECK(call.run(storage, storage_grant, "write", R"({"key":"state","value":"café"})") &&
              call.text() == "{}");
  OMARCHY_CHECK(call.run(storage, storage_grant, "read", R"({"key":"state"})") &&
              call.text() == R"({"found":true,"value":"café"})");
  OMARCHY_CHECK(!call.run(storage, storage_grant, "read", R"({"key":"state"})", 2));
  for (std::string_view payload : {
       R"({"key":"../escape","value":"a"})", R"({"key":".omarchy-tmp-bypass","value":"a"})",
       R"({"key":"state","value":"1234567"})", R"({"key":"other","value":"1234"})",
       R"({"key":"state","value":null})", R"({"key":"state","value":"a","x":1})",
       R"({"key":"state","key":"other","value":"a"})", R"({"value":"a","key":"state"})",
       R"({"key":"state","value":"a"} )", R"({"key":"state","value":"a"}x)", "[]"})
    OMARCHY_CHECK(!call.run(storage, storage_grant, "write", payload));
  OMARCHY_CHECK(!call.run(storage, storage_grant, "write", R"({"key":"state","value":"x"})", 1));
  OMARCHY_CHECK(call.run(storage, storage_grant, "read", R"({"key":"state"})") &&
              call.text() == R"({"found":true,"value":"café"})");
  OMARCHY_CHECK(call.run(storage, storage_grant, "remove", R"({"key":"state"})") &&
              !call.run(storage, storage_grant, "remove", R"({"key":"state"})"));
  OMARCHY_CHECK(call.run(storage, storage_grant, "write", R"({"key":"state","value":""})"));

  Probe probe;
  const auto notification_grant = grant("notifications.send", R"({"categories":["timer","other"]})");
  providers::LocalProvider notifications(notification_grant,
      definitions::EnforcementFamily::notifications, -1, Probe::send, &probe);
  const std::string payload = R"({"body":"Done\nNow","category":"timer","title":"Timer"})";
  OMARCHY_CHECK(call.run(notifications, notification_grant, "send", payload) &&
              probe.last == "fixture.local:timer:Timer:Done\nNow");
  for (std::string_view invalid : {
       R"({"body":"ok","category":"unapproved","title":"T"})",
       R"({"body":"ok","category":"timer","title":"\u001b"})",
       R"({"body":"ok","category":"timer","title":""})",
       R"({"body":"ok","category":"timer","title":null})",
       R"({"body":"ok","category":"timer","title":"T","plugin":"attacker"})",
       R"({"body":"ok","category":"timer"})", "{}", "null"})
    OMARCHY_CHECK(!call.run(notifications, notification_grant, "send", invalid));
  auto invalid_utf8 = payload;
  invalid_utf8[10] = static_cast<char>(0xc0);
  OMARCHY_CHECK(!call.run(notifications, notification_grant, "send", invalid_utf8));
  for (int mutation = 0; mutation < 9; ++mutation) {
    auto foreign = notification_grant;
    switch (mutation) {
    case 0: foreign.binding.plugin = permissions::PluginId("attacker"); break;
    case 1: foreign.binding.revision = permissions::Digest(std::string(64, 'c')); break;
    case 2: foreign.binding.policy_fingerprint = permissions::Digest(std::string(64, 'c')); break;
    case 3: ++foreign.binding.generation; break;
    case 4: ++foreign.grant.epoch; break;
    case 5: foreign.request.definition.canonical_name = definitions::Name("storage.private"); break;
    case 6: ++foreign.request.definition.definition_generation; break;
    case 7: foreign.request.definition.definition_digest = permissions::Digest(std::string(64, 'c')); break;
    case 8: foreign.request.scope = definitions::CanonicalScope(R"({"categories":["unapproved"]})"); break;
    }
    OMARCHY_CHECK(!call.run(notifications, foreign, "send", payload));
  }
  OMARCHY_CHECK(!call.run(notifications, notification_grant, "write", payload) && probe.calls == 1);
  for (auto state : {permissions::GrantState::denied, permissions::GrantState::revoked}) {
    auto unavailable = notification_grant;
    unavailable.grant.state = state;
    providers::LocalProvider provider(unavailable,
        definitions::EnforcementFamily::notifications, -1, Probe::send, &probe);
    OMARCHY_CHECK(!call.run(provider, notification_grant, "send", payload));
  }
  providers::LocalProvider missing(notification_grant,
      definitions::EnforcementFamily::notifications, -1, nullptr, nullptr);
  OMARCHY_CHECK(!call.run(missing, notification_grant, "send", payload));
  probe.succeeds = false;
  OMARCHY_CHECK(!call.run(notifications, notification_grant, "send", payload) && probe.calls == 2);

  for (std::string_view scope : {
       R"({"categories":[]})", R"({"categories":["timer","timer"]})",
       R"({"categories":[null]})", R"({"categories":[""]})",
       R"({"categories":["timer"],"extra":1})", R"({"categories":"timer"})"})
    try {
      auto invalid = grant("notifications.send", scope);
      providers::LocalProvider provider(invalid,
          definitions::EnforcementFamily::notifications, -1, Probe::send, &probe);
      OMARCHY_CHECK(false);
    } catch (const std::invalid_argument &) {}
  for (std::string_view scope : {
       R"({"itemBytes":0,"quotaBytes":8})", R"({"itemBytes":9,"quotaBytes":8})",
       R"({"itemBytes":1.5,"quotaBytes":8})", R"({"itemBytes":null,"quotaBytes":8})",
       R"({"itemBytes":1,"quotaBytes":67108865})", R"({"itemBytes":1,"quotaBytes":8,"x":1})"})
    try {
      auto invalid = grant("storage.private", scope);
      const int directory_fd = open(directory.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      OMARCHY_CHECK(directory_fd >= 0);
      struct CloseFd { int fd; ~CloseFd() { close(fd); } } owned{directory_fd};
      providers::LocalProvider provider(invalid,
          definitions::EnforcementFamily::private_storage, directory_fd, nullptr, nullptr);
      OMARCHY_CHECK(false);
    } catch (const std::invalid_argument &) {}
}
