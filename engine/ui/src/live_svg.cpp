#include "forge/ui/live_svg.h"

#include "svg_stage.h"

#include "forge/vector/morph.h"
#include "forge/vector/path.h"
#include "forge/vector/svg_shape.h"
#include "forge/vector/trig.h"

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

struct File;

// A document a live element's picture draws: the drawing it shows, read from the drawing's file or from a stage of it
// (part 5г) the clip needs; what the last frame's pose wrote on it. Part 5д: the old drawing, or in a swap also the new
// one over it.
struct Slot {
    std::string drawing;          // "" the main one, the picture's file
    const File* source = nullptr; // the drawing's file, as it read then (serial); null: nothing read yet
    u64 serial = 0;
    StageNeeds needs;    // the stage's
    File* stage = nullptr; // the stage the document is read from; null: the drawing's own text
    Written written;
    bool wrote = false;
};

// What the scripts said of one element, and the clip it shows (svg_pose).
struct Live {
    Rml::ObserverPtr<Rml::Element> element;
    std::map<std::string, SvgPart, std::less<>> parts;
    bool posed = false;
    SvgPose pose;
    std::map<std::string, Slot> base; // by the picture's path: the plugin's document
    std::map<std::string, Slot> top;  // by the picture's path: the document over it in a swap
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
    vector::Path path; // morphed: its outline now
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
    // Each element as lunasvg makes it: its place among the elements made under its parent, -1 when lunasvg makes
    // none of it; whether getElementById finds it (the first element made with that id).
    struct Made {
        i32 place = -1;
        bool by_id = false;
    };
    std::vector<Made> made;
    // A stage's file (part 5г): what each of its elements is; null for a picture's own file.
    std::shared_ptr<const Stage> stage;
    std::map<i32, vector::MotionMatrix> rest_m;    // an element's own transform
    std::map<std::string, i32, std::less<>> parts; // the element of a part's id (a stage's)
    std::map<std::string, i32, std::less<>> own;   // stage_part's, by id (own_part)
    // A drawing's file (part 5д): the text it was read from, as the pose shares it.
    std::shared_ptr<const std::string> shared;
};

// Whether lunasvg makes an element of this tag: it leaves out the others, with all inside them.
bool lunasvg_makes(const vector::SvgElement& e) {
    static const char* const kTags[] = {"a", "circle", "clipPath", "defs", "ellipse", "g", "image", "line", "linearGradient",
                                        "marker", "mask", "path", "pattern", "polygon", "polyline", "radialGradient", "rect",
                                        "stop", "style", "svg", "symbol", "text", "tspan", "use"};
    return e.prefix.empty() && std::any_of(std::begin(kTags), std::end(kTags), [&](const char* t) { return e.tag == t; });
}

std::vector<File::Made> made_of(const vector::SvgOutline& o) {
    std::vector<File::Made> made(o.elements.size());
    std::vector<i32> children(o.elements.size(), 0); // made so far under each element
    std::set<std::string_view> ids;
    for (usize i = 0; i < o.elements.size(); ++i) {
        const vector::SvgElement& e = o.elements[i];
        if (i == 0)
            made[i].place = 0;
        else if (e.parent >= 0 && made[static_cast<usize>(e.parent)].place >= 0 && lunasvg_makes(e))
            made[i].place = children[static_cast<usize>(e.parent)]++;
        if (made[i].place >= 0 && !e.id.empty()) made[i].by_id = ids.insert(e.id).second;
    }
    return made;
}

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
    // The checks' pictures (posed_documents) have a key, not a path: the picture's name after \x01.
    if (slash == std::string::npos) return !path.empty() && path[0] == '\x01' ? path.substr(1) : path;
    return path.substr(slash + 1);
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
    f.made = made_of(f.outline);
    f.rest = lunasvg::Document::loadFromData(text.data(), text.size());
    f.serial = ++serial;
    return f;
}

// A drawing's file (part 5д) by the text the pose shares: read again only when the text is another.
File& drawing_file(const std::string& key, const std::shared_ptr<const std::string>& text) {
    File& f = files()[key];
    if (f.rest && text && f.shared == text) return f;
    static const std::string none;
    File& g = file(key, text ? *text : none);
    g.shared = text;
    return g;
}

// The key of the stages an element draws from in files() (stage_file) starts so.
std::string stage_key(const void* owner) {
    char key[32];
    std::snprintf(key, sizeof key, "\x02%p\x03", owner);
    return key;
}

// The part of a drawing's own file an id names (stage_part), -1 for none.
i32 own_part(File& f, std::string_view id) {
    if (const auto it = f.own.find(id); it != f.own.end()) return it->second;
    const i32 e = stage_part(f.outline, id);
    f.own.emplace(std::string(id), e);
    return e;
}

// A character's drawings as a pose has them (part 5д): the picture's file, the main one, and the others, by name. A
// drawing's file is in files() by its picture's name beside the picture's (the key), read from the pose's text.
struct Family {
    struct Member {
        std::string name;    // "" the main one
        std::string key;     // in files(): the picture's path, or the drawing's
        std::string picture; // for the lines: the picture's path, or the drawing's file name
        File* file = nullptr;
    };
    std::vector<Member> members; // the main one first, then the others by name

    const Member* get(std::string_view name) const {
        if (name == vector::kMotionMainDrawing) name = {};
        for (const Member& m : members)
            if (m.name == name) return &m;
        return nullptr;
    }
    bool others() const { return members.size() > 1; }
    // Some drawing has a part with the id (forge::pixel::Rig::for_drawing's `known`).
    bool knows(std::string_view id) const {
        return std::any_of(members.begin(), members.end(), [&](const Member& m) { return own_part(*m.file, id) >= 0; });
    }
};

Family family_of(File& main, const std::string& path, const std::string& key, const SvgPose& pose) {
    Family fam;
    fam.members.push_back({"", path, path, &main});
    std::vector<const SvgDrawing*> ds;
    for (const SvgDrawing& d : pose.drawings)
        if (!d.name.empty() && d.name != vector::kMotionMainDrawing) ds.push_back(&d);
    std::sort(ds.begin(), ds.end(), [](const SvgDrawing* a, const SvgDrawing* b) { return a->name < b->name; });
    for (const SvgDrawing* d : ds) {
        if (fam.get(d->name)) continue;
        File& f = drawing_file(key + d->picture, d->text);
        if (!f.outline.error.empty()) {
            problem("«" + d->picture + "»: " + f.outline.error);
            continue;
        }
        fam.members.push_back({d->name, key + d->picture, d->picture, &f});
    }
    return fam;
}

// The directory of a picture's path, with its slash ("" for none).
std::string dir_of(const std::string& path) {
    const usize slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

Live* live_of(Rml::Element* element) {
    auto& m = lives();
    const auto it = m.find(element);
    if (it == m.end()) return nullptr;
    if (!it->second.element) {
        // Its stages go with it.
        const std::string key = stage_key(element);
        auto& fs = files();
        for (auto f = fs.lower_bound(key); f != fs.end() && f->first.rfind(key, 0) == 0;) f = fs.erase(f);
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

const vector::Morph* morph_of(File& f, const std::string& path, const std::string& part, i32 index, const std::string& target, i32 ti) {
    const std::string key = part + '\n' + target;
    if (const auto it = f.morphs.find(key); it != f.morphs.end()) return &it->second;
    if (f.broken.count(key)) return nullptr;
    const std::string who = "«" + file_name(path) + "», морфинг «" + part + "» в «" + target + "»: ";
    auto fail = [&](const std::string& why) {
        f.broken.insert(key);
        problem(who + why);
        return nullptr;
    };
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

// `quiet`: a drawing without the part says nothing, when another drawing of the swap has it (part 5д).
void apply_part(File& f, const std::string& path, lunasvg::Document& doc, const std::string& id, const SvgPart& p, bool quiet = false) {
    const i32 index = f.outline.find(id);
    lunasvg::Element e = doc.getElementById(id);
    if (index < 0 || !e) {
        if (!quiet) problem("«" + file_name(path) + "»: нет части с id «" + id + "»");
        return;
    }
    const vector::SvgElement& rest = f.outline.elements[static_cast<usize>(index)];
    if (!p.morph.empty()) {
        if (const vector::Morph* m = morph_of(f, path, id, index, p.morph, f.outline.find(p.morph))) {
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

bool shape_tag(const std::string& tag) { return stage_shape_tag(tag); }

bool drawable_tag(const std::string& tag) { return tag == "g" || tag == "use" || shape_tag(tag); }

// An element of the top level that draws something where it stands (SvgPlace::draws).
bool draws_tag(const std::string& tag) { return drawable_tag(tag) || tag == "image" || tag == "text" || tag == "a" || tag == "svg"; }

// SvgPlace::key of element i of the top level of the file drawn (a stage's, or the picture's own).
std::string top_key(const File& f, i32 i) {
    if (const Stage* st = f.stage.get()) {
        const std::string& copy = st->copy_of[static_cast<usize>(i)];
        if (!copy.empty()) return "c" + copy;
        const i32 own = st->file_of[static_cast<usize>(i)];
        return own >= 0 ? "e" + std::to_string(own) : std::string();
    }
    return "e" + std::to_string(i);
}

// The shapes a part draws (stage_leaves). On a stage, a part of the file does not draw its group's copies: the tab's
// fill, stroke and width of a group leave the copies in it as they are (forge::pixel::Rig::Impl::add_copy).
const std::vector<i32>& leaves_of(File& f, i32 index) {
    if (const auto it = f.leaves.find(index); it != f.leaves.end()) return it->second;
    std::vector<i32> all = stage_leaves(f.outline, index);
    if (f.stage && f.stage->copy_of[static_cast<usize>(index)].empty())
        all.erase(std::remove_if(all.begin(), all.end(), [&](i32 j) { return !f.stage->copy_of[static_cast<usize>(j)].empty(); }), all.end());
    return f.leaves[index] = std::move(all);
}

// The element a part's id names: on a stage a copy's, else the file's own part (stage_part: the first that draws,
// never one in a copy); in a picture's file the first element with the id, as in part 5в.
i32 part_element(File& f, std::string_view id) {
    if (!f.stage) return f.outline.find(id);
    if (const auto it = f.parts.find(id); it != f.parts.end()) return it->second;
    const Stage& st = *f.stage;
    i32 best = -1;
    if (const auto c = st.copy_root.find(id); c != st.copy_root.end()) best = c->second;
    else
        for (usize i = 0; i < f.outline.elements.size(); ++i) {
            const vector::SvgElement& e = f.outline.elements[i];
            if (!st.copy_of[i].empty() || e.id != id || !drawable_tag(e.tag)) continue;
            if (best < 0 || st.file_of[i] < st.file_of[static_cast<usize>(best)]) best = static_cast<i32>(i);
        }
    f.parts.emplace(std::string(id), best);
    return best;
}

// A shape a morph goes to: the file's first element with the id (on a stage, never one in a copy).
i32 target_element(File& f, std::string_view id) {
    if (!f.stage) return f.outline.find(id);
    i32 best = -1;
    for (usize i = 0; i < f.outline.elements.size(); ++i)
        if (f.stage->copy_of[i].empty() && f.outline.elements[i].id == id &&
            (best < 0 || f.stage->file_of[i] < f.stage->file_of[static_cast<usize>(best)]))
            best = static_cast<i32>(i);
    return best;
}

// SVG's transform list: matrix, translate, scale, rotate, skewX and skewY, the first outermost.
bool parse_transform(std::string_view s, vector::MotionMatrix& out) {
    out = {};
    usize i = 0;
    auto space = [&] {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    };
    auto rad = [](f64 deg, f64& sn, f64& cs) { vector::trig::sin_cos(deg * vector::trig::kPi / 180.0, sn, cs); };
    for (;;) {
        space();
        if (i >= s.size()) return true;
        const usize a = i;
        while (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) ++i;
        const std::string_view name = s.substr(a, i - a);
        space();
        if (i >= s.size() || s[i] != '(') return false;
        ++i;
        f64 v[6] = {};
        int k = 0;
        for (;;) {
            space();
            if (i < s.size() && s[i] == ')') {
                ++i;
                break;
            }
            if (k == 6 || i >= s.size()) return false;
            if (s[i] == '+') ++i;
            const auto [end, ec] = std::from_chars(s.data() + i, s.data() + s.size(), v[k]);
            if (ec != std::errc()) return false;
            i = static_cast<usize>(end - s.data());
            ++k;
        }
        vector::MotionMatrix m;
        f64 sn = 0, cs = 1;
        if (name == "matrix" && k == 6) m = {v[0], v[1], v[2], v[3], v[4], v[5]};
        else if (name == "translate" && (k == 1 || k == 2)) m = {1, 0, 0, 1, v[0], k == 2 ? v[1] : 0};
        else if (name == "scale" && (k == 1 || k == 2)) m = {v[0], 0, 0, k == 2 ? v[1] : v[0], 0, 0};
        else if (name == "rotate" && (k == 1 || k == 3)) {
            rad(v[0], sn, cs);
            m = {cs, sn, -sn, cs, 0, 0};
            if (k == 3) m = vector::MotionMatrix{1, 0, 0, 1, v[1], v[2]} * m * vector::MotionMatrix{1, 0, 0, 1, -v[1], -v[2]};
        } else if (name == "skewX" && k == 1) {
            rad(v[0], sn, cs);
            m = {1, 0, sn / cs, 1, 0, 0};
        } else if (name == "skewY" && k == 1) {
            rad(v[0], sn, cs);
            m = {1, sn / cs, 0, 1, 0, 0};
        } else return false;
        out = out * m;
    }
}

// An element's own transform as the file has it.
const vector::MotionMatrix& rest_matrix(File& f, i32 index) {
    if (const auto it = f.rest_m.find(index); it != f.rest_m.end()) return it->second;
    vector::MotionMatrix m;
    if (const std::string* t = f.outline.elements[static_cast<usize>(index)].attr("transform"))
        if (!parse_transform(*t, m)) m = {};
    return f.rest_m[index] = m;
}

// From an element's units to the picture's (the root's own, inside its viewBox), each element above it at the
// transform `now` gives it, else at its own.
vector::MotionMatrix screen(File& f, const std::map<i32, vector::MotionMatrix>& now, i32 index) {
    vector::MotionMatrix m;
    for (i32 k = index; k > 0; k = f.outline.elements[static_cast<usize>(k)].parent) {
        const auto it = now.find(k);
        m = (it != now.end() ? it->second : rest_matrix(f, k)) * m;
    }
    return m;
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
    const std::string* get(i32 element, std::string_view name) const {
        const auto it = by_element.find(element);
        if (it == by_element.end()) return nullptr;
        for (const auto& [n, x] : it->second)
            if (n == name) return &x;
        return nullptr;
    }
};

// An element of the file in the document: the root, an element lunasvg finds by its id, else (a shape without one,
// inside a part) by its place among the elements lunasvg makes under the element above it.
lunasvg::Element element_at(const File& f, lunasvg::Document& doc, i32 index) {
    const File::Made& m = f.made[static_cast<usize>(index)];
    if (m.place < 0) return {};
    if (index == 0) return doc.documentElement();
    if (m.by_id) return doc.getElementById(f.outline.elements[static_cast<usize>(index)].id);
    lunasvg::Element up = element_at(f, doc, f.outline.elements[static_cast<usize>(index)].parent);
    if (!up) return {};
    i32 place = m.place;
    for (const lunasvg::Node& n : up.children())
        if (n.isElement() && place-- == 0) return n.toElement();
    return {};
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

// A part's tint and flash (forge::pixel::Rig's Tone): the tint multiplies each channel by white + (tint − white) · k,
// the flash takes red, green and blue its share of the way to white + (colour − white) · k.
struct Tone {
    bool tint = false;
    f64 m[4] = {255, 255, 255, 255};
    f64 flash = 0;
    f64 f[3] = {255, 255, 255};

    void apply(u8& r, u8& g, u8& b, u8& a) const {
        u8* c[4] = {&r, &g, &b, &a};
        auto byte = [](f64 v) { return static_cast<u8>(std::clamp(std::lround(v), 0L, 255L)); };
        if (tint)
            for (int i = 0; i < 4; ++i) *c[i] = byte(*c[i] * m[i] / 255.0);
        if (flash > 0)
            for (int i = 0; i < 3; ++i) *c[i] = byte(*c[i] + (f[i] - *c[i]) * flash);
    }
};

bool tone_of(const vector::MotionPartPose& pp, Tone& t) {
    using vector::MotionProp;
    auto val = [&](MotionProp p) -> const vector::MotionValue& { return pp.values[static_cast<usize>(p)]; };
    auto has = [&](MotionProp p) { return pp.has[static_cast<usize>(p)]; };
    t = {};
    const f64 tk = has(MotionProp::Tint) ? std::clamp(val(MotionProp::Tint).v, 0.0, 1.0) : 0.0;
    const f64 fl = has(MotionProp::Flash) ? std::clamp(val(MotionProp::Flash).v, 0.0, 1.0) : 0.0;
    if (tk > 0) {
        t.tint = true;
        const vector::MotionColor& c = val(MotionProp::Tint).color;
        const u8 cs[4] = {c.r, c.g, c.b, c.a};
        for (int i = 0; i < 4; ++i) t.m[i] = 255 + (cs[i] - 255) * tk;
    }
    if (fl > 0) {
        t.flash = fl;
        const f64 ck = has(MotionProp::FlashColor) ? std::clamp(val(MotionProp::FlashColor).v, 0.0, 1.0) : 0.0;
        const vector::MotionColor& c = val(MotionProp::FlashColor).color;
        const u8 cs[3] = {c.r, c.g, c.b};
        if (ck > 0)
            for (int i = 0; i < 3; ++i) t.f[i] = 255 + (cs[i] - 255) * ck;
    }
    return t.tint || t.flash > 0;
}

// An opacity as ThorVG's SVG loader keeps one: 0 … 255.
u8 opacity8(const std::string& text) { return to_u8(std::clamp(vector::svg_number(text, 1.0), 0.0, 1.0)); }

// --- the character's other drawings (part 5д) ------------------------------------------------

// A shape of another drawing in a shape's units (forge::pixel::in_units): its points through the matrix, its stroke as
// wide as it looks there.
vector::Shape in_units(vector::Shape s, const vector::MotionMatrix& m) {
    for (vector::Contour& c : s.path.contours) {
        c.start = m.apply(c.start);
        for (vector::Cubic& q : c.curves) q.c1 = m.apply(q.c1), q.c2 = m.apply(q.c2), q.to = m.apply(q.to);
    }
    s.look.stroke_width *= std::sqrt(std::abs(m.a * m.d - m.b * m.c));
    return s;
}

// The pose a drawing takes of the clip's (forge::pixel::Rig::for_drawing), its parts as they stand in it: the parts it
// has and those no drawing has (their line stays); a mask or a carrier it lacks and another drawing has does not act
// there (no mask; the part carried as if its carrier were at rest, C = carry); an order step with a part or a neighbour
// it lacks is left out; a copy of a part it lacks is not born in it. Without other drawings, the pose as it is.
SvgPose for_drawing(const SvgPose& pose, const std::vector<vector::MotionPartPose>& parts, File& d, const Family& fam) {
    using vector::MotionProp;
    SvgPose out;
    out.picture = pose.picture;
    out.only = pose.only;
    out.view_x = pose.view_x, out.view_y = pose.view_y, out.view_w = pose.view_w, out.view_h = pose.view_h;
    if (!fam.others()) {
        out.parts = parts;
        out.copies = pose.copies;
        out.order = pose.order;
        return out;
    }
    auto own = [&](const std::string& id) -> const std::string& {
        for (const vector::MotionCopy& c : pose.copies)
            if (c.id == id) return c.source;
        return id;
    };
    auto takes = [&](const std::string& id) {
        const std::string& o = own(id);
        return own_part(d, o) >= 0 || !fam.knows(o);
    };
    auto lacks = [&](const std::string& id) { return own_part(d, id) < 0 && fam.knows(id); };
    for (const vector::MotionCopy& c : pose.copies)
        if (takes(c.id)) out.copies.push_back(c);
    for (const vector::MotionPartPose& pp : parts) {
        if (!takes(pp.id)) continue;
        vector::MotionPartPose& q = out.parts.emplace_back(pp);
        const usize mask = static_cast<usize>(MotionProp::Mask), link = static_cast<usize>(MotionProp::Link);
        if (q.has[mask] && !q.values[mask].target.empty() && lacks(q.values[mask].target)) {
            q.has[mask] = false;
            q.values[mask] = vector::motion_rest(MotionProp::Mask);
        }
        if (q.has[link] && !q.values[link].target.empty() && lacks(q.values[link].target)) q.values[link].target.clear();
    }
    for (const vector::MotionOrderStep& step : pose.order)
        if (takes(step.part) && (step.other.empty() || !lacks(step.other))) out.order.push_back(step);
    return out;
}

// The shape a swap pairs for an id in a drawing as drawn (forge::pixel::Rig::swap_shape): a shape part shown (neither it
// nor a group of it display: none), or a copy born of a shape part, hidden or not; -1 for none. In a drawing's own file
// (no stage) a copy's shape is its source's.
i32 swap_shape(File& f, const SvgPose& pose, const std::string& id) {
    auto shape = [&](i32 e) { return e >= 0 && shape_tag(f.outline.elements[static_cast<usize>(e)].tag); };
    for (const vector::MotionCopy& c : pose.copies)
        if (c.id == id) {
            const i32 src = part_element(f, c.source);
            if (!shape(src)) return -1;
            if (!f.stage) return src;
            const auto root = f.stage->copy_root.find(id);
            return root == f.stage->copy_root.end() ? -1 : root->second;
        }
    const i32 e = part_element(f, id);
    return shape(e) && !vector::display_none(f.outline, e) ? e : -1;
}

// A pair of a swap: one id, a shape in both drawings; its element in the old drawing and in the new.
struct Pair {
    std::string id;
    i32 a = -1, b = -1;
};

std::vector<Pair> pairs_of(File& a, const SvgPose& pa, File& b, const SvgPose& pb) {
    std::vector<Pair> out;
    std::set<std::string> seen;
    auto pair = [&](const std::string& id) {
        if (!seen.insert(id).second) return;
        const i32 x = swap_shape(a, pa, id);
        const i32 y = x >= 0 ? swap_shape(b, pb, id) : -1;
        if (y >= 0) out.push_back({id, x, y});
    };
    for (usize i = 0; i < a.outline.elements.size(); ++i)
        if (!a.outline.elements[i].id.empty() && (!a.stage || a.stage->copy_of[i].empty())) pair(a.outline.elements[i].id);
    for (const vector::MotionCopy& c : pa.copies) pair(c.id);
    return out;
}

// What a drawing does in a swap: the new one hides its pairs and says what they are now; the old one morphs each of its
// pairs from what it is now to that, in its units, by k. Every other shape fades: the old by 1 − k, the new by k.
struct SwapSide {
    struct Now {
        vector::Shape shape; // in its own units
        bool morphed = false; // by the clip
    };
    bool from = false; // the old drawing
    f64 k = 0;
    const std::vector<Pair>* pairs = nullptr;
    File* other = nullptr;  // the other drawing as drawn
    std::string other_name; // its name, for the key of a pair's morph
    std::map<i32, Now>* now = nullptr; // the new drawing's pairs, by their element there
};

// The morph into a shape of another drawing (forge::pixel::Rig::pose_alone): the main drawing's when it has the id, else
// the first other's by name; the target's look its own, in the morphing shape's units. Null with a line when it does not
// make.
const vector::Morph* cross_morph(File& f, const std::string& path, const std::string& part, i32 index, const std::string& target,
                                 const Family& fam, File*& tf, i32& ti) {
    const Family::Member* holder = nullptr;
    for (const Family::Member& m : fam.members)
        if (m.file->outline.find(target) >= 0) {
            holder = &m;
            break;
        }
    if (!holder) return morph_of(f, path, part, index, target, -1); // its line: no such shape
    tf = holder->file;
    ti = tf->outline.find(target);
    const std::string key = part + '\n' + target + "\n@" + holder->name + '\n' + std::to_string(tf->serial);
    if (const auto it = f.morphs.find(key); it != f.morphs.end()) return &it->second;
    if (f.broken.count(key)) return nullptr;
    const std::string who = "«" + file_name(path) + "», морфинг «" + part + "» в «" + target + "»: ";
    auto fail = [&](const std::string& why) {
        f.broken.insert(key);
        problem(who + why);
        return nullptr;
    };
    if (!shape_tag(tf->outline.elements[static_cast<usize>(ti)].tag))
        return fail("в рисунке «" + (holder->name.empty() ? std::string(vector::kMotionMainDrawing) : holder->name) + "» это не фигура");
    if (f.outline.elements[static_cast<usize>(index)].tag != "path")
        return fail("морфинг меняет контур d, а «" + part + "» — <" + f.outline.elements[static_cast<usize>(index)].tag +
                    ">; нарисуйте часть контуром <path>");
    vector::Shape from, to;
    std::string why;
    if (!vector::shape_path(f.outline, index, from.path, why)) return fail(why);
    if (!vector::shape_path(tf->outline, ti, to.path, why)) return fail("«" + target + "»: " + why);
    from.look = vector::look_from_svg(f.outline, index);
    to.look = vector::look_from_svg(tf->outline, ti, from.look);
    to = in_units(std::move(to), vector::inverse(screen(f, {}, index)) * screen(*tf, {}, ti));
    return &f.morphs.emplace(key, vector::prepare(from, to)).first->second;
}

// `fam` and `self` (part 5д): the drawings a morph finds a shape this drawing has not in, when it is not the main one;
// `side`: what it does in a swap.
Written apply_pose(File& f, const std::string& path, lunasvg::Document& doc, const SvgPose& pose, const Family* fam = nullptr,
                   std::string_view self = {}, const SwapSide* side = nullptr) {
    const vector::SvgOutline& o = f.outline;
    const Stage* st = f.stage.get();
    using vector::MotionProp;
    auto has = [](const vector::MotionPartPose& pp, MotionProp p) { return pp.has[static_cast<usize>(p)]; };
    auto val = [](const vector::MotionPartPose& pp, MotionProp p) -> const vector::MotionValue& { return pp.values[static_cast<usize>(p)]; };
    auto parent_of = [&](i32 i) { return o.elements[static_cast<usize>(i)].parent; };
    // Each part by its element.
    std::vector<std::pair<i32, const vector::MotionPartPose*>> parts;
    std::map<i32, const vector::MotionPartPose*> posed;
    for (const vector::MotionPartPose& pp : pose.parts) {
        const i32 index = part_element(f, pp.id);
        if (index < 0 || !element_at(f, doc, index)) {
            problem("«" + file_name(path) + "»: нет части с id «" + pp.id + "»");
            continue;
        }
        if (!posed.emplace(index, &pp).second) continue;
        parts.push_back({index, &pp});
    }
    Writes w;
    std::map<i32, LeafLook> looks; // the shapes the pose touches, as they are drawn now
    auto look = [&](i32 leaf) -> LeafLook& {
        auto it = looks.find(leaf);
        if (it == looks.end()) it = looks.emplace(leaf, rest_look(f, leaf)).first;
        return it->second;
    };

    // Copies (part 5г): shown from their birth, as the tab shows them; a copy of a hidden shape shows, one of a hidden
    // group shows nothing (ThorVG reads none of what is inside it).
    if (st)
        for (const auto& [id, root] : st->copy_root) {
            const bool drawn = std::any_of(pose.copies.begin(), pose.copies.end(), [&](const vector::MotionCopy& c) { return c.id == id; });
            const vector::SvgElement& e = o.elements[static_cast<usize>(root)];
            const std::string shown = e.property("display");
            const bool hidden = shown == "none" && !shape_tag(e.tag);
            w.set(root, "display", drawn && !hidden ? (shown.empty() || shown == "none" ? std::string("inline") : shown) : std::string("none"));
        }

    // A shape's outline and looks become a morph's at k: the clip's, then a swap's.
    auto put_morph = [&](i32 index, const vector::SvgOutline& to, i32 ti) {
        w.set(index, "d", vector::to_d(f.scratch.path));
        LeafLook& l = look(index);
        const vector::Look& lk = f.scratch.look;
        if (plain_paint(vector::inherited(o, index, "fill")) && plain_paint(vector::inherited(to, ti, "fill"))) {
            l.fill = {to_u8(lk.fill.r), to_u8(lk.fill.g), to_u8(lk.fill.b), lk.fill.on ? to_u8(lk.fill.opacity) : u8(0), false};
            l.fill_set = true;
        }
        if (plain_paint(vector::inherited(o, index, "stroke")) && plain_paint(vector::inherited(to, ti, "stroke"))) {
            l.stroke = {to_u8(lk.stroke.r), to_u8(lk.stroke.g), to_u8(lk.stroke.b), lk.stroke.on ? to_u8(lk.stroke.opacity) : u8(0), false};
            l.width = lk.stroke.on ? lk.stroke_width : 0;
            l.stroke_set = true;
        }
        l.opacity = lk.opacity;
        l.morphed = true;
        l.path = f.scratch.path;
    };

    // Morphs first: the shape's outline and looks become the pair's at k. A shape of another drawing is a target too
    // (part 5д), when this one has none with the id.
    std::map<i32, vector::Shape> morphed; // the shapes the clip morphs, as they are now
    for (const auto& [index, pp] : parts) {
        if (!has(*pp, MotionProp::Morph)) continue;
        const vector::MotionValue& m = val(*pp, MotionProp::Morph);
        if (!(m.v > 0) || m.target.empty()) continue;
        if (!shape_tag(o.elements[static_cast<usize>(index)].tag)) {
            problem("«" + file_name(path) + "», морфинг «" + pp->id + "»: это группа, морфинг бывает только у фигуры");
            continue;
        }
        i32 ti = target_element(f, m.target);
        File* tf = &f;
        const vector::Morph* mo = ti < 0 && fam && !self.empty() ? cross_morph(f, path, pp->id, index, m.target, *fam, tf, ti)
                                                                  : morph_of(f, path, pp->id, index, m.target, ti);
        if (!mo) continue;
        vector::mix(*mo, m.v, f.scratch);
        put_morph(index, tf->outline, ti);
        morphed[index] = f.scratch;
    }

    // A swap (part 5д): the new drawing tells what its pairs are now; the old one morphs each of its pairs from what it
    // is now to that, in its units (the chains of transforms at rest of both shapes to the picture's units).
    if (side)
        for (const Pair& pr : *side->pairs) {
            const i32 e = side->from ? pr.a : pr.b;
            auto now_of = [&](vector::Shape& out) {
                if (const auto it = morphed.find(e); it != morphed.end()) {
                    out = it->second;
                    return true;
                }
                std::string why;
                out.look = vector::look_from_svg(o, e);
                if (vector::shape_path(o, e, out.path, why)) return true;
                problem("«" + file_name(path) + "», смена рисунка, «" + pr.id + "»: " + why);
                return false;
            };
            if (!side->from) {
                SwapSide::Now n;
                n.morphed = morphed.count(e) > 0;
                if (now_of(n.shape)) (*side->now)[e] = std::move(n);
                continue;
            }
            const auto to = side->now->find(pr.b);
            if (to == side->now->end()) continue;
            if (o.elements[static_cast<usize>(e)].tag != "path") {
                problem("«" + file_name(path) + "», смена рисунка, «" + pr.id + "»: фигура не стала контуром");
                continue;
            }
            vector::Shape from;
            if (!now_of(from)) continue;
            const vector::Shape target = in_units(to->second.shape, vector::inverse(screen(f, {}, e)) * screen(*side->other, {}, pr.b));
            // The same pair at rest again and again: made ready once.
            const vector::Morph* mo = nullptr;
            vector::Morph fresh;
            if (!morphed.count(e) && !to->second.morphed) {
                const std::string key = "\x01" + pr.id + '\n' + side->other_name + '\n' + std::to_string(side->other->serial);
                auto it = f.morphs.find(key);
                if (it == f.morphs.end()) it = f.morphs.emplace(key, vector::prepare(from, target)).first;
                mo = &it->second;
            } else {
                fresh = vector::prepare(from, target);
                mo = &fresh;
            }
            vector::mix(*mo, std::clamp(side->k, 0.0, 1.0), f.scratch);
            put_morph(e, side->other->outline, pr.b);
        }

    // Links (part 5г): the part carrying each, a link that comes back round to its own part dropped (as the tab does).
    std::map<i32, i32> carrier;
    for (const auto& [index, pp] : parts) {
        if (!has(*pp, MotionProp::Link) || val(*pp, MotionProp::Link).target.empty()) continue;
        const i32 c = part_element(f, val(*pp, MotionProp::Link).target);
        if (c < 0) problem("«" + file_name(path) + "», крепление «" + pp->id + "»: нет части «" + val(*pp, MotionProp::Link).target + "»");
        else carrier[index] = c;
    }
    // What a part's place waits for: the posed parts it is inside, and its carrier with the posed parts that one is
    // inside (the file has no track of a carrier at rest, which the tab poses at rest and places after its groups).
    auto deps = [&](i32 p, std::vector<i32>& out) {
        out.clear();
        for (i32 q = parent_of(p); q >= 0; q = parent_of(q))
            if (posed.count(q)) out.push_back(q);
        if (const auto c = carrier.find(p); c != carrier.end()) {
            out.push_back(c->second);
            for (i32 q = parent_of(c->second); q >= 0; q = parent_of(q))
                if (posed.count(q)) out.push_back(q);
        }
    };
    std::vector<i32> work, next;
    for (auto it = carrier.begin(); it != carrier.end();) {
        std::set<i32> seen;
        bool back = false;
        deps(it->first, work);
        while (!work.empty() && !back) {
            const i32 q = work.back();
            work.pop_back();
            if (q == it->first) back = true;
            if (back || !seen.insert(q).second) continue;
            deps(q, next);
            work.insert(work.end(), next.begin(), next.end());
        }
        if (back) {
            problem("«" + file_name(path) + "»: крепление по кругу: «" + o.elements[static_cast<usize>(it->first)].id + "» и «" +
                    o.elements[static_cast<usize>(it->second)].id + "» несут друг друга");
            it = carrier.erase(it);
        } else ++it;
    }
    // Each part after what it waits for (a group before the parts in it, so a part's own look goes over its group's).
    std::vector<std::pair<i32, const vector::MotionPartPose*>> order;
    {
        std::map<i32, int> placed; // 1 placed, 2 on the way
        std::vector<std::pair<i32, usize>> stack;
        for (const auto& [index, pp] : parts) {
            if (placed.count(index)) continue;
            stack.push_back({index, 0});
            while (!stack.empty()) {
                auto& [q, i] = stack.back();
                deps(q, work);
                while (i < work.size() && (placed.count(work[i]) || !posed.count(work[i]))) ++i;
                if (i < work.size()) {
                    const i32 d = work[i];
                    placed[d] = 2;
                    stack.push_back({d, 0});
                    continue;
                }
                if (placed[q] != 1) order.push_back({q, posed[q]});
                placed[q] = 1;
                stack.pop_back();
            }
        }
    }

    // Then each part: its matrix, its opacity, its shapes' fill, stroke, width, dash and drawn share, whether it shows.
    std::map<i32, vector::MotionMatrix> now; // the transforms written, for where elements stand now
    for (const auto& [index, pp] : order) {
        const vector::SvgElement& rest = o.elements[static_cast<usize>(index)];
        vector::MotionMatrix local = pp->moves ? pp->local : rest_matrix(f, index);
        bool placed = pp->moves;
        if (has(*pp, MotionProp::Link)) {
            const vector::MotionValue& link = val(*pp, MotionProp::Link);
            const auto c = carrier.find(index);
            if (c != carrier.end() || link.m != vector::MotionMatrix{}) {
                // C over its groups: the part's place in picture units as it would stand, then C.
                vector::MotionMatrix carry = link.m;
                if (c != carrier.end()) {
                    vector::MotionMatrix at_rest;
                    for (i32 k = c->second; k > 0; k = parent_of(k)) at_rest = rest_matrix(f, k) * at_rest;
                    carry = screen(f, now, c->second) * vector::inverse(at_rest) * carry;
                }
                const vector::MotionMatrix pg = screen(f, now, parent_of(index));
                local = vector::inverse(pg) * carry * pg * local;
                placed = true;
            }
        }
        if (placed) {
            now[index] = local;
            w.set(index, "transform", matrix_text(local));
        }
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
        // The dash: each shape's own pattern moved along its line.
        if (has(*pp, MotionProp::Dash) && val(*pp, MotionProp::Dash).v != 0)
            for (i32 leaf : leaves_of(f, index)) {
                const std::string pattern = vector::inherited(o, leaf, "stroke-dasharray");
                if (pattern.empty() || pattern == "none") continue;
                w.set(leaf, "stroke-dashoffset", num(vector::svg_number(vector::inherited(o, leaf, "stroke-dashoffset"), 0.0) + val(*pp, MotionProp::Dash).v));
            }
        // The share of each shape's outline drawn, its contours one line after another; nothing when from ≥ to.
        const f64 from = has(*pp, MotionProp::DrawFrom) ? val(*pp, MotionProp::DrawFrom).v : 0.0;
        const f64 to = has(*pp, MotionProp::DrawTo) ? val(*pp, MotionProp::DrawTo).v : 1.0;
        if (from != 0 || to != 1) {
            const f64 a = std::clamp(from, 0.0, 1.0), b = std::clamp(to, 0.0, 1.0);
            for (i32 leaf : leaves_of(f, index)) {
                if (o.elements[static_cast<usize>(leaf)].tag != "path") {
                    problem("«" + file_name(path) + "», прорисовка «" + pp->id + "»: фигура не стала контуром");
                    continue;
                }
                const auto lk = looks.find(leaf);
                const bool bent = lk != looks.end() && lk->second.morphed;
                vector::Path whole;
                std::string why;
                if (bent) whole = lk->second.path;
                else if (!vector::shape_path(o, leaf, whole, why)) continue;
                const vector::Path part = a < b ? vector::trim(whole, a, b, !bent) : vector::Path{};
                w.set(leaf, "d", part.contours.empty() ? std::string("M0 0") : vector::to_d(part, !bent));
            }
        }
        if (has(*pp, MotionProp::Visible)) {
            const bool copy = st && !st->copy_of[static_cast<usize>(index)].empty() && st->copy_root.count(pp->id);
            if (val(*pp, MotionProp::Visible).v <= 0) w.set(index, "display", "none");
            else if (!copy) {
                const std::string shown = rest.property("display");
                w.set(index, "display", shown.empty() ? std::string("inline") : shown);
            }
        }
    }

    // Tints and flashes: the colours of each shape, its own part's first, then its groups' (copies in them too).
    std::set<i32> toned_leaves;
    for (const auto& [index, pp] : parts) {
        Tone t;
        if (!tone_of(*pp, t)) continue;
        for (i32 leaf : stage_leaves(o, index)) toned_leaves.insert(leaf);
    }
    std::map<i32, std::vector<Tone>> tones; // by shape
    for (const i32 leaf : toned_leaves) {
        std::vector<Tone>& ts = tones[leaf];
        for (i32 q = leaf; q >= 0; q = parent_of(q))
            if (const auto it = posed.find(q); it != posed.end())
                if (Tone t; tone_of(*it->second, t)) ts.push_back(t);
        LeafLook& l = look(leaf);
        auto colour = [&](Paint8& p) {
            for (const Tone& t : ts) t.apply(p.r, p.g, p.b, p.a);
        };
        if (!l.fill.other && l.fill.a > 0) {
            colour(l.fill);
            l.fill_set = true;
        }
        if (!l.stroke.other && l.stroke.a > 0 && l.width > 0) {
            colour(l.stroke);
            l.stroke_set = true;
        }
    }

    // Masks (part 5г): a part drawn only inside (or outside) the shapes of its mask part as they stand, white whatever
    // their colours; ThorVG keeps a mask shape's fill if it has one or no stroke, its stroke if it has one.
    if (st)
        for (const auto& [index, pp] : parts) {
            if (!has(*pp, MotionProp::Mask) || !(val(*pp, MotionProp::Mask).v > 0)) continue;
            const vector::MotionValue& mv = val(*pp, MotionProp::Mask);
            const StageMask key{pp->id, mv.target, mv.v > 1.5};
            const auto mk = st->mask.find(key);
            const i32 by = part_element(f, mv.target);
            if (mk == st->mask.end() || by < 0) {
                problem("«" + file_name(path) + "», маска «" + pp->id + "»: нет части «" + mv.target + "»");
                continue;
            }
            w.set(index, "mask", "url(#" + o.elements[static_cast<usize>(mk->second)].id + ")");
            const vector::MotionMatrix into = vector::inverse(screen(f, now, index));
            const std::string colour = std::get<2>(key) ? "#000000" : "#ffffff";
            for (i32 leaf : leaves_of(f, by)) {
                const auto mp = st->mask_path.find({key, st->file_of[static_cast<usize>(leaf)]});
                if (mp == st->mask_path.end()) continue;
                const i32 to_el = mp->second;
                const LeafLook& l = look(leaf);
                bool fill = l.fill.other || l.fill.a > 0;
                bool stroke = l.width > 0 && (l.stroke.other || l.stroke.a > 0);
                if (vector::display_none(o, leaf)) fill = true, stroke = false; // ThorVG reads a hidden shape without paints
                w.set(to_el, "fill", fill || !stroke ? colour : std::string("none"));
                w.set(to_el, "stroke", stroke ? colour : std::string("none"));
                w.set(to_el, "stroke-width", num(l.width));
                if (const std::string* d = w.get(leaf, "d")) w.set(to_el, "d", *d);
                if (const std::string* off = w.get(leaf, "stroke-dashoffset")) w.set(to_el, "stroke-dashoffset", *off);
                w.set(to_el, "transform", matrix_text(into * screen(f, now, leaf)));
            }
        }

    // The shapes' looks, a gradient a tone changes in its own copy (part 5г).
    for (const auto& [leaf, l] : looks) {
        const auto tn = tones.find(leaf);
        auto gradient = [&](const char* paint, const char* opacity, const std::map<std::pair<std::string, i32>, i32>& copies) {
            if (!st || tn == tones.end()) return;
            const auto g = copies.find({st->copy_of[static_cast<usize>(leaf)], st->file_of[static_cast<usize>(leaf)]});
            if (g == copies.end()) return;
            // ThorVG bakes the shape's fill (or stroke) opacity into the stops, then tones each.
            const u8 own = opacity8(vector::inherited(o, leaf, opacity));
            for (i32 s = g->second + 1; s < static_cast<i32>(o.elements.size()) && parent_of(s) == g->second; ++s) {
                const vector::SvgElement& stop = o.elements[static_cast<usize>(s)];
                u8 c[3] = {0, 0, 0};
                vector::parse_svg_color(stop.property("stop-color"), c);
                Paint8 p{c[0], c[1], c[2], static_cast<u8>(opacity8(stop.property("stop-opacity")) * own / 255), false};
                for (const Tone& t : tn->second) t.apply(p.r, p.g, p.b, p.a);
                w.set(s, "stop-color", hex6(p));
                w.set(s, "stop-opacity", num(p.a / 255.0));
            }
            w.set(leaf, paint, "url(#" + o.elements[static_cast<usize>(g->second)].id + ")");
            w.set(leaf, opacity, "1");
        };
        if (l.fill_set && !l.fill.other) {
            w.set(leaf, "fill", l.fill.a > 0 || !l.morphed ? hex6(l.fill) : std::string("none"));
            w.set(leaf, "fill-opacity", num(l.fill.a / 255.0));
        } else if (l.fill.other && st) {
            gradient("fill", "fill-opacity", st->fill_gradient);
        }
        if (l.stroke_set && !l.stroke.other) {
            w.set(leaf, "stroke", hex6(l.stroke));
            w.set(leaf, "stroke-opacity", num(l.stroke.a / 255.0));
            w.set(leaf, "stroke-width", num(l.width));
        } else if (l.stroke.other && l.width > 0 && st) {
            gradient("stroke", "stroke-opacity", st->stroke_gradient);
        }
        // A morphed shape without its own opacity track: the pair's opacity at k.
        if (l.morphed) {
            bool own_track = false;
            for (const auto& [index, pp] : parts)
                if (index == leaf && has(*pp, MotionProp::Opacity)) own_track = true;
            if (!own_track) w.set(leaf, "opacity", num(l.opacity));
        }
    }

    // A swap (part 5д): the new drawing hides its pairs (the old one draws them); every other shape fades, its opacity
    // as now times 1 − k in the old drawing, times k in the new.
    if (side) {
        const f64 k = std::clamp(side->from ? 1 - side->k : side->k, 0.0, 1.0);
        std::set<i32> paired;
        for (const Pair& pr : *side->pairs) paired.insert(side->from ? pr.a : pr.b);
        if (!side->from)
            for (const i32 e : paired) w.set(e, "display", "none");
        for (const i32 leaf : stage_leaves(o, 0)) {
            if (paired.count(leaf)) continue;
            const std::string* given = w.get(leaf, "opacity");
            const f64 own = std::clamp(vector::svg_number(given ? *given : o.elements[static_cast<usize>(leaf)].property("opacity"), 1.0), 0.0, 1.0);
            w.set(leaf, "opacity", num(own * k));
        }
    }

    // Part 6г2: a run of a scene's stage draws its own elements of the top level only; the box of the picture shown.
    if (!pose.only.empty())
        for (i32 i = 1; i < static_cast<i32>(o.elements.size()); ++i) {
            if (parent_of(i) != 0 || !draws_tag(o.elements[static_cast<usize>(i)].tag)) continue;
            const std::string key = top_key(f, i);
            if (!key.empty() && std::find(pose.only.begin(), pose.only.end(), key) == pose.only.end()) w.set(i, "display", "none");
        }
    if (pose.view_w > 0 && pose.view_h > 0) {
        w.set(0, "viewBox", num(pose.view_x) + " " + num(pose.view_y) + " " + num(pose.view_w) + " " + num(pose.view_h));
        w.set(0, "width", num(pose.view_w));
        w.set(0, "height", num(pose.view_h));
    }
    return write_all(f, doc, w);
}

// What a pose needs of a stage (part 5г), against the picture's own file: its copies, the shapes it draws along their
// length or morphs that are not a <path>, the gradients it tints, its masks, its order.
StageNeeds needs_of(File& f, const SvgPose& pose) {
    using vector::MotionProp;
    const vector::SvgOutline& o = f.outline;
    StageNeeds n;
    n.copies = pose.copies;
    n.order = pose.order;
    auto source_of = [&](const std::string& id, std::string& copy) -> i32 {
        for (const vector::MotionCopy& c : pose.copies)
            if (c.id == id) {
                copy = c.id;
                return stage_part(o, c.source);
            }
        copy.clear();
        return stage_part(o, id);
    };
    for (const vector::MotionPartPose& pp : pose.parts) {
        auto has = [&](MotionProp p) { return pp.has[static_cast<usize>(p)]; };
        auto val = [&](MotionProp p) -> const vector::MotionValue& { return pp.values[static_cast<usize>(p)]; };
        std::string copy;
        const i32 e = source_of(pp.id, copy);
        if (e < 0) continue;
        const bool drawn = (has(MotionProp::DrawFrom) && val(MotionProp::DrawFrom).v != 0) || (has(MotionProp::DrawTo) && val(MotionProp::DrawTo).v != 1);
        const bool morph = has(MotionProp::Morph) && val(MotionProp::Morph).v > 0 && !val(MotionProp::Morph).target.empty();
        if (drawn || morph)
            for (const i32 leaf : stage_leaves(o, e))
                if (o.elements[static_cast<usize>(leaf)].tag != "path" && (drawn || leaf == e)) n.outlines.insert(leaf);
        Tone t;
        if (tone_of(pp, t)) {
            auto toned = [&](const std::string& in, i32 under) {
                for (const i32 leaf : stage_leaves(o, under)) {
                    if (vector::inherited(o, leaf, "fill").rfind("url(", 0) == 0) n.fill_gradients.insert({in, leaf});
                    if (vector::inherited(o, leaf, "stroke").rfind("url(", 0) == 0) n.stroke_gradients.insert({in, leaf});
                }
            };
            toned(copy, e);
            // A group's tone reaches the copies of the parts inside it.
            if (copy.empty())
                for (const vector::MotionCopy& c : pose.copies) {
                    const i32 s = stage_part(o, c.source);
                    bool inside = false;
                    for (i32 k = s >= 0 ? o.elements[static_cast<usize>(s)].parent : -1; k >= 0 && !inside; k = o.elements[static_cast<usize>(k)].parent)
                        inside = k == e;
                    if (inside) toned(c.id, s);
                }
        }
        if (has(MotionProp::Mask) && val(MotionProp::Mask).v > 0 && !val(MotionProp::Mask).target.empty())
            n.masks.insert({pp.id, val(MotionProp::Mask).target, val(MotionProp::Mask).v > 1.5});
    }
    return n;
}

// The stage a live element (or a check) draws a picture from, its file in files() at `key`; null with a line when it does
// not make. `path` names the picture in the lines.
File* stage_file(File& f, const std::string& path, const std::string& key, const StageNeeds& needs) {
    Stage stage;
    std::vector<std::string> notes;
    const bool made = make_stage(f.text, f.outline, needs, stage, notes);
    for (const std::string& n : notes) problem("«" + file_name(path) + "»: " + n);
    if (!made) return nullptr;
    File& sf = file(key, stage.text);
    if (sf.outline.elements.size() != stage.file_of.size() || !sf.outline.error.empty()) {
        problem("«" + file_name(path) + "»: сцена картинки не сложилась (" + std::to_string(sf.outline.elements.size()) + " элементов вместо " +
                std::to_string(stage.file_of.size()) + ")");
        return nullptr;
    }
    if (!sf.stage) sf.stage = std::make_shared<const Stage>(std::move(stage));
    return &sf;
}

std::unique_ptr<lunasvg::Document> load(const std::string& text) { return lunasvg::Document::loadFromData(text.data(), text.size()); }

// Puts drawing `m` in a slot's document: read from the drawing's text, or from a stage of it when `want` needs one (a
// stage made stays while it covers what is wanted; `posed` false: none). A document that stays gets back first what
// the last frame's pose set on it (the attribute or the `style` the file gives, "" when it gives none: inherited or the
// initial value), so a frame does not build on the last one. The file the document is read from; null when it does not
// read (the slot reads it again next time).
File* place(Slot& s, const Family::Member& m, const std::string& stage_prefix, const StageNeeds& want, bool posed,
            Rml::UniquePtr<lunasvg::Document>& doc) {
    File& d = *m.file;
    bool fresh = !doc;
    if (s.source != &d || s.serial != d.serial || s.drawing != m.name) {
        s = {};
        s.drawing = m.name;
        s.source = &d;
        s.serial = d.serial;
        fresh = true;
    }
    if (!posed && s.stage) {
        s.stage = nullptr;
        s.needs = {};
        fresh = true;
    }
    if (posed && (s.stage ? !s.needs.covers(want) : !want.empty())) {
        s.needs.add(want);
        File* sf = stage_file(d, m.picture, stage_prefix + m.key, s.needs);
        if (std::unique_ptr<lunasvg::Document> made = sf ? load(sf->text) : nullptr) {
            doc = std::move(made);
            s.stage = sf;
            s.wrote = false;
            return sf;
        }
        fresh = fresh || s.stage; // drawn from the drawing's own text then
        s.stage = nullptr;
        s.needs = {};
    }
    File& from = s.stage ? *s.stage : d;
    if (fresh) {
        std::unique_ptr<lunasvg::Document> made = load(from.text);
        if (!made) {
            problem("«" + file_name(m.picture) + "»: картинка не читается как SVG");
            s.source = nullptr;
            return nullptr;
        }
        doc = std::move(made);
        s.wrote = false;
        return &from;
    }
    if (s.wrote && s.written.file == from.serial)
        for (const auto& [index, name] : s.written.attributes)
            if (lunasvg::Element e = element_at(from, *doc, index)) e.setAttribute(name, from.outline.elements[static_cast<usize>(index)].property(name));
    s.wrote = false;
    return &from;
}

// A pose on a picture's documents: the drawing shown in `doc` (part 5д: a document of its own when it is not the main
// one), or in a swap the old drawing in `doc` and the new one in `over`; what the scripts said (`said`) on each. `f` is
// the picture's file at `path`, its drawings' files in files() by `family_key` and their stages by `stage_prefix`.
void compose(File& f, const std::string& path, const std::string& family_key, const std::string& stage_prefix, const SvgPose& pose,
             const std::map<std::string, SvgPart, std::less<>>* said, Slot& base, Slot& top, Rml::UniquePtr<lunasvg::Document>& doc,
             Rml::UniquePtr<lunasvg::Document>& over) {
    const Family fam = family_of(f, path, family_key, pose);
    // The drawing shown, and in a swap the one it comes from (none when it is the same: forge::pixel::Rig::pose).
    const Family::Member* to = fam.get(pose.drawing);
    if (!to) {
        problem("«" + file_name(path) + "»: нет рисунка «" + pose.drawing + "»");
        to = &fam.members.front();
    }
    const Family::Member* from = nullptr;
    if (pose.swap) {
        from = fam.get(pose.swap_from);
        if (!from) problem("«" + file_name(path) + "»: нет рисунка «" + pose.swap_from + "»");
        if (from == to) from = nullptr;
    }
    const Family::Member& b = from ? *from : *to;
    const SvgPose pb = for_drawing(pose, from ? pose.from_parts : pose.parts, *b.file, fam);
    SvgPose pt;
    StageNeeds want = needs_of(*b.file, pb);
    if (from) {
        pt = for_drawing(pose, pose.parts, *to->file, fam);
        // The old drawing's pairs morph: each a <path> of its stage.
        for (const Pair& pr : pairs_of(*b.file, pb, *to->file, pt))
            if (b.file->outline.elements[static_cast<usize>(pr.a)].tag != "path") want.outlines.insert(pr.a);
    }
    File* drawn_b = place(base, b, stage_prefix, want, true, doc);
    File* drawn_t = from ? place(top, *to, stage_prefix + "\x04", needs_of(*to->file, pt), true, over) : nullptr;
    if (!drawn_t) {
        over.reset();
        top = {};
    }
    if (!drawn_b) return;
    // What the scripts said, on each drawing drawn; a drawing without the part says nothing when another one has it.
    if (said)
        for (const auto& [id, part] : *said) {
            const bool known = fam.others() && fam.knows(id);
            apply_part(*drawn_b, b.picture, *doc, id, part, known && own_part(*b.file, id) < 0);
            if (drawn_t) apply_part(*drawn_t, to->picture, *over, id, part, known && own_part(*to->file, id) < 0);
        }
    if (!drawn_t) {
        base.written = apply_pose(*drawn_b, b.picture, *doc, pb, &fam, b.name);
        base.wrote = true;
        return;
    }
    // The new drawing first: the old one morphs its pairs to what the new one's are now.
    const std::vector<Pair> pairs = pairs_of(*drawn_b, pb, *drawn_t, pt);
    std::map<i32, SwapSide::Now> now;
    const f64 k = std::isfinite(pose.swap_k) ? std::clamp(pose.swap_k, 0.0, 1.0) : 0.0;
    const SwapSide in{false, k, &pairs, drawn_b, b.name, &now}, out{true, k, &pairs, drawn_t, to->name, &now};
    top.written = apply_pose(*drawn_t, to->picture, *over, pt, &fam, to->name, &in);
    top.wrote = true;
    base.written = apply_pose(*drawn_b, b.picture, *doc, pb, &fam, b.name, &out);
    base.wrote = true;
}

void apply(Rml::Element* element, const Rml::String& path, const Rml::String& text, Rml::UniquePtr<lunasvg::Document>& doc,
           Rml::UniquePtr<lunasvg::Document>& over, bool made) {
    Live* live = live_of(element);
    if (!live || !doc) return;
    // A document the plugin has just read (the first time, or again after the element let it go) is the file's, not a
    // stage nor another drawing, wherever it stands in memory; nothing is over it.
    if (made) {
        live->base.erase(path);
        live->top.erase(path);
        over.reset();
    }
    File& f = file(path, text);
    if (!f.outline.error.empty()) {
        problem("«" + file_name(path) + "»: " + f.outline.error);
        return;
    }
    auto [it, first] = live->base.try_emplace(path);
    Slot& base = it->second;
    if (first) { // the plugin's reading of the file
        base.source = &f;
        base.serial = f.serial;
    }
    Slot& top = live->top[path];
    const std::string key = stage_key(element);
    // The picture is drawn from its file, or from a stage (part 5г) when the clip needs one, or from another drawing of
    // the character (part 5д).
    if (live->posed && file_name(path) == live->pose.picture) {
        compose(f, path, "\x05" + dir_of(path), key, live->pose, &live->parts, base, top, doc, over);
        return;
    }
    over.reset();
    live->top.erase(path);
    const Family::Member main{"", path, path, &f};
    if (File* drawn = place(base, main, key, {}, false, doc))
        for (const auto& [id, part] : live->parts) apply_part(*drawn, path, *doc, id, part);
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

SvgPose clip_pose(std::string picture, const vector::MotionFile& file, const vector::MotionClip& clip, f64 at, std::vector<SvgDrawing> drawings) {
    const vector::MotionShown shown = vector::motion_shown(file, clip, at);
    SvgPose pose;
    pose.picture = std::move(picture);
    pose.parts = vector::motion_pose(file, clip, at, shown.drawing);
    pose.order = vector::motion_order_at(clip, at);
    for (const vector::MotionCopy& copy : clip.copies)
        if (vector::motion_copy_drawn(copy, at)) pose.copies.push_back(copy);
    pose.drawings = std::move(drawings);
    pose.drawing = shown.drawing;
    if (shown.swap) {
        pose.swap = true;
        pose.swap_from = shown.from;
        pose.swap_k = shown.k;
        pose.from_parts = vector::motion_pose(file, clip, at, shown.from);
    }
    return pose;
}

SvgPose blend_pose(const vector::MotionFile& file, const SvgPose& from, const SvgPose& to, f64 k) {
    using vector::MotionProp;
    if (!(k > 0)) return from;
    if (k >= 1) return to;
    auto main = [](const std::string& d) { return d.empty() ? std::string(vector::kMotionMainDrawing) : d; };
    // The drawings and their shares: a pose shows one, a swap two.
    std::map<std::string, f64> weight;
    auto weigh = [&](const SvgPose& p, f64 of) {
        const std::string shown = main(p.drawing);
        const std::string before = main(p.swap_from);
        auto add = [&](const std::string& d, f64 w) {
            if (w > 0 && (d == vector::kMotionMainDrawing || file.drawing(d))) weight[d] += w;
        };
        if (p.swap) {
            add(before, of * (1 - p.swap_k));
            add(shown, of * p.swap_k);
        } else
            add(shown, of);
    };
    weigh(from, 1 - k);
    weigh(to, k);
    const std::string top = main(to.drawing);
    std::string under;
    for (const auto& [d, w] : weight)
        if (d != top && (under.empty() || w > weight[under])) under = d;

    SvgPose out;
    out.picture = to.picture;
    out.drawings = to.drawings;
    out.drawing = top;
    out.order = from.order;
    // The copies drawn on either side; one only one side draws comes in or goes out with its opacity.
    std::set<std::string> from_copies, to_copies;
    for (const vector::MotionCopy& c : from.copies) from_copies.insert(c.id);
    for (const vector::MotionCopy& c : to.copies) to_copies.insert(c.id);
    out.copies = to.copies;
    for (const vector::MotionCopy& c : from.copies)
        if (!to_copies.count(c.id)) out.copies.push_back(c);
    std::map<std::string, const vector::MotionPartPose*> a, b;
    for (const vector::MotionPartPose& p : from.parts) a.emplace(p.id, &p);
    for (const vector::MotionPartPose& p : to.parts) b.emplace(p.id, &p);
    std::vector<std::string> ids;
    for (const vector::MotionPartPose& p : to.parts) ids.push_back(p.id);
    for (const vector::MotionPartPose& p : from.parts)
        if (!b.count(p.id)) ids.push_back(p.id);
    for (const std::set<std::string>* copies : {&to_copies, &from_copies}) // a copy without tracks fades too
        for (const std::string& id : *copies)
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    constexpr usize kOpacity = static_cast<usize>(MotionProp::Opacity);
    for (const std::string& id : ids) {
        const auto ai = a.find(id), bi = b.find(id);
        const vector::MotionPartPose* pa = ai == a.end() ? nullptr : ai->second;
        const vector::MotionPartPose* pb = bi == b.end() ? nullptr : bi->second;
        vector::MotionPartPose pp;
        pp.id = id;
        const bool going = from_copies.count(id) && !to_copies.count(id), coming = to_copies.count(id) && !from_copies.count(id);
        if (going || coming) {
            // A copy where the side that draws it has it.
            if (const vector::MotionPartPose* side = going ? pa : pb) pp = *side;
            else
                for (usize i = 0; i < vector::kMotionPropCount; ++i) pp.values[i] = vector::motion_rest(static_cast<MotionProp>(i));
            if (!pp.has[kOpacity]) pp.values[kOpacity] = vector::motion_rest(MotionProp::Opacity);
            pp.has[kOpacity] = true;
            pp.values[kOpacity].v *= going ? 1 - k : k;
        } else
            for (usize i = 0; i < vector::kMotionPropCount; ++i) {
                const MotionProp prop = static_cast<MotionProp>(i);
                pp.has[i] = (pa && pa->has[i]) || (pb && pb->has[i]);
                const vector::MotionValue rest = vector::motion_rest(prop);
                pp.values[i] = !pp.has[i] ? rest
                                          : vector::motion_mix(prop, pa && pa->has[i] ? pa->values[i] : rest,
                                                               pb && pb->has[i] ? pb->values[i] : rest, k);
            }
        vector::motion_part_pose(file, pp, top);
        out.parts.push_back(std::move(pp));
    }
    if (!under.empty()) {
        out.swap = true;
        out.swap_from = under;
        out.swap_k = weight[top] / (weight[top] + weight[under]);
        out.from_parts = out.parts;
        for (vector::MotionPartPose& pp : out.from_parts) vector::motion_part_pose(file, pp, under);
    }
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
    live->base.clear();
    live->top.clear();
    // Made again from the files: what the clip changed goes, what svg_move and the others said comes back.
    Rml::SVG::SetLive(element, false);
    Rml::SVG::SetLive(element, true);
}

const SvgPose* svg_posed(Rml::Element* element) {
    Live* live = live_of(element);
    return live && live->posed ? &live->pose : nullptr;
}

namespace {

// The documents a pose draws a picture into, for checks (as a live element's apply draws them): the drawing shown, or in
// a swap the old one in `doc` and the new one in `over`; the stages they are read from ("" none).
struct Posed {
    Rml::UniquePtr<lunasvg::Document> doc, over;
    std::string stage, over_stage;
};

bool posed_documents(std::string_view svg, const SvgPose& pose, Posed& out, std::string& why) {
    const std::string text(svg);
    File& f = file("\x01" + pose.picture, text); // not a path a live element has
    if (!f.outline.error.empty()) {
        why = f.outline.error;
        return false;
    }
    Slot base, top;
    compose(f, "\x01" + pose.picture, "\x01\x05", stage_key(nullptr), pose, nullptr, base, top, out.doc, out.over);
    if (!out.doc || !base.source) {
        why = "картинка не читается как SVG";
        return false;
    }
    if (base.stage) out.stage = base.stage->text;
    if (top.stage) out.over_stage = top.stage->text;
    return true;
}

} // namespace

bool render_posed_svg(std::string_view svg, const SvgPose& pose, u32 w, u32 h, std::vector<u8>& rgba, std::string& why) {
    rgba.clear();
    Posed p;
    if (!posed_documents(svg, pose, p, why)) return false;
    lunasvg::Bitmap bmp = p.doc->renderToBitmap(static_cast<int>(w), static_cast<int>(h), 0);
    if (bmp.isNull()) return why = "картинка не рисуется", false;
    // The new drawing over the old at its scale, as the SVG plugin draws a live picture's second document.
    if (p.over && p.doc->width() > 0 && p.doc->height() > 0)
        p.over->render(bmp, lunasvg::Matrix(static_cast<float>(w) / p.doc->width(), 0, 0, static_cast<float>(h) / p.doc->height(), 0, 0));
    bmp.convertToRGBA();
    rgba.resize(static_cast<usize>(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        std::copy_n(bmp.data() + static_cast<usize>(y) * static_cast<usize>(bmp.stride()), static_cast<usize>(w) * 4,
                    rgba.begin() + static_cast<std::ptrdiff_t>(y) * w * 4);
    return true;
}

std::string posed_attribute(std::string_view svg, const SvgPose& pose, std::string_view id, std::string_view name, bool over) {
    std::string why;
    Posed p;
    if (!posed_documents(svg, pose, p, why)) return {};
    lunasvg::Document* doc = over ? p.over.get() : p.doc.get();
    if (!doc) return {};
    lunasvg::Element e = doc->getElementById(std::string(id));
    return e ? e.getAttribute(std::string(name)) : std::string();
}

std::string posed_stage(std::string_view svg, const SvgPose& pose, bool over) {
    std::string why;
    Posed p;
    posed_documents(svg, pose, p, why);
    return over ? p.over_stage : p.stage;
}

struct SvgPlaces::Impl {
    std::string path, text;
    bool loaded = false;
    Slot base, top;
    Rml::UniquePtr<lunasvg::Document> doc, over;
    std::vector<SvgPlace> places;
};

SvgPlaces::SvgPlaces() : impl_(std::make_unique<Impl>()) {}

SvgPlaces::~SvgPlaces() {
    // Its stages go with it.
    const std::string key = stage_key(this);
    auto& fs = files();
    for (auto f = fs.lower_bound(key); f != fs.end() && f->first.rfind(key, 0) == 0;) f = fs.erase(f);
}

bool SvgPlaces::load(std::string path, std::string text, std::string& why) {
    Impl& im = *impl_;
    im = {};
    const File& f = file(path, text);
    if (!f.outline.error.empty()) return why = f.outline.error, false;
    if (!f.rest) return why = "картинка не читается как SVG", false;
    im.path = std::move(path);
    im.text = std::move(text);
    im.loaded = true;
    return true;
}

const std::vector<SvgPlace>& SvgPlaces::place(const SvgPose& pose) {
    Impl& im = *impl_;
    im.places.clear();
    if (!im.loaded) return im.places;
    File& f = file(im.path, im.text);
    SvgPose all = pose;
    all.only.clear();
    all.view_w = all.view_h = 0;
    compose(f, im.path, "\x05" + dir_of(im.path), stage_key(this), all, nullptr, im.base, im.top, im.doc, im.over);
    const File* drawn = im.base.stage ? im.base.stage : im.base.source;
    if (!im.doc || !drawn) return im.places;
    const vector::SvgOutline& o = drawn->outline;
    auto matrix = [](const lunasvg::Matrix& m) {
        return vector::MotionMatrix{m.a, m.b, m.c, m.d, m.e, m.f};
    };
    const vector::MotionMatrix up = vector::inverse(matrix(im.doc->documentElement().getLocalMatrix()));
    // What the pose wrote, else the file's.
    auto value = [&](i32 i, lunasvg::Element& e, const char* name) {
        if (im.base.wrote && im.base.written.file == drawn->serial)
            for (const auto& [index, n] : im.base.written.attributes)
                if (index == i && n == name) return e.getAttribute(name);
        return o.elements[static_cast<usize>(i)].property(name);
    };
    for (i32 i = 1; i < static_cast<i32>(o.elements.size()); ++i) {
        const vector::SvgElement& el = o.elements[static_cast<usize>(i)];
        if (el.parent != 0) continue;
        lunasvg::Element e = element_at(*drawn, *im.doc, i);
        if (!e) continue;
        SvgPlace p;
        p.key = top_key(*drawn, i);
        p.id = el.id;
        p.m = up * matrix(e.getGlobalMatrix());
        p.opacity = std::clamp(vector::svg_number(value(i, e, "opacity"), 1.0), 0.0, 1.0);
        p.shown = value(i, e, "display") != "none";
        p.draws = draws_tag(el.tag);
        im.places.push_back(std::move(p));
    }
    return im.places;
}

SvgPose SvgPlaces::run_pose(const SvgPose& pose, std::vector<std::string> keys) {
    Impl& im = *impl_;
    SvgPose out = pose;
    out.parts.clear();
    out.only = std::move(keys);
    if (!im.loaded) return out;
    const vector::SvgOutline& o = file(im.path, im.text).outline;
    // The element of the top level a part is in, by its key.
    auto top_of = [&](const std::string& id) {
        std::string copy, source = id;
        for (const vector::MotionCopy& c : pose.copies)
            if (c.id == id) {
                copy = c.id;
                source = c.source;
                break;
            }
        i32 e = stage_part(o, source);
        if (e <= 0) return std::string();
        if (o.elements[static_cast<usize>(e)].parent == 0 && !copy.empty()) return "c" + copy;
        while (o.elements[static_cast<usize>(e)].parent > 0) e = o.elements[static_cast<usize>(e)].parent;
        return "e" + std::to_string(e);
    };
    std::set<std::string> want;
    for (const vector::MotionPartPose& pp : pose.parts)
        if (std::find(out.only.begin(), out.only.end(), top_of(pp.id)) != out.only.end()) want.insert(pp.id);
    using vector::MotionProp;
    for (bool more = true; more;) {
        more = false;
        for (const vector::MotionPartPose& pp : pose.parts) {
            if (!want.count(pp.id)) continue;
            for (const MotionProp p : {MotionProp::Link, MotionProp::Mask}) {
                const usize i = static_cast<usize>(p);
                if (pp.has[i] && !pp.values[i].target.empty() && want.insert(pp.values[i].target).second) more = true;
            }
        }
    }
    for (const vector::MotionPartPose& pp : pose.parts)
        if (want.count(pp.id)) out.parts.push_back(pp);
    return out;
}

void install_live_svg() { Rml::SVG::SetLiveApply(&apply); }

} // namespace forge::ui
