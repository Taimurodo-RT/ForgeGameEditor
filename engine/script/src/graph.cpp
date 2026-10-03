#include "forge/script/graph.h"

#include "forge/data/json.h"

#include <algorithm>

using namespace forge::script;

FORGE_REFLECT(forge::script::PinValue, 1) {
    t.field("pin", &PinValue::pin);
    t.field("value", &PinValue::value);
}
FORGE_REFLECT(forge::script::SlotContent, 1) {
    t.field("slot", &SlotContent::slot);
    t.field("nodes", &SlotContent::nodes);
}
FORGE_REFLECT(forge::script::GraphNode, 1) {
    t.field("uid", &GraphNode::uid);
    t.field("def", &GraphNode::def);
    t.field("version", &GraphNode::version);
    t.field("x", &GraphNode::x);
    t.field("y", &GraphNode::y);
    t.field("values", &GraphNode::values);
    t.field("slots", &GraphNode::slots);
    t.field("comment", &GraphNode::comment);
    t.field("collapsed", &GraphNode::collapsed);
}
FORGE_REFLECT(forge::script::GraphLink, 1) {
    t.field("from_node", &GraphLink::from_node);
    t.field("from_pin", &GraphLink::from_pin);
    t.field("to_node", &GraphLink::to_node);
    t.field("to_pin", &GraphLink::to_pin);
}
FORGE_REFLECT(forge::script::GraphVar, 1) {
    t.field("name", &GraphVar::name);
    t.field("type", &GraphVar::type);
    t.field("value", &GraphVar::value);
    t.field("title", &GraphVar::title);
}
FORGE_REFLECT(forge::script::GraphAsNode, 1) {
    t.field("enabled", &GraphAsNode::enabled);
    t.field("id", &GraphAsNode::id);
    t.field("kind", &GraphAsNode::kind);
    t.field("category", &GraphAsNode::category);
    t.field("icon", &GraphAsNode::icon);
    t.field("title", &GraphAsNode::title);
    t.field("help", &GraphAsNode::help);
}
FORGE_REFLECT(forge::script::Graph, 1) {
    t.field("kind", &Graph::kind);
    t.field("name", &Graph::name);
    t.field("variables", &Graph::variables);
    t.field("inputs", &Graph::inputs);
    t.field("outputs", &Graph::outputs);
    t.field("as_node", &Graph::as_node);
    t.field("nodes", &Graph::nodes);
    t.field("links", &Graph::links);
    t.field("next_uid", &Graph::next_uid);
}

namespace forge::script {

const std::string* GraphNode::value(std::string_view pin) const {
    for (const PinValue& v : values)
        if (v.pin == pin) return &v.value;
    return nullptr;
}

void GraphNode::set_value(std::string_view pin, std::string_view value) {
    for (PinValue& v : values)
        if (v.pin == pin) {
            v.value = std::string(value);
            return;
        }
    values.push_back({std::string(pin), std::string(value)});
}

const SlotContent* GraphNode::slot(std::string_view id) const {
    for (const SlotContent& s : slots)
        if (s.slot == id) return &s;
    return nullptr;
}

GraphNode* Graph::find(u32 uid) {
    for (GraphNode& n : nodes)
        if (n.uid == uid) return &n;
    return nullptr;
}

const GraphNode* Graph::find(u32 uid) const { return const_cast<Graph*>(this)->find(uid); }

const GraphVar* Graph::variable(std::string_view var) const {
    for (const GraphVar& v : variables)
        if (v.name == var) return &v;
    return nullptr;
}

GraphNode& Graph::add(std::string_view def, f32 x, f32 y) {
    GraphNode n;
    n.uid = next_uid++;
    n.def = std::string(def);
    n.x = x;
    n.y = y;
    nodes.push_back(std::move(n));
    return nodes.back();
}

void Graph::link(u32 from, std::string_view from_pin, u32 to, std::string_view to_pin) {
    links.push_back({from, std::string(from_pin), to, std::string(to_pin)});
}

void Graph::put_in_slot(u32 parent, std::string_view slot, u32 child) {
    GraphNode* p = find(parent);
    if (!p) return;
    for (SlotContent& s : p->slots)
        if (s.slot == slot) {
            s.nodes.push_back(child);
            return;
        }
    p->slots.push_back({std::string(slot), {child}});
}

void Graph::remove(u32 uid) {
    std::erase_if(nodes, [&](const GraphNode& n) { return n.uid == uid; });
    std::erase_if(links, [&](const GraphLink& l) { return l.from_node == uid || l.to_node == uid; });
    for (GraphNode& n : nodes)
        for (SlotContent& s : n.slots) std::erase(s.nodes, uid);
}

std::string Graph::to_json() const { return data::to_json(*this); }

bool Graph::from_json(std::string_view json, std::string* error) {
    Graph g;
    data::LoadReport report;
    if (!data::from_json(g, json, report)) {
        if (error) *error = report.error;
        return false;
    }
    for (const GraphNode& n : g.nodes) g.next_uid = std::max(g.next_uid, n.uid + 1);
    *this = std::move(g);
    return true;
}

void migrate(Graph& graph, const NodeLibrary& library, std::vector<std::string>* notes) {
    for (GraphNode& n : graph.nodes) {
        const NodeDef* def = library.find(n.def);
        // Follow descriptions until the node is current (a replacement may
        // itself have been replaced later).
        for (int guard = 0; def && n.version < def->version && guard < 64; ++guard) {
            const NodeMigration* step = nullptr;
            for (const NodeMigration& m : def->migrations)
                if (m.from == n.version) step = &m;
            if (!step) { // nothing to do between these versions
                n.version = def->version;
                break;
            }
            for (const PinRename& r : step->rename_pins) {
                for (PinValue& v : n.values)
                    if (v.pin == r.from) v.pin = r.to;
                for (GraphLink& l : graph.links) {
                    if (l.to_node == n.uid && l.to_pin == r.from) l.to_pin = r.to;
                    if (l.from_node == n.uid && l.from_pin == r.from) l.from_pin = r.to;
                }
                for (SlotContent& s : n.slots)
                    if (s.slot == r.from) s.slot = r.to;
            }
            for (const PinValueDef& v : step->set_values)
                if (!n.value(v.pin)) n.set_value(v.pin, v.value);
            if (!step->replaced_by.empty()) {
                if (notes) notes->push_back("node " + std::to_string(n.uid) + ": " + n.def + " -> " + step->replaced_by);
                n.def = step->replaced_by;
                def = library.find(n.def);
                n.version = 1;
                continue;
            }
            if (notes)
                notes->push_back("node " + std::to_string(n.uid) + ": " + n.def + " v" + std::to_string(n.version) + " -> v" +
                                 std::to_string(n.version + 1));
            ++n.version;
        }
    }
}

} // namespace forge::script
