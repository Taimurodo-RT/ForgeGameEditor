#pragma once

// Internals of ScriptHost shared by host.cpp and the engine functions.

#include "forge/script/host.h"

#include <lua.h>
#include <lualib.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace forge::script {

enum Handler : u8 { OnStart, OnTick, OnEnter, OnLeave, OnHit, OnMessage, kHandlerCount };
extern const char* const kHandlerNames[kHandlerCount];

struct ScriptHost::Impl {
    struct Module {
        std::string name;
        int table_ref = LUA_NOREF;
        std::vector<int> fn[kHandlerCount]; // refs of the handler functions
        SourceMap map;
    };
    struct Exposed {
        const reflect::TypeInfo* type = nullptr;
        flecs::entity_t id = 0;
    };
    struct Wait {
        lua_State* thread = nullptr;
        flecs::entity_t entity = 0;
        f64 remaining = 0;
        bool real = false; // counts real seconds, not world time
        std::string module;
        Handler handler = OnTick;
    };
    struct Message {
        flecs::entity_t to = 0, from = 0;
        std::string name;
        f64 value = 0;
    };

    ScriptHost& host;
    sim::Simulation& sim;
    scene::Scene& scene;
    flecs::world& ecs;

    std::unordered_map<std::string, Module> modules;
    struct Timing {
        u64 calls = 0;
        u64 ns = 0;
    };
    std::unordered_map<std::string, std::unordered_map<u32, Timing>> profile;
    // The profiled node running now. Timed nodes never wait, so they never
    // overlap: one start is enough.
    Timing* prof_open = nullptr;
    u64 prof_t0 = 0;
    // profile[current module], looked up once per module change.
    const Module* prof_module = nullptr;
    std::unordered_map<u32, Timing>* prof_nodes = nullptr;
    void forget_profile() { prof_module = nullptr, prof_nodes = nullptr, prof_open = nullptr; }
    std::unordered_map<std::string, Exposed> exposed;
    std::vector<lua_State*> pool;               // idle threads, reset
    std::unordered_map<lua_State*, int> refs;   // every thread we made -> its registry ref
    std::unordered_map<lua_State*, u64> calls;  // threads in use -> the call each runs (ScriptHost::running_call)
    u64 last_call = 0;
    std::vector<Wait> waits;
    std::vector<Message> outbox, inbox;
    std::vector<sim::TriggerEvent> areas; // ScriptHost::enter, for the next dispatch of triggers
    std::vector<std::pair<flecs::entity_t, f32>> due; // entities that run this tick and their dt, sorted when waits need it
    flecs::query<scene::Position, Script> scripted;
    flecs::entity_t script_id = 0, vars_id = 0, body_id = 0;

    // The call running now.
    flecs::entity_t current_entity = 0;
    u64 current_call = 0;
    const Module* current_module = nullptr;
    bool in_call = false;
    u64 call_start_ns = 0; // 0 until the budget check first looks at the clock
    u64 budget_ns = 0;
    u32 interrupts = 0;
    bool timed_out = false;
    f64 pending_wait = -1;
    bool pending_real = false;
    const sim::TickContext* ctx = nullptr;
    u64 rng = 1;
    bool api_ready = false; // the forge table is built (then the globals freeze)

    Impl(ScriptHost& h, sim::Simulation& s, scene::Scene& sc)
        : host(h), sim(s), scene(sc), ecs(sc.ecs()) {}

    void begin_call() {
        in_call = true;
        call_start_ns = 0;
        interrupts = 0;
        timed_out = false;
    }
    lua_State* acquire(lua_State* L);
    void release(lua_State* thread, bool finished);
    Exposed* find_exposed(std::string_view name);
};

ScriptHost::Impl* impl_of(lua_State* L);

// Values between reflected fields and Luau.
void push_field(lua_State* L, const reflect::TypeInfo* type, const void* at);
bool read_field(lua_State* L, int idx, const reflect::TypeInfo* type, void* at);

inline flecs::entity_t check_entity(lua_State* L, int idx) {
    return static_cast<flecs::entity_t>(luaL_checknumber(L, idx));
}
inline void push_entity(lua_State* L, flecs::entity_t e) { lua_pushnumber(L, static_cast<double>(e)); }

} // namespace forge::script
