#include "forge/assets/image_edit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace forge::assets {

namespace {

u8* at(CookedTexture& image, u32 x, u32 y) { return image.rgba8.data() + (static_cast<usize>(y) * image.width + x) * 4; }
const u8* at(const CookedTexture& image, u32 x, u32 y) {
    return image.rgba8.data() + (static_cast<usize>(y) * image.width + x) * 4;
}

// The area inside the picture.
PixelRect clipped(const CookedTexture& image, const PixelRect& area) {
    PixelRect r;
    if (area.x >= image.width || area.y >= image.height) return r;
    r.x = area.x;
    r.y = area.y;
    r.w = std::min(area.w, image.width - area.x);
    r.h = std::min(area.h, image.height - area.y);
    return r;
}

CookedTexture blank(u32 width, u32 height) {
    CookedTexture out;
    out.width = width;
    out.height = height;
    out.rgba8.assign(static_cast<usize>(width) * height * 4, 0);
    return out;
}

u8 to_u8(f64 v) { return static_cast<u8>(std::clamp(std::lround(v), 0l, 255l)); }

// HSL with every part 0..1.
void to_hsl(const u8* p, f64& h, f64& s, f64& l) {
    const f64 r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
    const f64 hi = std::max({r, g, b}), lo = std::min({r, g, b});
    l = (hi + lo) / 2;
    if (hi == lo) {
        h = s = 0;
        return;
    }
    const f64 d = hi - lo;
    s = l > 0.5 ? d / (2 - hi - lo) : d / (hi + lo);
    if (hi == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (hi == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
}

f64 hue_part(f64 p, f64 q, f64 t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

void from_hsl(f64 h, f64 s, f64 l, u8* p) {
    if (s <= 0) {
        p[0] = p[1] = p[2] = to_u8(l * 255);
        return;
    }
    const f64 q = l < 0.5 ? l * (1 + s) : l + s - l * s, k = 2 * l - q;
    p[0] = to_u8(hue_part(k, q, h + 1.0 / 3) * 255);
    p[1] = to_u8(hue_part(k, q, h) * 255);
    p[2] = to_u8(hue_part(k, q, h - 1.0 / 3) * 255);
}

u32 key_of(const u8* p) {
    return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 | p[3];
}
Rgba rgba_of(u32 key) {
    return {static_cast<u8>(key >> 24), static_cast<u8>(key >> 16), static_cast<u8>(key >> 8), static_cast<u8>(key)};
}

// Median cut: boxes of colours (with their counts) split at the median of their widest channel until there are n.
std::vector<Rgba> median_cut(std::vector<std::pair<u32, u32>> colors, usize n) {
    struct Box {
        usize from, to; // in colors
    };
    std::vector<Box> boxes{{0, colors.size()}};
    auto range = [&](const Box& b, int& channel) {
        int best = -1;
        for (int c = 0; c < 3; ++c) {
            u8 lo = 255, hi = 0;
            for (usize i = b.from; i < b.to; ++i) {
                const u8 v = static_cast<u8>(colors[i].first >> (24 - 8 * c));
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            if (hi - lo > best) {
                best = hi - lo;
                channel = c;
            }
        }
        return best;
    };
    while (boxes.size() < n) {
        // The box with the widest channel (the first such) is split.
        usize pick = boxes.size();
        int widest = 0, channel = 0;
        for (usize i = 0; i < boxes.size(); ++i) {
            if (boxes[i].to - boxes[i].from < 2) continue;
            int c = 0;
            const int r = range(boxes[i], c);
            if (r > widest) {
                widest = r;
                pick = i;
                channel = c;
            }
        }
        if (pick == boxes.size()) break;
        Box& b = boxes[pick];
        const int shift = 24 - 8 * channel;
        std::sort(colors.begin() + static_cast<std::ptrdiff_t>(b.from), colors.begin() + static_cast<std::ptrdiff_t>(b.to),
                  [shift](const auto& x, const auto& y) {
                      const u32 vx = (x.first >> shift) & 255, vy = (y.first >> shift) & 255;
                      return vx != vy ? vx < vy : x.first < y.first;
                  });
        u64 total = 0;
        for (usize i = b.from; i < b.to; ++i) total += colors[i].second;
        u64 seen = 0;
        usize mid = b.from + 1;
        for (usize i = b.from; i < b.to - 1; ++i) {
            seen += colors[i].second;
            mid = i + 1;
            if (seen * 2 >= total) break;
        }
        const Box second{mid, b.to};
        b.to = mid;
        boxes.push_back(second);
    }
    std::vector<Rgba> out;
    for (const Box& b : boxes) {
        u64 sum[3] = {0, 0, 0}, count = 0;
        for (usize i = b.from; i < b.to; ++i) {
            for (int c = 0; c < 3; ++c) sum[c] += ((colors[i].first >> (24 - 8 * c)) & 255) * colors[i].second;
            count += colors[i].second;
        }
        if (count == 0) continue;
        out.push_back({static_cast<u8>((sum[0] + count / 2) / count), static_cast<u8>((sum[1] + count / 2) / count),
                       static_cast<u8>((sum[2] + count / 2) / count), 255});
    }
    return out;
}

} // namespace

PixelRect whole_rect(const CookedTexture& image) { return {0, 0, image.width, image.height}; }

PixelRect united(const PixelRect& a, const PixelRect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const u32 x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
}

PixelRect frame_rect(const CookedTexture& image, u32 frames, u32 frame) {
    if (frames == 0 || frame >= frames || image.width % frames != 0) return {};
    const u32 w = image.width / frames;
    return {frame * w, 0, w, image.height};
}

Rgba pixel_at(const CookedTexture& image, u32 x, u32 y) {
    if (x >= image.width || y >= image.height) return {};
    const u8* p = at(image, x, y);
    return {p[0], p[1], p[2], p[3]};
}

void set_pixel(CookedTexture& image, u32 x, u32 y, Rgba c) {
    if (x >= image.width || y >= image.height) return;
    u8* p = at(image, x, y);
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

std::vector<u8> copy_rect(const CookedTexture& image, const PixelRect& rect) {
    const PixelRect r = clipped(image, rect);
    std::vector<u8> out(static_cast<usize>(r.w) * r.h * 4);
    for (u32 y = 0; y < r.h; ++y) std::memcpy(out.data() + static_cast<usize>(y) * r.w * 4, at(image, r.x, r.y + y), static_cast<usize>(r.w) * 4);
    return out;
}

void paste_rect(CookedTexture& image, const PixelRect& rect, std::span<const u8> pixels) {
    const PixelRect r = clipped(image, rect);
    if (r != rect || pixels.size() < static_cast<usize>(r.w) * r.h * 4) return;
    for (u32 y = 0; y < r.h; ++y) std::memcpy(at(image, r.x, r.y + y), pixels.data() + static_cast<usize>(y) * r.w * 4, static_cast<usize>(r.w) * 4);
}

// --- the pixel editor ---

PixelRect paint(CookedTexture& image, i32 x, i32 y, u32 size, Rgba color, const PixelRect& area_in) {
    const PixelRect area = clipped(image, area_in);
    size = std::clamp(size, 1u, 4u);
    const i64 x0 = static_cast<i64>(x) - (size - 1) / 2, y0 = static_cast<i64>(y) - (size - 1) / 2;
    PixelRect changed;
    for (i64 py = y0; py < y0 + size; ++py)
        for (i64 px = x0; px < x0 + size; ++px) {
            if (!area.contains(px, py)) continue;
            const u32 ux = static_cast<u32>(px), uy = static_cast<u32>(py);
            if (pixel_at(image, ux, uy) == color) continue;
            set_pixel(image, ux, uy, color);
            changed = united(changed, {ux, uy, 1, 1});
        }
    return changed;
}

PixelRect paint_line(CookedTexture& image, i32 x0, i32 y0, i32 x1, i32 y1, u32 size, Rgba color, const PixelRect& area) {
    PixelRect changed;
    const i32 dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    i32 err = dx + dy;
    for (;;) {
        changed = united(changed, paint(image, x0, y0, size, color, area));
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
    return changed;
}

PixelRect fill(CookedTexture& image, u32 x, u32 y, Rgba color, const PixelRect& area_in) {
    const PixelRect area = clipped(image, area_in);
    if (!area.contains(x, y)) return {};
    const Rgba from = pixel_at(image, x, y);
    if (from == color) return {};
    PixelRect changed;
    std::vector<std::pair<u32, u32>> stack{{x, y}};
    set_pixel(image, x, y, color);
    while (!stack.empty()) {
        const auto [px, py] = stack.back();
        stack.pop_back();
        changed = united(changed, {px, py, 1, 1});
        const i64 next[4][2] = {{px - 1ll, py}, {px + 1ll, py}, {px, py - 1ll}, {px, py + 1ll}};
        for (const auto& n : next) {
            if (!area.contains(n[0], n[1])) continue;
            const u32 nx = static_cast<u32>(n[0]), ny = static_cast<u32>(n[1]);
            if (pixel_at(image, nx, ny) != from) continue;
            set_pixel(image, nx, ny, color);
            stack.push_back({nx, ny});
        }
    }
    return changed;
}

std::vector<Rgba> picture_colors(const CookedTexture& image, usize max) {
    std::map<u32, u64> counts;
    for (usize i = 0; i + 3 < image.rgba8.size(); i += 4)
        if (image.rgba8[i + 3] != 0) ++counts[key_of(image.rgba8.data() + i)];
    std::vector<std::pair<u32, u64>> list(counts.begin(), counts.end());
    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<Rgba> out;
    for (usize i = 0; i < list.size() && i < max; ++i) out.push_back(rgba_of(list[i].first));
    return out;
}

// --- transformations ---

bool turn_area(CookedTexture& image, const PixelRect& area_in, Turn turn, std::string* why) {
    const PixelRect area = clipped(image, area_in);
    if (area.empty()) return false;
    if ((turn == Turn::Cw || turn == Turn::Ccw) && area.w != area.h) {
        if (why)
            *why = "Поворот на 90° меняет ширину и высоту: кадр " + std::to_string(area.w) + " × " + std::to_string(area.h) +
                   " в полосе на своё место не встанет. Поверните на 180° или отразите, а на 90° — картинку из одного кадра";
        return false;
    }
    const std::vector<u8> before = copy_rect(image, area);
    auto src = [&](u32 x, u32 y) { return before.data() + (static_cast<usize>(y) * area.w + x) * 4; };
    for (u32 y = 0; y < area.h; ++y)
        for (u32 x = 0; x < area.w; ++x) {
            u32 sx = x, sy = y;
            switch (turn) {
            case Turn::Cw: // what is at (x, y) now was at (y, n - 1 - x)
                sx = y;
                sy = area.w - 1 - x;
                break;
            case Turn::Ccw:
                sx = area.w - 1 - y;
                sy = x;
                break;
            case Turn::Half:
                sx = area.w - 1 - x;
                sy = area.h - 1 - y;
                break;
            case Turn::FlipH: sx = area.w - 1 - x; break;
            case Turn::FlipV: sy = area.h - 1 - y; break;
            }
            std::memcpy(at(image, area.x + x, area.y + y), src(sx, sy), 4);
        }
    return true;
}

CookedTexture turned(const CookedTexture& image, Turn turn) {
    const bool quarter = turn == Turn::Cw || turn == Turn::Ccw;
    CookedTexture out = blank(quarter ? image.height : image.width, quarter ? image.width : image.height);
    for (u32 y = 0; y < image.height; ++y)
        for (u32 x = 0; x < image.width; ++x) {
            u32 tx = x, ty = y;
            switch (turn) {
            case Turn::Cw:
                tx = image.height - 1 - y;
                ty = x;
                break;
            case Turn::Ccw:
                tx = y;
                ty = image.width - 1 - x;
                break;
            case Turn::Half:
                tx = image.width - 1 - x;
                ty = image.height - 1 - y;
                break;
            case Turn::FlipH: tx = image.width - 1 - x; break;
            case Turn::FlipV: ty = image.height - 1 - y; break;
            }
            std::memcpy(at(out, tx, ty), at(image, x, y), 4);
        }
    return out;
}

CookedTexture resized(const CookedTexture& image, u32 width, u32 height, bool smooth) {
    CookedTexture out = blank(width, height);
    if (width == 0 || height == 0 || image.width == 0 || image.height == 0) return out;
    if (!smooth) {
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                const u32 sx = static_cast<u32>((static_cast<u64>(x) * 2 + 1) * image.width / (2ull * width));
                const u32 sy = static_cast<u32>((static_cast<u64>(y) * 2 + 1) * image.height / (2ull * height));
                std::memcpy(at(out, x, y), at(image, std::min(sx, image.width - 1), std::min(sy, image.height - 1)), 4);
            }
        return out;
    }
    // Each axis by itself: a weight per source pixel for each target pixel (area when shrinking, a tent when
    // growing), the colours premultiplied by their alpha.
    struct Tap {
        u32 at;
        f64 w;
    };
    auto taps = [](u32 from, u32 to) {
        std::vector<std::vector<Tap>> all(to);
        const f64 scale = static_cast<f64>(from) / to;
        for (u32 i = 0; i < to; ++i) {
            std::vector<Tap>& t = all[i];
            if (scale >= 1) {
                const f64 a = i * scale, b = (i + 1) * scale;
                for (u32 s = static_cast<u32>(a); s < from && s < b; ++s) {
                    const f64 w = std::min<f64>(b, s + 1) - std::max<f64>(a, s);
                    if (w > 0) t.push_back({s, w});
                }
            } else {
                const f64 c = (i + 0.5) * scale - 0.5;
                const f64 lo = std::floor(c);
                const f64 f = c - lo;
                const i64 l = static_cast<i64>(lo);
                auto put = [&](i64 s, f64 w) { t.push_back({static_cast<u32>(std::clamp<i64>(s, 0, from - 1)), w}); };
                put(l, 1 - f);
                put(l + 1, f);
            }
            f64 sum = 0;
            for (const Tap& p : t) sum += p.w;
            for (Tap& p : t) p.w /= sum;
        }
        return all;
    };
    const auto tx = taps(image.width, width), ty = taps(image.height, height);
    // Premultiplied rows across first.
    std::vector<f64> mid(static_cast<usize>(width) * image.height * 4, 0);
    for (u32 y = 0; y < image.height; ++y)
        for (u32 x = 0; x < width; ++x) {
            f64* m = mid.data() + (static_cast<usize>(y) * width + x) * 4;
            for (const Tap& t : tx[x]) {
                const u8* p = at(image, t.at, y);
                const f64 a = p[3] / 255.0;
                for (int c = 0; c < 3; ++c) m[c] += p[c] * a * t.w;
                m[3] += p[3] * t.w;
            }
        }
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) {
            f64 sum[4] = {0, 0, 0, 0};
            for (const Tap& t : ty[y]) {
                const f64* m = mid.data() + (static_cast<usize>(t.at) * width + x) * 4;
                for (int c = 0; c < 4; ++c) sum[c] += m[c] * t.w;
            }
            u8* q = at(out, x, y);
            const f64 a = sum[3] / 255.0;
            for (int c = 0; c < 3; ++c) q[c] = a > 0 ? to_u8(sum[c] / a) : 0;
            q[3] = to_u8(sum[3]);
        }
    return out;
}

CookedTexture with_canvas(const CookedTexture& image, u32 width, u32 height, Anchor anchor) {
    CookedTexture out = blank(width, height);
    const int col = static_cast<int>(anchor) % 3, row = static_cast<int>(anchor) / 3;
    auto offset = [](int place, i64 to, i64 from) { return place == 0 ? 0 : place == 1 ? (to - from) / 2 : to - from; };
    const i64 ox = offset(col, width, image.width), oy = offset(row, height, image.height);
    for (u32 y = 0; y < image.height; ++y) {
        const i64 ty = y + oy;
        if (ty < 0 || ty >= height) continue;
        for (u32 x = 0; x < image.width; ++x) {
            const i64 tx = x + ox;
            if (tx < 0 || tx >= width) continue;
            std::memcpy(at(out, static_cast<u32>(tx), static_cast<u32>(ty)), at(image, x, y), 4);
        }
    }
    return out;
}

PixelRect opaque_bounds(const CookedTexture& image, const PixelRect& area_in) {
    const PixelRect area = clipped(image, area_in);
    PixelRect out;
    for (u32 y = area.y; y < area.y + area.h; ++y)
        for (u32 x = area.x; x < area.x + area.w; ++x)
            if (at(image, x, y)[3] != 0) out = united(out, {x, y, 1, 1});
    return out;
}

CookedTexture cropped(const CookedTexture& image, const PixelRect& rect) {
    const PixelRect r = clipped(image, rect);
    CookedTexture out = blank(r.w, r.h);
    out.rgba8 = copy_rect(image, r);
    return out;
}

bool each_frame(const CookedTexture& strip, u32 frames, const std::function<CookedTexture(const CookedTexture&)>& op,
                CookedTexture& out, std::string* why) {
    if (frames == 0 || strip.width % frames != 0) {
        if (why) *why = "Ширина " + std::to_string(strip.width) + " не делится на " + std::to_string(frames) + " кадров";
        return false;
    }
    std::vector<CookedTexture> done;
    for (u32 i = 0; i < frames; ++i) {
        done.push_back(op(cropped(strip, frame_rect(strip, frames, i))));
        if (done.back().width != done.front().width || done.back().height != done.front().height || done.back().width == 0 ||
            done.back().height == 0) {
            if (why) *why = "Кадры полосы вышли разного размера";
            return false;
        }
    }
    const u32 w = done.front().width, h = done.front().height;
    out = blank(w * frames, h);
    for (u32 i = 0; i < frames; ++i) paste_rect(out, {i * w, 0, w, h}, done[i].rgba8);
    return true;
}

bool sheet_to_strip(const CookedTexture& sheet, u32 frame_w, u32 frame_h, u32 columns, CookedTexture& out, u32* frames,
                    std::string* why) {
    auto refuse = [&](std::string text) {
        if (why) *why = std::move(text);
        return false;
    };
    if (frame_w == 0 || frame_h == 0 || columns == 0) return refuse("Размер кадра и столбцов — от 1");
    if (sheet.width % frame_w != 0 || sheet.height % frame_h != 0)
        return refuse("Лист " + std::to_string(sheet.width) + " × " + std::to_string(sheet.height) + " не делится на кадры " +
                      std::to_string(frame_w) + " × " + std::to_string(frame_h));
    const u32 have = sheet.width / frame_w, rows = sheet.height / frame_h;
    if (columns > have)
        return refuse("В листе шириной " + std::to_string(sheet.width) + " помещается " + std::to_string(have) + " столбц(а) по " +
                      std::to_string(frame_w) + ", а не " + std::to_string(columns));
    const u64 count = static_cast<u64>(columns) * rows;
    if (count * frame_w > 16384) return refuse("Полоса вышла бы шире 16384 пикселей");
    out = blank(static_cast<u32>(count) * frame_w, frame_h);
    u32 n = 0;
    for (u32 r = 0; r < rows; ++r)
        for (u32 c = 0; c < columns; ++c, ++n)
            paste_rect(out, {n * frame_w, 0, frame_w, frame_h}, copy_rect(sheet, {c * frame_w, r * frame_h, frame_w, frame_h}));
    if (frames) *frames = n;
    return true;
}

// --- effects ---

PixelRect apply_effect(CookedTexture& image, const PixelRect& area_in, const Effect& e) {
    const PixelRect area = clipped(image, area_in);
    if (area.empty()) return {};
    const std::vector<u8> before = copy_rect(image, area);
    // The colours the area reduces to, from the area's own colours.
    std::vector<Rgba> palette;
    if (e.kind == EffectKind::ReduceColors) {
        std::map<u32, u32> counts;
        for (usize i = 0; i < before.size(); i += 4)
            if (before[i + 3] != 0) ++counts[key_of(before.data() + i) | 255u];
        if (counts.size() > static_cast<usize>(std::clamp(e.a, 2, 64))) {
            std::vector<std::pair<u32, u32>> list(counts.begin(), counts.end());
            palette = median_cut(std::move(list), static_cast<usize>(std::clamp(e.a, 2, 64)));
        }
    }
    auto old = [&](i64 x, i64 y) -> const u8* {
        if (!area.contains(x, y)) return nullptr;
        return before.data() + ((static_cast<usize>(y) - area.y) * area.w + (static_cast<usize>(x) - area.x)) * 4;
    };
    const f64 contrast = e.b >= 0 ? 1 + std::clamp(e.b, 0, 100) / 100.0 * 3 : 1 + std::clamp(e.b, -100, 0) / 100.0;
    const f64 brightness = std::clamp(e.a, -100, 100) * 2.55;
    for (u32 y = area.y; y < area.y + area.h; ++y)
        for (u32 x = area.x; x < area.x + area.w; ++x) {
            u8* p = at(image, x, y);
            switch (e.kind) {
            case EffectKind::BrightnessContrast:
                if (p[3] == 0) break;
                for (int c = 0; c < 3; ++c) p[c] = to_u8((p[c] - 127.5) * contrast + 127.5 + brightness);
                break;
            case EffectKind::HueSaturation: {
                if (p[3] == 0) break;
                f64 h, s, l;
                to_hsl(p, h, s, l);
                h = std::fmod(h + std::clamp(e.a, -180, 180) / 360.0 + 1, 1.0);
                s = std::clamp(s * (1 + std::clamp(e.b, -100, 100) / 100.0), 0.0, 1.0);
                const f64 lt = std::clamp(e.c, -100, 100) / 100.0;
                l = lt >= 0 ? l + (1 - l) * lt : l + l * lt;
                from_hsl(h, s, l, p);
                break;
            }
            case EffectKind::Invert:
                if (p[3] == 0) break;
                for (int c = 0; c < 3; ++c) p[c] = static_cast<u8>(255 - p[c]);
                break;
            case EffectKind::Grayscale: {
                if (p[3] == 0) break;
                const u8 v = static_cast<u8>((299u * p[0] + 587u * p[1] + 114u * p[2] + 500u) / 1000u);
                p[0] = p[1] = p[2] = v;
                break;
            }
            case EffectKind::Outline: {
                if (p[3] != 0) break;
                const i64 next[4][2] = {{x - 1ll, y}, {x + 1ll, y}, {x, y - 1ll}, {x, y + 1ll}};
                for (const auto& n : next)
                    if (const u8* q = old(n[0], n[1]); q && q[3] != 0) {
                        p[0] = e.color.r;
                        p[1] = e.color.g;
                        p[2] = e.color.b;
                        p[3] = e.color.a;
                        break;
                    }
                break;
            }
            case EffectKind::AlphaThreshold: p[3] = p[3] >= std::clamp(e.a, 1, 255) ? 255 : 0; break;
            case EffectKind::ReduceColors: {
                if (p[3] == 0 || palette.empty()) break;
                const Rgba* best = nullptr;
                i64 best_d = 0;
                for (const Rgba& c : palette) {
                    const i64 dr = p[0] - c.r, dg = p[1] - c.g, db = p[2] - c.b;
                    const i64 d = dr * dr + dg * dg + db * db;
                    if (!best || d < best_d) {
                        best = &c;
                        best_d = d;
                    }
                }
                p[0] = best->r;
                p[1] = best->g;
                p[2] = best->b;
                break;
            }
            case EffectKind::ReplaceColor: {
                const int tol = std::clamp(e.a, 0, 255);
                const u8 want[4] = {e.color.r, e.color.g, e.color.b, e.color.a};
                bool near = true;
                for (int c = 0; c < 4 && near; ++c) near = std::abs(p[c] - want[c]) <= tol;
                if (!near) break;
                p[0] = e.to.r;
                p[1] = e.to.g;
                p[2] = e.to.b;
                p[3] = e.to.a;
                break;
            }
            }
        }
    // What changed, as one rectangle.
    PixelRect changed;
    for (u32 y = 0; y < area.h; ++y)
        for (u32 x = 0; x < area.w; ++x)
            if (std::memcmp(before.data() + (static_cast<usize>(y) * area.w + x) * 4, at(image, area.x + x, area.y + y), 4) != 0)
                changed = united(changed, {area.x + x, area.y + y, 1, 1});
    return changed;
}

} // namespace forge::assets
