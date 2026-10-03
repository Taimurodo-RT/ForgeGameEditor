#pragma once

// Gravity: one pull for the whole world (any direction, any strength,
// changeable during play) plus local sources placed in it.
//
// A source pulls toward its centre (a small planet, a black hole) or in a
// fixed direction (an upside-down room, a wind tunnel) inside its radius.
// It either adds to the world's pull or replaces it inside its area, and its
// strength can fade toward the edge.

#include "forge/core/types.h"
#include "forge/data/reflect.h"
#include "forge/world/coords.h"

#include <span>
#include <vector>

namespace forge::sim {

// Saved with its entity; the entity's Position is the centre.
struct GravitySource {
    f32 radius = 8;            // tiles
    f32 strength = 40;         // tiles / s²; negative pushes away
    bool toward_center = true; // false: pulls along (dir_x, dir_y)
    f32 dir_x = 0, dir_y = 1;
    bool replace = false;      // inside the radius the world's gravity is off
    bool fade = false;         // full strength at the centre, none at the edge
};

class GravityField {
public:
    struct Placed {
        f64 x, y; // centre, in tiles
        GravitySource source;
    };

    void set_world(f32 x, f32 y) {
        world_x_ = x;
        world_y_ = y;
    }
    f32 world_x() const { return world_x_; }
    f32 world_y() const { return world_y_; }

    // Once per tick, before bodies move.
    void build(std::span<const Placed> sources);

    // The pull at a point, in tiles / s².
    void at(f64 x, f64 y, f32& gx, f32& gy) const;

    usize source_count() const { return sources_.size(); }

private:
    f32 world_x_ = 0, world_y_ = 0;
    std::vector<Placed> sources_;
    // Which sources reach each chunk of the box around them.
    world::Rect rect_;
    i64 width_ = 0;
    std::vector<u32> cell_start_;
    std::vector<u32> cell_sources_;
    bool grid_ = false;
};

} // namespace forge::sim

FORGE_REFLECT_DECLARE(forge::sim::GravitySource)
