#include "../../tests/support/test_assert.hpp"
#include "MultiSurfaceRouter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace runtime = omarchy::plugin_runtime;
namespace host = runtime::host_session;
namespace surface = runtime::surface;
namespace wire = omarchy::plugin::wire;

namespace {

using omarchy::plugin_runtime::test_support::throws_exception;
using omarchy::plugin_runtime::test_support::exit_assertions::require;

constexpr std::uint64_t kGeneration = 41;

struct Endpoint final : host::SurfaceEndpoint {
  bool accept = true;
  std::size_t deliveries = 0;
  std::optional<host::OwnedAuthenticatedRenderMessage> last;

  bool receive(host::OwnedAuthenticatedRenderMessage message) override {
    ++deliveries;
    last.emplace(std::move(message));
    return accept;
  }
};

host::OwnedAuthenticatedRenderMessage message(
    std::uint64_t generation, std::uint64_t correlation = 0,
    std::optional<surface::SurfaceKey> key = std::nullopt) {
  return {.launch_generation = generation,
          .message_type = 0x2020,
          .correlation = correlation,
          .surface = key,
          .payload = {std::byte{0x2a}},
          .descriptors = {}};
}

int descriptor_message(host::OwnedAuthenticatedRenderMessage &value) {
  int descriptors[2] = {-1, -1};
  OMARCHY_CHECK_WITH(require, ::pipe2(descriptors, O_CLOEXEC) == 0);
  ::close(descriptors[1]);
  value.descriptors.emplace_back(descriptors[0]);
  return descriptors[0];
}

bool closed(int descriptor) {
  errno = 0;
  return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
}

} // namespace

int main() {
  OMARCHY_CHECK_WITH(require, throws_exception<std::invalid_argument>([&] {
    host::MultiSurfaceRouter invalid(0);
  }));

  host::MultiSurfaceRouter router(kGeneration);
  Endpoint first;
  Endpoint second;
  const surface::SurfaceKey first_key{.id = 1, .generation = kGeneration};
  const surface::SurfaceKey second_key{.id = 2, .generation = kGeneration};
  const std::array<std::uint64_t, 2> first_correlations{101, 102};
  const std::array<std::uint64_t, 1> second_correlations{201};
  const std::array<std::uint64_t, 1> duplicate_existing{101};
  const std::array<std::uint64_t, 2> duplicate_local{201, 201};
  const std::array<std::uint64_t, 1> zero_correlation{0};

  OMARCHY_CHECK_WITH(require, router.launchGeneration() == kGeneration);
  OMARCHY_CHECK_WITH(require, router.attach(first_key, first_correlations, first) ==
             host::AttachResult::attached);
  OMARCHY_CHECK_WITH(require, router.attach(first_key, {}, second) ==
             host::AttachResult::duplicate_surface);
  OMARCHY_CHECK_WITH(require, router.attach(second_key, duplicate_existing, second) ==
             host::AttachResult::duplicate_correlation);
  OMARCHY_CHECK_WITH(require, router.attach(second_key, duplicate_local, second) ==
             host::AttachResult::invalid_registration);
  OMARCHY_CHECK_WITH(require, router.attach(second_key, zero_correlation, second) ==
             host::AttachResult::invalid_registration);
  OMARCHY_CHECK_WITH(require, router.attach({.id = 0, .generation = kGeneration}, {}, second) ==
             host::AttachResult::invalid_registration);
  OMARCHY_CHECK_WITH(require, router.attach({.id = 2, .generation = kGeneration + 1}, {}, second) ==
             host::AttachResult::invalid_registration);
  std::array<std::uint64_t,
             host::MultiSurfaceRouter::kMaximumCorrelationsPerEndpoint + 1>
      excessive{};
  for (std::size_t index = 0; index < excessive.size(); ++index) {
    excessive[index] = 300 + index;
  }
  OMARCHY_CHECK_WITH(require, router.attach(second_key, excessive, second) ==
             host::AttachResult::invalid_registration);
  OMARCHY_CHECK_WITH(require, router.attach(second_key, second_correlations, second) ==
             host::AttachResult::attached);

  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration, 0, first_key)) ==
             host::RouteResult::delivered);
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration, 201)) ==
             host::RouteResult::delivered);
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration, 102, first_key)) ==
             host::RouteResult::delivered);
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration, 201, first_key)) ==
             host::RouteResult::conflicting_destination);
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration)) ==
             host::RouteResult::missing_destination);
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration, 999)) ==
             host::RouteResult::unknown_correlation);
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration, 0,
                              surface::SurfaceKey{.id = 999,
                                                  .generation = kGeneration})) ==
             host::RouteResult::unknown_surface);
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration - 1, 101)) ==
             host::RouteResult::stale_generation);
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration, 0,
                              surface::SurfaceKey{.id = 1,
                                                  .generation = kGeneration - 1})) ==
             host::RouteResult::stale_generation);
  OMARCHY_CHECK_WITH(require, first.deliveries == 2 && second.deliveries == 1);

  auto owned = message(kGeneration, 101, first_key);
  owned.message_type = 0x2010;
  owned.payload = {std::byte{0x10}, std::byte{0x20}};
  const int delivered_fd = descriptor_message(owned);
  OMARCHY_CHECK_WITH(require, router.route(std::move(owned)) == host::RouteResult::delivered &&
             first.last && first.last->message_type == 0x2010 &&
             first.last->payload ==
                 std::vector<std::byte>({std::byte{0x10}, std::byte{0x20}}) &&
             first.last->descriptors.size() == 1 &&
             first.last->descriptors.front().get() == delivered_fd);
  first.last.reset();
  OMARCHY_CHECK_WITH(require, closed(delivered_fd));

  auto unknown_owned = message(
      kGeneration, 0,
      surface::SurfaceKey{.id = 999, .generation = kGeneration});
  const int unknown_fd = descriptor_message(unknown_owned);
  const auto unknown_result = router.route(std::move(unknown_owned));
  OMARCHY_CHECK_WITH(require, unknown_result == host::RouteResult::unknown_surface &&
             closed(unknown_fd));
  auto conflict_owned = message(kGeneration, 201, first_key);
  const int conflict_fd = descriptor_message(conflict_owned);
  const auto conflict_result = router.route(std::move(conflict_owned));
  OMARCHY_CHECK_WITH(require, conflict_result == host::RouteResult::conflicting_destination &&
             closed(conflict_fd));

  first.accept = false;
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration, 101)) ==
             host::RouteResult::endpoint_rejected);

  OMARCHY_CHECK_WITH(require, !router.detach(first_key, second));
  OMARCHY_CHECK_WITH(require, !router.detach({.id = 1, .generation = kGeneration - 1}, first));
  OMARCHY_CHECK_WITH(require, router.detach(first_key, first));
  OMARCHY_CHECK_WITH(require, router.detach(first_key, first));
  OMARCHY_CHECK_WITH(require, router.size() == 1);
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration, 101)) ==
             host::RouteResult::unknown_correlation);

  first.accept = true;
  OMARCHY_CHECK_WITH(require, router.attach(first_key, first_correlations, first) ==
             host::AttachResult::attached);
  OMARCHY_CHECK_WITH(require, router.route(message(kGeneration, 101, first_key)) ==
             host::RouteResult::delivered);

  router.detachAll();
  OMARCHY_CHECK_WITH(require, router.size() == 0);

  std::vector<Endpoint> endpoints(wire::kMaximumPluginSurfaces);
  for (std::size_t index = 0; index < endpoints.size(); ++index) {
    const surface::SurfaceKey key{.id = 1000 + index,
                                  .generation = kGeneration};
    OMARCHY_CHECK_WITH(require, router.attach(key, {}, endpoints[index]) ==
               host::AttachResult::attached);
  }
  Endpoint overflow;
  OMARCHY_CHECK_WITH(require, router.attach({.id = 9999, .generation = kGeneration}, {}, overflow) ==
             host::AttachResult::capacity_exceeded);

  std::cout << "MultiSurfaceRouter tests passed\n";
  return 0;
}
