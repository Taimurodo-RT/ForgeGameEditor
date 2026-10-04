#pragma once

// The vertical slice's world: the sample side-view landscape with a flat
// village around x = 0 (two houses), and an old mine to the west: a stair
// tunnel down, a gallery with a pool, a sand pocket and copper, and a
// chamber at the end where the miner lost his pickaxe.

#include "forge/world/generators.h"

#include <string>

namespace slice {

using forge::f32;
using forge::f64;
using forge::i32;
using forge::u16;
using forge::u32;
using forge::u64;
using forge::u8;
using forge::world::TileId;

// Tiles past the sample ones (forge/world/generators.h).
enum SliceTile : TileId {
    TilePlanks = forge::world::TileSampleCount, // 17: solid boards
    TilePlankWall,                              // background boards
    TileTorch,                                  // background: a torch on the wall
    TileBeam,                                   // background: a support beam
    TileRoof,                                   // solid
    TileWindow,                                 // background
    TileBrick,                                  // solid
    TileSliceCount,
};

// Layers of the world.
constexpr u32 kWalls = 0, kBlocks = 1, kLiquids = 2;
// The first liquid kind added to the simulation.
constexpr u8 kWater = 1;

struct House {
    i32 x0, x1; // outer columns (the side walls)
};

class SliceGenerator final : public forge::world::Generator {
public:
    explicit SliceGenerator(u64 seed);
    void generate(forge::world::ChunkCoord coord, const forge::world::ChunkTiles& out) const override;

    // The ground line of the sample landscape, before the village is flattened.
    i32 raw_surface(i32 x) const;
    // The ground line as generated (the village flattened).
    i32 surface(i32 x) const;

    u64 seed() const { return seed_; }
    i32 village_y() const { return village_y_; } // the village's ground row
    House miner_house() const { return {8, 18}; }
    House smith_house() const { return {-32, -20}; }
    f64 spawn_x() const { return 2.5; }
    f64 spawn_y() const { return village_y_ - 1.0; }

    // The mine: the entrance column and ground row, the gallery row (its
    // floor is one below), and where the pickaxe lies.
    i32 mine_x() const { return kMineX; }
    i32 mine_y() const { return mine_y_; }
    i32 gallery_y() const { return mine_y_ + kStairs - 1; }
    i32 gallery_x0() const { return kMineX - kStairs; }      // east end
    i32 gallery_x1() const { return kMineX - kStairs - 60; } // west end, where the chamber starts
    f64 pickaxe_x() const { return kMineX - kStairs - 66.5; }
    f64 pickaxe_y() const { return gallery_y() - 0.5; }
    i32 pool_x0() const { return kMineX - 100; }
    i32 pool_x1() const { return kMineX - 92; }

    // "Деревня", "Старая шахта", "Пещеры" or "Поверхность".
    std::string location(f64 x, f64 y) const;

    static constexpr i32 kVillageHalf = 70;
    static constexpr i32 kVillageBlend = 110;
    static constexpr i32 kMineX = -150;
    static constexpr i32 kStairs = 80;

private:
    // Changes one tile for the village houses and the mine.
    void shape(i32 x, i32 y, TileId& wall, TileId& block, TileId& liquid, TileId base_block) const;

    forge::world::SideViewGenerator base_;
    u64 seed_;
    i32 village_y_ = 0;
    i32 mine_y_ = 0;
};

} // namespace slice
