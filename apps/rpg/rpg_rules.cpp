#include "rpg_rules.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace rpg {

bool overlap(const Box& a, const Box& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }

Box feet_box(f64 x, f64 y, const Feet& f) { return {x - f.half_w, y - f.depth, x + f.half_w, y}; }

Map::Map(i32 x0, i32 y0, i32 w, i32 h)
    : x0_(x0), y0_(y0), w_(std::max(0, w)), h_(std::max(0, h)), free_(static_cast<usize>(w_) * static_cast<usize>(h_), 0) {}

bool Map::free(i32 x, i32 y) const {
    x -= x0_, y -= y0_;
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return false;
    return free_[static_cast<usize>(y) * static_cast<usize>(w_) + static_cast<usize>(x)] != 0;
}

void Map::set(i32 x, i32 y, bool free) {
    x -= x0_, y -= y0_;
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    free_[static_cast<usize>(y) * static_cast<usize>(w_) + static_cast<usize>(x)] = free ? 1 : 0;
}

bool fits(const Map& map, const std::vector<Box>& bodies, f64 x, f64 y, const Feet& f) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    const Box b = feet_box(x, y, f);
    // The cells the box covers; an edge exactly on a cell's border does not reach into it.
    const i32 cx0 = static_cast<i32>(std::floor(b.x0)), cx1 = static_cast<i32>(std::ceil(b.x1)) - 1;
    const i32 cy0 = static_cast<i32>(std::floor(b.y0)), cy1 = static_cast<i32>(std::ceil(b.y1)) - 1;
    for (i32 cy = cy0; cy <= cy1; ++cy)
        for (i32 cx = cx0; cx <= cx1; ++cx)
            if (!map.free(cx, cy)) return false;
    for (const Box& o : bodies)
        if (overlap(b, o)) return false;
    return true;
}

Heading heading(bool up, bool down, bool left, bool right) {
    const f64 x = (right ? 1.0 : 0.0) - (left ? 1.0 : 0.0);
    const f64 y = (down ? 1.0 : 0.0) - (up ? 1.0 : 0.0);
    if (x != 0 && y != 0) return {x * std::sqrt(0.5), y * std::sqrt(0.5)};
    return {x, y};
}

Dir facing(Heading h, Dir previous) {
    if (h.none()) return previous;
    const i32 sx = h.x > 0 ? 1 : h.x < 0 ? -1 : 0, sy = h.y > 0 ? 1 : h.y < 0 ? -1 : 0;
    if (sy > 0) return sx < 0 ? Dir::SW : sx > 0 ? Dir::SE : Dir::S;
    if (sy < 0) return sx < 0 ? Dir::NW : sx > 0 ? Dir::NE : Dir::N;
    return sx < 0 ? Dir::W : Dir::E;
}

Dir toward(f64 dx, f64 dy, Dir previous) {
    if ((dx == 0 && dy == 0) || !std::isfinite(dx) || !std::isfinite(dy)) return previous;
    // Sectors of 45° from east, clockwise on the screen (y down): E, SE, S, SW, W, NW, N, NE.
    constexpr Dir kFromEast[] = {Dir::E, Dir::SE, Dir::S, Dir::SW, Dir::W, Dir::NW, Dir::N, Dir::NE};
    const f64 turns = std::atan2(dy, dx) / (std::acos(-1.0) / 4);
    const i32 sector = ((static_cast<i32>(std::lround(turns)) % 8) + 8) % 8;
    return kFromEast[sector];
}

namespace {
constexpr const char* kDirIds[] = {"s", "sw", "w", "nw", "n", "ne", "e", "se"};
}

const char* dir_id(Dir d) { return kDirIds[static_cast<usize>(d) % 8]; }

bool dir_of(std::string_view id, Dir& out) {
    for (usize i = 0; i < 8; ++i)
        if (id == kDirIds[i]) {
            out = static_cast<Dir>(i);
            return true;
        }
    return false;
}

namespace {

// One axis of one step: from `from` the axis goes by d; the farthest part of it that fits, by halving (the edge of a
// cell or a body, to within a hair: 2^-30 of a step).
f64 slide(const Map& map, const std::vector<Box>& bodies, f64 x, f64 y, f64 d, bool along_x, const Feet& f, bool& hit) {
    auto at = [&](f64 t) { return along_x ? fits(map, bodies, x + d * t, y, f) : fits(map, bodies, x, y + d * t, f); };
    if (at(1.0)) return d;
    hit = true;
    f64 lo = 0, hi = 1;
    for (i32 i = 0; i < 30; ++i) {
        const f64 mid = (lo + hi) / 2;
        (at(mid) ? lo : hi) = mid;
    }
    return d * lo;
}

} // namespace

Moved move(const Map& map, const std::vector<Box>& bodies, f64 x, f64 y, f64 dx, f64 dy, const Feet& f) {
    Moved m{x, y};
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(x) || !std::isfinite(y)) return m;
    const f64 longest = std::max(std::abs(dx), std::abs(dy));
    const i32 steps = std::max(1, static_cast<i32>(std::ceil(longest / kStepMax)));
    const f64 sx = dx / steps, sy = dy / steps;
    for (i32 i = 0; i < steps; ++i) {
        // Put where it does not fit: it goes as told until it does.
        if (!fits(map, bodies, m.x, m.y, f)) {
            m.x += sx, m.y += sy;
            continue;
        }
        if (sx != 0) m.x += slide(map, bodies, m.x, m.y, sx, true, f, m.hit_x);
        if (sy != 0) m.y += slide(map, bodies, m.x, m.y, sy, false, f, m.hit_y);
    }
    return m;
}

i32 nearest(const std::vector<Spot>& spots, f64 x, f64 y, f64 reach) {
    i32 best = -1;
    f64 best_d = reach * reach;
    for (usize i = 0; i < spots.size(); ++i) {
        const f64 ddx = spots[i].x - x, ddy = spots[i].y - y;
        const f64 d = ddx * ddx + ddy * ddy;
        if (d <= best_d && (best < 0 || d < best_d)) best = static_cast<i32>(i), best_d = d;
    }
    return best;
}

i32 cell_x(f64 x) { return static_cast<i32>(std::floor(x)); }
i32 cell_y(f64 y) { return static_cast<i32>(std::floor(y - 0.5)); }

} // namespace rpg
