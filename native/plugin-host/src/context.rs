//! Detached, read-only UI state. Identity and authority come from the admitted
//! session, never from this payload. Large snapshots use one sealed descriptor
//! so the control socket retains its small, fixed record/queue budget.
use serde::{Deserialize, Serialize};
use std::{
  collections::BTreeMap,
  fs::File,
  io::{self, Write},
  os::{fd::OwnedFd, unix::fs::FileExt},
};

const MAX_BYTES: usize = 65536;
const SEALS: rustix::fs::SealFlags = rustix::fs::SealFlags::SEAL
  .union(rustix::fs::SealFlags::SHRINK)
  .union(rustix::fs::SealFlags::GROW)
  .union(rustix::fs::SealFlags::WRITE);

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct UiContext {
  pub settings: serde_json::Map<String, serde_json::Value>,
  pub theme: Option<Theme>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Theme {
  pub foreground: String,
  pub background: String,
  pub accent: String,
  pub urgent: String,
  pub muted: String,
  pub shell_values: BTreeMap<String, String>,
  pub corner_radius: u32,
  pub gaps_out: u32,
  pub font_family: String,
}

impl UiContext {
  pub fn parse(bytes: &[u8]) -> io::Result<Self> {
    if bytes.len() > MAX_BYTES {
      return Err(io::Error::other("UI context exceeds 64 KiB"));
    }
    Ok(serde_json::from_slice(bytes)?)
  }

  fn bytes(&self) -> io::Result<Vec<u8>> {
    let bytes = serde_json::to_vec(self)?;
    Self::parse(&bytes)?;
    Ok(bytes)
  }

  pub(crate) fn seal(&self) -> io::Result<File> {
    let mut file = File::from(rustix::fs::memfd_create(
      "plugin-ui",
      rustix::fs::MemfdFlags::CLOEXEC | rustix::fs::MemfdFlags::ALLOW_SEALING,
    )?);
    file.write_all(&self.bytes()?)?;
    rustix::fs::fcntl_add_seals(&file, SEALS)?;
    Ok(file)
  }

  pub(crate) fn unseal(fd: OwnedFd) -> io::Result<Self> {
    let file = File::from(fd);
    let metadata = file.metadata()?;
    if !metadata.is_file()
      || metadata.len() > MAX_BYTES as u64
      || !rustix::fs::fcntl_get_seals(&file)?.contains(SEALS)
    {
      return Err(io::Error::other(
        "UI context requires a bounded sealed file",
      ));
    }
    let mut bytes = vec![0; metadata.len() as usize];
    file.read_exact_at(&mut bytes, 0)?;
    Self::parse(&bytes)
  }

  /// Atomic replacement keeps FileView watchers live; only this private
  /// directory is mounted read-only, never the shell's configuration tree.
  #[cfg(any(feature = "graphics", test))]
  pub(crate) fn publish(&self, directory: &std::path::Path) -> io::Result<()> {
    let mut file = tempfile::NamedTempFile::new_in(directory)?;
    file.write_all(&self.bytes()?)?;
    file.persist(directory.join("state.json"))?;
    Ok(())
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn snapshots_are_bounded_sealed_typed_and_atomically_replaced() {
    let mut context = UiContext::default();
    context.settings.insert(
      "nested".into(),
      serde_json::json!({"text": "x".repeat(8192)}),
    );
    assert_eq!(
      UiContext::unseal(context.seal().unwrap().into()).unwrap(),
      context
    );
    assert!(UiContext::unseal(File::open("/dev/null").unwrap().into()).is_err());
    let file = tempfile::tempfile().unwrap();
    assert!(UiContext::unseal(file.into()).is_err());
    for invalid in [
      br#"{"settings":[],"theme":null}"#.as_slice(),
      br#"{"settings":{},"theme":null,"id":"other"}"#,
      br#"{"settings":{},"theme":{}}"#,
    ] {
      assert!(UiContext::parse(invalid).is_err());
    }
    let directory = tempfile::tempdir().unwrap();
    context.publish(directory.path()).unwrap();
    let previous = File::open(directory.path().join("state.json")).unwrap();
    UiContext::default().publish(directory.path()).unwrap();
    assert!(previous.metadata().unwrap().len() > 8192);
    assert_eq!(
      UiContext::parse(&std::fs::read(directory.path().join("state.json")).unwrap()).unwrap(),
      UiContext::default()
    );
    context
      .settings
      .insert("large".into(), "x".repeat(MAX_BYTES).into());
    assert!(context.seal().is_err());
  }
}
