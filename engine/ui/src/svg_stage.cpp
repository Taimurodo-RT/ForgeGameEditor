#include "svg_stage.h"

#include "forge/vector/path.h"
#include "forge/vector/svg_shape.h"

#include <algorithm>

namespace forge::ui {

namespace {

bool drawable(std::string_view tag) { return tag == "g" || tag == "use" || stage_shape_tag(tag); }

std::string escaped(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    for (const char c : v) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

// The attributes of a shape's geometry, which its <path> has not.
bool geometry(std::string_view tag, std::string_view name) {
    if (tag == "rect") return name == "x" || name == "y" || name == "width" || name == "height" || name == "rx" || name == "ry";
    if (tag == "circle") return name == "cx" || name == "cy" || name == "r";
    if (tag == "ellipse") return name == "cx" || name == "cy" || name == "rx" || name == "ry";
    if (tag == "line") return name == "x1" || name == "y1" || name == "x2" || name == "y2";
    if (tag == "polyline" || tag == "polygon") return name == "points";
    return false;
}

bool under(const vector::SvgOutline& o, i32 j, i32 i) {
    for (i32 k = o.elements[static_cast<usize>(j)].parent; k >= 0; k = o.elements[static_cast<usize>(k)].parent)
        if (k == i) return true;
    return false;
}

// An element of a group as the stage has it: one of the file's, or a copy of it.
struct Item {
    i32 element = -1;
    const vector::MotionCopy* copy = nullptr;
    bool operator==(const Item&) const = default;
};

struct Builder {
    std::string_view text;
    usize base = 0; // where the outline's offsets start: past a BOM
    const vector::SvgOutline& o;
    const StageNeeds& n;
    Stage& st;
    std::vector<std::string>& notes;
    std::vector<i32> last;               // each element's last element inside it (itself when none)
    std::vector<std::vector<i32>> kids;  // each element's elements
    std::vector<char> dirty, dirty_copy; // written element by element (not as the file's text): in the file, in a copy
    std::map<i32, std::vector<Item>> lists; // a group's elements as the stage has them, where they are not the file's
    std::map<i32, std::vector<const vector::MotionCopy*>> copies; // a source's copies, by number
    std::set<std::string, std::less<>> ids;
    u32 fresh = 0;
    std::string out;

    std::string_view span(usize a, usize b) const { return text.substr(base + a, b - a); }
    const vector::SvgElement& el(i32 i) const { return o.elements[static_cast<usize>(i)]; }
    // Where an element's end tag starts (an element <…/> has none).
    usize close_of(i32 e) const { return text.rfind('<', base + el(e).end - 1) - base; }

    std::string new_id() {
        std::string id;
        do id = "forge-stage-" + std::to_string(++fresh);
        while (ids.count(id));
        ids.insert(id);
        return id;
    }

    i32 index() const { return static_cast<i32>(st.file_of.size()); }
    void origin(i32 file, const std::string& copy) {
        st.file_of.push_back(file);
        st.copy_of.push_back(copy);
    }

    void mark(std::vector<char>& d, i32 e) {
        for (; e >= 0; e = el(e).parent) d[static_cast<usize>(e)] = 1;
    }

    std::vector<Item>& list_of(i32 parent) {
        if (const auto it = lists.find(parent); it != lists.end()) return it->second;
        std::vector<Item>& v = lists[parent];
        for (const i32 c : kids[static_cast<usize>(parent)]) {
            v.push_back({c, nullptr});
            if (const auto cp = copies.find(c); cp != copies.end())
                for (const vector::MotionCopy* x : cp->second) v.push_back({c, x});
        }
        return v;
    }

    // The item a step names: a copy's id or a part of the file; null with a note when none.
    bool item_of(const std::string& id, Item& out_item, i32& parent) {
        for (const auto& [src, list] : copies)
            for (const vector::MotionCopy* c : list)
                if (c->id == id) {
                    out_item = {src, c};
                    parent = el(src).parent;
                    return true;
                }
        const i32 e = stage_part(o, id);
        if (e < 0) return false;
        out_item = {e, nullptr};
        parent = el(e).parent;
        return true;
    }

    void steps() {
        using vector::MotionOrder;
        for (const vector::MotionOrderStep& s : n.order) {
            Item item;
            i32 parent = -1;
            if (!item_of(s.part, item, parent)) {
                notes.push_back("порядок: в рисунке нет части «" + s.part + "»");
                continue;
            }
            if (parent < 0) continue;
            std::vector<Item>& list = list_of(parent);
            mark(dirty, parent);
            auto at = std::find(list.begin(), list.end(), item);
            if (at == list.end()) continue;
            if (s.kind == MotionOrder::Top) {
                list.erase(at);
                list.push_back(item);
                continue;
            }
            if (s.kind == MotionOrder::Bottom) {
                if (at == list.begin()) continue;
                list.erase(at);
                list.insert(list.begin(), item);
                continue;
            }
            const i32 oe = stage_part(o, s.other);
            if (oe < 0) {
                notes.push_back("порядок: в рисунке нет части «" + s.other + "»");
                continue;
            }
            if (el(oe).parent != parent) {
                notes.push_back(s.part + " и " + s.other + " в разных группах: порядок меняется только среди соседей");
                continue;
            }
            const Item other{oe, nullptr};
            if (other == item) continue;
            if (s.kind == MotionOrder::Above) {
                auto next = std::find(list.begin(), list.end(), other);
                if (next != list.end() && std::next(next) != list.end() && *std::next(next) == item) continue; // right above it already
            }
            list.erase(std::find(list.begin(), list.end(), item));
            auto o_at = std::find(list.begin(), list.end(), other);
            list.insert(s.kind == MotionOrder::Below ? o_at : std::next(o_at), item);
        }
    }

    void emit(i32 e, const std::string& copy) {
        const vector::SvgElement& x = el(e);
        if (!(copy.empty() ? dirty : dirty_copy)[static_cast<usize>(e)]) {
            out += span(x.begin, x.end);
            for (i32 k = e; k <= last[static_cast<usize>(e)]; ++k) {
                if (copy.empty()) st.of_file[static_cast<usize>(k)] = index();
                origin(k, copy);
            }
            return;
        }
        if (copy.empty()) st.of_file[static_cast<usize>(e)] = index();
        origin(e, copy);
        const bool outline = n.outlines.count(e) && x.tag != "path" && stage_shape_tag(x.tag);
        const bool closed = x.open_end == x.end;
        if (outline) {
            out += "<path";
            for (const auto& [name, value] : x.attrs)
                if (!geometry(x.tag, name)) out += " " + name + "=\"" + escaped(value) + "\"";
            vector::Path p;
            std::string why;
            vector::shape_path(o, e, p, why);
            out += " d=\"" + vector::to_d(p) + "\"";
            out += closed ? "/>" : ">";
        } else {
            out += span(x.begin, x.open_end);
        }
        if (closed) return;
        const usize close = close_of(e);
        const std::vector<i32>& ks = kids[static_cast<usize>(e)];
        std::vector<Item> items;
        for (const i32 c : ks) items.push_back({c, nullptr});
        const auto own = copy.empty() ? lists.find(e) : lists.end();
        if (own == lists.end() || own->second == items) {
            usize at = x.open_end;
            for (const i32 c : ks) {
                out += span(at, el(c).begin);
                emit(c, copy);
                at = el(c).end;
            }
            out += span(at, close);
        } else {
            // In another order or with copies: what lies between the elements (spaces, comments) is left out.
            out += span(x.open_end, ks.empty() ? close : el(ks.front()).begin);
            for (const Item& it : own->second) {
                if (it.copy) {
                    st.copy_root[it.copy->id] = index();
                    emit(it.element, it.copy->id);
                } else {
                    emit(it.element, copy);
                }
            }
            out += span(ks.empty() ? close : el(ks.back()).end, close);
        }
        if (e == 0 && copy.empty()) defs();
        if (outline) out += "</path>";
        else out += span(close, x.end);
    }

    void gradient(const std::string& copy, i32 leaf, const char* prop, std::map<std::pair<std::string, i32>, i32>& into) {
        const std::string paint = vector::inherited(o, leaf, prop);
        const usize a = paint.find("url(");
        if (a == std::string::npos) return;
        const usize b = paint.find(')', a);
        std::string id = paint.substr(a + 4, b == std::string::npos ? std::string::npos : b - a - 4);
        id.erase(std::remove_if(id.begin(), id.end(), [](char c) { return c == '\'' || c == '"' || c == ' '; }), id.end());
        if (id.empty() || id[0] != '#') return;
        id.erase(0, 1);
        const i32 g = o.find(id);
        if (g < 0 || (el(g).tag != "linearGradient" && el(g).tag != "radialGradient")) return;
        // Its stops, or those of the gradient it takes them from.
        std::vector<i32> stops;
        i32 s = g;
        for (int hop = 0; hop < 16 && s >= 0 && stops.empty(); ++hop) {
            for (const i32 c : kids[static_cast<usize>(s)])
                if (el(c).tag == "stop") stops.push_back(c);
            if (!stops.empty()) break;
            const std::string* h = el(s).attr("href");
            if (!h) h = el(s).attr("xlink:href");
            if (!h || h->size() < 2 || (*h)[0] != '#') break;
            s = o.find(std::string_view(*h).substr(1));
        }
        if (stops.empty()) return;
        into[{copy, leaf}] = index();
        origin(-1, "");
        const std::string& tag = el(g).tag;
        out += "<" + tag + " id=\"" + new_id() + "\" href=\"#" + escaped(id) + "\">";
        for (const i32 si : stops) {
            origin(-1, "");
            const std::string* off = el(si).attr("offset");
            std::string color = el(si).property("stop-color"), opacity = el(si).property("stop-opacity");
            if (color.empty()) color = "#000000";
            if (opacity.empty()) opacity = "1";
            out += "<stop offset=\"" + escaped(off ? *off : std::string("0")) + "\" stop-color=\"" + escaped(color) + "\" stop-opacity=\"" +
                   escaped(opacity) + "\"/>";
        }
        out += "</" + tag + ">";
    }

    void defs() {
        if (n.masks.empty() && n.fill_gradients.empty() && n.stroke_gradients.empty()) return;
        out += "<defs>";
        origin(-1, "");
        constexpr const char* kFar = " x=\"-100000\" y=\"-100000\" width=\"200000\" height=\"200000\"";
        for (const StageMask& m : n.masks) {
            const auto& [part, by, outside] = m;
            const i32 mp = stage_part(o, by);
            if (mp < 0) {
                notes.push_back("маска «" + part + "»: в рисунке нет части «" + by + "»");
                continue;
            }
            st.mask[m] = index();
            origin(-1, "");
            out += "<mask id=\"" + new_id() + "\" maskUnits=\"userSpaceOnUse\"" + kFar + ">";
            if (outside) {
                origin(-1, "");
                out += std::string("<rect") + kFar + " fill=\"#ffffff\"/>";
            }
            for (const i32 leaf : stage_leaves(o, mp)) {
                vector::Path p;
                std::string why;
                if (!vector::shape_path(o, leaf, p, why)) continue;
                st.mask_path[{m, leaf}] = index();
                origin(-1, "");
                out += "<path d=\"" + vector::to_d(p) + "\" fill=\"" + (outside ? "#000000" : "#ffffff") + "\" stroke=\"none\"";
                for (const char* prop : {"fill-rule", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray"}) {
                    const std::string v = vector::inherited(o, leaf, prop);
                    if (!v.empty()) out += std::string(" ") + prop + "=\"" + escaped(v) + "\"";
                }
                out += "/>";
            }
            out += "</mask>";
        }
        for (const auto& [copy, leaf] : n.fill_gradients) gradient(copy, leaf, "fill", st.fill_gradient);
        for (const auto& [copy, leaf] : n.stroke_gradients) gradient(copy, leaf, "stroke", st.stroke_gradient);
        out += "</defs>";
    }
};

} // namespace

bool StageNeeds::empty() const {
    return copies.empty() && outlines.empty() && fill_gradients.empty() && stroke_gradients.empty() && masks.empty() && order.empty();
}

bool StageNeeds::covers(const StageNeeds& o) const {
    for (const vector::MotionCopy& c : o.copies)
        if (std::find(copies.begin(), copies.end(), c) == copies.end()) return false;
    return std::includes(outlines.begin(), outlines.end(), o.outlines.begin(), o.outlines.end()) &&
           std::includes(fill_gradients.begin(), fill_gradients.end(), o.fill_gradients.begin(), o.fill_gradients.end()) &&
           std::includes(stroke_gradients.begin(), stroke_gradients.end(), o.stroke_gradients.begin(), o.stroke_gradients.end()) &&
           std::includes(masks.begin(), masks.end(), o.masks.begin(), o.masks.end()) && order == o.order;
}

void StageNeeds::add(const StageNeeds& o) {
    for (const vector::MotionCopy& c : o.copies)
        if (std::find(copies.begin(), copies.end(), c) == copies.end()) copies.push_back(c);
    outlines.insert(o.outlines.begin(), o.outlines.end());
    fill_gradients.insert(o.fill_gradients.begin(), o.fill_gradients.end());
    stroke_gradients.insert(o.stroke_gradients.begin(), o.stroke_gradients.end());
    masks.insert(o.masks.begin(), o.masks.end());
    order = o.order;
}

bool stage_shape_tag(std::string_view tag) {
    return tag == "path" || tag == "rect" || tag == "circle" || tag == "ellipse" || tag == "line" || tag == "polyline" || tag == "polygon";
}

std::vector<i32> stage_leaves(const vector::SvgOutline& o, i32 index) {
    std::vector<i32> out;
    if (stage_shape_tag(o.elements[static_cast<usize>(index)].tag)) {
        out.push_back(index);
        return out;
    }
    for (i32 j = index + 1; j < static_cast<i32>(o.elements.size()) && under(o, j, index); ++j)
        if (stage_shape_tag(o.elements[static_cast<usize>(j)].tag) && !o.inside(j, "defs") && !o.inside(j, "clipPath") && !o.inside(j, "mask") &&
            !o.inside(j, "pattern") && !o.inside(j, "symbol") && !o.inside(j, "marker"))
            out.push_back(j);
    return out;
}

i32 stage_part(const vector::SvgOutline& o, std::string_view id) {
    if (id.empty()) return -1;
    for (usize i = 0; i < o.elements.size(); ++i)
        if (o.elements[i].id == id && drawable(o.elements[i].tag)) return static_cast<i32>(i);
    return -1;
}

bool make_stage(std::string_view text, const vector::SvgOutline& o, const StageNeeds& n, Stage& st, std::vector<std::string>& notes) {
    st = {};
    if (o.elements.empty()) return false;
    Builder b{text, text.substr(0, 3) == "\xEF\xBB\xBF" ? 3u : 0u, o, n, st, notes, {}, {}, {}, {}, {}, {}, {}, 0, {}};
    const usize count = o.elements.size();
    b.last.resize(count);
    b.kids.resize(count);
    for (usize i = 0; i < count; ++i) b.last[i] = static_cast<i32>(i);
    for (usize i = count; i-- > 1;) {
        const i32 p = o.elements[i].parent;
        if (p < 0) continue;
        b.last[static_cast<usize>(p)] = std::max(b.last[static_cast<usize>(p)], b.last[i]);
    }
    for (usize i = 1; i < count; ++i)
        if (o.elements[i].parent >= 0) b.kids[static_cast<usize>(o.elements[i].parent)].push_back(static_cast<i32>(i));
    for (const vector::SvgElement& e : o.elements)
        if (!e.id.empty()) b.ids.insert(e.id);
    b.dirty.assign(count, 0);
    b.dirty_copy.assign(count, 0);
    b.mark(b.dirty, 0);

    for (const vector::MotionCopy& c : n.copies) {
        const i32 s = stage_part(o, c.source);
        if (s < 0) {
            notes.push_back("копия «" + c.id + "»: в рисунке нет части «" + c.source + "»");
            continue;
        }
        if (o.elements[static_cast<usize>(s)].parent < 0) continue;
        b.copies[s].push_back(&c);
    }
    for (auto& [s, list] : b.copies) {
        std::stable_sort(list.begin(), list.end(), [](const vector::MotionCopy* x, const vector::MotionCopy* y) { return x->number < y->number; });
        const i32 parent = o.elements[static_cast<usize>(s)].parent;
        b.mark(b.dirty, parent);
        b.list_of(parent);
    }
    for (const i32 e : n.outlines)
        if (e > 0 && static_cast<usize>(e) < count) {
            b.mark(b.dirty, e);
            b.mark(b.dirty_copy, e);
        }
    b.steps();

    st.of_file.assign(count, -1);
    const vector::SvgElement& root = o.elements[0];
    b.out.append(text.substr(0, b.base + root.begin));
    b.emit(0, "");
    b.out.append(text.substr(b.base + root.end));
    st.text = std::move(b.out);
    return true;
}

} // namespace forge::ui
