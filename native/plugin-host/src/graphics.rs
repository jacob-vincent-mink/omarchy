use crate::{
  channel::Channel,
  controller::Scroll,
  presentation::{self, Event, Frames, Region, Viewport},
};
use smithay::{
  backend::{
    allocator::{
      Allocator, Buffer as _, Fourcc, Modifier,
      dmabuf::{AsDmabuf, Dmabuf},
      gbm::{GbmAllocator, GbmBufferFlags, GbmDevice},
    },
    egl::{EGLContext, EGLDevice, EGLDisplay},
    input::{Axis, AxisSource, ButtonState, KeyState},
    renderer::{
      Bind, Color32F, Frame, ImportDma, Renderer,
      element::{
        Kind,
        surface::{WaylandSurfaceRenderElement, render_elements_from_surface_tree},
      },
      gles::GlesRenderer,
      utils::{
        RendererSurfaceStateUserData, draw_render_elements, on_commit_buffer_handler,
        with_renderer_surface_state,
      },
    },
  },
  input::{
    Seat, SeatHandler, SeatState,
    keyboard::FilterResult,
    pointer::{AxisFrame, ButtonEvent, CursorImageStatus, MotionEvent},
  },
  output::{Mode, Output, PhysicalProperties, Scale, Subpixel},
  reexports::wayland_protocols_wlr::layer_shell::v1::server::zwlr_layer_surface_v1::ZwlrLayerSurfaceV1,
  reexports::wayland_server::{
    Client, Display, ListeningSocket, Resource, Weak,
    backend::{ClientData, ClientId, DisconnectReason},
    protocol::{wl_buffer::WlBuffer, wl_output::WlOutput, wl_seat::WlSeat, wl_surface::WlSurface},
  },
  utils::{Rectangle, SERIAL_COUNTER, Serial, Transform},
  wayland::{
    buffer::BufferHandler,
    compositor::{
      CompositorClientState, CompositorHandler, CompositorState, SurfaceAttributes,
      TraversalAction, add_pre_commit_hook, get_parent, send_surface_state, with_states,
      with_surface_tree_downward,
    },
    dmabuf::{DmabufFeedbackBuilder, DmabufGlobal, DmabufHandler, DmabufState, ImportNotifier},
    output::{OutputHandler, OutputManagerState},
    shell::{
      wlr_layer::{
        Anchor, KeyboardInteractivity, Layer, LayerSurface, LayerSurfaceCachedState,
        WlrLayerShellHandler, WlrLayerShellState,
      },
      xdg::{
        PopupSurface, PositionerState, SurfaceCachedState, ToplevelSurface, XdgPopupSurfaceData,
        XdgShellHandler, XdgShellState,
      },
    },
    shm::{ShmHandler, ShmState},
  },
};
use std::{
  cell::RefCell,
  fs::File,
  os::unix::fs::MetadataExt,
  path::{Path, PathBuf},
  sync::Arc,
};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

struct LayerRole(RefCell<Weak<ZwlrLayerSurfaceV1>>);

/// One private compositor in the already resource-limited controller process.
/// Worker buffers are never forwarded to Qt: only completed controller output.
pub struct Graphics {
  display: Display<App>,
  socket: ListeningSocket,
  app: App,
  buffers: [Dmabuf; 2],
  allocator: GbmAllocator<File>,
  generation: u64,
  requested: Option<Viewport>,
  clients: Vec<Client>,
  node: PathBuf,
  frames: Frames,
  serial: u64,
  last_mask: Vec<Region>,
  pointer: smithay::input::pointer::PointerHandle<App>,
  keyboard: smithay::input::keyboard::KeyboardHandle<App>,
  buttons: u8,
}

impl Graphics {
  pub fn new(path: &Path, viewport: Viewport) -> Result<Self> {
    let pixels = viewport.pixels()?;
    let (device, node) = EGLDevice::enumerate()?
      .find_map(|device| device.render_device_path().ok().map(|path| (device, path)))
      .ok_or("no EGL render device")?;
    let egl = unsafe { EGLDisplay::new(device)? };
    let renderer = unsafe { GlesRenderer::new(EGLContext::new(&egl)?)? };
    let display = Display::<App>::new()?;
    let dh = display.handle();
    let mut dmabuf = DmabufState::new();
    let feedback =
      DmabufFeedbackBuilder::new(node.metadata()?.rdev(), renderer.dmabuf_formats()).build()?;
    dmabuf.create_global_with_default_feedback::<App>(&dh, &feedback);
    let mut seats = SeatState::new();
    let mut seat = seats.new_wl_seat(&dh, "plugin");
    let keyboard = seat.add_keyboard(Default::default(), 200, 25)?;
    let pointer = seat.add_pointer();
    let output = Output::new(
      "plugin".into(),
      PhysicalProperties {
        size: (0, 0).into(),
        subpixel: Subpixel::Unknown,
        make: "Omarchy".into(),
        model: "Private".into(),
      },
    );
    output.create_global::<App>(&dh);
    let mode = Mode {
      size: (pixels.0 as i32, pixels.1 as i32).into(),
      refresh: 60000,
    };
    output.change_current_state(
      Some(mode),
      Some(Transform::Normal),
      Some(Scale::Integer(viewport.scale as i32)),
      Some((0, 0).into()),
    );
    output.set_preferred(mode);
    OutputManagerState::new_with_xdg_output::<App>(&dh);
    let app = App {
      compositor: CompositorState::new_v6::<App>(&dh),
      xdg: XdgShellState::new::<App>(&dh),
      layer: WlrLayerShellState::new::<App>(&dh),
      shm: ShmState::new::<App>(&dh, vec![]),
      dmabuf,
      seats,
      renderer,
      output,
      viewport,
      layers: Vec::new(),
      popups: Vec::new(),
      surfaces: 0,
      failed: false,
      dirty: true,
    };
    let socket = ListeningSocket::bind_absolute(path.into())?;
    let gbm = GbmDevice::new(File::options().read(true).write(true).open(&node)?)?;
    let mut allocator = GbmAllocator::new(gbm, GbmBufferFlags::RENDERING);
    let buffers = allocate(&mut allocator, pixels)?;
    Ok(Self {
      display,
      socket,
      app,
      buffers,
      allocator,
      generation: 1,
      requested: None,
      clients: Vec::new(),
      node,
      frames: Frames::default(),
      serial: 0,
      last_mask: Vec::new(),
      pointer,
      keyboard,
      buttons: 0,
    })
  }

  pub fn render_node(&self) -> &Path {
    &self.node
  }

  pub fn describe(&mut self, channel: &Channel) -> Result<()> {
    self.frames.configure(self.generation)?;
    Event::Configured {
      generation: self.generation,
      viewport: self.app.viewport,
    }
    .send(channel)?;
    for (slot, buffer) in self.buffers.iter().enumerate() {
      if buffer.format().modifier != Modifier::Linear
        || buffer.format().code != Fourcc::Argb8888
        || buffer.num_planes() != 1
        || buffer.offsets().next() != Some(0)
      {
        return Err("unsupported compositor output buffer".into());
      }
      Event::Buffer(presentation::Buffer {
        generation: self.generation,
        slot: slot as u32,
        width: buffer.size().w as u32,
        height: buffer.size().h as u32,
        stride: buffer.strides().next().ok_or("missing stride")?,
        fd: buffer
          .handles()
          .next()
          .ok_or("missing output descriptor")?
          .try_clone_to_owned()?,
      })
      .send(channel)?;
      self.frames.describe(self.generation, slot as u32)?;
    }
    Ok(())
  }

  pub fn dispatch(&mut self) -> Result<()> {
    self
      .clients
      .retain(|client| client.get_credentials(&self.display.handle()).is_ok());
    while let Some(stream) = self.socket.accept()? {
      if self.clients.len() >= 8 {
        return Err("too many private display clients".into());
      }
      self.clients.push(
        self
          .display
          .handle()
          .insert_client(stream, Arc::new(ClientState::default()))?,
      );
    }
    self.display.dispatch_clients(&mut self.app)?;
    if self.app.failed {
      return Err("private surface limits exceeded".into());
    }
    self.app.layers.retain(LayerSurface::alive);
    self
      .app
      .popups
      .retain(|surface| surface.wl_surface().is_alive());
    self.display.flush_clients()?;
    Ok(())
  }

  pub fn presented(&mut self, serial: u64) -> Result<()> {
    Ok(self.frames.presented(serial)?)
  }

  pub fn configure(&mut self, viewport: Viewport, time: u32) -> Result<()> {
    viewport.pixels()?;
    self.requested = Some(viewport);
    // Geometry changes cancel private grabs, pressed input, and popup focus.
    // The host must resume input only after the new canvas has been presented.
    self.input(5, 0, 0, 0, time)
  }

  pub fn render(&mut self, channel: &Channel, time: u32) -> Result<()> {
    let Some(mut slot) = self.frames.writable_slot() else {
      return Ok(());
    };
    if let Some(viewport) = self.requested.take() {
      // A pending old frame prevents entry here. Qt retains its imported old
      // buffers until it switches nodes; none are reused as new output storage.
      self.buffers = allocate(&mut self.allocator, viewport.pixels()?)?;
      self.generation = self
        .generation
        .checked_add(1)
        .ok_or("viewport generation exhausted")?;
      self.app.viewport = viewport;
      let pixels = viewport.pixels()?;
      let mode = Mode {
        size: (pixels.0 as i32, pixels.1 as i32).into(),
        refresh: 60000,
      };
      if let Some(old) = self.app.output.current_mode() {
        self.app.output.delete_mode(old);
      }
      self.app.output.set_preferred(mode);
      self.app.output.change_current_state(
        Some(mode),
        None,
        Some(Scale::Integer(viewport.scale as i32)),
        None,
      );
      for (surface, _) in self.app.roots() {
        with_surface_tree_downward(
          &surface,
          (),
          |_, _, _| TraversalAction::DoChildren(()),
          |surface, states, _| {
            send_surface_state(surface, states, viewport.scale as i32, Transform::Normal)
          },
          |_, _, _| true,
        );
      }
      for layer in &self.app.layers {
        let rect = self.app.layer_rect(layer);
        layer.with_pending_state(|state| state.size = Some(rect.size));
        layer.send_pending_configure();
      }
      self.last_mask.clear();
      self.app.dirty = true;
      self.describe(channel)?;
      slot = self
        .frames
        .writable_slot()
        .ok_or("new viewport buffers unavailable")?;
    }
    if !self.app.dirty {
      return Ok(());
    }
    let surfaces = self.app.roots();
    let scale = self.app.viewport.scale as i32;
    let pixels = self.app.viewport.pixels()?;
    let size = (pixels.0 as i32, pixels.1 as i32);
    let elements = surfaces
      .iter()
      .flat_map(|(surface, pos)| {
        render_elements_from_surface_tree(
          &mut self.app.renderer,
          surface,
          (pos.0 * scale, pos.1 * scale),
          scale as f64,
          1.0,
          Kind::Unspecified,
        )
      })
      .collect::<Vec<WaylandSurfaceRenderElement<GlesRenderer>>>();
    let mut target = self.app.renderer.bind(&mut self.buffers[slot as usize])?;
    let damage = Rectangle::from_size(size.into());
    let mut frame = self
      .app
      .renderer
      .render(&mut target, size.into(), Transform::Normal)?;
    frame.clear(Color32F::new(0.0, 0.0, 0.0, 0.0), &[damage])?;
    draw_render_elements(&mut frame, scale as f64, &elements, &[damage])?;
    // This wait occurs only in the supervised per-plugin process. A stuck
    // worker fence cannot block the trusted shell's GUI/render thread.
    frame.finish()?.wait()?;
    let mask = self.app.mask(&surfaces)?;
    if mask != self.last_mask {
      Event::Mask {
        generation: self.generation,
        regions: mask.clone(),
      }
      .send(channel)?;
      self.last_mask = mask;
    }
    self.serial = self.serial.checked_add(1).ok_or("frame serial exhausted")?;
    self.frames.frame(self.generation, self.serial, slot)?;
    Event::Frame {
      generation: self.generation,
      serial: self.serial,
      slot,
    }
    .send(channel)?;
    self.app.dirty = false;
    for (surface, _) in surfaces {
      with_surface_tree_downward(
        &surface,
        (),
        |_, _, _| TraversalAction::DoChildren(()),
        |_, states, _| {
          for callback in states
            .cached_state
            .get::<SurfaceAttributes>()
            .current()
            .frame_callbacks
            .drain(..)
          {
            callback.done(time);
          }
        },
        |_, _, _| true,
      );
    }
    self.display.flush_clients()?;
    Ok(())
  }

  pub fn scroll(&mut self, scroll: Scroll, time: u32) -> Result<()> {
    scroll.validate()?;
    // Re-hit-test at the event position; scrolling does not acquire keyboard focus.
    self.input(2, 0, scroll.x, scroll.y, time)?;
    let mut frame = AxisFrame::new(time).source(if scroll.source == 0 {
      AxisSource::Wheel
    } else {
      AxisSource::Finger
    });
    for (axis, delta) in [
      (Axis::Horizontal, scroll.horizontal),
      (Axis::Vertical, scroll.vertical),
    ] {
      if scroll.source == 2 {
        frame = frame.stop(axis);
      } else if delta != 0 {
        // Wayland axis direction is opposite to QWheelEvent. Keep sub-detent
        // precision via v120, with 15 logical units per detent for old clients.
        frame = frame.value(
          axis,
          -f64::from(delta) / if scroll.source == 0 { 8.0 } else { 1.0 },
        );
        if scroll.source == 0 {
          frame = frame.v120(axis, -delta);
        }
      }
    }
    self.pointer.axis(&mut self.app, frame);
    self.pointer.frame(&mut self.app);
    Ok(())
  }

  pub fn input(&mut self, kind: u32, code: u32, x: i32, y: i32, time: u32) -> Result<()> {
    if kind <= 2 {
      if x < 0
        || y < 0
        || x >= self.app.viewport.width as i32
        || y >= self.app.viewport.height as i32
        || (kind < 2 && !(0x110..=0x117).contains(&code))
        || (kind == 2 && code != 0)
      {
        return Err("invalid host pointer input".into());
      }
      let point = (x as f64, y as f64).into();
      let focus = self
        .app
        .roots()
        .iter()
        .find_map(|(surface, pos)| {
          smithay::desktop::utils::under_from_surface_tree(
            surface,
            point,
            *pos,
            smithay::desktop::WindowSurfaceType::ALL,
          )
        })
        .map(|(surface, pos)| (surface, pos.to_f64()));
      self.pointer.motion(
        &mut self.app,
        focus.clone(),
        &MotionEvent {
          location: point,
          serial: SERIAL_COUNTER.next_serial(),
          time,
        },
      );
      if kind < 2 {
        if kind == 0 {
          self.buttons |= 1 << (code - 0x110);
        } else {
          self.buttons &= !(1 << (code - 0x110));
        }
        if kind == 0
          && focus
            .as_ref()
            .is_none_or(|(surface, _)| self.app.accepts_keyboard(surface))
        {
          self.keyboard.set_focus(
            &mut self.app,
            focus.map(|(surface, _)| surface),
            SERIAL_COUNTER.next_serial(),
          );
        }
        self.pointer.button(
          &mut self.app,
          &ButtonEvent {
            serial: SERIAL_COUNTER.next_serial(),
            time,
            button: code,
            state: if kind == 0 {
              ButtonState::Pressed
            } else {
              ButtonState::Released
            },
          },
        );
      }
      self.pointer.frame(&mut self.app);
    } else if kind <= 4 && (8..=767).contains(&code) && x == 0 && y == 0 {
      self.keyboard.input::<(), _>(
        &mut self.app,
        code.into(),
        if kind == 3 {
          KeyState::Pressed
        } else {
          KeyState::Released
        },
        SERIAL_COUNTER.next_serial(),
        time,
        |_, _, _| FilterResult::Forward,
      );
    } else if kind == 5 && code == 0 && x == 0 && y == 0 {
      self
        .keyboard
        .set_focus(&mut self.app, None, SERIAL_COUNTER.next_serial());
      self.keyboard.unset_grab(&mut self.app);
      for key in self.keyboard.pressed_keys() {
        self.keyboard.input::<(), _>(
          &mut self.app,
          key,
          KeyState::Released,
          SERIAL_COUNTER.next_serial(),
          time,
          |_, _, _| FilterResult::Forward,
        );
      }
      self
        .pointer
        .unset_grab(&mut self.app, SERIAL_COUNTER.next_serial(), time);
      let location = self.pointer.current_location();
      self.pointer.motion(
        &mut self.app,
        None,
        &MotionEvent {
          location,
          serial: SERIAL_COUNTER.next_serial(),
          time,
        },
      );
      for button in 0..8 {
        if self.buttons & (1 << button) != 0 {
          self.pointer.button(
            &mut self.app,
            &ButtonEvent {
              serial: SERIAL_COUNTER.next_serial(),
              time,
              button: 0x110 + button,
              state: ButtonState::Released,
            },
          );
        }
      }
      self.buttons = 0;
      self.pointer.frame(&mut self.app);
      for popup in &self.app.popups {
        popup.send_popup_done();
      }
    } else {
      return Err("invalid host keyboard input".into());
    }
    Ok(())
  }
}

fn allocate(allocator: &mut GbmAllocator<File>, pixels: (u32, u32)) -> Result<[Dmabuf; 2]> {
  let mut buffer = || -> Result<Dmabuf> {
    Ok(
      allocator
        .create_buffer(pixels.0, pixels.1, Fourcc::Argb8888, &[Modifier::Linear])?
        .export()?,
    )
  };
  Ok([buffer()?, buffer()?])
}

struct App {
  compositor: CompositorState,
  xdg: XdgShellState,
  layer: WlrLayerShellState,
  shm: ShmState,
  dmabuf: DmabufState,
  seats: SeatState<Self>,
  renderer: GlesRenderer,
  output: Output,
  viewport: Viewport,
  layers: Vec<LayerSurface>,
  popups: Vec<PopupSurface>,
  surfaces: usize,
  failed: bool,
  dirty: bool,
}
impl App {
  fn accepts_keyboard(&self, surface: &WlSurface) -> bool {
    let mut root = surface.clone();
    while let Some(parent) = get_parent(&root) {
      root = parent;
    }
    self
      .layers
      .iter()
      .find(|layer| layer.wl_surface() == &root)
      .is_none_or(|layer| layer_state(layer).keyboard_interactivity != KeyboardInteractivity::None)
  }

  fn layer_rect(&self, surface: &LayerSurface) -> Rectangle<i32, smithay::utils::Logical> {
    layer_geometry(self.viewport, layer_state(surface))
  }
  fn roots(&self) -> Vec<(WlSurface, (i32, i32))> {
    let mut roots = Vec::new();
    // Back-to-front: private background/bottom, windows, top/overlay. A layer
    // choice never changes the trusted host window's actual desktop layer.
    for layer in [Layer::Background, Layer::Bottom] {
      self.layer_roots(&mut roots, layer);
    }
    for top in self.xdg.toplevel_surfaces() {
      let bbox = smithay::desktop::utils::bbox_from_surface_tree(top.wl_surface(), (0, 0));
      self.append_root(
        &mut roots,
        top.wl_surface(),
        (
          ((self.viewport.width as i32 - bbox.size.w) / 2)
            .max(0)
            .saturating_sub(bbox.loc.x),
          ((self.viewport.height as i32 - bbox.size.h) / 2)
            .max(0)
            .saturating_sub(bbox.loc.y),
        ),
      );
    }
    for layer in [Layer::Top, Layer::Overlay] {
      self.layer_roots(&mut roots, layer);
    }
    roots.reverse();
    roots
  }
  fn layer_roots(&self, roots: &mut Vec<(WlSurface, (i32, i32))>, level: Layer) {
    for layer in self
      .layers
      .iter()
      .filter(|layer| layer_state(layer).layer == level)
    {
      let rect = self.layer_rect(layer);
      self.append_root(roots, layer.wl_surface(), (rect.loc.x, rect.loc.y));
    }
  }
  fn append_root(
    &self,
    roots: &mut Vec<(WlSurface, (i32, i32))>,
    surface: &WlSurface,
    pos: (i32, i32),
  ) {
    // Single-parent popup trees are bounded by the surface limits. Also avoid
    // revisiting an object, even if malformed protocol state reaches this path.
    if roots.iter().any(|(seen, _)| seen == surface) {
      return;
    }
    let pos = (pos.0.clamp(-8192, 8192), pos.1.clamp(-8192, 8192));
    roots.push((surface.clone(), pos));
    let origin = window_origin(surface);
    for popup in &self.popups {
      if popup.get_parent_surface().as_ref() == Some(surface) {
        let location = with_states(popup.wl_surface(), |states| {
          states
            .data_map
            .get::<XdgPopupSurfaceData>()
            .unwrap()
            .lock()
            .unwrap()
            .current
            .geometry
            .loc
        });
        let offset = window_origin(popup.wl_surface());
        self.append_root(
          roots,
          popup.wl_surface(),
          (
            pos
              .0
              .saturating_add(origin.0)
              .saturating_add(location.x)
              .saturating_sub(offset.0),
            pos
              .1
              .saturating_add(origin.1)
              .saturating_add(location.y)
              .saturating_sub(offset.1),
          ),
        );
      }
    }
  }
  fn configure_popup(
    &mut self,
    popup: &PopupSurface,
    positioner: PositionerState,
    token: Option<u32>,
  ) {
    let Some(parent) = popup.get_parent_surface() else {
      return;
    };
    let Some((_, pos)) = self
      .roots()
      .into_iter()
      .find(|(surface, _)| *surface == parent)
    else {
      self.failed = true;
      return;
    };
    let origin = window_origin(&parent);
    let target = Rectangle::new(
      (-pos.0 - origin.0, -pos.1 - origin.1).into(),
      (self.viewport.width as i32, self.viewport.height as i32).into(),
    );
    let Some(geometry) = popup_geometry(positioner, target) else {
      self.failed = true;
      return;
    };
    popup.with_pending_state(|state| {
      state.geometry = geometry;
      state.positioner = positioner;
    });
    if let Some(token) = token {
      popup.send_repositioned(token);
    } else if popup.send_configure().is_err() {
      self.failed = true;
    }
  }
  fn mask(&self, roots: &[(WlSurface, (i32, i32))]) -> Result<Vec<Region>> {
    let mut result = Ok(Vec::new());
    let viewport =
      Rectangle::from_size((self.viewport.width as i32, self.viewport.height as i32).into());
    for (surface, pos) in roots {
      // Match Smithay's hit testing: each mapped subsurface has its own local
      // input region, clipped to its own view, not the whole tree's bounding box.
      with_surface_tree_downward(
        surface,
        smithay::utils::Point::from(*pos),
        |_, states, parent: &smithay::utils::Point<i32, smithay::utils::Logical>| {
          let Ok(rows) = &mut result else {
            return TraversalAction::SkipChildren;
          };
          let view = states
            .data_map
            .get::<RendererSurfaceStateUserData>()
            .and_then(|data| data.lock().unwrap().view());
          let Some(view) = view else {
            return TraversalAction::SkipChildren;
          };
          let location = (
            parent.x.saturating_add(view.offset.x),
            parent.y.saturating_add(view.offset.y),
          )
            .into();
          let mut attributes = states.cached_state.get::<SurfaceAttributes>();
          let attributes = attributes.current();
          if let Err(error) = append_mask(
            rows,
            viewport,
            Rectangle::new(location, view.dst),
            attributes.input_region.as_ref(),
          ) {
            result = Err(error);
            TraversalAction::SkipChildren
          } else {
            TraversalAction::DoChildren(location)
          }
        },
        |_, _, _| {},
        |_, _, _| true,
      );
    }
    result
  }
}

fn layer_state(surface: &LayerSurface) -> LayerSurfaceCachedState {
  with_states(surface.wl_surface(), |states| {
    *states
      .cached_state
      .get::<LayerSurfaceCachedState>()
      .current()
  })
}

fn window_origin(surface: &WlSurface) -> (i32, i32) {
  with_states(surface, |states| {
    states
      .cached_state
      .get::<SurfaceCachedState>()
      .current()
      .geometry
      .map(|rect| (rect.loc.x.clamp(-8192, 8192), rect.loc.y.clamp(-8192, 8192)))
      .unwrap_or((0, 0))
  })
}

fn layer_geometry(
  viewport: Viewport,
  state: LayerSurfaceCachedState,
) -> Rectangle<i32, smithay::utils::Logical> {
  fn axis(
    length: u32,
    requested: i32,
    start: bool,
    end: bool,
    before: i32,
    after: i32,
  ) -> (i32, i32) {
    let length = i64::from(length);
    let before = if start {
      i64::from(before).clamp(-length, length)
    } else {
      0
    };
    let after = if end {
      i64::from(after).clamp(-length, length)
    } else {
      0
    };
    let available = (length - before - after).clamp(1, length);
    let size = if requested == 0 {
      available
    } else {
      i64::from(requested).clamp(1, available)
    };
    let position = match (start, end) {
      (true, false) => before,
      (false, true) => length - after - size,
      _ => (length + before - after - size) / 2,
    }
    .clamp(-length, length);
    (position as i32, size as i32)
  }
  let (x, width) = axis(
    viewport.width,
    state.size.w,
    state.anchor.contains(Anchor::LEFT),
    state.anchor.contains(Anchor::RIGHT),
    state.margin.left,
    state.margin.right,
  );
  let (y, height) = axis(
    viewport.height,
    state.size.h,
    state.anchor.contains(Anchor::TOP),
    state.anchor.contains(Anchor::BOTTOM),
    state.margin.top,
    state.margin.bottom,
  );
  Rectangle::new((x, y).into(), (width, height).into())
}

fn popup_geometry(
  positioner: PositionerState,
  target: Rectangle<i32, smithay::utils::Logical>,
) -> Option<Rectangle<i32, smithay::utils::Logical>> {
  // Bound arithmetic before calling the stock positioner implementation. Buffer
  // admission independently enforces physical dimensions and the pixel budget.
  if [
    positioner.rect_size.w,
    positioner.rect_size.h,
    positioner.anchor_rect.size.w,
    positioner.anchor_rect.size.h,
  ]
  .iter()
  .any(|value| !(1..=4096).contains(value))
    || [
      positioner.anchor_rect.loc.x,
      positioner.anchor_rect.loc.y,
      positioner.offset.x,
      positioner.offset.y,
    ]
    .iter()
    .any(|value| !(-8192..=8192).contains(value))
  {
    return None;
  }
  Some(positioner.get_unconstrained_geometry(target))
}

fn append_mask(
  result: &mut Vec<Region>,
  viewport: Rectangle<i32, smithay::utils::Logical>,
  view: Rectangle<i32, smithay::utils::Logical>,
  input: Option<&smithay::wayland::compositor::RegionAttributes>,
) -> Result<()> {
  let Some(clip) = view.intersection(viewport) else {
    return Ok(());
  };
  let mut add = |operation, rect: Rectangle<i32, smithay::utils::Logical>| -> Result<()> {
    if let Some(rect) = rect.intersection(clip) {
      if result.len() == presentation::MAX_REGIONS {
        return Err("too many input regions".into());
      }
      result.push(Region {
        operation,
        x: rect.loc.x as u32,
        y: rect.loc.y as u32,
        width: rect.size.w as u32,
        height: rect.size.h as u32,
      });
    }
    Ok(())
  };
  add(0, clip)?;
  if let Some(input) = input {
    if input.rects.len() > presentation::MAX_REGIONS {
      return Err("too many surface input regions".into());
    }
    for (kind, rect) in &input.rects {
      let mut rect = *rect;
      rect.loc.x = rect.loc.x.saturating_add(view.loc.x);
      rect.loc.y = rect.loc.y.saturating_add(view.loc.y);
      add(
        if matches!(kind, smithay::wayland::compositor::RectangleKind::Add) {
          1
        } else {
          2
        },
        rect,
      )?;
    }
  } else {
    add(1, clip)?;
  }
  Ok(())
}

#[cfg(test)]
mod tests {
  use super::*;
  use smithay::wayland::compositor::{RectangleKind, RegionAttributes};

  #[test]
  fn private_layer_geometry_respects_anchors_and_bounds_extreme_margins() {
    use smithay::wayland::shell::wlr_layer::Margins;
    let viewport = Viewport {
      width: 400,
      height: 300,
      scale: 1,
    };
    let mut state = LayerSurfaceCachedState {
      anchor: Anchor::TOP | Anchor::LEFT | Anchor::RIGHT,
      size: (0, 36).into(),
      margin: Margins {
        top: 8,
        left: 16,
        right: 24,
        bottom: 0,
      },
      ..Default::default()
    };
    assert_eq!(
      layer_geometry(viewport, state),
      Rectangle::new((16, 8).into(), (360, 36).into())
    );
    state.size.w = 100;
    assert_eq!(layer_geometry(viewport, state).loc.x, 146);
    state.anchor = Anchor::RIGHT | Anchor::BOTTOM;
    state.size = (80, 40).into();
    state.margin.right = 20;
    state.margin.bottom = 30;
    assert_eq!(
      layer_geometry(viewport, state),
      Rectangle::new((300, 230).into(), (80, 40).into())
    );
    state.anchor = Anchor::empty();
    assert_eq!(layer_geometry(viewport, state).loc, (160, 130).into());
    state.anchor = Anchor::LEFT;
    state.margin.left = -10;
    assert_eq!(layer_geometry(viewport, state).loc.x, -10);
    for length in [1, 4096] {
      for value in [i32::MIN, -1, 0, 1, i32::MAX] {
        state.anchor = Anchor::all();
        state.margin = Margins {
          top: value,
          bottom: value,
          left: value,
          right: value,
        };
        state.size.w = value;
        state.size.h = value;
        let rect = layer_geometry(
          Viewport {
            width: length,
            height: length,
            scale: 1,
          },
          state,
        );
        assert!((1..=length as i32).contains(&rect.size.w));
        assert!((1..=length as i32).contains(&rect.size.h));
        assert!((-i64::from(length)..=i64::from(length)).contains(&i64::from(rect.loc.x)));
        assert!((-i64::from(length)..=i64::from(length)).contains(&i64::from(rect.loc.y)));
      }
    }
  }

  #[test]
  fn private_popup_constraints_use_parent_coordinates_and_bounded_inputs() {
    use smithay::reexports::wayland_protocols::xdg::shell::server::xdg_positioner::{
      Anchor as PopupAnchor, ConstraintAdjustment, Gravity,
    };
    let mut positioner = PositionerState {
      rect_size: (120, 80).into(),
      anchor_rect: Rectangle::new((390, 290).into(), (10, 10).into()),
      anchor_edges: PopupAnchor::BottomRight,
      gravity: Gravity::BottomRight,
      constraint_adjustment: ConstraintAdjustment::SlideX | ConstraintAdjustment::SlideY,
      ..Default::default()
    };
    let target = Rectangle::from_size((400, 300).into());
    assert_eq!(
      popup_geometry(positioner, target).unwrap().loc,
      (280, 220).into()
    );
    assert_eq!(
      popup_geometry(positioner, Rectangle::new((-100, -50).into(), target.size))
        .unwrap()
        .loc,
      (180, 170).into()
    );
    positioner.constraint_adjustment = ConstraintAdjustment::empty();
    assert_eq!(
      popup_geometry(positioner, target).unwrap().loc,
      (400, 300).into()
    );
    positioner.offset.x = i32::MAX;
    assert!(popup_geometry(positioner, target).is_none());
    positioner.offset.x = 0;
    positioner.rect_size.w = 0;
    assert!(popup_geometry(positioner, target).is_none());
  }

  #[test]
  fn resize_waits_for_old_frame_and_cancels_pressed_input() {
    if std::env::var("OMARCHY_TEST_GRAPHICS").as_deref() != Ok("1") {
      return;
    }
    let root = tempfile::tempdir().unwrap();
    let initial = Viewport {
      width: 64,
      height: 48,
      scale: 1,
    };
    let mut graphics = Graphics::new(&root.path().join("display"), initial).unwrap();
    let (producer, consumer) = Channel::pair().unwrap();
    graphics.describe(&producer).unwrap();
    graphics.render(&producer, 0).unwrap();
    graphics.input(3, 50, 0, 0, 1).unwrap();
    graphics.input(0, 0x110, 10, 10, 1).unwrap();
    assert!(!graphics.keyboard.pressed_keys().is_empty());
    assert_ne!(graphics.buttons, 0);
    let next = Viewport {
      width: 80,
      height: 60,
      scale: 2,
    };
    graphics.configure(next, 2).unwrap();
    assert!(graphics.keyboard.pressed_keys().is_empty());
    assert_eq!(graphics.buttons, 0);
    graphics.render(&producer, 2).unwrap();
    assert_eq!(graphics.generation, 1, "resize overtook a pending frame");
    assert_eq!(graphics.app.viewport, initial);
    graphics.presented(1).unwrap();
    graphics.render(&producer, 3).unwrap();
    assert_eq!(graphics.generation, 2);
    assert_eq!(graphics.app.viewport, next);
    assert_eq!(graphics.buffers[0].size(), (160, 120).into());
    assert_eq!(graphics.app.output.modes().len(), 1);
    while let Ok(packet) = consumer.receive() {
      Event::decode(packet).unwrap();
    }
    graphics.presented(2).unwrap();
    graphics.configure(initial, 4).unwrap();
    graphics.render(&producer, 4).unwrap();
    assert_eq!(graphics.generation, 3);
    assert_eq!(graphics.buffers[0].size(), (64, 48).into());
    assert_eq!(graphics.app.output.modes().len(), 1);
  }

  #[test]
  fn independent_surface_regions_preserve_holes_and_clip_to_each_view() {
    let viewport = Rectangle::from_size((64, 64).into());
    let parent = Rectangle::new((10, 10).into(), (40, 40).into());
    let child = Rectangle::new((20, 20).into(), (8, 8).into());
    let input = RegionAttributes {
      rects: vec![
        (
          RectangleKind::Add,
          Rectangle::new((-10, -10).into(), (80, 80).into()),
        ),
        (
          RectangleKind::Subtract,
          Rectangle::new((5, 5).into(), (30, 30).into()),
        ),
      ],
    };
    let mut mask = Vec::new();
    append_mask(&mut mask, viewport, parent, Some(&input)).unwrap();
    append_mask(&mut mask, viewport, child, None).unwrap();
    for y in 0..64 {
      for x in 0..64 {
        let point = smithay::utils::Point::from((x, y));
        let expected =
          (parent.contains(point) && input.contains(point - parent.loc)) || child.contains(point);
        let (mut result, mut current, mut clipped) = (false, false, false);
        for region in &mask {
          let inside = Rectangle::new(
            (region.x as i32, region.y as i32).into(),
            (region.width as i32, region.height as i32).into(),
          )
          .contains(point);
          match region.operation {
            0 => {
              result |= current && clipped;
              current = false;
              clipped = inside;
            }
            1 if inside => current = true,
            2 if inside => current = false,
            _ => (),
          }
        }
        assert_eq!(
          result || (current && clipped),
          expected,
          "mask mismatch at {x},{y}"
        );
      }
    }
    let too_many = RegionAttributes {
      rects: vec![(RectangleKind::Add, viewport); presentation::MAX_REGIONS + 1],
    };
    assert!(append_mask(&mut Vec::new(), viewport, parent, Some(&too_many)).is_err());
  }
}
impl BufferHandler for App {
  fn buffer_destroyed(&mut self, _: &WlBuffer) {}
}
impl ShmHandler for App {
  fn shm_state(&self) -> &ShmState {
    &self.shm
  }
}
impl OutputHandler for App {}
impl CompositorHandler for App {
  fn compositor_state(&mut self) -> &mut CompositorState {
    &mut self.compositor
  }
  fn client_compositor_state<'a>(&self, client: &'a Client) -> &'a CompositorClientState {
    &client
      .get_data::<ClientState>()
      .expect("controller-created private client")
      .0
  }
  fn new_surface(&mut self, surface: &WlSurface) {
    // Smithay 0.7 keeps its size-validation hook after role destruction:
    // https://github.com/Smithay/smithay/pull/2071. Register before that hook
    // and give ONLY a destroyed role inert dimensions. The wl_surface may
    // legally commit a null buffer after destroying its layer role (Qt does).
    // A replacement role resets these dimensions below, before client requests;
    // validation of every live role remains unchanged. Remove with upstream fix.
    add_pre_commit_hook::<Self, _>(surface, |_, _, surface| {
      with_states(surface, |states| {
        if states
          .data_map
          .get::<LayerRole>()
          .is_some_and(|role| role.0.borrow().upgrade().is_err())
        {
          states
            .cached_state
            .get::<LayerSurfaceCachedState>()
            .pending()
            .size = (1, 1).into();
        }
      });
    });
    self.surfaces += 1;
    if self.surfaces > 64 {
      self.failed = true;
    }
    with_states(surface, |states| {
      send_surface_state(
        surface,
        states,
        self.viewport.scale as i32,
        Transform::Normal,
      )
    });
  }
  fn destroyed(&mut self, _: &WlSurface) {
    self.surfaces = self.surfaces.saturating_sub(1);
    self.dirty = true;
  }
  fn commit(&mut self, surface: &WlSurface) {
    with_states(surface, |states| {
      send_surface_state(
        surface,
        states,
        self.viewport.scale as i32,
        Transform::Normal,
      )
    });
    on_commit_buffer_handler::<Self>(surface);
    self.output.enter(surface);
    self.dirty = true;
    let too_large = with_renderer_surface_state(surface, |state| {
      state
        .buffer()
        .and_then(|buffer| smithay::backend::renderer::buffer_dimensions(buffer))
        .is_some_and(|size| {
          size.w <= 0
            || size.h <= 0
            || size.w > 8192
            || size.h > 8192
            || i64::from(size.w) * i64::from(size.h) > 8_388_608
        })
    })
    .unwrap_or(false);
    if too_large {
      self.failed = true;
    }
    for layer in &self.layers {
      if layer.wl_surface() == surface {
        let rect = self.layer_rect(layer);
        layer.with_pending_state(|state| state.size = Some(rect.size));
        layer.send_pending_configure();
      }
    }
  }
}
impl SeatHandler for App {
  type KeyboardFocus = WlSurface;
  type PointerFocus = WlSurface;
  type TouchFocus = WlSurface;
  fn seat_state(&mut self) -> &mut SeatState<Self> {
    &mut self.seats
  }
  fn focus_changed(&mut self, _: &Seat<Self>, _: Option<&WlSurface>) {}
  fn cursor_image(&mut self, _: &Seat<Self>, _: CursorImageStatus) {}
}
impl XdgShellHandler for App {
  fn xdg_shell_state(&mut self) -> &mut XdgShellState {
    &mut self.xdg
  }
  fn new_toplevel(&mut self, surface: ToplevelSurface) {
    surface.send_configure();
  }
  fn new_popup(&mut self, surface: PopupSurface, positioner: PositionerState) {
    if self.popups.len() >= 16 {
      self.failed = true;
      return;
    }
    surface.with_pending_state(|state| state.positioner = positioner);
    self.configure_popup(&surface, positioner, None);
    self.popups.push(surface);
  }
  fn grab(&mut self, _: PopupSurface, _: WlSeat, _: Serial) {}
  fn reposition_request(&mut self, surface: PopupSurface, positioner: PositionerState, token: u32) {
    self.configure_popup(&surface, positioner, Some(token));
  }
}
impl WlrLayerShellHandler for App {
  fn shell_state(&mut self) -> &mut WlrLayerShellState {
    &mut self.layer
  }
  fn new_layer_surface(&mut self, surface: LayerSurface, _: Option<WlOutput>, _: Layer, _: String) {
    with_states(surface.wl_surface(), |states| {
      if let Some(role) = states.data_map.get::<LayerRole>() {
        states
          .cached_state
          .get::<LayerSurfaceCachedState>()
          .pending()
          .size = (0, 0).into();
        *role.0.borrow_mut() = surface.shell_surface().downgrade();
      } else {
        states
          .data_map
          .insert_if_missing(|| LayerRole(RefCell::new(surface.shell_surface().downgrade())));
      }
    });
    if self.layers.len() >= 16 {
      self.failed = true;
      return;
    }
    self.layers.push(surface);
  }
  fn new_popup(&mut self, _: LayerSurface, popup: PopupSurface) {
    let positioner = popup.with_pending_state(|state| state.positioner);
    self.configure_popup(&popup, positioner, None);
  }
  fn layer_destroyed(&mut self, _: LayerSurface) {
    self.layers.retain(LayerSurface::alive);
    self.dirty = true;
  }
}
impl DmabufHandler for App {
  fn dmabuf_state(&mut self) -> &mut DmabufState {
    &mut self.dmabuf
  }
  fn dmabuf_imported(&mut self, _: &DmabufGlobal, buffer: Dmabuf, notifier: ImportNotifier) {
    let size = buffer.size();
    if size.w > 0
      && size.h > 0
      && size.w <= 8192
      && size.h <= 8192
      && i64::from(size.w) * i64::from(size.h) <= 8_388_608
      && self.renderer.import_dmabuf(&buffer, None).is_ok()
    {
      let _ = notifier.successful::<Self>();
    } else {
      notifier.failed();
    }
  }
}
#[derive(Default)]
struct ClientState(CompositorClientState);
impl ClientData for ClientState {
  fn initialized(&self, _: ClientId) {}
  fn disconnected(&self, _: ClientId, _: DisconnectReason) {}
}
smithay::delegate_compositor!(App);
smithay::delegate_xdg_shell!(App);
smithay::delegate_layer_shell!(App);
smithay::delegate_shm!(App);
smithay::delegate_dmabuf!(App);
smithay::delegate_seat!(App);
smithay::delegate_output!(App);
