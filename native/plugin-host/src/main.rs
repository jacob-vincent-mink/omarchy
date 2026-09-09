use std::{
  io,
  os::{fd::AsFd, unix::process::CommandExt},
  process::Command,
};

fn main() -> io::Result<()> {
  let args = std::env::args_os().skip(1).collect::<Vec<_>>();
  match args.as_slice() {
    [mode, root] if mode == "--manage" => {
      omarchy_plugin_host::management::run(std::path::Path::new(root))
    }
    [mode, title, body] if mode == "--notify" => {
      let text = |value: &std::ffi::OsStr| {
        value
          .to_str()
          .map(str::to_owned)
          .ok_or_else(|| io::Error::other("notification requires UTF-8 text"))
      };
      omarchy_plugin_host::notification::request(text(title)?, text(body)?)
    }
    [mode] if mode == "--worker" || mode == "--omarchy-worker" => {
      omarchy_plugin_host::worker::restrict_bootstrap()?;
      Err(
        Command::new("/usr/bin/quickshell")
          .stdout(io::stderr().as_fd().try_clone_to_owned()?)
          .args(["--no-color", "-p"])
          .arg(if mode == "--omarchy-worker" {
            "/runtime/shell/worker.qml"
          } else {
            "/plugin"
          })
          .exec(),
      )
    }
    [mode, entry] if mode == "--worker" => {
      omarchy_plugin_host::worker::restrict_bootstrap()?;
      let entry = std::path::Path::new(entry);
      if entry.as_os_str().is_empty()
        || entry
          .components()
          .any(|part| !matches!(part, std::path::Component::Normal(_)))
      {
        return Err(io::Error::other("invalid worker entry point"));
      }
      Err(
        Command::new("/usr/bin/quickshell")
          .stdout(io::stderr().as_fd().try_clone_to_owned()?)
          .arg("--no-color")
          .arg("-p")
          .arg(std::path::Path::new("/plugin").join(entry))
          .exec(),
      )
    }
    [mode, path] if mode == "--controller" => {
      omarchy_plugin_host::controller::run(std::path::Path::new(path), None)
    }
    [mode, path, root, id, epoch] if mode == "--controller" => {
      let id = id
        .to_str()
        .ok_or_else(|| io::Error::other("invalid plugin id"))?;
      let epoch = epoch
        .to_str()
        .and_then(|value| value.parse().ok())
        .ok_or_else(|| io::Error::other("invalid grant epoch"))?;
      let approval =
        omarchy_plugin_host::controller::Approval::open(std::path::Path::new(root), id, epoch)?;
      omarchy_plugin_host::controller::run(std::path::Path::new(path), Some(approval))
    }
    _ => Err(io::Error::other(
      "expected internal --worker or --controller SOCKET",
    )),
  }
}
