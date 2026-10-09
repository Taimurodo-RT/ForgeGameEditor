#include "forge/level/tile_edit.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace forge::level {

void disc_cells(i32 x, i32 y, i32 radius, std::vector<Cell>& out) {
    radius = std::max(radius, 0);
    // Cells whose centre lies within about radius + 0.4 of the brush centre,
    // so a radius of 1 is a plus and larger ones round blobs, as in pixel
    // editors.
    const i64 limit = 5 * static_cast<i64>(radius) * radius + 4 * static_cast<i64>(radius);
    for (i32 dy = -radius; dy <= radius; ++dy)
        for (i32 dx = -radius; dx <= radius; ++dx)
            if (5 * (static_cast<i64>(dx) * dx + static_cast<i64>(dy) * dy) <= limit) out.push_back({x + dx, y + dy});
}

void line_cells(i32 x0, i32 y0, i32 x1, i32 y1, std::vector<Cell>& out) {
    // Bresenham: every step moves to a cell that touches the previous one.
    const i32 dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const i32 sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    i32 err = dx + dy;
    for (;;) {
        out.push_back({x0, y0});
        if (x0 == x1 && y0 == y1) break;
        const i32 e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void rect_cells(i32 x0, i32 y0, i32 x1, i32 y1, bool filled, std::vector<Cell>& out) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    for (i32 y = y0; y <= y1; ++y)
        for (i32 x = x0; x <= x1; ++x)
            if (filled || y == y0 || y == y1 || x == x0 || x == x1) out.push_back({x, y});
}

void fill_cells(const Level& level, u32 layer, i32 x, i32 y, usize limit, std::vector<Cell>& out, bool* capped) {
    if (capped) *capped = false;
    if (!level.loaded(x, y)) return;
    const world::TileId target = level.tile(layer, x, y);
    // Visited cells, by chunk: a flat bitset per touched chunk.
    std::unordered_map<world::ChunkCoord, std::vector<u64>, world::ChunkCoordHash> seen;
    auto visit = [&](i32 cx, i32 cy) {
        std::vector<u64>& bits = seen[world::chunk_of(cx, cy)];
        if (bits.empty()) bits.assign(world::kChunkTiles / 64, 0);
        const u32 i = world::local_index(cx, cy);
        const u64 bit = 1ull << (i & 63);
        if (bits[i >> 6] & bit) return false;
        bits[i >> 6] |= bit;
        return true;
    };
    std::vector<Cell> stack{{x, y}};
    visit(x, y);
    const usize start = out.size();
    while (!stack.empty()) {
        const Cell c = stack.back();
        stack.pop_back();
        out.push_back(c);
        if (out.size() - start >= limit) {
            if (capped) *capped = true;
            return;
        }
        const Cell next[4] = {{c.x + 1, c.y}, {c.x - 1, c.y}, {c.x, c.y + 1}, {c.x, c.y - 1}};
        for (const Cell& n : next) {
            if (!level.loaded(n.x, n.y) || level.tile(layer, n.x, n.y) != target) continue;
            if (visit(n.x, n.y)) stack.push_back(n);
        }
    }
}

TileStroke::TileStroke(Level& level, std::string label) : level_(level), label_(std::move(label)) {}

usize TileStroke::paint(u32 layer, std::span<const Cell> cells, world::TileId value) {
    usize changed = 0;
    for (const Cell& c : cells) {
        if (!level_.loaded(c.x, c.y)) continue;
        const world::TileId before = level_.tile(layer, c.x, c.y);
        auto [it, fresh] = index_.try_emplace(Key{layer, c.x, c.y}, changes_.size());
        if (fresh) {
            if (before == value) {
                index_.erase(it); // nothing to remember yet
                continue;
            }
            changes_.push_back({c.x, c.y, layer, before, value});
            if (bounds_.empty()) bounds_ = {c.x, c.y, c.x + 1, c.y + 1};
            bounds_ = {std::min(bounds_.x0, c.x), std::min(bounds_.y0, c.y), std::max(bounds_.x1, c.x + 1),
                       std::max(bounds_.y1, c.y + 1)};
        } else {
            if (before == value) continue;
            changes_[it->second].after = value;
        }
        if (level_.set_tile(layer, c.x, c.y, value)) ++changed;
    }
    return changed;
}

usize TileStroke::paint_disc(u32 layer, i32 x, i32 y, i32 radius, world::TileId value) {
    scratch_.clear();
    disc_cells(x, y, radius, scratch_);
    return paint(layer, scratch_, value);
}

void TileStroke::set_all(bool after) {
    if (changes_.empty()) return;
    level_.ensure_loaded(bounds_);
    if (after) {
        for (const Change& c : changes_) level_.set_tile(c.layer, c.x, c.y, c.after);
    } else {
        for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) level_.set_tile(it->layer, it->x, it->y, it->before);
    }
}

void TileStroke::apply(editor::Document&) { set_all(true); }
void TileStroke::revert(editor::Document&) { set_all(false); }

SetOwnTiles::SetOwnTiles(Level& level, LevelTiles before, LevelTiles after, std::string label)
    : level_(level), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}

void SetOwnTiles::apply(editor::Document&) { level_.set_own_tiles(after_); }
void SetOwnTiles::revert(editor::Document&) { level_.set_own_tiles(before_); }

} // namespace forge::level
