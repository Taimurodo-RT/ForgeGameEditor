//! Tile worlds split into 64 × 64 chunks, ported from engine/world. Tiles are not entities: a chunk is a flat array
//! of tile ids per layer. `worldgen` makes chunks (the same tiles as the C++ generators, checked by
//! tests/parity.rs), `world` streams them around a focus on worker threads and saves changed chunks into region
//! files (`region`, `codec`) that the C++ version reads and writes too (checked by tests/save_parity.rs).

pub mod codec;
pub mod region;
pub mod world;
pub mod worldgen;

pub use world::{Chunk, Event, Rect, SaveReport, World};
pub use worldgen::{CHUNK, CHUNK_TILES, Generator};
