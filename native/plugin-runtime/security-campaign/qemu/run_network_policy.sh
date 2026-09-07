#!/bin/bash
# QEMU connection-time network-policy (T08) trial driver.
#
# Threat-model lane T08 ("approved origin reaches private services through DNS,
# redirects, or media handling") was previously marked unverified because its
# connection-time policy enforcement lives in the real resolver + libcurl path
# (the unit test fakes everything through OMARCHY_NETWORK_MEDIA_TESTING). This
# driver replaces the missing "controlled local network laboratory" with an
# in-guest one: a QEMU VM whose loopback carries one "public" (1.1.1.1) and one
# RFC1918 "private" (10.99.0.1) address, an /etc/hosts that resolves approved
# names to the public IP and leak names to the private IP, a stub DNS-free
# resolver setup, and a private-netns-style listener that must stay silent.
#
# It boots the real network_media_provider (compiled WITHOUT the testing
# short-circuits, driven over stdin fd 0) against this lab and asserts the
# product's own mitigations hold end-to-end:
#   positive control:      approved public HTTPS fetch succeeds and mints only
#                          the public media handle (wildcard skips the private)
#   concrete pointer:      a *.url handle pointed at a private address is
#                          rejected (media-source-invalid)
#   redirect:              a 3xx redirect is refused (redirect-rejected)
#   DNS-to-private:        a fetch whose DNS resolves to the private IP is
#                          refused at connect time (network-failed)
#   invariant:             zero bytes ever reach the private listener
#
# Manual/opt-in lane, NOT part of CTest: requires a bootable Arch rootfs staging
# dir (curl, python3, openssl, iproute2 and the provider's host .so deps staged),
# a host build of omarchy-plugin-network-media-provider-e2e-peer, KVM, and bwrap
# with user namespaces. See docs/plugin-security-assessment-results.md (T08).
set -uo pipefail
cd "$(dirname "$0")"

BUILD=${1:-${T08_BUILD:-}}
STAGE=${T08_STAGE:-rootfs}
IMG=${T08_IMG:-rootfs_t08.img}
KERNEL=${T08_KERNEL:-/boot/vmlinuz-linux}
INITRD=${T08_INITRD:-/boot/initramfs-linux.img}
GUEST_INIT=${T08_GUEST_INIT:-/root/t08.sh}

# Resolve to absolute host paths: inside the user-namespace staging the host
# root is ro-bound at /, so absolute paths resolve unchanged there.
STAGE=$(realpath "$STAGE")
IMG=$(realpath -m "$IMG")

require() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 2; }; }
require qemu-system-x86_64 bwrap mkfs.ext4

[[ -n $BUILD ]] || { echo "usage: $0 BUILD_DIRECTORY" >&2; exit 2; }
[[ -d $STAGE ]] || { echo "need \$T08_STAGE dir (Arch rootfs with curl/python/openssl/iproute2)" >&2; exit 2; }

E2E=$BUILD/provider-host/omarchy-plugin-network-media-provider-e2e-peer
cmake --build "$BUILD" --target omarchy-plugin-network-media-provider-e2e-peer >/dev/null 2>&1 \
  || cmake --build "$BUILD" --target omarchy-plugin-network-media-provider-e2e-peer
[[ -x $E2E ]] || { echo "e2e-peer not built: $E2E" >&2; exit 2; }

qemu_common=(-machine q35,accel=kvm -cpu host -smp 4 -m 2048
  -drive "file=$IMG,if=virtio,format=raw"
  -kernel "$KERNEL" -initrd "$INITRD"
  -display none -monitor none -no-reboot)

log=t08_boot.log
rm -f "$log"
# Bind the containing dirs of STAGE/IMG writable so the staging step (which
# ro-binds all of /) can write the image and drop files into the not-yet-booted
# staging tree.
WBINDS="--bind /home /home --bind /tmp /tmp"
# Build a fresh image: stage the e2e provider + guest driver, then mkfs.
# Inside the user namespace the host root is ro-bound at /, so $E2E (an
# absolute host path) resolves unchanged and the repo lane files live at
# /srv/t08/.
bwrap --die-with-parent --unshare-all --uid 0 --gid 0 --cap-add ALL \
  --ro-bind / / \
  $WBINDS \
  --bind "$(pwd)" /srv --proc /proc --dev /dev --setenv TERM dumb \
  /bin/bash -c "
        ${E2E:+install -m 0755 '$E2E' "$STAGE"/usr/bin/netprov}
    install -m 0755 /srv/t08/t08.sh "$STAGE"/root/t08.sh
    install -m 0755 /srv/t08/t08_test.py "$STAGE"/root/t08_test.py
    chown 0:0 "$STAGE"/usr/bin/netprov "$STAGE"/root/t08.sh "$STAGE"/root/t08_test.py
    rm -f "$IMG"; truncate -s 3G "$IMG"; chown 0:0 "$IMG"
    mkfs.ext4 -q -F -d "$STAGE" "$IMG" 
" || { echo "image build failed" >&2; exit 3; }

timeout 120 qemu-system-x86_64 "${qemu_common[@]}" \
  -append "root=/dev/vda rw console=ttyS0 init=$GUEST_INIT" \
  -serial "file:$log"
rc=$?

echo
echo "=== T08 network-policy results ==="
grep -aE "PASS |FAIL |T08-SUMMARY|T08-DRIVER-EXIT" "$log"
summary=$(grep -aoE "T08-SUMMARY total=[0-9]+ passed=[0-9]+ failed=[0-9]+" "$log" | tail -1)
driver_exit=$(awk -F= '/T08-DRIVER-EXIT=/{v=$2} END{print v+0}' "$log")
passed=$(echo "$summary" | grep -oE "passed=[0-9]+" | grep -oE "[0-9]+")
failed=$(echo "$summary" | grep -oE "failed=[0-9]+" | grep -oE "[0-9]+")
verdict=FAIL
if [[ ${failed:-1} == 0 && ${passed:-0} -ge 6 && ${driver_exit:-1} == 0 ]]; then
  verdict=PASS
fi
echo "T08-DRIVER-VERDICT=$verdict (passed=$passed failed=$failed driver_exit=$driver_exit rc=$rc)"
[[ $verdict == PASS ]]
