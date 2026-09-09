//! Validated presentation records shared by the controller and native Qt bridge.
use crate::channel::{Channel, Packet};
use std::{
  io,
  os::fd::{AsFd, OwnedFd},
};

const ARGB8888: u32 = 0x34325241;
const MAX_PIXELS: u64 = 8_388_608;
pub const MAX_REGIONS: usize = 128;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Viewport {
  pub width: u32,
  pub height: u32,
  pub scale: u32,
}
impl Viewport {
  pub fn pixels(self) -> io::Result<(u32, u32)> {
    if self.width == 0
      || self.height == 0
      || self.width > 4096
      || self.height > 4096
      || !(1..=4).contains(&self.scale)
    {
      return Err(invalid("invalid presentation viewport"));
    }
    let size = (self.width * self.scale, self.height * self.scale);
    validate_size(size.0, size.1)?;
    Ok(size)
  }
}

#[derive(Debug)]
pub struct Buffer {
  pub generation: u64,
  pub slot: u32,
  pub width: u32,
  pub height: u32,
  pub stride: u32,
  pub fd: OwnedFd,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Region {
  pub operation: u32,
  pub x: u32,
  pub y: u32,
  pub width: u32,
  pub height: u32,
}

#[derive(Debug)]
pub enum Event {
  Configured {
    generation: u64,
    viewport: Viewport,
  },
  Buffer(Buffer),
  Frame {
    generation: u64,
    serial: u64,
    slot: u32,
  },
  Mask {
    generation: u64,
    regions: Vec<Region>,
  },
}

impl Event {
  pub fn send(&self, channel: &Channel) -> io::Result<()> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(b"OPH\x01");
    let mut fds = Vec::new();
    match self {
      Self::Configured {
        generation,
        viewport,
      } => {
        viewport.pixels()?;
        if *generation == 0 {
          return Err(invalid("invalid viewport generation"));
        }
        put32(&mut bytes, 19);
        put64(&mut bytes, *generation);
        for value in [viewport.width, viewport.height, viewport.scale, 0] {
          put32(&mut bytes, value);
        }
      }
      Self::Buffer(buffer) => {
        validate_buffer(buffer)?;
        put32(&mut bytes, 16);
        put64(&mut bytes, buffer.generation);
        for value in [
          buffer.slot,
          buffer.width,
          buffer.height,
          ARGB8888,
          buffer.stride,
          0,
        ] {
          put32(&mut bytes, value);
        }
        put64(&mut bytes, 0); // Linear modifier; single plane, zero offset.
        fds.push(buffer.fd.as_fd());
      }
      Self::Frame {
        generation,
        serial,
        slot,
      } => {
        if *generation == 0 || *serial == 0 || *slot > 1 {
          return Err(invalid("invalid frame"));
        }
        put32(&mut bytes, 17);
        put64(&mut bytes, *generation);
        put64(&mut bytes, *serial);
        put32(&mut bytes, *slot);
        put32(&mut bytes, 0);
      }
      Self::Mask {
        generation,
        regions,
      } => {
        validate_regions(*generation, regions)?;
        put32(&mut bytes, 18);
        put64(&mut bytes, *generation);
        put32(&mut bytes, regions.len() as u32);
        for region in regions {
          for value in [
            region.operation,
            region.x,
            region.y,
            region.width,
            region.height,
          ] {
            put32(&mut bytes, value);
          }
        }
      }
    }
    channel.send(&bytes, &fds)
  }

  pub fn decode(mut packet: Packet) -> io::Result<Self> {
    let bytes = &packet.bytes;
    if bytes.len() < 16 || &bytes[..4] != b"OPH\x01" {
      return Err(invalid("invalid presentation record"));
    }
    let generation = get64(bytes, 8);
    if generation == 0 {
      return Err(invalid("invalid presentation generation"));
    }
    match get32(bytes, 4) {
      19 if bytes.len() == 32 && packet.fds.is_empty() && get32(bytes, 28) == 0 => {
        let viewport = Viewport {
          width: get32(bytes, 16),
          height: get32(bytes, 20),
          scale: get32(bytes, 24),
        };
        viewport.pixels()?;
        Ok(Self::Configured {
          generation,
          viewport,
        })
      }
      16 if bytes.len() == 48 && packet.fds.len() == 1 => {
        if get32(bytes, 28) != ARGB8888 || get32(bytes, 36) != 0 || get64(bytes, 40) != 0 {
          return Err(invalid("unsupported presentation buffer format"));
        }
        let buffer = Buffer {
          generation,
          slot: get32(bytes, 16),
          width: get32(bytes, 20),
          height: get32(bytes, 24),
          stride: get32(bytes, 32),
          fd: packet.fds.pop().unwrap(),
        };
        validate_buffer(&buffer)?;
        Ok(Self::Buffer(buffer))
      }
      17 if bytes.len() == 32 && packet.fds.is_empty() => {
        let serial = get64(bytes, 16);
        let slot = get32(bytes, 24);
        if serial == 0 || slot > 1 || get32(bytes, 28) != 0 {
          return Err(invalid("invalid presentation frame"));
        }
        Ok(Self::Frame {
          generation,
          serial,
          slot,
        })
      }
      18 if bytes.len() >= 20 && packet.fds.is_empty() => {
        let count = get32(bytes, 16) as usize;
        if count > MAX_REGIONS || bytes.len() != 20 + count * 20 {
          return Err(invalid("invalid presentation regions"));
        }
        let regions = bytes[20..]
          .chunks_exact(20)
          .map(|row| Region {
            operation: get32(row, 0),
            x: get32(row, 4),
            y: get32(row, 8),
            width: get32(row, 12),
            height: get32(row, 16),
          })
          .collect::<Vec<_>>();
        validate_regions(generation, &regions)?;
        Ok(Self::Mask {
          generation,
          regions,
        })
      }
      _ => Err(invalid("unknown presentation record or descriptor count")),
    }
  }
}

/// Track ownership independently of transport timing. The producer may write
/// only the non-displayed slot and must await acknowledgement before advancing.
#[derive(Default)]
pub struct Frames {
  generation: u64,
  described: u8,
  displayed: Option<u32>,
  pending: Option<(u64, u32)>,
  serial: u64,
}
impl Frames {
  pub fn configure(&mut self, generation: u64) -> io::Result<()> {
    if self.generation.checked_add(1) != Some(generation)
      || self.pending.is_some()
      || (self.generation != 0 && self.described != 3)
    {
      return Err(invalid("invalid presentation generation transition"));
    }
    self.generation = generation;
    self.described = 0;
    self.displayed = None;
    Ok(())
  }
  pub fn writable_slot(&self) -> Option<u32> {
    if self.described != 3 || self.pending.is_some() {
      None
    } else {
      Some(self.displayed.map_or(0, |slot| 1 - slot))
    }
  }
  pub fn describe(&mut self, generation: u64, slot: u32) -> io::Result<()> {
    if generation == 0 || slot > 1 || self.pending.is_some() {
      return Err(invalid("invalid buffer transition"));
    }
    if generation != self.generation || self.described & (1 << slot) != 0 {
      return Err(invalid("stale or duplicate buffer"));
    }
    self.described |= 1 << slot;
    Ok(())
  }
  pub fn frame(&mut self, generation: u64, serial: u64, slot: u32) -> io::Result<()> {
    if generation != self.generation
      || self.described != 3
      || serial <= self.serial
      || slot > 1
      || self.pending.is_some()
      || self.displayed == Some(slot)
    {
      return Err(invalid("invalid frame ownership transition"));
    }
    self.serial = serial;
    self.pending = Some((serial, slot));
    Ok(())
  }
  pub fn presented(&mut self, serial: u64) -> io::Result<()> {
    let Some((expected, slot)) = self.pending else {
      return Err(invalid("unsolicited presentation acknowledgement"));
    };
    if serial != expected {
      return Err(invalid("stale presentation acknowledgement"));
    }
    self.displayed = Some(slot);
    self.pending = None;
    Ok(())
  }
}

fn validate_size(width: u32, height: u32) -> io::Result<()> {
  if width == 0
    || height == 0
    || width > 8192
    || height > 8192
    || u64::from(width) * u64::from(height) > MAX_PIXELS
  {
    return Err(invalid("presentation dimensions exceed limits"));
  }
  Ok(())
}
fn validate_buffer(buffer: &Buffer) -> io::Result<()> {
  validate_size(buffer.width, buffer.height)?;
  if buffer.generation == 0
    || buffer.slot > 1
    || buffer.stride < buffer.width * 4
    || buffer.stride > buffer.width * 4 + 4096
    || !buffer.stride.is_multiple_of(4)
    || u64::from(buffer.stride) * u64::from(buffer.height) > 64 * 1024 * 1024
  {
    return Err(invalid("invalid presentation stride or slot"));
  }
  Ok(())
}
fn validate_regions(generation: u64, regions: &[Region]) -> io::Result<()> {
  if generation == 0 || regions.len() > MAX_REGIONS {
    return Err(invalid("too many presentation regions"));
  }
  for region in regions {
    if region.operation > 2
      || region.width == 0
      || region.height == 0
      || region.width > 4096
      || region.height > 4096
      || region.x > 4096 - region.width
      || region.y > 4096 - region.height
    {
      return Err(invalid("invalid presentation region"));
    }
  }
  Ok(())
}
fn put32(bytes: &mut Vec<u8>, value: u32) {
  bytes.extend_from_slice(&value.to_le_bytes());
}
fn put64(bytes: &mut Vec<u8>, value: u64) {
  bytes.extend_from_slice(&value.to_le_bytes());
}
fn get32(bytes: &[u8], at: usize) -> u32 {
  u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
}
fn get64(bytes: &[u8], at: usize) -> u64 {
  u64::from_le_bytes(bytes[at..at + 8].try_into().unwrap())
}
fn invalid(message: &str) -> io::Error {
  io::Error::new(io::ErrorKind::InvalidData, message)
}

#[cfg(test)]
mod tests {
  use super::*;
  #[test]
  fn frame_ownership_rejects_replays_and_writes_to_displayed_buffer() {
    let mut frames = Frames::default();
    assert!(frames.frame(1, 1, 0).is_err());
    assert!(frames.describe(1, 0).is_err());
    frames.configure(1).unwrap();
    frames.describe(1, 0).unwrap();
    assert!(frames.configure(2).is_err());
    frames.describe(1, 1).unwrap();
    assert!(frames.describe(1, 1).is_err());
    frames.frame(1, 1, 0).unwrap();
    assert!(frames.configure(2).is_err());
    assert!(frames.describe(2, 0).is_err());
    assert!(frames.frame(1, 2, 1).is_err());
    assert!(frames.presented(2).is_err());
    frames.presented(1).unwrap();
    assert!(frames.frame(1, 2, 0).is_err());
    frames.frame(1, 2, 1).unwrap();
    frames.presented(2).unwrap();
    assert!(frames.configure(1).is_err());
    assert!(frames.configure(3).is_err());
    frames.configure(2).unwrap();
    frames.describe(2, 0).unwrap();
    frames.describe(2, 1).unwrap();
    assert!(frames.frame(1, 3, 0).is_err());
    frames.frame(2, 3, 0).unwrap();
  }
  #[test]
  fn malformed_presentation_bytes_do_not_panic_or_allocate_from_lengths() {
    for length in 0..=4096 {
      let mut bytes = vec![255; length];
      if length >= 8 {
        bytes[..4].copy_from_slice(b"OPH\x01");
        bytes[4..8].copy_from_slice(&18u32.to_le_bytes());
      }
      assert!(
        Event::decode(Packet {
          bytes,
          fds: Vec::new()
        })
        .is_err()
      );
    }
    assert!(
      Viewport {
        width: 4096,
        height: 4096,
        scale: 4
      }
      .pixels()
      .is_err()
    );
    assert_eq!(
      Viewport {
        width: 800,
        height: 480,
        scale: 2
      }
      .pixels()
      .unwrap(),
      (1600, 960)
    );
  }
}
