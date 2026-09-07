#!/bin/bash
# QEMU sandbox-closure (T05) trial driver.
#
# Threat-model lane T05 ("worker reaches host files, session bus, network,
# native modules, or inherited descriptors") is enforced by the bubbleswrap
# closure built in contracts/sandbox policy.cpp. The packaged end-to-end bridge
# (systemd transient scope + the full Qt worker + seccomp filter) still needs a
# systemd session and the packaged runtime, but the *closure mechanism itself*
# -- the namespace + mount + capability semantics that actually stop an escape
# -- is a real-root, QEMU-unique question that no unprivileged host can answer.
#
# This driver boots the guest as real root and runs a hostile probe worker
# inside the product's exact bwrap argv (path-based fd substitutions), asserting
# every escape the forensics column names:
#   host files (shadow + a root-only stash), /proc escape to host root, network
#   reach, host native modules/executables, a mount/remount to widen the
#   closure, and inherited descriptors. The single writable host-backed path
#   (/state) must accept writes; the ro plugin/revision mount must not.
#
# Manual/opt-in lane, NOT part of CTest: requires a bootable Arch rootfs staging
# dir with the REAL bubblewrap installed in /usr/bin/bwrap (do not leave the
# placeholder dummy), a host gcc, KVM, and bwrap with user namespaces.
# See docs/plugin-security-assessment-results.md (T05).
set -uo pipefail
cd "$(dirname "$0")"

STAGE=${T05_STAGE:-../rootfs}
IMG=${T05_IMG:-rootfs_t05.img}
KERNEL=${T05_KERNEL:-/boot/vmlinuz-linux}
INITRD=${T05_INITRD:-/boot/initramfs-linux.img}
GUEST_INIT=${T05_GUEST_INIT:-/root/t05.sh}

# Resolve to absolute host paths: inside the user-namespace staging the host
# root is ro-bound at /, so absolute paths resolve unchanged there.
STAGE=$(realpath "$STAGE")
IMG=$(realpath -m "$IMG")

require() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 2; }; }
require qemu-system-x86_64 bwrap mkfs.ext4 gcc

[[ -d $STAGE ]] || { echo "need \$T05_STAGE dir (Arch rootfs with real bubblewrap)" >&2; exit 2; }
[[ -f $STAGE/usr/bin/bwrap ]] || { echo "stage missing /usr/bin/bwrap" >&2; exit 2; }

# Compile the hostile probe worker (glibc only, so it runs with /usr/lib staged).
gcc -O2 -Wall -o probe_worker t05/probe_worker.c || { echo "probe_worker build failed" >&2; exit 3; }

qemu_common=(-machine q35,accel=kvm -cpu host -smp 4 -m 2048
  -drive "file=$IMG,if=virtio,format=raw"
  -kernel "$KERNEL" -initrd "$INITRD"
  -display none -monitor none -no-reboot)

# Bind the containing dirs of STAGE/IMG writable so the staging step (which
# ro-binds all of /) can write the image and drop files into the not-yet-booted
# staging tree.
WBINDS="--bind /home /home --bind /tmp /tmp"
log=t05_boot.log
rm -f "$log"
bwrap --die-with-parent --unshare-all --uid 0 --gid 0 --cap-add ALL \
  --ro-bind / / \
  $WBINDS \
  --bind "$(pwd)" /srv --proc /proc --dev /dev --setenv TERM dumb \
  /bin/bash -c "
    install -m 0755 /srv/t05/t05.sh "$STAGE"/root/t05.sh
    install -m 0755 /srv/probe_worker "$STAGE"/usr/bin/probe_worker
    chown 0:0 "$STAGE"/root/t05.sh "$STAGE"/usr/bin/probe_worker
    rm -f "$IMG"; truncate -s 3G "$IMG"; chown 0:0 "$IMG"
    mkfs.ext4 -q -F -d "$STAGE" "$IMG" 
" || { echo "image build failed" >&2; exit 3; }

timeout 120 qemu-system-x86_64 "${qemu_common[@]}" \
  -append "root=/dev/vda rw console=ttyS0 init=$GUEST_INIT" \
  -serial "file:$log"
rc=$?

echo
echo "=== T05 sandbox-closure results ==="
grep -aE "RESULT-|OBS-|OVERALL|SANDBOX-BWRAP-EXIT" "$log"
fails=$(grep -acE "RESULT-[A-Za-z0-9_-]+=FAIL" "$log")
passes=$(grep -acE "RESULT-[A-Za-z0-9_-]+=PASS" "$log")
overall=$(grep -aoE "OVERALL=(PASS|FAIL)" "$log" | tail -1 | cut -d= -f2)
verdict=FAIL
if [[ ${fails:-1} == 0 && ${passes:-0} -ge 10 && $overall == PASS ]]; then
  verdict=PASS
fi
echo "T05-DRIVER-VERDICT=$verdict (passes=$passes fails=$fails overall=$overall rc=$rc)"
[[ $verdict == PASS ]]
