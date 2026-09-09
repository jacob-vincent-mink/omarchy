use crate::{
  channel::{Channel, Packet},
  context::UiContext,
  presentation::Viewport,
  store::Store,
  supervisor::{self, Limits},
};
use std::{
  io,
  path::Path,
  time::{Duration, Instant},
};

/// Version 1 control records are 16, 24 or 28 bytes, little-endian. No plugin id
/// is accepted here: identity belongs to the launched unit and admitted channel.
#[derive(Debug, PartialEq, Eq)]
// Control queues are bounded; keep snapshots inline instead of allocating
// another box for each context update.
#[allow(clippy::large_enum_variant)]
pub enum Control {
  Hello,
  Ping(u64),
  Pong(u64),
  Stop,
  Configure(Viewport),
  Presented(u64),
  Input {
    kind: u32,
    code: u32,
    x: i32,
    y: i32,
  },
  Scroll(Scroll),
  Context(UiContext),
  PanelState {
    serial: u32,
    open: bool,
  },
}

/// Host-local position and Qt-signed deltas. Source 0 is wheel (120 units per
/// detent), 1 is finger (logical pixels), 2 ends both finger axes (zero deltas).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Scroll {
  pub source: u32,
  pub x: i32,
  pub y: i32,
  pub horizontal: i32,
  pub vertical: i32,
}
impl Scroll {
  pub fn validate(self) -> io::Result<()> {
    if self.source > 2
      || !(0..4096).contains(&self.x)
      || !(0..4096).contains(&self.y)
      || !(-4096..=4096).contains(&self.horizontal)
      || !(-4096..=4096).contains(&self.vertical)
      || ((self.horizontal == 0 && self.vertical == 0) != (self.source == 2))
    {
      return Err(invalid("invalid host scroll input"));
    }
    Ok(())
  }
}

pub struct Approval {
  store: Store,
  id: String,
  epoch: u64,
  unit: String,
}
impl Approval {
  pub fn open(root: &Path, id: &str, epoch: u64) -> io::Result<Self> {
    supervisor::verify_controller_limits(Limits::default())?;
    let store = Store::open(root)?;
    let unit = supervisor::controller_identity()?;
    store.admit(id, epoch, &unit)?;
    Ok(Self {
      store,
      id: id.into(),
      epoch,
      unit,
    })
  }
  pub(crate) fn check(&self) -> io::Result<()> {
    self
      .store
      .with_authority(&self.id, self.epoch, &self.unit, |_| Ok(()))
  }
  pub(crate) fn with_request<T>(
    &self,
    kind: crate::requests::Kind,
    effect: impl FnOnce(&str, &crate::grants::Grants) -> io::Result<T>,
  ) -> io::Result<T> {
    self
      .store
      .with_authority(&self.id, self.epoch, &self.unit, |record| {
        let granted = match kind {
          crate::requests::Kind::Notification => record.grants.notifications,
          crate::requests::Kind::Settings => record.grants.settings.can_write(),
          crate::requests::Kind::OpenUrl => record.grants.open_urls,
          crate::requests::Kind::Http => !record.grants.http.is_empty(),
          crate::requests::Kind::Exec => !record.grants.exec.is_empty(),
        };
        if !granted {
          return Err(invalid("host request grant is not current"));
        }
        effect(&self.id, &record.grants)
      })
  }
}

impl Control {
  pub fn send(&self, channel: &Channel) -> io::Result<()> {
    if let Self::Context(context) = self {
      use std::os::fd::AsFd;
      let file = context.seal()?;
      return channel.send(b"OPH\x01\x09\0\0\0\0\0\0\0\0\0\0\0", &[file.as_fd()]);
    }
    if let Self::Scroll(scroll) = self {
      scroll.validate()?;
      return send_extended(
        channel,
        8,
        [
          scroll.source,
          scroll.x as u32,
          scroll.y as u32,
          scroll.horizontal as u32,
          scroll.vertical as u32,
        ],
      );
    }
    if let Self::Configure(viewport) = self {
      viewport.pixels()?;
      return send_extended(
        channel,
        5,
        [viewport.width, viewport.height, viewport.scale, 0],
      );
    }
    if let Self::Input { kind, code, x, y } = self {
      return send_extended(channel, 7, [*kind, *code, *x as u32, *y as u32]);
    }
    let (kind, serial): (u32, u64) = match self {
      Self::Hello => (1, 0),
      Self::Ping(serial) => (2, *serial),
      Self::Pong(serial) => (3, *serial),
      Self::Stop => (4, 0),
      Self::Presented(serial) => (6, *serial),
      Self::PanelState { serial, open } => (10, u64::from(*serial) | (u64::from(*open) << 32)),
      Self::Configure(_) | Self::Input { .. } | Self::Scroll(_) | Self::Context(_) => {
        unreachable!()
      }
    };
    let mut bytes = [0u8; 16];
    bytes[..4].copy_from_slice(b"OPH\x01");
    bytes[4..8].copy_from_slice(&kind.to_le_bytes());
    bytes[8..].copy_from_slice(&serial.to_le_bytes());
    channel.send(&bytes, &[])
  }

  pub fn decode(mut packet: Packet) -> io::Result<Self> {
    let bytes = packet.bytes;
    if bytes == b"OPH\x01\x09\0\0\0\0\0\0\0\0\0\0\0" && packet.fds.len() == 1 {
      return Ok(Self::Context(UiContext::unseal(packet.fds.pop().unwrap())?));
    }
    if !packet.fds.is_empty() || ![16, 24, 28].contains(&bytes.len()) || &bytes[..4] != b"OPH\x01" {
      return Err(invalid("invalid control record"));
    }
    let kind = u32::from_le_bytes(bytes[4..8].try_into().unwrap());
    if bytes.len() == 28 {
      if kind != 8 {
        return Err(invalid("invalid scroll record kind"));
      }
      let value = |offset| i32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap());
      let scroll = Scroll {
        source: value(8) as u32,
        x: value(12),
        y: value(16),
        horizontal: value(20),
        vertical: value(24),
      };
      scroll.validate()?;
      return Ok(Self::Scroll(scroll));
    }
    if bytes.len() == 24 {
      let values = bytes[8..]
        .chunks_exact(4)
        .map(|bytes| u32::from_le_bytes(bytes.try_into().unwrap()))
        .collect::<Vec<_>>();
      return match kind {
        5 if values[3] == 0 => {
          let viewport = Viewport {
            width: values[0],
            height: values[1],
            scale: values[2],
          };
          viewport.pixels()?;
          Ok(Self::Configure(viewport))
        }
        7 if values[0] <= 5 => Ok(Self::Input {
          kind: values[0],
          code: values[1],
          x: values[2] as i32,
          y: values[3] as i32,
        }),
        _ => Err(invalid("invalid extended control record")),
      };
    }
    let serial = u64::from_le_bytes(bytes[8..].try_into().unwrap());
    match (kind, serial) {
      (1, 0) => Ok(Self::Hello),
      (2, 1..) => Ok(Self::Ping(serial)),
      (3, 1..) => Ok(Self::Pong(serial)),
      (4, 0) => Ok(Self::Stop),
      (6, 1..) => Ok(Self::Presented(serial)),
      (10, value) if value >> 32 <= 1 => Ok(Self::PanelState {
        serial: value as u32,
        open: value >> 32 == 1,
      }),
      _ => Err(invalid("unknown control record")),
    }
  }
}

fn send_extended<const N: usize>(channel: &Channel, kind: u32, values: [u32; N]) -> io::Result<()> {
  let mut bytes = Vec::with_capacity(8 + N * 4);
  bytes.extend_from_slice(b"OPH\x01");
  bytes.extend_from_slice(&kind.to_le_bytes());
  for value in values {
    bytes.extend_from_slice(&value.to_le_bytes());
  }
  channel.send(&bytes, &[])
}

/// The graphics feature starts a worker only after revision admission and an
/// explicit host viewport. Health-only controllers cannot start plugin code.
pub fn run(path: &Path, approval: Option<Approval>) -> io::Result<()> {
  supervisor::verify_controller_limits(Limits::default())?;
  let channel = Channel::connect(path)?;
  Control::Hello.send(&channel)?;
  let mut deadline = Instant::now() + Duration::from_secs(3);
  let mut last_serial = 0;
  let mut rate_window = Instant::now();
  let mut records = 0;
  let mut heartbeat = Instant::now();
  let mut authority_check = Instant::now();
  #[cfg(feature = "graphics")]
  let mut graphics: Option<RunningGraphics> = None;
  #[cfg(feature = "graphics")]
  let mut context = UiContext::default();
  #[cfg(feature = "graphics")]
  let started = Instant::now();
  loop {
    let now = Instant::now();
    if now >= deadline {
      return Err(io::Error::new(
        io::ErrorKind::TimedOut,
        "host lease expired",
      ));
    }
    if now.duration_since(rate_window) >= Duration::from_secs(1) {
      rate_window = now;
      records = 0;
    }
    if now.duration_since(authority_check) >= Duration::from_millis(100) {
      if let Some(approval) = &approval {
        approval.check()?;
      }
      authority_check = now;
    }
    // Bound each dispatch as well as the total rate; future presentation work
    // must retain both limits so input traffic cannot starve the watchdog.
    for _ in 0..32 {
      let packet = match channel.receive() {
        Ok(packet) => packet,
        Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
        Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
        Err(error) => return Err(error),
      };
      records += 1;
      if records > 2048 {
        return Err(invalid("host control rate exceeded"));
      }
      match Control::decode(packet)? {
        Control::Stop => return Ok(()),
        Control::Ping(serial) if serial > last_serial => {
          last_serial = serial;
          deadline = Instant::now() + Duration::from_secs(3);
          Control::Pong(serial).send(&channel)?;
        }
        #[cfg(feature = "graphics")]
        Control::Configure(viewport) if graphics.is_none() => {
          let approval = approval
            .as_ref()
            .ok_or_else(|| invalid("graphics requires an admitted plugin revision"))?;
          graphics = Some(RunningGraphics::start(
            approval, viewport, &channel, &context,
          )?);
        }
        #[cfg(feature = "graphics")]
        Control::Context(mut next) => {
          let approval = approval
            .as_ref()
            .ok_or_else(|| invalid("UI context requires admission"))?;
          approval.store.with_authority(
            &approval.id,
            approval.epoch,
            &approval.unit,
            |record| {
              record.grants.settings.filter(&mut next.settings);
              Ok(())
            },
          )?;
          if let Some(graphics) = &graphics {
            next.publish(&graphics._runtime.path().join("context"))?;
          }
          if next.panel != context.panel
            && next.panel.as_ref().is_some_and(|panel| panel.open)
            && let Some(graphics) = &mut graphics
          {
            graphics.display.activate();
          }
          context = next;
        }
        #[cfg(feature = "graphics")]
        Control::Configure(viewport) => graphics
          .as_mut()
          .unwrap()
          .display
          .configure(viewport, started.elapsed().as_millis() as u32)
          .map_err(graphics_error)?,
        #[cfg(feature = "graphics")]
        Control::Presented(serial) => graphics
          .as_mut()
          .ok_or_else(|| invalid("presentation before configuration"))?
          .display
          .presented(serial)
          .map_err(graphics_error)?,
        #[cfg(feature = "graphics")]
        Control::Input { kind, code, x, y } => graphics
          .as_mut()
          .ok_or_else(|| invalid("input before configuration"))?
          .display
          .input(kind, code, x, y, started.elapsed().as_millis() as u32)
          .map_err(graphics_error)?,
        #[cfg(feature = "graphics")]
        Control::Scroll(scroll) => graphics
          .as_mut()
          .ok_or_else(|| invalid("scroll before configuration"))?
          .display
          .scroll(scroll, started.elapsed().as_millis() as u32)
          .map_err(graphics_error)?,
        _ => return Err(invalid("unexpected or stale host control record")),
      }
    }
    #[cfg(feature = "graphics")]
    if let Some(graphics) = &mut graphics {
      graphics.dispatch(
        &channel,
        started.elapsed().as_millis() as u32,
        approval.as_ref().unwrap(),
      )?;
    }
    if now.duration_since(heartbeat) >= Duration::from_secs(1) {
      supervisor::watchdog()?;
      heartbeat = now;
    }
    std::thread::sleep(Duration::from_millis(10));
  }
}

#[cfg(feature = "graphics")]
fn graphics_error(error: Box<dyn std::error::Error>) -> io::Error {
  io::Error::other(error.to_string())
}

#[cfg(feature = "graphics")]
struct RunningGraphics {
  display: crate::graphics::Graphics,
  child: std::process::Child,
  log: Vec<u8>,
  media: Option<crate::media::MediaProxy>,
  requests: Option<crate::requests::Broker>,
  _runtime: tempfile::TempDir,
}

#[cfg(feature = "graphics")]
impl RunningGraphics {
  fn start(
    approval: &Approval,
    viewport: Viewport,
    channel: &Channel,
    context: &UiContext,
  ) -> io::Result<Self> {
    use std::{
      fs::{File, OpenOptions},
      os::{
        fd::AsRawFd,
        unix::fs::{OpenOptionsExt, PermissionsExt},
      },
    };
    approval
      .store
      .with_authority(&approval.id, approval.epoch, &approval.unit, |record| {
        let managed_runtime = std::env::var_os("RUNTIME_DIRECTORY")
          .ok_or_else(|| invalid("controller runtime directory is unavailable"))?;
        crate::revision::require_private_directory(Path::new(&managed_runtime))?;
        let runtime = tempfile::Builder::new()
          .prefix("omarchy-display-")
          .permissions(std::fs::Permissions::from_mode(0o700))
          .tempdir_in(managed_runtime)?;
        let path = runtime.path().join("wayland");
        let context_path = runtime.path().join("context");
        std::fs::create_dir(&context_path)?;
        let mut context = context.clone();
        record.grants.settings.filter(&mut context.settings);
        context.publish(&context_path)?;
        let context_directory = File::open(context_path)?;
        let mut display =
          crate::graphics::Graphics::new(&path, viewport).map_err(graphics_error)?;
        let bootstrap = File::open(std::env::current_exe()?)?;
        let bundle = File::open(approval.store.revisions().join(&record.revision))?;
        let socket = OpenOptions::new()
          .read(true)
          .custom_flags(libc::O_PATH | libc::O_NOFOLLOW)
          .open(path)?;
        let manifest =
          crate::grants::Manifest::read(&approval.store.revisions().join(&record.revision))?;
        let (args, worker_runtime) = if let Some(entry) = &manifest.sandbox.entry_point {
          (
            vec![std::ffi::OsString::from("--worker"), entry.into()],
            None,
          )
        } else {
          let path = std::env::current_exe()?
            .parent()
            .ok_or_else(|| invalid("controller has no installation directory"))?
            .join("plugin-runtime");
          let directory = File::open(&path).map_err(|error| {
            std::io::Error::new(
              error.kind(),
              format!("shared worker runtime {}: {error}", path.display()),
            )
          })?;
          (
            vec![std::ffi::OsString::from("--omarchy-worker")],
            Some(directory),
          )
        };
        let media = record
          .grants
          .media
          .as_ref()
          .map(|name| {
            let address = std::env::var("DBUS_SESSION_BUS_ADDRESS")
              .map_err(|_| invalid("session bus address unavailable for media grant"))?;
            crate::media::MediaProxy::start(&address, runtime.path(), name, Limits::default())
          })
          .transpose()?;
        // Resolve staged assets before creating the broker that consumes them.
        let plugin_path = if record.grants.exec.is_empty() {
          None
        } else {
          Some(
            crate::worker::stage_plugin_assets(
              &approval.store.revisions().join(&record.revision),
              &record.revision,
              runtime.path(),
            )?
            .into_os_string()
            .into_string()
            .map_err(|_| invalid("staged plugin path is not UTF-8"))?,
          )
        };
        // Admitted plugin-path directories: assets when exec is granted, data
        // when storage is granted. Each drives both the sandbox environment
        // variable and the exec broker's `$...` token resolution.
        let mut paths = Vec::new();
        if let Some(path) = &plugin_path {
          paths.push(crate::exec_policy::PluginDir::assets(path.clone()));
        }
        if record.grants.storage {
          let data_path = crate::worker::storage_directory_path(&approval.id)?
            .into_os_string()
            .into_string()
            .map_err(|_| invalid("staged data path is not UTF-8"))?;
          paths.push(crate::exec_policy::PluginDir::data(data_path));
        }
        let requests = if worker_runtime.is_some()
          || record.grants.notifications
          || !record.grants.http.is_empty()
          || !record.grants.exec.is_empty()
          || record.grants.settings.can_write()
          || record.grants.open_urls
        {
          Some(crate::requests::Broker::start(
            runtime.path(),
            &std::env::current_exe()?,
            paths.clone(),
          )?)
        } else {
          None
        };
        // Runtime-readable view of the *admitted* grants, authored here from the
        // controller's own record (never plugin metadata) so a plugin can adapt
        // to declined optional access. It is mounted read-only into the sandbox.
        let grants_json_path = runtime.path().join("grants.json");
        std::fs::write(&grants_json_path, serde_json::to_vec(&record.grants)?)?;
        let grants_json = File::open(&grants_json_path)?;
        let storage = record
          .grants
          .storage
          .then(|| crate::worker::storage_directory(&approval.id))
          .transpose()?;
        let child = crate::worker::spawn(
          &bootstrap,
          &bundle,
          &socket,
          &args.iter().map(|arg| arg.as_os_str()).collect::<Vec<_>>(),
          Limits::default(),
          &record.grants,
          crate::worker::Resources {
            render_node: Some(display.render_node()),
            media: media.as_ref(),
            requests: requests.as_ref(),
            runtime: worker_runtime.as_ref(),
            context: worker_runtime.as_ref().map(|_| &context_directory),
            grants_json: Some(&grants_json),
            storage: storage.as_ref(),
            paths,
          },
        )?;
        let fd = child
          .stderr
          .as_ref()
          .ok_or_else(|| invalid("worker log pipe missing"))?
          .as_raw_fd();
        if unsafe { libc::fcntl(fd, libc::F_SETFL, libc::O_NONBLOCK) } < 0 {
          return Err(io::Error::last_os_error());
        }
        if context.panel.as_ref().is_some_and(|panel| panel.open) {
          display.activate();
        }
        display.describe(channel).map_err(graphics_error)?;
        Ok(Self {
          display,
          child,
          log: Vec::new(),
          media,
          requests,
          _runtime: runtime,
        })
      })
  }
  fn dispatch(&mut self, channel: &Channel, time: u32, approval: &Approval) -> io::Result<()> {
    use std::io::Read;
    if let Some(media) = &mut self.media {
      media.check()?;
    }
    if let Some(requests) = &mut self.requests {
      requests.dispatch(approval)?;
      if let Some(state) = requests.panel_state.take() {
        state.send(channel)?;
      }
    }
    let mut bytes = [0u8; 4096];
    if let Some(log) = &mut self.child.stderr {
      match log.read(&mut bytes) {
        Ok(count) if count > 0 => {
          self.log.clear();
          self.log.extend_from_slice(&bytes[..count]);
        }
        Ok(_) => (),
        Err(error) if error.kind() == io::ErrorKind::WouldBlock => (),
        Err(error) => return Err(error),
      }
    }
    if let Some(status) = self.child.try_wait()? {
      let log: String = String::from_utf8_lossy(&self.log)
        .chars()
        .filter(|ch| !ch.is_control() || *ch == '\n')
        .collect();
      return Err(io::Error::other(format!(
        "Quickshell worker exited ({status}): {log}"
      )));
    }
    self.display.dispatch().map_err(graphics_error)?;
    self.display.render(channel, time).map_err(graphics_error)
  }
}

#[cfg(feature = "graphics")]
impl Drop for RunningGraphics {
  fn drop(&mut self) {
    let _ = self.child.kill();
    let _ = self.child.try_wait();
  }
}

fn invalid(message: &str) -> io::Error {
  io::Error::new(io::ErrorKind::InvalidData, message)
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn panel_state_is_one_exact_descriptor_free_record() {
    let (sender, receiver) = Channel::pair().unwrap();
    for serial in [0, 1, u32::MAX] {
      for open in [false, true] {
        let state = Control::PanelState { serial, open };
        state.send(&sender).unwrap();
        let packet = receiver.receive().unwrap();
        assert_eq!(packet.bytes.len(), 16);
        assert!(packet.fds.is_empty());
        let mut invalid = packet.bytes.clone();
        assert_eq!(Control::decode(packet).unwrap(), state);
        invalid[12] = 2;
        assert!(
          Control::decode(Packet {
            bytes: invalid,
            fds: vec![]
          })
          .is_err()
        );
      }
    }
  }

  #[test]
  fn context_uses_exact_record_and_one_immutable_descriptor() {
    let (sender, receiver) = Channel::pair().unwrap();
    let context = Control::Context(UiContext::default());
    context.send(&sender).unwrap();
    let packet = receiver.receive().unwrap();
    assert_eq!(packet.bytes.len(), 16);
    assert_eq!(packet.fds.len(), 1);
    let bytes = packet.bytes.clone();
    assert_eq!(Control::decode(packet).unwrap(), context);
    for length in 0..16 {
      assert!(
        Control::decode(Packet {
          bytes: bytes[..length].to_vec(),
          fds: vec![UiContext::default().seal().unwrap().into()],
        })
        .is_err()
      );
    }
    for count in [0, 2] {
      assert!(
        Control::decode(Packet {
          bytes: bytes.clone(),
          fds: (0..count)
            .map(|_| UiContext::default().seal().unwrap().into())
            .collect(),
        })
        .is_err()
      );
    }
    for offset in 4..16 {
      let mut invalid = bytes.clone();
      invalid[offset] = 255;
      assert!(
        Control::decode(Packet {
          bytes: invalid,
          fds: vec![UiContext::default().seal().unwrap().into()],
        })
        .is_err()
      );
    }
  }

  #[test]
  fn scroll_records_are_bounded_and_exact() {
    let (sender, receiver) = Channel::pair().unwrap();
    for scroll in [
      Scroll {
        source: 0,
        x: 4095,
        y: 0,
        horizontal: -4096,
        vertical: 60,
      },
      Scroll {
        source: 1,
        x: 0,
        y: 4095,
        horizontal: 7,
        vertical: -9,
      },
      Scroll {
        source: 2,
        x: 0,
        y: 0,
        horizontal: 0,
        vertical: 0,
      },
    ] {
      let command = Control::Scroll(scroll);
      command.send(&sender).unwrap();
      let packet = receiver.receive().unwrap();
      assert_eq!(packet.bytes.len(), 28);
      let valid = packet.bytes.clone();
      assert_eq!(Control::decode(packet).unwrap(), command);
      for length in 0..28 {
        assert!(
          Control::decode(Packet {
            bytes: valid[..length].to_vec(),
            fds: vec![]
          })
          .is_err()
        );
      }
      for (offset, value) in [
        (4, 7i32),
        (8, 3),
        (12, -1),
        (16, 4096),
        (20, i32::MIN),
        (24, 4097),
      ] {
        let mut bytes = valid.clone();
        bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        assert!(Control::decode(Packet { bytes, fds: vec![] }).is_err());
      }
      assert!(
        Control::decode(Packet {
          bytes: valid,
          fds: vec![std::fs::File::open("/dev/null").unwrap().into()]
        })
        .is_err()
      );
    }
    for scroll in [
      Scroll {
        source: 0,
        x: 0,
        y: 0,
        horizontal: 0,
        vertical: 0,
      },
      Scroll {
        source: 2,
        x: 0,
        y: 0,
        horizontal: 1,
        vertical: 0,
      },
    ] {
      assert!(Control::Scroll(scroll).send(&sender).is_err());
    }
  }
  #[test]
  fn control_codec_rejects_unknown_fields_and_unexpected_descriptors() {
    let packet = |bytes| Packet {
      bytes,
      fds: Vec::new(),
    };
    for length in 0..32 {
      if length != 16 {
        assert!(Control::decode(packet(vec![0; length])).is_err());
      }
    }
    let mut valid = b"OPH\x01\x02\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00".to_vec();
    assert_eq!(
      Control::decode(packet(valid.clone())).unwrap(),
      Control::Ping(1)
    );
    for offset in 0..8 {
      let mut bad = valid.clone();
      bad[offset] = 255;
      assert!(Control::decode(packet(bad)).is_err());
    }
    valid[8] = 0;
    assert!(Control::decode(packet(valid)).is_err());
    let hello = b"OPH\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00".to_vec();
    assert!(
      Control::decode(Packet {
        bytes: hello,
        fds: vec![std::fs::File::open("/dev/null").unwrap().into()]
      })
      .is_err()
    );
  }
}
