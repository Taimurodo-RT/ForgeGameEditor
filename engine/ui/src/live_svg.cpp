#include "forge/ui/live_svg.h"

#include "forge/vector/morph.h"
#include "forge/vector/path.h"
#include "forge/vector/svg_shape.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/SVG/LivePicture.h>
#include <lunasvg.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace forge::ui {

namespace {

// The presentation attributes a pose set on a picture, by element of the file and name: the next frame puts them back
// to the file's before it sets its own.
struct Written {
    u64 file = 0; // File::serial: the file as it read then
    std::vector<std::pair<i32, std::string>> attributes;
};

// What the scripts said of one element, and the clip it shows (svg_pose).
struct Live {
    Rml::ObserverPtr<Rml::Element> element;
    std::map<std::string, SvgPart, std::less<>> parts;
    bool posed = false;
    SvgPose pose;
    std::map<std::string, Written> written; // by the picture's path
};

// A shape's paint as the tab's picture has it (ThorVG's): 0 … 255 channels, the alpha its opacity of the paint; a
// gradient, or what does not read as a colour, is `other` and does not mix.
struct Paint8 {
    u8 r = 0, g = 0, b = 0, a = 0;
    bool other = false;
};
// A shape of a part as drawn now: its paints, its stroke width (0 without a stroke) and the width SVG gives it.
struct LeafLook {
    Paint8 fill, stroke;
    f64 width = 0, own_width = 1;
    f64 opacity = 1;
    bool fill_set = false, stroke_set = false, morphed = false;
};

// A file as the live pictures read it: its outline, the document as the file draws it (for where a part stands
// before it is moved), and the morphs made ready.
struct File {
    std::string text;
    vector::SvgOutline outline;
    std::unique_ptr<lunasvg::Document> rest;
    std::map<std::string, vector::Morph> morphs; // key: part '\n' target
    std::set<std::string> broken;                // the same keys: the pair does not make
    vector::Shape scratch;
    std::map<i32, std::vector<i32>> leaves; // a part's shapes, by its element (svg_pose)
    std::map<i32, LeafLook> looks;          // a shape's look as the file draws it
    u64 serial = 0;                         // another each time the file is read
};

std::unordered_map<Rml::Element*, Live>& lives() {
    static std::unordered_map<Rml::Element*, Live> m;
    return m;
}

std::map<std::string, File>& files() {
    static std::map<std::string, File> m;
    return m;
}

std::set<std::string>& said() {
    static std::set<std::string> s;
    return s;
}

std::vector<std::string>& waiting() {
    static std::vector<std::string> v;
    return v;
}

void problem(std::string line) {
    if (said().insert(line).second) waiting().push_back(std::move(line));
}

std::string file_name(const std::string& path) {
    const usize slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string num(f64 v) {
    if (!std::isfinite(v)) v = 0;
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    return ec == std::errc() ? std::string(buf, end) : std::string("0");
}

File& file(const std::string& path, const std::string& text) {
    File& f = files()[path];
    if (f.rest && f.text == text) return f;
    static u64 serial = 0;
    f = {};
    f.text = text;
    f.outline = vector::scan_svg(text);
    f.rest = lunasvg::Document::loadFromData(text.data(), text.size());
    f.serial = ++serial;
    return f;
}

Live* live_of(Rml::Element* element) {
    auto& m = lives();
    const auto it = m.find(element);
    if (it == m.end()) return nullptr;
    if (!it->second.element) {
        m.erase(it); // another element at a dead one's address
        return nullptr;
    }
    return &it->second;
}

// Says something of a part: the element is live from now on and draws it on the next frame.
template <typename F>
bool say(Rml::Element* element, std::string_view part, F&& change) {
    if (!element || part.empty()) return false;
    auto& m = lives();
    for (auto it = m.begin(); it != m.end();) it = it->second.element ? std::next(it) : m.erase(it);
    Live* live = live_of(element);
    if (!live) {
        Live fresh;
        fresh.element = element->GetObserverPtr();
        live = &m.emplace(element, std::move(fresh)).first->second;
    }
    auto it = live->parts.find(part);
    if (it == live->parts.end()) it = live->parts.emplace(std::string(part), SvgPart{}).first;
    change(it->second);
    if (!Rml::SVG::IsLive(element)) Rml::SVG::SetLive(element, true); // the pictures are made again: apply runs then
    else Rml::SVG::Touch(element);
    return true;
}

// A plain colour or none: what a morph mixes. A gradient (url(...)) or currentColor stays as the file has it.
bool plain_paint(const std::string& v) {
    u8 c[3];
    return v.empty() || v == "none" || vector::parse_svg_color(v, c);
}

const vector::Morph* morph_of(File& f, const std::string& path, const std::string& part, i32 index, const std::string& target) {
    const std::string key = part + '\n' + target;
    if (const auto it = f.morphs.find(key); it != f.morphs.end()) return &it->second;
    if (f.broken.count(key)) return nullptr;
    const std::string who = "«" + file_name(path) + "», морфинг «" + part + "» в «" + target + "»: ";
    auto fail = [&](const std::string& why) {
        f.broken.insert(key);
        problem(who + why);
        return nullptr;
    };
    const i32 ti = f.outline.find(target);
    if (ti < 0) return fail("в файле нет фигуры с id «" + target + "»");
    if (f.outline.elements[static_cast<usize>(index)].tag != "path")
        return fail("морфинг меняет контур d, а «" + part + "» — <" + f.outline.elements[static_cast<usize>(index)].tag +
                    ">; нарисуйте часть контуром <path>");
    if (vector::transform_chain(f.outline, index) != vector::transform_chain(f.outline, ti))
        return fail("у фигур разные transform, их точки не в одних координатах");
    vector::Shape from, to;
    std::string why;
    if (!vector::shape_path(f.outline, index, from.path, why)) return fail(why);
    if (!vector::shape_path(f.outline, ti, to.path, why)) return fail("«" + target + "»: " + why);
    from.look = vector::look_from_svg(f.outline, index);
    to.look = vector::look_from_svg(f.outline, ti, from.look);
    return &f.morphs.emplace(key, vector::prepare(from, to)).first->second;
}

// The fill of the element and of the shapes inside it that have one of their own.
void fill_down(lunasvg::Element e, const std::string& colour, bool top) {
    const std::string& own = e.getAttribute("fill");
    if (top || (!own.empty() && own != "none" && own.rfind("url(", 0) != 0)) e.setAttribute("fill", colour);
    for (const lunasvg::Node& n : e.children())
        if (n.isElement()) fill_down(n.toElement(), colour, false);
}

void apply_part(File& f, const std::string& path, lunasvg::Document& doc, const std::string& id, const SvgPart& p) {
    const i32 index = f.outline.find(id);
    lunasvg::Element e = doc.getElementById(id);
    if (index < 0 || !e) {
        problem("«" + file_name(path) + "»: нет части с id «" + id + "»");
        return;
    }
    const vector::SvgElement& rest = f.outline.elements[static_cast<usize>(index)];
    if (!p.morph.empty()) {
        if (const vector::Morph* m = morph_of(f, path, id, index, p.morph)) {
            vector::mix(*m, p.k, f.scratch);
            e.setAttribute("d", vector::to_d(f.scratch.path));
            const vector::Look& look = f.scratch.look;
            const i32 ti = f.outline.find(p.morph);
            if (plain_paint(vector::inherited(f.outline, index, "fill")) && plain_paint(vector::inherited(f.outline, ti, "fill"))) {
                e.setAttribute("fill", look.fill.on ? vector::hex(look.fill) : "none");
                e.setAttribute("fill-opacity", num(look.fill.on ? look.fill.opacity : 0));
            }
            if (plain_paint(vector::inherited(f.outline, index, "stroke")) && plain_paint(vector::inherited(f.outline, ti, "stroke"))) {
                e.setAttribute("stroke", look.stroke.on ? vector::hex(look.stroke) : "none");
                e.setAttribute("stroke-opacity", num(look.stroke.on ? look.stroke.opacity : 0));
                e.setAttribute("stroke-width", num(look.stroke_width));
            }
            e.setAttribute("opacity", num(look.opacity));
        }
    }
    if (!p.fill.empty()) fill_down(e, p.fill, true);
    if (p.opacity >= 0) e.setAttribute("opacity", num(p.opacity));
    if (p.moved) {
        const std::string* own = rest.attr("transform");
        std::string t = "translate(" + num(p.x) + " " + num(p.y) + ")";
        if (p.turn != 0) {
            // Round the centre of the part as the file draws it, in its parent's units.
            f64 cx = 0, cy = 0;
            if (lunasvg::Element r = f.rest ? f.rest->getElementById(id) : lunasvg::Element()) {
                const lunasvg::Box b = r.getLocalBoundingBox();
                cx = b.x + b.w / 2;
                cy = b.y + b.h / 2;
            }
            t += " rotate(" + num(p.turn) + " " + num(cx) + " " + num(cy) + ")";
        }
        if (own) t += " " + *own;
        e.setAttribute("transform", t);
    }
}

// --- a clip's pose (svg_pose) ----------------------------------------------------------------

bool shape_tag(const std::string& tag) {
    return tag == "path" || tag == "rect" || tag == "circle" || tag == "ellipse" || tag == "line" || tag == "polyline" || tag == "polygon";
}

bool under(const vector::SvgOutline& o, i32 j, i32 i) {
    for (i32 k = o.elements[static_cast<usize>(j)].parent; k >= 0; k = o.elements[static_cast<usize>(k)].parent)
        if (k == i) return true;
    return false;
}

// The shapes a part draws: itself when it is one, else the shapes inside it (not those of <defs> and the like, which
// draw nothing where they are).
const std::vector<i32>& leaves_of(File& f, i32 index) {
    if (const auto it = f.leaves.find(index); it != f.leaves.end()) return it->second;
    std::vector<i32>& out = f.leaves[index];
    const vector::SvgOutline& o = f.outline;
    if (shape_tag(o.elements[static_cast<usize>(index)].tag)) {
        out.push_back(index);
        return out;
    }
    for (i32 j = index + 1; j < static_cast<i32>(o.elements.size()) && under(o, j, index); ++j)
        if (shape_tag(o.elements[static_cast<usize>(j)].tag) && !o.inside(j, "defs") && !o.inside(j, "clipPath") && !o.inside(j, "mask") &&
            !o.inside(j, "pattern") && !o.inside(j, "symbol") && !o.inside(j, "marker"))
            out.push_back(j);
    return out;
}

u8 to_u8(f64 v) { return static_cast<u8>(std::clamp(std::lround(v * 255.0), 0L, 255L)); }

Paint8 paint8(const vector::SvgOutline& o, i32 index, std::string_view prop, std::string_view opacity_prop, bool default_on) {
    const std::string v = vector::inherited(o, index, prop);
    Paint8 p;
    const u8 alpha = to_u8(std::clamp(vector::svg_number(vector::inherited(o, index, opacity_prop), 1.0), 0.0, 1.0));
    u8 c[3];
    if (v.empty()) {
        if (default_on) p.a = alpha;
    } else if (v == "none") {
    } else if (vector::parse_svg_color(v, c)) {
        p = {c[0], c[1], c[2], alpha, false};
    } else {
        p.other = true; // a gradient, currentColor
        p.a = 255;
    }
    return p;
}

const LeafLook& rest_look(File& f, i32 leaf) {
    if (const auto it = f.looks.find(leaf); it != f.looks.end()) return it->second;
    LeafLook l;
    const vector::SvgOutline& o = f.outline;
    l.fill = paint8(o, leaf, "fill", "fill-opacity", true);
    l.stroke = paint8(o, leaf, "stroke", "stroke-opacity", false);
    l.own_width = std::max(0.0, vector::svg_number(vector::inherited(o, leaf, "stroke-width"), 1.0));
    l.width = l.stroke.other || l.stroke.a > 0 ? l.own_width : 0;
    l.opacity = std::clamp(vector::svg_number(o.elements[static_cast<usize>(leaf)].property("opacity"), 1.0), 0.0, 1.0);
    return f.looks.emplace(leaf, l).first->second;
}

std::string hex6(const Paint8& p) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", p.r, p.g, p.b);
    return buf;
}

std::string matrix_text(const vector::MotionMatrix& m) {
    return "matrix(" + num(m.a) + " " + num(m.b) + " " + num(m.c) + " " + num(m.d) + " " + num(m.e) + " " + num(m.f) + ")";
}

// The presentation properties a pose sets, by element. lunasvg's Element::setAttribute sets them over the file's
// attribute and its `style` alike (and over a stylesheet's rule).
struct Writes {
    std::map<i32, std::vector<std::pair<std::string, std::string>>> by_element;
    void set(i32 element, std::string name, std::string value) {
        auto& v = by_element[element];
        for (auto& [n, x] : v)
            if (n == name) {
                x = std::move(value);
                return;
            }
        v.push_back({std::move(name), std::move(value)});
    }
};

lunasvg::Element element_at(const File& f, lunasvg::Document& doc, i32 index) {
    lunasvg::Element e = doc.getElementById(f.outline.elements[static_cast<usize>(index)].id);
    if (!e && index == 0) e = doc.documentElement();
    return e;
}

Written write_all(const File& f, lunasvg::Document& doc, const Writes& w) {
    Written out{f.serial, {}};
    for (const auto& [index, list] : w.by_element) {
        lunasvg::Element e = element_at(f, doc, index);
        if (!e) continue;
        for (const auto& [name, value] : list) {
            e.setAttribute(name, value);
            out.attributes.push_back({index, name});
        }
    }
    return out;
}

// The tab's fill of a part over a shape (forge::pixel::Rig::pose): from the shape's own colour by the share; a shape
// without a fill stays without one, a gradient changes halfway.
void fill_over(Paint8& p, const vector::MotionValue& v) {
    const f64 k = std::isfinite(v.v) ? std::clamp(v.v, 0.0, 1.0) : 1.0;
    if (k <= 0) return;
    if (!p.other && p.a == 0) return;
    if (p.other && k < 0.5) return;
    const u8 base = p.other ? 255 : p.a;
    const u8 fa = static_cast<u8>((base * v.color.a + 127) / 255);
    if (p.other || k >= 1) {
        p = {v.color.r, v.color.g, v.color.b, fa, false};
        return;
    }
    auto lerp = [k](u8 c0, u8 c1) { return static_cast<u8>(std::lround(c0 + (c1 - c0) * k)); };
    p = {lerp(p.r, v.color.r), lerp(p.g, v.color.g), lerp(p.b, v.color.b), lerp(p.a, fa), false};
}

// The stroke as the fill; a shape without one gets it at its own width, coming in from transparent.
void stroke_over(LeafLook& l, const vector::MotionValue& v) {
    const f64 k = std::clamp(v.v, 0.0, 1.0);
    if (!(k > 0)) return;
    Paint8& p = l.stroke;
    if (p.other && k < 0.5) return;
    const bool none = !p.other && (p.a == 0 || l.width <= 0);
    const u8 base = p.other || none ? 255 : p.a;
    const u8 sa = static_cast<u8>((base * v.color.a + 127) / 255);
    if (none) {
        if (l.width <= 0) l.width = l.own_width;
        p = {v.color.r, v.color.g, v.color.b, static_cast<u8>(std::lround(sa * k)), false};
    } else if (p.other || k >= 1) {
        p = {v.color.r, v.color.g, v.color.b, sa, false};
    } else {
        auto lerp = [k](u8 c0, u8 c1) { return static_cast<u8>(std::lround(c0 + (c1 - c0) * k)); };
        p = {lerp(p.r, v.color.r), lerp(p.g, v.color.g), lerp(p.b, v.color.b), lerp(p.a, sa), false};
    }
}

Written apply_pose(File& f, const std::string& path, lunasvg::Document& doc, const SvgPose& pose) {
    const vector::SvgOutline& o = f.outline;
    using vector::MotionProp;
    auto has = [](const vector::MotionPartPose& pp, MotionProp p) { return pp.has[static_cast<usize>(p)]; };
    auto val = [](const vector::MotionPartPose& pp, MotionProp p) -> const vector::MotionValue& { return pp.values[static_cast<usize>(p)]; };
    // Each part by its place in the file: a group before the parts in it, so a part's own look goes over its group's.
    std::vector<std::pair<i32, const vector::MotionPartPose*>> parts;
    for (const vector::MotionPartPose& pp : pose.parts) {
        const i32 index = o.find(pp.id);
        if (index < 0 || !doc.getElementById(pp.id)) {
            problem("«" + file_name(path) + "»: нет части с id «" + pp.id + "»");
            continue;
        }
        parts.push_back({index, &pp});
    }
    std::stable_sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Writes w;
    std::map<i32, LeafLook> looks; // the shapes the pose touches, as they are drawn now
    auto look = [&](i32 leaf) -> LeafLook& {
        auto it = looks.find(leaf);
        if (it == looks.end()) it = looks.emplace(leaf, rest_look(f, leaf)).first;
        return it->second;
    };
    // Morphs first: the shape's outline and looks become the pair's at k.
    for (const auto& [index, pp] : parts) {
        if (!has(*pp, MotionProp::Morph)) continue;
        const vector::MotionValue& m = val(*pp, MotionProp::Morph);
        if (!(m.v > 0) || m.target.empty()) continue;
        if (!shape_tag(o.elements[static_cast<usize>(index)].tag)) {
            problem("«" + file_name(path) + "», морфинг «" + pp->id + "»: это группа, морфинг бывает только у фигуры");
            continue;
        }
        const vector::Morph* mo = morph_of(f, path, pp->id, index, m.target);
        if (!mo) continue;
        vector::mix(*mo, m.v, f.scratch);
        w.set(index, "d", vector::to_d(f.scratch.path));
        LeafLook& l = look(index);
        const vector::Look& lk = f.scratch.look;
        const i32 ti = o.find(m.target);
        if (plain_paint(vector::inherited(o, index, "fill")) && plain_paint(vector::inherited(o, ti, "fill"))) {
            l.fill = {to_u8(lk.fill.r), to_u8(lk.fill.g), to_u8(lk.fill.b), lk.fill.on ? to_u8(lk.fill.opacity) : u8(0), false};
            l.fill_set = true;
        }
        if (plain_paint(vector::inherited(o, index, "stroke")) && plain_paint(vector::inherited(o, ti, "stroke"))) {
            l.stroke = {to_u8(lk.stroke.r), to_u8(lk.stroke.g), to_u8(lk.stroke.b), lk.stroke.on ? to_u8(lk.stroke.opacity) : u8(0), false};
            l.width = lk.stroke.on ? lk.stroke_width : 0;
            l.stroke_set = true;
        }
        l.opacity = lk.opacity;
        l.morphed = true;
    }
    // Then each part: its matrix, its opacity, its shapes' fill, stroke and width, whether it shows.
    for (const auto& [index, pp] : parts) {
        const vector::SvgElement& rest = o.elements[static_cast<usize>(index)];
        if (pp->moves) w.set(index, "transform", matrix_text(pp->local));
        if (has(*pp, MotionProp::Opacity)) {
            const auto it = looks.find(index);
            const f64 own = it != looks.end() && it->second.morphed ? it->second.opacity
                                                                     : std::clamp(vector::svg_number(rest.property("opacity"), 1.0), 0.0, 1.0);
            w.set(index, "opacity", num(own * std::clamp(val(*pp, MotionProp::Opacity).v, 0.0, 1.0)));
        }
        if (has(*pp, MotionProp::Fill) && val(*pp, MotionProp::Fill).v > 0)
            for (i32 leaf : leaves_of(f, index)) {
                LeafLook& l = look(leaf);
                const Paint8 before = l.fill;
                fill_over(l.fill, val(*pp, MotionProp::Fill));
                l.fill_set = l.fill_set || before.r != l.fill.r || before.g != l.fill.g || before.b != l.fill.b || before.a != l.fill.a ||
                             before.other != l.fill.other;
            }
        if (has(*pp, MotionProp::Stroke) && val(*pp, MotionProp::Stroke).v > 0)
            for (i32 leaf : leaves_of(f, index)) {
                LeafLook& l = look(leaf);
                stroke_over(l, val(*pp, MotionProp::Stroke));
                l.stroke_set = true;
            }
        if (has(*pp, MotionProp::StrokeWidth) && val(*pp, MotionProp::StrokeWidth).v > 0)
            for (i32 leaf : leaves_of(f, index)) {
                LeafLook& l = look(leaf);
                const vector::MotionValue& v = val(*pp, MotionProp::StrokeWidth);
                const f64 own = l.width > 0 ? l.width : l.own_width;
                l.width = std::max(0.0, own + (v.n - own) * v.v);
                l.stroke_set = true;
            }
        if (has(*pp, MotionProp::Visible)) {
            const std::string shown = rest.property("display");
            w.set(index, "display", val(*pp, MotionProp::Visible).v > 0 ? (shown.empty() ? std::string("inline") : shown) : std::string("none"));
        }
    }
    for (const auto& [leaf, l] : looks) {
        if (l.fill_set && !l.fill.other) {
            w.set(leaf, "fill", l.fill.a > 0 || !l.morphed ? hex6(l.fill) : std::string("none"));
            w.set(leaf, "fill-opacity", num(l.fill.a / 255.0));
        }
        if (l.stroke_set && !l.stroke.other) {
            w.set(leaf, "stroke", hex6(l.stroke));
            w.set(leaf, "stroke-opacity", num(l.stroke.a / 255.0));
            w.set(leaf, "stroke-width", num(l.width));
        }
        // A morphed shape without its own opacity track: the pair's opacity at k.
        if (l.morphed) {
            bool own_track = false;
            for (const auto& [index, pp] : parts)
                if (index == leaf && has(*pp, MotionProp::Opacity)) own_track = true;
            if (!own_track) w.set(leaf, "opacity", num(l.opacity));
        }
    }
    return write_all(f, doc, w);
}

void apply(Rml::Element* element, const Rml::String& path, const Rml::String& text, lunasvg::Document& doc) {
    Live* live = live_of(element);
    if (!live) return;
    File& f = file(path, text);
    if (!f.outline.error.empty()) {
        problem("«" + file_name(path) + "»: " + f.outline.error);
        return;
    }
    // What the last frame's pose set goes back to the file's first (the attribute or the `style` the file gives it, ""
    // when it gives none: inherited or the initial value), so a frame does not build on the last one: a fill or a morph
    // back at 0, a property the pose leaves.
    if (const auto it = live->written.find(path); it != live->written.end()) {
        if (it->second.file == f.serial)
            for (const auto& [index, name] : it->second.attributes)
                if (lunasvg::Element e = element_at(f, doc, index)) e.setAttribute(name, f.outline.elements[static_cast<usize>(index)].property(name));
        live->written.erase(it);
    }
    for (const auto& [id, part] : live->parts) apply_part(f, path, doc, id, part);
    if (live->posed && file_name(path) == live->pose.picture) live->written[path] = apply_pose(f, path, doc, live->pose);
}

} // namespace

bool svg_move(Rml::Element* element, std::string_view part, f64 x, f64 y) {
    return say(element, part, [&](SvgPart& p) {
        p.moved = true;
        p.x = std::isfinite(x) ? x : 0;
        p.y = std::isfinite(y) ? y : 0;
    });
}

bool svg_turn(Rml::Element* element, std::string_view part, f64 degrees) {
    return say(element, part, [&](SvgPart& p) {
        p.moved = true;
        p.turn = std::isfinite(degrees) ? degrees : 0;
    });
}

bool svg_color(Rml::Element* element, std::string_view part, std::string_view color, std::string* why) {
    u8 c[3];
    if (!vector::parse_svg_color(color, c)) {
        if (why) *why = "«" + std::string(color) + "» — не цвет: нужен \"#rrggbb\", \"#rgb\", \"rgb(r, g, b)\" или имя цвета";
        return false;
    }
    return say(element, part, [&](SvgPart& p) { p.fill = vector::hex(vector::rgb8(c[0], c[1], c[2])); });
}

bool svg_fade(Rml::Element* element, std::string_view part, f64 opacity) {
    return say(element, part, [&](SvgPart& p) { p.opacity = std::isfinite(opacity) ? std::clamp(opacity, 0.0, 1.0) : 1.0; });
}

bool svg_morph(Rml::Element* element, std::string_view part, std::string_view target, f64 k) {
    if (target.empty()) return false;
    return say(element, part, [&](SvgPart& p) {
        p.morph = std::string(target);
        p.k = std::isfinite(k) ? k : 0;
    });
}

void svg_reset(Rml::Element* element) {
    if (!element) return;
    lives().erase(element);
    if (Rml::SVG::IsLive(element)) Rml::SVG::SetLive(element, false);
}

void svg_reset_all(Rml::Element* root) {
    if (!root) return;
    std::vector<Rml::Element*> under;
    for (const auto& [element, live] : lives()) {
        if (!live.element) continue; // gone: live_of lets it go when it is asked
        for (Rml::Element* e = element; e; e = e->GetParentNode())
            if (e == root) {
                under.push_back(element);
                break;
            }
    }
    for (Rml::Element* element : under) svg_reset(element);
}

const SvgPart* svg_part(Rml::Element* element, std::string_view part) {
    Live* live = live_of(element);
    if (!live) return nullptr;
    const auto it = live->parts.find(part);
    return it == live->parts.end() ? nullptr : &it->second;
}

std::vector<std::string> svg_problems() {
    std::vector<std::string> out;
    out.swap(waiting());
    return out;
}

void svg_pose(Rml::Element* element, SvgPose pose) {
    if (!element) return;
    auto& m = lives();
    for (auto it = m.begin(); it != m.end();) it = it->second.element ? std::next(it) : m.erase(it);
    Live* live = live_of(element);
    if (!live) {
        Live fresh;
        fresh.element = element->GetObserverPtr();
        live = &m.emplace(element, std::move(fresh)).first->second;
    }
    live->posed = true;
    live->pose = std::move(pose);
    if (!Rml::SVG::IsLive(element)) Rml::SVG::SetLive(element, true); // the pictures are made again: apply runs then
    else Rml::SVG::Touch(element);
}

void svg_unpose(Rml::Element* element) {
    Live* live = live_of(element);
    if (!live || !live->posed) return;
    live->posed = false;
    live->pose = {};
    live->written.clear();
    // Made again from the files: what the clip changed goes, what svg_move and the others said comes back.
    Rml::SVG::SetLive(element, false);
    Rml::SVG::SetLive(element, true);
}

const SvgPose* svg_posed(Rml::Element* element) {
    Live* live = live_of(element);
    return live && live->posed ? &live->pose : nullptr;
}

namespace {

std::unique_ptr<lunasvg::Document> posed_document(std::string_view svg, const SvgPose& pose, std::string& why) {
    const std::string text(svg);
    std::unique_ptr<lunasvg::Document> doc = lunasvg::Document::loadFromData(text.data(), text.size());
    if (!doc) {
        why = "картинка не читается как SVG";
        return nullptr;
    }
    const std::string key = "\x01" + pose.picture; // not a path a live element has
    File& f = file(key, text);
    if (!f.outline.error.empty()) {
        why = f.outline.error;
        return nullptr;
    }
    apply_pose(f, pose.picture, *doc, pose);
    return doc;
}

} // namespace

bool render_posed_svg(std::string_view svg, const SvgPose& pose, u32 w, u32 h, std::vector<u8>& rgba, std::string& why) {
    rgba.clear();
    std::unique_ptr<lunasvg::Document> doc = posed_document(svg, pose, why);
    if (!doc) return false;
    lunasvg::Bitmap bmp = doc->renderToBitmap(static_cast<int>(w), static_cast<int>(h), 0);
    if (bmp.isNull()) return why = "картинка не рисуется", false;
    bmp.convertToRGBA();
    rgba.resize(static_cast<usize>(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        std::copy_n(bmp.data() + static_cast<usize>(y) * static_cast<usize>(bmp.stride()), static_cast<usize>(w) * 4,
                    rgba.begin() + static_cast<std::ptrdiff_t>(y) * w * 4);
    return true;
}

std::string posed_attribute(std::string_view svg, const SvgPose& pose, std::string_view id, std::string_view name) {
    std::string why;
    std::unique_ptr<lunasvg::Document> doc = posed_document(svg, pose, why);
    if (!doc) return {};
    lunasvg::Element e = doc->getElementById(std::string(id));
    return e ? e.getAttribute(std::string(name)) : std::string();
}

void install_live_svg() { Rml::SVG::SetLiveApply(&apply); }

} // namespace forge::ui
