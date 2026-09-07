#include "sqlite_authority_database.hpp"
#include "../../tests/support/temporary_directory.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <sys/stat.h>
#include <sys/wait.h>

namespace host = omarchy::plugin_runtime::host_session;
namespace support = omarchy::plugin_runtime::test_support;
using omarchy::plugin_runtime::UniqueFd;

namespace {
struct Fixture : support::TemporaryDirectory {
  UniqueFd root{::open(path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  auto open() { return host::SqliteAuthorityDatabase::open(root.get(), ::getuid()); }
};

void sql(host::SqliteAuthorityDatabase &database, const char *statement) {
  OMARCHY_CHECK(database.intact());
  const auto result = sqlite3_exec(database.connection(), statement, nullptr, nullptr, nullptr);
  support::require(result == SQLITE_OK, sqlite3_errmsg(database.connection()));
}

int scalar(host::SqliteAuthorityDatabase &database, const char *sql_text) {
  sqlite3_stmt *statement = nullptr;
  OMARCHY_CHECK(sqlite3_prepare_v2(database.connection(), sql_text, -1, &statement, nullptr) == SQLITE_OK);
  OMARCHY_CHECK(sqlite3_step(statement) == SQLITE_ROW);
  const auto result = sqlite3_column_int(statement, 0);
  OMARCHY_CHECK(sqlite3_step(statement) == SQLITE_DONE);
  OMARCHY_CHECK(sqlite3_finalize(statement) == SQLITE_OK);
  return result;
}

void rename_and_concurrency() {
  Fixture fixture;
  auto first = fixture.open();
  OMARCHY_CHECK(first);
  sql(*first, "CREATE TABLE authority(sequence INTEGER NOT NULL); INSERT INTO authority VALUES(1)");
  // Rename the directory into a sibling fixture, then put an untrusted fresh
  // directory at its old name. All subsequent SQLite work must stay fd-rooted.
  support::TemporaryDirectory destination;
  const auto retained = destination.path() / "retained";
  OMARCHY_CHECK(::rename(fixture.path().c_str(), retained.c_str()) == 0);
  OMARCHY_CHECK(::mkdir(fixture.path().c_str(), 0700) == 0);
  auto second = fixture.open();
  OMARCHY_CHECK(second);
  sql(*first, "BEGIN IMMEDIATE; UPDATE authority SET sequence=2");
  OMARCHY_CHECK(sqlite3_exec(second->connection(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_BUSY);
  OMARCHY_CHECK(scalar(*second, "SELECT sequence FROM authority") == 1);
  sql(*first, "COMMIT");
  OMARCHY_CHECK(scalar(*second, "SELECT sequence FROM authority") == 2);
  // Closing an independent Unix handle must not release another handle's lock.
  sql(*first, "BEGIN IMMEDIATE");
  second.reset();
  auto third = fixture.open();
  OMARCHY_CHECK(third);
  OMARCHY_CHECK(sqlite3_exec(third->connection(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_BUSY);
  sql(*first, "UPDATE authority SET sequence=3; COMMIT");
  third.reset();
  first.reset();
  auto reopened = fixture.open();
  OMARCHY_CHECK(reopened && scalar(*reopened, "SELECT sequence FROM authority") == 3);
  OMARCHY_CHECK(!std::filesystem::exists(fixture.path() / "authority.db"));
  OMARCHY_CHECK(std::filesystem::exists(retained / "authority.db"));
  OMARCHY_CHECK(scalar(*reopened, "PRAGMA synchronous") == 3);
  OMARCHY_CHECK(sqlite3_exec(reopened->connection(), "ATTACH ':memory:' AS extra", nullptr, nullptr, nullptr) != SQLITE_OK);
}

void hostile_files() {
  for (const auto *leaf : {"authority.db", "authority.db-journal", "authority.db-wal", "authority.db-shm"}) {
    for (int variant = 0; variant != 5; ++variant) {
      Fixture fixture;
      const auto file = fixture.path() / leaf;
      if (variant == 0) {
        OMARCHY_CHECK(::symlink("missing", file.c_str()) == 0);
      } else if (variant == 1) {
        OMARCHY_CHECK(::mkfifo(file.c_str(), 0600) == 0);
      } else if (variant == 2) {
        OMARCHY_CHECK(::mkdir(file.c_str(), 0700) == 0);
      } else {
        support::TemporaryDirectory::write_file(file, "", O_WRONLY | O_CREAT | O_EXCL);
        if (variant == 3)
          OMARCHY_CHECK(::chmod(file.c_str(), 0644) == 0);
        else
          OMARCHY_CHECK(::link(file.c_str(), (fixture.path() / "alias").c_str()) == 0);
      }
      OMARCHY_CHECK(!fixture.open());
    }
  }
  Fixture permissions;
  OMARCHY_CHECK(::chmod(permissions.path().c_str(), 0755) == 0);
  OMARCHY_CHECK(!permissions.open());
  Fixture owner;
  OMARCHY_CHECK(!host::SqliteAuthorityDatabase::open(owner.root.get(), ::getuid() + 1));
  Fixture corrupt;
  support::TemporaryDirectory::write_file(corrupt.path() / "authority.db", std::string(4096, 'x'), O_WRONLY | O_CREAT | O_EXCL);
  OMARCHY_CHECK(!corrupt.open());
  Fixture orphan_journal;
  support::TemporaryDirectory::write_file(orphan_journal.path() / "authority.db-journal", "", O_WRONLY | O_CREAT | O_EXCL);
  OMARCHY_CHECK(!orphan_journal.open());
  OMARCHY_CHECK(!std::filesystem::exists(orphan_journal.path() / "authority.db"));
  Fixture wal;
  sqlite3 *wal_database = nullptr;
  OMARCHY_CHECK(sqlite3_open((wal.path() / "authority.db").c_str(), &wal_database) == SQLITE_OK);
  OMARCHY_CHECK(sqlite3_exec(wal_database, "PRAGMA journal_mode=WAL; CREATE TABLE unsupported(id INTEGER)", nullptr, nullptr, nullptr) == SQLITE_OK);
  OMARCHY_CHECK(sqlite3_close(wal_database) == SQLITE_OK);
  OMARCHY_CHECK(::chmod((wal.path() / "authority.db").c_str(), 0600) == 0);
  OMARCHY_CHECK(!wal.open());
  Fixture oversized;
  UniqueFd large(::openat(oversized.root.get(), "authority.db", O_RDWR | O_CREAT | O_EXCL, 0600));
  OMARCHY_CHECK(large && ::ftruncate(large.get(), 64 * 1024 * 1024 + 1) == 0);
  OMARCHY_CHECK(!oversized.open());
  Fixture replaced;
  auto database = replaced.open();
  OMARCHY_CHECK(database);
  sql(*database, "CREATE TABLE authority(sequence INTEGER)");
  OMARCHY_CHECK(::renameat(replaced.root.get(), "authority.db", replaced.root.get(), "old.db") == 0);
  support::TemporaryDirectory::write_file(replaced.path() / "authority.db", "", O_WRONLY | O_CREAT | O_EXCL);
  OMARCHY_CHECK(!database->intact());
}

void crash_recovery() {
  for (const bool commit : {false, true}) {
    Fixture fixture;
    {
      auto database = fixture.open();
      OMARCHY_CHECK(database);
      sql(*database, "CREATE TABLE authority(sequence INTEGER, payload BLOB); INSERT INTO authority VALUES(1, zeroblob(131072))");
    }
    const auto child = ::fork();
    OMARCHY_CHECK(child >= 0);
    if (child == 0) {
      auto database = fixture.open();
      if (!database)
        ::_exit(1);
      sql(*database, "PRAGMA cache_size=1; BEGIN IMMEDIATE; UPDATE authority SET sequence=2, payload=randomblob(131072)");
      if (commit)
        sql(*database, "COMMIT");
      // No destructor or rollback: the parent reopens the actual hot journal.
      ::_exit(86);
    }
    int status = 0;
    OMARCHY_CHECK(::waitpid(child, &status, 0) == child);
    OMARCHY_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 86);
    OMARCHY_CHECK(std::filesystem::exists(fixture.path() / "authority.db-journal") == !commit);
    auto database = fixture.open();
    OMARCHY_CHECK(database);
    OMARCHY_CHECK(scalar(*database, "SELECT sequence FROM authority") == (commit ? 2 : 1));
    sql(*database, "BEGIN IMMEDIATE; UPDATE authority SET sequence=3; COMMIT");
    OMARCHY_CHECK(scalar(*database, "SELECT sequence FROM authority") == 3);
  }
}
} // namespace

int main() {
  return support::test_main([] {
    rename_and_concurrency();
    hostile_files();
    crash_recovery();
    return 0;
  });
}
