#!/bin/bash
# T05 sandbox-closure harness (guest init) -- QEMU root-lifecycle lane.
#
# Boots as real root, stashes host-only secret files, brings up lab loopback
# addresses, then runs the product's bubbleswrap closure (the exact argv from
# build_plan_for_worker in contracts/sandbox policy.cpp) around the probe
# worker, as root, and lets the worker's RESULT-* lines flow to the serial log
# for the host driver to assert.
#
# Path-based substitutions only, so the mount/PID/user/net namespace closure is
# byte-for-byte the product's; the systemd transient-scope and launcher fd
# wiring (status/barrier/seccomp/ro-bind-fd) is intentionally not plumbed here
# and is covered by the bridge unit tests.
set -u
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || mount -t tmpfs tmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null || true
hostname t05 2>/dev/null || true

ip link set lo up
ip addr add 1.1.1.1/32 dev lo 2>/dev/null || true
ip addr add 10.99.0.1/32 dev lo 2>/dev/null || true

# Host-only secrets the worker must never reach.
mkdir -p /root
echo "TOP-SECRET-HOST" > /root/host-secret.txt
chmod 0600 /root/host-secret.txt

# The probe worker is exec'd as /runtime/worker inside the closure.
echo "SHELL: bwrap version: $(/usr/bin/bwrap --version)"

# Closures for the fd bindings that the launcher normally provides over the
# control sockets: the revision source (ro, /plugin) and private state (rw,
# /state) and the writable result sink.
mkdir -p /plugin_src_ro /state_host
echo "plugin-placeholder" > /plugin_src_ro/fixture
chmod 0755 /runtime/worker 2>/dev/null || true

# ─── the product bwrap argv, path-bind substitutions ────────────────────────
# (see contracts/sandbox/src/policy.cpp build_plan_for_worker)
set +e
/usr/bin/bwrap \
  --unshare-user --unshare-pid --unshare-ipc --unshare-uts --unshare-net --unshare-cgroup \
  --disable-userns --assert-userns-disabled \
  --uid 0 --gid 0 --new-session --die-with-parent --as-pid-1 \
  --cap-drop ALL --hostname omarchy-plugin --clearenv \
  --setenv HOME /home/plugin --setenv LANG C.UTF-8 --setenv LC_ALL C.UTF-8 \
  --setenv PATH /runtime --setenv PWD /plugin --setenv QT_QPA_PLATFORM offscreen \
  --setenv QT_QUICK_CONTROLS_STYLE Basic --setenv QSG_RHI_BACKEND software \
  --setenv XDG_CACHE_HOME /tmp/cache --setenv XDG_CONFIG_HOME /state/config \
  --setenv XDG_DATA_HOME /state/data --setenv XDG_RUNTIME_DIR /run/plugin \
  --proc /proc --dev /dev \
  --dir /usr --ro-bind /usr/lib /usr/lib --symlink usr/lib /lib --symlink usr/lib /lib64 \
  --dir /runtime \
  --ro-bind /usr/bin/probe_worker /runtime/worker \
  --ro-bind /plugin_src_ro /plugin \
  --bind /state_host /state \
  --tmpfs /tmp --dir /tmp/cache \
  --tmpfs /run --dir /run/plugin --chmod 0700 /run/plugin \
  --tmpfs /home --dir /home/plugin --chmod 0700 /home/plugin \
  --chdir /plugin \
  -- /runtime/worker
bwrap_status=$?
echo "SANDBOX-BWRAP-EXIT=$bwrap_status"
echo "T05-DONE"
sync
poweroff -f 2>/dev/null || /sbin/poweroff -f 2>/dev/null || true
exit 0
