//! Chunk streaming, the same rules as engine/world/src/world.cpp: chunks around the focus are generated on worker
//! threads (nearest first, a bounded number in flight), far chunks are dropped a few per update, and edited chunks
//! are kept when they leave.

use crate::worldgen::{CHUNK, CHUNK_TILES, Generator};
use crossbeam_channel::{Receiver, Sender, unbounded};
use std::collections::HashMap;
use std::sync::Arc;

pub const LAYERS: usize = 2;
pub const SLOT_TILES: usize = LAYERS * CHUNK_TILES;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Rect {
    pub x0: i32,
    pub y0: i32,
    pub x1: i32,
    pub y1: i32,
}

impl Rect {
    pub fn expanded(self, n: i32) -> Rect {
        Rect { x0: self.x0 - n, y0: self.y0 - n, x1: self.x1 + n, y1: self.y1 + n }
    }
    pub fn contains(self, x: i32, y: i32) -> bool {
        x >= self.x0 && x < self.x1 && y >= self.y0 && y < self.y1
    }
    pub fn clipped(self, b: Rect) -> Rect {
        Rect { x0: self.x0.max(b.x0), y0: self.y0.max(b.y0), x1: self.x1.min(b.x1), y1: self.y1.min(b.y1) }
    }
}

pub struct Chunk {
    pub tiles: Box<[u16]>,
    pub revision: u32,
    pub edited: bool,
    pub ready: bool,
}

pub enum Event {
    Loaded((i32, i32)),
    Unloading((i32, i32)),
}

pub struct World {
    generator: Arc<Generator>,
    bounds: Rect,
    pub chunks: HashMap<(i32, i32), Chunk>,
    stored_edits: HashMap<(i32, i32), Box<[u16]>>,
    loading: usize,
    max_in_flight: usize,
    last_keep: Option<Rect>,
    unload_pending: bool,
    tx: Sender<((i32, i32), Box<[u16]>)>,
    rx: Receiver<((i32, i32), Box<[u16]>)>,
    pub events: Vec<Event>,
    pub generated: u64,
}

impl World {
    pub fn new(generator: Generator, bounds: Rect) -> Self {
        let (tx, rx) = unbounded();
        let max_in_flight = 64.max(rayon::current_num_threads() * 32);
        Self {
            generator: Arc::new(generator),
            bounds,
            chunks: HashMap::new(),
            stored_edits: HashMap::new(),
            loading: 0,
            max_in_flight,
            last_keep: None,
            unload_pending: false,
            tx,
            rx,
            events: Vec::new(),
            generated: 0,
        }
    }

    pub fn resident(&self) -> usize {
        self.chunks.len()
    }

    fn integrate_finished(&mut self) {
        while let Ok((coord, tiles)) = self.rx.try_recv() {
            self.loading -= 1;
            self.generated += 1;
            if let Some(keep) = self.last_keep {
                if !keep.contains(coord.0, coord.1) {
                    self.chunks.remove(&coord);
                    continue;
                }
            }
            if let Some(c) = self.chunks.get_mut(&coord) {
                c.tiles = tiles;
                c.ready = true;
                self.events.push(Event::Loaded(coord));
            }
        }
    }

    /// Once per frame; `focus` is in tiles.
    pub fn update(&mut self, focus: Rect) {
        self.integrate_finished();
        let chunks = Rect {
            x0: focus.x0.div_euclid(CHUNK),
            y0: focus.y0.div_euclid(CHUNK),
            x1: (focus.x1 + CHUNK - 1).div_euclid(CHUNK),
            y1: (focus.y1 + CHUNK - 1).div_euclid(CHUNK),
        };
        let want = chunks.expanded(2);
        let keep = chunks.expanded(4);
        if self.last_keep != Some(keep) {
            self.last_keep = Some(keep);
            self.unload_pending = true;
        }
        if self.unload_pending {
            self.unload_pending = false;
            let far: Vec<(i32, i32)> = self
                .chunks
                .iter()
                .filter(|(c, ch)| ch.ready && !keep.contains(c.0, c.1))
                .map(|(c, _)| *c)
                .take(65)
                .collect();
            if far.len() > 64 {
                self.unload_pending = true;
            }
            for c in far.into_iter().take(64) {
                self.events.push(Event::Unloading(c));
                let ch = self.chunks.remove(&c).unwrap();
                if ch.edited {
                    self.stored_edits.insert(c, ch.tiles);
                }
            }
        }
        if self.loading >= self.max_in_flight {
            return;
        }
        let r = want.clipped(self.bounds);
        let (cx2, cy2) = (r.x0 as i64 + r.x1 as i64, r.y0 as i64 + r.y1 as i64);
        let mut candidates = Vec::new();
        for y in r.y0..r.y1 {
            for x in r.x0..r.x1 {
                if self.chunks.contains_key(&(x, y)) {
                    continue;
                }
                let (dx, dy) = (2 * x as i64 + 1 - cx2, 2 * y as i64 + 1 - cy2);
                candidates.push((dx * dx + dy * dy, (x, y)));
            }
        }
        let budget = self.max_in_flight - self.loading;
        if candidates.len() > budget {
            candidates.select_nth_unstable_by_key(budget, |c| c.0);
            candidates.truncate(budget);
        }
        for (_, coord) in candidates {
            self.start_load(coord);
        }
    }

    fn start_load(&mut self, coord: (i32, i32)) {
        if let Some(tiles) = self.stored_edits.remove(&coord) {
            self.chunks.insert(coord, Chunk { tiles, revision: 1, edited: true, ready: true });
            self.events.push(Event::Loaded(coord));
            return;
        }
        self.chunks.insert(coord, Chunk { tiles: Box::new([]), revision: 0, edited: false, ready: false });
        self.loading += 1;
        let (g, tx) = (self.generator.clone(), self.tx.clone());
        rayon::spawn(move || {
            let mut tiles = vec![0u16; SLOT_TILES].into_boxed_slice();
            g.generate(coord.0, coord.1, &mut tiles);
            let _ = tx.send((coord, tiles));
        });
    }

    /// Waits for every load in flight (start of a benchmark).
    pub fn finish_loading(&mut self) {
        while self.loading > 0 {
            if let Ok(m) = self.rx.recv() {
                self.loading -= 1;
                self.generated += 1;
                if let Some(c) = self.chunks.get_mut(&m.0) {
                    c.tiles = m.1;
                    c.ready = true;
                    self.events.push(Event::Loaded(m.0));
                }
            }
        }
    }

    pub fn set_tile(&mut self, x: i32, y: i32, layer: usize, id: u16) -> bool {
        let coord = (x.div_euclid(CHUNK), y.div_euclid(CHUNK));
        let Some(c) = self.chunks.get_mut(&coord) else { return false };
        if !c.ready {
            return false;
        }
        let i = layer * CHUNK_TILES + (y.rem_euclid(CHUNK) * CHUNK + x.rem_euclid(CHUNK)) as usize;
        if c.tiles[i] == id {
            return false;
        }
        c.tiles[i] = id;
        c.revision += 1;
        c.edited = true;
        true
    }
}
