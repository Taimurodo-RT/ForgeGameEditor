#include "forge/vector/svg_shape.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace forge::vector {

namespace {

bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

// A length in user units: a number, "px" allowed.
bool length(const SvgElement& e, std::string_view attr, f64 fallback, f64& out, std::string& why) {
    const std::string* v = e.attr(attr);
    if (!v) {
        out = fallback;
        return true;
    }
    std::string_view t = trim(*v);
    if (t.size() > 2 && t.substr(t.size() - 2) == "px") t.remove_suffix(2);
    const char* end = t.data() + t.size();
    const auto [p, ec] = std::from_chars(t.data(), end, out);
    if (ec != std::errc() || p != end || !std::isfinite(out)) {
        why = std::string(attr) + "=\"" + *v + "\": нужно число (единицы, кроме px, не поддерживаются)";
        return false;
    }
    return true;
}

} // namespace

bool shape_path(const SvgOutline& svg, i32 index, Path& out, std::string& why) {
    if (index < 0 || static_cast<usize>(index) >= svg.elements.size()) {
        why = "нет такого элемента";
        return false;
    }
    const SvgElement& e = svg.elements[static_cast<usize>(index)];
    const std::string& t = e.tag;
    out = {};
    if (t == "path") {
        const std::string* d = e.attr("d");
        if (!d) {
            why = "у <path> нет d";
            return false;
        }
        std::string err;
        if (!parse_path(*d, out, &err)) {
            why = "d: " + err;
            return false;
        }
        return true;
    }
    if (t == "rect") {
        f64 x, y, w, h, rx, ry;
        if (!length(e, "x", 0, x, why) || !length(e, "y", 0, y, why) || !length(e, "width", 0, w, why) || !length(e, "height", 0, h, why) ||
            !length(e, "rx", -1, rx, why) || !length(e, "ry", -1, ry, why))
            return false;
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        out = rect(x, y, w, h, std::clamp(rx, 0.0, w / 2), std::clamp(ry, 0.0, h / 2));
        return true;
    }
    if (t == "circle") {
        f64 cx, cy, r;
        if (!length(e, "cx", 0, cx, why) || !length(e, "cy", 0, cy, why) || !length(e, "r", 0, r, why)) return false;
        out = circle(cx, cy, r);
        return true;
    }
    if (t == "ellipse") {
        f64 cx, cy, rx, ry;
        if (!length(e, "cx", 0, cx, why) || !length(e, "cy", 0, cy, why) || !length(e, "rx", 0, rx, why) || !length(e, "ry", 0, ry, why))
            return false;
        out = ellipse(cx, cy, rx, ry);
        return true;
    }
    if (t == "line") {
        f64 x1, y1, x2, y2;
        if (!length(e, "x1", 0, x1, why) || !length(e, "y1", 0, y1, why) || !length(e, "x2", 0, x2, why) || !length(e, "y2", 0, y2, why))
            return false;
        const Point pts[] = {{x1, y1}, {x2, y2}};
        out = polyline(pts, false);
        return true;
    }
    if (t == "polyline" || t == "polygon") {
        const std::string* p = e.attr("points");
        std::vector<Point> pts;
        std::string err;
        if (!p || !parse_points(*p, pts, &err)) {
            why = p ? "points: " + err : "у <" + t + "> нет points";
            return false;
        }
        out = polyline(pts, t == "polygon");
        return true;
    }
    why = "<" + t + "> — не фигура (нужны path, rect, circle, ellipse, line, polyline или polygon)";
    return false;
}

std::string transform_chain(const SvgOutline& svg, i32 index) {
    std::vector<std::string> parts;
    for (i32 i = index; i >= 0; i = svg.elements[static_cast<usize>(i)].parent)
        if (const std::string* t = svg.elements[static_cast<usize>(i)].attr("transform")) parts.push_back(std::string(trim(*t)));
    std::string out;
    for (usize k = parts.size(); k-- > 0;) out += (out.empty() ? "" : " | ") + parts[k];
    return out;
}

std::string inherited(const SvgOutline& svg, i32 index, std::string_view name) {
    for (i32 i = index; i >= 0; i = svg.elements[static_cast<usize>(i)].parent) {
        std::string v = svg.elements[static_cast<usize>(i)].property(name);
        if (!v.empty() && v != "inherit") return v;
    }
    return {};
}

bool display_none(const SvgOutline& svg, i32 index) {
    for (i32 i = index; i >= 0; i = svg.elements[static_cast<usize>(i)].parent)
        if (svg.elements[static_cast<usize>(i)].property("display") == "none") return true;
    return false;
}

f64 svg_number(const std::string& s, f64 fallback) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const f64 v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(v)) return fallback;
    if (*end == '%') return v / 100.0;
    return v;
}

bool parse_svg_color(std::string_view text, u8 rgb[3]) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    auto set = [&](int r, int g, int b) {
        rgb[0] = static_cast<u8>(r);
        rgb[1] = static_cast<u8>(g);
        rgb[2] = static_cast<u8>(b);
        return true;
    };
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (!text.empty() && text[0] == '#') {
        const std::string_view h = text.substr(1);
        for (char c : h)
            if (hex(c) < 0) return false;
        if (h.size() == 3) return set(hex(h[0]) * 17, hex(h[1]) * 17, hex(h[2]) * 17);
        if (h.size() == 6) return set(hex(h[0]) * 16 + hex(h[1]), hex(h[2]) * 16 + hex(h[3]), hex(h[4]) * 16 + hex(h[5]));
        return false;
    }
    std::string lower(text);
    for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (lower.rfind("rgb(", 0) == 0 && lower.back() == ')') {
        std::string body = lower.substr(4, lower.size() - 5);
        std::replace(body.begin(), body.end(), ',', ' ');
        const char* p = body.c_str();
        int ch[3];
        for (int& v : ch) {
            char* end = nullptr;
            f64 n = std::strtod(p, &end);
            if (end == p || !std::isfinite(n)) return false;
            if (*end == '%') n = n * 255.0 / 100.0, ++end;
            v = static_cast<int>(std::clamp(std::lround(n), 0L, 255L));
            p = end;
        }
        while (*p == ' ') ++p;
        if (*p) return false;
        return set(ch[0], ch[1], ch[2]);
    }
    static const struct {
        const char* name;
        u8 r, g, b;
    } kNames[] = {{"black", 0, 0, 0},       {"white", 255, 255, 255}, {"red", 255, 0, 0},       {"green", 0, 128, 0},
                  {"blue", 0, 0, 255},      {"yellow", 255, 255, 0},  {"orange", 255, 165, 0},  {"purple", 128, 0, 128},
                  {"gray", 128, 128, 128},  {"grey", 128, 128, 128},  {"silver", 192, 192, 192}, {"maroon", 128, 0, 0},
                  {"lime", 0, 255, 0},      {"navy", 0, 0, 128},      {"teal", 0, 128, 128},    {"aqua", 0, 255, 255},
                  {"fuchsia", 255, 0, 255}, {"olive", 128, 128, 0},   {"brown", 165, 42, 42},   {"pink", 255, 192, 203}};
    for (const auto& n : kNames)
        if (lower == n.name) return set(n.r, n.g, n.b);
    return false;
}

Look look_from_svg(const SvgOutline& svg, i32 index, const Look& base) {
    Look l = base;
    auto paint = [&](std::string_view prop, std::string_view opacity_prop, Paint& out, bool default_on) {
        const std::string v = inherited(svg, index, prop);
        u8 c[3];
        if (v.empty()) {
            out = default_on ? Paint{true, 0, 0, 0, 1} : Paint{};
        } else if (v == "none") {
            out.on = false;
        } else if (parse_svg_color(v, c)) {
            out = rgb8(c[0], c[1], c[2]);
        }
        out.opacity = std::clamp(svg_number(inherited(svg, index, opacity_prop), 1.0), 0.0, 1.0);
    };
    paint("fill", "fill-opacity", l.fill, true);
    paint("stroke", "stroke-opacity", l.stroke, false);
    l.stroke_width = std::max(0.0, svg_number(inherited(svg, index, "stroke-width"), 1.0));
    l.opacity = std::clamp(svg_number(svg.elements[static_cast<usize>(index)].property("opacity"), 1.0), 0.0, 1.0);
    return l;
}

} // namespace forge::vector
