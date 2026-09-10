# Ward threat model — STRIDE and trust assessment

Assessment correction: the September 10 PR review invalidated the blanket low-risk conclusions below. These tables are the historical control inventory, not current security sign-off. The [review regression ledger](review-regressions.md) supersedes their residual ratings for installation identity, signing-key exposure, revocation, host input admission and exec path confinement; additional audit gaps remain open.

This document is the analytical core of the Ward threat model. It accompanies the
diagrams in this directory:

- [`01-layers.svg`](01-layers.svg) — the L0 / L1 / L2 trust layers.
- [`02-dataflow.svg`](02-dataflow.svg) — approval → admission → spawn → request → output.
- [`03-trust-boundaries.svg`](03-trust-boundaries.svg) — the eight trust boundaries as gates.

Ward is the Rust runtime that runs Quickshell plugins in a sandboxed
worker. A plugin is a bundle of QML/native code that a reviewer imports and
approves. Approval selects a set of grants; the worker runs under those grants
and nothing below a trust boundary can widen its own authority.

The assessment below follows the source in `native/ward/src/`. Every row names
the concrete mechanism that enforces it and the module that owns it. Severity
uses the scale **None / Low / Medium / High / Critical** for the *residual*
risk after the listed control; the "threat" column is the raw, unmitigated
potential.

## Assets and their owners

| Asset | Owner | Why it matters |
| --- | --- | --- |
| Plugin revision bundle (`/plugin`) | Store, content-addressed | The untrusted code the worker runs. Pinned by digest so a path cannot substitute it. |
| Admitted grants record | Store, ed25519-signed | The authority the running session enforces. Tamper-detected; revision-bound. |
| Persistent plugin data (`/home/plugin` storage bind) | Store, per-id 0700 | Survives restarts; a writable host bind. Writes are real and not undoable. |
| Host helpers (`$OMARCHY_PATH/bin/*`) | Host, full CLI authority | The effect a request reaches. A granted command keeps its own authority. |
| Private compositor display | Controller, in-process | The only surface the worker can paint on; input is projected into it. |
| Session bus (media proxy) | Controller, xdg-dbus-proxy | A filtered MPRIS view, not the full bus. |

## STRIDE by trust boundary

### TB-1 · Host ⇄ Store (approval)

| Threat | Unmitigated potential | Control | Residual |
| --- | --- | --- | --- |
| Spoofing | A plugin forges an approval record for another id | `management` requires `review(revision).id == id`; the record is keyed by id and re-read from the signed store. | Low |
| Tampering | A record is edited on disk to widen grants | ed25519 signature over the record; `Store::open`/`admit` reject an unsigned or mismatched record. | Low |
| Repudiation | "I never approved that" | Approve bumps an epoch and persists the signed record; the running session is bound to the epoch it was admitted at. | Low |
| Information disclosure | A review leaks another plugin's grants | Review returns only the manifest's own requests; grants default to `Grants::default()`. | None |
| DoS | A huge/recursive approval request | 64 KiB stdin and request ceiling; `Grants` caps 256 dirs, 64 KiB record, bounded tree nodes. | Low |
| Elevation of privilege | Import silently approves | Import only stages a revision; Approve is explicit; a failed Approve does not revoke a running revision. | Low |

### TB-2 · Store ⇄ Controller (admission)

| Threat | Unmitigated potential | Control | Residual |
| --- | --- | --- | --- |
| Spoofing | A controller claims an id/epoch it does not own | `with_authority` re-reads the signed record under the store lock against the admitted id/epoch/unit. | Low |
| Tampering | `grants.json` is edited mid-run to widen access | The running session enforces the *admitted snapshot*; a later edit takes effect only on the next launch. `context.filter` uses the admitted grants. | Low |
| Repudiation | A request is attributed to the wrong plugin | Every request is peer-authenticated to the unit and re-checked under the lock (TB-5). | Low |
| Information disclosure | `grants.json` leaks host structure | It is authored from the record (never plugin metadata) and mounted read-only; the plugin can adapt to *declined* access. | None |
| DoS | A corrupt record blocks the loop | A missing/corrupt record maps to `Unavailable`, never a permission decision; the caller is not told to seek a wider grant. | Low |
| Elevation of privilege | A request implies a capability it was not granted | `with_request` checks the admitted grant per kind and denies otherwise. | Low |

### TB-3 · Controller ⇄ Supervisor (spawn)

| Threat | Unmitigated potential | Control | Residual |
| --- | --- | --- | --- |
| Spoofing | A worker selects its own limits | `verify_controller_limits` is called in the controller; the worker receives controller-owned descriptors, never a limit choice. | Low |
| Tampering | A host path is mounted into the sandbox | Only `--*-bind-fd` from controller-owned descriptors; no `--ro-bind` of host paths. | Low |
| Repudiation | A grant is claimed without a directory | A grant without its prepared directory (or vice versa) is an error, not a silent tmpfs downgrade. | None |
| Information disclosure | A granted directory is not 0700 | Storage and media directories require owner-private 0700. | Low |
| DoS | A runaway worker exhausts host resources | cgroup 512 MiB / 128 tasks / 50 % CPU; `--timeout`; cgroup.kill on Drop. | Low |
| Elevation of privilege | A worker re-arms a capability | The spawn is one-shot and non-restartable; the supervisor owns the unit. | Low |

### TB-4 · Supervisor ⇄ Worker (bwrap sandbox)

| Threat | Unmitigated potential | Control | Residual |
| --- | --- | --- | --- |
| Spoofing | The worker impersonates another cgroup member | `--unshare-cgroup`; the broker re-checks cgroup membership of the peer. | Low |
| Tampering | A file write reaches a host socket | Landlock allows only `RESOLVE_UNIX` on the granted sockets; no file→IPC grant. `restrict_bootstrap` closes fds 3..MAX before re-opening sockets (a connected socket bypasses path-Landlock). | Low |
| Repudiation | n/a (kernel-enforced) | Namespaces, caps, seccomp are kernel-enforced; the worker cannot remove them. | None |
| Information disclosure | The worker reads host files | `/tmp`, `/run/plugin` are tmpfs; `/home/plugin` is a tmpfs or a 0700 storage bind; no host path is bind-mounted. | Low |
| DoS | The worker escapes to the host and hogs it | `--cap-drop ALL`, `--die-with-parent`, cgroup limits, and a 10 s request timeout bound it. | Low |
| Elevation of privilege | The worker gains a capability | `--unshare-user`, `--disable-userns`, `--assert-userns-disabled`; no namespace is re-enabled. | Low |

### TB-5 · Worker ⇄ Broker (requests)

| Threat | Unmitigated potential | Control | Residual |
| --- | --- | --- | --- |
| Spoofing | An unauthenticated peer issues a request | `authenticate_member`: `SO_PEERCRED` uid == host uid **and** the peer is in the controller's cgroup; a failed auth is still charged. | Low |
| Tampering | A request packet is forged | Typed, versioned, `deny_unknown_fields` records; oversized or unknown records are rejected, not truncated. | Low |
| Repudiation | A request is attributed to the wrong id | `with_request` re-checks live authority under the store lock per request; the id is the admitted id, never worker-supplied. | Low |
| Information disclosure | A request carries a descriptor | Effect requests reject packets with fds; payloads are memfd-sealed and size-bounded. | Low |
| DoS | A worker floods the broker | Per-kind token `Budget`; 15 bounded delivery slots; an admission window cap (32) and 4 live clients; 3 s reply deadline. | Low |
| Elevation of privilege | A UI metadata request selects a host effect | UI metadata (panel/widget/switch) has `kind() == None` and cannot build a command; only data travels. | None |

### TB-6 · Worker ⇄ Compositor (input projection)

| Threat | Unmitigated potential | Control | Residual |
| --- | --- | --- | --- |
| Spoofing | The worker injects its own key/pointer | Input originates only from the trusted host (`Graphics::key`/`input`/`scroll`); the worker has no input path. | None |
| Tampering | A forged control record | `Control::decode` validates exact 16/24/28-byte records; `Key`/`Scroll` bounds are re-checked on send and decode. | Low |
| Repudiation | Focus is granted without a gesture | A layer gains `OnDemand` focus only after a real click; exclusive focus is armed only for a summoned panel. | Low |
| Information disclosure | The full host keymap enters the worker | The keymap is built incrementally from host-delivered keys (≤ 760); no full keymap or host socket enters the worker. | Low |
| DoS | A worker surface hogs the render thread | `frame.finish().wait()` is in the supervised per-plugin process, not the trusted shell; cgroup limits and surface caps bound it. | Low |
| Elevation of privilege | The worker grabs input | Only the trusted host may arm focus/grab; geometry changes cancel grabs, and the host resumes input only after a new canvas is presented. | None |

### TB-7 · Worker ⇄ Media proxy (MPRIS)

| Threat | Unmitigated potential | Control | Residual |
| --- | --- | --- | --- |
| Spoofing | The worker reaches the session bus directly | It sees only a unix socket; the proxy is `xdg-dbus-proxy` in the controller's service and dies with it. | Low |
| Tampering | A bus address is injected | The address must be a pathname Unix bus, no `;`, no control chars, ≤ 4096. | Low |
| Repudiation | A call is attributed to the wrong player | One selected player name; a name mismatch is an error. | None |
| Information disclosure | The worker introspects arbitrary objects | A fixed allow-list of transport/Properties/Introspect + play-control methods and two signals. | Low |
| DoS | The worker floods the bus | The proxy is a single child; a `check()` fails the session if it exits. | Low |
| Elevation of privilege | The worker gains bus ownership | `--filter`; no `--talk`, no `--own`, no wildcards; `OpenUri`/`Raise`/`Quit`/`Properties.Set` denied. | Low |

### TB-8 · Worker ⇄ Host (exec / http / settings / notify)

| Threat | Unmitigated potential | Control | Residual |
| --- | --- | --- | --- |
| Spoofing | The worker picks its own executable | Exec runs a sealed copy at an exact digest; the executable is a controller-prepared `PreparedExecutable`, never worker-supplied. | Low |
| Tampering | An argv argument escapes the granted tree | Positive-only argv `Tree`; whole-argument regex `\A…\z`; path-traversal guard on `$TOKEN/rel`; bounded sizes. | Low |
| Repudiation | A host effect is attributed to the wrong plugin | The helper is called with the admitted id; `settings` rejects host-structure keys (`id`/`sandbox`/`__proto__`/`constructor`/`prototype`). | Low |
| Information disclosure | A notification or URL smuggles a command | Notify escapes `&`/`<`/`>` and rejects control/RTL chars; the helper recognizes options in either slot; open-url accepts only bounded http(s) with a non-empty host. | Low |
| DoS | A request runs forever | `timeout --signal=KILL 1s..10s`; 128/32-token exec budgets; 3 s reply deadline; bounded output (2 MiB stdout/stderr). | Low |
| Elevation of privilege | **A granted command does more than its argv suggests** | **Documented, intentional:** argv matching is not semantic safety. A host command keeps its full authority; the reviewer's selection is the boundary. Mitigation is the reviewer plus revocation (which cannot undo a completed effect). | Medium (by design) |

## Trust assumptions and their status

| Assumption | Status | Note |
| --- | --- | --- |
| The approving account is the same session account | **Accepted limitation** | Not an independent human boundary. A compromise of the session is a compromise of the desktop; Ward bounds the *plugin*, not the session. |
| A completed host effect can be revoked | **Not possible** | Revocation prevents *future* effects but cannot undo a file written or a package installed. The store documents this; the reviewer is shown that a writable bind writes real host data. |
| The kernel (namespaces, caps, Landlock, seccomp, cgroup) is trusted | **Accepted, unavoidable** | This is the isolation root. `unsafe_op_in_unsafe_fn` is denied at the crate level; the few `unsafe` blocks are bounded and async-signal-safe after fork. |
| The host helpers are correct | **Trusted by construction** | A granted helper is a full-CLI command; its correctness is the host's, not the matcher's. This is the same-account limitation surfaced at TB-8. |
| Content-addressing pins the revision | **Enforced** | Revisions reject symlinks/hardlinks/special files and require `nlink == 1`; the bundle is pinned by digest, not by path. |
| The worker cannot widen its own grant | **Enforced at every boundary** | The admitted snapshot governs the run; `grants.json` is read-only and derived from the record; `context` is filtered through the admitted grants. |

## Historical residual-risk summary (superseded)

| Boundary | Highest residual | Reason it is acceptable |
| --- | --- | --- |
| TB-1 | Low | Signing + revision binding; import never auto-approves. |
| TB-2 | Low | Admitted snapshot + live re-check; a corrupt record is `Unavailable`, not a decision. |
| TB-3 | Low | Controller-owned descriptors + cgroup limits. |
| TB-4 | Low | Namespaces + caps + Landlock + seccomp; fds closed before re-open. |
| TB-5 | Low | Peer auth + cgroup + rate limits + per-request re-check. |
| TB-6 | None | Input originates only from the trusted host. |
| TB-7 | Low | Fixed MPRIS allow-list; no ownership/calls. |
| TB-8 | **Medium (by design)** | A granted host command retains full CLI authority; the reviewer's selection and revocation are the boundary. This is the single intentional residual. |

The original pen-test cases cover selected validator paths, not every boundary or reachable attack. Do not use these historical ratings as a release decision; use the current regression ledger and explicitly recorded end-to-end results.
