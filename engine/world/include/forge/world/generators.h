#pragma once

// Sample world generators: enough to stress the world with realistic data
// and to show that one world system serves different genres. Games will
// build their own generators from nodes later; these stay as templates.

#include "forge/world/world.h"

namespace forge::world {

// Tile ids the samples use. A real project gets its ids from the tile set
// asset; tile 0 is always empty.
enum SampleTile : TileId {
    TileAir = 0,
    TileGrass,
    TileDirt,
    TileStone,
    TileSand,
    TileCopper,
    TileIron,
    TileGold,
    TileDirtWall,
    TileStoneWall,
    TileWater,
    TileDeepWater,
    TileMeadow,
    TileForest,
    TileTree,
    TileRock,
    TileSnow,
    TileSampleCount,
};

// Side view, like Terraria. Layer 0: background walls, layer 1: blocks.
// Rolling surface, dirt over stone, winding caves, ore veins deeper down.
class SideViewGenerator final : public Generator {
public:
    explicit SideViewGenerator(u64 seed, i32 surface_y = 0) : seed_(seed), surface_y_(surface_y) {}
    void generate(ChunkCoord coord, const ChunkTiles& out) const override;

private:
    u64 seed_;
    i32 surface_y_;
};

// Top down, like Factorio or an RPG overworld. Layer 0: ground (biomes,
// lakes), layer 1: objects on it (trees, rocks).
class TopDownGenerator final : public Generator {
public:
    explicit TopDownGenerator(u64 seed) : seed_(seed) {}
    void generate(ChunkCoord coord, const ChunkTiles& out) const override;

private:
    u64 seed_;
};

// Deterministic noise used by the samples, in [0, 1). Exposed for tests and
// for game generators.
f32 value_noise(u64 seed, f32 x, f32 y);
f32 fractal_noise(u64 seed, f32 x, f32 y, u32 octaves);
u32 hash_tile(u64 seed, i32 x, i32 y);

} // namespace forge::world
