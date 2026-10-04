#include "slice_world.h"

#include "forge/sim/cells.h"

#include <algorithm>
#include <cmath>

namespace slice {

using namespace forge::world;
using forge::sim::kFull;
using forge::sim::make_liquid;

namespace {

f32 smoothstep(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Copper in the mine: floor patches and one in the ceiling, enough for the
// smith's order.
bool mine_copper(i32 x, i32 y, i32 east, i32 gy) {
    const bool floor = y == gy + 1 || y == gy + 2;
    const bool ceiling = y == gy - 5 || y == gy - 6;
    return (floor && x <= east - 4 && x >= east - 8) || (floor && x <= east - 26 && x >= east - 32) ||
           (ceiling && x <= east - 50 && x >= east - 56);
}

} // namespace

SliceGenerator::SliceGenerator(u64 seed) : base_(seed), seed_(seed) {
    village_y_ = raw_surface(0);
    mine_y_ = raw_surface(kMineX);
}

// The same line as SideViewGenerator draws (forge/world/generators.cpp).
i32 SliceGenerator::raw_surface(i32 x) const {
    const f32 wx = static_cast<f32>(x);
    const f32 hills = (fractal_noise(seed_, wx * 0.01f, 0.5f, 4) - 0.5f) * 90.0f;
    const f32 mountains = (fractal_noise(seed_ + 1, wx * 0.0015f, 0.5f, 3) - 0.5f) * 400.0f;
    return static_cast<i32>(hills + mountains);
}

i32 SliceGenerator::surface(i32 x) const {
    const i32 r = raw_surface(x);
    const i32 ax = std::abs(x);
    if (ax >= kVillageBlend) return r;
    if (ax <= kVillageHalf) return village_y_;
    const f32 t = smoothstep(static_cast<f32>(ax - kVillageHalf) / static_cast<f32>(kVillageBlend - kVillageHalf));
    return village_y_ + static_cast<i32>(std::lround(static_cast<f32>(r - village_y_) * t));
}

std::string SliceGenerator::location(f64 x, f64 y) const {
    if (x >= gallery_x1() - 16 && x <= kMineX + 2 && y > mine_y_ - 1 && y < gallery_y() + 8) return "Старая шахта";
    const i32 tx = static_cast<i32>(std::floor(x));
    if (std::abs(tx) <= kVillageBlend && y < village_y_ + 10) return "Деревня";
    if (y > surface(tx) + 12) return "Пещеры";
    return "Поверхность";
}

void SliceGenerator::generate(ChunkCoord coord, const ChunkTiles& out) const {
    base_.generate(coord, out);
    const i32 x0 = coord.x * kChunkSize, y0 = coord.y * kChunkSize;
    const i32 x1 = x0 + kChunkSize, y1 = y0 + kChunkSize;
    const bool village = x1 > -kVillageBlend && x0 < kVillageBlend;
    const i32 mine_west = gallery_x1() - 16, mine_east = kMineX + 2;
    const bool mine = x1 > mine_west && x0 <= mine_east && y1 > mine_y_ - 8 && y0 <= gallery_y() + 8;
    if (!village && !mine) return;

    TileId* walls = out.layer(kWalls);
    TileId* blocks = out.layer(kBlocks);
    TileId* liquids = out.layer(kLiquids);
    for (i32 lx = 0; lx < kChunkSize; ++lx) {
        const i32 x = x0 + lx;
        if (village && std::abs(x) < kVillageBlend) {
            const i32 r = raw_surface(x), t = surface(x);
            const i32 top = std::max(y0, std::min(r, t) - 14), bottom = std::min(y1, std::max(r, t) + 15);
            for (i32 y = top; y < bottom; ++y) {
                const u32 i = static_cast<u32>((y - y0) * kChunkSize + lx);
                const i32 depth = y - t;
                if (depth < 0) {
                    walls[i] = blocks[i] = liquids[i] = TileAir;
                } else if (depth == 0) {
                    walls[i] = TileAir;
                    blocks[i] = TileGrass;
                } else if (depth < 14) {
                    walls[i] = depth < 8 ? TileDirtWall : TileStoneWall;
                    blocks[i] = depth < 8 ? TileDirt : TileStone;
                } else if (y < r) {
                    walls[i] = TileStoneWall;
                    blocks[i] = TileStone;
                }
                if (depth < 0 && depth >= -14) shape(x, y, walls[i], blocks[i], liquids[i], blocks[i]);
                else if (depth == 0) shape(x, y, walls[i], blocks[i], liquids[i], blocks[i]);
            }
        }
        if (mine && x > mine_west && x <= mine_east) {
            const i32 top = std::max(y0, mine_y_ - 8), bottom = std::min(y1, gallery_y() + 8);
            for (i32 y = top; y < bottom; ++y) {
                const u32 i = static_cast<u32>((y - y0) * kChunkSize + lx);
                shape(x, y, walls[i], blocks[i], liquids[i], blocks[i]);
            }
        }
    }
}

void SliceGenerator::shape(i32 x, i32 y, TileId& wall, TileId& block, TileId& liquid, TileId base_block) const {
    // --- village houses ------------------------------------------------------
    const i32 v = village_y_;
    for (const House h : {miner_house(), smith_house()}) {
        if (x < h.x0 - 1 || x > h.x1 + 1 || y > v || y < v - 12) continue;
        const bool inside = x > h.x0 && x < h.x1;
        if (y == v && x >= h.x0 && x <= h.x1) block = TilePlanks; // floor
        if (y < v && y >= v - 6 && x >= h.x0 && x <= h.x1) {
            block = TileAir;
            wall = inside ? TilePlankWall : TileBeam;
            // Side walls above a door opening three tiles high.
            if (!inside && y <= v - 4) block = TilePlanks;
            const i32 mid = (h.x0 + h.x1) / 2;
            if (inside && y >= v - 5 && y <= v - 4 && ((x >= h.x0 + 2 && x <= h.x0 + 3) || (x >= h.x1 - 3 && x <= h.x1 - 2)))
                wall = TileWindow;
            if (x == mid && y == v - 4) wall = TileTorch;
        }
        // Roof: three rows stepping in.
        if (y == v - 7 && x >= h.x0 - 1 && x <= h.x1 + 1) block = TileRoof;
        if (y == v - 8 && x >= h.x0 && x <= h.x1) block = TileRoof;
        if (y == v - 9 && x >= h.x0 + 2 && x <= h.x1 - 2) block = TileRoof;
        // The smith's chimney.
        if (h.x0 == smith_house().x0 && x == h.x1 - 2 && y >= v - 12 && y <= v - 10) block = TileBrick;
    }

    // --- the mine --------------------------------------------------------------
    const i32 mx = kMineX, sy = mine_y_, gy = gallery_y();
    // Entrance: a beam and a lintel over the opening.
    if (y == sy - 4 && x >= mx - 2 && x <= mx + 1) block = TilePlanks;
    if (x == mx + 1 && y >= sy - 3 && y <= sy) {
        if (block == TileAir) wall = TileBeam;
    }
    // Stairs: one tile down for every tile west, four tiles high, on boards.
    const i32 i = mx - x;
    if (i >= 0 && i < kStairs) {
        const i32 top = sy + i - 3, bottom = sy + i;
        if (y >= top && y <= bottom) {
            block = TileAir;
            liquid = TileAir;
            wall = i % 10 == 5 ? TileBeam : TilePlankWall;
            if (i % 14 == 7 && y == top + 1) wall = TileTorch;
        } else if (y == bottom + 1) {
            block = TilePlanks;
        } else if (y == top - 1 && block == TileAir && i > 2) {
            block = TileStone; // a cave crossing the stairs does not open the roof
        }
    }
    // Gallery: west from the foot of the stairs.
    const i32 east = gallery_x0(), west = gallery_x1();
    if (x <= east && x >= west) {
        const i32 along = east - x;
        if (y >= gy - 4 && y <= gy) {
            block = TileAir;
            liquid = TileAir;
            wall = along % 10 == 0 ? TileBeam : TilePlankWall;
            if (along % 12 == 6 && y == gy - 3) wall = TileTorch;
        }
        const bool pool = x >= pool_x0() && x <= pool_x1();
        if (pool && y >= gy + 1 && y <= gy + 3) {
            block = TileAir;
            liquid = make_liquid(kWater, kFull);
            wall = TileStoneWall;
        } else if ((x >= pool_x0() - 1 && x <= pool_x1() + 1 && y >= gy + 1 && y <= gy + 4)) {
            block = TileStone; // the pool's banks and bottom
            liquid = TileAir;
        } else if (y == gy + 1 && base_block == TileAir) {
            block = TileStone;
        }
        // A pocket of sand over boards: dig the boards and it pours down.
        const bool pocket = along >= 35 && along <= 45;
        if (pocket && y == gy - 5) block = TilePlanks;
        if (pocket && y >= gy - 9 && y <= gy - 6) block = TileSand;
        if (mine_copper(x, y, east, gy)) block = TileCopper;
    }
    // Chamber at the west end, where the pickaxe lies.
    const f32 cx = static_cast<f32>(west - 6), cy = static_cast<f32>(gy - 3);
    const f32 dx = (static_cast<f32>(x) - cx) / 6.5f, dy = (static_cast<f32>(y) - cy) / 4.5f;
    if (dx * dx + dy * dy <= 1.0f && y <= gy) {
        block = TileAir;
        liquid = TileAir;
        wall = TileStoneWall;
        if ((x == static_cast<i32>(cx) - 4 || x == static_cast<i32>(cx) + 4) && y == gy - 3) wall = TileTorch;
    } else if (y == gy + 1 && std::fabs(static_cast<f32>(x) - cx) <= 7.0f) {
        block = TileStone;
    }
}

} // namespace slice
