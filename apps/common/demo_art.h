#pragma once

// Placeholder art for the demos, drawn in code, until real art comes in
// through the asset library.

#include "forge/core/types.h"
#include "forge/render/sprite_renderer.h"

#include <vector>

namespace forge::demo {

// Tile atlas for the sample world generators: 16 × 16 cells of 16 px.
constexpr u32 kTileCellPx = 16;
constexpr u32 kTileCells = 16;
std::vector<u8> make_tile_atlas();

// Sprite sheet: 8 critters with 2 walking frames each (frame = kind * 2 +
// step), then a leaf and a soft glow.
constexpr u32 kCritterKinds = 8;
constexpr u32 kFrameLeaf = 16;
constexpr u32 kFrameGlow = 17;

struct SheetImage {
    std::vector<u8> rgba;
    u32 width = 0, height = 0;
    std::vector<render::SpriteRect> frames;
    render::SpriteSheet sheet() const {
        return {rgba.data(), width, height, frames.data(), static_cast<u32>(frames.size())};
    }
};
SheetImage make_sprite_sheet();

} // namespace forge::demo
