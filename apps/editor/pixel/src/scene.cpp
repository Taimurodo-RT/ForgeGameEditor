#include "forge/pixel/scene.h"

#include "forge/assets/image.h"

#include <yyjson.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <optional>

namespace forge::pixel {

namespace {

// A number of the stage's SVG as it is, to the last bit: an actor stands where its matrix puts it.
std::string exact(f64 v) {
    if (v == 0) v = 0; // no "-0"
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

std::string matrix_text(const Affine& m) {
    return "matrix(" + exact(m.a) + " " + exact(m.b) + " " + exact(m.c) + " " + exact(m.d) + " " + exact(m.e) + " " + exact(m.f) + ")";
}

std::string_view as_text(const std::vector<u8>& bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

bool number(yyjson_val* v, f64& out) {
    if (!yyjson_is_num(v)) return false;
    out = yyjson_get_num(v);
    return std::isfinite(out);
}

constexpr f64 kMaxSceneFps = 240;
// The marks of the actors' clips one scene may have: a loop of a short clip goes round many times.
constexpr usize kMaxSceneMarks = 100000;

} // namespace

const StageItem* Stage::item(std::string_view id) const {
    for (const StageItem& it : items)
        if (it.id == id) return &it;
    return nullptr;
}

Affine actor_matrix(const StageItem& a) {
    const f64 r = a.rot * std::numbers::pi / 180, c = std::cos(r), s = std::sin(r);
    const Affine turn{c, s, -s, c, 0, 0};
    const Affine scale{a.scale * a.sx, 0, 0, a.scale * a.sy, 0, 0};
    return translation(a.x, a.y) * (turn * (scale * translation(-a.pivot.x, -a.pivot.y)));
}

bool camera_part(std::string_view id) {
    if (id == kCameraPart) return true;
    for (const char* l : kLayerParts)
        if (id == l) return true;
    return false;
}

Affine camera_matrix(const SceneCamera& cam, f64 w, f64 h) {
    const f64 r = -cam.rot * std::numbers::pi / 180, c = std::cos(r), s = std::sin(r);
    const Affine look{cam.zoom * c, cam.zoom * s, -cam.zoom * s, cam.zoom * c, 0, 0};
    return translation(w / 2, h / 2) * (look * translation(-(w / 2 + cam.x), -(h / 2 + cam.y)));
}

void frame_size(f64 stage_w, f64 stage_h, f64 side, u32& w, u32& h) {
    const f64 sw = std::max(1.0, stage_w), sh = std::max(1.0, stage_h);
    w = static_cast<u32>(std::max<i64>(1, std::lround(sw >= sh ? side : side * sw / sh)));
    h = static_cast<u32>(std::max<i64>(1, std::lround(sw >= sh ? side * sh / sw : side)));
}

std::string stage_svg(const Stage& st) {
    Drawing d;
    d.canvas = true;
    d.width = st.width();
    d.height = st.height();
    d.canvas_line = st.drawing.canvas_line;
    if (!st.background.empty()) {
        DrawShape bg;
        bg.kind = DrawShape::Kind::Rect;
        bg.w = d.width;
        bg.h = d.height;
        bg.fill = st.background;
        d.shapes.push_back(std::move(bg));
    }
    // Each item's group stands where it is put, with a rect of its size the rig takes its bounds from.
    std::vector<i32> at(st.drawing.shapes.size(), -1);
    for (usize i = 0; i < st.drawing.shapes.size(); ++i) {
        DrawShape s = st.drawing.shapes[i];
        if (s.parent >= 0) s.parent = at[static_cast<usize>(s.parent)];
        at[i] = static_cast<i32>(d.shapes.size());
        const StageItem* it = nullptr;
        for (const StageItem& x : st.items)
            if (x.group == static_cast<i32>(i)) it = &x;
        const bool picture = it && it->kind == StageItem::Kind::Picture;
        if (it) s.transform = picture ? "translate(" + exact(it->x) + " " + exact(it->y) + ")" : matrix_text(actor_matrix(*it));
        d.shapes.push_back(std::move(s));
        if (!it) continue;
        DrawShape box;
        box.kind = DrawShape::Kind::Rect;
        box.parent = at[i];
        box.fill = "none";
        if (!picture) box.x = it->origin.x, box.y = it->origin.y;
        box.w = it->w;
        box.h = it->h;
        d.shapes.push_back(std::move(box));
    }
    return drawing_svg(d, static_cast<u32>(d.width), static_cast<u32>(d.height));
}

// ---- the scene's file ----

bool read_scene_doc(std::string_view json, SceneDoc& out, std::string& why) {
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(json.data()), json.size(), 0, nullptr, &err);
    if (!doc) {
        why = std::string("это не JSON: ") + err.msg + " (знак " + std::to_string(err.pos) + ")";
        return false;
    }
    SceneDoc d;
    auto read = [&](yyjson_val* root) {
        if (!yyjson_is_obj(root)) return why = "в файле не объект JSON", false;
        if (yyjson_val* v = yyjson_obj_get(root, "script")) {
            if (!yyjson_is_str(v)) return why = "script: нужно имя файла строкой", false;
            d.script = yyjson_get_str(v);
        }
        if (yyjson_val* v = yyjson_obj_get(root, "seed")) {
            if (!yyjson_is_uint(v)) return why = "seed: нужно целое число от 0", false;
            d.seed = yyjson_get_uint(v);
        }
        if (yyjson_val* v = yyjson_obj_get(root, "params")) {
            if (!yyjson_is_obj(v)) return why = "params: нужен объект {\"имя\": число}", false;
            usize i, n;
            yyjson_val *key, *val;
            yyjson_obj_foreach(v, i, n, key, val) {
                f64 x;
                if (!number(val, x)) return why = std::string("params.") + yyjson_get_str(key) + ": нужно число", false;
                d.params[yyjson_get_str(key)] = x;
            }
        }
        if (yyjson_val* v = yyjson_obj_get(root, "fps"); v && (!number(v, d.fps) || d.fps <= 0 || d.fps > kMaxSceneFps))
            return why = "fps: нужно число больше 0 и не больше " + std::to_string(static_cast<u32>(kMaxSceneFps)), false;
        if (yyjson_val* r = yyjson_obj_get(root, "range")) {
            if (!yyjson_is_arr(r) || yyjson_arr_size(r) != 2 || !number(yyjson_arr_get(r, 0), d.from) || !number(yyjson_arr_get(r, 1), d.to) ||
                d.from < 0 || d.to <= d.from || d.to > kMaxRange)
                return why = "range: нужно [от, до] в секундах, 0 ≤ от < до ≤ " + std::to_string(static_cast<u32>(kMaxRange)), false;
            d.ranged = true;
        }
        for (const bool wait : {true, false}) {
            const char* field = wait ? "waits" : "durations";
            yyjson_val* v = yyjson_obj_get(root, field);
            if (!v) continue;
            if (!yyjson_is_obj(v)) return why = std::string(field) + ": нужен объект {\"имя\": секунды}", false;
            std::map<std::string, f64>& m = wait ? d.lengths.waits : d.lengths.durations;
            usize i, n;
            yyjson_val *name, *len;
            yyjson_obj_foreach(v, i, n, name, len) {
                f64 s = 0;
                if (!number(len, s) || s < 0) return why = std::string(field) + "." + yyjson_get_str(name) + ": нужно число секунд не меньше 0", false;
                m[yyjson_get_str(name)] = s;
            }
        }
        return true;
    };
    const bool ok = read(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (ok) out = std::move(d);
    return ok;
}

std::string write_scene_doc(const SceneDoc& d) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "script", d.script.c_str());
    yyjson_mut_obj_add_uint(doc, root, "seed", d.seed);
    yyjson_mut_val* params = yyjson_mut_obj(doc);
    for (const auto& [k, v] : d.params) yyjson_mut_obj_add(params, yyjson_mut_strcpy(doc, k.c_str()), yyjson_mut_real(doc, v));
    yyjson_mut_obj_add_val(doc, root, "params", params);
    yyjson_mut_obj_add_real(doc, root, "fps", d.fps);
    if (d.ranged) {
        yyjson_mut_val* r = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_real(doc, r, d.from);
        yyjson_mut_arr_add_real(doc, r, d.to);
        yyjson_mut_obj_add_val(doc, root, "range", r);
    }
    // Only when the scene has them, as a character's file.
    for (const bool wait : {true, false}) {
        const std::map<std::string, f64>& m = wait ? d.lengths.waits : d.lengths.durations;
        if (m.empty()) continue;
        yyjson_mut_val* field = yyjson_mut_obj(doc);
        for (const auto& [name, s] : m) yyjson_mut_obj_add(field, yyjson_mut_strcpy(doc, name.c_str()), yyjson_mut_real(doc, s));
        yyjson_mut_obj_add_val(doc, root, wait ? "waits" : "durations", field);
    }
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
    std::string out(text, len);
    out += '\n';
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

std::string new_scene_code(std::string_view name) {
    return "-- «" + std::string(name) +
           "»\n"
           "return {\n"
           "  stage = function(p, rng)\n"
           "    canvas(1920, 1080)\n"
           "    background(\"#203040\")\n"
           "  end,\n"
           "  scene = function(p)\n"
           "    wait(1)\n"
           "  end,\n"
           "}\n";
}

// ---- an actor's ids ----

namespace {

bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// url(#x) → url(#ёжик/x) in an attribute's value or a style's text; url('#x') and url("#x") too.
std::string prefix_urls(std::string_view text, const std::string& pre) {
    std::string out;
    usize i = 0;
    for (;;) {
        const usize u = text.find("url(", i);
        if (u == std::string_view::npos) {
            out.append(text.substr(i));
            return out;
        }
        usize j = u + 4;
        while (j < text.size() && (space(text[j]) || text[j] == '\'' || text[j] == '"')) ++j;
        out.append(text.substr(i, j - i));
        if (j < text.size() && text[j] == '#') {
            out += '#';
            out += pre;
            ++j;
        }
        i = j;
    }
}

// One tag with its ids, references and urls put under the name.
std::string prefix_tag(std::string_view tag, const std::string& pre) {
    if (tag.size() < 2 || tag[1] == '/') return std::string(tag);
    usize k = 1;
    while (k < tag.size() && !space(tag[k]) && tag[k] != '/' && tag[k] != '>') ++k;
    std::string out(tag.substr(0, k));
    while (k < tag.size()) {
        const char c = tag[k];
        if (space(c) || c == '/' || c == '>') {
            out += c;
            ++k;
            continue;
        }
        const usize ns = k;
        while (k < tag.size() && !space(tag[k]) && tag[k] != '=' && tag[k] != '>' && tag[k] != '/') ++k;
        const std::string_view name = tag.substr(ns, k - ns);
        out += name;
        const usize ws = k;
        while (k < tag.size() && space(tag[k])) ++k;
        if (k >= tag.size() || tag[k] != '=') {
            out += tag.substr(ws, k - ws);
            continue;
        }
        ++k;
        while (k < tag.size() && space(tag[k])) ++k;
        out += tag.substr(ws, k - ws);
        if (k >= tag.size() || (tag[k] != '"' && tag[k] != '\'')) continue;
        const char q = tag[k];
        const usize vs = k + 1;
        usize ve = tag.find(q, vs);
        if (ve == std::string_view::npos) ve = tag.size();
        const std::string_view v = tag.substr(vs, ve - vs);
        out += q;
        if (name == "id") out += pre + std::string(v);
        else if ((name == "href" || name == "xlink:href") && !v.empty() && v[0] == '#') out += "#" + pre + std::string(v.substr(1));
        else out += prefix_urls(v, pre);
        if (ve < tag.size()) out += q;
        k = ve < tag.size() ? ve + 1 : tag.size();
    }
    return out;
}

bool root_tag(std::string_view tag) {
    usize k = 1;
    while (k < tag.size() && !space(tag[k]) && tag[k] != '/' && tag[k] != '>') ++k;
    const std::string_view name = tag.substr(1, k - 1);
    return name == "svg" || (name.size() > 4 && name.substr(name.size() - 4) == ":svg");
}

std::string attr_text(std::string_view s) {
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

std::string prefix_svg_ids(std::string_view svg, std::string_view name) {
    const std::string pre = attr_text(name) + "/";
    std::string out;
    out.reserve(svg.size() + svg.size() / 8);
    usize i = 0, root_end = std::string::npos;
    while (i < svg.size()) {
        const usize lt = svg.find('<', i);
        if (lt == std::string_view::npos) {
            out += prefix_urls(svg.substr(i), pre);
            break;
        }
        out += prefix_urls(svg.substr(i, lt - i), pre); // text: a style's url(#…)
        const std::string_view rest = svg.substr(lt);
        auto copy_to = [&](std::string_view close, bool urls) {
            usize end = svg.find(close, lt);
            end = end == std::string_view::npos ? svg.size() : end + close.size();
            const std::string_view part = svg.substr(lt, end - lt);
            out += urls ? prefix_urls(part, pre) : std::string(part);
            i = end;
        };
        if (rest.starts_with("<!--")) {
            copy_to("-->", false);
            continue;
        }
        if (rest.starts_with("<![CDATA[")) {
            copy_to("]]>", true);
            continue;
        }
        if (rest.starts_with("<?")) {
            copy_to("?>", false);
            continue;
        }
        if (rest.starts_with("<!")) {
            copy_to(">", false);
            continue;
        }
        // A tag, up to its '>' outside quotes.
        usize j = lt + 1;
        char q = 0;
        for (; j < svg.size(); ++j) {
            const char c = svg[j];
            if (q) {
                if (c == q) q = 0;
            } else if (c == '"' || c == '\'') {
                q = c;
            } else if (c == '>') {
                break;
            }
        }
        const usize end = j < svg.size() ? j + 1 : svg.size();
        const std::string_view tag = svg.substr(lt, end - lt);
        out += prefix_tag(tag, pre);
        if (root_end == std::string::npos && tag.size() > 1 && tag[1] != '/' && root_tag(tag)) root_end = out.size();
        i = end;
    }
    // All of it in a group with the name, inside the root: where the scene's looks of the whole actor go.
    if (root_end != std::string::npos && root_end >= 2 && out[root_end - 2] != '/') {
        const usize close = out.rfind("</");
        if (close != std::string::npos && close >= root_end) {
            out.insert(close, "</g>\n");
            out.insert(root_end, "\n<g id=\"" + attr_text(name) + "\">");
        }
    }
    return out;
}

Pose prefix_pose(const Pose& pose, std::string_view name) {
    const std::string pre = std::string(name) + "/";
    auto under = [&](const std::string& id) { return id.empty() ? id : pre + id; };
    Pose out;
    for (const auto& [id, pp] : pose.parts) {
        PartPose q = pp;
        q.mask = under(pp.mask);
        q.carry_by = under(pp.carry_by);
        out.parts[pre + id] = std::move(q);
    }
    for (const auto& [id, m] : pose.morphs) out.morphs[pre + id] = {under(m.target), m.k};
    for (OrderStep s : pose.order) {
        s.part = under(s.part);
        s.other = under(s.other);
        out.order.push_back(std::move(s));
    }
    for (PartCopy c : pose.copies) {
        c.id = pre + c.id;
        c.source = pre + c.source;
        out.copies.push_back(std::move(c));
    }
    out.drawing = pose.drawing; // the drawings keep their names
    out.swap = pose.swap;
    out.swap_from = pose.swap_from;
    out.swap_k = pose.swap_k;
    return out;
}

void over_part(PartPose& clip, const PartPose& scene) {
    clip.x += scene.x;
    clip.y += scene.y;
    clip.rot += scene.rot;
    clip.skew_x += scene.skew_x;
    clip.skew_y += scene.skew_y;
    clip.dash += scene.dash;
    clip.scale *= scene.scale;
    clip.sx *= scene.sx;
    clip.sy *= scene.sy;
    clip.opacity *= scene.opacity;
    clip.visible = clip.visible && scene.visible;
    if (scene.has_fill && scene.fill_k > 0) clip.has_fill = true, clip.fill = scene.fill, clip.fill_k = scene.fill_k;
    if (scene.has_stroke && scene.stroke_k > 0) clip.has_stroke = true, clip.stroke = scene.stroke, clip.stroke_k = scene.stroke_k;
    if (scene.has_stroke_width && scene.stroke_width_k > 0)
        clip.has_stroke_width = true, clip.stroke_width = scene.stroke_width, clip.stroke_width_k = scene.stroke_width_k;
    if (scene.draw_from != 0 || scene.draw_to != 1) clip.draw_from = scene.draw_from, clip.draw_to = scene.draw_to;
    if (scene.has_tint && scene.tint_k > 0) clip.has_tint = true, clip.tint = scene.tint, clip.tint_k = scene.tint_k;
    if (scene.flash != 0) clip.flash = scene.flash;
    if (scene.has_flash_color && scene.flash_color_k > 0)
        clip.has_flash_color = true, clip.flash_color = scene.flash_color, clip.flash_color_k = scene.flash_color_k;
    if (scene.blur != 0) clip.blur = scene.blur;
    if (scene.has_shadow && scene.shadow_k > 0) clip.has_shadow = true, clip.shadow = scene.shadow, clip.shadow_k = scene.shadow_k;
    if (scene.shadow_x != 0) clip.shadow_x = scene.shadow_x;
    if (scene.shadow_y != 0) clip.shadow_y = scene.shadow_y;
    if (scene.shadow_blur != 0) clip.shadow_blur = scene.shadow_blur;
    if (!scene.mask.empty()) clip.mask = scene.mask, clip.mask_invert = scene.mask_invert;
}

namespace {

// The scene's transition of an actor as a whole: where it stands, its opacity, effects, mask and link go on its group
// of the stage (round its pivot in the scene's units, the actor's picture in it); its shapes' colours, widths, dashes,
// shares drawn, tints and flashes on the group round its picture in its own rig.
void split_actor(const PartPose& pp, PartPose& stage, PartPose& look) {
    stage = pp;
    look = {};
    look.has_fill = pp.has_fill, look.fill = pp.fill, look.fill_k = pp.fill_k;
    look.has_stroke = pp.has_stroke, look.stroke = pp.stroke, look.stroke_k = pp.stroke_k;
    look.has_stroke_width = pp.has_stroke_width, look.stroke_width = pp.stroke_width, look.stroke_width_k = pp.stroke_width_k;
    look.dash = pp.dash;
    look.draw_from = pp.draw_from, look.draw_to = pp.draw_to;
    look.has_tint = pp.has_tint, look.tint = pp.tint, look.tint_k = pp.tint_k;
    look.flash = pp.flash;
    look.has_flash_color = pp.has_flash_color, look.flash_color = pp.flash_color, look.flash_color_k = pp.flash_color_k;
    stage.has_fill = stage.has_stroke = stage.has_stroke_width = stage.has_tint = stage.has_flash_color = false;
    stage.dash = 0;
    stage.draw_from = 0, stage.draw_to = 1;
    stage.flash = 0;
}

} // namespace

// ---- characters ----

bool load_character(const SceneFiles& files, const std::string& file, SceneCharacter& out, std::string& why) {
    auto fail = [&](const std::string& reason) {
        why = "персонаж «" + file + "»: " + reason;
        return false;
    };
    const std::string json = file + ".pixel.json";
    std::vector<u8> bytes;
    std::string err;
    if (!files.read || !files.read(json, bytes, err)) return fail(err.empty() ? "нет файла «" + json + "»" : err);
    SceneCharacter c;
    c.file = file;
    if (!read_pixel_doc(as_text(bytes), c.doc, err)) return fail("«" + json + "»: " + err);
    if (c.doc.script.empty()) return fail("в «" + json + "» нет кода (script)");
    const usize slash = file.rfind('/');
    const std::string code = (slash == std::string::npos ? std::string() : file.substr(0, slash + 1)) + c.doc.script;
    bytes.clear();
    if (!files.read(code, bytes, err)) return fail(err.empty() ? "нет кода «" + code + "»" : err);
    c.code.assign(as_text(bytes));
    PoseScript script;
    script.set_module_reader(files.modules);
    if (!script.load(c.code)) return fail("код «" + c.doc.script + "»: " + script.error());
    if (script.is_scene()) return fail("«" + c.doc.script + "» — сцена, а не персонаж");
    if (script.has_draw()) {
        if (!script.draw(c.doc.params, c.doc.seed, c.drawn)) return fail("код «" + c.doc.script + "»: " + script.error());
        c.by_code = true;
        c.svg = drawing_svg(c.drawn, c.doc.frame_w, c.doc.frame_h);
    } else {
        if (c.doc.svg.empty()) return fail("нет рисунка: ни SVG, ни draw в коде");
        if (!files.svg || !files.svg(c.doc.svg, c.svg, err)) return fail(err.empty() ? "его рисунка SVG нет в «Ресурсах»" : err);
    }
    if (!c.rig.load(c.svg, err)) return fail("рисунок: " + err);
    u32 line = 0;
    if (!draw_drawings(script, c.doc, c.drawings, err, line) || !add_drawings(c.rig, c.drawings, err, line))
        return fail("код «" + c.doc.script + "»: " + err);
    apply_pivots(c.rig, c.doc, c.by_code ? &c.drawn : nullptr, &c.drawings);
    out = std::move(c);
    return true;
}

// ---- actors' clips ----

namespace {

// An actor's plays by start, then order.
std::vector<const ActorPlay*> plays_of(const Recording& rec, std::string_view actor) {
    std::vector<const ActorPlay*> out;
    for (const ActorPlay& p : rec.plays)
        if (actor.empty() || p.actor == actor) out.push_back(&p);
    std::stable_sort(out.begin(), out.end(), [](const ActorPlay* a, const ActorPlay* b) {
        if (a->actor != b->actor) return a->actor < b->actor;
        return a->start != b->start ? a->start < b->start : a->order < b->order;
    });
    return out;
}

} // namespace

namespace {

// The stop the scene stands at, null when none.
const SceneStop* stop_of(const Recording& rec, i32 waiting) {
    return waiting >= 0 && waiting < static_cast<i32>(rec.stops.size()) ? &rec.stops[static_cast<usize>(waiting)] : nullptr;
}

// Whether a thing of the recording made at `start` (`order`) has begun at t: not when made after the stop the scene
// stands at, at its moment.
bool begun(f64 start, u32 order, f64 t, const SceneStop* at) {
    if (start > t) return false;
    return !at || start != at->time || order < at->order;
}

} // namespace

namespace {

// What an actor shows at t (the scene's waits up to then, of those at t made before `until`) when its call k is the last
// begun: empty at rest; a stop holds what the one before it showed at its start; a play's blend (part 6г5а) from what it
// showed at its start, as vector::scene_actor_moment.
std::vector<ActorBlendStep> shows(const Recording& rec, const std::vector<const ActorPlay*>& mine, std::span<const f64> held, i64 k, f64 t, u32 until) {
    while (k >= 0 && mine[static_cast<usize>(k)]->kind == ActorPlay::Kind::Stop) {
        t = mine[static_cast<usize>(k)]->start;
        until = mine[static_cast<usize>(k--)]->order;
    }
    if (k < 0) return {};
    const ActorPlay& p = *mine[static_cast<usize>(k)];
    // Part 6в: the clip went on while the scene waited after its start.
    f64 waited = 0;
    for (usize i = 0; i < held.size() && i < rec.stops.size(); ++i) {
        const SceneStop& w = rec.stops[i];
        const bool after = w.time > p.start || (w.time == p.start && w.order > p.order);
        const bool by_then = w.time < t || (w.time == t && w.order < until);
        if (after && by_then) waited += held[i];
    }
    const f64 s = (t - p.start + waited) * p.speed;
    ActorBlendStep step;
    step.play = &p;
    if (p.kind == ActorPlay::Kind::Play) step.time = std::min(s, p.clip_length);
    else step.time = p.clip_length > 0 ? std::fmod(s, p.clip_length) : 0;
    step.time = std::max(0.0, step.time);
    // Its blend, over its clip's play, no longer than the clip when it is no loop.
    const f64 blend = !(p.clip_length > 0) ? 0 : p.kind == ActorPlay::Kind::Loop ? p.blend : std::min(p.blend, p.clip_length);
    if (blend > 0 && s < blend) step.k = vector::timing(vector::Timing::Smooth, std::max(0.0, s) / blend);
    if (step.k >= 1) return {step};
    // From what it showed at its start: the call before going on by its own time, or that held (a stop, a blend going on
    // then, the character at rest).
    std::vector<ActorBlendStep> out = shows(rec, mine, held, k - 1, p.start, p.order);
    if (out.size() == 1 && out[0].play == mine[static_cast<usize>(k - 1)]) out = shows(rec, mine, held, k - 1, t, until);
    if (out.empty()) out.push_back({});
    out.push_back(step);
    return out;
}

} // namespace

ActorMoment actor_moment(const Recording& rec, std::string_view actor, f64 t, std::span<const f64> held, i32 waiting) {
    if (actor.empty()) return {};
    const std::vector<const ActorPlay*> mine = plays_of(rec, actor);
    const SceneStop* at = stop_of(rec, waiting);
    i64 k = -1;
    for (usize i = 0; i < mine.size(); ++i)
        if (begun(mine[i]->start, mine[i]->order, t, at)) k = static_cast<i64>(i);
    std::vector<ActorBlendStep> steps = shows(rec, mine, held, k, t, std::numeric_limits<u32>::max());
    if (steps.empty()) return {};
    ActorMoment m;
    m.play = steps.back().play;
    m.time = steps.back().time;
    if (steps.size() > 1) m.blend = std::move(steps);
    return m;
}

namespace {

// The music calls by start, then as made.
std::vector<u32> music_calls(const Recording& rec) {
    std::vector<u32> out(rec.music.size());
    for (u32 i = 0; i < out.size(); ++i) out[i] = i;
    std::stable_sort(out.begin(), out.end(), [&](u32 a, u32 b) { return rec.music[a].start < rec.music[b].start; });
    return out;
}

// A volume going from `from` to `to` over `fade` seconds from `at`: its value at t.
struct Ramp {
    f64 at = 0, fade = 0, from = 0, to = 0;
    f64 at_time(f64 t) const {
        if (!(fade > 0) || t >= at + fade) return to;
        return from + (to - from) * std::clamp((t - at) / fade, 0.0, 1.0);
    }
};

} // namespace

std::vector<MusicVoice> music_at(const Recording& rec, f64 t, std::span<const f64> held, i32 waiting) {
    struct Playing {
        u32 call = 0;
        Ramp volume;
    };
    const SceneStop* at = stop_of(rec, waiting);
    // The moments music lives by: a scene's time and the waits before it (a stop at the same time made before it).
    auto lived = [&](f64 time, u32 order) {
        f64 s = time;
        for (usize i = 0; i < held.size() && i < rec.stops.size(); ++i)
            if (rec.stops[i].time < time || (rec.stops[i].time == time && rec.stops[i].order < order)) s += held[i];
        return s;
    };
    const f64 now_at = lived(t, std::numeric_limits<u32>::max());
    std::optional<Playing> now;
    std::vector<Playing> going; // the ones fading out
    for (const u32 i : music_calls(rec)) {
        const SceneMusic& m = rec.music[i];
        if (!begun(m.start, m.order, t, at)) continue;
        const f64 when = lived(m.start, m.order);
        if (now && !m.name.empty() && rec.music[now->call].name == m.name) {
            // The same music goes on: only its volume moves.
            now->volume = {when, m.fade, now->volume.at_time(when), m.volume};
            continue;
        }
        if (now) going.push_back({now->call, {when, m.fade, now->volume.at_time(when), 0}});
        now.reset();
        if (!m.name.empty()) now = Playing{i, {when, m.fade, 0, m.volume}};
    }
    auto since = [&](u32 call) { return now_at - lived(rec.music[call].start, rec.music[call].order); };
    std::vector<MusicVoice> out;
    for (const Playing& p : going)
        if (now_at < p.volume.at + p.volume.fade) out.push_back({rec.music[p.call].name, since(p.call), p.volume.at_time(now_at), p.call});
    if (now) out.push_back({rec.music[now->call].name, since(now->call), now->volume.at_time(now_at), now->call});
    return out;
}

std::vector<MusicRun> music_runs(const Recording& rec) {
    std::vector<MusicRun> out;
    bool open = false;
    for (const u32 i : music_calls(rec)) {
        const SceneMusic& m = rec.music[i];
        if (open && !m.name.empty() && out.back().name == m.name) {
            out.back().calls.push_back(i);
            continue;
        }
        if (open) out.back().end = m.start;
        open = !m.name.empty();
        if (open) out.push_back({m.name, m.start, rec.length, m.fade, {i}});
    }
    if (open) out.back().end = std::max(out.back().start, rec.length);
    return out;
}

bool actor_marks(const Recording& rec, SceneMarkSource& clips, std::vector<Mark>& out, std::string& why) {
    out.clear();
    const std::vector<const ActorPlay*> all = plays_of(rec, {});
    for (usize i = 0; i < all.size(); ++i) {
        const ActorPlay& p = *all[i];
        if (p.kind == ActorPlay::Kind::Stop) continue;
        // Up to the actor's next play, loop or stop, else to the scene's end, that moment too.
        const bool last = i + 1 == all.size() || all[i + 1]->actor != p.actor;
        const f64 end = last ? rec.length : all[i + 1]->start;
        const Recording* r = clips.clip_of(p, why);
        if (!r) {
            why = "актёр «" + p.actor + "»: " + why;
            return false;
        }
        if (r->marks.empty()) continue;
        const bool loop = p.kind == ActorPlay::Kind::Loop;
        const f64 round = p.clip_length / p.speed;
        for (u64 k = 0;; ++k) {
            const f64 base = snap_time(p.start + static_cast<f64>(k) * round);
            if (k > 0 && (!loop || !(round > 0) || base >= end)) break;
            // A loop's mark at its clip's end comes at each round's end, as a game's screen gives it
            // (vector::motion_marks_passed).
            for (const Mark& m : r->marks) {
                const f64 at = snap_time(base + m.time / p.speed);
                if (at > end || (at == end && !last)) continue;
                if (out.size() >= kMaxSceneMarks) {
                    why = "актёр «" + p.actor + "»: меток клипов больше " + std::to_string(kMaxSceneMarks) + " — клип по кругу слишком короткий";
                    return false;
                }
                Mark x = m;
                x.time = at;
                x.length = m.length / p.speed;
                x.actor = p.actor;
                out.push_back(std::move(x));
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Mark& a, const Mark& b) { return a.time < b.time; });
    return true;
}

// ---- the scene ----

struct Scene::Impl {
    SceneDoc doc;
    SceneFiles files;
    PoseScript script;
    Stage stage;
    std::map<std::string, std::unique_ptr<SceneCharacter>> characters; // by file
    std::map<std::string, assets::CookedTexture> pictures;            // by file

    struct Actor {
        std::string id;
        SceneCharacter* character = nullptr;
        std::map<std::string, f64> params; // the character's, the stage's over them
        Rig rig;                           // its picture, ids «ёжик/…», hosted in the stage's group «ёжик»
    };
    std::vector<std::unique_ptr<Actor>> actors;
    // A character's code by its file and parameters: each keeps the recordings of its clips.
    std::map<std::string, std::unique_ptr<PoseScript>> scripts;
    Rig rig; // after the actors: gone before the pictures it draws of theirs
    SceneHooks hooks;

    // Part 6б: the camera and the transition's frames as the last pose put them, and which of the scene's times it was.
    SceneCamera camera;
    PartPose layers[4]; // kLayerParts
    ScenePhase phase = ScenePhase::Scene;

    bool ok = false, recorded = false;
    Recording rec;
    std::vector<std::string> read; // the files of «Ресурсы» asked for, there or not
    std::string error;
    u32 error_line = 0;

    void note(const std::string& path) {
        if (std::find(read.begin(), read.end(), path) == read.end()) read.push_back(path);
    }

    Actor* actor(std::string_view id) {
        for (auto& a : actors)
            if (a->id == id) return a.get();
        return nullptr;
    }

    // The actor whose part «ёжик/head» is; null for the stage's own.
    Actor* owner(std::string_view id) {
        const usize slash = id.find('/');
        return slash == std::string_view::npos ? nullptr : actor(id.substr(0, slash));
    }

    std::map<std::string, f64> values(const Actor& a, const ActorPlay& p) const {
        std::map<std::string, f64> v = a.params;
        for (const auto& [k, x] : p.params) v[k] = x;
        return v;
    }

    SceneCharacter* character(const std::string& file, std::string& why) {
        if (auto it = characters.find(file); it != characters.end()) return it->second.get();
        SceneFiles noted = files;
        noted.read = [this](const std::string& path, std::vector<u8>& bytes, std::string& w) {
            note(path);
            return files.read && files.read(path, bytes, w);
        };
        auto c = std::make_unique<SceneCharacter>();
        if (!load_character(noted, file, *c, why)) return nullptr;
        return (characters[file] = std::move(c)).get();
    }

    PoseScript* script_for(const SceneCharacter& c, const std::map<std::string, f64>& params, std::string& why) {
        std::string key = c.file + '\n';
        for (const auto& [k, v] : params) key += k + '=' + exact(v) + '\n';
        if (auto it = scripts.find(key); it != scripts.end()) return it->second.get();
        auto s = std::make_unique<PoseScript>();
        s->set_module_reader(files.modules);
        if (!s->load(c.code)) {
            why = "код «" + c.doc.script + "»: " + s->error();
            return nullptr;
        }
        s->set_lengths(c.doc.lengths);
        return (scripts[key] = std::move(s)).get();
    }

    // The recording of an actor's clip with these parameters: null with why.
    const Recording* clip(const Actor& a, const std::string& name, const std::map<std::string, f64>& values, std::string& why) {
        const SceneCharacter& c = *a.character;
        PoseScript* s = script_for(c, values, why);
        if (!s) return nullptr;
        const std::vector<std::string>& clips = s->clips();
        if (std::find(clips.begin(), clips.end(), name) == clips.end()) {
            std::string have;
            for (const std::string& n : clips) have += (have.empty() ? "" : ", ") + ("«" + n + "»");
            why = "у персонажа «" + c.file + "» нет клипа «" + name + "»" + (have.empty() ? ": клипов у него нет" : " (есть " + have + ")");
            return nullptr;
        }
        const Recording* r = s->recording(c.rig, name, values, c.doc.seed);
        if (!r) why = "клип «" + name + "»: " + s->error();
        return r;
    }

    void make_hooks() {
        hooks.picture = [this](const std::string& file, f64& w, f64& h, std::string& why) {
            auto it = pictures.find(file);
            if (it == pictures.end()) {
                const usize dot = file.rfind('.');
                std::string ext = dot == std::string::npos ? std::string() : file.substr(dot + 1);
                for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (ext != "png") return why = "«" + file + "» — не PNG: на сцену ставятся картинки PNG", false;
                note(file);
                std::vector<u8> bytes;
                std::string err;
                if (!files.read || !files.read(file, bytes, err)) return why = err.empty() ? "нет картинки «" + file + "»" : err, false;
                assets::CookedTexture t;
                if (!assets::decode_image(bytes, t, &err) || t.width == 0 || t.height == 0)
                    return why = "картинка «" + file + "» не читается" + (err.empty() ? "" : ": " + err), false;
                it = pictures.emplace(file, std::move(t)).first;
            }
            w = it->second.width;
            h = it->second.height;
            return true;
        };
        hooks.character = [this](const std::string& file, f64 area[4], std::string& why) {
            const SceneCharacter* c = character(file, why);
            if (!c) return false;
            const vector::Point o = c->rig.origin();
            area[0] = o.x, area[1] = o.y, area[2] = c->rig.width(), area[3] = c->rig.height();
            return true;
        };
        hooks.clip_length = [this](const std::string& id, const std::string& name, const std::map<std::string, f64>& params, f64& length,
                                   std::string& why) {
            const Actor* a = actor(id);
            if (!a) return why = "на сцене нет актёра «" + id + "»", false;
            std::map<std::string, f64> v = a->params;
            for (const auto& [k, x] : params) v[k] = x;
            const Recording* r = clip(*a, name, v, why);
            if (!r) return false;
            length = r->length;
            return true;
        };
    }

    bool fail(std::string why, u32 line = 0) {
        error = std::move(why);
        error_line = line;
        return false;
    }

    // What pose() gives the pictures at t by the plays of `by`: the camera and the transitions' frames (part 6б), which of
    // the scene's times t is, the stage's pose (its own parts, its actors' groups) and each actor's (its clip's under its
    // name, the scene's over it). False with error() when an actor's clip does not pose; the rest is filled.
    bool compose(f64 t, const Recording* by, const Pose& scene, std::span<const f64> held, i32 waiting, ScenePoses& out) {
        out = {};
        out.scene = scene;
        if (by) {
            bool entering = false;
            for (const SceneTransition& tr : by->transitions) {
                const f64 end = snap_time(tr.start + tr.length);
                if (!tr.leave) entering = entering || (t >= tr.start && t < end);
                else if (t >= tr.start) out.phase = t < end ? ScenePhase::Leave : ScenePhase::Gone;
            }
            if (entering && out.phase == ScenePhase::Scene) out.phase = ScenePhase::Enter;
        }
        // The scene's pose: its own parts' on the stage, its actors' over their clips.
        std::map<std::string, Pose> over; // by actor
        for (const auto& [id, pp] : scene.parts) {
            if (id == kCameraPart) {
                out.camera = {pp.x, pp.y, pp.scale, pp.rot};
                continue;
            }
            bool layer = false;
            for (usize i = 0; i < std::size(kLayerParts); ++i)
                if (id == kLayerParts[i]) out.layers[i] = pp, layer = true;
            if (layer) continue;
            if (actor(id)) {
                PartPose look;
                split_actor(pp, out.stage.parts[id], look);
                over[id].parts[id] = look;
            } else if (const Actor* a = owner(id)) {
                over[a->id].parts[id] = pp;
            } else {
                out.stage.parts[id] = pp;
            }
        }
        for (const auto& [id, m] : scene.morphs) {
            if (const Actor* a = owner(id)) over[a->id].morphs[id] = m;
            else out.stage.morphs[id] = m;
        }
        for (const OrderStep& s : scene.order) {
            if (const Actor* a = owner(s.part)) over[a->id].order.push_back(s);
            else out.stage.order.push_back(s);
        }
        out.stage.copies = scene.copies;
        bool posed = true;
        std::string why;
        for (const auto& a : actors) {
            Pose& pose = out.actors[a->id];
            const ActorMoment m = by ? actor_moment(*by, a->id, t, held, waiting) : ActorMoment{};
            // A clip at its moment; the character at rest without one.
            auto clip_pose = [&](const ActorPlay* play, f64 time, Pose& out) {
                out = {};
                if (!play) return true;
                const std::map<std::string, f64> v = values(*a, *play);
                PoseScript* s = script_for(*a->character, v, why);
                if (s && s->pose(a->character->rig, time, play->clip, v, a->character->doc.seed, out)) return true;
                return fail("актёр «" + a->id + "», клип «" + play->clip + "»: " + (s ? s->error() : why));
            };
            if (m.play) {
                Pose clip;
                bool ok = m.blend.empty() ? clip_pose(m.play, m.time, clip) : clip_pose(m.blend.front().play, m.blend.front().time, clip);
                // Part 6г5а: in a blend, each step's clip blended in over the ones before it.
                for (usize i = 1; ok && i < m.blend.size(); ++i) {
                    Pose next;
                    ok = clip_pose(m.blend[i].play, m.blend[i].time, next);
                    if (ok) clip = blend_pose(a->character->rig, clip, next, m.blend[i].k);
                }
                if (ok) pose = prefix_pose(clip, a->id);
                else posed = false;
            }
            if (const auto o = over.find(a->id); o != over.end()) {
                for (const auto& [id, pp] : o->second.parts) over_part(pose.parts[id], pp);
                for (const auto& [id, mm] : o->second.morphs)
                    if (mm.k > 0) pose.morphs[id] = mm;
                pose.order.insert(pose.order.end(), o->second.order.begin(), o->second.order.end());
            }
        }
        return posed;
    }
};

Scene::Scene() : impl_(std::make_unique<Impl>()) {}
Scene::~Scene() = default;

bool Scene::load(std::string_view code, const SceneDoc& doc, const SceneFiles& files) {
    impl_ = std::make_unique<Impl>();
    Impl& im = *impl_;
    im.doc = doc;
    im.files = files;
    im.make_hooks();
    im.script.set_module_reader(files.modules);
    if (!im.script.load(code)) return im.fail(im.script.error(), im.script.error_line());
    if (!im.script.is_scene()) return im.fail("это не сцена: в коде нет stage = function(p, rng) … end и scene = function(p) … end");
    if (!im.script.stage(doc.params, doc.seed, im.hooks, im.stage)) return im.fail(im.script.error(), im.script.error_line());
    std::string why;
    if (!im.rig.load(stage_svg(im.stage), why)) return im.fail("рисунок сцены: " + why);
    apply_pivots(im.rig, PixelDoc{}, &im.stage.drawing);
    for (const StageItem& it : im.stage.items) {
        if (it.kind == StageItem::Kind::Picture) {
            if (!im.rig.host(it.id, im.pictures.at(it.file), it.w, it.h, why)) return im.fail("картинка «" + it.id + "»: " + why, it.line);
            im.rig.set_pivot(it.id, {it.x + it.w / 2, it.y + it.h / 2}); // its middle, as a group's
            continue;
        }
        auto a = std::make_unique<Impl::Actor>();
        a->id = it.id;
        a->character = im.characters.at(it.file).get();
        const SceneCharacter& c = *a->character;
        a->params = c.doc.params;
        for (const auto& [k, v] : it.params) a->params[k] = v;
        const std::string what = "актёр «" + it.id + "»: ";
        if (!a->rig.load(prefix_svg_ids(c.svg, it.id), why)) return im.fail(what + why, it.line);
        for (const NamedDrawing& d : c.drawings)
            if (!a->rig.add_drawing(d.name, prefix_svg_ids(d.svg, it.id), why)) return im.fail(what + why, it.line);
        // Its parts turn round the pivots its character's do, in each drawing.
        auto pivots = [&](const Rig& from, Rig& to) {
            const std::span<const Part> parts = from.parts();
            for (usize i = 0; i < parts.size(); ++i) to.set_pivot(it.id + "/" + parts[i].id, from.pivot(static_cast<i32>(i)));
        };
        pivots(c.rig, a->rig);
        for (const std::string& name : c.rig.drawings()) pivots(*c.rig.drawing(name), *a->rig.drawing(name));
        if (!im.rig.host(it.id, a->rig, why)) return im.fail(what + why, it.line);
        im.rig.set_pivot(it.id, {it.x, it.y});
        im.actors.push_back(std::move(a));
    }
    im.script.set_scene(&im.stage, &im.hooks);
    im.ok = true;
    return true;
}

bool Scene::loaded() const { return impl_->ok; }

bool Scene::record() {
    Impl& im = *impl_;
    im.recorded = false;
    im.rec = {};
    if (!im.ok) return im.error.empty() ? im.fail("сцена не загружена") : false;
    im.error.clear();
    im.error_line = 0;
    Recording rec;
    if (!im.script.record_scene(im.rig, im.stage, im.hooks, im.doc.params, im.doc.seed, im.doc.lengths, rec))
        return im.fail(im.script.error(), im.script.error_line());
    im.rec = std::move(rec);
    im.recorded = true;
    return true;
}

const Recording* Scene::recording() const { return impl_->recorded ? &impl_->rec : nullptr; }

bool Scene::frame(f64 t, std::span<const f64> held, i32 waiting) {
    Impl& im = *impl_;
    if (!im.ok) return im.error.empty() ? im.fail("сцена не загружена") : false;
    Pose scene;
    if (im.recorded && !im.script.frame(im.rec, &im.rig, t, im.doc.params, im.doc.seed, scene))
        return im.fail(im.script.error(), im.script.error_line());
    return pose(t, im.recorded ? &im.rec : nullptr, scene, held, waiting);
}

bool Scene::pose(f64 t, const Recording* rec, const Pose& scene, std::span<const f64> held, i32 waiting) {
    Impl& im = *impl_;
    if (!im.ok) return im.error.empty() ? im.fail("сцена не загружена") : false;
    im.error.clear();
    im.error_line = 0;
    ScenePoses ps;
    bool ok = im.compose(t, rec, scene, held, waiting, ps);
    im.camera = ps.camera;
    for (usize i = 0; i < std::size(kLayerParts); ++i) im.layers[i] = ps.layers[i];
    im.phase = ps.phase;
    std::string why;
    for (const auto& a : im.actors) {
        why.clear();
        if (!a->rig.pose(ps.actors[a->id], why)) ok = im.fail("актёр «" + a->id + "»: " + why);
    }
    why.clear();
    if (!im.rig.pose(ps.stage, why)) ok = im.fail(why);
    return ok;
}

bool Scene::poses(f64 t, std::span<const f64> held, i32 waiting, ScenePoses& out) {
    Impl& im = *impl_;
    out = {};
    if (!im.ok) return im.error.empty() ? im.fail("сцена не загружена") : false;
    if (!im.recorded) return im.fail("сцена не записана");
    im.error.clear();
    im.error_line = 0;
    Pose scene;
    if (!im.script.frame(im.rec, &im.rig, t, im.doc.params, im.doc.seed, scene)) return im.fail(im.script.error(), im.script.error_line());
    return im.compose(t, &im.rec, scene, held, waiting, out);
}

assets::CookedTexture Scene::draw(u32 w, u32 h, const Affine& px) { return impl_->ok ? impl_->rig.draw(w, h, px) : assets::CookedTexture{}; }
assets::CookedTexture Scene::draw(u32 w, u32 h) { return impl_->ok ? impl_->rig.draw(w, h) : assets::CookedTexture{}; }

const SceneCamera& Scene::camera() const { return impl_->camera; }
Affine Scene::camera_matrix() const { return pixel::camera_matrix(impl_->camera, impl_->stage.width(), impl_->stage.height()); }

namespace {

// Straight RGBA b over a, pixel by pixel.
void over(std::vector<u8>& a, const std::vector<u8>& b) {
    for (usize i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4) {
        const u32 sa = b[i + 3];
        if (sa == 0) continue;
        if (sa == 255) {
            std::copy_n(b.begin() + static_cast<std::ptrdiff_t>(i), 4, a.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const u32 da = a[i + 3];
        const u32 oa = sa * 255 + da * (255 - sa); // alpha times 255²
        if (oa == 0) continue;
        for (int c = 0; c < 3; ++c) a[i + c] = static_cast<u8>((b[i + c] * sa * 255 + a[i + c] * da * (255 - sa) + oa / 2) / oa);
        a[i + 3] = static_cast<u8>((oa + 127) / 255);
    }
}

} // namespace

assets::CookedTexture Scene::draw_frame(u32 w, u32 h, const Affine& px) {
    Impl& im = *impl_;
    if (!im.ok) return {};
    const f64 cw = im.stage.width(), ch = im.stage.height();
    const Affine look = camera_matrix();
    auto scene = [&](const Affine& m) { return im.rig.draw(w, h, m * look); };
    auto outside = [&](const Affine& m) { return draw_rect(w, h, cw, ch, kOutsideScene, m); };
    using Phase = ScenePhase;
    if (im.phase == Phase::Scene) return scene(px);
    if (im.phase == Phase::Gone) return outside(px);
    // A transition: its frames, each in its pose round the frame's middle, faded and masked, the new over the old.
    const bool enter = im.phase == Phase::Enter;
    const PartPose& old_pose = im.layers[enter ? 0 : 2];
    const PartPose& new_pose = im.layers[enter ? 1 : 3];
    auto layer = [&](const PartPose& pp, bool is_scene) {
        PartPose moved;
        moved.x = pp.x, moved.y = pp.y, moved.rot = pp.rot, moved.scale = pp.scale;
        const Affine m = px * pose_matrix(moved, {cw / 2, ch / 2});
        assets::CookedTexture t = is_scene ? scene(m) : outside(m);
        const f64 opacity = std::clamp(pp.opacity, 0.0, 1.0);
        std::vector<u8> mask;
        if (!pp.mask.empty()) mask = im.rig.area(im.rig.part(pp.mask), w, h, px * look);
        for (usize i = 0; i * 4 + 3 < t.rgba8.size(); ++i) {
            f64 a = t.rgba8[i * 4 + 3] * opacity;
            if (!pp.mask.empty()) {
                const f64 cover = i < mask.size() ? mask[i] / 255.0 : 0;
                a *= pp.mask_invert ? 1 - cover : cover;
            }
            t.rgba8[i * 4 + 3] = static_cast<u8>(std::clamp(std::round(a), 0.0, 255.0));
        }
        return t;
    };
    assets::CookedTexture out = layer(old_pose, !enter);
    const assets::CookedTexture top = layer(new_pose, enter);
    over(out.rgba8, top.rgba8);
    return out;
}

assets::CookedTexture Scene::draw_frame(u32 w, u32 h) {
    const f64 cw = impl_->stage.width(), ch = impl_->stage.height();
    const f64 k = std::min(w / cw, h / ch);
    return draw_frame(w, h, translation((w - cw * k) / 2, (h - ch * k) / 2) * Affine{k, 0, 0, k, 0, 0});
}

const Stage& Scene::stage() const { return impl_->stage; }
const SceneDoc& Scene::doc() const { return impl_->doc; }
PoseScript& Scene::script() { return impl_->script; }
Rig& Scene::rig() { return impl_->rig; }
const Rig& Scene::rig() const { return impl_->rig; }

PoseScript* Scene::actor_script(const ActorPlay& play, std::map<std::string, f64>& values, std::string& why) {
    Impl& im = *impl_;
    const Impl::Actor* a = im.actor(play.actor);
    if (!a) {
        why = "на сцене нет актёра «" + play.actor + "»";
        return nullptr;
    }
    values = im.values(*a, play);
    return im.script_for(*a->character, values, why);
}

const Rig* Scene::actor_rig(std::string_view id) const {
    const Impl::Actor* a = impl_->actor(id);
    return a ? &a->rig : nullptr;
}

bool Scene::actor_reach(std::string_view id, f64 out[4]) {
    Impl::Actor* a = impl_->actor(id);
    if (!a) return out[0] = out[1] = out[2] = out[3] = 0, false;
    return a->rig.reach({}, out);
}

const SceneCharacter* Scene::character(std::string_view actor) const {
    const Impl::Actor* a = impl_->actor(actor);
    return a ? a->character : nullptr;
}

bool Scene::actor_marks(const Recording& rec, std::vector<Mark>& out) {
    out.clear();
    std::string why;
    if (!pixel::actor_marks(rec, *this, out, why)) return out.clear(), impl_->fail(why);
    return true;
}

bool Scene::marks(std::vector<Mark>& out) {
    out.clear();
    Impl& im = *impl_;
    if (!im.recorded) return true;
    std::vector<Mark> theirs;
    if (!actor_marks(im.rec, theirs)) return false;
    out = im.rec.marks;
    out.insert(out.end(), theirs.begin(), theirs.end());
    std::stable_sort(out.begin(), out.end(), [](const Mark& a, const Mark& b) { return a.time < b.time; });
    return true;
}

const std::vector<std::string>& Scene::files() const { return impl_->read; }
const std::string& Scene::error() const { return impl_->error; }
u32 Scene::error_line() const { return impl_->error_line; }

const Recording* Scene::clip_of(const ActorPlay& play, std::string& why) {
    Impl& im = *impl_;
    const Impl::Actor* a = im.actor(play.actor);
    if (!a) {
        why = "на сцене нет актёра «" + play.actor + "»";
        return nullptr;
    }
    return im.clip(*a, play.clip, im.values(*a, play), why);
}

} // namespace forge::pixel
