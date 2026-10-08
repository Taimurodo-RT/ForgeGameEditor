#include "forge/editor/color_pick.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace forge::editor::design {

namespace {

u8 to_byte(f64 v) { return static_cast<u8>(std::clamp(std::lround(v * 255.0), 0L, 255L)); }

std::string_view trimmed(std::string_view t) {
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
    return t;
}

bool hex_letter(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

} // namespace

Color hsva_to_color(const Hsva& c) {
    const f64 h = std::fmod(std::fmod(static_cast<f64>(c.h), 360.0) + 360.0, 360.0) / 60.0;
    const f64 s = std::clamp(static_cast<f64>(c.s), 0.0, 1.0), v = std::clamp(static_cast<f64>(c.v), 0.0, 1.0);
    const f64 chroma = v * s;
    const f64 x = chroma * (1 - std::abs(std::fmod(h, 2.0) - 1));
    f64 r = 0, g = 0, b = 0;
    switch (static_cast<int>(h) % 6) {
    case 0: r = chroma, g = x; break;
    case 1: r = x, g = chroma; break;
    case 2: g = chroma, b = x; break;
    case 3: g = x, b = chroma; break;
    case 4: r = x, b = chroma; break;
    default: r = chroma, b = x; break;
    }
    const f64 m = v - chroma;
    return {to_byte(r + m), to_byte(g + m), to_byte(b + m), to_byte(std::clamp(static_cast<f64>(c.a), 0.0, 1.0))};
}

Hsva color_to_hsva(Color c, const Hsva& keep) {
    const f64 r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    const f64 hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
    Hsva out;
    out.v = static_cast<f32>(hi);
    out.a = c.a / 255.0f;
    if (hi <= 0) {
        out.h = keep.h; // black: any hue, any saturation
        out.s = keep.s;
        return out;
    }
    out.s = static_cast<f32>(d / hi);
    if (d <= 0) {
        out.h = keep.h; // a grey: any hue
        return out;
    }
    f64 h = 0;
    if (hi == r) h = std::fmod((g - b) / d, 6.0);
    else if (hi == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
    if (h < 0) h += 360;
    out.h = static_cast<f32>(h);
    return out;
}

std::optional<Color> parse_hex_input(std::string_view text) {
    if (!hex_problem(text).empty()) return std::nullopt;
    return parse_color(trimmed(text));
}

std::string hex_problem(std::string_view text) {
    text = trimmed(text);
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.empty()) return "Пусто: впишите цвет, например FF8800";
    for (char c : text)
        if (!hex_letter(c)) return "Не цвет: только цифры 0–9 и буквы A–F";
    switch (text.size()) {
    case 3:
    case 4:
    case 6:
    case 8: return {};
    default: return "Не хватает знаков: нужно 3, 4, 6 или 8 (FF8800, а с прозрачностью FF880080)";
    }
}

std::optional<u8> parse_alpha_input(std::string_view text) {
    text = trimmed(text);
    if (!text.empty() && text.back() == '%') text = trimmed(text.substr(0, text.size() - 1));
    if (text.empty()) return std::nullopt;
    const std::string s(text);
    char* end = nullptr;
    const f64 v = std::strtod(s.c_str(), &end);
    if (!end || *end != 0 || !std::isfinite(v) || v < 0 || v > 100) return std::nullopt;
    return to_byte(v / 100.0);
}

Color unpremultiply(u8 r, u8 g, u8 b, u8 a) {
    if (a == 0) return {0, 0, 0, 0};
    if (a == 255) return {r, g, b, 255};
    auto un = [&](u8 v) { return static_cast<u8>(std::clamp((v * 255 + a / 2) / a, 0, 255)); };
    return {un(r), un(g), un(b), a};
}

} // namespace forge::editor::design
