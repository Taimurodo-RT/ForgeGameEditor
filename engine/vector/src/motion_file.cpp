#include "forge/vector/motion_file.h"

#include "forge/vector/trig.h"

#include "json_text.h"

#include <yyjson.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>

namespace forge::vector {

namespace {

using json::number;
using json::put_number;
using json::put_string;
using json::text;

constexpr const char* kWords[] = {"x",       "y",       "rot",  "scale",  "sx",           "sy",      "skew_x",    "skew_y",
                                  "opacity", "fill",    "stroke", "stroke_width", "visible", "morph", "dash", "draw_from",
                                  "draw_to", "tint",    "flash", "flash_color",  "mask",    "link", "drawing"};
static_assert(std::size(kWords) == kMotionPropCount);

constexpr f64 kMaxSkew = 89; // forge::pixel::kMaxSkew: past it a skew's tangent runs off to infinity

bool is_color(MotionProp p) { return p == MotionProp::Fill || p == MotionProp::Stroke || p == MotionProp::Tint || p == MotionProp::FlashColor; }
bool is_number(MotionProp p) {
    return static_cast<usize>(p) <= static_cast<usize>(MotionProp::Opacity) || p == MotionProp::Dash || p == MotionProp::DrawFrom ||
           p == MotionProp::DrawTo || p == MotionProp::Flash;
}

u8 mix8(u8 a, u8 b, f64 k) { return static_cast<u8>(std::clamp(std::round(a + (b - a) * k), 0.0, 255.0)); }

// A moment as the recording keeps it: to the nanosecond.
f64 snap_ns(f64 t) { return std::round(t * 1e9) / 1e9; }

bool close(f64 a, f64 b) { return std::abs(a - b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)}); }

bool close(const MotionMatrix& m, const MotionMatrix& n) {
    return close(m.a, n.a) && close(m.b, n.b) && close(m.c, n.c) && close(m.d, n.d) && close(m.e, n.e) && close(m.f, n.f);
}

constexpr const char* kOrderWords[] = {"top", "bottom", "above", "below"};

f64 curve_at(const std::vector<f64>& c, f64 s) {
    if (c.empty()) return s;
    const f64 x = std::clamp(s, 0.0, 1.0) * static_cast<f64>(c.size() - 1);
    const auto i = static_cast<usize>(x);
    if (i + 1 >= c.size()) return c.back();
    return c[i] + (c[i + 1] - c[i]) * (x - static_cast<f64>(i));
}

// --- writing ---------------------------------------------------------------------------------

std::string hex8(const MotionColor& c) {
    char buf[10];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return buf;
}

void put_value(std::string& out, MotionProp p, const MotionValue& v) {
    if (p == MotionProp::Visible) {
        out += v.v > 0 ? "true" : "false";
        return;
    }
    if (is_number(p)) {
        put_number(out, v.v);
        return;
    }
    if (p == MotionProp::Drawing) {
        put_string(out, v.target);
        return;
    }
    if (p == MotionProp::Link) {
        out += '[';
        put_string(out, v.target);
        const f64 m[] = {v.m.a, v.m.b, v.m.c, v.m.d, v.m.e, v.m.f};
        for (const f64 x : m) {
            out += ", ";
            put_number(out, x);
        }
        out += ']';
        return;
    }
    out += '[';
    put_number(out, v.v);
    out += ", ";
    if (is_color(p)) put_string(out, hex8(v.color));
    else if (p == MotionProp::StrokeWidth) put_number(out, v.n);
    else put_string(out, v.target);
    out += ']';
}

void put_matrix(std::string& out, const MotionMatrix& m) {
    const f64 v[] = {m.a, m.b, m.c, m.d, m.e, m.f};
    out += '[';
    for (usize i = 0; i < 6; ++i) {
        if (i) out += ", ";
        put_number(out, v[i]);
    }
    out += ']';
}

// {"id": {"pivot": …, "frame": …, "own": …}, …}, a part a line, at `indent` and two more.
void put_parts(std::string& out, const std::vector<MotionPart>& parts, const std::string& indent) {
    out += '{';
    for (usize i = 0; i < parts.size(); ++i) {
        const MotionPart& p = parts[i];
        out += (i ? ",\n  " : "\n  ") + indent;
        put_string(out, p.id);
        out += ": {\"pivot\": [";
        put_number(out, p.pivot.x);
        out += ", ";
        put_number(out, p.pivot.y);
        out += "], \"frame\": ";
        put_matrix(out, p.frame);
        out += ", \"own\": ";
        put_matrix(out, p.own);
        out += '}';
    }
    out += parts.empty() ? "}" : "\n" + indent + "}";
}

// A change per unit of a parameter (part 5ж): a number (of a number, a share), [share, width] of a stroke width, the six
// numbers of a link's matrix.
void put_delta(std::string& out, MotionProp p, const MotionDelta& d) {
    if (p == MotionProp::StrokeWidth) {
        out += '[';
        put_number(out, d.v);
        out += ", ";
        put_number(out, d.n);
        out += ']';
        return;
    }
    if (p == MotionProp::Link) {
        out += '[';
        for (usize i = 0; i < 6; ++i) {
            if (i) out += ", ";
            put_number(out, d.m[i]);
        }
        out += ']';
        return;
    }
    put_number(out, d.v);
}

// `, "key": {"height": …}` of the parameters that change `field` of a piece; nothing when none does.
void put_by(std::string& out, const char* key, MotionProp p, const std::vector<MotionBy>& by, MotionDelta MotionBy::*field) {
    bool any = false;
    for (const MotionBy& b : by) {
        if ((b.*field).zero()) continue;
        out += any ? ", " : std::string(", \"") + key + "\": {";
        any = true;
        put_string(out, b.param);
        out += ": ";
        put_delta(out, p, b.*field);
    }
    if (any) out += '}';
}

void put_piece(std::string& out, const MotionFile& f, MotionProp p, const MotionPiece& pc) {
    out += "{\"at\": ";
    put_number(out, pc.at);
    switch (pc.kind) {
    case MotionPiece::Kind::Hold:
        out += ", \"hold\": ";
        put_value(out, p, pc.value);
        break;
    case MotionPiece::Kind::Ease:
        out += ", \"length\": ";
        put_number(out, pc.length);
        out += ", \"from\": ";
        put_value(out, p, pc.from);
        out += ", \"to\": ";
        put_value(out, p, pc.to);
        if (pc.has_bezier) {
            out += ", \"bezier\": [";
            put_number(out, pc.bezier.x1);
            out += ", ";
            put_number(out, pc.bezier.y1);
            out += ", ";
            put_number(out, pc.bezier.x2);
            out += ", ";
            put_number(out, pc.bezier.y2);
            out += ']';
        } else if (pc.curve >= 0 && static_cast<usize>(pc.curve) < f.curves.size()) {
            out += ", \"curve\": " + std::to_string(pc.curve);
        } else {
            out += ", \"ease\": ";
            put_string(out, timing_word(pc.timing));
        }
        break;
    case MotionPiece::Kind::Frames:
        out += ", \"end\": ";
        put_number(out, pc.end);
        out += ", \"frames\": [";
        for (usize i = 0; i < pc.frames.size(); ++i) {
            if (i) out += ", ";
            put_value(out, p, pc.frames[i]);
        }
        out += ']';
        break;
    }
    // How its numbers change with the parameters (part 5ж): those that change, each in the piece's own shape.
    if (pc.kind == MotionPiece::Kind::Hold) put_by(out, "by", p, pc.by, &MotionBy::value);
    if (pc.kind == MotionPiece::Kind::Ease) {
        put_by(out, "by_from", p, pc.by, &MotionBy::from);
        put_by(out, "by_to", p, pc.by, &MotionBy::to);
    }
    if (pc.kind == MotionPiece::Kind::Frames) {
        bool any = false;
        for (const MotionBy& b : pc.by) {
            if (std::all_of(b.frames.begin(), b.frames.end(), [](const MotionDelta& d) { return d.zero(); })) continue;
            out += any ? ", " : ", \"by\": {";
            any = true;
            put_string(out, b.param);
            out += ": [";
            for (usize i = 0; i < b.frames.size(); ++i) {
                if (i) out += ", ";
                put_delta(out, p, b.frames[i]);
            }
            out += ']';
        }
        if (any) out += '}';
    }
    out += '}';
}

// --- reading ---------------------------------------------------------------------------------

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "#rrggbbaa", or "#rrggbb" with a full alpha.
bool parse_hex(std::string_view s, MotionColor& out) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
    u8 c[4] = {0, 0, 0, 255};
    for (usize i = 0; i + 1 < s.size(); i += 2) {
        const int hi = hex_digit(s[i + 1]), lo = i + 2 < s.size() ? hex_digit(s[i + 2]) : -1;
        if (hi < 0 || lo < 0) return false;
        c[i / 2] = static_cast<u8>(hi * 16 + lo);
    }
    out = {c[0], c[1], c[2], c[3]};
    return true;
}

bool matrix(yyjson_val* v, MotionMatrix& out) {
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 6) return false;
    f64 m[6];
    for (usize i = 0; i < 6; ++i)
        if (!number(yyjson_arr_get(v, i), m[i])) return false;
    out = {m[0], m[1], m[2], m[3], m[4], m[5]};
    return true;
}

const char* value_shape(MotionProp p) {
    if (p == MotionProp::Visible) return "true или false";
    if (is_number(p)) return "число";
    if (is_color(p)) return "[доля, \"#rrggbbaa\"]";
    if (p == MotionProp::StrokeWidth) return "[доля, толщина]";
    if (p == MotionProp::Mask) return "[0, \"\"] (без маски), [1, \"id части\"] (внутри) или [2, \"id части\"] (снаружи)";
    if (p == MotionProp::Link) return "[\"id части\" (или \"\", отпущена), a, b, c, d, e, f]";
    if (p == MotionProp::Drawing) return "имя рисунка строкой (\"draw\" — главный)";
    return "[доля, \"id фигуры\"]";
}

bool value(yyjson_val* v, MotionProp p, MotionValue& out) {
    out = {};
    if (p == MotionProp::Visible) {
        if (!yyjson_is_bool(v)) return false;
        out.v = yyjson_get_bool(v) ? 1 : 0;
        return true;
    }
    if (is_number(p)) return number(v, out.v);
    if (p == MotionProp::Drawing) return text(v, out.target) && !out.target.empty();
    if (p == MotionProp::Link) {
        if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 7 || !text(yyjson_arr_get(v, 0), out.target)) return false;
        f64 m[6];
        for (usize i = 0; i < 6; ++i)
            if (!number(yyjson_arr_get(v, i + 1), m[i])) return false;
        out.m = {m[0], m[1], m[2], m[3], m[4], m[5]};
        return true;
    }
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 2 || !number(yyjson_arr_get(v, 0), out.v)) return false;
    yyjson_val* b = yyjson_arr_get(v, 1);
    if (is_color(p)) {
        std::string s;
        return text(b, s) && parse_hex(s, out.color);
    }
    if (p == MotionProp::StrokeWidth) return number(b, out.n);
    if (!text(b, out.target)) return false;
    if (p == MotionProp::Mask) return (out.v == 0 && out.target.empty()) || ((out.v == 1 || out.v == 2) && !out.target.empty());
    return true;
}

struct Reader {
    MotionFile& f;
    std::string& why;

    bool fail(const std::string& where, const std::string& what) {
        why = where + ": " + what;
        return false;
    }

    // A part with a record in some drawing's parts, the main one's or another's.
    bool any_part(std::string_view id) const {
        if (f.part(id)) return true;
        return std::any_of(f.drawings.begin(), f.drawings.end(), [&](const MotionDrawing& d) {
            return std::any_of(d.parts.begin(), d.parts.end(), [&](const MotionPart& p) { return p.id == id; });
        });
    }
    bool known_drawing(const std::string& name) const { return name == kMotionMainDrawing || f.drawing(name); }

    bool parts(yyjson_val* v, const std::string& where, std::vector<MotionPart>& out) {
        if (!yyjson_is_obj(v)) return fail(where, "нужен объект {\"id\": {\"pivot\": …}}");
        usize i, n;
        yyjson_val *key, *pv_;
        yyjson_obj_foreach(v, i, n, key, pv_) {
            MotionPart p;
            p.id.assign(yyjson_get_str(key), yyjson_get_len(key));
            const std::string at = where + "." + p.id;
            if (std::any_of(out.begin(), out.end(), [&](const MotionPart& o) { return o.id == p.id; })) return fail(at, "часть дважды");
            if (!yyjson_is_obj(pv_)) return fail(at, "нужен объект {\"pivot\": [x, y], \"frame\": […], \"own\": […]}");
            yyjson_val* pv = yyjson_obj_get(pv_, "pivot");
            if (!yyjson_is_arr(pv) || yyjson_arr_size(pv) != 2 || !number(yyjson_arr_get(pv, 0), p.pivot.x) || !number(yyjson_arr_get(pv, 1), p.pivot.y))
                return fail(at, "pivot: нужна опорная точка [x, y]");
            if (!matrix(yyjson_obj_get(pv_, "frame"), p.frame)) return fail(at, "frame: нужна матрица [a, b, c, d, e, f] — где стоит родитель части");
            if (!matrix(yyjson_obj_get(pv_, "own"), p.own)) return fail(at, "own: нужна матрица [a, b, c, d, e, f] — свой transform части");
            out.push_back(std::move(p));
        }
        return true;
    }

    bool piece(yyjson_val* v, const std::string& where, MotionProp p, const MotionClip& c, MotionPiece& out) {
        return piece_values(v, where, p, out) && piece_by(v, where, p, c, out);
    }

    std::string param_names() const {
        if (f.params.empty()) return "в params ни одного";
        std::string out = "есть";
        for (usize i = 0; i < f.params.size(); ++i) out += (i ? ", «" : " «") + f.params[i].name + "»";
        return out;
    }

    // A change per unit of a parameter in property p's shape (part 5ж).
    static bool delta(yyjson_val* v, MotionProp p, MotionDelta& out) {
        out = {};
        if (p == MotionProp::StrokeWidth)
            return yyjson_is_arr(v) && yyjson_arr_size(v) == 2 && number(yyjson_arr_get(v, 0), out.v) && number(yyjson_arr_get(v, 1), out.n);
        if (p == MotionProp::Link) {
            if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 6) return false;
            for (usize i = 0; i < 6; ++i)
                if (!number(yyjson_arr_get(v, i), out.m[i])) return false;
            return true;
        }
        return number(v, out.v);
    }
    static const char* delta_shape(MotionProp p) {
        if (p == MotionProp::StrokeWidth) return "[доля, толщина]";
        if (p == MotionProp::Link) return "шесть чисел a, b, c, d, e, f";
        return "число";
    }

    // How the piece's numbers change with the parameters (part 5ж): "by" of a hold and of frames, "by_from" and "by_to" of
    // a transition, each {"параметр": change per unit}; merged a parameter an entry, in the order of params.
    bool piece_by(yyjson_val* v, const std::string& where, MotionProp p, const MotionClip& c, MotionPiece& out) {
        const bool ease = out.kind == MotionPiece::Kind::Ease;
        for (const char* key : {"by", "by_from", "by_to"}) {
            yyjson_val* obj = yyjson_obj_get(v, key);
            if (!obj) continue;
            const std::string at = where + "." + key;
            const bool plain = std::string_view(key) == "by";
            if (ease == plain) return fail(at, ease ? "у перехода by_from и by_to" : "by_from и by_to только у перехода, у этого куска by");
            if (!motion_prop_has_numbers(p)) return fail(at, std::string("у ") + motion_prop_word(p) + " нет чисел, которые менял бы параметр");
            if (!yyjson_is_obj(obj)) return fail(at, "нужен объект {\"параметр\": изменение на единицу}");
            std::vector<std::string> seen;
            usize i, n;
            yyjson_val *k, *d;
            yyjson_obj_foreach(obj, i, n, k, d) {
                const std::string name(yyjson_get_str(k), yyjson_get_len(k));
                const std::string here = at + "." + name;
                if (std::find(seen.begin(), seen.end(), name) != seen.end()) return fail(here, "параметр дважды");
                seen.push_back(name);
                if (!f.param(name)) return fail(here, "нет параметра «" + name + "»: " + param_names());
                if (c.fixed.count(name)) return fail(here, "параметр в fixed клипа: клип от него не меняется");
                auto b = std::find_if(out.by.begin(), out.by.end(), [&](const MotionBy& x) { return x.param == name; });
                if (b == out.by.end()) {
                    out.by.push_back({});
                    b = out.by.end() - 1;
                    b->param = name;
                }
                if (out.kind == MotionPiece::Kind::Frames) {
                    if (!yyjson_is_arr(d) || yyjson_arr_size(d) != out.frames.size())
                        return fail(here, "нужен список изменений, по одному на кадр (их " + std::to_string(out.frames.size()) + "), каждое " +
                                              delta_shape(p));
                    b->frames.resize(out.frames.size());
                    for (usize j = 0; j < out.frames.size(); ++j)
                        if (!delta(yyjson_arr_get(d, j), p, b->frames[j]))
                            return fail(here + "[" + std::to_string(j) + "]", std::string("нужно ") + delta_shape(p));
                } else {
                    MotionDelta& dd = plain ? b->value : std::string_view(key) == "by_from" ? b->from : b->to;
                    if (!delta(d, p, dd)) return fail(here, std::string("нужно ") + delta_shape(p));
                }
            }
        }
        // A parameter that changes nothing is as none, as the writer leaves it out.
        std::erase_if(out.by, [](const MotionBy& b) {
            return b.value.zero() && b.from.zero() && b.to.zero() && std::all_of(b.frames.begin(), b.frames.end(), [](const MotionDelta& d) { return d.zero(); });
        });
        const auto index = [&](const MotionBy& b) { return f.param(b.param) - f.params.data(); };
        std::sort(out.by.begin(), out.by.end(), [&](const MotionBy& a, const MotionBy& b) { return index(a) < index(b); });
        return true;
    }

    bool piece_values(yyjson_val* v, const std::string& where, MotionProp p, MotionPiece& out) {
        if (!yyjson_is_obj(v)) return fail(where, "нужен объект {\"at\": …}");
        out = {};
        if (!number(yyjson_obj_get(v, "at"), out.at)) return fail(where, "at: нужно число — где кусок начинается, в секундах");
        yyjson_val* hold = yyjson_obj_get(v, "hold");
        yyjson_val* frames = yyjson_obj_get(v, "frames");
        yyjson_val* length = yyjson_obj_get(v, "length");
        if ((hold != nullptr) + (frames != nullptr) + (length != nullptr) != 1)
            return fail(where, "нужно одно из hold (значение стоит), length (переход) или frames (кадры)");
        if (hold) {
            out.kind = MotionPiece::Kind::Hold;
            if (!value(hold, p, out.value)) return fail(where, std::string("hold: нужно ") + value_shape(p));
            return true;
        }
        if (frames) {
            out.kind = MotionPiece::Kind::Frames;
            if (!number(yyjson_obj_get(v, "end"), out.end) || out.end <= out.at)
                return fail(where, "end: нужно число больше at — где кончаются кадры");
            if (!yyjson_is_arr(frames)) return fail(where, std::string("frames: нужен список значений, каждое ") + value_shape(p));
            const usize want = motion_frame_count(out.at, out.end);
            if (yyjson_arr_size(frames) != want)
                return fail(where, "frames: " + std::to_string(yyjson_arr_size(frames)) + " значений, а от at до end их " + std::to_string(want) +
                                       " (120 в секунду и последнее перед end)");
            usize i, n;
            yyjson_val* item;
            yyjson_arr_foreach(frames, i, n, item) {
                MotionValue x;
                if (!value(item, p, x)) return fail(where, "frames[" + std::to_string(i) + "]: нужно " + value_shape(p));
                out.frames.push_back(std::move(x));
            }
            return true;
        }
        out.kind = MotionPiece::Kind::Ease;
        if (!number(length, out.length) || out.length < 0) return fail(where, "length: нужно число от 0 — длина перехода в секундах");
        if (!value(yyjson_obj_get(v, "from"), p, out.from)) return fail(where, std::string("from: нужно ") + value_shape(p));
        if (!value(yyjson_obj_get(v, "to"), p, out.to)) return fail(where, std::string("to: нужно ") + value_shape(p));
        yyjson_val* ease = yyjson_obj_get(v, "ease");
        yyjson_val* bez = yyjson_obj_get(v, "bezier");
        yyjson_val* curve = yyjson_obj_get(v, "curve");
        if ((ease != nullptr) + (bez != nullptr) + (curve != nullptr) != 1)
            return fail(where, "у перехода нужно одно сглаживание: ease (слово), bezier (четыре числа) или curve (номер в curves)");
        if (ease) {
            std::string w;
            if (!text(ease, w) || !timing_from_word(w, out.timing)) return fail(where, "ease: нет такого сглаживания «" + w + "»");
        } else if (bez) {
            f64 b[4];
            if (!yyjson_is_arr(bez) || yyjson_arr_size(bez) != 4 || !number(yyjson_arr_get(bez, 0), b[0]) || !number(yyjson_arr_get(bez, 1), b[1]) ||
                !number(yyjson_arr_get(bez, 2), b[2]) || !number(yyjson_arr_get(bez, 3), b[3]))
                return fail(where, "bezier: нужны четыре числа x1, y1, x2, y2");
            out.has_bezier = true;
            out.bezier = {b[0], b[1], b[2], b[3]};
            if (!bezier_valid(out.bezier)) return fail(where, "bezier: x1 и x2 — от 0 до 1");
        } else {
            if (!yyjson_is_uint(curve) || yyjson_get_uint(curve) >= f.curves.size())
                return fail(where, "curve: нужен номер сглаживания в curves, их " + std::to_string(f.curves.size()));
            out.curve = static_cast<i32>(yyjson_get_uint(curve));
        }
        return true;
    }

    std::string drawing_names() const {
        std::string out;
        for (const MotionDrawing& d : f.drawings) out += ", " + d.name;
        return out;
    }

    bool track(yyjson_val* v, const std::string& where, const MotionClip& c, MotionTrack& out) {
        if (!yyjson_is_obj(v)) return fail(where, "нужен объект {\"part\": …, \"prop\": …, \"pieces\": […]}");
        out = {};
        if (!text(yyjson_obj_get(v, "part"), out.part)) return fail(where, "part: нужен id части строкой");
        std::string w;
        if (!text(yyjson_obj_get(v, "prop"), w) || !motion_prop_from_word(w, out.prop))
            return fail(where, "prop: нет такого свойства «" + w +
                                   "»: x, y, rot, scale, sx, sy, skew_x, skew_y, opacity, fill, stroke, stroke_width, visible, morph, dash, draw_from, "
                                   "draw_to, tint, flash, flash_color, mask, link, drawing");
        if (out.prop == MotionProp::Drawing && !out.part.empty()) return fail(where, "part: у дорожки рисунка часть \"\" — это рисунок всего персонажа");
        if (out.prop != MotionProp::Drawing && out.part.empty()) return fail(where, "part: нужен id части строкой");
        if ((motion_prop_moves(out.prop) || out.prop == MotionProp::Link) && !any_part(out.part))
            return fail(where, "части «" + out.part + "» нет в parts, а её " + w + " без опоры и рамки не посчитать");
        yyjson_val* pieces = yyjson_obj_get(v, "pieces");
        if (!yyjson_is_arr(pieces) || yyjson_arr_size(pieces) == 0) return fail(where, "pieces: нужен непустой список кусков");
        usize i, n;
        yyjson_val* item;
        yyjson_arr_foreach(pieces, i, n, item) {
            MotionPiece pc;
            const std::string at = where + ".pieces[" + std::to_string(i) + "]";
            if (!piece(item, at, out.prop, c, pc)) return false;
            if (motion_prop_held(out.prop) && pc.kind != MotionPiece::Kind::Hold)
                return fail(at, std::string("у ") + w + " только hold: она меняется вызовом в миг, а не переходом");
            if (out.prop == MotionProp::Drawing) {
                if (pc.kind == MotionPiece::Kind::Frames) return fail(at, "у дорожки рисунка только hold и переход");
                const std::vector<const std::string*> names = pc.kind == MotionPiece::Kind::Hold
                                                                  ? std::vector<const std::string*>{&pc.value.target}
                                                                  : std::vector<const std::string*>{&pc.from.target, &pc.to.target};
                for (const std::string* name : names)
                    if (!known_drawing(*name)) return fail(at, "нет рисунка «" + *name + "»: есть draw" + drawing_names());
            }
            if (i == 0 && pc.at != 0) return fail(at, "первый кусок — с 0");
            if (i > 0 && pc.at <= out.pieces.back().at) return fail(at, "куски не по времени: at не больше, чем у куска перед ним");
            if (pc.at > c.length) return fail(at, "at за концом клипа");
            if (i > 0 && out.pieces.back().kind == MotionPiece::Kind::Frames && out.pieces.back().end != pc.at)
                return fail(where + ".pieces[" + std::to_string(i - 1) + "]", "end кадров — at следующего куска");
            out.pieces.push_back(std::move(pc));
        }
        if (out.pieces.back().kind == MotionPiece::Kind::Frames && out.pieces.back().end != c.length)
            return fail(where + ".pieces[" + std::to_string(out.pieces.size() - 1) + "]", "end последних кадров — длина клипа");
        return true;
    }

    bool clip(yyjson_val* v, const std::string& where, MotionClip& out) {
        if (!yyjson_is_obj(v)) return fail(where, "нужен объект {\"length\": …, \"tracks\": […]}");
        if (!number(yyjson_obj_get(v, "length"), out.length) || out.length < 0) return fail(where, "length: нужно число от 0 — длина клипа в секундах");
        if (yyjson_val* l = yyjson_obj_get(v, "loop")) {
            if (!yyjson_is_bool(l)) return fail(where, "loop: нужно true или false");
            out.loop = yyjson_get_bool(l);
        }
        // The parameters the clip does not change with (part 5ж), before the tracks: a piece does not change with them.
        if (yyjson_val* fx = yyjson_obj_get(v, "fixed")) {
            if (!yyjson_is_obj(fx)) return fail(where, "fixed: нужен объект {\"параметр\": почему}");
            usize i, n;
            yyjson_val *k, *w;
            yyjson_obj_foreach(fx, i, n, k, w) {
                const std::string name(yyjson_get_str(k), yyjson_get_len(k));
                const std::string at = where + ".fixed." + name;
                if (!f.param(name)) return fail(at, "нет параметра «" + name + "»: " + param_names());
                std::string reason;
                if (!text(w, reason) || reason.empty()) return fail(at, "нужно, почему, строкой");
                if (!out.fixed.emplace(name, std::move(reason)).second) return fail(at, "параметр дважды");
            }
        }
        yyjson_val* tracks = yyjson_obj_get(v, "tracks");
        if (!yyjson_is_arr(tracks)) return fail(where, "tracks: нужен список дорожек");
        usize i, n;
        yyjson_val* item;
        yyjson_arr_foreach(tracks, i, n, item) {
            MotionTrack t;
            const std::string at = where + ".tracks[" + std::to_string(i) + "]";
            if (!track(item, at, out, t)) return false;
            for (const MotionTrack& o : out.tracks)
                if (o.part == t.part && o.prop == t.prop) return fail(at, "у части «" + t.part + "» уже есть дорожка " + motion_prop_word(t.prop));
            out.tracks.push_back(std::move(t));
        }
        if (yyjson_val* marks = yyjson_obj_get(v, "marks")) {
            if (!yyjson_is_arr(marks)) return fail(where, "marks: нужен список меток");
            yyjson_arr_foreach(marks, i, n, item) {
                const std::string at = where + ".marks[" + std::to_string(i) + "]";
                if (!yyjson_is_obj(item)) return fail(at, "нужен объект {\"sound\": имя, \"at\": …} или {\"emit\": имя, \"at\": …}");
                MotionMark m;
                yyjson_val* sound = yyjson_obj_get(item, "sound");
                yyjson_val* emit = yyjson_obj_get(item, "emit");
                if ((sound != nullptr) == (emit != nullptr) || !text(sound ? sound : emit, m.name) || m.name.empty())
                    return fail(at, "нужно одно имя: sound (звук) или emit (сообщение)");
                m.sound = sound != nullptr;
                if (!number(yyjson_obj_get(item, "at"), m.at) || m.at < 0 || m.at > out.length)
                    return fail(at, "at: нужен миг метки в клипе, от 0 до его длины");
                if (yyjson_val* l = yyjson_obj_get(item, "length"))
                    if (!number(l, m.length) || m.length < 0) return fail(at, "length: нужно число от 0 — окно метки");
                if (yyjson_val* o = yyjson_obj_get(item, "order")) {
                    if (!yyjson_is_uint(o) || yyjson_get_uint(o) > 0xffffffffu) return fail(at, "order: нужен номер метки среди вызовов сцены, целое от 0");
                    m.order = static_cast<u32>(yyjson_get_uint(o));
                }
                if (!out.marks.empty() && m.at < out.marks.back().at) return fail(at, "метки не по времени");
                out.marks.push_back(std::move(m));
            }
        }
        if (yyjson_val* copies = yyjson_obj_get(v, "copies")) {
            if (!yyjson_is_arr(copies)) return fail(where, "copies: нужен список копий");
            yyjson_arr_foreach(copies, i, n, item) {
                const std::string at = where + ".copies[" + std::to_string(i) + "]";
                if (!yyjson_is_obj(item)) return fail(at, "нужен объект {\"id\": …, \"source\": …, \"number\": …, \"born\": …}");
                MotionCopy c;
                if (!text(yyjson_obj_get(item, "id"), c.id) || c.id.empty()) return fail(at, "id: нужен id копии строкой");
                if (!text(yyjson_obj_get(item, "source"), c.source) || c.source.empty()) return fail(at, "source: нужна часть, копия которой это");
                if (c.source == c.id) return fail(at, "копия не может зваться, как её часть");
                yyjson_val* num = yyjson_obj_get(item, "number");
                if (!yyjson_is_uint(num) || yyjson_get_uint(num) == 0 || yyjson_get_uint(num) > 1u << 20)
                    return fail(at, "number: нужен номер копии у её части: 1, 2, …");
                c.number = static_cast<u32>(yyjson_get_uint(num));
                if (!number(yyjson_obj_get(item, "born"), c.born) || c.born < 0 || c.born > out.length)
                    return fail(at, "born: нужен миг рождения в клипе, от 0 до его длины");
                if (!out.copies.empty() && c.born < out.copies.back().born) return fail(at, "копии не по времени рождения");
                if (!any_part(c.id)) return fail(at, "копии «" + c.id + "» нет в parts: нужны её опорная точка и рамка (у неё они части «" + c.source + "»)");
                for (const MotionCopy& o : out.copies) {
                    if (o.id == c.id) return fail(at, "копия «" + c.id + "» дважды");
                    if (o.source == c.source && o.number == c.number) return fail(at, "у части «" + c.source + "» уже есть копия " + std::to_string(c.number));
                    if (o.id == c.source || o.source == c.id) return fail(at, "копия копии: «" + c.id + "» и «" + o.id + "»");
                }
                out.copies.push_back(std::move(c));
            }
            // Each part's copies are numbered 1, 2, … without a gap.
            std::map<std::string, u32, std::less<>> count;
            for (const MotionCopy& c : out.copies) ++count[c.source];
            for (usize k = 0; k < out.copies.size(); ++k)
                if (out.copies[k].number > count[out.copies[k].source])
                    return fail(where + ".copies[" + std::to_string(k) + "]", "number: у части «" + out.copies[k].source + "» копий " +
                                                                                 std::to_string(count[out.copies[k].source]) + ", номера — 1, 2, … без пропусков");
        }
        if (yyjson_val* order = yyjson_obj_get(v, "order")) {
            if (!yyjson_is_arr(order)) return fail(where, "order: нужен список шагов порядка");
            yyjson_arr_foreach(order, i, n, item) {
                const std::string at = where + ".order[" + std::to_string(i) + "]";
                if (!yyjson_is_obj(item)) return fail(at, "нужен объект {\"at\": …, \"part\": …, \"kind\": …}");
                MotionOrderStep st;
                if (!number(yyjson_obj_get(item, "at"), st.at) || st.at < 0 || st.at > out.length)
                    return fail(at, "at: нужен миг шага в клипе, от 0 до его длины");
                if (!out.order.empty() && st.at < out.order.back().at) return fail(at, "шаги порядка не по времени");
                if (!text(yyjson_obj_get(item, "part"), st.part) || st.part.empty()) return fail(at, "part: нужен id части строкой");
                std::string kind;
                text(yyjson_obj_get(item, "kind"), kind);
                usize k = 0;
                while (k < std::size(kOrderWords) && kind != kOrderWords[k]) ++k;
                if (k == std::size(kOrderWords)) return fail(at, "kind: нет такого шага «" + kind + "»: top, bottom, above или below");
                st.kind = static_cast<MotionOrder>(k);
                yyjson_val* other = yyjson_obj_get(item, "other");
                if (st.kind == MotionOrder::Above || st.kind == MotionOrder::Below) {
                    if (!text(other, st.other) || st.other.empty()) return fail(at, "other: у " + kind + " нужен сосед, id части строкой");
                } else if (other) {
                    return fail(at, "other: у " + kind + " соседа нет");
                }
                out.order.push_back(std::move(st));
            }
        }
        if (yyjson_val* nd = yyjson_obj_get(v, "not_drawn")) {
            if (!yyjson_is_arr(nd)) return fail(where, "not_drawn: нужен список слов");
            yyjson_arr_foreach(nd, i, n, item) {
                std::string w;
                if (!text(item, w)) return fail(where + ".not_drawn[" + std::to_string(i) + "]", "нужно слово строкой");
                out.not_drawn.push_back(std::move(w));
            }
        }
        return true;
    }

    bool root(yyjson_val* r) {
        if (!yyjson_is_obj(r)) return why = "в файле не объект JSON", false;
        std::string kind;
        if (!text(yyjson_obj_get(r, "forge"), kind) || kind != "motion") return why = "forge: это не файл клипов экрана (нужно \"motion\")", false;
        yyjson_val* ver = yyjson_obj_get(r, "version");
        if (!yyjson_is_uint(ver) || yyjson_get_uint(ver) == 0) return why = "version: нужно целое число от 1", false;
        if (yyjson_get_uint(ver) > kMotionFileVersion)
            return why = "version " + std::to_string(yyjson_get_uint(ver)) + ": файл новее этой игры (она читает " + std::to_string(kMotionFileVersion) + ")", false;
        if (yyjson_val* v = yyjson_obj_get(r, "character"); v && !text(v, f.character)) return why = "character: нужно имя строкой", false;
        if (yyjson_val* v = yyjson_obj_get(r, "from"); v && !text(v, f.from)) return why = "from: нужен файл персонажа строкой", false;
        if (!text(yyjson_obj_get(r, "picture"), f.picture) || f.picture.empty()) return why = "picture: нужно имя SVG-картинки строкой", false;
        if (yyjson_val* ps = yyjson_obj_get(r, "params")) {
            if (!yyjson_is_arr(ps)) return why = "params: нужен список параметров [{\"name\": …, \"value\": …}]", false;
            usize i, n;
            yyjson_val* item;
            yyjson_arr_foreach(ps, i, n, item) {
                const std::string at = "params[" + std::to_string(i) + "]";
                MotionParam prm;
                if (!yyjson_is_obj(item)) return fail(at, "нужен объект {\"name\": …, \"value\": …}");
                if (!text(yyjson_obj_get(item, "name"), prm.name) || prm.name.empty()) return fail(at, "name: нужно имя параметра строкой");
                if (!number(yyjson_obj_get(item, "value"), prm.value)) return fail(at, "value: нужно число — с ним вкладка отдала клипы");
                if (f.param(prm.name)) return fail(at, "параметр «" + prm.name + "» дважды");
                f.params.push_back(std::move(prm));
            }
        }
        if (yyjson_val* ps = yyjson_obj_get(r, "parts"))
            if (!parts(ps, "parts", f.parts)) return false;
        if (yyjson_val* ds = yyjson_obj_get(r, "drawings")) {
            if (!yyjson_is_obj(ds)) return why = "drawings: нужен объект {\"рисунок\": {\"picture\": …, \"parts\": {…}}}", false;
            usize i, n;
            yyjson_val *key, *v;
            yyjson_obj_foreach(ds, i, n, key, v) {
                MotionDrawing d;
                d.name.assign(yyjson_get_str(key), yyjson_get_len(key));
                const std::string where = "drawings." + d.name;
                if (d.name.empty()) return fail(where, "у рисунка нужно имя");
                if (d.name == kMotionMainDrawing) return fail(where, "так зовут главный рисунок");
                if (f.drawing(d.name)) return fail(where, "рисунок дважды");
                if (!yyjson_is_obj(v)) return fail(where, "нужен объект {\"picture\": …, \"parts\": {…}}");
                if (!text(yyjson_obj_get(v, "picture"), d.picture) || d.picture.empty()) return fail(where, "picture: нужно имя SVG-картинки рисунка строкой");
                // Beside the picture: a game reads it from the picture's folder, never another.
                if (d.picture.find_first_of("/\\") != std::string::npos || d.picture == "." || d.picture == "..")
                    return fail(where, "picture: имя файла рядом с картинкой, без папок");
                if (yyjson_val* ps = yyjson_obj_get(v, "parts"))
                    if (!parts(ps, where + ".parts", d.parts)) return false;
                f.drawings.push_back(std::move(d));
            }
            // By name, as the tab keeps them: a morph's target is looked for in them in this order.
            std::sort(f.drawings.begin(), f.drawings.end(), [](const MotionDrawing& a, const MotionDrawing& b) { return a.name < b.name; });
        }
        if (yyjson_val* curves = yyjson_obj_get(r, "curves")) {
            if (!yyjson_is_arr(curves)) return why = "curves: нужен список сглаживаний", false;
            usize i, n;
            yyjson_val* c;
            yyjson_arr_foreach(curves, i, n, c) {
                const std::string where = "curves[" + std::to_string(i) + "]";
                if (!yyjson_is_arr(c) || yyjson_arr_size(c) != kMotionCurvePoints)
                    return fail(where, "нужно " + std::to_string(kMotionCurvePoints) + " чисел");
                std::vector<f64> pts(kMotionCurvePoints);
                for (usize k = 0; k < kMotionCurvePoints; ++k)
                    if (!number(yyjson_arr_get(c, k), pts[k])) return fail(where + "[" + std::to_string(k) + "]", "нужно число");
                f.curves.push_back(std::move(pts));
            }
        }
        yyjson_val* clips = yyjson_obj_get(r, "clips");
        if (!yyjson_is_obj(clips) || yyjson_obj_size(clips) == 0) return why = "clips: нужен объект {\"клип\": {…}} хотя бы с одним клипом", false;
        usize i, n;
        yyjson_val *key, *v;
        yyjson_obj_foreach(clips, i, n, key, v) {
            MotionClip c;
            c.name.assign(yyjson_get_str(key), yyjson_get_len(key));
            if (f.clip(c.name)) return fail("clips." + c.name, "клип дважды");
            if (!clip(v, "clips." + c.name, c)) return false;
            f.clips.push_back(std::move(c));
        }
        return true;
    }
};

} // namespace

const char* motion_prop_word(MotionProp p) {
    const auto i = static_cast<usize>(p);
    return i < kMotionPropCount ? kWords[i] : kWords[0];
}

bool motion_prop_from_word(std::string_view word, MotionProp& out) {
    for (usize i = 0; i < kMotionPropCount; ++i)
        if (word == kWords[i]) {
            out = static_cast<MotionProp>(i);
            return true;
        }
    return false;
}

const char* motion_not_drawn_words(std::string_view word) {
    static constexpr const char* kSaid[] = {"пунктир", "прорисовка", "тон", "вспышка", "размытие", "тень", "маска", "крепление", "копии",
                                            "порядок частей", "смена рисунка", "морфинг в фигуру другого рисунка"};
    static_assert(std::size(kSaid) == std::size(kMotionNotDrawn));
    for (usize i = 0; i < std::size(kMotionNotDrawn); ++i)
        if (word == kMotionNotDrawn[i]) return kSaid[i];
    return nullptr;
}

bool motion_prop_moves(MotionProp p) { return static_cast<usize>(p) <= static_cast<usize>(MotionProp::SkewY); }

bool motion_prop_held(MotionProp p) { return p == MotionProp::Mask || p == MotionProp::Link; }

bool motion_prop_new(MotionProp p) { return static_cast<usize>(p) >= static_cast<usize>(MotionProp::Dash); }

bool motion_prop_has_numbers(MotionProp p) { return p != MotionProp::Visible && p != MotionProp::Mask && p != MotionProp::Drawing; }

const char* motion_order_word(MotionOrder k) {
    const auto i = static_cast<usize>(k);
    return i < std::size(kOrderWords) ? kOrderWords[i] : kOrderWords[0];
}

MotionValue motion_rest(MotionProp p) {
    MotionValue v;
    v.v = (p == MotionProp::Scale || p == MotionProp::Sx || p == MotionProp::Sy || p == MotionProp::Opacity || p == MotionProp::DrawTo ||
           p == MotionProp::Visible)
              ? 1
              : 0;
    if (p == MotionProp::Drawing) v.target = kMotionMainDrawing;
    return v;
}

MotionValue motion_mix(MotionProp p, const MotionValue& a, const MotionValue& b, f64 k) {
    if (k == 0) return a;
    if (k == 1) return b;
    MotionValue r;
    if (is_number(p)) {
        r.v = a.v + (b.v - a.v) * k; // past 0 … 1 too: back and elastic overshoot
        return r;
    }
    if (p == MotionProp::Visible || motion_prop_held(p) || p == MotionProp::Drawing) return k >= 1 ? b : a; // no values between
    if (p == MotionProp::StrokeWidth && a.v == 1 && b.v == 1) {
        r.v = 1;
        r.n = a.n + (b.n - a.n) * k; // two widths given: as numbers, past them too
        return r;
    }
    k = std::clamp(k, 0.0, 1.0);
    if (k == 0) return a;
    if (k == 1) return b;
    if (a.v <= 0 && b.v <= 0) return a;
    if (p == MotionProp::StrokeWidth) {
        // Each shape's width is own · (1 − v) + n · v; mixed linearly for every own width at once.
        r.v = a.v + (b.v - a.v) * k;
        r.n = (std::max(a.v, 0.0) * a.n * (1 - k) + std::max(b.v, 0.0) * b.n * k) / r.v;
        return r;
    }
    if (is_color(p)) {
        if (a.v <= 0) {
            r.color = b.color;
            r.v = b.v * k;
        } else if (b.v <= 0) {
            r.color = a.color;
            r.v = a.v * (1 - k);
        } else {
            r.color = {mix8(a.color.r, b.color.r, k), mix8(a.color.g, b.color.g, k), mix8(a.color.b, b.color.b, k), mix8(a.color.a, b.color.a, k)};
            r.v = a.v + (b.v - a.v) * k;
        }
        return r;
    }
    if (a.v <= 0) {
        r.target = b.target;
        r.v = b.v * k;
    } else if (b.v <= 0) {
        r.target = a.target;
        r.v = a.v * (1 - k);
    } else if (a.target == b.target) {
        r.target = a.target;
        r.v = a.v + (b.v - a.v) * k;
    } else if (k < 0.5) {
        r.target = a.target;
        r.v = a.v * (1 - 2 * k);
    } else {
        r.target = b.target;
        r.v = b.v * (2 * k - 1);
    }
    return r;
}

MotionValue motion_plain(MotionProp p, const MotionValue& v) {
    MotionValue r;
    if (is_number(p) || p == MotionProp::Visible) {
        r.v = v.v;
        return r;
    }
    if (p == MotionProp::Link) {
        r.target = v.target;
        r.m = v.m;
        return r;
    }
    if (p == MotionProp::Drawing) {
        r.target = v.target;
        return r;
    }
    if (v.v <= 0) return motion_rest(p);
    r.v = v.v;
    if (is_color(p)) r.color = v.color;
    else if (p == MotionProp::StrokeWidth) r.n = v.n;
    else r.target = v.target;
    return r;
}

bool motion_same(MotionProp p, const MotionValue& a, const MotionValue& b) {
    const MotionValue x = motion_plain(p, a), y = motion_plain(p, b);
    return close(x.v, y.v) && x.color == y.color && x.target == y.target && close(x.n, y.n) && close(x.m, y.m);
}

bool MotionDelta::zero() const { return v == 0 && n == 0 && std::all_of(std::begin(m), std::end(m), [](f64 x) { return x == 0; }); }

MotionValue motion_moved(MotionValue v, const MotionDelta& d, f64 units) {
    v.v += d.v * units;
    v.n += d.n * units;
    v.m = {v.m.a + d.m[0] * units, v.m.b + d.m[1] * units, v.m.c + d.m[2] * units,
           v.m.d + d.m[3] * units, v.m.e + d.m[4] * units, v.m.f + d.m[5] * units};
    return v;
}

usize motion_frame_count(f64 at, f64 end) {
    if (!(end > at)) return 1;
    // The moments before `end` (one within a few nanoseconds of it is `end`), then `end` itself.
    const f64 m = (end - at) * kMotionRate;
    return static_cast<usize>(std::max(1.0, std::ceil(m - 1e-6))) + 1;
}

f64 motion_frame_moment(f64 at, usize i) {
    return static_cast<f64>(std::llround(at * 1e9) + std::llround(static_cast<f64>(i) * 1e9 / kMotionRate)) / 1e9;
}

MotionMatrix operator*(const MotionMatrix& m, const MotionMatrix& n) {
    return {m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d,
            m.b * n.c + m.d * n.d, m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f};
}

MotionMatrix inverse(const MotionMatrix& m) {
    const f64 det = m.a * m.d - m.b * m.c;
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return {};
    MotionMatrix r{m.d / det, -m.b / det, -m.c / det, m.a / det, 0, 0};
    r.e = -(r.a * m.e + r.c * m.f);
    r.f = -(r.b * m.e + r.d * m.f);
    return r;
}

const MotionClip* MotionFile::clip(std::string_view name) const {
    for (const MotionClip& c : clips)
        if (c.name == name) return &c;
    return nullptr;
}

const MotionPart* MotionFile::part(std::string_view id) const {
    for (const MotionPart& p : parts)
        if (p.id == id) return &p;
    return nullptr;
}

const MotionPart* MotionFile::part(std::string_view id, std::string_view name) const {
    if (name.empty() || name == kMotionMainDrawing) return part(id);
    const MotionDrawing* d = drawing(name);
    if (!d) return nullptr;
    for (const MotionPart& p : d->parts)
        if (p.id == id) return &p;
    return nullptr;
}

const MotionDrawing* MotionFile::drawing(std::string_view name) const {
    for (const MotionDrawing& d : drawings)
        if (d.name == name) return &d;
    return nullptr;
}

const MotionParam* MotionFile::param(std::string_view name) const {
    for (const MotionParam& p : params)
        if (p.name == name) return &p;
    return nullptr;
}

bool read_motion_file(std::string_view json, MotionFile& out, std::string& why) {
    out = {};
    why.clear();
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(json.data()), json.size(), 0, nullptr, &err);
    if (!doc) {
        why = "не JSON: " + std::string(err.msg ? err.msg : "") + " (байт " + std::to_string(err.pos) + ")";
        return false;
    }
    Reader r{out, why};
    const bool ok = r.root(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!ok) out = {};
    return ok;
}

u32 motion_file_version(const MotionFile& f) {
    if (!f.params.empty()) return 4;
    if (!f.drawings.empty()) return 3;
    for (const MotionClip& c : f.clips)
        for (const MotionTrack& t : c.tracks)
            if (t.prop == MotionProp::Drawing) return 3;
    for (const MotionClip& c : f.clips) {
        if (!c.copies.empty() || !c.order.empty()) return 2;
        for (const MotionTrack& t : c.tracks)
            if (motion_prop_new(t.prop)) return 2;
    }
    return 1;
}

std::string write_motion_file(const MotionFile& f) {
    std::string out = "{\n  \"forge\": \"motion\",\n  \"version\": " + std::to_string(motion_file_version(f)) + ",\n  \"character\": ";
    put_string(out, f.character);
    out += ",\n  \"from\": ";
    put_string(out, f.from);
    out += ",\n  \"picture\": ";
    put_string(out, f.picture);
    if (!f.params.empty()) {
        out += ",\n  \"params\": [";
        for (usize i = 0; i < f.params.size(); ++i) {
            out += i ? ", {\"name\": " : "{\"name\": ";
            put_string(out, f.params[i].name);
            out += ", \"value\": ";
            put_number(out, f.params[i].value);
            out += '}';
        }
        out += ']';
    }
    out += ",\n  \"parts\": ";
    put_parts(out, f.parts, "  ");
    out += ",\n";
    if (!f.drawings.empty()) {
        out += "  \"drawings\": {";
        for (usize i = 0; i < f.drawings.size(); ++i) {
            const MotionDrawing& d = f.drawings[i];
            out += i ? ",\n    " : "\n    ";
            put_string(out, d.name);
            out += ": {\"picture\": ";
            put_string(out, d.picture);
            out += ", \"parts\": ";
            put_parts(out, d.parts, "    ");
            out += '}';
        }
        out += "\n  },\n";
    }
    out += "  \"curves\": [";
    for (usize i = 0; i < f.curves.size(); ++i) {
        out += i ? ",\n    [" : "\n    [";
        for (usize k = 0; k < f.curves[i].size(); ++k) {
            if (k) out += ", ";
            put_number(out, f.curves[i][k]);
        }
        out += ']';
    }
    out += f.curves.empty() ? "],\n" : "\n  ],\n";
    out += "  \"clips\": {";
    for (usize i = 0; i < f.clips.size(); ++i) {
        const MotionClip& c = f.clips[i];
        out += i ? ",\n    " : "\n    ";
        put_string(out, c.name);
        out += ": {\n      \"length\": ";
        put_number(out, c.length);
        out += std::string(",\n      \"loop\": ") + (c.loop ? "true" : "false") + ",\n      \"tracks\": [";
        for (usize t = 0; t < c.tracks.size(); ++t) {
            const MotionTrack& tr = c.tracks[t];
            out += t ? ",\n        {\"part\": " : "\n        {\"part\": ";
            put_string(out, tr.part);
            out += ", \"prop\": ";
            put_string(out, motion_prop_word(tr.prop));
            out += ", \"pieces\": [";
            for (usize k = 0; k < tr.pieces.size(); ++k) {
                out += k ? ",\n          " : "\n          ";
                put_piece(out, f, tr.prop, tr.pieces[k]);
            }
            out += "]}";
        }
        out += c.tracks.empty() ? "],\n" : "\n      ],\n";
        out += "      \"marks\": [";
        for (usize m = 0; m < c.marks.size(); ++m) {
            const MotionMark& mk = c.marks[m];
            out += m ? ", {" : "{";
            out += mk.sound ? "\"sound\": " : "\"emit\": ";
            put_string(out, mk.name);
            out += ", \"at\": ";
            put_number(out, mk.at);
            if (mk.length > 0) {
                out += ", \"length\": ";
                put_number(out, mk.length);
            }
            if (mk.order > 0) out += ", \"order\": " + std::to_string(mk.order);
            out += '}';
        }
        out += "],\n      \"not_drawn\": [";
        for (usize w = 0; w < c.not_drawn.size(); ++w) {
            if (w) out += ", ";
            put_string(out, c.not_drawn[w]);
        }
        out += ']';
        if (!c.copies.empty()) {
            out += ",\n      \"copies\": [";
            for (usize k = 0; k < c.copies.size(); ++k) {
                const MotionCopy& cp = c.copies[k];
                out += k ? ",\n        {\"id\": " : "\n        {\"id\": ";
                put_string(out, cp.id);
                out += ", \"source\": ";
                put_string(out, cp.source);
                out += ", \"number\": " + std::to_string(cp.number) + ", \"born\": ";
                put_number(out, cp.born);
                out += '}';
            }
            out += "\n      ]";
        }
        if (!c.order.empty()) {
            out += ",\n      \"order\": [";
            for (usize k = 0; k < c.order.size(); ++k) {
                const MotionOrderStep& st = c.order[k];
                out += k ? ",\n        {\"at\": " : "\n        {\"at\": ";
                put_number(out, st.at);
                out += ", \"part\": ";
                put_string(out, st.part);
                out += ", \"kind\": ";
                put_string(out, motion_order_word(st.kind));
                if (st.kind == MotionOrder::Above || st.kind == MotionOrder::Below) {
                    out += ", \"other\": ";
                    put_string(out, st.other);
                }
                out += '}';
            }
            out += "\n      ]";
        }
        if (!c.fixed.empty()) {
            out += ",\n      \"fixed\": {";
            bool first = true;
            for (const auto& [name, why] : c.fixed) {
                if (!first) out += ", ";
                first = false;
                put_string(out, name);
                out += ": ";
                put_string(out, why);
            }
            out += '}';
        }
        out += "\n    }";
    }
    out += f.clips.empty() ? "}\n}\n" : "\n  }\n}\n";
    return out;
}

f64 motion_ease(const MotionFile& file, const MotionPiece& piece, f64 s) {
    if (piece.has_bezier) return bezier(piece.bezier, s);
    if (piece.curve >= 0 && static_cast<usize>(piece.curve) < file.curves.size()) return curve_at(file.curves[static_cast<usize>(piece.curve)], s);
    return timing(piece.timing, s);
}

MotionValue motion_piece_value(const MotionFile& file, MotionProp p, const MotionPiece& pc, f64 t) {
    switch (pc.kind) {
    case MotionPiece::Kind::Hold: return pc.value;
    case MotionPiece::Kind::Ease: {
        // As the recording reads a block (forge::pixel::block_value): on its nanoseconds, so its end is its end.
        const f64 local = snap_ns(snap_ns(t) - pc.at);
        if (pc.length <= 0) return local < 0 ? pc.from : pc.to;
        if (local <= 0) return pc.from;
        if (local >= pc.length) return pc.to;
        if (p == MotionProp::Visible) return pc.from; // at its end, whatever its easing does on the way
        return motion_mix(p, pc.from, pc.to, motion_ease(file, pc, std::min(local / pc.length, 1.0)));
    }
    case MotionPiece::Kind::Frames: {
        const usize n = pc.frames.size();
        if (n == 0) return motion_rest(p);
        if (n == 1 || t <= pc.at) return pc.frames.front();
        if (t >= pc.end) return pc.frames.back();
        // The value whose moment is t or the last before it, on the nanoseconds; the last one's is the moment before
        // `end`.
        const f64 last = static_cast<f64>(std::llround(pc.end * 1e9) - 1) / 1e9;
        if (t >= last) return pc.frames.back();
        const auto moment = [&](usize i) { return i + 1 >= n ? last : motion_frame_moment(pc.at, i); };
        auto i = static_cast<usize>(std::max(0.0, std::floor((t - pc.at) * kMotionRate)));
        i = std::min(i, n - 2);
        while (i > 0 && t < moment(i)) --i;
        while (i + 2 < n && t >= moment(i + 1)) ++i;
        const f64 t0 = moment(i), t1 = moment(i + 1);
        return motion_mix(p, pc.frames[i], pc.frames[i + 1], t1 > t0 ? std::clamp((t - t0) / (t1 - t0), 0.0, 1.0) : 0.0);
    }
    }
    return motion_rest(p);
}

MotionValue motion_track_value(const MotionFile& file, const MotionTrack& track, f64 t, f64 length) {
    if (track.pieces.empty()) return motion_rest(track.prop);
    // On its nanoseconds, as the recording is read: a game's moment a hair before a piece's start (frames added up) is at it.
    t = snap_ns(std::clamp(t, 0.0, std::max(length, 0.0)));
    auto it = std::upper_bound(track.pieces.begin(), track.pieces.end(), t, [](f64 x, const MotionPiece& p) { return x < p.at; });
    if (it != track.pieces.begin()) --it;
    return motion_piece_value(file, track.prop, *it, t);
}

MotionMatrix motion_matrix(f64 x, f64 y, f64 rot, f64 scale, f64 sx, f64 sy, f64 skew_x, f64 skew_y, Point pv) {
    f64 s = 0, c = 1;
    trig::sin_cos(rot * trig::kPi / 180.0, s, c);
    const f64 kx = scale * sx, ky = scale * sy;
    // T(x, y) · T(pivot) · R · S · Kx · Ky · T(−pivot), Kx and Ky SVG's skewX and skewY
    MotionMatrix rs{c * kx, s * kx, -s * ky, c * ky, 0, 0};
    if (skew_x != 0 || skew_y != 0) {
        auto tangent = [](f64 deg) {
            f64 ts = 0, tc = 1;
            trig::sin_cos(std::clamp(deg, -kMaxSkew, kMaxSkew) * trig::kPi / 180.0, ts, tc);
            return ts / tc;
        };
        rs = rs * MotionMatrix{1, 0, tangent(skew_x), 1, 0, 0} * MotionMatrix{1, tangent(skew_y), 0, 1, 0, 0};
    }
    return MotionMatrix{1, 0, 0, 1, x + pv.x, y + pv.y} * rs * MotionMatrix{1, 0, 0, 1, -pv.x, -pv.y};
}

MotionShown motion_shown(const MotionFile& file, const MotionClip& clip, f64 t) {
    MotionShown out;
    const auto track = std::find_if(clip.tracks.begin(), clip.tracks.end(), [](const MotionTrack& tr) { return tr.prop == MotionProp::Drawing; });
    if (track == clip.tracks.end() || track->pieces.empty()) return out;
    t = snap_ns(std::clamp(t, 0.0, std::max(clip.length, 0.0)));
    auto it = std::upper_bound(track->pieces.begin(), track->pieces.end(), t, [](f64 x, const MotionPiece& p) { return x < p.at; });
    if (it != track->pieces.begin()) --it;
    const MotionPiece& pc = *it;
    if (pc.kind != MotionPiece::Kind::Ease) {
        out.drawing = pc.kind == MotionPiece::Kind::Hold ? pc.value.target : kMotionMainDrawing;
        return out;
    }
    // As the tab's frame takes a swap's share (forge::pixel::block_share): its easing on nanoseconds.
    const f64 local = snap_ns(t - pc.at);
    f64 k = 0;
    if (pc.length <= 0) k = local < 0 ? 0 : 1;
    else if (local <= 0) k = 0;
    else if (local >= pc.length) k = 1;
    else k = motion_ease(file, pc, local / pc.length);
    out.drawing = k >= 1 ? pc.to.target : pc.from.target;
    if (k > 0 && k < 1 && pc.from.target != pc.to.target) {
        out.drawing = pc.to.target;
        out.swap = true;
        out.from = pc.from.target;
        out.k = k;
    }
    return out;
}

std::vector<MotionPartPose> motion_pose(const MotionFile& file, const MotionClip& clip, f64 t, std::string_view drawing) {
    std::vector<MotionPartPose> out;
    std::map<std::string, usize, std::less<>> at;
    std::set<std::string_view> unborn; // a copy not born yet has no element to pose
    for (const MotionCopy& c : clip.copies)
        if (!motion_copy_drawn(c, t)) unborn.insert(c.id);
    for (const MotionTrack& tr : clip.tracks) {
        if (unborn.count(tr.part) || tr.prop == MotionProp::Drawing) continue;
        auto [it, fresh] = at.try_emplace(tr.part, out.size());
        if (fresh) {
            out.emplace_back();
            out.back().id = tr.part;
            for (usize p = 0; p < kMotionPropCount; ++p) out.back().values[p] = motion_rest(static_cast<MotionProp>(p));
        }
        MotionPartPose& pp = out[it->second];
        const auto i = static_cast<usize>(tr.prop);
        pp.has[i] = true;
        pp.values[i] = motion_track_value(file, tr, t, clip.length);
    }
    for (MotionPartPose& pp : out) motion_part_pose(file, pp, drawing);
    return out;
}

void motion_part_pose(const MotionFile& file, MotionPartPose& pp, std::string_view drawing) {
    pp.moves = false;
    pp.local = {};
    for (usize p = 0; p < kMotionPropCount; ++p) pp.moves = pp.moves || (pp.has[p] && motion_prop_moves(static_cast<MotionProp>(p)));
    const MotionPart* part = pp.moves ? file.part(pp.id, drawing) : nullptr;
    if (!part) {
        pp.moves = false;
        return;
    }
    auto v = [&](MotionProp p) { return pp.values[static_cast<usize>(p)].v; };
    const MotionMatrix a = motion_matrix(v(MotionProp::X), v(MotionProp::Y), v(MotionProp::Rot), v(MotionProp::Scale), v(MotionProp::Sx),
                                         v(MotionProp::Sy), v(MotionProp::SkewX), v(MotionProp::SkewY), part->pivot);
    pp.local = inverse(part->frame) * a * part->frame * part->own;
}

bool motion_clip_missing(const MotionClip& clip, const SvgOutline& svg, std::string& part, std::string& role) {
    return motion_clip_missing(clip, std::vector<const SvgOutline*>{&svg}, part, role);
}

bool motion_clip_missing(const MotionClip& clip, const std::vector<const SvgOutline*>& svgs, std::string& part, std::string& role) {
    auto drawn = [&](std::string_view id) {
        for (const SvgOutline* svg : svgs)
            for (const SvgElement& e : svg->elements)
                if (e.id == id)
                    if (e.tag == "g" || e.tag == "use" || e.tag == "path" || e.tag == "rect" || e.tag == "circle" || e.tag == "ellipse" ||
                        e.tag == "line" || e.tag == "polyline" || e.tag == "polygon")
                        return true;
        return false;
    };
    auto copy = [&](std::string_view id) {
        return std::any_of(clip.copies.begin(), clip.copies.end(), [&](const MotionCopy& c) { return c.id == id; });
    };
    auto missing = [&](const std::string& id, std::string what) {
        if (id.empty() || drawn(id)) return false;
        part = id;
        role = std::move(what);
        return true;
    };
    for (const MotionCopy& c : clip.copies)
        if (missing(c.source, "копия «" + c.id + "»")) return true;
    for (const MotionTrack& t : clip.tracks) {
        if (t.prop != MotionProp::Mask && t.prop != MotionProp::Link) continue;
        for (const MotionPiece& pc : t.pieces)
            if (missing(pc.value.target, std::string(t.prop == MotionProp::Mask ? "маска «" : "крепление «") + t.part + "»")) return true;
    }
    for (const MotionOrderStep& s : clip.order)
        if ((!copy(s.part) && missing(s.part, "порядок «" + s.part + "»")) || missing(s.other, "порядок «" + s.part + "»")) return true;
    return false;
}

bool motion_copy_drawn(const MotionCopy& copy, f64 t) { return snap_ns(copy.born) <= snap_ns(t); }

std::vector<MotionOrderStep> motion_order_at(const MotionClip& clip, f64 t) {
    std::vector<MotionOrderStep> out;
    for (const MotionOrderStep& st : clip.order) {
        if (!(snap_ns(st.at) <= snap_ns(t))) continue;
        const auto copy = std::find_if(clip.copies.begin(), clip.copies.end(), [&](const MotionCopy& c) { return c.id == st.part; });
        if (copy == clip.copies.end() || motion_copy_drawn(*copy, t)) out.push_back(st);
    }
    return out;
}

MotionClip motion_clip_with(const MotionFile& file, const MotionClip& clip, const std::vector<MotionParam>& values) {
    MotionClip out = clip;
    for (const MotionParam& given : values) {
        const MotionParam* own = file.param(given.name);
        if (!own || clip.fixed.count(given.name)) continue;
        const f64 units = given.value - own->value;
        for (MotionTrack& t : out.tracks)
            for (MotionPiece& pc : t.pieces)
                for (const MotionBy& b : pc.by) {
                    if (b.param != given.name) continue;
                    if (pc.kind == MotionPiece::Kind::Hold) pc.value = motion_moved(pc.value, b.value, units);
                    if (pc.kind == MotionPiece::Kind::Ease) {
                        pc.from = motion_moved(pc.from, b.from, units);
                        pc.to = motion_moved(pc.to, b.to, units);
                    }
                    for (usize i = 0; i < pc.frames.size() && i < b.frames.size(); ++i) pc.frames[i] = motion_moved(pc.frames[i], b.frames[i], units);
                }
    }
    return out;
}

f64 motion_clip_time(const MotionClip& clip, f64 played, bool loop) {
    if (!(clip.length > 0) || !(played > 0)) return 0;
    if (!loop) return std::min(played, clip.length);
    return std::fmod(played, clip.length);
}

std::vector<usize> motion_marks_passed(const MotionClip& clip, f64 before, f64 played, bool loop) {
    std::vector<std::pair<f64, usize>> hit; // when, which
    const f64 len = clip.length;
    for (usize i = 0; i < clip.marks.size(); ++i) {
        const f64 at = clip.marks[i].at;
        f64 when = at; // the first time the clip is at the mark after `before`
        if (before >= 0) {
            if (loop && len > 0) {
                when = at + (std::floor((before - at) / len) + 1) * len;
                if (when <= before) when += len; // rounding
            } else if (at <= before) {
                continue;
            }
        }
        if (!loop && at > len) continue;
        if (when <= played) hit.push_back({when, i});
    }
    std::stable_sort(hit.begin(), hit.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<usize> out;
    for (const auto& [when, i] : hit) out.push_back(i);
    return out;
}

} // namespace forge::vector
