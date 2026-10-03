#pragma once

// Simple bodies: boxes that move, fall and slide along tiles. This is the
// fast path for the many (critters, enemies, items, the player in most 2D
// games): no rotation, no stacking on each other, but hundreds of thousands
// of them per tick. Crates that tumble and things that must push each other
// use the rigid-body physics instead.

#include "forge/core/types.h"
#include "forge/data/reflect.h"
#include "forge/scene/scene.h"
#include "forge/sim/tiles.h"

namespace forge::sim {

// What a body touched during its last tick. OnGround is the side gravity
// pulls it toward (the floor normally, a wall when gravity points sideways).
enum Contact : u8 {
    OnGround = 1 << 0,
    HitLeft = 1 << 1,
    HitRight = 1 << 2,
    HitTop = 1 << 3,
    HitBottom = 1 << 4,
};

// Saved with its entity. The entity's Position is the centre of the box.
struct Body {
    f32 vx = 0, vy = 0;              // tiles per second
    f32 half_w = 0.4f, half_h = 0.4f; // half the box size, in tiles
    f32 gravity = 1;                  // multiplies the gravity it is in (0: flies; negative: a balloon)
    bool collide = true;              // false: passes through tiles
    // Not saved: what it touched, the gravity it felt (to jump the other way)
    // and how far it moved in its last tick.
    u8 contacts = 0;
    u8 liquid = 0; // kind of liquid it is in (0: none), for swimming
    f32 gx = 0, gy = 0;
    f32 last_dx = 0, last_dy = 0;
};

// Moves one body over dt: gravity (gx, gy, already scaled by body.gravity)
// speeds it up, its speed along the pull is capped at max_fall, and it
// slides along solid tiles. Returns the contacts, also stored in the body.
u8 move_body(scene::Position& p, Body& body, f32 dt, f32 gx, f32 gy, f32 max_fall, const TileView& tiles);

// Where to draw a body between ticks (alpha from SimClock::alpha()).
inline void draw_position(const scene::Position& p, const Body& b, f32 alpha, f64& tile_x, f64& tile_y) {
    tile_x = p.tile_x() - static_cast<f64>((1.0f - alpha) * b.last_dx);
    tile_y = p.tile_y() - static_cast<f64>((1.0f - alpha) * b.last_dy);
}

} // namespace forge::sim

FORGE_REFLECT_DECLARE(forge::sim::Body)
