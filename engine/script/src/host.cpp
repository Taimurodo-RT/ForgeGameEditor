#include "host_impl.h"

#include "forge/core/log.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"

#include <luacode.h>
#include <luacodegen.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

FORGE_REFLECT(forge::script::Script, 1) {
    t.field("name", &forge::script::Script::name).label("Скрипт");
    t.field("started", &forge::script::Script::started).hidden();
    t.field("failed", &forge::script::Script::failed).transient().read_only();
}
FORGE_REFLECT(forge::script::VarKind, 1) {
    t.value("None", forge::script::VarKind::None);
    t.value("Bool", forge::script::VarKind::Bool);
    t.value("Number", forge::script::VarKind::Number);
    t.value("String", forge::script::VarKind::String);
    t.value("Vec2", forge::script::VarKind::Vec2);
}
FORGE_REFLECT(forge::script::ScriptVar, 1) {
    t.field("name", &forge::script::ScriptVar::name);
    t.field("kind", &forge::script::ScriptVar::kind);
    t.field("x", &forge::script::ScriptVar::x);
    t.field("y", &forge::script::ScriptVar::y);
    t.field("text", &forge::script::ScriptVar::text);
}
FORGE_REFLECT(forge::script::ScriptVars, 1) { t.field("vars", &forge::script::ScriptVars::vars).label("Переменные"); }

namespace forge::script {

const char* const kHandlerNames[kHandlerCount] = {"on_start", "on_tick", "on_enter", "on_leave", "on_hit", "on_message"};

const ScriptVar* ScriptVars::find(std::string_view name) const {
    for (const ScriptVar& v : vars)
        if (v.name == name) return &v;
    return nullptr;
}

ScriptVar& ScriptVars::get_or_add(std::string_view name) {
    for (ScriptVar& v : vars)
        if (v.name == name) return v;
    vars.push_back({});
    vars.back().name = std::string(name);
    return vars.back();
}

ScriptHost::Impl* impl_of(lua_State* L) { return static_cast<ScriptHost::Impl*>(lua_callbacks(L)->userdata); }

ScriptHost& ScriptHost::of(lua_State* L) { return impl_of(L)->host; }

void ScriptHost::set_user(std::string_view key, void* value) {
    for (auto& [k, v] : users_)
        if (k == key) {
            v = value;
            return;
        }
    users_.emplace_back(std::string(key), value);
}

void* ScriptHost::user(std::string_view key) const {
    for (const auto& [k, v] : users_)
        if (k == key) return v;
    return nullptr;
}

flecs::entity_t ScriptHost::running() const { return impl_->current_entity; }

bool ScriptHost::api_built() const { return impl_->api_ready; }

lua_State* ScriptHost::Impl::acquire(lua_State* L) {
    if (!pool.empty()) {
        lua_State* t = pool.back();
        pool.pop_back();
        return t;
    }
    lua_State* t = lua_newthread(L);
    refs[t] = lua_ref(L, -1);
    lua_pop(L, 1);
    return t;
}

void ScriptHost::Impl::release(lua_State* thread, bool finished) {
    // A thread whose call returned is clean but for its results; one that
    // failed or is dropped while waiting needs a full reset.
    if (finished) lua_settop(thread, 0);
    else lua_resetthread(thread);
    pool.push_back(thread);
}

ScriptHost::Impl::Exposed* ScriptHost::Impl::find_exposed(std::string_view name) {
    auto it = exposed.find(std::string(name));
    return it == exposed.end() ? nullptr : &it->second;
}

namespace {

// Stops a call that runs past its budget. Checking the clock at every loop
// turn would cost more than the loop, so it looks every 256 safepoints.
void on_interrupt(lua_State* L, int gc) {
    if (gc >= 0) return;
    ScriptHost::Impl* im = impl_of(L);
    if (!im->in_call) return;
    if (!im->timed_out) {
        if ((++im->interrupts & 255) != 0) return;
        // The clock starts at the first look: short calls never read it.
        const u64 now = time_now_ns();
        if (im->call_start_ns == 0) im->call_start_ns = now;
        if (now - im->call_start_ns <= im->budget_ns) return;
        im->timed_out = true;
    }
    // Keeps failing until the call is over, even if the script catches it.
    // luaL_error cannot see the running line from here: add it ourselves.
    lua_Debug ar{};
    const bool where = lua_getinfo(L, 0, "sl", &ar) && ar.currentline > 0;
    lua_pushfstring(L, "%s:%d: скрипт выполнялся дольше %.0f мс (бесконечный цикл?)", where ? ar.short_src : "?",
                    where ? ar.currentline : 0, static_cast<f64>(im->budget_ns) / 1e6);
    lua_error(L);
}

// "door:12: attempt to index nil" -> line 12, the text after it.
void parse_error(const std::string& raw, i32& line, std::string& message) {
    line = 0;
    message = raw;
    usize i = raw.find(':');
    while (i != std::string::npos) {
        usize j = i + 1;
        while (j < raw.size() && raw[j] >= '0' && raw[j] <= '9') ++j;
        if (j > i + 1 && j < raw.size() && raw[j] == ':') {
            line = std::atoi(raw.c_str() + i + 1);
            message = raw.substr(j + 1);
            if (!message.empty() && message[0] == ' ') message.erase(0, 1);
            return;
        }
        i = raw.find(':', i + 1);
    }
}

bool alive(flecs::world& ecs, flecs::entity_t e) { return e != 0 && ecs_is_alive(ecs.c_ptr(), e); }

} // namespace

ScriptHost::ScriptHost(sim::Simulation& sim, scene::Scene& scene, const ScriptDesc& desc)
    : sim_(sim), scene_(scene), desc_(desc), impl_(std::make_unique<Impl>(*this, sim, scene)) {
    Impl& im = *impl_;
    im.budget_ns = static_cast<u64>(desc_.budget_ms * 1e6);
    im.rng = desc_.seed ? desc_.seed : 1;

    scene_.register_component<Script>();
    scene_.register_component<ScriptVars>();
    flecs::world& ecs = scene_.ecs();
    im.script_id = ecs.component<Script>().id();
    im.vars_id = ecs.component<ScriptVars>().id();
    im.body_id = ecs.component<sim::Body>().id();
    im.scripted = ecs.query<scene::Position, Script>();

    expose<Script>();
    expose<sim::Body>();
    expose<sim::RigidBody>();
    expose<sim::Trigger>();
    expose<sim::KeepAwake>();
    expose<sim::GravitySource>();
    expose<sim::TimeBubble>();
    expose<sim::OutsideTime>();

    L_ = luaL_newstate();
    lua_callbacks(L_)->userdata = impl_.get();
    lua_callbacks(L_)->interrupt = &on_interrupt;
    luaL_openlibs(L_);
    if (desc_.native && luau_codegen_supported()) luau_codegen_create(L_);

    register_core_api(api_);
    sim_.add_system([this](const sim::TickContext& ctx) { tick(ctx); });
}

ScriptHost::~ScriptHost() {
    impl_->scripted.destruct();
    lua_close(L_);
}

void ScriptHost::expose(const reflect::TypeInfo* type, flecs::entity_t component) {
    impl_->exposed[type->name] = {type, component};
    // Short names too: "Body" for "forge::sim::Body".
    const usize colon = type->name.rfind(':');
    if (colon != std::string::npos) impl_->exposed[type->name.substr(colon + 1)] = {type, component};
}

namespace {

// Builds the forge table from the API once, then freezes the globals. After
// this nothing can be added to forge (functions must be registered first).
void build_api(lua_State* L, const ScriptApi& api) {
    lua_newtable(L); // forge
    for (const ApiFunction& f : api.all()) {
        // Walk / create the tables of a dotted name.
        int depth = 0;
        usize start = 0;
        for (;;) {
            const usize dot = f.name.find('.', start);
            if (dot == std::string::npos) break;
            const std::string part = f.name.substr(start, dot - start);
            lua_getfield(L, -1, part.c_str());
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_newtable(L);
                lua_pushvalue(L, -1);
                lua_setfield(L, -3, part.c_str());
            }
            ++depth;
            start = dot + 1;
        }
        const std::string leaf = f.name.substr(start);
        lua_pushcfunction(L, f.fn, f.name.c_str());
        lua_setfield(L, -2, leaf.c_str());
        lua_pop(L, depth);
    }
    lua_setglobal(L, "forge");
    luaL_sandbox(L);
}

} // namespace

bool ScriptHost::load(std::string_view name, std::string_view source, const SourceMap* map, std::vector<ScriptError>* errors) {
    FORGE_ZONE();
    Impl& im = *impl_;
    if (!im.api_ready) {
        build_api(L_, api_);
        im.api_ready = true;
    }

    auto fail = [&](const std::string& raw, const char* handler) {
        ScriptError e;
        e.script = std::string(name);
        e.handler = handler;
        parse_error(raw, e.line, e.message);
        if (map) e.node = map->node_at(e.line);
        FORGE_WARN("script %s: %s", e.script.c_str(), raw.c_str());
        if (errors) errors->push_back(e);
        if (errors_.size() >= desc_.max_errors) errors_.erase(errors_.begin());
        errors_.push_back(std::move(e));
        ++stats_.errors;
        return false;
    };

    lua_CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    size_t size = 0;
    char* bytecode = luau_compile(source.data(), source.size(), &opts, &size);
    const std::string chunk = "=" + std::string(name);

    lua_State* T = lua_newthread(L_);
    luaL_sandboxthread(T); // its own globals: modules cannot touch each other's
    const int loaded = luau_load(T, chunk.c_str(), bytecode, size, 0);
    std::free(bytecode);
    if (loaded != 0) {
        const std::string msg = lua_tostring(T, -1);
        lua_pop(L_, 1);
        return fail(msg, "load");
    }
    if (desc_.native && luau_codegen_supported()) luau_codegen_compile(T, -1);

    im.begin_call();
    const int status = lua_resume(T, L_, 0);
    im.in_call = false;
    if (status != LUA_OK) {
        const std::string msg = status == LUA_YIELD ? std::string("модуль не может ждать при загрузке") : lua_tostring(T, -1);
        lua_pop(L_, 1);
        return fail(msg, "load");
    }
    if (lua_gettop(T) < 1 || !lua_istable(T, -1)) {
        lua_pop(L_, 1);
        return fail("модуль должен вернуть таблицу обработчиков (return S)", "load");
    }

    Impl::Module m;
    m.name = std::string(name);
    if (map) m.map = *map;
    m.table_ref = lua_ref(T, -1);
    for (int h = 0; h < kHandlerCount; ++h) {
        lua_getfield(T, -1, kHandlerNames[h]);
        if (lua_isfunction(T, -1)) {
            m.fn[h].push_back(lua_ref(T, -1));
        } else if (lua_istable(T, -1)) { // a list of handlers, each run on its own
            const int n = lua_objlen(T, -1);
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(T, -1, i);
                if (lua_isfunction(T, -1)) m.fn[h].push_back(lua_ref(T, -1));
                lua_pop(T, 1);
            }
        }
        lua_pop(T, 1);
    }
    lua_pop(L_, 1); // the loading thread; the module's functions keep its globals

    auto it = im.modules.find(m.name);
    if (it != im.modules.end()) {
        lua_unref(L_, it->second.table_ref);
        for (const std::vector<int>& refs : it->second.fn)
            for (int r : refs) lua_unref(L_, r);
        im.profile.erase(m.name); // its nodes may have changed
        im.forget_profile();
        it->second = std::move(m);
    } else {
        im.modules.emplace(m.name, std::move(m));
    }
    stats_.modules = static_cast<u32>(im.modules.size());
    return true;
}

std::vector<const reflect::TypeInfo*> ScriptHost::exposed_types() const {
    std::vector<const reflect::TypeInfo*> out;
    for (const auto& [name, ex] : impl_->exposed)
        if (name == ex.type->name) out.push_back(ex.type); // full names only, not the short aliases
    std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->name < b->name; });
    return out;
}

std::vector<NodeTime> ScriptHost::node_profile() const {
    std::vector<NodeTime> out;
    for (const auto& [script, nodes] : impl_->profile)
        for (const auto& [node, t] : nodes) out.push_back({script, node, t.calls, static_cast<f64>(t.ns) / 1e6});
    std::sort(out.begin(), out.end(), [](const NodeTime& a, const NodeTime& b) { return a.ms > b.ms; });
    return out;
}

void ScriptHost::reset_node_profile() {
    impl_->profile.clear();
    impl_->forget_profile();
}

bool ScriptHost::loaded(std::string_view name) const { return impl_->modules.count(std::string(name)) != 0; }

bool ScriptHost::run(std::string_view source, std::string* error) {
    Impl& im = *impl_;
    if (!im.api_ready) {
        build_api(L_, api_);
        im.api_ready = true;
    }
    lua_CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    size_t size = 0;
    char* bytecode = luau_compile(source.data(), source.size(), &opts, &size);
    lua_State* T = lua_newthread(L_);
    luaL_sandboxthread(T);
    int status = luau_load(T, "=console", bytecode, size, 0);
    std::free(bytecode);
    if (status == 0) {
        im.begin_call();
        status = lua_resume(T, L_, 0);
        im.in_call = false;
    }
    bool good = status == LUA_OK;
    if (!good && error) *error = status == LUA_YIELD ? "нельзя ждать вне объекта" : lua_tostring(T, -1);
    lua_pop(L_, 1);
    return good;
}

void ScriptHost::send(flecs::entity_t to, std::string_view message, f64 value, flecs::entity_t from) {
    impl_->outbox.push_back({to, from, std::string(message), value});
}

namespace {

// Runs a handler (or resumes a paused one) on a pooled thread and deals with
// the outcome: done, paused in forge.wait, or failed.
struct Caller {
    ScriptHost::Impl& im;
    lua_State* L;
    std::vector<ScriptError>& errors;
    ScriptStats& stats;
    u32 max_errors;

    void report(const std::string& module, Handler h, flecs::entity_t e, const std::string& raw) {
        ScriptError err;
        err.script = module;
        err.handler = kHandlerNames[h];
        err.entity = e;
        parse_error(raw, err.line, err.message);
        auto it = im.modules.find(module);
        if (it != im.modules.end()) err.node = it->second.map.node_at(err.line);
        FORGE_WARN("script %s.%s (entity %llu): %s", module.c_str(), err.handler.c_str(),
                   static_cast<unsigned long long>(e), raw.c_str());
        if (errors.size() >= max_errors) errors.erase(errors.begin());
        errors.push_back(std::move(err));
        ++stats.errors;
        if (e != 0 && alive(im.ecs, e))
            if (Script* s = static_cast<Script*>(ecs_get_mut_id(im.ecs.c_ptr(), e, im.script_id))) s->failed = true;
    }

    void finish(lua_State* T, int status, const std::string& module, Handler h, flecs::entity_t e) {
        if (status == LUA_OK) {
            im.release(T, true);
        } else if (status == LUA_YIELD) {
            const f64 wait = im.pending_wait < 0 ? 0 : im.pending_wait;
            im.waits.push_back({T, e, wait, im.pending_real, module, h});
        } else {
            report(module, h, e, lua_isstring(T, -1) ? lua_tostring(T, -1) : "ошибка");
            im.release(T, false);
        }
        im.pending_wait = -1;
        im.pending_real = false;
    }

    int resume(lua_State* T, int nargs, const ScriptHost::Impl::Module* m, flecs::entity_t e) {
        im.current_entity = e;
        im.current_module = m;
        im.begin_call();
        const int status = lua_resume(T, L, nargs);
        im.in_call = false;
        im.current_entity = 0;
        im.current_module = nullptr;
        ++stats.calls;
        return status;
    }

    template <typename PushArgs>
    void call(const ScriptHost::Impl::Module& m, Handler h, flecs::entity_t e, PushArgs&& push) {
        for (int ref : m.fn[h]) {
            lua_State* T = im.acquire(L);
            lua_getref(T, ref);
            push_entity(T, e);
            const int nargs = 1 + push(T);
            finish(T, resume(T, nargs, &m, e), m.name, h, e);
            if (!alive(im.ecs, e)) return; // the first handler destroyed it
        }
    }
};

} // namespace

void ScriptHost::tick(const sim::TickContext& ctx) {
    FORGE_ZONE_N("Scripts");
    const u64 t0 = time_now_ns();
    Impl& im = *impl_;
    im.ctx = &ctx;
    stats_.calls = 0;
    stats_.messages = 0;
    if (ctx.rewinding) { // the past replays as it was recorded
        stats_.ms = ns_to_ms(time_now_ns() - t0);
        return;
    }
    if (!im.api_ready) {
        build_api(L_, api_);
        im.api_ready = true;
    }
    world_time_ += static_cast<f64>(ctx.dt) * ctx.time_scale;

    // Who runs this tick, with their own dt (as each_due computes it).
    struct Run {
        flecs::entity_t e;
        f32 dt;
        bool start;
        const Impl::Module* m;
    };
    std::vector<Run> runs;
    im.due.clear();
    const std::string* last_name = nullptr;
    const Impl::Module* last_module = nullptr;
    flecs::world& ecs = scene_.ecs();
    im.scripted.run([&](flecs::iter& it) {
        while (it.next()) {
            const u32 n = static_cast<u32>(it.count());
            const flecs::entity_t* ents = it.c_ptr()->entities;
            scene::Position* pos = &it.field<scene::Position>(0)[0];
            Script* scr = &it.field<Script>(1)[0];
            const auto* own = static_cast<const sim::OutsideTime*>(
                ctx.outside_time ? ecs_table_get_id(ecs.c_ptr(), it.c_ptr()->table, ctx.outside_time, it.c_ptr()->offset)
                                 : nullptr);
            for (u32 i = 0; i < n; ++i) {
                if (scr[i].failed) continue;
                const u32 due = ctx.zones.ticks_due(pos[i].chunk(), ctx.tick);
                if (due == 0) continue;
                const f32 scale = own ? own[i].scale
                                      : (ctx.time.empty() ? ctx.time_scale
                                                          : ctx.time_scale * ctx.time.at(pos[i].tile_x(), pos[i].tile_y()));
                if (scale <= 0) continue;
                const f32 dt = ctx.dt * static_cast<f32>(due) * scale;
                im.due.push_back({ents[i], dt});
                // Neighbours in a table usually run the same module.
                if (!last_name || *last_name != scr[i].name) {
                    auto m = im.modules.find(scr[i].name);
                    last_name = &scr[i].name;
                    last_module = m == im.modules.end() ? nullptr : &m->second;
                }
                if (!last_module) continue;
                runs.push_back({ents[i], dt, !scr[i].started, last_module});
                scr[i].started = true;
            }
        }
    });
    stats_.scripted = static_cast<u32>(im.due.size());

    Caller c{im, L_, errors_, stats_, desc_.max_errors};
    auto ok = [&](flecs::entity_t e) {
        if (!alive(ecs, e)) return false;
        const Script* s = static_cast<const Script*>(ecs_get_id(ecs.c_ptr(), e, im.script_id));
        return s && !s->failed;
    };
    auto module_of = [&](flecs::entity_t e) -> const Impl::Module* {
        if (!alive(ecs, e)) return nullptr;
        const Script* s = static_cast<const Script*>(ecs_get_id(ecs.c_ptr(), e, im.script_id));
        if (!s || s->failed) return nullptr;
        auto m = im.modules.find(s->name);
        return m == im.modules.end() ? nullptr : &m->second;
    };

    for (const Run& r : runs) {
        if (r.start && ok(r.e)) c.call(*r.m, OnStart, r.e, [](lua_State*) { return 0; });
        if (!r.m->fn[OnTick].empty() && ok(r.e))
            c.call(*r.m, OnTick, r.e, [&](lua_State* T) {
                lua_pushnumber(T, r.dt);
                return 1;
            });
    }

    // Triggers (who came in and went out) and rigid bodies that hit something.
    for (const sim::TriggerEvent& ev : ctx.events.triggers)
        if (const Impl::Module* m = module_of(ev.trigger))
            c.call(*m, ev.entered ? OnEnter : OnLeave, ev.trigger, [&](lua_State* T) {
                push_entity(T, ev.other);
                return 1;
            });
    for (const sim::RigidContact& ev : ctx.events.rigid) {
        if (const Impl::Module* m = module_of(ev.a))
            c.call(*m, OnHit, ev.a, [&](lua_State* T) {
                push_entity(T, ev.b);
                return 1;
            });
        if (ev.b != 0)
            if (const Impl::Module* m = module_of(ev.b))
                c.call(*m, OnHit, ev.b, [&](lua_State* T) {
                    push_entity(T, ev.a);
                    return 1;
                });
    }

    // Messages sent during the last tick, in the order they were sent.
    im.inbox.clear();
    im.inbox.swap(im.outbox);
    for (const Impl::Message& msg : im.inbox) {
        if (msg.to == 0) { // to everyone listening
            for (const auto& [e, dt] : im.due)
                if (const Impl::Module* m = module_of(e); m && !m->fn[OnMessage].empty()) {
                    ++stats_.messages;
                    c.call(*m, OnMessage, e, [&](lua_State* T) {
                        lua_pushlstring(T, msg.name.data(), msg.name.size());
                        lua_pushnumber(T, msg.value);
                        push_entity(T, msg.from);
                        return 3;
                    });
                }
            continue;
        }
        if (const Impl::Module* m = module_of(msg.to)) {
            ++stats_.messages;
            c.call(*m, OnMessage, msg.to, [&](lua_State* T) {
                lua_pushlstring(T, msg.name.data(), msg.name.size());
                lua_pushnumber(T, msg.value);
                push_entity(T, msg.from);
                return 3;
            });
        }
    }

    // Paused handlers whose time is up. Their entity's own time counts: a
    // wait in slow motion lasts longer, in stopped time or asleep it holds.
    std::vector<Impl::Wait> waiting;
    waiting.swap(im.waits);
    if (!waiting.empty()) std::sort(im.due.begin(), im.due.end());
    for (Impl::Wait& w : waiting) {
        if (w.entity != 0 && !ok(w.entity)) {
            im.release(w.thread, false);
            continue;
        }
        f64 dt;
        if (w.real) dt = ctx.dt;
        else if (w.entity == 0) dt = static_cast<f64>(ctx.dt) * ctx.time_scale;
        else {
            auto d = std::lower_bound(im.due.begin(), im.due.end(), std::pair<flecs::entity_t, f32>{w.entity, -1e30f});
            dt = d != im.due.end() && d->first == w.entity ? d->second : 0;
        }
        w.remaining -= dt;
        if (w.remaining > 0) {
            im.waits.push_back(w);
            continue;
        }
        auto m = im.modules.find(w.module);
        c.finish(w.thread, c.resume(w.thread, 0, m == im.modules.end() ? nullptr : &m->second, w.entity), w.module,
                 w.handler, w.entity);
    }
    stats_.waiting = static_cast<u32>(im.waits.size());
    stats_.memory = static_cast<usize>(lua_gc(L_, LUA_GCCOUNT, 0)) * 1024;
    im.ctx = nullptr;
    stats_.ms = ns_to_ms(time_now_ns() - t0);
}

bool check_syntax(std::string_view source, std::string* error) {
    lua_CompileOptions opts{};
    size_t size = 0;
    char* bytecode = luau_compile(source.data(), source.size(), &opts, &size);
    // A failed compile gives a 0 byte, then the message.
    const bool ok = bytecode && size > 0 && bytecode[0] != 0;
    if (!ok && error) *error = bytecode && size > 1 ? std::string(bytecode + 1, size - 1) : std::string("не компилируется");
    std::free(bytecode);
    return ok;
}

} // namespace forge::script
