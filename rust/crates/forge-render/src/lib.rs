//! Drawing a tile world on wgpu, ported from engine/render: tiles (`tilemap`), sprites (`sprite_batch`, `sprites`),
//! particles on the video card (`particles`), and light and shadow over the tiles (`light`, `lighting`). The parts
//! that do not need a video card match the C++ version exactly (tests/data/lamp_light.txt is checked by both).

pub mod camera;
pub mod gpu;
pub mod light;
pub mod lighting;
pub mod particles;
pub mod sprite_batch;
pub mod sprites;
pub mod tilemap;

pub use camera::Camera2D;
pub use gpu::{offscreen_device, read_pixels, render_target};
pub use light::{
    Color, LightGrid, LightKind, LightRules, MAX_LAMP_RADIUS, PointLight, SkyKey, lamp_falloff, lamp_light, sky_at,
};
pub use lighting::{LightRenderer, LightStats};
pub use particles::{ParticleEmit, ParticleLook, ParticleStats, ParticleSystem};
pub use sprite_batch::{Sprite, SpriteBatch, SpriteBatchStats, SpriteList, ViewBox, draw_order, pack_color};
pub use sprites::{SpriteRect, SpriteRenderer, SpriteSheet, SpriteStats};
pub use tilemap::{TileAtlas, TilemapRenderer, TilemapStats};
