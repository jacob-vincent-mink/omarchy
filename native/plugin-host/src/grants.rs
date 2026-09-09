use serde::{Deserialize, Serialize};
use std::{
  collections::{BTreeMap, BTreeSet},
  fs::{File, OpenOptions},
  io::{self, Read},
  os::{
    fd::AsRawFd,
    unix::fs::{MetadataExt, OpenOptionsExt},
  },
  path::{Component, Path, PathBuf},
};

#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq, Eq)]
#[serde(default, deny_unknown_fields)]
pub struct Requests {
  pub read: Vec<String>,
  pub network: bool,
  pub media: bool,
  pub notifications: bool,
  pub storage: bool,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq, Eq)]
#[serde(default, deny_unknown_fields)]
pub struct Grants {
  pub read: BTreeMap<String, ReadDirectory>,
  pub network: bool,
  pub media: Option<String>,
  pub notifications: bool,
  pub storage: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct ReadDirectory {
  pub path: PathBuf,
  pub device: u64,
  pub inode: u64,
}

impl ReadDirectory {
  /// This selection comes from the approving user, not the plugin manifest.
  pub fn select(path: &Path) -> io::Result<Self> {
    let path = path.canonicalize()?;
    let file = open_directory(&path)?;
    let metadata = file.metadata()?;
    Ok(Self {
      path,
      device: metadata.dev(),
      inode: metadata.ino(),
    })
  }

  pub fn open(&self) -> io::Result<File> {
    let file = open_directory(&self.path)?;
    let metadata = file.metadata()?;
    if metadata.dev() != self.device || metadata.ino() != self.inode {
      return Err(invalid("approved directory was replaced"));
    }
    Ok(file)
  }
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Manifest {
  pub schema_version: u32,
  pub id: String,
  pub name: String,
  pub version: String,
  pub kinds: Vec<String>,
  pub entry_points: BTreeMap<String, String>,
  pub sandbox: SandboxManifest,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct SandboxManifest {
  pub version: u32,
  pub entry_point: String,
  pub requests: Requests,
}

impl Manifest {
  pub fn read(revision: &Path) -> io::Result<Self> {
    let manifest: Self = read_json(&revision.join("manifest.json"))?;
    validate_id(&manifest.id)?;
    if manifest.schema_version != 1
      || manifest.sandbox.version != 1
      || manifest.name.is_empty()
      || manifest.name.len() > 128
      || manifest.name.chars().any(char::is_control)
      || manifest.version.is_empty()
      || manifest.version.len() > 64
      || manifest.version.chars().any(char::is_control)
      || manifest.kinds.is_empty()
      || manifest.kinds.len() > 6
      || manifest.entry_points.len() > 6
    {
      return Err(invalid("invalid sandbox plugin manifest"));
    }
    for kind in &manifest.kinds {
      validate_id(kind)?;
    }
    for entry in manifest
      .entry_points
      .values()
      .chain([&manifest.sandbox.entry_point])
    {
      if !safe_relative(entry) || !revision.join(entry).is_file() {
        return Err(invalid(
          "entry point is not a file in the approved revision",
        ));
      }
    }
    let requests = &manifest.sandbox.requests;
    if requests.read.len() > 8
      || requests.read.iter().collect::<BTreeSet<_>>().len() != requests.read.len()
    {
      return Err(invalid("invalid requested directory slots"));
    }
    for name in &requests.read {
      validate_id(name)?;
    }
    Ok(manifest)
  }
}

impl Grants {
  pub fn validate(&self, requests: &Requests) -> io::Result<()> {
    if self.read.len() > 8
      || self.network && !requests.network
      || self.notifications && !requests.notifications
      || self.storage && !requests.storage
      || self.media.is_some() && !requests.media
    {
      return Err(invalid("grants exceed the reviewed request"));
    }
    for (name, directory) in &self.read {
      validate_id(name)?;
      if !requests.read.contains(name) || !safe_absolute(&directory.path) {
        return Err(invalid("invalid read-only directory grant"));
      }
    }
    if let Some(name) = &self.media {
      validate_media(name)?;
    }
    Ok(())
  }
}

pub(crate) fn validate_media(name: &str) -> io::Result<()> {
  let Some(player) = name.strip_prefix("org.mpris.MediaPlayer2.") else {
    return Err(invalid("invalid media service scope"));
  };
  if player.is_empty()
    || name.len() > 255
    || !player.split('.').all(|part| {
      !part.is_empty()
        && part
          .bytes()
          .next()
          .is_some_and(|byte| byte.is_ascii_alphabetic() || byte == b'_')
        && part
          .bytes()
          .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
    })
  {
    return Err(invalid("media grants require one exact service name"));
  }
  Ok(())
}

pub(crate) fn validate_id(id: &str) -> io::Result<()> {
  if id.is_empty()
    || id.len() > 96
    || id.contains("..")
    || !id
      .bytes()
      .next()
      .is_some_and(|byte| byte.is_ascii_alphanumeric())
    || !id
      .bytes()
      .all(|byte| byte.is_ascii_alphanumeric() || b"._-".contains(&byte))
  {
    return Err(invalid("invalid plugin or resource identifier"));
  }
  Ok(())
}

pub(crate) fn read_json<T: serde::de::DeserializeOwned>(path: &Path) -> io::Result<T> {
  let path_file = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_NOFOLLOW | libc::O_PATH)
    .open(path)?;
  if !path_file.metadata()?.is_file() {
    return Err(invalid("expected a regular JSON file"));
  }
  let mut file = File::open(format!("/proc/self/fd/{}", path_file.as_raw_fd()))?;
  let mut bytes = Vec::new();
  (&mut file).take(65537).read_to_end(&mut bytes)?;
  if bytes.len() > 65536 {
    return Err(invalid("plugin JSON exceeds size limit"));
  }
  serde_json::from_slice(&bytes).map_err(|_| invalid("invalid plugin JSON"))
}

fn safe_relative(path: &str) -> bool {
  !path.is_empty()
    && path.len() <= 4096
    && Path::new(path)
      .components()
      .all(|part| matches!(part, Component::Normal(_)))
}
fn safe_absolute(path: &Path) -> bool {
  path.is_absolute()
    && path.as_os_str().len() <= 4096
    && path
      .components()
      .all(|part| matches!(part, Component::RootDir | Component::Normal(_)))
}
fn open_directory(path: &Path) -> io::Result<File> {
  if !safe_absolute(path) {
    return Err(invalid(
      "directory grant must be an absolute normalized path",
    ));
  }
  let file = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_PATH | libc::O_NOFOLLOW)
    .open(path)?;
  if !file.metadata()?.is_dir() {
    return Err(invalid("read grant must name a directory"));
  }
  Ok(file)
}
pub(crate) fn invalid(message: &str) -> io::Error {
  io::Error::new(io::ErrorKind::InvalidData, message)
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::fs;
  #[test]
  fn requests_never_become_implicit_grants() {
    let requests = Requests {
      read: vec!["music".into()],
      network: true,
      media: true,
      notifications: true,
      storage: true,
    };
    let grants = Grants::default();
    grants.validate(&requests).unwrap();
    assert!(!grants.network && grants.read.is_empty() && grants.media.is_none());
    assert!(
      Grants {
        network: true,
        ..Grants::default()
      }
      .validate(&Requests::default())
      .is_err()
    );
    for name in [
      "*",
      "org.mpris.MediaPlayer2.*",
      "org.mpris.MediaPlayer2.vlc --talk=*",
      "org.freedesktop.Notifications",
    ] {
      assert!(
        Grants {
          media: Some(name.into()),
          ..Grants::default()
        }
        .validate(&requests)
        .is_err()
      );
    }
    Grants {
      media: Some("org.mpris.MediaPlayer2.firefox.instance1".into()),
      ..Grants::default()
    }
    .validate(&requests)
    .unwrap();
    assert!(serde_json::from_str::<Requests>(r#"{"hostExec":true}"#).is_err());
    assert!(serde_json::from_str::<Grants>(r#"{"network":true,"network":false}"#).is_err());
  }
  #[test]
  fn directory_selection_is_not_a_replaceable_path() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("selected");
    fs::create_dir(&path).unwrap();
    let selected = ReadDirectory::select(&path).unwrap();
    selected.open().unwrap();
    fs::rename(&path, root.path().join("old")).unwrap();
    fs::create_dir(&path).unwrap();
    assert!(selected.open().is_err());
    for path in ["../escape", "/absolute", "dir/../escape", ""] {
      assert!(!safe_relative(path));
    }
    for id in ["../escape", "--unit", "*", "a/b", "a\n"] {
      assert!(validate_id(id).is_err());
    }
  }
}
