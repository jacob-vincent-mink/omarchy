use omarchy_ward::{
  channel::{Channel, Listener},
  controller::Control,
  grants::Grants,
  revision::Revision,
  store::Store,
  supervisor::{Limits, Unit},
};
use std::{
  ffi::OsStr,
  io,
  os::unix::fs::PermissionsExt,
  path::Path,
  time::{Duration, Instant},
};

fn receive(channel: &Channel) -> Control {
  let deadline = Instant::now() + Duration::from_secs(2);
  loop {
    match channel.receive() {
      Ok(packet) => return Control::decode(packet).unwrap(),
      Err(error) if error.kind() == io::ErrorKind::WouldBlock && Instant::now() < deadline => {
        std::thread::sleep(Duration::from_millis(10))
      }
      Err(error) => panic!("controller did not reply: {error}"),
    }
  }
}

#[test]
fn recorded_controller_is_stopped_by_revocation_or_interrupted_publication() {
  if std::env::var("OMARCHY_TEST_SYSTEMD").as_deref() != Ok("1") {
    return;
  }
  for pending in [false, true] {
    let root = tempfile::Builder::new()
      .permissions(std::fs::Permissions::from_mode(0o700))
      .tempdir()
      .unwrap();
    let source = root.path().join("source");
    std::fs::create_dir(&source).unwrap();
    std::fs::write(
      source.join("worker.qml"),
      "import Quickshell\nShellRoot {}\n",
    )
    .unwrap();
    std::fs::write(source.join("manifest.json"), serde_json::to_vec(&serde_json::json!({
      "schemaVersion": 1, "id": "test.widget", "name": "Test", "version": "1", "kinds": ["barWidget"],
      "entryPoints": {"barWidget": "worker.qml"}, "sandbox": {"version": 1, "entryPoint": "worker.qml", "requests": {}}
    })).unwrap()).unwrap();
    let store = Store::initialize(&root.path().join("state")).unwrap();
    let revision = Revision::import(&source, &store.revisions()).unwrap();
    let approved = store.approve(&revision.digest, Grants::default()).unwrap();
    let path = root.path().join("host");
    let listener = Listener::bind(&path).unwrap();
    let (mut unit, running) = store
      .launch(
        "test.widget",
        Path::new(env!("CARGO_BIN_EXE_omarchy-ward")),
        &path,
      )
      .unwrap();
    assert!(running.epoch > approved.epoch);
    let deadline = Instant::now() + Duration::from_secs(2);
    let channel = loop {
      match listener.accept() {
        Ok(channel) => break channel,
        Err(error) if error.kind() == io::ErrorKind::WouldBlock && Instant::now() < deadline => {
          std::thread::sleep(Duration::from_millis(10))
        }
        Err(error) => panic!("approved controller did not connect: {error}"),
      }
    };
    unit.authenticate(&channel).unwrap();
    assert_eq!(receive(&channel), Control::Hello);
    Control::Ping(1).send(&channel).unwrap();
    assert_eq!(receive(&channel), Control::Pong(1));
    if pending {
      std::fs::File::create(root.path().join("state/test.widget.pending"))
        .unwrap()
        .sync_all()
        .unwrap();
    } else {
      store.revoke("test.widget").unwrap();
    }
    let deadline = Instant::now() + Duration::from_secs(2);
    while unit.running().unwrap() && Instant::now() < deadline {
      std::thread::sleep(Duration::from_millis(20));
    }
    assert!(!unit.running().unwrap(), "revoked controller survived");
    assert!(
      unit.authenticate(&channel).is_err(),
      "dead peer was admitted"
    );
    if pending {
      store.recover("test.widget").unwrap();
    }
    let revoked = store.read("test.widget").unwrap();
    assert!(!revoked.enabled && revoked.active_unit.is_none());
    assert!(
      store
        .with_authority("test.widget", running.epoch, unit.name(), |_| Ok(()))
        .is_err()
    );
    unit.stop().unwrap();
  }
}

#[test]
fn real_controller_admission_and_host_lease() {
  if std::env::var("OMARCHY_TEST_SYSTEMD").as_deref() != Ok("1") {
    eprintln!("set OMARCHY_TEST_SYSTEMD=1 for temporary controller channel tests");
    return;
  }
  for mode in ["stop", "eof", "silent", "replay", "malformed", "unexpected"] {
    let root = tempfile::Builder::new()
      .permissions(std::fs::Permissions::from_mode(0o700))
      .tempdir()
      .unwrap();
    let path = root.path().join("host");
    let listener = Listener::bind(&path).unwrap();
    assert!(
      Listener::bind(&path).is_err(),
      "must not replace an existing listener"
    );
    let mut unit = Unit::start(
      Path::new(env!("CARGO_BIN_EXE_omarchy-ward")),
      &[OsStr::new("--controller"), path.as_os_str()],
      Limits::default(),
    )
    .unwrap();
    let deadline = Instant::now() + Duration::from_secs(2);
    let channel = loop {
      match listener.accept() {
        Ok(channel) => break channel,
        Err(error) if error.kind() == io::ErrorKind::WouldBlock && Instant::now() < deadline => {
          std::thread::sleep(Duration::from_millis(10))
        }
        Err(error) => panic!("controller did not connect: {error}"),
      }
    };
    unit.authenticate(&channel).unwrap();
    let (impostor, _other) = Channel::pair().unwrap();
    assert!(
      unit.authenticate(&impostor).is_err(),
      "same uid is not controller identity"
    );
    assert_eq!(receive(&channel), Control::Hello);
    Control::Ping(1).send(&channel).unwrap();
    assert_eq!(receive(&channel), Control::Pong(1));
    match mode {
      "stop" => Control::Stop.send(&channel).unwrap(),
      "eof" => drop(channel),
      "silent" => (),
      "replay" => Control::Ping(1).send(&channel).unwrap(),
      "malformed" => channel.send(b"unknown", &[]).unwrap(),
      "unexpected" => Control::Pong(2).send(&channel).unwrap(),
      _ => unreachable!(),
    }
    let deadline = Instant::now() + Duration::from_secs(4);
    while unit.running().unwrap() && Instant::now() < deadline {
      std::thread::sleep(Duration::from_millis(20));
    }
    assert!(!unit.running().unwrap(), "controller survived {mode}");
    unit.stop().unwrap();
    println!("verified controller {mode}");
  }
}
