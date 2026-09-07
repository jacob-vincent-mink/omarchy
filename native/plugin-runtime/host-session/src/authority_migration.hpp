#pragma once

#include "sqlite_authority_database.hpp"
#include "permission_contract.hpp"

namespace omarchy::plugin_runtime::host_session {
// Called only while holding the existing private authority lifetime lock.
[[nodiscard]] std::unique_ptr<SqliteAuthorityDatabase>
open_authority_database(int root, int lock, std::uint32_t uid,
                        const omarchy::plugins::permissions::PluginId &plugin);
} // namespace omarchy::plugin_runtime::host_session
