#![cfg(feature = "graphics")]
#[path = "support/desktop.rs"]
mod desktop;
use desktop::{Desktop, Frame, Host};
use omarchy_plugin_host::{
  grants::Grants, presentation::Viewport, revision::Revision, store::Store,
};
use std::{
  fs,
  os::unix::fs::PermissionsExt,
  path::PathBuf,
  time::{Duration, Instant},
};

struct Admission(Store);
impl Drop for Admission {
  fn drop(&mut self) {
    let _ = self.0.revoke("test.shared");
  }
}

#[test]
fn missing_or_invalid_shared_runtime_fails_without_presenting() {
  if std::env::var("OMARCHY_TEST_GRAPHICS").as_deref() != Ok("1")
    || std::env::var("OMARCHY_TEST_SYSTEMD").as_deref() != Ok("1")
  {
    return;
  }
  use omarchy_plugin_host::session::{Session, Update};
  for invalid_directory in [false, true] {
    let root = desktop::runtime();
    let controller = root.path().join("omarchy-plugin-host");
    fs::copy(env!("CARGO_BIN_EXE_omarchy-plugin-host"), &controller).unwrap();
    if invalid_directory {
      fs::write(root.path().join("plugin-runtime"), "not a directory").unwrap();
    }
    let source = root.path().join("source");
    fs::create_dir(&source).unwrap();
    fs::write(source.join("Widget.qml"), "import QtQuick\nItem {}\n").unwrap();
    fs::write(
      source.join("manifest.json"),
      serde_json::to_vec(&serde_json::json!({
        "schemaVersion": 1, "id": "test.shared", "name": "Shared runtime", "version": "1",
        "kinds": ["bar-widget"], "entryPoints": {"barWidget": "Widget.qml"},
        "sandbox": {"version": 1, "requests": {}}
      }))
      .unwrap(),
    )
    .unwrap();
    let store = root.path().join("store");
    let admission = Admission(Store::initialize(&store).unwrap());
    let revision = Revision::import(&source, &admission.0.revisions()).unwrap();
    admission
      .0
      .approve(&revision.digest, Grants::default())
      .unwrap();
    let session = Session::start(
      store,
      "test.shared".into(),
      controller,
      Viewport {
        width: 400,
        height: 240,
        scale: 1,
      },
    )
    .unwrap();
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
      match session.poll().unwrap() {
        Some(Update::Failed(error)) => {
          assert!(!error.is_empty());
          break;
        }
        Some(Update::Presentation(omarchy_plugin_host::presentation::Event::Frame { .. })) => {
          panic!("invalid runtime produced a frame");
        }
        // Ready acknowledges controller admission, not worker content.
        Some(Update::Ready | Update::Presentation(_) | Update::PanelState { .. }) => (),
        None => {
          assert!(
            Instant::now() < deadline,
            "invalid runtime did not fail startup"
          );
          std::thread::sleep(Duration::from_millis(5));
        }
      }
    }
  }
}

fn wait_frame(
  display: &mut Desktop,
  start: Instant,
  log: &PathBuf,
  name: &str,
  ready: impl Fn(&Frame) -> bool,
) {
  let deadline = Instant::now() + Duration::from_secs(3);
  while Instant::now() < deadline {
    for frame in display.step(start.elapsed().as_millis() as u32) {
      if ready(&frame) {
        if let Some(directory) = std::env::var_os("OMARCHY_TEST_SURFACE_FRAMES") {
          frame.save(PathBuf::from(directory).join(format!("shared-{name}.ppm")));
        }
        return;
      }
    }
    std::thread::sleep(Duration::from_millis(5));
  }
  panic!(
    "shared runtime did not reach {name}: {}",
    fs::read_to_string(log).unwrap()
  );
}

#[test]
fn staged_shared_runtime_loads_original_contracts_without_bundled_host_modules() {
  for settings_granted in [false, true] {
    shared_runtime(settings_granted);
  }
}

fn shared_runtime(settings_granted: bool) {
  if std::env::var("OMARCHY_TEST_GRAPHICS").as_deref() != Ok("1")
    || std::env::var("OMARCHY_TEST_SYSTEMD").as_deref() != Ok("1")
  {
    return;
  }
  let Some(controller) = std::env::var_os("OMARCHY_TEST_PLUGIN_HOST") else {
    eprintln!("set OMARCHY_TEST_PLUGIN_HOST to the CMake-staged controller");
    return;
  };
  let Some(module) = std::env::var_os("OMARCHY_TEST_QT_BRIDGE") else {
    return;
  };
  let controller = PathBuf::from(controller);
  let runtime = controller.parent().unwrap().join("plugin-runtime/shell");
  assert_eq!(
    fs::read(runtime.join("worker.qml")).unwrap(),
    include_bytes!("../runtime/worker.qml")
  );
  let root = desktop::runtime();
  let source = root.path().join("source");
  fs::create_dir(&source).unwrap();
  fs::write(source.join("manifest.json"), serde_json::to_vec(&serde_json::json!({
    "schemaVersion": 1, "id": "test.shared", "name": "Shared runtime", "version": "1",
    "kinds": ["bar-widget", "service", "overlay"],
    "entryPoints": {"barWidget": "Widget #.qml", "service": "Service.qml", "overlay": "Overlay.qml"},
    "barWidget": {"defaultSection": "right"}, "sandbox": {"version": 1, "requests": {"settings": {"read": ["width", "fontSize", "nested"], "write": ["width", "fontSize", "nested"]}}}
  })).unwrap()).unwrap();
  fs::write(
    source.join("Service.qml"),
    r#"
import QtQuick
Item {
  property var shell: null
  property var manifest: null
  property int presses: 0
}
"#,
  )
  .unwrap();
  fs::write(
    source.join("Widget #.qml"),
    r##"
import QtQuick
import qs.Ui
import qs.Commons
BarWidget {
  id: root
  moduleName: "test.shared"
  implicitWidth: settings.width
  implicitHeight: barSize
  readonly property var own: bar && bar.shell ? bar.shell.serviceFor(moduleName) : null
  readonly property bool scoped: own && bar.shell.serviceFor("other.plugin") === null
    && bar.shell.summon("other.plugin", "") === false
    && bar.shell.updateEntryInline("other.plugin", {}) === false
    && settings.id === undefined && settings.sandbox === undefined
    && settings.hidden === undefined
    && settings.nested.text.length === 8192 && settings.nested.other === undefined
    && Style.fontBaseSize === settings.fontSize && Style.cornerRadius === 7 && Style.gapsOut === 4
    && Style.resolvedFontFamily === "monospace"
  Rectangle {
    anchors.centerIn: parent
    width: root.implicitWidth; height: 20
    color: !root.scoped ? "#ee1122" : root.own.presses === 0 ? Color.accent
      : root.own.presses === 1 ? Color.foreground : Color.urgent
  }
  MouseArea {
    anchors.fill: parent
    acceptedButtons: Qt.LeftButton | Qt.RightButton
    onClicked: mouse => {
      if (mouse.button === Qt.RightButton) {
        var entry = Object.assign({}, root.settings, { id: root.moduleName, width: 80 })
        root.settings = entry
        root.bar.shell.updateEntryInline(root.moduleName, entry)
        return
      }
      root.own.presses++
      root.bar.shell.summon(root.moduleName, JSON.stringify({ source: "button" }))
    }
  }
}
"##,
  )
  .unwrap();
  fs::write(source.join("Overlay.qml"), r##"
import QtQuick
import Quickshell
import Quickshell.Wayland
Item {
  id: root
  property var shell: null
  property var manifest: null
  property bool opened: false
  property bool payloadReceived: false
  function open(payload) { payloadReceived = JSON.parse(payload).source === "button"; opened = true }
  function close() { opened = false }
  PanelWindow {
    visible: root.opened
    implicitWidth: 200; implicitHeight: 120
    color: root.payloadReceived ? "#44ee22" : "#ee1122"
    WlrLayershell.layer: WlrLayer.Overlay
    WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive
    Item {
      anchors.fill: parent
      focus: true
      Keys.onEscapePressed: root.shell.hide(root.manifest.id)
    }
  }
}
"##).unwrap();
  assert!(!source.join("Commons").exists() && !source.join("Ui").exists());
  let admission = Admission(Store::initialize(&root.path().join("store")).unwrap());
  let revision = Revision::import(&source, &admission.0.revisions()).unwrap();
  admission
    .0
    .approve(
      &revision.digest,
      Grants {
        settings: omarchy_plugin_host::settings::Grant {
          read: ["width".into(), "fontSize".into(), "nested".into()].into(),
          write: if settings_granted {
            ["width".into(), "fontSize".into(), "nested".into()].into()
          } else {
            Default::default()
          },
        },
        ..Default::default()
      },
    )
    .unwrap();
  fs::create_dir(root.path().join("shell")).unwrap();
  let host_qml = root.path().join("shell/shell.qml");
  let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
    .parent()
    .unwrap()
    .parent()
    .unwrap()
    .to_path_buf();
  for module in ["Commons", "Ui"] {
    std::os::unix::fs::symlink(
      repo.join("shell").join(module),
      root.path().join("shell").join(module),
    )
    .unwrap();
  }
  std::os::unix::fs::symlink(
    repo.join("shell/services"),
    root.path().join("shell/Services"),
  )
  .unwrap();
  fs::create_dir(root.path().join("bin")).unwrap();
  for name in ["omarchy-plugin-settings-apply", "omarchy-shell"] {
    fs::copy(
      repo.join("bin").join(name),
      root.path().join("bin").join(name),
    )
    .unwrap();
  }
  // systemd does not inherit the Qt caller's private IPC environment.
  let private_controller = root.path().join("controller");
  let quote =
    |path: &std::path::Path| format!("'{}'", path.display().to_string().replace('\'', "'\\''"));
  fs::write(&private_controller, format!(
    "#!/bin/bash\nexport OMARCHY_PATH={0} XDG_RUNTIME_DIR={0} WAYLAND_DISPLAY=wayland PATH={0}/bin:/usr/bin\nexec {1} \"$@\"\n",
    quote(root.path()), quote(&controller))).unwrap();
  fs::set_permissions(&private_controller, fs::Permissions::from_mode(0o700)).unwrap();
  let host_context = root.path().join("host-context.json");
  let context = |width, font_size, foreground| {
    serde_json::json!({
      "settings": {"id": "test.shared", "sandbox": true, "width": width, "fontSize": font_size,
        "nested": {"text": "x".repeat(8192)}, "hidden": "host-only"},
      "foreground": foreground, "fontSize": font_size, "enabled": true,
      "untouched": {"otherPlugin": "preserved"},
    })
  };
  fs::write(
    &host_context,
    serde_json::to_vec(&context(40, 12, "#22aadd")).unwrap(),
  )
  .unwrap();
  let shell_source = fs::read_to_string(repo.join("shell/shell.qml")).unwrap();
  let method = |signature: &str, closing: &str| {
    let start = shell_source.find(signature).unwrap();
    let end = start + shell_source[start..].find(closing).unwrap() + closing.len();
    &shell_source[start..end]
  };
  let host_source = r##"
import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import "Services"
ShellRoot {
  id: shell
  property var shellConfig: ({plugins: []})
  property var builtinShellConfig: ({})
  property alias sandboxedPlugins: plugins
  function persistShellConfig(config) {
    shellConfig = config
    plugins.sync(config.plugins)
    var context = JSON.parse(data.text())
    context.settings = config.plugins[0]
    data.setText(JSON.stringify(context))
  }
  IpcHandler {
    target: "shell"
    @SAVE_SETTINGS@
  }
  SandboxedPlugins { id: plugins }
  FileView {
    id: data
    path: Quickshell.env("TEST_CONTEXT")
    watchChanges: true
    onFileChanged: reload()
    onLoaded: {
      var context = JSON.parse(text())
      Color.accent = "#eeaa22"
      Color.foreground = context.foreground
      Color.urgent = "#aa44dd"
      Color.shellValues = { "font.base-size": String(context.fontSize), "bar.size-horizontal": "26", "bar.scale-with-font": "false" }
      Style.applyShellValues(Color.shellValues)
      Style.cornerRadius = 7
      Style.gapsOut = 4
      Style.resolvedFontFamily = "monospace"
      shell.shellConfig = {plugins: [JSON.parse(JSON.stringify(context.settings))]}
      plugins.sync(context.enabled ? [context.settings] : [])
      // Subsequent mutation of the caller's object must not mutate the copy.
      context.settings.nested.other = "not for the worker"
    }
  }
  FloatingWindow {
    implicitWidth: 400; implicitHeight: 240; color: "#24303a"
    Rectangle {
      x: 15; y: 65; width: 30; height: 30; color: "#bb6633"
      MouseArea { anchors.fill: parent; onClicked: parent.color = "#ee5599" }
    }
  }
}
"##;
  fs::write(
    &host_qml,
    host_source.replace(
      "@SAVE_SETTINGS@",
      method("function saveSandboxSettings(", "\n    }"),
    ),
  )
  .unwrap();
  let log = root.path().join("host.log");
  let mut display = Desktop::new(
    root.path(),
    Viewport {
      width: 400,
      height: 240,
      scale: 1,
    },
  );
  let _host = Host(
    desktop::command(root.path(), &host_qml, &module)
      .env("HOME", root.path().join("home"))
      .env("OMARCHY_PLUGIN_STORE", root.path().join("store"))
      .env("OMARCHY_PLUGIN_HOST", private_controller)
      .env("OMARCHY_PLUGIN_CONTEXT", "1")
      .env("TEST_CONTEXT", &host_context)
      .stdout(fs::File::create(&log).unwrap())
      .stderr(fs::File::options().append(true).open(&log).unwrap())
      .spawn()
      .unwrap(),
  );
  let start = Instant::now();
  wait_frame(&mut display, start, &log, "ready", |frame| {
    frame.count([0xee, 0xaa, 0x22]) == 800
  });
  let click = |display: &mut Desktop, x, y| {
    display
      .graphics
      .input(2, 0, x, y, start.elapsed().as_millis() as u32)
      .unwrap();
    for kind in [0, 1] {
      display
        .graphics
        .input(kind, 0x110, x, y, start.elapsed().as_millis() as u32)
        .unwrap();
      if kind == 0 {
        display.step(start.elapsed().as_millis() as u32);
        std::thread::sleep(Duration::from_millis(10));
      }
    }
  };
  click(&mut display, 30, 80);
  wait_frame(&mut display, start, &log, "click-through", |frame| {
    frame.count([0xee, 0x55, 0x99]) == 900
  });
  for (index, color) in [[0x22, 0xaa, 0xdd], [0xaa, 0x44, 0xdd]]
    .into_iter()
    .enumerate()
  {
    click(&mut display, 375, 13);
    wait_frame(&mut display, start, &log, "opened", |frame| {
      frame.count([0x44, 0xee, 0x22]) == 24000
    });
    for kind in [3, 4] {
      display
        .graphics
        .input(kind, 9, 0, 0, start.elapsed().as_millis() as u32)
        .unwrap();
    }
    wait_frame(
      &mut display,
      start,
      &log,
      &format!("closed-{index}"),
      |frame| {
        frame.count(color) == if index == 0 { 800 } else { 1200 }
          && frame.count([0x44, 0xee, 0x22]) == 0
      },
    );
    if index == 0 {
      fs::write(
        &host_context,
        serde_json::to_vec(&context(60, 14, "#11cc99")).unwrap(),
      )
      .unwrap();
      wait_frame(&mut display, start, &log, "live-context", |frame| {
        frame.count([0x11, 0xcc, 0x99]) == 1200
      });
    }
  }
  for kind in [2, 0, 1] {
    display
      .graphics
      .input(
        kind,
        if kind == 2 { 0 } else { 0x111 },
        375,
        13,
        start.elapsed().as_millis() as u32,
      )
      .unwrap();
    display.step(start.elapsed().as_millis() as u32);
    std::thread::sleep(Duration::from_millis(10));
  }
  // Wait beyond the debounce and broker round trip, then assert the host's
  // persisted entry, not just the widget's optimistic local settings.
  let deadline = Instant::now() + Duration::from_secs(2);
  let mut saved_pixels = 0;
  while Instant::now() < deadline {
    for frame in display.step(start.elapsed().as_millis() as u32) {
      saved_pixels = frame.count([0xaa, 0x44, 0xdd]);
      if let Some(directory) = std::env::var_os("OMARCHY_TEST_SURFACE_FRAMES") {
        frame
          .save(PathBuf::from(directory).join(format!("shared-settings-{settings_granted}.ppm")));
      }
    }
    std::thread::sleep(Duration::from_millis(5));
  }
  assert_eq!(
    saved_pixels,
    if settings_granted { 1600 } else { 1200 },
    "settings callback did not reconcile with host state"
  );
  let mut saved: serde_json::Value =
    serde_json::from_slice(&fs::read(&host_context).unwrap()).unwrap();
  assert_eq!(
    saved["settings"]["width"],
    if settings_granted { 80 } else { 60 }
  );
  assert_eq!(saved["settings"]["id"], "test.shared");
  assert_eq!(saved["settings"]["sandbox"], true);
  assert_eq!(
    saved["settings"]["hidden"], "host-only",
    "saving selected keys must preserve unreadable settings"
  );
  assert_eq!(saved["untouched"]["otherPlugin"], "preserved");
  saved["enabled"] = false.into();
  fs::write(&host_context, serde_json::to_vec(&saved).unwrap()).unwrap();
  wait_frame(&mut display, start, &log, "stopped", |frame| {
    frame.count([0xaa, 0x44, 0xdd]) == 0
  });
  saved["enabled"] = true.into();
  fs::write(&host_context, serde_json::to_vec(&saved).unwrap()).unwrap();
  wait_frame(&mut display, start, &log, "restored", |frame| {
    frame.count([0xee, 0xaa, 0x22]) == if settings_granted { 1600 } else { 1200 }
  });
  admission.0.revoke("test.shared").unwrap();
  wait_frame(&mut display, start, &log, "revoked", |frame| {
    frame.count([0xee, 0xaa, 0x22]) == 0
  });
}
