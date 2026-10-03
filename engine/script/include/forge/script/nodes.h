#pragma once

// The node library: what every node is, described as data.
//
// The engine does not know any particular node. A node is a NodeDef record:
// its pins, slots, palette place and how it turns into Luau. Records come from
//   - *.nodes.json files (the standard library is built in; modules and
//     projects add their own and may override ours by id),
//   - engine functions (ScriptApi): one node per function, automatically,
//   - reflected components: "get/set <field>" nodes for every field,
//   - graphs marked "make a node" (macros): authors grow the library
//     without leaving the editor.
// So adding or changing a node is editing data, never engine code.
//
// Graphs refer to nodes and pins by stable ids, never by titles, and store
// the version of the description they were made with. When a description
// changes, its migrations (renamed pins, new values, a replacement node)
// bring old graphs up to date on load. A node whose description is gone
// stays in the graph untouched ("missing") until its module comes back.
//
// How a node becomes Luau (exactly one of):
//   call   an engine function: forge.<call>(inputs...) with results to outputs
//   lua    a template: an expression for pure nodes, statements otherwise
//          {in:pin}    the input's value (wire, typed value or default)
//          {out:pin}   the variable holding an output
//          {slot:id}   the blocks in a slot, then what its wire leads to
//          {body}      events: what follows the event
//          {self}      this entity
//          {uid}       a number unique to this node (for helper names)
//          {pure}      a line of its own: computes the node's inputs again
//                      (a loop condition is checked on every pass)
//          {vardefault} variable nodes: the default of the graph variable named by the "name" input
//   graph  a graph marked "make a node": called as a local function
//   event  events: the module handler it runs in (on_start, on_tick…);
//          its outputs are the handler's arguments

#include "forge/core/types.h"
#include "forge/data/reflect.h"
#include "forge/script/api.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace forge::script {

struct Graph;

struct LocalText {
    std::string ru, en;
    const std::string& get() const { return ru.empty() ? en : ru; }
};

enum class NodeKind : u8 {
    Event,   // starts a chain: only outputs and a flow out
    Action,  // flow in, flow out ("next"), inputs and outputs
    Pure,    // no flow: computes outputs from inputs whenever they are needed
    Flow,    // branches and loops: flow in and out, plus slots of nested blocks
    Comment, // editor only: a note, a group frame, a reroute dot
};

struct PinDef {
    std::string id; // stable; graphs refer to it
    ValueType type = ValueType::Any;
    std::string enum_type; // ValueType::Enum: the reflected enum's name
    // Default value as text, the way the editor shows it: 0.8, true, door_open,
    // "3, 4" for a Vec2, "self" for this entity.
    std::string value;
    bool required = false; // must be wired or filled in ("Подключите актёра")
    bool constant = false; // a typed value only, never a wire (a variable's name)
    LocalText title;
    bool has_range = false;
    f64 min = 0, max = 0;
};

struct SlotDef {
    std::string id; // also the id of the flow wire that leaves after the slot's blocks
    LocalText title;
};

struct PinRename {
    std::string from, to;
};

struct PinValueDef {
    std::string pin, value;
};

// Brings graphs made with version `from` of the node one version up.
struct NodeMigration {
    u32 from = 1;
    std::vector<PinRename> rename_pins;
    std::vector<PinValueDef> set_values; // for new pins, or new meaning of old ones
    std::string replaced_by;             // the node became another one (same pins after renames)
};

struct NodeDef {
    std::string id; // "std.flow.if", "api.entity.destroy", "comp.Body.vx.get"
    u32 version = 1;
    NodeKind kind = NodeKind::Action;
    std::string category; // palette group
    std::string icon;     // Material Symbols name
    LocalText title, help;
    std::vector<PinDef> inputs, outputs;
    std::vector<SlotDef> slots;
    bool latent = false; // may wait before it is done (not timed by the node profiler)
    bool hidden = false; // not offered in the palette (kept for old graphs)

    std::string call;
    std::string lua;
    std::string graph;
    std::string event;

    std::vector<NodeMigration> migrations;
    std::string module; // where the description came from (filled on load)

    const PinDef* input(std::string_view pin) const;
    const PinDef* output(std::string_view pin) const;
    const SlotDef* slot(std::string_view slot_id) const;
    bool has_flow_in() const { return kind == NodeKind::Action || kind == NodeKind::Flow; }
    bool has_next() const { return kind != NodeKind::Pure && kind != NodeKind::Comment; }
};

// What a *.nodes.json file holds.
struct NodeFile {
    std::string module;
    std::vector<NodeDef> nodes;
};

class NodeLibrary {
public:
    NodeLibrary();
    ~NodeLibrary();

    // The built-in standard library (events, flow, logic, math, variables…).
    bool add_standard(std::vector<std::string>* errors = nullptr);
    // A *.nodes.json text; later files override earlier ones node by node.
    bool add_json(std::string_view json, std::string_view source, std::vector<std::string>* errors = nullptr);
    bool add_file(const std::filesystem::path& path, std::vector<std::string>* errors = nullptr);
    // Every engine function becomes "api.<name>" (hidden ones excepted).
    void add_api(const ScriptApi& api);
    // "comp.<Type>.<field>.get" / ".set" for each field of these components.
    void add_components(const std::vector<const reflect::TypeInfo*>& types);
    // A graph marked as a node; it is compiled into the scripts that use it.
    bool add_graph_node(const Graph& graph, std::vector<std::string>* errors = nullptr);
    void add(NodeDef def);

    const NodeDef* find(std::string_view id) const;
    const Graph* graph(std::string_view name) const; // body of a graph node
    const std::vector<NodeDef>& all() const { return defs_; }

private:
    std::vector<NodeDef> defs_;
    std::unordered_map<std::string, usize> index_;
    std::vector<std::unique_ptr<Graph>> graphs_;
};

} // namespace forge::script

FORGE_REFLECT_DECLARE(forge::script::ValueType)
FORGE_REFLECT_DECLARE(forge::script::NodeKind)
FORGE_REFLECT_DECLARE(forge::script::LocalText)
FORGE_REFLECT_DECLARE(forge::script::PinDef)
FORGE_REFLECT_DECLARE(forge::script::SlotDef)
FORGE_REFLECT_DECLARE(forge::script::PinRename)
FORGE_REFLECT_DECLARE(forge::script::PinValueDef)
FORGE_REFLECT_DECLARE(forge::script::NodeMigration)
FORGE_REFLECT_DECLARE(forge::script::NodeDef)
FORGE_REFLECT_DECLARE(forge::script::NodeFile)
