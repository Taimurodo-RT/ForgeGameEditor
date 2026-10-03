#include "forge/sim/zones.h"

#include "forge/core/profile.h"

#include <algorithm>

namespace forge::sim {

using world::ChunkCoord;
using world::Rect;

namespace {
constexpr i64 kMaxGridCells = 1 << 20;
}

void Zones::update(world::World& world, std::span<const Rect> focus_tiles, u64 tick) {
    FORGE_ZONE_N("Zones update");
    std::vector<ChunkCoord> loaded;
    Rect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    world.for_each_ready([&](world::Chunk& c) {
        loaded.push_back(c.coord);
        box.x0 = std::min(box.x0, c.coord.x);
        box.y0 = std::min(box.y0, c.coord.y);
        box.x1 = std::max(box.x1, c.coord.x + 1);
        box.y1 = std::max(box.y1, c.coord.y + 1);
    });

    std::vector<Rect> focus_chunks;
    focus_chunks.reserve(focus_tiles.size());
    for (const Rect& f : focus_tiles)
        if (!f.empty()) focus_chunks.push_back(world::chunks_of(f));

    grid_.clear();
    sparse_.clear();
    if (!loaded.empty() && static_cast<i64>(box.x1 - box.x0) * (box.y1 - box.y0) <= kMaxGridCells) {
        rect_ = box;
        width_ = box.x1 - box.x0;
        grid_.assign(static_cast<usize>(width_) * static_cast<usize>(box.y1 - box.y0), 0);
        // Paint each focus: the Near ring first, then the Active core.
        for (const Rect& f : focus_chunks) {
            for (const auto& [margin, zone] : {std::pair{desc_.near_margin, Zone::Near},
                                               std::pair{desc_.active_margin, Zone::Active}}) {
                const Rect r = f.expanded(margin).clipped(box);
                for (i32 y = r.y0; y < r.y1; ++y) {
                    u8* row = grid_.data() + static_cast<usize>((y - box.y0) * width_);
                    for (i32 x = r.x0; x < r.x1; ++x)
                        row[x - box.x0] = std::max(row[x - box.x0], static_cast<u8>(zone));
                }
            }
        }
    } else {
        // Loaded chunks spread too far apart for a grid: one by one.
        for (ChunkCoord c : loaded) {
            Zone z = Zone::Asleep;
            for (const Rect& f : focus_chunks) {
                if (f.expanded(desc_.active_margin).contains(c.x, c.y)) {
                    z = Zone::Active;
                    break;
                }
                if (f.expanded(desc_.near_margin).contains(c.x, c.y)) z = Zone::Near;
            }
            if (z != Zone::Asleep) sparse_[c] = z;
        }
    }

    stats_ = {};
    woken_.clear();
    for (ChunkCoord c : loaded) {
        const Zone z = zone_of(c);
        if (z == Zone::Active) ++stats_.active;
        else if (z == Zone::Near) ++stats_.near;
        else ++stats_.asleep;
        if (z == Zone::Asleep) continue;
        auto [it, added] = last_awake_.try_emplace(c, tick);
        // Awake now but not at the previous update: it slept in between.
        if (!added && updated_ && it->second < prev_tick_) woken_.push_back({c, tick - it->second});
        it->second = tick;
    }
    prev_tick_ = tick;
    updated_ = true;
}

} // namespace forge::sim
