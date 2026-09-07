#pragma once

#include "omarchy/plugin_runtime/grant_snapshot.hpp"
#include "manifest_contract.hpp"
#include "omarchy/plugin/wire/permission_snapshot.hpp"

#include <optional>

namespace omarchy::plugin_runtime::host_session {

// Structural identity also accepts durably denied/revoked required grants:
// those remain reviewable authority, but must never produce a worker payload.
[[nodiscard]] bool permission_snapshot_matches_manifest(
    const plugins::manifest::ManifestV2 &manifest,
    const policy::GrantSnapshot &grants) noexcept;

// Projects one already-authoritative activation snapshot into the manifest's
// canonical request index space. Failure is transactional and produces no
// payload for the worker.
[[nodiscard]] std::optional<plugin::wire::permission_snapshot::PermissionSnapshot>
project_permission_snapshot(const plugins::manifest::ManifestV2 &manifest,
                            const policy::GrantSnapshot &grants) noexcept;

} // namespace omarchy::plugin_runtime::host_session
