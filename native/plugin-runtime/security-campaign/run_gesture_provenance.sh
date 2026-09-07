#!/bin/bash
#
# T07 gesture-provenance interactive lane.
#
# Verifies, on a real compositor display, that a genuine pointer/touch gesture
# reaching a projected plugin surface ingresses as
#   decision=accepted ... trusted-physical=true input-sequence=<n>
# on the surface-host 'host-input' audit line, and that a synthesized
# (non-spontaneous) gesture would carry trusted-physical=false and be rejected.
# The one-use exact-binding semantics of eligibility consumption are covered by
# the automated gesture_eligibility/gesture_intent unit tests; the unique
# incremental value here is the live real-gesture provenance demonstration.
#
# This is a MANUAL lane: it needs a visible Wayland/X session and a human
# gesture on the projected surface window. It is intentionally NOT part of
# ./run_campaign.sh. Run it from the repo root while a real session is active:
#
#   OMARCHY_T07_LIVE=1 ./native/plugin-runtime/security-campaign/run_gesture_provenance.sh
#
# It runs the same packaged-worker bridge as the plugin-neutral-surfaces-real-bwrap
# lane (OMARCHY_REQUIRE_PACKAGED_WORKER_TEST), so it needs the packaged worker at
# /usr/lib/omarchy/plugin-security/0.1.0/bin/omarchy-plugin-qml-worker to be current
# and the real systemd user session (DBus user bus) the production supervisor uses.
# Refresh the packaged worker with /tmp/refresh_packaged_worker.sh (one sudo step)
# if that bridge is stale.
#
# Exit codes: 0 = verified (accepted+trusted-physical=true observed),
#             1 = failed, 77 = skipped (env not set / no live marker).

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/plugin-runtime}"
export OMARCHY_T07_LIVE="${OMARCHY_T07_LIVE:-1}"
export OMARCHY_REQUIRE_PACKAGED_WORKER_TEST=1

if [[ ${OMARCHY_T07_LIVE:-0} != 1 && ${OMARCHY_T07_LIVE:-0} != true ]]; then
  echo "T07-DRIVER-SKIP set OMARCHY_T07_LIVE=1 to run the interactive lane" >&2
  exit 77
fi

echo "T07-DRIVER building surface-service test..." >&2
cmake --build "$BUILD_DIR" --target omarchy-plugin-surface-service-test >&2

# The interactive run needs the real display platform, not offscreen.
unset QT_QPA_PLATFORM 2>/dev/null || true

echo "T07-DRIVER launching interactive lane (gesture on the projected surface window)..." >&2
# Run the test binary directly (not via ctest --output-on-failure, which
# suppresses the harness's captured stderr on a successful exit) so the
# T07-PASS / T07-FAIL verdict lines always reach the log.
"$BUILD_DIR/bridge/omarchy-plugin-surface-service-test" --t07-live-only \
  > /tmp/t07_live_driver.log 2>&1

if grep -q "T07-PASS" /tmp/t07_live_driver.log; then
  echo "T07-DRIVER-VERDICT=PASS (accepted+trusted-physical=true observed)"
  exit 0
fi
if grep -q "T07-SKIP" /tmp/t07_live_driver.log; then
  echo "T07-DRIVER-VERDICT=SKIPPED"
  exit 77
fi
echo "T07-DRIVER-VERDICT=FAIL (no trusted accepted gesture observed)" >&2
exit 1
