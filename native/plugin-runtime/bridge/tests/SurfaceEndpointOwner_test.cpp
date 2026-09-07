#include "../../tests/support/test_assert.hpp"

#include "SurfaceEndpointOwner.h"

#include "SurfaceEndpoint.h"
#include "omarchy/plugin/wire/state.hpp"
#include "remote_surface.hpp"
#include "surface_host.hpp"

#include <QQuickWindow>

#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace bridge = omarchy::plugin_runtime::bridge;
namespace channel = omarchy::plugin_runtime::channel;
namespace host = omarchy::plugin_runtime::host_session;
namespace permissions = omarchy::plugins::permissions;
namespace surface_host = omarchy::plugin_runtime::surface_host;
namespace wire = omarchy::plugin::wire;

using omarchy::plugin_runtime::test_support::require;

permissions::ActivationBinding binding(std::uint64_t generation = 7) {
  return {.plugin = permissions::PluginId("org.example.owner"),
          .revision = permissions::Digest(std::string(64, 'a')),
          .policy_fingerprint = permissions::Digest(std::string(64, 'b')),
          .generation = generation};
}

class Port final : public channel::SurfaceSessionPort {
public:
  explicit Port(std::uint64_t generation = 7) {
    description = {
        .binding = binding(generation),
        .key = {.id = 1, .generation = generation},
        .session_nonce = generation + 40,
        .plugin_id = "org.example.owner",
        .surface_name = "pet",
        .canonical_surfaces =
            R"({"pet":{"keyboardFocus":false,"maximumFramesPerSecond":60,"maximumHeight":64,"maximumWidth":128,"role":"desktop-overlay"}})",
    };
  }

  std::optional<channel::SurfaceDescription>
  describe(std::string_view name) const noexcept override {
    ++describe_calls;
    if (!running || name != description.surface_name)
      return {};
    return description;
  }

  bool attach(const channel::SurfaceDescription &expected,
              host::SurfaceEndpoint &candidate) noexcept override {
    ++attach_calls;
    if (!running || fail_attach || expected != description ||
        endpoint != nullptr)
      return false;
    endpoint = &candidate;
    return true;
  }

  bool detach(const channel::SurfaceDescription &expected,
              const host::SurfaceEndpoint &candidate) noexcept override {
    ++detach_calls;
    if (expected != description)
      return true;
    if (endpoint != &candidate)
      return false;
    if (reenter) {
      auto callback = std::move(reenter);
      callback();
    }
    endpoint = nullptr;
    return true;
  }

  bool arm_surface_intent(const channel::SurfaceDescription &,
                          std::uint64_t) noexcept override {
    return false;
  }

  void clear_surface_intent_eligibility(
      const channel::SurfaceDescription &) noexcept override {}

  mutable std::size_t describe_calls = 0;
  std::size_t attach_calls = 0;
  std::size_t detach_calls = 0;
  channel::SurfaceDescription description;
  host::SurfaceEndpoint *endpoint = nullptr;
  bool running = true;
  bool fail_attach = false;

private:
  bool
  send_render_packet_impl(const channel::SurfaceDescription &expected,
                          const wire::EnvelopeHeader &, std::vector<std::byte>,
                          std::vector<host::UniqueFd>) noexcept override {
    ++send_calls;
    if (reenter) {
      auto callback = std::move(reenter);
      callback();
    }
    return expected == description;
  }

public:
  std::size_t send_calls = 0;
  std::function<void()> reenter;
};

class MultiplexPort final : public channel::SurfaceSessionPort {
public:
  MultiplexPort() {
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
      if (!canonical.empty())
        canonical += ',';
      const auto name = "surface" + std::to_string(index);
      canonical += "\"" + name +
                   "\":{\"keyboardFocus\":false,\"maximumFramesPerSecond\":60,"
                   "\"maximumHeight\":64,\"maximumWidth\":128,"
                   "\"role\":\"desktop-overlay\"}";
    }
    canonical = '{' + canonical + '}';
  }

  std::optional<channel::SurfaceDescription>
  describe(std::string_view name) const noexcept override {
    ++describe_calls;
    for (std::size_t index = 0; index < endpoints.size(); ++index)
      if (name == "surface" + std::to_string(index))
        return description(index);
    return {};
  }

  bool attach(const channel::SurfaceDescription &expected,
              host::SurfaceEndpoint &candidate) noexcept override {
    const auto index =
        expected.key.id == 0 ? endpoints.size() : expected.key.id - 1;
    if (index >= endpoints.size() || expected != description(index) ||
        endpoints[index] != nullptr)
      return false;
    endpoints[index] = &candidate;
    ++attach_calls;
    return true;
  }

  bool detach(const channel::SurfaceDescription &expected,
              const host::SurfaceEndpoint &candidate) noexcept override {
    const auto index =
        expected.key.id == 0 ? endpoints.size() : expected.key.id - 1;
    if (index >= endpoints.size() || endpoints[index] != &candidate)
      return false;
    endpoints[index] = nullptr;
    ++detach_calls;
    return true;
  }

  bool arm_surface_intent(const channel::SurfaceDescription &,
                          std::uint64_t) noexcept override {
    return false;
  }

  void clear_surface_intent_eligibility(
      const channel::SurfaceDescription &) noexcept override {}

  permissions::ActivationBinding exact_binding = binding();
  mutable std::size_t describe_calls = 0;
  std::size_t attach_calls = 0;
  std::size_t detach_calls = 0;

private:
  channel::SurfaceDescription description(std::size_t index) const {
    const auto name = "surface" + std::to_string(index);
    return {.binding = exact_binding,
            .key = {.id = index + 1, .generation = exact_binding.generation},
            .session_nonce = 47,
            .plugin_id = "org.example.owner",
            .surface_name = name,
            .canonical_surfaces = canonical};
  }

  bool
  send_render_packet_impl(const channel::SurfaceDescription &expected,
                          const wire::EnvelopeHeader &, std::vector<std::byte>,
                          std::vector<host::UniqueFd>) noexcept override {
    return expected.key.id > 0 && expected.key.id <= endpoints.size();
  }

  std::array<const host::SurfaceEndpoint *, 8> endpoints{};
  std::string canonical;
};

class Clock final : public surface_host::MonotonicClock {
public:
  std::uint64_t now_nanoseconds() const override { return 1'000'000'000; }
};

void place(bridge::RemotePluginSurface &remote, QQuickWindow &window,
           qreal width = 64, qreal height = 32) {
  remote.setParentItem(window.contentItem());
  remote.setWidth(width);
  remote.setHeight(height);
}

bridge::PublishedSurfaceAttachment published(Port &port, QString key,
                                             qulonglong revision = 1) {
  return bridge::SurfaceEndpointOwnerTestAccess::published(
      std::move(key), port.description.binding, port.description.surface_name,
      revision);
}

void trusted_geometry_is_derived_and_retryable() {
  Port port;
  Clock clock;
  auto owner = bridge::SurfaceEndpointOwnerTestAccess::create(
      clock, port.description.binding, 1, port);
  const QString key = QStringLiteral("opaque-current-row");
  auto slot = published(port, key);
  bridge::RemotePluginSurface remote;
  remote.setWidth(64);
  remote.setHeight(32);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, slot, key, remote) ==
                  bridge::SurfaceEndpointAttachResult::not_ready &&
              bridge::SurfaceEndpointOwnerTestAccess::count(*owner) ==
                  0 &&
              port.describe_calls == 0 && port.attach_calls == 0);

  QQuickWindow window;
  place(remote, window);
  const auto geometry =
      bridge::SurfaceEndpointOwnerTestAccess::geometry(remote);
  OMARCHY_CHECK(geometry && geometry->logical_width == 64 &&
              geometry->logical_height == 32 && geometry->dpr_numerator > 0 &&
              geometry->dpr_denominator > 0);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, slot, key, remote) ==
                  bridge::SurfaceEndpointAttachResult::attached &&
              bridge::SurfaceEndpointOwnerTestAccess::count(*owner) ==
                  1 &&
              port.attach_calls == 1 && port.send_calls == 1);
}

void invalid_geometry_and_context_fail_without_ownership() {
  Port port;
  Clock clock;
  auto owner = bridge::SurfaceEndpointOwnerTestAccess::create(
      clock, port.description.binding, 1, port);
  const QString key = QStringLiteral("opaque-exact");
  auto slot = published(port, key);
  QQuickWindow window;
  bridge::RemotePluginSurface remote;
  place(remote, window, 64.5, 32);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, slot, key, remote) ==
              bridge::SurfaceEndpointAttachResult::rejected);
  remote.setWidth(std::numeric_limits<qreal>::quiet_NaN());
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, slot, key, remote) ==
              bridge::SurfaceEndpointAttachResult::rejected);
  remote.setWidth(4097);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, slot, key, remote) ==
              bridge::SurfaceEndpointAttachResult::rejected);
  remote.setWidth(0);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, slot, key, remote) ==
              bridge::SurfaceEndpointAttachResult::not_ready);
  remote.setWidth(64);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, slot, QStringLiteral("spoofed"), remote) ==
                  bridge::SurfaceEndpointAttachResult::rejected &&
              bridge::SurfaceEndpointOwnerTestAccess::count(*owner) ==
                  0);

  Port replacement(8);
  auto stale = bridge::SurfaceEndpointOwnerTestAccess::published(
      key, replacement.description.binding,
      replacement.description.surface_name, 2);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, stale, key, remote) ==
              bridge::SurfaceEndpointAttachResult::rejected);
  auto stale_revision = published(port, key, 2);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, stale_revision, key, remote) ==
              bridge::SurfaceEndpointAttachResult::rejected);
  auto zero_revision = published(port, key, 0);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, zero_revision, key, remote) ==
              bridge::SurfaceEndpointAttachResult::rejected);
}

void duplicate_key_and_cross_slot_remote_reuse_fail() {
  Port first;
  Clock clock;
  auto owner = bridge::SurfaceEndpointOwnerTestAccess::create(
      clock, first.description.binding, 1, first);
  QQuickWindow window;
  bridge::RemotePluginSurface first_remote;
  bridge::RemotePluginSurface second_remote;
  place(first_remote, window);
  place(second_remote, window);
  const QString first_key = QStringLiteral("opaque-first");
  const QString second_key = QStringLiteral("opaque-second");
  auto first_slot = published(first, first_key);
  auto second_slot =
      bridge::SurfaceEndpointOwnerTestAccess::published(
          second_key, first.description.binding, first.description.surface_name,
          1);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, first_slot, first_key, first_remote) ==
              bridge::SurfaceEndpointAttachResult::attached);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, first_slot, first_key, second_remote) ==
              bridge::SurfaceEndpointAttachResult::rejected);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, second_slot, second_key, first_remote) ==
                  bridge::SurfaceEndpointAttachResult::rejected &&
              first.attach_calls == 1);
}

void key_and_expanded_pixel_bounds_are_exact() {
  Clock clock;
  QQuickWindow window;
  bridge::RemotePluginSurface remote;
  place(remote, window);
  Port oversized_port;
  auto oversized_owner =
      bridge::SurfaceEndpointOwnerTestAccess::create(
          clock, oversized_port.description.binding, 1,
          oversized_port);
  const QString oversized_key(513, QLatin1Char('k'));
  auto oversized = published(oversized_port, oversized_key);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *oversized_owner, oversized, oversized_key, remote) ==
              bridge::SurfaceEndpointAttachResult::rejected);

  Port exact_port;
  auto exact_owner = bridge::SurfaceEndpointOwnerTestAccess::create(
      clock, exact_port.description.binding, 1, exact_port);
  bridge::RemotePluginSurface exact_remote;
  place(exact_remote, window);
  const QString exact_key(512, QLatin1Char('k'));
  auto exact = published(exact_port, exact_key);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *exact_owner, exact, exact_key, exact_remote) ==
              bridge::SurfaceEndpointAttachResult::attached);
  bridge::SurfaceEndpointOwnerTestAccess::close_all(*exact_owner);

  OMARCHY_CHECK(!bridge::SurfaceEndpointOwnerTestAccess::geometry(
              4096, 4096, 2.0));
}

void eighth_endpoint_is_accepted_and_ninth_is_rejected() {
  MultiplexPort port;
  Clock clock;
  auto owner = bridge::SurfaceEndpointOwnerTestAccess::create(
      clock, port.exact_binding, 1, port);
  QQuickWindow window;
  std::vector<std::unique_ptr<bridge::RemotePluginSurface>> remotes;
  for (std::size_t index = 0; index < 9; ++index) {
    auto remote = std::make_unique<bridge::RemotePluginSurface>();
    place(*remote, window);
    const auto name = "surface" + std::to_string(index);
    const auto key = QStringLiteral("opaque-") + QString::number(index);
    auto exact = bridge::SurfaceEndpointOwnerTestAccess::published(
        key, port.exact_binding, name, 1);
    const auto result =
        bridge::SurfaceEndpointOwnerTestAccess::attach(*owner, exact,
                                                                 key, *remote);
    OMARCHY_CHECK(result == (index < 8
                           ? bridge::SurfaceEndpointAttachResult::attached
                           : bridge::SurfaceEndpointAttachResult::rejected));
    remotes.push_back(std::move(remote));
  }
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::count(*owner) ==
                  8 &&
              port.attach_calls == 8);
  bridge::SurfaceEndpointOwnerTestAccess::close_all(*owner);
  OMARCHY_CHECK(port.detach_calls == 8);
}

void teardown_replacement_and_remote_destruction_are_exact() {
  Clock clock;
  Port first(7);
  auto owner = bridge::SurfaceEndpointOwnerTestAccess::create(
      clock, first.description.binding, 1, first);
  QQuickWindow window;
  auto remote = std::make_unique<bridge::RemotePluginSurface>();
  place(*remote, window);
  const QString key = QStringLiteral("opaque-replaced");
  auto first_slot = published(first, key);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, first_slot, key, *remote) ==
              bridge::SurfaceEndpointAttachResult::attached);
  bridge::SurfaceEndpointOwnerTestAccess::close_all(*owner);
  OMARCHY_CHECK(first.detach_calls == 1 &&
              bridge::SurfaceEndpointOwnerTestAccess::count(*owner) ==
                  0);

  Port second(8);
  owner = bridge::SurfaceEndpointOwnerTestAccess::create(
      clock, second.description.binding, 2, second);
  auto second_slot = published(second, key, 2);
  remote = std::make_unique<bridge::RemotePluginSurface>();
  place(*remote, window);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, second_slot, key, *remote) ==
              bridge::SurfaceEndpointAttachResult::attached);
  remote.reset();
  OMARCHY_CHECK(second.detach_calls == 1);
  auto replacement_remote = std::make_unique<bridge::RemotePluginSurface>();
  place(*replacement_remote, window);
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, second_slot, key, *replacement_remote) ==
                  bridge::SurfaceEndpointAttachResult::attached &&
              bridge::SurfaceEndpointOwnerTestAccess::count(*owner) ==
                  1);
  bridge::SurfaceEndpointOwnerTestAccess::close_all(*owner);
  OMARCHY_CHECK(second.detach_calls == 2 &&
              bridge::SurfaceEndpointOwnerTestAccess::count(*owner) ==
                  0);
}

void teardown_reentry_preserves_endpoint_lifetime() {
  for (const bool destroy_remote : {true, false}) {
    Clock clock;
    Port port;
    auto owner = bridge::SurfaceEndpointOwnerTestAccess::create(
        clock, port.description.binding, 1, port);
    QQuickWindow window;
    auto remote = std::make_unique<bridge::RemotePluginSurface>();
    bridge::RemotePluginSurface replacement;
    place(*remote, window);
    place(replacement, window);
    const QString key = destroy_remote ? QStringLiteral("opaque-remote-reentry")
                                      : QStringLiteral("opaque-close-all-reentry");
    auto slot = published(port, key);
    OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
                    *owner, slot, key, *remote) ==
                  bridge::SurfaceEndpointAttachResult::attached);

    bridge::SurfaceEndpointAttachResult retry =
        bridge::SurfaceEndpointAttachResult::attached;
    port.reenter = [&] {
      auto duplicate = published(port, key);
      retry = bridge::SurfaceEndpointOwnerTestAccess::attach(
          *owner, duplicate, key, replacement);
      if (destroy_remote)
        bridge::SurfaceEndpointOwnerTestAccess::close_all(*owner);
    };
    if (destroy_remote)
      remote.reset();
    else
      bridge::SurfaceEndpointOwnerTestAccess::close_all(*owner);
    OMARCHY_CHECK(retry == bridge::SurfaceEndpointAttachResult::rejected &&
                  port.detach_calls == 1 &&
                  bridge::SurfaceEndpointOwnerTestAccess::count(*owner) ==
                      (destroy_remote ? 1 : 0));
    bridge::SurfaceEndpointOwnerTestAccess::close_all(*owner);
    OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::count(*owner) == 0 &&
                  port.detach_calls == 1);
  }
}

void failed_endpoint_attach_is_transactional() {
  Port port;
  port.fail_attach = true;
  Clock clock;
  auto owner = bridge::SurfaceEndpointOwnerTestAccess::create(
      clock, port.description.binding, 1, port);
  QQuickWindow window;
  bridge::RemotePluginSurface remote;
  place(remote, window);
  const QString key = QStringLiteral("opaque-fault");
  auto slot = published(port, key);
  OMARCHY_CHECK(
      bridge::SurfaceEndpointOwnerTestAccess::attach(*owner, slot,
                                                               key, remote) ==
              bridge::SurfaceEndpointAttachResult::rejected &&
          bridge::SurfaceEndpointOwnerTestAccess::count(*owner) == 0);
  port.fail_attach = false;
  OMARCHY_CHECK(bridge::SurfaceEndpointOwnerTestAccess::attach(
              *owner, slot, key, remote) ==
              bridge::SurfaceEndpointAttachResult::attached);
}

} // namespace

void run_surface_endpoint_owner_tests() {
  trusted_geometry_is_derived_and_retryable();
  invalid_geometry_and_context_fail_without_ownership();
  duplicate_key_and_cross_slot_remote_reuse_fail();
  key_and_expanded_pixel_bounds_are_exact();
  eighth_endpoint_is_accepted_and_ninth_is_rejected();
  teardown_replacement_and_remote_destruction_are_exact();
  teardown_reentry_preserves_endpoint_lifetime();
  failed_endpoint_attach_is_transactional();
}
