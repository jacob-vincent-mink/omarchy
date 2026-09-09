//! The worker may request text, never a desktop action, image, application
//! identity, executable, or notification replacement identifier.
use crate::channel::{Channel, Listener, Packet};
use serde::{Deserialize, Serialize};
use std::{
  fs::{File, OpenOptions},
  io,
  os::unix::fs::OpenOptionsExt,
  path::{Path, PathBuf},
  process::{Child, Command, Stdio},
  time::{Duration, Instant},
};

/// One explicit operation, with no worker-selected method, executable or identity.
/// The socket is mounted only for an admitted notification grant. All connected
/// peers must also belong to this controller's kernel-owned unit.
pub struct Broker {
  listener: Listener,
  socket: File,
  program: PathBuf,
  clients: Vec<(Channel, Instant)>,
  delivery: Option<Delivery>,
  budget: Budget,
  admission_window: Instant,
  admissions: u32,
}

struct Delivery {
  child: Child,
  channel: Channel,
  started: Instant,
}

impl Drop for Delivery {
  fn drop(&mut self) {
    let _ = self.child.kill();
    let _ = self.child.try_wait();
  }
}

impl Broker {
  pub fn start(root: &Path) -> io::Result<Self> {
    let program = PathBuf::from(
      std::env::var_os("OMARCHY_PATH")
        .ok_or_else(|| invalid("OMARCHY_PATH is required for notification delivery"))?,
    )
    .join("bin/omarchy-notification-send");
    if !program.is_absolute() || !program.is_file() {
      return Err(invalid("notification helper unavailable"));
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
      program,
      clients: Vec::new(),
      delivery: None,
      budget: Budget::new(now),
      admission_window: now,
      admissions: 0,
    })
  }

  pub(crate) fn socket(&self) -> &File {
    &self.socket
  }

  pub fn dispatch(&mut self, approval: &crate::controller::Approval) -> io::Result<()> {
    let now = Instant::now();
    if let Some(delivery) = &mut self.delivery {
      let status = delivery.child.try_wait()?;
      if status.is_some() || now.duration_since(delivery.started) >= Duration::from_secs(2) {
        let delivery = self.delivery.take().unwrap();
        approval.with_notifications(|_| {
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
      if self.delivery.is_some() {
        let _ = reply(&client, Status::Busy);
        continue;
      }
      if !self.budget.take(now) {
        let _ = reply(&client, Status::RateLimited);
        continue;
      }
      self.delivery = approval.with_notifications(|id| {
        let result = Command::new("/usr/bin/timeout")
          .args(["--signal=KILL", "1s"])
          .arg(&self.program)
          .args(request.arguments(id)?)
          .stdin(Stdio::null())
          .stdout(Stdio::null())
          .stderr(Stdio::null())
          .spawn();
        match result {
          Ok(child) => Ok(Some(Delivery {
            child,
            channel: client,
            started: now,
          })),
          Err(_) => {
            let _ = reply(&client, Status::Failed);
            Ok(None)
          }
        }
      })?;
    }
    Ok(())
  }
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Request {
  version: u32,
  title: String,
  body: String,
}

impl Request {
  pub fn new(title: String, body: String) -> io::Result<Self> {
    let request = Self {
      version: 1,
      title,
      body,
    };
    request.validate()?;
    Ok(request)
  }

  pub fn decode(packet: Packet) -> io::Result<Self> {
    if !packet.fds.is_empty() || packet.bytes.len() > crate::channel::MAX_BYTES {
      return Err(invalid(
        "notification packet exceeds limits or carries descriptors",
      ));
    }
    let request: Self =
      serde_json::from_slice(&packet.bytes).map_err(|_| invalid("invalid notification request"))?;
    request.validate()?;
    Ok(request)
  }

  fn validate(&self) -> io::Result<()> {
    if self.version != 1
      || self.title.is_empty()
      || self.title.len() > 160
      || self.body.len() > 2048
      || self.title.chars().any(|ch| forbidden(ch, false))
      || self.body.chars().any(|ch| forbidden(ch, true))
    {
      return Err(invalid("notification text exceeds its allowed format"));
    }
    Ok(())
  }

  /// Arguments for the existing helper; no shell re-parsing. Both positionals
  /// have trusted prefixes because that helper recognizes options in either slot.
  pub(crate) fn arguments(&self, id: &str) -> io::Result<Vec<String>> {
    crate::grants::validate_id(id)?;
    self.validate()?;
    Ok(vec![
      "--app-name".into(),
      format!("omarchy-plugin-{id}"),
      "--urgency".into(),
      "low".into(),
      "--expire-time".into(),
      "5000".into(),
      format!("Plugin {id}: {}", self.title),
      format!("Message: {}", escape_body(&self.body)),
    ])
  }
}

#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Status {
  Delivered,
  Invalid,
  Busy,
  RateLimited,
  Failed,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Reply {
  version: u32,
  status: Status,
}

pub(crate) fn reply(channel: &Channel, status: Status) -> io::Result<()> {
  channel.send(&serde_json::to_vec(&Reply { version: 1, status })?, &[])
}

/// Worker-side helper. Its own wait is bounded; the controller and GUI never
/// wait synchronously for the requester or notification delivery.
pub fn request(title: String, body: String) -> io::Result<()> {
  request_at(Path::new("/run/plugin/notify"), Request::new(title, body)?)
}

fn request_at(path: &Path, request: Request) -> io::Result<()> {
  let channel = Channel::connect(path)?;
  channel.send(&serde_json::to_vec(&request)?, &[])?;
  let deadline = Instant::now() + Duration::from_secs(3);
  loop {
    match channel.receive() {
      Ok(packet) => {
        if !packet.fds.is_empty() {
          return Err(invalid("notification reply carried descriptors"));
        }
        let reply: Reply = serde_json::from_slice(&packet.bytes)
          .map_err(|_| invalid("invalid notification reply"))?;
        if reply.version != 1 {
          return Err(invalid("unknown notification reply version"));
        }
        return if reply.status == Status::Delivered {
          Ok(())
        } else {
          Err(io::Error::other(format!(
            "notification request rejected: {:?}",
            reply.status
          )))
        };
      }
      Err(error) if error.kind() == io::ErrorKind::WouldBlock && Instant::now() < deadline => {
        std::thread::sleep(Duration::from_millis(5))
      }
      Err(error) => return Err(error),
    }
  }
}

pub(crate) struct Budget {
  tokens: u8,
  refilled: Instant,
}
impl Budget {
  pub fn new(now: Instant) -> Self {
    Self {
      tokens: 2,
      refilled: now,
    }
  }
  pub fn take(&mut self, now: Instant) -> bool {
    let refill = now.saturating_duration_since(self.refilled).as_secs() / 30;
    if refill > 0 {
      self.tokens = (self.tokens + refill.min(2) as u8).min(2);
      self.refilled = now;
    }
    if self.tokens == 0 {
      false
    } else {
      self.tokens -= 1;
      true
    }
  }
}

fn forbidden(ch: char, body: bool) -> bool {
  (ch.is_control() && !(body && ch == '\n'))
    || matches!(ch, '\u{061c}' | '\u{200e}' | '\u{200f}' | '\u{202a}'..='\u{202e}' | '\u{2066}'..='\u{2069}')
}
fn escape_body(body: &str) -> String {
  body
    .replace('&', "&amp;")
    .replace('<', "&lt;")
    .replace('>', "&gt;")
}
fn invalid(message: &str) -> io::Error {
  io::Error::new(io::ErrorKind::InvalidData, message)
}

#[cfg(test)]
mod tests {
  use super::*;
  #[test]
  fn request_cannot_select_authority_or_smuggle_actions() {
    let packet = |bytes: &[u8]| Packet {
      bytes: bytes.to_vec(),
      fds: Vec::new(),
    };
    for bytes in [
      br#"{"version":1,"title":"ok","body":"","id":"other"}"#.as_slice(),
      br#"{"version":1,"title":"ok","body":"","exec":"sh"}"#,
      br#"{"version":2,"title":"ok","body":""}"#,
      br#"{"version":1,"title":"a","title":"b","body":""}"#,
    ] {
      assert!(Request::decode(packet(bytes)).is_err());
    }
    for text in ["\0", "\n", "\u{202e}", "\u{2066}"] {
      assert!(Request::new(text.into(), String::new()).is_err());
    }
    assert!(Request::new("x".repeat(161), String::new()).is_err());
    assert!(Request::new("x".into(), "x".repeat(2049)).is_err());
    let request = Request::new(
      "--image=/secret".into(),
      "--exec <img src='/secret'/> & body".into(),
    )
    .unwrap();
    let args = request.arguments("test.widget").unwrap();
    assert_eq!(args.len(), 8);
    assert_eq!(args[6], "Plugin test.widget: --image=/secret");
    assert_eq!(
      args[7],
      "Message: --exec &lt;img src='/secret'/&gt; &amp; body"
    );
    assert!(
      !args
        .iter()
        .any(|arg| arg == "--exec" || arg == "--image" || arg == "--replace-id")
    );
  }

  #[test]
  fn notification_budget_is_small_and_replenishes_without_sleeping() {
    let start = Instant::now();
    let mut budget = Budget::new(start);
    assert!(budget.take(start));
    assert!(budget.take(start));
    assert!(!budget.take(start));
    assert!(!budget.take(start + Duration::from_secs(29)));
    assert!(budget.take(start + Duration::from_secs(30)));
    assert!(!budget.take(start + Duration::from_secs(30)));
    let later = start + Duration::from_secs(300);
    assert!(budget.take(later));
    assert!(budget.take(later));
    assert!(!budget.take(later));
  }

  #[test]
  fn existing_helper_emits_only_fixed_notification_fields() {
    use std::{fs, os::unix::fs::PermissionsExt};
    let root = tempfile::tempdir().unwrap();
    // Exercise the real helper's option parser without connecting to any bus.
    let busctl = root.path().join("busctl");
    fs::write(
      &busctl,
      "#!/bin/bash\nprintf '%s\\0' \"$@\" > \"$OMARCHY_NOTIFICATION_CAPTURE\"\n",
    )
    .unwrap();
    fs::set_permissions(&busctl, fs::Permissions::from_mode(0o700)).unwrap();
    let capture = root.path().join("record");
    let request = Request::new(
      "--image=/secret".into(),
      "--exec <img src='/secret'/> & body".into(),
    )
    .unwrap();
    let status = Command::new(
      Path::new(env!("CARGO_MANIFEST_DIR")).join("../../bin/omarchy-notification-send"),
    )
    .args(request.arguments("test.widget").unwrap())
    .env_clear()
    .env("PATH", root.path())
    .env("OMARCHY_NOTIFICATION_CAPTURE", &capture)
    .status()
    .unwrap();
    assert!(status.success());
    let bytes = fs::read(capture).unwrap();
    let args = bytes
      .split(|byte| *byte == 0)
      .map(|arg| std::str::from_utf8(arg).unwrap())
      .collect::<Vec<_>>();
    assert_eq!(
      args,
      [
        "--user",
        "--",
        "call",
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify",
        "susssasa{sv}i",
        "omarchy-plugin-test.widget",
        "0",
        "",
        "Plugin test.widget: --image=/secret",
        "Message: --exec &lt;img src='/secret'/&gt; &amp; body",
        "0",
        "1",
        "urgency",
        "y",
        "0",
        "5000",
        "",
      ]
    );
  }
}
