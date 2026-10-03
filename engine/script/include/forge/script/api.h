#pragma once

// The engine's functions for scripts, each described once.
//
// A description gives a function its Luau name (forge.entity.destroy), its
// parameters with types, defaults and titles, and where it sits in the node
// palette. From that one record the script host makes the Luau function and
// the node library (step 6b) makes the node: adding an engine function to
// scripts and nodes is one registration, with no node written by hand.
//
//   api.add("entity.destroy", &destroy)
//       .action()
//       .category("Объекты").icon("delete")
//       .title("Уничтожить", "Destroy")
//       .param("actor", ValueType::Entity, "Объект", "Actor");

#include "forge/core/types.h"

#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace forge::script {

using CFunction = int (*)(lua_State*);

// Types of values that travel between nodes and engine functions. They map
// onto Luau values: Vec2 is a Luau vector (z = 0), Entity is a number.
enum class ValueType : u8 {
    Any,
    Bool,
    Number,
    Integer,
    String,
    Vec2,
    Color,
    Entity,
    Asset,
    Enum,
    Table, // a list or a record (results of searches)
};

const char* value_type_name(ValueType type);
bool parse_value_type(std::string_view name, ValueType& out);

enum class ApiKind : u8 {
    Action, // does something: a block with a flow in and out
    Pure,   // only computes a value from its inputs: a data node
    Latent, // may wait before it finishes (wait, walk to): an action that takes time
};

struct ApiParam {
    std::string id; // stable: graphs refer to it, never to the title
    ValueType type = ValueType::Any;
    std::string enum_type;    // ValueType::Enum: the reflected enum's name
    std::string default_lua;  // Luau literal used when nothing is connected ("" = required)
    std::string title_ru, title_en;
    bool has_range = false;
    f64 min = 0, max = 0;
};

struct ApiFunction {
    std::string name; // dotted: "entity.destroy" is forge.entity.destroy
    CFunction fn = nullptr;
    ApiKind kind = ApiKind::Action;
    std::string category; // palette group, Russian (the editor's language)
    std::string icon;     // Material Symbols name
    std::string title_ru, title_en;
    std::string help_ru;
    std::vector<ApiParam> params;
    std::vector<ApiParam> results;
    bool hidden = false; // for compiled code only, not a node in the palette
};

class ApiBuilder {
public:
    explicit ApiBuilder(ApiFunction& f) : f_(f) {}
    ApiBuilder& action() { f_.kind = ApiKind::Action; return *this; }
    ApiBuilder& pure() { f_.kind = ApiKind::Pure; return *this; }
    ApiBuilder& latent() { f_.kind = ApiKind::Latent; return *this; }
    ApiBuilder& category(const char* name) { f_.category = name; return *this; }
    ApiBuilder& icon(const char* name) { f_.icon = name; return *this; }
    ApiBuilder& title(const char* ru, const char* en) { f_.title_ru = ru; f_.title_en = en; return *this; }
    ApiBuilder& help(const char* ru) { f_.help_ru = ru; return *this; }
    ApiBuilder& hidden() { f_.hidden = true; return *this; }
    // default_lua empty: the input must be connected or filled in.
    ApiBuilder& param(const char* id, ValueType type, const char* ru, const char* en, const char* default_lua = "");
    ApiBuilder& range(f64 min, f64 max); // for the last param
    ApiBuilder& enum_type(const char* name); // for the last param
    ApiBuilder& result(const char* id, ValueType type, const char* ru, const char* en);

private:
    ApiFunction& f_;
};

class ScriptApi {
public:
    // Replaces a function of the same name (modules may override the engine's).
    ApiBuilder add(std::string_view name, CFunction fn);
    const ApiFunction* find(std::string_view name) const;
    const std::vector<ApiFunction>& all() const { return functions_; }

private:
    std::vector<ApiFunction> functions_;
};

} // namespace forge::script
