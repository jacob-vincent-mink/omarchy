#pragma once

#include "authority_store.hpp"
#include "sqlite_authority_database.hpp"

#include <span>

namespace omarchy::plugin_runtime::host_session::authority_sql {

enum class SchemaState { empty, current, invalid };
#ifdef OMARCHY_AUTHORITY_STORE_TESTING
void set_crash_point_for_testing(AuthorityCrashPoint point) noexcept;
void checkpoint_for_testing(AuthorityCrashPoint point);
#endif
[[nodiscard]] SchemaState schema_state(SqliteAuthorityDatabase &database);

// Only the lifetime-locked authority owner may call these functions. Import
// receives already-validated legacy state; it never manufactures consent.
[[nodiscard]] bool initialize(SqliteAuthorityDatabase &database,
                              const permissions::PluginId &plugin,
                              const AuthoritySlots &slots,
                              std::span<const policy::GrantSnapshot> snapshots);
[[nodiscard]] std::optional<AuthoritySlots>
read_slots(SqliteAuthorityDatabase &database, const permissions::PluginId &plugin);
[[nodiscard]] std::optional<policy::GrantSnapshot>
load_snapshot(SqliteAuthorityDatabase &database,
              const AuthorityRevisionRef &reference);

// Compare the complete reviewed preimage inside BEGIN IMMEDIATE. Admission
// fencing/draining must already have happened when replacing active authority.
[[nodiscard]] AuthorityMutationResult
replace(SqliteAuthorityDatabase &database, const permissions::PluginId &plugin,
        const AuthoritySlots &expected, const AuthoritySlots &next,
        std::span<const policy::GrantSnapshot> snapshots = {});

} // namespace omarchy::plugin_runtime::host_session::authority_sql
