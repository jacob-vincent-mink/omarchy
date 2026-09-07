# Plugin runtime security assessment results

Assessment baseline: `cb29aa3504b56195a73cf5741bf9124f0e97e043`. This is the
execution result for the black-box and white-box testing lanes left **pending**
in [plugin-security-threat-model.md](plugin-security-threat-model.md). It
records the agent testing work actually performed, new regression tests added,
the CVSS v4.0 disposition for each hypothesis, and the patch decision.

Scope mirrors the parent threat model: the opt-in schema-v2 runtime under
`native/plugin-runtime/`. Testing was performed in an isolated local build
(Release, BUILD_TESTING=ON) plus a separate Clang/ASan fuzz build. No live
authority, installed plugin, host configuration, or real credential was
touched.

The connection-time network-policy (T08) and sandbox-closure (T05) lanes were
subsequently executed end-to-end on QEMU (below) and are no longer unverified.
The compositor gesture-provenance lane (T07) and root-owned package lifecycle
repeat remain **unverified** exactly as before; this document does not relabel
them.

## What was executed

### Baselines

- Reference build: `build/plugin-runtime` (GCC, Release, `-Werror`). Full suite before changes: 82 tests, 81 pass + 1 opt-in skip (`plugin-neutral-surfaces-real-bwrap`).
- Fuzz build: `build/plugin-runtime-fuzz` (Clang 22, ASan/UBSan, `PLUGIN_SECURITY_BUILD_FUZZERS=ON`).

### Black-box lane (documented interfaces, no implementation source)

Ran the project's own bounded libFuzzer targets, which feed only the public
untrusted-parse entry points (the same entry points a remote endpooint or
malicious worker reaches) and stop on any sanitizer finding:

| Target | Boundary | Executions | Result |
| --- | --- | --- | --- |
| `omarchy-plugin-envelope-fuzzer` | T03 wire envelope decode (`decode_packet`) | 263,011,757 | no crash / no sanitizer hit |
| `omarchy-plugin-manifest-fuzzer` | T01 v2 manifest parse (`parse_manifest_v2`), seeded with a valid fixture | 15,136,028 (8,301,852 raw + 6,834,176 seeded) | no crash / no sanitizer hit |
| `omarchy-plugin-render-fuzzer` | T06 surface frame/render message decode (all seven decoders) | 420,116,751 | no crash / no sanitizer hit |

Total ≈ **698M** fuzzed executions across the three reachable parse
boundaries, all ASan/UBSan-clean. `envelope` and `render` reject on the fixed
magic/version/header fields (steady low coverage); `manifest` reached the
descriptor/JSON logic (58/183 counters) once seeded. No input produced a
crash, sanitizer report, or out-of-bounds access in the trusted host process.

### White-box lane (authorizing and effect-time source)

Reviewed the authorizing/effect-time code for every pure-C++ P1 boundary:

- **T01** `discovery/src/revision_ingress.cpp`: strict ustar only, no links/specials, bounded paths/components/depth, `O_NOFOLLOW`+identity re-checks after every open, `renameat2(RENAME_NOREPLACE)`, cancel-safe staging recovery, checksum + post-extract metadata pinning. Defenses match the threat model.
- **T03** `host-session/src/structured_broker.cpp`, `contracts/wire/src/envelope.cpp`, `broker/src/broker_schema.cpp`: private origin stamp, exact binding+session-nonce match, monotonic correlation (replay rejected), message-type/length caps, fail-stop on audit failure.
- **T06** `contracts/surface/src/frame_transport.cpp`, `shared_layout.cpp`: checked multiplication/`slot_base` range-limit, `region_fits` bounds before every access, fixed two-slot double-buffer, seq double-check around the pixel copy.
- **T09** `provider-host/src/provider_catalog.cpp`: root-owned profile/executable pinning, `open_secure_path` (no symlink/setid, world-write excluded), digest match before use.
- **T11** `host-session/src/sqlite_authority_database.cpp`: transactional compare-and-publish, poison-on-ambiguity, cutover stamp (source-confirmed), plus a QEMU hard-power-loss campaign below.
- **T13** `providers/src/private_storage_backend.cpp`: strict key grammar, `O_NOFOLLOW`, `nlink==1` checks, temp+rename with fsync, per-item and total quota.

No confirmed logic flaw was established by source review in these boundaries.
Launcher tests were re-run five consecutive times without reproducing the
documented intermittent descriptor-injection failure (that item stays
unresolved/unconfirmed, not fixed).

## T11 exercised on QEMU (hard power-loss)

The T11 lane — physical power-loss durability of the SQLite authority store —
was previously unverified because a real power cut cannot be induced on the
development host. A disposable QEMU/KVM VM supplies that missing lane:

- A guest harness (`t11_power_harness.cpp`, CMake build artifact
  `omarchy-plugin-t11-harness`) performs atomic publish+promote generations,
  printing `DURABLE-GEN-<n>` only after a durable commit; the host driver
  (`security-campaign/qemu/run_power_loss_trials.sh`) boots the guest, SIGKILLs
  qemu at a random instant (equivalent to yanking power — no fsync grace), then
  reboots in `verify` mode and requires the recovered active generation to
  equal the last durable commit.
- **Result: 25/25 hard power-loss trials recovered to a fully coherent,
  durably-committed authority whose active generation exactly matched the last
  committed point; zero torn (`VERIFY-TORN-UNSAFE`), poisoned, or fresh-store
  collapses.** The rollback-journal + `synchronous=EXTRA` design over the
  constrained VFS held under real single-paged power cut.
- An early apparent failure at generation 7 (`SQLITE_CONSTRAINT_CHECK`) was a
  **harness artifact, not a product defect**: the harness expanded its revision
  string to 64 chars of `g`, which is not a hex character, so the store's own
  `CHECK(revision NOT GLOB '*[^0-9a-f]*')` correctly rejected it — confirming
  the schema's constraint enforcement works as designed. Restricting expansion
  to `0-9a-f` resolved it.
- Reproduce + rootfs layout are documented in
  `security-campaign/qemu/README.md`; this lane is manual/opt-in, not CTest.

## T08 exercised on QEMU (connection-time network policy)

The T08 lane — an approved origin reaching private services through DNS,
redirects, or media handling — was previously unverified because its policy
enforcement lives in the real resolver + libcurl path (the unit test fakes
everything through `OMARCHY_NETWORK_MEDIA_TESTING`). A disposable QEMU VM
supplies the missing controlled local-network laboratory:

- The guest runs the real `network_media_provider` (built WITHOUT the testing
  short-circuits, driven over its stdin control fd) on a loopback that carries
  one "public" (1.1.1.1) and one RFC1918 `private` (10.99.0.1) address, with an
  `/etc/hosts` mapping approved names to the public IP and leak names to the
  private IP, and a provider-side TLS CA for the positive-control HTTPS fetch.
  The host driver (`security-campaign/qemu/run_network_policy.sh`) boots it and
  asserts the product's own mitigations hold through the real resolver.
- **Result: 6/6 assertions PASS.** The approved public HTTPS fetch succeeded
  and minted only the public media handle (the wildcard entry skipped the
  private source); a concrete `*.url` pointer at the private address was
  rejected (`media-source-invalid`); a 3xx redirect was refused
  (`redirect-rejected`); a fetch whose DNS resolved to the private IP was
  refused at connect time (`network-failed`); and the private listener logged
  **zero connections** across the whole run (private-reach invariant).
- The one wall hit during bring-up — the provider's staged libcurl returning
  curl rc=60 because it uses its compiled-in `/etc/ssl/certs/ca-certificates.crt`
  bundle rather than `SSL_CERT_FILE` — was a **harness-level trust bootstrap**
  (the lab self-signed CA was written into the OS bundle), not a product
  defect; production uses real public CAs that already live in the OS store.
- Reproduce + rootfs layout are documented in
  `security-campaign/qemu/README.md`; manual/opt-in, not CTest.

## T05 exercised on QEMU (sandbox closure, as real root)

The T05 lane — a hostile worker reaching host files, session bus, network,
native modules, or inherited descriptors — is enforced by the bubbleswrap
closure built in `contracts/sandbox/src/policy.cpp`. The packaged end-to-end
bridge (systemd transient scope + full Qt worker + seccomp + launcher fd
wiring) still needs a systemd session and packaged runtime, but the *closure
mechanism itself* — the namespace/mount/capability semantics that actually stop
an escape — is a real-root question no unprivileged host can answer. A QEMU VM
boots as real root and runs a hostile probe worker inside the product's exact
bwrap argv (path-based fd substitutions):

- The guest stashes a root-only secret, brings up lab loopback addresses, then
  execs `probe_worker` (`security-campaign/qemu/t05/probe_worker.c`) inside the
  product bwrap argv (`run_sandbox_closure.sh`). The worker tries every escape
  the forensics column names — reading `/etc/shadow` and the root secret,
  escaping to host root via `/proc`, reaching the network, executing host native
  modules/executables, remounting/bind-mounting to widen the closure, and
  inheriting host descriptors.
- **Result: 10/10 probes PASS.** The worker runs (as root inside the closure)
  yet reads no host file, opens no socket, sees no host process/executable,
  cannot mount/remount to escape, and inherits no host read/write descriptors;
  the sole writable host-backed path (`/state`) accepts writes while the ro
  plugin/revision mount (`/plugin`) rejects them. A note records that this
  kernel lets an unprivileged worker self-grant a *child* user namespace
  (unprivileged-userns), but that does not reach the host because a child
  userns' caps never cover the sandbox's host-owned mounts/namespaces and there
  is no host namespace fd to `setns()` into.
- Scope note: the QEMU lane above closes the **closure mechanism**. The full
  packaged bridge (systemd transient scope + packaged Qt worker + launcher
  fd/seccomp wiring + seccomp filter) was subsequently verified **PASS on the
  live host**: `plugin-neutral-surfaces-real-bwrap` boots the production
  supervisor under the real systemd user session and the packaged worker, then
  attaches, renders, and routes a trusted gesture end-to-end. The only
  prerequisite was refreshing the packaged worker to the current build (the
  canonical `/usr/lib/.../omarchy-plugin-qml-worker` was stale, causing a
  schema/version handshake failure that is packaging skew, not a product
  defect). Reproduce + rootfs layout are documented in
  `security-campaign/qemu/README.md`; manual/opt-in, not CTest.

## New regression tests added

These encode the adversarial inputs exercised above into the standard (non
opt-in) CTest suite, alongside the boundary cases already present.

- `contracts/surface/tests/boundary_test.cpp` → `plugin-surface-boundary` (labels `adversarial;integration;render;security`). T06 cases: over/undersized pixel-frame rejection; odd slot-sequence and zero frame-sequence rejection; out-of-range slot (`slot_base(2)` absent, publish slot 2 rejected); per-slot accept + stale cross-slot replay rejection; foreign-surface notification rejection (`stale_surface`); inconsistent-allocation consumer rejection; undersized double-buffer mapping rejection on `initialize_frame_mapping`.
- `providers/tests/private_storage_backend_test.cpp` → `plugin-private-storage` additions. T13 cases: exact-fill total-quota boundary (admit at cursor, reject one byte over); non-regular entry (subdirectory) in the private root invalidates a quota scan so data cannot be hidden behind a directory before a later write.

Both pass. Full suite after changes: **83 tests, 82 pass + 1 opt-in skip**, and
the security-labelled set is **60/60 pass**.

## CVSS v4.0 disposition

CVSS uses the FIRST v4.0 calculator per the parent threat model. Per that
document's own discipline, a blocked or unestablished hypothesis is **not**
scored 0.0, and no numeric score is a number earned only by a reproduced,
confirmed finding. No confirmed vulnerability was established in the pure-C++
boundaries that this environment could exercise. Therefore:

| Hypothesis / boundary | Confirmed by this campaign? | CVSS v4.0 |
| --- | --- | --- |
| T01 archive traversal/replacement | No — ingress holds under review + 36 manifest/ingress tests; fuzz clean | not scored |
| T03 spoofed/replayed broker request | No — stamp/correlation/version checks hold; envelope fuzzer 263M clean; broker/session/adversarial tests pass | not scored |
| T06 host memory corruption from frames | No — checked bounds hold; render fuzzer 420M clean; new boundary test passes | not scored |
| T09 provider executable/pathname replacement | No — pinning + digest + `execveat` hold under review | not scored |
| T11 torn/rolled-back SQLite authority | No — transaction + poison + cutover hold under crash-matrix+source review and under 25/25 QEMU hard power-loss trials (no torn/resurrected state) | not scored |
| T13 storage escape / quota bypass | No — key/no-follow/nlink/quota hold; new quota boundary test passes | not scored |
| T04 revocation/epoch race at effect time | Not independently re-established; existing broker/session fence tests pass | pending, not confirmed |
| T02 review/revoke interleavings | Not independently re-established; consent/authority tests pass | pending, not confirmed |
| T05 packaged end-to-end sandbox | Closure mechanism verified as real root on QEMU (10/10 probes: no host file/network/proc/mount/descriptor escape; only `/state` writable). Full packaged bridge verified PASS on the live host through the production supervisor + real systemd scope + packaged worker (attach, render, trusted-gesture route) | not scored |
| T07 compositor gesture provenance | Verified live on a real display: a genuine pointer gesture on a projected plugin surface ingressed as `decision=accepted ... trusted-physical=true` and consumed the one-use eligibility binding | not scored |
| T08 connection-time network policy | Verified end-to-end on QEMU through real resolver/curl (6/6: public fetch ok, private/re-direct/DNS-to-private refused, zero private reach) | not scored |
| T15 fake trusted-prompt / phishing | By design, host ownership does not prevent visual deception; requires review-guidance control, not reachability testing | unverified / remediation item |

**Why "not scored" for the unconfirmed rows:** for each, the assumed control
failure (the thing CVSS would rate) has no reproduction, and most require an
environment this assessment could not use. Assigning a Base vector to an
unreproduced hypothesis would be unsupported precision of exactly the kind the
parent document forbids. The absence of a score means "no confirmed
vulnerability," not "severity 0."

**Conditional (hypothetical, deliberately excluded from any confirmed count):**
if one assumed worker-boundary failure (e.g. T05/T06) were later confirmed, the
typical starting vector would begin `CVSS:4.0/AV:L/AC:L/AT:P/PR:L/UI:N/...`
(local adversary via the worker process, physical/attacker-per-side-channel
complexity, partial execution-trace info leaked to scope). It is stated here
only to document the scoring path, and must not appear as a finding.

## Patch decision

- **No code patch is warranted for the boundaries whose hypotheses resolved as unconfirmed** (T01, T03, T06, T09, T11, T13): the authorizing and effect-time controls hold under source review and the new adversarial cases pass. A patch with no reproduced defect would be speculative churn.
- **The committed deliverable is the added regression coverage** (`boundary_test.cpp` plus the private-storage quota additions), locking in the adversarial inputs so a future regression on T06/T13 fails loudly in CI.
- **Action items that remain genuinely open and are not reachable in this environment** (so no in-code patch is appropriate yet): root-owned package lifecycle repeat (T09 root path). T15 needs a product decision on unmistakable trusted-review origin, not a reachability test. (T11 physical power-loss durability was exercised in QEMU to 25/25 pass; T08 connection-time network policy was exercised to 6/6 pass; the T05 sandbox closure mechanism to 10/10 pass and its full packaged bridge to PASS on the live host; and the T07 compositor gesture provenance to PASS on a real display — none of these remain open.)

## Remaining gaps (unchanged unless noted)

Same as the parent document except that T11 physical power-loss durability was
exercised in QEMU (25/25 trials), T08 connection-time network policy was
exercised end-to-end on QEMU (6/6 assertions through the real resolver/curl),
the T05 sandbox closure mechanism was exercised on QEMU as real root (10/10
probes) and its full packaged bridge verified PASS on the live host, and the
T07 compositor gesture provenance was verified PASS on a real display — all of
these are above. A root-owned package lifecycle repeat, sustained resource
stress, and disposable-network/graphical re-derivation remain unverified. The
intermittent launcher descriptor-injection failure did not reproduce in five
consecutive runs and remains unresolved/unconfirmed. No production rollout is
inferred from this commit.
