# Original plugin compatibility proof

This is the source baseline and acceptance ledger for the four actual competition selections. It is not a claim that any plugin has complete sandboxed feature parity. The requirements supplement [the runtime plan](sandboxed-quickshell.md); smaller replacements and previous schema-v2 ports do not satisfy them.

## Baselines and review boundaries

These are the clean default-branch checkouts inspected on 2026-09-08 under `../`, in the `jacob-vincent-mink` directory. Pin the commits, not merely their manifest versions, when comparing original and candidate behavior.

| Plugin | Review fork | Base branch | Original commit | Manifest |
| --- | --- | --- | --- | --- |
| Radio Atlas | `jacob-vincent-mink/omarchy-radio-atlas` | `main` | `d5e445e35f3e545fbbb10410aff08ff91fb80647` | `akshar.radio-atlas`, 0.1.4 |
| Omagotchi | `jacob-vincent-mink/omagotchi` | `master` | `e1b75eb717a8ce85e749dce8ccdf74b386cfa8c5` | `slcode777.omagotchi`, 0.1.0 |
| AirPods | `jacob-vincent-mink/omarchy-pods` | `main` | `fff7fec600a5b9a61cdb40e93eccbcceb4b8f824` | `io.github.thisisgm.omapods`, 1.3.6 |
| GitHub | `jacob-vincent-mink/omarchy-github` | `main` | `9d00e24f7ddd88a2ec21ac1bf823d359c6bdb708` | `robzolkos.github`, 0.3.0 |

All four manifests are schema 1. Required edits must go on dedicated branches, with PRs targeting these forks and their explicit base branches only. Do not push, open PRs, or otherwise write to the original authors' repositories. Keep the original feature implementation and assets wherever possible; account separately for manifest/bootstrap changes, host adapters, helper changes, and runtime changes. The first compatibility change is [Radio Atlas fork PR #3](https://github.com/jacob-vincent-mink/omarchy-radio-atlas/pull/3), a draft from `rust-sandbox-compat` into this fork's `main`: two bar activation calls plus a focused regression test. The other three plugin checkouts remain unchanged. None is a completed sandbox port.

## Shared host dependencies

All four import the original `qs.Commons` and `qs.Ui` modules. Those modules are not just colors and controls: `Color.qml` watches the current theme, `Style.qml` reads theme configuration and compositor options, and panel/widget primitives consume bar geometry, popup ownership, focus and lifecycle. Packaging unchanged plugin QML with inert defaults would not establish theme or panel parity.

The current native activation surface (`shell/services/native/SandboxedPluginSurface.qml`) is one nonexclusive top-layer canvas on the first screen, not a bar slot. The shared worker now provides own-service/overlay lifecycle and initial/live detached settings and style tokens without restarting plugin instances. The GitHub and Omagotchi review forks now declare sandbox requests; the other private experiments add declarations to staged copies. Real bar placement, panel switching, correct host-relative popup anchoring and full font-asset parity remain required. Host QObject access must not cross the isolation boundary.

The separate graphics work owns fractional scaling and output changes; coordinate requirements discovered here must feed that shared contract rather than create a second protocol. Omagotchi needs output identity, origin, reserved edges and window geometry as well as a drawable viewport. Radio Atlas needs a screensaver lifecycle signal. Neither need implies permission to expose the raw Hyprland command socket.

## GitHub

Sources: `README.md`, `manifest.json`, `Panel.qml`, `Service.qml`, `omarchy-github-fetch`, and `tests/` at the pinned commit.

- [ ] Preserve all sections: notifications, review requests, authored PRs and check rollups, assigned issues, active Actions, recent failures, and repository metrics.
- [ ] Preserve complete pagination, owned/organization scope, archived/fork/draft filtering, search across the full fetched list, all metric filters and sorts, rendered-row bounds, and activity expansion/notification paging.
- [ ] Preserve Actions Off/Recent/All modes, concurrency and repository limits, failure window/count, refresh interval, and unlit-icon setting.
- [ ] Preserve individual notification reads and the two-step, expiring bulk confirmation. Keep the captured list boundary and same-second ID handling; do not broaden the bulk operation to unseen arrivals. Preserve failure recovery and refresh after each attempt.
- [ ] Preserve direct links in both browser-tab and web-app modes, all documented keyboard and bar mouse controls, settings-page transitions and persisted inline settings.
- [ ] Preserve loading, logged-out, missing-dependency, rate-limit, partial-result and error behavior. Use existing `gh` authentication without adding a token setup workflow or exposing credentials to the presentation code.

The original helper implements domain behavior through `gh api` and `jq` and remains the implementation. The fork now declares eleven generic exec leaves and invokes `/bootstrap --exec gh` explicitly in the worker; there is no `gh` shim or PATH override. Its GraphQL, pagination, aggregation and notification-boundary logic is unchanged. Standalone execution still calls the installed `gh`. The worker needs neither host credentials/keyring access nor network/HTTP grants: supervised host-side `gh` owns authentication and network requests. `Panel.qml:182` still launches the original browser helpers, while `Panel.qml:199` uses the scoped own-settings callback. These are separate explicit grants, not writable `shell.json` or arbitrary host execution.

Current private helper evidence: six ten-case runs used the actual restricted worker, executable broker and installed `/usr/bin/gh` against a Unix HTTP fixture and fake credentials, with Internet access disabled. Ordinary access, read-only selection, declined sign-in, logged-out state, partial-result errors and rate limiting passed. Pagination, filters, repository metrics, authored-PR rollups, Actions modes and limits, and exact single/bulk notification operations were checked against fixture responses and recorded requests. Token-revealing flags, arbitrary GraphQL and foreign URLs were rejected. Declined sign-in caused zero HTTP calls; read-only selections caused zero writes. The original helper, panel-source and service-source suites also pass. This does not check off full graphical behavior, 12-repository simultaneous load, actual-account compatibility, or the final desktop demonstration.

The original settings page was verified against runtime `411ec4f7` on a private 1000×800 display. Plugin files were copied byte-for-byte from the pinned commit; only the staged manifest gained `sandbox: { version: 1, requests: { settings: true } }`. With only settings access approved, real pointer/keyboard input changed all six visible controls: browser-tab preference, owned-and-organizations scope, a 30-minute refresh interval, unlit icon, archived repositories and forked repositories. The existing host IPC/configuration writer persisted the exact string, integer and boolean values while preserving unrelated settings and the sandbox marker. Closing/reopening the original page and restarting the private Quickshell host restored all six values; rendered captures were inspected, not inferred from JSON alone. Repeating with the grant denied left the saved values unchanged, reverted all six controls and displayed the bounded save-error notice. No plugin QML or helper edits were needed. The fixture, account-free data and captures remain uncommitted experiments; no real desktop configuration was loaded or changed. This proves local preference persistence and page interaction, not browser opening, authenticated data filtering or the remaining bar-settings schema controls.

## Radio Atlas

Sources: `README.md`, `RadioAtlas.qml`, `BarWidget.qml`, `Globe.qml`, `RadioModel.js`, `radio-fetch`, `radio-player`, `radio-session`, `radio-sandbox`, `radio-proxy`, `radio-state`, and `tests/` at the pinned commit.

- [ ] Preserve theme-aware globe drag, deep wheel zoom, station signals, country selection and focus, location estimates, progressive world population, country priority, and session catalog retention.
- [ ] Preserve cached full-directory search/country browsing, background persistent world/country cache refresh, retries, random tuning with recent exclusions, favorites, listening history, and malformed/oversized-state handling.
- [ ] Preserve station identity and track metadata, favoriting the currently playing station, independent volume and mute, playlist previous/next, automatic failed-stream skipping, and integration with the stock Omarchy media controls.
- [ ] Preserve every documented keyboard shortcut; bar left/open, middle/random, right/stop, and wheel/volume; click-through outside the card and screensaver dismissal.
- [ ] Preserve playback when the panel closes, stop semantics, diagnostics, and saved favorites/history/volume across restart or reinstall. Separately verify that disabling/revoking the sandbox terminates its owned helpers.
- [ ] Preserve the player's bounded proxy and rejection of private/local stream destinations, including redirected destinations. Do not remove the original playback isolation merely to make startup pass.

The bar and overlay coordinate through files under `XDG_RUNTIME_DIR/omarchy-radio-atlas`; state is under `XDG_DATA_HOME/radio-atlas`, caches under `XDG_CACHE_HOME/omarchy-radio-atlas`. The original helper uses `setsid`, mpv IPC, mpv-mpris and audio output. `radio-session:35` creates another network/PID namespace with Bubblewrap and `CAP_NET_ADMIN` to configure loopback; the current worker's seccomp policy denies `unshare`. Thus a direct nested launch has a concrete policy conflict requiring a targeted experiment and design, not a manifest-only promise. Existing selected-player MPRIS access controls an already running host player; it does not provide this plugin's audio playback or export its newly created player to the stock media service. `RadioAtlas.qml:138` dismisses on the screensaver's Hyprland open-window event.

## Omagotchi

Sources: `README.md`, `Service.qml`, `BarWidget.qml`, `Panel.qml`, `RoamWindow.qml`, `PetSprite.qml`, sprites and bundled sounds at the pinned commit.

- [ ] Preserve the egg, baby, child, both teen paths and all three adults; active-minute age and care averages; evolution thresholds, generation, adult farewell and new egg.
- [ ] Preserve all five needs, update/orphan influence on need rates, feed, mouse scrubbing, affection, sleep/wake/re-sleep behavior, and home-only care while roaming.
- [ ] Preserve bar sprite/mood animation, middle-click petting, themed room/sprites, all event sounds, volume and mute, evolution/farewell/corrupt-save notifications.
- [ ] Preserve child-and-older departure animation, tractor beam and panel-to-screen handoff, wandering, climbing, riding moved windows, falling from removed windows, roaming naps, picking up/dragging/dropping, stun/affection effects and return home.
- [ ] Preserve click-through except the pet, largest-screen selection, active-workspace platform filtering and bottom reserved-edge handling. Record the original source's scale-1 coordinate assumption separately from sandbox regressions; do not claim the original already supports fractional scaling correctly.
- [ ] Preserve both saved files, bounded startup reads, atomic writes and restart recovery. Test staged copies without aging, resetting or overwriting the user's original pet.

The service runs independently of the bar and creates the roaming window even when hidden. `BarWidget.qml:12` obtains only its own service via `bar.shell.serviceFor`. The review fork forwards the original package and sound calls through explicitly selected exec leaves and notifications through the text broker. Both original files use the storage-granted private home. The original `checkupdates` still fetches repository databases; its observation must not be replaced with a fabricated zero. Private trials now verify both package observations, Feed and saved-state recovery, and one original sound event. These are partial results, not completion of the feature checklist. `RoamWindow.qml:55-100` still needs monitor/workspace state and visible window rectangles with stable identity, not titles, application secrets or compositor control.

The original Feed action was also exercised with fresh generation-three baby data and volume 0.25 against a private PipeWire server without hardware modules or a session manager. With the sound leaf selected, the installed `pw-play` consumed the immutable staged `eat.wav` and the capture contained 27,103 nonzero samples; with the leaf declined, there was no playback stream and every captured sample was zero. Both runs completed the original meal and saved zero hunger, and their panel captures were inspected. The worker had no PipeWire socket. This verifies the host-exec audio path for one event, not every clip, real-speaker output, notifications, roaming or complete evolution behavior. The concrete experiment remains outside Ward and uncommitted; Ward's maintained regression uses a synthetic executable alias and verifies its invocation name and rejection after retargeting to different bytes.

## AirPods

Sources: root `README.md`, `manifest.json`, `Panel.qml`, `Service.qml`, `Model.js`, `AirPodsIcon.qml`, `daemon/ipcpath.hpp`, `daemon/librepods-ctl.cpp` and the daemon command dispatch in `daemon/main.cpp` at the pinned commit. The daemon's older upstream README is not the current plugin feature specification.

- [ ] Preserve per-pod/case or Max battery, charging/in-ear hints, BLE battery while disconnected, case lid reporting, hardware-specific icon and capability-gated rows.
- [ ] Preserve available listening modes, adaptive level, Conversation Awareness, One-Bud ANC, all three ear-detection behaviors, and right-click mode cycling.
- [ ] Preserve keyboard shortcuts, adaptive arrow repeat/queued controls, optimistic-value settling, visible command errors, refresh, panel switching and disconnected icon setting.
- [ ] Preserve no-polling status-file updates, daemon absence/unsupported-schema states, custom control-executable setting and daemon restart recovery.
- [ ] Preserve the existing external daemon and its hardware behavior, including highest-bitrate playback selection and capture-aware activation. Do not change pairing, keys, codec policy or select a headset microphone profile to make the demonstration work.

The plugin is intentionally only a display and command client, not a Bluetooth implementation. `Service.qml:39` watches `XDG_STATE_HOME/librepods/status.json`; the selected read-directory grant is a candidate for this published state, but path mapping and atomic replacement/watch behavior still need runtime tests. Controls call `librepods-ctl` through one pathname Unix socket, `XDG_RUNTIME_DIR/librepods.sock`. A raw socket grant would also expose daemon verbs `forget`, `disconnect`, `connect` and `reopen`, which the plugin deliberately does not offer. Evaluate a restricted control bridge for only its existing verbs rather than treating socket access as equivalent to panel permission. The daemon can remain outside the plugin sandbox; do not run a second daemon or replace the user's installed service during preparation.

Volume/output selection, Bluetooth connect/disconnect/forget, spatial audio and microphone mode are explicitly absent from the original plugin. Preserve its handoff to the stock Audio/Bluetooth panels; do not silently add these as new plugin controls or confuse them with missing sandbox features.

## Evidence and next implementation step

Baseline checks run on the unchanged checkouts on 2026-09-08:

- GitHub: `tests/helper-test.sh`, `tests/panel-source-test.sh`, `tests/service-source-test.sh` passed. The helper suite uses its own fake `gh`; no real account notification was marked read.
- Radio Atlas: `node tests/model.test.mjs` passed; `PYTHONDONTWRITEBYTECODE=1 python3 tests/proxy.test.py` passed all 10 tests. The full `tests/run` and real playback were not run at this checkpoint.
- AirPods: `deno run --allow-read tests/model.test.js` passed. The daemon build/hardware suite was not run, and no daemon or device controls were exercised.
- Omagotchi: source inventory only; no automated test entry point was found in this checkout. No live pet was loaded or changed.

These are original-source baselines, not sandbox compatibility evidence. No checklist item is checked off by those results. The storage API versus persistent-filesystem contract remains an explicit pending choice. Authentication, playback, package/window observations and AirPods control still need least-authority boundaries driven by the original calls, not replacement UIs or a general provider framework.

Private-display experiments then loaded the original plugins with one manifest-driven bootstrap and worker-local facades. GitHub exposed intercepted Escape and missing private layer focus; both host bugs now have regression coverage. Dynamic entries load from approved `file:///plugin/…` paths rather than unresolved `qs:` URLs. The GitHub six-control settings observation below predates the exact-key settings rewrite; it must be rerun with the canonical read/write key sets before serving as current-runtime evidence. The other observations remain no-grant preparation, not complete ports:

| Original implementation | Private-display observation | Explicit limit |
| --- | --- | --- |
| GitHub, no source changes | Original logged-out dashboard; bar open, search/typing, two-Escape behavior; all six settings-page controls save, reopen and survive private-host restart; denied saves revert visibly | No account or browser access; preference persistence does not prove authenticated filtering |
| Omagotchi, no source changes | Original service initializes; original egg, room and needs render; bar open and Escape close | Only ephemeral private state; no existing pet, package observations, audio or roaming proof |
| AirPods, no source changes | Original daemon-absent panel; bar open and Escape close, using its existing show-disconnected setting | No status-file grant, daemon control or hardware access |
| Radio Atlas, two bar call changes | Original globe and offline state render; bar toggles the retained original overlay open and closed | No station data, playback, middle-click tuning or keyboard proof |

All four captures were visually inspected on the private display. Each test revoked its own isolated admission afterward. Radio Atlas's original bar failed to open because its activation used `bar.run` command strings; replacing those with existing scoped `bar.shell.toggle` / `summon` calls made the same pointer test pass without changing the overlay, globe, manifest, helpers or playback isolation. Its new handler test fails against the original source and passes on the fork branch, checking both activation targets, the random payload and unchanged stop dispatch. These observations narrow the required source changes; they do not check off full-feature acceptance or substitute temporary state for persistent storage.

The bootstrap is now packaged as `native/ward/runtime/worker.qml` with shared host UI modules. The packaged GitHub experiment copies original source byte-for-byte, adding only `sandbox: { version: 1, requests: {} }` to its staged manifest. Initial/live palette and font-size updates now preserve typed search text; first Escape still clears search and second closes the original panel. Captures were visually inspected. The existing committed fixture also verifies detached own settings, retained service state, own-overlay payloads, cross-plugin/settings-write denial, click-through and revocation through the production host adapter. Only GitHub has been rerun against this packaged context-aware loader. No full-feature checklist is completed, and the active desktop remains unchanged.

Final acceptance must run all four on this real desktop after private preparation. Before activation, inventory the existing plugin installation/settings and owned processes without logging account or pairing secrets, stage recoverable state copies, define the exact restoration steps, and protect unrelated services. Record original versus sandboxed outcomes against every feature above, plus plugin diff size and shared runtime changes. Use controlled notification threads for destructive account actions; do not clear an actual inbox as a test. Hardware-dependent checks require the actual paired device and must distinguish unsupported model features from failures. Missing devices or unanswered resource decisions remain gaps, never grounds to mark the smaller demonstration complete.
