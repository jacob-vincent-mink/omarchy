# QEMU lanes

Hardware-backed lifecycle, network-policy, and sandbox-closure lanes that a
desktop host cannot exercise. All are manual/opt-in, NOT part of CTest. They
need KVM, bwrap with user namespaces, a bootable Arch rootfs, and several
minutes. The guest boots with `init=/root/<lane>.sh`, which mounts
proc/sys/dev and runs the lane's driver on ttyS0; the host asserts the emitted
`*`-marker lines.

## QEMU power-loss lane (threat T11)

This directory holds the disposable-VM tooling that exercised the previously
unverified T11 lane — "physical power-loss resurrection/stale-generation" — for
the SQLite authority store. It is NOT part of CTest. It needs KVM, bwrap with
user namespaces, a bootable Arch rootfs, and several minutes.

## Components

- `run_power_loss_trials.sh` — host-side driver: for each trial it reimages a
  fresh authority store, boots the guest in `publish` mode, hard-kills
  (SIGKILL) qemu at a random instant (real power loss, no fsync grace), reboots
  in `verify` mode, and asserts the recovered active generation equals the last
  durably-committed generation. See the invariant note at the top of the file.
- `t11_power_harness.cpp` (in `../tests/support/`) — the guest-side harness.
  `publish N`: performs N atomic publish+promote generations, printing
  `BEGIN-GEN-<n>` before each transaction and `DURABLE-GEN-<n>` only after a
  durable commit (so the host knows the last committed point). `verify`: reopens
  the store and reports `VERIFY-OK-ACTIVE-GEN-<n>`, `VERIFY-CLEAN-NO-ACTIVE`
  (empty/fresh), `VERIFY-POISONED` (fail-closed), or `VERIFY-TORN-UNSAFE` (the
  unsafe case: claims an active authority it cannot fully decode).
- This harness is a build artifact (`omarchy-plugin-t11-harness` CMake target),
  not a committed unit test.

## Methodology

The harness is compiled on the host (Release, same sqlite3/Qt6 as the current
Arch), staged into a disposable rootfs together with its dynamically-linked
libraries, and that rootfs is mkfs'd into a raw ext4 image. Scripts that must
stamp root-owned/setuid files (pacstrap, mkfs.ext4 -d) run inside an
unprivileged bwrap user namespace (root-in-sandbox) since the host has no
passwordless sudo. The guest boots with `init=/path/to/t11.sh` which mounts
proc/sys/dev, creates `/root/auth` mode 0700 (the store's `trusted_directory`
requires exactly 0700), reads `t11mode=publish|verify` from the kernel cmdline,
and runs the harness on ttyS0.

Each observed result: after the SIGKILL power cut, `verify` must report an
active generation strictly equal to the last `DURABLE-GEN-<n>`, never a torn or
half-published grant as a valid authority. The store uses
`PRAGMA synchronous=EXTRA` + rollback-journal mode via a constrained custom VFS
(`/proc/self/fd/<dirfd>`), so a committed transaction's pages are fsync'd to the
host image before `DURABLE-GEN-<n>` is printed; a torn journal page must not
resurrect an earlier grant.

## Reproduce

```sh
# 1. Build the harness (test-artifact target):
cmake --build build/plugin-runtime --target omarchy-plugin-t11-harness

# 2. Bootstrap a disposable Arch rootfs (bwrap user-ns, pacman --root), install
#    base+linux, copy the harness to <stage>/usr/local/bin/t11, copy its ldd deps
#    into <stage>/usr/lib, and drop an init script at <stage>/root/t11.sh that
#    mounts proc/sys/dev, mkdir -p -m 0700 /root/auth, and runs the harness per
#    $T11GENS/$T11SLEEP from the kernel cmdline (t11mode=publish|verify).

# 3. Run the campaign (defaults: 10 trials, 40 gens, 120ms sleep):
T11_IMG=/path/rootfs.img T11_STAGE=/path/stage ./run_power_loss_trials.sh 25
```

## Observed results

See `docs/plugin-security-assessment-results.md` (T11). Summary: 25/25 hard
power-loss trials recovered to a coherent, fully-committed authority whose
active generation exactly matched the last durable commit; zero torn, poisoned,
or unsafe verdicts.

Note: an early harness run appeared to fail at generation 7 with
`SQLITE_CONSTRAINT_CHECK`. That was a harness artifact, not a product defect —
the harness's 64-char expanded revision string used `g`, which is not a hex
character, so the store's own `CHECK(revision NOT GLOB '*[^0-9a-f]*')` correctly
rejected it. The fix (restricting expansion to `0-9a-f`) confirms the schema's
constraint enforcement works as designed.

## Connection-time network policy (threat T08)

The T08 lane — an approved origin reaching private services through DNS,
redirects, or media handling — is enforced by the provider's real
resolver + libcurl path, which the unit test could only fake via
`OMARCHY_NETWORK_MEDIA_TESTING`. This QEMU lane supplies the controlled
local-network laboratory the product docs called for.

Components:
- `t08/t08.sh` — guest init: brings up one public (1.1.1.1) and one RFC1918
  private (10.99.0.1) loopback address, writes an `/etc/hosts` that resolves
  approved names to the public IP and leak names to the private IP, and points
  the provider's TLS trust at a freshly generated CA.
- `t08/t08_test.py` — guest driver: runs the real provider (built WITHOUT the
  testing short-circuits) over its stdin control fd and asserts the six
  product mitigations, finally printing `T08-SUMMARY`/`T08-DRIVER-EXIT`.
- `run_network_policy.sh` — host driver: builds the e2e-peer target, stages
  provider + driver into the rootfs, reimages, boots, asserts 6/6 + exit 0.

Reproduce:
```sh
cmake --build build/plugin-runtime --target omarchy-plugin-network-media-provider-e2e-peer
T08_BUILD=build/plugin-runtime T08_STAGE=/path/stage T08_IMG=/path/rootfs_t08.img \
  ./run_network_policy.sh
```

## Sandbox closure as real root (threat T05)

The T05 lane — a hostile worker reaching host files, session bus, network,
native modules, or inherited descriptors — is the bubbleswrap closure built in
`contracts/sandbox/src/policy.cpp`. Its *closure mechanism* (namespace/mount/
capability semantics) can only be answered as real root; a QEMU VM boots as
root and the probe worker runs inside the product's exact bwrap argv.

Components:
- `t05/probe_worker.c` — hostile C worker (glibc only) that attempts each
  escape and prints `RESULT-<name>=PASS|FAIL` plus `OVERALL`.
- `t05/t05.sh` — guest init: stashes a root-only secret, then execs the probe
  worker inside the reusable product bwrap argv (path-based fd substitutions;
  the systemd scope + launcher fd/seccomp wiring is covered by bridge tests).
- `run_sandbox_closure.sh` — host driver: compiles the probe worker, requires a
  REAL bubblewrap in the staging rootfs, reimages, boots, asserts 10/10 + PASS.

Prerequisites: the staging rootfs must have the real `bubblewrap` installed in
`/usr/bin/bwrap` (not the placeholder `#!/bin/sh exit 0`), a host `gcc`, and
KVM. Reproduce:
```sh
T05_STAGE=/path/stage-with-real-bwrap T05_IMG=/path/rootfs_t05.img \
  ./run_sandbox_closure.sh
```
