use crate::{
  controller::Control,
  presentation,
  session::{Session, Update},
};
use std::{io, os::fd::AsRawFd, path::PathBuf};

#[cxx::bridge(namespace = "omarchy")]
mod ffi {
  enum EventKind {
    Empty,
    Ready,
    Configured,
    Buffer,
    Frame,
    Mask,
    PanelState,
    Failed,
  }
  struct NativeRegion {
    operation: u32,
    x: u32,
    y: u32,
    width: u32,
    height: u32,
  }
  struct BufferInfo {
    fd: i32,
    width: u32,
    height: u32,
    stride: u32,
  }
  struct NativeEvent {
    kind: EventKind,
    generation: u64,
    width: u32,
    height: u32,
    scale: u32,
    serial: u64,
    slot: u32,
    panel_serial: u32,
    panel_open: bool,
    buffer: Box<NativeBuffer>,
    regions: Vec<NativeRegion>,
    error: String,
  }
  extern "Rust" {
    type Session;
    type NativeBuffer;
    fn begin(
      root: &str,
      id: &str,
      controller: &str,
      width: u32,
      height: u32,
      scale: u32,
      context: &str,
    ) -> Result<Box<Session>>;
    fn next(session: &Session) -> Result<NativeEvent>;
    fn input(session: &Session, kind: u32, code: u32, x: i32, y: i32) -> Result<()>;
    fn scroll(
      session: &Session,
      source: u32,
      x: i32,
      y: i32,
      horizontal: i32,
      vertical: i32,
    ) -> Result<()>;
    fn presented(session: &Session, serial: u64) -> Result<()>;
    fn configure(session: &Session, width: u32, height: u32, scale: u32) -> Result<()>;
    fn context(session: &Session, json: &str) -> Result<()>;
    fn info(self: &NativeBuffer) -> BufferInfo;
  }
}
pub struct NativeBuffer(Option<presentation::Buffer>);
impl NativeBuffer {
  fn info(&self) -> ffi::BufferInfo {
    self.0.as_ref().map_or(
      ffi::BufferInfo {
        fd: -1,
        width: 0,
        height: 0,
        stride: 0,
      },
      |buffer| ffi::BufferInfo {
        fd: buffer.fd.as_raw_fd(),
        width: buffer.width,
        height: buffer.height,
        stride: buffer.stride,
      },
    )
  }
}
fn begin(
  root: &str,
  id: &str,
  controller: &str,
  width: u32,
  height: u32,
  scale: u32,
  context: &str,
) -> io::Result<Box<Session>> {
  Session::start_with_context(
    PathBuf::from(root),
    id.into(),
    PathBuf::from(controller),
    presentation::Viewport {
      width,
      height,
      scale,
    },
    parse_context(context)?,
  )
  .map(Box::new)
}
fn parse_context(json: &str) -> io::Result<crate::context::UiContext> {
  if json.is_empty() {
    Ok(crate::context::UiContext::default())
  } else {
    crate::context::UiContext::parse(json.as_bytes())
  }
}
fn context(session: &Session, json: &str) -> io::Result<()> {
  session.send(Control::Context(parse_context(json)?))
}
fn next(session: &Session) -> io::Result<ffi::NativeEvent> {
  let mut event = ffi::NativeEvent {
    kind: ffi::EventKind::Empty,
    generation: 0,
    width: 0,
    height: 0,
    scale: 0,
    serial: 0,
    slot: 0,
    panel_serial: 0,
    panel_open: false,
    buffer: Box::new(NativeBuffer(None)),
    regions: Vec::new(),
    error: String::new(),
  };
  match session.poll()? {
    None => (),
    Some(Update::Ready) => event.kind = ffi::EventKind::Ready,
    Some(Update::PanelState { serial, open }) => {
      event.kind = ffi::EventKind::PanelState;
      event.panel_serial = serial;
      event.panel_open = open;
    }
    Some(Update::Presentation(presentation::Event::Configured {
      generation,
      viewport,
    })) => {
      event.kind = ffi::EventKind::Configured;
      event.generation = generation;
      event.width = viewport.width;
      event.height = viewport.height;
      event.scale = viewport.scale;
    }
    Some(Update::Failed(error)) => {
      event.kind = ffi::EventKind::Failed;
      event.error = error;
    }
    Some(Update::Presentation(presentation::Event::Buffer(buffer))) => {
      event.kind = ffi::EventKind::Buffer;
      event.slot = buffer.slot;
      event.buffer.0 = Some(buffer);
    }
    Some(Update::Presentation(presentation::Event::Frame { serial, slot, .. })) => {
      event.kind = ffi::EventKind::Frame;
      event.serial = serial;
      event.slot = slot;
    }
    Some(Update::Presentation(presentation::Event::Mask { regions, .. })) => {
      event.kind = ffi::EventKind::Mask;
      event.regions = regions
        .into_iter()
        .map(|r| ffi::NativeRegion {
          operation: r.operation,
          x: r.x,
          y: r.y,
          width: r.width,
          height: r.height,
        })
        .collect();
    }
  }
  Ok(event)
}
fn input(session: &Session, kind: u32, code: u32, x: i32, y: i32) -> io::Result<()> {
  if kind > 5 {
    return Err(io::Error::other("invalid input kind"));
  }
  session.send(Control::Input { kind, code, x, y })
}
fn presented(session: &Session, serial: u64) -> io::Result<()> {
  session.send(Control::Presented(serial))
}
fn scroll(
  session: &Session,
  source: u32,
  x: i32,
  y: i32,
  horizontal: i32,
  vertical: i32,
) -> io::Result<()> {
  session.send(Control::Scroll(crate::controller::Scroll {
    source,
    x,
    y,
    horizontal,
    vertical,
  }))
}
fn configure(session: &Session, width: u32, height: u32, scale: u32) -> io::Result<()> {
  session.send(Control::Configure(presentation::Viewport {
    width,
    height,
    scale,
  }))
}
