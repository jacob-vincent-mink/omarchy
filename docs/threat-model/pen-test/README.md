# Ward pen-test

Two independent passes were run against the Ward runtime: a **white-box** pass
that reads the source and targets a named invariant, and a **black-box** pass
that works from only the plugin authoring docs
([`docs/sandboxed-plugin-authoring.md`](../../sandboxed-plugin-authoring.md)).
Each pass crafts the concrete thing a plugin would emit — a manifest request, an
argv, an HTTP request, a settings write, a notification, a context, a
presentation record — and drives it through the **real** Ward validation code.

The assertions prove the host-side boundary **contains** the attack. They are
regression guards, not a claim of a vulnerability.

## Layout

| Path | Audience | Contents |
| --- | --- | --- |
| `README.md` (this file) | everyone | methodology and the findings summary |
| `whitebox.md` | reviewers | the code-reading pass, boundary by boundary |
| `blackbox.md` | reviewers | the docs-only pass, attack by attack |
| `findings.md` | reviewers | every case, the boundary, the control, the result |
| `plugins/` | **fork only** | the malicious plugin bundles (offensive assets) |

The malicious plugin bundles in `plugins/` are the **offensive** artifacts and
are kept in the `jacob-vincent-mink/omarchy` fork, not merged into mainline.
The **negative tests** that prove each boundary holds live in
[`native/ward/tests/pen_test.rs`](../../../native/ward/tests/pen_test.rs) and
[`native/ward/tests/pen_test_blackbox.rs`](../../../native/ward/tests/pen_test_blackbox.rs)
and ship in mainline as regression guards.

## Method

A real Ward plugin runs as a sandboxed worker (L2). It can only touch the
sockets and files the controller mounted for it, and every request to the host
passes a peer-authenticated, rate-limited, per-request re-checked broker. A
plugin cannot widen its own grant: the admitted record governs the run, a
`grants.json` edit cannot widen a running plugin, and a capability whose socket
is not admitted cannot be connected at all.

The pen-test therefore attacks the boundaries a plugin can actually reach:

- **TB-1/TB-2** — a plugin manifest can only *request*; it approves nothing.
- **TB-4** — the worker sandbox (namespaces, caps, Landlock, seccomp).
- **TB-5** — the requests broker (peer auth, rate limit, re-check).
- **TB-6** — presentation (buffers) and context (one-way, sealed).
- **TB-8** — exec, http, settings, and notification (the host-effect surface).

The negative tests exercise the real, public validation entry points
(`exec_policy::Tree::check`, `http::Scope::validate`, `grants::Grants::validate`,
`settings::Grant::validate`/`check_write`, `notification::Request::new`/`decode`,
`context::UiContext::parse`, `presentation::Event::decode`). The private
scope-vs-request matching (`http::Scope::check`) is covered by the module's own
test `methods_origins_paths_and_required_query_filters_cannot_be_widened`,
which the black-box report references rather than duplicates.

## Result

The original 21 cases are useful validator regressions, not a complete penetration test. The September 10 review identified critical coverage gaps and a failing real-worker fixture, superseding the earlier claim that the full integration suite was green. Track current findings and executed regressions in the [review ledger](../review-regressions.md); report skipped opt-in suites separately from passing tests.

In particular, constructing a filesystem grant is not exercising `Store::approve` or a worker mount; validating an HTTP scope is not checking a hostile request against that scope; and rejecting a packet in the sender does not test a receiver's decoder. Each result must identify the boundary actually reached.

See [`findings.md`](findings.md) for the case-by-case table.
