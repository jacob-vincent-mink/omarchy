# Ward plugin authoring (preview)

Ward is Omarchy's isolated plugin security system. This is the developer and coding-agent reference for its sandbox currently implemented on this branch, not the trusted in-process plugin contract. Native payload installation, real bar slots and final desktop compatibility gates remain unfinished; see the [development preview](omarchy-shell.md#sandboxed-plugin-development-preview) and [implementation inventory](../plans/sandboxed-plugin-grants.md). Proposed shortcuts are not accepted syntax until implemented.

Ward's product scope is third-party plugins distributed through the Omarchy plugin registry. Omarchy's own first-party plugins remain in-process, and users may explicitly choose the trusted in-process path for their own plugins. Ward is not a restriction on that choice. Trust must come from the installation decision, not a downloaded manifest claiming to be first-party. This preview still selects Ward through the `sandbox` declaration; registry provenance and mandatory registry-install routing are not yet implemented.

Ward's source and tests use synthetic plugins and generic resource fixtures. Compatibility checks for concrete third-party plugins belong outside `native/ward`; first-party plugins are not sandbox-port targets.

## Start here

A plugin is a Git repository containing `manifest.json`, QML and its scripts/assets. Add `sandbox` to select isolation. Review snapshots the repository; approval binds that revision. Later checkout edits do not change approved code. There are no install scripts, build hooks, post-approval hooks or manifest-driven data-copy hooks. Include already-built assets before review.

```json
{
  "schemaVersion": 1,
  "id": "example.pet",
  "name": "Example Pet",
  "version": "1",
  "kinds": ["bar-widget"],
  "entryPoints": { "barWidget": "Widget.qml" },
  "barWidget": { "name": "Example Pet", "category": "custom", "defaultSection": "right", "allowMultiple": false },
  "sandbox": {
    "version": 1,
    "requests": {
      "storage": true,
      "notifications": true,
      "settings": { "read": ["volume"], "write": ["volume"] }
    }
  }
}
```

The shared loader requires exactly one `bar-widget` kind with `entryPoints.barWidget`; an own `service` and `overlay` may also be declared with matching keys. These entry points are QML `Item`s. A custom `sandbox.entryPoint` is a separate development route for a complete worker QML configuration, responsible for its own windows and lifecycle. Other shared-loader kinds are rejected.

The outer manifest follows the [ordinary manifest reference](omarchy-shell.md#plugin-manifest). All entry-point values are safe paths **relative to the repository root**, such as `Widget.qml` or `ui/Overlay.qml`: no absolute paths, variables, URLs or traversal. Files must exist. Reviewed bundles are limited to 64 MiB, 4,096 entries and 16 directory levels; symlinks, hardlinked files and special files are rejected. Root `.git` data is excluded.

## Two resources, not four unrelated directories

The plugin owns two resources: **the bundle it ships** and **the data it writes**. Each has a path for local sandboxed code and a path usable by an explicitly approved host command. The current `OMARCHY_PLUGIN_*` variable names obscure that distinction: they are host-command paths, not local data/asset roots.

| Resource | Local sandbox path | Host-command path | Relationship |
| --- | --- | --- | --- |
| Shipped bundle: QML, scripts, images, sounds | `/plugin` (read-only) | `$OMARCHY_PLUGIN_PATH` | Omarchy automatically stages a host-visible copy of the approved bundle when exec access is granted |
| Mutable data: saves and generated files | `$HOME` (`/home/plugin`) | `$OMARCHY_PLUGIN_DATA` | With storage granted, these name **the same directory**, mounted under different paths—not two copies |

### DATA is the host path of the sandbox's home

With storage granted, Omarchy sets `OMARCHY_PLUGIN_DATA` to the host directory `$XDG_STATE_HOME/omarchy/plugins/<id>` (default `~/.local/state/omarchy/plugins/<id>`), and mounts **that very directory** at `/home/plugin` inside the worker. The worker's `HOME` is `/home/plugin`; it is never the desktop user's home.

These are two ways to address one saved file:

```text
Sandbox code:   $HOME/save.json
               /home/plugin/save.json

Host command:  $OMARCHY_PLUGIN_DATA/save.json
               <host state home>/omarchy/plugins/<id>/save.json
```

Write it using `$HOME` inside the sandbox. Only if a host command needs that file do you pass the corresponding DATA path through an approved exec request. There is no copy, synchronization or extra staging step between HOME and DATA. Authors must not construct the machine-specific host storage path themselves.

These paths are not audio-specific. Any approved host command can receive a shipped file through PATH or a saved/generated file through DATA, provided its selected exec leaf admits that argument. For example, an image tool could read a generated image through DATA; a player could read a shipped sound through PATH. The distinction is the file's owner and lifetime, not which command reads it.

Without storage granted, `$HOME` instead names a fresh 32 MiB temporary filesystem and `OMARCHY_PLUGIN_DATA` is absent. Successful writing therefore does not prove persistence: check `grants.storage`. A plugin that stores data entirely inside its own sandbox **still requests storage** when that data must survive restart; it does not use `/plugin` for persistence.

Storage is keyed by plugin id, not revision. Updates preserve saves; revocation removes access but retains data; regranting reconnects the same identity to those files. The host directory has mode 0700. No persistent disk quota is enforced yet; bound caches and remove obsolete generated files yourself. Existing data from an unsandboxed plugin is not automatically imported from the desktop user's home.

Worker-local XDG suffixes are preserved exactly: `$HOME/.local/state/pet/save.json` corresponds to `$OMARCHY_PLUGIN_DATA/.local/state/pet/save.json`, **not** `$OMARCHY_PLUGIN_DATA/save.json`.

### When an author uses `/plugin`

`/plugin` is where sandboxed code reads the approved bundle. **It is never writable storage.** You often need not spell it out in QML: `Qt.resolvedUrl("assets/pet.png")` resolves relative to the QML file, which itself lives under `/plugin`. Use an explicit `/plugin/...` path when a bundled script or worker-local tool needs a bundle-root-relative filename.

For example, `["/bin/bash", "/plugin/scripts/save.sh"]` starts a bundled script inside the sandbox. The script might read `/plugin/defaults/save.json` and write `$HOME/save.json`. Neither action is a host exec request. Ordinary Quickshell `Process` calls and bundled native modules inherit the sandbox restrictions.

For Omagotchi's host-side `pw-play`, `/plugin/sounds/chime.wav` would be the wrong argument: `/plugin` is a mount in the worker's filesystem, not in the host command's filesystem. Pass `$OMARCHY_PLUGIN_PATH/sounds/chime.wav` instead. Omarchy stages those shipped sounds automatically; no storage grant or author-written DATA initialization is needed for that playback.

`OMARCHY_PLUGIN_PATH` exists when exec access is admitted. Its value names a fresh controller-owned copy of the reviewed bundle, not the editable checkout. Removed assets do not carry over from another revision, and the service manager removes temporary staging when the controller stops, including abnormal exits. Never cache this host path across launches.

Both `OMARCHY_PLUGIN_*` variables contain host-side absolute paths. Their values are **not additional mounts accessible to local `FileView`, `cp`, `open()` or QML image loading**. To create a save path in QML use `Quickshell.env("HOME") + "/save.json"`; in Bash use `"$HOME/save.json"`. JSON and QML strings do not perform shell variable expansion.

### Paths in the manifest

Entry points are repository-relative: `"barWidget": "ui/Widget.qml"`, not `/plugin/ui/Widget.qml` and not a variable. Shipped assets need no filesystem grant. For persistent saves request `storage`, with no data-directory path in the manifest. Only exec argument constraints use `$OMARCHY_PLUGIN_PATH/...` or `$OMARCHY_PLUGIN_DATA/...`, because those constraints describe arguments to a host command.

Access to unrelated user files is separate: request a named `filesystem` slot, let the user choose its host directory, then read it locally at `/grants/<slot>/...`. That mount path has no automatic host-command translation. `/tmp` and `$XDG_RUNTIME_DIR` (`/run/plugin`) are temporary. File access does not authorize connecting to host pathname sockets in saved data or selected folders.

### How files get into DATA

There is no separate DATA staging action. The host creates the persistent directory when a storage-granted worker launches and mounts it at `$HOME`. Anything the plugin writes there is immediately in the same directory addressed by `$OMARCHY_PLUGIN_DATA` for host commands.

Leave read-only defaults in the bundle. If a mutable copy is needed, initialize it on first use inside the worker without overwriting an existing save:

```bash
#!/bin/bash
set -euo pipefail

if [[ ! -e $HOME/save.json ]]; then
  cp -- /plugin/defaults/save.json "$HOME/save.json"
fi
```

Initialize once in the owning service before concurrent writers start. For ongoing updates use atomic replacement and a data-format version; never copy defaults over retained saves on every update. Request `"storage": { "required": true }` if operation without persistence would be misleading. With optional storage, handle a declined grant: disable persistence-dependent features or explain that files are temporary.

Writing locally needs no exec grant. Only a host command consuming the file needs its own selected exec permission. Do not copy the bundle into DATA merely for host asset access; PATH staging serves that purpose already.

### Path tokens in exec declarations and calls

The exec matcher substitutes only the two plugin path tokens. It resolves candidate arguments and `exact.value`, `oneOf.values` and `text.prefix` constraints. It does not interpolate regex patterns or arbitrary manifest fields. `$HOME`, `${OMARCHY_PLUGIN_DATA}`, `~`, command substitutions and embedded `--file=$OMARCHY_PLUGIN_DATA/x` are not this token syntax. Supply a path as its own argument beginning with the exact `$OMARCHY_PLUGIN_DATA` or `$OMARCHY_PLUGIN_PATH` token.

Prefer literal tokens so plugin code need not know a machine-specific host path:

```bash
/bootstrap --exec player --volume 0.5 '$OMARCHY_PLUGIN_PATH/sounds/chime.wav'
```

The single quotes deliberately prevent Bash expansion. In a QML `Process.command` argv array use `"$OMARCHY_PLUGIN_PATH/sounds/chime.wav"`. Reading `Quickshell.env("OMARCHY_PLUGIN_PATH")` and appending the suffix also works, but only as an exec argument. Neither form grants authority; the entire argv must match a selected leaf. A host command's own `$HOME` is the desktop user's home, not `/home/plugin`.

Token-relative `.`/`..`, empty components and traversal are rejected. This lexical check is not a host-filesystem sandbox or a promise about symlinks created in writable data. An approved CLI retains its own account, configuration, filesystem and network authority; constrain arguments accordingly. Prefer exact shipped filenames to open-ended text prefixes. Host asset staging is an implementation detail, not a stable path to cache across launches.

## Sandbox manifest schema

This is the complete current `sandbox` request vocabulary. Unknown fields inside `sandbox`, requests and request objects are rejected. Omitted request fields default to denied/not requested. Declaration is not approval. The serialized manifest and saved approval are each limited to 64 KiB, which can bind before count limits.

`sandbox` contains `version: 1`, required `requests: { ... }`, and optional `entryPoint`. Atomic requests use `true` for **optional**, `false` for not requested, or `{ "required": true }` for required. `{}` and `{ "required": false }` also request optional access. Required access is still user-selected; missing required grants prevent activation.

| `sandbox.requests` key | Accepted shape | Meaning and selection |
| --- | --- | --- |
| `storage` | Atomic request | Persistent private home; `--allow-storage` |
| `network` | Atomic request | Broad host network namespace, including local services; `--allow-network`. Cannot be granted together with scoped HTTP |
| `notifications` | Atomic request | Text-only notifications; `--allow-notifications` |
| `openUrls` | Atomic request | HTTP(S) browser/webapp handoff; `--allow-open-urls`. Can transmit data without worker networking |
| `media` | Atomic request | Filtered access to one existing MPRIS player; user selects exact `org.mpris.MediaPlayer2.Name` with `--media` |
| `filesystem` | Array of `{ "name": "notes", "access": "read", "required": false }` | Up to 256 unique slots. `access` defaults to `read`; `readwrite` permits both. `write` alone is rejected. Select with `--read notes=/folder` or `--write notes=/folder` |
| `settings` | `{ "read": ["volume"], "write": ["volume"], "required": false }` | Independent exact top-level keys of own inline settings; `--read-setting` and `--write-setting`. Lists default empty, combined limit 256 |
| `http` | Names mapped to `{ "scope": { ... }, "required": false }` | Up to 32 scopes; individually selected by `--http name` |
| `exec` | Names mapped to `{ "executable": "/usr/bin/tool", "tree": { ... }, "required": ["leaf"] }` | Up to 16 installed executables; selected by `--exec name:leaf`. `required` defaults empty and lists required terminal leaves |

Identifiers for resource names, settings and leaves are 1–96 ASCII characters: start with a letter or digit, then letters, digits, `.`, `_` or `-`; `..` is forbidden. Settings additionally exclude `id`, `sandbox`, `__proto__`, `constructor` and `prototype`. Each key covers its whole JSON value, not a nested path. Read does not imply write, nor write imply read. There is no all-settings wildcard, system-settings namespace, application-account provider or arbitrary host-command permission.

### Exec tree schema

This is current syntax, not the proposed flat argv form. A node has optional `end` (a unique leaf name) and `next` (array, default empty). A step is `{ "arg": <constraint>, "then": <node> }`. A selected `end` permits the complete argv ending there only, not trailing arguments or descendants. Empty dead-end nodes are invalid.

| Matcher | Shape and semantics |
| --- | --- |
| Exact | `{ "kind": "exact", "value": "status" }`: one literal argument |
| Alternatives | `{ "kind": "oneOf", "values": ["0.25", "0.5"] }`: 1–32 distinct literals |
| Integer | `{ "kind": "integer", "min": 1, "max": 100 }`: unsigned 64-bit range, decimal digits without sign or leading zero except `0` |
| Text | `{ "kind": "text", "prefix": "query=", "min": 6, "max": 128 }`: literal prefix and UTF-8 byte bounds on the **whole** argument |
| Pattern | `{ "kind": "pattern", "value": "/threads/[0-9]+", "max": 64 }`: bounded whole-argument regex, not a substring |

Limits: 32 arguments, 8 KiB per argument, 64 KiB combined argv, 512 nodes and 64 KiB serialized tree. NUL is rejected. Regex source is capped at 4 KiB, nesting at 32 and compiled automata at 64 KiB; backreferences/lookaround are unsupported. Text bounds with a path prefix apply to the final resolved host path, not the token. This complete exec request permits one shipped sound:

```json
{
  "executable": "/usr/bin/pw-play",
  "tree": {
    "next": [{
      "arg": { "kind": "exact", "value": "--volume" },
      "then": { "next": [{
        "arg": { "kind": "exact", "value": "0.5" },
        "then": { "next": [{
          "arg": { "kind": "exact", "value": "$OMARCHY_PLUGIN_PATH/sounds/chime.wav" },
          "then": { "end": "chime" }
        }] }
      }] }
    }]
  }
}
```

Place it at `sandbox.requests.exec.player`; select with `--exec player:chime`. Executable paths must be absolute, contain no traversal and resolve to an installed regular executable. Symlink aliases are supported: the declared path remains the invocation name (`argv[0]`), while approval pins the resolved executable's bytes. Each launch resolves the path again and executes a sealed copy only if its bytes still match; changing the binary requires reapproval. This does not pin libraries, configuration or descendants. The matcher cannot infer CLI safety. Never permit arbitrary shell programs, GraphQL documents or CLI flags merely to shorten a declaration.

Host jobs preserve host user/group mappings and run a sealed executable copy inside a child cgroup under the controller's resource limits. They do not add a filesystem or user-namespace sandbox to the approved CLI. For a shebang script, the interpreter receives a `/proc/self/fd/...` script path, so scripts that locate adjacent files through `$0` need explicit paths. Normal exit, cancellation and owner death terminate remaining job-group processes; cleanup waits for the kernel's empty-group event with a one-second bound. Effects handed to another host service are still outside that cleanup.

### HTTP scope schema

Each scope requires `origin`, `method`, `path`; optional `subtree` defaults false, `query` defaults empty, `body` absent/null means no body. `origin` is one canonical HTTP(S) origin without trailing slash. Methods: `GET`, `HEAD`, `POST`, `PUT`, `PATCH`, `DELETE`, `OPTIONS`. GET/HEAD cannot declare a body.

`path` is an exact canonical absolute URL path, optionally containing `*` for one nonempty segment. For a subtree set `subtree: true` with a trailing-slash path and no `*`. Percent-encoded paths, ambiguous normalization, credentials and fragments are unsupported. Query values may be percent encoded; matching compares decoded strings.

`query` maps keys to `{ "required": false, "value": <field> }`. Unknown/duplicate keys are denied. Query fields permit only exact strings or bounded strings. `body` maps keys to fields; supplied JSON must have exactly those keys, with no implicit extra fields.

| HTTP field | Shape |
| --- | --- |
| Exact JSON | `{ "kind": "exact", "value": <any JSON value> }` |
| String | `{ "kind": "string", "max": 256 }` |
| String or null | `{ "kind": "nullableString", "max": 256 }` |
| Exact-field object | `{ "kind": "object", "fields": { "name": <field> } }` |

Limits: 64 query keys, 256 fields per object, validator nesting depth 4, string/exact-value bounds up to 64 KiB, URL 4 KiB, scope path 2 KiB, total request 64 KiB. The broker owns TLS and does not inherit cookies, proxies or host credentials, nor automatically follow redirects. Domain origins reject private/special resolved addresses; literal IP origins explicitly select an address. This is separate from granting an authenticated host CLI.

## Runtime interfaces

Read `/run/plugin/grants.json`, not the manifest, to discover admitted access. This read-only snapshot contains booleans for atomic grants, `null` or a selected service for `media`, selected maps for `filesystem`/`http`/`exec`, and `settings.read`/`settings.write` arrays. `grants.storage` controls persistence; `grants.exec.<name>.selected` lists admitted leaves. Modifying a local copy changes no authority.

| Operation | Worker interface | Limits/semantics |
| --- | --- | --- |
| Host CLI | `/bootstrap --exec <name> <argv...>`; shared alias `omarchy-ward-exec` | Literal argv, binary stdout/stderr, CLI exit status. Host cwd `/`, EOF stdin, no caller-selected environment. Two concurrent jobs, ten-second job deadline, 2 MiB each stdout/stderr. Forwarder may retry explicit not-started busy/rate-limit replies for up to 120 seconds; never block the UI thread |
| HTTP | `/bootstrap --http [metadata-file]`; alias `omarchy-ward-http` | One JSON stdin request: `scope`, `method`, `url`, optional `body`. Raw response on stdout; optional status/selected-header JSON in a worker-local file. 2 MiB response body, twelve concurrent jobs, ten-second deadline. Check HTTP status separately from transport success |
| Notification | `/bootstrap --notify <title> <body>` | Title 1–160 bytes, body up to 2,048 bytes, constrained controls, no actions/images. Fixed plugin identity; two deliveries per 30 seconds |
| Open link | `/bootstrap --open-url browser <url>` or `... webapp <url>` | HTTP(S) up to 2,048 bytes, no whitespace/controls/backslashes; two deliveries per 30 seconds. Success means launch accepted, not page loaded |
| Shared link aliases | `omarchy-launch-browser <url>`, `omarchy-launch-webapp <url>` | Exactly one URL argument; denial produces shared-runtime feedback |
| Save own settings | Own shell API's `updateEntryInline(id, patch)`; low-level `/bootstrap --settings '<JSON object>'` | Widgets reach the API through `bar.shell`; service/overlay Items receive `shell`. Only selected writable keys. Host merges without deleting unselected values. Shared API acceptance queues a save, not durable confirmation; rejected saves restore host values and show feedback |
| Existing player | MPRIS through the worker's filtered `DBUS_SESSION_BUS_ADDRESS` | Exact selected player at `/org/mpris/MediaPlayer2`: property Get/GetAll, introspection, Next/Previous/Pause/PlayPause/Stop/Play/Seek/SetPosition and PropertiesChanged/Seeked. No Properties.Set, OpenUri, Raise, Quit, arbitrary bus access or player publication |

All broker operations share a 32-connections-per-second admission ceiling. Revocation cancels owned jobs and stops the worker but cannot undo completed writes or retract effects delegated to other host services.

### Approved grants are signed into the record

The review is what turns a request into durable authority: every `approve` (and every later publish — launch, `stop`, `revoke`, `recover`) re-mints an ed25519 signature over `{ id, revision, enabled, grants }`, and every read re-verifies it before the grants are trusted. Runtime state (`epoch`, `active_unit`) is deliberately excluded so the controller can advance it to start/stop a worker without holding the signing key. The consequence is that a hand-edit of the store's grant record — adding a permission the reviewer never approved — fails closed at dispatch and at launch. The private key lives next to the records (`secrets/signing.key`, mode 0600), so this seals against accidental and editorial edits and against writers working outside the review tool; it is not a hardware-backed boundary, and a determined same-account process that can read the key can also re-sign. Genuine binding to a human reviewer needs the key outside the desktop account.

### Machine-readable operation results

Prefix a direct bootstrap operation with `--json` to receive one versioned JSON record on stdout, including the operation's output. There is no result file or extra pipe:

```text
/bootstrap --json --exec player --volume 0.5 '$OMARCHY_PLUGIN_PATH/sounds/chime.wav'
/bootstrap --json --http
```

The HTTP command still reads its request from stdin; JSON mode includes response headers and body in the record and accepts no metadata-file argument. Notification, settings and open-link commands accept the same prefix. Internal worker/controller/management modes do not. No extra permission is granted by requesting JSON. Quickshell can use `Process` with `StdioCollector` and parse the complete stdout using `JSON.parse(text)` in `onStreamFinished`; catch parsing failures as unknown outcomes. No native QML adapter is needed.

Every complete record contains `version: 1` and `status`:

| `status` | Meaning |
| --- | --- |
| `completed` | The command returned an outcome, HTTP returned a response, or the notification/settings/link helper acknowledged delivery. This does **not** mean application success |
| `denied` | Ward rejected the operation's grant or selected arguments/scope; the requested operation was not started |
| `invalid` | Malformed or unsupported request; the requested operation was not started |
| `busy`, `rate_limited` | Capacity/admission rejection; this attempt was not started |
| `failed` | Operational failure, including executable verification, launch, transport or output delivery. Not a grant denial; effects may already have occurred |
| `timed_out` | No completed outcome within the deadline; effects may already have occurred |
| `unavailable` | Admission snapshot, broker connection or a valid reply was unavailable; do not assume the operation never ran |

A completed command adds `exitCode` or `signal`, plus `stdout` and `stderr`; a completed HTTP response adds `httpStatus`, the selected `headers` map and `body`. Output values are strings when the original bytes are valid UTF-8 (including escaped controls), otherwise objects of the form `{ "base64": "..." }` using standard padded Base64. Bytes are never replaced or silently discarded. Existing 2 MiB per-stream/body bounds apply before encoding; JSON escaping or Base64 can enlarge the encoded record. Child output cannot impersonate a Ward status because it is serialized as data inside the record, not appended alongside it.

For example, `{"version":1,"status":"completed","exitCode":1,"stdout":"","stderr":"invalid input"}` means the host command ran and exited 1, while `{"version":1,"status":"denied"}` means Ward refused it. HTTP 403 is `completed` with `httpStatus: 403`, not Ward `denied`. In JSON mode, helper exit 0 means a `completed` outcome was emitted, even for command exit 1 or HTTP 403; Ward errors return exit 1. Inspect the record's application exit/status separately. Delivery acknowledgement does not prove a browser loaded a page or a notification was seen.

Without `--json`, raw mode is unchanged: binary stdout/stderr and the command's exit code (128 + signal for signal termination), raw HTTP body with the optional metadata file, and exit 1 for HTTP status >= 400 or Ward errors. Raw exit 1 alone cannot classify the failure. Shared aliases retain this raw behavior; use `/bootstrap --json ...` directly for structured results.

Missing, empty, partial, unknown-version or unknown-status results are **unknown outcomes**, not permission denial or proof that nothing happened. Writing stdout can fail after an external effect. Never blindly retry `failed`, `timed_out`, `unavailable` or an unknown outcome for a mutation. The exec forwarder already retries only explicit not-started busy/rate-limit conditions for up to 120 seconds. The read-only grants snapshot lets the caller identify intentionally absent broker sockets, but the host still rechecks live approval on every request.

The shared runtime provides packaged `qs.Commons`/`qs.Ui`, detached live theme/style/settings, widget `bar`/`settings` properties and own-service/panel operations. Widgets reach the own shell API through `bar.shell`; service/overlay Items may declare `shell`, `manifest` and `omarchyPath` properties for injection. The API's `serviceFor(id)`, `summon`, `hide`, `toggle` and `isPluginOpen` are scoped to this plugin. Panels expose `open(payloadJson)`, `close()` and `opened`; the host manages input/dismissal. These are not access to other plugins, desktop config, authentication objects or compositor sockets. Worker `OMARCHY_PATH` names the restricted runtime, not the desktop source checkout.

Helpers, bundled scripts, timers, local IPC and computation need no separate execution permission. Their external effects still need grants. Rendering includes the selected GPU render node, not screen capture, audio output, microphone access, arbitrary devices, compositor observations, package mutation or host execution. Unsupported families are tracked in the [grant inventory](../plans/sandboxed-plugin-grants.md); do not invent manifest fields or use broad exec grants as undocumented substitutes.

## Review and development

Use `omarchy plugin validate ./my-plugin` for ordinary validation, then `omarchy plugin add <git-url>` and `omarchy plugin review <id> --json`. Review performs native sandbox validation, runs no plugin code and returns the immutable revision. Ordinary validation alone does not prove native admission will succeed.

Approve that exact revision with selected flags, then enable separately (replace the revision placeholder):

```text
omarchy plugin approve example.pet --revision <sha256> --allow-storage --read-setting volume --write-setting volume
omarchy plugin enable example.pet
```

`--yes` skips terminal confirmation, not revision binding or grant selection; agents need authority for that approval. `omarchy plugin review <id> --ui` opens the same workflow. Missing required access prevents activation. `stop` halts a running instance without disconnecting its approval — it is the non-destructive predecessor to a re-approval, which requires no active instance (the old path forced a destructive `revoke` first). `disable` revokes and stops; `update` changes the checkout but does not approve it. Re-review and reapprove changed code. Never remove `sandbox` to work around startup failure.

Keep this reference, parsing/enforcement, CLI/reviewer selection and tests synchronized. Examples must pass the native parser. See the [test guide](testing.md). GPU fixtures need short wall-clock and explicit memory/swap limits around the outer test process as well as worker limits; do not leave capture loops running between interactions.
