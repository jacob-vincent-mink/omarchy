# Ward threat model

A security assessment of **Ward**, the Rust runtime that runs Quickshell
plugins in a sandboxed worker ([`native/ward`](../../native/ward)). Ward gives
a reviewer one approval, one supervisor, and one sandboxed worker; a plugin
runs under its admitted grants and nothing below a trust boundary can widen its
own authority.

## Deliverables

| File | What it shows |
| --- | --- |
| [`01-layers.svg`](01-layers.svg) | The L0 / L1 / L2 trust layers: trusted host, supervisor/kernel boundary, untrusted sandbox. |
| [`02-dataflow.svg`](02-dataflow.svg) | approval → admission → spawn → request → output, with the invariants that make each hop safe. |
| [`03-trust-boundaries.svg`](03-trust-boundaries.svg) | the eight trust boundaries as gates, each fail-closed. |
| [`04-stride-and-trust.md`](04-stride-and-trust.md) | the STRIDE and trust assessment, boundary by boundary, with the concrete controls. |
| [`pen-test/`](pen-test/) | the white-box and black-box pen-test and its findings. |

## The eight trust boundaries

| Boundary | Between | Enforced by |
| --- | --- | --- |
| TB-1 | Host ⇄ Store (approval) | ed25519-signed, revision-bound records |
| TB-2 | Store ⇄ Controller (admission) | fail-closed grants validation; the admitted snapshot governs the run |
| TB-3 | Controller ⇄ Supervisor (spawn) | cgroup limits; controller-owned descriptors only |
| TB-4 | Supervisor ⇄ Worker (sandbox) | namespaces + caps + Landlock + seccomp; fds closed before re-open |
| TB-5 | Worker ⇄ Broker (requests) | peer-credential UID + cgroup; rate limits; per-request re-check |
| TB-6 | Worker ⇄ Compositor (input) | the host projects key/pointer/scroll; the worker has no input path |
| TB-7 | Worker ⇄ Media proxy (MPRIS) | a fixed allow-list; no ownership, no calls |
| TB-8 | Worker ⇄ Host (effects) | positive-only argv, exact http scope, exact settings keys, text policy |

## Bottom line

The September 10 PR review found security-critical gaps in installation identity, filesystem authority selection, revocation and host input ownership, plus exec path and integration-test gaps. See the [review regression ledger](review-regressions.md) for fixes and their actual evidence. The earlier 21 validator-focused cases are not exhaustive adversarial coverage and do not establish that all integration suites pass.

An approved host CLI retains its own authority; argv matching cannot prove semantic safety. The approving account is also the desktop account, but this accepted limitation does not permit a filesystem grant to expose signing keys to a sandboxed worker. Graphics drivers, the private Wayland parser and installed/physical desktop behavior remain distinct audit and acceptance surfaces. Ward is not yet merge-ready on the strength of this threat model.

See [`04-stride-and-trust.md`](04-stride-and-trust.md) for the full assessment
and [`pen-test/findings.md`](pen-test/findings.md) for the case-by-case
results.
