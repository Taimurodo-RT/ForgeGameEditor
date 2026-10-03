#pragma once

// Simulation zones: how much of the world is alive each tick.
//
//   Active  around every focus (the camera, objects that must stay awake):
//           simulated every tick at full detail.
//   Near    a ring further out: simulated every few ticks with a longer
//           step, so it keeps moving at a fraction of the cost.
//   Asleep  everything else that is loaded, and everything unloaded:
//           not simulated at all.
//
// When a chunk wakes up (becomes Active or Near again), it is reported with
// the number of ticks it slept, so game rules can catch up in one go: crops
// grow, furnaces finish, animals wander somewhere else.

#include "forge/core/types.h"
#include "forge/world/world.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace forge::sim {

enum class Zone : u8 { Asleep = 0, Near = 1, Active = 2 };

struct ZoneDesc {
    i32 active_margin = 1; // chunks around a focus that are Active
    i32 near_margin = 3;   // chunks around a focus that are at least Near
    u32 near_every = 4;    // Near chunks run one tick in this many, with a step this many times longer
};

struct ChunkWake {
    world::ChunkCoord coord;
    u64 slept_ticks = 0;
};

struct ZoneStats {
    u32 active = 0, near = 0, asleep = 0; // loaded chunks in each zone
};

class Zones {
public:
    explicit Zones(const ZoneDesc& desc = {}) : desc_(desc) {}

    // Once per frame, before the ticks: assigns a zone to every loaded chunk
    // from the focus rectangles (in tiles) and lists the chunks that woke up.
    void update(world::World& world, std::span<const world::Rect> focus_tiles, u64 tick);

    Zone zone_of(world::ChunkCoord c) const {
        if (!grid_.empty()) {
            if (!rect_.contains(c.x, c.y)) return Zone::Asleep;
            return static_cast<Zone>(grid_[static_cast<usize>((c.y - rect_.y0) * width_ + (c.x - rect_.x0))]);
        }
        auto it = sparse_.find(c);
        return it == sparse_.end() ? Zone::Asleep : it->second;
    }

    // How many ticks a chunk advances on this tick: 1 when Active, near_every
    // on its turn when Near (chunks take turns so the work spreads evenly
    // over the ticks), else 0.
    u32 ticks_due(world::ChunkCoord c, u64 tick) const {
        const Zone z = zone_of(c);
        if (z == Zone::Active) return 1;
        if (z == Zone::Near) {
            const u32 n = desc_.near_every;
            const u32 turn = static_cast<u32>(c.x * 3 + c.y * 5) % n;
            return (tick % n) == turn ? n : 0;
        }
        return 0;
    }

    std::span<const ChunkWake> woken() const { return woken_; }
    const ZoneDesc& desc() const { return desc_; }
    ZoneStats stats() const { return stats_; }

private:
    ZoneDesc desc_;
    world::Rect rect_;
    i64 width_ = 0;
    std::vector<u8> grid_;
    std::unordered_map<world::ChunkCoord, Zone, world::ChunkCoordHash> sparse_;
    // Last tick each chunk was awake (kept after it sleeps or unloads).
    std::unordered_map<world::ChunkCoord, u64, world::ChunkCoordHash> last_awake_;
    u64 prev_tick_ = 0;
    bool updated_ = false;
    std::vector<ChunkWake> woken_;
    ZoneStats stats_;
};

} // namespace forge::sim
