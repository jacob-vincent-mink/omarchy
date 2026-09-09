# Original plugin compatibility proof

This is the source baseline and acceptance ledger for the four actual competition selections. It is not a claim that any of them currently runs sandboxed. The requirements supplement [the runtime plan](sandboxed-quickshell.md); smaller replacements and previous schema-v2 ports do not satisfy them.

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

The current native activation surface (`shell/services/native/SandboxedPluginSurface.qml`) is one nonexclusive top-layer canvas on the first screen. It is not a bar slot. None of these plugins has a `sandbox` declaration or worker entry point. Required shared integration still includes real bar placement and sizing, worker-local access to its own service, detached settings and style updates, panel switching, opening/closing without stopping the service, and correct popup anchoring. Host QObject access must not cross the isolation boundary.

The separate graphics work owns fractional scaling and output changes; coordinate requirements discovered here must feed that shared contract rather than create a second protocol. Omagotchi needs output identity, origin, reserved edges and window geometry as well as a drawable viewport. Radio Atlas needs a screensaver lifecycle signal. Neither need implies permission to expose the raw Hyprland command socket.

## GitHub

Sources: `README.md`, `manifest.json`, `Panel.qml`, `Service.qml`, `omarchy-github-fetch`, and `tests/` at the pinned commit.

- [ ] Preserve all sections: notifications, review requests, authored PRs and check rollups, assigned issues, active Actions, recent failures, and repository metrics.
- [ ] Preserve complete pagination, owned/organization scope, archived/fork/draft filtering, search across the full fetched list, all metric filters and sorts, rendered-row bounds, and activity expansion/notification paging.
- [ ] Preserve Actions Off/Recent/All modes, concurrency and repository limits, failure window/count, refresh interval, and unlit-icon setting.
- [ ] Preserve individual notification reads and the two-step, expiring bulk confirmation. Keep the captured list boundary and same-second ID handling; do not broaden the bulk operation to unseen arrivals. Preserve failure recovery and refresh after each attempt.
- [ ] Preserve direct links in both browser-tab and web-app modes, all documented keyboard and bar mouse controls, settings-page transitions and persisted inline settings.
- [ ] Preserve loading, logged-out, missing-dependency, rate-limit, partial-result and error behavior. Use existing `gh` authentication without adding a token setup workflow or exposing credentials to the presentation code.

The original helper already implements the domain behavior through `gh api` and `jq`; retain it as the first candidate. `Panel.qml:182` launches host browser helpers, and `Panel.qml:199` writes the widget's inline settings through `bar.shell.updateEntryInline`. These need bounded host operations, not a writable `shell.json` grant or arbitrary host execution. The private worker has neither host credentials/keyring access nor browser launching today. A read-only credential directory alone does not cover keyring-backed `gh` authentication. How the existing helper reaches approved authentication remains an explicit resource-design gap, not a reason to replace the dashboard.

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

The service runs independently of the bar and creates the roaming window even when hidden. `BarWidget.qml:12` obtains only its own service via `bar.shell.serviceFor`. `Service.qml` directly runs `checkupdates`, `pacman -Qdtq`, `pw-play` and notifications and writes two files under `XDG_STATE_HOME/omarchy`. The original `checkupdates` fetches repository databases; replacing its result with zero silently removes behavior. The current worker provides neither host package state nor audio output. `RoamWindow.qml:55-100` needs monitor/workspace state and visible window rectangles with stable identity, not titles, application secrets or compositor control. Persistent storage remains unresolved; a fresh temporary pet is not parity.

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

These are original-source baselines, not sandbox compatibility evidence. No checklist item is checked off by those results. The next implementation step is the shared worker-local UI/bar bootstrap using an original entry point, then its actual host resources; do not build four replacement UIs. The storage API versus persistent-filesystem contract remains an explicit pending choice. Authentication, playback, package observations, window observations and AirPods control need similarly explicit, least-authority boundaries driven by these actual calls, not a general provider framework.

A subsequent private-display experiment loaded the original GitHub `Panel.qml`, `Service.qml` and helper byte-for-byte with unchanged `qs.Ui`/`qs.Commons`, a worker-local `PluginBarApi`, staged manifest/bootstrap and no grants. The Octocat and logged-out dashboard rendered and the bar button opened the original panel. This exposed two host bugs: `PluginView` intercepted Escape, and the private compositor did not give the newly mapped KeyboardPanel its requested exclusive focus. Focused regression tests reproduced both failures. With host key forwarding and host-activation-gated private layer focus, the unchanged original now passes bar opening, `/` to search, typing, first Escape clearing search while retaining the panel, and second Escape closing it, without an additional panel click. Search, cleared and closed captures were visually inspected. The experiment remains outside the repository and is not a port or live-desktop result. Theme synchronization, real bar placement, authentication, host links and settings persistence remain unimplemented.

The same experiment now uses one manifest-driven bootstrap for all four plugins, reusing unchanged `PluginBarApi`, `PluginShellApi`, `qs.Ui` and `qs.Commons` inside the worker. Its local facade provides own-service lookup and own-overlay lifecycle only; no host QObject or command executor is injected. Dynamic entry points must load from their approved `file:///plugin/…` paths: resolving an unimported plugin directory through Quickshell's rewritten `qs:` URLs failed in the probe. This bootstrap is still experimental and uncommitted, not an installed runtime module or a settled host-context interface.

| Original implementation | Private-display observation | Explicit limit |
| --- | --- | --- |
| GitHub, no source changes | Original logged-out dashboard; bar open, search/typing, first Escape clears and second closes | No account, browser or settings-write access |
| Omagotchi, no source changes | Original service initializes; original egg, room and needs render; bar open and Escape close | Only ephemeral private state; no existing pet, package observations, audio or roaming proof |
| AirPods, no source changes | Original daemon-absent panel; bar open and Escape close, using its existing show-disconnected setting | No status-file grant, daemon control or hardware access |
| Radio Atlas, two bar call changes | Original globe and offline state render; bar toggles the retained original overlay open and closed | No station data, playback, middle-click tuning or keyboard proof |

All four captures were visually inspected on the private display. Each test revoked its own isolated admission afterward. Radio Atlas's original bar failed to open because its activation used `bar.run` command strings; replacing those with existing scoped `bar.shell.toggle` / `summon` calls made the same pointer test pass without changing the overlay, globe, manifest, helpers or playback isolation. Its new handler test fails against the original source and passes on the fork branch, checking both activation targets, the random payload and unchanged stop dispatch. These observations narrow the required source changes; they do not check off full-feature acceptance or substitute temporary state for persistent storage.

The shared bootstrap has since moved into `native/plugin-host/runtime/worker.qml` with CMake staging of the existing host UI modules. A new packaged-runtime GitHub experiment copied its original source byte-for-byte, adding only `sandbox: { version: 1, requests: {} }` to the staged manifest: no plugin-local worker or vendored host modules. Against the staged controller/runtime it again opened the original dashboard, typed search, cleared it on first Escape and closed on second Escape; all three captures were visually inspected. A committed original-contract fixture additionally tests retained own-service state, own-overlay payloads, denied cross-plugin lookup/settings writes, transparent-area click-through and revocation. The earlier four-plugin observations above remain experimental; only GitHub has been rerun against the packaged loader at this checkpoint. No full-feature checklist is completed by this step, and the active desktop remains unchanged.

Final acceptance must run all four on this real desktop after private preparation. Before activation, inventory the existing plugin installation/settings and owned processes without logging account or pairing secrets, stage recoverable state copies, define the exact restoration steps, and protect unrelated services. Record original versus sandboxed outcomes against every feature above, plus plugin diff size and shared runtime changes. Use controlled notification threads for destructive account actions; do not clear an actual inbox as a test. Hardware-dependent checks require the actual paired device and must distinguish unsupported model features from failures. Missing devices or unanswered resource decisions remain gaps, never grounds to mark the smaller demonstration complete.
