#include "forge/editor/ui_design.h"

#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace forge::editor::design {

namespace {

// --- enums as words (the JSON keeps words, so files stay readable) ---

template <typename E, usize N> const char* word(E value, const char* const (&words)[N]) {
    const usize i = static_cast<usize>(value);
    return i < N ? words[i] : words[0];
}
template <typename E, usize N> E from_word(std::string_view text, const char* const (&words)[N], E fallback) {
    for (usize i = 0; i < N; ++i)
        if (text == words[i]) return static_cast<E>(i);
    return fallback;
}

const char* const kPaintKinds[] = {"solid", "linear", "radial", "image"};
const char* const kFits[] = {"fill", "fit", "tile", "stretch"};
const char* const kStrokeAligns[] = {"inside", "center", "outside"};
const char* const kStrokeStyles[] = {"solid", "dashed", "dotted"};
const char* const kEffects[] = {"drop-shadow", "inner-shadow", "layer-blur", "background-blur"};
const char* const kBlends[] = {"normal",     "multiply",    "screen",     "overlay",    "darken",     "lighten",
                               "color-dodge", "color-burn", "hard-light", "soft-light", "difference", "exclusion",
                               "hue",        "saturation",  "color",      "luminosity", "plus-lighter"};
const char* const kTextAligns[] = {"left", "center", "right", "justify"};
const char* const kTextCases[] = {"none", "upper", "lower", "title"};
const char* const kDecorations[] = {"none", "underline", "strike"};
const char* const kConstraints[] = {"start", "end", "both", "center", "scale"};
const char* const kSizings[] = {"fixed", "hug", "fill"};
const char* const kLayouts[] = {"none", "row", "column", "wrap"};
const char* const kArtRepeats[] = {"stretch", "repeat", "round", "space"};
const char* const kScreenFits[] = {"expand", "fit", "stretch"};
const char* const kNodeTypes[] = {"frame", "rectangle", "ellipse", "text", "image"};
const char* const kTypeWords[] = {"Рамка", "Прямоугольник", "Эллипс", "Текст", "Картинка"};

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string fmt(f32 v) {
    // Short numbers: 12, 12.5, 0.25 (no "12.000000").
    if (std::fabs(v - std::round(v)) < 0.0005f) return std::to_string(static_cast<long long>(std::lround(v)));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}
std::string px(f32 v) { return fmt(v) + "px"; }

std::string css_color(Color c, f32 opacity = 1) {
    const f32 a = std::clamp(c.a / 255.0f * opacity, 0.0f, 1.0f);
    if (a >= 0.999f) return color_hex(Color{c.r, c.g, c.b, 255});
    char buf[64];
    std::snprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %s)", c.r, c.g, c.b, fmt(a).c_str());
    return buf;
}

std::string escape_html(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string css_string(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\a ";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

// --- JSON writing ---

yyjson_mut_val* color_val(yyjson_mut_doc* doc, Color c) { return yyjson_mut_strcpy(doc, color_hex(c).c_str()); }

void put_num(yyjson_mut_doc* doc, yyjson_mut_val* obj, const char* key, f32 v) {
    yyjson_mut_obj_add_real(doc, obj, key, std::round(static_cast<double>(v) * 1000.0) / 1000.0);
}

yyjson_mut_val* write_paint(yyjson_mut_doc* doc, const Paint& p) {
    yyjson_mut_val* o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, o, "kind", word(p.kind, kPaintKinds));
    if (p.kind == PaintKind::Solid) yyjson_mut_obj_add_val(doc, o, "color", color_val(doc, p.color));
    if (p.kind == PaintKind::Linear) put_num(doc, o, "angle", p.angle);
    if (p.kind == PaintKind::Linear || p.kind == PaintKind::Radial) {
        yyjson_mut_val* stops = yyjson_mut_arr(doc);
        for (const GradientStop& s : p.stops) {
            yyjson_mut_val* so = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, so, "color", color_val(doc, s.color));
            put_num(doc, so, "at", s.position);
            yyjson_mut_arr_append(stops, so);
        }
        yyjson_mut_obj_add_val(doc, o, "stops", stops);
    }
    if (p.kind == PaintKind::Image) {
        yyjson_mut_obj_add_strcpy(doc, o, "image", p.image.c_str());
        yyjson_mut_obj_add_str(doc, o, "fit", word(p.fit, kFits));
        if (p.fit == ImageFit::Tile) {
            if (p.tile > 0) put_num(doc, o, "tile", p.tile);
            if (p.offset_x != 0) put_num(doc, o, "offset_x", p.offset_x);
            if (p.offset_y != 0) put_num(doc, o, "offset_y", p.offset_y);
        }
    }
    if (p.opacity != 1) put_num(doc, o, "opacity", p.opacity);
    if (!p.visible) yyjson_mut_obj_add_bool(doc, o, "visible", false);
    return o;
}

yyjson_mut_val* write_node(yyjson_mut_doc* doc, const Node& n) {
    yyjson_mut_val* o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, o, "id", n.id);
    yyjson_mut_obj_add_strcpy(doc, o, "name", n.name.c_str());
    yyjson_mut_obj_add_str(doc, o, "type", word(n.type, kNodeTypes));
    if (!n.visible) yyjson_mut_obj_add_bool(doc, o, "visible", false);
    if (n.locked) yyjson_mut_obj_add_bool(doc, o, "locked", true);
    put_num(doc, o, "x", n.x);
    put_num(doc, o, "y", n.y);
    put_num(doc, o, "w", n.w);
    put_num(doc, o, "h", n.h);
    if (n.rotation != 0) put_num(doc, o, "rotation", n.rotation);
    if (n.horizontal != Constraint::Start) yyjson_mut_obj_add_str(doc, o, "horizontal", word(n.horizontal, kConstraints));
    if (n.vertical != Constraint::Start) yyjson_mut_obj_add_str(doc, o, "vertical", word(n.vertical, kConstraints));
    if (n.width_sizing != Sizing::Fixed) yyjson_mut_obj_add_str(doc, o, "width_sizing", word(n.width_sizing, kSizings));
    if (n.height_sizing != Sizing::Fixed) yyjson_mut_obj_add_str(doc, o, "height_sizing", word(n.height_sizing, kSizings));
    if (n.absolute) yyjson_mut_obj_add_bool(doc, o, "absolute", true);
    if (n.layout.mode != LayoutMode::None) {
        yyjson_mut_val* l = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, l, "mode", word(n.layout.mode, kLayouts));
        put_num(doc, l, "gap", n.layout.gap);
        yyjson_mut_val* pad = yyjson_mut_arr(doc);
        for (f32 v : n.layout.padding) yyjson_mut_arr_add_real(doc, pad, v);
        yyjson_mut_obj_add_val(doc, l, "padding", pad);
        yyjson_mut_obj_add_uint(doc, l, "align", n.layout.align);
        if (n.layout.space_between) yyjson_mut_obj_add_bool(doc, l, "space_between", true);
        yyjson_mut_obj_add_val(doc, o, "layout", l);
    }
    if (n.clip) yyjson_mut_obj_add_bool(doc, o, "clip", true);
    if (!n.fills.empty()) {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (const Paint& p : n.fills) yyjson_mut_arr_append(a, write_paint(doc, p));
        yyjson_mut_obj_add_val(doc, o, "fills", a);
    }
    if (!n.strokes.empty()) {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (const Stroke& s : n.strokes) {
            yyjson_mut_val* so = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, so, "color", color_val(doc, s.color));
            put_num(doc, so, "width", s.width);
            yyjson_mut_obj_add_str(doc, so, "align", word(s.align, kStrokeAligns));
            if (s.style != StrokeStyle::Solid) yyjson_mut_obj_add_str(doc, so, "style", word(s.style, kStrokeStyles));
            if (!s.visible) yyjson_mut_obj_add_bool(doc, so, "visible", false);
            yyjson_mut_arr_append(a, so);
        }
        yyjson_mut_obj_add_val(doc, o, "strokes", a);
    }
    if (n.radius != std::array<f32, 4>{}) {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (f32 v : n.radius) yyjson_mut_arr_add_real(doc, a, v);
        yyjson_mut_obj_add_val(doc, o, "radius", a);
    }
    if (n.opacity != 1) put_num(doc, o, "opacity", n.opacity);
    if (n.blend != Blend::Normal) yyjson_mut_obj_add_str(doc, o, "blend", blend_css(n.blend));
    if (!n.effects.empty()) {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (const Effect& e : n.effects) {
            yyjson_mut_val* eo = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, eo, "kind", word(e.kind, kEffects));
            if (e.kind == EffectKind::DropShadow || e.kind == EffectKind::InnerShadow) {
                yyjson_mut_obj_add_val(doc, eo, "color", color_val(doc, e.color));
                put_num(doc, eo, "x", e.x);
                put_num(doc, eo, "y", e.y);
                put_num(doc, eo, "spread", e.spread);
            }
            put_num(doc, eo, "blur", e.blur);
            if (!e.visible) yyjson_mut_obj_add_bool(doc, eo, "visible", false);
            yyjson_mut_arr_append(a, eo);
        }
        yyjson_mut_obj_add_val(doc, o, "effects", a);
    }
    if (!n.frame.image.empty()) {
        const FrameArt& f = n.frame;
        yyjson_mut_val* fo = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, fo, "image", f.image.c_str());
        yyjson_mut_val* slice = yyjson_mut_arr(doc);
        for (f32 v : f.slice) yyjson_mut_arr_add_real(doc, slice, v);
        yyjson_mut_obj_add_val(doc, fo, "slice", slice);
        if (f.scale != 1) put_num(doc, fo, "scale", f.scale);
        if (!f.fill) yyjson_mut_obj_add_bool(doc, fo, "fill", false);
        if (f.repeat != ArtRepeat::Stretch) yyjson_mut_obj_add_str(doc, fo, "repeat", word(f.repeat, kArtRepeats));
        if (!f.visible) yyjson_mut_obj_add_bool(doc, fo, "visible", false);
        yyjson_mut_obj_add_val(doc, o, "frame", fo);
    }
    if (!n.mask.image.empty()) {
        yyjson_mut_val* mo = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, mo, "image", n.mask.image.c_str());
        yyjson_mut_obj_add_str(doc, mo, "fit", word(n.mask.fit, kFits));
        if (!n.mask.visible) yyjson_mut_obj_add_bool(doc, mo, "visible", false);
        yyjson_mut_obj_add_val(doc, o, "mask", mo);
    }
    if (n.type == NodeType::Text) {
        yyjson_mut_obj_add_strcpy(doc, o, "text", n.text.c_str());
        const TextStyle& t = n.text_style;
        yyjson_mut_val* to = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, to, "family", t.family.c_str());
        put_num(doc, to, "size", t.size);
        yyjson_mut_obj_add_uint(doc, to, "weight", t.weight);
        if (t.italic) yyjson_mut_obj_add_bool(doc, to, "italic", true);
        if (t.line_height > 0) put_num(doc, to, "line_height", t.line_height);
        if (t.letter_spacing != 0) put_num(doc, to, "letter_spacing", t.letter_spacing);
        yyjson_mut_obj_add_str(doc, to, "align", word(t.align, kTextAligns));
        if (t.text_case != TextCase::None) yyjson_mut_obj_add_str(doc, to, "case", word(t.text_case, kTextCases));
        if (t.decoration != TextDecoration::None)
            yyjson_mut_obj_add_str(doc, to, "decoration", word(t.decoration, kDecorations));
        yyjson_mut_obj_add_val(doc, to, "color", color_val(doc, t.color));
        yyjson_mut_obj_add_val(doc, o, "style", to);
    }
    if (!n.children.empty()) {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (const Node& c : n.children) yyjson_mut_arr_append(a, write_node(doc, c));
        yyjson_mut_obj_add_val(doc, o, "children", a);
    }
    return o;
}

// --- JSON reading ---

f32 num(yyjson_val* o, const char* key, f32 fallback) {
    yyjson_val* v = yyjson_obj_get(o, key);
    return yyjson_is_num(v) ? static_cast<f32>(yyjson_get_num(v)) : fallback;
}
bool flag(yyjson_val* o, const char* key, bool fallback) {
    yyjson_val* v = yyjson_obj_get(o, key);
    return yyjson_is_bool(v) ? yyjson_get_bool(v) : fallback;
}
std::string str(yyjson_val* o, const char* key, std::string fallback = {}) {
    yyjson_val* v = yyjson_obj_get(o, key);
    return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : fallback;
}
Color color_of(yyjson_val* o, const char* key, Color fallback) {
    yyjson_val* v = yyjson_obj_get(o, key);
    if (!yyjson_is_str(v)) return fallback;
    return parse_color(yyjson_get_str(v)).value_or(fallback);
}
template <typename E, usize N> E enum_of(yyjson_val* o, const char* key, const char* const (&words)[N], E fallback) {
    yyjson_val* v = yyjson_obj_get(o, key);
    return yyjson_is_str(v) ? from_word(yyjson_get_str(v), words, fallback) : fallback;
}
template <usize N> std::array<f32, N> nums(yyjson_val* o, const char* key) {
    std::array<f32, N> out{};
    yyjson_val* a = yyjson_obj_get(o, key);
    if (!yyjson_is_arr(a)) return out;
    for (usize i = 0; i < N && i < yyjson_arr_size(a); ++i) {
        yyjson_val* v = yyjson_arr_get(a, i);
        if (yyjson_is_num(v)) out[i] = static_cast<f32>(yyjson_get_num(v));
    }
    return out;
}

Paint read_paint(yyjson_val* o) {
    Paint p;
    p.kind = enum_of(o, "kind", kPaintKinds, PaintKind::Solid);
    p.color = color_of(o, "color", p.color);
    p.angle = num(o, "angle", p.angle);
    if (yyjson_val* stops = yyjson_obj_get(o, "stops"); yyjson_is_arr(stops)) {
        usize i, n;
        yyjson_val* s;
        yyjson_arr_foreach(stops, i, n, s) p.stops.push_back({color_of(s, "color", Color{}), num(s, "at", 0)});
    }
    p.image = str(o, "image");
    p.fit = enum_of(o, "fit", kFits, ImageFit::Fill);
    p.tile = std::max(num(o, "tile", 0), 0.0f);
    p.offset_x = num(o, "offset_x", 0);
    p.offset_y = num(o, "offset_y", 0);
    p.opacity = num(o, "opacity", 1);
    p.visible = flag(o, "visible", true);
    return p;
}

bool read_node(yyjson_val* o, Node& n, int depth) {
    if (!yyjson_is_obj(o) || depth > 64) return false;
    n.id = static_cast<u32>(num(o, "id", 0));
    n.name = str(o, "name");
    n.type = enum_of(o, "type", kNodeTypes, NodeType::Frame);
    n.visible = flag(o, "visible", true);
    n.locked = flag(o, "locked", false);
    n.x = num(o, "x", 0);
    n.y = num(o, "y", 0);
    n.w = num(o, "w", 100);
    n.h = num(o, "h", 100);
    n.rotation = num(o, "rotation", 0);
    n.horizontal = enum_of(o, "horizontal", kConstraints, Constraint::Start);
    n.vertical = enum_of(o, "vertical", kConstraints, Constraint::Start);
    n.width_sizing = enum_of(o, "width_sizing", kSizings, Sizing::Fixed);
    n.height_sizing = enum_of(o, "height_sizing", kSizings, Sizing::Fixed);
    n.absolute = flag(o, "absolute", false);
    if (yyjson_val* l = yyjson_obj_get(o, "layout"); yyjson_is_obj(l)) {
        n.layout.mode = enum_of(l, "mode", kLayouts, LayoutMode::None);
        n.layout.gap = num(l, "gap", 0);
        n.layout.padding = nums<4>(l, "padding");
        n.layout.align = static_cast<u8>(std::clamp(static_cast<int>(num(l, "align", 0)), 0, 8));
        n.layout.space_between = flag(l, "space_between", false);
    }
    n.clip = flag(o, "clip", false);
    usize i, count;
    yyjson_val* v;
    if (yyjson_val* a = yyjson_obj_get(o, "fills"); yyjson_is_arr(a)) {
        yyjson_arr_foreach(a, i, count, v) n.fills.push_back(read_paint(v));
    }
    if (yyjson_val* a = yyjson_obj_get(o, "strokes"); yyjson_is_arr(a)) {
        yyjson_arr_foreach(a, i, count, v) {
            Stroke s;
            s.color = color_of(v, "color", s.color);
            s.width = num(v, "width", 1);
            s.align = enum_of(v, "align", kStrokeAligns, StrokeAlign::Inside);
            s.style = enum_of(v, "style", kStrokeStyles, StrokeStyle::Solid);
            s.visible = flag(v, "visible", true);
            n.strokes.push_back(s);
        }
    }
    n.radius = nums<4>(o, "radius");
    n.opacity = num(o, "opacity", 1);
    n.blend = enum_of(o, "blend", kBlends, Blend::Normal);
    if (yyjson_val* a = yyjson_obj_get(o, "effects"); yyjson_is_arr(a)) {
        yyjson_arr_foreach(a, i, count, v) {
            Effect e;
            e.kind = enum_of(v, "kind", kEffects, EffectKind::DropShadow);
            e.color = color_of(v, "color", e.color);
            e.x = num(v, "x", 0);
            e.y = num(v, "y", 0);
            e.blur = num(v, "blur", 0);
            e.spread = num(v, "spread", 0);
            e.visible = flag(v, "visible", true);
            n.effects.push_back(e);
        }
    }
    if (yyjson_val* f = yyjson_obj_get(o, "frame"); yyjson_is_obj(f)) {
        n.frame.image = str(f, "image");
        if (yyjson_is_arr(yyjson_obj_get(f, "slice"))) n.frame.slice = nums<4>(f, "slice");
        n.frame.scale = std::max(num(f, "scale", 1), 0.01f);
        n.frame.fill = flag(f, "fill", true);
        n.frame.repeat = enum_of(f, "repeat", kArtRepeats, ArtRepeat::Stretch);
        n.frame.visible = flag(f, "visible", true);
    }
    if (yyjson_val* m = yyjson_obj_get(o, "mask"); yyjson_is_obj(m)) {
        n.mask.image = str(m, "image");
        n.mask.fit = enum_of(m, "fit", kFits, ImageFit::Stretch);
        n.mask.visible = flag(m, "visible", true);
    }
    n.text = str(o, "text");
    if (yyjson_val* t = yyjson_obj_get(o, "style"); yyjson_is_obj(t)) {
        TextStyle& s = n.text_style;
        s.family = str(t, "family", s.family);
        s.size = num(t, "size", s.size);
        s.weight = static_cast<u16>(num(t, "weight", s.weight));
        s.italic = flag(t, "italic", false);
        s.line_height = num(t, "line_height", 0);
        s.letter_spacing = num(t, "letter_spacing", 0);
        s.align = enum_of(t, "align", kTextAligns, TextAlign::Left);
        s.text_case = enum_of(t, "case", kTextCases, TextCase::None);
        s.decoration = enum_of(t, "decoration", kDecorations, TextDecoration::None);
        s.color = color_of(t, "color", s.color);
    }
    if (yyjson_val* a = yyjson_obj_get(o, "children"); yyjson_is_arr(a)) {
        yyjson_arr_foreach(a, i, count, v) {
            Node c;
            if (read_node(v, c, depth + 1)) n.children.push_back(std::move(c));
        }
    }
    return true;
}

u32 max_id(const Node& n) {
    u32 m = n.id;
    for (const Node& c : n.children) m = std::max(m, max_id(c));
    return m;
}

// --- CSS ---

std::string gradient_stops(const Paint& p) {
    std::string out;
    std::vector<GradientStop> stops = p.stops;
    if (stops.empty()) stops = {{Color{255, 255, 255, 255}, 0}, {Color{0, 0, 0, 255}, 1}};
    std::sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.position < b.position; });
    for (const GradientStop& s : stops) {
        if (!out.empty()) out += ", ";
        out += css_color(s.color, p.opacity) + " " + fmt(s.position * 100) + "%";
    }
    return out;
}

void add(std::string& css, const char* property, const std::string& value) {
    css += "  ";
    css += property;
    css += ": ";
    css += value;
    css += ";\n";
}

const char* flex_align(int i) { return i == 0 ? "flex-start" : i == 1 ? "center" : "flex-end"; }

} // namespace

std::optional<Color> parse_color(std::string_view text) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    for (char c : text)
        if (hex_digit(c) < 0) return std::nullopt;
    Color c;
    auto pair = [&](usize i) { return static_cast<u8>(hex_digit(text[i]) * 16 + hex_digit(text[i + 1])); };
    auto single = [&](usize i) { return static_cast<u8>(hex_digit(text[i]) * 17); };
    switch (text.size()) {
    case 3: c = {single(0), single(1), single(2), 255}; break;
    case 4: c = {single(0), single(1), single(2), single(3)}; break;
    case 6: c = {pair(0), pair(2), pair(4), 255}; break;
    case 8: c = {pair(0), pair(2), pair(4), pair(6)}; break;
    default: return std::nullopt;
    }
    return c;
}

std::string color_hex(Color c) {
    char buf[16];
    if (c.a == 255) std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    else std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return buf;
}

const char* blend_css(Blend b) { return word(b, kBlends); }
const char* node_type_name(NodeType t) { return word(t, kNodeTypes); }

Screen make_screen(std::string title, f32 width, f32 height) {
    Screen s;
    s.title = std::move(title);
    s.width = width;
    s.height = height;
    s.root.id = s.next_id++;
    s.root.name = s.title;
    s.root.type = NodeType::Frame;
    s.root.w = width;
    s.root.h = height;
    s.root.clip = true;
    return s;
}

std::string save_screen(const Screen& screen) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_uint(doc, root, "version", 1);
    yyjson_mut_obj_add_strcpy(doc, root, "title", screen.title.c_str());
    put_num(doc, root, "width", screen.width);
    put_num(doc, root, "height", screen.height);
    if (!screen.guides.empty()) {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (const Guide& g : screen.guides) {
            yyjson_mut_val* go = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_bool(doc, go, "vertical", g.vertical);
            put_num(doc, go, "at", g.position);
            yyjson_mut_arr_append(a, go);
        }
        yyjson_mut_obj_add_val(doc, root, "guides", a);
    }
    {
        yyjson_mut_val* so = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, so, "font", screen.text.family.c_str());
        put_num(doc, so, "font_size", screen.text.size);
        yyjson_mut_obj_add_val(doc, so, "text_color", color_val(doc, screen.text.color));
        yyjson_mut_obj_add_str(doc, so, "fit", word(screen.fit, kScreenFits));
        if (screen.fit == ScreenFit::Fit) yyjson_mut_obj_add_val(doc, so, "bars", color_val(doc, screen.bars));
        if (screen.safe > 0) put_num(doc, so, "safe", screen.safe);
        yyjson_mut_obj_add_val(doc, root, "settings", so);
    }
    yyjson_mut_obj_add_val(doc, root, "root", write_node(doc, screen.root));
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
    std::string out = text ? std::string(text, len) + "\n" : std::string();
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

bool load_screen(std::string_view json, Screen& out, std::string* error) {
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(json.data()), json.size(), 0, nullptr, &err);
    if (!doc) {
        if (error) *error = std::string("JSON: ") + (err.msg ? err.msg : "?") + " at " + std::to_string(err.pos);
        return false;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    Screen s;
    s.title = str(root, "title");
    s.width = std::max(num(root, "width", 1920), 1.0f);
    s.height = std::max(num(root, "height", 1080), 1.0f);
    if (yyjson_val* a = yyjson_obj_get(root, "guides"); yyjson_is_arr(a)) {
        usize i, n;
        yyjson_val* g;
        yyjson_arr_foreach(a, i, n, g) s.guides.push_back({flag(g, "vertical", true), num(g, "at", 0)});
    }
    if (yyjson_val* so = yyjson_obj_get(root, "settings"); yyjson_is_obj(so)) {
        s.text.family = str(so, "font", s.text.family);
        s.text.size = std::max(num(so, "font_size", s.text.size), 1.0f);
        s.text.color = color_of(so, "text_color", s.text.color);
        s.fit = enum_of(so, "fit", kScreenFits, ScreenFit::Expand);
        s.bars = color_of(so, "bars", s.bars);
        s.safe = std::max(num(so, "safe", 0), 0.0f);
    }
    const bool ok = read_node(yyjson_obj_get(root, "root"), s.root, 0);
    yyjson_doc_free(doc);
    if (!ok) {
        if (error) *error = "no root frame";
        return false;
    }
    s.root.w = s.width;
    s.root.h = s.height;
    s.next_id = max_id(s.root) + 1;
    out = std::move(s);
    return true;
}

Node* find(Node& root, u32 id) {
    if (root.id == id) return &root;
    for (Node& c : root.children)
        if (Node* n = find(c, id)) return n;
    return nullptr;
}

const Node* find(const Node& root, u32 id) { return find(const_cast<Node&>(root), id); }

Node* parent_of(Node& root, u32 id) {
    for (Node& c : root.children) {
        if (c.id == id) return &root;
        if (Node* p = parent_of(c, id)) return p;
    }
    return nullptr;
}

std::vector<u32> path_to(const Node& root, u32 id) {
    if (root.id == id) return {id};
    for (const Node& c : root.children) {
        std::vector<u32> p = path_to(c, id);
        if (!p.empty()) {
            p.insert(p.begin(), root.id);
            return p;
        }
    }
    return {};
}

std::optional<Node> remove(Node& root, u32 id) {
    Node* parent = parent_of(root, id);
    if (!parent) return std::nullopt;
    for (auto it = parent->children.begin(); it != parent->children.end(); ++it) {
        if (it->id != id) continue;
        Node out = std::move(*it);
        parent->children.erase(it);
        return out;
    }
    return std::nullopt;
}

void renumber(Screen& screen, Node& node) {
    node.id = screen.next_id++;
    for (Node& c : node.children) renumber(screen, c);
}

std::string fresh_name(const Screen& screen, NodeType type) {
    const std::string base = word(type, kTypeWords);
    int highest = 0;
    auto visit = [&](auto&& self, const Node& n) -> void {
        if (n.name.rfind(base + " ", 0) == 0) highest = std::max(highest, std::atoi(n.name.c_str() + base.size() + 1));
        for (const Node& c : n.children) self(self, c);
    };
    visit(visit, screen.root);
    return base + " " + std::to_string(highest + 1);
}

std::string node_css(const Node& n, const Node* parent, f32 pw, f32 ph) {
    std::string css = "#n" + std::to_string(n.id) + " {\n";
    if (!n.visible) add(css, "display", "none");

    const bool in_flow = parent && parent->layout.mode != LayoutMode::None && !n.absolute;
    std::vector<std::string> transforms;
    if (!parent) {
        add(css, "position", "relative");
        add(css, "width", "100%");
        add(css, "height", "100%");
    } else if (in_flow) {
        add(css, "position", "relative");
        const bool row = parent->layout.mode != LayoutMode::Column;
        auto axis = [&](Sizing sizing, f32 size, const char* property, bool main) {
            switch (sizing) {
            case Sizing::Fixed:
                add(css, property, px(size));
                if (main) add(css, "flex-shrink", "0");
                break;
            case Sizing::Hug:
                if (main) add(css, "flex-shrink", "0");
                break;
            case Sizing::Fill:
                if (main) add(css, "flex", "1 1 0");
                else add(css, "align-self", "stretch");
                break;
            }
        };
        axis(n.width_sizing, n.w, "width", row);
        axis(n.height_sizing, n.h, "height", !row);
    } else {
        add(css, "position", "absolute");
        auto axis = [&](Constraint c, Sizing sizing, f32 pos, f32 size, f32 parent_size, const char* start,
                        const char* end, const char* length, const char* translate) {
            const bool hug = sizing == Sizing::Hug;
            switch (c) {
            case Constraint::Start:
                add(css, start, px(pos));
                if (!hug) add(css, length, px(size));
                break;
            case Constraint::End:
                add(css, end, px(parent_size - pos - size));
                if (!hug) add(css, length, px(size));
                break;
            case Constraint::Both:
                add(css, start, px(pos));
                add(css, end, px(parent_size - pos - size));
                break;
            case Constraint::Center:
                if (hug) {
                    add(css, start, "calc(50% + " + px(pos + size * 0.5f - parent_size * 0.5f) + ")");
                    transforms.push_back(std::string(translate) + "(-50%)");
                } else {
                    add(css, start, "calc(50% + " + px(pos - parent_size * 0.5f) + ")");
                    add(css, length, px(size));
                }
                break;
            case Constraint::Scale:
                add(css, start, fmt(parent_size > 0 ? pos / parent_size * 100 : 0) + "%");
                if (!hug) add(css, length, fmt(parent_size > 0 ? size / parent_size * 100 : 0) + "%");
                break;
            }
        };
        axis(n.horizontal, n.width_sizing, n.x, n.w, pw, "left", "right", "width", "translateX");
        axis(n.vertical, n.height_sizing, n.y, n.h, ph, "top", "bottom", "height", "translateY");
    }
    add(css, "box-sizing", "border-box");

    // Auto layout.
    if (n.is_container() && n.layout.mode != LayoutMode::None) {
        const AutoLayout& l = n.layout;
        add(css, "display", "flex");
        add(css, "flex-direction", l.mode == LayoutMode::Column ? "column" : "row");
        if (l.mode == LayoutMode::Wrap) add(css, "flex-wrap", "wrap");
        if (l.gap != 0) add(css, "gap", px(l.gap));
        if (l.padding != std::array<f32, 4>{})
            add(css, "padding", px(l.padding[0]) + " " + px(l.padding[1]) + " " + px(l.padding[2]) + " " + px(l.padding[3]));
        const int row = l.align / 3, col = l.align % 3;
        const bool horizontal = l.mode != LayoutMode::Column;
        const int main = horizontal ? col : row, cross = horizontal ? row : col;
        add(css, "justify-content", l.space_between ? "space-between" : flex_align(main));
        add(css, "align-items", flex_align(cross));
        if (l.mode == LayoutMode::Wrap) add(css, "align-content", flex_align(cross));
    }

    // Fills: CSS lists background layers top first.
    if (n.type != NodeType::Text) {
        std::vector<const Paint*> shown;
        for (const Paint& p : n.fills)
            if (p.visible) shown.push_back(&p);
        if (shown.size() == 1 && shown[0]->kind == PaintKind::Solid) {
            add(css, "background-color", css_color(shown[0]->color, shown[0]->opacity));
        } else if (!shown.empty()) {
            std::string images, sizes, repeats, positions;
            for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
                const Paint& p = **it;
                std::string image, size = "auto", repeat = "no-repeat", position = "0% 0%";
                switch (p.kind) {
                case PaintKind::Solid: {
                    const std::string c = css_color(p.color, p.opacity);
                    image = "linear-gradient(" + c + ", " + c + ")";
                    break;
                }
                case PaintKind::Linear: image = "linear-gradient(" + fmt(p.angle) + "deg, " + gradient_stops(p) + ")"; break;
                case PaintKind::Radial: image = "radial-gradient(ellipse farthest-side, " + gradient_stops(p) + ")"; break;
                case PaintKind::Image:
                    image = "url(" + css_string("../" + p.image) + ")";
                    switch (p.fit) {
                    case ImageFit::Fill: size = "cover"; position = "center"; break;
                    case ImageFit::Fit: size = "contain"; position = "center"; break;
                    case ImageFit::Tile:
                        repeat = "repeat";
                        if (p.tile > 0) size = px(p.tile) + " auto";
                        position = px(p.offset_x) + " " + px(p.offset_y);
                        break;
                    case ImageFit::Stretch: size = "100% 100%"; break;
                    }
                    break;
                }
                auto join = [](std::string& list, const std::string& v) { list += (list.empty() ? "" : ", ") + v; };
                join(images, image);
                join(sizes, size);
                join(repeats, repeat);
                join(positions, position);
            }
            add(css, "background-image", images);
            add(css, "background-size", sizes);
            add(css, "background-repeat", repeats);
            add(css, "background-position", positions);
        }
    }

    // Stroke: an outline does not change the layer's size, like Figma's strokes.
    for (const Stroke& s : n.strokes) {
        if (!s.visible || s.width <= 0) continue;
        const char* style = s.style == StrokeStyle::Dashed ? "dashed" : s.style == StrokeStyle::Dotted ? "dotted" : "solid";
        add(css, "outline", px(s.width) + " " + style + " " + css_color(s.color));
        const f32 offset = s.align == StrokeAlign::Inside ? -s.width : s.align == StrokeAlign::Center ? -s.width * 0.5f : 0;
        add(css, "outline-offset", px(offset));
        break; // one stroke for now
    }

    // The frame picture: CSS border-image over the layer's own box (no border, so nothing inside moves).
    if (!n.frame.image.empty() && n.frame.visible) {
        const FrameArt& f = n.frame;
        std::string widths;
        for (f32 v : f.slice) widths += (widths.empty() ? "" : " ") + px(v * f.scale);
        add(css, "border-image-source", "url(" + css_string("../" + f.image) + ")");
        add(css, "border-image-slice", fmt(f.slice[0]) + " " + fmt(f.slice[1]) + " " + fmt(f.slice[2]) + " " + fmt(f.slice[3]) +
                                           (f.fill ? " fill" : ""));
        add(css, "border-image-width", widths);
        add(css, "border-image-repeat", word(f.repeat, kArtRepeats));
    }
    if (!n.mask.image.empty() && n.mask.visible) {
        add(css, "mask-image", "url(" + css_string("../" + n.mask.image) + ")");
        const ImageFit fit = n.mask.fit;
        add(css, "mask-size", fit == ImageFit::Fill ? "cover" : fit == ImageFit::Fit ? "contain" : fit == ImageFit::Tile ? "auto" : "100% 100%");
        add(css, "mask-position", "center");
        add(css, "mask-repeat", fit == ImageFit::Tile ? "repeat" : "no-repeat");
    }

    if (n.type == NodeType::Ellipse) add(css, "border-radius", "50%");
    else if (n.radius != std::array<f32, 4>{})
        add(css, "border-radius", px(n.radius[0]) + " " + px(n.radius[1]) + " " + px(n.radius[2]) + " " + px(n.radius[3]));

    // Effects.
    std::string shadows, text_shadows, filter, backdrop;
    for (const Effect& e : n.effects) {
        if (!e.visible) continue;
        switch (e.kind) {
        case EffectKind::DropShadow:
        case EffectKind::InnerShadow: {
            const bool inner = e.kind == EffectKind::InnerShadow;
            if (n.type == NodeType::Text && !inner) {
                text_shadows += (text_shadows.empty() ? "" : ", ") + px(e.x) + " " + px(e.y) + " " + px(e.blur) + " " +
                                css_color(e.color);
                break;
            }
            shadows += (shadows.empty() ? "" : ", ") + std::string(inner ? "inset " : "") + px(e.x) + " " + px(e.y) +
                       " " + px(e.blur) + " " + px(e.spread) + " " + css_color(e.color);
            break;
        }
        case EffectKind::LayerBlur: filter += (filter.empty() ? "" : " ") + ("blur(" + px(e.blur * 0.5f) + ")"); break;
        case EffectKind::BackgroundBlur:
            backdrop += (backdrop.empty() ? "" : " ") + ("blur(" + px(e.blur * 0.5f) + ")");
            break;
        }
    }
    if (!shadows.empty()) add(css, "box-shadow", shadows);
    if (!text_shadows.empty()) add(css, "text-shadow", text_shadows);
    if (!filter.empty()) add(css, "filter", filter);
    if (!backdrop.empty()) add(css, "backdrop-filter", backdrop);

    if (n.opacity < 1) add(css, "opacity", fmt(std::max(n.opacity, 0.0f)));
    if (n.blend != Blend::Normal) add(css, "mix-blend-mode", blend_css(n.blend));
    if (n.clip) add(css, "overflow", "hidden");
    if (n.rotation != 0) transforms.push_back("rotate(" + fmt(n.rotation) + "deg)");
    if (!transforms.empty()) {
        std::string t;
        for (const std::string& part : transforms) t += (t.empty() ? "" : " ") + part;
        add(css, "transform", t);
    }

    if (n.type == NodeType::Text) {
        const TextStyle& t = n.text_style;
        if (!t.family.empty()) add(css, "font-family", css_string(t.family)); // else the screen's
        if (t.size > 0) add(css, "font-size", px(t.size));
        add(css, "font-weight", std::to_string(t.weight));
        if (t.italic) add(css, "font-style", "italic");
        add(css, "line-height", t.line_height > 0 ? px(t.line_height) : "normal");
        if (t.letter_spacing != 0) add(css, "letter-spacing", px(t.letter_spacing));
        add(css, "text-align", word(t.align, kTextAligns));
        if (t.text_case != TextCase::None)
            add(css, "text-transform", t.text_case == TextCase::Upper ? "uppercase"
                                       : t.text_case == TextCase::Lower ? "lowercase"
                                                                        : "capitalize");
        if (t.decoration != TextDecoration::None)
            add(css, "text-decoration", t.decoration == TextDecoration::Underline ? "underline" : "line-through");
        add(css, "color", css_color(t.color));
        add(css, "white-space", n.width_sizing == Sizing::Hug ? "pre" : "pre-wrap");
    }
    css += "}\n";
    return css;
}

namespace {

void write_css(const Node& n, const Node* parent, f32 pw, f32 ph, std::string& css) {
    css += node_css(n, parent, pw, ph);
    for (const Node& c : n.children) write_css(c, &n, n.w, n.h, css);
}

void write_elements(const Node& n, std::string& html, int depth) {
    html.append(static_cast<usize>(depth) * 2, ' ');
    html += "<div id=\"n" + std::to_string(n.id) + "\" class=\"" + node_type_name(n.type) + "\" title=\"" +
            escape_html(n.name) + "\">";
    if (n.type == NodeType::Text) {
        html += escape_html(n.text);
    } else if (!n.children.empty()) {
        html += "\n";
        for (const Node& c : n.children) write_elements(c, html, depth + 1);
        html.append(static_cast<usize>(depth) * 2, ' ');
    }
    html += "</div>\n";
}

} // namespace

std::string screen_html(const Screen& screen, const HtmlOptions& options) {
    std::string html = "<!DOCTYPE html>\n<html lang=\"ru\">\n<head>\n<meta charset=\"utf-8\">\n<title>" +
                       escape_html(screen.title) + "</title>\n";
    for (const std::string& sheet : options.stylesheets)
        html += "<link rel=\"stylesheet\" href=\"" + escape_html(sheet) + "\">\n";
    html += "<!-- Made by Forge «Интерфейс» from " + escape_html(screen.title) +
            ".json; changes here are overwritten. -->\n<style>\nhtml, body {\n  margin: 0;\n  width: 100%;\n  height: 100%;\n"
            "  overflow: hidden;\n}\n";
    // The screen's texts by default.
    html += "body {\n  font-family: " + css_string(screen.text.family) + ";\n  font-size: " + px(screen.text.size) +
            ";\n  color: " + css_color(screen.text.color) + ";\n}\n";
    write_css(screen.root, nullptr, screen.width, screen.height, html);
    html += "</style>\n</head>\n<body>\n";
    write_elements(screen.root, html, 0);
    html += "</body>\n</html>\n";
    return html;
}

// --- snapping ---

namespace {

struct Candidate {
    f32 delta;
    f32 distance;
};

void consider(std::optional<Candidate>& best, f32 target, f32 point, f32 threshold) {
    const f32 d = std::fabs(target - point);
    if (d > threshold) return;
    if (!best || d < best->distance) best = Candidate{target - point, d};
}

bool overlaps(f32 a0, f32 a1, f32 b0, f32 b1) { return a0 < b1 && b0 < a1; }

// Equal spacing along one axis (horizontal: x). Fills candidate positions of
// the box's start edge.
void gap_candidates(const Rect& box, const std::vector<Rect>& boxes, bool horizontal, std::vector<f32>& starts) {
    auto s0 = [&](const Rect& r) { return horizontal ? r.x : r.y; };
    auto s1 = [&](const Rect& r) { return horizontal ? r.right() : r.bottom(); };
    auto c0 = [&](const Rect& r) { return horizontal ? r.y : r.x; };
    auto c1 = [&](const Rect& r) { return horizontal ? r.bottom() : r.right(); };
    const f32 size = horizontal ? box.w : box.h;
    const f32 mid = s0(box) + size * 0.5f;
    std::vector<Rect> row;
    for (const Rect& r : boxes)
        if (overlaps(c0(r), c1(r), c0(box), c1(box))) row.push_back(r);
    const Rect* before = nullptr;
    const Rect* after = nullptr;
    for (const Rect& r : row) {
        if (s1(r) <= mid && (!before || s1(r) > s1(*before))) before = &r;
        if (s0(r) >= mid && (!after || s0(r) < s0(*after))) after = &r;
    }
    if (before && after) starts.push_back((s1(*before) + s0(*after) - size) * 0.5f);
    // Gaps that already exist between neighbours in the row.
    std::sort(row.begin(), row.end(), [&](const Rect& a, const Rect& b) { return s0(a) < s0(b); });
    for (usize i = 0; i + 1 < row.size(); ++i) {
        const f32 gap = s0(row[i + 1]) - s1(row[i]);
        if (gap <= 0) continue;
        if (before) starts.push_back(s1(*before) + gap);
        if (after) starts.push_back(s0(*after) - gap - size);
    }
}

void collect_lines(const Rect& box, const SnapTargets& t, SnapResult& r) {
    auto line = [&](bool vertical, f32 at, const Rect& other) {
        SnapLine l;
        l.vertical = vertical;
        l.at = at;
        l.from = vertical ? std::min(box.y, other.y) : std::min(box.x, other.x);
        l.to = vertical ? std::max(box.bottom(), other.bottom()) : std::max(box.right(), other.right());
        r.lines.push_back(l);
    };
    const f32 eps = 0.5f;
    std::vector<Rect> all = t.boxes;
    all.push_back(t.frame);
    for (const Rect& o : all) {
        if (r.snapped_x)
            for (f32 a : {box.x, box.cx(), box.right()})
                for (f32 b : {o.x, o.cx(), o.right()})
                    if (std::fabs(a - b) < eps) line(true, b, o);
        if (r.snapped_y)
            for (f32 a : {box.y, box.cy(), box.bottom()})
                for (f32 b : {o.y, o.cy(), o.bottom()})
                    if (std::fabs(a - b) < eps) line(false, b, o);
    }
    // Equal gaps: show the gaps on both sides of the box that match.
    auto gaps = [&](bool horizontal) {
        auto s0 = [&](const Rect& q) { return horizontal ? q.x : q.y; };
        auto s1 = [&](const Rect& q) { return horizontal ? q.right() : q.bottom(); };
        auto c0 = [&](const Rect& q) { return horizontal ? q.y : q.x; };
        auto c1 = [&](const Rect& q) { return horizontal ? q.bottom() : q.right(); };
        const Rect* before = nullptr;
        const Rect* after = nullptr;
        for (const Rect& q : t.boxes) {
            if (!overlaps(c0(q), c1(q), c0(box), c1(box))) continue;
            if (s1(q) <= s0(box) + eps && (!before || s1(q) > s1(*before))) before = &q;
            if (s0(q) >= s1(box) - eps && (!after || s0(q) < s0(*after))) after = &q;
        }
        if (!before || !after) return;
        const f32 g0 = s0(box) - s1(*before), g1 = s0(*after) - s1(box);
        if (g0 <= 0 || std::fabs(g0 - g1) > eps) return;
        const f32 across0 = (std::max(c0(*before), c0(box)) + std::min(c1(*before), c1(box))) * 0.5f;
        const f32 across1 = (std::max(c0(*after), c0(box)) + std::min(c1(*after), c1(box))) * 0.5f;
        r.gaps.push_back({horizontal, s1(*before), s0(box), across0});
        r.gaps.push_back({horizontal, s1(box), s0(*after), across1});
    };
    if (r.snapped_x) gaps(true);
    if (r.snapped_y) gaps(false);
}

} // namespace

SnapResult snap_box(const Rect& box, const SnapTargets& t, f32 threshold) {
    std::optional<Candidate> bx, by;
    auto targets_x = [&](const Rect& o) {
        for (f32 b : {o.x, o.cx(), o.right()})
            for (f32 a : {box.x, box.cx(), box.right()}) consider(bx, b, a, threshold);
    };
    auto targets_y = [&](const Rect& o) {
        for (f32 b : {o.y, o.cy(), o.bottom()})
            for (f32 a : {box.y, box.cy(), box.bottom()}) consider(by, b, a, threshold);
    };
    for (const Rect& o : t.boxes) {
        targets_x(o);
        targets_y(o);
    }
    if (t.frame.w > 0) {
        targets_x(t.frame);
        targets_y(t.frame);
    }
    for (f32 x : t.xs)
        for (f32 a : {box.x, box.cx(), box.right()}) consider(bx, x, a, threshold);
    for (f32 y : t.ys)
        for (f32 a : {box.y, box.cy(), box.bottom()}) consider(by, y, a, threshold);
    std::vector<f32> starts;
    gap_candidates(box, t.boxes, true, starts);
    for (f32 s : starts) consider(bx, s, box.x, threshold);
    starts.clear();
    gap_candidates(box, t.boxes, false, starts);
    for (f32 s : starts) consider(by, s, box.y, threshold);

    SnapResult r;
    if (bx) {
        r.dx = bx->delta;
        r.snapped_x = true;
    } else if (t.grid > 0) {
        r.dx = std::round(box.x / t.grid) * t.grid - box.x;
    }
    if (by) {
        r.dy = by->delta;
        r.snapped_y = true;
    } else if (t.grid > 0) {
        r.dy = std::round(box.y / t.grid) * t.grid - box.y;
    }
    Rect moved = box;
    moved.x += r.dx;
    moved.y += r.dy;
    collect_lines(moved, t, r);
    return r;
}

SnapResult snap_edges(const Rect& box, bool left, bool right, bool top, bool bottom, const SnapTargets& t,
                      f32 threshold) {
    std::optional<Candidate> bx, by;
    std::vector<Rect> all = t.boxes;
    if (t.frame.w > 0) all.push_back(t.frame);
    const f32 ex = left ? box.x : box.right();
    const f32 ey = top ? box.y : box.bottom();
    for (const Rect& o : all) {
        if (left || right)
            for (f32 b : {o.x, o.cx(), o.right()}) consider(bx, b, ex, threshold);
        if (top || bottom)
            for (f32 b : {o.y, o.cy(), o.bottom()}) consider(by, b, ey, threshold);
    }
    if (left || right)
        for (f32 x : t.xs) consider(bx, x, ex, threshold);
    if (top || bottom)
        for (f32 y : t.ys) consider(by, y, ey, threshold);
    SnapResult r;
    if (bx) {
        r.dx = bx->delta;
        r.snapped_x = true;
    } else if (t.grid > 0 && (left || right)) {
        r.dx = std::round(ex / t.grid) * t.grid - ex;
    }
    if (by) {
        r.dy = by->delta;
        r.snapped_y = true;
    } else if (t.grid > 0 && (top || bottom)) {
        r.dy = std::round(ey / t.grid) * t.grid - ey;
    }
    Rect moved = box;
    if (left) {
        moved.x += r.dx;
        moved.w -= r.dx;
    } else if (right) {
        moved.w += r.dx;
    }
    if (top) {
        moved.y += r.dy;
        moved.h -= r.dy;
    } else if (bottom) {
        moved.h += r.dy;
    }
    // Only lines through the edges that moved.
    SnapResult lines;
    lines.snapped_x = r.snapped_x;
    lines.snapped_y = r.snapped_y;
    collect_lines(moved, t, lines);
    const f32 mx = left ? moved.x : moved.right(), my = top ? moved.y : moved.bottom();
    for (const SnapLine& l : lines.lines)
        if (std::fabs(l.at - (l.vertical ? mx : my)) < 0.5f) r.lines.push_back(l);
    return r;
}

} // namespace forge::editor::design
