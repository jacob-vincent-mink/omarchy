use crate::{
  grants::{Grants, validate_id},
  media::MediaProxy,
  sandbox, supervisor,
};
use std::{
  ffi::OsStr,
  fs::{File, OpenOptions},
  io,
  os::{
    fd::{AsFd, AsRawFd},
    unix::{
      fs::{FileTypeExt, OpenOptionsExt},
      process::CommandExt,
    },
  },
  path::Path,
  process::{Child, Command, Stdio},
};

#[derive(Default)]
pub struct Resources<'a> {
  pub render_node: Option<&'a Path>,
  pub media: Option<&'a MediaProxy>,
  pub notifications: Option<&'a crate::notification::Broker>,
  pub runtime: Option<&'a File>,
}

/// Launch only from a resource-limited trusted controller. All arguments and
/// descriptors are controller-owned, never supplied by a worker RPC. The bundle
/// must already be an approved immutable revision: pinning a directory prevents
/// pathname substitution, not modification of its contents.
///
/// Grants must come from the controller's current admitted record. Read-only
/// directory slots, optional network access, a selected media proxy, and a GPU
/// render node and text notifications are implemented; storage is not yet.
/// `bootstrap` must restrict itself before loading any code from the bundle.
/// The caller must drain stderr with a bounded log budget and supervise child
/// exit; dropping the returned Child is not a substitute for stopping the Unit.
pub fn spawn(
  bootstrap: &File,
  bundle: &File,
  display: &File,
  args: &[&OsStr],
  limits: supervisor::Limits,
  grants: &Grants,
  resources: Resources<'_>,
) -> io::Result<Child> {
  supervisor::verify_controller_limits(limits)?;
  if !bootstrap.metadata()?.is_file()
    || !bundle.metadata()?.is_dir()
    || !display.metadata()?.file_type().is_socket()
  {
    return Err(io::Error::new(
      io::ErrorKind::InvalidInput,
      "invalid worker mount descriptors",
    ));
  }
  if grants.read.len() > 8 || grants.storage {
    return Err(io::Error::new(
      io::ErrorKind::Unsupported,
      "worker resource grant is not implemented yet",
    ));
  }
  let media = match (&grants.media, resources.media) {
    (Some(name), Some(proxy)) => Some(proxy.socket(name)?),
    (None, None) => None,
    _ => return Err(io::Error::other("media grant and prepared proxy disagree")),
  };
  let notifications = match (grants.notifications, resources.notifications) {
    (true, Some(broker)) => Some(broker.socket()),
    (false, None) => None,
    _ => {
      return Err(io::Error::other(
        "notification grant and prepared broker disagree",
      ));
    }
  };
  let mut directories = Vec::new();
  for (name, directory) in &grants.read {
    validate_id(name)?;
    directories.push((name, directory.open()?));
  }
  let mut command = Command::new("/usr/bin/bwrap");
  command.env_clear().args([
    "--unshare-all",
    "--unshare-user",
    "--unshare-cgroup",
    "--disable-userns",
    "--assert-userns-disabled",
    "--die-with-parent",
    "--new-session",
    "--cap-drop",
    "ALL",
    "--clearenv",
    "--ro-bind",
    "/usr",
    "/usr",
    "--symlink",
    "usr/lib",
    "/lib",
    "--symlink",
    "usr/lib",
    "/lib64",
    "--symlink",
    "usr/bin",
    "/bin",
    "--symlink",
    "usr/bin",
    "/sbin",
    "--proc",
    "/proc",
    "--dev",
    "/dev",
    "--size",
    "16777216",
    "--tmpfs",
    "/tmp",
    "--size",
    "33554432",
    "--tmpfs",
    "/home/plugin",
    "--size",
    "8388608",
    "--tmpfs",
    "/run/plugin",
    "--ro-bind",
    "/etc/fonts",
    "/etc/fonts",
  ]);
  if grants.network {
    command.args([
      "--share-net",
      "--ro-bind",
      "/etc/resolv.conf",
      "/etc/resolv.conf",
      "--ro-bind",
      "/etc/hosts",
      "/etc/hosts",
      "--ro-bind",
      "/etc/ssl",
      "/etc/ssl",
      "--ro-bind",
      "/etc/ca-certificates",
      "/etc/ca-certificates",
    ]);
  }
  if let Some(node) = resources.render_node {
    let metadata = node.metadata()?;
    let name = node
      .file_name()
      .and_then(OsStr::to_str)
      .ok_or_else(|| io::Error::other("invalid render node"))?;
    if node.parent() != Some(Path::new("/dev/dri"))
      || !metadata.file_type().is_char_device()
      || !name
        .strip_prefix("renderD")
        .is_some_and(|value| value.parse::<u32>().is_ok_and(|minor| minor >= 128))
    {
      return Err(io::Error::other("graphics requires a DRM render node"));
    }
    use std::os::unix::fs::MetadataExt;
    let device_number = format!(
      "{}:{}",
      libc::major(metadata.rdev()),
      libc::minor(metadata.rdev())
    );
    let render_sys = Path::new("/sys/dev/char")
      .join(&device_number)
      .canonicalize()?;
    let device_sys = render_sys.join("device").canonicalize()?;
    if !device_sys.starts_with("/sys/devices") || !render_sys.starts_with(&device_sys) {
      return Err(io::Error::other("unsupported render-device topology"));
    }
    command
      .arg("--dev-bind")
      .arg(node)
      .arg(node)
      .arg("--ro-bind")
      .arg(&device_sys)
      .arg(&device_sys)
      .arg("--symlink")
      .arg(&render_sys)
      .arg(format!("/sys/dev/char/{device_number}"))
      .arg("--symlink")
      .arg(&render_sys)
      .arg(format!("/sys/class/drm/{name}"))
      .args(["--setenv", "QSG_RHI_BACKEND", "opengl"]);
  }
  let mounts = [
    (bootstrap, "/bootstrap"),
    (bundle, "/plugin"),
    (display, "/run/plugin/wayland"),
  ];
  let mut descriptors = mounts.map(|(file, _)| file.as_raw_fd()).to_vec();
  for (file, destination) in mounts {
    command.args(["--ro-bind-fd", &file.as_raw_fd().to_string(), destination]);
  }
  if let Some(runtime) = resources.runtime {
    if !runtime.metadata()?.is_dir() {
      return Err(io::Error::other("shared worker runtime is not a directory"));
    }
    command.args([
      "--ro-bind-fd",
      &runtime.as_raw_fd().to_string(),
      "/runtime",
      "--setenv",
      "OMARCHY_PATH",
      "/runtime",
    ]);
    descriptors.push(runtime.as_raw_fd());
  }
  for (name, file) in &directories {
    command.args([
      "--ro-bind-fd",
      &file.as_raw_fd().to_string(),
      &format!("/grants/{name}"),
    ]);
    descriptors.push(file.as_raw_fd());
  }
  if let Some(file) = media {
    command.args([
      "--ro-bind-fd",
      &file.as_raw_fd().to_string(),
      "/run/plugin/media",
      "--setenv",
      "DBUS_SESSION_BUS_ADDRESS",
      "unix:path=/run/plugin/media",
    ]);
    descriptors.push(file.as_raw_fd());
  }
  if let Some(file) = notifications {
    command.args([
      "--ro-bind-fd",
      &file.as_raw_fd().to_string(),
      "/run/plugin/notify",
    ]);
    descriptors.push(file.as_raw_fd());
  }
  for (name, value) in [
    ("HOME", "/home/plugin"),
    ("PATH", "/usr/bin"),
    ("LANG", "C.UTF-8"),
    ("XDG_RUNTIME_DIR", "/run/plugin"),
    ("WAYLAND_DISPLAY", "wayland"),
    ("QT_QPA_PLATFORM", "wayland"),
    ("QT_QPA_PLATFORMTHEME", "none"),
    ("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1"),
    ("QML_IMPORT_PATH", "/plugin/native"),
  ] {
    command.args(["--setenv", name, value]);
  }
  command
    .args([
      "--remount-ro",
      "/",
      "--chdir",
      "/plugin",
      "--",
      "/bootstrap",
    ])
    .args(args)
    .stdin(Stdio::null())
    .stdout(Stdio::null())
    .stderr(Stdio::piped());
  // Only the child clears CLOEXEC on these borrowed descriptors. The parent
  // retains its flags, including when other threads are concurrently spawning.
  // No allocation or non-async-signal-safe operation is used after fork.
  unsafe {
    command.pre_exec(move || {
      for &fd in &descriptors {
        if libc::fcntl(fd, libc::F_SETFD, 0) < 0 {
          return Err(io::Error::last_os_error());
        }
      }
      Ok(())
    });
  }
  command.spawn()
}

/// Call in the bootstrap, before loading QML, native modules, or helpers.
/// An error is terminal: the process must exit without running plugin code.
pub fn restrict_bootstrap() -> io::Result<()> {
  // Do not rely solely on Bubblewrap's descriptor cleanup. In particular, a
  // connected inherited socket bypasses pathname-based Landlock restrictions.
  if unsafe { libc::syscall(libc::SYS_close_range, 3u32, u32::MAX, 0) } < 0 {
    return Err(io::Error::last_os_error());
  }
  let socket = OpenOptions::new()
    .read(true)
    .custom_flags(libc::O_PATH | libc::O_NOFOLLOW)
    .open("/run/plugin/wayland")?;
  let mut sockets = vec![socket];
  for path in ["/run/plugin/media", "/run/plugin/notify"] {
    match OpenOptions::new()
      .read(true)
      .custom_flags(libc::O_PATH | libc::O_NOFOLLOW)
      .open(path)
    {
      Ok(socket) => sockets.push(socket),
      Err(error) if error.kind() == io::ErrorKind::NotFound => (),
      Err(error) => return Err(error),
    }
  }
  let directories = ["/tmp", "/home/plugin", "/run/plugin"].map(|path| {
    OpenOptions::new()
      .read(true)
      .custom_flags(libc::O_PATH | libc::O_NOFOLLOW)
      .open(path)
  });
  let directories = directories.into_iter().collect::<io::Result<Vec<_>>>()?;
  sandbox::restrict_private_worker(
    &sockets.iter().map(AsFd::as_fd).collect::<Vec<_>>(),
    &directories.iter().map(AsFd::as_fd).collect::<Vec<_>>(),
  )
}
