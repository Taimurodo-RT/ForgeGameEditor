#pragma once

// How tiles block moving things, and a fast read-only view of the loaded
// tiles for the simulation threads.

#include "forge/core/types.h"
#include "forge/world/world.h"

#include <vector>

namespace forge::sim {

enum class TileShape : u8 {
    Empty,
    Solid,    // blocks from every side
    Platform, // one way: stands on top, passes through from below and the sides
};

// Which tile ids are solid, per game. One byte per possible id.
class CollisionRules {
public:
    explicit CollisionRules(u32 layer = 1) : layer_(layer), shapes_(65536, static_cast<u8>(TileShape::Empty)) {}

    void set(world::TileId id, TileShape shape) { shapes_[id] = static_cast<u8>(shape); }
    TileShape shape(world::TileId id) const { return static_cast<TileShape>(shapes_[id]); }
    u32 layer() const { return layer_; } // the tile layer that collides
    const u8* table() const { return shapes_.data(); }

private:
    u32 layer_;
    std::vector<u8> shapes_;
};

// Tile shapes of every loaded chunk, readable from any thread while nothing
// changes the set of loaded chunks (tile edits are seen right away: the view
// points at the chunks' own arrays). Tiles in chunks that are not loaded
// count as Solid, so nothing falls out of the world into the unknown.
class TileView {
public:
    // Call when chunks came or went (cheap: two pointers per chunk).
    // liquid_layer: the layer holding liquids, if the world has one.
    void rebuild(world::World& world, const CollisionRules& rules, u32 liquid_layer = ~0u);

    TileShape shape(i32 x, i32 y) const {
        const i32 cx = (x >> world::kChunkShift) - rect_.x0;
        const i32 cy = (y >> world::kChunkShift) - rect_.y0;
        if (static_cast<u32>(cx) >= static_cast<u32>(width_) || static_cast<u32>(cy) >= static_cast<u32>(height_))
            return TileShape::Solid;
        const world::TileId* tiles = chunks_[static_cast<usize>(cy) * static_cast<usize>(width_) + static_cast<usize>(cx)];
        if (!tiles) return TileShape::Solid;
        return static_cast<TileShape>(table_[tiles[world::local_index(x, y)]]);
    }

    bool solid(i32 x, i32 y) const { return shape(x, y) == TileShape::Solid; }

    // The liquid cell at a tile (see cells.h), 0 when none or not loaded.
    u16 liquid(i32 x, i32 y) const {
        if (liquids_.empty()) return 0;
        const i32 cx = (x >> world::kChunkShift) - rect_.x0;
        const i32 cy = (y >> world::kChunkShift) - rect_.y0;
        if (static_cast<u32>(cx) >= static_cast<u32>(width_) || static_cast<u32>(cy) >= static_cast<u32>(height_)) return 0;
        const world::TileId* l = liquids_[static_cast<usize>(cy) * static_cast<usize>(width_) + static_cast<usize>(cx)];
        return l ? l[world::local_index(x, y)] : 0;
    }

private:
    world::Rect rect_;
    i32 width_ = 0, height_ = 0;
    std::vector<const world::TileId*> chunks_;
    std::vector<const world::TileId*> liquids_;
    const u8* table_ = nullptr;
};

} // namespace forge::sim
