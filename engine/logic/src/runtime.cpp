// Running links: the forge.logic functions and the scripts of copies.

#include "forge/logic/logic.h"

#include "forge/core/hash.h"
#include "forge/core/log.h"
#include "forge/objects/library.h"
#include "forge/scene/scene.h"
#include "forge/sim/simulation.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace forge::logic {

struct Runtime::Impl {
    static Runtime& of(lua_State* L) { return *static_cast<Runtime*>(script::ScriptHost::of(L).user("logic")); }
};

namespace {

flecs::entity_t entity_arg(lua_State* L, int i) {
    return lua_isnumber(L, i) ? static_cast<flecs::entity_t>(lua_tonumber(L, i)) : 0;
}

std::string_view string_arg(lua_State* L, int i) {
    size_t len = 0;
    const char* s = lua_tolstring(L, i, &len);
    return s ? std::string_view(s, len) : std::string_view();
}

} // namespace

// The functions compiled links call. Each finds its Runtime through the host.
struct Api {
    static Runtime& rt(lua_State* L) { return Runtime::Impl::of(L); }

    static int is_hero(lua_State* L) {
        lua_pushboolean(L, rt(L).game_.is_hero(entity_arg(L, 1)));
        return 1;
    }
    static int hero(lua_State* L) {
        const flecs::entity_t h = rt(L).game_.hero();
        if (h) lua_pushnumber(L, static_cast<double>(h));
        else lua_pushnil(L);
        return 1;
    }
    static int has(lua_State* L) {
        lua_pushboolean(L, rt(L).game_.has(entity_arg(L, 1), string_arg(L, 2)));
        return 1;
    }
    static int act(lua_State* L) {
        Runtime& r = rt(L);
        const std::string_view action = string_arg(L, 1);
        if (!r.game_.act(action, entity_arg(L, 2), string_arg(L, 3), entity_arg(L, 4), entity_arg(L, 5))) {
            static std::set<std::string> said;
            if (said.insert(std::string(action)).second)
                FORGE_WARN("Связи: игра не умеет действие «%.*s»", static_cast<int>(action.size()), action.data());
        }
        return 0;
    }
    // Where a link sends the hero: asked for now, done by the game after the tick.
    static int go(lua_State* L) {
        Runtime& r = rt(L);
        const u32 link = static_cast<u32>(lua_isnumber(L, 4) ? lua_tonumber(L, 4) : 0);
        if (!r.game_.go(entity_arg(L, 1), string_arg(L, 2), string_arg(L, 3), link)) {
            static bool said = false;
            if (!said) FORGE_WARN("Связи: игра не умеет переходить на другой уровень");
            said = true;
        }
        return 0;
    }
    static int hint(lua_State* L) {
        rt(L).game_.hint(entity_arg(L, 1), string_arg(L, 2));
        return 0;
    }
    static int sound(lua_State* L) {
        rt(L).game_.sound(entity_arg(L, 1), string_arg(L, 2));
        return 0;
    }
    static int night(lua_State* L) {
        lua_pushboolean(L, rt(L).game_.night());
        return 1;
    }
    static int fired(lua_State* L) {
        rt(L).game_.fired(static_cast<u32>(lua_tonumber(L, 1)));
        return 0;
    }
    // True the first time for this copy and link (then remembered in its
    // ScriptVars, so it lasts through saves). An area's entity is saved with
    // nothing: the hero remembers for it (link ids are the game's, one each).
    static int first(lua_State* L) {
        Runtime& r = rt(L);
        flecs::entity_t e = entity_arg(L, 1);
        if (r.is_area_entity(e)) e = r.game_.hero();
        const std::string key = "связь " + std::to_string(static_cast<u32>(lua_tonumber(L, 2)));
        flecs::world& ecs = r.host_.scene().ecs();
        if (!e || !ecs.is_alive(e)) {
            lua_pushboolean(L, false);
            return 1;
        }
        script::ScriptVars& vars = ecs.entity(e).ensure<script::ScriptVars>();
        script::ScriptVar& v = vars.get_or_add(key);
        const bool done = v.kind == script::VarKind::Bool && v.x != 0;
        v.kind = script::VarKind::Bool;
        v.x = 1;
        ecs.entity(e).modified<script::ScriptVars>();
        lua_pushboolean(L, !done);
        return 1;
    }
    // The nearest copy of a template around an entity, or nil.
    static int nearest(lua_State* L) {
        Runtime& r = rt(L);
        const objects::Template* t = r.library_.find(string_arg(L, 1));
        const flecs::entity_t from = entity_arg(L, 2);
        const f32 radius = static_cast<f32>(lua_isnumber(L, 3) ? lua_tonumber(L, 3) : 32.0);
        flecs::world& ecs = r.host_.scene().ecs();
        if (!t || !from || !ecs.is_alive(from)) {
            lua_pushnil(L);
            return 1;
        }
        const scene::Position* p = ecs.entity(from).try_get<scene::Position>();
        if (!p) {
            lua_pushnil(L);
            return 1;
        }
        std::vector<flecs::entity_t> found;
        r.host_.scene().query_radius(p->tile_x(), p->tile_y(), radius, found);
        flecs::entity_t best = 0;
        f64 best_d = 1e300;
        for (flecs::entity_t id : found) {
            if (id == from) continue;
            const flecs::entity e = ecs.entity(id);
            const objects::ObjectRef* ref = e.try_get<objects::ObjectRef>();
            const scene::Position* q = e.try_get<scene::Position>();
            if (!ref || !q || ref->key != t->key) continue;
            const f64 d = std::hypot(q->tile_x() - p->tile_x(), q->tile_y() - p->tile_y());
            if (d < best_d) best_d = d, best = id;
        }
        if (best) lua_pushnumber(L, static_cast<double>(best));
        else lua_pushnil(L);
        return 1;
    }
};

Runtime::Runtime(script::ScriptHost& host, const objects::Library& library, Game& game)
    : host_(host), library_(library), game_(game) {
    host_.set_user("logic", this);
    if (host_.api_built()) FORGE_ERROR("Связи: Runtime создан после первого запуска скриптов, forge.logic им не виден");
    script::ScriptApi& api = host_.api();
    auto add = [&](const char* name, script::CFunction fn) { api.add(name, fn).hidden().category("Связи"); };
    add("logic.is_hero", &Api::is_hero);
    add("logic.hero", &Api::hero);
    add("logic.has", &Api::has);
    add("logic.act", &Api::act);
    add("logic.go", &Api::go);
    add("logic.hint", &Api::hint);
    add("logic.sound", &Api::sound);
    add("logic.night", &Api::night);
    add("logic.fired", &Api::fired);
    add("logic.first", &Api::first);
    add("logic.nearest", &Api::nearest);
}

Runtime::~Runtime() {
    if (observer_) observer_.destruct();
    if (host_.user("logic") == this) host_.set_user("logic", nullptr);
}

bool Runtime::load(const Logic& logic, const Verbs& verbs, std::vector<Problem>* problems) {
    std::vector<Thing> things;
    things.reserve(library_.templates().size() + 1 + areas_.size() + other_areas_.size() + levels_.size());
    things.push_back(hero_thing());
    for (const objects::Template& t : library_.templates()) things.push_back(thing_of(library_, t));
    things.insert(things.end(), areas_.begin(), areas_.end());
    things.insert(things.end(), other_areas_.begin(), other_areas_.end());
    things.insert(things.end(), levels_.begin(), levels_.end());
    const FindThing find = [&](std::string_view id) -> const Thing* {
        for (const Thing& t : things)
            if (t.id == id) return &t;
        return nullptr;
    };
    if (!nodes_) nodes_ = std::make_unique<script::NodeLibrary>(node_library(host_.api()));
    compiled_ = compile(logic, verbs, find, nodes_.get());
    ids_.clear();
    for (const Link& l : logic.links) ids_.push_back(l.id);
    for (const ThingScheme& t : logic.schemes) ids_.push_back(t.id);
    if (problems) *problems = compiled_.problems;
    for (const Problem& p : compiled_.problems)
        if (p.warning) FORGE_WARN("Связь %u: %s", p.link, p.text.c_str());
        else FORGE_WARN("Связь %u не работает: %s", p.link, p.text.c_str());

    bool ok = true;
    listening_.clear();
    touch_.clear();
    for (const Module& m : compiled_.modules) {
        if (!host_.load(m.name, m.source, &m.map)) ok = false;
        if (const objects::Template* t = library_.find(m.thing)) {
            listening_[t->key] = m.name;
            touch_[t->key] = m.touch;
        }
    }
    // Copies already in the scene follow the new links, and so do areas.
    if (scene_) scene_->ecs().each([&](flecs::entity e, const objects::ObjectRef&) { attach_one(e); });
    sync_areas();
    return ok;
}

void Runtime::set_areas(std::vector<Thing> areas) { areas_ = std::move(areas); }
void Runtime::set_other_areas(std::vector<Thing> areas) { other_areas_ = std::move(areas); }
void Runtime::set_levels(std::vector<Thing> levels) { levels_ = std::move(levels); }

void Runtime::sync_areas() {
    if (!scene_) return;
    flecs::world& ecs = scene_->ecs();
    std::unordered_map<std::string, flecs::entity_t> keep;
    for (const Module& m : compiled_.modules) {
        if (!is_area(m.thing)) continue;
        if (std::none_of(areas_.begin(), areas_.end(), [&](const Thing& t) { return t.id == m.thing; })) continue;
        flecs::entity_t id = 0;
        if (const auto it = area_entities_.find(m.thing); it != area_entities_.end() && ecs.is_alive(it->second)) id = it->second;
        flecs::entity e = id ? ecs.entity(id) : ecs.entity();
        const script::Script* has = e.try_get<script::Script>();
        if (!has || has->name != m.name) e.set<script::Script>({m.name, false, false});
        keep[m.thing] = e.id();
    }
    for (const auto& [area, id] : area_entities_)
        if (!keep.contains(area) && ecs.is_alive(id)) ecs.entity(id).destruct();
    area_entities_ = std::move(keep);
}

void Runtime::area_event(std::string_view area, flecs::entity_t hero, bool entered) {
    const flecs::entity_t e = area_entity(area);
    if (e) host_.enter(e, hero, entered);
}

flecs::entity_t Runtime::area_entity(std::string_view area) const {
    const auto it = area_entities_.find(std::string(area));
    return it == area_entities_.end() ? 0 : it->second;
}

bool Runtime::is_area_entity(flecs::entity_t e) const {
    if (!e) return false;
    for (const auto& [area, id] : area_entities_)
        if (id == e) return true;
    return false;
}

void Runtime::attach(scene::Scene& scene) {
    if (scene_ != &scene) area_entities_.clear();
    scene_ = &scene;
    flecs::world& ecs = scene.ecs();
    if (observer_) observer_.destruct();
    observer_ = ecs.observer<objects::ObjectRef>().event(flecs::OnSet).each([this](flecs::entity e, objects::ObjectRef&) {
        attach_one(e);
    });
    ecs.each([&](flecs::entity e, const objects::ObjectRef&) { attach_one(e); });
    sync_areas();
}

void Runtime::attach_one(flecs::entity e) {
    const objects::ObjectRef& ref = e.get<objects::ObjectRef>();
    const script::Script* has = e.try_get<script::Script>();
    const bool ours = has && has->name.starts_with("logic:");
    const auto it = listening_.find(ref.key);
    if (it == listening_.end()) {
        if (ours) {
            e.remove<script::Script>();
            e.remove<sim::Trigger>();
        }
        return;
    }
    if (!has || (ours && has->name != it->second)) e.set<script::Script>({it->second, false, false});
    if (has && !ours) return; // its own script: links do not replace it
    if (touch_[ref.key]) {
        if (!e.has<sim::Trigger>()) e.set<sim::Trigger>({kTouchRadius});
    } else if (ours) {
        e.remove<sim::Trigger>();
    }
}

i32 Runtime::link_at(std::string_view module, i32 line) const {
    for (const Module& m : compiled_.modules)
        if (m.name == module) {
            const u32 n = m.map.node_at(line);
            return n == script::SourceMap::kNoNode ? -1 : static_cast<i32>(n);
        }
    return -1;
}

} // namespace forge::logic
