#include "authority_store.hpp"
#include "authority_migration.hpp"
#include "authority_sql.hpp"
#include "authority_snapshot_codec.hpp"
#ifdef OMARCHY_AUTHORITY_STORE_TESTING
#include <sqlite3.h>
#endif

#include <algorithm>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

namespace omarchy::plugin_runtime::host_session {
namespace {
constexpr std::string_view kLockName = ".authority.lock";

bool trusted_directory(const struct stat &metadata, std::uint32_t uid) {
  return S_ISDIR(metadata.st_mode) && metadata.st_uid == uid &&
         (metadata.st_mode & 0777) == 0700;
}

bool trusted_file(const struct stat &metadata, std::uint32_t uid) {
  return S_ISREG(metadata.st_mode) && metadata.st_uid == uid &&
         metadata.st_nlink == 1 && (metadata.st_mode & 0777) == 0600;
}

bool secure_created_file(int descriptor, std::uint32_t uid) {
  struct stat metadata{};
  return ::fchmod(descriptor, 0600) == 0 &&
         ::fstat(descriptor, &metadata) == 0 && trusted_file(metadata, uid);
}

std::optional<AuthorityRevisionRef> snapshot_reference(const policy::GrantSnapshot &snapshot) {
  std::vector<std::byte> bytes;
  if (!authority_snapshot_codec::encode_snapshot(snapshot, bytes))
    return {};
  return authority_snapshot_codec::reference_for(snapshot, plugins::manifest::sha256_hex(bytes));
}

bool activatable(const policy::GrantSnapshot &snapshot) {
  return std::ranges::all_of(snapshot.dynamic_grants, [](const auto &dynamic) {
    return !dynamic.request.required ||
           dynamic.grant.state == permissions::GrantState::granted;
  });
}
} // namespace

AuthorityStore::AuthorityStore(UniqueFd root, UniqueFd lock,
                               std::unique_ptr<SqliteAuthorityDatabase> database,
                               std::uint32_t expected_uid,
                               permissions::PluginId expected_plugin)
    : root_(std::move(root)), lock_(std::move(lock)), database_(std::move(database)),
      expected_uid_(expected_uid), expected_plugin_(std::move(expected_plugin)),
      owner_pid_(::getpid()) {}

AuthorityStore::~AuthorityStore() {
  std::unique_lock lock(mutation_mutex_);
  auto live = bound_live_.lock();
  bound_live_.reset();
  if (!live)
    return;
  const auto closed = live->close_effect_admission();
  lock.unlock();
  if (closed == LiveGenerationState::EffectAdmissionCloseResult::ready_to_drain)
    live->drain_closed_effects();
}

std::unique_ptr<AuthorityStore>
AuthorityStore::open(int root_directory_fd, std::uint32_t expected_uid,
                     permissions::PluginId expected_plugin) {
  UniqueFd root(::fcntl(root_directory_fd, F_DUPFD_CLOEXEC, 0));
  struct stat metadata{};
  if (!root || ::fstat(root.get(), &metadata) < 0 ||
      !trusted_directory(metadata, expected_uid))
    return nullptr;
  int lock_fd = ::openat(
      root.get(), std::string(kLockName).c_str(),
      O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  if (lock_fd >= 0) {
    if (!secure_created_file(lock_fd, expected_uid)) {
      ::close(lock_fd);
      return nullptr;
    }
  } else if (errno == EEXIST) {
    lock_fd = ::openat(root.get(), std::string(kLockName).c_str(),
                       O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  }
  UniqueFd lock(lock_fd);
  if (!lock || ::fstat(lock.get(), &metadata) < 0 ||
      !trusted_file(metadata, expected_uid) ||
      ::flock(lock.get(), LOCK_EX | LOCK_NB) < 0)
    return nullptr;
  auto database = open_authority_database(root.get(), lock.get(), expected_uid, expected_plugin);
  if (!database)
    return nullptr;
  return std::unique_ptr<AuthorityStore>(
      new AuthorityStore(std::move(root), std::move(lock), std::move(database), expected_uid,
                         std::move(expected_plugin)));
}

std::optional<AuthoritySlots> AuthorityStore::read_slots() const {
  std::scoped_lock lock(mutation_mutex_);
  if (::getpid() != owner_pid_ || poisoned_ || transitioning_)
    return std::nullopt;
  auto slots = authority_sql::read_slots(*database_, expected_plugin_);
  if (!slots)
    return std::nullopt;
  return slots;
}

std::optional<AuthorityView> AuthorityStore::read_authority_view() const {
  std::scoped_lock lock(mutation_mutex_);
  if (::getpid() != owner_pid_ || poisoned_ || transitioning_)
    return std::nullopt;
  auto slots = authority_sql::read_slots(*database_, expected_plugin_);
  if (!slots)
    return std::nullopt;
  std::optional<policy::GrantSnapshot> active;
  if (slots->active) {
    active = authority_sql::load_snapshot(*database_, *slots->active);
    if (!active || active->binding.plugin != expected_plugin_)
      return std::nullopt;
  }
  return AuthorityView{.authority_slots = *slots, .active = std::move(active)};
}

std::optional<FilesystemIdentity> AuthorityStore::root_identity() const {
  std::scoped_lock lock(mutation_mutex_);
  struct stat metadata{};
  if (::getpid() != owner_pid_ || ::fstat(root_.get(), &metadata) < 0)
    return std::nullopt;
  return FilesystemIdentity{
      .device = static_cast<std::uint64_t>(metadata.st_dev),
      .inode = static_cast<std::uint64_t>(metadata.st_ino)};
}

std::optional<PreparedLiveBinding> AuthorityStore::prepare_live_activation(
    const permissions::ActivationBinding &binding,
    const std::shared_ptr<LiveGenerationState> &live) {
  std::unique_lock lock(mutation_mutex_);
  if (::getpid() != owner_pid_ || poisoned_ || transitioning_ || !live ||
      !live->current(binding))
    return {};
  auto slots = authority_sql::read_slots(*database_, expected_plugin_);
  if (!slots || !slots->active ||
      slots->active->generation != binding.generation)
    return {};
  auto active = authority_sql::load_snapshot(*database_, *slots->active);
  if (!active || active->binding != binding || !activatable(*active))
    return {};
  struct stat metadata{};
  if (::fstat(root_.get(), &metadata) < 0)
    return {};
  const FilesystemIdentity identity{
      .device = static_cast<std::uint64_t>(metadata.st_dev),
      .inode = static_cast<std::uint64_t>(metadata.st_ino)};
  prepared_root_identity_ = identity;
  if (auto previous = bound_live_.lock(); previous != live) {
    const auto fenced = fence_bound_live(lock, *slots);
    if (fenced != AuthorityMutationResult::applied)
      return {};
    if (!live->current(binding)) {
      transitioning_ = false;
      return {};
    }
    bound_live_ = live;
    transitioning_ = false;
  }
  if (mutation_epoch_ == UINT64_MAX)
    return {};
  ++mutation_epoch_;
  return PreparedLiveBinding(this, identity, binding, live, mutation_epoch_);
}

bool AuthorityStore::commit_live_activation(
    PreparedLiveBinding prepared,
    const permissions::ActivationBinding &expected_binding,
    const std::shared_ptr<LiveGenerationState> &expected_live) {
  std::scoped_lock lock(mutation_mutex_);
  const auto bound = bound_live_.lock();
  if (prepared.owner_ != this || ::getpid() != owner_pid_ || poisoned_ ||
      transitioning_ || prepared.mutation_epoch_ != mutation_epoch_ ||
      prepared_root_identity_ != prepared.root_identity_ ||
      prepared.binding_ != expected_binding ||
      prepared.live_ != expected_live || bound != prepared.live_ ||
      !prepared.live_ || !prepared.live_->current(prepared.binding_) ||
      mutation_epoch_ == UINT64_MAX)
    return false;
  ++mutation_epoch_;
  return true;
}

AuthorityMutationResult
AuthorityStore::fail_closed(std::unique_lock<std::mutex> &lock,
                            AuthorityFenceObserver *observer) {
  poisoned_ = true;
  transitioning_ = true;
  auto live = bound_live_.lock();
  bound_live_.reset();
  if (live) {
    const auto closed = live->close_effect_admission();
    if (observer)
      observer->live_generation_closed();
    if (closed == LiveGenerationState::EffectAdmissionCloseResult::reentrant) {
      transitioning_ = false;
      return AuthorityMutationResult::reentrant_effect;
    }
    lock.unlock();
    live->drain_closed_effects();
    lock.lock();
  }
  transitioning_ = false;
  return AuthorityMutationResult::io_error;
}

AuthorityMutationResult
AuthorityStore::fence_bound_live(std::unique_lock<std::mutex> &lock,
                                 const AuthoritySlots &preimage,
                                 AuthorityFenceObserver *observer) {
  if (!lock.owns_lock() || transitioning_ || poisoned_)
    return poisoned_ ? AuthorityMutationResult::poisoned
                     : AuthorityMutationResult::invalid;
  transitioning_ = true;
  auto live = bound_live_.lock();
  bound_live_.reset();
  if (live) {
    const auto closed = live->close_effect_admission();
    if (observer)
      observer->live_generation_closed();
    if (closed == LiveGenerationState::EffectAdmissionCloseResult::reentrant) {
      transitioning_ = false;
      poisoned_ = true;
      return AuthorityMutationResult::reentrant_effect;
    }
    lock.unlock();
    live->drain_closed_effects();
    lock.lock();
  }
  try {
    const auto exact = authority_sql::read_slots(*database_, expected_plugin_);
    if (::getpid() != owner_pid_ || !transitioning_ || !exact ||
        *exact != preimage) {
      transitioning_ = false;
      poisoned_ = true;
      return AuthorityMutationResult::io_error;
    }
  } catch (...) {
    transitioning_ = false;
    poisoned_ = true;
    return AuthorityMutationResult::io_error;
  }
  return AuthorityMutationResult::applied;
}

#ifdef OMARCHY_AUTHORITY_STORE_TESTING
void AuthorityStore::set_crash_point_for_testing(
    AuthorityCrashPoint point) noexcept {
  authority_sql::set_crash_point_for_testing(point);
}

AuthorityMutationResult AuthorityStore::replace_active_for_testing(
    const policy::GrantSnapshot &snapshot) {
  std::scoped_lock lock(mutation_mutex_);
  // Deliberately corrupt the active record for boundary-negative tests. This
  // does not constitute a second writer or bypass the production consent API.
  sqlite3_stmt *statement = nullptr;
  const auto db = database_->connection();
  if (sqlite3_prepare_v2(db, "UPDATE revisions SET policy=?1 WHERE digest=(SELECT active FROM authority)", -1, &statement, nullptr) != SQLITE_OK)
    return AuthorityMutationResult::io_error;
  const auto policy = snapshot.binding.policy_fingerprint.view();
  const bool applied = sqlite3_bind_text(statement, 1, policy.data(), policy.size(), SQLITE_TRANSIENT) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_DONE;
  sqlite3_finalize(statement);
  return applied ? AuthorityMutationResult::applied : AuthorityMutationResult::io_error;
}
#endif

AuthorityMutationResult AuthorityStore::publish_candidate(
    const VerifiedRevision &verified, const policy::GrantSnapshot &snapshot,
    std::uint64_t expected_sequence,
    const definitions::TrustedDefinitionRegistry &definitions) {
  std::unique_lock lock(mutation_mutex_);
  if (::getpid() != owner_pid_)
    return AuthorityMutationResult::io_error;
  if (mutation_epoch_ == UINT64_MAX)
    return AuthorityMutationResult::poisoned;
  ++mutation_epoch_;
  if (poisoned_ || transitioning_)
    return AuthorityMutationResult::poisoned;
  if (!authority_snapshot_codec::complete_snapshot(
          verified, snapshot, definitions))
    return AuthorityMutationResult::invalid;
  if (snapshot.binding.plugin != expected_plugin_)
    return AuthorityMutationResult::invalid;
  auto slots = authority_sql::read_slots(*database_, expected_plugin_);
  if (!slots)
    return fail_closed(lock);
  if (slots->sequence != expected_sequence)
    return AuthorityMutationResult::stale_sequence;
  if (slots->sequence == UINT64_MAX)
    return AuthorityMutationResult::invalid;
  if (slots->generation_high_watermark == UINT64_MAX ||
      snapshot.binding.generation != slots->generation_high_watermark + 1)
    return AuthorityMutationResult::invalid;
  const auto previous = *slots;

  const auto reference = snapshot_reference(snapshot);
  if (!reference)
    return AuthorityMutationResult::invalid;
  slots->candidate = *reference;
  slots->generation_high_watermark = snapshot.binding.generation;
  ++slots->sequence;
  const auto result = authority_sql::replace(*database_, expected_plugin_, previous, *slots, std::span(&snapshot, 1));
  if (result == AuthorityMutationResult::io_error)
    return fail_closed(lock);
  return result;
}

AuthorityMutationResult AuthorityStore::promote_candidate(
    const permissions::ActivationBinding &candidate,
    std::uint64_t expected_sequence) {
  return promote_candidate(candidate, expected_sequence, nullptr);
}

AuthorityMutationResult AuthorityStore::promote_candidate(
    const permissions::ActivationBinding &candidate,
    std::uint64_t expected_sequence, AuthorityFenceObserver *observer) {
  std::unique_lock lock(mutation_mutex_);
  if (::getpid() != owner_pid_)
    return AuthorityMutationResult::io_error;
  if (mutation_epoch_ == UINT64_MAX)
    return AuthorityMutationResult::poisoned;
  ++mutation_epoch_;
  if (poisoned_ || transitioning_)
    return AuthorityMutationResult::poisoned;
  auto slots = authority_sql::read_slots(*database_, expected_plugin_);
  if (!slots)
    return fail_closed(lock, observer);
  if (slots->sequence != expected_sequence)
    return AuthorityMutationResult::stale_sequence;
  if (!slots->candidate)
    return AuthorityMutationResult::invalid;
  auto candidate_snapshot =
      authority_sql::load_snapshot(*database_, *slots->candidate);
  if (!candidate_snapshot)
    return AuthorityMutationResult::invalid;
  if (candidate_snapshot->binding != candidate)
    return AuthorityMutationResult::invalid;
  if (!activatable(*candidate_snapshot))
    return AuthorityMutationResult::invalid;
  if (slots->sequence == UINT64_MAX)
    return AuthorityMutationResult::invalid;
  const auto previous = *slots;
  const auto fenced = fence_bound_live(lock, previous, observer);
  if (fenced != AuthorityMutationResult::applied)
    return fenced;
  poisoned_ = true;
  try {
    slots->active = std::move(slots->candidate);
    slots->candidate.reset();
    ++slots->sequence;
    const auto result = authority_sql::replace(*database_, expected_plugin_, previous, *slots);
    if (result != AuthorityMutationResult::applied) {
      transitioning_ = false;
      return result;
    }
    poisoned_ = false;
    transitioning_ = false;
    return AuthorityMutationResult::applied;
  } catch (...) {
    transitioning_ = false;
    return AuthorityMutationResult::io_error;
  }
}

AuthorityRevocationResult AuthorityStore::revoke_active(
    const definitions::CapabilityReference &definition,
    std::uint64_t expected_sequence, AuthorityFenceObserver *observer) {
  const auto failure = [](AuthorityMutationResult status) {
    return AuthorityRevocationResult{
        .status = status, .binding = std::nullopt, .activatable = false};
  };
  std::unique_lock lock(mutation_mutex_);
  if (::getpid() != owner_pid_)
    return failure(AuthorityMutationResult::io_error);
  if (mutation_epoch_ == UINT64_MAX)
    return failure(AuthorityMutationResult::poisoned);
  ++mutation_epoch_;
  if (poisoned_ || transitioning_)
    return failure(AuthorityMutationResult::poisoned);

  auto slots = authority_sql::read_slots(*database_, expected_plugin_);
  if (!slots)
    return failure(fail_closed(lock, observer));
  if (!slots->active)
    return failure(AuthorityMutationResult::io_error);
  if (slots->sequence != expected_sequence)
    return failure(AuthorityMutationResult::stale_sequence);
  if (slots->sequence == UINT64_MAX ||
      slots->generation_high_watermark == UINT64_MAX)
    return failure(AuthorityMutationResult::invalid);
  auto active = authority_sql::load_snapshot(*database_, *slots->active);
  if (!active || active->binding.plugin != expected_plugin_)
    return failure(fail_closed(lock, observer));

  bool found = false;
    for (auto &dynamic : active->dynamic_grants) {
      if (dynamic.request.definition != definition)
        continue;
      if (found || dynamic.grant.state != permissions::GrantState::granted)
        return failure(AuthorityMutationResult::invalid);
      dynamic.grant.state = permissions::GrantState::revoked;
      found = true;
    }
  if (!found)
    return failure(AuthorityMutationResult::invalid);

  const auto next_generation = slots->generation_high_watermark + 1;
  active->binding.generation = next_generation;
  for (auto &dynamic : active->dynamic_grants) {
    dynamic.binding = active->binding;
    dynamic.grant.epoch = next_generation;
  }

  const auto previous = *slots;
  const auto fenced = fence_bound_live(lock, previous, observer);
  if (fenced != AuthorityMutationResult::applied)
    return failure(fenced);
  // From this fence until a complete typed success result exists, every exit
  // leaves this process unable to reuse potentially ambiguous durable state.
  poisoned_ = true;
  try {
    const auto reference = snapshot_reference(*active);
    if (!reference)
      return failure(AuthorityMutationResult::io_error);

    slots->active = *reference;
    slots->candidate.reset();
    slots->generation_high_watermark = next_generation;
    ++slots->sequence;
    const auto replaced = authority_sql::replace(*database_, expected_plugin_, previous, *slots, std::span(&*active, 1));
    if (replaced != AuthorityMutationResult::applied)
      return failure(replaced);
    AuthorityRevocationResult success{.status =
                                          AuthorityMutationResult::applied,
                                      .binding = active->binding,
                                      .activatable = activatable(*active)};
    static_assert(
        std::is_nothrow_move_constructible_v<AuthorityRevocationResult>);
    poisoned_ = false;
    transitioning_ = false;
    return success;
  } catch (...) {
    // Once the old live generation is fenced, no exceptional persistence path
    // may leave this in-process store usable against ambiguous durable state.
    transitioning_ = false;
    return failure(AuthorityMutationResult::io_error);
  }
}

GrantResolution
AuthorityStore::resolve(std::string_view plugin_id,
                        std::string_view revision_sha256) const {
  std::scoped_lock lock(mutation_mutex_);
  if (::getpid() != owner_pid_ || poisoned_ || transitioning_)
    return {};
  auto slots = authority_sql::read_slots(*database_, expected_plugin_);
  if (!slots || plugin_id != expected_plugin_.view() ||
      !slots->active)
    return {};
  auto snapshot = authority_sql::load_snapshot(*database_, *slots->active);
  if (!snapshot || snapshot->binding.plugin.view() != plugin_id ||
      snapshot->binding.revision.view() != revision_sha256)
    return {};
  const auto status = activatable(*snapshot) ? GrantStatus::activatable
                                             : GrantStatus::permission_disabled;
  return {.snapshot = std::move(snapshot), .status = status};
}

} // namespace omarchy::plugin_runtime::host_session
