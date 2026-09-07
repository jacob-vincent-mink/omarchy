#pragma once

#include "authority_store.hpp"
#include "sqlite_authority_database.hpp"
#include <sqlite3.h>

namespace omarchy::plugin_runtime::host_session {
class AuthorityStoreTestAccess final {
public:
  static bool execute(AuthorityStore &store, const char *sql) {
    return sqlite3_exec(store.database_->connection(), sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  }
  static int count(AuthorityStore &store, const char *sql) {
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(store.database_->connection(), sql, -1, &statement, nullptr) != SQLITE_OK)
      return -1;
    const auto result = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement);
    return result;
  }
  static void set_mutation_epoch(AuthorityStore &store,
                                 std::uint64_t epoch) noexcept {
    store.mutation_epoch_ = epoch;
  }
  [[nodiscard]] static AuthorityMutationResult
  replace_active(AuthorityStore &store, const policy::GrantSnapshot &snapshot) {
    return store.replace_active_for_testing(snapshot);
  }
  static void crash_at(AuthorityCrashPoint point) noexcept {
    AuthorityStore::set_crash_point_for_testing(point);
  }
  [[nodiscard]] static AuthorityRevocationResult
  revoke_active(AuthorityStore &store,
                const definitions::CapabilityReference &capability,
                std::uint64_t expected_sequence,
                AuthorityFenceObserver *observer = nullptr) {
    return store.revoke_active(capability, expected_sequence, observer);
  }
};
} // namespace omarchy::plugin_runtime::host_session
