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
// The game's hours (forge::level::LevelLight::time; they do not run in the
// game). Night from 21:00 till 4:30: the sky stays at its night light and
// links «Только ночью» happen (SliceLogic::night). Dawn till 8:00, from 8 to
// 17 the light the game always had, dusk till 21:00.
inline constexpr f64 kNightFrom = 21.0, kNightTill = 4.5, kDayFrom = 8.0, kDayTill = 17.0;
bool is_night(f64 hour);
// The sky's light through a day, by those hours.
std::span<const forge::render::SkyKey> day_sky();
// The sky's light at an hour, by day_sky().
forge::Color sky_light(f64 hour);
// What an hour means in the game, for the editor's «Свет».
const char* hour_words(f64 hour);

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
