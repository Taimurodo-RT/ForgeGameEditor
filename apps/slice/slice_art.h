#pragma once

// Art of the vertical slice, drawn in code like the demos' placeholder art:
// the sample tiles plus boards, roofs and torches, and a sprite sheet with
// the demo critters, people (two cells tall) and items.

#include "demo_art.h"
#include "slice_world.h"

namespace slice {

std::vector<u8> make_atlas();
forge::demo::SheetImage make_sheet();
forge::render::LightRules light_rules();

// Frames past the demo sheet's (forge::demo::kFrame*). People are 1 × 2 tiles.
enum Frame : u32 {
    FrameHero = 40, // idle; +1, +2 walking; +3 in the air
    FrameMiner = 44, // +1 walking
    FrameSmith = 46, // +1 walking
    FramePickaxe = 48,
    FrameCoins,
    FrameDust,
    FrameSpark,
    FrameWood,
    FrameStone,
    FrameDirt,
    FrameSand,
    FrameTorch, // 56
    FrameAnvil,
    FrameFlame,
    FrameDrop,
    FrameKey, // 60
    FrameDoor, // a piece of a closed door (one tile)
    FrameCount = 64,
};

// Tiles that block bodies and light.
bool is_solid(TileId t);
// Seconds to break a block with bare hands (0: cannot be broken by hand).
f32 hand_time(TileId t);
// Seconds with the pickaxe.
f32 pickaxe_time(TileId t);
// The inventory item a broken block gives ("dirt", "stone"...), or "".
const char* drop_of(TileId t);

} // namespace slice
