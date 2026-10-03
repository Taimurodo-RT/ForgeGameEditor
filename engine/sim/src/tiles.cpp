#include "forge/sim/tiles.h"

#include "forge/core/log.h"
#include "forge/core/profile.h"

#include <algorithm>

namespace forge::sim {

namespace {
// A focus far from another (two players at opposite ends of a huge world)
// would make the grid huge; beyond this the far chunks read as Solid.
constexpr i64 kMaxGridCells = 1 << 22;
} // namespace

void TileView::rebuild(world::World& world, const CollisionRules& rules, u32 liquid_layer) {
    FORGE_ZONE_N("Tile view rebuild");
    table_ = rules.table();
    world::Rect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    world.for_each_ready([&](world::Chunk& c) {
        box.x0 = std::min(box.x0, c.coord.x);
        box.y0 = std::min(box.y0, c.coord.y);
        box.x1 = std::max(box.x1, c.coord.x + 1);
        box.y1 = std::max(box.y1, c.coord.y + 1);
    });
    chunks_.clear();
    liquids_.clear();
    if (box.empty()) {
        rect_ = {};
        width_ = height_ = 0;
        return;
    }
    if (static_cast<i64>(box.x1 - box.x0) * (box.y1 - box.y0) > kMaxGridCells) {
        FORGE_WARN("sim: loaded chunks span %d x %d chunks; tile collisions limited to a part of them",
                   box.x1 - box.x0, box.y1 - box.y0);
        box.x1 = std::min(box.x1, box.x0 + 2048);
        box.y1 = std::min(box.y1, box.y0 + 2048);
    }
    rect_ = box;
    width_ = box.x1 - box.x0;
    height_ = box.y1 - box.y0;
    chunks_.assign(static_cast<usize>(width_) * static_cast<usize>(height_), nullptr);
    const bool liquids = liquid_layer < world.layer_count();
    if (liquids) liquids_.assign(chunks_.size(), nullptr);
    const u32 layer = std::min(rules.layer(), world.layer_count() - 1);
    world.for_each_ready([&](world::Chunk& c) {
        if (!box.contains(c.coord.x, c.coord.y)) return;
        const usize i = static_cast<usize>(c.coord.y - box.y0) * static_cast<usize>(width_) + static_cast<usize>(c.coord.x - box.x0);
        chunks_[i] = c.layer(layer);
        if (liquids) liquids_[i] = c.layer(liquid_layer);
    });
}

} // namespace forge::sim
