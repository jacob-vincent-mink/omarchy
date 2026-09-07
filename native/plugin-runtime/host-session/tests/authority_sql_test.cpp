#include "authority_sql.hpp"
#include "authority_snapshot_codec.hpp"
#include "../../tests/support/temporary_directory.hpp"

#include <sqlite3.h>
#include <sys/wait.h>

namespace host = omarchy::plugin_runtime::host_session;
namespace sql = host::authority_sql;
namespace support = omarchy::plugin_runtime::test_support;
namespace permissions = omarchy::plugins::permissions;
namespace definitions = omarchy::plugins::definitions;
namespace policy = omarchy::plugin_runtime::policy;
using omarchy::plugin_runtime::UniqueFd;
const permissions::PluginId plugin("org.example.sql-authority");

namespace {
struct Fixture : support::TemporaryDirectory {
  UniqueFd root{::open(path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  auto open() { return host::SqliteAuthorityDatabase::open(root.get(), ::getuid()); }
};

policy::GrantSnapshot snapshot(std::uint64_t generation, char revision = 'a') {
  policy::GrantSnapshot result;
  result.binding = {plugin, permissions::Digest(std::string(64, revision)),
      permissions::Digest(std::string(64, 'b')), generation};
  definitions::DynamicRevisionGrant grant;
  grant.binding = result.binding;
  grant.request.definition = {definitions::Name("storage.private"), UINT32_MAX,
      definitions::Digest(std::string(64, 'c'))};
  grant.request.scope = definitions::CanonicalScope("{\"namespace\":\"reviewed\"}");
  grant.request.required = true;
  OMARCHY_CHECK(grant.request.operations.insert(definitions::Name("read")));
  OMARCHY_CHECK(grant.request.operations.insert(definitions::Name("write")));
  grant.grant.operations = grant.request.operations;
  grant.grant.state = permissions::GrantState::granted;
  grant.grant.epoch = generation;
  result.dynamic_grants.push_back(grant);
  return result;
}

host::AuthorityRevisionRef reference(const policy::GrantSnapshot &snapshot) {
  std::vector<std::byte> bytes;
  OMARCHY_CHECK(host::authority_snapshot_codec::encode_snapshot(snapshot, bytes));
  return host::authority_snapshot_codec::reference_for(snapshot, omarchy::plugins::manifest::sha256_hex(bytes));
}

std::vector<std::byte> encoded(const policy::GrantSnapshot &snapshot) {
  std::vector<std::byte> bytes;
  OMARCHY_CHECK(host::authority_snapshot_codec::encode_snapshot(snapshot, bytes));
  return bytes;
}

void execute(host::SqliteAuthorityDatabase &db, const char *statement) {
  OMARCHY_CHECK(sqlite3_exec(db.connection(), statement, nullptr, nullptr, nullptr) == SQLITE_OK);
}

int count(host::SqliteAuthorityDatabase &db, const char *statement) {
  sqlite3_stmt *query = nullptr;
  OMARCHY_CHECK(sqlite3_prepare_v2(db.connection(), statement, -1, &query, nullptr) == SQLITE_OK);
  OMARCHY_CHECK(sqlite3_step(query) == SQLITE_ROW);
  const auto result = sqlite3_column_int(query, 0);
  OMARCHY_CHECK(sqlite3_step(query) == SQLITE_DONE);
  OMARCHY_CHECK(sqlite3_finalize(query) == SQLITE_OK);
  return result;
}

void transactions_and_rows() {
  Fixture fixture;
  auto database = fixture.open();
  OMARCHY_CHECK(database && sql::schema_state(*database) == sql::SchemaState::empty);
  OMARCHY_CHECK(sql::initialize(*database, plugin, {}, {}));
  OMARCHY_CHECK(sql::schema_state(*database) == sql::SchemaState::current);
  OMARCHY_CHECK(!sql::initialize(*database, plugin, {}, {}));
  const auto initial = sql::read_slots(*database, plugin);
  OMARCHY_CHECK(initial && *initial == host::AuthoritySlots{});
  OMARCHY_CHECK(!sql::read_slots(*database, permissions::PluginId("other.plugin")));
  const auto first = snapshot(1);
  host::AuthoritySlots candidate{1, 1, {}, reference(first)};
  OMARCHY_CHECK(sql::replace(*database, plugin, *initial, candidate, std::span(&first, 1)) == host::AuthorityMutationResult::applied);
  auto loaded = sql::load_snapshot(*database, *candidate.candidate);
  OMARCHY_CHECK(loaded && encoded(*loaded) == encoded(first));
  OMARCHY_CHECK(count(*database, "SELECT count(*) FROM revisions") == 1);
  OMARCHY_CHECK(count(*database, "SELECT count(*) FROM grants WHERE definition_generation=4294967295 AND required=1 AND state=0") == 1);
  OMARCHY_CHECK(count(*database, "SELECT count(*) FROM operations WHERE granted=1") == 2);
  OMARCHY_CHECK(sql::replace(*database, plugin, *initial, candidate, std::span(&first, 1)) == host::AuthorityMutationResult::stale_sequence);
  host::AuthoritySlots active{2, 1, candidate.candidate, {}};
  OMARCHY_CHECK(sql::replace(*database, plugin, candidate, active) == host::AuthorityMutationResult::applied);
  auto revoked = snapshot(2);
  revoked.dynamic_grants[0].grant.state = permissions::GrantState::revoked;
  host::AuthoritySlots next{3, 2, reference(revoked), {}};
  OMARCHY_CHECK(sql::replace(*database, plugin, active, next, std::span(&revoked, 1)) == host::AuthorityMutationResult::applied);
  OMARCHY_CHECK(count(*database, "SELECT count(*) FROM revisions") == 1);
  OMARCHY_CHECK(count(*database, "SELECT count(*) FROM grants") == 1);
  OMARCHY_CHECK(count(*database, "SELECT count(*) FROM operations") == 2);
  OMARCHY_CHECK(!sql::load_snapshot(*database, *active.active));
  database.reset();
  database = fixture.open();
  OMARCHY_CHECK(database && sql::read_slots(*database, plugin) == next);
  loaded = sql::load_snapshot(*database, *next.active);
  OMARCHY_CHECK(loaded && encoded(*loaded) == encoded(revoked));
}

void rollback_and_corruption() {
  Fixture fixture;
  auto database = fixture.open();
  const auto first = snapshot(1);
  const host::AuthoritySlots before{2, 1, reference(first), {}};
  OMARCHY_CHECK(sql::initialize(*database, plugin, before, std::span(&first, 1)));
  const auto second = snapshot(2, 'd');
  host::AuthoritySlots missing{3, 2, before.active, reference(snapshot(2, 'e'))};
  // Inserts a complete new revision, then fails the requested head reference.
  // Both the insertion and head change must disappear on transaction rollback.
  OMARCHY_CHECK(sql::replace(*database, plugin, before, missing, std::span(&second, 1)) == host::AuthorityMutationResult::io_error);
  OMARCHY_CHECK(sql::read_slots(*database, plugin) == before);
  OMARCHY_CHECK(count(*database, "SELECT count(*) FROM revisions") == 1);
  OMARCHY_CHECK(!sql::load_snapshot(*database, reference(second)));
  auto stale = before;
  stale.active->snapshot_digest = permissions::Digest(std::string(64, 'f'));
  OMARCHY_CHECK(sql::replace(*database, plugin, stale, missing, std::span(&second, 1)) == host::AuthorityMutationResult::stale_sequence);
  OMARCHY_CHECK(sqlite3_exec(database->connection(), "UPDATE authority SET sequence=2", nullptr, nullptr, nullptr) != SQLITE_OK);
  OMARCHY_CHECK(sqlite3_exec(database->connection(), "UPDATE grants SET definition_generation=0", nullptr, nullptr, nullptr) != SQLITE_OK);
  execute(*database, "UPDATE operations SET granted=0 WHERE operation='write'");
  OMARCHY_CHECK(!sql::load_snapshot(*database, *before.active));
  execute(*database, "CREATE TABLE unexpected(value TEXT) STRICT");
  OMARCHY_CHECK(sql::schema_state(*database) == sql::SchemaState::invalid);
  OMARCHY_CHECK(!sql::read_slots(*database, plugin));
}

void invalid_initialization() {
  Fixture fixture;
  auto database = fixture.open();
  auto first = snapshot(1);
  const host::AuthoritySlots initial{2, 1, reference(first), {}};
  OMARCHY_CHECK(!sql::initialize(*database, plugin, initial, {}));
  OMARCHY_CHECK(sql::schema_state(*database) == sql::SchemaState::empty);
  // A mismatched embedded binding cannot be imported as exact consent.
  first.dynamic_grants[0].binding.generation = 2;
  OMARCHY_CHECK(!sql::initialize(*database, plugin, initial, std::span(&first, 1)));
  OMARCHY_CHECK(sql::schema_state(*database) == sql::SchemaState::empty);
  first = snapshot(1);
  first.dynamic_grants[0].grant.operations.insert(definitions::Name("undeclared"));
  OMARCHY_CHECK(!sql::initialize(*database, plugin, initial, std::span(&first, 1)));
  OMARCHY_CHECK(sql::schema_state(*database) == sql::SchemaState::empty);
  execute(*database, "PRAGMA user_version=99");
  OMARCHY_CHECK(sql::schema_state(*database) == sql::SchemaState::invalid);
  OMARCHY_CHECK(!sql::initialize(*database, plugin, {}, {}));
}

void full_width_counters() {
  Fixture fixture;
  auto database = fixture.open();
  const auto first = snapshot(UINT64_MAX - 1);
  const host::AuthoritySlots before{UINT64_MAX - 1, UINT64_MAX - 1, reference(first), {}};
  OMARCHY_CHECK(sql::initialize(*database, plugin, before, std::span(&first, 1)));
  const auto second = snapshot(UINT64_MAX, 'd');
  const host::AuthoritySlots after{UINT64_MAX, UINT64_MAX, reference(second), {}};
  OMARCHY_CHECK(sql::replace(*database, plugin, before, after, std::span(&second, 1)) == host::AuthorityMutationResult::applied);
  database.reset();
  database = fixture.open();
  OMARCHY_CHECK(database && sql::read_slots(*database, plugin) == after);
  const auto loaded = sql::load_snapshot(*database, *after.active);
  OMARCHY_CHECK(loaded && encoded(*loaded) == encoded(second));
  OMARCHY_CHECK(sql::replace(*database, plugin, after, {}) == host::AuthorityMutationResult::invalid);
}

void exact_optional_choices() {
  Fixture fixture;
  auto database = fixture.open();
  auto reviewed = snapshot(1);
  auto &partial = reviewed.dynamic_grants[0];
  partial.request.required = false;
  partial.grant.operations = {};
  OMARCHY_CHECK(partial.grant.operations.insert(definitions::Name("read")));
  auto denied = partial;
  denied.request.definition.canonical_name = definitions::Name("system.observe");
  denied.request.scope = definitions::CanonicalScope("{\"dataset\":\"outputs\"}");
  denied.grant.operations = denied.request.operations;
  denied.grant.state = permissions::GrantState::denied;
  reviewed.dynamic_grants.push_back(denied);
  const host::AuthoritySlots initial{2, 1, reference(reviewed), {}};
  OMARCHY_CHECK(sql::initialize(*database, plugin, initial, std::span(&reviewed, 1)));
  auto loaded = sql::load_snapshot(*database, *initial.active);
  OMARCHY_CHECK(loaded && encoded(*loaded) == encoded(reviewed));
  OMARCHY_CHECK(count(*database, "SELECT count(*) FROM operations WHERE name='storage.private' AND granted=0 AND operation='write'") == 1);
  OMARCHY_CHECK(count(*database, "SELECT count(*) FROM grants WHERE name='system.observe' AND state=1 AND required=0") == 1);
}

void crash_before_commit() {
  Fixture fixture;
  const auto first = snapshot(1);
  const host::AuthoritySlots before{2, 1, reference(first), {}};
  {
    auto database = fixture.open();
    OMARCHY_CHECK(sql::initialize(*database, plugin, before, std::span(&first, 1)));
  }
  const auto child = ::fork();
  OMARCHY_CHECK(child >= 0);
  if (child == 0) {
    auto database = fixture.open();
    if (!database)
      ::_exit(1);
    sqlite3_commit_hook(database->connection(), [](void *) -> int { ::_exit(86); }, nullptr);
    const auto second = snapshot(2, 'd');
    const host::AuthoritySlots next{3, 2, reference(second), {}};
    (void)sql::replace(*database, plugin, before, next, std::span(&second, 1));
    ::_exit(1);
  }
  int status = 0;
  OMARCHY_CHECK(::waitpid(child, &status, 0) == child);
  OMARCHY_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 86);
  auto database = fixture.open();
  OMARCHY_CHECK(database && sql::read_slots(*database, plugin) == before);
  OMARCHY_CHECK(count(*database, "SELECT count(*) FROM revisions") == 1);
  const auto loaded = sql::load_snapshot(*database, *before.active);
  OMARCHY_CHECK(loaded && encoded(*loaded) == encoded(first));
}
} // namespace

int main() {
  return support::test_main([] {
    transactions_and_rows();
    rollback_and_corruption();
    invalid_initialization();
    full_width_counters();
    exact_optional_choices();
    crash_before_commit();
    return 0;
  });
}
