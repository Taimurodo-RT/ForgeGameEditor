#pragma once

// Scripts: Luau code that gives entities their behaviour.
//
// An entity with a Script component runs the module named there. A module is
// a Luau table of handlers, written by hand or compiled from a node graph:
//
//   local S = {}
//   function S.on_start(self) end              -- once, the first tick it is awake
//   function S.on_tick(self, dt) end           -- every tick its chunk is due; dt is its own world time
//   function S.on_enter(self, other) end       -- something entered its Trigger
//   function S.on_leave(self, other) end
//   function S.on_hit(self, other) end         -- its rigid body started touching another (0: tiles)
//   function S.on_message(self, name, value, from) end
//   return S
//
// A handler may also be a list of functions (S.on_tick = {f, g}): each runs
// on its own, so one of them waiting does not hold up the others. Compiled
// graphs use this for several events of the same kind.
//
// Engine functions live in the `forge` table (see api.h and api_core.cpp).
//
// Safety: each module runs in its own sandboxed environment (no files, no
// os); a call that runs longer than the budget is stopped; an error stops the
// scripts of that one entity and is reported with the line (and the node, for
// compiled graphs), the rest of the game goes on.
//
// Time: forge.wait(s) pauses a handler for s seconds of that entity's world
// time, so slow motion, stopped time, time bubbles and OutsideTime apply to
// scripts as they do to bodies. Entities in asleep chunks do not run.
//
// State that must last (saves, reloading a script) belongs in components or
// in ScriptVars, not in Luau locals: a module can be replaced while the game
// runs and nothing is lost.

#include "forge/core/types.h"
#include "forge/data/reflect.h"
#include "forge/script/api.h"
#include "forge/sim/simulation.h"

#include <flecs.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace forge::script {

// Saved with the entity.
struct Script {
    std::string name;   // module to run
    bool started = false; // on_start already ran
    bool failed = false;  // an error stopped it (not saved: a reload or a new game tries again)
};

enum class VarKind : u8 { None, Bool, Number, String, Vec2 };

struct ScriptVar {
    std::string name;
    VarKind kind = VarKind::None;
    f64 x = 0, y = 0; // Bool: x != 0; Number: x; Vec2: x, y
    std::string text;
};

// Variables of one entity, set from scripts and nodes ("Дверь открыта").
// Saved with the entity and shown in the inspector.
struct ScriptVars {
    std::vector<ScriptVar> vars;
    const ScriptVar* find(std::string_view name) const;
    ScriptVar& get_or_add(std::string_view name);
};

// Which node made each line of a compiled graph (1-based lines).
struct SourceMap {
    std::vector<u32> line_node; // [line - 1] -> node index, kNoNode where none
    static constexpr u32 kNoNode = ~0u;
    u32 node_at(i32 line) const {
        return line >= 1 && static_cast<usize>(line) <= line_node.size() ? line_node[static_cast<usize>(line - 1)] : kNoNode;
    }
};

struct ScriptError {
    std::string script;
    std::string handler; // "on_tick", "load"…
    flecs::entity_t entity = 0;
    i32 line = 0;
    u32 node = SourceMap::kNoNode;
    std::string message;
};

struct ScriptDesc {
    f64 budget_ms = 5;      // longest a single call may run
    bool native = true;     // compile to machine code where Luau supports it (x64, arm64)
    u32 max_errors = 256;   // kept for the editor; older ones are dropped
    u64 seed = 1;           // forge.random
};

// Time spent in one node of a graph compiled with profiling on.
struct NodeTime {
    std::string script;
    u32 node = 0;
    u64 calls = 0;
    f64 ms = 0; // total; blocks that may wait (flow, latent, graph nodes) are not timed
};

struct ScriptStats {
    u32 modules = 0;
    u32 scripted = 0; // entities with a Script in the last tick
    u32 calls = 0;    // handler calls in the last tick
    u32 waiting = 0;  // handlers paused in forge.wait
    u32 messages = 0; // delivered in the last tick
    u32 errors = 0;   // since the start
    f64 ms = 0;       // last tick
    usize memory = 0; // bytes the Luau heap uses
};

class ScriptHost {
public:
    // Adds itself to the simulation's systems: scripts run every tick after
    // the systems added before it.
    ScriptHost(sim::Simulation& sim, scene::Scene& scene, const ScriptDesc& desc = {});
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Engine functions. The core set is there from the start; add more
    // before loading modules that use them.
    ScriptApi& api() { return api_; }
    // The `forge` table is made at the first load, run or tick: functions
    // added after that are not seen by scripts.
    bool api_built() const;

    // Lets scripts read and write a component by name through its
    // reflection: forge.get(e, "Health", "current"). Simulation components
    // (Body, RigidBody, Trigger, KeepAwake, GravitySource, TimeBubble,
    // OutsideTime, Script) are exposed already.
    template <typename T>
    void expose() {
        expose(reflect::type_of<T>(), scene_.ecs().component<T>().id());
    }
    void expose(const reflect::TypeInfo* type, flecs::entity_t component);
    // The exposed components, for the node library's field nodes.
    std::vector<const reflect::TypeInfo*> exposed_types() const;

    // Compiles and loads a module, or replaces it (paused handlers of the old
    // version finish as they were). On failure the old version stays.
    bool load(std::string_view name, std::string_view source, const SourceMap* map = nullptr,
              std::vector<ScriptError>* errors = nullptr);
    bool loaded(std::string_view name) const;

    // Runs a piece of code once, outside any entity (console, tests).
    bool run(std::string_view source, std::string* error = nullptr);

    // An area the game checks itself, not a Trigger (a named place of the
    // level the hero came into or left): its on_enter / on_leave, run with
    // the triggers of the tick under way (else the next one). It need not
    // have a Position: an entity in no chunk waits in the world's time.
    void enter(flecs::entity_t area, flecs::entity_t other, bool entered);

    // A message for an entity's on_message, delivered at the next tick.
    // to = 0: to every scripted entity running then (a button's «Сообщение
    // логике»).
    void send(flecs::entity_t to, std::string_view message, f64 value = 0, flecs::entity_t from = 0);

    // Per-node time of graphs compiled with CompileOptions::profile, slowest
    // first, since the start or the last reset.
    std::vector<NodeTime> node_profile() const;
    void reset_node_profile();

    const std::vector<ScriptError>& errors() const { return errors_; }
    void clear_errors() { errors_.clear(); }
    const ScriptStats& stats() const { return stats_; }
    lua_State* state() { return L_; }

    // World time that passed for scripts (seconds, scaled by time speed).
    f64 world_time() const { return world_time_; }

    // For engine functions registered from outside the script module: the
    // host a Luau state belongs to, and pointers they keep with it by name.
    static ScriptHost& of(lua_State* L);
    void set_user(std::string_view key, void* value);
    void* user(std::string_view key) const;
    // The entity whose handler is running (0 outside handlers).
    flecs::entity_t running() const;
    scene::Scene& scene() { return scene_; }

    struct Impl; // shared with the engine functions in api_core.cpp

private:
    void tick(const sim::TickContext& ctx);

    sim::Simulation& sim_;
    scene::Scene& scene_;
    ScriptDesc desc_;
    ScriptApi api_;
    lua_State* L_ = nullptr;
    std::unique_ptr<Impl> impl_;
    std::vector<ScriptError> errors_;
    std::vector<std::pair<std::string, void*>> users_;
    ScriptStats stats_;
    f64 world_time_ = 0;
};

// What scripts reach of the game around the world: its variables (the ones
// dialogues, quests, saves and screens use: "hero.hearts", "inv.coins") and
// its screens. The game sets it with set_user(kGameBridge, &bridge); without
// one, variables read 0 and screens do nothing.
inline constexpr std::string_view kGameBridge = "game";
class GameBridge {
public:
    virtual ~GameBridge() = default;
    // A number, or a text (text set): what the variable holds.
    virtual f64 var(std::string_view name, std::string* text) = 0;
    virtual void set_var(std::string_view name, f64 number) = 0;
    virtual void set_text(std::string_view name, std::string_view text) = 0;
    // "show", "hide" or "toggle" a screen by its name.
    virtual void screen(std::string_view what, std::string_view name) = 0;
};

// The engine's own functions (log, wait, entities, variables, time…).
void register_core_api(ScriptApi& api);

// Whether Luau source compiles; the compiler's message otherwise.
bool check_syntax(std::string_view source, std::string* error = nullptr);

} // namespace forge::script

FORGE_REFLECT_DECLARE(forge::script::Script)
FORGE_REFLECT_DECLARE(forge::script::VarKind)
FORGE_REFLECT_DECLARE(forge::script::ScriptVar)
FORGE_REFLECT_DECLARE(forge::script::ScriptVars)
