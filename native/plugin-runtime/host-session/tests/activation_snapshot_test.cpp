#include "../../tests/support/test_assert.hpp"
#include "../../tests/support/capability_fixture.hpp"
#include "../../tests/support/temporary_directory.hpp"

#include "activation_snapshot.hpp"
#include "permission_projection.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace host = omarchy::plugin_runtime::host_session;
namespace definitions = omarchy::plugins::definitions;
namespace manifest = omarchy::plugins::manifest;
namespace permissions = omarchy::plugins::permissions;
namespace policy = omarchy::plugin_runtime::policy;
namespace snapshot_wire = omarchy::plugin::wire::permission_snapshot;

namespace {

constexpr std::string_view kPlugin = "org.example.secure";
const std::string kRevision(64, 'a');
const std::string kRequest = manifest::requested_capability_fingerprint({});

using omarchy::plugin_runtime::test_support::require;

class TemporaryTree : public omarchy::plugin_runtime::test_support::TemporaryDirectory {
public:
  TemporaryTree() {
    activation_ = root_ / "activation";
    revisions_ = root_ / "revisions";
    state_ = root_ / "state";
    authority_ = root_ / "authority";
    std::filesystem::create_directory(activation_);
    std::filesystem::create_directory(revisions_);
    std::filesystem::create_directory(state_);
    std::filesystem::create_directory(authority_);
    std::filesystem::create_directory(revisions_ / "active");
    std::filesystem::create_directory(state_ / "plugin-state");
    OMARCHY_CHECK(::chmod((state_ / "plugin-state").c_str(), 0700) == 0);
    write(revisions_ / "active" / "identity",
          std::string(kPlugin) + "\n" + kRevision + "\n");
    write(activation_ / "current", record());
  }

  [[nodiscard]] static std::string
  record(std::string_view revision = "active",
         std::string_view state = "plugin-state") {
    return "format=omarchy-plugin-activation-v2\nplugin=" +
           std::string(kPlugin) +
           "\nrevision-directory=" + std::string(revision) +
           "\nrevision-sha256=" + kRevision +
           "\nstate-directory=" + std::string(state) + "\n";
  }

  static void write(const std::filesystem::path &path, std::string_view bytes) {
    write_file(path, bytes, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC);
  }

  [[nodiscard]] int open_directory(const std::filesystem::path &path) const {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
      throw std::runtime_error("directory open failed");
    return fd;
  }

  [[nodiscard]] const std::filesystem::path &activation() const {
    return activation_;
  }
  [[nodiscard]] const std::filesystem::path &revisions() const {
    return revisions_;
  }
  [[nodiscard]] const std::filesystem::path &state() const { return state_; }
  [[nodiscard]] const std::filesystem::path &authority() const {
    return authority_;
  }

private:
  std::filesystem::path activation_;
  std::filesystem::path revisions_;
  std::filesystem::path state_;
  std::filesystem::path authority_;
};

std::string read_relative(int directory_fd, std::string_view name) {
  const std::string owned(name);
  const int fd =
      ::openat(directory_fd, owned.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return {};
  std::string result;
  char bytes[256];
  for (;;) {
    const ssize_t count = ::read(fd, bytes, sizeof(bytes));
    if (count <= 0)
      break;
    result.append(bytes, static_cast<std::size_t>(count));
  }
  ::close(fd);
  return result;
}

class DescriptorVerifier final : public host::RevisionVerifier {
public:
  std::function<void()> before_read;
  std::string request_fingerprint = kRequest;
  mutable int calls = 0;

  std::optional<host::VerifiedRevision>
  verify_open_revision(int revision_directory_fd) const override {
    ++calls;
    if (before_read)
      before_read();
    const std::string identity =
        read_relative(revision_directory_fd, "identity");
    const auto split = identity.find('\n');
    const auto end = identity.find('\n', split + 1);
    if (split == std::string::npos || end == std::string::npos ||
        end + 1 != identity.size())
      return std::nullopt;
    manifest::ManifestV2 verified_manifest;
    verified_manifest.id = identity.substr(0, split);
    return host::VerifiedRevision{
        .manifest = std::move(verified_manifest),
        .tree_sha256 = identity.substr(split + 1, end - split - 1),
        .request_sha256 = request_fingerprint};
  }
};

policy::GrantSnapshot grants(std::uint64_t generation = 7) {
  policy::GrantSnapshot result;
  result.binding = {
      .plugin = permissions::PluginId(kPlugin),
      .revision = permissions::Digest(kRevision),
      .policy_fingerprint = permissions::Digest(
          manifest::requested_capability_fingerprint({})),
      .generation = generation};
  return result;
}

class Authority final : public host::GrantAuthority {
public:
  policy::GrantSnapshot snapshot = grants();
  host::GrantStatus status = host::GrantStatus::activatable;
  std::function<void()> before_return;
  mutable int calls = 0;

  host::GrantResolution
  resolve(std::string_view plugin_id,
          std::string_view revision_sha256) const override {
    ++calls;
    if (before_return)
      before_return();
    if (plugin_id != kPlugin || revision_sha256 != kRevision)
      return {};
    return {.snapshot = snapshot, .status = status};
  }
};

struct OpenRoots {
  host::UniqueFd activation;
  host::UniqueFd revisions;
  host::UniqueFd state;
  host::UniqueFd authority;

  explicit OpenRoots(const TemporaryTree &tree)
      : activation(tree.open_directory(tree.activation())),
        revisions(tree.open_directory(tree.revisions())),
        state(tree.open_directory(tree.state())),
        authority(tree.open_directory(tree.authority())) {}
};

host::FilesystemIdentity identity(int descriptor) {
  struct stat metadata {};
  OMARCHY_CHECK(::fstat(descriptor, &metadata) == 0);
  return {.device = static_cast<std::uint64_t>(metadata.st_dev),
          .inode = static_cast<std::uint64_t>(metadata.st_ino)};
}

host::ActivationResult load(const TemporaryTree &tree,
                            DescriptorVerifier &verifier,
                            Authority &authority,
                            std::optional<host::FilesystemIdentity>
                                authority_root = std::nullopt,
                            std::string expected_state = "plugin-state") {
  OpenRoots roots(tree);
  host::ActivationSource source(roots.activation.get(), roots.revisions.get(), roots.state.get(),
                                verifier, authority,
                                authority_root.value_or(identity(roots.authority.get())),
                                std::move(expected_state), ::getuid());
  // ActivationSource owns duplicates rather than borrowing caller descriptors.
  roots.activation.reset();
  roots.revisions.reset();
  roots.state.reset();
  return source.load("current");
}

template <typename Mutate>
void reject_tree(Mutate mutate, host::ActivationError expected,
                 std::source_location location = std::source_location::current()) {
  TemporaryTree tree;
  mutate(tree);
  DescriptorVerifier verifier;
  Authority authority;
  require(load(tree, verifier, authority).error == expected,
          "unexpected activation rejection", location);
}

void happy_path_and_revocation() {
  TemporaryTree tree;
  DescriptorVerifier verifier;
  Authority authority;
  auto result = load(tree, verifier, authority);
  OMARCHY_CHECK(result.snapshot.has_value() &&
              result.error == host::ActivationError::none);
  OMARCHY_CHECK(verifier.calls == 1 && authority.calls == 1);
  OMARCHY_CHECK(result.snapshot->manifest.id == kPlugin);
  auto binding = result.snapshot->grants.binding;
  OMARCHY_CHECK(binding.generation == 7);
  OMARCHY_CHECK(result.snapshot->live->current(binding));
  auto stale = binding;
  ++stale.generation;
  OMARCHY_CHECK(!result.snapshot->live->current(stale));
  (void)result.snapshot->live->revoke_and_drain();
  OMARCHY_CHECK(result.snapshot->live->generation() == 0 &&
              !result.snapshot->live->current(binding));
  OMARCHY_CHECK(::fcntl(result.snapshot->activation_record.get(), F_GETFD) >= 0 &&
              ::fcntl(result.snapshot->revision_directory.get(), F_GETFD) >=
                  0 &&
              ::fcntl(result.snapshot->state_directory.get(), F_GETFD) >= 0);
  OMARCHY_CHECK((::fcntl(result.snapshot->activation_record.get(), F_GETFD) &
           FD_CLOEXEC) != 0 &&
              (::fcntl(result.snapshot->revision_directory.get(), F_GETFD) &
               FD_CLOEXEC) != 0 &&
              (::fcntl(result.snapshot->state_directory.get(), F_GETFD) &
               FD_CLOEXEC) != 0);
}

void required_denial_retains_verified_activation_for_administration() {
  TemporaryTree tree;
  DescriptorVerifier verifier;
  Authority authority;
  authority.status = host::GrantStatus::permission_disabled;

  const auto result = load(tree, verifier, authority);
  OMARCHY_CHECK(result.snapshot.has_value() &&
              result.error == host::ActivationError::none &&
              result.grant_status == host::GrantStatus::permission_disabled);
  OMARCHY_CHECK(verifier.calls == 1 && authority.calls == 1 &&
              result.snapshot->grants.binding == authority.snapshot.binding);
}

void path_swaps_do_not_retarget_descriptors() {
  TemporaryTree tree;
  std::filesystem::create_directory(tree.revisions() / "replacement");
  TemporaryTree::write(tree.revisions() / "replacement" / "identity",
                       "org.attacker\n" + std::string(64, 'f') + "\n");
  std::filesystem::create_directory(tree.state() / "replacement");
  TemporaryTree::write(tree.state() / "plugin-state" / "marker", "original");
  TemporaryTree::write(tree.state() / "replacement" / "marker", "replacement");

  DescriptorVerifier verifier;
  verifier.before_read = [&] {
    std::filesystem::rename(tree.revisions() / "active",
                            tree.revisions() / "original-moved");
    std::filesystem::rename(tree.revisions() / "replacement",
                            tree.revisions() / "active");
  };
  Authority authority;
  authority.before_return = [&] {
    std::filesystem::rename(tree.state() / "plugin-state",
                            tree.state() / "original-moved");
    std::filesystem::rename(tree.state() / "replacement",
                            tree.state() / "plugin-state");
  };
  auto result = load(tree, verifier, authority);
  OMARCHY_CHECK(result.snapshot.has_value());
  OMARCHY_CHECK(
      read_relative(result.snapshot->revision_directory.get(), "identity") ==
          std::string(kPlugin) + "\n" + kRevision + "\n");
  OMARCHY_CHECK(read_relative(result.snapshot->state_directory.get(), "marker") ==
              "original");

  const auto before = identity(result.snapshot->activation_record.get());
  std::filesystem::rename(tree.activation() / "current",
                          tree.activation() / "original-record");
  TemporaryTree::write(tree.activation() / "current", TemporaryTree::record());
  const auto after = identity(result.snapshot->activation_record.get());
  OMARCHY_CHECK(before.device == after.device && before.inode == after.inode);
}

void symlinks_and_aliases_are_rejected() {
  reject_tree([](TemporaryTree &tree) {
    std::filesystem::rename(tree.activation() / "current",
                            tree.activation() / "real");
    std::filesystem::create_symlink("real", tree.activation() / "current");
  }, host::ActivationError::record_unavailable);
  reject_tree([](TemporaryTree &tree) {
    std::filesystem::rename(tree.revisions() / "active",
                            tree.revisions() / "real");
    std::filesystem::create_directory_symlink("real",
                                              tree.revisions() / "active");
  }, host::ActivationError::revision_unavailable);
  reject_tree([](TemporaryTree &tree) {
    std::filesystem::rename(tree.state() / "plugin-state",
                            tree.state() / "real");
    std::filesystem::create_directory_symlink("real",
                                              tree.state() / "plugin-state");
  }, host::ActivationError::state_unavailable);
  {
    TemporaryTree tree;
    OpenRoots roots(tree);
    DescriptorVerifier verifier;
    Authority authority;
    host::ActivationSource source(roots.activation.get(), roots.revisions.get(),
                                  roots.revisions.get(), verifier, authority,
                                  identity(roots.authority.get()), "plugin-state",
                                  ::getuid());
    OMARCHY_CHECK(source.load("current").error == host::ActivationError::root_alias);
  }
}

void grant_authority_aliases_are_rejected() {
  {
    TemporaryTree tree;
    OpenRoots roots(tree);
    DescriptorVerifier verifier;
    Authority authority;
    for (const int root : {roots.activation.get(), roots.revisions.get(), roots.state.get()})
      OMARCHY_CHECK(load(tree, verifier, authority, identity(root)).error ==
                  host::ActivationError::root_alias);
  }
  for (const auto &relative : {"revisions/active", "state/plugin-state"}) {
    TemporaryTree tree;
    const int selected = tree.open_directory(tree.path() / relative);
    DescriptorVerifier verifier;
    Authority authority;
    const auto result = load(tree, verifier, authority, identity(selected));
    ::close(selected);
    OMARCHY_CHECK(result.error == host::ActivationError::revision_state_alias);
  }
}

void every_authority_inode_must_be_distinct() {
  std::array<host::FilesystemIdentity, 5> identities{};
  for (std::size_t index = 0; index < identities.size(); ++index) {
    identities[index] = {.device = 1, .inode = index + 1};
  }
  OMARCHY_CHECK(host::distinct_authority_objects(identities));
  for (std::size_t left = 0; left < identities.size(); ++left) {
    for (std::size_t right = left + 1; right < identities.size(); ++right) {
      const auto original = identities[right];
      identities[right] = identities[left];
      OMARCHY_CHECK(!host::distinct_authority_objects(identities));
      identities[right] = original;
    }
  }
}

void inspected_activation_records_are_exact_and_pinned() {
  {
    TemporaryTree tree;
    const int root = tree.open_directory(tree.activation());
    auto inspected = host::inspect_activation_record(
        root, "current", static_cast<std::uint32_t>(::getuid()));
    ::close(root);
    OMARCHY_CHECK(inspected && inspected->record().plugin_id == kPlugin &&
                inspected->unchanged() &&
                (::fcntl(inspected->descriptor(), F_GETFD) & FD_CLOEXEC) != 0);
    OMARCHY_CHECK(::chmod((tree.activation() / "current").c_str(), 0400) == 0 &&
                !inspected->unchanged());
  }
  {
    TemporaryTree tree;
    OMARCHY_CHECK(::chmod((tree.activation() / "current").c_str(), 0640) == 0);
    const int root = tree.open_directory(tree.activation());
    OMARCHY_CHECK(!host::inspect_activation_record(
                root, "current", static_cast<std::uint32_t>(::getuid())));
    DescriptorVerifier verifier;
    Authority authority;
    const int revisions = tree.open_directory(tree.revisions());
    const int state = tree.open_directory(tree.state());
    host::ActivationSource source(root, revisions, state, verifier, authority,
                                  {.device = 99, .inode = 99}, "plugin-state",
                                  ::getuid());
    ::close(revisions);
    ::close(state);
    OMARCHY_CHECK(source.load("current").error ==
                host::ActivationError::record_untrusted);
    ::close(root);
  }
  {
    TemporaryTree tree;
    std::filesystem::create_hard_link(tree.activation() / "current",
                                      tree.activation() / "alias");
    const int root = tree.open_directory(tree.activation());
    OMARCHY_CHECK(!host::inspect_activation_record(
                root, "current", static_cast<std::uint32_t>(::getuid())));
    ::close(root);
  }
}

void identity_policy_and_mode_mismatches_are_rejected() {
  {
    TemporaryTree tree;
    TemporaryTree::write(
        tree.activation() / "current",
        "format=omarchy-plugin-activation-v1\nplugin=" + std::string(kPlugin) +
            "\nrevision-directory=active\nrevision-sha256=" + kRevision +
            "\nstate-directory=plugin-state\ngeneration=7\n");
    DescriptorVerifier verifier;
    Authority authority;
    OMARCHY_CHECK(load(tree, verifier, authority).error ==
                host::ActivationError::record_invalid &&
                verifier.calls == 0 && authority.calls == 0);
  }
  {
    TemporaryTree tree;
    std::filesystem::create_directory(tree.state() / "other-state");
    OMARCHY_CHECK(::chmod((tree.state() / "other-state").c_str(), 0700) == 0);
    TemporaryTree::write(tree.activation() / "current",
                         TemporaryTree::record("active", "other-state"));
    DescriptorVerifier verifier;
    Authority authority;
    OMARCHY_CHECK(load(tree, verifier, authority).error ==
                host::ActivationError::record_invalid &&
                verifier.calls == 0 && authority.calls == 0);
    DescriptorVerifier expected_verifier;
    Authority expected_authority;
    OMARCHY_CHECK(load(tree, expected_verifier, expected_authority, std::nullopt,
                 "other-state")
                .snapshot.has_value());
  }
  reject_tree([](TemporaryTree &tree) {
    TemporaryTree::write(tree.revisions() / "active" / "identity",
                         "org.wrong\n" + kRevision + "\n");
  }, host::ActivationError::revision_unverified);
  {
    TemporaryTree tree;
    DescriptorVerifier verifier;
    Authority authority;
    authority.snapshot.binding.policy_fingerprint =
        permissions::Digest(std::string(64, 'f'));
    OMARCHY_CHECK(load(tree, verifier, authority).error ==
                host::ActivationError::grant_mismatch);
  }
  {
    TemporaryTree tree;
    DescriptorVerifier verifier;
    Authority authority;
    verifier.request_fingerprint = std::string(64, 'c');
    OMARCHY_CHECK(load(tree, verifier, authority).error ==
                host::ActivationError::grant_mismatch);
  }
  {
    TemporaryTree tree;
    DescriptorVerifier verifier;
    Authority authority;
    authority.snapshot.binding.generation = 8;
    const auto result = load(tree, verifier, authority);
    OMARCHY_CHECK(result.snapshot && result.snapshot->grants.binding.generation == 8);
  }
  reject_tree([](TemporaryTree &tree) {
    ::chmod((tree.activation() / "current").c_str(), 0660);
  }, host::ActivationError::record_untrusted);
  reject_tree([](TemporaryTree &tree) {
    ::chmod(tree.state().c_str(), 0770);
  }, host::ActivationError::root_untrusted);
  for (const mode_t mode : {mode_t{0755}, mode_t{0750}, mode_t{04700},
                            mode_t{02700}, mode_t{01700}}) {
    reject_tree([&](TemporaryTree &tree) {
      OMARCHY_CHECK(::chmod((tree.state() / "plugin-state").c_str(), mode) == 0);
    }, host::ActivationError::state_unavailable);
  }
  {
    TemporaryTree tree;
    struct stat metadata {};
    OMARCHY_CHECK(::stat((tree.state() / "plugin-state").c_str(), &metadata) == 0 &&
                metadata.st_uid == ::getuid() &&
                (metadata.st_mode & 0777) == 0700);
    DescriptorVerifier verifier;
    Authority authority;
    OMARCHY_CHECK(load(tree, verifier, authority).snapshot.has_value());
  }
}

void permission_projection_is_manifest_indexed_and_exact() {
  auto manifest_value = manifest::parse_manifest_v2(
      R"({"schemaVersion":2,"id":"org.example.secure","name":"Secure","version":"1","runtime":{"apiVersion":1,"qml":"Main.qml"},"surfaces":{},"permissions":{"required":[{"capability":"storage.private","definitionGeneration":1,"definitionDigest":"8ced97a7d1cf93616946f20bf27fa47f0e79e2ee81903ab0b7b9d31aa23d3e66","operations":["read","remove","write"],"reason":"state","itemBytes":4096,"quotaBytes":4096}],"optional":[{"capability":"notifications.send","definitionGeneration":1,"definitionDigest":"522f9db4a5e0d8978a0c1293c3ead01839ad882bb640d8fdb16c3011635e0872","operations":["send"],"reason":"alerts","categories":["status"]},{"capability":"local.status","reason":"status","definitionGeneration":7,"definitionDigest":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","operations":["status.read","status.write"],"dataset":"summary"}]}})");
  policy::GrantSnapshot snapshot;
  snapshot.binding = {
      .plugin = permissions::PluginId(manifest_value.id),
      .revision = permissions::Digest(kRevision),
      .policy_fingerprint = permissions::Digest(
          manifest::requested_capability_fingerprint(manifest_value.requests)),
      .generation = 41};
  for (const auto &request : manifest::canonical_capability_requests(manifest_value.requests)) {
    definitions::DynamicRequest resolved{
        .definition = {.canonical_name = definitions::Name(request.capability),
                       .definition_generation = request.definition_generation,
                       .definition_digest = definitions::Digest(request.definition_digest)},
        .operations = {}, .scope = definitions::CanonicalScope(request.canonical_scope),
        .required = request.required};
    for (const auto &operation : request.operations)
      resolved.operations.insert(definitions::Name(operation));
    snapshot.dynamic_grants.push_back({
        .binding = snapshot.binding, .request = resolved,
        .grant = {.operations = resolved.operations,
                  .state = request.required ? permissions::GrantState::granted :
                      (request.capability == "local.status" ? permissions::GrantState::revoked :
                                                             permissions::GrantState::denied),
                  .epoch = 41}});
  }

  const auto projected =
      host::project_permission_snapshot(manifest_value, snapshot);
  OMARCHY_CHECK(projected &&
              projected->manifest_request_fingerprint ==
                  manifest::requested_capability_fingerprint(
                      manifest_value.requests) &&
              projected->permissions ==
                  std::vector<snapshot_wire::PermissionRow>{
                      {snapshot_wire::GrantState::revoked, 0x0003},
                      {snapshot_wire::GrantState::denied, 0x0000},
                      {snapshot_wire::GrantState::granted, 0x0007}});

  auto reordered = manifest_value;
  std::ranges::reverse(reordered.requests);
  OMARCHY_CHECK(host::project_permission_snapshot(reordered, snapshot) == projected);

  const auto rejected = [&](auto mutate, std::string_view message) {
    auto candidate = snapshot;
    mutate(candidate);
    require(!host::project_permission_snapshot(manifest_value, candidate),
            message);
  };
  {
    auto changed_manifest = manifest_value;
    changed_manifest.requests.front().canonical_scope = R"({"categories":["changed"]})";
    OMARCHY_CHECK(!host::project_permission_snapshot(changed_manifest, snapshot));
  }
  rejected([](auto &candidate) {
    candidate.binding.policy_fingerprint =
        permissions::Digest(std::string(64, 'f'));
  }, "policy fingerprint mismatch was projected");
  rejected([](auto &candidate) {
    candidate.dynamic_grants[2].grant.state = permissions::GrantState::denied;
  }, "denied required grant was projected");
  rejected([](auto &candidate) {
    candidate.dynamic_grants[0].binding.generation++;
  }, "cross-generation dynamic grant was projected");
  rejected([](auto &candidate) {
    candidate.dynamic_grants[0].request.required = true;
  }, "mismatched dynamic request was projected");
  rejected([](auto &candidate) {
    candidate.dynamic_grants[0].grant.epoch = 0;
  }, "zero-epoch dynamic grant was projected");
  rejected([](auto &candidate) {
    candidate.dynamic_grants[0].grant.state =
        permissions::GrantState::granted;
    candidate.dynamic_grants[0].grant.operations = {};
  }, "empty granted dynamic row was projected");
  rejected([](auto &candidate) {
    candidate.dynamic_grants[0].grant.operations = {};
  }, "empty revoked dynamic row was projected");
  {
    auto candidate = snapshot;
    candidate.dynamic_grants[0].grant.state =
        permissions::GrantState::denied;
    const auto denied =
        host::project_permission_snapshot(manifest_value, candidate);
    OMARCHY_CHECK(denied &&
                denied->permissions.front() ==
                    snapshot_wire::PermissionRow{
                        snapshot_wire::GrantState::denied, 0x0000});
  }
  {
    auto candidate = snapshot;
    candidate.dynamic_grants[0].grant.state =
        permissions::GrantState::granted;
    candidate.dynamic_grants[0].grant.operations = {};
    candidate.dynamic_grants[0].grant.operations.insert(
        definitions::Name("status.read"));
    const auto partial =
        host::project_permission_snapshot(manifest_value, candidate);
    OMARCHY_CHECK(partial &&
                partial->permissions.front() ==
                    snapshot_wire::PermissionRow{
                        snapshot_wire::GrantState::granted, 0x0001});
    auto required_manifest = manifest_value;
    auto &required_request = *std::ranges::find_if(
        required_manifest.requests, [](const auto &request) {
          return request.capability == "local.status";
        });
    required_request.required = true;
    candidate.binding.policy_fingerprint = permissions::Digest(
        manifest::requested_capability_fingerprint(required_manifest.requests));
    for (auto &grant : candidate.dynamic_grants) grant.binding = candidate.binding;
    candidate.dynamic_grants[0].request.required = true;
    const auto required_partial =
        host::project_permission_snapshot(required_manifest, candidate);
    OMARCHY_CHECK(required_partial &&
                required_partial->permissions.front() ==
                    snapshot_wire::PermissionRow{
                        snapshot_wire::GrantState::granted, 0x0001});
  }
  rejected([](auto &candidate) {
    candidate.dynamic_grants.push_back(candidate.dynamic_grants.front());
  }, "duplicate dynamic grant was projected");

  const auto sixteen_manifest =
      omarchy::plugin_runtime::test_support::sixteen_operation_manifest(9, 'e');
  const auto &sixteen_request = sixteen_manifest.requests.front();
  policy::GrantSnapshot sixteen_snapshot;
  sixteen_snapshot.binding = {
      .plugin = permissions::PluginId(sixteen_manifest.id),
      .revision = permissions::Digest(std::string(64, 'f')),
      .policy_fingerprint = permissions::Digest(
          manifest::requested_capability_fingerprint(sixteen_manifest.requests)),
      .generation = 52};
  definitions::DynamicRequest sixteen_dynamic{
      .definition =
          {.canonical_name = definitions::Name(sixteen_request.capability),
           .definition_generation = sixteen_request.definition_generation,
           .definition_digest =
               definitions::Digest(sixteen_request.definition_digest)},
      .operations = {},
      .scope = definitions::CanonicalScope("{}"),
      .required = false};
  for (const auto &operation : sixteen_request.operations)
    sixteen_dynamic.operations.insert(definitions::Name(operation));
  definitions::DynamicGrant sixteen_grant{
      .operations = {},
      .state = permissions::GrantState::granted,
      .epoch = 52};
  sixteen_grant.operations.insert(definitions::Name("op-15"));
  sixteen_snapshot.dynamic_grants.push_back(
      {.binding = sixteen_snapshot.binding,
       .request = sixteen_dynamic,
       .grant = sixteen_grant});
  const auto high_bit =
      host::project_permission_snapshot(sixteen_manifest, sixteen_snapshot);
  OMARCHY_CHECK(high_bit && high_bit->permissions.size() == 1 &&
              high_bit->permissions.front() ==
                  snapshot_wire::PermissionRow{
                      snapshot_wire::GrantState::granted, 0x8000});
  sixteen_snapshot.dynamic_grants[0].grant.operations =
      sixteen_dynamic.operations;
  const auto full_mask =
      host::project_permission_snapshot(sixteen_manifest, sixteen_snapshot);
  OMARCHY_CHECK(full_mask && full_mask->permissions.front().operation_mask == 0xffff);
}

} // namespace

int main() {
  return omarchy::plugin_runtime::test_support::test_main([&] {
    happy_path_and_revocation();
    required_denial_retains_verified_activation_for_administration();
    path_swaps_do_not_retarget_descriptors();
    symlinks_and_aliases_are_rejected();
    grant_authority_aliases_are_rejected();
    every_authority_inode_must_be_distinct();
    inspected_activation_records_are_exact_and_pinned();
    identity_policy_and_mode_mismatches_are_rejected();
    permission_projection_is_manifest_indexed_and_exact();
    std::cout << "activation snapshot tests passed\n";
    return 0;
  }, "activation snapshot test failed: ");
}
