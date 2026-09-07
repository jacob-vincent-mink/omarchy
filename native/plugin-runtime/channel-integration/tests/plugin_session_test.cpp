#include "../../tests/support/qt_wait.hpp"
#include "../../tests/support/blocking_gate.hpp"
#include "../../tests/support/gesture_clock.hpp"
#include "../../tests/support/echo_fixture.hpp"
#include "../../tests/support/broker_fixture.hpp"
#include "../../tests/support/permission_fixture.hpp"
#include "../../tests/support/runtime_fixture.hpp"
#include "../../tests/support/test_assert.hpp"

#include "audit_store.hpp"
#include "../../host-session/tests/authority_store_test_access.hpp"
#include "authority_sql.hpp"
#include "omarchy/plugin_runtime/providers/local_provider.hpp"
#include "omarchy/plugin_runtime/launcher/test_supervisor.h"
#include "omarchy/plugin_runtime/provider_host/provider_host.hpp"
#include "omarchy/plugin_runtime/surface/render_messages.hpp"
#include "plugin_permission_authority.hpp"
#include "plugin_session.hpp"
#include "runtime_roots.hpp"
#include "activation_catalog.hpp"
#include "channel_roles.hpp"
#include "session_runtime_factory.hpp"
#include "structured_broker.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QThread>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>

void session_runtime_factory_tests();

namespace channel = omarchy::plugin_runtime::channel;
namespace audit = omarchy::plugins::audit;
namespace broker = omarchy::plugin_runtime::broker;
namespace definitions = omarchy::plugins::definitions;
namespace host = omarchy::plugin_runtime::host_session;
namespace launcher = omarchy::plugin_runtime::launcher;
namespace manifest = omarchy::plugins::manifest;
namespace permissions = omarchy::plugins::permissions;
namespace policy = omarchy::plugin_runtime::policy;
namespace providers = omarchy::plugin_runtime::providers;
namespace provider_host = omarchy::plugin_runtime::provider_host;
namespace runtime = omarchy::plugin_runtime::runtime;
namespace sandbox = omarchy::plugin_runtime::sandbox;
namespace surface = omarchy::plugin_runtime::surface;
namespace wire = omarchy::plugin::wire;

static_assert(!std::is_convertible_v<channel::PluginSession *, channel::SurfaceSessionPort *>);
static_assert(!std::is_convertible_v<channel::AuthenticatedSessionRuntime *, host::DispatchAuthority *> &&
              !std::is_convertible_v<channel::AuthenticatedSessionRuntime *, channel::GenerationAuthority *> &&
              !std::is_move_constructible_v<channel::AuthenticatedSessionRuntime>);

namespace {

template <typename... T>
constexpr bool move_only_owners =
    ((!std::is_default_constructible_v<T> && !std::is_aggregate_v<T> &&
      !std::is_copy_constructible_v<T> && !std::is_copy_assignable_v<T> &&
      std::is_nothrow_move_constructible_v<T> &&
      std::is_nothrow_move_assignable_v<T> && std::is_nothrow_destructible_v<T>) && ...);
static_assert(move_only_owners<channel::RuntimeRoots, channel::ActivationCatalog,
                               channel::ActivationCatalogEntry,
                               channel::PreparedPluginSession,
                               host::InspectedActivationRecord>);

template <typename Source, typename Target>
constexpr bool role_mapping_contract(Source invalid, Target fallback) {
  using channel::detail::role_as;
  using channel::detail::valid_role;
  return role_as<Target>(Source::control) == Target::control &&
         role_as<Target>(Source::broker) == Target::broker &&
         role_as<Target>(Source::render) == Target::render &&
         role_as<Target>(invalid, fallback) == fallback && !valid_role(invalid) &&
         valid_role(Source::control) && valid_role(Source::broker) &&
         valid_role(Source::render);
}
static_assert(role_mapping_contract(static_cast<launcher::EndpointRole>(3),
                                    wire::EndpointRole::control) &&
              role_mapping_contract(static_cast<wire::EndpointRole>(0),
                                    launcher::EndpointRole::control) &&
              role_mapping_contract(static_cast<host::ChannelLane>(255),
                                    wire::EndpointRole::control) &&
              role_mapping_contract(static_cast<wire::EndpointRole>(65535),
                                    host::ChannelLane::control) &&
              role_mapping_contract(static_cast<wire::EndpointRole>(4),
                                    launcher::EndpointMask::none));

static_assert(
    !std::is_copy_constructible_v<channel::AuthenticatedSessionChannel> &&
    !std::is_move_constructible_v<channel::AuthenticatedSessionChannel>);
static_assert(!std::is_copy_constructible_v<channel::AuthenticatedMessage> &&
              std::is_move_constructible_v<channel::AuthenticatedMessage>);

using omarchy::plugin_runtime::test_support::require;
using omarchy::plugin_runtime::test_support::await;
using namespace omarchy::plugin_runtime::test_support;

permissions::ActivationBinding binding() {
  return {
      .plugin = permissions::PluginId("fixture.product-session"),
      .revision = permissions::Digest(std::string(64, 'a')),
      .policy_fingerprint = permissions::Digest(
          manifest::requested_capability_fingerprint({})),
      .generation = 17,
  };
}

policy::GrantSnapshot grants() {
  policy::GrantSnapshot value;
  value.binding = binding();
  return value;
}

class Scope final : public launcher::test_support::ReadyScope {
public:
  AttachResult attach(std::string_view unit, pid_t monitor_pid,
                      pid_t worker_pid, const sandbox::SandboxPlan &plan,
                      launcher::Deadline, std::string &) override {
    if (monitor_pid <= 0 || worker_pid <= 0 ||
        plan.worker_descriptors != std::vector<int>({3, 4, 5}))
      return {};
    name = unit;
    peer_pid = worker_pid;
    ++attachments;
    return {.attached = true, .cleanup_required = true};
  }
  bool terminate_scope_validated(std::string_view, launcher::Deadline,
                                  std::string &) noexcept override {
    ++terminations;
    return true;
  }

  std::string name;
  pid_t peer_pid = -1;
  std::atomic<int> attachments{0};
  std::atomic<int> terminations{0};
};

auto peer_supervisor_factory(std::shared_ptr<Scope> scope) {
  return [scope = std::move(scope)] {
    return launcher::test_support::make_supervisor(
        FAKE_BWRAP_PATH, CHANNEL_PEER_PATH, scope);
  };
}

using EffectBarrier = BlockingGate;

class Dispatch final : public host::DispatchAuthority {
  class Lease final : public host::DispatchAuthorityLease {
  public:
    Lease(permissions::ActivationBinding binding,
          std::shared_ptr<host::LiveGenerationState> live,
          std::shared_ptr<EffectBarrier> barrier)
        : binding_(std::move(binding)), live_(std::move(live)),
          barrier_(std::move(barrier)) {}

    bool current_at_effect() const noexcept override {
      if (barrier_)
        barrier_->arrive_and_wait();
      return live_ && live_->current(binding_);
    }

  private:
    permissions::ActivationBinding binding_;
    std::shared_ptr<host::LiveGenerationState> live_;
    std::shared_ptr<EffectBarrier> barrier_;
  };

public:
  explicit Dispatch(std::shared_ptr<host::LiveGenerationState> live,
                    std::shared_ptr<EffectBarrier> barrier = {})
      : live_(std::move(live)), barrier_(std::move(barrier)) {}

  std::unique_ptr<host::DispatchAuthorityLease>
  acquire(const permissions::ActivationBinding &binding, std::uint64_t,
          const wire::PacketView &) override {
    return std::make_unique<Lease>(binding, live_, barrier_);
  }

private:
  std::shared_ptr<host::LiveGenerationState> live_;
  std::shared_ptr<EffectBarrier> barrier_;
};

class RuntimeFactory final {
public:
  RuntimeFactory() {
    factory.test_on_create = [this](const manifest::ManifestV2 &verified_manifest,
        const policy::GrantSnapshot &snapshot, int revision_directory_fd,
        int private_state_directory_fd, const auto &live_generation,
        const auto &gesture_eligibility) {
      ++calls;
      saw_manifest = verified_manifest.surface_names ==
                     std::vector<std::string>({"bar", "panel", "overlay"});
      descriptors_valid = ::fcntl(revision_directory_fd, F_GETFD) >= 0 &&
          ::fcntl(private_state_directory_fd, F_GETFD) >= 0;
      gesture_authority_valid = gesture_eligibility != nullptr;
      live_generation_valid =
          live_generation && live_generation->current(snapshot.binding);
      gesture_lifetime = gesture_eligibility;
    };
    factory.test_on_destroy = [count = destructions] { ++*count; };
  }
  operator channel::SessionRuntimeFactory &() { return factory; }

  std::shared_ptr<std::atomic<int>> destructions =
      std::make_shared<std::atomic<int>>(0);
  int calls = 0;
  bool saw_manifest = false;
  bool descriptors_valid = false;
  bool gesture_authority_valid = false;
  bool live_generation_valid = false;
  std::weak_ptr<runtime::GestureEligibilityLatch> gesture_lifetime;

private:
  channel::SessionRuntimeFactory factory{definitions::TrustedDefinitionRegistry{},
                                        channel::RuntimeServices{}};
};

class ActivationTree : public TemporaryDirectory {
protected:
  ActivationTree() {
    std::filesystem::create_directories(revision_);
    std::filesystem::create_directories(state_);
  }

  host::ActivationSnapshot open_snapshot(
      std::string_view record_name, manifest::ManifestV2 verified_manifest,
      policy::GrantSnapshot snapshot_grants,
      std::shared_ptr<host::LiveGenerationState> live) const {
    host::UniqueFd record(::open((revision_ / record_name).c_str(), O_RDONLY | O_CLOEXEC));
    host::UniqueFd revision(::open(revision_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    host::UniqueFd state(::open(state_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    OMARCHY_CHECK(record.get() >= 0 && revision.get() >= 0 && state.get() >= 0);
    return {
        .record = {.plugin_id = std::string(snapshot_grants.binding.plugin.view()),
                   .revision_directory = "revision",
                   .revision_sha256 = std::string(snapshot_grants.binding.revision.view()),
                   .state_directory = "state"},
        .manifest = std::move(verified_manifest),
        .grants = std::move(snapshot_grants),
        .activation_record = std::move(record),
        .revision_directory = std::move(revision),
        .state_directory = std::move(state),
        .live = std::move(live),
    };
  }

  std::filesystem::path revision_ = root_ / "revision";
  std::filesystem::path state_ = root_ / "state";
};

class ActivationFixture final : public ActivationTree {
public:
  explicit ActivationFixture(
      std::string_view worker_mode = "session-happy",
      std::span<const std::byte> dynamic_request = {},
      permissions::ActivationBinding activation_binding = binding())
      : live_(std::make_shared<host::LiveGenerationState>(
            std::move(activation_binding))) {
    std::ofstream(revision_ / "worker-mode") << worker_mode << '\n';
    if (!dynamic_request.empty()) {
      std::ofstream request(state_ / "dynamic-request", std::ios::binary);
      request.write(reinterpret_cast<const char *>(dynamic_request.data()),
                    static_cast<std::streamsize>(dynamic_request.size()));
      request.close();
      OMARCHY_CHECK(static_cast<bool>(request));
      OMARCHY_CHECK(::chmod((state_ / "dynamic-request").c_str(), 0600) == 0);
    }
    OMARCHY_CHECK(::chmod((revision_ / "worker-mode").c_str(), 0444) == 0 &&
                ::chmod(revision_.c_str(), 0555) == 0);
  }

  host::ActivationSnapshot snapshot() const {
    auto snapshot_grants = grants();
    manifest::ManifestV2 verified_manifest;
    verified_manifest.id = std::string(snapshot_grants.binding.plugin.view());
    verified_manifest.surface_names = {"bar", "panel", "overlay"};
    return snapshot(std::move(verified_manifest), std::move(snapshot_grants));
  }

  host::ActivationSnapshot
  snapshot(manifest::ManifestV2 verified_manifest,
           policy::GrantSnapshot snapshot_grants) const {
    return open_snapshot("worker-mode", std::move(verified_manifest),
                         std::move(snapshot_grants), live_);
  }

  const std::filesystem::path &state_directory() const noexcept {
    return state_;
  }

  const std::shared_ptr<host::LiveGenerationState> &live() const noexcept {
    return live_;
  }

private:
  std::shared_ptr<host::LiveGenerationState> live_;
};

manifest::ManifestV2
provider_manifest(const definitions::ResolvedDefinition &resolved) {
  return echo_manifest(resolved, binding().plugin.view(),
                       "exercise delayed provider settlement", true);
}

policy::GrantSnapshot
provider_grants(const definitions::ResolvedDefinition &resolved,
                const manifest::ManifestV2 &verified_manifest) {
  policy::GrantSnapshot value;
  value.binding = binding();
  value.binding.policy_fingerprint = permissions::Digest(
      manifest::requested_capability_fingerprint(verified_manifest.requests));
  value.dynamic_grants.push_back(echo_grant(
      resolved, value.binding, true, permissions::GrantState::granted,
      value.binding.generation));
  return value;
}

std::vector<std::byte>
provider_invocation(const policy::GrantSnapshot &snapshot) {
  return ungestured_invocation(snapshot.dynamic_grants.front().request.definition,
                               "echo");
}

class ProviderProfileFixture final : public TemporaryDirectory {
public:
  ProviderProfileFixture() {
    marker_ = root_ / "started";
    const auto package = root_ / "pkg";
    const auto admin = root_ / "admin";
    const auto executable = root_ / "bin/provider-peer";
    std::filesystem::create_directories(package);
    std::filesystem::create_directories(admin);
    std::filesystem::create_directories(executable.parent_path());
    std::filesystem::copy_file(PROVIDER_COMPOSITION_PEER_PATH, executable);
    OMARCHY_CHECK(::chmod(root_.c_str(), 0755) == 0 &&
                ::chmod(package.c_str(), 0755) == 0 &&
                ::chmod(admin.c_str(), 0755) == 0 &&
                ::chmod(executable.parent_path().c_str(), 0755) == 0 &&
                ::chmod(executable.c_str(), 0500) == 0);
    std::ofstream profile(package / "echo.profile");
    profile << "schema=1\n"
            << "adapter-class=service.echo.adapter\n"
            << "contract-digest=" << std::string(64, 'd') << "\n"
            << "abi-version=1\n"
            << "group=echo.group\n"
            << "executable=/bin/provider-peer\n"
            << "executable-sha256="
            << manifest::sha256_hex(read_file(executable)) << "\n"
            << "arg=delayed-pid\n"
            << "arg=" << marker_.string() << "\n";
    profile.close();
    OMARCHY_CHECK(static_cast<bool>(profile) &&
                ::chmod((package / "echo.profile").c_str(), 0644) == 0);
  }

  std::shared_ptr<const provider_host::ProviderCatalog> catalog() const {
    const int root =
        ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    OMARCHY_CHECK(root >= 0);
    const std::array<std::string_view, 1> package{"pkg"};
    const std::array<std::string_view, 1> admin{"admin"};
    provider_host::CatalogError error{};
    auto result = provider_host::ProviderCatalog::load(
        root, package, admin, static_cast<std::uint32_t>(::getuid()), error);
    ::close(root);
    OMARCHY_CHECK(result && error == provider_host::CatalogError::none);
    return result;
  }

  std::size_t starts() const {
    std::ifstream marker(marker_);
    return static_cast<std::size_t>(
        std::count(std::istreambuf_iterator<char>(marker),
                   std::istreambuf_iterator<char>(), '\n'));
  }

  pid_t pid() const {
    return static_cast<pid_t>(std::stol(read_file(marker_)));
  }

private:
  std::filesystem::path marker_;
};

class InvalidQmlWorkerFixture final : public ActivationTree {
public:
  explicit InvalidQmlWorkerFixture(bool valid_qml = false) {
    std::ofstream(revision_ / "manifest.json")
        << R"({
  "schemaVersion": 2,
  "id": "fixture.product-session",
  "name": "Invalid QML startup fixture",
  "version": "1.0.0",
  "runtime": {"apiVersion": 1, "qml": "Broken.qml"},
  "surfaces": {
    "bar": {"role": "bar-embedded", "defaultSection": "right"}
  },
  "permissions": {"required": [], "optional": []}
})";
    std::ofstream(revision_ / "Broken.qml")
        << (valid_qml ? "import QtQuick\nItem {}\n"
                      : "import QtQuick\nItem { this is not valid QML }\n");
    for (const auto &entry :
         std::filesystem::directory_iterator(revision_))
      OMARCHY_CHECK(::chmod(entry.path().c_str(), 0444) == 0);
    OMARCHY_CHECK(::chmod(revision_.c_str(), 0555) == 0);
  }

  host::ActivationSnapshot snapshot() const {
    auto snapshot_grants = grants();
    manifest::ManifestV2 verified_manifest;
    verified_manifest.id = std::string(snapshot_grants.binding.plugin.view());
    verified_manifest.runtime.api_version = 1;
    verified_manifest.runtime.qml = "Broken.qml";
    verified_manifest.surface_names = {"bar"};
    verified_manifest.canonical_surfaces =
        R"({"bar":{"defaultSection":"right","role":"bar-embedded"}})";
    auto live = std::make_shared<host::LiveGenerationState>(snapshot_grants.binding);
    return open_snapshot("manifest.json", std::move(verified_manifest),
                         std::move(snapshot_grants), std::move(live));
  }
};

class RuntimeRootFixture final : public TemporaryDirectory {
public:
  explicit RuntimeRootFixture(std::string_view worker_mode = "session-happy") {
    activation_ = root_ / "activation";
    revisions_ = root_ / "revisions";
    state_ = root_ / "state";
    authority_ = root_ / "authority";
    revision_ = revisions_ / "installed";
    state_directory_ = state_ / plugin_;
    std::filesystem::create_directory(activation_);
    std::filesystem::create_directory(revisions_);
    std::filesystem::create_directory(state_);
    std::filesystem::create_directory(authority_);
    std::filesystem::copy(RUNTIME_ROOT_REVISION_FIXTURE, revision_,
                          std::filesystem::copy_options::recursive);
    std::ofstream(revision_ / "worker-mode") << worker_mode << '\n';
    verified_ = freeze_revision(revision_, plugin_);
    std::filesystem::create_directory(state_directory_);
    std::ofstream(state_directory_ / "identity") << "pinned\n";
    OMARCHY_CHECK(::chmod(authority_.c_str(), 0700) == 0 &&
                ::chmod(state_directory_.c_str(), 0700) == 0);

    activation_fd_ = open_directory(activation_);
    revisions_fd_ = open_directory(revisions_);
    state_fd_ = open_directory(state_);
    authority_fd_ = open_directory(authority_);
    store_ = host::AuthorityStore::open(authority_fd_.get(), ::getuid(),
                                        permissions::PluginId(plugin_));
    OMARCHY_CHECK(store_ != nullptr);
  }

  policy::GrantSnapshot snapshot(std::uint64_t generation) const {
    return permission_snapshot(definitions_, verified_->manifest,
                               verified_->tree_sha256, generation);
  }

  policy::GrantSnapshot publish(std::uint64_t generation,
                                std::uint64_t sequence,
                                bool deny_notifications = false) {
    auto value = snapshot(generation);
    if (deny_notifications)
      for (auto &grant : value.dynamic_grants)
        if (grant.request.definition.canonical_name.view() == "notifications.send")
          grant.grant.state = permissions::GrantState::denied;
    OMARCHY_CHECK(store_->publish_candidate(*verified_, value, sequence, definitions_) ==
                host::AuthorityMutationResult::applied);
    return value;
  }

  void promote(const policy::GrantSnapshot &value, std::uint64_t sequence) {
    OMARCHY_CHECK(store_->promote_candidate(value.binding, sequence) ==
                host::AuthorityMutationResult::applied);
  }

  policy::GrantSnapshot activate(bool deny_notifications = false) {
    record();
    auto value = publish(1, 0, deny_notifications);
    promote(value, 1);
    return value;
  }

  void replace_active(const policy::GrantSnapshot &value) {
    OMARCHY_CHECK(host::AuthorityStoreTestAccess::replace_active(*store_, value) ==
                host::AuthorityMutationResult::applied);
  }

  void record(std::string revision_sha256 = {}) const {
    if (revision_sha256.empty())
      revision_sha256 = verified_->tree_sha256;
    const auto bytes =
        "format=omarchy-plugin-activation-v2\nplugin=" + plugin_ +
        "\nrevision-directory=installed\nrevision-sha256=" + revision_sha256 +
        "\nstate-directory=" + plugin_ + "\n";
    std::ofstream file(activation_ / "current", std::ios::trunc);
    file << bytes;
    file.close();
    OMARCHY_CHECK(::chmod((activation_ / "current").c_str(), 0600) == 0);
  }


  channel::PluginRuntimePreparationResult prepare_result(
      channel::RuntimeServices services = {},
      std::function<launcher::Supervisor()> supervisor_factory = {},
      std::string activation_record = "current",
      std::uint32_t trusted_uid = static_cast<std::uint32_t>(::getuid()),
      bool valid_authority = true,
      void (*before_final_fence)(host::AuthorityStore &, void *) noexcept =
          nullptr,
      void *before_final_fence_context = nullptr) {
    store_.reset();
    auto authority =
        valid_authority
            ? host::UniqueFd(
                  ::fcntl(authority_fd_.get(), F_DUPFD_CLOEXEC, 0))
            : host::UniqueFd{};
    return channel::ReviewedSessionTestAccess::prepare_from_parts(
        activation_fd_.get(), revisions_fd_.get(), state_fd_.get(), std::move(authority),
        permissions::PluginId(plugin_), trusted_uid,
        std::move(activation_record),
        std::make_shared<const definitions::TrustedDefinitionRegistry>(
            definitions_),
        std::make_shared<const channel::RuntimeServices>(std::move(services)),
        {}, {}, std::move(supervisor_factory), before_final_fence,
        before_final_fence_context);
  }

  host::AuthorityRevocationResult revoke_required() {
    const auto view = store_->read_authority_view();
    OMARCHY_CHECK(view && view->active);
    const auto required = std::ranges::find_if(
        view->active->dynamic_grants,
        [](const auto &grant) { return grant.request.required; });
    OMARCHY_CHECK(required != view->active->dynamic_grants.end());
    return host::AuthorityStoreTestAccess::revoke_active(
        *store_, required->request.definition, view->authority_slots.sequence);
  }

  void close_borrowed_roots() {
    for (auto *fd : {&activation_fd_, &revisions_fd_, &state_fd_, &authority_fd_})
      fd->reset();
  }

  void corrupt_active() {
    OMARCHY_CHECK(host::AuthorityStoreTestAccess::execute(*store_,
        "UPDATE grants SET scope='corrupt' WHERE digest=(SELECT active FROM authority)"));
  }

  std::uint64_t authority_sequence() const {
    auto directory = open_directory(authority_);
    auto database = host::SqliteAuthorityDatabase::open(directory.get(), ::getuid());
    // Inspect the durable head even when intentionally corrupt grants make
    // reopening the production authority fail closed.
    const auto slot_state = database
        ? host::authority_sql::read_slots(*database, permissions::PluginId(plugin_))
        : std::nullopt;
    OMARCHY_CHECK(slot_state.has_value());
    return slot_state->sequence;
  }

  void corrupt_prepared_backing() {
    std::ofstream activation(activation_ / "current", std::ios::trunc);
    activation << "invalid\n";
    activation.close();
    auto directory = open_directory(authority_);
    auto database = host::SqliteAuthorityDatabase::open(directory.get(), ::getuid());
    OMARCHY_CHECK(database && sqlite3_exec(database->connection(), "DELETE FROM authority",
        nullptr, nullptr, nullptr) == SQLITE_OK);
  }

private:
  static host::UniqueFd open_directory(const std::filesystem::path &path) {
    host::UniqueFd fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    OMARCHY_CHECK(fd);
    return fd;
  }

  const std::string plugin_ = "org.example.status";
  std::filesystem::path activation_;
  std::filesystem::path revisions_;
  std::filesystem::path state_;
  std::filesystem::path authority_;
  std::filesystem::path revision_;
  std::filesystem::path state_directory_;
  host::UniqueFd activation_fd_;
  host::UniqueFd revisions_fd_;
  host::UniqueFd state_fd_;
  host::UniqueFd authority_fd_;
  definitions::TrustedDefinitionRegistry definitions_ = packaged_registry();
  std::unique_ptr<host::AuthorityStore> store_;
  std::optional<host::VerifiedRevision> verified_;
};

struct PeerSessionFixture {
  ActivationFixture activation;
  RuntimeFactory runtime_factory;
  std::shared_ptr<Scope> scope = std::make_shared<Scope>();
  host::SessionToken identity;
  wire::SessionSequence sequence;
  unsigned published = 0;

  explicit PeerSessionFixture(std::string_view mode = "session-scripted")
      : activation(mode) {}

  std::unique_ptr<channel::PluginSession> commit(
      std::vector<std::string> surfaces, channel::PluginSessionEvents *events = nullptr,
      channel::SurfaceIntentSink *sink = nullptr,
      std::shared_ptr<runtime::GestureEligibilityClock> clock = {},
      host::SessionLimits limits = {}) {
    auto snapshot_grants = grants();
    manifest::ManifestV2 manifest;
    manifest.id = std::string(snapshot_grants.binding.plugin.view());
    manifest.surface_names = std::move(surfaces);
    channel::PluginSessionCreateError error{};
    auto prepared = channel::PluginSessionTestAccess::prepare_from_activation(
        launcher::test_support::make_supervisor(FAKE_BWRAP_PATH, CHANNEL_PEER_PATH, scope),
        activation.snapshot(std::move(manifest), std::move(snapshot_grants)),
        runtime_factory, error, limits, std::nullopt, std::move(clock));
    OMARCHY_CHECK(prepared && error == channel::PluginSessionCreateError::none);
    identity = channel::PluginSessionTestAccess::transport(*prepared).first;
    auto product = channel::PluginSessionTestAccess::commit(std::move(prepared), error, events, sink);
    OMARCHY_CHECK(product && error == channel::PluginSessionCreateError::none);
    return product;
  }

  // The controlled worker reads wire packets; all host admission, I/O and
  // routing remain production code. Publishing never invokes a host callback.
  void publish(host::OwnedMessage message) {
    OMARCHY_CHECK(message.lane == host::ChannelLane::render && message.descriptors.empty());
    const auto outbound = sequence.take_outbound(wire::EndpointRole::render);
    OMARCHY_CHECK(outbound);
    std::vector<std::byte> bytes(wire::kHeaderSize + message.payload.size());
    const auto encoded = wire::encode_packet({
        .envelope_version = wire::kEnvelopeVersion,
        .header_size = wire::kHeaderSize,
        .endpoint_role = wire::EndpointRole::render,
        .message_type = message.message_type,
        .role_protocol_version = surface::kRenderRoleVersion,
        .payload_length = static_cast<std::uint32_t>(message.payload.size()),
        .launch_generation = identity.generation,
        .correlation_id = message.correlation_id,
        .lane_sequence = outbound.value}, message.payload, bytes);
    OMARCHY_CHECK(encoded);
    const auto pending = activation.state_directory() / "incoming-pending";
    std::ofstream packet(pending, std::ios::binary);
    packet.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    packet.close();
    OMARCHY_CHECK(packet);
    std::filesystem::rename(pending, activation.state_directory() /
        ("incoming-" + std::to_string(++published)));
  }

  void finish(std::unique_ptr<channel::PluginSession> &product) {
    product.reset();
    await([&] { return *runtime_factory.destructions == 1 && scope->terminations == 1; },
          "real product session did not reclaim its runtime and scope");
  }
};

host::OwnedMessage render_envelope(std::uint64_t correlation,
                                   surface::RenderMessageType type,
                                   std::vector<std::byte> payload,
                                   std::vector<host::UniqueFd> descriptors = {}) {
  return {.lane = host::ChannelLane::render,
          .message_type = static_cast<std::uint16_t>(type),
          .correlation_id = correlation,
          .payload = std::move(payload),
          .descriptors = std::move(descriptors)};
}

host::OwnedMessage render_message(std::uint64_t sequence,
                                  std::uint64_t correlation,
                                  surface::SurfaceKey key) {
  const auto encoded = surface::encode_frame_ready({
      .surface = key,
      .slot = 0,
      .slot_sequence = 2,
      .frame_sequence = sequence,
  });
  return render_envelope(correlation,
                         surface::RenderMessageType::frame_ready,
                         {encoded.begin(), encoded.end()});
}

struct Endpoint final : host::SurfaceEndpoint {
  std::size_t deliveries = 0;
  std::optional<host::OwnedAuthenticatedRenderMessage> last;
  std::vector<std::uint64_t> correlations;
  bool receive(host::OwnedAuthenticatedRenderMessage message) override {
    OMARCHY_CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
    correlations.push_back(message.correlation);
    ++deliveries;
    last.emplace(std::move(message));
    return true;
  }
};

struct Events final : channel::PluginSessionEvents {
  host::SessionState last_state = host::SessionState::idle;
  bool saw_running = false;
  std::vector<host::RouteResult> rejected;
  void state_changed(host::SessionState state, host::SessionError) override {
    last_state = state;
    saw_running = saw_running || state == host::SessionState::running;
  }
  void render_rejected(host::RouteResult result) override {
    rejected.push_back(result);
  }
};

using GestureClock = omarchy::plugin_runtime::test_support::GestureClock<1000>;

struct IntentSink final : channel::SurfaceIntentSink {
  bool accept(host::AdmittedSurfaceIntent intent) override {
    ++accepted;
    auto publication = intent.take_if_fresh();
    was_fresh = publication.has_value();
    if (!publication)
      return false;
    source = std::string(publication->source_name());
    target = std::string(publication->target_name());
    action = publication->action();
    input_sequence = publication->input_sequence();
    return was_fresh;
  }

  std::size_t accepted = 0;
  bool was_fresh = false;
  std::string source;
  std::string target;
  surface::SurfaceIntentAction action = surface::SurfaceIntentAction::open;
  std::uint64_t input_sequence = 0;
};

host::OwnedMessage
intent_message(const surface::SurfaceIntentRequest &request) {
  const auto encoded = surface::encode_surface_intent(request);
  return render_envelope(0,
                         surface::RenderMessageType::surface_intent,
                         {encoded.begin(), encoded.end()});
}

void shared_gesture_authority_has_one_concurrent_winner() {
  const auto activation = binding();
  const surface::SurfaceKey source{.id = 1,
                                   .generation = activation.generation};
  const surface::SurfaceKey target{.id = 2,
                                   .generation = activation.generation};
  for (std::uint64_t iteration = 1; iteration <= 100; ++iteration) {
    auto clock = std::make_shared<GestureClock>();
    auto latch = std::make_shared<runtime::GestureEligibilityLatch>(clock);
    host::GestureIntentAuthority intents(activation, *latch);
    OMARCHY_CHECK(intents.declare_surface(source, "bar") ==
                    host::SurfaceDeclarationResult::declared &&
                intents.declare_surface(target, "panel") ==
                    host::SurfaceDeclarationResult::declared &&
                intents.attach_surface(source) &&
                intents.arm(source, iteration));
    const definitions::DynamicInvocation::GestureClaim claim{
        .surface_id = source.id,
        .surface_generation = source.generation,
        .input_sequence = iteration,
    };
    const surface::SurfaceIntentRequest request{
        .source = source,
        .target = target,
        .input_sequence = iteration,
        .action = surface::SurfaceIntentAction::open,
        .requested_output = {},
    };
    std::barrier start(3);
    bool broker_won = false;
    bool surface_won = false;
    std::thread broker([&] {
      start.arrive_and_wait();
      broker_won = latch->consume(activation, claim).has_value();
    });
    std::thread surface_request([&] {
      start.arrive_and_wait();
      auto admitted = intents.admit(request);
      surface_won =
          admitted.intent && admitted.intent->take_if_fresh().has_value();
    });
    start.arrive_and_wait();
    broker.join();
    surface_request.join();
    OMARCHY_CHECK(broker_won != surface_won);
    OMARCHY_CHECK(!latch->consume(activation, claim) &&
                !intents.admit(request).intent);
  }
}

bool count_notification_effect(std::string_view, std::string_view, std::string_view, std::string_view, void *context) noexcept {
  ++*static_cast<std::atomic<int> *>(context);
  return true;
}

void effect_time_revocation_fences_an_authenticated_request() {
  auto definitions = packaged_registry();
  manifest::ManifestV2 manifest;
  manifest.id = "fixture.effect-fence";
  manifest.requests.push_back(capability_request(definitions, "notifications.send",
                                                 R"({"categories":["complete"]})"));
  const auto snapshot = permission_snapshot(definitions, manifest, std::string(64, 'e'), 23);
  const auto activation = snapshot.binding;
  auto live = std::make_shared<host::LiveGenerationState>(activation);
  auto barrier = std::make_shared<EffectBarrier>();
  Dispatch dispatch(live, barrier);
  std::atomic<int> provider_effects{0};
  audit::BoundedAuditLog audit_log;
  providers::LocalProvider notifications(snapshot.dynamic_grants.front(),
      definitions::EnforcementFamily::notifications, -1,
      count_notification_effect, &provider_effects);
  host::StructuredBroker broker(snapshot.binding, 91, definitions,
      {{.grant = snapshot.dynamic_grants.front(),
        .adapter = {.binding = definitions.find("notifications.send")->definition->adapter,
                    .dispatch = [&notifications](const auto &request, auto response,
                                                  std::size_t &written) noexcept {
                      return notifications.dispatch(request, response, written);
                    }}}}, audit_log, dispatch);
  auto extracted = broker.take_admission();
  OMARCHY_CHECK(extracted && extracted.admission);
  const auto payload = omarchy::plugin_runtime::test_support::notification_request();
  auto authenticated = extracted.admission->admit({
      .message_type =
          broker::kDynamicInvokeMessage,
      .correlation_id = 1,
      .payload = payload,
  });
  OMARCHY_CHECK(authenticated && authenticated.request);
  bool authority_stale = false;
  std::thread effect([&] {
    std::array<std::byte, 64> response{};
    auto transaction =
        broker.dispatch(std::move(*authenticated.request), response);
    authority_stale =
        transaction.state() == host::TransactionState::fatal &&
        transaction.fatal() == host::DispatchFatal::authority_stale;
  });
  barrier->wait_entered();
  (void)live->revoke_and_drain();
  barrier->release();
  effect.join();
  OMARCHY_CHECK(authority_stale && provider_effects == 0);
}

void product_session_intercepts_gesture_intents_before_render_routing() {
  PeerSessionFixture fixture;
  const auto &identity = fixture.identity;
  Events events;
  IntentSink sink;
  auto clock = std::make_shared<GestureClock>();
  auto product = fixture.commit({"barWidget", "panel", "overlay"}, &events, &sink, clock);
  auto shared_gesture = fixture.runtime_factory.gesture_lifetime.lock();
  OMARCHY_CHECK(shared_gesture);
  product->start();
  await([&] { return product->state() == host::SessionState::running; },
        "intent product session did not start");

  Endpoint bar;
  Endpoint panel;
  const std::array<std::uint64_t, 1> bar_correlations{701};
  const std::array<std::uint64_t, 1> panel_correlations{702};
  const auto bar_attachment =
      product->attach("barWidget", bar_correlations, bar);
  OMARCHY_CHECK(static_cast<bool>(bar_attachment));
  const surface::SurfaceKey panel_key{.id = 2,
                                      .generation = identity.generation};
  const surface::SurfaceKey overlay_key{.id = 3,
                                        .generation = identity.generation};
  const surface::SurfaceIntentRequest request{
      .source = bar_attachment.key,
      .target = panel_key,
      .input_sequence = 91,
      .action = surface::SurfaceIntentAction::toggle,
      .requested_output = {},
  };

  auto cold_target = request;
  cold_target.input_sequence = 89;
  OMARCHY_CHECK(product->arm_surface_intent(bar_attachment.key,
                                      cold_target.input_sequence));
  fixture.publish(intent_message(cold_target));
  await([&] { return sink.accepted == 1; },
        "cold unattached panel intent did not reach the shell sink");
  OMARCHY_CHECK(sink.was_fresh && sink.source == "barWidget" &&
              sink.target == "panel" && sink.input_sequence == 89);

  auto cold_overlay = request;
  cold_overlay.target = overlay_key;
  cold_overlay.input_sequence = 90;
  OMARCHY_CHECK(product->arm_surface_intent(bar_attachment.key,
                                      cold_overlay.input_sequence));
  fixture.publish(intent_message(cold_overlay));
  await([&] { return sink.accepted == 2; },
        "cold unattached overlay intent did not reach the shell sink");
  OMARCHY_CHECK(sink.was_fresh && sink.source == "barWidget" &&
              sink.target == "overlay" && sink.input_sequence == 90);

  fixture.publish(intent_message(request));
  await([&] { return events.rejected.size() == 1; },
        "intent without a gesture was not rejected");

  OMARCHY_CHECK(product->arm_surface_intent(bar_attachment.key, 90));
  auto malformed = intent_message(request);
  malformed.payload[46] = std::byte{1}; // Reserved field, with valid wire length.
  fixture.publish(std::move(malformed));
  await([&] { return events.rejected.size() == 2; },
        "malformed intent was not rejected");
  OMARCHY_CHECK(
      product->arm_surface_intent(bar_attachment.key, request.input_sequence));
  product->clear_surface_intent_eligibility(bar_attachment.key);
  fixture.publish(intent_message(request));
  await([&] { return events.rejected.size() == 3; },
        "failed input delivery did not clear intent eligibility");
  OMARCHY_CHECK(
      product->arm_surface_intent(bar_attachment.key, request.input_sequence));
  const definitions::DynamicInvocation::GestureClaim claim{
      .surface_id = bar_attachment.key.id,
      .surface_generation = bar_attachment.key.generation,
      .input_sequence = request.input_sequence,
  };
  OMARCHY_CHECK(shared_gesture->consume(product->binding(), claim).has_value());
  const definitions::DynamicInvocation::GestureClaim panel_claim{
      .surface_id = panel_key.id,
      .surface_generation = panel_key.generation,
      .input_sequence = 92,
  };
  const auto panel_attachment =
      product->attach("panel", panel_correlations, panel);
  OMARCHY_CHECK(panel_attachment && panel_attachment.key == panel_key &&
              product->arm_surface_intent(panel_attachment.key,
                                          panel_claim.input_sequence));
  product->clear_surface_intent_eligibility(bar_attachment.key);
  OMARCHY_CHECK(shared_gesture->consume(product->binding(), panel_claim).has_value());
  fixture.publish(intent_message(request));
  await([&] { return events.rejected.size() == 4; },
        "surface intent reused a gesture consumed by the broker");
  OMARCHY_CHECK(
      product->arm_surface_intent(bar_attachment.key, request.input_sequence));

  fixture.publish(intent_message(request));
  await([&] { return sink.accepted == 3; },
        "eligible intent did not reach the shell sink");
  OMARCHY_CHECK(sink.was_fresh && sink.source == "barWidget" &&
              sink.target == "panel" &&
              sink.action == surface::SurfaceIntentAction::toggle &&
              sink.input_sequence == request.input_sequence);
  OMARCHY_CHECK(!shared_gesture->consume(product->binding(), claim));
  fixture.publish(intent_message(request));
  await([&] { return events.rejected.size() == 5; },
        "replayed intent was not rejected");

  OMARCHY_CHECK(product->detach("panel", panel) &&
              product->arm_surface_intent(bar_attachment.key, 92));
  auto detached_target = request;
  detached_target.input_sequence = 92;
  fixture.publish(intent_message(detached_target));
  await([&] { return sink.accepted == 4; },
        "detached canonical target did not remain addressable");
  OMARCHY_CHECK(sink.target == "panel" && sink.input_sequence == 92);

  OMARCHY_CHECK(product->attach("panel", panel_correlations, panel) &&
              product->arm_surface_intent(bar_attachment.key, 93) &&
              product->detach("barWidget", bar));
  auto detached_source = request;
  detached_source.input_sequence = 93;
  fixture.publish(intent_message(detached_source));
  await([&] { return events.rejected.size() == 6; },
        "source detach did not clear intent eligibility");

  auto expired = request;
  expired.source = panel_key;
  expired.target = overlay_key;
  expired.input_sequence = 94;
  OMARCHY_CHECK(product->arm_surface_intent(panel_key, expired.input_sequence));
  clock->now += 5'000'000'000ULL;
  fixture.publish(intent_message(expired));
  await([&] { return events.rejected.size() == 7; },
        "injected clock expiry did not fence the real product gesture authority");

  product->revoke();
  await([&] { return product->state() == host::SessionState::revoked; },
        "intent session did not revoke");
  OMARCHY_CHECK(!product->arm_surface_intent(bar_attachment.key, 94));
  auto revoked = request;
  revoked.input_sequence = 94;
  fixture.publish(intent_message(revoked));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  OMARCHY_CHECK(sink.accepted == 4 && bar.deliveries == 0 && panel.deliveries == 0);
  fixture.finish(product);
}

void product_session_routes_two_surfaces_over_one_launch() {
  PeerSessionFixture fixture;
  const auto &identity = fixture.identity;
  Events events;
  auto product = fixture.commit({"barWidget", "panel"}, &events);

  Endpoint first;
  Endpoint second;
  const std::array<std::uint64_t, 1> first_correlations{101};
  const std::array<std::uint64_t, 1> second_correlations{202};
  OMARCHY_CHECK(product->attach("barWidget", first_correlations, first).status ==
              channel::SurfaceAttachStatus::session_not_running);

  product->start();
  await([&] { return product->state() == host::SessionState::running; },
        "product session did not start");
  channel::PluginSessionTestAccess::set_surface_attach_fault(
      *product, channel::SurfaceAttachFault::after_router);
  OMARCHY_CHECK(product->attach("barWidget", first_correlations, first).status ==
                  channel::SurfaceAttachStatus::allocation_failed &&
              product->surface_count() == 0);
  OMARCHY_CHECK(product->attach("missing", first_correlations, first).status ==
              channel::SurfaceAttachStatus::undeclared_surface);
  const auto first_attachment =
      product->attach("barWidget", first_correlations, first);
  OMARCHY_CHECK(first_attachment.status == channel::SurfaceAttachStatus::attached);
  OMARCHY_CHECK(product->attach("panel", first_correlations, second).status ==
                  channel::SurfaceAttachStatus::invalid_correlations &&
              product->surface_count() == 1);
  const auto second_attachment =
      product->attach("panel", second_correlations, second);
  OMARCHY_CHECK(second_attachment.status == channel::SurfaceAttachStatus::attached);
  OMARCHY_CHECK(first_attachment.key ==
                  surface::SurfaceKey{.id = 1, .generation = 17} &&
              second_attachment.key ==
                  surface::SurfaceKey{.id = 2, .generation = 17});
  OMARCHY_CHECK(product->attach("barWidget", first_correlations, second).status ==
              channel::SurfaceAttachStatus::already_attached);
  const std::array<std::uint64_t, 9> invalid_correlations{};
  OMARCHY_CHECK(product->attach("barWidget", invalid_correlations, second).status ==
              channel::SurfaceAttachStatus::already_attached);
  OMARCHY_CHECK(!product->detach("barWidget", second));

  fixture.publish(render_message(1, 0, first_attachment.key));
  fixture.publish(render_message(2, 0, second_attachment.key));
  fixture.publish(render_message(3, 0, {.id = 99, .generation = identity.generation}));
  const auto second_key = surface::encode_surface_key(second_attachment.key);
  fixture.publish(render_envelope(101, surface::RenderMessageType::surface_allocated,
                                  {second_key.begin(), second_key.end()}));
  fixture.publish(render_message(5, 0,
      {.id = first_attachment.key.id, .generation = first_attachment.key.generation + 1}));
  const bool routed = await([&] { return first.deliveries == 1 && second.deliveries == 1 &&
                                        events.rejected.size() == 3; });
  require(routed, "real render routing: first=" + std::to_string(first.deliveries) +
      " second=" + std::to_string(second.deliveries) + " rejected=" +
      std::to_string(events.rejected.size()) + " state=" +
      std::to_string(static_cast<unsigned>(product->state())) + " error=" +
      std::to_string(static_cast<unsigned>(product->error())));
  OMARCHY_CHECK(fixture.scope->attachments == 1 &&
      events.rejected == std::vector<host::RouteResult>({
          host::RouteResult::unknown_surface, host::RouteResult::conflicting_destination,
          host::RouteResult::stale_generation}) && product->surface_count() == 2);
  const auto first_key = surface::encode_surface_key(first_attachment.key);
  fixture.publish(render_envelope(101, surface::RenderMessageType::surface_allocated,
                                  {first_key.begin(), first_key.end()}));
  await([&] { return first.deliveries == 2; }, "allocation acknowledgement did not reach its endpoint");
  OMARCHY_CHECK(first.last && first.last->descriptors.empty() &&
                first.last->message_type == static_cast<std::uint16_t>(surface::RenderMessageType::surface_allocated));

  product->revoke();
  await([&] { return product->state() == host::SessionState::revoked; },
        "product session did not revoke");
  OMARCHY_CHECK(!fixture.activation.live()->current(product->binding()) && product->surface_count() == 0);
  OMARCHY_CHECK(!product->detach("barWidget", first) && !product->detach("panel", second));
  OMARCHY_CHECK(product->attach("barWidget", first_correlations, first).status ==
                channel::SurfaceAttachStatus::session_not_running);
  fixture.publish(render_message(8, 0, first_attachment.key));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  OMARCHY_CHECK(first.deliveries == 2);
  fixture.finish(product);
}

std::unique_ptr<channel::PluginSession> commit_activation(
    launcher::Supervisor supervisor, host::ActivationSnapshot snapshot,
    channel::SessionRuntimeFactory &runtime_factory,
    std::string_view label, host::SessionLimits limits = {},
    channel::PluginSessionEvents *events = nullptr) {
  channel::PluginSessionCreateError error{};
  auto prepared = channel::PluginSessionTestAccess::prepare_from_activation(
      std::move(supervisor), std::move(snapshot), runtime_factory, error, limits);
  require(prepared && error == channel::PluginSessionCreateError::none,
          std::string(label) + " did not prepare: " +
              std::to_string(static_cast<int>(error)));
  auto product = channel::PluginSessionTestAccess::commit(
      std::move(prepared), error, events);
  require(product && error == channel::PluginSessionCreateError::none,
          std::string(label) + " did not commit");
  return product;
}

struct PreparedChannelFixture {
  ActivationFixture activation;
  RuntimeFactory runtime_factory;
  std::shared_ptr<Scope> scope = std::make_shared<Scope>();
  std::unique_ptr<channel::PreparedPluginSession> prepared;
  launcher::Deadline deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

  explicit PreparedChannelFixture(std::string_view mode = "session-idle", bool handshake = true)
      : activation(mode) {
    channel::PluginSessionCreateError error{};
    prepared = channel::PluginSessionTestAccess::prepare_from_activation(
        launcher::test_support::make_supervisor(FAKE_BWRAP_PATH, CHANNEL_PEER_PATH, scope),
        activation.snapshot(), runtime_factory, error);
    OMARCHY_CHECK(prepared && error == channel::PluginSessionCreateError::none);
    auto [identity, transport] = channel::PluginSessionTestAccess::transport(*prepared);
    OMARCHY_CHECK(transport.launch(identity, deadline) == host::ChannelError::none);
    if (handshake)
      OMARCHY_CHECK(transport.handshake(deadline) == host::ChannelError::none);
  }

  void finish() {
    prepared.reset();
    await([&] { return *runtime_factory.destructions == 1 && scope->terminations == 1; },
          "real channel did not release its runtime and scope exactly once");
  }
};

host::OwnedMessage queued_render(std::uint64_t correlation = 41) {
  return {.lane = host::ChannelLane::render,
          .message_type = static_cast<std::uint16_t>(surface::RenderMessageType::surface_allocate),
          .correlation_id = correlation,
          .payload = std::vector<std::byte>(96, std::byte{0x11}), .descriptors = {}};
}

void real_session_queue_fences() {
  using omarchy::plugin_runtime::test_support::await_without_ui_dispatch;
  // Each ceiling independently rejects a queued message and releases its FD.
  // Revocation must release admitted descriptors before worker/UI dispatch.
  for (int ceiling = 0; ceiling < 3; ++ceiling) {
    PeerSessionFixture fixture("session-idle");
    host::SessionLimits limits;
    if (ceiling == 0) limits.maximum_queued_messages = 1;
    if (ceiling == 1) limits.maximum_queued_bytes = 96;
    if (ceiling == 2) {
      limits.maximum_queued_descriptors = 1;
      limits.maximum_descriptors_per_message = 1;
    }
    auto product = fixture.commit({}, nullptr, nullptr, {}, limits);
    auto &io = channel::PluginSessionTestAccess::io(*product);
    int admitted_pipe[2], rejected_pipe[2];
    OMARCHY_CHECK(::pipe2(admitted_pipe, O_CLOEXEC | O_NONBLOCK) == 0);
    OMARCHY_CHECK(::pipe2(rejected_pipe, O_CLOEXEC | O_NONBLOCK) == 0);
    host::UniqueFd admitted_reader(admitted_pipe[0]), rejected_reader(rejected_pipe[0]);
    auto first = queued_render();
    first.descriptors.emplace_back(admitted_pipe[1]);
    OMARCHY_CHECK(io.enqueue(std::move(first)));
    auto excess = queued_render();
    excess.descriptors.emplace_back(rejected_pipe[1]);
    OMARCHY_CHECK(!io.enqueue(std::move(excess)));
    std::byte byte{};
    OMARCHY_CHECK(io.error() == host::SessionError::queue_limit &&
                  ::read(rejected_reader.get(), &byte, 1) == 0);
    OMARCHY_CHECK(::read(admitted_reader.get(), &byte, 1) == -1 && errno == EAGAIN);
    io.revoke();
    OMARCHY_CHECK(!io.enqueue(queued_render()) && ::read(admitted_reader.get(), &byte, 1) == 0);
    product.reset();
    await_without_ui_dispatch([&] { return *fixture.runtime_factory.destructions == 1; },
                              "unstarted channel was not reclaimed without UI dispatch");
  }

  // Hold delivery on the UI while real authenticated packets fill the queue.
  // Both directions share the same allowance, and queued messages cannot
  // cross the revocation epoch even when the worker has already accepted them.
  for (int test = 0; test < 6; ++test) {
    const int action = test % 3;
    const int ceiling = test / 3;
    PeerSessionFixture fixture;
    Endpoint endpoint;
    host::SessionLimits limits;
    if (ceiling == 0) limits.maximum_queued_messages = 2;
    if (ceiling == 1) limits.maximum_queued_bytes = 2 * surface::SurfaceKeyLayout::size;
    limits.io_timeout = std::chrono::milliseconds(100);
    auto product = fixture.commit({"bar"}, nullptr, nullptr, {}, limits);
    auto &io = channel::PluginSessionTestAccess::io(*product);
    io.start();
    await_without_ui_dispatch([&] { return io.state() == host::SessionState::running; },
                              "real I/O startup stalled without UI dispatch");
    const std::array<std::uint64_t, 3> correlations{11, 12, 13};
    const auto attached = product->attach("bar", correlations, endpoint);
    OMARCHY_CHECK(attached);
    const auto key = surface::encode_surface_key(attached.key);
    const auto incoming = [&key](std::uint64_t correlation) {
      return host::OwnedMessage{.lane = host::ChannelLane::render,
          .message_type = static_cast<std::uint16_t>(surface::RenderMessageType::surface_allocated),
          .correlation_id = correlation,
          .payload = {key.begin(), key.end()}, .descriptors = {}};
    };
    fixture.publish(incoming(11));
    fixture.publish(incoming(12));
    await_without_ui_dispatch([&] {
      return host::PluginSessionIoTestAccess::pending_deliveries(io) == 2;
    }, "real inbound packets did not reach the held delivery queue");
    auto excess = incoming(41);
    OMARCHY_CHECK(endpoint.correlations.empty() && !io.enqueue(std::move(excess)));
    if (action == 0) {
      await([&] { return endpoint.correlations.size() == 2; }, "FIFO delivery stalled");
      OMARCHY_CHECK((endpoint.correlations == std::vector<std::uint64_t>{11, 12}));
    } else if (action == 1) {
      io.revoke();
      await([&] { return io.state() == host::SessionState::revoked; }, "revocation stalled");
      QCoreApplication::processEvents();
      OMARCHY_CHECK(endpoint.correlations.empty() && !io.enqueue(queued_render()));
    } else {
      fixture.publish(incoming(13));
      await_without_ui_dispatch([&] { return io.state() == host::SessionState::failed; },
                                "inbound delivery overflow did not fail closed");
      OMARCHY_CHECK(io.error() == host::SessionError::queue_limit);
    }
    const auto before = std::chrono::steady_clock::now();
    product.reset();
    OMARCHY_CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100));
    await_without_ui_dispatch([&] {
      return *fixture.runtime_factory.destructions == 1 && fixture.scope->terminations == 1;
    }, "real channel teardown depended on UI dispatch");
  }
}

void real_session_backpressure() {
  using omarchy::plugin_runtime::test_support::await_without_ui_dispatch;
  PeerSessionFixture fixture("session-host-saturation");
  host::SessionLimits limits;
  limits.io_timeout = std::chrono::milliseconds(100);
  auto product = fixture.commit({}, nullptr, nullptr, {}, limits);
  auto &io = channel::PluginSessionTestAccess::io(*product);
  io.start();
  await_without_ui_dispatch([&] { return io.state() == host::SessionState::running; },
                            "saturation session failed to start");
  unsigned admitted = 0;
  // The peer does not read until signalled. Fill the actual socket and queue;
  // retrying admission must never drop or duplicate an accepted packet/FD.
  const auto fill_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  do {
    auto message = queued_render();
    message.descriptors.emplace_back(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    OMARCHY_CHECK(message.descriptors.front());
    if (io.enqueue(std::move(message))) ++admitted;
    io.wake();
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < fill_until);
  OMARCHY_CHECK(admitted > limits.maximum_queued_messages &&
                io.state() == host::SessionState::running &&
                host::PluginSessionIoTestAccess::pending_outbound(io) == limits.maximum_queued_messages);
  OMARCHY_CHECK(::kill(fixture.scope->peer_pid, SIGUSR1) == 0);
  await_without_ui_dispatch([&] {
    return host::PluginSessionIoTestAccess::pending_outbound(io) == 0;
  }, "writable readiness did not drain queued descriptors");
  const auto key = surface::encode_surface_key({.id = 1, .generation = 1});
  OMARCHY_CHECK(io.enqueue({.lane = host::ChannelLane::render,
      .message_type = static_cast<std::uint16_t>(surface::RenderMessageType::surface_suspend),
      .correlation_id = 0, .payload = {key.begin(), key.end()}, .descriptors = {}}));
  await([&] { return std::filesystem::exists(fixture.activation.state_directory() / "drain-verified"); },
        "peer did not verify FIFO packets and descriptors after queue saturation");
  unsigned received = 0;
  std::ifstream(fixture.activation.state_directory() / "drain-verified") >> received;
  OMARCHY_CHECK(received == admitted + 1 && io.state() == host::SessionState::running);
  fixture.finish(product);
}

void real_channel_admission_and_revocation() {
  const struct {
    host::ChannelLane lane;
    std::uint16_t type;
    std::size_t descriptors = 0;
  } rejected[]{
      {host::ChannelLane::control, wire::kSettingsSnapshotMessage},
      {host::ChannelLane::control, wire::kPresentationSnapshotMessage},
      {host::ChannelLane::control, wire::kPermissionSnapshotMessage},
      {host::ChannelLane::broker, 0x0100},
      {static_cast<host::ChannelLane>(255), 0x0100},
      {host::ChannelLane::render, 0x2010, launcher::kMaximumTransportDescriptors + 1},
  };
  for (const auto &test : rejected) {
    PreparedChannelFixture fixture;
    auto [identity, transport] = channel::PluginSessionTestAccess::transport(*fixture.prepared);
    host::OwnedMessage message{.lane = test.lane, .message_type = test.type,
                               .payload = std::vector<std::byte>(96), .descriptors = {}};
    for (std::size_t index = 0; index < test.descriptors; ++index) {
      message.descriptors.emplace_back(::open("/dev/null", O_RDONLY | O_CLOEXEC));
      OMARCHY_CHECK(message.descriptors.back());
    }
    OMARCHY_CHECK(transport.send(message, fixture.deadline) == host::SendStatus::fatal &&
                  transport.send(message, fixture.deadline) == host::SendStatus::fatal);
    fixture.finish();
  }
  for (const auto mutate : {
           +[](host::SessionToken &token) { token.plugin_id += ".spoof"; },
           +[](host::SessionToken &token) { token.revision_sha256[0] = 'c'; },
           +[](host::SessionToken &token) { ++token.generation; },
           +[](host::SessionToken &token) { ++token.session_nonce; }}) {
    PreparedChannelFixture fixture;
    auto [identity, transport] = channel::PluginSessionTestAccess::transport(*fixture.prepared);
    auto wrong = identity;
    mutate(wrong);
    OMARCHY_CHECK(!transport.revoke(wrong, fixture.deadline) && fixture.scope->terminations == 0);
    OMARCHY_CHECK(transport.revoke(identity, fixture.deadline));
    transport.terminate(fixture.deadline);
    fixture.finish();
  }
}

void real_channel_backpressure() {
  for (int change = 0; change < 7; ++change) {
    PreparedChannelFixture fixture("session-host-saturation");
    auto [identity, transport] = channel::PluginSessionTestAccess::transport(*fixture.prepared);
    const auto key = surface::encode_surface_key({.id = 1, .generation = 1});
    host::OwnedMessage message{
        .lane = host::ChannelLane::render,
        .message_type = static_cast<std::uint16_t>(surface::RenderMessageType::surface_allocate),
        .correlation_id = 41, .payload = std::vector<std::byte>(96, std::byte{0x11}), .descriptors = {}};
    message.descriptors.emplace_back(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    OMARCHY_CHECK(message.descriptors.back());
    const int borrowed = message.descriptors.front().get();
    unsigned sent_packets = 0;
    host::SendStatus status = host::SendStatus::complete;
    for (unsigned attempt = 0; attempt < 10000; ++attempt) {
      status = transport.send(message, fixture.deadline);
      if (status != host::SendStatus::complete)
        break;
      ++sent_packets;
    }
    require(status == host::SendStatus::would_block,
            "saturation failed: status=" + std::to_string(static_cast<int>(status)));
    OMARCHY_CHECK(transport.send(message, fixture.deadline) == host::SendStatus::would_block);
    if (change == 0) {
      OMARCHY_CHECK(::kill(fixture.scope->peer_pid, SIGUSR1) == 0);
      const auto send_ready = [&](std::string_view failure) {
        status = host::SendStatus::would_block;
        await([&] {
          if (status == host::SendStatus::would_block)
            status = transport.send(message, fixture.deadline);
          return status != host::SendStatus::would_block;
        }, failure);
        require(status == host::SendStatus::complete,
                std::string(failure) + ": status=" + std::to_string(static_cast<int>(status)));
      };
      send_ready("real socket did not resume its pending send");
      OMARCHY_CHECK(::fcntl(borrowed, F_GETFD) >= 0);
      message.message_type = static_cast<std::uint16_t>(surface::RenderMessageType::surface_suspend);
      message.correlation_id = 0;
      message.payload.assign(key.begin(), key.end());
      message.descriptors.clear();
      send_ready("saturation drain marker");
      await([&] { return std::filesystem::exists(
          fixture.activation.state_directory() / "drain-verified"); },
          "peer did not verify exact payload and contiguous retry sequence");
      unsigned received_packets = 0;
      std::ifstream(fixture.activation.state_directory() / "drain-verified") >> received_packets;
      OMARCHY_CHECK(received_packets == sent_packets + 2 && ::fcntl(borrowed, F_GETFD) == -1);
    } else {
      if (change == 1)
        message.payload[0] = std::byte{2};
      else if (change == 2)
        ++message.correlation_id;
      else if (change == 4)
        message.lane = host::ChannelLane::control;
      else if (change == 5)
        ++message.message_type;
      else if (change == 6)
        message.payload.push_back(std::byte{0});
      else {
        message.descriptors.emplace_back(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        OMARCHY_CHECK(message.descriptors.back());
      }
      OMARCHY_CHECK(transport.send(message, fixture.deadline) == host::SendStatus::fatal);
      OMARCHY_CHECK(::fcntl(borrowed, F_GETFD) >= 0);
      if (change == 3)
        OMARCHY_CHECK(::fcntl(message.descriptors.front().get(), F_GETFD) >= 0);
    }
    fixture.finish();
  }
}

void real_channel_expiry_and_wake_fences() {
  for (int operation = 0; operation < 3; ++operation) {
    PreparedChannelFixture fixture("session-idle", operation != 0);
    auto [identity, transport] = channel::PluginSessionTestAccess::transport(*fixture.prepared);
    const auto expired = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    if (operation == 0) {
      OMARCHY_CHECK(transport.handshake(expired) == host::ChannelError::handshake_failed);
    } else if (operation == 1) {
      const auto received = transport.receive(expired);
      OMARCHY_CHECK(received.status == host::ReceiveStatus::fatal && received.message.payload.empty());
    } else {
      const host::SessionWakeHandler wake{.function = [](void *) noexcept {}, .context = nullptr};
      OMARCHY_CHECK(transport.install_wake_handler(wake) && !transport.install_wake_handler(wake));
      transport.clear_wake_handler();
      OMARCHY_CHECK(transport.install_wake_handler(wake));
      OMARCHY_CHECK(transport.revoke(identity, fixture.deadline));
      OMARCHY_CHECK(!transport.install_wake_handler(wake));
    }
    fixture.finish();
  }
}

void real_product_rejects_invalid_worker_render_packets() {
  for (int fault = 0; fault < 5; ++fault) {
    PeerSessionFixture fixture(fault == 4 ? "session-scripted-descriptor" : "session-scripted");
    auto product = fixture.commit({"bar"});
    product->start();
    await([&] { return product->state() == host::SessionState::running; },
          "render rejection fixture did not start");
    Endpoint endpoint;
    const std::array<std::uint64_t, 1> correlations{101};
    const auto attached = product->attach("bar", correlations, endpoint);
    OMARCHY_CHECK(attached);
    auto message = render_message(1, 0, attached.key);
    if (fault == 0)
      message.payload.pop_back();
    else if (fault == 1)
      message.correlation_id = 101;
    else if (fault == 2) {
      message.message_type = static_cast<std::uint16_t>(surface::RenderMessageType::surface_allocate);
      message.correlation_id = 101;
      message.payload.resize(96);
    } else if (fault == 3) {
      message.message_type = static_cast<std::uint16_t>(surface::RenderMessageType::surface_intent);
      message.payload.resize(175);
    }
    fixture.publish(std::move(message));
    await([&] { return product->state() == host::SessionState::failed && product->surface_count() == 0; },
          "invalid worker packet did not fail the real product channel");
    OMARCHY_CHECK(endpoint.deliveries == 0 && product->surface_count() == 0 &&
                  product->attach("bar", correlations, endpoint).status ==
                      channel::SurfaceAttachStatus::session_not_running);
    fixture.finish(product);
  }
}

void prepared_commit_retains_activation_and_reuses_one_launch() {
  ActivationFixture fixture;
  RuntimeFactory runtime_factory;
  auto scope = std::make_shared<Scope>();
  auto product = commit_activation(
      launcher::test_support::make_supervisor(FAKE_BWRAP_PATH,
                                              CHANNEL_PEER_PATH, scope),
      fixture.snapshot(), runtime_factory, "product session");
  OMARCHY_CHECK(runtime_factory.calls == 1 && runtime_factory.saw_manifest &&
              runtime_factory.descriptors_valid &&
              runtime_factory.gesture_authority_valid &&
              runtime_factory.live_generation_valid);
  OMARCHY_CHECK(product->manifest().surface_names ==
              std::vector<std::string>({"bar", "panel", "overlay"}) &&
              product->binding() == grants().binding &&
              ::fcntl(channel::PluginSessionTestAccess::activation_record_fd(
                          *product),
                      F_GETFD) >= 0);

  product->start();
  await([&] {
    return product->state() == host::SessionState::running ||
           product->state() == host::SessionState::failed;
  }, "public product session did not settle startup");
  require(product->state() == host::SessionState::running,
          "public product session failed startup with error " +
              std::to_string(static_cast<unsigned>(product->error())));
  Endpoint bar;
  Endpoint panel;
  Endpoint overlay;
  Endpoint panel_replacement;
  const std::array<std::uint64_t, 1> bar_correlations{501};
  const std::array<std::uint64_t, 1> panel_correlations{502};
  const std::array<std::uint64_t, 1> overlay_correlations{503};
  OMARCHY_CHECK(product->attach("bar", bar_correlations, bar) &&
              product->attach("panel", panel_correlations, panel) &&
              product->attach("overlay", overlay_correlations, overlay));
  OMARCHY_CHECK(scope->attachments == 1);
  OMARCHY_CHECK(product->detach("panel", panel) &&
              product->attach("panel", panel_correlations, panel_replacement));
  OMARCHY_CHECK(scope->attachments == 1);

  product->stop();
  await([&] { return product->state() == host::SessionState::stopped; },
        "public product session did not stop");
  OMARCHY_CHECK(product->surface_count() == 0 &&
              product->attach("bar", bar_correlations, bar).status ==
                  channel::SurfaceAttachStatus::session_not_running);
  product.reset();
  await(
      [&] {
    return *runtime_factory.destructions == 1 &&
           runtime_factory.gesture_lifetime.expired();
  },
        "owned broker/provider runtime outlived session teardown");
}

void delayed_provider_replies_settle_without_restarting_session() {
  definitions::TrustedDefinitionRegistry definitions;
  OMARCHY_CHECK(definitions.install(echo_definition(),
                              3));
  const auto resolved = definitions.find("service.echo");
  OMARCHY_CHECK(resolved.has_value());
  auto verified_manifest = provider_manifest(*resolved);
  auto snapshot = provider_grants(*resolved, verified_manifest);
  const auto invocation = provider_invocation(snapshot);
  ActivationFixture fixture("session-provider", invocation, snapshot.binding);
  ProviderProfileFixture provider;
  channel::RuntimeServices services;
  services.provider_catalog = provider.catalog();
  channel::SessionRuntimeFactory runtime_factory(std::move(definitions),
                                                 std::move(services));
  auto worker_scope = std::make_shared<Scope>();
  auto product = commit_activation(
      launcher::test_support::make_supervisor(FAKE_BWRAP_PATH,
                                              CHANNEL_PEER_PATH, worker_scope),
      fixture.snapshot(std::move(verified_manifest), std::move(snapshot)),
      runtime_factory, "provider-backed session", channel::provider_backed_session_limits());

  product->start();
  await([&] { return product->state() == host::SessionState::running; },
        "provider-backed session did not start");
  const auto replies = fixture.state_directory() / "provider-replies";
  await(
      [&] {
        return std::filesystem::exists(replies) ||
               product->state() == host::SessionState::failed;
      },
      "delayed provider replies did not cross the authenticated channel");
  require(std::filesystem::exists(replies),
          "delayed provider session failed with state " +
              std::to_string(static_cast<unsigned>(product->state())) +
              ", error " +
              std::to_string(static_cast<unsigned>(product->error())) +
              ", provider starts " + std::to_string(provider.starts()));
  const auto provider_pid = provider.pid();
  const auto expected = "81 " + std::to_string(provider_pid) + "\n82 " +
                        std::to_string(provider_pid) + "\n";
  OMARCHY_CHECK(read_file(replies) == expected && provider.starts() == 1 &&
              ::kill(provider_pid, 0) == 0 &&
              product->state() == host::SessionState::running &&
              product->error() == host::SessionError::none &&
              worker_scope->attachments == 1);

  product->stop();
  await([&] { return product->state() == host::SessionState::stopped; },
        "provider-backed session did not stop");
  product.reset();
  await(
      [&] {
        errno = 0;
        return ::kill(provider_pid, 0) < 0 && errno == ESRCH;
      },
      "provider process survived session teardown");
  errno = 0;
  OMARCHY_CHECK(::waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD);
}

void startup_ack_failures_never_publish_product_session() {
  using namespace std::chrono_literals;
  for (const std::string_view stage : {"settings", "presentation", "permissions"}) {
    for (const std::string_view fault : {
             "session-startup-wrong-generation",
             "session-startup-wrong-role",
             "session-startup-wrong-type",
             "session-startup-wrong-correlation",
             "session-startup-payload",
             "session-startup-descriptor",
             "session-startup-peer-loss",
             "session-startup-missing",
             "session-startup-duplicate",
         }) {
      const auto mode = std::string(fault) + "-" + std::string(stage);
      ActivationFixture fixture(mode);
      RuntimeFactory runtime_factory;
      auto scope = std::make_shared<Scope>();
      host::SessionLimits limits;
      limits.startup_timeout = fault == "session-startup-missing" ? 100ms : 2s;
      Events events;
      auto product = commit_activation(
          launcher::test_support::make_supervisor(FAKE_BWRAP_PATH,
                                                  CHANNEL_PEER_PATH, scope),
          fixture.snapshot(), runtime_factory, "startup rejection fixture", limits, &events);
      product->start();
      if (fault == "session-startup-duplicate") {
        await([&] { return product->state() == host::SessionState::running && events.saw_running; },
              "duplicate acknowledgement fixture never published its running state");
        std::ofstream(fixture.state_directory() / "release-startup-ack") << '\n';
      }
      await([&] { return product->state() == host::SessionState::failed; },
            mode + " left a product session published");
      await([&] { return events.last_state == host::SessionState::failed; },
            "startup failure did not withdraw observer publication");
      const auto expected_error =
          fault == "session-startup-missing"
              ? host::SessionError::startup_deadline_expired
              : host::SessionError::channel_failed;
      // Post-startup rejection may consume the I/O budget while terminating the peer.
      require(product->error() == expected_error ||
                  (fault == "session-startup-duplicate" &&
                   product->error() == host::SessionError::io_deadline_expired),
              mode + " reported startup failure " +
                  std::to_string(static_cast<unsigned>(product->error())));
      Endpoint endpoint;
      const std::array<std::uint64_t, 1> correlations{1};
      require(product->attach("bar", correlations, endpoint).status ==
                  channel::SurfaceAttachStatus::session_not_running,
              std::string(mode) + " retained a surface publication path");
      require(scope->attachments == 1,
              std::string(mode) + " sandbox launch count was " +
                  std::to_string(scope->attachments.load()) + " with error " +
                  std::to_string(static_cast<unsigned>(product->error())));
      product.reset();
      await([&] { return *runtime_factory.destructions == 1; },
            "failed startup retained its runtime authority");
      await([&] { return scope->terminations.load() == 1; },
            "failed startup did not remove its resource scope");
      require(scope->terminations == 1,
              std::string(mode) + " cleanup count was termination=" +
                  std::to_string(scope->terminations.load()));
    }
  }
}

void authority_loss_between_snapshot_and_ack_never_publishes() {
  for (const std::string_view stage : {"settings", "presentation", "permissions"}) {
    for (const bool public_revoke : {false, true}) {
      ActivationFixture fixture("session-startup-authority-loss-" + std::string(stage));
      RuntimeFactory runtime_factory;
      auto scope = std::make_shared<Scope>();
      host::SessionLimits limits;
      limits.startup_timeout = std::chrono::milliseconds(250);
      Events events;
      auto product = commit_activation(
          launcher::test_support::make_supervisor(FAKE_BWRAP_PATH,
                                                  CHANNEL_PEER_PATH, scope),
          fixture.snapshot(), runtime_factory, "authority-loss startup fixture", limits, &events);
      product->start();
      await(
          [&] {
            return std::filesystem::exists(
                fixture.state_directory() / "startup-snapshot-received");
          },
          "worker did not receive the projected snapshot before authority loss");
      if (public_revoke)
        product->revoke();
      else
        OMARCHY_CHECK(fixture.live()->revoke_and_drain() ==
                      host::LiveGenerationRevokeResult::drained);
      const auto terminal = public_revoke ? host::SessionState::revoked : host::SessionState::failed;
      std::ofstream(fixture.state_directory() / "release-startup-ack") << '\n';
      await([&] { return product->state() == terminal; },
            "post-revocation startup acknowledgement did not fail closed");
      Endpoint endpoint;
      const std::array<std::uint64_t, 1> correlations{1};
      OMARCHY_CHECK(product->error() == (public_revoke ? host::SessionError::none : host::SessionError::channel_failed) &&
                  !events.saw_running &&
                  product->attach("bar", correlations, endpoint).status ==
                      channel::SurfaceAttachStatus::session_not_running &&
                  scope->attachments == 1);
      product.reset();
      await([&] {
        return *runtime_factory.destructions == 1 && scope->terminations == 1;
      }, "authority-loss startup did not clean up exactly once");
      OMARCHY_CHECK(scope->terminations == 1);
    }
  }
}

bool real_qml_worker_startup(bool valid_qml) {
  if (!std::filesystem::exists("/usr/bin/bwrap"))
    return false;
  InvalidQmlWorkerFixture fixture(valid_qml);
  RuntimeFactory runtime_factory;
  auto scope = std::make_shared<Scope>();
  host::SessionLimits limits;
  limits.startup_timeout = std::chrono::seconds(3);
  Events events;
  auto product = commit_activation(
      launcher::test_support::make_supervisor("/usr/bin/bwrap",
                                              QML_WORKER_PATH, scope),
      fixture.snapshot(), runtime_factory, "real-QML worker fixture", limits, &events);
  product->start();
  await([&] {
    return (product->state() == host::SessionState::running && events.saw_running) ||
           product->state() == host::SessionState::failed;
  }, "real-QML worker startup did not settle");
  if (scope->attachments == 0)
    return false;
  if (valid_qml) {
    require(product->state() == host::SessionState::running &&
                product->error() == host::SessionError::none &&
                events.saw_running && scope->attachments == 1,
            "ordered startup snapshots did not reach a running worker: state=" +
                std::to_string(static_cast<unsigned>(product->state())) +
                " error=" + std::to_string(static_cast<unsigned>(product->error())) +
                " saw-running=" + std::to_string(events.saw_running) +
                " attachments=" + std::to_string(scope->attachments.load()));
    product->stop();
    await([&] { return product->state() == host::SessionState::stopped; },
          "valid-QML worker did not stop");
  } else {
    Endpoint endpoint;
    const std::array<std::uint64_t, 1> correlations{1};
    OMARCHY_CHECK(product->state() == host::SessionState::failed &&
                product->error() == host::SessionError::channel_failed &&
                !events.saw_running &&
                product->attach("bar", correlations, endpoint).status ==
                    channel::SurfaceAttachStatus::session_not_running &&
                scope->attachments == 1);
  }
  product.reset();
  await([&] {
    return *runtime_factory.destructions == 1 && scope->terminations == 1;
  }, "real-QML worker did not clean up exactly once");
  require(scope->terminations == 1,
          "real-QML worker cleanup count was termination=" +
              std::to_string(scope->terminations.load()));
  return true;
}

void prepared_session_is_thread_agnostic_before_commit() {
  ActivationFixture fixture;
  RuntimeFactory runtime_factory;
  channel::PluginSessionCreateError create_error{};
  auto prepared = channel::PluginSessionTestAccess::prepare_from_activation(
      launcher::test_support::make_supervisor(
          FAKE_BWRAP_PATH, CHANNEL_PEER_PATH, std::make_shared<Scope>()),
      fixture.snapshot(), runtime_factory, create_error);
  OMARCHY_CHECK(prepared && create_error == channel::PluginSessionCreateError::none &&
              runtime_factory.calls == 1 && *runtime_factory.destructions == 0);
  const auto destructions = runtime_factory.destructions;
  std::thread discard(
      [prepared = std::move(prepared)]() mutable { prepared.reset(); });
  discard.join();
  OMARCHY_CHECK(*destructions == 1);
}

void preparation_rejects_invalid_grant_snapshots() {
  ActivationFixture fixture;
  const auto rejected = [&](host::ActivationSnapshot snapshot,
                            std::string_view message) {
    RuntimeFactory runtime_factory;
    channel::PluginSessionCreateError create_error{};
    auto prepared = channel::PluginSessionTestAccess::prepare_from_activation(
        launcher::test_support::make_supervisor(
            FAKE_BWRAP_PATH, CHANNEL_PEER_PATH, std::make_shared<Scope>()),
        std::move(snapshot), runtime_factory, create_error);
    require(!prepared &&
                create_error ==
                    channel::PluginSessionCreateError::invalid_activation &&
                runtime_factory.calls == 0,
            message);
  };

  const auto registry = packaged_registry();
  const auto notification = capability_request(registry, "notifications.send",
                                                R"({"categories":["complete"]})");
  auto source_mismatch = fixture.snapshot();
  source_mismatch.manifest.requests.push_back(notification);
  rejected(std::move(source_mismatch),
           "manifest/grant source mismatch reached runtime creation");

  auto invalid_requests = fixture.snapshot();
  invalid_requests.grants.dynamic_grants.push_back({});
  rejected(std::move(invalid_requests),
           "invalid request set reached runtime creation");

  auto invalid_grants = fixture.snapshot();
  invalid_grants.grants.dynamic_grants.push_back(
      capability_grant(registry, notification, invalid_grants.grants.binding));
  rejected(std::move(invalid_grants),
           "undeclared grant set reached runtime creation");

  auto policy_mismatch = fixture.snapshot();
  policy_mismatch.grants.binding.policy_fingerprint =
      permissions::Digest(std::string(64, 'c'));
  policy_mismatch.live = std::make_shared<host::LiveGenerationState>(
      policy_mismatch.grants.binding);
  rejected(std::move(policy_mismatch),
           "policy request fingerprint mismatch reached runtime creation");

  auto manifest_projection_mismatch = fixture.snapshot();
  auto declared_manifest = manifest_projection_mismatch.manifest;
  declared_manifest.requests.push_back(notification);
  manifest_projection_mismatch.grants = permission_snapshot(
      registry, declared_manifest,
      manifest_projection_mismatch.grants.binding.revision.view(),
      manifest_projection_mismatch.grants.binding.generation);
  manifest_projection_mismatch.live =
      std::make_shared<host::LiveGenerationState>(
          manifest_projection_mismatch.grants.binding);
  rejected(std::move(manifest_projection_mismatch),
           "unprojectable manifest/grant authority reached runtime creation");
}


class CountingRuntimeHooks final
    : public channel::PluginRuntimeHooks {
public:
  void state_changed(host::SessionState state, host::SessionError error) override {
    if (state == host::SessionState::running &&
        error == host::SessionError::none)
      running.fetch_add(1, std::memory_order_release);
  }
  void render_rejected(host::RouteResult) override {}
  bool accept(host::AdmittedSurfaceIntent) override {
    intents.fetch_add(1, std::memory_order_release);
    return false;
  }

  std::atomic<int> running = 0;
  std::atomic<int> intents = 0;
};

struct FinalFenceMutation final {
  permissions::ActivationBinding binding;
  std::atomic<int> calls = 0;
};

void invalidate_final_fence(host::AuthorityStore &store,
                            void *opaque) noexcept {
  auto &mutation = *static_cast<FinalFenceMutation *>(opaque);
  mutation.calls.fetch_add(1, std::memory_order_release);
  (void)store.promote_candidate(mutation.binding, UINT64_MAX);
}

void prepared_root_commits_on_ui_with_exact_hooks() {
  RuntimeRootFixture fixture;
  const auto active = fixture.activate(true);
  auto scope = std::make_shared<Scope>();
  auto supervisor = peer_supervisor_factory(scope);
  std::unique_ptr<channel::PreparedPluginSession> prepared;
  const auto ui_thread = std::this_thread::get_id();
  std::thread::id worker_thread;
  std::thread worker([&] {
    worker_thread = std::this_thread::get_id();
    prepared = fixture.prepare_result({}, std::move(supervisor)).runtime;
  });
  worker.join();
  OMARCHY_CHECK(prepared && worker_thread != ui_thread && scope->attachments == 0);
  CountingRuntimeHooks hooks;
  PeerSessionFixture unattested;
  channel::PluginSessionCreateError error{};
  auto session_only = channel::PluginSessionTestAccess::prepare_from_activation(
      peer_supervisor_factory(unattested.scope)(), unattested.activation.snapshot(),
      unattested.runtime_factory, error);
  OMARCHY_CHECK(session_only && error == channel::PluginSessionCreateError::none &&
              !channel::ReviewedSessionTestAccess::commit(
                  std::move(session_only), hooks, *QCoreApplication::instance()) &&
              unattested.scope->attachments == 0 && hooks.running == 0);
  auto root = channel::ReviewedSessionTestAccess::commit(
      std::move(prepared), hooks, *QCoreApplication::instance());
  OMARCHY_CHECK(root != nullptr &&
              channel::ReviewedSessionTestAccess::ui_affine(
                  *root, *QCoreApplication::instance()));
  await([&] { return hooks.running.load(std::memory_order_acquire) == 1; },
        "prepared runtime did not deliver its exact running Hook");
  auto &port = channel::ReviewedSessionTestAccess::surface_session(*root);
  const auto description = port.describe("barWidget");
  OMARCHY_CHECK(description.has_value());
  Endpoint endpoint;
  bool rejected = false;
  std::thread foreign([&] {
    rejected = channel::ReviewedSessionTestAccess::session_binding(*root) == active.binding &&
               !port.describe("barWidget") && !port.attach(*description, endpoint) &&
               !port.arm_surface_intent(*description, 1);
  });
  foreign.join();
  OMARCHY_CHECK(rejected && port.describe("barWidget") == description);
  const auto live = channel::PluginSessionTestAccess::live_generation(*root);
  OMARCHY_CHECK(live->current(active.binding));
  root.reset();
  // The reviewed session now owns its own teardown: admission is closed
  // synchronously, while process cleanup remains asynchronous and bounded.
  OMARCHY_CHECK(!live->current(active.binding) && !live->acquire_effect(active.binding));
  await([&] {
    return scope->terminations == 1 && scope->peer_pid > 0 &&
           ::kill(scope->peer_pid, 0) < 0 && errno == ESRCH;
  }, "reviewed session did not reap its worker after destruction");
  OMARCHY_CHECK(hooks.running == 1 && !live->current(active.binding));
}

void required_denied_activation_is_permission_only_after_reopen() {
  RuntimeRootFixture fixture;
  const auto active = fixture.activate();
  const auto revoked = fixture.revoke_required();
  OMARCHY_CHECK(revoked.status == host::AuthorityMutationResult::applied &&
              revoked.binding.has_value() && !revoked.activatable);

  const auto prepared = fixture.prepare_result();
  OMARCHY_CHECK(!prepared.runtime && prepared.permission_disabled);
}

void consent_only_preparation_requires_an_exact_reviewable_activation() {
  enum class Scenario { unpublished, candidate_only, corrupt_active,
                        foreign_policy, foreign_revision };
  for (const auto scenario : {Scenario::unpublished, Scenario::candidate_only,
                             Scenario::corrupt_active, Scenario::foreign_policy,
                             Scenario::foreign_revision}) {
    RuntimeRootFixture fixture;
    fixture.record(scenario == Scenario::foreign_revision ? std::string(64, 'c') : "");
    if (scenario == Scenario::candidate_only || scenario == Scenario::corrupt_active ||
        scenario == Scenario::foreign_policy) {
      auto active = fixture.publish(1, 0);
      if (scenario != Scenario::candidate_only) {
        fixture.promote(active, 1);
        if (scenario == Scenario::corrupt_active) {
          fixture.corrupt_active();
        } else {
          active.binding.policy_fingerprint = permissions::Digest(std::string(64, 'b'));
          fixture.replace_active(active);
        }
      }
    }
    const auto before = fixture.authority_sequence();
    const auto prepared = fixture.prepare_result();
    OMARCHY_CHECK(!prepared.runtime &&
                prepared.permission_disabled == (scenario == Scenario::unpublished) &&
                fixture.authority_sequence() == before);
  }
}

void prepared_root_final_fence_rejects_intervening_mutation() {
  RuntimeRootFixture fixture;
  const auto active = fixture.activate(true);
  auto scope = std::make_shared<Scope>();
  FinalFenceMutation mutation{.binding = active.binding};
  auto thread_reaped = std::make_shared<std::atomic<bool>>(false);
  channel::RuntimeServices services;
  services.context = std::shared_ptr<void>(new int, [thread_reaped](void *value) {
    delete static_cast<int *>(value);
    QObject::connect(QThread::currentThread(), &QObject::destroyed,
                     [thread_reaped] { thread_reaped->store(true); });
  });
  auto supervisor = peer_supervisor_factory(scope);
  auto prepared = fixture.prepare_result(
      std::move(services), std::move(supervisor), "current", ::getuid(), true,
      invalidate_final_fence, &mutation).runtime;
  CountingRuntimeHooks hooks;
  auto root = channel::ReviewedSessionTestAccess::commit(
      std::move(prepared), hooks, *QCoreApplication::instance());
  OMARCHY_CHECK(!root && mutation.calls.load(std::memory_order_acquire) == 1 &&
              scope->attachments == 0 &&
              hooks.running.load(std::memory_order_acquire) == 0);
  // Rejection destroys the session asynchronously, even though it never starts.
  await([&] { return thread_reaped->load(); },
        "final-fence rejection did not reap its session thread");
}

void prepared_root_commit_is_ui_only_and_path_independent() {
  {
    RuntimeRootFixture fixture;
    const auto active = fixture.activate(true);
    auto scope = std::make_shared<Scope>();
    auto supervisor = peer_supervisor_factory(scope);
    auto prepared = fixture.prepare_result({}, std::move(supervisor)).runtime;
    CountingRuntimeHooks hooks;
    std::unique_ptr<channel::PluginSession> rejected;
    std::thread wrong_thread([&] {
      rejected = channel::ReviewedSessionTestAccess::commit(
          std::move(prepared), hooks, *QCoreApplication::instance());
    });
    wrong_thread.join();
    OMARCHY_CHECK(!rejected && scope->attachments == 0 &&
                hooks.running.load(std::memory_order_acquire) == 0);
  }

  RuntimeRootFixture fixture;
  const auto active = fixture.activate(true);
  auto scope = std::make_shared<Scope>();
  auto supervisor = peer_supervisor_factory(scope);
  std::unique_ptr<channel::PreparedPluginSession> prepared;
  std::thread worker([&] {
    prepared = fixture.prepare_result({}, std::move(supervisor)).runtime;
  });
  worker.join();
  OMARCHY_CHECK(prepared != nullptr && scope->attachments == 0);
  fixture.close_borrowed_roots();
  fixture.corrupt_prepared_backing();
  CountingRuntimeHooks hooks;
  auto root = channel::ReviewedSessionTestAccess::commit(
      std::move(prepared), hooks, *QCoreApplication::instance());
  OMARCHY_CHECK(root != nullptr);
  await([&] { return hooks.running.load(std::memory_order_acquire) == 1; },
        "path-independent UI commit did not start its pinned runtime");
}

void composed_root_is_the_composed_authority_path() {
  using namespace std::chrono_literals;
  RuntimeRootFixture fixture("session-idle");
  const auto active = fixture.activate(true);
  auto prepared = fixture.prepare_result(
      {},
      [] {
        return launcher::test_support::make_supervisor(
            FAKE_BWRAP_PATH, CHANNEL_PEER_PATH, std::make_shared<Scope>());
      }).runtime;
  CountingRuntimeHooks hooks;
  auto root = channel::ReviewedSessionTestAccess::commit(
      std::move(prepared), hooks, *QCoreApplication::instance());
  OMARCHY_CHECK(root != nullptr);
  await([&] { return hooks.running.load(std::memory_order_acquire) == 1; },
        "composed root did not reach its exact running generation");

  auto locked = fixture.prepare_result(
      {},
      [] {
        return launcher::test_support::make_supervisor(
            FAKE_BWRAP_PATH, CHANNEL_PEER_PATH, std::make_shared<Scope>());
      }).runtime;
  OMARCHY_CHECK(!locked);
  fixture.close_borrowed_roots();

  auto &surface_session = channel::ReviewedSessionTestAccess::surface_session(*root);
  const auto g1_surface = surface_session.describe("barWidget");
  OMARCHY_CHECK(g1_surface && g1_surface->binding.generation == 1 &&
              g1_surface->key ==
                  surface::SurfaceKey{.id = 1, .generation = 1} &&
              g1_surface->plugin_id == "org.example.status" &&
              g1_surface->surface_name == "barWidget" &&
              !g1_surface->canonical_surfaces.empty() &&
              !surface_session.describe("BarWidget"));
  Endpoint stale_g1_endpoint;
  OMARCHY_CHECK(surface_session.attach(*g1_surface, stale_g1_endpoint) &&
              surface_session.detach(*g1_surface, stale_g1_endpoint) &&
              !surface_session.detach(*g1_surface, stale_g1_endpoint));
}

void composed_root_rejects_unusable_authority_and_providers() {
  RuntimeRootFixture bad_fd;
  {
    OMARCHY_CHECK(!bad_fd.prepare_result({}, {}, "current", ::getuid(), false).runtime);
  }
  {
    OMARCHY_CHECK(!bad_fd.prepare_result(
                {}, {}, "current", std::numeric_limits<std::uint32_t>::max()).runtime);
  }

  {
    RuntimeRootFixture wrong_record;
    wrong_record.record();
    OMARCHY_CHECK(!wrong_record.prepare_result({}, {}, "../current").runtime);
  }

  RuntimeRootFixture missing_provider("session-notification");
  const auto active = missing_provider.activate();
  auto scope = std::make_shared<Scope>();
  auto rejected = missing_provider.prepare_result(
      {}, peer_supervisor_factory(scope)).runtime;
  OMARCHY_CHECK(!rejected && scope->attachments == 0);
}




} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    if (argc == 2 &&
        std::string_view(argv[1]) == "--session-runtime-factory-only") {
      session_runtime_factory_tests();
      std::cout << "session runtime factory tests passed\n";
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--composed-root-only") {
      composed_root_is_the_composed_authority_path();
      composed_root_rejects_unusable_authority_and_providers();
      prepared_root_commits_on_ui_with_exact_hooks();
      prepared_root_final_fence_rejects_intervening_mutation();
      prepared_root_commit_is_ui_only_and_path_independent();
      std::cout << "composed root tests passed\n";
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--runtime-root-fence-only") {
      required_denied_activation_is_permission_only_after_reopen();
      prepared_root_final_fence_rejects_intervening_mutation();
      std::cout << "runtime root fence tests passed\n";
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--prepared-session-only") {
      prepared_session_is_thread_agnostic_before_commit();
      std::cout << "prepared session test passed\n";
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--provider-settlement-only") {
      delayed_provider_replies_settle_without_restarting_session();
      std::cout << "provider settlement test passed\n";
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--startup-fence-only") {
      preparation_rejects_invalid_grant_snapshots();
      startup_ack_failures_never_publish_product_session();
      authority_loss_between_snapshot_and_ack_never_publishes();
      std::cout << "startup fence tests passed\n";
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--required-denial-only") {
      required_denied_activation_is_permission_only_after_reopen();
      std::cout << "required-denial preparation test passed\n";
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--consent-only-classification-only") {
      consent_only_preparation_requires_an_exact_reviewable_activation();
      std::cout << "consent-only classification tests passed\n";
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--real-worker-invalid-qml-only") {
      if (!real_qml_worker_startup(false))
        return 77;
      std::cout << "real worker invalid-QML startup test passed\n";
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--real-worker-valid-qml-only") {
      if (!real_qml_worker_startup(true))
        return 77;
      std::cout << "real worker ordered-startup test passed\n";
      return 0;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--gesture-intent-only") {
      product_session_intercepts_gesture_intents_before_render_routing();
      std::cout << "product session gesture-intent tests passed\n";
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--session-io-only") {
      real_session_queue_fences();
      real_session_backpressure();
      std::cout << "real session I/O tests passed\n";
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--channel-boundaries-only") {
      real_channel_admission_and_revocation();
      real_channel_backpressure();
      real_channel_expiry_and_wake_fences();
      real_product_rejects_invalid_worker_render_packets();
      std::cout << "session channel boundary tests passed\n";
      return 0;
    }
    product_session_routes_two_surfaces_over_one_launch();
    effect_time_revocation_fences_an_authenticated_request();
    shared_gesture_authority_has_one_concurrent_winner();
    product_session_intercepts_gesture_intents_before_render_routing();
    preparation_rejects_invalid_grant_snapshots();
    prepared_commit_retains_activation_and_reuses_one_launch();
    startup_ack_failures_never_publish_product_session();
    authority_loss_between_snapshot_and_ack_never_publishes();
    prepared_session_is_thread_agnostic_before_commit();
    composed_root_is_the_composed_authority_path();
    composed_root_rejects_unusable_authority_and_providers();
    prepared_root_commits_on_ui_with_exact_hooks();
    prepared_root_final_fence_rejects_intervening_mutation();
    prepared_root_commit_is_ui_only_and_path_independent();
    required_denied_activation_is_permission_only_after_reopen();
    consent_only_preparation_requires_an_exact_reviewable_activation();
    session_runtime_factory_tests();
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "plugin product session tests passed\n";
  return 0;
}
