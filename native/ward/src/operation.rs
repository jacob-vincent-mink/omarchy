//! Shared operation replies. Policy decisions remain distinct from execution
//! failures; a completed external command may itself have a nonzero exit code.
use crate::{
  channel::{Channel, Packet},
  grants::Grants,
  requests::Kind,
};
use serde::{Deserialize, Serialize};
use std::{
  fmt,
  fs::File,
  io::{self, Read},
  path::Path,
  time::{Duration, Instant},
};

#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Status {
  Completed,
  Invalid,
  Busy,
  RateLimited,
  Failed,
  Denied,
  TimedOut,
  Unavailable,
}

impl fmt::Display for Status {
  fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
    write!(f, "Ward request: {self:?}")
  }
}
impl std::error::Error for Status {}

impl Status {
  pub fn error(self) -> io::Error {
    io::Error::new(
      match self {
        Self::Denied => io::ErrorKind::PermissionDenied,
        Self::Busy | Self::RateLimited => io::ErrorKind::WouldBlock,
        Self::TimedOut => io::ErrorKind::TimedOut,
        _ => io::ErrorKind::Other,
      },
      self,
    )
  }

  pub fn from_error(error: &io::Error) -> Self {
    error
      .get_ref()
      .and_then(|error| error.downcast_ref::<Self>())
      .copied()
      .unwrap_or(if error.kind() == io::ErrorKind::TimedOut {
        Self::TimedOut
      } else {
        Self::Failed
      })
  }
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

pub(crate) fn decode(packet: Packet) -> io::Result<Status> {
  if !packet.fds.is_empty() {
    return Err(Status::Unavailable.error());
  }
  let reply: Reply =
    serde_json::from_slice(&packet.bytes).map_err(|_| Status::Unavailable.error())?;
  if reply.version != 1 {
    return Err(Status::Unavailable.error());
  }
  Ok(reply.status)
}

pub(crate) fn await_reply(channel: &Channel) -> io::Result<()> {
  let deadline = Instant::now() + Duration::from_secs(3);
  loop {
    match channel.receive() {
      Ok(packet) => {
        return match decode(packet)? {
          Status::Completed => Ok(()),
          status => Err(status.error()),
        };
      }
      Err(error) if error.kind() == io::ErrorKind::WouldBlock && Instant::now() < deadline => {
        std::thread::sleep(Duration::from_millis(5))
      }
      Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
        return Err(Status::TimedOut.error());
      }
      Err(_) => return Err(Status::Unavailable.error()),
    }
  }
}

/// The read-only admission snapshot explains intentionally absent sockets.
/// This is caller feedback, not authority: the broker rechecks every request.
pub(crate) fn grants() -> io::Result<Grants> {
  let mut bytes = Vec::new();
  File::open("/run/plugin/grants.json")
    .and_then(|file| {
      file
        .take((crate::grants::MAX_PERSISTED_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
    })
    .map_err(|_| Status::Unavailable.error())?;
  if bytes.len() > crate::grants::MAX_PERSISTED_BYTES {
    return Err(Status::Unavailable.error());
  }
  serde_json::from_slice(&bytes).map_err(|_| Status::Unavailable.error())
}

pub(crate) fn connect(kind: Kind) -> io::Result<Channel> {
  let grants = grants()?;
  let (allowed, path) = match kind {
    Kind::Notification => (grants.notifications, "/run/plugin/notify"),
    Kind::Settings => (grants.settings.can_write(), "/run/plugin/settings"),
    Kind::OpenUrl => (grants.open_urls, "/run/plugin/open-url"),
    Kind::Http => (!grants.http.is_empty(), "/run/plugin/http"),
    Kind::Exec => (!grants.exec.is_empty(), "/run/plugin/exec"),
  };
  if !allowed {
    return Err(Status::Denied.error());
  }
  Channel::connect(Path::new(path)).map_err(|error| {
    if error.kind() == io::ErrorKind::WouldBlock {
      Status::Busy.error()
    } else {
      Status::Unavailable.error()
    }
  })
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn policy_and_operation_errors_keep_distinct_machine_statuses() {
    for status in [
      Status::Denied,
      Status::Failed,
      Status::Invalid,
      Status::Busy,
      Status::RateLimited,
      Status::TimedOut,
      Status::Unavailable,
    ] {
      let (sender, receiver) = Channel::pair().unwrap();
      reply(&sender, status).unwrap();
      let error = await_reply(&receiver).unwrap_err();
      assert_eq!(Status::from_error(&error), status);
    }
    // A host OS permission failure is operational, not Ward refusing a grant.
    assert_eq!(
      Status::from_error(&io::Error::from(io::ErrorKind::PermissionDenied)),
      Status::Failed
    );
    let (sender, receiver) = Channel::pair().unwrap();
    reply(&sender, Status::Completed).unwrap();
    await_reply(&receiver).unwrap();
    for bytes in [
      br#"{"version":2,"status":"denied"}"#.as_slice(),
      br#"{"version":1,"status":"unknown"}"#,
      b"not JSON",
    ] {
      assert_eq!(
        Status::from_error(
          &decode(Packet {
            bytes: bytes.into(),
            fds: vec![]
          })
          .unwrap_err()
        ),
        Status::Unavailable
      );
    }
  }
}
