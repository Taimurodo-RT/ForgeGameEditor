//! Sample world generators, ported one to one from engine/world/src/generators.cpp: the same seed gives the same
//! tiles bit for bit (tests/parity.rs checks this against tiles/world_chunks.txt, which the C++ tests check too).
//! Arithmetic on seeds wraps as in C++.

pub const CHUNK: i32 = 64;
pub const CHUNK_TILES: usize = (CHUNK * CHUNK) as usize;

pub const AIR: u16 = 0;
pub const GRASS: u16 = 1;
pub const DIRT: u16 = 2;
pub const STONE: u16 = 3;
pub const SAND: u16 = 4;
pub const COPPER: u16 = 5;
pub const IRON: u16 = 6;
pub const GOLD: u16 = 7;
pub const DIRT_WALL: u16 = 8;
pub const STONE_WALL: u16 = 9;
pub const WATER: u16 = 10;
pub const DEEP_WATER: u16 = 11;
pub const MEADOW: u16 = 12;
pub const FOREST: u16 = 13;
pub const TREE: u16 = 14;
pub const ROCK: u16 = 15;
pub const SNOW: u16 = 16;

fn mix(mut x: u64) -> u64 {
    x ^= x >> 30;
    x = x.wrapping_mul(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x = x.wrapping_mul(0x94d049bb133111eb);
    x ^= x >> 31;
    x
}

pub fn hash_tile(seed: u64, x: i32, y: i32) -> u32 {
    let k = ((x as u32 as u64) << 32) | (y as u32 as u64);
    (mix(k ^ mix(seed)) >> 32) as u32
}

fn corner(seed: u64, x: i32, y: i32) -> f32 {
    hash_tile(seed, x, y) as f32 * (1.0 / 4294967296.0)
}

fn smooth(t: f32) -> f32 {
    t * t * (3.0 - 2.0 * t)
}

pub fn value_noise(seed: u64, x: f32, y: f32) -> f32 {
    let (fx, fy) = (x.floor(), y.floor());
    let (ix, iy) = (fx as i32, fy as i32);
    let (tx, ty) = (smooth(x - fx), smooth(y - fy));
    let a = corner(seed, ix, iy);
    let b = corner(seed, ix.wrapping_add(1), iy);
    let c = corner(seed, ix, iy.wrapping_add(1));
    let d = corner(seed, ix.wrapping_add(1), iy.wrapping_add(1));
    let top = a + (b - a) * tx;
    let bottom = c + (d - c) * tx;
    top + (bottom - top) * ty
}

pub fn fractal_noise(seed: u64, x: f32, y: f32, octaves: u32) -> f32 {
    let (mut sum, mut amplitude, mut total, mut frequency) = (0.0, 0.5, 0.0, 1.0);
    for i in 0..octaves {
        sum +=
            value_noise(seed.wrapping_add((i as u64).wrapping_mul(0x9E3779B97F4A7C15)), x * frequency, y * frequency)
                * amplitude;
        total += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    sum / total
}

const STEP: i32 = 4;
const GRID: i32 = CHUNK / STEP + 1;

/// Smooth noise sampled every STEP tiles and filled in bilinearly (as ChunkField in C++).
struct ChunkField([f32; (GRID * GRID) as usize]);

impl ChunkField {
    fn new(seed: u64, x0: i32, y0: i32, fx: f32, fy: f32, octaves: u32) -> Self {
        let mut g = [0.0; (GRID * GRID) as usize];
        for gy in 0..GRID {
            for gx in 0..GRID {
                g[(gy * GRID + gx) as usize] =
                    fractal_noise(seed, (x0 + gx * STEP) as f32 * fx, (y0 + gy * STEP) as f32 * fy, octaves);
            }
        }
        Self(g)
    }

    fn at(&self, lx: i32, ly: i32) -> f32 {
        let (gx, gy) = (lx / STEP, ly / STEP);
        let tx = (lx % STEP) as f32 * (1.0 / STEP as f32);
        let ty = (ly % STEP) as f32 * (1.0 / STEP as f32);
        let r0 = (gy * GRID + gx) as usize;
        let r1 = r0 + GRID as usize;
        let g = &self.0;
        let top = g[r0] + (g[r0 + 1] - g[r0]) * tx;
        let bottom = g[r1] + (g[r1 + 1] - g[r1]) * tx;
        top + (bottom - top) * ty
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Generator {
    /// Side view, like Terraria. Layer 0: background walls, layer 1: blocks.
    Side { seed: u64, surface_y: i32 },
    /// Top down, like Factorio. Layer 0: ground (biomes, lakes), layer 1: trees and rocks.
    TopDown { seed: u64 },
    /// Every tile of every layer empty: around a level made only of its own chunks.
    Empty,
}

impl Generator {
    /// Fills `out`, which holds whole layers of CHUNK_TILES each; layers past the generator's own are cleared.
    /// The side view needs at least two layers.
    pub fn generate(&self, cx: i32, cy: i32, out: &mut [u16]) {
        assert!(
            !out.is_empty() && out.len().is_multiple_of(CHUNK_TILES),
            "a chunk is whole layers of {CHUNK_TILES} tiles"
        );
        out.fill(AIR);
        match *self {
            Generator::Side { seed, surface_y } => {
                let (walls, rest) = out.split_at_mut(CHUNK_TILES);
                side(seed, surface_y, cx, cy, walls, &mut rest[..CHUNK_TILES]);
            }
            Generator::TopDown { seed } => {
                let (ground, rest) = out.split_at_mut(CHUNK_TILES);
                top_down(seed, cx, cy, ground, rest.get_mut(..CHUNK_TILES));
            }
            Generator::Empty => {}
        }
    }
}

fn side(seed: u64, surface_y: i32, cx: i32, cy: i32, walls: &mut [u16], blocks: &mut [u16]) {
    let (x0, y0) = (cx * CHUNK, cy * CHUNK);
    let mut surface = [0i32; CHUNK as usize];
    let mut dirt_depth = [0i32; CHUNK as usize];
    let mut highest = i32::MAX;
    for lx in 0..CHUNK as usize {
        let wx = x0.wrapping_add(lx as i32) as f32;
        let hills = (fractal_noise(seed, wx * 0.01, 0.5, 4) - 0.5) * 90.0;
        let mountains = (fractal_noise(seed.wrapping_add(1), wx * 0.0015, 0.5, 3) - 0.5) * 400.0;
        surface[lx] = surface_y + (hills + mountains) as i32;
        dirt_depth[lx] = 6 + (value_noise(seed.wrapping_add(5), wx * 0.05, 0.5) * 12.0) as i32;
        highest = highest.min(surface[lx]);
    }
    if y0 + CHUNK <= highest {
        walls.fill(AIR);
        blocks.fill(AIR);
        return;
    }
    let tunnels = ChunkField::new(seed.wrapping_add(2), x0, y0, 0.025, 0.04, 3);
    let caverns = ChunkField::new(seed.wrapping_add(3), x0, y0, 0.012, 0.016, 2);
    for ly in 0..CHUNK {
        let wy = y0 + ly;
        for lx in 0..CHUNK {
            let i = (ly * CHUNK + lx) as usize;
            let wx = x0 + lx;
            let depth = wy - surface[lx as usize];
            if depth < 0 {
                walls[i] = AIR;
                blocks[i] = AIR;
                continue;
            }
            let dirt = depth < dirt_depth[lx as usize];
            walls[i] = if depth == 0 {
                AIR
            } else if dirt {
                DIRT_WALL
            } else {
                STONE_WALL
            };
            let mut block = if depth == 0 {
                GRASS
            } else if dirt {
                DIRT
            } else {
                STONE
            };
            if depth > 12 {
                let tunnel = (tunnels.at(lx, ly) - 0.5).abs();
                let width = 0.03 + (depth as f32).min(2000.0) * 0.00001;
                let cavern = depth > 80 && caverns.at(lx, ly) > 0.7;
                if tunnel < width || cavern {
                    block = AIR;
                } else if block == STONE {
                    let ore = value_noise(seed.wrapping_add(4), wx as f32 * 0.18, wy as f32 * 0.18);
                    if ore > 0.86 {
                        block = if depth < 400 {
                            COPPER
                        } else if depth < 1500 {
                            IRON
                        } else {
                            GOLD
                        };
                    }
                }
            }
            blocks[i] = block;
        }
    }
}

fn top_down(seed: u64, cx: i32, cy: i32, ground: &mut [u16], mut objects: Option<&mut [u16]>) {
    let (x0, y0) = (cx * CHUNK, cy * CHUNK);
    let heights = ChunkField::new(seed, x0, y0, 0.004, 0.004, 5);
    let moistures = ChunkField::new(seed.wrapping_add(7), x0, y0, 0.003, 0.003, 3);
    let colds = ChunkField::new(seed.wrapping_add(9), x0, y0, 0.0015, 0.0015, 2);
    for ly in 0..CHUNK {
        for lx in 0..CHUNK {
            let i = (ly * CHUNK + lx) as usize;
            let (wx, wy) = (x0 + lx, y0 + ly);
            let height = heights.at(lx, ly);
            let moisture = moistures.at(lx, ly);
            let cold = colds.at(lx, ly);
            let dice = hash_tile(seed.wrapping_add(11), wx, wy) % 100;
            let g = if height < 0.36 {
                DEEP_WATER
            } else if height < 0.42 {
                WATER
            } else if height < 0.45 {
                SAND
            } else if cold < 0.32 {
                SNOW
            } else if moisture > 0.56 {
                FOREST
            } else {
                MEADOW
            };
            let mut o = AIR;
            if (g == FOREST && dice < 30) || ((g == MEADOW || g == SNOW) && dice < 3) {
                o = TREE;
            }
            if height > 0.62 && g != FOREST && dice >= 90 {
                o = ROCK;
            }
            ground[i] = g;
            if let Some(objects) = objects.as_deref_mut() {
                objects[i] = o;
            }
        }
    }
}
