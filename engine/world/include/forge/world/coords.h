#pragma once

// Tile and chunk coordinates. The world is a grid of tiles split into square
// chunks of kChunkSize × kChunkSize. Tile coordinates are 32-bit and signed,
// so a world can extend ±2 billion tiles from its origin; y grows downward,
// like screen space and like most tile editors.

#include "forge/core/types.h"

namespace forge::world {

inline constexpr i32 kChunkShift = 6;
inline constexpr i32 kChunkSize = 1 << kChunkShift; // 64 tiles
inline constexpr u32 kChunkTiles = static_cast<u32>(kChunkSize * kChunkSize);

using TileId = u16;
inline constexpr TileId kEmptyTile = 0;

struct ChunkCoord {
    i32 x = 0;
    i32 y = 0;
    friend bool operator==(ChunkCoord a, ChunkCoord b) { return a.x == b.x && a.y == b.y; }
    friend bool operator!=(ChunkCoord a, ChunkCoord b) { return !(a == b); }
};

struct ChunkCoordHash {
    usize operator()(ChunkCoord c) const {
        u64 k = (static_cast<u64>(static_cast<u32>(c.x)) << 32) | static_cast<u32>(c.y);
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdull;
        k ^= k >> 33;
        return static_cast<usize>(k);
    }
};

// Half-open rectangle [x0, x1) × [y0, y1), in tiles or in chunks.
struct Rect {
    i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    bool empty() const { return x1 <= x0 || y1 <= y0; }
    bool contains(i32 x, i32 y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    Rect expanded(i32 by) const { return {x0 - by, y0 - by, x1 + by, y1 + by}; }
    Rect clipped(const Rect& to) const {
        return {x0 > to.x0 ? x0 : to.x0, y0 > to.y0 ? y0 : to.y0, x1 < to.x1 ? x1 : to.x1, y1 < to.y1 ? y1 : to.y1};
    }
    friend bool operator==(const Rect& a, const Rect& b) {
        return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
    }
};

// Arithmetic shift rounds toward negative infinity, so tile -1 is in chunk -1.
inline ChunkCoord chunk_of(i32 tile_x, i32 tile_y) { return {tile_x >> kChunkShift, tile_y >> kChunkShift}; }

inline u32 local_index(i32 tile_x, i32 tile_y) {
    return static_cast<u32>(tile_y & (kChunkSize - 1)) * kChunkSize + static_cast<u32>(tile_x & (kChunkSize - 1));
}

// Chunks touched by a rectangle of tiles.
inline Rect chunks_of(const Rect& tiles) {
    if (tiles.empty()) return {};
    return {tiles.x0 >> kChunkShift, tiles.y0 >> kChunkShift, ((tiles.x1 - 1) >> kChunkShift) + 1,
            ((tiles.y1 - 1) >> kChunkShift) + 1};
}

} // namespace forge::world
