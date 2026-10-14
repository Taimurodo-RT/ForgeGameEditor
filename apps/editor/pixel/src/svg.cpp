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

std::string_view local_name(std::string_view name) {
    const usize colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

void put_utf8(std::string& out, u32 c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// The five entities of XML and character references; anything else stays as written.
std::string decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const usize end = s.find(';', i);
        if (end == std::string_view::npos || end - i > 10) {
            out += s[i];
            continue;
        }
        const std::string_view name = s.substr(i + 1, end - i - 1);
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name.size() > 1 && name[0] == '#') {
            u32 c = 0;
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string_view digits = name.substr(hex ? 2 : 1);
            const auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), c, hex ? 16 : 10);
            if (ec != std::errc() || p != digits.data() + digits.size() || c > 0x10FFFF) {
                out += s.substr(i, end - i + 1);
            } else {
                put_utf8(out, c);
            }
        } else {
            out += s.substr(i, end - i + 1);
        }
        i = end;
    }
    return out;
}

struct Scanner {
    std::string_view s;
    usize i = 0;
    u32 line = 1;
    SvgOutline out;
    std::vector<i32> open; // elements not closed yet, innermost last

    bool at(std::string_view w) const { return s.substr(i, w.size()) == w; }
    void step(usize n) {
        for (usize k = 0; k < n && i < s.size(); ++k, ++i)
            if (s[i] == '\n') ++line;
    }
    void skip_space() {
        while (i < s.size() && space(s[i])) step(1);
    }
    bool fail(const std::string& why) {
        out.error = "строка " + std::to_string(line) + ": " + why;
        return false;
    }
    // Up to and past `end`; false when the file ends first.
    bool skip_past(std::string_view end, const char* what) {
        const usize at_end = s.find(end, i);
        if (at_end == std::string_view::npos) return fail(std::string(what) + " не закрыт");
        step(at_end + end.size() - i);
        return true;
    }
    std::string_view name() {
        const usize from = i;
        while (i < s.size() && !space(s[i]) && s[i] != '/' && s[i] != '>' && s[i] != '=') ++i;
        return s.substr(from, i - from);
    }

    bool doctype() {
        // <!DOCTYPE …> may hold an internal subset in brackets.
        int depth = 0;
        while (i < s.size()) {
            const char c = s[i];
            step(1);
            if (c == '[') ++depth;
            else if (c == ']') --depth;
            else if (c == '>' && depth <= 0) return true;
        }
        return fail("<!DOCTYPE> не закрыт");
    }

    bool start_tag() {
        step(1); // <
        const u32 at_line = line;
        const std::string_view tag = name();
        if (tag.empty()) return fail("после «<» нет имени элемента");
        SvgElement e;
        e.tag = std::string(local_name(tag));
        e.line = at_line;
        e.parent = open.empty() ? -1 : open.back();
        if (open.empty() && !out.elements.empty()) return fail("второй корневой элемент <" + e.tag + ">");
        for (;;) {
            skip_space();
            if (i >= s.size()) return fail("элемент <" + e.tag + "> не закрыт");
            if (at("/>")) {
                step(2);
                out.elements.push_back(std::move(e));
                return true;
            }
            if (s[i] == '>') {
                step(1);
                open.push_back(static_cast<i32>(out.elements.size()));
                out.elements.push_back(std::move(e));
                return true;
            }
            const std::string_view attr = name();
            if (attr.empty()) return fail("в <" + e.tag + "> лишний знак «" + std::string(1, s[i]) + "»");
            skip_space();
            if (i >= s.size() || s[i] != '=') return fail("у атрибута «" + std::string(attr) + "» нет значения");
            step(1);
            skip_space();
            if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) return fail("значение «" + std::string(attr) + "» не в кавычках");
            const char quote = s[i];
            step(1);
            const usize end = s.find(quote, i);
            if (end == std::string_view::npos) return fail("значение «" + std::string(attr) + "» не закрыто");
            std::string value = decode(s.substr(i, end - i));
            step(end + 1 - i);
            if (attr == "id") e.id = value;
            e.attrs.emplace_back(std::string(attr), std::move(value));
        }
    }

    bool end_tag() {
        step(2); // </
        const std::string_view tag = local_name(name());
        skip_space();
        if (i >= s.size() || s[i] != '>') return fail("</" + std::string(tag) + " не закрыт");
        step(1);
        if (open.empty() || out.elements[static_cast<usize>(open.back())].tag != tag)
            return fail("</" + std::string(tag) + "> закрывает не тот элемент" +
                        (open.empty() ? std::string() : " (открыт <" + out.elements[static_cast<usize>(open.back())].tag + ">)"));
        open.pop_back();
        return true;
    }

    bool run() {
        while (i < s.size()) {
            if (s[i] != '<') {
                step(1);
                continue;
            }
            bool ok;
            if (at("<!--")) ok = skip_past("-->", "комментарий");
            else if (at("<![CDATA[")) ok = skip_past("]]>", "CDATA");
            else if (at("<?")) ok = skip_past("?>", "<?…?>");
            else if (at("<!")) ok = doctype();
            else if (at("</")) ok = end_tag();
            else ok = start_tag();
            if (!ok) return false;
        }
        if (!open.empty()) return fail("элемент <" + out.elements[static_cast<usize>(open.back())].tag + "> не закрыт");
        if (out.elements.empty()) return fail("в файле нет ни одного элемента");
        if (out.elements.front().tag != "svg") return fail("это не SVG: корень — <" + out.elements.front().tag + ">");
        return true;
    }
};

bool inside(const SvgOutline& svg, i32 index, std::string_view tag) {
    for (i32 p = svg.elements[static_cast<usize>(index)].parent; p >= 0; p = svg.elements[static_cast<usize>(p)].parent)
        if (svg.elements[static_cast<usize>(p)].tag == tag) return true;
    return false;
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

const std::string* SvgElement::attr(std::string_view name) const {
    for (const auto& [k, v] : attrs)
        if (k == name) return &v;
    return nullptr;
}

std::string SvgElement::property(std::string_view name) const {
    if (const std::string* style = attr("style")) {
        std::string_view rest = *style;
        while (!rest.empty()) {
            const usize semi = rest.find(';');
            const std::string_view decl = rest.substr(0, semi);
            rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
            const usize colon = decl.find(':');
            if (colon != std::string_view::npos && trim(decl.substr(0, colon)) == name) return std::string(trim(decl.substr(colon + 1)));
        }
    }
    const std::string* v = attr(name);
    return v ? std::string(trim(*v)) : std::string();
}

i32 SvgOutline::find(std::string_view id) const {
    if (id.empty()) return -1;
    for (usize i = 0; i < elements.size(); ++i)
        if (elements[i].id == id) return static_cast<i32>(i);
    return -1;
}

std::string SvgOutline::name_of(i32 index) const {
    const SvgElement& e = elements[static_cast<usize>(index)];
    if (!e.id.empty()) return "«" + e.id + "»";
    return "<" + e.tag + "> в строке " + std::to_string(e.line);
}

SvgOutline scan_svg(std::string_view text) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3); // UTF-8 BOM
    Scanner sc;
    sc.s = text;
    if (!sc.run()) sc.out.elements.clear();
    return std::move(sc.out);
}

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
        if (inside(svg, i, "clipPath") && lower_eq(e.property("clip-rule"), "evenodd"))
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
