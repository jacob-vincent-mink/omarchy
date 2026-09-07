# Secure plugin runtime POC

This POC adds an opt-in schema-v2 plugin runtime to Omarchy. Plugin QML runs in an isolated offscreen worker; the existing trusted Quickshell owns native windows and presents the worker's validated frames. External effects cross a permission broker. Schema v1 remains unsandboxed, trusted-by-default code inside the shell: none of the v2 guarantees apply to it, and rejected v2 plugins never fall back to v1.

The implementation is under `native/plugin-runtime/`. This is the reviewer explainer, not a production-readiness claim or a compatibility promise for existing plugins. Building or installing the versioned package does not activate it.

## Review map

Read from the trust contracts toward the shell integration. Tests live beside the boundary they exercise; `tests/support/` contains shared fixtures, not another runtime implementation.

| Area | Responsibility and useful starting points |
| --- | --- |
| Contracts | `contracts/manifest/` validates runtime documents; `contracts/capability-definitions/` resolves author requests against trusted definitions; `contracts/wire/` and `contracts/surface/` define bounded authenticated messages and frame/input layouts. `common/` contains shared codecs and resource ownership helpers. |
| Authority and effects | `discovery/` publishes immutable revisions. `host-session/` owns exact consent, SQLite authority, live-effect fencing, gestures, and `StructuredBroker`; `audit/` records broker decisions. `providers/` implements local effects. |
| Process isolation and providers | `contracts/sandbox/` and `launcher/` construct the worker boundary and supervise exact processes. `provider-host/` pins and supervises trusted external adapters. |
| QML worker | `worker/` enforces authenticated startup, exposes manifest-bound runtime calls, and renders Qt Quick scenes offscreen. `presentation/` is the worker's explicitly named QML SDK. |
| Session and presentation | `channel-integration/` composes reviewed sessions and owns lifecycle decisions. `surface-host/` and `trusted-bridge/` validate frame transport and physical input. `bridge/` projects native state into the trusted shell's `Omarchy.PluginHost` module. |
| Integration and delivery | `native/plugin-runtime/shell/` supplies package-owned surface wrappers. The existing shell registry and bar attach them only behind the v2 gate. `bin/omarchy-plugin-secure-install` and `bin/omarchy-plugin-permissions` expose terminal workflows. `packaging/` builds and verifies the inert versioned payload. |

## Trust boundary

The trusted shell never evaluates plugin QML. Each active v2 plugin gets a Bubblewrap-isolated Qt/QML worker with no host-home, Wayland, X11, session-bus, shell-IPC, SSH-agent, or network connection. Steady-state seccomp excludes arbitrary process and socket creation. Plugin-owned Qt Quick, JavaScript, assets, and pure-QML modules stay inside that boundary. The host validates descriptor ownership, message identity/order, allocation bounds, frame layouts, and input sequences rather than trusting worker-provided sizes or handles.

The host, kernel, Qt, Bubblewrap, systemd, SQLite, trusted capability definitions, and installed provider code are part of the trusted computing base. Filesystem ownership checks do not defend against arbitrary hostile code already running as the owning host UID. In particular, the same-UID Quickshell IPC socket is trusted session control, not proof of human consent; it remains outside the worker. The CLI's `interactive_cli` actor records ingress provenance, and its terminal prompts are an ergonomic confirmation.

Consent distinguishes two kinds of authority:

- `sandboxed-plugin`: a bounded effect enforced by its trusted capability definition and provider.
- `trusted-host-extension`: host command execution, including explicitly approved interpreters. The worker remains isolated, but the approved command can have the user's host authority.

The tier comes from the trusted enforcement definition, not a publisher's title or risk label. Unknown enforcement families classify conservatively as host extensions. Granting any host-extension row requires a separate acknowledgement bound to the exact native review fingerprint. Stale or missing acknowledgement rejects the complete publication; denying optional host rows does not require it. Host-package installation is a third, independent decision and cannot supply permission consent.

## From archive to authorized effect

1. Secure ingress validates a bounded archive and publishes an immutable revision. An archive contains either an exact schema-v2 `manifest.json` or a `manifest.author.json`, never both. Author requests declare inclusive `definitionVersions.minimum` and `maximum` generation bounds. The trusted installed registry resolves these before publication, retaining the author source and generated exact manifest together in the content hash. Resolution creates no grant; runtime authorization never resolves version ranges.
2. Native review binds the revision, exact definition generation/digest, requested operations, required/optional choices, scope, authority preimage, and trust tier. Review may narrow operations; a scope change needs fresh review. Unknown definitions, incompatible pins, malformed documents, and unavailable requested providers fail closed as appropriate to review or activation.
3. Authority publication compares the exact reviewed preimage transactionally. Activation rechecks the immutable revision, current definitions, grant, generation, and available adapter contract. Missing required granted providers prevent activation; optional grants can be temporarily ineffective without rewriting durable consent. A denied request never becomes granted merely because a provider appears.
4. QML invokes `runtime.invoke(capability, operation, arguments)`. The worker transmits the manifest-bound definition reference. The broker independently checks the authenticated activation, exact permission and operation, trusted gesture when required, and live provider contract, then supplies the reviewed scope itself. Arguments cannot replace scope or select another provider.
5. A live-effect lease fences dispatch. Revocation closes new admission before bounded outstanding work drains; replacement stops the old activation before starting a new generation. Stale replies and presentation callbacks cannot revive authority. Audit failure, malformed transport, and ambiguous persistence fail closed.

The nine built-in capability definitions are generated from one source into package-owned `capabilities.d` plus `metadata/capability-catalog-v1.json`. Exact runtime pins remain mandatory. Root-owned `/etc/omarchy/plugin-capabilities.d` can extend recognized contracts; it cannot shadow package definitions or load arbitrary code. Definitions bind authority identity, scope grammar, operations, trusted review text, gesture policy, adapter ABI, and semantic digest. Duplicate identities or equivalent adapter/operation aliases reject the catalog. A new enforcement family or scope grammar still requires code review.

## Authority persistence and session ownership

`AuthorityStore` owns a private SQLite database with STRICT relational records for exact revision/definition identities, scopes, choices, and full-width generation counters. One transaction compares the reviewed preimage, publishes the new head, and removes superseded rows. SQLite replaces the publication mechanism, not effect-admission fencing. Ambiguous errors poison the authority owner instead of permitting reuse of potentially stale grants.

The connection uses SQLite's Unix locking and rollback journal with `synchronous=EXTRA`. A restricted VFS path adapter anchors database and journal operations to a validated directory descriptor; SQLite still implements page I/O, locking, sync, and journal recovery. Both files must satisfy private ownership, regular-file, mode, and single-link checks. WAL, shared-memory sidecars, attached databases, and extension loading are excluded. Durability remains subject to SQLite's filesystem/sync assumptions, not a claim of tested physical power-loss recovery.

On first open, validated legacy records can be imported atomically. A durable cutover stamp in the existing private authority lock prevents later database loss from reimporting stale legacy grants. Once stamped, missing, empty, or unrecognized database state rejects startup. The old writer is removed; legacy encoding remains only for validated migration and exact digests. Development builds and staging do not open the installed authority store.

`RuntimeSessionOwner` owns catalog reconciliation, product phases, retries, delivery epochs, preparation/permission jobs, bounded completions/intents, and session resources. `PluginRuntimeController` forwards Qt requests and implements the trusted host projection port; it does not own another lifecycle machine. `PluginSession` directly owns reviewed commit, exact surface access, and asynchronous worker/provider composition. Its I/O queue delivers through that session's existing endpoints, without a separate delivery interface or observer hierarchy.

Product delivery epochs, durable authority generations, queue fences, and authenticated wire identities have different jobs and remain distinct. Blocking work runs off the UI thread. The runtime withdraws Qt presentation resources before their session port is destroyed and rechecks exact binding after reentrant publication. Destruction closes live effect admission before queued transport cleanup. Transport reports bounded operation results; it does not approve consent or choose product retries.

## Providers and host commands

Local private storage stays descriptor-rooted; notifications are host-labelled. Both use the same `StructuredBroker` authorization, audit, and settlement path as external adapters. Built-in dispatch is synchronous and bounded; there is no generic asynchronous provider-cancellation or broker-handle API. Reserved wire cancellation IDs are not supported product operations.

External profiles come only from package-owned `providers.d` and root-owned `/etc/omarchy/plugin-providers.d`. Profiles pin the exact adapter tuple, executable digest, arguments, environment allowlist, timeout, and process group identity. The catalog opens and hashes the executable descriptor; `execveat` prevents pathname replacement from retargeting launch. Scripts and shebang wrappers are not provider executables. Catalog loading and review start no provider process.

The first authorized call starts one process per activation/profile group after verified systemd-scope attachment with resource ceilings. A bounded descriptor-free `SOCK_SEQPACKET` protocol checks version, correlation, scope, and reply size. Timeout, crash, malformed output, or wrong correlation permanently fails that process for the activation. Teardown retains exact scope/pidfd authority until cleanup is verified; unresolved cleanup is never reported successful. These are trusted host adapters with specific confinement, not plugin-supplied workers.

Packaged external effects are deliberately specific:

- Network fetch accepts granted HTTPS origins/methods, rejects fetch redirects and non-public target addresses, and exposes bounded response data. Media playback accepts activation-bound opaque source handles; every stream connection and bounded redirect is checked for public addresses. Playback runs in a second namespace with no network and only its PipeWire connection.
- Desktop opening requires a fresh trusted gesture and an exact granted HTTPS origin. Fixed `xdg-open` or Chromium launch paths provide browser-tab or web-app-window presentation; the plugin cannot choose an executable or arbitrary browser arguments.
- System observation exposes fixed package-summary counts or sanitized output/window rectangles. It does not return raw command output, package names, window titles/classes, process IDs, or compositor addresses. Only this profile inherits the two required compositor-discovery environment variables.
- `bash.execute/run` enforces manifest-declared command names, absolute executable paths, complete argv rules, fixed environments, account-home access, and output/time limits. `runtime.execute("bash", command, arguments)` is shorthand for this manifest-bound request, not a shell-text parser.

Command argv rules do not constrain a program's internal behavior or pin mutable script contents. Explicitly approving `sh -c`, Python code, configuration-dependent tools, or broad patterns can expose files and accounts. Executable selection requires a root-owned, non-symlinked, non-set-ID path; execution uses a pinned descriptor, no PATH lookup, working directory `/`, bounded captured output, and no inherited shell environment. `accountHome: true` permits account credential use. There is no second administrator command-policy registry: exact user consent to the manifest scope is the authority.

Schema-v2 `dependencies.aur` names are separately reviewed host-package requests. Terminal installation/review asks before invoking Omarchy's AUR package helper outside the sandbox; graphical review does not install them. AUR build/install scripts have host authority and can require administrator privileges. Declining or failing leaves permissions unchanged. Installed packages remain after cancellation, revocation, or plugin removal and are managed normally; installation alone grants no command execution.

## Presentation and migration boundary

The certified native-QML closure is `QtQml`, `QtQuick`, Basic-style `QtQuick.Controls`, `QtQuick.Effects`, `QtQuick.Layouts`, and `QtQuick.Shapes`. Exact read-only module closures and trusted resource namespaces are admitted; representative Controls types preload before steady-state seccomp. Other styles, `QtQuick.Dialogs`, platform integrations, and host theme discovery remain absent. Controls `Dialog`/`Popup` are offscreen scene objects, not native platform dialogs.

`Omarchy.PluginPresentation 1.0` is a pure-QML SDK embedded in the worker. It supplies controls, panel/keyboard primitives, bounded host-projected theme/sizing values, and a private-storage helper. Helpers needing effects call the same visible manifest-bound `runtime` API. It is not the trusted shell's `qs.Commons` or `qs.Ui`; it exposes no compositor, shell process, D-Bus, or ambient host-file API. Permission projections (`runtime.permissions`, `hasPermission`, `permissionState`) help presentation but never authorize effects.

There are no standard `Quickshell` URI facades. `PanelWindow`, `PopupWindow`, native `Region` masks, and Wayland attached policies cannot honestly become worker Items: they carry window placement, focus, lifecycle, or compositor authority. Each presented bar/panel/overlay is a manifest-declared surface with a `QQuickItem` entry root. The host owns windows, placement, layer, focus, visibility, dismissal, and lifetime; the worker owns its scene, bounded `inputRegions`, and manifest-bound surface intents.

An intent's optional output name must exactly match a connected host screen; otherwise the host chooses the source, focused, or first screen. Only desktop overlays may request `initiallyVisible`. Trusted physical pointer/touch input or a spontaneous non-repeating key press on an already gesture-focused surface can carry one-use gesture authority. Synthesized events, repeats, key releases, input-method commits, and unfocused surfaces cannot mint it.

The four separately maintained reference ports—GitHub, Radio Atlas, Omagotchi, and AirPods—exercise these explicit migration boundaries. They are not shipped by this repository or package. Full Quickshell embedding, sandbox-local helpers, general third-party provider enrollment, and source compatibility with v1 are outside this POC.

## Build, test, and package

Use the system Qt supplied by Omarchy. The touch injector uses Qt's private QPA API, so an incompatible Qt update can require a runtime rebuild; the experimental package deliberately does not pin Qt versions or block system updates. Required native development dependencies are declared by CMake; Arch runtime dependencies are fixed in `packaging/runtime-dependencies-v1.txt`.

```bash
cmake -S native/plugin-runtime -B build/plugin-runtime -G Ninja -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON
cmake --build build/plugin-runtime -j2
ctest --test-dir build/plugin-runtime --output-on-failure
./test/shell
```

For ASan/UBSan, configure a separate build with `-DOMARCHY_PLUGIN_SANITIZERS=ON`, retain leak detection, and exclude only `plugin-sandbox-enforcement` from that sanitizer run: its intentional syscall filter conflicts with sanitizer runtime behavior. Run that test natively. The fakeroot ownership variant is native-only. The opt-in `plugin-neutral-surfaces-real-bwrap` test requires the correctly installed package and explicit `OMARCHY_REQUIRE_PACKAGED_WORKER_TEST`; a default skip is not packaged-worker verification. Graphical acceptance tests belong in a disposable VM; visual changes also require running-UI verification under the repository's visual-verification guide.

Stage and validate without installing into the live system:

```bash
stage=$(mktemp -d)
sudo chown root:root "$stage"
sudo chmod 755 "$stage"
sudo env DESTDIR="$stage" cmake --install build/plugin-runtime
sudo native/plugin-runtime/packaging/verify-package.sh --staging "$stage" 0.1.0
sudo native/plugin-runtime/packaging/verify-package-test.sh "$stage" 0.1.0 build/plugin-runtime
```

`packaging/arch/build-package.sh <output-directory>` archives the committed runtime tree and builds without installing it. `test-package-lifecycle.sh <archive>` verifies install/removal in a disposable pacman root; `test-package-reproducibility.sh` compares clean-build installed payloads and normalized metadata. The outer archives need not be byte-identical because makepkg records build-directory context. The exact payload/mode allowlist is `packaging/package-manifest-v1.txt`.

The package installs side-by-side under `/usr/lib/omarchy/plugin-security/0.1.0`, with no PATH command, systemd unit, global QML import, shell configuration, plugin, or permission grant. `/usr` and the versioned library root are fixed; use `DESTDIR` for staging, not a different prefix.

Activation is an explicit development overlay: the shell process must receive `OMARCHY_PLUGIN_V2_ENABLED=1`, `OMARCHY_PLUGIN_V2_SHELL_ENTRY=/usr/lib/omarchy/plugin-security/0.1.0/shell/SecurePluginHost.qml`, and the versioned `qml` directory on its import path. Removing the overlay and restarting deactivates v2. Do not infer authorization to alter a live session or migrate installed authority from a successful build.

## Verification status and remaining limits

Each review block was configured and built in Release mode with tests enabled. The complete finalization native suite passed 81 tests with one opt-in skip; ASan/UBSan passed 79 with leak detection enabled, one opt-in skip, and the native-only exclusions above. CLI metadata/routing and the focused plugin shell suites passed, including the custom-bar regression in a temporary compositor-connected shell. The committed Release archive built with tests disabled and passed archive metadata verification; six metadata regressions cover the required SQLite dependency and mismatched dependency declarations. Root-owned staged-package verification including negative cases passed before finalization; its repeat ownership/lifecycle verification remains unverified without administrator authentication. The four reference-port contracts and eleven offscreen QML cases passed in the preceding compatibility check. These are development results, not an independent security audit or a claim of production readiness.

An intermittent launcher descriptor-injection test failure occurred during development and passed on later complete runs; its cause remains unresolved. Keep it visible during repeated/concurrent testing rather than claiming it was fixed. The reference Omagotchi fixture also has an existing missing-sprite warning. No installed runtime, live authority, or desktop configuration was changed by this finalization.

Review should concentrate on effect-time authorization and revocation ordering, exact descriptor/process ownership, untrusted message bounds, physical-input provenance, SQLite cutover recovery, and how clearly consent communicates host-extension authority. Further feature expansion or release deployment needs its own review and verification; this POC intentionally stops short of those claims.
