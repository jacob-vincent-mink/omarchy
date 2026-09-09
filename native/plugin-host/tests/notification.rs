use omarchy_plugin_host::{
  channel::{Channel, Packet},
  controller::Approval,
  grants::Grants,
  notification::Broker,
  revision::Revision,
  store::Store,
  supervisor::{self, Limits},
  worker,
};
use std::{
  ffi::OsStr,
  fs::{self, File, OpenOptions},
  io::{self, Read, Write},
  os::{
    fd::AsFd,
    unix::{
      fs::{OpenOptionsExt, PermissionsExt},
      net::{UnixListener, UnixStream},
    },
  },
  path::Path,
  time::{Duration, Instant},
};

fn receive(channel: &Channel) -> String {
  let deadline = Instant::now() + Duration::from_secs(3);
  loop {
    match channel.receive() {
      Ok(Packet { bytes, fds }) => {
        assert!(fds.is_empty());
        let value: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(value["version"], 1);
        return value["status"].as_str().unwrap().into();
      }
      Err(error) if error.kind() == io::ErrorKind::WouldBlock && Instant::now() < deadline => {
        std::thread::sleep(Duration::from_millis(5));
      }
      Err(error) => panic!("notification reply missing: {error}"),
    }
  }
}

fn send(bytes: &[u8]) -> Channel {
  let channel = Channel::connect(Path::new("/run/plugin/notify")).unwrap();
  channel.send(bytes, &[]).unwrap();
  channel
}

#[test]
fn notification_worker_child() {
  if !Path::new("/bootstrap").exists() {
    return;
  }
  worker::restrict_bootstrap().unwrap();
  let mut display = UnixStream::connect("/run/plugin/wayland").unwrap();
  let mode = fs::read_to_string("/plugin/mode").unwrap();
  if mode == "denied" {
    assert!(!Path::new("/run/plugin/notify").exists());
    assert!(omarchy_plugin_host::notification::request("test".into(), "body".into()).is_err());
  } else if mode == "timeout" {
    let started = Instant::now();
    assert_eq!(
      receive(&send(br#"{"version":1,"title":"timeout","body":""}"#)),
      "failed"
    );
    assert!(started.elapsed() < Duration::from_secs(2));
  } else if mode == "revoke" {
    let channel = send(br#"{"version":1,"title":"revoke","body":""}"#);
    // The parent revokes while the private recorder is blocked. No success or
    // continued worker execution is permitted after the whole unit is stopped.
    assert_ne!(receive(&channel), "delivered");
    panic!("revocation should terminate the worker before it continues");
  } else {
    for bytes in [
      b"malformed".as_slice(),
      br#"{"version":1,"title":"x","body":"","id":"other"}"#,
      br#"{"version":1,"title":"x","body":"","exec":"sh"}"#,
    ] {
      assert_eq!(receive(&send(bytes)), "invalid");
    }
    let channel = Channel::connect(Path::new("/run/plugin/notify")).unwrap();
    channel
      .send(
        br#"{"version":1,"title":"x","body":""}"#,
        &[File::open("/dev/null").unwrap().as_fd()],
      )
      .unwrap();
    assert_eq!(receive(&channel), "invalid");
    let first = send(
      br#"{"version":1,"title":"--image=/secret","body":"--exec <img src='/secret'/> & body"}"#,
    );
    std::thread::sleep(Duration::from_millis(50));
    assert_eq!(
      receive(&send(br#"{"version":1,"title":"busy","body":""}"#)),
      "busy"
    );
    assert_eq!(receive(&first), "delivered");
    omarchy_plugin_host::notification::request("second".into(), "line one\nline two".into())
      .unwrap();
    assert_eq!(
      receive(&send(br#"{"version":1,"title":"rate","body":""}"#)),
      "rate_limited"
    );
  }
  display.write_all(b"PASS").unwrap();
}

#[test]
fn notification_controller_child() {
  let Some(root) = std::env::var_os("OMARCHY_NOTIFICATION_TEST_ROOT") else {
    return;
  };
  let root = Path::new(&root);
  let state = root.join("state");
  let epoch = std::env::var("OMARCHY_NOTIFICATION_TEST_EPOCH")
    .unwrap()
    .parse()
    .unwrap();
  let approval = Approval::open(&state, "test.notification", epoch).unwrap();
  let record = Store::open(&state)
    .unwrap()
    .read("test.notification")
    .unwrap();
  let mut broker = if record.grants.notifications {
    Some(Broker::start(root).unwrap())
  } else {
    None
  };
  let listener = UnixListener::bind(root.join("wayland")).unwrap();
  listener.set_nonblocking(true).unwrap();
  let path_fd = |path: &Path| {
    OpenOptions::new()
      .read(true)
      .custom_flags(libc::O_PATH | libc::O_NOFOLLOW)
      .open(path)
      .unwrap()
  };
  let mut child = worker::spawn(
    &path_fd(&std::env::current_exe().unwrap()),
    &path_fd(
      &Store::open(&state)
        .unwrap()
        .revisions()
        .join(record.revision),
    ),
    &path_fd(&root.join("wayland")),
    &[
      OsStr::new("--exact"),
      OsStr::new("notification_worker_child"),
      OsStr::new("--nocapture"),
    ],
    Limits::default(),
    &record.grants,
    worker::Resources {
      notifications: broker.as_ref(),
      ..Default::default()
    },
  )
  .unwrap();
  let mut display = None;
  let deadline = Instant::now() + Duration::from_secs(4);
  loop {
    if let Some(broker) = &mut broker {
      broker.dispatch(&approval).unwrap();
    }
    if display.is_none()
      && let Ok((stream, _)) = listener.accept()
    {
      stream.set_nonblocking(true).unwrap();
      display = Some(stream);
    }
    let mut bytes = [0; 4];
    if display
      .as_mut()
      .is_some_and(|stream| stream.read(&mut bytes).is_ok_and(|count| count == 4))
    {
      assert_eq!(&bytes, b"PASS");
      fs::write(root.join("passed"), b"PASS").unwrap();
      break;
    }
    if let Some(status) = child.try_wait().unwrap() {
      let mut log = String::new();
      child
        .stderr
        .take()
        .unwrap()
        .read_to_string(&mut log)
        .unwrap();
      panic!("notification worker exited {status}: {log}");
    }
    assert!(Instant::now() < deadline, "notification test timed out");
    supervisor::watchdog().unwrap();
    std::thread::sleep(Duration::from_millis(5));
  }
}

fn quote(value: &str) -> String {
  format!("'{}'", value.replace('\'', "'\\''"))
}

#[test]
fn notification_authority_is_bounded_and_revocation_stops_inflight_delivery() {
  if std::env::var("OMARCHY_TEST_SYSTEMD").as_deref() != Ok("1") {
    return;
  }
  for mode in ["denied", "allowed", "timeout", "revoke"] {
    let root = tempfile::Builder::new()
      .permissions(fs::Permissions::from_mode(0o700))
      .tempdir()
      .unwrap();
    let source = root.path().join("source");
    fs::create_dir(&source).unwrap();
    fs::write(
      source.join("worker.qml"),
      "import Quickshell\nShellRoot {}\n",
    )
    .unwrap();
    fs::write(source.join("mode"), mode).unwrap();
    fs::write(source.join("manifest.json"), serde_json::to_vec(&serde_json::json!({
      "schemaVersion": 1, "id": "test.notification", "name": "Test", "version": "1", "kinds": ["panel"],
      "entryPoints": {"panel": "worker.qml"}, "sandbox": {"version": 1, "entryPoint": "worker.qml", "requests": {"notifications": true}}
    })).unwrap()).unwrap();
    let store = Store::initialize(&root.path().join("state")).unwrap();
    let revision = Revision::import(&source, &store.revisions()).unwrap();
    store
      .approve(
        &revision.digest,
        Grants {
          notifications: mode != "denied",
          ..Default::default()
        },
      )
      .unwrap();
    fs::create_dir(root.path().join("bin")).unwrap();
    let helper = root.path().join("bin/omarchy-notification-send");
    fs::write(&helper, format!("#!/bin/bash\nprintf '%s\\0' \"$@\" >> {}\nif [[ $7 == 'Plugin test.notification: revoke' || $7 == 'Plugin test.notification: timeout' ]]; then\n  touch {}\n  sleep 30\nelse\n  sleep 0.2\nfi\n",
      quote(root.path().join("record").to_str().unwrap()), quote(root.path().join("inflight").to_str().unwrap()))).unwrap();
    fs::set_permissions(&helper, fs::Permissions::from_mode(0o700)).unwrap();
    let controller = root.path().join("controller");
    fs::write(&controller, format!("#!/bin/bash\nexport OMARCHY_PATH={}\nexport OMARCHY_NOTIFICATION_TEST_ROOT={}\nexport OMARCHY_NOTIFICATION_TEST_EPOCH=\"$5\"\nexec {} --exact notification_controller_child --nocapture\n",
      quote(root.path().to_str().unwrap()), quote(root.path().to_str().unwrap()), quote(std::env::current_exe().unwrap().to_str().unwrap()))).unwrap();
    fs::set_permissions(&controller, fs::Permissions::from_mode(0o700)).unwrap();
    let (mut unit, _) = store
      .launch(
        "test.notification",
        &controller,
        &root.path().join("unused"),
      )
      .unwrap();
    let deadline = Instant::now() + Duration::from_secs(6);
    let mut spoofed = false;
    while unit.running().unwrap() && Instant::now() < deadline {
      if mode == "allowed"
        && !spoofed
        && let Ok(channel) = Channel::connect(&root.path().join("notify"))
      {
        channel
          .send(br#"{"version":1,"title":"impostor","body":""}"#, &[])
          .unwrap();
        spoofed = true;
      }
      if mode == "revoke" && root.path().join("inflight").exists() {
        store.revoke("test.notification").unwrap();
        break;
      }
      std::thread::sleep(Duration::from_millis(5));
    }
    unit.stop().unwrap();
    assert!(!unit.running().unwrap());
    if mode == "revoke" {
      assert!(root.path().join("inflight").exists());
      assert!(!root.path().join("passed").exists());
      assert!(!store.read("test.notification").unwrap().enabled);
    } else {
      assert!(
        root.path().join("passed").exists(),
        "controller failed in {mode}: {}",
        String::from_utf8_lossy(
          &std::process::Command::new("journalctl")
            .args(["--user", "--no-pager", "-n", "40", "-u", unit.name()])
            .output()
            .unwrap()
            .stdout
        )
      );
    }
    let bytes = fs::read(root.path().join("record")).unwrap_or_default();
    let args = bytes
      .split(|byte| *byte == 0)
      .filter(|arg| !arg.is_empty())
      .map(|arg| String::from_utf8(arg.to_vec()).unwrap())
      .collect::<Vec<_>>();
    match mode {
      "denied" => assert!(args.is_empty()),
      "allowed" => {
        assert!(spoofed);
        assert_eq!(args.len(), 16);
        assert_eq!(
          &args[..6],
          &[
            "--app-name",
            "omarchy-plugin-test.notification",
            "--urgency",
            "low",
            "--expire-time",
            "5000"
          ]
        );
        assert_eq!(args[6], "Plugin test.notification: --image=/secret");
        assert_eq!(
          args[7],
          "Message: --exec &lt;img src='/secret'/&gt; &amp; body"
        );
        assert_eq!(args[14], "Plugin test.notification: second");
        assert_eq!(args[15], "Message: line one\nline two");
      }
      "revoke" | "timeout" => assert_eq!(args.len(), 8),
      _ => unreachable!(),
    }
  }
}

#[cfg(feature = "graphics")]
#[test]
fn native_quickshell_process_can_use_only_the_text_notification_helper() {
  use omarchy_plugin_host::{
    controller::Control,
    presentation::{Event, Viewport},
    session::{Session, Update},
  };
  if std::env::var("OMARCHY_TEST_SYSTEMD").as_deref() != Ok("1")
    || std::env::var("OMARCHY_TEST_GRAPHICS").as_deref() != Ok("1")
  {
    return;
  }
  let root = tempfile::Builder::new()
    .permissions(fs::Permissions::from_mode(0o700))
    .tempdir()
    .unwrap();
  let source = root.path().join("source");
  fs::create_dir(&source).unwrap();
  fs::write(source.join("worker.qml"), r#"
import QtQuick
import Quickshell
import Quickshell.Io
ShellRoot {
  FloatingWindow { implicitWidth: 64; implicitHeight: 64 }
  Process {
    command: ["/bootstrap", "--notify", "--image=/secret", "--exec <img src='/secret'/> & body"]
    running: true
    onExited: (code, status) => { if (code === 0) second.running = true; }
  }
  Process {
    id: second
    command: ["/bootstrap", "--notify", "confirmed reply", "Native Process inherited sandbox restrictions"]
  }
}
"#).unwrap();
  fs::write(source.join("manifest.json"), serde_json::to_vec(&serde_json::json!({
    "schemaVersion": 1, "id": "test.notification", "name": "Test", "version": "1", "kinds": ["panel"],
    "entryPoints": {"panel": "worker.qml"}, "sandbox": {"version": 1, "entryPoint": "worker.qml", "requests": {"notifications": true}}
  })).unwrap()).unwrap();
  let store = Store::initialize(&root.path().join("state")).unwrap();
  let revision = Revision::import(&source, &store.revisions()).unwrap();
  store
    .approve(
      &revision.digest,
      Grants {
        notifications: true,
        ..Default::default()
      },
    )
    .unwrap();
  fs::create_dir(root.path().join("bin")).unwrap();
  let helper = root.path().join("bin/omarchy-notification-send");
  fs::write(
    &helper,
    format!(
      "#!/bin/bash\nprintf '%s\\0' \"$@\" >> {}\n",
      quote(root.path().join("record").to_str().unwrap())
    ),
  )
  .unwrap();
  fs::set_permissions(&helper, fs::Permissions::from_mode(0o700)).unwrap();
  let controller = root.path().join("controller");
  fs::write(
    &controller,
    format!(
      "#!/bin/bash\nexport OMARCHY_PATH={}\nexec {} \"$@\"\n",
      quote(root.path().to_str().unwrap()),
      quote(env!("CARGO_BIN_EXE_omarchy-plugin-host"))
    ),
  )
  .unwrap();
  fs::set_permissions(&controller, fs::Permissions::from_mode(0o700)).unwrap();
  let session = Session::start(
    root.path().join("state"),
    "test.notification".into(),
    controller,
    Viewport {
      width: 64,
      height: 64,
      scale: 1,
    },
  )
  .unwrap();
  let deadline = Instant::now() + Duration::from_secs(4);
  let bytes = loop {
    match session.poll().unwrap() {
      Some(Update::Failed(error)) => panic!("Quickshell notification test failed: {error}"),
      Some(Update::Presentation(Event::Frame { serial, .. })) => {
        session.send(Control::Presented(serial)).unwrap()
      }
      _ => (),
    }
    let bytes = fs::read(root.path().join("record")).unwrap_or_default();
    if bytes.iter().filter(|byte| **byte == 0).count() == 16 {
      break bytes;
    }
    assert!(
      Instant::now() < deadline,
      "native Process did not complete two requests"
    );
    std::thread::sleep(Duration::from_millis(5));
  };
  store.revoke("test.notification").unwrap();
  let args = bytes
    .split(|byte| *byte == 0)
    .filter(|arg| !arg.is_empty())
    .collect::<Vec<_>>();
  assert_eq!(args[6], b"Plugin test.notification: --image=/secret");
  assert_eq!(
    args[7],
    b"Message: --exec &lt;img src='/secret'/&gt; &amp; body"
  );
  assert_eq!(args[14], b"Plugin test.notification: confirmed reply");
}
