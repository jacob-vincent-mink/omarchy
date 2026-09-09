//! Bounded worker requests share one admission, delivery and cleanup loop.
use crate::{
  channel::{Channel, Listener, Packet},
  context::UiContext,
  controller::Control,
  notification::{Budget, Request as Notification, Status, reply},
};
use std::{
  fs::{File, OpenOptions},
  io,
  os::unix::fs::OpenOptionsExt,
  path::{Path, PathBuf},
  process::{Child, Command, Stdio},
  time::{Duration, Instant},
};

#[derive(Clone, Copy)]
pub enum Kind {
  Notification,
  Settings,
}

enum Request {
  Notification(Notification),
  Settings(UiContext),
}

impl Request {
  fn decode(packet: Packet) -> io::Result<Self> {
    if packet.bytes.starts_with(b"OPH\x01") {
      if let Control::Context(context) = Control::decode(packet)?
        && context.theme.is_none()
      {
        return Ok(Self::Settings(context));
      }
      Err(invalid("invalid settings request"))
    } else {
      Notification::decode(packet).map(Self::Notification)
    }
  }

  fn kind(&self) -> Kind {
    match self {
      Self::Notification(_) => Kind::Notification,
      Self::Settings(_) => Kind::Settings,
    }
  }

  fn command(&self, directory: &Path, id: &str) -> io::Result<Command> {
    let (helper, args) = match self {
      Self::Notification(request) => ("omarchy-notification-send", request.arguments(id)?),
      Self::Settings(context) => {
        let mut settings = context.settings.clone();
        if settings
          .remove("id")
          .is_some_and(|value| value.as_str() != Some(id))
          || ["sandbox", "__proto__", "constructor", "prototype"]
            .iter()
            .any(|key| settings.contains_key(*key))
        {
          return Err(invalid(
            "settings cannot change plugin identity or host structure",
          ));
        }
        (
          "omarchy-plugin-settings-apply",
          vec![id.into(), serde_json::to_string(&settings)?],
        )
      }
    };
    let mut command = Command::new("/usr/bin/timeout");
    command
      .args(["--signal=KILL", "1s"])
      .arg(directory.join(helper))
      .args(args)
      .stdin(Stdio::null())
      .stdout(Stdio::null())
      .stderr(Stdio::null());
    Ok(command)
  }
}

/// Explicit operations, with no worker-selected method, executable or identity.
/// The socket is mounted only for admitted host-request grants. All connected
/// peers must also belong to this controller's kernel-owned unit.
pub struct Broker {
  listener: Listener,
  socket: File,
  directory: PathBuf,
  clients: Vec<(Channel, Instant)>,
  deliveries: [Option<Delivery>; 2],
  budgets: [Budget; 2],
  admission_window: Instant,
  admissions: u32,
}

struct Delivery {
  child: Child,
  channel: Channel,
  started: Instant,
  kind: Kind,
}

impl Drop for Delivery {
  fn drop(&mut self) {
    let _ = self.child.kill();
    let _ = self.child.try_wait();
  }
}

impl Broker {
  pub fn start(root: &Path) -> io::Result<Self> {
    let directory = PathBuf::from(
      std::env::var_os("OMARCHY_PATH")
        .ok_or_else(|| invalid("OMARCHY_PATH is required for host requests"))?,
    )
    .join("bin");
    if !directory.is_absolute() || !directory.is_dir() {
      return Err(invalid("host helpers unavailable"));
    }
    let path = root.join("notify");
    let listener = Listener::bind(&path)?;
    let socket = OpenOptions::new()
      .read(true)
      .custom_flags(libc::O_PATH | libc::O_NOFOLLOW)
      .open(path)?;
    let now = Instant::now();
    Ok(Self {
      listener,
      socket,
      directory,
      clients: Vec::new(),
      deliveries: [None, None],
      budgets: [Budget::new(now, 2, 30), Budget::new(now, 8, 1)],
      admission_window: now,
      admissions: 0,
    })
  }

  pub(crate) fn socket(&self) -> &File {
    &self.socket
  }

  pub fn dispatch(&mut self, approval: &crate::controller::Approval) -> io::Result<()> {
    let now = Instant::now();
    for slot in &mut self.deliveries {
      if let Some(delivery) = slot {
        let status = delivery.child.try_wait()?;
        if status.is_some() || now.duration_since(delivery.started) >= Duration::from_secs(2) {
          let delivery = slot.take().unwrap();
          approval.with_request(delivery.kind, |_| {
            let status = if status.is_some_and(|status| status.success()) {
              Status::Delivered
            } else {
              Status::Failed
            };
            let _ = reply(&delivery.channel, status);
            Ok(())
          })?;
        }
      }
    }
    if now.duration_since(self.admission_window) >= Duration::from_secs(1) {
      self.admission_window = now;
      self.admissions = 0;
    }
    for _ in 0..4 {
      // Stop accepting until the next window; even failed authentication is
      // charged, before doing procfs/pidfd work. The kernel backlog is bounded.
      if self.admissions >= 32 || self.clients.len() >= 4 {
        break;
      }
      let client = match self.listener.accept() {
        Ok(client) => client,
        Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
        Err(error) => return Err(error),
      };
      self.admissions += 1;
      if crate::supervisor::authenticate_member(&client).is_ok() {
        self.clients.push((client, now));
      }
    }
    let mut index = 0;
    while index < self.clients.len() {
      let packet = match self.clients[index].0.receive() {
        Ok(packet) => Some(packet),
        Err(error)
          if error.kind() == io::ErrorKind::WouldBlock
            && now.duration_since(self.clients[index].1) < Duration::from_secs(1) =>
        {
          index += 1;
          continue;
        }
        Err(_) => None,
      };
      let (client, _) = self.clients.swap_remove(index);
      let request = packet.and_then(|packet| Request::decode(packet).ok());
      let Some(request) = request else {
        let _ = reply(&client, Status::Invalid);
        continue;
      };
      let kind = request.kind();
      let slot = kind as usize;
      if self.deliveries[slot].is_some() {
        let _ = reply(&client, Status::Busy);
        continue;
      }
      if !self.budgets[slot].take(now) {
        let _ = reply(&client, Status::RateLimited);
        continue;
      }
      let result = approval.with_request(kind, |id| request.command(&self.directory, id)?.spawn());
      match result {
        Ok(child) => {
          self.deliveries[slot] = Some(Delivery {
            child,
            channel: client,
            started: now,
            kind,
          })
        }
        Err(_) => {
          let _ = reply(&client, Status::Denied);
        }
      }
    }
    Ok(())
  }
}

/// Worker helper: only data travels to the controller. A plugin id in the
/// original inline settings is checked against the admitted id, never selected.
pub fn save_settings(json: &str) -> io::Result<()> {
  if json.len() > 65_000 {
    return Err(invalid("settings exceed the UI context bound"));
  }
  let context = UiContext {
    settings: serde_json::from_str(json)?,
    theme: None,
  };
  let channel = Channel::connect(Path::new("/run/plugin/settings"))?;
  Control::Context(context).send(&channel)?;
  crate::notification::await_reply(&channel)
}

fn invalid(message: &str) -> io::Error {
  io::Error::new(io::ErrorKind::InvalidData, message)
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::os::fd::AsFd;

  #[test]
  fn settings_reuse_sealed_transport_without_selecting_host_authority() {
    let (sender, receiver) = Channel::pair().unwrap();
    let context = UiContext {
      settings: serde_json::from_value(serde_json::json!({
        "id": "test.widget", "text": "$(touch /secret)", "nested": {"value": "x".repeat(8192)}
      }))
      .unwrap(),
      theme: None,
    };
    Control::Context(context.clone()).send(&sender).unwrap();
    let request = Request::decode(receiver.receive().unwrap()).unwrap();
    let command = request
      .command(Path::new("/trusted/bin"), "test.widget")
      .unwrap();
    let args = command.get_args().collect::<Vec<_>>();
    assert_eq!(command.get_program(), "/usr/bin/timeout");
    assert_eq!(args.len(), 5);
    assert_eq!(args[2], "/trusted/bin/omarchy-plugin-settings-apply");
    assert_eq!(args[3], "test.widget");
    let settings: serde_json::Value = serde_json::from_str(args[4].to_str().unwrap()).unwrap();
    assert!(settings.get("id").is_none());
    assert_eq!(settings["text"], "$(touch /secret)");
    assert!(
      request
        .command(Path::new("/trusted/bin"), "other.widget")
        .is_err()
    );
    for key in ["sandbox", "__proto__", "constructor", "prototype"] {
      let mut forged = context.clone();
      forged.settings.insert(key.into(), true.into());
      assert!(
        Request::Settings(forged)
          .command(Path::new("/trusted/bin"), "test.widget")
          .is_err()
      );
    }
    Control::Ping(1).send(&sender).unwrap();
    assert!(Request::decode(receiver.receive().unwrap()).is_err());
    // Replacing the required sealed descriptor with an ordinary file fails.
    Control::Context(context).send(&sender).unwrap();
    let packet = receiver.receive().unwrap();
    sender
      .send(&packet.bytes, &[File::open("/dev/null").unwrap().as_fd()])
      .unwrap();
    assert!(Request::decode(receiver.receive().unwrap()).is_err());
  }
}
