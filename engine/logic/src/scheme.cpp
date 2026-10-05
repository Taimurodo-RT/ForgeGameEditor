// Links as schemes («Схема»): the node graph of a link, and back.

#include "forge/logic/logic.h"

#include <algorithm>
#include <map>

extern const unsigned char forge_logic_nodes[];
extern const unsigned long long forge_logic_nodes_size;

namespace forge::logic {

namespace {

using script::Graph;
using script::GraphNode;

// Where nodes go: columns left to right, a row under them for values and
// «Иначе».
constexpr f32 kColumn = 280, kRow = 190;

const std::string& side_thing(const Link& l, Side s) { return s == Side::A ? l.a : l.b; }

// The scheme the verb and these refinements make.
Graph build(const Link& l, const VerbDef& v, const Thing& a, const Thing& b) {
    Graph g;
    g.name = "link " + std::to_string(l.id);
    GraphNode& when = g.add("logic.when", 0, 0);
    const u32 w = when.uid;
    const std::string* needed = nullptr;
    if (!v.needs.empty()) {
        const std::string& t = side_thing(l, v.needs == "a" ? Side::A : Side::B);
        if (t != kHero) needed = &t;
    }
    const bool checks = needed || l.night;
    f32 x = kColumn;
    u32 last = w;
    std::string exit = script::kFlowNext;
    u32 cond_if = 0;
    if (checks) {
        // The checks: values under «Когда», joined by «И».
        std::vector<std::pair<u32, std::string>> conds;
        f32 y = kRow;
        if (needed) {
            GraphNode& has = g.add("logic.has", 0, y);
            has.set_value("thing", *needed);
            g.link(w, "hero", has.uid, "who");
            conds.push_back({has.uid, "yes"});
            y += 130;
        }
        if (l.night) {
            GraphNode& night = g.add("logic.night", 0, y);
            conds.push_back({night.uid, "yes"});
        }
        GraphNode& iff = g.add("std.flow.if", x + (conds.size() > 1 ? kColumn : 0), 0);
        cond_if = iff.uid;
        if (conds.size() == 1) {
            g.link(conds[0].first, conds[0].second, cond_if, "cond");
        } else {
            GraphNode& both = g.add("std.logic.and", kColumn, kRow);
            g.link(conds[0].first, conds[0].second, both.uid, "a");
            g.link(conds[1].first, conds[1].second, both.uid, "b");
            g.link(both.uid, "result", cond_if, "cond");
            x += kColumn;
        }
        g.link(last, exit, cond_if);
        last = cond_if;
        exit = "then";
        x += kColumn;
    }
    if (l.once) {
        GraphNode& once = g.add("logic.once", x, 0);
        g.link(last, exit, once.uid);
        last = once.uid;
        exit = "first";
        x += kColumn;
    }
    GraphNode& act = g.add("logic.act", x, 0);
    act.set_value("action", v.action);
    act.set_value("thing", side_thing(l, v.target));
    g.link(w, v.target == Side::A ? "a" : "b", act.uid, "target");
    g.link(w, v.target == Side::A ? "b" : "a", act.uid, "other");
    g.link(last, exit, act.uid);
    last = act.uid;
    exit = script::kFlowNext;
    x += kColumn;
    if (l.sound && !v.sound.empty()) {
        GraphNode& sound = g.add("logic.sound", x, 0);
        sound.set_value("cue", v.sound);
        g.link(last, exit, sound.uid);
    }
    if (cond_if && l.hint && !v.fail.empty()) {
        const f32 hx = g.find(cond_if)->x + kColumn;
        GraphNode& hint = g.add("logic.hint", hx, kRow + 60);
        hint.set_value("text", fill(v.fail, a, b));
        g.link(cond_if, "else", hint.uid);
    }
    return g;
}

// The scheme's meaning without its looks: nodes (with the values set) and
// wires between them, in an order that does not depend on uids or places.
std::string shape(const Graph& g) {
    std::map<u32, std::string> label;
    for (const GraphNode& n : g.nodes) {
        std::vector<std::string> vals;
        for (const script::PinValue& v : n.values)
            if (!v.value.empty() && v.pin.rfind('_', 0) != 0) vals.push_back(v.pin + "=" + v.value);
        std::sort(vals.begin(), vals.end());
        std::string s = n.def;
        for (const std::string& v : vals) s += "|" + v;
        label[n.uid] = s;
    }
    std::vector<std::string> nodes, wires;
    for (const auto& [_, s] : label) nodes.push_back(s);
    for (const script::GraphLink& l : g.links) {
        const auto from = label.find(l.from_node), to = label.find(l.to_node);
        if (from == label.end() || to == label.end()) continue;
        wires.push_back(from->second + "." + l.from_pin + ">" + to->second + "." + l.to_pin);
    }
    std::sort(nodes.begin(), nodes.end());
    std::sort(wires.begin(), wires.end());
    std::string out;
    for (const std::string& s : nodes) out += s + "\n";
    out += "--\n";
    for (const std::string& s : wires) out += s + "\n";
    return out;
}

} // namespace

script::NodeLibrary node_library(const script::ScriptApi& api) {
    script::NodeLibrary lib;
    lib.add_standard();
    lib.add_api(api);
    lib.add_json(std::string_view(reinterpret_cast<const char*>(forge_logic_nodes), static_cast<usize>(forge_logic_nodes_size)),
                 "logic");
    return lib;
}

u32 when_node(const script::Graph& graph) {
    for (const GraphNode& n : graph.nodes)
        if (n.def == "logic.when") return n.uid;
    return 0;
}

namespace {

// The refinements (night 1, once 2, sound 4, hint 8) whose scheme this is,
// the link's own first; -1: none, the scheme is the author's own.
int refinements_of(const std::string& graph_shape, const Link& link, const VerbDef& v, const Thing& a, const Thing& b) {
    const int now = (link.night ? 1 : 0) | (link.once ? 2 : 0) | (link.sound ? 4 : 0) | (link.hint ? 8 : 0);
    for (int i = -1; i < 16; ++i) {
        const int m = i < 0 ? now : i;
        if (i == now) continue;
        Link plain = link;
        plain.night = (m & 1) != 0;
        plain.once = (m & 2) != 0;
        plain.sound = (m & 4) != 0;
        plain.hint = (m & 8) != 0;
        plain.code.clear();
        plain.graph.clear();
        if (shape(build(plain, v, a, b)) == graph_shape) return m;
    }
    return -1;
}

} // namespace

bool own_scheme(const Link& link, const VerbDef& verb, const Thing& a, const Thing& b) {
    if (link.graph.empty()) return false;
    // Words ask often and schemes rarely change: what was found is kept.
    static thread_local std::map<std::string, bool> seen;
    std::string key = link.verb + "|" + link.a + "|" + link.b + "|" + std::to_string(link.id) + "|" + link.graph;
    if (const auto it = seen.find(key); it != seen.end()) return it->second;
    Graph g;
    const bool own = g.from_json(link.graph) && when_node(g) && refinements_of(shape(g), link, verb, a, b) < 0;
    if (seen.size() > 512) seen.clear();
    seen.emplace(std::move(key), own);
    return own;
}

bool own_scheme(const Link& link, const Verbs& verbs, const FindThing& things) {
    const VerbDef* v = verbs.find(link.verb);
    const Thing* a = things(link.a);
    const Thing* b = things(link.b);
    if (!v || !a || !b) return !link.graph.empty();
    return own_scheme(link, *v, *a, *b);
}

script::Graph scheme_of(const Link& link, const Verbs& verbs, const FindThing& things) {
    const VerbDef* v = verbs.find(link.verb);
    const Thing* a = things(link.a);
    const Thing* b = things(link.b);
    if (!link.graph.empty()) {
        // Its own scheme, or the places of its plain one.
        Graph g;
        if (g.from_json(link.graph) && when_node(g)) {
            if (!v || !a || !b) return g;
            const int m = refinements_of(shape(g), link, *v, *a, *b);
            const int now = (link.night ? 1 : 0) | (link.once ? 2 : 0) | (link.sound ? 4 : 0) | (link.hint ? 8 : 0);
            if (m < 0 || m == now) return g;
        }
    }
    if (!v || !a || !b) {
        Graph g;
        g.add("logic.when", 0, 0);
        return g;
    }
    Link plain = link;
    plain.code.clear();
    return build(plain, *v, *a, *b);
}

void set_scheme(Link& link, const script::Graph& graph, const Verbs& verbs, const FindThing& things) {
    link.code.clear();
    link.graph = graph.to_json();
    const VerbDef* v = verbs.find(link.verb);
    const Thing* a = things(link.a);
    const Thing* b = things(link.b);
    if (!v || !a || !b) return;
    // Just refinements: the link takes them (and keeps the places).
    const int m = refinements_of(shape(graph), link, *v, *a, *b);
    if (m < 0) return;
    link.night = (m & 1) != 0;
    link.once = (m & 2) != 0;
    link.sound = (m & 4) != 0;
    link.hint = (m & 8) != 0;
}

script::Graph new_thing_scheme(std::string_view thing) {
    Graph g;
    g.name = "thing " + std::string(thing);
    g.add("std.event.start", 0, 0);
    g.add("std.event.tick", 0, kRow);
    return g;
}

bool thing_event(std::string_view def) { return def.rfind("std.event.", 0) == 0; }

usize scheme_nodes(const Link& link) {
    if (link.graph.empty()) return 0;
    Graph g;
    return g.from_json(link.graph) ? g.nodes.size() : 0;
}

} // namespace forge::logic
