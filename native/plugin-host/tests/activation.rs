#![cfg(feature = "graphics")]
#[path = "support/desktop.rs"]
mod desktop;
#[path = "support/operator.rs"]
mod operator;
use desktop::{Desktop, Host};
use omarchy_plugin_host::{controller::Scroll, presentation::Viewport};
use std::{
  fs,
  path::PathBuf,
  process::Command,
  sync::mpsc,
  time::{Duration, Instant},
};

#[test]
fn commands_activate_the_shell_host_with_click_through_and_revocation() {
  activation(false);
}

#[test]
fn graphical_review_approves_only_selected_access_before_enabling() {
  activation(true);
}

enum Stage {
  Click(i32, i32),
  Scroll(i32),
  Key(u32),
  Capture(&'static str),
  Enabled,
  Disabled,
}

fn activation(review_ui: bool) {
  let Some(module) = std::env::var_os("OMARCHY_TEST_QT_BRIDGE") else {
    return;
  };
  if std::env::var("OMARCHY_TEST_GRAPHICS").as_deref() != Ok("1")
    || std::env::var("OMARCHY_TEST_SYSTEMD").as_deref() != Ok("1")
  {
    return;
  }
  let root = desktop::runtime();
  let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
    .parent()
    .unwrap()
    .parent()
    .unwrap()
    .to_path_buf();
  let home = root.path().join("home");
  let plugin = home.join(".config/omarchy/plugins/test.activation");
  fs::create_dir_all(&plugin).unwrap();
  for module in ["Commons", "Ui"] {
    std::os::unix::fs::symlink(repo.join("shell").join(module), root.path().join(module)).unwrap();
  }
  fs::write(plugin.join("manifest.json"), serde_json::to_vec(&serde_json::json!({
    "schemaVersion": 1, "id": "test.activation", "name": "Activation", "version": "1", "kinds": ["panel"],
    "entryPoints": {"panel": "worker.qml"}, "sandbox": {"version": 1, "entryPoint": "worker.qml", "requests": {"network": true, "notifications": true, "read": ["notes"]}}
  })).unwrap()).unwrap();
  fs::write(
    plugin.join("worker.qml"),
    r##"
import QtQuick
import Quickshell
ShellRoot {
  FloatingWindow {
    implicitWidth: 240; implicitHeight: 160; color: "#304050"
    Text { x: 20; y: 15; text: "Approved Quickshell"; color: "white" }
    Rectangle {
      id: button; x: 20; y: 60; width: 100; height: 50; color: "#bb6633"
      Text { anchors.centerIn: parent; text: "Click me"; color: "white" }
      MouseArea { anchors.fill: parent; onClicked: button.color = "#44ee22" }
    }
    Rectangle { x: 170; y: 72; width: 28; height: 28; color: "#ffb13b"
      NumberAnimation on rotation { from: 0; to: 360; duration: 1800; loops: Animation.Infinite }
    }
  }
}
"##,
  )
  .unwrap();
  let host_qml = root.path().join("host.qml");
  fs::write(&host_qml, format!(r##"
import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import "file://{}/shell/services" as Services
import "file://{}/shell/plugins/panels/plugin-review" as Review
ShellRoot {{
  Services.SandboxedPlugins {{ id: plugins }}
  Review.Panel {{ id: reviewer }}
  PanelWindow {{
    anchors {{ top: true; bottom: true; left: true; right: true }}
    color: "#171c25"
    WlrLayershell.layer: WlrLayer.Bottom
    Rectangle {{ id: underneath; width: 60; height: 60; color: "#445566"
      MouseArea {{ anchors.fill: parent; onClicked: underneath.color = "#eeaa22" }}
    }}
  }}
  IpcHandler {{
    target: "shell"
    function ping(): string {{ return "ok" }}
    function enablePlugin(id: string, placement: string): string {{ return plugins.enable(id) }}
    function pluginStatus(id: string): string {{ return JSON.stringify(plugins.status(id)) }}
    function setPluginEnabled(id: string, enabled: string): string {{ plugins.disable(id); return "ok" }}
    function summon(id: string, payload: string): string {{ reviewer.open(payload); return "ok" }}
    function listPlugins(): string {{ return JSON.stringify([{{id: "test.activation", enabled: plugins.status("test.activation").state === "running"}}]) }}
    function reviewState(): string {{
      function find(items, name) {{
        for (var i = 0; i < items.length; i++) {{
          var item = items[i]
          if (item.objectName === name) return item
          var found = find(item.children || [], name)
          if (found) return found
        }}
        return null
      }}
      var window = find(reviewer.data, "plugin-review-window")
      function point(name) {{
        var item = find(window.contentItem, name)
        if (!item) return null
        var p = item.mapToItem(null, item.width / 2, item.height / 2)
        return [Math.round(p.x), Math.round(p.y)]
      }}
      var review = reviewer.review
      var scroll = find(window.contentItem, "review-scroll")
      var folder = find(window.contentItem, "review-folder-notes")
      return JSON.stringify({{visible: window.visible, busy: review.busy, error: review.error, revision: review.revision,
        scrollY: scroll.contentItem.contentY, scrollMoving: scroll.contentItem.moving,
        folders: review.folders, folderField: point("review-folder-notes"),
        folderFocus: folder && folder.activeFocus,
        approved: review.selectionApproved, current: review.current, network: review.network, notifications: review.notifications,
        notificationButton: point("review-notifications"), approvalButton: point("review-approve"), revokeButton: point("review-revoke"), closeButton: point("review-close")}})
    }}
  }}
}}
"##, repo.display(), repo.display())).unwrap();
  let env = operator::environment(root.path(), &repo, &host_qml, &module);
  let mut outer = Desktop::new(
    root.path(),
    Viewport {
      width: 800,
      height: 480,
      scale: 1,
    },
  );
  let log = root.path().join("host.log");
  let mut host = Host(
    desktop::command(root.path(), &host_qml, &module)
      .envs(env.iter().cloned())
      .env_remove("HYPRLAND_INSTANCE_SIGNATURE")
      .stdout(fs::File::create(&log).unwrap())
      .stderr(fs::File::options().append(true).open(&log).unwrap())
      .spawn()
      .unwrap(),
  );
  let (progress, stages) = mpsc::channel();
  let (resume, proceed) = mpsc::channel();
  let commands = std::thread::spawn(move || {
    let run = |name: &str, args: &[&str]| operator::run(&env, name, args);
    let review: serde_json::Value = serde_json::from_str(&run(
      "omarchy-plugin-review",
      &["test.activation", "--json"],
    ))
    .unwrap();
    if !review_ui {
      run(
        "omarchy-plugin-approve",
        &[
          "test.activation",
          "--revision",
          review["revision"].as_str().unwrap(),
          "--yes",
        ],
      );
    }
    // The review/approval work gives the private IPC endpoint time to start;
    // still explicitly wait for it, without touching a desktop socket.
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
      let result = Command::new("/usr/bin/timeout")
        .args(["1s", "omarchy-shell", "shell", "ping"])
        .envs(env.iter().cloned())
        .output()
        .unwrap();
      if result.status.success() && String::from_utf8_lossy(&result.stdout).trim() == "ok" {
        break;
      }
      assert!(Instant::now() < deadline, "private IPC host did not start");
      std::thread::sleep(Duration::from_millis(25));
    }
    let wait = |predicate: &dyn Fn(&serde_json::Value) -> bool| {
      let deadline = Instant::now() + Duration::from_secs(8);
      loop {
        let state: serde_json::Value =
          serde_json::from_str(&run("omarchy-shell", &["shell", "reviewState"])).unwrap();
        if predicate(&state) {
          break state;
        }
        assert!(Instant::now() < deadline, "review state timed out: {state}");
        std::thread::sleep(Duration::from_millis(25));
      }
    };
    let click = |state: &serde_json::Value, key: &str| {
      let point = state[key].as_array().unwrap();
      std::thread::sleep(Duration::from_millis(100));
      progress
        .send(Stage::Click(
          point[0].as_i64().unwrap() as i32,
          point[1].as_i64().unwrap() as i32,
        ))
        .unwrap();
    };
    if review_ui {
      run("omarchy-plugin-review", &["test.activation", "--ui"]);
      let state = wait(&|state| state["busy"] == false && state["revision"].is_object());
      assert_eq!(state["error"], "");
      assert_eq!(state["network"], false);
      assert_eq!(state["notifications"], false);
      assert_eq!(state["current"]["approved"], false);
      click(&state, "notificationButton");
      wait(&|state| state["notifications"] == true);
      let capture = |label| {
        std::thread::sleep(Duration::from_millis(150));
        progress.send(Stage::Capture(label)).unwrap();
      };
      capture("selection");
      progress.send(Stage::Scroll(-720)).unwrap();
      let state = wait(&|state| {
        state["scrollY"].as_f64().unwrap_or(0.0) > 0.0 && state["scrollMoving"] == false
      });
      click(&state, "folderField");
      wait(&|state| state["folderFocus"] == true);
      progress.send(Stage::Key(53)).unwrap(); // x: an invalid relative folder
      capture("typing");
      let state = wait(&|state| state["folders"]["notes"] == "x");
      click(&state, "approvalButton");
      let state = wait(&|state| {
        state["busy"] == false
          && state["error"]
            .as_str()
            .unwrap_or("")
            .contains("--read requires")
      });
      assert_eq!(state["current"]["approved"], false);
      capture("error");
      click(&state, "folderField");
      wait(&|state| state["folderFocus"] == true);
      progress.send(Stage::Key(22)).unwrap(); // Backspace: leave the folder denied
      let state = wait(&|state| state["folders"]["notes"] == "");
      click(&state, "approvalButton");
      let state = wait(&|state| state["approved"] == true && state["busy"] == false);
      assert_eq!(state["error"], "");
      assert_eq!(
        state["current"]["enabled"], false,
        "approval started a plugin"
      );
      assert_eq!(state["current"]["grants"]["network"], false);
      assert_eq!(state["current"]["grants"]["notifications"], true);
      assert_eq!(state["current"]["grants"]["read"], serde_json::json!({}));
      capture("approved");
      click(&state, "approvalButton");
      let state = wait(&|state| state["current"]["enabled"] == true && state["busy"] == false);
      capture("enabled");
      click(&state, "closeButton");
      wait(&|state| state["visible"] == false);
    } else {
      let enabled = run("omarchy-plugin-enable", &["test.activation"]);
      assert!(enabled.contains("Enabled test.activation"));
    }
    progress.send(Stage::Enabled).unwrap();
    proceed.recv_timeout(Duration::from_secs(8)).unwrap();
    if review_ui {
      run("omarchy-plugin-review", &["test.activation", "--ui"]);
      let state = wait(&|state| state["current"]["enabled"] == true && state["busy"] == false);
      assert_eq!(
        state["notifications"], false,
        "reopen silently selected an existing grant"
      );
      assert_eq!(
        state["current"]["grants"]["notifications"], true,
        "reopen changed the saved grant"
      );
      click(&state, "revokeButton");
      let state = wait(&|state| {
        state["current"]["enabled"] == false
          && state["current"]["approved"] == false
          && state["busy"] == false
      });
      click(&state, "closeButton");
      wait(&|state| state["visible"] == false);
    } else {
      run("omarchy-plugin-disable", &["test.activation"]);
    }
    progress.send(Stage::Disabled).unwrap();
  });
  let start = Instant::now();
  let mut enabled = false;
  let mut enabled_at = 0;
  let mut clicked = false;
  let mut clicked_underneath = false;
  let mut verified = false;
  let mut disabled = false;
  let mut withdrawn = false;
  let mut latest_frame: Option<desktop::Frame> = None;
  while start.elapsed() < Duration::from_secs(15) {
    let time = start.elapsed().as_millis() as u32;
    while let Ok(stage) = stages.try_recv() {
      match stage {
        Stage::Enabled => {
          enabled = true;
          enabled_at = time;
        }
        Stage::Disabled => disabled = true,
        Stage::Click(x, y) => {
          for kind in [0, 1] {
            outer.graphics.input(kind, 0x110, x, y, time).unwrap();
          }
        }
        Stage::Key(code) => {
          for kind in [3, 4] {
            outer.graphics.input(kind, code, 0, 0, time).unwrap();
          }
        }
        Stage::Scroll(vertical) => outer
          .graphics
          .scroll(
            Scroll {
              source: 0,
              x: 200,
              y: 290,
              horizontal: 0,
              vertical,
            },
            time,
          )
          .unwrap(),
        Stage::Capture(label) => {
          if let (Some(frame), Some(path)) = (
            &latest_frame,
            std::env::var_os("OMARCHY_TEST_REVIEW_CAPTURE"),
          ) {
            frame.save(PathBuf::from(path).with_extension(format!("{label}.ppm")));
          }
        }
      }
    }
    if enabled && !clicked && time > enabled_at + 150 {
      for kind in [0, 1] {
        outer.graphics.input(kind, 0x110, 320, 245, time).unwrap();
      }
      clicked = true;
    }
    for frame in outer.step(time) {
      let count = |rgb| frame.count(rgb);
      if !clicked_underneath && count([0x44, 0xee, 0x22]) > 1000 {
        for kind in [0, 1] {
          outer.graphics.input(kind, 0x110, 20, 20, time).unwrap();
        }
        clicked_underneath = true;
      }
      if !verified && count([0x44, 0xee, 0x22]) > 1000 && count([0xee, 0xaa, 0x22]) > 1000 {
        if let Some(path) = std::env::var_os("OMARCHY_TEST_ACTIVATION_CAPTURE") {
          frame.save(path);
        }
        verified = true;
        resume.send(()).unwrap();
      }
      withdrawn = count([0x30, 0x40, 0x50]) == 0 && count([0xee, 0xaa, 0x22]) > 1000;
      latest_frame = Some(frame);
    }
    if (disabled && withdrawn) || host.0.try_wait().unwrap().is_some() {
      break;
    }
    std::thread::sleep(Duration::from_millis(5));
  }
  eprintln!(
    "host exit: {:?}\n{}",
    host.0.try_wait().unwrap(),
    fs::read_to_string(log).unwrap()
  );
  commands.join().unwrap();
  assert!(
    enabled && verified && disabled && withdrawn,
    "enabled={enabled}, input+click-through={verified}, disabled={disabled}, withdrawn={withdrawn}"
  );
}
