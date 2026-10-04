#pragma once

// Placeholder art for the demos, drawn in code, until real art comes in
// through the asset library.

#include "forge/core/types.h"
#include "forge/render/lighting.h"
#include "forge/render/sprite_renderer.h"

#include <vector>

namespace forge::demo {

// Tile atlas for the sample world generators: 16 × 16 cells of 16 px.
constexpr u32 kTileCellPx = 16;
constexpr u32 kTileCells = 16;
std::vector<u8> make_tile_atlas();

// Sprite sheet: 8 critters with 2 walking frames each (frame = kind * 2 +
// step), then a leaf, a soft glow, a wooden crate, a ball, factory
// buildings and items.
constexpr u32 kCritterKinds = 8;
constexpr u32 kFrameLeaf = 16;
constexpr u32 kFrameGlow = 17;
constexpr u32 kFrameCrate = 18;
constexpr u32 kFrameBall = 19;
// Factory pieces (drawn facing east where it matters), then items.
constexpr u32 kFrameBelt = 20;
constexpr u32 kFramePipe = 21;
constexpr u32 kFramePole = 22;
constexpr u32 kFrameDrill = 23;
constexpr u32 kFrameFurnace = 24;
constexpr u32 kFrameAssembler = 25;
constexpr u32 kFrameBoiler = 26;
constexpr u32 kFrameEngine = 27;
constexpr u32 kFramePump = 28;
constexpr u32 kFrameChest = 29;
constexpr u32 kFrameOre = 30;
constexpr u32 kFramePlate = 31;
constexpr u32 kFrameGear = 32;
constexpr u32 kFrameCoal = 33;
constexpr u32 kFrameSolid = 34; // plain white, for overlays

struct SheetImage {
    std::vector<u8> rgba;
    u32 width = 0, height = 0;
    std::vector<render::SpriteRect> frames;
    render::SpriteSheet sheet() const {
        return {rgba.data(), width, height, frames.data(), static_cast<u32>(frames.size())};
    }
};
SheetImage make_sprite_sheet();

// How the sample tiles take part in lighting. Side view: sunlight from the
// open sky, rock blocks it. Top down: daylight everywhere, or night.
render::LightRules side_view_light_rules();
render::LightRules top_down_light_rules(bool night);

// A warm torch.
inline render::PointLight torch(f64 x, f64 y) { return {x, y, 2.2f, 1.6f, 0.9f}; }

} // namespace forge::demo
