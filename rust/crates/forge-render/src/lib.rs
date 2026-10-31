//! Drawing a tile world, ported from engine/render: the camera, the list of sprites a frame draws, and light and
//! shadow over the tiles. The parts that do not need a video card come first and match the C++ version exactly
//! (tests/data/lamp_light.txt is checked by both).

pub mod camera;
pub mod light;
pub mod sprite_batch;

pub use camera::Camera2D;
pub use light::{
    Color, LightGrid, LightKind, LightRules, MAX_LAMP_RADIUS, PointLight, SkyKey, lamp_falloff, lamp_light, sky_at,
};
pub use sprite_batch::{Sprite, SpriteBatch, SpriteBatchStats, SpriteList, ViewBox, draw_order, pack_color};
