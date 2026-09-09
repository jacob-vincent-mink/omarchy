use crate::{
  grants::{Grants, MAX_PERSISTED_BYTES, Manifest, invalid, read_json, validate_id},
  revision::{Revision, require_private_directory},
  supervisor::{Limits, Unit, validate_name},
};
use serde::{Deserialize, Serialize};
use std::{
  ffi::OsString,
  fs::{self, File, OpenOptions},
  io::{self, Write},
  os::{
    fd::AsRawFd,
    unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt},
  },
  path::{Path, PathBuf},
  time::{Duration, Instant},
};

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Record {
  pub version: u32,
  pub id: String,
  pub revision: String,
  pub epoch: u64,
  pub enabled: bool,
  pub grants: Grants,
  pub active_unit: Option<String>,
}

pub struct Store {
  root: PathBuf,
}

impl Store {
  pub fn open(root: &Path) -> io::Result<Self> {
    require_private_directory(root)?;
    require_private_directory(&root.join("revisions"))?;
    Ok(Self { root: root.into() })
  }

  /// Explicit initialization; opening a missing store never creates grants.
  pub fn initialize(root: &Path) -> io::Result<Self> {
    if !root.is_absolute() {
      return Err(invalid("plugin store must have an absolute path"));
    }
    for path in [root.to_path_buf(), root.join("revisions")] {
      match fs::DirBuilder::new().mode(0o700).create(&path) {
        Ok(()) => (),
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => (),
        Err(error) => return Err(error),
      }
      require_private_directory(&path)?;
    }
    File::open(root)?.sync_all()?;
    File::open(
      root
        .parent()
        .ok_or_else(|| invalid("invalid plugin store root"))?,
    )?
    .sync_all()?;
    Self::open(root)
  }

  pub fn revisions(&self) -> PathBuf {
    self.root.join("revisions")
  }

  pub fn read(&self, id: &str) -> io::Result<Record> {
    let _lock = self.lock(id)?;
    self.require_ready(id)?;
    self.record(id)
  }

  /// Approval is an administrative operation on an exact reviewed snapshot.
  /// Neither manifest requests nor worker messages may call this API.
  pub fn approve(&self, revision: &str, grants: Grants) -> io::Result<Record> {
    Revision::verify(&self.revisions(), revision)?;
    let manifest = Manifest::read(&self.revisions().join(revision))?;
    grants.validate(&manifest.sandbox.requests)?;
    for directory in grants.filesystem.values() {
      directory.open()?;
    }
    let _lock = self.lock(&manifest.id)?;
    self.require_ready(&manifest.id)?;
    let previous = match self.record(&manifest.id) {
      Ok(record) => Some(record),
      Err(error) if error.kind() == io::ErrorKind::NotFound => None,
      Err(error) => return Err(error),
    };
    if previous
      .as_ref()
      .is_some_and(|record| record.active_unit.is_some())
    {
      return Err(invalid(
        "stop or revoke the previous instance before approving a revision",
      ));
    }
    let record = Record {
      version: 1,
      id: manifest.id,
      revision: revision.into(),
      epoch: next_epoch(previous.as_ref().map_or(0, |record| record.epoch))?,
      enabled: true,
      grants,
      active_unit: None,
    };
    // publish() preflights the serialized size below, before it creates the
    // durable pending marker, so no publish transition (approve, launch,
    // revoke, recover) can poison a prior usable record with an over-limit
    // approval.
    self.publish(&record, None, |_| Ok(()))?;
    Ok(record)
  }

  /// Reserve and record the unit before launching it. A crash at any point
  /// therefore leaves either no possible worker or an identity recovery can stop.
  pub fn launch(&self, id: &str, program: &Path, host_socket: &Path) -> io::Result<(Unit, Record)> {
    let _lock = self.lock(id)?;
    self.require_ready(id)?;
    let mut record = self.record(id)?;
    self.validate_authority(&record)?;
    let unit = Unit::prepare()?;
    let previous = record.active_unit.replace(unit.name().into());
    record.epoch = next_epoch(record.epoch)?;
    self.publish(&record, previous.as_deref(), |_| Ok(()))?;
    let args = [
      OsString::from("--controller"),
      host_socket.into(),
      self.root.clone().into(),
      id.into(),
      record.epoch.to_string().into(),
    ];
    let unit = unit.launch(
      program,
      &args.iter().map(OsString::as_os_str).collect::<Vec<_>>(),
      Limits::default(),
    )?;
    Ok((unit, record))
  }

  /// Keep admission serialized through the beginning/completion of a bounded
  /// effect. Revocation cannot report success while an admitted effect holds it.
  pub fn with_authority<T>(
    &self,
    id: &str,
    epoch: u64,
    unit: &str,
    effect: impl FnOnce(&Record) -> io::Result<T>,
  ) -> io::Result<T> {
    let _lock = self.lock(id)?;
    self.require_ready(id)?;
    let record = self.record(id)?;
    if !record.enabled || record.epoch != epoch || record.active_unit.as_deref() != Some(unit) {
      return Err(invalid("plugin authority is disabled or stale"));
    }
    effect(&record)
  }

  pub fn admit(&self, id: &str, epoch: u64, unit: &str) -> io::Result<Record> {
    self.with_authority(id, epoch, unit, |record| {
      self.validate_authority(record)?;
      Ok(record.clone())
    })
  }

  pub fn revoke(&self, id: &str) -> io::Result<()> {
    let _lock = self.lock(id)?;
    if self.pending(id)? {
      return self.recover_locked(id);
    }
    let mut record = match self.record(id) {
      Ok(record) => record,
      // An installed or reviewed plugin need not have been approved yet.
      Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
      Err(error) => return Err(error),
    };
    record.enabled = false;
    record.epoch = record.epoch.saturating_add(1);
    let previous = record.active_unit.take();
    self.publish(&record, previous.as_deref(), |_| Ok(()))
  }

  /// Explicit recovery only disables authority; it never completes a pending
  /// approval by guessing what the user intended.
  pub fn recover(&self, id: &str) -> io::Result<()> {
    let _lock = self.lock(id)?;
    if !self.pending(id)? {
      return Err(invalid("no interrupted publication to recover"));
    }
    self.recover_locked(id)
  }

  fn recover_locked(&self, id: &str) -> io::Result<()> {
    match self.record(id) {
      Ok(mut record) => {
        if let Some(unit) = record.active_unit.take() {
          Unit::recover(&unit)?.stop()?;
        }
        record.enabled = false;
        record.epoch = record.epoch.saturating_add(1);
        self.finish(&record, &mut |_| Ok(()))
      }
      Err(error) if error.kind() == io::ErrorKind::NotFound => {
        // No unit may be launched until its record has been durably published.
        fs::remove_file(self.path(id, "pending"))?;
        File::open(&self.root)?.sync_all()
      }
      Err(error) => Err(error),
    }
  }

  fn validate_authority(&self, record: &Record) -> io::Result<()> {
    if !record.enabled {
      return Err(invalid("plugin is not approved"));
    }
    Revision::verify(&self.revisions(), &record.revision)?;
    let manifest = Manifest::read(&self.revisions().join(&record.revision))?;
    if manifest.id != record.id {
      return Err(invalid("revision identity mismatch"));
    }
    record.grants.validate(&manifest.sandbox.requests)?;
    // A required grant that the approval did not cover must block activation
    // with an actionable explanation; it is never implied by the request. A
    // declined *optional* request does not block startup.
    let missing = record.grants.required_gap(&manifest.sandbox.requests);
    if !missing.is_empty() {
      return Err(invalid(&format!(
        "plugin requires the following access before it can start; approve each one first: {}",
        missing.join(", ")
      )));
    }
    Ok(())
  }

  fn record(&self, id: &str) -> io::Result<Record> {
    let record: Record = read_json(&self.path(id, "json"))?;
    if record.version != 1
      || record.id != id
      || record.epoch == 0
      || record.revision.len() != 64
      || !record
        .revision
        .bytes()
        .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
      return Err(invalid("invalid plugin grant record"));
    }
    if let Some(unit) = &record.active_unit {
      validate_name(unit)?;
    }
    Ok(record)
  }

  fn publish(
    &self,
    record: &Record,
    previous_unit: Option<&str>,
    mut checkpoint: impl FnMut(u8) -> io::Result<()>,
  ) -> io::Result<()> {
    // Preflight the serialized size before the pending marker exists. A
    // record may be exactly at the budget at approve() and still grow past it
    // once launch() attaches an active unit and bumps the epoch, so every
    // publish transition must be sized before committing any durable state.
    // Rejecting here leaves the prior usable record untouched.
    Self::check_record_size(record)?;
    let marker = OpenOptions::new()
      .write(true)
      .create_new(true)
      .mode(0o600)
      .open(self.path(&record.id, "pending"))?;
    marker.sync_all()?;
    checkpoint(1)?;
    File::open(&self.root)?.sync_all()?;
    checkpoint(2)?;
    if let Some(unit) = previous_unit {
      Unit::recover(unit)?.stop()?;
    }
    self.finish(record, &mut checkpoint)
  }

  fn finish(
    &self,
    record: &Record,
    checkpoint: &mut impl FnMut(u8) -> io::Result<()>,
  ) -> io::Result<()> {
    Self::check_record_size(record)?;
    let bytes = serde_json::to_vec(record).map_err(|_| invalid("could not encode grant record"))?;
    let mut temporary = tempfile::NamedTempFile::new_in(&self.root)?;
    temporary.write_all(&bytes)?;
    temporary.as_file().sync_all()?;
    checkpoint(3)?;
    temporary
      .persist(self.path(&record.id, "json"))
      .map_err(|error| error.error)?;
    checkpoint(4)?;
    File::open(&self.root)?.sync_all()?;
    checkpoint(5)?;
    fs::remove_file(self.path(&record.id, "pending"))?;
    checkpoint(6)?;
    File::open(&self.root)?.sync_all()?;
    checkpoint(7)
  }

  fn check_record_size(record: &Record) -> io::Result<()> {
    let bytes = serde_json::to_vec(record).map_err(|_| invalid("could not encode grant record"))?;
    if bytes.len() > MAX_PERSISTED_BYTES {
      return Err(invalid(&format!(
        "approved grant record is too large ({} bytes); reduce the number or length of selected directories",
        bytes.len()
      )));
    }
    Ok(())
  }

  fn lock(&self, id: &str) -> io::Result<File> {
    validate_id(id)?;
    let lock = OpenOptions::new()
      .read(true)
      .write(true)
      .create(true)
      .truncate(false)
      .mode(0o600)
      .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
      .open(self.path(id, "lock"))?;
    let metadata = lock.metadata()?;
    if !metadata.is_file() || metadata.nlink() != 1 || metadata.uid() != unsafe { libc::geteuid() }
    {
      return Err(invalid("invalid plugin writer lock"));
    }
    let deadline = Instant::now() + Duration::from_secs(1);
    loop {
      if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } == 0 {
        return Ok(lock);
      }
      let error = io::Error::last_os_error();
      if error.kind() != io::ErrorKind::WouldBlock || Instant::now() >= deadline {
        return Err(error);
      }
      std::thread::sleep(Duration::from_millis(10));
    }
  }
  fn path(&self, id: &str, extension: &str) -> PathBuf {
    self.root.join(format!("{id}.{extension}"))
  }
  fn pending(&self, id: &str) -> io::Result<bool> {
    match self.path(id, "pending").symlink_metadata() {
      Ok(_) => Ok(true),
      Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(false),
      Err(error) => Err(error),
    }
  }
  fn require_ready(&self, id: &str) -> io::Result<()> {
    if self.pending(id)? {
      Err(invalid(
        "interrupted grant publication requires explicit recovery",
      ))
    } else {
      Ok(())
    }
  }
}

fn next_epoch(epoch: u64) -> io::Result<u64> {
  epoch
    .checked_add(1)
    .ok_or_else(|| invalid("grant epoch exhausted"))
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::{
    os::unix::fs::PermissionsExt,
    process::{Command, Stdio},
  };

  fn fixture() -> (tempfile::TempDir, Store, Revision) {
    let root = tempfile::Builder::new()
      .permissions(fs::Permissions::from_mode(0o700))
      .tempdir()
      .unwrap();
    let source = root.path().join("source");
    fs::create_dir(&source).unwrap();
    fs::write(
      source.join("worker.qml"),
      "import Quickshell\nShellRoot {}\n",
    )
    .unwrap();
    fs::write(source.join("manifest.json"), serde_json::to_vec(&serde_json::json!({
      "schemaVersion": 1, "id": "test.widget", "name": "Test", "version": "1", "kinds": ["barWidget"],
      "entryPoints": {"barWidget": "worker.qml"}, "sandbox": {"version": 1, "entryPoint": "worker.qml",
        "requests": {"network": true, "storage": true, "filesystem": [{"name": "files"}]}}
    })).unwrap()).unwrap();
    let store = Store::initialize(&root.path().join("state")).unwrap();
    let revision = Revision::import(&source, &store.revisions()).unwrap();
    (root, store, revision)
  }

  /// Build a store + revision whose manifest declares `count` optional
  /// filesystem slots (each with the maximum legal 96-character name) and
  /// returns them along with a data root holding a selected directory for
  /// every slot.
  fn filesystem_fixture(
    count: usize,
    deep: bool,
  ) -> (
    tempfile::TempDir,
    Store,
    Revision,
    Vec<String>,
    std::path::PathBuf,
  ) {
    let root = tempfile::Builder::new()
      .permissions(fs::Permissions::from_mode(0o700))
      .tempdir()
      .unwrap();
    let source = root.path().join("source");
    fs::create_dir(&source).unwrap();
    fs::write(
      source.join("worker.qml"),
      "import Quickshell\nShellRoot {}\n",
    )
    .unwrap();
    let names: Vec<String> = (0..count)
      .map(|i| format!("grant-{i:04}-{}", "n".repeat(85)))
      .collect();
    let data = if deep {
      root.path().join(format!("deep-{}", "y".repeat(140)))
    } else {
      root.path().join("data")
    };
    fs::create_dir(&data).unwrap();
    for i in 0..count {
      fs::create_dir(data.join(format!("d{i}"))).unwrap();
    }
    fs::write(
      source.join("manifest.json"),
      serde_json::to_vec(&serde_json::json!({
        "schemaVersion": 1, "id": "size.widget", "name": "S", "version": "1", "kinds": ["barWidget"],
        "entryPoints": {"barWidget": "worker.qml"}, "sandbox": {"version": 1, "entryPoint": "worker.qml",
          "requests": {"filesystem": names.iter().map(crate::grants::FileSystemRequest::optional).collect::<Vec<_>>()}}
      }))
      .unwrap(),
    )
    .unwrap();
    let store = Store::initialize(&root.path().join("state")).unwrap();
    let revision = Revision::import(&source, &store.revisions()).unwrap();
    (root, store, revision, names, data)
  }

  fn select_all<P: AsRef<Path>>(data: &P, names: &[String]) -> Grants {
    use crate::grants::{Access, FileSystemGrant, Target};
    let mut grants = Grants::default();
    for (i, name) in names.iter().enumerate() {
      grants.filesystem.insert(
        name.clone(),
        FileSystemGrant::select(
          &data.as_ref().join(format!("d{i}")),
          Access::Read,
          Target::Directory,
        )
        .unwrap(),
      );
    }
    grants
  }

  #[test]
  fn max_count_filesystem_grants_round_trip_within_the_persisted_budget() {
    use crate::grants::MAX_GRANTED_DIRS;
    let (_root, store, revision, names, data) = filesystem_fixture(MAX_GRANTED_DIRS, false);
    let grants = select_all(&data, &names);
    let record = store.approve(&revision.digest, grants).unwrap();
    assert_eq!(record.grants.filesystem.len(), MAX_GRANTED_DIRS);
    let persisted = serde_json::to_vec(&record).unwrap();
    assert!(
      persisted.len() <= MAX_PERSISTED_BYTES,
      "max-count record exceeds the persisted budget: {} bytes",
      persisted.len()
    );
    // The persisted file is smaller than the in-memory JSON (no pretty format)
    // and reopens through the capped read path without truncation.
    let reopened = store.read("size.widget").unwrap();
    assert_eq!(reopened.grants.filesystem.len(), MAX_GRANTED_DIRS);
    assert_eq!(reopened.revision, revision.digest);
  }

  #[test]
  fn oversize_approval_is_rejected_before_publishing_or_poisoning() {
    use crate::grants::MAX_GRANTED_DIRS;
    // A realistic shorter-path record first, so there is a prior usable
    // approval to guarantee is left untouched by a rejected oversize attempt.
    let (_root, store, revision, all_names, _data) = filesystem_fixture(MAX_GRANTED_DIRS, false);
    let tiny = store
      .approve(&revision.digest, select_all(&_data, &all_names[0..1]))
      .unwrap();
    assert!(!store.pending("size.widget").unwrap());
    // The oversize attempt mounts the same slots under a very deep path so the
    // persisted record would exceed the 64 KiB cap while still validating and
    // opening every selected directory.
    let deep = _root.path().join(format!("deep-{}", "y".repeat(140)));
    fs::create_dir(&deep).unwrap();
    for i in 0..all_names.len() {
      fs::create_dir(deep.join(format!("d{i}"))).unwrap();
    }
    let oversize = select_all(&deep, &all_names);
    let error = store.approve(&revision.digest, oversize).unwrap_err();
    let message = error.to_string();
    assert!(message.contains("too large"), "unexpected error: {message}");
    // No pending marker was created, and the prior usable approval is intact.
    assert!(!store.pending("size.widget").unwrap());
    let current = store.read("size.widget").unwrap();
    assert_eq!(current.grants.filesystem.len(), 1);
    assert_eq!(current.epoch, tiny.epoch);
  }

  #[test]
  fn launch_growth_oversize_is_rejected_before_pending_preserving_prior_approval() {
    use crate::grants::{Access as GrantAccess, FileSystemGrant, Target};
    let root = tempfile::Builder::new()
      .permissions(fs::Permissions::from_mode(0o700))
      .tempdir()
      .unwrap();
    let store = Store::initialize(&root.path().join("state")).unwrap();
    // The active-unit string below is long only to widen the "growth band"
    // (its serialized contribution) past the per-grant step, so the boundary
    // count is hit deterministically; the mechanism is identical for a real
    // short unit name that pushes an already-near-limit record over the cap.
    let unit: String = format!("unit-{}", "x".repeat(2000));
    let mut count = 0usize;
    loop {
      let mut grants = Grants::default();
      for i in 0..=count {
        grants.filesystem.insert(
          format!("g{i}"),
          FileSystemGrant {
            path: format!("/data/d{i:04}/{}", "y".repeat(520)).into(),
            device: 1,
            inode: i as u64 + 1,
            access: GrantAccess::Read,
            target: Target::Directory,
          },
        );
      }
      let base = Record {
        version: 1,
        id: "size.widget".into(),
        revision: "ab".repeat(32),
        epoch: 1,
        enabled: true,
        grants,
        active_unit: None,
      };
      let grown = Record {
        active_unit: Some(unit.clone()),
        ..base.clone()
      };
      let base_size = serde_json::to_vec(&base).unwrap().len();
      let grown_size = serde_json::to_vec(&grown).unwrap().len();
      if base_size <= MAX_PERSISTED_BYTES && grown_size > MAX_PERSISTED_BYTES {
        // Commit the bare approval as the usable prior state.
        store.publish(&base, None, |_| Ok(())).unwrap();
        assert!(!store.pending("size.widget").unwrap());
        // launch()'s growth (active unit + epoch bump) must be rejected before
        // publish() creates a pending marker, so the prior approval is kept.
        let error = store.publish(&grown, None, |_| Ok(())).unwrap_err();
        assert!(error.to_string().contains("too large"));
        assert!(!store.pending("size.widget").unwrap());
        let current = store.read("size.widget").unwrap();
        assert!(current.enabled);
        assert_eq!(current.epoch, 1);
        assert_eq!(current.active_unit, None);
        assert_eq!(current.grants.filesystem.len(), count + 1);
        return;
      }
      if count > crate::grants::MAX_GRANTED_DIRS * 2 {
        panic!("could not construct a budget-boundary grant set");
      }
      count += 1;
    }
  }

  #[test]
  fn grants_are_revision_bound_and_revocation_is_not_approval() {
    let (_root, store, revision) = fixture();
    let approved = store.approve(&revision.digest, Grants::default()).unwrap();
    assert!(approved.enabled && !approved.grants.network);
    assert_eq!(store.read("test.widget").unwrap().revision, revision.digest);
    assert!(
      store
        .approve(
          &revision.digest,
          Grants {
            notifications: true,
            ..Grants::default()
          }
        )
        .is_err()
    );
    store.revoke("test.widget").unwrap();
    let revoked = store.read("test.widget").unwrap();
    assert!(!revoked.enabled && revoked.epoch > approved.epoch);
    assert!(store.validate_authority(&revoked).is_err());
    let approved = store
      .approve(
        &revision.digest,
        Grants {
          network: true,
          ..Grants::default()
        },
      )
      .unwrap();
    assert!(approved.epoch > revoked.epoch);
    let mut record = approved.clone();
    let reservation = Unit::prepare().unwrap();
    record.active_unit = Some(reservation.name().into());
    store.publish(&record, None, |_| Ok(())).unwrap();
    assert!(
      store
        .with_authority("test.widget", revoked.epoch, reservation.name(), |_| Ok(()))
        .is_err()
    );
    assert!(
      store
        .with_authority("test.widget", record.epoch, "wrong", |_| Ok(()))
        .is_err()
    );
    store
      .with_authority("test.widget", record.epoch, reservation.name(), |_| Ok(()))
      .unwrap();
    assert!(store.approve(&revision.digest, Grants::default()).is_err());
    fs::write(revision.path.join("worker.qml"), "modified").unwrap();
    assert!(store.validate_authority(&record).is_err());
  }

  #[test]
  fn publication_crash_child() {
    let Some(root) = std::env::var_os("OMARCHY_STORE_CRASH_ROOT") else {
      return;
    };
    let stage: u8 = std::env::var("OMARCHY_STORE_CRASH_STAGE")
      .unwrap()
      .parse()
      .unwrap();
    let store = Store::open(Path::new(&root)).unwrap();
    let _lock = store.lock("test.widget").unwrap();
    let mut record = store.record("test.widget").unwrap();
    record.epoch += 1;
    record.grants.network = true;
    store
      .publish(&record, None, |checkpoint| {
        if checkpoint == stage {
          unsafe {
            libc::_exit(71);
          }
        }
        Ok(())
      })
      .unwrap();
    panic!("crash checkpoint was not reached");
  }

  #[test]
  fn publication_errors_and_process_crashes_have_explicit_recovery() {
    for crash in [false, true] {
      for stage in 1..=7 {
        let (_root, store, revision) = fixture();
        let approved = store.approve(&revision.digest, Grants::default()).unwrap();
        if crash {
          let status = Command::new(std::env::current_exe().unwrap())
            .args([
              "--exact",
              "store::tests::publication_crash_child",
              "--nocapture",
            ])
            .env("OMARCHY_STORE_CRASH_ROOT", &store.root)
            .env("OMARCHY_STORE_CRASH_STAGE", stage.to_string())
            .stdout(Stdio::null())
            .status()
            .unwrap();
          assert_eq!(status.code(), Some(71));
        } else {
          let mut next = approved.clone();
          next.epoch += 1;
          next.grants.network = true;
          let _lock = store.lock("test.widget").unwrap();
          assert!(
            store
              .publish(&next, None, |checkpoint| {
                if checkpoint == stage {
                  Err(io::Error::other("injected filesystem failure"))
                } else {
                  Ok(())
                }
              })
              .is_err()
          );
        }
        let reopened = Store::open(&store.root).unwrap();
        if stage <= 5 {
          assert!(
            reopened.read("test.widget").is_err(),
            "pending transaction was admitted"
          );
          assert!(
            reopened
              .approve(&revision.digest, Grants::default())
              .is_err()
          );
          reopened.recover("test.widget").unwrap();
          let recovered = reopened.read("test.widget").unwrap();
          assert!(
            !recovered.enabled,
            "recovery must not finish an interrupted approval"
          );
        } else {
          // Marker removal is the visibility commit point. The record was
          // already synchronized; a missing acknowledgement may still commit
          // exactly the authority approved by this operation, never other data.
          let committed = reopened.read("test.widget").unwrap();
          assert!(committed.enabled && committed.grants.network);
          assert_eq!(committed.revision, approved.revision);
          assert_eq!(committed.epoch, approved.epoch + 1);
        }
      }
    }
  }

  #[test]
  fn malformed_state_and_writer_contention_fail_closed() {
    let (_root, store, revision) = fixture();
    store.approve(&revision.digest, Grants::default()).unwrap();
    let lock = store.lock("test.widget").unwrap();
    let root = store.root.clone();
    let digest = revision.digest.clone();
    let writer = std::thread::spawn(move || {
      Store::open(&root)
        .unwrap()
        .approve(&digest, Grants::default())
        .unwrap_err()
    });
    assert_eq!(writer.join().unwrap().kind(), io::ErrorKind::WouldBlock);
    drop(lock);
    assert!(store.read("test.widget").unwrap().enabled);
    fs::write(store.path("test.widget", "json"), b"{invalid").unwrap();
    assert!(store.read("test.widget").is_err());
    assert!(store.approve(&revision.digest, Grants::default()).is_err());
    assert!(store.read("../escape").is_err());
  }

  #[test]
  fn exhausted_epoch_cannot_prevent_revocation() {
    let (_root, store, revision) = fixture();
    let mut record = store.approve(&revision.digest, Grants::default()).unwrap();
    record.epoch = u64::MAX;
    store.publish(&record, None, |_| Ok(())).unwrap();
    assert!(store.approve(&revision.digest, Grants::default()).is_err());
    store.revoke("test.widget").unwrap();
    assert!(!store.read("test.widget").unwrap().enabled);
  }
}
