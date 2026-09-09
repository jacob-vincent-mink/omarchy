//! Detached, read-only UI state. Identity and authority come from the admitted
//! session, never from this payload. Large snapshots use one sealed descriptor
//! so the control socket retains its small, fixed record/queue budget.
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, fs::File, io, os::fd::OwnedFd};

const MAX_BYTES: usize = 65536;

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct UiContext {
  pub settings: serde_json::Map<String, serde_json::Value>,
  pub theme: Option<Theme>,
  /// Desired own-panel state. Repeated context updates do not replay an action.
  pub panel: Option<PanelCommand>,
  pub geometry: Option<crate::geometry::Snapshot>,
  /// Host allocation inside the bar on this worker's output. Not desktop observation.
  pub bar: Option<BarPlacement>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BarPlacement {
  pub x: f64,
  pub y: f64,
  pub width: f64,
  pub height: f64,
  pub size: u32,
  pub position: BarPosition,
  pub visible: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum BarPosition {
  Top,
  Bottom,
  Left,
  Right,
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
    if let Some(geometry) = &context.geometry {
      geometry.validate()?;
    }
    if context.bar.as_ref().is_some_and(|bar| {
      ![bar.x, bar.y]
        .into_iter()
        .all(|v| v.is_finite() && v.abs() <= 4096.)
        || ![bar.width, bar.height]
          .into_iter()
          .all(|v| v.is_finite() && (0.0..=1024.).contains(&v))
        || !(1..=1024).contains(&bar.size)
    }) {
      return Err(io::Error::other("invalid bar allocation"));
    }
    if context.panel.as_ref().is_some_and(|panel| {
      panel.serial == 0 || panel.payload.len() > 4096 || (!panel.open && !panel.payload.is_empty())
    }) {
      return Err(io::Error::other("invalid own-panel command"));
    }
    Ok(context)
  }

  #[cfg(any(feature = "graphics", test))]
  pub(crate) fn filter(&mut self, grants: &crate::grants::Grants) {
    grants.settings.filter(&mut self.settings);
    if !grants.desktop_geometry {
      self.geometry = None;
    }
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
  fn bar_allocation_is_host_owned_bounded_and_independent_of_grants() {
    let json = br#"{"settings":{},"bar":{"x":123.5,"y":2,"width":60,"height":32,"size":40,"position":"bottom","visible":true}}"#;
    let mut context = UiContext::parse(json).unwrap();
    context.filter(&crate::grants::Grants::default());
    assert!(context.bar.is_some());
    assert_eq!(
      UiContext::unseal(context.seal().unwrap().into()).unwrap(),
      context
    );
    for (field, value) in [
      ("x", "4097"),
      ("y", "-4097"),
      ("width", "1025"),
      ("height", "-1"),
      ("size", "0"),
      ("position", "\"elsewhere\""),
      ("visible", "1"),
    ] {
      let mut value_json: serde_json::Value = serde_json::from_slice(json).unwrap();
      value_json["bar"][field] = serde_json::from_str(value).unwrap();
      assert!(
        UiContext::parse(&serde_json::to_vec(&value_json).unwrap()).is_err(),
        "{field}"
      );
    }
  }

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
