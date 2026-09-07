#include "authority_migration.hpp"
#include "authority_sql.hpp"
#include "authority_snapshot_codec.hpp"
#include "omarchy/plugin_runtime/canonical_sha256.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omarchy::plugin_runtime::host_session {
namespace {
constexpr std::string_view kSlotsName = "slots";
constexpr std::string_view migration_stamp = "SQLITE_AUTHORITY_1\n";
bool trusted_file(const struct stat &metadata, std::uint32_t uid) {
  return S_ISREG(metadata.st_mode) && metadata.st_uid == uid &&
         metadata.st_nlink == 1 && (metadata.st_mode & 07777) == 0600;
}

std::optional<std::vector<std::byte>> read_file(int root_fd,
                                                std::string_view name,
                                                std::uint32_t uid) {
  const std::string owned_name(name);
  UniqueFd file(
      ::openat(root_fd, owned_name.c_str(),
               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (!file)
    return std::nullopt;
  struct stat before{}, after{};
  if (::fstat(file.get(), &before) < 0 || !trusted_file(before, uid) ||
      before.st_size < 0 ||
      static_cast<std::uint64_t>(before.st_size) >
          authority_snapshot_codec::kMaximumEncodedAuthorityBytes)
    return std::nullopt;
  std::vector<std::byte> bytes(static_cast<std::size_t>(before.st_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count =
        ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return std::nullopt;
    offset += static_cast<std::size_t>(count);
  }
  std::byte trailing{};
  if (::read(file.get(), &trailing, 1) != 0)
    return std::nullopt;
  if (::fstat(file.get(), &after) < 0 || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
    return std::nullopt;
  return bytes;
}

std::string digest(std::span<const std::byte> bytes) {
  return plugins::manifest::sha256_hex(bytes);
}

std::string record_name(const AuthorityRevisionRef &reference) {
  return "grant-" + std::string(reference.snapshot_digest.view());
}

std::optional<AuthoritySlots> read_slots_unlocked(int root_fd,
                                                  std::uint32_t uid) {
  struct stat metadata{};
  if (::fstatat(root_fd, std::string(kSlotsName).c_str(), &metadata,
                AT_SYMLINK_NOFOLLOW) < 0) {
    if (errno == ENOENT)
      return AuthoritySlots{};
    return std::nullopt;
  }
  auto bytes = read_file(root_fd, kSlotsName, uid);
  if (!bytes)
    return std::nullopt;
  return authority_snapshot_codec::decode_slots(*bytes);
}

bool valid_slots(const AuthoritySlots &slots) {
  if (slots.sequence < slots.generation_high_watermark)
    return false;
  for (const auto *reference : {&slots.active, &slots.candidate}) {
    if (!*reference)
      continue;
    if (!canonical_sha256((*reference)->snapshot_digest.view()) ||
        (*reference)->generation == 0 ||
        (*reference)->generation > slots.generation_high_watermark)
      return false;
  }
  if (slots.candidate &&
      slots.candidate->generation != slots.generation_high_watermark)
    return false;
  if (slots.active && slots.candidate &&
      slots.active->generation >= slots.candidate->generation)
    return false;
  return true;
}

std::optional<policy::GrantSnapshot>
load_snapshot(int root_fd, std::uint32_t uid,
              const AuthorityRevisionRef &reference) {
  auto bytes = read_file(root_fd, record_name(reference), uid);
  if (!bytes || digest(*bytes) != reference.snapshot_digest.view())
    return std::nullopt;
  policy::GrantSnapshot snapshot;
  std::vector<std::byte> canonical;
  if (!authority_snapshot_codec::decode_snapshot(*bytes, snapshot) ||
      !authority_snapshot_codec::encode_snapshot(snapshot, canonical) ||
      canonical != *bytes ||
      snapshot.binding.generation != reference.generation)
    return std::nullopt;
  try {
    definitions::Name previous;
    bool have_previous = false;
    for (const auto &dynamic : snapshot.dynamic_grants) {
      if (dynamic.binding != snapshot.binding ||
          dynamic.grant.epoch == 0 ||
          static_cast<std::uint8_t>(dynamic.grant.state) >
              static_cast<std::uint8_t>(permissions::GrantState::revoked) ||
          !std::ranges::all_of(dynamic.grant.operations.values(),
                               [&](const auto &operation) {
                                 return dynamic.request.operations.contains(
                                     operation);
                               }) ||
          (have_previous &&
           !(previous < dynamic.request.definition.canonical_name)))
        return std::nullopt;
      previous = dynamic.request.definition.canonical_name;
      have_previous = true;
    }
  } catch (...) {
    return std::nullopt;
  }
  return snapshot;
}


} // namespace

std::unique_ptr<SqliteAuthorityDatabase>
open_authority_database(int root, int lock, std::uint32_t uid,
                        const permissions::PluginId &plugin) {
  // A persisted stamp prevents database loss from resurrecting old legacy
  // grants. No SQL authority mutation is exposed until this one-time stamp is
  // synced. A crash before it is synced can only repeat the same import.
  struct stat lock_metadata{}, database_metadata{};
  if (::fstat(lock, &lock_metadata) < 0 || !trusted_file(lock_metadata, uid))
    return {};
  std::array<char, 64> stamp{};
  const auto size = ::pread(lock, stamp.data(), stamp.size(), 0);
  if (size < 0 || (size != 0 &&
      (size != static_cast<ssize_t>(migration_stamp.size()) ||
       std::string_view(stamp.data(), size) != migration_stamp)))
    return {};
  const bool migrated = size != 0;
  if (migrated && (::fstatat(root, "authority.db", &database_metadata, AT_SYMLINK_NOFOLLOW) < 0 ||
                   database_metadata.st_size == 0))
    return {};
  auto database = SqliteAuthorityDatabase::open(root, uid);
  if (!database)
    return {};
  const auto schema = authority_sql::schema_state(*database);
  if (schema == authority_sql::SchemaState::invalid ||
      (migrated && schema != authority_sql::SchemaState::current))
    return {};
  if (schema == authority_sql::SchemaState::empty) {
    const auto slots = read_slots_unlocked(root, uid);
    if (!slots || !valid_slots(*slots))
      return {};
    std::vector<policy::GrantSnapshot> snapshots;
    for (const auto *reference : {&slots->active, &slots->candidate}) {
      if (!*reference)
        continue;
      auto snapshot = load_snapshot(root, uid, **reference);
      if (!snapshot || snapshot->binding.plugin != plugin)
        return {};
      snapshots.push_back(std::move(*snapshot));
    }
    if (!authority_sql::initialize(*database, plugin, *slots, snapshots))
      return {};
#ifdef OMARCHY_AUTHORITY_STORE_TESTING
    authority_sql::checkpoint_for_testing(AuthorityCrashPoint::migration_after_commit);
#endif
  }
  const auto slots = authority_sql::read_slots(*database, plugin);
  if (!slots)
    return {};
  for (const auto *reference : {&slots->active, &slots->candidate}) {
    if (*reference) {
      const auto snapshot = authority_sql::load_snapshot(*database, **reference);
      if (!snapshot || snapshot->binding.plugin != plugin)
        return {};
    }
  }
  if (!migrated) {
    if (::pwrite(lock, migration_stamp.data(), migration_stamp.size(), 0) !=
            static_cast<ssize_t>(migration_stamp.size()))
      return {};
#ifdef OMARCHY_AUTHORITY_STORE_TESTING
    authority_sql::checkpoint_for_testing(AuthorityCrashPoint::migration_after_stamp_write);
#endif
  }
  // Also sync an already-visible stamp: it may have been written by a process
  // that exited before syncing it. No subsequent mutation may outrun cutover.
  if (::fsync(lock) < 0 || ::fsync(root) < 0)
    return {};
#ifdef OMARCHY_AUTHORITY_STORE_TESTING
  authority_sql::checkpoint_for_testing(AuthorityCrashPoint::migration_after_stamp_sync);
#endif
  return database;
}
} // namespace omarchy::plugin_runtime::host_session
