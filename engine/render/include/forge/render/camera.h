#pragma once

#include "forge/core/types.h"
#include "forge/world/coords.h"

namespace forge::render {

// What the player sees: centre of the screen in tiles and pixels per tile.
struct Camera2D {
    f64 x = 0;
    f64 y = 0;
    f32 zoom = 16.0f;

    // Tiles covering a screen of width × height pixels.
    world::Rect visible_tiles(u32 width, u32 height) const;
    void screen_to_tile(f32 sx, f32 sy, u32 width, u32 height, f64& tx, f64& ty) const;
    // The centre moved to a whole screen pixel, so the picture does not
    // shimmer while scrolling. Every renderer draws around this point.
    f64 snapped_x() const;
    f64 snapped_y() const;
};

} // namespace forge::render
