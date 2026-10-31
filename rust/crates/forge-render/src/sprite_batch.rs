//! The list of sprites to draw this frame, as `SpriteBatch` in engine/render. Game code (any number of threads at
//! once) appends sprites; `finish` copies them into upload memory and, when asked, sorts the ones on screen into draw
//! order. The sort produces a list of indices, so the 32-byte sprites themselves are only ever copied in order.
//!
//! Positions are in tiles relative to an origin chosen at `begin` (normally the camera), so floats stay precise
//! anywhere in a huge world.

use rayon::prelude::*;
use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicU32, Ordering};

/// Exactly what the GPU reads per sprite: 32 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, bytemuck::Pod, bytemuck::Zeroable)]
pub struct Sprite {
    /// Centre, tiles from the batch origin.
    pub x: f32,
    pub y: f32,
    /// Size in tiles; negative w mirrors the picture.
    pub w: f32,
    pub h: f32,
    /// Radians, clockwise on screen.
    pub angle: f32,
    /// Picture in the sprite sheet.
    pub frame: u32,
    /// Tint, RGBA8 (see `pack_color`); alpha fades the sprite.
    pub color: u32,
    /// Draw order: lower first (see `draw_order`).
    pub order: u32,
}

impl Default for Sprite {
    fn default() -> Self {
        Sprite { x: 0.0, y: 0.0, w: 1.0, h: 1.0, angle: 0.0, frame: 0, color: 0xffff_ffff, order: 0 }
    }
}

const _: () = assert!(std::mem::size_of::<Sprite>() == 32);

pub fn pack_color(r: u8, g: u8, b: u8, a: u8) -> u32 {
    r as u32 | (g as u32) << 8 | (b as u32) << 16 | (a as u32) << 24
}

/// Order for top-down games: by layer first, then by the sprite's feet, so objects lower on screen are drawn over the
/// ones behind them. `y_from_top` is in tiles from the top of the view (0 .. view height). Only the low 24 bits are
/// used.
pub fn draw_order(layer: u8, y_from_top: f32, view_height: f32) -> u32 {
    let t = if view_height > 0.0 { y_from_top / view_height } else { 0.0 };
    let clamped = t.clamp(0.0, 1.0);
    (layer as u32) << 16 | (clamped * 65535.0) as u32
}

/// A rectangle in tiles relative to the batch origin.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ViewBox {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SpriteBatchStats {
    /// Pushed this frame.
    pub submitted: u32,
    /// Instances the GPU will draw.
    pub drawn: u32,
    /// Did not fit into the capacity.
    pub dropped: u32,
}

/// What `finish` wrote.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SpriteList {
    /// Copied to `out_sprites`, in push order.
    pub sprites: u32,
    /// Sorted: entries in `out_indices`; unsorted: the same as `sprites`.
    pub draws: u32,
}

const BLOCK: usize = 8192;

fn visible(s: &Sprite, v: &ViewBox) -> bool {
    // Radius that covers the sprite at any angle.
    let r = (s.w.abs() + s.h.abs()) * 0.5;
    s.x + r >= v.x0 && s.x - r <= v.x1 && s.y + r >= v.y0 && s.y - r <= v.y1
}

#[derive(Default)]
pub struct SpriteBatch {
    items: Box<[UnsafeCell<Sprite>]>,
    count: AtomicU32,
    origin: (f64, f64),
    keys: Vec<u64>,
    stats: SpriteBatchStats,
}

// `push` hands every caller its own range of `items` (an atomic counter), and everything else takes `&mut self`.
unsafe impl Sync for SpriteBatch {}

impl SpriteBatch {
    /// Starts a frame. `capacity` is the most sprites that can be pushed.
    pub fn begin(&mut self, origin_x: f64, origin_y: f64, capacity: u32) {
        self.origin = (origin_x, origin_y);
        if capacity as usize > self.items.len() {
            self.items = (0..capacity).map(|_| UnsafeCell::new(Sprite::default())).collect();
        }
        *self.count.get_mut() = 0;
        self.stats = SpriteBatchStats::default();
    }

    pub fn origin(&self) -> (f64, f64) {
        self.origin
    }

    /// Room for `count` sprites, safe from any thread; `None` when the batch is full (those sprites are counted as
    /// dropped).
    #[allow(clippy::mut_from_ref)]
    pub fn push_many(&self, count: u32) -> Option<&mut [Sprite]> {
        let at = self.count.fetch_add(count, Ordering::Relaxed);
        let end = at.checked_add(count)?;
        if end as usize > self.items.len() {
            return None;
        }
        // SAFETY: the counter gave this caller [at, end) alone until the next `begin` or `finish`, which take
        // `&mut self`; `UnsafeCell<Sprite>` has the layout of `Sprite`.
        unsafe {
            let first = self.items[at as usize].get();
            Some(std::slice::from_raw_parts_mut(first, count as usize))
        }
    }

    pub fn push(&self, sprite: Sprite) {
        if let Some(s) = self.push_many(1) {
            s[0] = sprite;
        }
    }

    pub fn len(&self) -> u32 {
        self.count.load(Ordering::Relaxed).min(self.items.len() as u32)
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn stats(&self) -> SpriteBatchStats {
        self.stats
    }

    /// Copies the sprites to `out_sprites` (at most its length). When sorted, also writes to `out_indices` the
    /// sprites that touch the view, in draw order (sprites with the same order keep the order they were pushed in).
    /// Unsorted, every sprite is drawn in push order and off-screen ones are left to the GPU to clip.
    pub fn finish(
        &mut self,
        view: &ViewBox,
        sorted: bool,
        out_sprites: &mut [Sprite],
        out_indices: &mut [u32],
    ) -> SpriteList {
        let pushed = *self.count.get_mut();
        let size = pushed.min(self.items.len() as u32);
        let n = size.min(out_sprites.len() as u32) as usize;
        self.stats.submitted = size;
        self.stats.dropped = pushed - n as u32;
        // SAFETY: `&mut self`: nobody else holds a pushed range now.
        let items: &[Sprite] = unsafe { std::slice::from_raw_parts(self.items.as_ptr() as *const Sprite, n) };
        out_sprites[..n]
            .par_chunks_mut(BLOCK)
            .zip(items.par_chunks(BLOCK))
            .for_each(|(to, from)| to.copy_from_slice(from));
        if !sorted {
            self.stats.drawn = n as u32;
            return SpriteList { sprites: n as u32, draws: n as u32 };
        }
        // (order, index) of the visible sprites, block by block, then sorted: the index makes every key unique, so
        // equal orders keep push order.
        let blocks: Vec<Vec<u64>> = items
            .par_chunks(BLOCK)
            .enumerate()
            .map(|(b, chunk)| {
                let base = b * BLOCK;
                chunk
                    .iter()
                    .enumerate()
                    .filter(|(_, s)| visible(s, view))
                    .map(|(i, s)| (s.order as u64) << 32 | (base + i) as u64)
                    .collect()
            })
            .collect();
        self.keys.clear();
        for b in &blocks {
            self.keys.extend_from_slice(b);
        }
        self.keys.par_sort_unstable();
        let total = self.keys.len().min(out_indices.len());
        out_indices[..total].par_iter_mut().zip(self.keys[..total].par_iter()).for_each(|(o, k)| *o = *k as u32);
        self.stats.drawn = total as u32;
        SpriteList { sprites: n as u32, draws: total as u32 }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn draw_order_is_layer_then_feet() {
        assert_eq!(draw_order(0, 0.0, 100.0), 0);
        assert_eq!(draw_order(1, 100.0, 100.0), 1 << 16 | 65535);
        assert_eq!(draw_order(2, 50.0, 100.0), 2 << 16 | 32767);
        assert_eq!(draw_order(0, -5.0, 100.0), 0);
        assert_eq!(draw_order(0, 5.0, 0.0), 0);
        assert_eq!(pack_color(1, 2, 3, 4), 0x0403_0201);
    }
}
