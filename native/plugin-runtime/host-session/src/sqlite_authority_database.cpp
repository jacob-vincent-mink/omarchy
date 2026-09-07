#include "sqlite_authority_database.hpp"
#include "omarchy/plugin_runtime/unique_fd.hpp"

#include <sqlite3.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <type_traits>
#include <sys/stat.h>
#include <unistd.h>

namespace omarchy::plugin_runtime::host_session {
namespace {
constexpr sqlite3_int64 maximum_file_bytes = 64 * 1024 * 1024;
std::atomic<std::uint64_t> next_vfs{1};

bool trusted_file(const struct stat &metadata, std::uint32_t uid) {
  return S_ISREG(metadata.st_mode) && metadata.st_uid == uid &&
         metadata.st_nlink == 1 && (metadata.st_mode & 07777) == 0600 &&
         metadata.st_size >= 0 && metadata.st_size <= maximum_file_bytes;
}
} // namespace

struct SqliteAuthorityDatabase::State {
  // First member: callbacks receive this address. Preserve Unix pAppData and
  // delegate with the original VFS, not a replacement filesystem implementation.
  sqlite3_vfs vfs{};
  sqlite3_vfs *unix_vfs = nullptr;
  UniqueFd directory;
  std::uint32_t uid = 0;
  pid_t pid = ::getpid();
  std::string vfs_name;
  std::string path;
  struct stat database_identity{};
  sqlite3 *database = nullptr;
  bool registered = false;

  ~State() {
    // The owner finalizes statements first. close_v2 would leave a zombie
    // connection referring to a destroyed VFS, so it is intentionally unused.
    if (database && sqlite3_close(database) != SQLITE_OK)
      std::terminate();
    if (registered)
      sqlite3_vfs_unregister(&vfs);
  }

  static State &from(sqlite3_vfs *vfs) {
    static_assert(std::is_pointer_interconvertible_with_class(&State::vfs));
    return *reinterpret_cast<State *>(vfs);
  }

  bool root_intact() const {
    struct stat metadata{};
    return ::getpid() == pid && ::fstat(directory.get(), &metadata) == 0 &&
           S_ISDIR(metadata.st_mode) && metadata.st_uid == uid &&
           (metadata.st_mode & 07777) == 0700;
  }

  // Only SQLite's fixed database and rollback journal may be opened. WAL and
  // shared-memory files are checked for absence, never silently ignored.
  const char *filename(const char *name) const {
    if (!name)
      return nullptr;
    if (path == name)
      return "authority.db";
    if (path.size() < std::strlen(name) &&
        std::strncmp(name, path.c_str(), path.size()) == 0) {
      const auto *suffix = name + path.size();
      if (std::strcmp(suffix, "-journal") == 0)
        return "authority.db-journal";
      if (std::strcmp(suffix, "-wal") == 0)
        return "authority.db-wal";
      if (std::strcmp(suffix, "-shm") == 0)
        return "authority.db-shm";
    }
    return nullptr;
  }

  bool metadata(const char *name, bool missing_allowed, bool *exists = nullptr) const {
    struct stat value{};
    if (::fstatat(directory.get(), name, &value, AT_SYMLINK_NOFOLLOW) < 0) {
      if (exists)
        *exists = false;
      return missing_allowed && errno == ENOENT;
    }
    if (exists)
      *exists = true;
    if (std::strcmp(name, "authority.db-wal") == 0 ||
        std::strcmp(name, "authority.db-shm") == 0)
      return false;
    return trusted_file(value, uid);
  }

  static int full_path(sqlite3_vfs *vfs, const char *name, int count, char *out) {
    const auto &self = from(vfs);
    // Do not canonicalize the descriptor into its mutable original pathname.
    if (!self.root_intact() || !name || self.path != name ||
        count <= 0 || self.path.size() >= static_cast<std::size_t>(count))
      return SQLITE_CANTOPEN;
    std::memcpy(out, name, self.path.size() + 1);
    return SQLITE_OK;
  }

  static int open_file(sqlite3_vfs *vfs, sqlite3_filename name,
                       sqlite3_file *file, int flags, int *out_flags) {
    auto &self = from(vfs);
    file->pMethods = nullptr;
    const auto *leaf = self.filename(name);
    const bool main = (flags & SQLITE_OPEN_MAIN_DB) != 0;
    const bool journal = (flags & SQLITE_OPEN_MAIN_JOURNAL) != 0;
    if (!self.root_intact() || !leaf || main == journal ||
        std::strcmp(leaf, main ? "authority.db" : "authority.db-journal") != 0 ||
        !self.metadata(leaf, (flags & SQLITE_OPEN_CREATE) != 0))
      return SQLITE_CANTOPEN;
    // Precreate with private mode independently of the host's umask. Existing
    // files are never chmod'ed into apparent trustworthiness.
    if ((flags & SQLITE_OPEN_CREATE) != 0) {
      UniqueFd created(::openat(self.directory.get(), leaf,
          O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
      if (!created && errno != EEXIST)
        return SQLITE_CANTOPEN;
      if (created && (::fchmod(created.get(), 0600) < 0 ||
                      ::fsync(self.directory.get()) < 0))
        return SQLITE_IOERR_DIR_FSYNC;
    }
    if (!self.metadata(leaf, false))
      return SQLITE_CANTOPEN;
    return self.unix_vfs->xOpen(self.unix_vfs, name, file,
                                flags | SQLITE_OPEN_NOFOLLOW, out_flags);
  }

  static int access_file(sqlite3_vfs *vfs, const char *name, int, int *result) {
    const auto &self = from(vfs);
    *result = 0;
    const auto *leaf = self.filename(name);
    bool exists = false;
    if (!self.root_intact() || !leaf || !self.metadata(leaf, true, &exists))
      return SQLITE_IOERR_ACCESS;
    *result = exists;
    return SQLITE_OK;
  }

  static int delete_file(sqlite3_vfs *vfs, const char *name, int sync_directory) {
    auto &self = from(vfs);
    const auto *leaf = self.filename(name);
    if (!self.root_intact() || !leaf ||
        std::strcmp(leaf, "authority.db-journal") != 0 ||
        !self.metadata(leaf, true))
      return SQLITE_IOERR_DELETE;
    return self.unix_vfs->xDelete(self.unix_vfs, name, sync_directory);
  }
};

SqliteAuthorityDatabase::SqliteAuthorityDatabase(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
SqliteAuthorityDatabase::~SqliteAuthorityDatabase() = default;

std::unique_ptr<SqliteAuthorityDatabase>
SqliteAuthorityDatabase::open(int directory_fd, std::uint32_t expected_uid) {
  auto state = std::make_unique<State>();
  state->directory.reset(::fcntl(directory_fd, F_DUPFD_CLOEXEC, 0));
  state->uid = expected_uid;
  state->unix_vfs = sqlite3_vfs_find("unix");
  bool database_exists = false;
  bool journal_exists = false;
  if (!state->directory || !state->root_intact() || !state->unix_vfs ||
      !state->metadata("authority.db", true, &database_exists) ||
      !state->metadata("authority.db-journal", true, &journal_exists) ||
      (!database_exists && journal_exists) ||
      !state->metadata("authority.db-wal", true) ||
      !state->metadata("authority.db-shm", true))
    return nullptr;
  state->vfs = *state->unix_vfs;
  state->vfs_name = "omarchy-authority-" + std::to_string(next_vfs.fetch_add(1));
  state->path = "/proc/self/fd/" + std::to_string(state->directory.get()) + "/authority.db";
  state->vfs.zName = state->vfs_name.c_str();
  state->vfs.xFullPathname = State::full_path;
  state->vfs.xOpen = State::open_file;
  state->vfs.xAccess = State::access_file;
  state->vfs.xDelete = State::delete_file;
  if (sqlite3_vfs_register(&state->vfs, 0) != SQLITE_OK)
    return nullptr;
  state->registered = true;
  if (sqlite3_open_v2(state->path.c_str(), &state->database,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW |
          SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_NOMUTEX,
      state->vfs_name.c_str()) != SQLITE_OK)
    return nullptr;
  sqlite3_extended_result_codes(state->database, 1);
  if (sqlite3_db_config(state->database, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) != SQLITE_OK ||
      sqlite3_db_config(state->database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) != SQLITE_OK ||
      sqlite3_db_config(state->database, SQLITE_DBCONFIG_DQS_DDL, 0, nullptr) != SQLITE_OK ||
      sqlite3_db_config(state->database, SQLITE_DBCONFIG_DQS_DML, 0, nullptr) != SQLITE_OK ||
      sqlite3_db_config(state->database, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr) != SQLITE_OK)
    return nullptr;
  sqlite3_limit(state->database, SQLITE_LIMIT_ATTACHED, 0);
  sqlite3_limit(state->database, SQLITE_LIMIT_LENGTH, 8 * 1024 * 1024);
  sqlite3_limit(state->database, SQLITE_LIMIT_SQL_LENGTH, 64 * 1024);
  // No WAL conversion: a WAL database belongs to an unsupported storage
  // contract even if its sidecars happen to be absent at this instant.
  sqlite3_stmt *mode = nullptr;
  const auto prepared = sqlite3_prepare_v2(state->database, "PRAGMA journal_mode", -1, &mode, nullptr);
  const auto *mode_text = prepared == SQLITE_OK && sqlite3_step(mode) == SQLITE_ROW
      ? sqlite3_column_text(mode, 0) : nullptr;
  const bool rollback_mode = mode_text &&
      std::strcmp(reinterpret_cast<const char *>(mode_text), "delete") == 0;
  sqlite3_finalize(mode);
  sqlite3_stmt *page_size_statement = nullptr;
  const auto page_size_prepared = sqlite3_prepare_v2(state->database, "PRAGMA page_size", -1, &page_size_statement, nullptr);
  const auto page_size = page_size_prepared == SQLITE_OK &&
      sqlite3_step(page_size_statement) == SQLITE_ROW
      ? sqlite3_column_int(page_size_statement, 0) : 0;
  sqlite3_finalize(page_size_statement);
  if (page_size < 512 || page_size > 65536 || (page_size & (page_size - 1)) != 0)
    return nullptr;
  const auto configuration =
      "PRAGMA synchronous=EXTRA; PRAGMA temp_store=MEMORY; PRAGMA mmap_size=0;"
      "PRAGMA foreign_keys=ON; PRAGMA max_page_count=" +
      std::to_string(maximum_file_bytes / page_size) + ";";
  if (!rollback_mode || sqlite3_exec(state->database,
      configuration.c_str(),
      nullptr, nullptr, nullptr) != SQLITE_OK ||
      ::fstatat(state->directory.get(), "authority.db", &state->database_identity,
                AT_SYMLINK_NOFOLLOW) < 0)
    return nullptr;
  return std::unique_ptr<SqliteAuthorityDatabase>(new SqliteAuthorityDatabase(std::move(state)));
}

bool SqliteAuthorityDatabase::intact() const {
  struct stat metadata{};
  return state_->root_intact() &&
         ::fstatat(state_->directory.get(), "authority.db", &metadata, AT_SYMLINK_NOFOLLOW) == 0 &&
         trusted_file(metadata, state_->uid) &&
         metadata.st_dev == state_->database_identity.st_dev &&
         metadata.st_ino == state_->database_identity.st_ino &&
         state_->metadata("authority.db-journal", true) &&
         state_->metadata("authority.db-wal", true) &&
         state_->metadata("authority.db-shm", true);
}

sqlite3 *SqliteAuthorityDatabase::connection() const { return state_->database; }

} // namespace omarchy::plugin_runtime::host_session
