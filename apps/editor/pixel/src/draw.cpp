#include "forge/pixel/draw.h"

#include "script_impl.h"

#include "forge/pixel/code_sites.h"
#include "forge/pixel/rig.h"
#include "forge/vector/svg_shape.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace forge::pixel {

namespace {

using Kind = DrawShape::Kind;
using Mode = PoseScript::Impl::Mode;

const char* kind_word(Kind k) {
    switch (k) {
    case Kind::Rect: return "rect";
    case Kind::Circle: return "circle";
    case Kind::Ellipse: return "ellipse";
    case Kind::Line: return "line";
    case Kind::Poly: return "poly";
    case Kind::Path: return "path";
    case Kind::Group: return "group";
    }
    return "?";
}

// The fields each call takes, as the error about a field it does not have lists them.
const char* fields_of(Kind k) {
    switch (k) {
    case Kind::Rect: return "x, y, w, h, r";
    case Kind::Circle: return "x, y, r";
    case Kind::Ellipse: return "x, y, rx, ry";
    case Kind::Line: return "x1, y1, x2, y2";
    case Kind::Poly: return "points";
    case Kind::Path: return "d";
    case Kind::Group: return "x, y, rot, scale";
    }
    return "";
}

bool own_field(Kind k, std::string_view f) {
    switch (k) {
    case Kind::Rect: return f == "x" || f == "y" || f == "w" || f == "h" || f == "r";
    case Kind::Circle: return f == "x" || f == "y" || f == "r";
    case Kind::Ellipse: return f == "x" || f == "y" || f == "rx" || f == "ry";
    case Kind::Line: return f == "x1" || f == "y1" || f == "x2" || f == "y2";
    case Kind::Poly: return f == "points";
    case Kind::Path: return f == "d";
    case Kind::Group: return f == "x" || f == "y" || f == "rot" || f == "scale";
    }
    return false;
}

bool common_field(std::string_view f) {
    return f == "fill" || f == "stroke" || f == "stroke_width" || f == "dash" || f == "cap" || f == "join" || f == "opacity" ||
           f == "hidden" || f == "pivot";
}

// A name as a part's id: letters (of any language), digits, _ and -, not first a digit or -; well-formed UTF-8.
bool good_id(std::string_view s) {
    if (s.empty()) return false;
    for (usize i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        if (c < 0x80) {
            const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            const bool more = (c >= '0' && c <= '9') || c == '-';
            if (!letter && (i == 0 || !more)) return false;
            ++i;
            continue;
        }
        const usize n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || c == 0xC0 || c == 0xC1 || i + n > s.size()) return false;
        for (usize j = 1; j < n; ++j)
            if ((static_cast<u8>(s[i + j]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

// What the error says the call is: «rect «torso»», «rect» without an id.
std::string named(Kind k, const std::string& id) { return id.empty() ? kind_word(k) : std::string(kind_word(k)) + " «" + id + "»"; }

// Reads a call's fields. Nothing here raises a Luau error: Luau throws past frames built without exceptions, so a string
// alive then would never be freed. An error is kept in `err` and raised by the caller once these are gone.
struct Reader {
    lua_State* L;
    Kind kind;
    const std::string& id;
    int opts; // the fields' table on the stack; 0 when none
    std::string& err;

    bool fail(const std::string& text) {
        if (err.empty()) err = named(kind, id) + ": " + text;
        return false;
    }
    // Pushes the field (nil when there is none), raw: a metatable of the author's runs nothing here.
    void push(const char* f) {
        if (!opts) {
            lua_pushnil(L);
            return;
        }
        lua_pushstring(L, f);
        lua_rawget(L, opts);
    }
    bool number(const char* f, bool needed, f64 fallback, f64& out, bool non_negative = false) {
        out = fallback;
        push(f);
        const int type = lua_type(L, -1);
        const f64 v = type == LUA_TNUMBER ? lua_tonumber(L, -1) : 0;
        lua_pop(L, 1);
        if (type == LUA_TNIL) return !needed || fail(std::string("нет ") + f);
        if (type != LUA_TNUMBER) return fail(std::string(f) + " — число");
        if (!std::isfinite(v)) return fail(std::string(f) + " — конечное число");
        if (non_negative && v < 0) return fail(std::string(f) + " не меньше 0");
        out = v;
        return true;
    }
    bool color(const char* f, std::string& out) {
        push(f);
        const int type = lua_type(L, -1);
        std::string text = type == LUA_TSTRING ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (type == LUA_TNIL) return true;
        if (type != LUA_TSTRING) return fail(std::string(f) + " — цвет строкой: \"#d04a30\"");
        if (text == "none") {
            out = text;
            return true;
        }
        u8 rgb[3];
        if (!vector::parse_svg_color(text, rgb))
            return fail(std::string(f) + ": цвет «" + text + "» не читается (#rgb, #rrggbb, rgb(r, g, b), имя вроде red или \"none\")");
        char hex[8];
        std::snprintf(hex, sizeof hex, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
        out = hex;
        return true;
    }
    // A word of a few: cap = "round".
    bool word(const char* f, std::initializer_list<const char*> words, std::string& out) {
        push(f);
        const int type = lua_type(L, -1);
        std::string text = type == LUA_TSTRING ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (type == LUA_TNIL) return true;
        std::string list;
        for (const char* w : words) {
            if (text == w) {
                out = text;
                return true;
            }
            list += (list.empty() ? "\"" : ", \"") + std::string(w) + "\"";
        }
        return fail(std::string(f) + " — " + list + (type == LUA_TSTRING ? ", а не «" + text + "»" : std::string()));
    }
    // Numbers not below 0, not all 0: dash = { 4, 2 }.
    bool lengths(const char* f, std::vector<f64>& out) {
        push(f);
        const bool has = !lua_isnil(L, -1);
        bool ok = !has || (lua_istable(L, -1) && lua_objlen(L, -1) > 0);
        bool any = false;
        for (int i = 1; ok && has && i <= lua_objlen(L, -1); ++i) {
            lua_rawgeti(L, -1, i);
            ok = lua_type(L, -1) == LUA_TNUMBER && std::isfinite(lua_tonumber(L, -1)) && lua_tonumber(L, -1) >= 0;
            if (ok) out.push_back(lua_tonumber(L, -1)), any = any || lua_tonumber(L, -1) > 0;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (!ok || (has && !any)) return fail(std::string(f) + " — длины штрихов и промежутков, числа не меньше 0 и не все 0: " + f + " = { 4, 2 }");
        return true;
    }
    // Two numbers, { x, y }.
    bool point(const char* f, bool& has, vector::Point& out) {
        push(f);
        has = !lua_isnil(L, -1);
        bool ok = !has || (lua_istable(L, -1) && lua_objlen(L, -1) == 2);
        f64 xy[2] = {0, 0};
        for (int i = 0; i < 2 && ok && has; ++i) {
            lua_rawgeti(L, -1, i + 1);
            ok = lua_type(L, -1) == LUA_TNUMBER && std::isfinite(lua_tonumber(L, -1));
            xy[i] = ok ? lua_tonumber(L, -1) : 0;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (!ok) return fail(std::string(f) + " — два числа: " + f + " = { 16, 14 }");
        out = {xy[0], xy[1]};
        return true;
    }
};

// Adds the shape the running call draws, with its arguments at 1…: [id], [fields], and for a group its function last
// (its index in `fn`). False with im.draw_error.
bool add_shape(lua_State* L, PoseScript::Impl& im, Kind kind, int& fn) {
    const char* what = kind_word(kind);
    std::string& err = im.draw_error;
    err.clear();
    if (im.mode != Mode::Draw || !im.drawing) {
        err = std::string(what) + " рисует только внутри draw = function(p, rng) … end";
        return false;
    }
    std::string id;
    int at = 1;
    if (lua_type(L, at) == LUA_TSTRING) {
        id = lua_tostring(L, at++);
        if (!good_id(id)) {
            err = std::string(what) + ": id «" + id + "» — буквы, цифры, _ и -, не с цифры и не с -";
            return false;
        }
    } else if (!lua_isnil(L, at) && !lua_istable(L, at) && !(kind == Kind::Group && lua_isfunction(L, at))) {
        err = std::string(what) + ": первым идёт id строкой: " + what + "(\"body\", { … })";
        return false;
    }
    int opts = 0;
    if (lua_istable(L, at)) opts = at++;
    Reader r{L, kind, id, opts, err};
    if (!opts && kind != Kind::Group) return r.fail(std::string("поля таблицей: ") + what + "(\"id\", { " + fields_of(kind) + " })");
    if (kind == Kind::Group) {
        if (!lua_isfunction(L, at)) return r.fail("последним идёт функция: group(\"body\", function() … end)");
        fn = at;
    }
    // Every field is one of the call's: a typo («widht») is an error, not a shape drawn otherwise.
    if (opts) {
        lua_pushnil(L);
        while (lua_next(L, opts)) {
            lua_pop(L, 1);
            const std::string f = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : std::string("?");
            if (!own_field(kind, f) && !common_field(f)) {
                lua_pop(L, 1);
                return r.fail("поля «" + f + "» нет (есть " + fields_of(kind) + ", fill, stroke, stroke_width, dash, cap, join, opacity, hidden, pivot)");
            }
        }
    }
    Drawing& d = *im.drawing;
    if (!id.empty() && im.drawn_ids.count(id)) return r.fail("id «" + id + "» уже есть в рисунке");
    if (d.shapes.size() >= kMaxDrawShapes) return r.fail("фигур больше " + std::to_string(kMaxDrawShapes));
    if (kind == Kind::Group && im.groups.size() >= kMaxDrawDepth) return r.fail("групп друг в друге больше " + std::to_string(kMaxDrawDepth));
    DrawShape s;
    s.kind = kind;
    s.id = id;
    s.parent = im.groups.empty() ? -1 : im.groups.back();
    const SiteRef site = im.site_ref(im.site);
    if (const CallSite* c = im.call(site); c && site.file == 0) s.line = c->line;
    if (!r.color("fill", s.fill) || !r.color("stroke", s.stroke)) return false;
    r.push("stroke_width");
    const bool width = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (width && !r.number("stroke_width", false, 1, s.stroke_width, true)) return false;
    if (!r.lengths("dash", s.dash) || !r.word("cap", {"butt", "round", "square"}, s.cap) ||
        !r.word("join", {"miter", "round", "bevel"}, s.join))
        return false;
    if (!r.number("opacity", false, 1, s.opacity)) return false;
    if (s.opacity < 0 || s.opacity > 1) return r.fail("opacity от 0 до 1");
    r.push("hidden");
    const int hidden = lua_type(L, -1);
    s.hidden = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (hidden != LUA_TNIL && hidden != LUA_TBOOLEAN) return r.fail("hidden — true или false");
    if (!r.point("pivot", s.has_pivot, s.pivot)) return false;
    if (s.has_pivot && id.empty()) return r.fail("pivot у фигуры без id: опорная точка бывает у части");
    bool ok = true;
    switch (kind) {
    case Kind::Rect:
        ok = r.number("x", true, 0, s.x) && r.number("y", true, 0, s.y) && r.number("w", true, 0, s.w, true) &&
             r.number("h", true, 0, s.h, true) && r.number("r", false, 0, s.r, true);
        break;
    case Kind::Circle: ok = r.number("x", true, 0, s.x) && r.number("y", true, 0, s.y) && r.number("r", true, 0, s.r, true); break;
    case Kind::Ellipse:
        ok = r.number("x", true, 0, s.x) && r.number("y", true, 0, s.y) && r.number("rx", true, 0, s.rx, true) &&
             r.number("ry", true, 0, s.ry, true);
        break;
    case Kind::Line:
        ok = r.number("x1", true, 0, s.x1) && r.number("y1", true, 0, s.y1) && r.number("x2", true, 0, s.x2) &&
             r.number("y2", true, 0, s.y2);
        break;
    case Kind::Poly: {
        r.push("points");
        const bool table = lua_istable(L, -1);
        const int n = table ? lua_objlen(L, -1) : 0;
        for (int i = 1; i <= n && ok; ++i) {
            lua_rawgeti(L, -1, i);
            ok = lua_type(L, -1) == LUA_TNUMBER && std::isfinite(lua_tonumber(L, -1));
            if (ok) s.points.push_back(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (!table) return r.fail("нет points: poly(\"id\", { points = { 0, 0, 8, 0, 4, 6 } })");
        if (!ok) return r.fail("points — пары чисел x, y");
        if (n < 6 || n % 2 != 0) return r.fail("points — пары чисел x, y, не меньше трёх точек");
        break;
    }
    case Kind::Path: {
        r.push("d");
        const bool text = lua_type(L, -1) == LUA_TSTRING;
        if (text) s.d = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (!text) return r.fail("нет d строкой: path(\"id\", { d = \"M 0 0 L 4 0 L 2 3 Z\" })");
        vector::Path p;
        std::string why;
        if (!vector::parse_path(s.d, p, &why)) return r.fail("d не читается: " + why);
        break;
    }
    case Kind::Group:
        ok = r.number("x", false, 0, s.x) && r.number("y", false, 0, s.y) && r.number("rot", false, 0, s.rot) &&
             r.number("scale", false, 1, s.scale);
        break;
    }
    if (!ok) return false;
    if (!id.empty()) im.drawn_ids.insert(id);
    d.shapes.push_back(std::move(s));
    return true;
}

// Raises the error add_shape kept, its text on the stack and no string of ours alive.
[[noreturn]] void raise(lua_State* L, PoseScript::Impl& im) {
    lua_pushlstring(L, im.draw_error.data(), im.draw_error.size());
    im.draw_error.clear();
    lua_error(L);
}

int shape_call(lua_State* L, Kind kind) {
    PoseScript::Impl& im = impl_of(L);
    int fn = 0;
    if (!add_shape(L, im, kind, fn)) raise(L, im);
    return 0;
}

int api_rect(lua_State* L) { return shape_call(L, Kind::Rect); }
int api_circle(lua_State* L) { return shape_call(L, Kind::Circle); }
int api_ellipse(lua_State* L) { return shape_call(L, Kind::Ellipse); }
int api_line(lua_State* L) { return shape_call(L, Kind::Line); }
int api_poly(lua_State* L) { return shape_call(L, Kind::Poly); }
int api_path(lua_State* L) { return shape_call(L, Kind::Path); }

// group(id, fn), group(id, { … }, fn), group({ … }, fn), group(fn): what fn draws is inside the group.
int api_group(lua_State* L) {
    PoseScript::Impl& im = impl_of(L);
    int fn = 0;
    if (!add_shape(L, im, Kind::Group, fn)) raise(L, im);
    im.groups.push_back(static_cast<i32>(im.drawing->shapes.size()) - 1);
    lua_pushvalue(L, fn);
    lua_call(L, 0, 0);
    im.groups.pop_back();
    return 0;
}

int api_canvas(lua_State* L) {
    PoseScript::Impl& im = impl_of(L);
    if (im.mode != Mode::Draw || !im.drawing) luaL_error(L, "canvas — только внутри draw = function(p, rng) … end");
    Drawing& d = *im.drawing;
    if (d.canvas) luaL_error(L, "canvas — один раз");
    if (!d.shapes.empty()) luaL_error(L, "canvas — до первой фигуры");
    const f64 w = luaL_checknumber(L, 1), h = luaL_checknumber(L, 2);
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0) luaL_error(L, "canvas(w, h): w и h больше 0");
    d.canvas = true;
    d.width = w;
    d.height = h;
    const SiteRef site = im.site_ref(im.site);
    if (const CallSite* c = im.call(site); c && site.file == 0) d.canvas_line = c->line;
    return 0;
}

std::string escaped(std::string_view s) {
    std::string out;
    for (char c : s) {
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

} // namespace

void register_draw_api(lua_State* L) {
    const std::pair<const char*, lua_CFunction> fns[] = {{"canvas", &api_canvas}, {"rect", &api_rect},   {"circle", &api_circle},
                                                         {"ellipse", &api_ellipse}, {"line", &api_line}, {"poly", &api_poly},
                                                         {"path", &api_path},       {"group", &api_group}};
    for (const auto& [name, fn] : fns) {
        lua_pushcfunction(L, fn, name);
        lua_setglobal(L, name);
    }
}

std::string drawing_svg(const Drawing& d, u32 w, u32 h) {
    const f64 cw = d.canvas ? d.width : w, ch = d.canvas ? d.height : h;
    const std::string sw = format_number(cw), sh = format_number(ch);
    std::string out = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + sw + "\" height=\"" + sh + "\" viewBox=\"0 0 " + sw + " " +
                      sh + "\">\n";
    std::vector<i32> open; // the groups not closed yet
    auto attr = [&](const char* name, f64 v) { out += std::string(" ") + name + "=\"" + format_number(v) + "\""; };
    auto close_to = [&](i32 parent) {
        while (!open.empty() && open.back() != parent) {
            open.pop_back();
            out += std::string(2 * (open.size() + 1), ' ') + "</g>\n";
        }
    };
    for (usize i = 0; i < d.shapes.size(); ++i) {
        const DrawShape& s = d.shapes[i];
        close_to(s.parent);
        out += std::string(2 * (open.size() + 1), ' ');
        out += s.kind == Kind::Rect      ? "<rect"
               : s.kind == Kind::Circle  ? "<circle"
               : s.kind == Kind::Ellipse ? "<ellipse"
               : s.kind == Kind::Line    ? "<line"
               : s.kind == Kind::Poly    ? "<polygon"
               : s.kind == Kind::Path    ? "<path"
                                         : "<g";
        if (!s.id.empty()) out += " id=\"" + escaped(s.id) + "\"";
        switch (s.kind) {
        case Kind::Rect:
            attr("x", s.x), attr("y", s.y), attr("width", s.w), attr("height", s.h);
            if (s.r > 0) attr("rx", s.r);
            break;
        case Kind::Circle: attr("cx", s.x), attr("cy", s.y), attr("r", s.r); break;
        case Kind::Ellipse: attr("cx", s.x), attr("cy", s.y), attr("rx", s.rx), attr("ry", s.ry); break;
        case Kind::Line: attr("x1", s.x1), attr("y1", s.y1), attr("x2", s.x2), attr("y2", s.y2); break;
        case Kind::Poly: {
            out += " points=\"";
            for (usize k = 0; k < s.points.size(); ++k) out += (k ? (k % 2 ? "," : " ") : "") + format_number(s.points[k]);
            out += "\"";
            break;
        }
        case Kind::Path: out += " d=\"" + escaped(s.d) + "\""; break;
        case Kind::Group: {
            std::string t;
            if (s.x != 0 || s.y != 0) t += "translate(" + format_number(s.x) + " " + format_number(s.y) + ")";
            if (s.rot != 0) t += (t.empty() ? "" : " ") + std::string("rotate(") + format_number(s.rot) + ")";
            if (s.scale != 1) t += (t.empty() ? "" : " ") + std::string("scale(") + format_number(s.scale) + ")";
            if (!t.empty()) out += " transform=\"" + t + "\"";
            break;
        }
        }
        if (!s.fill.empty()) out += " fill=\"" + s.fill + "\"";
        if (!s.stroke.empty()) out += " stroke=\"" + s.stroke + "\"";
        else if (s.kind == Kind::Line) out += " stroke=\"#000000\"";
        if (s.stroke_width >= 0) attr("stroke-width", s.stroke_width);
        if (!s.dash.empty()) {
            out += " stroke-dasharray=\"";
            for (usize k = 0; k < s.dash.size(); ++k) out += (k ? " " : "") + format_number(s.dash[k]);
            out += "\"";
        }
        if (!s.cap.empty()) out += " stroke-linecap=\"" + s.cap + "\"";
        if (!s.join.empty()) out += " stroke-linejoin=\"" + s.join + "\"";
        if (s.opacity != 1) attr("opacity", s.opacity);
        if (s.hidden) out += " display=\"none\"";
        if (s.kind == Kind::Group) {
            out += ">\n";
            open.push_back(static_cast<i32>(i));
        } else {
            out += "/>\n";
        }
    }
    close_to(-1);
    return out + "</svg>\n";
}

bool drawn_pivot(const Drawing& d, const std::string& id, vector::Point& out) {
    for (const DrawShape& s : d.shapes)
        if (s.has_pivot && s.id == id) {
            out = s.pivot;
            return true;
        }
    return false;
}

bool PoseScript::has_draw() const { return impl_->has_draw; }
const std::vector<std::string>& PoseScript::drawings() const { return impl_->drawings; }

namespace {

// «draw», «angry»: the names a drawing goes by (part 4д).
std::string drawing_names(const PoseScript::Impl& im) {
    std::string out = "«draw»";
    for (const std::string& n : im.drawings) out += ", «" + n + "»";
    return out;
}

// Runs the drawing function on top of T's stack (the module under it) into out: what draw and draw_named share.
bool run_drawing(PoseScript::Impl& im, lua_State* T, const std::map<std::string, f64>& values, u64 seed, Drawing& out) {
    push_params(im, T, values);
    lua_getglobal(T, "rng");
    // The drawing's chance: from the seed alone, the same on every run.
    im.rng = frame_seed(seed, 0, "draw");
    Drawing d;
    im.drawing = &d;
    im.drawn_ids.clear();
    im.groups.clear();
    const auto was = im.mode;
    im.mode = PoseScript::Impl::Mode::Draw;
    im.begin_call();
    im.active = T;
    const int status = lua_pcall(T, 2, 0, 0);
    im.in_call = false;
    im.mode = was;
    im.drawing = nullptr;
    im.drawn_ids.clear();
    im.groups.clear();
    bool ok = status == LUA_OK;
    if (!ok) {
        im.fail(lua_tostring(T, -1));
        lua_pop(T, 1);
    } else if (d.shapes.empty()) {
        im.error = "draw ничего не нарисовал: rect, circle, ellipse, line, poly, path, group";
        ok = false;
    }
    lua_pop(im.L, 1);
    if (ok) out = std::move(d);
    return ok;
}

} // namespace

bool PoseScript::draw(const std::map<std::string, f64>& values, u64 seed, Drawing& out) {
    Impl& im = *impl_;
    out = {};
    im.error.clear();
    im.error_line = 0;
    if (!im.ok) {
        im.error = "скрипт не загружен";
        return false;
    }
    if (!im.has_draw) {
        im.error = "в коде нет draw = function(p, rng) … end";
        return false;
    }
    lua_State* T = im.module();
    if (!T) return false;
    lua_getfield(T, -1, "draw");
    return run_drawing(im, T, values, seed, out);
}

bool PoseScript::draw_named(std::string_view name, const std::map<std::string, f64>& values, u64 seed, Drawing& out) {
    if (name == "draw") return draw(values, seed, out);
    Impl& im = *impl_;
    out = {};
    im.error.clear();
    im.error_line = 0;
    if (!im.ok) {
        im.error = "скрипт не загружен";
        return false;
    }
    if (!std::binary_search(im.drawings.begin(), im.drawings.end(), name)) {
        im.error = "нет рисунка «" + std::string(name) + "»: есть " + drawing_names(im);
        return false;
    }
    lua_State* T = im.module();
    if (!T) return false;
    lua_getfield(T, -1, "drawings");
    lua_pushlstring(T, name.data(), name.size());
    lua_gettable(T, -2);
    lua_remove(T, -2);
    if (run_drawing(im, T, values, seed, out)) return true;
    // Its name before the line: «рисунок «angry», строка 12: …».
    const std::string head = "рисунок «" + std::string(name) + "»";
    if (im.error.rfind("draw ничего", 0) == 0) {
        im.error = head + " ничего не нарисовал: rect, circle, ellipse, line, poly, path, group";
        im.error_line = im.files[0].code.drawing_line(name);
        if (im.error_line) im.error = "строка " + std::to_string(im.error_line) + ": " + im.error;
    } else {
        im.error = head + (im.error.rfind("строка ", 0) == 0 ? ", " : ": ") + im.error;
    }
    return false;
}

} // namespace forge::pixel
