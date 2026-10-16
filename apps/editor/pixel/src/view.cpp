#include "forge/pixel/view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace forge::pixel {

f64 zoom_of(i32 step) {
    step = std::clamp(step, 0, kZoomTop);
    return step == kZoomTop ? 16.0 : std::pow(1.25, step);
}

std::string zoom_words(i32 step) {
    if (step <= 0) return {};
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", zoom_of(step));
    std::string s = buf;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    std::replace(s.begin(), s.end(), '.', ',');
    return "×" + s;
}

Affine PictureView::affine() const {
    const f64 z = zoom();
    return {z, 0, 0, z, ox, oy};
}

void PictureView::zoom_to(i32 to, vector::Point at) {
    const f64 was = zoom();
    step = std::clamp(to, 0, kZoomTop);
    const f64 z = zoom();
    // The point of the view at ×1 under `at` before, under it again.
    const f64 px = (at.x - ox) / was, py = (at.y - oy) / was;
    ox = at.x - z * px;
    oy = at.y - z * py;
    clamp();
}

void PictureView::pan(f64 dx, f64 dy) {
    ox += dx;
    oy += dy;
    clamp();
}

void PictureView::fit() {
    step = 0;
    ox = oy = 0;
    clamp();
}

void PictureView::clamp() {
    const f64 z = zoom();
    auto axis = [z](f64 b0, f64 b1, f64 w, f64& o) {
        const f64 lo = z * b0, hi = z * b1;
        if (hi - lo > w) o = std::clamp(o, w - hi, -lo);
        else o = (w - lo - hi) / 2;
    };
    axis(box[0], box[2], width, ox);
    axis(box[1], box[3], height, oy);
}

f64 grid_step(f64 points_per_unit, f64 cell) {
    if (!(points_per_unit > 0) || !std::isfinite(points_per_unit)) return 1;
    for (f64 tens = 1;; tens *= 10)
        for (f64 k : {1.0, 2.0, 5.0})
            if (k * tens * points_per_unit >= cell || k * tens >= 1e12) return k * tens;
}

void draw_grid(assets::CookedTexture& image, const Affine& px, f64 step, u32 line) {
    const u32 w = image.width, h = image.height;
    if (w == 0 || h == 0 || image.rgba8.size() < static_cast<usize>(w) * h * 4 || !(step > 0) || line == 0 ||
        px.a == 0 || px.d == 0)
        return;
    // How much of the grey each pixel takes: the most of the lines through it.
    std::vector<u8> cover(static_cast<usize>(w) * h, 0);
    constexpr u8 kLine = 102, kAxis = 204; // 40 % and 80 % of 255
    auto lines = [&](f64 scale, f64 shift, u32 extent, bool columns) {
        // The picture units the image's extent spans, a line every step of them.
        f64 u0 = (0 - shift) / scale, u1 = (extent - shift) / scale;
        if (u0 > u1) std::swap(u0, u1);
        const f64 k0 = std::ceil(u0 / step - 1e-9), k1 = std::floor(u1 / step + 1e-9);
        for (f64 k = k0; k <= k1; ++k) {
            const f64 at = std::floor(scale * (k * step) + shift);
            const u8 a = k == 0 ? kAxis : kLine;
            for (u32 i = 0; i < line; ++i) {
                const f64 c = at + i;
                if (c < 0 || c >= extent) continue;
                const u32 ci = static_cast<u32>(c);
                if (columns)
                    for (u32 y = 0; y < h; ++y) cover[static_cast<usize>(y) * w + ci] = std::max(cover[static_cast<usize>(y) * w + ci], a);
                else
                    for (u32 x = 0; x < w; ++x) cover[static_cast<usize>(ci) * w + x] = std::max(cover[static_cast<usize>(ci) * w + x], a);
            }
        }
    };
    lines(px.a, px.e, w, true);
    lines(px.d, px.f, h, false);
    for (usize i = 0, n = cover.size(); i < n; ++i) {
        if (!cover[i]) continue;
        u8* p = &image.rgba8[i * 4];
        const f64 ga = cover[i] / 255.0, da = p[3] / 255.0;
        const f64 oa = ga + da * (1 - ga);
        for (int c = 0; c < 3; ++c) p[c] = static_cast<u8>(std::lround((128 * ga + p[c] * da * (1 - ga)) / oa));
        p[3] = static_cast<u8>(std::lround(oa * 255));
    }
}

std::string coords_words(vector::Point p) {
    auto one = [](f64 v) {
        const f64 r = std::round(v * 10) / 10;
        char buf[48];
        if (r == 0) return std::string("0");
        if (r == std::round(r)) std::snprintf(buf, sizeof buf, "%.0f", r);
        else std::snprintf(buf, sizeof buf, "%.1f", r);
        return std::string(buf);
    };
    return one(p.x) + ", " + one(p.y);
}

std::string color_words(const u8 rgba[4]) {
    if (rgba[3] == 0) return {};
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", rgba[0], rgba[1], rgba[2]);
    return buf;
}

} // namespace forge::pixel
