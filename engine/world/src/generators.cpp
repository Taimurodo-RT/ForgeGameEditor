#include "forge/world/generators.h"

#include "forge/core/profile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace forge::world {

namespace {

u64 mix(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

f32 corner(u64 seed, i32 x, i32 y) { return static_cast<f32>(hash_tile(seed, x, y)) * (1.0f / 4294967296.0f); }

f32 smooth(f32 t) { return t * t * (3.0f - 2.0f * t); }

// Smooth noise sampled every kStep tiles across a chunk and filled in
// bilinearly. Low-frequency fields (hills, biomes, caves) look the same and
// cost a fraction: 17 × 17 samples instead of 64 × 64.
constexpr i32 kStep = 4;
constexpr i32 kGrid = kChunkSize / kStep + 1;

struct ChunkField {
    f32 grid[kGrid * kGrid];

    ChunkField(u64 seed, i32 x0, i32 y0, f32 fx, f32 fy, u32 octaves) {
        for (i32 gy = 0; gy < kGrid; ++gy)
            for (i32 gx = 0; gx < kGrid; ++gx)
                grid[gy * kGrid + gx] = fractal_noise(seed, static_cast<f32>(x0 + gx * kStep) * fx,
                                                      static_cast<f32>(y0 + gy * kStep) * fy, octaves);
    }

    f32 at(i32 lx, i32 ly) const {
        const i32 gx = lx / kStep, gy = ly / kStep;
        const f32 tx = static_cast<f32>(lx % kStep) * (1.0f / kStep);
        const f32 ty = static_cast<f32>(ly % kStep) * (1.0f / kStep);
        const f32* r0 = grid + gy * kGrid + gx;
        const f32* r1 = r0 + kGrid;
        const f32 top = r0[0] + (r0[1] - r0[0]) * tx;
        const f32 bottom = r1[0] + (r1[1] - r1[0]) * tx;
        return top + (bottom - top) * ty;
    }
};

} // namespace

u32 hash_tile(u64 seed, i32 x, i32 y) {
    const u64 k = (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(y);
    return static_cast<u32>(mix(k ^ mix(seed)) >> 32);
}

f32 value_noise(u64 seed, f32 x, f32 y) {
    const f32 fx = std::floor(x), fy = std::floor(y);
    const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy);
    const f32 tx = smooth(x - fx), ty = smooth(y - fy);
    const f32 a = corner(seed, ix, iy), b = corner(seed, ix + 1, iy);
    const f32 c = corner(seed, ix, iy + 1), d = corner(seed, ix + 1, iy + 1);
    const f32 top = a + (b - a) * tx;
    const f32 bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

f32 fractal_noise(u64 seed, f32 x, f32 y, u32 octaves) {
    f32 sum = 0, amplitude = 0.5f, total = 0, frequency = 1;
    for (u32 i = 0; i < octaves; ++i) {
        sum += value_noise(seed + i * 0x9E3779B97F4A7C15ull, x * frequency, y * frequency) * amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return sum / total;
}

// --- side view -------------------------------------------------------------

void SideViewGenerator::generate(ChunkCoord coord, const ChunkTiles& out) const {
    FORGE_ZONE_N("Generate side view");
    TileId* walls = out.layer(0);
    TileId* blocks = out.layer(1);
    const i32 x0 = coord.x * kChunkSize;
    const i32 y0 = coord.y * kChunkSize;

    i32 surface[kChunkSize];
    i32 dirt_depth[kChunkSize];
    i32 highest = INT32_MAX;
    for (i32 lx = 0; lx < kChunkSize; ++lx) {
        const f32 wx = static_cast<f32>(x0 + lx);
        const f32 hills = (fractal_noise(seed_, wx * 0.01f, 0.5f, 4) - 0.5f) * 90.0f;
        const f32 mountains = (fractal_noise(seed_ + 1, wx * 0.0015f, 0.5f, 3) - 0.5f) * 400.0f;
        surface[lx] = surface_y_ + static_cast<i32>(hills + mountains);
        dirt_depth[lx] = 6 + static_cast<i32>(value_noise(seed_ + 5, wx * 0.05f, 0.5f) * 12.0f);
        highest = std::min(highest, surface[lx]);
    }

    // Sky only: the common case above ground.
    if (y0 + kChunkSize <= highest) {
        std::memset(out.data, 0, sizeof(TileId) * kChunkTiles * out.layer_count);
        return;
    }

    const ChunkField tunnels(seed_ + 2, x0, y0, 0.025f, 0.04f, 3);
    const ChunkField caverns(seed_ + 3, x0, y0, 0.012f, 0.016f, 2);
    for (i32 ly = 0; ly < kChunkSize; ++ly) {
        const i32 wy = y0 + ly;
        for (i32 lx = 0; lx < kChunkSize; ++lx) {
            const u32 i = static_cast<u32>(ly * kChunkSize + lx);
            const i32 wx = x0 + lx;
            const i32 depth = wy - surface[lx];
            if (depth < 0) {
                walls[i] = TileAir;
                blocks[i] = TileAir;
                continue;
            }
            const bool dirt = depth < dirt_depth[lx];
            walls[i] = depth == 0 ? TileAir : (dirt ? TileDirtWall : TileStoneWall);
            TileId block = depth == 0 ? TileGrass : (dirt ? TileDirt : TileStone);

            if (depth > 12) {
                // Winding tunnels: thin bands where the noise crosses its middle.
                const f32 tunnel = std::fabs(tunnels.at(lx, ly) - 0.5f);
                const f32 width = 0.03f + std::min(static_cast<f32>(depth), 2000.0f) * 0.00001f;
                // Large caverns deeper down.
                const bool cavern = depth > 80 && caverns.at(lx, ly) > 0.7f;
                if (tunnel < width || cavern) {
                    block = TileAir;
                } else if (block == TileStone) {
                    const f32 ore = value_noise(seed_ + 4, static_cast<f32>(wx) * 0.18f, static_cast<f32>(wy) * 0.18f);
                    if (ore > 0.86f) block = depth < 400 ? TileCopper : (depth < 1500 ? TileIron : TileGold);
                }
            }
            blocks[i] = block;
        }
    }
    for (u32 l = 2; l < out.layer_count; ++l) std::memset(out.layer(l), 0, sizeof(TileId) * kChunkTiles);
}

// --- top down --------------------------------------------------------------

void EmptyGenerator::generate(ChunkCoord coord, const ChunkTiles& out) const {
    (void)coord;
    std::fill(out.data, out.data + static_cast<usize>(out.layer_count) * kChunkTiles, kEmptyTile);
}

void TopDownGenerator::generate(ChunkCoord coord, const ChunkTiles& out) const {
    FORGE_ZONE_N("Generate top down");
    TileId* ground = out.layer(0);
    TileId* objects = out.layer_count > 1 ? out.layer(1) : nullptr;
    const i32 x0 = coord.x * kChunkSize;
    const i32 y0 = coord.y * kChunkSize;

    const ChunkField heights(seed_, x0, y0, 0.004f, 0.004f, 5);
    const ChunkField moistures(seed_ + 7, x0, y0, 0.003f, 0.003f, 3);
    const ChunkField colds(seed_ + 9, x0, y0, 0.0015f, 0.0015f, 2);
    for (i32 ly = 0; ly < kChunkSize; ++ly) {
        for (i32 lx = 0; lx < kChunkSize; ++lx) {
            const u32 i = static_cast<u32>(ly * kChunkSize + lx);
            const i32 wx = x0 + lx, wy = y0 + ly;
            const f32 height = heights.at(lx, ly);
            const f32 moisture = moistures.at(lx, ly);
            const f32 cold = colds.at(lx, ly);
            const u32 dice = hash_tile(seed_ + 11, wx, wy) % 100;

            TileId g, o = TileAir;
            if (height < 0.36f) g = TileDeepWater;
            else if (height < 0.42f) g = TileWater;
            else if (height < 0.45f) g = TileSand;
            else if (cold < 0.32f) g = TileSnow;
            else if (moisture > 0.56f) g = TileForest;
            else g = TileMeadow;

            if (g == TileForest && dice < 30) o = TileTree;
            else if ((g == TileMeadow || g == TileSnow) && dice < 3) o = TileTree;
            if (height > 0.62f && g != TileForest && dice >= 90) o = TileRock;

            ground[i] = g;
            if (objects) objects[i] = o;
        }
    }
    for (u32 l = 2; l < out.layer_count; ++l) std::memset(out.layer(l), 0, sizeof(TileId) * kChunkTiles);
}

} // namespace forge::world
