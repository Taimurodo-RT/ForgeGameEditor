//! 2D light and shadow over a tile world, as engine/render/src/lighting.cpp. Every frame the visible tiles (and a
//! margin around them) become a grid of cells; tiles decide how much light passes through each cell, and light
//! sources (the sky, glowing tiles, lamps) are spread over the grid by the video card. Lamps with a radius are worked
//! out here, on the processor: their light ends where the radius says however bright they are.
//!
//! This file is the part without the video card: the grid (`LightGrid`) and the lamps with a radius (`lamp_light`),
//! the same numbers as the C++ version (tests/data/lamp_light.txt).

// `!(x > 0)` is meant: it is true for NaN too, as in the C++ version.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use crate::camera::Camera2D;
use forge_world::{CHUNK, CHUNK_TILES, World};
use rayon::prelude::*;
use std::cmp::Reverse;
use std::collections::BinaryHeap;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Color {
    pub r: f32,
    pub g: f32,
    pub b: f32,
    pub a: f32,
}

impl Color {
    pub const fn rgb(r: f32, g: f32, b: f32) -> Color {
        Color { r, g, b, a: 1.0 }
    }
}

/// How tiles take part in lighting.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum LightKind {
    /// Air, floors: light passes almost freely.
    #[default]
    Open = 0,
    /// Water, leaves, glass: dims light.
    Dense = 1,
    /// Rock, walls: light goes only a few tiles in.
    Solid = 2,
    /// Nothing gets through.
    Opaque = 3,
}

#[derive(Clone, Debug, PartialEq)]
pub struct LightRules {
    /// The layer whose tiles block light (usually the front layer).
    pub blocking_layer: u32,
    /// The kind of every tile id on that layer; ids past the end are open, and so is id 0 always.
    pub kinds: Vec<LightKind>,
    /// Light kept when passing one tile of each kind.
    pub transmit: [f32; 4],
    /// Sky light (side view): cells where every layer is empty glow with this.
    pub sky: bool,
    /// Tiles the sky shines through as if the cell were empty (by id, non-zero: yes; ids past the end: no), on the
    /// layers of the mask (bit per layer).
    pub sky_through: Vec<u8>,
    pub sky_through_layers: u32,
    pub sky_color: Color,
    /// Added everywhere after spreading (top down: daylight; night: low).
    pub ambient: Color,
    /// Tiles on the blocking layer that glow (lava, crystals): colour per id.
    pub glow: Vec<Color>,
}

impl Default for LightRules {
    fn default() -> Self {
        LightRules {
            blocking_layer: 1,
            kinds: Vec::new(),
            transmit: [0.93, 0.82, 0.72, 0.0],
            sky: false,
            sky_through: Vec::new(),
            sky_through_layers: 0,
            sky_color: Color::rgb(1.0, 1.0, 1.0),
            ambient: Color::rgb(0.0, 0.0, 0.0),
            glow: Vec::new(),
        }
    }
}

/// A lamp. Without a radius its colour channels above 1 shine further (up to 4): the light spreads as the sky's
/// does. With one, the colour is the light at the centre and the radius how far it goes (see `lamp_light`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PointLight {
    pub x: f64,
    pub y: f64,
    pub r: f32,
    pub g: f32,
    pub b: f32,
    /// Tiles, up to `MAX_LAMP_RADIUS`; 0: none.
    pub radius: f32,
}

impl PointLight {
    pub const fn new(x: f64, y: f64, r: f32, g: f32, b: f32, radius: f32) -> PointLight {
        PointLight { x, y, r, g, b, radius }
    }
}

/// How much world around the screen the light grid has, in tiles: what is there still lights the screen (and
/// unloaded chunks there are dark).
pub const LIGHT_MARGIN: i32 = 40;
/// The farthest a lamp with a radius reaches, in tiles.
pub const MAX_LAMP_RADIUS: f32 = LIGHT_MARGIN as f32;
pub const MAX_CELLS_W: u32 = 1024;
pub const MAX_CELLS_H: u32 = 640;
/// How far light spreads on the video card, in cells.
pub const STEPS: u32 = 40;

fn channel(v: f32) -> u32 {
    (v.clamp(0.0, 3.996) * 256.0) as u32
}

/// A light source as the spread reads a cell: 10 bits a channel (×256, up to 4), the kind in the top 2.
pub fn pack_source(r: f32, g: f32, b: f32, kind: LightKind) -> u32 {
    channel(r) | channel(g) << 10 | channel(b) << 20 | (kind as u32) << 30
}

/// How much of a lamp's light is left `d` tiles along its way: all of it at the centre, smoothly less, none at the
/// radius and beyond.
pub fn lamp_falloff(d: f64, radius: f64) -> f32 {
    if !(radius > 0.0) || !(d < radius) {
        return 0.0;
    }
    let q = if d > 0.0 { d / radius } else { 0.0 };
    let k = 1.0 - q * q;
    (k * k) as f32
}

const SQRT2: f64 = std::f64::consts::SQRT_2;
const FAR: f64 = f64::INFINITY;

/// The grid a lamp's light is worked out over.
struct LampGrid<'a> {
    cells: &'a [u32],
    w: u32,
    h: u32,
    x0: i32,
    y0: i32,
    step: u32,
    /// Tiles of air one tile of each kind counts for; `FAR`: none passes.
    cost: [f64; 4],
}

fn lamp_grid<'a>(cells: &'a [u32], w: u32, h: u32, x0: i32, y0: i32, step: u32, transmit: &[f32; 4]) -> LampGrid<'a> {
    let open = transmit[0] as f64;
    let mut cost = [1.0; 4];
    for (c, t) in cost.iter_mut().zip(transmit) {
        let t = *t as f64;
        *c = if !(t > 0.0) {
            FAR
        } else if open > 0.0 && open < 1.0 && t < open {
            t.ln() / open.ln()
        } else {
            1.0
        };
    }
    LampGrid { cells, w, h, x0, y0, step: step.max(1), cost }
}

/// One lamp's light over the cells around it.
#[derive(Default)]
struct LampBox {
    cx0: i32,
    cy0: i32,
    bw: u32,
    bh: u32,
    rgb: Vec<f32>,
    way: Vec<f64>,
}

/// A distance in the queue: never NaN, so ordered as `f64::total_cmp`, as `std::greater<std::pair<f64, u32>>`.
#[derive(Clone, Copy, PartialEq)]
struct Way(f64);

impl Eq for Way {}

impl PartialOrd for Way {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for Way {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        self.0.total_cmp(&other.0)
    }
}

fn one_lamp(g: &LampGrid, l: &PointLight, out: &mut LampBox) {
    out.bw = 0;
    out.bh = 0;
    let radius = (l.radius as f64).min(MAX_LAMP_RADIUS as f64);
    if !(radius > 0.0) || !l.x.is_finite() || !l.y.is_finite() {
        return;
    }
    let step = g.step as f64;
    let fx = (l.x - g.x0 as f64) / step;
    let fy = (l.y - g.y0 as f64) / step;
    if fx < 0.0 || fy < 0.0 || fx >= g.w as f64 || fy >= g.h as f64 {
        return; // off the grid: farther than the margin
    }
    let (lx, ly) = (fx as i32, fy as i32);
    let reach = (radius / step).ceil() as i32 + 1;
    out.cx0 = (lx - reach).max(0);
    out.cy0 = (ly - reach).max(0);
    let cx1 = (lx + reach).min(g.w as i32 - 1);
    let cy1 = (ly + reach).min(g.h as i32 - 1);
    out.bw = (cx1 - out.cx0 + 1) as u32;
    out.bh = (cy1 - out.cy0 + 1) as u32;
    let n = out.bw as usize * out.bh as usize;
    out.way.clear();
    out.way.resize(n, FAR);
    out.rgb.clear();
    out.rgb.resize(n * 3, 0.0);

    // The shortest way from the lamp's cell to every cell of the box, through its 8 neighbours; a cell costs by its
    // kind, a diagonal step √2.
    let mut open = BinaryHeap::new();
    let start = (ly - out.cy0) as u32 * out.bw + (lx - out.cx0) as u32;
    out.way[start as usize] = 0.0;
    open.push(Reverse((Way(0.0), start)));
    // A way longer than this cannot end inside the radius (see below).
    let longest = radius * 2.0 + step * 2.0;
    while let Some(Reverse((Way(d), i))) = open.pop() {
        if d > out.way[i as usize] {
            continue;
        }
        let bx = (i % out.bw) as i32;
        let by = (i / out.bw) as i32;
        for dy in -1..=1 {
            for dx in -1..=1 {
                if dx == 0 && dy == 0 {
                    continue;
                }
                let (nx, ny) = (bx + dx, by + dy);
                if nx < 0 || ny < 0 || nx >= out.bw as i32 || ny >= out.bh as i32 {
                    continue;
                }
                let gi = (out.cy0 + ny) as usize * g.w as usize + (out.cx0 + nx) as usize;
                let cost = g.cost[(g.cells[gi] >> 30) as usize];
                if cost == FAR {
                    continue;
                }
                let nd = d + cost * step * if dx != 0 && dy != 0 { SQRT2 } else { 1.0 };
                let ni = ny as u32 * out.bw + nx as u32;
                if nd >= out.way[ni as usize] || nd > longest {
                    continue;
                }
                out.way[ni as usize] = nd;
                open.push(Reverse((Way(nd), ni)));
            }
        }
    }

    // The way through free air would be the same steps (max + (√2 − 1) min cells); in the open, light goes
    // straight, so a cell is as far as its straight distance, made longer by as much as its way is longer than the
    // free one.
    for y in 0..out.bh {
        for x in 0..out.bw {
            let i = y as usize * out.bw as usize + x as usize;
            let way = out.way[i];
            if way == FAR {
                continue;
            }
            let cx = out.cx0 + x as i32;
            let cy = out.cy0 + y as i32;
            let tx = g.x0 as f64 + (cx as f64 + 0.5) * step;
            let ty = g.y0 as f64 + (cy as f64 + 0.5) * step;
            let straight = (tx - l.x).hypot(ty - l.y);
            let (ax, ay) = ((cx - lx).abs(), (cy - ly).abs());
            let free = (ax.max(ay) as f64 + (SQRT2 - 1.0) * ax.min(ay) as f64) * step;
            let d = if free > 0.0 { straight * (way / free) } else { straight };
            let k = lamp_falloff(d, radius);
            if k <= 0.0 {
                continue;
            }
            out.rgb[i * 3] = (l.r * k).clamp(0.0, 4.0);
            out.rgb[i * 3 + 1] = (l.g * k).clamp(0.0, 4.0);
            out.rgb[i * 3 + 2] = (l.b * k).clamp(0.0, 4.0);
        }
    }
}

fn merge_lamp(b: &LampBox, w: u32, rgb: &mut [f32]) {
    for y in 0..b.bh as usize {
        for x in 0..b.bw as usize {
            let from = &b.rgb[(y * b.bw as usize + x) * 3..][..3];
            let to = ((b.cy0 as usize + y) * w as usize + b.cx0 as usize + x) * 3;
            for c in 0..3 {
                rgb[to + c] = rgb[to + c].max(from[c]);
            }
        }
    }
}

/// Lamps with a radius over a grid of cells: `cells` (w × h, row by row, each `step` tiles, the first at tile x0,
/// y0) hold a `LightKind` in their top 2 bits. Light goes from the lamp's point to a cell's centre as far as straight
/// through the air; when it must go around blocks or pass through water or rock, the way is longer by as much as it
/// loses there with `transmit`. `rgb` (3 per cell) gets the brightest light per channel, up to 4; cells no lamp
/// reaches are left as they were.
#[allow(clippy::too_many_arguments)]
pub fn lamp_light(
    cells: &[u32],
    w: u32,
    h: u32,
    x0: i32,
    y0: i32,
    step: u32,
    transmit: &[f32; 4],
    lamps: &[PointLight],
    rgb: &mut Vec<f32>,
) {
    rgb.resize(w as usize * h as usize * 3, 0.0);
    let g = lamp_grid(cells, w, h, x0, y0, step, transmit);
    let mut b = LampBox::default();
    for l in lamps {
        one_lamp(&g, l, &mut b);
        merge_lamp(&b, w, rgb);
    }
}

/// The sky's colour at an hour of the day (0..24, in order); between two keys it changes evenly, from the last on
/// to the first of the next day.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SkyKey {
    pub hour: f32,
    pub color: Color,
}

/// The sky's colour at an hour (any, taken within 0..24); no keys: `fallback`.
pub fn sky_at(keys: &[SkyKey], hour: f64, fallback: Color) -> Color {
    if keys.is_empty() || !hour.is_finite() {
        return fallback;
    }
    let mut hour = hour % 24.0;
    if hour < 0.0 {
        hour += 24.0;
    }
    // The key at or before this hour (the last of yesterday before the first).
    let mut at = keys.len() - 1;
    for (i, k) in keys.iter().enumerate() {
        if k.hour as f64 <= hour {
            at = i;
        }
    }
    let a = keys[at];
    let b = keys[(at + 1) % keys.len()];
    let (from, mut to, mut now) = (a.hour as f64, b.hour as f64, hour);
    if to <= from {
        to += 24.0; // across midnight
    }
    if now < from {
        now += 24.0;
    }
    let span = to - from;
    let t = if span > 0.0 { ((now - from) / span).clamp(0.0, 1.0) as f32 } else { 0.0 };
    let mix = |x: f32, y: f32| if x == y { x } else { x + (y - x) * t };
    Color::rgb(mix(a.color.r, b.color.r), mix(a.color.g, b.color.g), mix(a.color.b, b.color.b))
}

/// The grid of one frame: what the video card spreads, and the light of the lamps with a radius.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LightGrid {
    /// w × h cells, row by row, as `pack_source`.
    pub cells: Vec<u32>,
    pub w: u32,
    pub h: u32,
    /// The first cell, in tiles.
    pub x0: i32,
    pub y0: i32,
    /// Tiles per cell (grows when zoomed far out).
    pub step: u32,
    /// The lamps with a radius: 3 per cell, empty without them.
    pub lamp_rgb: Vec<f32>,
    /// `lamp_rgb` as the video card reads it, 10 bits a channel.
    pub lamp_packed: Vec<u32>,
    pub lights: u32,
    pub lamps: u32,
}

fn floor_div(a: i32, b: i32) -> i32 {
    if a >= 0 { a / b } else { -((-a + b - 1) / b) }
}

impl LightGrid {
    /// The visible tiles plus a margin, one cell per `step` tiles, with its corner on a multiple of step so cells do
    /// not shift while scrolling. A cell looks at the tile in its middle; unloaded chunks are dark and block light.
    /// Lamps without a radius: the brightest source wins in each cell; those with one go their own way.
    pub fn build(
        world: &World,
        rules: &LightRules,
        camera: &Camera2D,
        width: u32,
        height: u32,
        lights: &[PointLight],
    ) -> LightGrid {
        let view = camera.visible_tiles(width, height).expanded(LIGHT_MARGIN);
        let mut step = 1;
        while (view.x1 - view.x0) / step >= MAX_CELLS_W as i32 - 1
            || (view.y1 - view.y0) / step >= MAX_CELLS_H as i32 - 1
        {
            step *= 2;
        }
        let gx0 = floor_div(view.x0, step) * step;
        let gy0 = floor_div(view.y0, step) * step;
        let gw = (floor_div(view.x1 - gx0, step) + 1) as u32;
        let gh = (floor_div(view.y1 - gy0, step) + 1) as u32;
        let mut cells = vec![0u32; gw as usize * gh as usize];

        let layers = forge_world::world::LAYERS as u32;
        let blocking = rules.blocking_layer.min(layers - 1) as usize;
        let dark = pack_source(0.0, 0.0, 0.0, LightKind::Opaque);
        cells.par_chunks_mut(gw as usize).enumerate().for_each(|(row, out)| {
            let ty = gy0 + row as i32 * step + step / 2;
            let mut chunk = None;
            let mut chunk_x = i32::MIN;
            for (col, cell) in out.iter_mut().enumerate() {
                let tx = gx0 + col as i32 * step + step / 2;
                let cx = tx.div_euclid(CHUNK);
                if cx != chunk_x {
                    chunk_x = cx;
                    chunk = world.chunks.get(&(cx, ty.div_euclid(CHUNK))).filter(|c| c.ready);
                }
                let Some(c) = chunk else {
                    *cell = dark;
                    continue;
                };
                let index = (ty.rem_euclid(CHUNK) * CHUNK + tx.rem_euclid(CHUNK)) as usize;
                let front = c.tiles[blocking * CHUNK_TILES + index] as usize;
                let kind =
                    if front != 0 { rules.kinds.get(front).copied().unwrap_or_default() } else { LightKind::Open };
                let (mut r, mut g, mut b) = (0.0f32, 0.0f32, 0.0f32);
                if front != 0
                    && let Some(glow) = rules.glow.get(front)
                {
                    (r, g, b) = (glow.r, glow.g, glow.b);
                }
                if rules.sky {
                    let open = (0..layers as usize).all(|l| {
                        let id = c.tiles[l * CHUNK_TILES + index] as usize;
                        id == 0
                            || ((rules.sky_through_layers >> l) & 1 != 0
                                && rules.sky_through.get(id).is_some_and(|t| *t != 0))
                    });
                    if open {
                        r = r.max(rules.sky_color.r);
                        g = g.max(rules.sky_color.g);
                        b = b.max(rules.sky_color.b);
                    }
                }
                *cell = pack_source(r, g, b, kind);
            }
        });

        let mut lamps = Vec::new();
        for l in lights {
            if l.radius > 0.0 {
                lamps.push(*l);
                continue;
            }
            let cx = (l.x.floor() as i32 - gx0) / step;
            let cy = (l.y.floor() as i32 - gy0) / step;
            if l.x < gx0 as f64 || l.y < gy0 as f64 || cx >= gw as i32 || cy >= gh as i32 {
                continue;
            }
            let cell = &mut cells[cy as usize * gw as usize + cx as usize];
            let r = (*cell & 1023).max(channel(l.r));
            let g = ((*cell >> 10) & 1023).max(channel(l.g));
            let b = ((*cell >> 20) & 1023).max(channel(l.b));
            *cell = r | g << 10 | b << 20 | (*cell & (3 << 30));
        }

        let mut lamp_rgb = Vec::new();
        let mut lamp_packed = Vec::new();
        if !lamps.is_empty() {
            let lg = lamp_grid(&cells, gw, gh, gx0, gy0, step as u32, &rules.transmit);
            let boxes: Vec<LampBox> = lamps
                .par_iter()
                .map(|l| {
                    let mut b = LampBox::default();
                    one_lamp(&lg, l, &mut b);
                    b
                })
                .collect();
            lamp_rgb = vec![0.0; cells.len() * 3];
            for b in &boxes {
                merge_lamp(b, gw, &mut lamp_rgb);
            }
            lamp_packed =
                lamp_rgb.chunks_exact(3).map(|c| channel(c[0]) | channel(c[1]) << 10 | channel(c[2]) << 20).collect();
        }
        LightGrid {
            cells,
            w: gw,
            h: gh,
            x0: gx0,
            y0: gy0,
            step: step as u32,
            lamp_rgb,
            lamp_packed,
            lights: lights.len() as u32,
            lamps: lamps.len() as u32,
        }
    }

    /// The light of the lamps with a radius at a tile point (none outside the grid or without them).
    pub fn lamp_at(&self, x: f64, y: f64) -> Color {
        if self.lamp_rgb.is_empty() || self.step == 0 {
            return Color::rgb(0.0, 0.0, 0.0);
        }
        let fx = (x - self.x0 as f64) / self.step as f64;
        let fy = (y - self.y0 as f64) / self.step as f64;
        if !(fx >= 0.0 && fy >= 0.0 && fx < self.w as f64 && fy < self.h as f64) {
            return Color::rgb(0.0, 0.0, 0.0);
        }
        let i = (fy as usize * self.w as usize + fx as usize) * 3;
        Color::rgb(self.lamp_rgb[i], self.lamp_rgb[i + 1], self.lamp_rgb[i + 2])
    }
}
