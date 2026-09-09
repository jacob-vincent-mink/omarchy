# Ward architecture (development preview)

Ward runs plugin QML outside the desktop shell and admits host access through explicit, revision-bound grants. This reference describes the implementation, not a completed security audit or a feature-parity claim. See the [authoring contract](sandboxed-plugin-authoring.md) for manifest syntax and runtime APIs, and the [implementation inventory](../plans/sandboxed-plugin-grants.md) for remaining resource work.

## Scope and trust

The intended boundary is third-party code distributed through the Omarchy plugin registry. Omarchy's first-party plugins remain trusted and in-process; users must retain an explicit trusted in-process installation path for their own code. A downloaded manifest cannot establish that trust. The current preview selects Ward with a manifest `sandbox` declaration or retained native config marker; registry provenance and mandatory registry-install routing are not implemented yet.

![Ward architecture: review authorizes a per-plugin controller; the existing shell exchanges bounded input and rendered pixels with a sandboxed worker through that controller.](images/ward-architecture.svg)

The dashed service boundary is a shared resource and lifetime boundary, not a filesystem sandbox. The worker's orange boundary is the sandbox. The controller, native Qt bridge, policy store, packaged runtime and relevant OS/graphics stack are trusted; plugin QML and its helpers are not.

## Admission and lifetime

1. `omarchy plugin review` validates and snapshots the bundle without running it. The private revision store identifies reviewed bytes by SHA-256; the editable checkout is not the launch source.
2. `approve --revision …` selects only requested resources. Required-but-unselected requests prevent activation; optional ones may be declined. Approval does not start a worker. The CLI and first-party review panel use the same native management path.
3. Enabling creates one host-owned `PluginSession`, with a `PluginView` importer per output. A host-side session thread launches one generated systemd user service and authenticates its controller connection using peer credentials, a pidfd and the service cgroup. The controller admits the stored identity, revision, epoch and unit before starting plugin code. Additional outputs and bar placements do not launch additional services or workers.
4. Revocation invalidates admission and stops owned work. Host connection leases and a systemd watchdog bound orphaned or stalled sessions. `stop` preserves approval; `disable` revokes it. Neither revocation nor process cleanup can undo completed writes or effects delegated to another service.

Approval records sign `{ id, revision, enabled, grants }` and verify it on reads. The signing key is held under the same desktop account: this detects unapproved record edits, but is not a boundary against a determined same-account process able to read the key. Runtime epoch/unit state is separate. A running controller enforces its admitted grant snapshot while rechecking live authority; grants cannot silently widen mid-session.

## Three execution domains

| Domain | Responsibility and boundary |
| --- | --- |
| Existing Quickshell desktop | Loads only Omarchy's native host component, never isolated plugin QML. Owns real layer-shell windows, bar slots, focus, dismissal and input clipping. Trusted first-party and explicitly trusted local plugins use the separate in-process path. |
| Per-plugin Rust controller | Runs a private Smithay Wayland compositor, grant brokers and worker supervision. Its systemd service limits the combined controller/worker/owned-job workload to 512 MiB memory, no swap, 128 tasks and 50% CPU, with no automatic restart. |
| Untrusted worker | A separate Quickshell process loads the reviewed plugin and packaged shared UI/runtime. Bubblewrap constructs its filesystem and namespaces; bootstrap applies Landlock and seccomp before loading plugin code. Ordinary QML `Process`, Bash and native modules remain inside this boundary. |

The worker receives a read-only `/plugin` bundle, restricted `/runtime`, private temporary paths and a selected GPU render node. Its Wayland connection is to its own controller, not Hyprland. No host session bus, PipeWire socket, compositor socket or desktop home is exposed by default. A media grant provides a filtered bus proxy, not the host bus. Landlock ABI 9 is required to prevent pathname Unix sockets inside granted directories from turning file access into host IPC; unsupported isolation fails closed.

## Pixels, input and context

The worker submits ordinary Wayland surfaces to the private compositor. Each admitted output has two bounded DMA-BUF presentation slots and one Qt importer in the existing shell. Versioned records validate output identity, topology epoch, dimensions, generations, frame serials, descriptor counts and input regions. Each stream has independent acknowledgements governing buffer reuse; one slow output cannot make another reuse an in-flight buffer. The host receives pixels and bounded state, not worker QObjects or executable QML.

Host-owned topology admits up to eight outputs, 4,096 logical pixels per axis, scales from 1 to 4 in units of 1/120, eight megapixels per output and 32 megapixels in aggregate (256 MiB for the two ARGB buffer sets, before other graphics/process memory). Output-local rendering preserves negative desktop origins, portrait geometry and mixed fractional scales without allocating pixels for desktop gaps. Output IDs never recycle within a session; topology epochs increase. Old input and presentation records cannot revive retired outputs. Importers detach before replacements are created, and retired buffer descriptors remain owned by their outstanding render nodes. Zero outputs removes presentation while retaining the logical service.

In the other direction, the host forwards only input delivered to the plugin's host item and focused keyboard events, including host-resolved key symbols. It owns real focus and clips interaction to allowed regions, including the plugin's own slot over the visible bar. Private layer-shell requests cannot reserve host screen space or independently acquire host focus.

A bounded, detached context carries theme, selected own settings, own-panel commands and up to 32 independent bar placements. One shared loader creates a widget per placement, with one service and one optional overlay. Omarchy owns one active own-panel presentation per plugin: clicks select the originating placement; keyboard/IPC summon uses the focused monitor, then an available configured output. Output loss dismisses its panel and clears held input. Unspecified private layer-shell outputs use the activated output; explicit worker output choices remain subject to host clipping.

Roaming pixels and input are restricted to the active owner output by default. An Omarchy-owned entry setting, `sandboxPresentation.overlayOutputs: "all"`, permits them across admitted outputs; plugin settings and manifests do not choose that policy. Bar regions remain clipped to this plugin's own slots on every output. This setting does not grant desktop observation.

The controller filters context by admitted grants before publishing a read-only snapshot. Optional `desktopGeometry` adds bounded output/workspace/window rectangles with opaque IDs; it adds no titles, app IDs, content or compositor control. A grant-filtered `geometryOutputs` association maps private presentation outputs to observed IDs. Both the observations and association disappear without the grant; required presentation topology does not.

## Resource paths

Resources take different paths; not every grant is a broker RPC:

- Filesystem slots are host-selected mounts under `/grants/<name>`. Storage binds one private per-plugin data directory at worker `$HOME`; without storage, home is temporary. Updates retain identity-based data, and revocation retains files while removing access. Persistent disk quota is not yet enforced.
- Notifications, own-settings writes, HTTP scopes and browser/webapp handoffs use bounded controller-owned request channels. HTTP does not inherit host credentials. Broad `network`, if explicitly selected instead of scoped HTTP, shares the host network namespace, including local services.
- Media uses a proxy restricted to the exact selected existing MPRIS player and supported operations. It does not provide general audio/device access or permission to publish a player.
- Host exec is an explicit exception: a selected argument-tree leaf admits a complete invocation of a reviewed executable. Verification seals matching executable bytes before launch; child cgroups supervise jobs and descendants. The CLI retains its host account, filesystem, network, libraries and configuration authority. Argument matching is not a semantic safety proof. Plugin-lifetime foreground jobs require separate review; ordinary requests have deadlines and all jobs retain concurrency/output bounds.

For approved host commands, `$OMARCHY_PLUGIN_PATH` identifies a temporary copy of reviewed assets and `$OMARCHY_PLUGIN_DATA` identifies the same directory mounted as worker home. These host paths are not additional worker mounts. See the [path reference](sandboxed-plugin-authoring.md#two-resources-not-four-unrelated-directories).

## Current limits and source map

The shared loader supports per-placement bar widgets with an optional own service/overlay. Protocol, real private-display mixed-DPI and two-output Qt host tests cover shared state, panel ownership, roaming policy and hotplug. These are not physical-monitor or installed-VM acceptance. Service-only readiness, final popup/input compatibility, default native packaging, disk budgets, full original-plugin acceptance and broader adversarial review remain release work. Missing native support or failed admission never falls back to loading isolated QML in-process.

| Area | Implementation |
| --- | --- |
| Review, revisions and grants | [`management.rs`](../native/ward/src/management.rs), [`revision.rs`](../native/ward/src/revision.rs), [`store.rs`](../native/ward/src/store.rs), [`grants.rs`](../native/ward/src/grants.rs) |
| Admission and process ownership | [`session.rs`](../native/ward/src/session.rs), [`supervisor.rs`](../native/ward/src/supervisor.rs), [`controller.rs`](../native/ward/src/controller.rs) |
| Worker restrictions and loader | [`worker.rs`](../native/ward/src/worker.rs), [`sandbox.rs`](../native/ward/src/sandbox.rs), [`worker.qml`](../native/ward/runtime/worker.qml) |
| Rendering, input and context | [`graphics.rs`](../native/ward/src/graphics.rs), [`topology.rs`](../native/ward/src/topology.rs), [`presentation.rs`](../native/ward/src/presentation.rs), [`pluginsession.cpp`](../native/ward/qt/pluginsession.cpp), [`pluginview.cpp`](../native/ward/qt/pluginview.cpp), [`context.rs`](../native/ward/src/context.rs) |
| Desktop ownership | [`SandboxedPlugins.qml`](../shell/services/SandboxedPlugins.qml), [`SandboxedPluginSession.qml`](../shell/services/native/SandboxedPluginSession.qml), [`SandboxedOutputSurface.qml`](../shell/services/native/SandboxedOutputSurface.qml), [`SandboxedBarWidget.qml`](../shell/services/SandboxedBarWidget.qml) |
| Resource effects | [`requests.rs`](../native/ward/src/requests.rs), [`exec_policy.rs`](../native/ward/src/exec_policy.rs), [`host_job.rs`](../native/ward/src/host_job.rs), [`http.rs`](../native/ward/src/http.rs), [`media.rs`](../native/ward/src/media.rs) |

Maintained synthetic coverage lives in [`native/ward/tests/`](../native/ward/tests/) and the [shell integration fixture](../test/shell.d/fixtures/ward-integration/). Concrete plugin trials are separate development evidence, not a substitute for containment or original-feature acceptance.
