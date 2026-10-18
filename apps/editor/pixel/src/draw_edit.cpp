#include "forge/pixel/draw_edit.h"
#include "forge/pixel/rig.h"

#include "forge/vector/svg_shape.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace forge::pixel {

namespace {

using Kind = DrawShape::Kind;

bool same(f64 a, f64 b) { return format_number(a) == format_number(b); }
bool same(vector::Point a, vector::Point b) { return same(a.x, b.x) && same(a.y, b.y); }

std::string trimmed(std::string_view t) {
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
    return std::string(t);
}

// A number as typed: a comma for the point too ("0,5").
bool typed_number(std::string_view text, f64& out) {
    std::string t = trimmed(text);
    std::replace(t.begin(), t.end(), ',', '.');
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return !t.empty() && end && *end == '\0';
}

// A colour as draw reads it, "#rrggbb" or "none"; false when it does not.
bool drawn_color(std::string_view text, std::string& out) {
    if (text == "none") return out = "none", true;
    u8 rgb[3];
    if (!vector::parse_svg_color(text, rgb)) return false;
    char hex[8];
    std::snprintf(hex, sizeof hex, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
    out = hex;
    return true;
}

bool not_negative(std::string_view f) { return f == "w" || f == "h" || f == "r" || f == "rx" || f == "ry" || f == "stroke_width"; }

// Where the fields of the call are: a shape's last argument, a group's the one before its function. A group with none
// (group("body", function() … end)) gets one after its id: `insert` then names where.
struct Fields {
    const CodeArg* table = nullptr;
    bool insert = false;
    u32 at = 0;
};

bool fields_of(const CodeSites& code, const DrawShape& s, Fields& out, std::string& why) {
    if (s.module) return why = "фигуру рисует код модуля: правится в нём", false;
    if (s.site < 0 || static_cast<usize>(s.site) >= code.sites().size()) return why = "у фигуры нет вызова в коде", false;
    const CallSite& c = code.sites()[static_cast<usize>(s.site)];
    const usize n = c.args.size();
    const CodeArg* fields = nullptr;
    if (s.kind == Kind::Group) {
        if (n >= 2 && c.args[n - 1].kind == ArgKind::Function) {
            const CodeArg& before = c.args[n - 2];
            if (before.kind == ArgKind::Table) fields = &before;
            else if (before.kind == ArgKind::String || before.kind == ArgKind::Other) {
                out.insert = true;
                out.at = before.span.end;
                return true;
            }
        }
    } else if (n > 0 && c.args[n - 1].kind == ArgKind::Table) {
        fields = &c.args[n - 1];
    }
    if (!fields) return why = "поля фигуры не таблицей в вызове: правится в коде", false;
    out.table = fields;
    return true;
}

} // namespace

f64 tool_round(f64 v, f64 step) {
    const f64 r = step > 0 ? std::round(v / step) * step : std::round(v * 10) / 10;
    return r + 0.0; // no -0
}

vector::Point tool_point(vector::Point p, f64 step) { return {tool_round(p.x, step), tool_round(p.y, step)}; }

f64 snap_steps(f64 v, f64 step) { return step > 0 ? std::round(v / step) * step + 0.0 : v; }

const char* new_shape_word(NewShape::Kind k) {
    switch (k) {
    case NewShape::Kind::Rect: return "rect";
    case NewShape::Kind::Ellipse: return "ellipse";
    case NewShape::Kind::Line: return "line";
    case NewShape::Kind::Path: return "path";
    case NewShape::Kind::Poly: return "poly";
    }
    return "?";
}

std::string new_shape_id(NewShape::Kind k, const std::set<std::string>& taken) {
    for (u32 n = 1;; ++n) {
        std::string id = std::string(new_shape_word(k)) + "_" + std::to_string(n);
        if (!taken.count(id)) return id;
    }
}

std::set<std::string> taken_shape_ids(const Rig& main, const std::vector<const Drawing*>& drawings) {
    std::set<std::string> taken;
    for (const Part& p : main.parts()) taken.insert(p.id);
    for (const Drawing* d : drawings)
        for (const DrawShape& s : d->shapes)
            if (!s.id.empty()) taken.insert(s.id);
    return taken;
}

void add_pen_point(std::vector<vector::Point>& points, vector::Point p) {
    if (points.empty() || !same(points.back(), p)) points.push_back(p);
}

bool shape_call(const NewShape& s, const std::string& id, std::string& out, std::string& why) {
    out.clear();
    why.clear();
    constexpr const char* kNoSize = "фигура без размера не рисуется: протяните мышью";
    auto n = [](f64 v) { return format_number(v); };
    const std::string head = std::string(new_shape_word(s.kind)) + "(" + quote_string(id) + ", { ";
    const std::string color = quote_string(s.color);
    std::vector<vector::Point> pts;
    for (const vector::Point& p : s.points) add_pen_point(pts, p);
    switch (s.kind) {
    case NewShape::Kind::Rect:
    case NewShape::Kind::Ellipse: {
        if (s.points.size() != 2) return why = kNoSize, false;
        const vector::Point a = s.points[0], b = s.points[1];
        const f64 w = std::fabs(b.x - a.x), h = std::fabs(b.y - a.y);
        if (same(w, 0) || same(h, 0)) return why = kNoSize, false;
        if (s.kind == NewShape::Kind::Rect)
            out = head + "x = " + n(std::min(a.x, b.x)) + ", y = " + n(std::min(a.y, b.y)) + ", w = " + n(w) + ", h = " + n(h);
        else
            out = head + "x = " + n((a.x + b.x) / 2) + ", y = " + n((a.y + b.y) / 2) + ", rx = " + n(w / 2) + ", ry = " + n(h / 2);
        out += ", fill = " + color + " })";
        return true;
    }
    case NewShape::Kind::Line: {
        if (s.points.size() != 2 || same(s.points[0], s.points[1])) return why = kNoSize, false;
        const vector::Point a = s.points[0], b = s.points[1];
        out = head + "x1 = " + n(a.x) + ", y1 = " + n(a.y) + ", x2 = " + n(b.x) + ", y2 = " + n(b.y) + ", stroke = " + color + " })";
        return true;
    }
    case NewShape::Kind::Path: {
        if (pts.size() < 2) return why = "перу нужно две разные точки: щёлкните ещё", false;
        std::string d;
        for (usize i = 0; i < pts.size(); ++i) d += (i == 0 ? "M " : " L ") + n(pts[i].x) + " " + n(pts[i].y);
        out = head + "d = " + quote_string(d) + ", fill = \"none\", stroke = " + color + " })";
        return true;
    }
    case NewShape::Kind::Poly: {
        if (pts.size() >= 2 && same(pts.front(), pts.back())) pts.pop_back(); // closed on its first point
        if (pts.size() < 3) return why = "замкнутому перу нужно три разные точки", false;
        std::string list;
        for (const vector::Point& p : pts) list += (list.empty() ? "" : ", ") + n(p.x) + ", " + n(p.y);
        out = head + "points = { " + list + " }, fill = " + color + " })";
        return true;
    }
    }
    return false;
}

std::vector<std::string> shape_fields(DrawShape::Kind k) {
    std::vector<std::string> f;
    switch (k) {
    case Kind::Rect: f = {"x", "y", "w", "h", "r"}; break;
    case Kind::Circle: f = {"x", "y", "r"}; break;
    case Kind::Ellipse: f = {"x", "y", "rx", "ry"}; break;
    case Kind::Line: f = {"x1", "y1", "x2", "y2"}; break;
    case Kind::Group: f = {"x", "y", "rot", "scale"}; break;
    case Kind::Poly:
    case Kind::Path: break;
    }
    if (k != Kind::Line) f.push_back("fill");
    for (const char* c : {"stroke", "stroke_width", "opacity"}) f.push_back(c);
    return f;
}

bool shape_field_is_color(std::string_view field) { return field == "fill" || field == "stroke"; }

bool shape_field_number(const DrawShape& s, std::string_view field, f64& out) {
    const std::vector<std::string> fields = shape_fields(s.kind);
    if (shape_field_is_color(field) || std::find(fields.begin(), fields.end(), field) == fields.end()) return false;
    if (field == "x") out = s.x;
    else if (field == "y") out = s.y;
    else if (field == "w") out = s.w;
    else if (field == "h") out = s.h;
    else if (field == "r") out = s.r;
    else if (field == "rx") out = s.rx;
    else if (field == "ry") out = s.ry;
    else if (field == "x1") out = s.x1;
    else if (field == "y1") out = s.y1;
    else if (field == "x2") out = s.x2;
    else if (field == "y2") out = s.y2;
    else if (field == "rot") out = s.rot;
    else if (field == "scale") out = s.scale;
    else if (field == "opacity") out = s.opacity;
    else if (field == "stroke_width") {
        if (s.stroke_width < 0) return false;
        out = s.stroke_width;
    } else {
        return false;
    }
    return true;
}

std::string shape_field_text(const DrawShape& s, std::string_view field) {
    if (field == "fill") return s.fill;
    if (field == "stroke") return s.stroke;
    f64 v = 0;
    return shape_field_number(s, field, v) ? format_number(v) : std::string();
}

bool can_edit_shape(const CodeSites& code, const DrawShape& s, std::string_view field, std::string& why) {
    why.clear();
    const std::vector<std::string> fields = shape_fields(s.kind);
    if (std::find(fields.begin(), fields.end(), field) == fields.end()) return why = "у фигуры нет поля «" + std::string(field) + "»", false;
    Fields f;
    if (!fields_of(code, s, f, why)) return false;
    const CodeField* has = f.table ? field_of(*f.table, field) : nullptr;
    // A colour of an expression stays the code's. A number field is a number or an expression of one: draw refuses others.
    if (shape_field_is_color(field) && has && has->value.kind != ArgKind::String)
        return why = "цвет «" + std::string(field) + "» задан выражением: правится в коде", false;
    return true;
}

bool edit_shape_field(const CodeSites& code, std::string_view source, const DrawShape& s, std::string_view field,
                      std::string_view text, std::string& out, std::string& why) {
    out.clear();
    if (!can_edit_shape(code, s, field, why)) return false;
    Fields f;
    fields_of(code, s, f, why);
    const std::string name(field);
    const std::string typed = trimmed(text);
    std::string value_text;
    if (shape_field_is_color(field)) {
        if (typed.empty()) return why = "пусто не пишется: «none» — без цвета, свой цвет — уберите поле в коде", false;
        std::string c;
        if (!drawn_color(typed, c)) return why = "цвет «" + typed + "» не читается (#rgb, #rrggbb, rgb(r, g, b), имя вроде red или none)", false;
        if (c == (field == "fill" ? s.fill : s.stroke)) return out.assign(source), true;
        value_text = quote_string(typed);
        if (f.insert) {
            out = std::string(source.substr(0, f.at)) + ", { " + name + " = " + value_text + " }" + std::string(source.substr(f.at));
            return true;
        }
        return rewrite_field(source, *f.table, name, value_text, out, why);
    }
    f64 v = 0;
    if (!typed_number(typed, v)) return why = "«" + name + "» — число", false;
    if (!std::isfinite(v)) return why = "«" + name + "» — конечное число", false;
    if (not_negative(field) && v < 0) return why = "«" + name + "» не меньше 0", false;
    if (field == "opacity" && (v < 0 || v > 1)) return why = "«opacity» от 0 до 1", false;
    f64 old = field == "stroke_width" ? 1 : 0; // SVG's own width when none is written
    shape_field_number(s, field, old);
    if (same(v, old)) return out.assign(source), true;
    if (f.insert) {
        out = std::string(source.substr(0, f.at)) + ", { " + name + " = " + format_number(v) + " }" + std::string(source.substr(f.at));
        return true;
    }
    return rewrite_field_number(source, *f.table, name, old, v, out, why);
}

} // namespace forge::pixel
