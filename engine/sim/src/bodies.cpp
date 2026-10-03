#include "forge/sim/bodies.h"

#include <algorithm>
#include <cmath>

FORGE_REFLECT(forge::sim::Body, 1) {
    t.field("vx", &forge::sim::Body::vx);
    t.field("vy", &forge::sim::Body::vy);
    t.field("half_w", &forge::sim::Body::half_w);
    t.field("half_h", &forge::sim::Body::half_h);
    t.field("gravity", &forge::sim::Body::gravity);
    t.field("collide", &forge::sim::Body::collide);
}

namespace forge::sim {

namespace {

// Bodies move in steps of at most this many tiles, so a fast one cannot jump
// over a one-tile wall.
constexpr f32 kMaxStep = 0.45f;
// Edges exactly on a tile border do not count as inside the next tile.
constexpr f32 kEdge = 1e-3f;

// Without SSE4.1, std::floor is a library call; this is a few instructions.
i32 floor_i(f32 v) {
    const i32 i = static_cast<i32>(v);
    return i - (v < static_cast<f32>(i));
}

u32 steps_for(f32 d) {
    const f32 n = std::fabs(d) * (1.0f / kMaxStep);
    const u32 i = static_cast<u32>(n);
    return i + (static_cast<f32>(i) < n);
}

} // namespace

u8 move_body(scene::Position& p, Body& body, f32 dt, f32 gx, f32 gy, f32 max_fall, const TileView& tiles) {
    body.vx += gx * dt;
    body.vy += gy * dt;
    body.gx = gx;
    body.gy = gy;
    const f32 g = std::sqrt(gx * gx + gy * gy);
    if (g > 1e-6f) {
        const f32 nx = gx / g, ny = gy / g;
        const f32 along = body.vx * nx + body.vy * ny;
        if (along > max_fall) {
            body.vx -= (along - max_fall) * nx;
            body.vy -= (along - max_fall) * ny;
        }
    }
    const f32 dx = body.vx * dt, dy = body.vy * dt;
    const f32 start_x = p.x, start_y = p.y;
    u8 contacts = 0;

    if (!body.collide) {
        p.x += dx;
        p.y += dy;
    } else {
        // Tiles are found from the chunk origin plus the local position, so
        // the maths stays in small floats anywhere in a huge world.
        const i32 bx = p.cx * world::kChunkSize, by = p.cy * world::kChunkSize;
        const f32 hw = body.half_w, hh = body.half_h;

        // Horizontal, then vertical: sliding along floors and walls.
        if (dx != 0) {
            const u32 steps = steps_for(dx);
            const f32 step = dx / static_cast<f32>(steps);
            const i32 row0 = by + floor_i(p.y - hh + kEdge), row1 = by + floor_i(p.y + hh - kEdge);
            for (u32 s = 0; s < steps; ++s) {
                const f32 nx = p.x + step;
                const i32 col = bx + (step > 0 ? floor_i(nx + hw - kEdge) : floor_i(nx - hw));
                bool hit = false;
                for (i32 row = row0; row <= row1 && !hit; ++row) hit = tiles.solid(col, row);
                if (hit) {
                    if (step > 0) {
                        p.x = static_cast<f32>(col - bx) - hw;
                        contacts |= HitRight;
                    } else {
                        p.x = static_cast<f32>(col + 1 - bx) + hw;
                        contacts |= HitLeft;
                    }
                    body.vx = 0;
                    break;
                }
                p.x = nx;
            }
        }
        if (dy != 0) {
            const u32 steps = steps_for(dy);
            const f32 step = dy / static_cast<f32>(steps);
            const i32 col0 = bx + floor_i(p.x - hw + kEdge), col1 = bx + floor_i(p.x + hw - kEdge);
            for (u32 s = 0; s < steps; ++s) {
                const f32 ny = p.y + step;
                bool hit = false;
                if (step > 0) {
                    const i32 row = by + floor_i(ny + hh - kEdge);
                    // A platform holds only what was above its top before this step.
                    const bool above = p.y + hh <= static_cast<f32>(row - by) + kEdge;
                    for (i32 col = col0; col <= col1 && !hit; ++col) {
                        const TileShape shape = tiles.shape(col, row);
                        hit = shape == TileShape::Solid || (shape == TileShape::Platform && above);
                    }
                    if (hit) {
                        p.y = static_cast<f32>(row - by) - hh;
                        contacts |= HitBottom;
                    }
                } else {
                    const i32 row = by + floor_i(ny - hh);
                    for (i32 col = col0; col <= col1 && !hit; ++col) hit = tiles.solid(col, row);
                    if (hit) {
                        p.y = static_cast<f32>(row + 1 - by) + hh;
                        contacts |= HitTop;
                    }
                }
                if (hit) {
                    body.vy = 0;
                    break;
                }
                p.y = ny;
            }
        }
    }
    // Ground is the side the pull points to most.
    if (g > 1e-6f) {
        const u8 down = std::fabs(gy) >= std::fabs(gx) ? (gy > 0 ? HitBottom : HitTop) : (gx > 0 ? HitRight : HitLeft);
        if (contacts & down) contacts |= OnGround;
    }
    body.last_dx = p.x - start_x;
    body.last_dy = p.y - start_y;
    body.contacts = contacts;
    return contacts;
}

} // namespace forge::sim
