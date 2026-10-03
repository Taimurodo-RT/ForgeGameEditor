#pragma once

// A node graph document: what the editor saves and what the compiler reads.
//
// Nodes are placed on the canvas (x, y) or nested in a slot of another node
// (then their order in the slot is the order they run in). Wires (links) join
// an output to an input: flow wires go from "next" or a slot id to "in", data
// wires from an output pin to an input pin. Inputs with no wire use the typed
// value stored on the node, or the pin's default.
//
// The same document serves scripts, dialogues and animation state machines:
// they differ in node library and compiler, not in format.

#include "forge/core/types.h"
#include "forge/script/nodes.h"

#include <string>
#include <string_view>
#include <vector>

namespace forge::script {

constexpr const char* kFlowIn = "in";
constexpr const char* kFlowNext = "next";

struct PinValue {
    std::string pin, value;
};

struct SlotContent {
    std::string slot;
    std::vector<u32> nodes; // uids, in the order they run
};

struct GraphNode {
    u32 uid = 0; // unique in the graph, never reused
    std::string def;
    u32 version = 1; // of the description it was made with
    f32 x = 0, y = 0;
    std::vector<PinValue> values;
    std::vector<SlotContent> slots;
    std::string comment;
    bool collapsed = false;

    const std::string* value(std::string_view pin) const;
    void set_value(std::string_view pin, std::string_view value);
    const SlotContent* slot(std::string_view id) const;
};

struct GraphLink {
    u32 from_node = 0;
    std::string from_pin;
    u32 to_node = 0;
    std::string to_pin;
};

// Variables of the entity running the graph (stored in its ScriptVars).
struct GraphVar {
    std::string name;
    ValueType type = ValueType::Number;
    std::string value; // starting value, as text
    LocalText title;
};

// Settings of a graph that is offered as a node.
struct GraphAsNode {
    bool enabled = false;
    std::string id; // node id, e.g. "my.open_door"
    NodeKind kind = NodeKind::Action; // Action or Pure
    std::string category, icon;
    LocalText title, help;
};

struct Graph {
    std::string kind = "script";
    std::string name;
    std::vector<GraphVar> variables;
    // A graph used as a node: its pins. "std.graph.entry" outputs the
    // inputs; "std.graph.return" takes the outputs.
    std::vector<PinDef> inputs, outputs;
    GraphAsNode as_node;
    std::vector<GraphNode> nodes;
    std::vector<GraphLink> links;
    u32 next_uid = 1;

    GraphNode* find(u32 uid);
    const GraphNode* find(u32 uid) const;
    const GraphVar* variable(std::string_view name) const;

    // Building, as the editor and tests do.
    GraphNode& add(std::string_view def, f32 x = 0, f32 y = 0);
    void link(u32 from, std::string_view from_pin, u32 to, std::string_view to_pin = kFlowIn);
    void put_in_slot(u32 parent, std::string_view slot, u32 child);
    void remove(u32 uid); // with its links and slot places

    std::string to_json() const;
    bool from_json(std::string_view json, std::string* error = nullptr);
};

// Brings nodes made with older descriptions up to date (renamed pins,
// replaced nodes). Notes say what changed, for the editor's log.
void migrate(Graph& graph, const NodeLibrary& library, std::vector<std::string>* notes = nullptr);

} // namespace forge::script

FORGE_REFLECT_DECLARE(forge::script::PinValue)
FORGE_REFLECT_DECLARE(forge::script::SlotContent)
FORGE_REFLECT_DECLARE(forge::script::GraphNode)
FORGE_REFLECT_DECLARE(forge::script::GraphLink)
FORGE_REFLECT_DECLARE(forge::script::GraphVar)
FORGE_REFLECT_DECLARE(forge::script::GraphAsNode)
FORGE_REFLECT_DECLARE(forge::script::Graph)
