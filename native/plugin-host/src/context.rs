//! Detached, read-only UI state. Identity and authority come from the admitted
//! session, never from this payload. Large snapshots use one sealed descriptor
//! so the control socket retains its small, fixed record/queue budget.
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, fs::File, io, os::fd::OwnedFd};

const MAX_BYTES: usize = 65536;

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct UiContext {
  pub settings: serde_json::Map<String, serde_json::Value>,
  pub theme: Option<Theme>,
  /// Desired own-panel state. Repeated context updates do not replay an action.
  pub panel: Option<PanelCommand>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PanelCommand {
  pub serial: u32,
  pub open: bool,
  pub payload: String,
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
    let context: Self = serde_json::from_slice(bytes)?;
    if context.panel.as_ref().is_some_and(|panel| {
      panel.serial == 0 || panel.payload.len() > 4096 || (!panel.open && !panel.payload.is_empty())
    }) {
      return Err(io::Error::other("invalid own-panel command"));
    }
    Ok(context)
  }

  fn bytes(&self) -> io::Result<Vec<u8>> {
    let bytes = serde_json::to_vec(self)?;
    Self::parse(&bytes)?;
    Ok(bytes)
  }

  pub(crate) fn seal(&self) -> io::Result<File> {
    crate::payload::seal(&self.bytes()?, MAX_BYTES)
  }

  pub(crate) fn unseal(fd: OwnedFd) -> io::Result<Self> {
    Self::parse(&crate::payload::read(fd, MAX_BYTES)?)
  }

  /// Atomic replacement keeps FileView watchers live; only this private
  /// directory is mounted read-only, never the shell's configuration tree.
  #[cfg(any(feature = "graphics", test))]
  pub(crate) fn publish(&self, directory: &std::path::Path) -> io::Result<()> {
    use std::io::Write;
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
  fn panel_commands_are_optional_bounded_and_survive_transport() {
    let mut context = UiContext::parse(br#"{"settings":{},"theme":null}"#).unwrap();
    assert_eq!(context.panel, None);
    context.panel = Some(PanelCommand {
      serial: u32::MAX,
      open: true,
      payload: "value".into(),
    });
    assert_eq!(
      UiContext::unseal(context.seal().unwrap().into()).unwrap(),
      context
    );
    for value in ["0", "-1", "1.5", "4294967296", "null", "true"] {
      assert!(
        UiContext::parse(
          format!(r#"{{"settings":{{}},"theme":null,"panel":{{"serial":{value},"open":false,"payload":""}}}}"#).as_bytes()
        )
        .is_err()
      );
    }
    context.panel.as_mut().unwrap().payload = "x".repeat(4097);
    assert!(context.seal().is_err());
    context.panel.as_mut().unwrap().payload = "unexpected".into();
    context.panel.as_mut().unwrap().open = false;
    assert!(context.seal().is_err());
  }

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
