#include "../../tests/support/child_process.hpp"
#include "../../tests/support/permission_fixture.hpp"
#include "../../tests/support/authority_fixture.hpp"
#include "../../tests/support/test_assert.hpp"

#include "authority_store.hpp"
#include "authority_store_test_access.hpp"
#include "authority_snapshot_codec.hpp"

#include "manifest_contract.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace host = omarchy::plugin_runtime::host_session;
namespace policy = omarchy::plugin_runtime::policy;
namespace permissions = omarchy::plugins::permissions;
namespace definitions = omarchy::plugins::definitions;
namespace manifest = omarchy::plugins::manifest;
namespace test_support = omarchy::plugin_runtime::test_support;

class FenceProbe final : public host::AuthorityFenceObserver {
public:
  void live_generation_closed() noexcept override {
    calls.fetch_add(1, std::memory_order_release);
  }

  std::atomic<unsigned> calls = 0;
};

namespace allocation_failure {
thread_local bool armed = false;
thread_local bool fired = false;
thread_local std::weak_ptr<host::LiveGenerationState> live;
thread_local permissions::ActivationBinding binding;
} // namespace allocation_failure

void *operator new(std::size_t size) {
  if (allocation_failure::armed) {
    const auto live = allocation_failure::live.lock();
    if (live && !live->current(allocation_failure::binding)) {
      allocation_failure::armed = false;
      allocation_failure::fired = true;
      throw std::bad_alloc();
    }
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size))
    return memory;
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }
[[gnu::noinline]] void operator delete(void *memory) noexcept {
  std::free(memory);
}
[[gnu::noinline]] void operator delete[](void *memory) noexcept {
  std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept {
  ::operator delete(memory);
}
void operator delete[](void *memory, std::size_t) noexcept {
  ::operator delete[](memory);
}

namespace {
constexpr std::string_view kPlugin = "org.example.authority";

std::string hex(char value) { return std::string(64, value); }

definitions::CapabilityDefinition dynamic_definition(std::string_view name,
                                                     std::string_view operation,
                                                     char digest_byte) {
  auto definition = test_support::reviewed_service_definition(
      name, "Fetch selected data", "Uses an exact reviewed adapter", digest_byte);
  definition.operations.insert(
      {.name = definitions::Name(operation),
       .label = definitions::Label("Read selected data")});
  return definition;
}

using omarchy::plugin_runtime::test_support::require;

bool activatable(const host::GrantResolution &resolution) {
  return resolution.snapshot.has_value() &&
         resolution.status == host::GrantStatus::activatable;
}

bool unavailable(const host::GrantResolution &resolution) {
  return !resolution.snapshot &&
         resolution.status == host::GrantStatus::unavailable;
}

void snapshot_format_has_one_request_fingerprint() {
  namespace codec = host::authority_snapshot_codec;
  policy::GrantSnapshot snapshot{
      .binding = {.plugin = permissions::PluginId(kPlugin),
                  .revision = permissions::Digest(hex('a')),
                  .policy_fingerprint = permissions::Digest(hex('b')),
                  .generation = 7},
      .dynamic_grants = {}};
  const auto golden = std::string("OMGRANT\x03\0\x15", 10) +
                      std::string(kPlugin) + std::string("\0\x40", 2) + hex('a') +
                      std::string("\0\x40", 2) + hex('b') +
                      std::string("\0\0\0\0\0\0\0\x07\0", 9);
  std::vector<std::byte> encoded;
  OMARCHY_CHECK(codec::encode_snapshot(snapshot, encoded) &&
              std::ranges::equal(encoded, std::as_bytes(std::span(golden))));
  policy::GrantSnapshot decoded;
  OMARCHY_CHECK(codec::decode_snapshot(encoded, decoded) &&
              decoded.binding == snapshot.binding && decoded.dynamic_grants.empty());
  for (std::size_t size = 0; size < encoded.size(); ++size)
    OMARCHY_CHECK(!codec::decode_snapshot(std::span(encoded).first(size), decoded));
  for (const auto version : {0, 1, 2, 4, 255}) {
    auto changed = encoded;
    changed[7] = static_cast<std::byte>(version);
    OMARCHY_CHECK(!codec::decode_snapshot(changed, decoded));
  }
  encoded.push_back(std::byte{0});
  OMARCHY_CHECK(!codec::decode_snapshot(encoded, decoded));
}

bool prepare_and_commit_live(
    host::AuthorityStore &store,
    const permissions::ActivationBinding &binding,
    const std::shared_ptr<host::LiveGenerationState> &live) {
  auto prepared = store.prepare_live_activation(binding, live);
  return prepared &&
         store.commit_live_activation(std::move(*prepared), binding, live);
}

std::shared_ptr<host::LiveGenerationState> committed_live(
    host::AuthorityStore &store, const permissions::ActivationBinding &binding) {
  auto live = std::make_shared<host::LiveGenerationState>(binding);
  OMARCHY_CHECK(prepare_and_commit_live(store, binding, live));
  return live;
}

struct Fixture : omarchy::plugin_runtime::test_support::AuthorityFixture {
  explicit Fixture(bool open_store = true)
      : AuthorityFixture(kPlugin, open_store) {}
};

struct Review {
  host::VerifiedRevision verified;
  policy::GrantSnapshot snapshot;
};

Review
review(std::uint64_t generation, char revision = 'a', bool required = true,
              permissions::GrantState state = permissions::GrantState::granted) {
  manifest::ManifestV2 manifest;
  manifest.id = kPlugin;
  const auto registry = test_support::packaged_registry();
  manifest.requests.push_back(test_support::capability_request(
      registry, "notifications.send", "{\"categories\":[\"status\"]}", required));
  auto snapshot = test_support::permission_snapshot(registry, manifest, hex(revision), generation, state);
  return {.verified = {.manifest = std::move(manifest),
                       .tree_sha256 = hex(revision),
                       .request_sha256 = std::string(
                           snapshot.binding.policy_fingerprint.view())},
          .snapshot = std::move(snapshot)};
}

Review dynamic_review(
    Fixture &fixture, std::uint64_t generation,
    permissions::GrantState state = permissions::GrantState::granted) {
  OMARCHY_CHECK(fixture.definitions.install(
              dynamic_definition("zeta.fetch", "zeta.read", 'd'),
              2) &&
              fixture.definitions.install(
                  dynamic_definition("alpha.fetch", "alpha.read", 'e'),
                  3));
  manifest::ManifestV2 manifest;
  manifest.id = kPlugin;
  for (const auto pair : {std::pair{"zeta.fetch", "zeta.read"},
                          std::pair{"alpha.fetch", "alpha.read"}}) {
    const auto installed = fixture.definitions.find(pair.first);
    OMARCHY_CHECK(installed.has_value());
    manifest.requests.push_back(
        {.capability = pair.first,
         .reason = "status",
         .canonical_scope = "wide",
         .definition_generation = installed->generation,
         .definition_digest = std::string(installed->digest.view()),
         .operations = {pair.second},
         .required = true});
  }
  auto snapshot = test_support::permission_snapshot(
      fixture.definitions, manifest, hex('d'), generation, state);
  return {.verified = {.manifest = std::move(manifest),
                       .tree_sha256 = hex('d'),
                       .request_sha256 = std::string(
                           snapshot.binding.policy_fingerprint.view())},
          .snapshot = std::move(snapshot)};
}

host::AuthorityMutationResult publish(Fixture &fixture, const Review &value,
                                      std::uint64_t sequence) {
  return fixture.store->publish_candidate(
      value.verified, value.snapshot, sequence, fixture.definitions);
}

bool publish_and_promote(Fixture &fixture, const Review &value) {
  return publish(fixture, value, 0) ==
             host::AuthorityMutationResult::applied &&
         fixture.store->promote_candidate(value.snapshot.binding, 1) ==
             host::AuthorityMutationResult::applied;
}

void reopen(Fixture &fixture) {
  fixture.store.reset();
  fixture.store = host::AuthorityStore::open(fixture.root.get(), ::getuid(),
                                             permissions::PluginId(kPlugin));
  OMARCHY_CHECK(fixture.store != nullptr);
}

template <typename Mutation>
void crash_during(Fixture &fixture, host::AuthorityCrashPoint point,
                  Mutation mutation) {
  // This models loss of the sole mutating process after an exact syscall
  // boundary. It verifies restart state, not storage-device power-loss rules.
  fixture.store.reset();
  test_support::expect_child_exit(86, [&] {
    auto store = host::AuthorityStore::open(fixture.root.get(), ::getuid(),
                                            permissions::PluginId(kPlugin));
    if (!store)
      ::_exit(87);
    host::AuthorityStoreTestAccess::crash_at(point);
    mutation(*store);
    ::_exit(88);
  });
  reopen(fixture);
}

void require_complete_active(Fixture &fixture, const Review &expected) {
  const auto view = fixture.store->read_authority_view();
  OMARCHY_CHECK(view && view->authority_slots.active && view->active &&
              view->authority_slots.active->generation ==
                  expected.snapshot.binding.generation &&
              view->active->binding == expected.snapshot.binding &&
              activatable(fixture.store->resolve(
                  kPlugin, expected.snapshot.binding.revision.view())));
}

void require_no_legacy_writes(const Fixture &fixture) {
  for (const auto &entry : std::filesystem::directory_iterator(fixture.path)) {
    const auto name = entry.path().filename().string();
    OMARCHY_CHECK(name != "slots" && !name.starts_with("grant-") &&
                  !name.starts_with(".grant.") && !name.starts_with(".slots."));
  }
}

constexpr std::array transaction_crash_points{
    host::AuthorityCrashPoint::transaction_begin,
    host::AuthorityCrashPoint::revision_rows,
    host::AuthorityCrashPoint::authority_head,
    host::AuthorityCrashPoint::before_commit,
    host::AuthorityCrashPoint::after_commit,
};

void roundtrip_and_lifecycle() {
  Fixture fixture;
  auto slots = fixture.store->read_slots();
  OMARCHY_CHECK(slots && slots->sequence == 0 &&
              slots->generation_high_watermark == 0);

  auto first = review(1);
  const auto first_publish = publish(fixture, first, 0);
  require(first_publish == host::AuthorityMutationResult::applied,
          "candidate publication failed: " +
              std::to_string(static_cast<int>(first_publish)));
  OMARCHY_CHECK(unavailable(fixture.store->resolve(kPlugin, hex('a'))));
  OMARCHY_CHECK(fixture.store->promote_candidate(first.snapshot.binding, 1) ==
              host::AuthorityMutationResult::applied);
  auto resolved = fixture.store->resolve(kPlugin, hex('a'));
  OMARCHY_CHECK(activatable(resolved) &&
              resolved.snapshot->binding == first.snapshot.binding);
  OMARCHY_CHECK(unavailable(
              fixture.store->resolve("org.example.other", hex('a'))));

  auto second = review(2, 'b', false, permissions::GrantState::denied);
  OMARCHY_CHECK(publish(fixture, second, 1) ==
              host::AuthorityMutationResult::stale_sequence);
  OMARCHY_CHECK(publish(fixture, second, 2) ==
              host::AuthorityMutationResult::applied);
  OMARCHY_CHECK(fixture.store->promote_candidate(second.snapshot.binding, 3) ==
              host::AuthorityMutationResult::applied);
  slots = fixture.store->read_slots();
  OMARCHY_CHECK(slots && slots->generation_high_watermark == 2 && !slots->candidate);
  OMARCHY_CHECK(publish(fixture, second, 4) ==
              host::AuthorityMutationResult::invalid);
}

void live_activation_binding_and_revocation() {
  Fixture fixture;
  auto first = review(1);
  OMARCHY_CHECK(publish_and_promote(fixture, first));

  auto live =
      std::make_shared<host::LiveGenerationState>(first.snapshot.binding);
  auto wrong_binding = first.snapshot.binding;
  wrong_binding.revision = permissions::Digest(hex('f'));
  auto wrong_live = std::make_shared<host::LiveGenerationState>(wrong_binding);
  OMARCHY_CHECK(!fixture.store->prepare_live_activation(first.snapshot.binding, {}));
  OMARCHY_CHECK(!fixture.store->prepare_live_activation(wrong_binding, wrong_live));
  OMARCHY_CHECK(
      !fixture.store->prepare_live_activation(first.snapshot.binding,
                                              wrong_live));

  auto older =
      fixture.store->prepare_live_activation(first.snapshot.binding, live);
  auto newest =
      fixture.store->prepare_live_activation(first.snapshot.binding, live);
  OMARCHY_CHECK(older && newest &&
              !fixture.store->commit_live_activation(
                  std::move(*older), first.snapshot.binding, live) &&
              fixture.store->commit_live_activation(
                  std::move(*newest), first.snapshot.binding, live) &&
              !fixture.store->commit_live_activation(
                  std::move(*newest), first.snapshot.binding, live));

  auto wrong_expected_binding =
      fixture.store->prepare_live_activation(first.snapshot.binding, live);
  OMARCHY_CHECK(wrong_expected_binding &&
              !fixture.store->commit_live_activation(
                  std::move(*wrong_expected_binding), wrong_binding, live));
  auto wrong_expected_live =
      fixture.store->prepare_live_activation(first.snapshot.binding, live);
  OMARCHY_CHECK(wrong_expected_live &&
              !fixture.store->commit_live_activation(
                  std::move(*wrong_expected_live), first.snapshot.binding,
                  wrong_live));

  {
    Fixture other;
    auto other_first = review(1);
    OMARCHY_CHECK(publish_and_promote(other, other_first));
    auto cross_store =
        fixture.store->prepare_live_activation(first.snapshot.binding, live);
    OMARCHY_CHECK(cross_store &&
                !other.store->commit_live_activation(
                    std::move(*cross_store), first.snapshot.binding, live));
  }

  const auto mutation_invalidates = [&](auto mutation,
                                        std::string_view message) {
    auto prepared =
        fixture.store->prepare_live_activation(first.snapshot.binding, live);
    OMARCHY_CHECK(prepared.has_value());
    mutation();
    require(!fixture.store->commit_live_activation(
                std::move(*prepared), first.snapshot.binding, live),
            message);
  };
  mutation_invalidates(
      [&] {
        (void)publish(fixture, first, UINT64_MAX);
      },
      "publish attempt did not invalidate a prepared binding");
  mutation_invalidates(
      [&] {
        (void)fixture.store->promote_candidate(first.snapshot.binding,
                                               UINT64_MAX);
      },
      "promote attempt did not invalidate a prepared binding");
  mutation_invalidates(
      [&] {
        (void)host::AuthorityStoreTestAccess::revoke_active(
            *fixture.store, first.snapshot.dynamic_grants[0].request.definition, UINT64_MAX);
      },
      "revoke attempt did not invalidate a prepared binding");

  {
    Fixture successful;
    auto active = review(1);
    auto candidate = review(2, 'b');
    OMARCHY_CHECK(publish_and_promote(successful, active));
    auto active_live =
        std::make_shared<host::LiveGenerationState>(active.snapshot.binding);
    auto before_publish = successful.store->prepare_live_activation(
        active.snapshot.binding, active_live);
    OMARCHY_CHECK(before_publish &&
                publish(successful, candidate, 2) ==
                    host::AuthorityMutationResult::applied &&
                !successful.store->commit_live_activation(
                    std::move(*before_publish), active.snapshot.binding,
                    active_live));
    auto before_promote = successful.store->prepare_live_activation(
        active.snapshot.binding, active_live);
    OMARCHY_CHECK(before_promote &&
                successful.store->promote_candidate(candidate.snapshot.binding,
                                                     3) ==
                    host::AuthorityMutationResult::applied &&
                !successful.store->commit_live_activation(
                    std::move(*before_promote), active.snapshot.binding,
                    active_live));
  }

  OMARCHY_CHECK(prepare_and_commit_live(*fixture.store, first.snapshot.binding, live));
  OMARCHY_CHECK(prepare_and_commit_live(*fixture.store, first.snapshot.binding, live) &&
              live->current(first.snapshot.binding));

  auto replacement =
      std::make_shared<host::LiveGenerationState>(first.snapshot.binding);
  OMARCHY_CHECK(prepare_and_commit_live(*fixture.store, first.snapshot.binding, replacement) &&
              !live->current(first.snapshot.binding) &&
              replacement->current(first.snapshot.binding));

  auto second = review(2, 'b');
  OMARCHY_CHECK(publish(fixture, second, 2) ==
                  host::AuthorityMutationResult::applied &&
              replacement->current(first.snapshot.binding));
  auto third = review(3, 'c');
  OMARCHY_CHECK(publish(fixture, third, 3) ==
                  host::AuthorityMutationResult::applied &&
              replacement->current(first.snapshot.binding));
  OMARCHY_CHECK(fixture.store->promote_candidate(third.snapshot.binding, 4) ==
                  host::AuthorityMutationResult::applied &&
              !replacement->current(first.snapshot.binding));

  auto promoted = committed_live(*fixture.store, third.snapshot.binding);
  fixture.store.reset();
  OMARCHY_CHECK(!promoted->current(third.snapshot.binding));
}

void mutation_epoch_saturates_fail_closed() {
  Fixture fixture;
  auto active = review(1);
  OMARCHY_CHECK(publish_and_promote(fixture, active));
  const auto before = fixture.store->read_slots();
  auto live =
      std::make_shared<host::LiveGenerationState>(active.snapshot.binding);
  host::AuthorityStoreTestAccess::set_mutation_epoch(*fixture.store,
                                                     UINT64_MAX - 1);
  auto last = fixture.store->prepare_live_activation(active.snapshot.binding,
                                                     live);
  OMARCHY_CHECK(last &&
              !fixture.store->commit_live_activation(
                  std::move(*last), active.snapshot.binding, live) &&
              !fixture.store->prepare_live_activation(active.snapshot.binding,
                                                      live) &&
              publish(fixture, active, UINT64_MAX) ==
                  host::AuthorityMutationResult::poisoned &&
              fixture.store->promote_candidate(active.snapshot.binding,
                                               UINT64_MAX) ==
                  host::AuthorityMutationResult::poisoned &&
              host::AuthorityStoreTestAccess::revoke_active(
                  *fixture.store, active.snapshot.dynamic_grants[0].request.definition,
                  UINT64_MAX)
                      .status == host::AuthorityMutationResult::poisoned &&
              fixture.store->read_slots() == before);
}

void live_effect_transitions_drain_before_authority_changes() {
  using namespace std::chrono_literals;
  enum class Transition { rebind, promote, revoke };
  for (const auto transition : {Transition::rebind, Transition::promote,
                               Transition::revoke}) {
    Fixture fixture;
    auto value = review(1, 'a', transition != Transition::revoke);
    OMARCHY_CHECK(publish_and_promote(fixture, value));
    auto second = review(2, 'b');
    if (transition == Transition::promote)
      OMARCHY_CHECK(publish(fixture, second, 2) ==
          host::AuthorityMutationResult::applied);
    auto live = committed_live(*fixture.store, value.snapshot.binding);
    auto effect = live->acquire_effect(value.snapshot.binding);
    OMARCHY_CHECK(effect.has_value());
    auto replacement =
        std::make_shared<host::LiveGenerationState>(value.snapshot.binding);
    FenceProbe fence;
    std::atomic<bool> finished = false;
    bool applied = false;
    std::thread mutator([&] {
      switch (transition) {
      case Transition::rebind:
        applied = prepare_and_commit_live(*fixture.store, value.snapshot.binding,
                                          replacement);
        break;
      case Transition::promote:
        applied = fixture.store->promote_candidate(second.snapshot.binding, 3) ==
                  host::AuthorityMutationResult::applied;
        break;
      case Transition::revoke:
        applied = host::AuthorityStoreTestAccess::revoke_active(
            *fixture.store, value.snapshot.dynamic_grants[0].request.definition,
            2, &fence).status == host::AuthorityMutationResult::applied;
        break;
      }
      finished.store(true, std::memory_order_release);
    });
    const auto fenced = [&] {
      return live->generation() == 0 &&
             (transition != Transition::revoke ||
              fence.calls.load(std::memory_order_acquire) == 1);
    };
    for (int attempt = 0; attempt < 200 && !fenced(); ++attempt)
      std::this_thread::sleep_for(1ms);
    OMARCHY_CHECK(fenced() && !finished.load(std::memory_order_acquire));
    if (transition == Transition::revoke)
      OMARCHY_CHECK(!fixture.store->read_authority_view());
    else
      OMARCHY_CHECK(!fixture.store->read_slots());
    effect.reset();
    mutator.join();
    OMARCHY_CHECK(applied);
  }
}

void reentrant_effect_revoke_poisoned_without_durable_change() {
  Fixture fixture;
  auto value = review(1, 'a', false);
  OMARCHY_CHECK(publish_and_promote(fixture, value));
  const auto before = fixture.store->read_slots();
  auto live = committed_live(*fixture.store, value.snapshot.binding);
  auto effect = live->acquire_effect(value.snapshot.binding);
  OMARCHY_CHECK(effect.has_value());
  const auto result = host::AuthorityStoreTestAccess::revoke_active(
      *fixture.store, value.snapshot.dynamic_grants[0].request.definition, 2);
  OMARCHY_CHECK(result.status == host::AuthorityMutationResult::reentrant_effect &&
              !live->current(value.snapshot.binding) &&
              !fixture.store->read_slots());
  effect.reset();
  reopen(fixture);
  OMARCHY_CHECK(fixture.store->read_slots() == before);
}

void promotion_revokes_before_failed_replacement() {
  Fixture fixture;
  auto first = review(1);
  OMARCHY_CHECK(publish_and_promote(fixture, first));
  auto live = committed_live(*fixture.store, first.snapshot.binding);
  auto second = review(2, 'b');
  OMARCHY_CHECK(publish(fixture, second, 2) ==
              host::AuthorityMutationResult::applied);
  const auto before = fixture.store->read_slots();
  OMARCHY_CHECK(::chmod(fixture.path.c_str(), 0500) == 0);
  const auto promoted =
      fixture.store->promote_candidate(second.snapshot.binding, 3);
  OMARCHY_CHECK(::chmod(fixture.path.c_str(), 0700) == 0);
  OMARCHY_CHECK(promoted == host::AuthorityMutationResult::io_error &&
              !live->current(first.snapshot.binding) &&
              !fixture.store->read_slots() &&
              unavailable(fixture.store->resolve(kPlugin, hex('a'))));
  reopen(fixture);
  OMARCHY_CHECK(fixture.store->read_slots() == before);
}

void exact_builtin_revoke_rebases_and_invalidates_candidate() {
  Fixture fixture;
  auto first = review(1);
  OMARCHY_CHECK(publish_and_promote(fixture, first));
  auto live = committed_live(*fixture.store, first.snapshot.binding);

  auto pending = review(2, 'b', false);
  OMARCHY_CHECK(publish(fixture, pending, 2) ==
              host::AuthorityMutationResult::applied);
  const auto revoked = host::AuthorityStoreTestAccess::revoke_active(
      *fixture.store, first.snapshot.dynamic_grants[0].request.definition, 3);
  OMARCHY_CHECK(revoked.status == host::AuthorityMutationResult::applied &&
              revoked.binding && revoked.binding->generation == 3 &&
              revoked.binding->revision == first.snapshot.binding.revision &&
              !revoked.activatable && !live->current(first.snapshot.binding));
  const auto view = fixture.store->read_authority_view();
  auto revoked_live =
      std::make_shared<host::LiveGenerationState>(*revoked.binding);
  OMARCHY_CHECK(
      view && view->active && !view->authority_slots.candidate &&
              view->authority_slots.generation_high_watermark == 3 &&
              view->active->binding == *revoked.binding &&
          view->active->dynamic_grants[0].grant.state == permissions::GrantState::revoked &&
              view->active->dynamic_grants[0].grant.epoch == 3 &&
              fixture.store->resolve(kPlugin, hex('a')).status ==
                  host::GrantStatus::permission_disabled &&
              !fixture.store->prepare_live_activation(*revoked.binding,
                                                      revoked_live));
  OMARCHY_CHECK(host::AuthorityStoreTestAccess::revoke_active(
              *fixture.store, first.snapshot.dynamic_grants[0].request.definition, 4)
              .status == host::AuthorityMutationResult::invalid);

  Fixture optional;
  auto optional_value = review(1, 'a', false);
  OMARCHY_CHECK(publish_and_promote(optional, optional_value));
  const auto stale = host::AuthorityStoreTestAccess::revoke_active(
      *optional.store, optional_value.snapshot.dynamic_grants[0].request.definition, 1);
  OMARCHY_CHECK(stale.status == host::AuthorityMutationResult::stale_sequence);
  const auto result = host::AuthorityStoreTestAccess::revoke_active(
      *optional.store, optional_value.snapshot.dynamic_grants[0].request.definition, 2);
  const auto resolved = optional.store->resolve(kPlugin, hex('a'));
  OMARCHY_CHECK(result.status == host::AuthorityMutationResult::applied &&
              result.activatable && activatable(resolved) &&
              resolved.snapshot->binding.generation == 2 &&
              resolved.snapshot->dynamic_grants[0].grant.state ==
                  permissions::GrantState::revoked);
}

void exact_dynamic_revoke_rebases_every_epoch() {
  Fixture fixture;
  auto value = dynamic_review(fixture, 1);
  OMARCHY_CHECK(publish_and_promote(fixture, value));
  const auto target = value.snapshot.dynamic_grants[0];
  const auto result = host::AuthorityStoreTestAccess::revoke_active(
      *fixture.store, target.request.definition, 2);
  const auto view = fixture.store->read_authority_view();
  OMARCHY_CHECK(result.status == host::AuthorityMutationResult::applied &&
              !result.activatable && view && view->active &&
              view->active->binding.generation == 2 &&
              view->active->dynamic_grants.size() == 2);
  for (std::size_t index = 0; index < view->active->dynamic_grants.size();
       ++index) {
    const auto &dynamic = view->active->dynamic_grants[index];
    OMARCHY_CHECK(dynamic.binding == view->active->binding &&
                dynamic.grant.epoch == 2 &&
                dynamic.request == value.snapshot.dynamic_grants[index].request &&
                dynamic.grant.operations ==
                    value.snapshot.dynamic_grants[index].grant.operations);
  }
  OMARCHY_CHECK(view->active->dynamic_grants[0].grant.state ==
              permissions::GrantState::revoked &&
              view->active->dynamic_grants[1].grant.state ==
                  permissions::GrantState::granted);
}

void revoke_failures_poison_after_effect_fence() {
  for (const bool fail_allocation : {false, true}) {
    Fixture fixture;
    auto value = review(1, 'a', false);
    OMARCHY_CHECK(publish_and_promote(fixture, value));
    auto live = committed_live(*fixture.store, value.snapshot.binding);
    if (fail_allocation) {
      allocation_failure::live = live;
      allocation_failure::binding = value.snapshot.binding;
      allocation_failure::fired = false;
      allocation_failure::armed = true;
    } else {
      OMARCHY_CHECK(::chmod(fixture.path.c_str(), 0500) == 0);
    }
    const auto result = host::AuthorityStoreTestAccess::revoke_active(
        *fixture.store, value.snapshot.dynamic_grants[0].request.definition, 2);
    if (fail_allocation) {
      allocation_failure::armed = false;
      allocation_failure::live.reset();
      OMARCHY_CHECK(allocation_failure::fired);
    } else {
      OMARCHY_CHECK(::chmod(fixture.path.c_str(), 0700) == 0);
    }
    OMARCHY_CHECK(
        result.status == host::AuthorityMutationResult::io_error &&
                !live->current(value.snapshot.binding) &&
                !fixture.store->read_slots() &&
                unavailable(fixture.store->resolve(kPlugin, hex('a'))) &&
                !fixture.store->prepare_live_activation(value.snapshot.binding,
                                                        live) &&
            publish(fixture, value, 2) ==
                    host::AuthorityMutationResult::poisoned);
  }
}

void crash_orphan_retry_and_cleanup() {
  Fixture fixture;
  const auto first = review(1);
  crash_during(fixture, host::AuthorityCrashPoint::revision_rows, [&](host::AuthorityStore &store) {
    (void)store.publish_candidate(first.verified, first.snapshot, 0, fixture.definitions);
  });
  OMARCHY_CHECK(fixture.store->read_slots() == host::AuthoritySlots{});
  OMARCHY_CHECK(host::AuthorityStoreTestAccess::count(*fixture.store, "SELECT count(*) FROM revisions") == 0);
  OMARCHY_CHECK(publish(fixture, first, 0) == host::AuthorityMutationResult::applied);
  const auto replacement = review(2, 'b');
  OMARCHY_CHECK(publish(fixture, replacement, 1) == host::AuthorityMutationResult::applied);
  OMARCHY_CHECK(host::AuthorityStoreTestAccess::count(*fixture.store, "SELECT count(*) FROM revisions") == 1);
  const auto replaced = fixture.store->read_slots();
  OMARCHY_CHECK(replaced && replaced->generation_high_watermark == 2 &&
      fixture.store->promote_candidate(first.snapshot.binding, 2) == host::AuthorityMutationResult::invalid &&
      publish(fixture, first, 2) == host::AuthorityMutationResult::invalid &&
      fixture.store->read_slots() == replaced);
  require_no_legacy_writes(fixture);
}

void durable_publish_and_promotion_crash_matrix() {
  const auto first = review(1);
  const auto second = review(2, 'b');
  const auto publish_case = [&](host::AuthorityCrashPoint point) {
    Fixture fixture;
    OMARCHY_CHECK(publish_and_promote(fixture, first));
    const auto old_slots = fixture.store->read_slots();
    OMARCHY_CHECK(old_slots && old_slots->active && !old_slots->candidate);
    crash_during(fixture, point, [&](host::AuthorityStore &store) {
      (void)store.publish_candidate(second.verified, second.snapshot,
                                    old_slots->sequence, fixture.definitions);
    });

    auto recovered = fixture.store->read_slots();
    OMARCHY_CHECK(recovered && recovered->active == old_slots->active);
    require_complete_active(fixture, first);
    require_no_legacy_writes(fixture);
    OMARCHY_CHECK(recovered->candidate.has_value() == (point == host::AuthorityCrashPoint::after_commit));
    if (!recovered->candidate) {
      OMARCHY_CHECK(*recovered == *old_slots &&
                  publish(fixture, second, recovered->sequence) == host::AuthorityMutationResult::applied);
      recovered = fixture.store->read_slots();
    }
    OMARCHY_CHECK(recovered && recovered->candidate && recovered->active &&
                recovered->active->generation == 1 &&
                recovered->candidate->generation == 2 &&
                recovered->sequence == old_slots->sequence + 1 &&
                unavailable(fixture.store->resolve(kPlugin, hex('b'))));
    OMARCHY_CHECK(fixture.store->promote_candidate(second.snapshot.binding,
                                             recovered->sequence) ==
                host::AuthorityMutationResult::applied);
    require_complete_active(fixture, second);
  };

  for (const auto point : transaction_crash_points)
    publish_case(point);

  for (const auto point : transaction_crash_points) {
    Fixture fixture;
    OMARCHY_CHECK(publish_and_promote(fixture, first));
    OMARCHY_CHECK(publish(fixture, second, 2) ==
                host::AuthorityMutationResult::applied);
    const auto old_slots = fixture.store->read_slots();
    OMARCHY_CHECK(old_slots && old_slots->active && old_slots->candidate);
    crash_during(fixture, point, [&](host::AuthorityStore &store) {
      (void)store.promote_candidate(second.snapshot.binding,
                                    old_slots->sequence);
    });

    auto recovered = fixture.store->read_slots();
    OMARCHY_CHECK(recovered.has_value());
    require_no_legacy_writes(fixture);
    OMARCHY_CHECK(recovered->candidate.has_value() == (point != host::AuthorityCrashPoint::after_commit));
    if (recovered->candidate) {
      OMARCHY_CHECK(*recovered == *old_slots);
      require_complete_active(fixture, first);
      OMARCHY_CHECK(fixture.store->promote_candidate(second.snapshot.binding,
                                               recovered->sequence) ==
                  host::AuthorityMutationResult::applied);
    } else {
      OMARCHY_CHECK(recovered->active && recovered->active->generation == 2 &&
                  recovered->sequence == old_slots->sequence + 1);
    }
    require_complete_active(fixture, second);
  }
}

void missing_and_corrupt_references_fail_closed() {
  for (const bool candidate : {false, true}) {
    for (const bool missing : {false, true}) {
      Fixture fixture;
      const auto first = review(1);
      const auto second = review(2, 'b');
      OMARCHY_CHECK(publish_and_promote(fixture, first));
      if (candidate)
        OMARCHY_CHECK(publish(fixture, second, 2) == host::AuthorityMutationResult::applied);
      const std::string reference = candidate ? "candidate" : "active";
      const std::string mutation = missing
          ? "PRAGMA foreign_keys=OFF; DELETE FROM revisions WHERE digest=(SELECT " + reference + " FROM authority); PRAGMA foreign_keys=ON"
          : "UPDATE grants SET scope='corrupt' WHERE digest=(SELECT " + reference + " FROM authority)";
      OMARCHY_CHECK(host::AuthorityStoreTestAccess::execute(*fixture.store, mutation.c_str()));
      fixture.store.reset();
      // Startup validates both references, not only whichever is about to run.
      OMARCHY_CHECK(!host::AuthorityStore::open(fixture.root.get(), ::getuid(), permissions::PluginId(kPlugin)));
    }
  }
}

void concurrency_fork_and_umask() {
  Fixture fixture;
  auto left = review(1, 'a');
  auto right = review(1, 'b');
  host::AuthorityMutationResult left_result{};
  host::AuthorityMutationResult right_result{};
  std::thread one([&] {
    left_result = publish(fixture, left, 0);
  });
  std::thread two([&] {
    right_result = publish(fixture, right, 0);
  });
  one.join();
  two.join();
  const int applied = (left_result == host::AuthorityMutationResult::applied) +
      (right_result == host::AuthorityMutationResult::applied);
  OMARCHY_CHECK(applied == 1);

  test_support::expect_child_exit(0, [&] {
    const bool rejected = !fixture.store->read_slots() &&
                          unavailable(
                              fixture.store->resolve(kPlugin, hex('a'))) &&
                          !fixture.store->root_identity();
    ::_exit(rejected ? 0 : 1);
  });
  OMARCHY_CHECK(fixture.store->read_slots().has_value());

  Fixture hostile_umask(false);
  const auto old_umask = ::umask(0777);
  hostile_umask.store = host::AuthorityStore::open(
      hostile_umask.root.get(), ::getuid(), permissions::PluginId(kPlugin));
  auto value = review(1);
  const bool published =
      hostile_umask.store &&
      publish(hostile_umask, value, 0) ==
          host::AuthorityMutationResult::applied;
  ::umask(old_umask);
  OMARCHY_CHECK(published);
}

void required_denial_cannot_promote() {
  Fixture fixture;
  auto denied = review(1, 'c', true, permissions::GrantState::denied);
  OMARCHY_CHECK(publish(fixture, denied, 0) ==
              host::AuthorityMutationResult::applied);
  OMARCHY_CHECK(fixture.store->promote_candidate(denied.snapshot.binding, 1) ==
              host::AuthorityMutationResult::invalid);
}

void incomplete_and_mismatched_reviews_fail() {
  Fixture fixture;
  for (auto mutate : std::array<void (*)(Review &), 3>{
           [](Review &value) { value.snapshot.dynamic_grants = {}; },
           [](Review &value) { value.verified.request_sha256 = hex('f'); },
           [](Review &value) { value.verified.tree_sha256 = hex('f'); }}) {
    auto value = review(1);
    mutate(value);
    OMARCHY_CHECK(publish(fixture, value, 0) ==
                host::AuthorityMutationResult::invalid);
  }
}

void dynamic_completeness_and_restart() {
  Fixture fixture;
  auto value = dynamic_review(fixture, 1);
  const auto rejected = [&](auto mutate) {
    auto malformed = value.snapshot;
    mutate(malformed);
    OMARCHY_CHECK(fixture.store->publish_candidate(value.verified, malformed, 0,
                                                fixture.definitions) ==
                  host::AuthorityMutationResult::invalid);
  };
  for (auto mutate : std::array<void (*)(policy::GrantSnapshot &), 7>{
           [](auto &s) { s.dynamic_grants.pop_back(); },
           [](auto &s) { s.dynamic_grants.push_back(s.dynamic_grants.back()); },
           [](auto &s) { s.dynamic_grants[0].binding.generation = 2; },
           [](auto &s) { s.dynamic_grants[0].grant.state = static_cast<permissions::GrantState>(255); },
           [](auto &s) { s.dynamic_grants[0].request.definition.canonical_name = definitions::Name("unreviewed.definition"); },
           [](auto &s) { ++s.dynamic_grants[0].request.definition.definition_generation; },
           [](auto &s) { s.dynamic_grants[0].request.definition.definition_digest = definitions::Digest(hex('f')); }})
    rejected(mutate);
  auto changed_manifest = value.verified;
  changed_manifest.manifest.requests[0].operations[0] = "unreviewed.operation";
  OMARCHY_CHECK(fixture.store->publish_candidate(changed_manifest, value.snapshot, 0,
                                            fixture.definitions) ==
              host::AuthorityMutationResult::invalid);
  for (const auto state : {permissions::GrantState::granted,
                           permissions::GrantState::denied,
                           permissions::GrantState::revoked}) {
    for (const auto scope : {" narrow", "narrow", "incomparable", "profile=anything"}) {
      rejected([&](auto &malformed) {
        malformed.dynamic_grants[0].request.scope = definitions::CanonicalScope(scope);
        malformed.dynamic_grants[0].grant.state = state;
      });
    }
  }
  OMARCHY_CHECK(publish_and_promote(fixture, value));
  reopen(fixture);
  OMARCHY_CHECK(activatable(fixture.store->resolve(kPlugin, hex('d'))));

  Fixture denied_fixture;
  auto denied =
      dynamic_review(denied_fixture, 1, permissions::GrantState::denied);
  OMARCHY_CHECK(publish(denied_fixture, denied, 0) == host::AuthorityMutationResult::applied);
  OMARCHY_CHECK(denied_fixture.store->promote_candidate(denied.snapshot.binding, 1) ==
              host::AuthorityMutationResult::invalid);
}

void corrupt_records_fail_closed() {
  for (int mode = 0; mode < 3; ++mode) {
    Fixture fixture;
    auto value = review(1);
    OMARCHY_CHECK(publish_and_promote(fixture, value));
    if (mode == 0) {
      OMARCHY_CHECK(host::AuthorityStoreTestAccess::execute(*fixture.store,
          "UPDATE grants SET scope='corrupt'"));
    } else if (mode == 1) {
      OMARCHY_CHECK(::fchmodat(fixture.root.get(), "authority.db", 0644, 0) == 0);
    } else {
      OMARCHY_CHECK(::linkat(fixture.root.get(), "authority.db", fixture.root.get(), "extra", 0) == 0);
    }
    OMARCHY_CHECK(unavailable(fixture.store->resolve(kPlugin, hex('a'))));
  }
}

void root_lock_and_slots_metadata() {
  Fixture fixture(false);
  OMARCHY_CHECK(!host::AuthorityStore::open(
              fixture.root.get(), ::getuid() + 1, permissions::PluginId(kPlugin)));
  fixture.store = host::AuthorityStore::open(
      fixture.root.get(), ::getuid(), permissions::PluginId(kPlugin));
  OMARCHY_CHECK(fixture.store != nullptr);
  struct stat root_metadata {};
  const auto root_identity = fixture.store->root_identity();
  OMARCHY_CHECK(::fstat(fixture.root.get(), &root_metadata) == 0 && root_identity &&
              root_identity->device ==
                  static_cast<std::uint64_t>(root_metadata.st_dev) &&
              root_identity->inode ==
                  static_cast<std::uint64_t>(root_metadata.st_ino));
  OMARCHY_CHECK(!host::AuthorityStore::open(fixture.root.get(), ::getuid(),
                                      permissions::PluginId(kPlugin)));

  Fixture malformed(false);
  int slots = ::openat(malformed.root.get(), "slots",
                       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  OMARCHY_CHECK(slots >= 0);
  ::close(slots);
  OMARCHY_CHECK(!host::AuthorityStore::open(malformed.root.get(), ::getuid(), permissions::PluginId(kPlugin)));

  Fixture symlink_slots(false);
  OMARCHY_CHECK(::symlinkat("missing", symlink_slots.root.get(), "slots") == 0);
  OMARCHY_CHECK(!host::AuthorityStore::open(symlink_slots.root.get(), ::getuid(), permissions::PluginId(kPlugin)));

  Fixture special(false);
  OMARCHY_CHECK(::mkfifoat(special.root.get(), ".authority.lock", 0600) == 0);
  OMARCHY_CHECK(!host::AuthorityStore::open(special.root.get(), ::getuid(),
                                      permissions::PluginId(kPlugin)));
}

host::AuthoritySlots write_legacy(Fixture &fixture, const Review &first,
                                  const Review *candidate = nullptr) {
  const auto record = [&](const Review &review) {
    std::vector<std::byte> bytes;
    OMARCHY_CHECK(host::authority_snapshot_codec::encode_snapshot(review.snapshot, bytes));
    const auto reference = host::authority_snapshot_codec::reference_for(review.snapshot, manifest::sha256_hex(bytes));
    test_support::TemporaryDirectory::write_file(
        fixture.path / ("grant-" + std::string(reference.snapshot_digest.view())),
        {reinterpret_cast<const char *>(bytes.data()), bytes.size()}, O_WRONLY | O_CREAT | O_TRUNC);
    return reference;
  };
  host::AuthoritySlots slots{2, first.snapshot.binding.generation, record(first), {}};
  if (candidate) {
    slots.sequence = 3;
    slots.generation_high_watermark = candidate->snapshot.binding.generation;
    slots.candidate = record(*candidate);
  }
  const auto bytes = host::authority_snapshot_codec::encode_slots(slots);
  test_support::TemporaryDirectory::write_file(fixture.path / "slots",
      {reinterpret_cast<const char *>(bytes.data()), bytes.size()}, O_WRONLY | O_CREAT | O_TRUNC);
  return slots;
}

void legacy_migration_and_no_reimport() {
  Fixture fixture(false);
  const auto first = review(1, 'a', false);
  const auto second = review(2, 'b', false);
  const auto before = write_legacy(fixture, first, &second);
  const auto legacy_bytes = test_support::read_file(fixture.path / "slots");
  reopen(fixture);
  OMARCHY_CHECK(fixture.store->read_slots() == before);
  require_complete_active(fixture, first);
  OMARCHY_CHECK(fixture.store->promote_candidate(second.snapshot.binding, 3) == host::AuthorityMutationResult::applied);
  const auto revoked = host::AuthorityStoreTestAccess::revoke_active(
      *fixture.store, second.snapshot.dynamic_grants[0].request.definition, 4);
  OMARCHY_CHECK(revoked.status == host::AuthorityMutationResult::applied && revoked.binding && revoked.binding->generation == 3);
  reopen(fixture);
  const auto current = fixture.store->read_authority_view();
  OMARCHY_CHECK(current && current->active && current->active->binding == revoked.binding &&
      current->active->dynamic_grants[0].grant.state == permissions::GrantState::revoked);
  OMARCHY_CHECK(test_support::read_file(fixture.path / "slots") == legacy_bytes);
  fixture.store.reset();
  // Valid old legacy grants still exist. Losing the new database must not
  // import them again or reset the generation high-watermark.
  OMARCHY_CHECK(::unlinkat(fixture.root.get(), "authority.db", 0) == 0);
  OMARCHY_CHECK(!host::AuthorityStore::open(fixture.root.get(), ::getuid(), permissions::PluginId(kPlugin)));
  OMARCHY_CHECK(!std::filesystem::exists(fixture.path / "authority.db"));
}

void migration_crash_recovery() {
  for (const auto point : {host::AuthorityCrashPoint::before_commit,
                          host::AuthorityCrashPoint::migration_after_commit,
                          host::AuthorityCrashPoint::migration_after_stamp_write,
                          host::AuthorityCrashPoint::migration_after_stamp_sync}) {
    Fixture fixture(false);
    const auto first = review(1);
    const auto second = review(2, 'b');
    const auto before = write_legacy(fixture, first, &second);
    test_support::expect_child_exit(86, [&] {
      host::AuthorityStoreTestAccess::crash_at(point);
      (void)host::AuthorityStore::open(fixture.root.get(), ::getuid(), permissions::PluginId(kPlugin));
      ::_exit(87);
    });
    reopen(fixture);
    OMARCHY_CHECK(fixture.store->read_slots() == before);
    require_complete_active(fixture, first);
    OMARCHY_CHECK(fixture.store->promote_candidate(second.snapshot.binding, before.sequence) ==
        host::AuthorityMutationResult::applied);
    reopen(fixture);
    require_complete_active(fixture, second);
  }
}

void invalid_legacy_never_grants() {
  for (int mode = 0; mode < 4; ++mode) {
    Fixture fixture(false);
    auto first = review(1);
    const auto slots = write_legacy(fixture, first);
    const auto file = fixture.path / ("grant-" + std::string(slots.active->snapshot_digest.view()));
    if (mode == 0) {
      OMARCHY_CHECK(::unlink(file.c_str()) == 0);
    } else if (mode == 1) {
      test_support::TemporaryDirectory::write_file(file, "corrupt", O_WRONLY | O_TRUNC);
    } else if (mode == 2) {
      OMARCHY_CHECK(::chmod(file.c_str(), 0644) == 0);
    } else {
      OMARCHY_CHECK(::link(file.c_str(), (fixture.path / "alias").c_str()) == 0);
    }
    OMARCHY_CHECK(!host::AuthorityStore::open(fixture.root.get(), ::getuid(), permissions::PluginId(kPlugin)));
  }
}

void durable_revocation_crash_matrix() {
  for (const auto point : transaction_crash_points) {
    Fixture fixture;
    const auto first = review(1, 'a', false);
    OMARCHY_CHECK(publish_and_promote(fixture, first));
    const auto before = fixture.store->read_slots();
    crash_during(fixture, point, [&](host::AuthorityStore &store) {
      auto live = committed_live(store, first.snapshot.binding);
      (void)host::AuthorityStoreTestAccess::revoke_active(
          store, first.snapshot.dynamic_grants[0].request.definition, 2);
    });
    const auto view = fixture.store->read_authority_view();
    OMARCHY_CHECK(view && view->active);
    if (point == host::AuthorityCrashPoint::after_commit) {
      OMARCHY_CHECK(view->authority_slots.sequence == 3 &&
          view->active->binding.generation == 2 &&
          view->active->dynamic_grants[0].grant.state == permissions::GrantState::revoked);
    } else {
      OMARCHY_CHECK(view->authority_slots == before &&
          view->active->dynamic_grants[0].grant.state == permissions::GrantState::granted);
    }
    require_no_legacy_writes(fixture);
  }
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--crash-matrix-only") {
      durable_publish_and_promotion_crash_matrix();
      durable_revocation_crash_matrix();
      migration_crash_recovery();
      missing_and_corrupt_references_fail_closed();
      return 0;
    }
    snapshot_format_has_one_request_fingerprint();
    roundtrip_and_lifecycle();
    live_activation_binding_and_revocation();
    mutation_epoch_saturates_fail_closed();
    live_effect_transitions_drain_before_authority_changes();
    reentrant_effect_revoke_poisoned_without_durable_change();
    promotion_revokes_before_failed_replacement();
    exact_builtin_revoke_rebases_and_invalidates_candidate();
    exact_dynamic_revoke_rebases_every_epoch();
    revoke_failures_poison_after_effect_fence();
    crash_orphan_retry_and_cleanup();
    durable_publish_and_promotion_crash_matrix();
    missing_and_corrupt_references_fail_closed();
    concurrency_fork_and_umask();
    required_denial_cannot_promote();
    incomplete_and_mismatched_reviews_fail();
    dynamic_completeness_and_restart();
    corrupt_records_fail_closed();
    root_lock_and_slots_metadata();
    legacy_migration_and_no_reimport();
    migration_crash_recovery();
    invalid_legacy_never_grants();
    durable_revocation_crash_matrix();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
