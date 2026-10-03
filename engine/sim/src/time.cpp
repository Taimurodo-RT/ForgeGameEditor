#include "forge/sim/time.h"

#include <algorithm>
#include <climits>
#include <cmath>

FORGE_REFLECT(forge::sim::TimeBubble, 1) {
    t.field("radius", &forge::sim::TimeBubble::radius);
    t.field("scale", &forge::sim::TimeBubble::scale);
    t.field("fade", &forge::sim::TimeBubble::fade);
}
FORGE_REFLECT(forge::sim::OutsideTime, 1) { t.field("scale", &forge::sim::OutsideTime::scale); }

namespace forge::sim {

void TimeField::build(std::span<const Placed> bubbles) {
    bubbles_.assign(bubbles.begin(), bubbles.end());
    box_ = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (const Placed& b : bubbles_) {
        const f64 r = b.bubble.radius;
        box_.x0 = std::min(box_.x0, static_cast<i32>(std::floor(b.x - r)));
        box_.y0 = std::min(box_.y0, static_cast<i32>(std::floor(b.y - r)));
        box_.x1 = std::max(box_.x1, static_cast<i32>(std::floor(b.x + r)) + 1);
        box_.y1 = std::max(box_.y1, static_cast<i32>(std::floor(b.y + r)) + 1);
    }
}

f32 TimeField::at(f64 x, f64 y) const {
    if (bubbles_.empty() || x < box_.x0 || y < box_.y0 || x >= box_.x1 || y >= box_.y1) return 1;
    f32 scale = 1;
    for (const Placed& b : bubbles_) {
        const f32 dx = static_cast<f32>(b.x - x), dy = static_cast<f32>(b.y - y);
        const f32 r = b.bubble.radius;
        const f32 d2 = dx * dx + dy * dy;
        if (d2 > r * r) continue;
        f32 s = std::max(0.0f, b.bubble.scale);
        if (b.bubble.fade) {
            const f32 k = 1.0f - std::sqrt(d2) / r;
            s = 1.0f + (s - 1.0f) * k;
        }
        scale *= s;
    }
    return scale;
}

} // namespace forge::sim
