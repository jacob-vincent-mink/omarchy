//! Shared private-display fixture for tests of the trusted Qt host. No fixture
//! connects to the desktop compositor; only its supervised workers use systemd.
use omarchy_plugin_host::{
  channel::Channel,
  graphics::Graphics,
  presentation::{Event, Viewport},
};
use smithay::backend::{
  allocator::{
    Fourcc, Modifier,
    dmabuf::{Dmabuf, DmabufFlags},
  },
  egl::{EGLContext, EGLDevice, EGLDisplay},
  renderer::{ExportMem, ImportDma, gles::GlesRenderer},
};
use std::{
  ffi::OsStr,
  fs,
  io::{self, Write},
  os::unix::fs::PermissionsExt,
  path::Path,
  process::{Child, Command},
};

pub struct Host(pub Child);
impl Drop for Host {
  fn drop(&mut self) {
    let _ = self.0.kill();
    let _ = self.0.wait();
  }
}

pub fn runtime() -> tempfile::TempDir {
  let root = tempfile::Builder::new()
    .permissions(fs::Permissions::from_mode(0o700))
    .tempdir()
    .unwrap();
  fs::create_dir(root.path().join("systemd")).unwrap();
  std::os::unix::fs::symlink(
    std::path::PathBuf::from(std::env::var_os("XDG_RUNTIME_DIR").unwrap()).join("systemd/private"),
    root.path().join("systemd/private"),
  )
  .unwrap();
  root
}

pub fn command(root: &Path, qml: &Path, module: &OsStr) -> Command {
  let mut command = Command::new("/usr/bin/quickshell");
  command
    .args(["--no-color", "-n", "-p"])
    .arg(qml)
    .env_remove("DISPLAY")
    .env("XDG_RUNTIME_DIR", root)
    .env("XDG_CONFIG_HOME", root.join("config"))
    .env("XDG_CACHE_HOME", root.join("cache"))
    .env("WAYLAND_DISPLAY", "wayland")
    .env("QT_QPA_PLATFORM", "wayland")
    .env("QT_QPA_PLATFORMTHEME", "none")
    .env("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1")
    .env("QSG_RHI_BACKEND", "opengl")
    .env("QSG_RENDER_LOOP", "threaded")
    .env("QML_IMPORT_PATH", module);
  command
}

pub struct Desktop {
  pub graphics: Graphics,
  producer: Channel,
  consumer: Channel,
  renderer: GlesRenderer,
  buffers: [Option<Dmabuf>; 2],
  pixels: (i32, i32),
}

pub struct Frame {
  pixels: Vec<u8>,
  size: (i32, i32),
}
impl Frame {
  pub fn count(&self, rgb: [u8; 3]) -> usize {
    self
      .pixels
      .chunks_exact(4)
      .filter(|pixel| pixel[..3] == rgb)
      .count()
  }
  pub fn save(&self, path: impl AsRef<Path>) {
    let mut file = io::BufWriter::new(fs::File::create(path).unwrap());
    write!(file, "P6\n{} {}\n255\n", self.size.0, self.size.1).unwrap();
    for pixel in self.pixels.chunks_exact(4) {
      file.write_all(&pixel[..3]).unwrap();
    }
  }
}
impl Desktop {
  pub fn new(root: &Path, viewport: Viewport) -> Self {
    let mut graphics = Graphics::new(&root.join("wayland"), viewport).unwrap();
    let (producer, consumer) = Channel::pair().unwrap();
    graphics.describe(&producer).unwrap();
    let device = EGLDevice::enumerate()
      .unwrap()
      .find(|device| device.render_device_path().is_ok())
      .unwrap();
    let egl = unsafe { EGLDisplay::new(device).unwrap() };
    let renderer = unsafe { GlesRenderer::new(EGLContext::new(&egl).unwrap()).unwrap() };
    let (width, height) = viewport.pixels().unwrap();
    Self {
      graphics,
      producer,
      consumer,
      renderer,
      buffers: [None, None],
      pixels: (width as i32, height as i32),
    }
  }

  pub fn step(&mut self, time: u32) -> Vec<Frame> {
    self.graphics.dispatch().unwrap();
    self.graphics.render(&self.producer, time).unwrap();
    let mut frames = Vec::new();
    for _ in 0..8 {
      let packet = match self.consumer.receive() {
        Ok(packet) => packet,
        Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
        Err(error) => panic!("private host presentation failed: {error}"),
      };
      match Event::decode(packet).unwrap() {
        Event::Buffer(buffer) => {
          assert_eq!((buffer.width as i32, buffer.height as i32), self.pixels);
          let mut builder = Dmabuf::builder(
            self.pixels,
            Fourcc::Argb8888,
            Modifier::Linear,
            DmabufFlags::empty(),
          );
          assert!(builder.add_plane(buffer.fd, 0, 0, buffer.stride));
          self.buffers[buffer.slot as usize] = builder.build();
        }
        Event::Frame { serial, slot, .. } => {
          let texture = self
            .renderer
            .import_dmabuf(self.buffers[slot as usize].as_ref().unwrap(), None)
            .unwrap();
          let mapping = self
            .renderer
            .copy_texture(
              &texture,
              smithay::utils::Rectangle::from_size(self.pixels.into()),
              Fourcc::Abgr8888,
            )
            .unwrap();
          frames.push(Frame {
            pixels: self.renderer.map_texture(&mapping).unwrap().to_vec(),
            size: self.pixels,
          });
          self.graphics.presented(serial).unwrap();
        }
        _ => (),
      }
    }
    frames
  }
}
