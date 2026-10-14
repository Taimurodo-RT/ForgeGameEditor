#include "forge/pixel/svg.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace forge::pixel {

namespace {

bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool lower_eq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        if (x != b[i]) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

bool hex_digits(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
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

std::vector<std::string> unsupported(const SvgOutline& svg) {
    std::vector<std::string> out;
    for (usize k = 0; k < svg.elements.size(); ++k) {
        const i32 i = static_cast<i32>(k);
        const SvgElement& e = svg.elements[k];
        const std::string who = svg.name_of(i);
        const std::string& t = e.tag;
        if (t == "marker") out.push_back(who + ": маркеры <marker> ThorVG не рисует");
        if (t == "svg" && e.parent >= 0) out.push_back(who + ": вложенный <svg> ThorVG рисует не так, как браузер");
        if (t == "text" || t == "tspan" || t == "textPath") out.push_back(who + ": текст <" + t + "> в этой версии не рисуется — переведите его в кривые");
        if (t == "image") out.push_back(who + ": вставленные картинки <image> в этой версии не рисуются");
        if (t == "foreignObject" || t == "script") out.push_back(who + ": <" + t + "> не рисуется");
        if (t == "animate" || t == "animateTransform" || t == "animateMotion" || t == "set")
            out.push_back(who + ": анимация <" + t + "> не играет — движение задаёт скрипт позы");
        if (e.parent >= 0 && svg.elements[static_cast<usize>(e.parent)].tag == "filter" && t != "feGaussianBlur")
            out.push_back(who + ": из фильтров ThorVG рисует только размытие, <" + t + "> не нарисуется");
        for (const char* m : {"marker-start", "marker-mid", "marker-end", "marker"})
            if (t != "marker" && !e.property(m).empty() && e.property(m) != "none") out.push_back(who + ": " + m + " — маркеры ThorVG не рисует");
        if (svg.inside(i, "clipPath") && lower_eq(e.property("clip-rule"), "evenodd"))
            out.push_back(who + ": clip-rule=\"evenodd\" в <clipPath> — ThorVG обрежет по правилу nonzero");
        for (const char* p : {"fill", "stroke", "stop-color", "color", "flood-color"}) {
            const std::string v = e.property(p);
            const bool short_alpha = v.size() == 5 && v[0] == '#' && hex_digits(std::string_view(v).substr(1));
            const bool long_alpha = v.size() == 9 && v[0] == '#' && hex_digits(std::string_view(v).substr(1));
            const bool hsla = v.size() > 5 && lower_eq(std::string_view(v).substr(0, 5), "hsla(");
            if (short_alpha || long_alpha || hsla) out.push_back(who + ": цвет " + p + "=\"" + v + "\" — такую запись ThorVG не читает");
        }
    }
    return out;
}

bool shape_path(const SvgOutline& svg, i32 index, vector::Path& out, std::string& why) {
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
        if (!vector::parse_path(*d, out, &err)) {
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
        out = vector::rect(x, y, w, h, std::clamp(rx, 0.0, w / 2), std::clamp(ry, 0.0, h / 2));
        return true;
    }
    if (t == "circle") {
        f64 cx, cy, r;
        if (!length(e, "cx", 0, cx, why) || !length(e, "cy", 0, cy, why) || !length(e, "r", 0, r, why)) return false;
        out = vector::circle(cx, cy, r);
        return true;
    }
    if (t == "ellipse") {
        f64 cx, cy, rx, ry;
        if (!length(e, "cx", 0, cx, why) || !length(e, "cy", 0, cy, why) || !length(e, "rx", 0, rx, why) || !length(e, "ry", 0, ry, why))
            return false;
        out = vector::ellipse(cx, cy, rx, ry);
        return true;
    }
    if (t == "line") {
        f64 x1, y1, x2, y2;
        if (!length(e, "x1", 0, x1, why) || !length(e, "y1", 0, y1, why) || !length(e, "x2", 0, x2, why) || !length(e, "y2", 0, y2, why))
            return false;
        const vector::Point pts[] = {{x1, y1}, {x2, y2}};
        out = vector::polyline(pts, false);
        return true;
    }
    if (t == "polyline" || t == "polygon") {
        const std::string* p = e.attr("points");
        std::vector<vector::Point> pts;
        std::string err;
        if (!p || !vector::parse_points(*p, pts, &err)) {
            why = p ? "points: " + err : "у <" + t + "> нет points";
            return false;
        }
        out = vector::polyline(pts, t == "polygon");
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

} // namespace forge::pixel
