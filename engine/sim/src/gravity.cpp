#include "forge/sim/gravity.h"

#include "forge/core/profile.h"

#include <algorithm>
#include <cmath>

FORGE_REFLECT(forge::sim::GravitySource, 1) {
    t.field("radius", &forge::sim::GravitySource::radius);
    t.field("strength", &forge::sim::GravitySource::strength);
    t.field("toward_center", &forge::sim::GravitySource::toward_center);
    t.field("dir_x", &forge::sim::GravitySource::dir_x);
    t.field("dir_y", &forge::sim::GravitySource::dir_y);
    t.field("replace", &forge::sim::GravitySource::replace);
    t.field("fade", &forge::sim::GravitySource::fade);
}

namespace forge::sim {

namespace {

constexpr i64 kMaxGridCells = 1 << 20;

world::Rect chunks_reached(const GravityField::Placed& s) {
    const f64 r = s.source.radius;
    const world::Rect tiles{static_cast<i32>(std::floor(s.x - r)), static_cast<i32>(std::floor(s.y - r)),
                            static_cast<i32>(std::floor(s.x + r)) + 1, static_cast<i32>(std::floor(s.y + r)) + 1};
    return world::chunks_of(tiles);
}

} // namespace

void GravityField::build(std::span<const Placed> sources) {
    FORGE_ZONE_N("Gravity field");
    sources_.assign(sources.begin(), sources.end());
    grid_ = false;
    if (sources_.empty()) return;

    world::Rect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (const Placed& s : sources_) {
        const world::Rect r = chunks_reached(s);
        box.x0 = std::min(box.x0, r.x0);
        box.y0 = std::min(box.y0, r.y0);
        box.x1 = std::max(box.x1, r.x1);
        box.y1 = std::max(box.y1, r.y1);
    }
    // Sources spread too widely for a grid: every point checks them all.
    if (static_cast<i64>(box.x1 - box.x0) * (box.y1 - box.y0) > kMaxGridCells) return;

    rect_ = box;
    width_ = box.x1 - box.x0;
    const usize cells = static_cast<usize>(width_) * static_cast<usize>(box.y1 - box.y0);
    cell_start_.assign(cells + 1, 0);
    for (const Placed& s : sources_) {
        const world::Rect r = chunks_reached(s);
        for (i32 y = r.y0; y < r.y1; ++y)
            for (i32 x = r.x0; x < r.x1; ++x) ++cell_start_[static_cast<usize>((y - box.y0) * width_ + (x - box.x0))];
    }
    u32 running = 0;
    for (usize c = 0; c <= cells; ++c) {
        const u32 n = c < cells ? cell_start_[c] : 0;
        cell_start_[c] = running;
        running += n;
    }
    cell_sources_.assign(running, 0);
    std::vector<u32> fill(cell_start_.begin(), cell_start_.end() - 1);
    for (u32 i = 0; i < sources_.size(); ++i) {
        const world::Rect r = chunks_reached(sources_[i]);
        for (i32 y = r.y0; y < r.y1; ++y)
            for (i32 x = r.x0; x < r.x1; ++x)
                cell_sources_[fill[static_cast<usize>((y - box.y0) * width_ + (x - box.x0))]++] = i;
    }
    grid_ = true;
}

void GravityField::at(f64 x, f64 y, f32& gx, f32& gy) const {
    f32 ax = 0, ay = 0;
    bool replaced = false;
    auto apply = [&](const Placed& s) {
        const f32 dx = static_cast<f32>(s.x - x), dy = static_cast<f32>(s.y - y);
        const f32 d2 = dx * dx + dy * dy;
        const f32 r = s.source.radius;
        if (d2 > r * r) return;
        const f32 d = std::sqrt(d2);
        f32 k = s.source.strength;
        if (s.source.fade) k *= 1.0f - d / r;
        f32 nx, ny;
        if (s.source.toward_center) {
            if (d < 1e-3f) return; // at the very centre: no direction
            nx = dx / d;
            ny = dy / d;
        } else {
            const f32 len = std::sqrt(s.source.dir_x * s.source.dir_x + s.source.dir_y * s.source.dir_y);
            if (len < 1e-6f) return;
            nx = s.source.dir_x / len;
            ny = s.source.dir_y / len;
        }
        ax += nx * k;
        ay += ny * k;
        replaced |= s.source.replace;
    };
    if (grid_) {
        const i32 cx = static_cast<i32>(std::floor(x / world::kChunkSize));
        const i32 cy = static_cast<i32>(std::floor(y / world::kChunkSize));
        if (rect_.contains(cx, cy)) {
            const usize cell = static_cast<usize>((cy - rect_.y0) * width_ + (cx - rect_.x0));
            for (u32 k = cell_start_[cell]; k < cell_start_[cell + 1]; ++k) apply(sources_[cell_sources_[k]]);
        }
    } else {
        for (const Placed& s : sources_) apply(s);
    }
    gx = replaced ? ax : ax + world_x_;
    gy = replaced ? ay : ay + world_y_;
}

} // namespace forge::sim
