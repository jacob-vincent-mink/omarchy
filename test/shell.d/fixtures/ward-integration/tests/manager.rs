#[path = "../../../../../native/ward/tests/support/desktop.rs"]
mod desktop;
#[path = "support/operator.rs"]
mod operator;
use desktop::{Desktop, Host};
use omarchy_ward::presentation::Viewport;
use std::{
  fs,
  path::PathBuf,
  process::Command,
  sync::mpsc,
  time::{Duration, Instant},
};

enum Stage {
  Click(i32, i32),
  Capture(&'static str),
}

#[test]
fn graphical_installer_requires_validated_revision_and_explicit_yolo_trust() {
  if std::env::var("OMARCHY_TEST_GRAPHICS").as_deref() != Ok("1")
    || std::env::var("OMARCHY_TEST_SYSTEMD").as_deref() != Ok("1")
  {
    return;
  }
  let module = std::env::var_os("OMARCHY_TEST_QT_BRIDGE").expect("select Qt module");
  let root = desktop::runtime();
  let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
    .join("../../../..")
    .canonicalize()
    .unwrap();
  for name in ["Commons", "Ui"] {
    std::os::unix::fs::symlink(repo.join("shell").join(name), root.path().join(name)).unwrap();
  }
  let host_qml = root.path().join("host.qml");
  fs::write(&host_qml, format!(r##"
import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import "file://{}/shell/plugins/panels/plugins" as Plugins
ShellRoot {{
  Plugins.Panel {{ id: manager }}
  PanelWindow {{ anchors {{ top: true; bottom: true; left: true; right: true }} color: "#171c25"; WlrLayershell.layer: WlrLayer.Bottom }}
  IpcHandler {{
    target: "shell"
    function ping(): string {{ return "ok" }}
    function rescanPlugins(): string {{ return "ok" }}
    function listPlugins(): string {{ return "[]" }}
    function summon(id: string, payload: string): string {{ manager.open(payload); return "ok" }}
    function setSource(source: string): string {{ manager.model.source = source; return "ok" }}
    function managerState(): string {{
      function find(items, name) {{
        for (const item of items) {{
          if (item.objectName === name) return item
          const found = find(item.children || [], name)
          if (found) return found
        }}
        return null
      }}
      const window = find(manager.data, "plugin-manager-window")
      function point(name) {{
        const item = find(window.contentItem, name)
        if (!item) return null
        const p = item.mapToItem(null, item.width / 2, item.height / 2)
        return [Math.round(p.x), Math.round(p.y)]
      }}
      const model = manager.model
      const add = find(window.contentItem, "plugin-add")
      return JSON.stringify({{busy:model.busy, error:model.error, inspected:model.inspected, adding:model.adding,
        yolo:model.yolo, confirmed:model.trustConfirmed, selected:model.selectedId,
        addEnabled:add.enabled, validate:point("plugin-validate"), add:point("plugin-add"),
        mode:point("plugin-yolo"), trust:point("plugin-trust-confirm")}})
    }}
  }}
}}
"##, repo.display())).unwrap();
  let env = operator::environment(root.path(), &repo, &host_qml, &module);
  let mut sources = vec![];
  for (id, ward) in [("test.manager-ward", true), ("test.manager-yolo", false)] {
    let source = root.path().join(id);
    fs::create_dir(&source).unwrap();
    let mut manifest = serde_json::json!({"schemaVersion":1,"id":id,"name":if ward {"Ward example"} else {"YOLO example"},"version":"1","kinds":["bar-widget"],"entryPoints":{"barWidget":"Widget.qml"}});
    if ward {
      manifest["sandbox"] = serde_json::json!({"version":1,"requests":{}});
    }
    fs::write(
      source.join("manifest.json"),
      serde_json::to_vec(&manifest).unwrap(),
    )
    .unwrap();
    fs::write(source.join("Widget.qml"), "import QtQuick\nItem {}\n").unwrap();
    for args in [
      vec!["init", "--template=", "-q"],
      vec!["add", "."],
      vec![
        "-c",
        "core.hooksPath=/dev/null",
        "-c",
        "commit.gpgsign=false",
        "-c",
        "user.name=Test",
        "-c",
        "user.email=test@example.com",
        "commit",
        "-qm",
        "fixture",
      ],
    ] {
      assert!(
        Command::new("git")
          .arg("-C")
          .arg(&source)
          .args(args)
          .envs(env.iter().cloned())
          .status()
          .unwrap()
          .success()
      );
    }
    sources.push(source);
  }
  let mut display = Desktop::new(
    root.path(),
    Viewport {
      width: 900,
      height: 800,
      scale_fixed: 120,
    },
  );
  let log = root.path().join("host.log");
  let _host = Host(
    desktop::command(root.path(), &host_qml, &module)
      .envs(env.iter().cloned())
      .env_remove("HYPRLAND_INSTANCE_SIGNATURE")
      .stdout(fs::File::create(&log).unwrap())
      .stderr(fs::File::options().append(true).open(&log).unwrap())
      .spawn()
      .unwrap(),
  );
  let (send, receive) = mpsc::channel();
  let (ack, wait_ack) = mpsc::channel();
  let operator = std::thread::spawn(move || {
    let run = |args: &[&str]| operator::run(&env, "omarchy-shell", args);
    let start = Instant::now();
    loop {
      if Command::new("timeout")
        .args(["1s", "omarchy-shell", "shell", "ping"])
        .envs(env.iter().cloned())
        .output()
        .unwrap()
        .status
        .success()
      {
        break;
      }
      assert!(
        start.elapsed() < Duration::from_secs(10),
        "manager host did not start"
      );
      std::thread::sleep(Duration::from_millis(25));
    }
    let state =
      || serde_json::from_str::<serde_json::Value>(&run(&["shell", "managerState"])).unwrap();
    let wait = |predicate: &dyn Fn(&serde_json::Value) -> bool| {
      let start = Instant::now();
      loop {
        let value = state();
        assert_eq!(value["error"], "", "manager command failed: {value}");
        if predicate(&value) {
          return value;
        }
        assert!(
          start.elapsed() < Duration::from_secs(10),
          "manager stalled: {value}"
        );
        std::thread::sleep(Duration::from_millis(30));
      }
    };
    let click = |value: &serde_json::Value, name: &str| {
      let point = value[name].as_array().unwrap();
      send
        .send(Stage::Click(
          point[0].as_i64().unwrap() as i32,
          point[1].as_i64().unwrap() as i32,
        ))
        .unwrap();
    };
    let capture = |name| {
      send.send(Stage::Capture(name)).unwrap();
      wait_ack.recv_timeout(Duration::from_secs(5)).unwrap();
    };
    for (index, source) in sources.iter().enumerate() {
      run(&["shell", "summon", "omarchy.plugins", "{\"add\":true}"]);
      wait(&|state| state["busy"] == false);
      run(&["shell", "setSource", source.to_str().unwrap()]);
      if index == 1 {
        click(&state(), "mode");
        wait(&|state| state["yolo"] == true);
      }
      capture(if index == 0 {
        "manager-ward-add"
      } else {
        "manager-yolo-warning"
      });
      click(&state(), "validate");
      let checked = wait(&|state| state["busy"] == false && state["inspected"].is_object());
      if index == 1 {
        assert_eq!(checked["confirmed"], false);
        assert_eq!(
          checked["addEnabled"], false,
          "YOLO omitted trust acknowledgement"
        );
        capture("manager-yolo-unconfirmed");
        click(&checked, "trust");
        wait(&|state| state["confirmed"] == true && state["addEnabled"] == true);
      }
      capture(if index == 0 {
        "manager-ward-validated"
      } else {
        "manager-yolo-confirmed"
      });
      click(&state(), "add");
      wait(&|state| state["adding"] == false && state["busy"] == false);
      capture(if index == 0 {
        "manager-ward-installed"
      } else {
        "manager-yolo-installed"
      });
    }
    let records: serde_json::Value = serde_json::from_str(&operator::run(
      &env,
      "omarchy-plugin-installation",
      &["list"],
    ))
    .unwrap();
    assert!(
      records
        .as_array()
        .unwrap()
        .iter()
        .any(|row| row["id"] == "test.manager-yolo" && row["mode"] == "yolo")
    );
    assert!(
      records
        .as_array()
        .unwrap()
        .iter()
        .any(|row| row["id"] == "test.manager-ward" && row["mode"] == "ward")
    );
  });
  let start = Instant::now();
  let mut capture = None;
  let mut frame = None;
  while !operator.is_finished() || capture.is_some() {
    let time = start.elapsed().as_millis() as u32;
    while let Ok(stage) = receive.try_recv() {
      match stage {
        Stage::Click(x, y) => {
          for kind in [0, 1] {
            display.graphics.input(kind, 0x110, x, y, time).unwrap();
          }
        }
        Stage::Capture(name) => capture = Some((name, Instant::now())),
      }
    }
    for next in display.step(time) {
      frame = Some(next);
    }
    if let (Some((name, requested)), Some(frame)) = (capture, &frame) {
      if requested.elapsed() > Duration::from_millis(350) {
        if let Some(directory) = std::env::var_os("OMARCHY_TEST_SURFACE_FRAMES") {
          frame.save(PathBuf::from(directory).join(format!("{name}.ppm")));
        }
        capture = None;
        ack.send(()).unwrap();
      }
    }
    assert!(
      start.elapsed() < Duration::from_secs(45),
      "manager fixture timed out: {}",
      fs::read_to_string(&log).unwrap()
    );
    std::thread::sleep(Duration::from_millis(5));
  }
  assert!(
    operator.join().is_ok(),
    "manager operator failed: {}",
    fs::read_to_string(&log).unwrap()
  );
}
