#include "forge/script/nodes.h"

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/data/json.h"
#include "forge/script/graph.h"

#include <algorithm>

extern const unsigned char forge_std_nodes[];
extern const unsigned long long forge_std_nodes_size;

using namespace forge::script;

FORGE_REFLECT(forge::script::ValueType, 1) {
    t.value("any", ValueType::Any);
    t.value("bool", ValueType::Bool);
    t.value("number", ValueType::Number);
    t.value("integer", ValueType::Integer);
    t.value("string", ValueType::String);
    t.value("vec2", ValueType::Vec2);
    t.value("color", ValueType::Color);
    t.value("entity", ValueType::Entity);
    t.value("asset", ValueType::Asset);
    t.value("enum", ValueType::Enum);
    t.value("table", ValueType::Table);
}
FORGE_REFLECT(forge::script::NodeKind, 1) {
    t.value("event", NodeKind::Event);
    t.value("action", NodeKind::Action);
    t.value("pure", NodeKind::Pure);
    t.value("flow", NodeKind::Flow);
    t.value("comment", NodeKind::Comment);
}
FORGE_REFLECT(forge::script::LocalText, 1) {
    t.field("ru", &LocalText::ru);
    t.field("en", &LocalText::en);
}
FORGE_REFLECT(forge::script::PinDef, 1) {
    t.field("id", &PinDef::id);
    t.field("type", &PinDef::type);
    t.field("enum_type", &PinDef::enum_type);
    t.field("value", &PinDef::value);
    t.field("required", &PinDef::required);
    t.field("constant", &PinDef::constant);
    t.field("title", &PinDef::title);
    t.field("has_range", &PinDef::has_range);
    t.field("min", &PinDef::min);
    t.field("max", &PinDef::max);
}
FORGE_REFLECT(forge::script::SlotDef, 1) {
    t.field("id", &SlotDef::id);
    t.field("title", &SlotDef::title);
}
FORGE_REFLECT(forge::script::PinRename, 1) {
    t.field("from", &PinRename::from);
    t.field("to", &PinRename::to);
}
FORGE_REFLECT(forge::script::PinValueDef, 1) {
    t.field("pin", &PinValueDef::pin);
    t.field("value", &PinValueDef::value);
}
FORGE_REFLECT(forge::script::NodeMigration, 1) {
    t.field("from", &NodeMigration::from);
    t.field("rename_pins", &NodeMigration::rename_pins);
    t.field("set_values", &NodeMigration::set_values);
    t.field("replaced_by", &NodeMigration::replaced_by);
}
FORGE_REFLECT(forge::script::NodeDef, 1) {
    t.field("id", &NodeDef::id);
    t.field("version", &NodeDef::version);
    t.field("kind", &NodeDef::kind);
    t.field("category", &NodeDef::category);
    t.field("icon", &NodeDef::icon);
    t.field("title", &NodeDef::title);
    t.field("help", &NodeDef::help);
    t.field("inputs", &NodeDef::inputs);
    t.field("outputs", &NodeDef::outputs);
    t.field("slots", &NodeDef::slots);
    t.field("latent", &NodeDef::latent);
    t.field("hidden", &NodeDef::hidden);
    t.field("call", &NodeDef::call);
    t.field("lua", &NodeDef::lua);
    t.field("graph", &NodeDef::graph);
    t.field("event", &NodeDef::event);
    t.field("migrations", &NodeDef::migrations);
    t.field("module", &NodeDef::module).transient();
}
FORGE_REFLECT(forge::script::NodeFile, 1) {
    t.field("module", &NodeFile::module);
    t.field("nodes", &NodeFile::nodes);
}

namespace forge::script {

const PinDef* NodeDef::input(std::string_view pin) const {
    for (const PinDef& p : inputs)
        if (p.id == pin) return &p;
    return nullptr;
}

const PinDef* NodeDef::output(std::string_view pin) const {
    for (const PinDef& p : outputs)
        if (p.id == pin) return &p;
    return nullptr;
}

const SlotDef* NodeDef::slot(std::string_view slot_id) const {
    for (const SlotDef& s : slots)
        if (s.id == slot_id) return &s;
    return nullptr;
}

NodeLibrary::NodeLibrary() = default;
NodeLibrary::~NodeLibrary() = default;
NodeLibrary::NodeLibrary(NodeLibrary&&) noexcept = default;
NodeLibrary& NodeLibrary::operator=(NodeLibrary&&) noexcept = default;

void NodeLibrary::add(NodeDef def) {
    auto it = index_.find(def.id);
    if (it != index_.end()) {
        defs_[it->second] = std::move(def);
        return;
    }
    index_[def.id] = defs_.size();
    defs_.push_back(std::move(def));
}

const NodeDef* NodeLibrary::find(std::string_view id) const {
    auto it = index_.find(std::string(id));
    return it == index_.end() ? nullptr : &defs_[it->second];
}

const Graph* NodeLibrary::graph(std::string_view name) const {
    for (const auto& g : graphs_)
        if (g->name == name) return g.get();
    return nullptr;
}

bool NodeLibrary::add_json(std::string_view json, std::string_view source, std::vector<std::string>* errors) {
    NodeFile file;
    data::LoadReport report;
    if (!data::from_json(file, json, report)) {
        if (errors) errors->push_back(std::string(source) + ": " + report.error);
        return false;
    }
    if (errors)
        for (const std::string& w : report.warnings) errors->push_back(std::string(source) + ": " + w);
    bool ok = true;
    for (NodeDef& d : file.nodes) {
        if (d.id.empty()) {
            if (errors) errors->push_back(std::string(source) + ": a node without an id");
            ok = false;
            continue;
        }
        bool reserved = false;
        for (const PinDef& p : d.inputs) reserved |= p.id == kFlowIn;
        for (const PinDef& p : d.outputs) reserved |= p.id == kFlowNext;
        if (reserved) { // flow wires use these names
            if (errors) errors->push_back(std::string(source) + ": " + d.id + ": pins \"in\" and \"next\" are reserved for flow");
            ok = false;
            continue;
        }
        d.module = file.module.empty() ? std::string(source) : file.module;
        add(std::move(d));
    }
    return ok;
}

bool NodeLibrary::add_file(const std::filesystem::path& path, std::vector<std::string>* errors) {
    std::vector<u8> bytes;
    if (!read_file(path, bytes)) {
        if (errors) errors->push_back(path_to_utf8(path) + ": cannot read");
        return false;
    }
    return add_json(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), path_to_utf8(path), errors);
}

bool NodeLibrary::add_standard(std::vector<std::string>* errors) {
    return add_json(std::string_view(reinterpret_cast<const char*>(forge_std_nodes), static_cast<usize>(forge_std_nodes_size)), "std", errors);
}

namespace {

// A Luau default from the API ("\"\"", "nil", "1", "self") as the text the editor shows.
void api_default(const std::string& lua, PinDef& pin) {
    if (lua.empty()) {
        pin.required = true;
    } else if (lua == "nil") {
        pin.value.clear();
    } else if (lua.size() >= 2 && lua.front() == '"' && lua.back() == '"') {
        pin.value = lua.substr(1, lua.size() - 2);
    } else {
        pin.value = lua;
    }
}

PinDef pin_from(const ApiParam& p) {
    PinDef pin;
    pin.id = p.id;
    pin.type = p.type;
    pin.enum_type = p.enum_type;
    pin.title = {p.title_ru, p.title_en};
    pin.has_range = p.has_range;
    pin.min = p.min;
    pin.max = p.max;
    api_default(p.default_lua, pin);
    // "Which object" is this one unless wired otherwise.
    if (pin.required && p.type == ValueType::Entity && p.id == "actor") {
        pin.required = false;
        pin.value = "self";
    }
    return pin;
}

bool field_type(const reflect::TypeInfo* t, ValueType& out) {
    using reflect::Kind;
    switch (t->kind) {
    case Kind::Bool: out = ValueType::Bool; return true;
    case Kind::I8: case Kind::U8: case Kind::I16: case Kind::U16: case Kind::I32: case Kind::U32: case Kind::I64:
    case Kind::U64: out = ValueType::Integer; return true;
    case Kind::F32: case Kind::F64: out = ValueType::Number; return true;
    case Kind::String: out = ValueType::String; return true;
    case Kind::Vec2: out = ValueType::Vec2; return true;
    case Kind::Color: out = ValueType::Color; return true;
    case Kind::Enum: out = ValueType::Enum; return true;
    default: return false;
    }
}

std::string short_name(const std::string& name) {
    const usize colon = name.rfind(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

} // namespace

void NodeLibrary::add_api(const ScriptApi& api) {
    for (const ApiFunction& f : api.all()) {
        if (f.hidden) continue;
        NodeDef d;
        d.id = "api." + f.name;
        d.kind = f.kind == ApiKind::Pure ? NodeKind::Pure : NodeKind::Action;
        d.latent = f.kind == ApiKind::Latent;
        d.category = f.category;
        d.icon = f.icon;
        d.title = {f.title_ru, f.title_en};
        d.help = {f.help_ru, {}};
        for (const ApiParam& p : f.params) d.inputs.push_back(pin_from(p));
        for (const ApiParam& p : f.results) d.outputs.push_back(pin_from(p));
        d.call = f.name;
        d.module = "engine";
        // A description already in the library (from a .nodes.json) wins:
        // it may give better titles or hide the function.
        if (!find(d.id)) add(std::move(d));
    }
}

void NodeLibrary::add_components(const std::vector<const reflect::TypeInfo*>& types) {
    for (const reflect::TypeInfo* t : types) {
        const std::string type = short_name(t->name);
        for (const reflect::FieldInfo& f : t->fields) {
            if (f.flags & (reflect::FieldHidden | reflect::FieldTransient)) continue;
            ValueType vt;
            if (!field_type(f.type, vt)) continue;
            const std::string label = f.label.empty() ? f.name : f.label;
            PinDef actor;
            actor.id = "actor";
            actor.type = ValueType::Entity;
            actor.value = "self";
            actor.title = {"Объект", "Actor"};
            PinDef value;
            value.id = "value";
            value.type = vt;
            value.enum_type = vt == ValueType::Enum ? f.type->name : std::string();
            value.title = {label, f.name};
            if (f.has_range) {
                value.has_range = true;
                value.min = f.min;
                value.max = f.max;
            }

            NodeDef get;
            get.id = "comp." + type + "." + f.name + ".get";
            get.kind = NodeKind::Pure;
            get.category = "Компоненты";
            get.icon = "input";
            get.title = {type + ": " + label, type + ": " + f.name};
            get.inputs = {actor};
            get.outputs = {value};
            get.lua = "forge.get({in:actor}, \"" + t->name + "\", \"" + f.name + "\")";
            get.module = "engine";

            NodeDef set = get;
            set.id = "comp." + type + "." + f.name + ".set";
            set.kind = NodeKind::Action;
            set.icon = "edit";
            set.title = {"Задать " + type + ": " + label, "Set " + type + ": " + f.name};
            value.required = true;
            set.inputs = {actor, value};
            set.outputs.clear();
            set.lua = "forge.set({in:actor}, \"" + t->name + "\", \"" + f.name + "\", {in:value})";
            if (f.flags & reflect::FieldReadOnly) set.hidden = true;

            if (!find(get.id)) add(std::move(get));
            if (!find(set.id)) add(std::move(set));
        }
    }
}

bool NodeLibrary::add_graph_node(const Graph& graph, std::vector<std::string>* errors) {
    if (!graph.as_node.enabled || graph.as_node.id.empty() || graph.name.empty()) {
        if (errors) errors->push_back("graph \"" + graph.name + "\" is not marked as a node (id and name needed)");
        return false;
    }
    NodeDef d;
    d.id = graph.as_node.id;
    d.kind = graph.as_node.kind == NodeKind::Pure ? NodeKind::Pure : NodeKind::Action;
    d.category = graph.as_node.category.empty() ? "Мои ноды" : graph.as_node.category;
    d.icon = graph.as_node.icon.empty() ? "widgets" : graph.as_node.icon;
    d.title = graph.as_node.title.get().empty() ? LocalText{graph.name, graph.name} : graph.as_node.title;
    d.help = graph.as_node.help;
    d.inputs = graph.inputs;
    d.outputs = graph.outputs;
    d.graph = graph.name;
    d.module = "graph:" + graph.name;
    for (auto& g : graphs_)
        if (g->name == graph.name) {
            *g = graph;
            add(std::move(d));
            return true;
        }
    graphs_.push_back(std::make_unique<Graph>(graph));
    add(std::move(d));
    return true;
}

} // namespace forge::script
