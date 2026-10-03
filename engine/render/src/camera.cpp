#include "forge/render/camera.h"

#include <cmath>

namespace forge::render {

world::Rect Camera2D::visible_tiles(u32 width, u32 height) const {
    const f64 half_w = static_cast<f64>(width) / (2.0 * zoom);
    const f64 half_h = static_cast<f64>(height) / (2.0 * zoom);
    return {static_cast<i32>(std::floor(x - half_w)), static_cast<i32>(std::floor(y - half_h)),
            static_cast<i32>(std::ceil(x + half_w)) + 1, static_cast<i32>(std::ceil(y + half_h)) + 1};
}

void Camera2D::screen_to_tile(f32 sx, f32 sy, u32 width, u32 height, f64& tx, f64& ty) const {
    tx = x + (static_cast<f64>(sx) - static_cast<f64>(width) * 0.5) / zoom;
    ty = y + (static_cast<f64>(sy) - static_cast<f64>(height) * 0.5) / zoom;
}

f64 Camera2D::snapped_x() const { return std::round(x * zoom) / zoom; }
f64 Camera2D::snapped_y() const { return std::round(y * zoom) / zoom; }

} // namespace forge::render
