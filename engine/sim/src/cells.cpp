#include "forge/sim/cells.h"

#include "forge/core/jobs.h"
#include "forge/core/profile.h"

#include <algorithm>
#include <unordered_map>

namespace forge::sim {

using world::ChunkCoord;
using world::kChunkShift;
using world::kChunkSize;
using world::TileId;

namespace {

constexpr u16 kCompress = 32;  // extra amount a cell holds per full cell above it (pressure)
constexpr u16 kEvaporate = 6;  // thinner films on a floor dry up, so spreading ends
constexpr i32 kStrip = 2;      // cells along an edge a neighbour's change wakes
constexpr u8 kLeft = 1, kRight = 2, kTop = 4, kBottom = 8, kWritten = 16;

// How much of two stacked cells' total the lower one holds.
i32 stable_lower(i32 total) {
    if (total <= kFull) return kFull;
    if (total < 2 * kFull + kCompress) return (kFull * kFull + total * kCompress) / (kFull + kCompress);
    return (total + kCompress) / 2;
}

struct Dir {
    i32 x, y;
};
// down, and the sideways direction, for each gravity direction.
constexpr Dir kDown[4] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
constexpr Dir kSide[4] = {{1, 0}, {0, 1}, {1, 0}, {0, 1}};

} // namespace

void CellSim::Box::add(i32 ax0, i32 ay0, i32 ax1, i32 ay1) {
    ax0 = std::max(ax0, 0);
    ay0 = std::max(ay0, 0);
    ax1 = std::min(ax1, kChunkSize);
    ay1 = std::min(ay1, kChunkSize);
    if (ax1 <= ax0 || ay1 <= ay0) return;
    if (empty()) {
        x0 = static_cast<u8>(ax0), y0 = static_cast<u8>(ay0), x1 = static_cast<u8>(ax1), y1 = static_cast<u8>(ay1);
        return;
    }
    x0 = static_cast<u8>(std::min<i32>(x0, ax0));
    y0 = static_cast<u8>(std::min<i32>(y0, ay0));
    x1 = static_cast<u8>(std::max<i32>(x1, ax1));
    y1 = static_cast<u8>(std::max<i32>(y1, ay1));
}

u8 CellSim::add_liquid(const LiquidKind& kind) {
    if (kinds_.size() > kMaxLiquidKinds) return 0;
    kinds_.push_back(kind);
    if (kinds_.back().thickness == 0) kinds_.back().thickness = 1;
    return static_cast<u8>(kinds_.size() - 1);
}

void CellSim::add_reaction(u8 a, u8 b, TileId result) {
    if (a > kMaxLiquidKinds || b > kMaxLiquidKinds) return;
    reactions_[a * 16 + b] = result;
}

void CellSim::set_falling(TileId tile, bool slides) { falling_[tile] = slides ? 2 : 1; }

void CellSim::rebuild(world::World& world, const CollisionRules& rules) {
    FORGE_ZONE_N("Cells rebuild");
    shapes_ = rules.table();
    // Keep what is known about chunks that stay.
    struct Kept {
        u32 seen_revision;
        Box work;
    };
    std::unordered_map<ChunkCoord, Kept, world::ChunkCoordHash> kept;
    for (usize i = 0; i < state_count_; ++i) {
        const ChunkState& s = states_[i];
        kept[s.chunk->coord] = {s.seen_revision, s.work};
    }

    std::vector<world::Chunk*> chunks;
    world::Rect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    const bool has_layers = world.layer_count() > std::max(block_layer_, liquid_layer_);
    world.for_each_ready([&](world::Chunk& c) {
        chunks.push_back(&c);
        box.x0 = std::min(box.x0, c.coord.x);
        box.y0 = std::min(box.y0, c.coord.y);
        box.x1 = std::max(box.x1, c.coord.x + 1);
        box.y1 = std::max(box.y1, c.coord.y + 1);
    });
    state_count_ = 0;
    grid_.clear();
    width_ = height_ = 0;
    if (chunks.empty() || !has_layers) return;
    // Same limit as the tile view: chunks beyond it are not simulated.
    if (static_cast<i64>(box.x1 - box.x0) * (box.y1 - box.y0) > (i64{1} << 22)) {
        box.x1 = std::min(box.x1, box.x0 + 2048);
        box.y1 = std::min(box.y1, box.y0 + 2048);
    }
    rect_ = box;
    width_ = box.x1 - box.x0;
    height_ = box.y1 - box.y0;
    grid_.assign(static_cast<usize>(width_) * static_cast<usize>(height_), -1);
    states_.reset(new ChunkState[chunks.size()]);
    for (world::Chunk* c : chunks) {
        if (!box.contains(c->coord.x, c->coord.y)) continue;
        ChunkState& s = states_[state_count_];
        s.chunk = c;
        s.blocks = c->layer(block_layer_);
        s.liquid = c->layer(liquid_layer_);
        auto it = kept.find(c->coord);
        if (it != kept.end()) {
            s.seen_revision = it->second.seen_revision;
            s.work = it->second.work;
        } else {
            s.seen_revision = c->revision;
            s.work = Box::all(); // new here: check it once
        }
        grid_[static_cast<usize>(c->coord.y - box.y0) * static_cast<usize>(width_) + static_cast<usize>(c->coord.x - box.x0)] =
            static_cast<i32>(state_count_);
        ++state_count_;
    }
}

void CellSim::wake(const world::Rect& tiles) {
    const world::Rect cr = world::chunks_of(tiles);
    for (i32 y = cr.y0; y < cr.y1; ++y)
        for (i32 x = cr.x0; x < cr.x1; ++x)
            if (ChunkState* s = state_at({x, y})) s->work = Box::all();
}

bool CellSim::pour(i32 x, i32 y, u8 kind, u16 amount) {
    ChunkState* s = state_at(world::chunk_of(x, y));
    if (!s || kind == 0 || kind > liquid_count()) return false;
    const u32 i = world::local_index(x, y);
    if (shapes_[s->blocks[i]] == static_cast<u8>(TileShape::Solid)) return false;
    const u16 cell = s->liquid[i];
    if (cell != 0 && liquid_kind(cell) != kind) return false;
    s->liquid[i] = make_liquid(kind, static_cast<u16>(std::min<u32>(liquid_amount(cell) + amount, kMaxAmount)));
    ++s->chunk->revision;
    s->chunk->edited = true;
    s->seen_revision = s->chunk->revision;
    s->work.add(static_cast<i32>(i % kChunkSize) - 1, static_cast<i32>(i / kChunkSize) - 1,
                static_cast<i32>(i % kChunkSize) + 2, static_cast<i32>(i / kChunkSize) + 2);
    return true;
}

void CellSim::touch(ChunkCoord c, u8 bits) {
    if (ChunkState* n = state_at(c)) n->touched.fetch_or(bits, std::memory_order_relaxed);
}

void CellSim::step(const Zones& zones, u64 tick, u32 down) {
    FORGE_ZONE_N("Cells");
    stats_ = {};
    down &= 3u;
    // Edits from outside (digging, building): the whole chunk is looked at,
    // and the edges of its neighbours.
    for (usize i = 0; i < state_count_; ++i) {
        ChunkState& s = states_[i];
        if (s.chunk->revision == s.seen_revision) continue;
        s.seen_revision = s.chunk->revision;
        s.work = Box::all();
        const ChunkCoord c = s.chunk->coord;
        touch({c.x - 1, c.y}, kRight);
        touch({c.x + 1, c.y}, kLeft);
        touch({c.x, c.y - 1}, kBottom);
        touch({c.x, c.y + 1}, kTop);
    }
    for (usize i = 0; i < state_count_; ++i) {
        ChunkState& s = states_[i];
        const u8 t = s.touched.exchange(0, std::memory_order_relaxed);
        if (t & kLeft) s.work.add(0, 0, kStrip, kChunkSize);
        if (t & kRight) s.work.add(kChunkSize - kStrip, 0, kChunkSize, kChunkSize);
        if (t & kTop) s.work.add(0, 0, kChunkSize, kStrip);
        if (t & kBottom) s.work.add(0, kChunkSize - kStrip, kChunkSize, kChunkSize);
    }

    // Four passes in a 2 × 2 pattern: chunks processed at the same time are
    // never neighbours, so a cell flowing over a chunk edge cannot collide
    // with another thread.
    std::vector<ChunkState*> pass;
    std::atomic<u32> moved{0};
    for (u32 color = 0; color < 4; ++color) {
        pass.clear();
        for (usize i = 0; i < state_count_; ++i) {
            ChunkState& s = states_[i];
            const ChunkCoord c = s.chunk->coord;
            if (s.work.empty() || static_cast<u32>((c.x & 1) | ((c.y & 1) << 1)) != color) continue;
            if (zones.ticks_due(c, tick) == 0) continue;
            pass.push_back(&s);
        }
        stats_.active_chunks += static_cast<u32>(pass.size());
        // A few chunks per job: most have only a few cells to look at.
        jobs::parallel_for(static_cast<u32>(pass.size()), 4, [&](u32 b, u32 e) {
            for (u32 k = b; k < e; ++k) {
                ChunkState& s = *pass[k];
                s.next = {};
                if (process_chunk(s, tick, down)) moved.fetch_add(1, std::memory_order_relaxed);
                s.work = s.next;
            }
        });
    }
    // Chunks that neighbours changed: new revision for the renderer and the
    // save; the strips they touched are looked at next tick.
    for (usize i = 0; i < state_count_; ++i) {
        ChunkState& s = states_[i];
        const u8 t = s.touched.load(std::memory_order_relaxed);
        if (t & kWritten) {
            ++s.chunk->revision;
            s.chunk->edited = true;
            s.touched.fetch_and(static_cast<u8>(~kWritten), std::memory_order_relaxed);
        }
        s.seen_revision = s.chunk->revision;
    }
    stats_.moved = moved.load();
}

// Processes one chunk from its "bottom" row up. Returns true if anything
// changed (in it or, through its edges, in a neighbour).
bool CellSim::process_chunk(ChunkState& s, u64 tick, u32 down) {
    const Dir d = kDown[down];
    const Dir side = kSide[down];
    const ChunkCoord cc = s.chunk->coord;
    const i32 ox = cc.x * kChunkSize, oy = cc.y * kChunkSize;
    bool changed = false;

    // A cell anywhere: in this chunk directly, elsewhere through the grid.
    struct Cell {
        TileId* block = nullptr;
        TileId* liquid = nullptr;
        ChunkState* owner = nullptr;
    };
    auto cell = [&](i32 x, i32 y) -> Cell {
        const ChunkCoord c{x >> kChunkShift, y >> kChunkShift};
        ChunkState* owner = (c.x == cc.x && c.y == cc.y) ? &s : state_at(c);
        if (!owner) return {};
        const u32 i = world::local_index(x, y);
        return {owner->blocks + i, owner->liquid + i, owner};
    };
    // A cell changed: it and its neighbours are looked at next tick, in this
    // chunk or over its edges.
    auto changed_at = [&](i32 wx, i32 wy) {
        const i32 lx = wx - ox, ly = wy - oy;
        if (lx >= 0 && lx < kChunkSize && ly >= 0 && ly < kChunkSize) {
            s.next.add(lx - 1, ly - 1, lx + 2, ly + 2);
            u8 bits = 0;
            i32 nx = 0, ny = 0;
            if (lx == 0) nx = -1, bits |= kRight;
            if (lx == kChunkSize - 1) nx = 1, bits |= kLeft;
            if (ly == 0) ny = -1, bits |= kBottom;
            if (ly == kChunkSize - 1) ny = 1, bits |= kTop;
            if (nx != 0) touch({cc.x + nx, cc.y}, bits & (kLeft | kRight));
            if (ny != 0) touch({cc.x, cc.y + ny}, bits & (kTop | kBottom));
            if (nx != 0 && ny != 0) touch({cc.x + nx, cc.y + ny}, bits);
            return;
        }
        // In a neighbour: it was written, and the strip facing us wakes.
        const i32 nx = lx < 0 ? -1 : (lx >= kChunkSize ? 1 : 0);
        const i32 ny = ly < 0 ? -1 : (ly >= kChunkSize ? 1 : 0);
        u8 bits = kWritten;
        if (nx < 0) bits |= kRight;
        if (nx > 0) bits |= kLeft;
        if (ny < 0) bits |= kBottom;
        if (ny > 0) bits |= kTop;
        touch({cc.x + nx, cc.y + ny}, bits);
        // Our own cells next to it may move into the space it changed.
        s.next.add(lx - 1, ly - 1, lx + 2, ly + 2);
    };
    auto solid = [&](TileId block) { return shapes_[block] == static_cast<u8>(TileShape::Solid); };

    // Only the work rectangle, as rows from the bottom r and columns col.
    const Box w = s.work;
    i32 r0, r1, c0, c1;
    switch (down) {
    case 0: r0 = kChunkSize - w.y1, r1 = kChunkSize - w.y0, c0 = w.x0, c1 = w.x1; break;
    case 1: r0 = w.x0, r1 = w.x1, c0 = w.y0, c1 = w.y1; break;
    case 2: r0 = w.y0, r1 = w.y1, c0 = w.x0, c1 = w.x1; break;
    default: r0 = kChunkSize - w.x1, r1 = kChunkSize - w.x0, c0 = w.y0, c1 = w.y1; break;
    }
    // Alternate the sideways order each tick so nothing drifts one way.
    const bool flip = (tick & 1) != 0;
    for (i32 r = r0; r < r1; ++r) {
        for (i32 k = c0; k < c1; ++k) {
            const i32 col = flip ? c0 + c1 - 1 - k : k;
            // r counts rows from the bottom (in the gravity's sense).
            i32 lx, ly;
            switch (down) {
            case 0: lx = col, ly = kChunkSize - 1 - r; break;
            case 1: lx = r, ly = col; break;
            case 2: lx = col, ly = r; break;
            default: lx = kChunkSize - 1 - r, ly = col; break;
            }
            const i32 x = ox + lx, y = oy + ly;
            const u32 i = static_cast<u32>(ly * kChunkSize + lx);
            TileId& block = s.blocks[i];
            TileId& liq = s.liquid[i];

            // Falling tiles.
            if (const u8 fall = falling_[block]; fall != 0) {
                Cell below = cell(x + d.x, y + d.y);
                if (below.block && *below.block == 0) {
                    // Into air, or sinking through liquid (which takes its place).
                    std::swap(liq, *below.liquid);
                    *below.block = block;
                    block = 0;
                    changed_at(x + d.x, y + d.y);
                    changed_at(x, y);
                    changed = true;
                    continue;
                }
                if (fall == 2) {
                    const i32 first = ((tick + static_cast<u64>(x * 7 + y * 13)) & 1) ? 1 : -1;
                    for (i32 dir : {first, -first}) {
                        Cell beside = cell(x + side.x * dir, y + side.y * dir);
                        Cell diag = cell(x + side.x * dir + d.x, y + side.y * dir + d.y);
                        if (beside.block && diag.block && *beside.block == 0 && *diag.block == 0 &&
                            *beside.liquid == 0) {
                            std::swap(liq, *diag.liquid);
                            *diag.block = block;
                            block = 0;
                            changed_at(x + side.x * dir + d.x, y + side.y * dir + d.y);
                            changed_at(x, y);
                            changed = true;
                            break;
                        }
                    }
                }
                continue;
            }

            if (liq == 0) continue;
            const u8 kind = liquid_kind(liq);
            i32 amount = liquid_amount(liq);
            const i32 start = amount;

            // A block was put into the liquid: it is pushed up, or lost.
            if (solid(block)) {
                Cell above = cell(x - d.x, y - d.y);
                if (above.block && !solid(*above.block) && (*above.liquid == 0 || liquid_kind(*above.liquid) == kind)) {
                    *above.liquid = make_liquid(kind, static_cast<u16>(std::min<i32>(liquid_amount(*above.liquid) + amount, kMaxAmount)));
                    changed_at(x - d.x, y - d.y);
                }
                liq = 0;
                changed_at(x, y);
                changed = true;
                continue;
            }
            if (kind > liquid_count()) {
                liq = 0;
                changed_at(x, y);
                changed = true;
                continue;
            }
            const LiquidKind& lk = kinds_[kind];

            // Can this liquid flow into a cell? Reactions happen here.
            auto open = [&](Cell& c, i32 cx, i32 cy) -> bool {
                if (!c.block || solid(*c.block)) return false;
                if (*c.liquid == 0 || liquid_kind(*c.liquid) == kind) return true;
                const TileId made = reactions_[kind * 16 + liquid_kind(*c.liquid)];
                if (made == 0) return false;
                amount -= std::min<i32>(amount, liquid_amount(*c.liquid));
                *c.liquid = 0;
                *c.block = made;
                changed_at(cx, cy);
                return false;
            };

            // Down.
            if (Cell below = cell(x + d.x, y + d.y); amount > 0 && open(below, x + d.x, y + d.y)) {
                const i32 there = liquid_amount(*below.liquid);
                i32 flow = lk.level ? stable_lower(amount + there) - there : kFull - there;
                flow = std::clamp(flow, 0, std::min(amount, kMaxAmount - there));
                if (flow > 0) {
                    *below.liquid = make_liquid(kind, static_cast<u16>(there + flow));
                    amount -= flow;
                    changed_at(x + d.x, y + d.y);
                }
            }
            // Sideways, both ways, toward the lower neighbour.
            if (amount > 0) {
                const i32 first = flip ? -1 : 1;
                for (i32 dir : {first, -first}) {
                    if (amount <= 0) break;
                    const i32 nx = x + side.x * dir, ny = y + side.y * dir;
                    Cell next = cell(nx, ny);
                    if (!open(next, nx, ny)) continue;
                    const i32 there = liquid_amount(*next.liquid);
                    // Half the difference evens two cells out exactly; thicker
                    // liquids pass on less per tick.
                    const i32 flow = (amount - there) / (2 * lk.thickness);
                    if (flow <= 0) continue;
                    *next.liquid = make_liquid(kind, static_cast<u16>(there + flow));
                    amount -= flow;
                    changed_at(nx, ny);
                }
            }
            // Up, under pressure (levelling liquids only).
            if (lk.level && amount > kFull) {
                Cell above = cell(x - d.x, y - d.y);
                if (open(above, x - d.x, y - d.y)) {
                    const i32 there = liquid_amount(*above.liquid);
                    const i32 flow = std::clamp(amount - stable_lower(amount + there), 0, amount);
                    if (flow > 0) {
                        *above.liquid = make_liquid(kind, static_cast<u16>(there + flow));
                        amount -= flow;
                        changed_at(x - d.x, y - d.y);
                    }
                }
            }
            // A thin film on a floor dries up (so spreading ends); a drop
            // resting on more liquid stays: it is how levels rise.
            if (amount > 0 && amount < kEvaporate) {
                Cell below = cell(x + d.x, y + d.y);
                if (!below.block || solid(*below.block)) amount = 0;
            }
            if (amount != start) {
                liq = make_liquid(kind, static_cast<u16>(amount));
                changed_at(x, y);
                changed = true;
            }
        }
    }
    if (changed) {
        ++s.chunk->revision;
        s.chunk->edited = true;
    }
    return changed;
}

} // namespace forge::sim
