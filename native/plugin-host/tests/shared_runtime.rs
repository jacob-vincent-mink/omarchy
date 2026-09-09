#![cfg(feature = "graphics")]
#[path = "support/desktop.rs"]
mod desktop;
use desktop::{Desktop, Frame, Host};
use omarchy_plugin_host::{
  grants::Grants, presentation::Viewport, revision::Revision, store::Store,
};
use std::{
  fs,
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
        Some(Update::Ready | Update::Presentation(_)) => (),
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
    "barWidget": {"defaultSection": "right"}, "sandbox": {"version": 1, "requests": {}}
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
BarWidget {
  id: root
  moduleName: "test.shared"
  implicitWidth: 40
  implicitHeight: barSize
  readonly property var own: bar && bar.shell ? bar.shell.serviceFor(moduleName) : null
  readonly property bool scoped: own && bar.shell.serviceFor("other.plugin") === null
    && bar.shell.summon("other.plugin", "") === false
    && bar.shell.updateEntryInline(moduleName, {}) === false
  Rectangle {
    anchors.centerIn: parent
    width: 40; height: 20
    color: !root.scoped ? "#ee1122" : root.own.presses === 0 ? "#eeaa22"
      : root.own.presses === 1 ? "#22aadd" : "#aa44dd"
  }
  MouseArea {
    anchors.fill: parent
    onClicked: {
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
    .approve(&revision.digest, Grants::default())
    .unwrap();
  let host_qml = root.path().join("host.qml");
  fs::write(&host_qml, r##"
import QtQuick
import Quickshell
import Omarchy.PluginHost
ShellRoot {
  FloatingWindow {
    implicitWidth: 400; implicitHeight: 240; color: "#24303a"
    Rectangle {
      x: 15; y: 65; width: 30; height: 30; color: "#bb6633"
      MouseArea { anchors.fill: parent; onClicked: parent.color = "#ee5599" }
    }
    PluginView {
      anchors.fill: parent
      Component.onCompleted: start(Quickshell.env("TEST_STORE"), "test.shared", Quickshell.env("TEST_CONTROLLER"), 400, 240)
      onStateChanged: if (error) console.error(error)
    }
  }
}
"##).unwrap();
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
      .env("TEST_STORE", root.path().join("store"))
      .env("TEST_CONTROLLER", controller)
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
      |frame| frame.count(color) == 800 && frame.count([0x44, 0xee, 0x22]) == 0,
    );
  }
  admission.0.revoke("test.shared").unwrap();
  wait_frame(&mut display, start, &log, "revoked", |frame| {
    frame.count([0xaa, 0x44, 0xdd]) == 0
  });
}
