#!/bin/bash
# QEMU power-loss (T11) trial driver for the SQLite authority store.
#
# Threat-model lane T11 ("physical power-loss resurrection of stale authority")
# was previously marked unverified because a real power cut cannot be induced on
# the development host. This driver supplies that missing lane on a disposable
# QEMU VM: it boots the guest in "publish" mode, hard-kills (SIGKILL) the qemu
# process at a random instant mid-publish (equivalent to yanking power / hitting
# reset -- no grace for fsync), then reboots in "verify" mode and asserts the
# persisted authority is a fully coherent, durably-committed generation.
#
# Invariant asserted per trial (see the verify verdict in t11_power_harness.cpp):
#   the recovered active generation MUST equal the last generation for which a
#   committed fsync (DURABLE-GEN-<n>) was reported before the kill. A torn or
#   half-published surface must never surface as a valid active authority
#   (VERIFY-TORN-UNSAFE / VERIFY-POISONED are fail-closed and count as failures
#   to the target invariant, since the store must either recover to the last
#   durable commit or report no active authority at all).
#
# This is a manual/opt-in lane, NOT part of CTest: it requires a bootable
# Arch rootfs image, KVM, bwrap with user namespaces, and several minutes.
# See docs/plugin-security-assessment-results.md (T11) for methodology and
# observed results.
set -uo pipefail

cd "$(dirname "$0")"

# --- Configuration -----------------------------------------------------------
IMG=${T11_IMG:-rootfs.img}          # raw ext4 image (built from a rootfs staging dir)
STAGE=${T11_STAGE:-rootfs}         # staging dir mkfs'd into $IMG before each trial
KERNEL=${T11_KERNEL:-/boot/vmlinuz-linux}
INITRD=${T11_INITRD:-/boot/initramfs-linux.img}
GENS=${T11_GENS:-40}
SLEEP_MS=${T11_SLEEP_MS:-120}
TRIALS=${1:-10}
# Guest layout produced by the init script that boots into t11_power_harness.
GUEST_INIT=${T11_GUEST_INIT:-/root/t11.sh}   # mounts /proc /sys /dev, runs harness
GUEST_HARNESS=${T11_GUEST_HARNESS:-/usr/local/bin/t11}

require() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 2; }; }
require qemu-system-x86_64 bwrap mkfs.ext4 e2fsck

[[ -f "$IMG" || -d "$STAGE" ]] || { echo "need \$T11_IMG or \$T11_STAGE" >&2; exit 2; }

qemu_common=(-machine q35,accel=kvm -cpu host -smp 4 -m 2048
  -drive "file=$IMG,if=virtio,format=raw"
  -kernel "$KERNEL" -initrd "$INITRD"
  -display none -monitor none -no-reboot)

reimage() {
  # Fresh, empty authority store per trial: rebuild the image from the staging
  # dir (which has no populated /root/auth yet). Must run in a user namespace so
  # root-owned files and setuid bits can be stamped without host root.
  bwrap --die-with-parent --unshare-all --uid 0 --gid 0 --cap-add ALL \
    --ro-bind / / --bind "$(pwd)" /srv --proc /proc --dev /dev \
    --setenv TERM dumb /bin/bash -c \
    "rm -f /srv/$IMG; truncate -s 3G /srv/$IMG; chown 0:0 /srv/$IMG; \
     mkfs.ext4 -q -F -d /srv/$STAGE /srv/$IMG; e2fsck -f -y /srv/$IMG >/dev/null 2>&1"
}

boot_publish() {
  local log=$1
  timeout 60 qemu-system-x86_64 "${qemu_common[@]}" \
    -append "root=/dev/vda rw console=ttyS0 init=$GUEST_INIT t11mode=publish T11GENS=$GENS T11SLEEP=$SLEEP_MS" \
    -serial "file:$log" &
  QPID=$!
}

boot_verify() {
  local log=$1
  timeout 40 qemu-system-x86_64 "${qemu_common[@]}" \
    -append "root=/dev/vda rw console=ttyS0 init=$GUEST_INIT t11mode=verify" \
    -serial "file:$log"
}

wait_for_marker() {
  local log=$1 marker=$2 tries=${3:-200}
  for ((i=0; i<tries; i++)); do
    grep -aqE "$marker" "$log" 2>/dev/null && return 0
    sleep 0.05
  done
  return 1
}

declare -A results
fails=0
for t in $(seq 1 "$TRIALS"); do
  pblog="pb_${t}.log"; vlog="vf_${t}.log"
  rm -f "$pblog" "$vlog"
  target=$((RANDOM % (GENS - 4) + 3))   # kill during a gen in [3, GENS-1]

  reimage   # independent, fresh authority per trial

  boot_publish "$pblog"
  QPID=$!
  if ! wait_for_marker "$pblog" "BEGIN-GEN-${target}"; then
    echo "trial $t: FAIL -- never reached BEGIN-GEN-$target"
    kill -9 "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    fails=$((fails+1)); continue
  fi
  sleep 0.06   # let the previous commit land / straddle into the next write
  kill -9 "$QPID" 2>/dev/null            # hard power loss (no grace for fsync)
  wait "$QPID" 2>/dev/null

  boot_verify "$vlog"
  wait_for_marker "$vlog" "T11-VERIFY-DONE" 120

  last_begin=$(grep -aoE "BEGIN-GEN-[0-9]+" "$pblog" | tail -1 | grep -oE "[0-9]+")
  last_durable=$(grep -aoE "DURABLE-GEN-[0-9]+" "$pblog" | tail -1 | grep -oE "[0-9]+")
  verify_line=$(grep -aoE "VERIFY-OK-ACTIVE-GEN-[0-9]+|VERIFY-CLEAN-NO-ACTIVE|VERIFY-POISONED|VERIFY-TORN-UNSAFE" "$vlog" | tail -1)
  slots_line=$(grep -aoE "slots seq=[0-9]+ hw=[0-9]+ active_gen=-?[0-9]+ cand_gen=-?[0-9]+" "$vlog" | tail -1)
  verify_active=$(grep -aoE "VERIFY-OK-ACTIVE-GEN-[0-9]+" "$vlog" | tail -1 | grep -oE "[0-9]+$")

  echo "trial $t: kill@gen=$target last_begin=$last_begin last_durable=${last_durable:-0} [$verify_line] {$slots_line}"

  verdict=unknown
  case "$verify_line" in
    *VERIFY-TORN-UNSAFE*) verdict=unsafe;;
    *VERIFY-POISONED*)    verdict=poison;;
    *VERIFY-OK-ACTIVE-GEN-*) verdict=ok;;
    *VERIFY-CLEAN-NO-ACTIVE*) verdict=clean;;
  esac
  if [[ $verdict == ok && -n ${verify_active:-} && -n ${last_durable:-} && $verify_active == "$last_durable" ]]; then
    results[ok]=$(( ${results[ok]:-0} + 1 ))
  else
    echo "  -> record mismatch or unexpected verdict (verify_active=${verify_active:-?} last_durable=$last_durable)"
    fails=$((fails+1)); results[$verdict]=$(( ${results[$verdict]:-0} + 1 ))
  fi
done

echo
echo "=== POWER-LOSS TRIAL SUMMARY ($TRIALS) ==="
for k in ok clean poison unsafe unknown; do
  echo "  $k: ${results[$k]:-0}"
done
echo "  FAILURES: $fails"
(( fails == 0 ))
