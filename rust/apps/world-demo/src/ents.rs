//! The crowd: N creatures in bevy_ecs that wander over the top-down world, turn now and then, and do not walk into
//! water or rocks (tiles looked up in a passability grid built from the generator). Systems run on all cores.

use bevy_ecs::prelude::*;
use forge_world::worldgen::{self, Generator};

pub const AREA: i32 = 4096; // the creatures live in AREA × AREA tiles around the centre of the world

#[derive(Component, Clone, Copy)]
pub struct Pos(pub [f32; 2]);

#[derive(Component, Clone, Copy)]
pub struct Vel(pub [f32; 2]);

#[derive(Component, Clone, Copy)]
pub struct Mind {
    pub seed: u32,
    pub timer: f32,
    pub color: u32,
}

#[derive(Resource)]
pub struct Passable {
    pub origin: [i32; 2],
    pub bits: Vec<u64>,
}

impl Passable {
    pub fn walkable(&self, x: f32, y: f32) -> bool {
        let (tx, ty) = (x.floor() as i32 - self.origin[0], y.floor() as i32 - self.origin[1]);
        if tx < 0 || ty < 0 || tx >= AREA || ty >= AREA {
            return false;
        }
        let i = (ty * AREA + tx) as usize;
        self.bits[i >> 6] & (1 << (i & 63)) != 0
    }
}

#[derive(Resource, Default)]
pub struct Tick {
    pub dt: f32,
}

/// Drawn instances, filled by the last system.
#[derive(Resource, Default)]
pub struct Drawn(pub Vec<[u32; 3]>);

fn hash(mut x: u32) -> u32 {
    x ^= x >> 16;
    x = x.wrapping_mul(0x7feb352d);
    x ^= x >> 15;
    x = x.wrapping_mul(0x846ca68b);
    x ^ (x >> 16)
}

fn think(tick: Res<Tick>, mut q: Query<(&mut Vel, &mut Mind)>) {
    let dt = tick.dt;
    q.par_iter_mut().for_each(|(mut v, mut m)| {
        m.timer -= dt;
        if m.timer <= 0.0 {
            m.seed = hash(m.seed);
            let a = (m.seed & 0xffff) as f32 * (std::f32::consts::TAU / 65536.0);
            let speed = 1.5 + ((m.seed >> 16) & 0xff) as f32 * (4.0 / 255.0);
            v.0 = [a.cos() * speed, a.sin() * speed];
            m.timer = 0.5 + ((m.seed >> 24) as f32) * (3.0 / 255.0);
        }
    });
}

fn walk(tick: Res<Tick>, grid: Res<Passable>, mut q: Query<(&mut Pos, &mut Vel)>) {
    let dt = tick.dt;
    q.par_iter_mut().for_each(|(mut p, mut v)| {
        let nx = p.0[0] + v.0[0] * dt;
        let ny = p.0[1] + v.0[1] * dt;
        if grid.walkable(nx, p.0[1]) {
            p.0[0] = nx;
        } else {
            v.0[0] = -v.0[0];
        }
        if grid.walkable(p.0[0], ny) {
            p.0[1] = ny;
        } else {
            v.0[1] = -v.0[1];
        }
    });
}

fn gather(mut drawn: ResMut<Drawn>, q: Query<(&Pos, &Mind)>) {
    let out = &mut drawn.0;
    out.clear();
    out.extend(q.iter().map(|(p, m)| [p.0[0].to_bits(), p.0[1].to_bits(), m.color]));
}

pub struct Crowd {
    pub world: bevy_ecs::world::World,
    pub schedule: Schedule,
}

impl Crowd {
    pub fn new(count: u32, generator: Generator, centre: [i32; 2]) -> Self {
        let origin = [centre[0] - AREA / 2, centre[1] - AREA / 2];
        // Passability from the generator, chunk by chunk on all cores.
        let per_side = AREA / worldgen::CHUNK;
        let rows: Vec<Vec<u64>> = {
            use rayon::prelude::*;
            (0..per_side)
                .into_par_iter()
                .map(|cy| {
                    let mut row = vec![0u64; (AREA * worldgen::CHUNK) as usize / 64];
                    let mut tiles = vec![0u16; forge_world::world::SLOT_TILES];
                    for cx in 0..per_side {
                        generator.generate(
                            origin[0] / worldgen::CHUNK + cx,
                            origin[1] / worldgen::CHUNK + cy,
                            &mut tiles,
                        );
                        for ly in 0..worldgen::CHUNK {
                            for lx in 0..worldgen::CHUNK {
                                let i = (ly * worldgen::CHUNK + lx) as usize;
                                let (g, o) = (tiles[i], tiles[worldgen::CHUNK_TILES + i]);
                                let ok = g != worldgen::WATER && g != worldgen::DEEP_WATER && o != worldgen::ROCK;
                                if ok {
                                    let b = (ly * AREA + cx * worldgen::CHUNK + lx) as usize;
                                    row[b >> 6] |= 1 << (b & 63);
                                }
                            }
                        }
                    }
                    row
                })
                .collect()
        };
        let grid = Passable { origin, bits: rows.concat() };

        let mut world = bevy_ecs::world::World::new();
        let mut seed = 2026u32;
        let mut batch = Vec::with_capacity(count as usize);
        while batch.len() < count as usize {
            seed = hash(seed);
            let x = origin[0] as f32 + (seed % AREA as u32) as f32 + 0.5;
            seed = hash(seed);
            let y = origin[1] as f32 + (seed % AREA as u32) as f32 + 0.5;
            if !grid.walkable(x, y) {
                continue;
            }
            let color = 0xff000000 | (hash(seed) & 0x00ffffff) | 0x00404040;
            batch.push((Pos([x, y]), Vel([0.0, 0.0]), Mind { seed, timer: 0.0, color }));
        }
        world.spawn_batch(batch);
        world.insert_resource(grid);
        world.insert_resource(Tick::default());
        world.insert_resource(Drawn::default());
        let mut schedule = Schedule::default();
        schedule.add_systems((think, walk, gather).chain());
        Self { world, schedule }
    }

    pub fn step(&mut self, dt: f32) {
        self.world.resource_mut::<Tick>().dt = dt;
        if std::env::var("CROWD_PROFILE").is_ok() {
            let t = std::time::Instant::now();
            let mut a = Schedule::default();
            a.add_systems(think);
            let mut b = Schedule::default();
            b.add_systems(walk);
            let mut c = Schedule::default();
            c.add_systems(gather);
            a.run(&mut self.world);
            let t1 = t.elapsed();
            b.run(&mut self.world);
            let t2 = t.elapsed();
            c.run(&mut self.world);
            let t3 = t.elapsed();
            eprintln!("think {:?} walk {:?} gather {:?}", t1, t2 - t1, t3 - t2);
            return;
        }
        self.schedule.run(&mut self.world);
    }

    pub fn drawn(&self) -> &[[u32; 3]] {
        &self.world.resource::<Drawn>().0
    }
}
