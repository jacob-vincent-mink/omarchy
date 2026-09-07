#include "authority_sql.hpp"
#include "authority_snapshot_codec.hpp"
#include "omarchy/plugin_runtime/big_endian.hpp"
#include "omarchy/plugin_runtime/canonical_sha256.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>

namespace omarchy::plugin_runtime::host_session::authority_sql {
namespace {
#ifdef OMARCHY_AUTHORITY_STORE_TESTING
std::atomic<AuthorityCrashPoint> crash_point{AuthorityCrashPoint::none};
void crash_after(AuthorityCrashPoint point) {
  if (crash_point.load(std::memory_order_relaxed) == point)
    ::_exit(86);
}
#define AUTHORITY_CRASH(point) crash_after(AuthorityCrashPoint::point)
#else
#define AUTHORITY_CRASH(point) ((void)0)
#endif
// BLOB counters are exactly eight big-endian bytes: no SQLite signed-integer
// truncation, decimal coercion, or floating-point rounding at UINT64_MAX.
constexpr std::array<std::string_view, 4> schema = {
R"(CREATE TABLE revisions(
  digest TEXT PRIMARY KEY NOT NULL CHECK(length(digest)=64 AND digest NOT GLOB '*[^0-9a-f]*'),
  plugin TEXT NOT NULL CHECK(length(plugin) BETWEEN 1 AND 128),
  revision TEXT NOT NULL CHECK(length(revision)=64 AND revision NOT GLOB '*[^0-9a-f]*'),
  policy TEXT NOT NULL CHECK(length(policy)=64 AND policy NOT GLOB '*[^0-9a-f]*'),
  generation BLOB NOT NULL CHECK(length(generation)=8 AND generation>x'0000000000000000')
) STRICT)",
R"(CREATE TABLE grants(
  digest TEXT NOT NULL REFERENCES revisions(digest) ON DELETE CASCADE,
  name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 128),
  definition_generation INTEGER NOT NULL CHECK(definition_generation BETWEEN 1 AND 4294967295),
  definition_digest TEXT NOT NULL CHECK(length(definition_digest)=64 AND definition_digest NOT GLOB '*[^0-9a-f]*'),
  scope TEXT NOT NULL CHECK(length(CAST(scope AS BLOB)) BETWEEN 1 AND 4096),
  required INTEGER NOT NULL CHECK(required IN (0,1)),
  state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 2),
  epoch BLOB NOT NULL CHECK(length(epoch)=8 AND epoch>x'0000000000000000'),
  PRIMARY KEY(digest,name)
) STRICT)",
R"(CREATE TABLE operations(
  digest TEXT NOT NULL,
  name TEXT NOT NULL,
  operation TEXT NOT NULL CHECK(length(operation) BETWEEN 1 AND 128),
  granted INTEGER NOT NULL CHECK(granted IN (0,1)),
  PRIMARY KEY(digest,name,operation),
  FOREIGN KEY(digest,name) REFERENCES grants(digest,name) ON DELETE CASCADE
) STRICT)",
R"(CREATE TABLE authority(
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  plugin TEXT NOT NULL CHECK(length(plugin) BETWEEN 1 AND 128),
  sequence BLOB NOT NULL CHECK(length(sequence)=8),
  high_watermark BLOB NOT NULL CHECK(length(high_watermark)=8 AND high_watermark<=sequence),
  active TEXT REFERENCES revisions(digest) DEFERRABLE INITIALLY DEFERRED,
  candidate TEXT REFERENCES revisions(digest) DEFERRABLE INITIALLY DEFERRED
) STRICT)"};

struct SqlError {};

class Statement {
public:
  Statement(sqlite3 *database, std::string_view sql) {
    if (sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &value_, nullptr) != SQLITE_OK) {
      sqlite3_finalize(value_);
      throw SqlError{};
    }
  }
  ~Statement() { sqlite3_finalize(value_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  void text(int index, std::string_view value) {
    check(sqlite3_bind_text(value_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
  }
  void number(int index, sqlite3_int64 value) { check(sqlite3_bind_int64(value_, index, value)); }
  void counter(int index, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    big_endian::put<std::uint64_t>(bytes, 0, value);
    check(sqlite3_bind_blob(value_, index, bytes.data(), bytes.size(), SQLITE_TRANSIENT));
  }
  void reference(int index, const std::optional<AuthorityRevisionRef> &value) {
    if (value)
      text(index, value->snapshot_digest.view());
    else
      check(sqlite3_bind_null(value_, index));
  }
  bool row() {
    const auto result = sqlite3_step(value_);
    if (result != SQLITE_ROW && result != SQLITE_DONE)
      throw SqlError{};
    return result == SQLITE_ROW;
  }
  void done() { if (row()) throw SqlError{}; }
  bool null(int index) const { return sqlite3_column_type(value_, index) == SQLITE_NULL; }
  std::string_view text(int index) const {
    if (sqlite3_column_type(value_, index) != SQLITE_TEXT)
      throw SqlError{};
    const auto *bytes = sqlite3_column_text(value_, index);
    if (!bytes)
      throw SqlError{};
    return {reinterpret_cast<const char *>(bytes), static_cast<std::size_t>(sqlite3_column_bytes(value_, index))};
  }
  sqlite3_int64 number(int index) const {
    if (sqlite3_column_type(value_, index) != SQLITE_INTEGER)
      throw SqlError{};
    return sqlite3_column_int64(value_, index);
  }
  std::uint64_t counter(int index) const {
    if (sqlite3_column_type(value_, index) != SQLITE_BLOB || sqlite3_column_bytes(value_, index) != 8)
      throw SqlError{};
    const auto *bytes = static_cast<const std::byte *>(sqlite3_column_blob(value_, index));
    if (!bytes)
      throw SqlError{};
    return big_endian::get<std::uint64_t>(std::span(bytes, 8), 0);
  }
private:
  static void check(int result) { if (result != SQLITE_OK) throw SqlError{}; }
  sqlite3_stmt *value_ = nullptr;
};

void execute(sqlite3 *database, const char *sql) {
  if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw SqlError{};
}

class Transaction {
public:
  explicit Transaction(sqlite3 *database) : database_(database) { execute(database_, "BEGIN IMMEDIATE"); }
  ~Transaction() {
    if (!committed_)
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
  }
  void commit() { execute(database_, "COMMIT"); committed_ = true; }
private:
  sqlite3 *database_;
  bool committed_ = false;
};

bool valid_slots(const AuthoritySlots &slots) {
  if (slots.sequence < slots.generation_high_watermark)
    return false;
  for (const auto *ref : {&slots.active, &slots.candidate}) {
    if (*ref && (!canonical_sha256((*ref)->snapshot_digest.view()) ||
        (*ref)->generation == 0 || (*ref)->generation > slots.generation_high_watermark))
      return false;
  }
  return (!slots.candidate || slots.candidate->generation == slots.generation_high_watermark) &&
      (!slots.active || !slots.candidate || slots.active->generation < slots.candidate->generation);
}

std::optional<AuthorityRevisionRef> reference(Statement &row, int digest_index, int generation_index) {
  if (row.null(digest_index)) {
    if (!row.null(generation_index))
      throw SqlError{};
    return {};
  }
  return AuthorityRevisionRef{permissions::Digest(row.text(digest_index)), row.counter(generation_index)};
}

AuthoritySlots slots_unchecked(sqlite3 *database, const permissions::PluginId &plugin) {
  Statement row(database,
      "SELECT a.plugin,a.sequence,a.high_watermark,a.active,r.generation,a.candidate,c.generation,a.singleton "
      "FROM authority a LEFT JOIN revisions r ON r.digest=a.active "
      "LEFT JOIN revisions c ON c.digest=a.candidate");
  if (!row.row() || row.text(0) != plugin.view() || row.number(7) != 1)
    throw SqlError{};
  AuthoritySlots slots{row.counter(1), row.counter(2), reference(row, 3, 4), reference(row, 5, 6)};
  row.done();
  if (!valid_slots(slots))
    throw SqlError{};
  return slots;
}

AuthorityRevisionRef snapshot_reference(const policy::GrantSnapshot &snapshot) {
  if (snapshot.binding.generation == 0 || snapshot.dynamic_grants.size() > 128)
    throw SqlError{};
  definitions::Name previous;
  bool first = true;
  for (const auto &grant : snapshot.dynamic_grants) {
    if (grant.binding != snapshot.binding || !definitions::valid_dynamic_grant_shape(grant) ||
        !definitions::canonical_identifier(grant.request.definition.canonical_name.view()) ||
        grant.request.definition.definition_generation == 0 ||
        !canonical_sha256(grant.request.definition.definition_digest.view()) ||
        (!first && !(previous < grant.request.definition.canonical_name)))
      throw SqlError{};
    previous = grant.request.definition.canonical_name;
    first = false;
  }
  std::vector<std::byte> canonical;
  if (!authority_snapshot_codec::encode_snapshot(snapshot, canonical))
    throw SqlError{};
  return authority_snapshot_codec::reference_for(snapshot, plugins::manifest::sha256_hex(canonical));
}

policy::GrantSnapshot snapshot_unchecked(sqlite3 *database, const AuthorityRevisionRef &expected) {
  Statement revision(database, "SELECT plugin,revision,policy,generation FROM revisions WHERE digest=?1");
  revision.text(1, expected.snapshot_digest.view());
  if (!revision.row())
    throw SqlError{};
  policy::GrantSnapshot snapshot;
  snapshot.binding = {permissions::PluginId(revision.text(0)), permissions::Digest(revision.text(1)),
      permissions::Digest(revision.text(2)), revision.counter(3)};
  revision.done();
  Statement grants(database,
      "SELECT name,definition_generation,definition_digest,scope,required,state,epoch "
      "FROM grants WHERE digest=?1 ORDER BY name");
  grants.text(1, expected.snapshot_digest.view());
  while (grants.row()) {
    if (snapshot.dynamic_grants.size() == 128)
      throw SqlError{};
    const auto definition_generation = grants.number(1);
    const auto required = grants.number(4);
    const auto state = grants.number(5);
    if (definition_generation < 1 || definition_generation > UINT32_MAX ||
        required < 0 || required > 1 || state < 0 || state > 2)
      throw SqlError{};
    definitions::DynamicRevisionGrant grant;
    grant.binding = snapshot.binding;
    grant.request.definition = {definitions::Name(grants.text(0)),
        static_cast<std::uint32_t>(definition_generation), definitions::Digest(grants.text(2))};
    grant.request.scope = definitions::CanonicalScope(grants.text(3));
    grant.request.required = required;
    grant.grant.state = static_cast<permissions::GrantState>(state);
    grant.grant.epoch = grants.counter(6);
    Statement operations(database, "SELECT operation,granted FROM operations WHERE digest=?1 AND name=?2 ORDER BY operation");
    operations.text(1, expected.snapshot_digest.view());
    operations.text(2, grant.request.definition.canonical_name.view());
    while (operations.row()) {
      const auto granted = operations.number(1);
      const definitions::Name operation(operations.text(0));
      if (granted < 0 || granted > 1 || !grant.request.operations.insert(operation) ||
          (granted && !grant.grant.operations.insert(operation)))
        throw SqlError{};
    }
    snapshot.dynamic_grants.push_back(std::move(grant));
  }
  if (snapshot_reference(snapshot) != expected)
    throw SqlError{};
  return snapshot;
}

void insert_snapshot(sqlite3 *database, const permissions::PluginId &plugin,
                      const policy::GrantSnapshot &snapshot) {
  if (snapshot.binding.plugin != plugin)
    throw SqlError{};
  const auto ref = snapshot_reference(snapshot);
  Statement existing(database, "SELECT 1 FROM revisions WHERE digest=?1");
  existing.text(1, ref.snapshot_digest.view());
  if (existing.row()) {
    (void)snapshot_unchecked(database, ref);
    return;
  }
  Statement revision(database, "INSERT INTO revisions VALUES(?1,?2,?3,?4,?5)");
  revision.text(1, ref.snapshot_digest.view());
  revision.text(2, plugin.view());
  revision.text(3, snapshot.binding.revision.view());
  revision.text(4, snapshot.binding.policy_fingerprint.view());
  revision.counter(5, snapshot.binding.generation);
  revision.done();
  for (const auto &grant : snapshot.dynamic_grants) {
    Statement insert(database, "INSERT INTO grants VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
    insert.text(1, ref.snapshot_digest.view());
    insert.text(2, grant.request.definition.canonical_name.view());
    insert.number(3, grant.request.definition.definition_generation);
    insert.text(4, grant.request.definition.definition_digest.view());
    insert.text(5, grant.request.scope.view());
    insert.number(6, grant.request.required);
    insert.number(7, static_cast<std::uint8_t>(grant.grant.state));
    insert.counter(8, grant.grant.epoch);
    insert.done();
    for (const auto &operation : grant.request.operations.values()) {
      Statement op(database, "INSERT INTO operations VALUES(?1,?2,?3,?4)");
      op.text(1, ref.snapshot_digest.view());
      op.text(2, grant.request.definition.canonical_name.view());
      op.text(3, operation.view());
      op.number(4, grant.grant.operations.contains(operation));
      op.done();
    }
  }
}

void write_head(sqlite3 *database, const permissions::PluginId &plugin, const AuthoritySlots &slots) {
  Statement head(database, "INSERT INTO authority VALUES(1,?1,?2,?3,?4,?5) "
      "ON CONFLICT(singleton) DO UPDATE SET sequence=excluded.sequence,high_watermark=excluded.high_watermark,"
      "active=excluded.active,candidate=excluded.candidate");
  head.text(1, plugin.view());
  head.counter(2, slots.sequence);
  head.counter(3, slots.generation_high_watermark);
  head.reference(4, slots.active);
  head.reference(5, slots.candidate);
  head.done();
  for (const auto *ref : {&slots.active, &slots.candidate}) {
    if (*ref && snapshot_unchecked(database, **ref).binding.plugin != plugin)
      throw SqlError{};
  }
  execute(database, "DELETE FROM revisions WHERE digest NOT IN "
      "(SELECT active FROM authority WHERE active IS NOT NULL UNION SELECT candidate FROM authority WHERE candidate IS NOT NULL)");
}
} // namespace

SchemaState schema_state(SqliteAuthorityDatabase &database) {
  try {
    if (!database.intact())
      return SchemaState::invalid;
    Statement tables(database.connection(), "SELECT sql FROM sqlite_schema WHERE sql IS NOT NULL ORDER BY name");
    std::size_t found = 0;
    while (tables.row()) {
      if (std::ranges::find(schema, tables.text(0)) == schema.end() || ++found > schema.size())
        return SchemaState::invalid;
    }
    Statement version(database.connection(), "PRAGMA user_version");
    if (!version.row())
      return SchemaState::invalid;
    const auto value = version.number(0);
    if (found == 0 && value == 0)
      return SchemaState::empty;
    return found == schema.size() && value == 1 ? SchemaState::current : SchemaState::invalid;
  } catch (...) {
    return SchemaState::invalid;
  }
}

bool initialize(SqliteAuthorityDatabase &database, const permissions::PluginId &plugin,
                const AuthoritySlots &slots, std::span<const policy::GrantSnapshot> snapshots) {
  try {
    if (!valid_slots(slots) || snapshots.size() > 2 || !database.intact())
      return false;
    Transaction transaction(database.connection());
    if (schema_state(database) != SchemaState::empty)
      return false;
    for (const auto statement : schema)
      Statement(database.connection(), statement).done();
    for (const auto &snapshot : snapshots)
      insert_snapshot(database.connection(), plugin, snapshot);
    write_head(database.connection(), plugin, slots);
    execute(database.connection(), "PRAGMA user_version=1");
    AUTHORITY_CRASH(before_commit);
    transaction.commit();
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<AuthoritySlots> read_slots(SqliteAuthorityDatabase &database, const permissions::PluginId &plugin) {
  try {
    if (!database.intact() || schema_state(database) != SchemaState::current)
      return {};
    return slots_unchecked(database.connection(), plugin);
  } catch (...) {
    return {};
  }
}

std::optional<policy::GrantSnapshot> load_snapshot(SqliteAuthorityDatabase &database, const AuthorityRevisionRef &reference) {
  try {
    if (!database.intact() || schema_state(database) != SchemaState::current)
      return {};
    return snapshot_unchecked(database.connection(), reference);
  } catch (...) {
    return {};
  }
}

AuthorityMutationResult replace(SqliteAuthorityDatabase &database, const permissions::PluginId &plugin,
    const AuthoritySlots &expected, const AuthoritySlots &next, std::span<const policy::GrantSnapshot> snapshots) {
  if (!valid_slots(expected) || !valid_slots(next) || snapshots.size() > 1 ||
      expected.sequence == UINT64_MAX || next.sequence != expected.sequence + 1 ||
      next.generation_high_watermark < expected.generation_high_watermark ||
      next.generation_high_watermark - expected.generation_high_watermark > 1)
    return AuthorityMutationResult::invalid;
  try {
    if (!database.intact() || schema_state(database) != SchemaState::current)
      return AuthorityMutationResult::io_error;
    Transaction transaction(database.connection());
    AUTHORITY_CRASH(transaction_begin);
    if (slots_unchecked(database.connection(), plugin) != expected)
      return AuthorityMutationResult::stale_sequence;
    for (const auto &snapshot : snapshots)
      insert_snapshot(database.connection(), plugin, snapshot);
    AUTHORITY_CRASH(revision_rows);
    write_head(database.connection(), plugin, next);
    AUTHORITY_CRASH(authority_head);
    AUTHORITY_CRASH(before_commit);
    transaction.commit();
    AUTHORITY_CRASH(after_commit);
    return AuthorityMutationResult::applied;
  } catch (...) {
    return AuthorityMutationResult::io_error;
  }
}

#ifdef OMARCHY_AUTHORITY_STORE_TESTING
void set_crash_point_for_testing(AuthorityCrashPoint point) noexcept {
  crash_point.store(point, std::memory_order_relaxed);
}
void checkpoint_for_testing(AuthorityCrashPoint point) { crash_after(point); }
#endif
#undef AUTHORITY_CRASH

} // namespace omarchy::plugin_runtime::host_session::authority_sql
