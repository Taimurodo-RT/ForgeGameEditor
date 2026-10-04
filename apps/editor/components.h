#pragma once

// Components of the editor's sample scene. Described with FORGE_REFLECT, so
// the inspector, undo and saving work for them with no editor code.

#include "forge/core/math.h"
#include "forge/core/types.h"
#include "forge/data/reflect.h"

namespace forge::editor_app {

struct Transform {
    Vec2 position{};  // tiles
    f32 rotation = 0; // degrees, clockwise
    Vec2 scale{1, 1};
};

enum class Picture : u32 { Critter, Crate, Ball, Leaf, Glow, Drill, Furnace, Chest };

struct Look {
    Picture picture = Picture::Crate;
    u32 variant = 0; // which critter
    Color tint{};
    u8 layer = 1;
    bool visible = true;
};

// Moves the object while the game runs (Play).
struct Mover {
    Vec2 velocity{};  // tiles per second
    f32 spin = 0;     // degrees per second
    f32 bounce_radius = 6; // turns back this far from where it started
};

u32 sprite_frame(const Look& look, f64 time);

} // namespace forge::editor_app

FORGE_REFLECT_DECLARE(forge::editor_app::Transform)
FORGE_REFLECT_DECLARE(forge::editor_app::Picture)
FORGE_REFLECT_DECLARE(forge::editor_app::Look)
FORGE_REFLECT_DECLARE(forge::editor_app::Mover)
