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

// What the scripts said of one element.
struct Live {
    Rml::ObserverPtr<Rml::Element> element;
    std::map<std::string, SvgPart, std::less<>> parts;
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
    f = {};
    f.text = text;
    f.outline = vector::scan_svg(text);
    f.rest = lunasvg::Document::loadFromData(text.data(), text.size());
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
    if (!live) live = &m.emplace(element, Live{element->GetObserverPtr(), {}}).first->second;
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

void apply(Rml::Element* element, const Rml::String& path, const Rml::String& text, lunasvg::Document& doc) {
    Live* live = live_of(element);
    if (!live) return;
    File& f = file(path, text);
    if (!f.outline.error.empty()) {
        problem("«" + file_name(path) + "»: " + f.outline.error);
        return;
    }
    for (const auto& [id, part] : live->parts) apply_part(f, path, doc, id, part);
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

void install_live_svg() { Rml::SVG::SetLiveApply(&apply); }

} // namespace forge::ui
