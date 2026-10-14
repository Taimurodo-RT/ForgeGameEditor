#include "svg_reference.h"

#include <lunasvg.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace forge::edits {

std::vector<u8> svg_pixels(std::string_view svg, int w, int h, const std::string& color) {
    std::unique_ptr<lunasvg::Document> doc = lunasvg::Document::loadFromData(svg.data(), svg.size());
    if (!doc || w <= 0 || h <= 0) return {};
    if (!color.empty()) doc->documentElement().setAttribute("color", color);
    lunasvg::Bitmap bitmap = doc->renderToBitmap(w, h);
    if (bitmap.isNull()) return {};
    std::vector<u8> out(static_cast<usize>(w) * static_cast<usize>(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u8* p = bitmap.data() + static_cast<usize>(y) * static_cast<usize>(bitmap.stride()) + static_cast<usize>(x) * 4;
            u8* q = out.data() + (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4;
            q[0] = p[2]; // lunasvg: BGRA
            q[1] = p[1];
            q[2] = p[0];
            q[3] = p[3];
        }
    return out;
}

std::vector<u8> svg_frame(std::string_view svg, int r, int c, int w, int h) {
    const std::vector<u8> ref = svg_pixels(svg, r, r);
    if (ref.empty() || c <= 0 || 2 * c > r || w < 2 * c || h < 2 * c) return {};
    std::vector<u8> out(static_cast<usize>(w) * static_cast<usize>(h) * 4);
    for (int y = 0; y < h; ++y) {
        const int ry = y < c ? y : y >= h - c ? r - (h - y) : r / 2;
        for (int x = 0; x < w; ++x) {
            const int rx = x < c ? x : x >= w - c ? r - (w - x) : r / 2;
            std::copy_n(ref.data() + (static_cast<usize>(ry) * static_cast<usize>(r) + static_cast<usize>(rx)) * 4, 4,
                        out.data() + (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4);
        }
    }
    return out;
}

int pixels_diff(const std::vector<u8>& a, const std::vector<u8>& b, bool rgb) {
    if (a.empty() || a.size() != b.size()) return 999;
    int worst = 0;
    for (usize i = 0; i < a.size(); ++i)
        if (!rgb || i % 4 != 3) worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    return worst;
}

std::string hex_color(int r, int g, int b) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r & 255, g & 255, b & 255);
    return buf;
}

} // namespace forge::edits
