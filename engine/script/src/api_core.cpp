// The engine's own functions for scripts. Each is described for the node
// palette where it is registered (register_core_api), so these descriptions
// are also the nodes of the "Объекты", "Время", "Переменные"… groups.

#include "host_impl.h"

#include "forge/core/log.h"
#include "forge/core/time.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace forge::script {

// --- ValueType, ScriptApi ----------------------------------------------------

namespace {
const char* const kTypeNames[] = {"any", "bool", "number", "integer", "string", "vec2",
                                  "color", "entity", "asset", "enum", "table"};
}

const char* value_type_name(ValueType type) { return kTypeNames[static_cast<u8>(type)]; }

bool parse_value_type(std::string_view name, ValueType& out) {
    for (u8 i = 0; i < std::size(kTypeNames); ++i)
        if (name == kTypeNames[i]) {
            out = static_cast<ValueType>(i);
            return true;
        }
    return false;
}

ApiBuilder& ApiBuilder::param(const char* id, ValueType type, const char* ru, const char* en, const char* default_lua) {
    ApiParam p;
    p.id = id;
    p.type = type;
    p.title_ru = ru;
    p.title_en = en;
    p.default_lua = default_lua;
    f_.params.push_back(std::move(p));
    return *this;
}

ApiBuilder& ApiBuilder::range(f64 min, f64 max) {
    if (!f_.params.empty()) {
        f_.params.back().has_range = true;
        f_.params.back().min = min;
        f_.params.back().max = max;
    }
    return *this;
}

ApiBuilder& ApiBuilder::enum_type(const char* name) {
    if (!f_.params.empty()) f_.params.back().enum_type = name;
    return *this;
}

ApiBuilder& ApiBuilder::result(const char* id, ValueType type, const char* ru, const char* en) {
    ApiParam p;
    p.id = id;
    p.type = type;
    p.title_ru = ru;
    p.title_en = en;
    f_.results.push_back(std::move(p));
    return *this;
}

ApiBuilder ScriptApi::add(std::string_view name, CFunction fn) {
    for (ApiFunction& f : functions_)
        if (f.name == name) {
            f = {};
            f.name = std::string(name);
            f.fn = fn;
            return ApiBuilder(f);
        }
    functions_.push_back({});
    functions_.back().name = std::string(name);
    functions_.back().fn = fn;
    return ApiBuilder(functions_.back());
}

const ApiFunction* ScriptApi::find(std::string_view name) const {
    for (const ApiFunction& f : functions_)
        if (f.name == name) return &f;
    return nullptr;
}

// --- reflected fields <-> Luau ---------------------------------------------

void push_field(lua_State* L, const reflect::TypeInfo* type, const void* at) {
    using reflect::Kind;
    switch (type->kind) {
    case Kind::Bool: lua_pushboolean(L, *static_cast<const bool*>(at)); return;
    case Kind::I8: lua_pushnumber(L, *static_cast<const i8*>(at)); return;
    case Kind::U8: lua_pushnumber(L, *static_cast<const u8*>(at)); return;
    case Kind::I16: lua_pushnumber(L, *static_cast<const i16*>(at)); return;
    case Kind::U16: lua_pushnumber(L, *static_cast<const u16*>(at)); return;
    case Kind::I32: lua_pushnumber(L, *static_cast<const i32*>(at)); return;
    case Kind::U32: lua_pushnumber(L, *static_cast<const u32*>(at)); return;
    case Kind::I64: lua_pushnumber(L, static_cast<double>(*static_cast<const i64*>(at))); return;
    case Kind::U64: lua_pushnumber(L, static_cast<double>(*static_cast<const u64*>(at))); return;
    case Kind::F32: lua_pushnumber(L, *static_cast<const f32*>(at)); return;
    case Kind::F64: lua_pushnumber(L, *static_cast<const f64*>(at)); return;
    case Kind::String: {
        const std::string& s = *static_cast<const std::string*>(at);
        lua_pushlstring(L, s.data(), s.size());
        return;
    }
    case Kind::Vec2: {
        const Vec2& v = *static_cast<const Vec2*>(at);
        lua_pushvector(L, v.x, v.y, 0);
        return;
    }
    case Kind::Color: {
        const Color& c = *static_cast<const Color*>(at);
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, c.r), lua_setfield(L, -2, "r");
        lua_pushnumber(L, c.g), lua_setfield(L, -2, "g");
        lua_pushnumber(L, c.b), lua_setfield(L, -2, "b");
        lua_pushnumber(L, c.a), lua_setfield(L, -2, "a");
        return;
    }
    case Kind::Enum: {
        i64 v = 0;
        if (type->size == 1) v = type->enum_signed ? *static_cast<const i8*>(at) : *static_cast<const u8*>(at);
        else if (type->size == 2) v = type->enum_signed ? *static_cast<const i16*>(at) : *static_cast<const u16*>(at);
        else if (type->size == 4) v = type->enum_signed ? *static_cast<const i32*>(at) : *static_cast<const u32*>(at);
        else v = *static_cast<const i64*>(at);
        if (const reflect::EnumValue* e = type->find_enum(v)) lua_pushstring(L, e->name.c_str());
        else lua_pushnumber(L, static_cast<double>(v));
        return;
    }
    default: lua_pushnil(L); return;
    }
}

bool read_field(lua_State* L, int idx, const reflect::TypeInfo* type, void* at) {
    using reflect::Kind;
    auto num = [&](double& out) {
        if (lua_isboolean(L, idx)) out = lua_toboolean(L, idx);
        else if (lua_isnumber(L, idx)) out = lua_tonumber(L, idx);
        else return false;
        return true;
    };
    double n = 0;
    switch (type->kind) {
    case Kind::Bool: *static_cast<bool*>(at) = lua_toboolean(L, idx) != 0; return true;
    case Kind::I8: if (!num(n)) return false; *static_cast<i8*>(at) = static_cast<i8>(n); return true;
    case Kind::U8: if (!num(n)) return false; *static_cast<u8*>(at) = static_cast<u8>(n); return true;
    case Kind::I16: if (!num(n)) return false; *static_cast<i16*>(at) = static_cast<i16>(n); return true;
    case Kind::U16: if (!num(n)) return false; *static_cast<u16*>(at) = static_cast<u16>(n); return true;
    case Kind::I32: if (!num(n)) return false; *static_cast<i32*>(at) = static_cast<i32>(n); return true;
    case Kind::U32: if (!num(n)) return false; *static_cast<u32*>(at) = static_cast<u32>(n); return true;
    case Kind::I64: if (!num(n)) return false; *static_cast<i64*>(at) = static_cast<i64>(n); return true;
    case Kind::U64: if (!num(n)) return false; *static_cast<u64*>(at) = static_cast<u64>(n); return true;
    case Kind::F32: if (!num(n)) return false; *static_cast<f32*>(at) = static_cast<f32>(n); return true;
    case Kind::F64: if (!num(n)) return false; *static_cast<f64*>(at) = n; return true;
    case Kind::String: {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        if (!s) return false;
        static_cast<std::string*>(at)->assign(s, len);
        return true;
    }
    case Kind::Vec2: {
        const float* v = lua_tovector(L, idx);
        if (!v) return false;
        *static_cast<Vec2*>(at) = {v[0], v[1]};
        return true;
    }
    case Kind::Color: {
        if (!lua_istable(L, idx)) return false;
        Color& c = *static_cast<Color*>(at);
        const char* names[4] = {"r", "g", "b", "a"};
        f32* parts[4] = {&c.r, &c.g, &c.b, &c.a};
        for (int i = 0; i < 4; ++i) {
            lua_getfield(L, idx, names[i]);
            if (lua_isnumber(L, -1)) *parts[i] = static_cast<f32>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        return true;
    }
    case Kind::Enum: {
        i64 v = 0;
        if (lua_type(L, idx) == LUA_TSTRING) {
            const reflect::EnumValue* e = type->find_enum(std::string_view(lua_tostring(L, idx)));
            if (!e) return false;
            v = e->value;
        } else if (lua_isnumber(L, idx)) {
            v = static_cast<i64>(lua_tonumber(L, idx));
        } else {
            return false;
        }
        if (type->size == 1) *static_cast<u8*>(at) = static_cast<u8>(v);
        else if (type->size == 2) *static_cast<u16*>(at) = static_cast<u16>(v);
        else if (type->size == 4) *static_cast<u32*>(at) = static_cast<u32>(v);
        else *static_cast<i64*>(at) = v;
        return true;
    }
    default: return false;
    }
}

namespace {

using Impl = ScriptHost::Impl;

bool alive(Impl& im, flecs::entity_t e) {
    return e != 0 && ecs_is_alive(im.ecs.c_ptr(), e);
}

// --- general ----------------------------------------------------------------

int api_log(lua_State* L) {
    std::string text;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) text += ' ';
        text.append(s, len);
        lua_pop(L, 1);
    }
    Impl& im = *impl_of(L);
    FORGE_INFO("[%s] %s", im.current_module ? im.current_module->name.c_str() : "script", text.c_str());
    return 0;
}

int wait_common(lua_State* L, bool real) {
    const double seconds = luaL_optnumber(L, 1, 0);
    if (!lua_isyieldable(L)) luaL_error(L, "ждать можно только внутри обработчика объекта");
    Impl& im = *impl_of(L);
    im.pending_wait = seconds;
    im.pending_real = real;
    return lua_yield(L, 0);
}
int api_wait(lua_State* L) { return wait_common(L, false); }
int api_wait_real(lua_State* L) { return wait_common(L, true); }

// The "Compare" node: one function for every operator, so the editor can
// switch it without rewiring. Unicode signs are what the editor shows.
int api_compare(lua_State* L) {
    const std::string_view op = luaL_checkstring(L, 2);
    bool r = false;
    if (op == "=" || op == "==") r = lua_equal(L, 1, 3);
    else if (op == "~=" || op == "!=" || op == "\xE2\x89\xA0") r = !lua_equal(L, 1, 3);
    else if (op == "<") r = lua_lessthan(L, 1, 3);
    else if (op == "<=" || op == "\xE2\x89\xA4") r = lua_lessthan(L, 1, 3) || lua_equal(L, 1, 3);
    else if (op == ">") r = lua_lessthan(L, 3, 1);
    else if (op == ">=" || op == "\xE2\x89\xA5") r = lua_lessthan(L, 3, 1) || lua_equal(L, 1, 3);
    else luaL_error(L, "unknown comparison \"%s\"", op.data());
    lua_pushboolean(L, r);
    return 1;
}

int api_now(lua_State* L) {
    lua_pushnumber(L, impl_of(L)->host.world_time());
    return 1;
}

u64 next_random(Impl& im) {
    u64 x = im.rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    im.rng = x;
    return x;
}

int api_random(lua_State* L) {
    const double v = static_cast<double>(next_random(*impl_of(L)) >> 11) * (1.0 / 9007199254740992.0);
    const double a = luaL_optnumber(L, 1, 0), b = luaL_optnumber(L, 2, 1);
    lua_pushnumber(L, a + (b - a) * v);
    return 1;
}

int api_send(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t to = check_entity(L, 1);
    size_t len = 0;
    const char* name = luaL_checklstring(L, 2, &len);
    im.host.send(to, std::string_view(name, len), luaL_optnumber(L, 3, 0), im.current_entity);
    return 0;
}

// --- entities ---------------------------------------------------------------

const scene::Position* position_of(Impl& im, flecs::entity_t e) {
    if (!alive(im, e)) return nullptr;
    return im.ecs.entity(e).try_get<scene::Position>();
}

int api_alive(lua_State* L) {
    lua_pushboolean(L, alive(*impl_of(L), static_cast<flecs::entity_t>(luaL_optnumber(L, 1, 0))));
    return 1;
}

int api_destroy(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    if (alive(im, e)) ecs_delete(im.ecs.c_ptr(), e);
    return 0;
}

int api_position(lua_State* L) {
    const scene::Position* p = position_of(*impl_of(L), check_entity(L, 1));
    if (!p) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    lua_pushnumber(L, p->tile_x());
    lua_pushnumber(L, p->tile_y());
    return 2;
}

int api_set_position(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    if (!alive(im, e)) return 0;
    if (scene::Position* p = im.ecs.entity(e).try_get_mut<scene::Position>())
        *p = scene::Position::at_tile(luaL_checknumber(L, 2), luaL_checknumber(L, 3));
    return 0;
}

int api_move(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    if (!alive(im, e)) return 0;
    if (scene::Position* p = im.ecs.entity(e).try_get_mut<scene::Position>())
        *p = scene::Position::at_tile(p->tile_x() + luaL_checknumber(L, 2), p->tile_y() + luaL_checknumber(L, 3));
    return 0;
}

int api_distance(lua_State* L) {
    Impl& im = *impl_of(L);
    const scene::Position* a = position_of(im, check_entity(L, 1));
    const scene::Position* b = position_of(im, check_entity(L, 2));
    if (!a || !b) {
        lua_pushnumber(L, HUGE_VAL);
        return 1;
    }
    lua_pushnumber(L, std::hypot(a->tile_x() - b->tile_x(), a->tile_y() - b->tile_y()));
    return 1;
}

int api_velocity(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    const sim::Body* b = alive(im, e) ? static_cast<const sim::Body*>(ecs_get_id(im.ecs.c_ptr(), e, im.body_id)) : nullptr;
    lua_pushnumber(L, b ? b->vx : 0);
    lua_pushnumber(L, b ? b->vy : 0);
    return 2;
}

int api_set_velocity(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    if (!alive(im, e)) return 0;
    if (auto* b = static_cast<sim::Body*>(ecs_get_mut_id(im.ecs.c_ptr(), e, im.body_id))) {
        b->vx = static_cast<f32>(luaL_checknumber(L, 2));
        b->vy = static_cast<f32>(luaL_checknumber(L, 3));
    }
    return 0;
}

int api_spawn(lua_State* L) {
    Impl& im = *impl_of(L);
    flecs::entity e = im.scene.spawn(scene::Position::at_tile(luaL_checknumber(L, 1), luaL_checknumber(L, 2)));
    if (!e.is_valid()) {
        lua_pushnumber(L, 0);
        return 1;
    }
    size_t len = 0;
    const char* script = luaL_optlstring(L, 3, "", &len);
    if (len > 0) e.set<Script>({std::string(script, len)});
    push_entity(L, e.id());
    return 1;
}

// Entities within a radius of a point, nearest first (ties by id), with a
// component if one is named.
void find_near(lua_State* L, Impl& im, std::vector<std::pair<double, flecs::entity_t>>& out) {
    const double x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2);
    const float r = static_cast<float>(luaL_checknumber(L, 3));
    const char* comp = luaL_optstring(L, 4, "");
    flecs::entity_t need = 0;
    if (comp[0] != 0) {
        Impl::Exposed* ex = im.find_exposed(comp);
        if (!ex) luaL_error(L, "компонент «%s» не открыт для скриптов", comp);
        need = ex->id;
    }
    std::vector<flecs::entity_t> found;
    im.scene.query_radius(x, y, r, found);
    out.clear();
    for (flecs::entity_t e : found) {
        if (e == im.current_entity) continue;
        if (need && !ecs_has_id(im.ecs.c_ptr(), e, need)) continue;
        const scene::Position* p = position_of(im, e);
        if (!p) continue;
        out.push_back({std::hypot(p->tile_x() - x, p->tile_y() - y), e});
    }
    std::sort(out.begin(), out.end());
}

int api_find(lua_State* L) {
    Impl& im = *impl_of(L);
    std::vector<std::pair<double, flecs::entity_t>> near;
    find_near(L, im, near);
    lua_createtable(L, static_cast<int>(near.size()), 0);
    for (usize i = 0; i < near.size(); ++i) {
        push_entity(L, near[i].second);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int api_nearest(lua_State* L) {
    Impl& im = *impl_of(L);
    std::vector<std::pair<double, flecs::entity_t>> near;
    find_near(L, im, near);
    push_entity(L, near.empty() ? 0 : near.front().second);
    return 1;
}

// --- components through reflection ----------------------------------------------

Impl::Exposed& check_component(lua_State* L, Impl& im, int idx) {
    const char* name = luaL_checkstring(L, idx);
    Impl::Exposed* ex = im.find_exposed(name);
    if (!ex) luaL_error(L, "компонент «%s» не открыт для скриптов", name);
    return *ex;
}

const reflect::FieldInfo& check_field(lua_State* L, const Impl::Exposed& ex, int idx) {
    const char* name = luaL_checkstring(L, idx);
    const reflect::FieldInfo* f = ex.type->find_field(name);
    if (!f) luaL_error(L, "у компонента «%s» нет поля «%s»", ex.type->name.c_str(), name);
    return *f;
}

int api_get(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    Impl::Exposed& ex = check_component(L, im, 2);
    const reflect::FieldInfo& f = check_field(L, ex, 3);
    const void* data = alive(im, e) ? ecs_get_id(im.ecs.c_ptr(), e, ex.id) : nullptr;
    if (!data) {
        lua_pushnil(L);
        return 1;
    }
    push_field(L, f.type, static_cast<const u8*>(data) + f.offset);
    return 1;
}

int api_set(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    Impl::Exposed& ex = check_component(L, im, 2);
    const reflect::FieldInfo& f = check_field(L, ex, 3);
    luaL_checkany(L, 4);
    if (!alive(im, e)) return 0;
    void* data = ecs_ensure_id(im.ecs.c_ptr(), e, ex.id, ex.type->size);
    if (!read_field(L, 4, f.type, static_cast<u8*>(data) + f.offset))
        luaL_error(L, "поле «%s» нельзя задать значением типа %s", f.name.c_str(), luaL_typename(L, 4));
    ecs_modified_id(im.ecs.c_ptr(), e, ex.id);
    return 0;
}

int api_has(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    Impl::Exposed& ex = check_component(L, im, 2);
    lua_pushboolean(L, alive(im, e) && ecs_has_id(im.ecs.c_ptr(), e, ex.id));
    return 1;
}

int api_add(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    Impl::Exposed& ex = check_component(L, im, 2);
    if (alive(im, e)) ecs_ensure_id(im.ecs.c_ptr(), e, ex.id, ex.type->size);
    return 0;
}

int api_remove(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    Impl::Exposed& ex = check_component(L, im, 2);
    if (alive(im, e)) ecs_remove_id(im.ecs.c_ptr(), e, ex.id);
    return 0;
}

// --- variables --------------------------------------------------------------------

int api_var(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const auto* vars = alive(im, e) ? static_cast<const ScriptVars*>(ecs_get_id(im.ecs.c_ptr(), e, im.vars_id)) : nullptr;
    const ScriptVar* v = vars ? vars->find(name) : nullptr;
    if (!v || v->kind == VarKind::None) {
        if (lua_gettop(L) >= 3) lua_pushvalue(L, 3); // the default
        else lua_pushnil(L);
        return 1;
    }
    switch (v->kind) {
    case VarKind::Bool: lua_pushboolean(L, v->x != 0); break;
    case VarKind::Number: lua_pushnumber(L, v->x); break;
    case VarKind::String: lua_pushlstring(L, v->text.data(), v->text.size()); break;
    case VarKind::Vec2: lua_pushvector(L, static_cast<float>(v->x), static_cast<float>(v->y), 0); break;
    default: lua_pushnil(L); break;
    }
    return 1;
}

int api_set_var(lua_State* L) {
    Impl& im = *impl_of(L);
    const flecs::entity_t e = check_entity(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (!alive(im, e)) return 0;
    auto* vars = static_cast<ScriptVars*>(ecs_ensure_id(im.ecs.c_ptr(), e, im.vars_id, sizeof(ScriptVars)));
    ScriptVar& v = vars->get_or_add(name);
    v.text.clear();
    v.x = v.y = 0;
    switch (lua_type(L, 3)) {
    case LUA_TBOOLEAN: v.kind = VarKind::Bool; v.x = lua_toboolean(L, 3); break;
    case LUA_TNUMBER: v.kind = VarKind::Number; v.x = lua_tonumber(L, 3); break;
    case LUA_TSTRING: v.kind = VarKind::String; v.text = lua_tostring(L, 3); break;
    case LUA_TVECTOR: {
        const float* p = lua_tovector(L, 3);
        v.kind = VarKind::Vec2;
        v.x = p[0];
        v.y = p[1];
        break;
    }
    case LUA_TNIL: v.kind = VarKind::None; break;
    default: luaL_error(L, "переменная «%s» не может хранить %s", name, luaL_typename(L, 3));
    }
    // No ecs_modified_id: nothing observes variables, and this is the
    // hottest call of compiled graphs.
    return 0;
}

// --- world and time ---------------------------------------------------------------

int api_tile(lua_State* L) {
    Impl& im = *impl_of(L);
    lua_pushnumber(L, im.scene.world().tile(static_cast<u32>(luaL_checkinteger(L, 1)), luaL_checkinteger(L, 2),
                                            luaL_checkinteger(L, 3)));
    return 1;
}

int api_set_tile(lua_State* L) {
    Impl& im = *impl_of(L);
    im.scene.world().set_tile(static_cast<u32>(luaL_checkinteger(L, 1)), luaL_checkinteger(L, 2), luaL_checkinteger(L, 3),
                              static_cast<world::TileId>(luaL_checkinteger(L, 4)));
    return 0;
}

int api_time_scale(lua_State* L) {
    lua_pushnumber(L, impl_of(L)->sim.time_scale());
    return 1;
}

int api_set_time_scale(lua_State* L) {
    impl_of(L)->sim.set_time_scale(static_cast<f32>(luaL_checknumber(L, 1)));
    return 0;
}

// Node profiler: compiled graphs (with profiling on) wrap each node in these.
int api_prof_enter(lua_State* L) {
    Impl& im = *impl_of(L);
    if (!im.current_module) return 0;
    Impl::Timing& t = im.profile[im.current_module->name][static_cast<u32>(luaL_checkinteger(L, 1))];
    t.started.push_back(time_now_ns());
    return 0;
}

int api_prof_leave(lua_State* L) {
    Impl& im = *impl_of(L);
    if (!im.current_module) return 0;
    Impl::Timing& t = im.profile[im.current_module->name][static_cast<u32>(luaL_checkinteger(L, 1))];
    if (t.started.empty()) return 0;
    t.ns += time_now_ns() - t.started.back();
    t.started.pop_back();
    ++t.calls;
    return 0;
}

} // namespace

void register_core_api(ScriptApi& api) {
    using V = ValueType;
    // General
    api.add("log", &api_log).action().category("Отладка").icon("description").title("Записать в журнал", "Log")
        .param("text", V::Any, "Текст", "Text", "\"\"");
    api.add("wait", &api_wait).latent().category("Поток").icon("hourglass_top").title("Ждать", "Wait")
        .help("Пауза по времени объекта: замедление и остановка времени её растягивают.")
        .param("seconds", V::Number, "Секунды", "Seconds", "1").range(0, 3600);
    api.add("wait_real", &api_wait_real).latent().category("Поток").icon("timer").title("Ждать (реальное время)", "Wait (real time)")
        .param("seconds", V::Number, "Секунды", "Seconds", "1").range(0, 3600);
    api.add("now", &api_now).pure().category("Время").icon("schedule").title("Время мира", "World time")
        .result("seconds", V::Number, "Секунды", "Seconds");
    api.add("random", &api_random).pure().category("Математика").icon("casino").title("Случайное число", "Random")
        .param("min", V::Number, "От", "Min", "0").param("max", V::Number, "До", "Max", "1")
        .result("value", V::Number, "Число", "Value");
    api.add("send", &api_send).action().category("События").icon("send").title("Отправить сообщение", "Send message")
        .param("target", V::Entity, "Кому", "Target").param("message", V::String, "Сообщение", "Message")
        .param("value", V::Number, "Значение", "Value", "0");

    api.add("compare", &api_compare).hidden().pure().category("Логика").title("Сравнить", "Compare")
        .param("a", V::Any, "A", "A").param("op", V::String, "Как", "Op").param("b", V::Any, "B", "B")
        .result("result", V::Bool, "Да", "Result");
    api.add("prof.enter", &api_prof_enter).hidden().action().category("Отладка").title("Начало замера", "Profile enter")
        .param("node", V::Integer, "Нода", "Node");
    api.add("prof.leave", &api_prof_leave).hidden().action().category("Отладка").title("Конец замера", "Profile leave")
        .param("node", V::Integer, "Нода", "Node");

    // Entities
    api.add("entity.alive", &api_alive).pure().category("Объекты").icon("check_circle").title("Объект существует", "Is alive")
        .param("actor", V::Entity, "Объект", "Actor").result("alive", V::Bool, "Да", "Alive");
    api.add("entity.destroy", &api_destroy).action().category("Объекты").icon("delete").title("Уничтожить", "Destroy")
        .param("actor", V::Entity, "Объект", "Actor");
    api.add("entity.position", &api_position).pure().category("Объекты").icon("location_on").title("Позиция", "Position")
        .param("actor", V::Entity, "Объект", "Actor").result("x", V::Number, "X", "X").result("y", V::Number, "Y", "Y");
    api.add("entity.set_position", &api_set_position).action().category("Объекты").icon("pin_drop").title("Переместить в", "Set position")
        .param("actor", V::Entity, "Объект", "Actor").param("x", V::Number, "X", "X").param("y", V::Number, "Y", "Y");
    api.add("entity.move", &api_move).action().category("Объекты").icon("open_with").title("Сдвинуть", "Move by")
        .param("actor", V::Entity, "Объект", "Actor").param("dx", V::Number, "По X", "dX", "0").param("dy", V::Number, "По Y", "dY", "0");
    api.add("entity.distance", &api_distance).pure().category("Объекты").icon("straighten").title("Расстояние", "Distance")
        .param("a", V::Entity, "От", "From").param("b", V::Entity, "До", "To").result("tiles", V::Number, "Клеток", "Tiles");
    api.add("entity.velocity", &api_velocity).pure().category("Движение").icon("speed").title("Скорость", "Velocity")
        .param("actor", V::Entity, "Объект", "Actor").result("vx", V::Number, "По X", "vX").result("vy", V::Number, "По Y", "vY");
    api.add("entity.set_velocity", &api_set_velocity).action().category("Движение").icon("speed").title("Задать скорость", "Set velocity")
        .param("actor", V::Entity, "Объект", "Actor").param("vx", V::Number, "По X", "vX", "0").param("vy", V::Number, "По Y", "vY", "0");
    api.add("entity.spawn", &api_spawn).action().category("Объекты").icon("add_circle").title("Создать объект", "Spawn")
        .param("x", V::Number, "X", "X").param("y", V::Number, "Y", "Y").param("script", V::String, "Скрипт", "Script", "\"\"")
        .result("actor", V::Entity, "Объект", "Actor");
    api.add("entity.find", &api_find).pure().category("Объекты").icon("radar").title("Объекты в радиусе", "Find in radius")
        .param("x", V::Number, "X", "X").param("y", V::Number, "Y", "Y").param("radius", V::Number, "Радиус", "Radius", "8")
        .param("component", V::String, "С компонентом", "With component", "\"\"")
        .result("list", V::Table, "Список", "List");
    api.add("entity.nearest", &api_nearest).pure().category("Объекты").icon("my_location").title("Ближайший объект", "Nearest")
        .param("x", V::Number, "X", "X").param("y", V::Number, "Y", "Y").param("radius", V::Number, "Радиус", "Radius", "8")
        .param("component", V::String, "С компонентом", "With component", "\"\"")
        .result("actor", V::Entity, "Объект", "Actor");

    // Components (the node library also makes one node per reflected field)
    api.add("get", &api_get).pure().category("Компоненты").icon("input").title("Взять поле", "Get field")
        .param("actor", V::Entity, "Объект", "Actor").param("component", V::String, "Компонент", "Component")
        .param("field", V::String, "Поле", "Field").result("value", V::Any, "Значение", "Value");
    api.add("set", &api_set).action().category("Компоненты").icon("edit").title("Задать поле", "Set field")
        .param("actor", V::Entity, "Объект", "Actor").param("component", V::String, "Компонент", "Component")
        .param("field", V::String, "Поле", "Field").param("value", V::Any, "Значение", "Value");
    api.add("has", &api_has).pure().category("Компоненты").icon("rule").title("Есть компонент", "Has component")
        .param("actor", V::Entity, "Объект", "Actor").param("component", V::String, "Компонент", "Component")
        .result("has", V::Bool, "Есть", "Has");
    api.add("add", &api_add).action().category("Компоненты").icon("add_box").title("Добавить компонент", "Add component")
        .param("actor", V::Entity, "Объект", "Actor").param("component", V::String, "Компонент", "Component");
    api.add("remove", &api_remove).action().category("Компоненты").icon("indeterminate_check_box").title("Убрать компонент", "Remove component")
        .param("actor", V::Entity, "Объект", "Actor").param("component", V::String, "Компонент", "Component");

    // Variables
    api.add("var", &api_var).pure().category("Переменные").icon("database").title("Переменная", "Variable")
        .param("actor", V::Entity, "Объект", "Actor").param("name", V::String, "Имя", "Name")
        .param("default", V::Any, "Если нет", "Default", "nil").result("value", V::Any, "Значение", "Value");
    api.add("set_var", &api_set_var).action().category("Переменные").icon("edit").title("Задать переменную", "Set variable")
        .param("actor", V::Entity, "Объект", "Actor").param("name", V::String, "Имя", "Name").param("value", V::Any, "Значение", "Value");

    // World and time
    api.add("world.tile", &api_tile).pure().category("Мир").icon("grid_on").title("Тайл", "Tile")
        .param("layer", V::Integer, "Слой", "Layer", "1").param("x", V::Integer, "X", "X").param("y", V::Integer, "Y", "Y")
        .result("tile", V::Integer, "Тайл", "Tile");
    api.add("world.set_tile", &api_set_tile).action().category("Мир").icon("format_paint").title("Поставить тайл", "Set tile")
        .param("layer", V::Integer, "Слой", "Layer", "1").param("x", V::Integer, "X", "X").param("y", V::Integer, "Y", "Y")
        .param("tile", V::Integer, "Тайл", "Tile");
    api.add("time.scale", &api_time_scale).pure().category("Время").icon("speed").title("Скорость времени", "Time speed")
        .result("scale", V::Number, "Скорость", "Scale");
    api.add("time.set_scale", &api_set_time_scale).action().category("Время").icon("slow_motion_video")
        .title("Задать скорость времени", "Set time speed")
        .param("scale", V::Number, "Скорость", "Scale", "1").range(0, 10);
}

} // namespace forge::script
