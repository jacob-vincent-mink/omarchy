#[cfg(feature = "graphics")]
#[path = "support/media_widget.rs"]
mod media_widget;
mod support;
use omarchy_plugin_host::{
  grants::{Grants, ReadDirectory},
  media::MediaProxy,
  supervisor::{Limits, Unit},
  worker,
};
use std::{
  ffi::{OsStr, OsString},
  fs,
  io::{Read, Write},
  os::unix::{
    fs::PermissionsExt,
    net::{UnixListener, UnixStream},
  },
  path::Path,
  time::{Duration, Instant},
};
use support::{ALIAS, OBJECT, OTHER, SELECTED, call, path_fd};

#[test]
fn media_worker_child() {
  if !Path::new("/bootstrap").exists() {
    return;
  }
  worker::restrict_bootstrap().unwrap();
  let address = std::env::var("DBUS_SESSION_BUS_ADDRESS").unwrap();
  assert_eq!(address, "unix:path=/run/plugin/media");
  let mut display = UnixStream::connect("/run/plugin/wayland").unwrap();
  display
    .set_read_timeout(Some(Duration::from_secs(2)))
    .unwrap();
  let result = std::panic::catch_unwind(|| {
    let selected_owner = fs::read_to_string("/plugin/selected").unwrap();
    let other_owner = fs::read_to_string("/plugin/other").unwrap();
    let player = "org.mpris.MediaPlayer2.Player";
    for name in [SELECTED, selected_owner.as_str()] {
      assert!(
        call(&address, name, OBJECT, player, "PlayPause", &[])
          .status
          .success()
      );
      let properties = call(
        &address,
        name,
        OBJECT,
        "org.freedesktop.DBus.Properties",
        "GetAll",
        &["s", player],
      );
      assert!(
        properties.status.success(),
        "property read denied: {}",
        String::from_utf8_lossy(&properties.stderr)
      );
      assert!(String::from_utf8_lossy(&properties.stdout).contains("Sandbox track"));
      // Check after allowed calls too: acquiring visibility must not turn a
      // method-filtered unique owner into an unrestricted service connection.
      for (path, interface, method) in [
        (OBJECT, player, "OpenUri"),
        (OBJECT, "org.mpris.MediaPlayer2", "Quit"),
        (OBJECT, "org.mpris.MediaPlayer2", "Raise"),
        (OBJECT, "org.freedesktop.DBus.Properties", "Set"),
        (OBJECT, "org.example.Unrelated", "Unsafe"),
        ("/different", player, "PlayPause"),
      ] {
        assert!(
          !call(&address, name, path, interface, method, &[])
            .status
            .success(),
          "unexpected authority: {name} {interface}.{method}"
        );
      }
    }
    for name in [
      OTHER,
      ALIAS,
      other_owner.as_str(),
      "org.freedesktop.Notifications",
    ] {
      assert!(
        !call(&address, name, OBJECT, player, "PlayPause", &[])
          .status
          .success(),
        "unselected name accessible: {name}"
      );
    }
    let names = call(
      &address,
      "org.freedesktop.DBus",
      "/org/freedesktop/DBus",
      "org.freedesktop.DBus",
      "ListNames",
      &[],
    );
    assert!(names.status.success());
    let names = String::from_utf8(names.stdout).unwrap();
    assert!(names.contains(SELECTED) && !names.contains(OTHER) && !names.contains(ALIAS));
    assert!(
      !call(
        &address,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "RequestName",
        &["su", "org.mpris.MediaPlayer2.Impostor", "0"]
      )
      .status
      .success()
    );
    assert!(
      !call(
        &address,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus.Monitoring",
        "BecomeMonitor",
        &["asu", "0", "0"]
      )
      .status
      .success()
    );
    assert_eq!(
      UnixStream::connect("/grants/bus/bus")
        .unwrap_err()
        .raw_os_error(),
      Some(libc::EACCES)
    );
    std::os::unix::fs::symlink("/grants/bus/bus", "/tmp/bus").unwrap();
    assert_eq!(
      UnixStream::connect("/tmp/bus").unwrap_err().raw_os_error(),
      Some(libc::EACCES)
    );
  });
  if result.is_err() {
    display.write_all(b"FAIL").unwrap();
    return;
  }
  let mut connected = UnixStream::connect("/run/plugin/media").unwrap();
  connected
    .set_read_timeout(Some(Duration::from_secs(1)))
    .unwrap();
  display.write_all(b"PASS").unwrap();
  let mut byte = [0];
  display.read_exact(&mut byte).unwrap();
  assert_eq!(byte, [b'R']);
  assert_eq!(
    connected.read(&mut byte).unwrap(),
    0,
    "old proxy connection survived shutdown"
  );
  assert!(
    !call(
      &address,
      SELECTED,
      OBJECT,
      "org.mpris.MediaPlayer2.Player",
      "PlayPause",
      &[]
    )
    .status
    .success()
  );
  display.write_all(b"GONE").unwrap();
}

#[test]
fn media_controller_child() {
  let Some(root) = std::env::var_os("OMARCHY_MEDIA_TEST_ROOT") else {
    return;
  };
  let root = Path::new(&root);
  let address = std::env::var("OMARCHY_MEDIA_TEST_BUS").unwrap();
  let proxy = MediaProxy::start(&address, root, SELECTED, Limits::default()).unwrap();
  let listener = UnixListener::bind(root.join("wayland")).unwrap();
  listener.set_nonblocking(true).unwrap();
  let mut grants = Grants {
    media: Some(SELECTED.into()),
    ..Grants::default()
  };
  grants.read.insert(
    "bus".into(),
    ReadDirectory::select(
      Path::new(address.strip_prefix("unix:path=").unwrap())
        .parent()
        .unwrap(),
    )
    .unwrap(),
  );
  let mut child = support::Process(
    worker::spawn(
      &path_fd(&std::env::current_exe().unwrap()),
      &path_fd(&root.join("bundle")),
      &path_fd(&root.join("wayland")),
      &[
        OsStr::new("--exact"),
        OsStr::new("media_worker_child"),
        OsStr::new("--nocapture"),
      ],
      Limits::default(),
      &grants,
      worker::Resources {
        render_node: None,
        media: Some(&proxy),
        requests: None,
        runtime: None,
        context: None,
      },
    )
    .unwrap(),
  );
  let deadline = Instant::now() + Duration::from_secs(3);
  let mut connection = loop {
    if let Ok((stream, _)) = listener.accept() {
      break stream;
    }
    assert!(
      Instant::now() < deadline && child.0.try_wait().unwrap().is_none(),
      "media worker did not connect"
    );
    std::thread::sleep(Duration::from_millis(5));
  };
  connection
    .set_read_timeout(Some(Duration::from_secs(3)))
    .unwrap();
  let mut result = [0; 4];
  connection.read_exact(&mut result).unwrap();
  if &result != b"PASS" {
    let _ = child.0.wait();
    let mut log = String::new();
    if let Some(stderr) = &mut child.0.stderr {
      stderr.read_to_string(&mut log).unwrap();
    }
    panic!("media worker failed: {log}");
  }
  drop(proxy);
  connection.write_all(b"R").unwrap();
  connection.read_exact(&mut result).unwrap();
  assert_eq!(&result, b"GONE");
  fs::write(root.join("passed"), result).unwrap();
}

#[test]
fn selected_media_is_filtered_inside_the_worker_and_disconnects_on_shutdown() {
  let Some(player) = std::env::var_os("OMARCHY_TEST_MEDIA_PLAYER") else {
    eprintln!("set OMARCHY_TEST_MEDIA_PLAYER to the built private player fixture");
    return;
  };
  if std::env::var("OMARCHY_TEST_SYSTEMD").as_deref() != Ok("1") {
    return;
  }
  let bus = support::Bus::new(&player);
  // The trusted fake service accepts these. Rejection in the worker therefore
  // proves filtering rather than missing methods or absent destinations.
  for name in [SELECTED, OTHER, ALIAS] {
    assert!(
      call(
        &bus.address,
        name,
        OBJECT,
        "org.mpris.MediaPlayer2.Player",
        "OpenUri",
        &[]
      )
      .status
      .success()
    );
  }
  let root = tempfile::Builder::new()
    .permissions(fs::Permissions::from_mode(0o700))
    .tempdir()
    .unwrap();
  fs::create_dir(root.path().join("bundle")).unwrap();
  fs::write(root.path().join("bundle/selected"), &bus.selected_owner).unwrap();
  fs::write(root.path().join("bundle/other"), &bus.other_owner).unwrap();
  let args = [
    OsString::from(format!("OMARCHY_MEDIA_TEST_ROOT={}", root.path().display())),
    OsString::from(format!("OMARCHY_MEDIA_TEST_BUS={}", bus.address)),
    std::env::current_exe().unwrap().into_os_string(),
    "--exact".into(),
    "media_controller_child".into(),
    "--nocapture".into(),
  ];
  let mut unit = Unit::start(
    Path::new("/usr/bin/env"),
    &args.iter().map(OsString::as_os_str).collect::<Vec<_>>(),
    Limits::default(),
  )
  .unwrap();
  let deadline = Instant::now() + Duration::from_secs(7);
  while unit.running().unwrap() && Instant::now() < deadline {
    std::thread::sleep(Duration::from_millis(10));
  }
  unit.stop().unwrap();
  let result = fs::read(root.path().join("passed")).unwrap_or_else(|error| {
    let log = std::process::Command::new("journalctl")
      .args(["--user", "--no-pager", "-n", "40", "-u", unit.name()])
      .output()
      .unwrap();
    panic!(
      "media controller failed: {error}\n{}",
      String::from_utf8_lossy(&log.stdout)
    );
  });
  assert_eq!(result, b"GONE");
  // Proxy teardown cannot stop the selected service or unrelated bus clients.
  assert!(
    call(
      &bus.address,
      OTHER,
      OBJECT,
      "org.mpris.MediaPlayer2.Player",
      "PlayPause",
      &[]
    )
    .status
    .success()
  );
}

#[cfg(feature = "graphics")]
#[test]
fn admitted_media_widget_uses_native_mpris_and_loses_access_on_revocation() {
  use omarchy_plugin_host::{
    controller::{Control, Scroll},
    presentation::{Event, Viewport},
    revision::Revision,
    session::{Session, Update},
    store::Store,
  };
  use smithay::backend::{
    allocator::{
      Fourcc, Modifier,
      dmabuf::{Dmabuf, DmabufFlags},
    },
    egl::{EGLContext, EGLDevice, EGLDisplay},
    renderer::{ExportMem, ImportDma, gles::GlesRenderer},
  };
  let Some(player) = std::env::var_os("OMARCHY_TEST_MEDIA_PLAYER") else {
    return;
  };
  if std::env::var("OMARCHY_TEST_SYSTEMD").as_deref() != Ok("1")
    || std::env::var("OMARCHY_TEST_GRAPHICS").as_deref() != Ok("1")
  {
    return;
  }
  let bus = support::Bus::new(&player);
  let root = tempfile::Builder::new()
    .permissions(fs::Permissions::from_mode(0o700))
    .tempdir()
    .unwrap();
  let source = root.path().join("source");
  fs::create_dir(&source).unwrap();
  media_widget::package(&source);
  fs::write(source.join("manifest.json"), serde_json::to_vec(&serde_json::json!({
    "schemaVersion": 1, "id": "test.media", "name": "Media", "version": "1", "kinds": ["panel"],
    "entryPoints": {"panel": "worker.qml"}, "sandbox": {"version": 1, "entryPoint": "worker.qml", "requests": {"media": true}}
  })).unwrap()).unwrap();
  let state = root.path().join("state");
  let store = Store::initialize(&state).unwrap();
  let revision = Revision::import(&source, &store.revisions()).unwrap();
  store
    .approve(
      &revision.digest,
      Grants {
        media: Some(SELECTED.into()),
        ..Grants::default()
      },
    )
    .unwrap();
  // Only this test controller sees the fake bus; do not change user-manager
  // environment or expose a manifest-selected bus address in the runtime API.
  let controller = root.path().join("controller");
  let quote = |value: &str| format!("'{}'", value.replace('\'', "'\\''"));
  fs::write(
    &controller,
    format!(
      "#!/bin/bash\nexport DBUS_SESSION_BUS_ADDRESS={}\nexec {} \"$@\"\n",
      quote(&bus.address),
      quote(env!("CARGO_BIN_EXE_omarchy-plugin-host"))
    ),
  )
  .unwrap();
  fs::set_permissions(&controller, fs::Permissions::from_mode(0o700)).unwrap();
  let session = Session::start(
    state,
    "test.media".into(),
    controller,
    Viewport {
      width: 600,
      height: 320,
      scale: 1,
    },
  )
  .unwrap();
  let device = EGLDevice::enumerate()
    .unwrap()
    .find(|device| device.render_device_path().is_ok())
    .unwrap();
  let egl = unsafe { EGLDisplay::new(device).unwrap() };
  let mut renderer = unsafe { GlesRenderer::new(EGLContext::new(&egl).unwrap()).unwrap() };
  let mut buffers: [Option<Dmabuf>; 2] = [None, None];
  let click = |button, x, y| {
    for kind in [0, 1] {
      session
        .send(Control::Input {
          kind,
          code: button,
          x,
          y,
        })
        .unwrap();
    }
  };
  let scroll = |vertical| {
    session
      .send(Control::Scroll(Scroll {
        source: 0,
        x: 200,
        y: 18,
        horizontal: 0,
        vertical,
      }))
      .unwrap();
  };
  let mut phase = 0;
  let mut frame_number = 0;
  let start = Instant::now();
  let mut animation_sample = None;
  let mut animated = false;
  let mut failure = String::new();
  while start.elapsed() < Duration::from_secs(10) && phase < 12 {
    match session
      .poll()
      .unwrap_or_else(|error| Some(Update::Failed(error.to_string())))
    {
      Some(Update::Failed(error)) => {
        failure = error;
        break;
      }
      Some(Update::Presentation(Event::Buffer(buffer))) => {
        let mut builder = Dmabuf::builder(
          (buffer.width as i32, buffer.height as i32),
          Fourcc::Argb8888,
          Modifier::Linear,
          DmabufFlags::empty(),
        );
        assert!(builder.add_plane(buffer.fd, 0, 0, buffer.stride));
        buffers[buffer.slot as usize] = builder.build();
      }
      Some(Update::Presentation(Event::Frame { serial, slot, .. })) => {
        frame_number += 1;
        let texture = renderer
          .import_dmabuf(buffers[slot as usize].as_ref().unwrap(), None)
          .unwrap();
        let mapping = renderer
          .copy_texture(
            &texture,
            smithay::utils::Rectangle::from_size((600, 320).into()),
            Fourcc::Abgr8888,
          )
          .unwrap();
        let bytes = renderer.map_texture(&mapping).unwrap();
        let count = |rgb: [u8; 3]| bytes.chunks_exact(4).filter(|p| p[..3] == rgb).count();
        let loaded = count([0x44, 0xee, 0x22]) > 200;
        let playing = count([0x33, 0xcc, 0x88]) > 200;
        let paused = count([0x33, 0x88, 0xff]) > 200;
        let popup = count([0xbb, 0x66, 0x33]) > 200;
        let track0 = count([0xff, 0xee, 0x44]) > 200;
        let track1 = count([0xee, 0x55, 0x99]) > 200;
        let track2 = count([0xff, 0x77, 0x44]) > 200;
        let art = count([0xee, 0xb3, 0x44]) > 1500;
        let tooltip = count([0xee, 0xdd, 0x99]) > 200;
        // The bar occupies at most 600*36 dark pixels. Extra dark pixels
        // below it prove the tooltip's actual popup rendered, not just `open`.
        let dark_pixels = count([0x10, 0x13, 0x15]);
        let popup_pixels = bytes
          .chunks_exact(600 * 4)
          .skip(36)
          .take(260)
          .any(|row| row.chunks_exact(4).any(|pixel| pixel[3] != 0));
        if phase >= 10
          && bytes
            .chunks_exact(600 * 4)
            .skip(100)
            .take(100)
            .any(|row| row.chunks_exact(4).any(|pixel| pixel[3] != 0))
        {
          failure = "dismissed media popup reappeared during its fade-out".into();
          break;
        }
        if phase == 0 && loaded && paused {
          // Sample only the widget's label region, not fixture telemetry. The
          // source widget's scrolling text must actually change rendered pixels.
          let label = bytes
            .chunks_exact(600 * 4)
            .take(36)
            .flat_map(|row| row[210 * 4..380 * 4].iter().copied())
            .collect::<Vec<_>>();
          if let Some(previous) = &animation_sample {
            animated |= previous != &label;
          } else {
            animation_sample = Some(label);
          }
          if animated && start.elapsed() > Duration::from_secs(2) {
            click(0x110, 200, 18);
            phase = 1;
          }
        } else if phase == 1 && playing {
          click(0x112, 200, 18);
          phase = 2;
        } else if phase == 2 && track1 {
          scroll(120);
          phase = 3;
        } else if phase == 3 && track0 {
          scroll(-120);
          phase = 4;
        } else if phase == 4 && track1 {
          click(0x111, 200, 18);
          phase = 5;
        } else if phase == 5 && popup && art {
          click(0x110, 338, 148);
          phase = 6;
        } else if phase == 6 && track2 {
          click(0x110, 230, 148);
          phase = 7;
        } else if phase == 7 && track1 {
          click(0x110, 284, 148);
          phase = 8;
        } else if phase == 8 && paused {
          session
            .send(Control::Input {
              kind: 5,
              code: 0,
              x: 0,
              y: 0,
            })
            .unwrap();
          phase = 9;
        } else if phase == 9 && !popup && !popup_pixels {
          session
            .send(Control::Input {
              kind: 2,
              code: 0,
              x: 200,
              y: 18,
            })
            .unwrap();
          phase = 10;
        } else if phase == 10 && tooltip && dark_pixels > 28_000 {
          session
            .send(Control::Input {
              kind: 2,
              code: 0,
              x: 500,
              y: 18,
            })
            .unwrap();
          phase = 11;
        } else if phase == 11 && !tooltip && !popup_pixels {
          phase = 12;
        }
        if popup
          && art
          && let Some(path) = std::env::var_os("OMARCHY_TEST_MEDIA_CAPTURE")
        {
          let mut file = std::io::BufWriter::new(fs::File::create(path).unwrap());
          write!(file, "P6\n600 320\n255\n").unwrap();
          for pixel in bytes.chunks_exact(4) {
            file.write_all(&pixel[..3]).unwrap();
          }
        }
        if let Some(directory) = std::env::var_os("OMARCHY_TEST_MEDIA_FRAMES") {
          let mut file = std::io::BufWriter::new(
            fs::File::create(Path::new(&directory).join(format!("{frame_number:04}.ppm"))).unwrap(),
          );
          write!(file, "P6\n600 320\n255\n").unwrap();
          for pixel in bytes.chunks_exact(4) {
            file.write_all(&pixel[..3]).unwrap();
          }
        }
        if let Err(error) = session.send(Control::Presented(serial)) {
          failure = error.to_string();
          break;
        }
      }
      _ => (),
    }
    std::thread::sleep(Duration::from_millis(5));
  }
  if phase < 12
    && let Some(unit) = store.read("test.media").unwrap().active_unit
  {
    let log = std::process::Command::new("journalctl")
      .args(["--user", "--no-pager", "-n", "100", "-u", &unit])
      .output()
      .unwrap();
    eprintln!("{}", String::from_utf8_lossy(&log.stdout));
  }
  store.revoke("test.media").unwrap();
  assert!(
    phase == 12 && animated,
    "ported media widget failed at phase {phase}, animation={animated}, frames={frame_number}, error={failure}"
  );
  let deadline = Instant::now() + Duration::from_secs(2);
  loop {
    match session.poll() {
      Err(_) | Ok(Some(Update::Failed(_))) => break,
      _ if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(5)),
      _ => panic!("revoked media session remained connected"),
    }
  }
  assert!(
    call(
      &bus.address,
      OTHER,
      OBJECT,
      "org.mpris.MediaPlayer2.Player",
      "PlayPause",
      &[]
    )
    .status
    .success()
  );
}
