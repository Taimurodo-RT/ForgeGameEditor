//! Chunk streaming, the same rules as engine/world/src/world.cpp: chunks around the focus are generated on worker
//! threads (nearest first, a bounded number in flight), far chunks are dropped a few per update, and edited chunks
//! are kept when they leave. With a save folder (`open_save`) changed chunks go to region files (`region`) and come
//! back from them in the next session.

use crate::codec::{decode_chunk, encode_chunk};
use crate::region::{self, ChunkLocation, RegionStore};
use crate::worldgen::{CHUNK, CHUNK_TILES, Generator};
use crossbeam_channel::{Receiver, Sender, unbounded};
use std::collections::{BTreeSet, HashMap};
use std::io;
use std::path::Path;
use std::sync::Arc;
use std::time::Instant;

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

/// A finished chunk on its way back from a worker thread.
struct Generated {
    coord: (i32, i32),
    tiles: Box<[u16]>,
    /// Restored from the save folder rather than generated.
    from_disk: bool,
}

pub struct Chunk {
    pub tiles: Box<[u16]>,
    pub revision: u32,
    /// Differs from what the generator makes, so it must be kept on unload.
    pub edited: bool,
    pub ready: bool,
    /// `revision` when the chunk last matched the save on disk (`None`: never saved).
    pub saved_revision: Option<u32>,
}

/// What a save wrote.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SaveReport {
    pub ok: bool,
    pub chunks: u32,
    pub regions: u32,
    pub bytes: usize,
    pub ms: f64,
}

pub enum Event {
    Loaded((i32, i32)),
    Unloading((i32, i32)),
}

pub struct World {
    generator: Arc<Generator>,
    bounds: Rect,
    pub chunks: HashMap<(i32, i32), Chunk>,
    /// Changed chunks that were unloaded before being saved, encoded.
    stored_edits: HashMap<(i32, i32), Vec<u8>>,
    store: Option<RegionStore>,
    /// Chunks in the save folder that are no longer changed (`set_chunk_tiles` with `edited` false): not read from
    /// there, removed by the next save.
    forgotten: BTreeSet<(i32, i32)>,
    loading: usize,
    max_in_flight: usize,
    last_keep: Option<Rect>,
    unload_pending: bool,
    tx: Sender<Generated>,
    rx: Receiver<Generated>,
    pub events: Vec<Event>,
    pub generated: u64,
    /// Chunks restored from stored edits or the save folder.
    pub restored: u64,
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
            store: None,
            forgotten: BTreeSet::new(),
            loading: 0,
            max_in_flight,
            last_keep: None,
            unload_pending: false,
            tx,
            rx,
            events: Vec::new(),
            generated: 0,
            restored: 0,
        }
    }

    pub fn resident(&self) -> usize {
        self.chunks.len()
    }

    fn integrate(&mut self, g: Generated) {
        self.loading -= 1;
        if g.from_disk {
            self.restored += 1;
        } else {
            self.generated += 1;
        }
        if let Some(keep) = self.last_keep
            && !keep.contains(g.coord.0, g.coord.1)
        {
            self.chunks.remove(&g.coord);
            return;
        }
        if let Some(c) = self.chunks.get_mut(&g.coord) {
            c.tiles = g.tiles;
            c.ready = true;
            // Straight from the save: unchanged until edited again.
            c.edited = g.from_disk;
            c.saved_revision = g.from_disk.then_some(c.revision);
            self.events.push(Event::Loaded(g.coord));
        }
    }

    fn integrate_finished(&mut self) {
        while let Ok(g) = self.rx.try_recv() {
            self.integrate(g);
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
                // Changed and not yet on disk: kept until the next save.
                if ch.edited && ch.saved_revision != Some(ch.revision) {
                    self.stored_edits.insert(c, encode_chunk(&ch.tiles));
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
        if let Some(bytes) = self.stored_edits.remove(&coord) {
            let mut tiles = vec![0u16; SLOT_TILES].into_boxed_slice();
            if decode_chunk(&bytes, &mut tiles) {
                self.restored += 1;
                let chunk = Chunk { tiles, revision: 1, edited: true, ready: true, saved_revision: None };
                self.chunks.insert(coord, chunk);
                self.events.push(Event::Loaded(coord));
                return;
            }
        }
        let disk = self.saved_location(coord);
        let chunk = Chunk { tiles: Box::new([]), revision: 0, edited: false, ready: false, saved_revision: None };
        self.chunks.insert(coord, chunk);
        self.loading += 1;
        let (g, tx) = (self.generator.clone(), self.tx.clone());
        rayon::spawn(move || {
            let mut tiles = vec![0u16; SLOT_TILES].into_boxed_slice();
            let from_disk = disk
                .and_then(|loc| region::read_chunk_bytes(&loc).ok())
                .is_some_and(|bytes| decode_chunk(&bytes, &mut tiles));
            if !from_disk {
                g.generate(coord.0, coord.1, &mut tiles);
            }
            let _ = tx.send(Generated { coord, tiles, from_disk });
        });
    }

    /// Where the save folder keeps this chunk, unless it is forgotten.
    fn saved_location(&self, coord: (i32, i32)) -> Option<ChunkLocation> {
        if self.forgotten.contains(&coord) {
            return None;
        }
        self.store.as_ref()?.locate(coord)
    }

    /// Waits for every load in flight (start of a benchmark, a save).
    pub fn finish_loading(&mut self) {
        while self.loading > 0 {
            if let Ok(g) = self.rx.recv() {
                self.integrate(g);
            }
        }
    }

    /// Keeps this world's changes in a folder of region files: chunks saved there come back instead of being
    /// generated. Before the first `update`.
    pub fn open_save(&mut self, folder: &Path) -> io::Result<()> {
        assert!(self.chunks.is_empty(), "open_save before the first update");
        self.store = Some(RegionStore::open(folder, "r", LAYERS as u32)?);
        Ok(())
    }

    pub fn has_save(&self) -> bool {
        self.store.is_some()
    }

    /// Writes every chunk changed since the last save (in memory or unloaded) to the save folder. Only the regions
    /// holding them are rewritten.
    pub fn save(&mut self) -> SaveReport {
        let Some(mut store) = self.store.take() else { return SaveReport::default() };
        let report = self.write_changes(&mut store, true);
        self.store = Some(store);
        report
    }

    /// The world as it is now written into another folder that holds a copy of the save folder: what `save` would
    /// write goes there. The world and its own folder stay as they were, nothing counts as saved.
    pub fn save_copy(&mut self, folder: &Path) -> SaveReport {
        if self.store.is_none() {
            return SaveReport::default();
        }
        match RegionStore::open(folder, "r", LAYERS as u32) {
            Ok(mut copy) => self.write_changes(&mut copy, false),
            Err(e) => {
                log::error!("world save: {}: {e}", folder.display());
                SaveReport::default()
            }
        }
    }

    /// The save folder was moved (renamed) to `folder`, its files as they were: chunks are read and saved there.
    pub fn moved_save(&mut self, folder: &Path) {
        if let Some(store) = &mut self.store {
            store.moved(folder);
        }
    }

    fn write_changes(&mut self, into: &mut RegionStore, saved: bool) -> SaveReport {
        let start = Instant::now();
        // No chunk may be read from the region files while they are rewritten.
        self.finish_loading();
        let mut changed: Vec<(i32, i32)> = self
            .chunks
            .iter()
            .filter(|(_, c)| c.ready && c.edited && c.saved_revision != Some(c.revision))
            .map(|(coord, _)| *coord)
            .collect();
        changed.sort_unstable();
        let encoded: Vec<Vec<u8>> = {
            use rayon::prelude::*;
            changed.par_iter().map(|coord| encode_chunk(&self.chunks[coord].tiles)).collect()
        };
        let mut writes: Vec<region::Write> =
            changed.iter().zip(&encoded).map(|(c, b)| region::Write { chunk: *c, bytes: Some(b) }).collect();
        for (coord, bytes) in &self.stored_edits {
            writes.push(region::Write { chunk: *coord, bytes: Some(bytes) });
        }
        // Saved chunks nobody has changed any more go (unless changed again).
        let mut removed = 0;
        for coord in &self.forgotten {
            if self.stored_edits.contains_key(coord) || self.chunks.get(coord).is_some_and(|c| c.ready && c.edited) {
                continue;
            }
            writes.push(region::Write { chunk: *coord, bytes: None });
            removed += 1;
        }
        let written = into.write(&writes);
        let report = SaveReport {
            ok: written.ok,
            chunks: (writes.len() - removed) as u32,
            regions: written.regions,
            bytes: written.bytes,
            ms: start.elapsed().as_secs_f64() * 1000.0,
        };
        if report.ok && saved {
            for coord in &changed {
                let c = self.chunks.get_mut(coord).unwrap();
                c.saved_revision = Some(c.revision);
            }
            self.stored_edits.clear();
            self.forgotten.clear();
        }
        report
    }

    /// Unloaded changed chunks not saved yet.
    pub fn stored_edits(&self) -> usize {
        self.stored_edits.len()
    }

    /// The chunk differs from what the generator makes: in memory, kept changed, or in the save folder.
    pub fn edited(&self, coord: (i32, i32)) -> bool {
        if let Some(c) = self.chunks.get(&coord)
            && c.ready
        {
            return c.edited;
        }
        self.stored_edits.contains_key(&coord) || self.saved_location(coord).is_some()
    }

    /// A chunk's tiles as they are now (layer after layer), without loading it: in memory, kept changed, saved, or
    /// made by the generator. `None` while it is loading (`finish_loading` first) or when a saved one cannot be read.
    pub fn peek_chunk(&self, coord: (i32, i32)) -> Option<Vec<u16>> {
        let mut out = vec![0u16; SLOT_TILES];
        if let Some(c) = self.chunks.get(&coord) {
            if !c.ready {
                return None;
            }
            out.copy_from_slice(&c.tiles);
            return Some(out);
        }
        if let Some(bytes) = self.stored_edits.get(&coord) {
            return decode_chunk(bytes, &mut out).then_some(out);
        }
        if let Some(loc) = self.saved_location(coord) {
            let bytes = region::read_chunk_bytes(&loc).ok()?;
            return decode_chunk(&bytes, &mut out).then_some(out);
        }
        self.generator.generate(coord.0, coord.1, &mut out);
        Some(out)
    }

    /// Replaces a ready chunk's tiles. `edited` false: they are what the generator makes, the chunk is not kept, and
    /// a copy in the save folder goes at the next save. False when it is not ready.
    pub fn set_chunk_tiles(&mut self, coord: (i32, i32), tiles: &[u16], edited: bool) -> bool {
        let Some(c) = self.chunks.get_mut(&coord) else { return false };
        if !c.ready {
            return false;
        }
        c.tiles.copy_from_slice(tiles);
        c.revision += 1;
        c.edited = edited;
        if !edited && self.store.as_ref().is_some_and(|s| s.locate(coord).is_some()) {
            self.forgotten.insert(coord);
        }
        true
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
