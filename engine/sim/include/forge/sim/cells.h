#pragma once

// Liquids and falling tiles: simple cellular rules in the spirit of
// Terraria, not real fluid dynamics.
//
// Liquids live in their own tile layer, so they stream, save and draw with
// the world. Each cell holds one liquid and an amount: kFull is a full tile.
// Every tick a liquid flows down, then spreads to the sides, and (when its
// kind levels) pushes up under pressure, so water poured into one side of a
// U-shaped cave rises on the other side until both levels match.
//
// Falling tiles (sand, gravel) drop when the cell under them is free, sink
// through liquids, and the ones that slide also roll off to the sides,
// piling up into slopes.
//
// Only the parts of chunks where something moved last tick are processed: a
// chunk keeps a small rectangle of cells to look at, which grows around every
// change and is empty once everything settles. Neighbours pouring over an
// edge and edits by the player wake the cells they touch.
// "Down" follows the world's gravity (one of the four axis directions).

#include "forge/core/types.h"
#include "forge/sim/tiles.h"
#include "forge/sim/zones.h"
#include "forge/world/world.h"

#include <atomic>
#include <memory>
#include <vector>

namespace forge::sim {

// A liquid cell: kind in the top 4 bits (0 = none), amount in the low 12.
inline constexpr u16 kFull = 1024;      // a full tile
inline constexpr u16 kMaxAmount = 4095; // squeezed at the bottom of deep liquid
inline constexpr u8 kMaxLiquidKinds = 15;

inline u8 liquid_kind(u16 cell) { return static_cast<u8>(cell >> 12); }
inline u16 liquid_amount(u16 cell) { return static_cast<u16>(cell & 0x0fff); }
inline u16 make_liquid(u8 kind, u16 amount) {
    return amount == 0 || kind == 0 ? u16{0} : static_cast<u16>((kind << 12) | (amount > kMaxAmount ? kMaxAmount : amount));
}

struct LiquidKind {
    // Rises in connected vessels to a common level (the author's switch; on
    // by default). Off: only falls and spreads, like Terraria.
    bool level = true;
    // How fast it spreads sideways: 1 = water, 2 = oil, 4 = lava, 8 = honey.
    u8 thickness = 1;
    // What it does to bodies inside it: slows them (per second) and holds
    // them up (0 = sinks like a stone, 1 = floats without weight).
    f32 drag = 2.5f;
    f32 buoyancy = 0.8f;
    // For rigid bodies: lighter ones float (a crate of density 0.5 floats
    // half under water), heavier ones sink.
    f32 density = 1;
};

struct CellStats {
    u32 active_chunks = 0; // processed in the last tick
    u32 moved = 0;         // cells that changed in the last tick
};

class CellSim {
public:
    CellSim(u32 block_layer, u32 liquid_layer) : block_layer_(block_layer), liquid_layer_(liquid_layer) {}

    // Kinds are numbered 1, 2, ... in the order they are added (0 if full).
    u8 add_liquid(const LiquidKind& kind);
    const LiquidKind& liquid(u8 kind) const { return kinds_[kind]; }
    u8 liquid_count() const { return static_cast<u8>(kinds_.size() - 1); }
    // Liquid a flowing into liquid b turns b's cell into this tile (water
    // reaching lava makes stone). Without a rule, two liquids do not mix.
    void add_reaction(u8 a, u8 b, world::TileId result);
    // Tiles that fall; slides: also rolls off diagonally into slopes.
    void set_falling(world::TileId tile, bool slides);

    u32 block_layer() const { return block_layer_; }
    u32 liquid_layer() const { return liquid_layer_; }

    // Chunks came or went (cheap). Newly loaded chunks are checked once.
    void rebuild(world::World& world, const CollisionRules& rules);
    // Wakes the chunks under a rectangle of tiles (after edits made other
    // than through this class; edits through World::set_tile are noticed
    // on their own by the chunk's revision).
    void wake(const world::Rect& tiles);

    // Adds liquid to a cell (pouring); false if the cell is solid, holds
    // another liquid or its chunk is not loaded.
    bool pour(i32 x, i32 y, u8 kind, u16 amount);

    // One tick over awake chunks that the zones say are due. down: 0 = +y,
    // 1 = -x, 2 = -y, 3 = +x (screen directions: down, left, up, right).
    void step(const Zones& zones, u64 tick, u32 down);

    const CellStats& stats() const { return stats_; }

private:
    // Local cell rectangle, half-open, 0..64.
    struct Box {
        u8 x0 = kNone, y0 = kNone, x1 = 0, y1 = 0;
        static constexpr u8 kNone = 64;
        bool empty() const { return x1 <= x0 || y1 <= y0; }
        void add(i32 x0_, i32 y0_, i32 x1_, i32 y1_);
        static Box all() { return {0, 0, 64, 64}; }
    };
    struct ChunkState {
        world::Chunk* chunk = nullptr;
        world::TileId* blocks = nullptr;
        world::TileId* liquid = nullptr;
        u32 seen_revision = 0;
        Box work;  // cells to look at this tick
        Box next;  // collected for the next tick
        // Set by neighbours during a pass: edges whose strip of cells must be
        // looked at (1 left, 2 right, 4 top, 8 bottom) and 16 if they wrote
        // into this chunk.
        std::atomic<u8> touched{0};
    };

    ChunkState* state_at(world::ChunkCoord c) {
        const i32 x = c.x - rect_.x0, y = c.y - rect_.y0;
        if (static_cast<u32>(x) >= static_cast<u32>(width_) || static_cast<u32>(y) >= static_cast<u32>(height_)) return nullptr;
        const i32 i = grid_[static_cast<usize>(y) * static_cast<usize>(width_) + static_cast<usize>(x)];
        return i < 0 ? nullptr : &states_[static_cast<usize>(i)];
    }
    bool process_chunk(ChunkState& s, u64 tick, u32 down);
    void touch(world::ChunkCoord c, u8 bits);

    u32 block_layer_, liquid_layer_;
    std::vector<LiquidKind> kinds_{LiquidKind{}}; // [0] unused: no liquid
    // reactions_[a * 16 + b]: tile made when a flows into b, 0 = none.
    world::TileId reactions_[256] = {};
    std::vector<u8> falling_ = std::vector<u8>(65536, 0); // 1 falls, 2 falls and slides
    const u8* shapes_ = nullptr;

    world::Rect rect_;
    i32 width_ = 0, height_ = 0;
    std::vector<i32> grid_;
    std::unique_ptr<ChunkState[]> states_;
    usize state_count_ = 0;
    CellStats stats_;
};

} // namespace forge::sim
