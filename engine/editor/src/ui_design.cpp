#include "forge/editor/ui_design.h"

#include "forge/core/path.h"

#include <yyjson.h>

#include <algorithm>
#include <cctype>
#include <charconv>
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
const char* const kBarFroms[] = {"left", "right", "bottom", "top"};
const char* const kActions[] = {"show", "hide", "toggle", "close", "message", "change", "talk", "pause", "resume",
                                "menu", "quit", "new", "continue", "load", "save", "settings"};
const char* const kScreenShows[] = {"playing", "command", "menu"};
const char* const kWindowOvers[] = {"any", "game", "menu"};
const char* const kWindowEndings[] = {"none", "win", "lose"};
const char* const kEasings[] = {"smooth", "linear", "in", "out", "back", "bounce", "elastic"};
const char* const kMotions[] = {"none", "pulse", "float", "swing", "spin", "shake", "blink", "custom"};
const char* const kAppears[] = {"none", "fade", "rise", "drop", "zoom", "left", "right"};
const char* const kLists[] = {"", "items", "quests"};
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
    if (!p.style.empty()) yyjson_mut_obj_add_strcpy(doc, o, "style", p.style.c_str());
    return o;
}

yyjson_mut_val* write_text_style(yyjson_mut_doc* doc, const TextStyle& t) {
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
    if (!t.style.empty()) yyjson_mut_obj_add_strcpy(doc, to, "style", t.style.c_str());
    return to;
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
        yyjson_mut_obj_add_val(doc, o, "style", write_text_style(doc, n.text_style));
    }
    if (!n.show_if.empty()) yyjson_mut_obj_add_strcpy(doc, o, "show_if", n.show_if.c_str());
    if (!n.picture_from.empty()) yyjson_mut_obj_add_strcpy(doc, o, "picture_from", n.picture_from.c_str());
    if (n.list != ListSource::None) {
        yyjson_mut_obj_add_str(doc, o, "list", word(n.list, kLists));
        put_num(doc, o, "list_gap", n.list_gap);
    }
    if (!n.bar.value.empty()) {
        yyjson_mut_val* bo = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, bo, "value", n.bar.value.c_str());
        yyjson_mut_obj_add_strcpy(doc, bo, "max", n.bar.max.c_str());
        yyjson_mut_obj_add_str(doc, bo, "from", word(n.bar.from, kBarFroms));
        yyjson_mut_obj_add_val(doc, o, "bar", bo);
    }
    if (!n.on_click.empty()) {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (const Action& act : n.on_click) {
            yyjson_mut_val* ao = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, ao, "do", word(act.kind, kActions));
            if (!act.target.empty()) yyjson_mut_obj_add_strcpy(doc, ao, "target", act.target.c_str());
            yyjson_mut_arr_append(a, ao);
        }
        yyjson_mut_obj_add_val(doc, o, "on_click", a);
    }
    if (!n.click_sound.empty()) yyjson_mut_obj_add_strcpy(doc, o, "click_sound", n.click_sound.c_str());
    if (n.motion.kind != MotionKind::None) {
        const Motion& m = n.motion;
        yyjson_mut_val* mo = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, mo, "kind", word(m.kind, kMotions));
        put_num(doc, mo, "duration", m.duration);
        if (m.delay > 0) put_num(doc, mo, "delay", m.delay);
        yyjson_mut_obj_add_str(doc, mo, "easing", word(m.easing, kEasings));
        if (!m.loop) yyjson_mut_obj_add_bool(doc, mo, "loop", false);
        if (m.back) yyjson_mut_obj_add_bool(doc, mo, "back", true);
        if (m.strength != 1) put_num(doc, mo, "strength", m.strength);
        if (!m.keys.empty()) {
            yyjson_mut_val* ka = yyjson_mut_arr(doc);
            for (const MotionKey& k : m.keys) {
                yyjson_mut_val* ko = yyjson_mut_obj(doc);
                put_num(doc, ko, "at", k.at);
                if (k.x != 0) put_num(doc, ko, "x", k.x);
                if (k.y != 0) put_num(doc, ko, "y", k.y);
                if (k.scale != 1) put_num(doc, ko, "scale", k.scale);
                if (k.rotation != 0) put_num(doc, ko, "rotation", k.rotation);
                if (k.opacity != 1) put_num(doc, ko, "opacity", k.opacity);
                if (k.tint) yyjson_mut_obj_add_val(doc, ko, "color", color_val(doc, k.color));
                if (k.radius >= 0) put_num(doc, ko, "radius", k.radius);
                if (k.blur > 0) put_num(doc, ko, "blur", k.blur);
                if (k.brightness != 1) put_num(doc, ko, "brightness", k.brightness);
                yyjson_mut_arr_append(ka, ko);
            }
            yyjson_mut_obj_add_val(doc, mo, "keys", ka);
        }
        yyjson_mut_obj_add_val(doc, o, "motion", mo);
    }
    if (n.smooth > 0) {
        put_num(doc, o, "smooth", n.smooth);
        if (n.smooth_easing != Easing::Smooth) yyjson_mut_obj_add_str(doc, o, "smooth_easing", word(n.smooth_easing, kEasings));
    }
    if (!n.component.empty()) yyjson_mut_obj_add_strcpy(doc, o, "component", n.component.c_str());
    if (!n.variant.empty()) {
        yyjson_mut_val* vo = yyjson_mut_obj(doc);
        for (const auto& [property, value] : n.variant) yyjson_mut_obj_add_strncpy(doc, vo, property.c_str(), value.c_str(), value.size());
        yyjson_mut_obj_add_val(doc, o, "variant", vo);
    }
    if (n.master) yyjson_mut_obj_add_uint(doc, o, "master", n.master);
    if (!n.overrides.empty()) {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (const std::string& field : n.overrides) yyjson_mut_arr_add_strcpy(doc, a, field.c_str());
        yyjson_mut_obj_add_val(doc, o, "overrides", a);
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
    p.style = str(o, "style");
    return p;
}

void read_text_style(yyjson_val* t, TextStyle& s) {
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
    s.style = str(t, "style");
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
        read_text_style(t, n.text_style);
    }
    n.show_if = str(o, "show_if");
    n.picture_from = str(o, "picture_from");
    n.list = enum_of(o, "list", kLists, ListSource::None);
    n.list_gap = std::max(num(o, "list_gap", 8), 0.0f);
    if (yyjson_val* b = yyjson_obj_get(o, "bar"); yyjson_is_obj(b)) {
        n.bar.value = str(b, "value");
        n.bar.max = str(b, "max", "100");
        n.bar.from = enum_of(b, "from", kBarFroms, BarFrom::Left);
    }
    if (yyjson_val* a = yyjson_obj_get(o, "on_click"); yyjson_is_arr(a)) {
        usize i, count;
        yyjson_val* v;
        yyjson_arr_foreach(a, i, count, v) {
            if (!yyjson_is_obj(v)) continue;
            const std::optional<ActionKind> kind = parse_action(str(v, "do"));
            if (kind) n.on_click.push_back({*kind, str(v, "target")});
        }
    }
    n.click_sound = str(o, "click_sound");
    if (yyjson_val* mo = yyjson_obj_get(o, "motion"); yyjson_is_obj(mo)) {
        Motion& m = n.motion;
        m.kind = enum_of(mo, "kind", kMotions, MotionKind::None);
        m.duration = std::clamp(num(mo, "duration", 1), 0.05f, 600.0f);
        m.delay = std::max(num(mo, "delay", 0), 0.0f);
        m.easing = enum_of(mo, "easing", kEasings, Easing::Smooth);
        m.loop = flag(mo, "loop", true);
        m.back = flag(mo, "back", false);
        m.strength = std::max(num(mo, "strength", 1), 0.0f);
        if (yyjson_val* ka = yyjson_obj_get(mo, "keys"); yyjson_is_arr(ka)) {
            usize ki, kn;
            yyjson_val* kv;
            yyjson_arr_foreach(ka, ki, kn, kv) {
                if (!yyjson_is_obj(kv)) continue;
                MotionKey k;
                k.at = std::clamp(num(kv, "at", 0), 0.0f, 1.0f);
                k.x = num(kv, "x", 0);
                k.y = num(kv, "y", 0);
                k.scale = num(kv, "scale", 1);
                k.rotation = num(kv, "rotation", 0);
                k.opacity = std::clamp(num(kv, "opacity", 1), 0.0f, 1.0f);
                if (yyjson_obj_get(kv, "color")) {
                    k.tint = true;
                    k.color = color_of(kv, "color", k.color);
                }
                k.radius = num(kv, "radius", -1);
                k.blur = std::max(num(kv, "blur", 0), 0.0f);
                k.brightness = std::max(num(kv, "brightness", 1), 0.0f);
                m.keys.push_back(k);
            }
        }
    }
    n.smooth = std::max(num(o, "smooth", 0), 0.0f);
    n.smooth_easing = enum_of(o, "smooth_easing", kEasings, Easing::Smooth);
    n.component = str(o, "component");
    if (yyjson_val* vo = yyjson_obj_get(o, "variant"); yyjson_is_obj(vo)) {
        yyjson_val *key, *value;
        yyjson_obj_iter it = yyjson_obj_iter_with(vo);
        while ((key = yyjson_obj_iter_next(&it))) {
            value = yyjson_obj_iter_get_val(key);
            if (yyjson_is_str(value)) n.variant.emplace_back(yyjson_get_str(key), yyjson_get_str(value));
        }
    }
    n.master = static_cast<u32>(num(o, "master", 0));
    if (yyjson_val* a = yyjson_obj_get(o, "overrides"); yyjson_is_arr(a)) {
        yyjson_arr_foreach(a, i, count, v) if (yyjson_is_str(v)) n.overrides.emplace_back(yyjson_get_str(v));
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
const char* action_word(ActionKind k) { return word(k, kActions); }

const char* list_word(ListSource s) { return word(s, kLists); }
const char* window_over_word(WindowOver o) { return word(o, kWindowOvers); }
const char* window_ending_word(WindowEnding e) { return word(e, kWindowEndings); }
const char* screen_fit_word(ScreenFit f) { return word(f, kScreenFits); }

std::vector<std::pair<std::string, std::string>> list_fields(ListSource s) {
    if (s == ListSource::Quests)
        return {{"item.title", "Название задания"}, {"item.text", "Что сейчас делать"}, {"item.done", "Выполнено (1 или 0)"},
                {"item.index", "Номер в списке"}};
    if (s == ListSource::Items)
        return {{"item.name", "Название предмета"}, {"item.count", "Сколько"}, {"item.icon", "Картинка предмета"},
                {"item.about", "Описание"}, {"item.id", "Имя в данных"}, {"item.index", "Номер в списке"}};
    return {};
}
std::optional<ActionKind> parse_action(std::string_view text) {
    for (usize i = 0; i < std::size(kActions); ++i)
        if (text == kActions[i]) return static_cast<ActionKind>(i);
    return std::nullopt;
}

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
    yyjson_mut_obj_add_uint(doc, root, "next_id", screen.next_id);
    if (screen.library) yyjson_mut_obj_add_uint(doc, root, "next_style_id", screen.next_style_id);
    if (screen.library) yyjson_mut_obj_add_bool(doc, root, "library", true);
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
        if (!screen.library) {
            yyjson_mut_obj_add_str(doc, so, "show", word(screen.show, kScreenShows));
            if (screen.pauses) yyjson_mut_obj_add_bool(doc, so, "pauses", true);
            if (!screen.esc_closes) yyjson_mut_obj_add_bool(doc, so, "esc_closes", false);
            if (screen.dim) yyjson_mut_obj_add_bool(doc, so, "dim", true);
            if (screen.over != WindowOver::Any) yyjson_mut_obj_add_str(doc, so, "over", word(screen.over, kWindowOvers));
            if (screen.ending != WindowEnding::None) yyjson_mut_obj_add_str(doc, so, "ending", word(screen.ending, kWindowEndings));
            if (screen.appear != Appear::None) {
                yyjson_mut_obj_add_str(doc, so, "appear", word(screen.appear, kAppears));
                put_num(doc, so, "appear_time", screen.appear_time);
            }
            if (!screen.music.empty()) yyjson_mut_obj_add_strcpy(doc, so, "music", screen.music.c_str());
            if (!screen.button_sound.empty()) yyjson_mut_obj_add_strcpy(doc, so, "button_sound", screen.button_sound.c_str());
        }
        yyjson_mut_obj_add_val(doc, root, "settings", so);
    }
    if (!screen.colors.empty()) {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (const NamedColor& c : screen.colors) {
            yyjson_mut_val* co = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, co, "key", c.key.c_str());
            yyjson_mut_obj_add_strcpy(doc, co, "name", c.name.c_str());
            yyjson_mut_obj_add_val(doc, co, "color", color_val(doc, c.color));
            yyjson_mut_arr_append(a, co);
        }
        yyjson_mut_obj_add_val(doc, root, "colors", a);
    }
    if (!screen.text_styles.empty()) {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        for (const NamedTextStyle& t : screen.text_styles) {
            yyjson_mut_val* to = write_text_style(doc, t.style);
            yyjson_mut_obj_add_strcpy(doc, to, "key", t.key.c_str());
            yyjson_mut_obj_add_strcpy(doc, to, "name", t.name.c_str());
            yyjson_mut_arr_append(a, to);
        }
        yyjson_mut_obj_add_val(doc, root, "text_styles", a);
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
    s.next_id = static_cast<u32>(yyjson_get_uint(yyjson_obj_get(root, "next_id")));
    s.next_style_id = std::max(1u, static_cast<u32>(yyjson_get_uint(yyjson_obj_get(root, "next_style_id"))));
    s.library = flag(root, "library", false);
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
        // Screens made before the game showed them: only on command, so
        // none appears over the game by surprise.
        s.show = enum_of(so, "show", kScreenShows, ScreenShow::Command);
        s.pauses = flag(so, "pauses", false);
        s.esc_closes = flag(so, "esc_closes", true);
        s.dim = flag(so, "dim", false);
        s.over = enum_of(so, "over", kWindowOvers, WindowOver::Any);
        s.ending = enum_of(so, "ending", kWindowEndings, WindowEnding::None);
        s.appear = enum_of(so, "appear", kAppears, Appear::None);
        s.appear_time = std::clamp(num(so, "appear_time", 0.25f), 0.05f, 10.0f);
        s.music = str(so, "music");
        s.button_sound = str(so, "button_sound");
    }
    if (yyjson_val* a = yyjson_obj_get(root, "colors"); yyjson_is_arr(a)) {
        usize i, n;
        yyjson_val* c;
        yyjson_arr_foreach(a, i, n, c) s.colors.push_back({str(c, "key"), str(c, "name"), color_of(c, "color", Color{})});
    }
    if (yyjson_val* a = yyjson_obj_get(root, "text_styles"); yyjson_is_arr(a)) {
        usize i, n;
        yyjson_val* t;
        yyjson_arr_foreach(a, i, n, t) {
            NamedTextStyle named;
            named.key = str(t, "key");
            named.name = str(t, "name");
            read_text_style(t, named.style);
            named.style.style.clear();
            s.text_styles.push_back(std::move(named));
        }
    }
    const bool ok = read_node(yyjson_obj_get(root, "root"), s.root, 0);
    yyjson_doc_free(doc);
    if (!ok) {
        if (error) *error = "no root frame";
        return false;
    }
    s.root.w = s.width;
    s.root.h = s.height;
    s.next_id = std::max(s.next_id, max_id(s.root) + 1);
    // Older libraries had no counter. Reserve all of their current style keys.
    auto reserve_style = [&](const std::string& key) {
        if (key.size() < 2 || (key[0] != 'c' && key[0] != 't')) return;
        u32 id = 0;
        const auto result = std::from_chars(key.data() + 1, key.data() + key.size(), id);
        if (result.ec == std::errc{} && result.ptr == key.data() + key.size() && id < UINT32_MAX)
            s.next_style_id = std::max(s.next_style_id, id + 1);
    };
    for (const NamedColor& color : s.colors) reserve_style(color.key);
    for (const NamedTextStyle& style : s.text_styles) reserve_style(style.key);
    out = std::move(s);
    return true;
}

std::string fresh_style_key(Screen& library, bool color) {
    for (;;) {
        const std::string key = std::string(color ? "c" : "t") + std::to_string(library.next_style_id++);
        const bool used = std::any_of(library.colors.begin(), library.colors.end(), [&](const auto& c) { return c.key == key; }) ||
                          std::any_of(library.text_styles.begin(), library.text_styles.end(), [&](const auto& t) { return t.key == key; });
        if (!used) return key;
    }
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

std::filesystem::path screen_file(const std::filesystem::path& ui_dir, std::string_view name, std::string_view ext) {
    std::string file(name);
    file += ext;
    return ui_dir / utf8_path(file);
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

// --- components ---

namespace {

std::string node_text(const Node& n) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_doc_set_root(doc, write_node(doc, n));
    usize len = 0;
    char* text = yyjson_mut_write(doc, 0, &len);
    std::string out = text ? std::string(text, len) : std::string();
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

void set_masters(Node& n) {
    n.master = n.id;
    for (Node& c : n.children) set_masters(c);
}

// The layers of an instance with their masters and their names' path from
// its top ("Надпись", "Иконка/Тень"): another variant's layers are found by
// that path.
struct Mine {
    u32 master;
    std::string path;
    const Node* node;
};
void index_layers(const Node& n, const std::string& path, std::vector<Mine>& out) {
    out.push_back({n.master, path, &n});
    for (const Node& c : n.children) index_layers(c, path.empty() ? c.name : path + "/" + c.name, out);
}

// Copies one override from the author's layer onto the component's.
void keep_override(Node& to, const Node& from, std::string_view field) {
    if (field == "text") to.text = from.text;
    else if (field == "text_style") to.text_style = from.text_style;
    else if (field == "fills") to.fills = from.fills;
    else if (field == "strokes") to.strokes = from.strokes;
    else if (field == "effects") to.effects = from.effects;
    else if (field == "radius") to.radius = from.radius;
    else if (field == "visible") to.visible = from.visible;
    else if (field == "opacity") to.opacity = from.opacity;
    else if (field == "blend") to.blend = from.blend;
    else if (field == "frame") to.frame = from.frame;
    else if (field == "mask") to.mask = from.mask;
    else if (field == "motion") {
        to.motion = from.motion;
        to.smooth = from.smooth;
        to.smooth_easing = from.smooth_easing;
    }
    else if (field == "game") {
        to.show_if = from.show_if;
        to.bar = from.bar;
        to.on_click = from.on_click;
        to.click_sound = from.click_sound;
        to.picture_from = from.picture_from;
        to.list = from.list;
        to.list_gap = from.list_gap;
    }
    else if (field == "layout") {
        to.layout = from.layout;
        to.clip = from.clip;
    } else if (field == "size") {
        to.w = from.w;
        to.h = from.h;
        to.width_sizing = from.width_sizing;
        to.height_sizing = from.height_sizing;
    } else if (field == "place") {
        to.x = from.x;
        to.y = from.y;
        to.rotation = from.rotation;
        to.horizontal = from.horizontal;
        to.vertical = from.vertical;
        to.absolute = from.absolute;
    } else if (field == "name") to.name = from.name;
}

// Lower case for the Russian and Latin letters of state names.
std::string lower(std::string_view text) {
    std::string out;
    for (usize i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out += static_cast<char>(std::tolower(c));
        } else if (c == 0xD0 && i + 1 < text.size()) {
            const unsigned char d = static_cast<unsigned char>(text[i + 1]);
            if (d >= 0x90 && d <= 0x9F) out += {static_cast<char>(0xD0), static_cast<char>(d + 0x20)}; // А..П
            else if (d >= 0xA0 && d <= 0xAF) out += {static_cast<char>(0xD1), static_cast<char>(d - 0x20)}; // Р..Я
            else if (d == 0x81) out += {static_cast<char>(0xD1), static_cast<char>(0x91)}; // Ё
            else out += {static_cast<char>(c), static_cast<char>(d)};
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

} // namespace

std::vector<Component> components(const Screen& library) {
    std::vector<Component> out;
    for (const Node& n : library.root.children) {
        if (n.component.empty()) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const Component& c) { return c.name == n.component; });
        if (it == out.end()) {
            out.push_back({n.component, {}, {}});
            it = out.end() - 1;
        }
        it->variants.push_back(n.id);
        for (const auto& [property, value] : n.variant) {
            auto p = std::find_if(it->properties.begin(), it->properties.end(),
                                  [&](const ComponentProperty& cp) { return cp.name == property; });
            if (p == it->properties.end()) {
                it->properties.push_back({property, {}});
                p = it->properties.end() - 1;
            }
            if (std::find(p->values.begin(), p->values.end(), value) == p->values.end()) p->values.push_back(value);
        }
    }
    return out;
}

const Component* find_component(const std::vector<Component>& list, std::string_view name) {
    for (const Component& c : list)
        if (c.name == name) return &c;
    return nullptr;
}

std::string variant_value(const std::vector<std::pair<std::string, std::string>>& values, std::string_view property) {
    for (const auto& [p, v] : values)
        if (p == property) return v;
    return {};
}

void set_variant_value(std::vector<std::pair<std::string, std::string>>& values, const std::string& property,
                       const std::string& value) {
    for (auto& [p, v] : values)
        if (p == property) {
            v = value;
            return;
        }
    values.emplace_back(property, value);
}

const Node* find_variant(const Screen& library, std::string_view component,
                         const std::vector<std::pair<std::string, std::string>>& values) {
    const Node* best = nullptr;
    int best_score = -1;
    for (const Node& n : library.root.children) {
        if (n.component != component) continue;
        int score = 0;
        for (const auto& [property, value] : values)
            if (variant_value(n.variant, property) == value) ++score;
        if (score > best_score) {
            best = &n;
            best_score = score;
        }
    }
    return best;
}

std::optional<Node> make_instance(Screen& screen, const Screen& library, std::string_view component,
                                  const std::vector<std::pair<std::string, std::string>>& values) {
    const Node* v = find_variant(library, component, values);
    if (!v) return std::nullopt;
    Node n = *v;
    set_masters(n);
    renumber(screen, n);
    n.x = n.y = 0;
    n.horizontal = n.vertical = Constraint::Start;
    n.absolute = false;
    n.locked = false;
    n.visible = true;
    n.overrides.clear();
    return n;
}

bool sync_instance(Screen& screen, Node& instance, const Screen& library) {
    // The component by its variant first: a renamed component keeps its copies.
    std::string name = instance.component;
    for (const Node& variant : library.root.children)
        if (variant.id == instance.master && !variant.component.empty()) name = variant.component;
    const Node* v = find_variant(library, name, instance.variant);
    if (!v) return false;
    const Node old = instance;
    std::vector<Mine> mine;
    index_layers(old, {}, mine);
    Node fresh = *v;
    set_masters(fresh);
    std::vector<u32> used;
    auto unused = [&](const Node* node) { return node != &old && std::find(used.begin(), used.end(), node->id) == used.end(); };
    auto visit = [&](auto&& self, Node& n, const std::string& path, bool top) -> void {
        const Node* was = nullptr;
        if (top) was = &old;
        for (const Mine& m : mine)
            if (!was && m.master == n.master && unused(m.node)) was = m.node;
        for (const Mine& m : mine)
            if (!was && m.path == path && unused(m.node)) was = m.node;
        if (was) {
            n.id = was->id;
            used.push_back(was->id);
            for (const std::string& field : was->overrides) keep_override(n, *was, field);
            n.overrides = was->overrides;
        } else {
            n.id = screen.next_id++;
            n.overrides.clear();
        }
        for (Node& c : n.children) self(self, c, path.empty() ? c.name : path + "/" + c.name, false);
    };
    visit(visit, fresh, {}, true);
    // The instance's own place on the screen stays.
    fresh.name = old.name;
    fresh.x = old.x;
    fresh.y = old.y;
    fresh.rotation = old.rotation;
    fresh.horizontal = old.horizontal;
    fresh.vertical = old.vertical;
    fresh.absolute = old.absolute;
    fresh.width_sizing = old.width_sizing; // how it sits in its parent's layout
    fresh.height_sizing = old.height_sizing;
    fresh.visible = old.visible;
    fresh.locked = old.locked;
    fresh.component = name;
    std::vector<std::pair<std::string, std::string>> values = v->variant;
    fresh.variant = values;
    if (node_text(fresh) == node_text(old)) return false;
    instance = std::move(fresh);
    return true;
}

bool sync_instances(Screen& screen, const Screen& library) {
    bool changed = false;
    auto visit = [&](auto&& self, Node& n) -> void {
        if (!n.component.empty() && n.master) {
            changed |= sync_instance(screen, n, library);
            return; // what is inside came from the library
        }
        for (Node& c : n.children) self(self, c);
    };
    if (!screen.library) visit(visit, screen.root);
    else
        for (Node& variant : screen.root.children)
            for (Node& c : variant.children) visit(visit, c); // components made of components
    return changed;
}

bool apply_styles(Screen& screen, const Screen& library) {
    bool changed = false;
    auto visit = [&](auto&& self, Node& n) -> void {
        for (Paint& p : n.fills) {
            if (p.style.empty() || p.kind != PaintKind::Solid) continue;
            for (const NamedColor& c : library.colors)
                if (c.key == p.style && p.color != c.color) {
                    p.color = c.color;
                    changed = true;
                }
        }
        if (!n.text_style.style.empty())
            for (const NamedTextStyle& t : library.text_styles) {
                if (t.key != n.text_style.style) continue;
                TextStyle s = t.style;
                s.style = t.key;
                s.align = n.text_style.align; // where the text sits is the layer's own
                if (s != n.text_style) {
                    n.text_style = s;
                    changed = true;
                }
            }
        for (Node& c : n.children) self(self, c);
    };
    visit(visit, screen.root);
    return changed;
}

std::string override_of(std::string_view field) {
    auto starts = [&](std::string_view prefix) { return field.substr(0, prefix.size()) == prefix; };
    if (field == "x" || field == "y" || field == "rotation" || field == "horizontal" || field == "vertical" ||
        field == "absolute")
        return "place";
    if (field == "w" || field == "h" || field == "width_sizing" || field == "height_sizing") return "size";
    if (starts("layout") || field == "clip") return "layout";
    if (starts("fill")) return "fills";
    if (starts("stroke")) return "strokes";
    if (starts("effect")) return "effects";
    if (starts("radius")) return "radius";
    if (starts("frame.")) return "frame";
    if (starts("mask.")) return "mask";
    if (field == "show_if" || starts("bar.") || starts("click") || field == "picture_from" || starts("list"))
        return "game";
    if (starts("motion") || starts("smooth")) return "motion";
    if (field == "opacity" || field == "blend" || field == "visible" || field == "text" || field == "name")
        return std::string(field);
    if (field == "family" || field == "size" || field == "weight" || field == "italic" || field == "line_height" ||
        field == "letter_spacing" || field == "text_align" || field == "text_case" || field == "decoration" ||
        field == "text_color" || field == "text_style")
        return "text_style";
    return {};
}

void detach(Node& instance) {
    auto visit = [&](auto&& self, Node& n, bool top) -> void {
        if (!top && !n.component.empty() && n.master) return; // an instance inside stays one
        n.master = 0;
        n.overrides.clear();
        if (top) {
            n.component.clear();
            n.variant.clear();
        }
        for (Node& c : n.children) self(self, c, false);
    };
    visit(visit, instance, true);
}

const Node* instance_of(const Node& root, u32 id) {
    const std::vector<u32> path = path_to(root, id);
    const Node* n = &root;
    for (usize i = 0; i < path.size(); ++i) {
        if (i > 0) {
            const Node* next = nullptr;
            for (const Node& c : n->children)
                if (c.id == path[i]) next = &c;
            if (!next) return nullptr;
            n = next;
        }
        if (!n->component.empty() && n->master) return n;
    }
    return nullptr;
}

std::string state_selector(std::string_view value) {
    const std::string v = lower(value);
    auto starts = [&](std::string_view prefix) { return v.rfind(prefix, 0) == 0; };
    if (starts("навед") || starts("hover")) return ":hover";
    if (starts("нажат") || starts("press") || starts("active")) return ":active";
    if (starts("выключ") || starts("недоступ") || starts("disabled")) return ".disabled";
    if (starts("выбран") || starts("selected")) return ".selected";
    if (starts("фокус") || starts("focus")) return ":focus";
    return {};
}

std::string state_property(const Component& component, const Screen&) {
    for (const ComponentProperty& p : component.properties)
        for (const std::string& value : p.values)
            if (!state_selector(value).empty()) return p.name;
    return {};
}

namespace {

// --- movement ---

const char* easing_css(Easing e) {
    switch (e) {
    case Easing::Smooth: return "ease-in-out";
    case Easing::Linear: return "linear";
    case Easing::EaseIn: return "ease-in";
    case Easing::EaseOut: return "ease-out";
    case Easing::Back: return "back-out";
    case Easing::Bounce: return "bounce-out";
    case Easing::Elastic: return "elastic-out";
    }
    return "ease-in-out";
}

} // namespace

std::vector<MotionKey> motion_keys(const Motion& m) {
    const f32 k = m.strength;
    auto key = [](f32 at) {
        MotionKey mk;
        mk.at = at;
        return mk;
    };
    std::vector<MotionKey> out;
    switch (m.kind) {
    case MotionKind::None: break;
    case MotionKind::Pulse: {
        out = {key(0), key(0.5f), key(1)};
        out[1].scale = 1 + 0.08f * k;
        break;
    }
    case MotionKind::Float: {
        out = {key(0), key(0.5f), key(1)};
        out[1].y = -8 * k;
        break;
    }
    case MotionKind::Swing: {
        out = {key(0), key(0.25f), key(0.75f), key(1)};
        out[1].rotation = 8 * k;
        out[2].rotation = -8 * k;
        break;
    }
    case MotionKind::Spin: {
        out = {key(0), key(1)};
        out[1].rotation = 360;
        break;
    }
    case MotionKind::Shake: {
        const f32 xs[] = {0, -6, 6, -6, 6, -3, 0};
        const f32 ats[] = {0, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1};
        for (int i = 0; i < 7; ++i) {
            out.push_back(key(ats[i]));
            out.back().x = xs[i] * k;
        }
        break;
    }
    case MotionKind::Blink: {
        out = {key(0), key(0.5f), key(1)};
        out[1].opacity = std::clamp(1 - 0.8f * k, 0.0f, 1.0f);
        break;
    }
    case MotionKind::Custom: {
        out = m.keys;
        std::stable_sort(out.begin(), out.end(), [](const MotionKey& a, const MotionKey& b) { return a.at < b.at; });
        // From its place and back to it, unless the author says otherwise.
        if (out.empty() || out.front().at > 0) out.insert(out.begin(), key(0));
        if (out.back().at < 1) out.push_back(key(1));
        break;
    }
    }
    return out;
}

std::vector<u32> movable_order(const Screen& screen, const std::vector<u32>& ids) {
    std::vector<u32> out;
    auto visit = [&](auto&& self, const Node& n, bool inside) -> void {
        for (const Node& c : n.children) {
            const bool picked = std::find(ids.begin(), ids.end(), c.id) != ids.end();
            if (picked && !inside) out.push_back(c.id);
            self(self, c, inside || picked);
        }
    };
    visit(visit, screen.root, false);
    return out;
}

std::string move_refusal(const Screen& screen, const std::vector<u32>& ids, u32 parent) {
    const std::vector<u32> moving = movable_order(screen, ids);
    if (moving.empty()) return "Экран целиком не переносится: выберите слой.";
    const Node* target = find(screen.root, parent);
    if (!target || !target->is_container()) return "Положить можно только в рамку.";
    const std::vector<u32> target_path = path_to(screen.root, parent);
    for (u32 id : moving)
        if (std::find(target_path.begin(), target_path.end(), id) != target_path.end())
            return "Рамку нельзя положить в саму себя или в то, что внутри неё.";
    if (const Node* instance = instance_of(screen.root, parent))
        return "Внутрь копии компонента «" + instance->component + "» класть нельзя: её слои задаёт компонент.";
    for (u32 id : target_path)
        if (const Node* a = find(screen.root, id); a && a->list != ListSource::None)
            return "В список класть нельзя: он повторяет свою первую ячейку.";
    if (screen.library && parent == screen.root.id) return "Наверху библиотеки только варианты компонентов.";
    for (u32 id : moving) {
        if (const Node* instance = instance_of(screen.root, id); instance && instance->id != id)
            return "Слой копии компонента «" + instance->component + "» не выносится из неё: её слои задаёт компонент.";
        const std::vector<u32> path = path_to(screen.root, id);
        for (usize i = 0; i + 1 < path.size(); ++i)
            if (const Node* a = find(screen.root, path[i]); a && a->list != ListSource::None)
                return "Слой списка не выносится: список повторяет свою первую ячейку.";
        if (screen.library && path.size() == 2) return "Вариант компонента остаётся наверху библиотеки.";
    }
    return {};
}

bool place_at(Node& n, const Rect& box, f32 pw, f32 ph, const Rect& parent_box) {
    bool fits = true;
    // The inverse of node_css's anchoring for one axis: rel and shown are the wanted start and length in the
    // frame's shown box (shown_parent long); own_parent the frame's own length.
    auto axis = [&fits](Constraint c, bool hug, f32 rel, f32 shown, f32 own_parent, f32 shown_parent, f32& pos, f32& size) {
        const f32 grown = shown_parent - own_parent; // what a row or column added to the frame
        auto whole = [](f32 v) { return std::round(v); };
        switch (c) {
        case Constraint::Start:
            pos = whole(rel);
            if (!hug) size = whole(shown);
            break;
        case Constraint::End:
            pos = whole(rel - grown);
            if (!hug) size = whole(shown);
            break;
        case Constraint::Both:
            pos = whole(rel);
            size = whole(shown - grown);
            if (size < 1) {
                size = 1;
                fits = false;
            }
            break;
        case Constraint::Center:
            if (hug) pos = whole(rel + shown * 0.5f - grown * 0.5f - size * 0.5f);
            else {
                pos = whole(rel - grown * 0.5f);
                size = whole(shown);
            }
            break;
        case Constraint::Scale: {
            // Shares of the frame's shown size: kept to a hundredth of a pixel of the frame's own size.
            const f32 k = shown_parent > 0 && own_parent > 0 ? own_parent / shown_parent : 1.0f;
            pos = std::round(rel * k * 100.0f) / 100.0f;
            if (!hug) size = std::round(shown * k * 100.0f) / 100.0f;
            break;
        }
        }
    };
    axis(n.horizontal, n.width_sizing == Sizing::Hug, box.x - parent_box.x, box.w, pw, parent_box.w, n.x, n.w);
    axis(n.vertical, n.height_sizing == Sizing::Hug, box.y - parent_box.y, box.h, ph, parent_box.h, n.y, n.h);
    return fits;
}

bool move_stays(const Screen& screen, const std::vector<u32>& ids, u32 parent, usize index) {
    const std::vector<u32> moving = movable_order(screen, ids);
    const Node* target = find(screen.root, parent);
    if (moving.empty() || !target) return false;
    for (u32 id : moving) {
        const std::vector<u32> path = path_to(screen.root, id);
        if (path.size() < 2 || path[path.size() - 2] != parent) return false;
    }
    // The children's order now, and as the move would make it.
    std::vector<u32> now, after;
    usize kept = 0;
    bool placed = false;
    for (const Node& c : target->children) {
        now.push_back(c.id);
        if (std::find(moving.begin(), moving.end(), c.id) != moving.end()) continue;
        if (kept++ == index) {
            after.insert(after.end(), moving.begin(), moving.end());
            placed = true;
        }
        after.push_back(c.id);
    }
    if (!placed) after.insert(after.end(), moving.begin(), moving.end());
    return now == after;
}

bool move_layers(Screen& screen, const std::vector<u32>& ids, u32 parent, usize index,
                 const std::vector<std::pair<f32, f32>>& places) {
    if (!move_refusal(screen, ids, parent).empty()) return false;
    const std::vector<u32> moving = movable_order(screen, ids);
    if (!places.empty() && places.size() != moving.size()) return false;
    if (move_stays(screen, ids, parent, index)) return true; // where they are: nothing changes
    // Where they go: before the index-th of the parent's children that stay.
    const Node* target = find(screen.root, parent);
    u32 before = 0;
    usize kept = 0;
    for (const Node& c : target->children) {
        if (std::find(moving.begin(), moving.end(), c.id) != moving.end()) continue;
        if (kept++ == index) {
            before = c.id;
            break;
        }
    }
    std::vector<Node> taken;
    for (u32 id : moving)
        if (std::optional<Node> n = remove(screen.root, id)) taken.push_back(std::move(*n));
    Node* into = find(screen.root, parent);
    if (!into || taken.size() != moving.size()) return false;
    for (usize i = 0; i < taken.size(); ++i) {
        if (!places.empty()) {
            taken[i].x = places[i].first;
            taken[i].y = places[i].second;
        }
        taken[i].absolute = false; // in a frame with auto layout it joins the flow; elsewhere x and y place it
    }
    auto at = std::find_if(into->children.begin(), into->children.end(), [&](const Node& c) { return before && c.id == before; });
    into->children.insert(at, std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end()));
    return true;
}

usize move_motion_key(Motion& m, usize index, f32 at) {
    if (index >= m.keys.size()) return index;
    at = std::clamp(at, 0.0f, 1.0f);
    // Keys written out of order (by hand, an old file) are put in order first, as motion_keys plays them.
    if (!std::is_sorted(m.keys.begin(), m.keys.end(), [](const MotionKey& a, const MotionKey& b) { return a.at < b.at; })) {
        std::vector<usize> order(m.keys.size());
        for (usize i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return m.keys[a].at < m.keys[b].at; });
        std::vector<MotionKey> sorted;
        for (usize i : order) sorted.push_back(m.keys[i]);
        index = static_cast<usize>(std::find(order.begin(), order.end(), index) - order.begin());
        m.keys = std::move(sorted);
    }
    MotionKey k = m.keys[index];
    const f32 was = k.at;
    if (at == was) return index;
    k.at = at;
    m.keys.erase(m.keys.begin() + static_cast<std::ptrdiff_t>(index));
    // Later: before the keys at its new time; earlier: after them.
    auto it = at > was ? std::lower_bound(m.keys.begin(), m.keys.end(), at, [](const MotionKey& a, f32 t) { return a.at < t; })
                       : std::upper_bound(m.keys.begin(), m.keys.end(), at, [](f32 t, const MotionKey& a) { return t < a.at; });
    const usize to = static_cast<usize>(it - m.keys.begin());
    m.keys.insert(it, k);
    return to;
}

namespace {

// @keyframes for a layer's motion: every key states every property that
// moves, on top of the layer's own transform, filter, opacity and colour.
void write_keyframes(const Node& n, const std::string& name, const std::string& transform, const std::string& filter,
                     std::string& out) {
    const std::vector<MotionKey> keys = motion_keys(n.motion);
    bool place = false, fade = false, tint = false, corners = false, look = false;
    for (const MotionKey& k : keys) {
        place |= k.x != 0 || k.y != 0 || k.scale != 1 || k.rotation != 0;
        fade |= k.opacity != 1;
        tint |= k.tint;
        corners |= k.radius >= 0;
        look |= k.blur > 0 || k.brightness != 1;
    }
    const bool text = n.type == NodeType::Text;
    Color base_color = text ? n.text_style.color : Color{0, 0, 0, 0};
    if (!text)
        for (const Paint& p : n.fills)
            if (p.visible && p.kind == PaintKind::Solid) base_color = p.color;
    const f32 base_radius = n.radius[0];
    out += "@keyframes " + name + " {\n";
    for (const MotionKey& k : keys) {
        out += "  " + fmt(k.at * 100) + "% {\n";
        auto line = [&](const char* property, const std::string& value) { out += "    " + std::string(property) + ": " + value + ";\n"; };
        if (place)
            line("transform", (transform.empty() ? "" : transform + " ") + "translate(" + px(k.x) + ", " + px(k.y) + ") scale(" +
                                  fmt(k.scale) + ") rotate(" + fmt(k.rotation) + "deg)");
        if (fade) line("opacity", fmt(std::clamp(k.opacity * n.opacity, 0.0f, 1.0f)));
        if (tint) line(text ? "color" : "background-color", css_color(k.tint ? k.color : base_color));
        if (corners && n.type != NodeType::Ellipse) line("border-radius", px(k.radius >= 0 ? k.radius : base_radius));
        if (look)
            line("filter", (filter.empty() ? "" : filter + " ") + "blur(" + px(k.blur) + ") brightness(" + fmt(k.brightness) + ")");
        out += "  }\n";
    }
    out += "}\n";
}

} // namespace

std::string node_css(const Node& n, const Node* parent, f32 pw, f32 ph) { return node_css(n, parent, pw, ph, nullptr, 0); }

std::string node_css(const Node& n, const Node* parent, f32 pw, f32 ph, std::string* keyframes, f32 smooth_from_parent) {
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
                    if (p.image.empty()) continue; // no file picked yet: nothing drawn (a url of "../" names the game's folder)
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
            if (!images.empty()) {
                add(css, "background-image", images);
                add(css, "background-size", sizes);
                add(css, "background-repeat", repeats);
                add(css, "background-position", positions);
            }
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
    if (n.list != ListSource::None) {
        add(css, "overflow-x", "hidden");
        add(css, "overflow-y", "auto");
    } else if (n.clip) {
        add(css, "overflow", "hidden");
        // RmlUi clips only content that overflows the layout, and freely placed layers do not: «always» clips
        // them as the canvas shows, for the eye and for the pointer alike (a rounded frame hid them from the
        // eye only, and its hidden part still took clicks).
        if (parent) add(css, "clip", "always");
    }
    if (!n.picture_from.empty()) {
        add(css, "background-size", "contain");
        add(css, "background-repeat", "no-repeat");
        add(css, "background-position", "center");
    }
    if (n.rotation != 0) transforms.push_back("rotate(" + fmt(n.rotation) + "deg)");
    std::string transform;
    for (const std::string& part : transforms) transform += (transform.empty() ? "" : " ") + part;
    if (!transform.empty()) add(css, "transform", transform);

    // Movement: changes of the look ease in (states), and the layer's own motion.
    const f32 smooth = n.smooth > 0 ? n.smooth : smooth_from_parent;
    if (smooth > 0) add(css, "transition", "all " + fmt(smooth) + "s " + easing_css(n.smooth > 0 ? n.smooth_easing : Easing::Smooth));
    if (keyframes && n.motion.kind != MotionKind::None) {
        const std::string name = "m" + std::to_string(n.id);
        write_keyframes(n, name, transform, filter, *keyframes);
        const Motion& m = n.motion;
        std::string a = fmt(m.duration) + "s " + easing_css(m.easing);
        if (m.delay > 0) a += " " + fmt(m.delay) + "s";
        if (m.loop) a += " infinite";
        if (m.back) a += " alternate";
        add(css, "animation", a + " " + name);
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

void write_css(const Node& n, const Node* parent, f32 pw, f32 ph, std::string& css, std::string* keyframes, f32 smooth) {
    css += node_css(n, parent, pw, ph, keyframes, smooth);
    const f32 inner = n.smooth > 0 ? n.smooth : smooth;
    for (const Node& c : n.children) write_css(c, &n, n.w, n.h, css, keyframes, inner);
}

// The CSS of every layer of a subtree by id (parent: the subtree's parent, with its size).
void css_by_id(const Node& n, const Node* parent, f32 pw, f32 ph, std::vector<std::pair<u32, std::string>>& out) {
    out.emplace_back(n.id, node_css(n, parent, pw, ph));
    for (const Node& c : n.children) css_by_id(c, &n, n.w, n.h, out);
}

// Instances of components with states: the other states' looks, as rules
// that apply in that state ("#n5:hover #n7 {...}"), only where they differ.
void write_states(const Screen& screen, const Screen& library, std::string& css) {
    const std::vector<Component> list = components(library);
    auto visit = [&](auto&& self, const Node& n, const Node* parent, f32 pw, f32 ph) -> void {
        if (n.component.empty() || !n.master) {
            for (const Node& c : n.children) self(self, c, &n, n.w, n.h);
            return;
        }
        const Component* component = find_component(list, n.component);
        const std::string property = component ? state_property(*component, library) : std::string();
        if (property.empty()) return;
        std::vector<std::pair<u32, std::string>> base;
        css_by_id(n, parent, pw, ph, base);
        const std::string now = variant_value(n.variant, property);
        for (const ComponentProperty& p : component->properties) {
            if (p.name != property) continue;
            for (const std::string& value : p.values) {
                const std::string selector = state_selector(value);
                if (selector.empty() || value == now) continue;
                std::vector<std::pair<std::string, std::string>> values = n.variant;
                set_variant_value(values, property, value);
                const Node* v = find_variant(library, n.component, values);
                if (!v || variant_value(v->variant, property) != value) continue;
                Screen scratch;
                scratch.next_id = 0x40000000u; // layers the usual look lacks: not on the page
                Node look = n;
                look.variant = values;
                sync_instance(scratch, look, library);
                std::vector<std::pair<u32, std::string>> other;
                css_by_id(look, parent, pw, ph, other);
                const std::string top = "#n" + std::to_string(n.id) + selector;
                for (const auto& [id, rule] : base) {
                    const auto it = std::find_if(other.begin(), other.end(), [&](const auto& o) { return o.first == id; });
                    const std::string head = "#n" + std::to_string(id);
                    const std::string where = id == n.id ? top : top + " " + head;
                    if (it == other.end()) {
                        css += where + " {\n  display: none;\n}\n";
                    } else if (it->second != rule) {
                        // What the usual look sets and this one does not goes back to its default.
                        std::string body = it->second.substr(head.size());
                        auto names = [](const std::string& r) {
                            std::vector<std::string> out;
                            for (usize at = r.find("\n  "); at != std::string::npos; at = r.find("\n  ", at + 1)) {
                                const usize colon = r.find(':', at);
                                if (colon != std::string::npos) out.push_back(r.substr(at + 3, colon - at - 3));
                            }
                            return out;
                        };
                        const std::vector<std::string> mine = names(body);
                        std::string resets;
                        for (const std::string& name : names(rule))
                            if (std::find(mine.begin(), mine.end(), name) == mine.end())
                                resets += "  " + name + (name == "display" ? ": block;\n" : ": unset;\n");
                        body.insert(body.size() - 2, resets);
                        css += where + body;
                    }
                }
            }
        }
    };
    visit(visit, screen.root, nullptr, screen.width, screen.height);
}

// A JSON string literal (for the click attribute).
std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
            continue;
        }
        out += c;
    }
    return out + "\"";
}

// What the game reads off a layer while it runs (forge::game::GameScreens):
// forge-text (a text with variables in it), forge-show-if, forge-bar-*,
// forge-click (a JSON list of [what, target]).
void write_game_attributes(const Node& n, std::string& html) {
    auto attr = [&](const char* name, std::string_view value) {
        html += std::string(" ") + name + "=\"" + escape_html(value) + "\"";
    };
    if (n.type == NodeType::Text && n.text.find('{') != std::string::npos) attr("forge-text", n.text);
    if (!n.show_if.empty()) attr("forge-show-if", n.show_if);
    if (!n.picture_from.empty()) attr("forge-picture", n.picture_from);
    if (n.list != ListSource::None && !n.children.empty()) {
        const Node& cell = n.children.front();
        attr("forge-list", list_word(n.list));
        attr("forge-list-gap", fmt(n.list_gap));
        attr("forge-cell", fmt(cell.x) + " " + fmt(cell.y) + " " + fmt(cell.w) + " " + fmt(cell.h));
    }
    if (!n.bar.value.empty()) {
        attr("forge-bar-value", n.bar.value);
        attr("forge-bar-max", n.bar.max);
        attr("forge-bar-from", word(n.bar.from, kBarFroms));
    }
    if (!n.on_click.empty()) {
        std::string list = "[";
        for (const Action& a : n.on_click) {
            if (list.size() > 1) list += ",";
            list += "[" + json_string(action_word(a.kind)) + "," + json_string(a.target) + "]";
        }
        attr("forge-click", list + "]");
        if (!n.click_sound.empty()) attr("forge-click-sound", n.click_sound);
    }
}

// Copies of a component shown in its «Выключена» look: buttons that are off.
void off_buttons(const Node& n, const std::vector<Component>& list, const Screen& library, std::vector<u32>& out) {
    if (!n.component.empty() && n.master) {
        const Component* component = find_component(list, n.component);
        const std::string property = component ? state_property(*component, library) : std::string();
        if (!property.empty() && state_selector(variant_value(n.variant, property)) == ".disabled") out.push_back(n.id);
    }
    for (const Node& c : n.children) off_buttons(c, list, library, out);
}

// Layers that show something (or do something when clicked) take the mouse in
// the game; the rest of a screen lets clicks through to the world under it.
bool takes_mouse(const Node& n) {
    if (!n.on_click.empty() || n.type == NodeType::Text || n.type == NodeType::Image) return true;
    if (n.list != ListSource::None) return true; // the wheel scrolls it
    if (!n.frame.image.empty() && n.frame.visible) return true;
    for (const Paint& p : n.fills)
        if (p.visible && p.opacity > 0 && (p.kind != PaintKind::Solid || p.color.a > 0)) return true;
    for (const Stroke& st : n.strokes)
        if (st.visible && st.width > 0 && st.color.a > 0) return true;
    return false;
}
void mouse_layers(const Node& n, std::string& list) {
    if (takes_mouse(n)) {
        if (!list.empty()) list += ", ";
        list += "#n" + std::to_string(n.id);
    }
    for (const Node& c : n.children) mouse_layers(c, list);
}

void write_elements(const Node& n, std::string& html, int depth, const std::vector<u32>& off, const Screen* screen = nullptr) {
    html.append(static_cast<usize>(depth) * 2, ' ');
    html += "<div id=\"n" + std::to_string(n.id) + "\" class=\"" + node_type_name(n.type) + "\" title=\"" +
            escape_html(n.name) + "\"";
    if (screen) { // the root: how the game shows the screen
        char size[64];
        std::snprintf(size, sizeof(size), "%g %g", static_cast<double>(screen->width), static_cast<double>(screen->height));
        html += std::string(" forge-screen=\"") + word(screen->show, kScreenShows) + "\" forge-fit=\"" +
                word(screen->fit, kScreenFits) + "\" forge-size=\"" + size + "\"";
        if (screen->fit == ScreenFit::Fit) html += " forge-bars=\"" + color_hex(screen->bars) + "\"";
        if (screen->pauses) html += " forge-pauses=\"1\"";
        if (!screen->esc_closes) html += " forge-esc=\"0\"";
        if (screen->dim && screen->show == ScreenShow::Command) html += " forge-dim=\"1\"";
        if (screen->over != WindowOver::Any && screen->show == ScreenShow::Command)
            html += std::string(" forge-over=\"") + word(screen->over, kWindowOvers) + "\"";
        if (screen->ending != WindowEnding::None && screen->show == ScreenShow::Command)
            html += std::string(" forge-ending=\"") + word(screen->ending, kWindowEndings) + "\"";
        if (screen->appear != Appear::None)
            html += std::string(" forge-appear=\"") + word(screen->appear, kAppears) + "\" forge-appear-time=\"" +
                    fmt(screen->appear_time) + "\"";
        if (!screen->music.empty()) html += " forge-music=\"" + escape_html(screen->music) + "\"";
        if (!screen->button_sound.empty()) html += " forge-button-sound=\"" + escape_html(screen->button_sound) + "\"";
    }
    write_game_attributes(n, html);
    if (std::find(off.begin(), off.end(), n.id) != off.end()) html += " forge-disabled=\"1\"";
    html += ">";
    if (n.type == NodeType::Text) {
        html += escape_html(n.text);
    } else if (!n.children.empty()) {
        html += "\n";
        for (const Node& c : n.children) write_elements(c, html, depth + 1, off);
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
    std::string keyframes;
    write_css(screen.root, nullptr, screen.width, screen.height, html, options.motion ? &keyframes : nullptr, 0);
    html += keyframes;
    if (options.library && !screen.library) write_states(screen, *options.library, html);
    if (!screen.library) {
        // Over the world only what shows something takes the mouse (the
        // screen itself is see-through for clicks, the page too: else it
        // caught them for the screens under it).
        std::string list;
        for (const Node& c : screen.root.children) mouse_layers(c, list);
        html += "html, body, #n" + std::to_string(screen.root.id) + " {\n  pointer-events: none;\n}\n";
        if (!list.empty()) html += list + " {\n  pointer-events: auto;\n}\n";
        // Buttons take the keyboard (Tab, then Enter or Space) where the world
        // does not: the main menu and windows that stop the game. Over a
        // running game Space and Enter stay the game's.
        if (screen.show == ScreenShow::Menu || (screen.show == ScreenShow::Command && screen.pauses)) {
            std::string keys;
            auto visit = [&](auto&& self, const Node& n) -> void {
                if (!n.on_click.empty()) keys += (keys.empty() ? "#n" : ", #n") + std::to_string(n.id);
                for (const Node& c : n.children) self(self, c);
            };
            for (const Node& c : screen.root.children) visit(visit, c);
            if (!keys.empty()) html += keys + " {\n  tab-index: auto;\n}\n";
        }
    }
    // A window that darkens what is under it: a veil over the whole player's screen, under the window, that
    // takes the clicks (what is under it gets none).
    const bool veil = !screen.library && screen.dim && screen.show == ScreenShow::Command;
    if (veil)
        html += "#forge-dim {\n  position: absolute;\n  left: 0px;\n  top: 0px;\n  width: 100%;\n  height: 100%;\n"
                "  background-color: rgba(0, 0, 0, 0.5);\n  pointer-events: auto;\n}\n";
    html += "</style>\n</head>\n<body>\n";
    if (veil) html += "<div id=\"forge-dim\"></div>\n";
    std::vector<u32> off;
    if (options.library && !screen.library) off_buttons(screen.root, components(*options.library), *options.library, off);
    write_elements(screen.root, html, 0, off, screen.library ? nullptr : &screen);
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
