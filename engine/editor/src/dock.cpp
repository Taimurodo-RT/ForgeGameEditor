#include "forge/editor/dock.h"

#include <yyjson.h>

#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace forge::editor {

struct DockLayout::Node {
    // Split
    bool split = false;
    bool column = false; // children above each other
    f32 ratio = 0.5f;    // the first child's share
    std::unique_ptr<Node> a, b;
    // Stack
    bool view = false;
    std::vector<std::string> panels;
    usize active = 0;

    Node* parent = nullptr;
    DockRect rect;
};

namespace {

using Node = DockLayout::Node;

std::unique_ptr<Node> make_stack(std::vector<std::string> panels, bool view = false) {
    auto n = std::make_unique<Node>();
    n->panels = std::move(panels);
    n->view = view;
    return n;
}

std::unique_ptr<Node> make_split(bool column, f32 ratio, std::unique_ptr<Node> a, std::unique_ptr<Node> b) {
    auto n = std::make_unique<Node>();
    n->split = true;
    n->column = column;
    n->ratio = ratio;
    a->parent = n.get();
    b->parent = n.get();
    n->a = std::move(a);
    n->b = std::move(b);
    return n;
}

std::unique_ptr<Node> read_node(yyjson_val* v, int depth) {
    if (!yyjson_is_obj(v) || depth > 32) return nullptr;
    yyjson_val* row = yyjson_obj_get(v, "row");
    yyjson_val* column = yyjson_obj_get(v, "column");
    if (row || column) {
        yyjson_val* r = row ? row : column;
        if (!yyjson_is_num(r)) return nullptr;
        auto a = read_node(yyjson_obj_get(v, "a"), depth + 1);
        auto b = read_node(yyjson_obj_get(v, "b"), depth + 1);
        if (!a || !b) return nullptr;
        const f32 ratio = std::clamp(static_cast<f32>(yyjson_get_num(r)), 0.05f, 0.95f);
        return make_split(column != nullptr, ratio, std::move(a), std::move(b));
    }
    if (yyjson_get_bool(yyjson_obj_get(v, "view"))) return make_stack({}, true);
    yyjson_val* panels = yyjson_obj_get(v, "panels");
    if (!yyjson_is_arr(panels) || yyjson_arr_size(panels) == 0) return nullptr;
    std::vector<std::string> names;
    usize i, n;
    yyjson_val* p;
    yyjson_arr_foreach(panels, i, n, p) {
        if (!yyjson_is_str(p)) return nullptr;
        names.emplace_back(yyjson_get_str(p), yyjson_get_len(p));
    }
    auto s = make_stack(std::move(names));
    s->active = std::min<usize>(static_cast<usize>(yyjson_get_uint(yyjson_obj_get(v, "active"))), s->panels.size() - 1);
    return s;
}

void write_node(const Node& n, std::string& out) {
    if (n.split) {
        char ratio[32];
        std::snprintf(ratio, sizeof(ratio), "%.4g", static_cast<double>(n.ratio));
        out += n.column ? "{\"column\":" : "{\"row\":";
        out += ratio;
        out += ",\"a\":";
        write_node(*n.a, out);
        out += ",\"b\":";
        write_node(*n.b, out);
        out += "}";
        return;
    }
    if (n.view) {
        out += "{\"view\":true}";
        return;
    }
    out += "{\"panels\":[";
    for (usize i = 0; i < n.panels.size(); ++i) {
        if (i) out += ",";
        out += "\"" + n.panels[i] + "\""; // panel ids are plain words
    }
    out += "],\"active\":" + std::to_string(n.active) + "}";
}

// Every stack of the tree, and whether the tree is a valid layout.
void walk(const Node& n, int& views, std::vector<std::string>& panels) {
    if (n.split) {
        walk(*n.a, views, panels);
        walk(*n.b, views, panels);
        return;
    }
    if (n.view) ++views;
    panels.insert(panels.end(), n.panels.begin(), n.panels.end());
}

} // namespace

DockLayout::DockLayout() : root_(make_stack({}, true)) {}
DockLayout::~DockLayout() = default;

bool DockLayout::load(std::string_view json, const std::vector<std::string>& panels) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) return false;
    std::unique_ptr<Node> root = read_node(yyjson_doc_get_root(doc), 0);
    yyjson_doc_free(doc);
    if (!root) return false;
    int views = 0;
    std::vector<std::string> found;
    walk(*root, views, found);
    if (views != 1 || found.size() != panels.size()) return false;
    std::unordered_set<std::string> wanted(panels.begin(), panels.end());
    for (const std::string& p : found)
        if (wanted.erase(p) != 1) return false; // unknown or twice
    root_ = std::move(root);
    stacks_.clear();
    stack_nodes_.clear();
    splitters_.clear();
    splitter_nodes_.clear();
    ++version_;
    return true;
}

std::string DockLayout::save() const {
    std::string out;
    write_node(*root_, out);
    return out;
}

void DockLayout::place(Node& n, const DockRect& r) {
    n.rect = r;
    if (n.split) {
        const f32 length = std::max(0.0f, (n.column ? r.h : r.w) - gap_);
        const f32 first = std::round(length * n.ratio);
        DockRect a = r, b = r, gap = r;
        if (n.column) {
            a.h = first;
            gap.y = r.y + first;
            gap.h = gap_;
            b.y = gap.y + gap_;
            b.h = length - first;
        } else {
            a.w = first;
            gap.x = r.x + first;
            gap.w = gap_;
            b.x = gap.x + gap_;
            b.w = length - first;
        }
        splitters_.push_back({gap, n.column});
        splitter_nodes_.push_back(&n);
        place(*n.a, a);
        place(*n.b, b);
        return;
    }
    Stack s;
    s.frame = r;
    s.view = n.view;
    s.panels = n.panels;
    s.active = n.active;
    const f32 header = n.view ? 0.0f : std::min(header_, r.h);
    s.header = {r.x, r.y, r.w, header};
    s.body = {r.x, r.y + header, r.w, r.h - header};
    stacks_.push_back(std::move(s));
    stack_nodes_.push_back(&n);
}

void DockLayout::layout(const DockRect& area, f32 gap, f32 header) {
    area_ = area;
    gap_ = gap;
    header_ = header;
    stacks_.clear();
    stack_nodes_.clear();
    splitters_.clear();
    splitter_nodes_.clear();
    place(*root_, area);
}

DockLayout::Node* DockLayout::find_stack(std::string_view panel) const {
    std::vector<Node*> todo{root_.get()};
    while (!todo.empty()) {
        Node* n = todo.back();
        todo.pop_back();
        if (n->split) {
            todo.push_back(n->a.get());
            todo.push_back(n->b.get());
        } else if (std::find(n->panels.begin(), n->panels.end(), panel) != n->panels.end()) {
            return n;
        }
    }
    return nullptr;
}

std::unique_ptr<Node>& DockLayout::slot_of(Node* node) {
    if (!node->parent) return root_;
    return node->parent->a.get() == node ? node->parent->a : node->parent->b;
}

bool DockLayout::panel_rect(std::string_view panel, DockRect& out) const {
    for (const Stack& s : stacks_)
        if (s.active < s.panels.size() && s.panels[s.active] == panel) {
            out = s.body;
            return true;
        }
    return false;
}

DockRect DockLayout::view_rect() const {
    for (const Stack& s : stacks_)
        if (s.view) return s.body;
    return {};
}

bool DockLayout::has_panel(std::string_view panel) const { return find_stack(panel) != nullptr; }

void DockLayout::activate(std::string_view panel) {
    Node* n = find_stack(panel);
    if (!n) return;
    const usize i = static_cast<usize>(std::find(n->panels.begin(), n->panels.end(), panel) - n->panels.begin());
    if (n->active == i) return;
    n->active = i;
    ++version_;
}

DockLayout::Drop DockLayout::drop_at(f32 x, f32 y, std::string_view panel) const {
    Drop d;
    const Node* source = find_stack(panel);
    if (!source) return d;
    for (usize i = 0; i < stacks_.size(); ++i) {
        const Stack& s = stacks_[i];
        if (!s.frame.contains(x, y) || s.frame.w <= 0 || s.frame.h <= 0) continue;
        const f32 fx = (x - s.frame.x) / s.frame.w, fy = (y - s.frame.y) / s.frame.h;
        // The nearest edge within a quarter of the size, else the middle.
        const f32 edges[4] = {fx, 1 - fx, fy, 1 - fy};
        const Zone zones[4] = {Zone::Left, Zone::Right, Zone::Top, Zone::Bottom};
        usize nearest = 0;
        for (usize e = 1; e < 4; ++e)
            if (edges[e] < edges[nearest]) nearest = e;
        Zone zone = edges[nearest] < 0.25f ? zones[nearest] : Zone::Center;
        if (s.header.contains(x, y)) zone = Zone::Center; // onto the tabs
        if (zone == Zone::Center && s.view) return d;      // the view holds no tabs
        const bool alone = stack_nodes_[i] == source && source->panels.size() == 1;
        if (alone) return d; // moving a lone panel next to itself changes nothing
        if (zone == Zone::Center && stack_nodes_[i] == source) return d;
        d.zone = zone;
        d.stack = static_cast<i32>(i);
        DockRect p = s.frame;
        const f32 share = 0.4f;
        switch (zone) {
        case Zone::Left: p.w = std::round(p.w * share); break;
        case Zone::Right: p.x += std::round(p.w * (1 - share)); p.w = std::round(p.w * share); break;
        case Zone::Top: p.h = std::round(p.h * share); break;
        case Zone::Bottom: p.y += std::round(p.h * (1 - share)); p.h = std::round(p.h * share); break;
        default: break;
        }
        d.preview = p;
        return d;
    }
    return d;
}

void DockLayout::remove_panel(std::string_view panel) {
    Node* n = find_stack(panel);
    if (!n) return;
    const auto it = std::find(n->panels.begin(), n->panels.end(), panel);
    const usize index = static_cast<usize>(it - n->panels.begin());
    n->panels.erase(it);
    if (n->active > index || n->active >= n->panels.size()) n->active = n->active > 0 ? n->active - 1 : 0;
    if (!n->panels.empty() || n->view) return;
    // The stack is empty: its parent split gives way to the other child.
    Node* parent = n->parent;
    if (!parent) return; // cannot happen: the view is always somewhere
    std::unique_ptr<Node> other = std::move(parent->a.get() == n ? parent->b : parent->a);
    other->parent = parent->parent;
    slot_of(parent) = std::move(other); // destroys the parent and the empty stack
}

bool DockLayout::move(std::string_view panel, const Drop& drop) {
    if (drop.zone == Zone::None || drop.stack < 0 || static_cast<usize>(drop.stack) >= stack_nodes_.size()) return false;
    Node* target = stack_nodes_[static_cast<usize>(drop.stack)];
    Node* source = find_stack(panel);
    if (!source) return false;
    if (source == target && (drop.zone == Zone::Center || source->panels.size() == 1)) return false;
    const std::string name(panel);
    remove_panel(panel); // target survives: only the emptied source and its parent go
    if (drop.zone == Zone::Center) {
        target->panels.push_back(name);
        target->active = target->panels.size() - 1;
    } else {
        std::unique_ptr<Node>& slot = slot_of(target);
        Node* parent = target->parent;
        std::unique_ptr<Node> old = std::move(slot);
        auto fresh = make_stack({name});
        const bool first = drop.zone == Zone::Left || drop.zone == Zone::Top;
        const bool column = drop.zone == Zone::Top || drop.zone == Zone::Bottom;
        const f32 share = 0.4f;
        std::unique_ptr<Node> split = first ? make_split(column, share, std::move(fresh), std::move(old))
                                            : make_split(column, 1 - share, std::move(old), std::move(fresh));
        split->parent = parent;
        slot = std::move(split);
    }
    // Pointers from the last layout may be gone: lay out again now.
    layout(area_, gap_, header_);
    ++version_;
    return true;
}

void DockLayout::drag_splitter(usize index, f32 x, f32 y) {
    if (index >= splitter_nodes_.size()) return;
    Node* n = splitter_nodes_[index];
    const DockRect& r = n->rect;
    const f32 length = (n->column ? r.h : r.w) - gap_;
    if (length <= 0) return;
    const f32 at = (n->column ? y - r.y : x - r.x) - gap_ * 0.5f;
    f32 ratio = at / length;
    const f32 min = kMinSize / length;
    ratio = min < 0.5f ? std::clamp(ratio, min, 1 - min) : 0.5f;
    if (ratio == n->ratio) return;
    n->ratio = ratio;
    layout(area_, gap_, header_);
    ++version_;
}

} // namespace forge::editor
