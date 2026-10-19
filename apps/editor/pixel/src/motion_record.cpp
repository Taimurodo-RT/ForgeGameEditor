// The clip functions of a character's script (step 1 of docs/models/движение-модель.md) and their recording.
//
// Motion Canvas writes `yield* body.y(2, 0.1)`; Luau has no yield* that a call could tell from being an argument. So
// a call leaves a task in its function's queue and returns it; a flow function (all, chain, sequence …) takes the
// tasks it is given out of the queue. What stays in the queue runs one after another, from the function's time, when
// the function ends or reads a value (body.y(), now(), log, duration). Nothing is laid out before its time is known.
//
// The Luau functions raise errors with luaL_error, and only when no object with a destructor is alive in their frame
// (the editor has no exceptions): the work that needs C++ objects is done in helpers that return false with
// MotionState::error, raised once they are gone.

#include "script_impl.h"

#include "forge/core/time.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <span>

namespace forge::pixel {

int api_part_set(lua_State* L); // script.cpp

namespace {

using Impl = PoseScript::Impl;
using Mode = Impl::Mode;

constexpr const char* kPartMeta = "forge.motion.part";
constexpr const char* kActorMeta = "forge.motion.actor"; // a scene's actor (part 6а): a part with play, loop, stop
constexpr const char* kPropMeta = "forge.motion.prop";
constexpr const char* kTaskMeta = "forge.motion.task";
constexpr const char* kBezierMeta = "forge.motion.bezier";
constexpr const char* kCameraMeta = "forge.motion.camera"; // a scene's camera (part 6б)
constexpr const char* kLayerMeta = "forge.motion.layer";   // a frame of a scene's own transition (part 6б)
// The properties a part has as fields (body.y, { y = 2 }): all but morph, a method of its own, the mask and the order.
constexpr Prop kHandleProps[] = {Prop::X,          Prop::Y,       Prop::Rot,     Prop::Scale,      Prop::Sx,      Prop::Sy,
                                 Prop::SkewX,      Prop::SkewY,   Prop::Opacity, Prop::Fill,       Prop::Stroke,  Prop::StrokeWidth,
                                 Prop::Dash,       Prop::DrawFrom, Prop::DrawTo, Prop::Visible,    Prop::Tint,    Prop::Flash,
                                 Prop::FlashColor, Prop::Blur,    Prop::Shadow,  Prop::ShadowX,    Prop::ShadowY, Prop::ShadowBlur};
constexpr const char* kFieldWords = "x, y, rot, scale, sx, sy, skew_x, skew_y, opacity, fill, stroke, stroke_width, dash, draw_from, draw_to, "
                                    "visible, tint, flash, flash_color, blur, shadow, shadow_x, shadow_y, shadow_blur";
constexpr const char* kNumberWords = "x, y, rot, scale, sx, sy, skew_x, skew_y, opacity, dash, draw_from, draw_to, flash, blur, shadow_x, "
                                     "shadow_y, shadow_blur";
// A scene's camera has x, y, zoom (its scale) and rot; a transition's frame x, y, rot, scale and opacity (part 6б).
constexpr Prop kCameraProps[] = {Prop::X, Prop::Y, Prop::Scale, Prop::Rot};
constexpr Prop kLayerProps[] = {Prop::X, Prop::Y, Prop::Rot, Prop::Scale, Prop::Opacity};
constexpr f64 kForever = std::numeric_limits<f64>::infinity();
constexpr u32 kMaxIterations = 100000;
constexpr u32 kMaxBindingDepth = 16;

struct TaskRef {
    u32 id = 0;
    u64 generation = 0;
};

struct Place {
    f64 t = 0;
    bool background = false; // directly under spawn or at: a loop may go on to the clip's end
    SiteRef start_site;
    f64 start_t = -1;
};

// ---- errors ----

// An error of the call running now: luaL_error puts its place, the call's line (script.cpp, fail).
[[noreturn]] void raise(lua_State* L, MotionState& ms) {
    if (ms.from_lua) {
        lua_pushlstring(L, ms.error.data(), ms.error.size());
        lua_error(L);
    }
    luaL_error(L, "%s", ms.error.c_str());
}

bool call_error(MotionState& ms, std::string msg) {
    ms.error = std::move(msg);
    ms.from_lua = false;
    return false;
}

// An error of a task found while laying it out: its own call's line, as Luau writes one.
bool task_error(const Impl& im, MotionState& ms, SiteRef s, const std::string& msg) {
    const CallSite* c = im.call(s);
    ms.error = c ? (s.file == 0 ? std::string("pose") : im.files[s.file].name + ".luau") + ":" + std::to_string(c->line) +
                       ": " + msg
                 : msg;
    ms.from_lua = true;
    return false;
}

bool lua_failed(lua_State* L, MotionState& ms) {
    const char* m = lua_tostring(L, -1);
    ms.error = m ? m : "ошибка без текста";
    ms.from_lua = true;
    lua_pop(L, 1);
    return false;
}

MotionState& motion(lua_State* L, const char* what) {
    Impl& im = impl_of(L);
    if (im.mode != Mode::Record || !im.motion) {
        if (im.scene) luaL_error(L, "%s — внутри scene = function(p) … end", what);
        luaL_error(L, "%s — внутри функции клипа: clips = { walk = function(p) … end }", what);
    }
    if (im.motion->in_tween)
        luaL_error(L, "%s — внутри tween(с, функция) только мгновенные значения: body.y(v)", what);
    if (im.motion->left && im.motion->contexts.size() == 1)
        luaL_error(L, "%s: после leave сцена кончается — leave последней строкой scene", what);
    return *im.motion;
}

// ---- handles ----

bool is_meta(lua_State* L, int idx, const char* meta) {
    if (!lua_getmetatable(L, idx)) return false;
    luaL_getmetatable(L, meta);
    const bool same = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return same;
}

// What a handle is of: a part (or a scene's actor), a scene's camera, a frame of its transition (part 6б).
enum class Handle : u8 { Part, Camera, Layer };

std::span<const Prop> handle_props(Handle h) {
    if (h == Handle::Camera) return kCameraProps;
    if (h == Handle::Layer) return kLayerProps;
    return kHandleProps;
}

// A property's field: the camera's scale is its zoom.
const char* field_word(Handle h, Prop p) { return h == Handle::Camera && p == Prop::Scale ? "zoom" : prop_word(p); }

const char* field_words(Handle h) {
    if (h == Handle::Camera) return "x, y, zoom, rot";
    if (h == Handle::Layer) return "x, y, rot, scale, opacity";
    return kFieldWords;
}

// The handle of a recorded part by its id: the camera and the frames of transitions only in a scene.
Handle handle_of(const Impl& im, std::string_view id) {
    if (!im.scene || !camera_part(id)) return Handle::Part;
    return id == kCameraPart ? Handle::Camera : Handle::Layer;
}

bool handle_has(Handle h, Prop p) {
    const std::span<const Prop> props = handle_props(h);
    return std::find(props.begin(), props.end(), p) != props.end();
}

// A part's handle (or a scene's actor's, with meta kActorMeta; the camera's and a transition frame's, part 6б).
void push_part(lua_State* L, const char* id, const char* meta = kPartMeta, Handle h = Handle::Part) {
    lua_newtable(L);
    lua_pushstring(L, id);
    lua_setfield(L, -2, "id");
    for (Prop p : handle_props(h)) {
        lua_newtable(L);
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, "part");
        lua_pushinteger(L, static_cast<int>(p));
        lua_setfield(L, -2, "prop");
        luaL_getmetatable(L, kPropMeta);
        lua_setmetatable(L, -2);
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, field_word(h, p));
    }
    luaL_getmetatable(L, meta);
    lua_setmetatable(L, -2);
    lua_setreadonly(L, -1, 1);
}

void push_camera(lua_State* L) { push_part(L, kCameraPart, kCameraMeta, Handle::Camera); }

bool part_handle(lua_State* L, int idx) {
    return lua_istable(L, idx) && (is_meta(L, idx, kPartMeta) || is_meta(L, idx, kActorMeta) || is_meta(L, idx, kCameraMeta) ||
                                   is_meta(L, idx, kLayerMeta));
}

// The id of the part handle at idx; raises when it is none.
const char* part_id(lua_State* L, int idx, const char* what) {
    if (!part_handle(L, idx))
        luaL_error(L, "%s — вызывайте у части через двоеточие: part(\"body\"):%s(…)", what, what);
    lua_getfield(L, idx, "id");
    const char* id = lua_tostring(L, -1);
    lua_pop(L, 1); // the handle keeps the string
    return id;
}

// The part and the property of the property handle at idx.
const char* prop_of(lua_State* L, int idx, Prop& p) {
    if (!lua_istable(L, idx) || !is_meta(L, idx, kPropMeta))
        luaL_error(L, "нужно свойство части: part(\"body\").y");
    lua_getfield(L, idx, "prop");
    p = static_cast<Prop>(lua_tointeger(L, -1));
    lua_getfield(L, idx, "part");
    lua_getfield(L, -1, "id");
    const char* id = lua_tostring(L, -1);
    lua_pop(L, 3);
    return id;
}

u32 part_slot(MotionState& ms, const char* id) {
    const auto [it, added] = ms.parts.try_emplace(id, static_cast<u32>(ms.rec->parts.size()));
    if (added) ms.rec->parts.push_back(id);
    return it->second;
}

i32 recorded_part(const Recording& rec, const char* id) {
    for (usize i = 0; i < rec.parts.size(); ++i)
        if (rec.parts[i] == id) return static_cast<i32>(i);
    return -1;
}

void push_task(lua_State* L, MotionState& ms, u32 id) {
    auto* r = static_cast<TaskRef*>(lua_newuserdata(L, sizeof(TaskRef)));
    r->id = id;
    r->generation = ms.rec->generation;
    luaL_getmetatable(L, kTaskMeta);
    lua_setmetatable(L, -2);
}

// A task of this recording at idx, or false.
bool task_at(lua_State* L, int idx, const MotionState& ms, u32& id) {
    if (lua_type(L, idx) != LUA_TUSERDATA || !is_meta(L, idx, kTaskMeta)) return false;
    const auto* r = static_cast<const TaskRef*>(lua_touserdata(L, idx));
    if (r->generation != ms.rec->generation || r->id >= ms.tasks.size()) return false;
    id = r->id;
    return true;
}

u32 add_task(MotionState& ms, Task&& t, bool pending = true) {
    const auto id = static_cast<u32>(ms.tasks.size());
    ms.tasks.push_back(std::move(t));
    if (pending) ms.contexts.back().pending.push_back(id);
    return id;
}

int ref_function(lua_State* L, MotionState& ms, int idx) {
    const int r = lua_ref(L, idx);
    ms.refs.push_back(r);
    return r;
}

// ---- arguments ----

bool read_length(lua_State* L, int idx, MotionState& ms, const char* what, f64& out) {
    out = 0;
    if (lua_isnoneornil(L, idx)) return true;
    if (!lua_isnumber(L, idx)) return call_error(ms, std::string(what) + ": длина — число секунд");
    out = lua_tonumber(L, idx);
    if (!std::isfinite(out) || out < 0) return call_error(ms, std::string(what) + ": длина — число секунд не меньше 0");
    return true;
}

bool read_value(lua_State* L, int idx, Prop p, MotionState& ms, PropValue& out) {
    out = {};
    if (prop_is_number(p)) {
        if (lua_type(L, idx) != LUA_TNUMBER)
            return call_error(ms, std::string(prop_word(p)) + ": нужно число, а не " + luaL_typename(L, idx));
        out.v = lua_tonumber(L, idx);
        if (!std::isfinite(out.v)) return call_error(ms, std::string(prop_word(p)) + ": не число");
        if (p == Prop::Flash && (out.v < 0 || out.v > 1)) return call_error(ms, "flash — доля от 0 до 1");
        if ((p == Prop::Blur || p == Prop::ShadowBlur) && out.v < 0) return call_error(ms, std::string(prop_word(p)) + " — число не ниже 0");
        return true;
    }
    if (prop_is_color(p)) {
        if (lua_isnil(L, idx)) return true; // its rest
        const char* text = lua_type(L, idx) == LUA_TSTRING ? lua_tostring(L, idx) : nullptr;
        if (!text || !parse_part_color(text, out.color))
            return call_error(ms, std::string(prop_word(p)) + ": нужен цвет \"#rrggbb\" или \"#rrggbbaa\"");
        out.v = 1;
        return true;
    }
    if (p == Prop::StrokeWidth) {
        if (lua_isnil(L, idx)) return true; // its own width
        if (lua_type(L, idx) != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, idx)) || lua_tonumber(L, idx) < 0)
            return call_error(ms, "stroke_width: нужно число не меньше 0 или nil (своя толщина)");
        out.v = 1;
        out.n = lua_tonumber(L, idx);
        return true;
    }
    if (p == Prop::Visible) {
        if (lua_type(L, idx) != LUA_TBOOLEAN) return call_error(ms, std::string("visible: нужно true или false, а не ") + luaL_typename(L, idx));
        out.v = lua_toboolean(L, idx) ? 1 : 0;
        return true;
    }
    if (p == Prop::Drawing) {
        // swap (part 4д): the name of a drawing, "draw" the main one.
        if (lua_type(L, idx) != LUA_TSTRING) return call_error(ms, "swap: имя рисунка — строка, swap(\"angry\", 0.4)");
        const Impl& im = impl_of(L);
        out.target = lua_tostring(L, idx);
        out.v = 1;
        if (out.target == "draw" || std::binary_search(im.drawings.begin(), im.drawings.end(), out.target)) return true;
        std::string names = "«draw»";
        for (const std::string& n : im.drawings) names += ", «" + n + "»";
        return call_error(ms, "нет рисунка «" + out.target + "»: есть " + names);
    }
    // morph: a shape of the SVG, or nil (or the part itself) for its own shape
    if (lua_isnil(L, idx)) return true;
    if (lua_type(L, idx) != LUA_TSTRING) return call_error(ms, "morph: нужно имя фигуры");
    out.target = lua_tostring(L, idx);
    out.v = 1;
    return true;
}

// A value of a handle's property: the camera's zoom is a number above 0 (part 6б).
bool read_field(lua_State* L, int idx, Handle h, Prop p, MotionState& ms, PropValue& out) {
    if (h != Handle::Camera || p != Prop::Scale) return read_value(L, idx, p, ms, out);
    out = {};
    if (lua_type(L, idx) != LUA_TNUMBER) return call_error(ms, std::string("zoom: нужно число, а не ") + luaL_typename(L, idx));
    out.v = lua_tonumber(L, idx);
    if (!std::isfinite(out.v) || out.v <= 0) return call_error(ms, "zoom — число больше 0: во сколько раз камера приближает");
    return true;
}

bool read_timing(Impl& im, lua_State* L, int idx, MotionState& ms, MoveStep& st) {
    if (lua_isnoneornil(L, idx)) return true;
    if (lua_type(L, idx) == LUA_TSTRING) {
        if (!vector::timing_from_word(lua_tostring(L, idx), st.timing))
            return call_error(ms, std::string("нет сглаживания «") + lua_tostring(L, idx) +
                                      "» (smooth, linear, in, out, back, bounce, elastic, in_sine, out_sine, in_out_sine "
                                      "… in_out_bounce)");
        return true;
    }
    // bezier(x1, y1, x2, y2) (part 4ж): its numbers and its call, computed exactly.
    if (lua_type(L, idx) == LUA_TUSERDATA && is_meta(L, idx, kBezierMeta)) {
        st.bezier = static_cast<i32>(ms.rec->beziers.size());
        ms.rec->beziers.push_back(*static_cast<const BezierUse*>(lua_touserdata(L, idx)));
        return true;
    }
    if (!lua_isfunction(L, idx)) return call_error(ms, "сглаживание — слово, bezier(…) или функция(k)");
    // An author's easing, taken at kCurvePoints points.
    std::vector<f64> curve(kCurvePoints);
    const Mode was = im.mode;
    for (usize i = 0; i < kCurvePoints; ++i) {
        lua_pushvalue(L, idx);
        lua_pushnumber(L, static_cast<f64>(i) / static_cast<f64>(kCurvePoints - 1));
        im.mode = Mode::Frame; // it may read nothing of the clip
        const int status = lua_pcall(L, 1, 1, 0);
        im.mode = was;
        if (status != LUA_OK) return lua_failed(L, ms);
        if (lua_type(L, -1) != LUA_TNUMBER || !std::isfinite(lua_tonumber(L, -1))) {
            lua_pop(L, 1);
            return call_error(ms, "сглаживание-функция должна вернуть число");
        }
        curve[i] = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    st.curve = static_cast<i32>(ms.rec->curves.size());
    ms.rec->curves.push_back(std::move(curve));
    return true;
}

bool read_path(lua_State* L, int idx, MotionState& ms, MoveStep& st) {
    if (lua_isnoneornil(L, idx)) return true;
    const char* w = lua_tostring(L, idx);
    if (w && std::strcmp(w, "arc") == 0) st.arc = true;
    else if (w && std::strcmp(w, "arc_reverse") == 0) st.arc = st.arc_reverse = true;
    else return call_error(ms, "путь — \"arc\" или \"arc_reverse\"");
    return true;
}

// {y = 2, sy = 0.9}: the properties in a fixed order, so the recording is the same each time; the camera's and a
// transition frame's only theirs (part 6б).
bool read_table(lua_State* L, int idx, MotionState& ms, const char* what, MoveStep& st, Handle h = Handle::Part) {
    if (!lua_istable(L, idx)) return call_error(ms, std::string(what) + ": нужна таблица, как { y = 2, sy = 0.9 }");
    const std::span<const Prop> props = handle_props(h);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
        const bool known = key && std::any_of(props.begin(), props.end(), [&](Prop p) { return std::strcmp(key, field_word(h, p)) == 0; });
        if (!known) {
            const std::string k = key ? key : "?";
            lua_pop(L, 2);
            return call_error(ms, std::string(what) + ": поля «" + k + "» нет (есть " + field_words(h) + ")");
        }
        lua_pop(L, 1);
    }
    for (Prop p : props) {
        lua_getfield(L, idx, field_word(h, p));
        if (!lua_isnil(L, -1)) {
            PropValue v;
            if (!read_field(L, lua_gettop(L), h, p, ms, v)) {
                lua_pop(L, 1);
                return false;
            }
            st.props.emplace_back(p, std::move(v));
        }
        lua_pop(L, 1);
    }
    if (st.props.empty()) return call_error(ms, std::string(what) + ": в таблице нет ни одного свойства");
    return true;
}

bool read_spring(lua_State* L, int idx, int tol, MotionState& ms, MoveStep& st) {
    st.spring = {};
    if (lua_isnoneornil(L, idx)) {
        vector::spring_preset("plop", st.spring);
    } else if (lua_type(L, idx) == LUA_TSTRING) {
        if (!vector::spring_preset(lua_tostring(L, idx), st.spring))
            return call_error(ms, std::string("нет пружины «") + lua_tostring(L, idx) + "» (" + vector::kSpringWords +
                                      ", или таблица {mass, stiffness, damping, velocity})");
    } else if (lua_istable(L, idx)) {
        f64* fields[] = {&st.spring.mass, &st.spring.stiffness, &st.spring.damping, &st.spring.velocity};
        const char* names[] = {"mass", "stiffness", "damping", "velocity"};
        for (usize i = 0; i < 4; ++i) {
            lua_getfield(L, idx, names[i]);
            if (!lua_isnil(L, -1)) {
                if (lua_type(L, -1) != LUA_TNUMBER) {
                    lua_pop(L, 1);
                    return call_error(ms, std::string("пружина: ") + names[i] + " — число");
                }
                *fields[i] = lua_tonumber(L, -1);
            }
            lua_pop(L, 1);
        }
    } else {
        return call_error(ms, std::string("пружина — слово (") + vector::kSpringWords + ") или таблица {mass, stiffness, damping, velocity}");
    }
    const vector::Spring& s = st.spring;
    if (!(s.mass > 0) || !(s.stiffness >= 0) || !(s.damping >= 0) || !std::isfinite(s.mass) ||
        !std::isfinite(s.stiffness) || !std::isfinite(s.damping) || !std::isfinite(s.velocity))
        return call_error(ms, "пружина: масса больше 0, жёсткость и затухание не меньше 0");
    st.tolerance = 0.001;
    if (!lua_isnoneornil(L, tol)) {
        st.tolerance = lua_tonumber(L, tol);
        if (!(st.tolerance > 0) || !std::isfinite(st.tolerance)) return call_error(ms, "пружина: допуск — число больше 0");
    }
    return true;
}

SiteRef here(const Impl& im) { return im.site_ref(im.site); }

// The part of the picture that a part of the clip is in the picture's checks: itself, or the part a copy is of
// (part 4г); -1 for none.
i32 rig_part(const Rig& rig, const Recording& rec, std::string_view id) {
    if (const i32 c = copy_of(rec, id); c >= 0) return rig.part(rec.copies[static_cast<usize>(c)].source);
    return rig.part(id);
}

// The picture of the character a part of the clip is in (part 4д): the main one, else the first of its drawings by
// name that has it; a copy's is its part's.
const Rig& rig_of(const Rig& rig, const Recording& rec, std::string_view id) {
    if (const i32 c = copy_of(rec, id); c >= 0) return rig.holder(rec.copies[static_cast<usize>(c)].source);
    return rig.holder(id);
}

// Why a name is no part to go by: none in the picture, or a copy, which is gone by only as c in its function.
std::string no_part(const Recording& rec, const std::string& name) {
    std::string why = "в рисунке нет части «" + name + "»";
    if (copy_of(rec, name) >= 0) why += ": копию зовут только через c в copies(…)";
    return why;
}

// ---- a scene (part 6а of docs/models/сцены-модель.md) ----

// The item of a scene's stage by id: null outside a scene's recording and for the stage's own shapes.
const StageItem* scene_item(const Impl& im, std::string_view id) { return im.scene_stage ? im.scene_stage->item(id) : nullptr; }

bool scene_actor_id(const Impl& im, std::string_view id) {
    const StageItem* it = scene_item(im, id);
    return it && it->kind == StageItem::Kind::Actor;
}

// The actor a part of a scene is in («ёжик» of «ёжик/head»); empty for the stage's own parts and outside a scene.
std::string_view actor_of(const Impl& im, std::string_view id) {
    const usize slash = id.find('/');
    if (slash == std::string_view::npos || !scene_actor_id(im, id.substr(0, slash))) return {};
    return id.substr(0, slash);
}

// What only a character's own code does to its parts: false with why for an actor's part in a scene.
bool character_only(const Impl& im, MotionState& ms, std::string_view id, const char* what) {
    if (actor_of(im, id).empty()) return true;
    return call_error(ms, "у части актёра «" + std::string(id) + "» нет " + what +
                              ": пути, привязка, крепление и копии частей — в коде персонажа");
}

// A part of the scene that goes by another (a path, a target, a carrier) is of the stage: false with why for an
// actor's part.
bool stage_target(const Impl& im, MotionState& ms, std::string_view target, const char* what) {
    if (actor_of(im, target).empty()) return true;
    return call_error(ms, std::string(what) + ": «" + std::string(target) + "» — часть актёра, а здесь нужна часть сцены или актёр целиком");
}

// Two parts of one picture: the stage's, or the same actor's (masks, morphs).
bool same_picture(const Impl& im, MotionState& ms, std::string_view id, std::string_view other, const char* what) {
    if (actor_of(im, id) == actor_of(im, other)) return true;
    return call_error(ms, std::string(what) + ": «" + std::string(other) + "» — из другого рисунка: часть актёра — своего актёра, "
                                                                              "остальное — сцены");
}

bool check_morph(const Impl& im, MotionState& ms, const char* id, const PropValue& v) {
    if (v.v > 0 && !same_picture(im, ms, id, v.target, "морфинг")) return false;
    if (!im.rig) return true;
    const Rig& pr = rig_of(*im.rig, *ms.rec, id);
    const i32 p = rig_part(pr, *ms.rec, id);
    if (p >= 0 && !pr.parts()[static_cast<usize>(p)].shape)
        return call_error(ms, std::string("«") + id + "» — группа, морфинг бывает только у фигуры");
    if (v.v > 0 && im.rig->holder(v.target).outline().find(v.target) < 0)
        return call_error(ms, "в рисунке нет фигуры «" + v.target + "»");
    return true;
}

// ---- reading values ----

bool resolve_now(Impl& im, MotionState& ms) {
    std::string why;
    if (resolve(*ms.rec, &im, why)) return true;
    ms.error = why;
    ms.from_lua = true;
    return false;
}

// What a part's property is at t by what is recorded so far.
bool value_now(Impl& im, MotionState& ms, u32 part, Prop p, f64 t, PropValue& out) {
    if (!resolve_now(im, ms)) return false;
    const i32 tr = ms.rec->track(part, p);
    if (tr < 0) {
        out = first_value(*ms.rec, part, p); // part 5б
        return true;
    }
    std::string why;
    if (sample(*ms.rec, static_cast<u32>(tr), t, &im, out, why)) return true;
    ms.error = why;
    ms.from_lua = true;
    return false;
}

bool flush(Impl& im, lua_State* L, usize ci);

// ---- layout ----

bool add_block(Impl& im, MotionState& ms, const MoveStep& st, u32 part, Prop p, f64 t, f64 length, Block::To to,
               const PropValue* value, u32 ref, const Place& at, i32* index = nullptr) {
    Recording& rec = *ms.rec;
    // A picture of a scene has no shapes: no colours, widths, dashes, shares drawn or morphs (part 6а).
    if (const StageItem* it = scene_item(im, rec.parts[part]); it && it->kind == StageItem::Kind::Picture) {
        constexpr Prop kShapes[] = {Prop::Fill,   Prop::Stroke, Prop::StrokeWidth, Prop::Dash,       Prop::DrawFrom,
                                    Prop::DrawTo, Prop::Tint,   Prop::Flash,       Prop::FlashColor, Prop::Morph};
        if (std::find(std::begin(kShapes), std::end(kShapes), p) != std::end(kShapes))
            return task_error(im, ms, st.site,
                              "у картинки «" + rec.parts[part] + "» нет «" + prop_word(p) +
                                  "»: у неё место, поворот, масштаб, наклон, прозрачность, видимость, размытие, тень, маска, порядок, "
                                  "путь и крепление");
    }
    if (rec.blocks.size() >= kMaxBlocks)
        return task_error(im, ms, st.site, "в клипе больше " + std::to_string(kMaxBlocks) + " блоков: столько переходов не нужно, проверьте циклы");
    Block b;
    b.part = part;
    b.prop = p;
    b.start = snap_time(t);
    b.length = snap_time(length);
    b.order = ms.order++;
    b.to = to;
    if (value) b.value = *value;
    b.ref = ref;
    b.binding = st.binding;
    b.timing = st.timing;
    b.curve = st.curve;
    b.bezier = st.bezier;
    b.site = st.site;
    b.value_arg = st.value_arg;
    b.length_arg = st.length_arg;
    b.timing_arg = st.timing_arg;
    b.value_field = st.value_field;
    if (at.start_t >= 0 && b.start == at.start_t) b.start_site = at.start_site;
    if (index) *index = static_cast<i32>(rec.blocks.size());
    rec.blocks.push_back(std::move(b));
    return true;
}

bool layout(Impl& im, lua_State* L, u32 id, const Place& at, f64& end);

// ---- a scene's camera (part 6б) ----

// The middle of the scene's canvas: the camera's pivot, which its x and y move.
vector::Point stage_middle(const Impl& im) {
    return im.scene_stage ? vector::Point{im.scene_stage->width() / 2, im.scene_stage->height() / 2}
                          : vector::Point{kSceneWidth / 2, kSceneHeight / 2};
}

// The camera's bone: it stands by no other part.
void camera_bone(const Impl& im, MotionState& ms) {
    if (bone_of(*ms.rec, kCameraPart) < 0) ms.rec->bones.push_back({kCameraPart, stage_middle(im), -1});
}

// Whether a track of the camera follows at t by the blocks laid so far: the last of them that started by then is a
// follow's.
bool following(const Recording& rec, u32 part, Prop p, f64 t) {
    const Block* last = nullptr;
    for (const Block& b : rec.blocks)
        if (b.part == part && b.prop == p && b.start <= t && (!last || b.start > last->start || (b.start == last->start && b.order > last->order)))
            last = &b;
    return last && last->to == Block::To::Follow;
}

// Where x or y alone of the camera is set while it follows, the follow ends: the other one stands where it left it.
bool end_follow(Impl& im, MotionState& ms, const MoveStep& st, u32 part, Prop moved, f64 t, const Place& at) {
    const Prop other = moved == Prop::X ? Prop::Y : Prop::X;
    if (!following(*ms.rec, part, other, t)) return true;
    MoveStep hold;
    hold.site = st.site;
    return add_block(im, ms, hold, part, other, t, 0, Block::To::Hold, nullptr, 0, at);
}

bool layout_move(Impl& im, MotionState& ms, u32 id, const Place& at, f64& end) {
    const u32 part = ms.tasks[id].part;
    const usize steps = ms.tasks[id].steps.size();
    std::vector<std::pair<Prop, u32>> first; // the chain's first block of each property, for back
    auto note_first = [&](Prop p, i32 b) {
        for (const auto& f : first)
            if (f.first == p) return;
        first.emplace_back(p, static_cast<u32>(b));
    };
    const Handle handle = handle_of(im, ms.rec->parts[part]);
    f64 t = at.t;
    for (usize si = 0; si < steps; ++si) {
        const MoveStep st = ms.tasks[id].steps[si];
        const usize made = ms.rec->blocks.size();
        const f64 step_t = t;
        switch (st.kind) {
        case MoveStep::Kind::Wait:
            ms.rec->pauses.push_back({snap_time(t), snap_time(st.length), st.site, st.length_arg});
            t = snap_time(t + st.length);
            break;
        case MoveStep::Kind::Tween:
        case MoveStep::Kind::Rest:
        case MoveStep::Kind::Bind: {
            i32 bx = -1, by = -1;
            for (const auto& [p, v] : st.props) {
                i32 b = -1;
                const Block::To to = st.kind == MoveStep::Kind::Rest   ? Block::To::Rest
                                     : st.kind == MoveStep::Kind::Bind ? Block::To::Binding
                                                                       : Block::To::Value;
                if (!add_block(im, ms, st, part, p, t, st.kind == MoveStep::Kind::Bind ? 0 : st.length, to, &v, 0, at, &b))
                    return false;
                note_first(p, b);
                if (p == Prop::X) bx = b;
                if (p == Prop::Y) by = b;
            }
            if (st.arc && bx >= 0 && by >= 0) {
                Block& x = ms.rec->blocks[static_cast<usize>(bx)];
                Block& y = ms.rec->blocks[static_cast<usize>(by)];
                x.arc = by, y.arc = bx;
                x.arc_reverse = y.arc_reverse = st.arc_reverse;
            }
            if (st.kind != MoveStep::Kind::Bind) t = snap_time(t + st.length);
            break;
        }
        case MoveStep::Kind::Back: {
            if (first.empty()) return task_error(im, ms, st.site, "back(…): до него в цепочке нет перехода, возвращаться не к чему");
            const auto chain = first;
            for (const auto& [p, b0] : chain)
                if (!add_block(im, ms, st, part, p, t, st.length, Block::To::Back, nullptr, b0, at)) return false;
            t = snap_time(t + st.length);
            break;
        }
        case MoveStep::Kind::Restore: {
            std::vector<u32>& stack = ms.save_stacks[part];
            if (stack.empty())
                return task_error(im, ms, st.site, "restore(…) без save(): сначала part(\"" + ms.rec->parts[part] + "\"):save()");
            const u32 s = stack.back();
            stack.pop_back();
            for (usize p = 0; p < kPropCount; ++p) {
                // The mask, the link and the order are no values to go back to; the drawing is no part's; the camera
                // has its own four (part 6б).
                if (prop_is_call(static_cast<Prop>(p)) || static_cast<Prop>(p) == Prop::Drawing) continue;
                if (handle != Handle::Part && !handle_has(handle, static_cast<Prop>(p))) continue;
                i32 b = -1;
                if (!add_block(im, ms, st, part, static_cast<Prop>(p), t, st.length, Block::To::Saved, nullptr, s, at, &b))
                    return false;
                note_first(static_cast<Prop>(p), b);
            }
            t = snap_time(t + st.length);
            break;
        }
        case MoveStep::Kind::Spring: {
            // Its length from where the property is by now: the recording so far. A later block that changes where
            // it starts moves its start, not its length.
            f64 longest = 0;
            for (const auto& [p, v] : st.props) {
                PropValue from;
                if (!value_now(im, ms, part, p, t, from)) return false;
                std::vector<f64> positions;
                const char* why = nullptr;
                if (!vector::spring_steps(st.spring, from.v, v.v, st.tolerance, positions, why))
                    return task_error(im, ms, st.site, why ? why : "пружина");
                const f64 length = snap_time(static_cast<f64>(positions.size()) / vector::kSpringRate);
                i32 b = -1;
                if (!add_block(im, ms, st, part, p, t, length, Block::To::Value, &v, 0, at, &b)) return false;
                Block& blk = ms.rec->blocks[static_cast<usize>(b)];
                blk.spring = true;
                blk.spring_def = st.spring;
                blk.tolerance = st.tolerance;
                note_first(p, b);
                longest = std::max(longest, length);
            }
            t = snap_time(t + longest);
            break;
        }
        case MoveStep::Kind::Along: {
            // x, y and, with turn, rot: along the path by the timing (part 4в).
            const bool turn = ms.rec->alongs[static_cast<usize>(st.along)].turn;
            for (Prop p : {Prop::X, Prop::Y, Prop::Rot}) {
                if (p == Prop::Rot && !turn) continue;
                i32 b = -1;
                if (!add_block(im, ms, st, part, p, t, st.length, Block::To::Along, nullptr, static_cast<u32>(st.along), at, &b))
                    return false;
                note_first(p, b);
            }
            t = snap_time(t + st.length);
            break;
        }
        case MoveStep::Kind::Snap: {
            // Where the target's pivot stands now, by what is recorded so far; x and y to the numbers that put this
            // part's pivot there as it stands now.
            if (!resolve_now(im, ms)) return false;
            const Recording& rec = *ms.rec;
            const i32 tb = bone_of(rec, st.target), ob = bone_of(rec, ms.rec->parts[part]);
            Affine wt, frame;
            std::string why;
            if (tb < 0 || ob < 0) return task_error(im, ms, st.site, "snap_to: у записи нет костей частей");
            if (!bone_world(rec, static_cast<u32>(tb), t, &im, wt, why) || !bone_frame(rec, static_cast<u32>(ob), t, &im, frame, why))
                return task_error(im, ms, st.site, why);
            if (!(std::abs(frame.a * frame.d - frame.b * frame.c) > 1e-12))
                return task_error(im, ms, st.site, "часть «" + ms.rec->parts[part] + "» сжата в точку: место для неё не посчитать");
            const vector::Point to = inverse(frame).apply(wt.apply(rec.bones[static_cast<usize>(tb)].pivot));
            const vector::Point pv = rec.bones[static_cast<usize>(ob)].pivot;
            const auto ref = static_cast<u32>(ms.rec->snaps.size());
            ms.rec->snaps.push_back(st.target);
            for (Prop p : {Prop::X, Prop::Y}) {
                PropValue v;
                v.v = p == Prop::X ? to.x - pv.x : to.y - pv.y;
                i32 b = -1;
                if (!add_block(im, ms, st, part, p, t, st.length, Block::To::Snap, &v, ref, at, &b)) return false;
                note_first(p, b);
            }
            t = snap_time(t + st.length);
            break;
        }
        case MoveStep::Kind::Follow: {
            // The camera's middle from where it is onto the target's pivot as it stands now, then with it (part 6б).
            if (!resolve_now(im, ms)) return false;
            const Recording& rec = *ms.rec;
            const i32 tb = bone_of(rec, st.target);
            if (tb < 0) return task_error(im, ms, st.site, "follow: у записи нет кости части «" + st.target + "»");
            Affine wt;
            std::string why;
            if (!bone_world(rec, static_cast<u32>(tb), t, &im, wt, why)) return task_error(im, ms, st.site, why);
            const vector::Point to = wt.apply(rec.bones[static_cast<usize>(tb)].pivot), pv = stage_middle(im);
            const auto ref = static_cast<u32>(ms.rec->follows.size());
            ms.rec->follows.push_back({st.target, pv});
            for (Prop p : {Prop::X, Prop::Y}) {
                PropValue v;
                v.v = p == Prop::X ? to.x - pv.x : to.y - pv.y;
                i32 b = -1;
                if (!add_block(im, ms, st, part, p, t, st.length, Block::To::Follow, &v, ref, at, &b)) return false;
                note_first(p, b);
            }
            t = snap_time(t + st.length);
            break;
        }
        case MoveStep::Kind::Unfollow:
            // follow(nil): the camera stands where the follow left it.
            for (Prop p : {Prop::X, Prop::Y})
                if (following(*ms.rec, part, p, t) && !add_block(im, ms, st, part, p, t, 0, Block::To::Hold, nullptr, 0, at)) return false;
            break;
        }
        // x or y alone set ends a follow of the camera's place (part 6б).
        if (handle == Handle::Camera) {
            bool x = false, y = false;
            for (usize b = made; b < ms.rec->blocks.size(); ++b) {
                const Block& blk = ms.rec->blocks[b];
                if (blk.to == Block::To::Follow || blk.to == Block::To::Hold) continue;
                x = x || blk.prop == Prop::X;
                y = y || blk.prop == Prop::Y;
            }
            if (x != y && !end_follow(im, ms, st, part, x ? Prop::X : Prop::Y, step_t, at)) return false;
        }
    }
    end = t;
    return true;
}

// f(arg[, part]) in a context of its own from at.t: what it leaves runs from there; end is where it ends. A scene's own
// transition is f(old, new), the handles of its two frames (part 6б).
bool run_function(Impl& im, lua_State* L, int fn, const f64* arg, const Place& at, f64& end, const char* part = nullptr,
                  const char* const* frames = nullptr) {
    MotionState& ms = *im.motion;
    const usize ci = ms.contexts.size();
    ms.contexts.push_back({{}, at.t, at.start_site, at.start_t});
    lua_getref(L, fn);
    int n = 0;
    if (arg) {
        lua_pushnumber(L, *arg);
        n = 1;
    }
    if (part) {
        push_part(L, part);
        ++n;
    }
    for (int i = 0; frames && i < 2; ++i, ++n) push_part(L, frames[i], kLayerMeta, Handle::Layer);
    if (lua_pcall(L, n, 0, 0) != LUA_OK) {
        ms.contexts.resize(ci);
        return lua_failed(L, ms);
    }
    if (!flush(im, L, ci)) {
        ms.contexts.resize(ci);
        return false;
    }
    end = ms.contexts[ci].cursor;
    ms.contexts.resize(ci);
    return true;
}

f64 event_length(const std::map<std::string, f64>* m, const std::string& name, f64 fallback, bool& from_file) {
    from_file = false;
    if (m) {
        const auto it = m->find(name);
        if (it != m->end() && std::isfinite(it->second) && it->second >= 0) {
            from_file = true;
            return it->second;
        }
    }
    return fallback;
}

bool layout_loop(Impl& im, lua_State* L, u32 id, const Place& at, f64& end) {
    MotionState& ms = *im.motion;
    const Task task = ms.tasks[id];
    f64 limit = kForever;
    f64 count = kForever;
    switch (task.kind) {
    case Task::Kind::Loop:
        count = task.number;
        if (std::isinf(count)) {
            if (!at.background)
                return task_error(im, ms, task.site,
                                  "loop(f) без числа повторов идёт до конца клипа: только прямо в spawn(…) или at(…); "
                                  "иначе loop(n, f) или loop_for(с, f)");
            if (!ms.main_done) {
                ms.deferred.push_back({id, at.t, at.start_site, at.start_t});
                ms.tasks[id].end = kForever;
                end = at.t;
                return true;
            }
            limit = ms.rec->length;
        }
        break;
    case Task::Kind::LoopFor: limit = snap_time(at.t + task.number); break;
    case Task::Kind::LoopUntil: {
        bool from_file = false;
        const f64 length = event_length(ms.lengths ? &ms.lengths->durations : nullptr, task.name, task.number, from_file);
        ms.rec->events.push_back({task.name, false, snap_time(at.t), snap_time(length), from_file, task.site});
        limit = snap_time(at.t + length);
        break;
    }
    default: break;
    }
    f64 cur = at.t;
    for (u32 i = 0;; ++i) {
        if (static_cast<f64>(i) >= count) break;
        if (!std::isinf(limit) && cur >= limit - 1e-9) break;
        if (i >= kMaxIterations) return task_error(im, ms, task.site, "больше " + std::to_string(kMaxIterations) + " повторов");
        const f64 arg = i;
        f64 e = cur;
        if (!run_function(im, L, task.fn, &arg, {cur, false, at.start_site, at.start_t}, e)) return false;
        if (!std::isinf(limit) && e <= cur)
            return task_error(im, ms, task.site, "повтор не занял времени и шёл бы вечно: в нём нужен переход или wait(с)");
        cur = snap_time(e);
    }
    end = std::isinf(limit) ? cur : limit;
    return true;
}

bool layout_tween(Impl& im, lua_State* L, u32 id, const Place& at, f64& end) {
    MotionState& ms = *im.motion;
    const Task task = ms.tasks[id];
    const f64 length = task.number;
    const auto n = static_cast<u32>(std::max(1.0, std::round(length * 60)));
    ms.in_tween = true;
    ms.tween.clear();
    ms.tween_frames = n + 1;
    for (u32 i = 0; i <= n; ++i) {
        ms.tween_frame = i;
        ms.tween_t = snap_time(at.t + length * i / n);
        lua_getref(L, task.fn);
        lua_pushnumber(L, static_cast<f64>(i) / n);
        lua_pushnumber(L, ms.tween_t - at.t);
        if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            ms.in_tween = false;
            return lua_failed(L, ms);
        }
    }
    ms.in_tween = false;
    MoveStep st;
    st.site = task.site;
    st.length_arg = 0;
    for (TweenTrack& tt : ms.tween) {
        // Frames before the first value take it; a frame it skipped keeps the one before.
        usize first = 0;
        while (first < tt.set.size() && !tt.set[first]) ++first;
        for (usize i = 0; i < tt.frames.size(); ++i)
            if (i < first) tt.frames[i] = tt.frames[first];
            else if (!tt.set[i] && i > 0) tt.frames[i] = tt.frames[i - 1];
        i32 b = -1;
        if (!add_block(im, ms, st, tt.part, tt.prop, at.t, length, Block::To::Value, &tt.frames.back(), 0, at, &b)) return false;
        ms.rec->blocks[static_cast<usize>(b)].frames = std::move(tt.frames);
    }
    ms.tween.clear();
    end = snap_time(at.t + length);
    return true;
}

// A scene's transition (part 6б): from 0, not waiting; its leave from where the scene's flow is, waiting its length.
// A kind is written as a function would be: its new frame from where it comes to rest.
bool layout_transition(Impl& im, lua_State* L, u32 id, const Place& at, f64& end) {
    MotionState& ms = *im.motion;
    const Task task = ms.tasks[id];
    if (!task.leave && at.t != 0) return task_error(im, ms, task.site, "transition — в начале scene");
    const char* const frames[2] = {kLayerParts[task.leave ? 2 : 0], kLayerParts[task.leave ? 3 : 1]};
    f64 length = task.number;
    if (task.fn >= 0) {
        f64 e = at.t;
        if (!run_function(im, L, task.fn, nullptr, {at.t, false, at.start_site, at.start_t}, e, nullptr, frames)) return false;
        length = snap_time(e - at.t);
    } else {
        const MoveStep& st = task.steps[0];
        MoveStep set;
        set.site = st.site;
        const u32 part = part_slot(ms, frames[1]);
        const vector::Point mid = stage_middle(im);
        std::vector<std::pair<Prop, f64>> from;
        if (task.name == "растворение") from = {{Prop::Opacity, 0}};
        else if (task.name == "приближение") from = {{Prop::Scale, 0.5}, {Prop::Opacity, 0}};
        else if (task.from == "лево") from = {{Prop::X, -2 * mid.x}};
        else if (task.from == "верх") from = {{Prop::Y, -2 * mid.y}};
        else if (task.from == "низ") from = {{Prop::Y, 2 * mid.y}};
        else from = {{Prop::X, 2 * mid.x}};
        for (const auto& [p, v] : from) {
            PropValue a, b = rest_value(p);
            a.v = v;
            if (!add_block(im, ms, set, part, p, at.t, 0, Block::To::Value, &a, 0, at) ||
                !add_block(im, ms, st, part, p, at.t, length, Block::To::Value, &b, 0, at))
                return false;
        }
    }
    ms.rec->transitions.push_back({task.leave, task.name, task.from, at.t, length, task.site});
    end = task.leave ? snap_time(at.t + length) : at.t;
    return true;
}

bool layout(Impl& im, lua_State* L, u32 id, const Place& at_in, f64& end) {
    MotionState& ms = *im.motion;
    Place at = at_in;
    at.t = snap_time(at.t);
    {
        Task& t = ms.tasks[id];
        if (t.laid) return task_error(im, ms, t.site, "этот переход уже записан, второй раз его не поставить");
        t.laid = true;
        t.start = at.t;
    }
    const Task task = ms.tasks[id]; // a copy: laying out runs Luau, which adds tasks
    f64 t = at.t;
    f64 children_end = t;
    bool ok = true;
    switch (task.kind) {
    case Task::Kind::Move: ok = layout_move(im, ms, id, at, end); break;
    case Task::Kind::Wait:
        ms.rec->pauses.push_back({t, snap_time(task.number), task.site, task.length_arg});
        end = t + task.number;
        break;
    case Task::Kind::All:
    case Task::Kind::Any: {
        f64 e = task.kind == Task::Kind::All ? t : kForever;
        for (u32 c : task.children) {
            f64 ce = t;
            if (!layout(im, L, c, {t, false, at.start_site, at.start_t}, ce)) return false;
            e = task.kind == Task::Kind::All ? std::max(e, ce) : std::min(e, ce);
        }
        end = task.children.empty() ? t : e;
        break;
    }
    case Task::Kind::Chain: {
        f64 cur = t;
        for (u32 c : task.children)
            if (!layout(im, L, c, {cur, false, at.start_site, at.start_t}, cur)) return false;
        end = cur;
        break;
    }
    case Task::Kind::Sequence: {
        // Motion Canvas's: each starts `step` after the one before, and it waits `step` after the last one too.
        f64 e = t + task.number * static_cast<f64>(task.children.size());
        for (usize i = 0; i < task.children.size(); ++i) {
            f64 ce = t;
            if (!layout(im, L, task.children[i], {snap_time(t + task.number * static_cast<f64>(i)), false, at.start_site, at.start_t}, ce))
                return false;
            e = std::max(e, ce);
        }
        end = e;
        break;
    }
    case Task::Kind::Delay: {
        const f64 s = snap_time(t + task.number);
        end = s;
        for (u32 c : task.children)
            if (!layout(im, L, c, {s, false, task.site, s}, end)) return false;
        break;
    }
    case Task::Kind::At:
    case Task::Kind::Spawn: {
        const f64 s = task.kind == Task::Kind::At ? snap_time(task.number) : t;
        const SiteRef ss = task.kind == Task::Kind::At ? task.site : at.start_site;
        const f64 st = task.kind == Task::Kind::At ? s : at.start_t;
        children_end = s;
        for (u32 c : task.children) {
            f64 ce = s;
            if (!layout(im, L, c, {s, true, ss, st}, ce)) return false;
            children_end = std::max(children_end, ms.tasks[c].end);
        }
        end = t;
        break;
    }
    case Task::Kind::Join: {
        f64 e = t;
        for (u32 c : task.children) {
            const Task& j = ms.tasks[c];
            if (!j.laid) return task_error(im, ms, task.site, "join(…): этот переход ещё не записан — spawn(…) должен идти раньше");
            if (std::isinf(j.end)) return task_error(im, ms, task.site, "join(…): этот переход идёт до конца клипа, его не дождаться");
            e = std::max(e, j.end);
        }
        end = e;
        break;
    }
    case Task::Kind::Loop:
    case Task::Kind::LoopFor:
    case Task::Kind::LoopUntil: ok = layout_loop(im, L, id, at, end); break;
    case Task::Kind::Run: ok = run_function(im, L, task.fn, nullptr, at, end); break;
    case Task::Kind::WaitUntil: {
        bool from_file = false;
        const f64 length = event_length(ms.lengths ? &ms.lengths->waits : nullptr, task.name, task.number, from_file);
        ms.rec->events.push_back({task.name, true, t, snap_time(length), from_file, task.site});
        end = t + length;
        break;
    }
    case Task::Kind::Mark:
        ms.rec->marks.push_back({task.mark, task.name, t, snap_time(task.number), task.site, {}});
        end = t;
        break;
    case Task::Kind::Save: {
        Saved s;
        s.part = task.part;
        s.time = t;
        s.order = ms.order++;
        ms.save_stacks[task.part].push_back(static_cast<u32>(ms.rec->saves.size()));
        ms.rec->saves.push_back(s);
        end = t;
        break;
    }
    case Task::Kind::TweenFn: ok = layout_tween(im, L, id, at, end); break;
    case Task::Kind::Transition: ok = layout_transition(im, L, id, at, end); break;
    case Task::Kind::Play: {
        // An actor's clip from now (part 6а): a play waits its length, a loop and a stop do not.
        ActorPlay play = task.play;
        play.start = t;
        play.order = ms.order++;
        if (at.start_t >= 0 && play.start == at.start_t) play.start_site = at.start_site;
        end = play.kind == ActorPlay::Kind::Play ? t + play.length : t;
        ms.rec->plays.push_back(std::move(play));
        break;
    }
    case Task::Kind::Copies: {
        // Born now; their functions go together from here, as all's (part 4г).
        const auto n = static_cast<u32>(task.number);
        for (u32 k = 0; k < n; ++k) ms.rec->copies[task.copy + k].born = t;
        f64 e = t;
        for (u32 k = 0; k < n; ++k) {
            const f64 i = k + 1;
            const std::string copy = ms.rec->copies[task.copy + k].id; // a copy: the function adds copies
            f64 ce = t;
            if (!run_function(im, L, task.fn, &i, {t, false, at.start_site, at.start_t}, ce, copy.c_str())) return false;
            e = std::max(e, ce);
        }
        end = e;
        break;
    }
    }
    if (!ok) return false;
    end = std::isinf(end) ? end : snap_time(end);
    Task& done = ms.tasks[id];
    if (done.kind == Task::Kind::At || done.kind == Task::Kind::Spawn) done.end = children_end;
    else if (!std::isinf(done.end)) done.end = end;
    return true;
}

// Lays out what waits in context ci, one after another from its cursor.
bool flush(Impl& im, lua_State* L, usize ci) {
    MotionState& ms = *im.motion;
    while (!ms.contexts[ci].pending.empty()) {
        std::vector<u32> list;
        list.swap(ms.contexts[ci].pending);
        for (u32 id : list) {
            if (ms.tasks[id].consumed || ms.tasks[id].laid) continue;
            const MotionContext& c = ms.contexts[ci];
            f64 end = c.cursor;
            if (!layout(im, L, id, {c.cursor, false, c.start_site, c.start_t}, end)) return false;
            ms.contexts[ci].cursor = end;
        }
    }
    return true;
}

// The tasks given to a flow function, from argument `from` on: tasks, functions (each becomes run(f)), lists of them.
bool collect(lua_State* L, MotionState& ms, int from, const char* what) {
    ms.args.clear();
    const int top = lua_gettop(L);
    for (int i = from; i <= top; ++i) {
        const int n = lua_istable(L, i) && !part_handle(L, i) && !is_meta(L, i, kPropMeta) ? lua_objlen(L, i) : -1;
        for (int k = n < 0 ? 0 : 1; k <= std::max(n, 0); ++k) {
            int idx = i;
            if (n >= 0) {
                lua_rawgeti(L, i, k);
                idx = lua_gettop(L);
            }
            u32 id = 0;
            bool ok = true;
            if (task_at(L, idx, ms, id)) {
                ms.args.push_back(id);
            } else if (lua_isfunction(L, idx)) {
                Task t;
                t.kind = Task::Kind::Run;
                t.fn = ref_function(L, ms, idx);
                ms.args.push_back(add_task(ms, std::move(t), false));
            } else {
                ok = false;
            }
            if (n >= 0) lua_pop(L, 1);
            if (!ok)
                return call_error(ms, std::string(what) + ": аргумент " + std::to_string(i - from + 1) +
                                          " — не переход и не функция (а " + luaL_typename(L, i) + ")");
        }
    }
    for (u32 id : ms.args) {
        Task& t = ms.tasks[id];
        if (t.laid)
            return call_error(ms, std::string(what) + ": переход уже записан — между ним и " + what +
                                      "(…) значение читали (body.y(), now(), log), и он пошёл по очереди");
        if (t.consumed) return call_error(ms, std::string(what) + ": этот переход уже отдан другой функции");
        t.consumed = true;
        std::vector<u32>& p = ms.contexts.back().pending;
        const auto it = std::find(p.begin(), p.end(), id);
        if (it != p.end()) p.erase(it);
    }
    return true;
}

// A flow function's task with what collect() read.
u32 flow_task(Impl& im, MotionState& ms, Task::Kind kind, f64 number) {
    Task t;
    t.kind = kind;
    t.children = ms.args;
    t.number = number;
    t.site = here(im);
    for (u32 c : t.children)
        if (ms.tasks[c].kind == Task::Kind::Run && ms.tasks[c].site.site < 0) ms.tasks[c].site = t.site;
    return add_task(ms, std::move(t));
}

f64 seconds(lua_State* L, int idx, const char* what) {
    if (lua_type(L, idx) != LUA_TNUMBER) luaL_error(L, "%s: нужно число секунд", what);
    const f64 s = lua_tonumber(L, idx);
    if (!std::isfinite(s) || s < 0) luaL_error(L, "%s: секунды — число не меньше 0", what);
    return s;
}

// ---- Luau: flow ----

int flow(lua_State* L, Task::Kind kind, const char* what, int from, f64 number) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, what);
    if (!collect(L, ms, from, what)) raise(L, ms);
    push_task(L, ms, flow_task(im, ms, kind, number));
    return 1;
}

int api_all(lua_State* L) { return flow(L, Task::Kind::All, "all", 1, 0); }
int api_any(lua_State* L) { return flow(L, Task::Kind::Any, "any", 1, 0); }
int api_chain(lua_State* L) { return flow(L, Task::Kind::Chain, "chain", 1, 0); }
int api_spawn(lua_State* L) { return flow(L, Task::Kind::Spawn, "spawn", 1, 0); }
int api_sequence(lua_State* L) { return flow(L, Task::Kind::Sequence, "sequence", 2, seconds(L, 1, "sequence(шаг, …)")); }
int api_at(lua_State* L) { return flow(L, Task::Kind::At, "at", 2, seconds(L, 1, "at(с, …)")); }

int api_delay(lua_State* L) {
    const f64 s = seconds(L, 1, "delay(с, переход)");
    if (lua_gettop(L) != 2) luaL_error(L, "delay(с, переход): один переход или функция");
    return flow(L, Task::Kind::Delay, "delay", 2, s);
}

int api_join(lua_State* L) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "join");
    if (lua_gettop(L) == 0) luaL_error(L, "join(…): каких ждать — тех, что дал spawn(…)");
    ms.args.clear();
    for (int i = 1; i <= lua_gettop(L); ++i) {
        u32 id = 0;
        if (!task_at(L, i, ms, id)) luaL_error(L, "join(…): аргумент %d — не переход", i);
        ms.args.push_back(id);
    }
    push_task(L, ms, flow_task(im, ms, Task::Kind::Join, 0));
    return 1;
}

int api_wait(lua_State* L) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "wait");
    Task t;
    t.kind = Task::Kind::Wait;
    t.number = seconds(L, 1, "wait(с)");
    t.site = here(im);
    t.length_arg = 0;
    push_task(L, ms, add_task(ms, std::move(t)));
    return 1;
}

int named_task(lua_State* L, Task::Kind kind, const char* what) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, what);
    const char* name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    if (!name || !*name) luaL_error(L, "%s(\"имя\", с): нужно имя события", what);
    const f64 fallback = lua_isnoneornil(L, 2) ? 0 : seconds(L, 2, what);
    Task t;
    t.kind = kind;
    t.name = name;
    t.number = fallback;
    t.site = here(im);
    t.length_arg = 1;
    push_task(L, ms, add_task(ms, std::move(t)));
    return 1;
}

int api_wait_until(lua_State* L) { return named_task(L, Task::Kind::WaitUntil, "wait_until"); }

int mark(lua_State* L, Mark::Kind kind, const char* what) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, what);
    const char* name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    if (!name || !*name) luaL_error(L, "%s(\"имя\"): нужно имя", what);
    const f64 length = kind == Mark::Kind::Emit && !lua_isnoneornil(L, 2) ? seconds(L, 2, what) : 0;
    Task t;
    t.kind = Task::Kind::Mark;
    t.mark = kind;
    t.name = name;
    t.number = length;
    t.site = here(im);
    t.length_arg = kind == Mark::Kind::Emit ? 1 : -1;
    push_task(L, ms, add_task(ms, std::move(t)));
    return 1;
}

int api_emit(lua_State* L) { return mark(L, Mark::Kind::Emit, "emit"); }
int api_sound(lua_State* L) { return mark(L, Mark::Kind::Sound, "sound"); }

int fn_task(lua_State* L, Task::Kind kind, const char* what, int fn_arg, f64 number, const char* name = nullptr) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, what);
    if (!lua_isfunction(L, fn_arg)) luaL_error(L, "%s: нужна функция", what);
    Task t;
    t.kind = kind;
    t.number = number;
    t.fn = ref_function(L, ms, fn_arg);
    t.site = here(im);
    if (name) t.name = name;
    t.length_arg = kind == Task::Kind::Loop ? -1 : 0;
    push_task(L, ms, add_task(ms, std::move(t)));
    return 1;
}

int api_loop(lua_State* L) {
    if (lua_isfunction(L, 1)) return fn_task(L, Task::Kind::Loop, "loop", 1, kForever);
    if (lua_type(L, 1) != LUA_TNUMBER) luaL_error(L, "loop(n, функция(i)) или loop(функция(i))");
    const f64 n = lua_tonumber(L, 1);
    if (!(n >= 0)) luaL_error(L, "loop(n, …): повторов не меньше 0");
    return fn_task(L, Task::Kind::Loop, "loop", 2, std::floor(n));
}

int api_loop_for(lua_State* L) { return fn_task(L, Task::Kind::LoopFor, "loop_for", 2, seconds(L, 1, "loop_for(с, функция)")); }

int api_loop_until(lua_State* L) {
    const char* name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    if (!name || !*name) luaL_error(L, "loop_until(\"имя\", функция): нужно имя события");
    const f64 fallback = lua_isnoneornil(L, 3) ? 0 : seconds(L, 3, "loop_until");
    return fn_task(L, Task::Kind::LoopUntil, "loop_until", 2, fallback, name);
}

int api_run(lua_State* L) { return fn_task(L, Task::Kind::Run, "run", 1, 0); }
int api_tween(lua_State* L) { return fn_task(L, Task::Kind::TweenFn, "tween", 2, seconds(L, 1, "tween(с, функция(k))")); }

bool flush_current(Impl& im, lua_State* L) { return flush(im, L, im.motion->contexts.size() - 1); }

int api_now(lua_State* L) {
    Impl& im = impl_of(L);
    if (im.mode == Mode::Record && im.motion) {
        MotionState& ms = *im.motion;
        if (ms.in_tween) {
            lua_pushnumber(L, ms.tween_t);
            return 1;
        }
        if (!flush_current(im, L)) raise(L, ms);
        ms.read = true; // no start_from after it (part 5б)
        lua_pushnumber(L, ms.contexts.back().cursor);
        return 1;
    }
    lua_pushnumber(L, im.frame_rec ? im.frame_t : 0);
    return 1;
}

bool duration_event(Impl& im, lua_State* L, const char* name, f64 fallback, f64& out) {
    MotionState& ms = *im.motion;
    if (!flush_current(im, L)) return false;
    bool from_file = false;
    out = event_length(ms.lengths ? &ms.lengths->durations : nullptr, name, fallback, from_file);
    ms.rec->events.push_back({name, false, ms.contexts.back().cursor, snap_time(out), from_file, here(im)});
    return true;
}

int api_duration(lua_State* L) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "duration");
    const char* name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    if (!name || !*name) luaL_error(L, "duration(\"имя\", с): нужно имя события");
    const f64 fallback = lua_isnoneornil(L, 2) ? 0 : seconds(L, 2, "duration");
    f64 length = 0;
    if (!duration_event(im, L, name, fallback, length)) raise(L, ms);
    lua_pushnumber(L, length);
    return 1;
}

bool log_line(Impl& im, lua_State* L) {
    MotionState& ms = *im.motion;
    if (!ms.in_tween && !flush_current(im, L)) return false;
    std::string text;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        size_t n = 0;
        const char* s = value_text(L, i, &n);
        if (i > 1) text += '\t';
        text.append(s, n);
        lua_pop(L, 1);
    }
    const SiteRef s = here(im);
    const CallSite* c = im.call(s);
    ms.rec->log.push_back({ms.in_tween ? ms.tween_t : ms.contexts.back().cursor, s, c ? c->line : 0, std::move(text)});
    return true;
}

int api_log(lua_State* L) {
    Impl& im = impl_of(L);
    if (im.mode != Mode::Record || !im.motion) return 0; // a frame's log would repeat each frame
    if (im.motion->left && im.motion->contexts.size() == 1) luaL_error(L, "log: после leave сцена кончается — leave последней строкой scene");
    if (!log_line(im, L)) raise(L, *im.motion);
    return 0;
}

// ---- Luau: parts ----

int api_part(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    Impl& im = impl_of(L);
    if (im.mode == Mode::Draw) luaL_error(L, "part — в клипах и pose: в draw частей ещё нет, их рисуют rect, circle, group …");
    if (im.mode == Mode::Record && im.motion && copy_of(*im.motion->rec, id) >= 0)
        luaL_error(L, "«%s» — копия: её берут в copies(…, function(i, c) … end)", id);
    if (!*id) luaL_error(L, "part(\"\"): у части есть имя, её id в рисунке");
    // A scene's camera by its name (part 6б); its transitions' frames are given to their functions only.
    if (im.scene && std::strcmp(id, kCameraPart) == 0) {
        push_camera(L);
        return 1;
    }
    if ((im.scene && camera_part(id)) || (im.rig && im.rig->holder(id).part(id) < 0)) luaL_error(L, "в рисунке нет части «%s»", id);
    push_part(L, id);
    return 1;
}

bool glob(const char* mask, const char* s) {
    if (!*mask) return !*s;
    if (*mask == '*') return glob(mask + 1, s) || (*s && glob(mask, s + 1));
    return *s && (*mask == '?' || *mask == *s) && glob(mask + 1, s + 1);
}

int api_parts(lua_State* L) {
    const char* mask = lua_isnoneornil(L, 1) ? "*" : luaL_checkstring(L, 1);
    Impl& im = impl_of(L);
    if (im.mode == Mode::Draw) luaL_error(L, "parts — в клипах и pose: в draw частей ещё нет, их рисуют rect, circle, group …");
    if (!im.rig) luaL_error(L, "parts(…): рисунка нет");
    lua_newtable(L);
    int n = 0;
    // The main picture's parts, then those only other drawings have (part 4д), by name.
    std::vector<const Rig*> pictures{im.rig};
    for (const std::string& name : im.rig->drawings()) pictures.push_back(im.rig->drawing(name));
    for (const Rig* r : pictures)
        for (const Part& p : r->parts())
            if (glob(mask, p.id.c_str()) && &im.rig->holder(p.id) == r) {
                push_part(L, p.id.c_str());
                lua_rawseti(L, -2, ++n);
            }
    return 1;
}

// copies("искра", n, function(i, c) … end) (part 4г): n copies of a part of the picture, numbered on from the part's
// copies before them in the clip. The task, laid out, gives them their birth and runs the function for each.
bool copies_task(Impl& im, lua_State* L, MotionState& ms, u32& task) {
    std::string source;
    if (lua_type(L, 1) == LUA_TSTRING) {
        source = lua_tostring(L, 1);
    } else if (part_handle(L, 1)) {
        lua_getfield(L, 1, "id");
        source = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
    } else {
        return call_error(ms, "copies(\"искра\", n, функция(i, c)): первым — имя части или часть");
    }
    Recording& rec = *ms.rec;
    if (scene_actor_id(im, source)) return call_error(ms, "копии актёра — не в этой версии: поставьте в stage столько актёров, сколько нужно");
    if (!character_only(im, ms, source, "copies")) return false;
    if (const i32 c = copy_of(rec, source); c >= 0)
        return call_error(ms, "«" + source + "» — копия, копии делают с части рисунка: copies(\"" + rec.copies[static_cast<usize>(c)].source + "\", …)");
    if (im.rig) {
        const Rig& sr = im.rig->holder(source);
        const i32 si = sr.part(source);
        if (si < 0) return call_error(ms, "в рисунке нет части «" + source + "»");
        if (!sr.has_shapes(si))
            return call_error(ms, "у части «" + source + "» нет фигур, копиям рисовать нечего: группу с display=\"none\" ThorVG не рисует — " +
                                      "спрячьте её в клипе: " + source + ".visible(false)");
    }
    if (lua_type(L, 2) != LUA_TNUMBER) return call_error(ms, "copies: вторым — число копий, целое от 1 до 64");
    const f64 n = lua_tonumber(L, 2);
    if (!(n >= 1 && n <= kMaxCopiesCall) || n != std::floor(n)) return call_error(ms, "copies: число копий — целое от 1 до 64");
    if (!lua_isfunction(L, 3)) return call_error(ms, "copies: третьим — функция(i, c): её зовут для каждой копии");
    if (lua_gettop(L) > 3) return call_error(ms, "copies: часть, число и функция — не больше");
    if (rec.copies.size() + static_cast<usize>(n) > kMaxCopies) return call_error(ms, "в клипе больше 256 копий");
    u32 before = 0;
    for (const Copy& c : rec.copies) before += c.source == source;
    std::vector<Copy> made;
    for (u32 k = 1; k <= static_cast<u32>(n); ++k) {
        Copy c;
        c.number = before + k;
        c.id = source + "[" + std::to_string(c.number) + "]";
        c.source = source;
        c.born = kForever; // until laid out
        c.site = here(im);
        if (im.rig && im.rig->holder(c.id).part(c.id) >= 0) return call_error(ms, "копии нужно имя «" + c.id + "», а так зовут часть рисунка");
        made.push_back(std::move(c));
    }
    Task t;
    t.kind = Task::Kind::Copies;
    t.site = here(im);
    t.number = n;
    t.copy = static_cast<u32>(rec.copies.size());
    t.fn = ref_function(L, ms, 3);
    rec.copies.insert(rec.copies.end(), made.begin(), made.end());
    task = add_task(ms, std::move(t));
    return true;
}

int api_copies(lua_State* L) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "copies");
    u32 task = 0;
    if (!copies_task(im, L, ms, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

// A transition chain of a part (or of one property) with its first step.
u32 move_task(Impl& im, MotionState& ms, const char* id, bool single, Prop p, MoveStep&& st) {
    Task t;
    t.kind = Task::Kind::Move;
    t.part = part_slot(ms, id);
    t.single = single;
    t.prop = p;
    t.site = st.site;
    t.steps.push_back(std::move(st));
    (void)im;
    return add_task(ms, std::move(t));
}

// swap("angry", 0.4, "smooth") (part 4д): the character goes to another of its drawings, a transition of the drawing's
// track (its part kDrawingPart).
bool swap_task(Impl& im, lua_State* L, MotionState& ms, u32& task) {
    if (im.scene) return call_error(ms, "swap — у персонажа: рисунок актёра меняют клипы его персонажа");
    MoveStep st;
    st.site = here(im);
    st.value_arg = 0;
    st.length_arg = 1;
    st.timing_arg = 2;
    if (lua_gettop(L) > 3) return call_error(ms, "swap(\"angry\", с, сглаживание): имя, длина и сглаживание — не больше");
    PropValue v;
    if (!read_value(L, 1, Prop::Drawing, ms, v) || !read_length(L, 2, ms, "swap", st.length) || !read_timing(im, L, 3, ms, st))
        return false;
    st.props.emplace_back(Prop::Drawing, std::move(v));
    task = move_task(im, ms, kDrawingPart, true, Prop::Drawing, std::move(st));
    return true;
}

int api_swap(lua_State* L) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "swap");
    u32 task = 0;
    if (!swap_task(im, L, ms, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

bool part_to(Impl& im, lua_State* L, MotionState& ms, const char* id, u32& task) {
    MoveStep st;
    st.site = here(im);
    st.value_arg = 0;
    st.value_field = true;
    st.length_arg = 1;
    st.timing_arg = 2;
    if (!read_table(L, 2, ms, "part:to", st, handle_of(im, id)) || !read_length(L, 3, ms, "part:to", st.length) ||
        !read_timing(im, L, 4, ms, st) || !read_path(L, 5, ms, st))
        return false;
    task = move_task(im, ms, id, false, Prop::X, std::move(st));
    return true;
}

int api_part_to(lua_State* L) {
    const char* id = part_id(L, 1, "to");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "part:to");
    u32 task = 0;
    if (!part_to(im, L, ms, id, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

bool rest_task(Impl& im, lua_State* L, MotionState& ms, const char* id, bool single, Prop p, int arg, u32& task) {
    MoveStep st;
    st.kind = MoveStep::Kind::Rest;
    st.site = here(im);
    st.length_arg = 0;
    st.timing_arg = 1;
    if (!read_length(L, arg, ms, "rest", st.length) || !read_timing(im, L, arg + 1, ms, st)) return false;
    const Handle h = handle_of(im, id);
    if (single) st.props.emplace_back(p, rest_value(p));
    else
        for (usize i = 0; i < kPropCount; ++i)
            if (!prop_is_call(static_cast<Prop>(i)) && static_cast<Prop>(i) != Prop::Drawing && (h == Handle::Part || handle_has(h, static_cast<Prop>(i))))
                st.props.emplace_back(static_cast<Prop>(i), rest_value(static_cast<Prop>(i)));
    task = move_task(im, ms, id, single, p, std::move(st));
    return true;
}

int api_part_rest(lua_State* L) {
    const char* id = part_id(L, 1, "rest");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "part:rest");
    u32 task = 0;
    if (!rest_task(im, L, ms, id, false, Prop::X, 2, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

u32 save_task(Impl& im, MotionState& ms, const char* id) {
    Task t;
    t.kind = Task::Kind::Save;
    t.part = part_slot(ms, id);
    t.site = here(im);
    return add_task(ms, std::move(t));
}

int api_part_save(lua_State* L) {
    const char* id = part_id(L, 1, "save");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "part:save");
    push_task(L, ms, save_task(im, ms, id));
    return 1;
}

bool restore_task(Impl& im, lua_State* L, MotionState& ms, const char* id, u32& task) {
    MoveStep st;
    st.kind = MoveStep::Kind::Restore;
    st.site = here(im);
    st.length_arg = 0;
    st.timing_arg = 1;
    if (!read_length(L, 2, ms, "restore", st.length) || !read_timing(im, L, 3, ms, st)) return false;
    task = move_task(im, ms, id, false, Prop::X, std::move(st));
    return true;
}

int api_part_restore(lua_State* L) {
    const char* id = part_id(L, 1, "restore");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "part:restore");
    u32 task = 0;
    if (!restore_task(im, L, ms, id, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

bool morph_task(Impl& im, lua_State* L, MotionState& ms, const char* id, u32& task) {
    MoveStep st;
    st.site = here(im);
    st.value_arg = 0;
    st.length_arg = 1;
    st.timing_arg = 2;
    PropValue v;
    if (!read_value(L, 2, Prop::Morph, ms, v)) return false;
    if (v.target == id) v = {};
    if (!check_morph(im, ms, id, v) || !read_length(L, 3, ms, "morph", st.length) || !read_timing(im, L, 4, ms, st))
        return false;
    st.props.emplace_back(Prop::Morph, std::move(v));
    task = move_task(im, ms, id, true, Prop::Morph, std::move(st));
    return true;
}

int api_part_morph(lua_State* L) {
    const char* id = part_id(L, 1, "morph");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "part:morph");
    u32 task = 0;
    if (!morph_task(im, L, ms, id, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

bool spring_task(Impl& im, lua_State* L, MotionState& ms, const char* id, bool single, Prop p, int to, u32& task) {
    MoveStep st;
    st.kind = MoveStep::Kind::Spring;
    st.site = here(im);
    st.value_arg = 0;
    st.value_field = !single;
    if (single) {
        if (!prop_is_number(p)) return call_error(ms, std::string("пружина — только у чисел: ") + kNumberWords);
        PropValue v;
        if (!read_field(L, to, handle_of(im, id), p, ms, v)) return false;
        st.props.emplace_back(p, std::move(v));
    } else {
        if (!read_table(L, to, ms, "part:spring", st, handle_of(im, id))) return false;
        for (const auto& [q, v] : st.props)
            if (!prop_is_number(q)) return call_error(ms, std::string("пружина — только у чисел: ") + kNumberWords);
    }
    if (!read_spring(L, to + 1, to + 2, ms, st)) return false;
    task = move_task(im, ms, id, single, p, std::move(st));
    return true;
}

int api_part_spring(lua_State* L) {
    const char* id = part_id(L, 1, "spring");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "part:spring");
    u32 task = 0;
    if (!spring_task(im, L, ms, id, false, Prop::X, 2, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

// part:to_top(), :to_bottom(), :above("body"), :below("body"): the part among its neighbours in its group, at its
// time in the flow, with no length.
bool order_task(Impl& im, lua_State* L, MotionState& ms, const char* id, OrderStep::Kind kind, const char* what, u32& task) {
    MoveStep st;
    st.site = here(im);
    PropValue v;
    v.v = static_cast<f64>(kind);
    if (kind == OrderStep::Kind::Above || kind == OrderStep::Kind::Below) {
        const char* other = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
        if (!other || lua_gettop(L) > 2) return call_error(ms, std::string(what) + "(\"body\"): одно имя соседней части");
        v.target = other;
        st.value_arg = 0;
        if (im.rig) {
            if (im.rig->holder(other).part(other) < 0) return call_error(ms, no_part(*ms.rec, v.target));
            if (v.target == id) return call_error(ms, std::string(what) + ": часть «" + id + "» рядом с самой собой не поставить");
            // Neighbours in the picture the part is in (part 4д); a copy stands among its part's.
            const Rig& r = rig_of(*im.rig, *ms.rec, id);
            if (!r.siblings(rig_part(r, *ms.rec, id), r.part(other)))
                return call_error(ms, std::string(id) + " и " + v.target + " в разных группах: порядок меняется только среди соседей");
        }
    } else if (lua_gettop(L) > 1) {
        return call_error(ms, std::string(what) + "() — без аргументов");
    }
    st.props.emplace_back(Prop::Order, std::move(v));
    task = move_task(im, ms, id, true, Prop::Order, std::move(st));
    return true;
}

int order_call(lua_State* L, OrderStep::Kind kind, const char* what) {
    const char* id = part_id(L, 1, what);
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, what);
    u32 task = 0;
    if (!order_task(im, L, ms, id, kind, what, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

// eye:mask("lid"[, { invert = true }]), eye:mask(nil): a block «часть · маска» without a length (part 4б).
bool mask_task(Impl& im, lua_State* L, MotionState& ms, const char* id, u32& task) {
    MoveStep st;
    st.site = here(im);
    PropValue v;
    const int n = lua_gettop(L);
    if (n > 3) return call_error(ms, "mask(\"lid\", { invert = true }): имя части-маски и, если нужно, таблица");
    if (!lua_isnoneornil(L, 2)) {
        if (lua_type(L, 2) != LUA_TSTRING) return call_error(ms, "mask: нужно имя части-маски или nil (без маски)");
        v.target = lua_tostring(L, 2);
        v.v = 1;
        if (!lua_isnoneornil(L, 3)) {
            if (!lua_istable(L, 3)) return call_error(ms, "mask: второй аргумент — { invert = true } или { invert = false }");
            lua_pushnil(L);
            while (lua_next(L, 3)) {
                const bool known = lua_type(L, -2) == LUA_TSTRING && std::strcmp(lua_tostring(L, -2), "invert") == 0 &&
                                   lua_type(L, -1) == LUA_TBOOLEAN;
                lua_pop(L, 1);
                if (!known) {
                    lua_pop(L, 1);
                    return call_error(ms, "mask: второй аргумент — { invert = true } или { invert = false }");
                }
            }
            lua_getfield(L, 3, "invert");
            if (lua_toboolean(L, -1)) v.v = 2;
            lua_pop(L, 1);
        }
        if (!same_picture(im, ms, id, v.target, "маска")) return false;
        if (im.rig) {
            const Rig& mr = im.rig->holder(v.target);
            const i32 b = mr.part(v.target);
            if (b < 0) return call_error(ms, no_part(*ms.rec, v.target));
            if (v.target == id) return call_error(ms, std::string("часть «") + id + "» не маска сама себе");
            if (!mr.has_shapes(b))
                return call_error(ms, "у маски «" + v.target + "» нет фигур: спрячьте фигуры в ней по одной (hidden = true) или саму группу через " +
                                          v.target + ".visible(false)");
        }
    } else if (n > 2 && !lua_isnil(L, 3)) {
        return call_error(ms, "mask(nil): без маски таблица не нужна");
    }
    st.props.emplace_back(Prop::Mask, std::move(v));
    task = move_task(im, ms, id, true, Prop::Mask, std::move(st));
    return true;
}

int api_part_mask(lua_State* L) {
    const char* id = part_id(L, 1, "mask");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "mask");
    u32 task = 0;
    if (!mask_task(im, L, ms, id, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

// ---- part 4в: paths, places and links ----

// The bone of a part and of the parts it is inside, a parent before it: where it stands in a frame without the
// picture.
i32 add_bone(MotionState& ms, const Rig& rig, i32 part) {
    if (part < 0) return -1;
    const std::string& id = rig.parts()[static_cast<usize>(part)].id;
    if (const i32 b = bone_of(*ms.rec, id); b >= 0) return b;
    const i32 parent = add_bone(ms, rig, rig.parts()[static_cast<usize>(part)].parent);
    ms.rec->bones.push_back({id, rig.pivot(part), parent});
    return static_cast<i32>(ms.rec->bones.size() - 1);
}

// The bone of a part of the clip: a copy's stands as its part at rest, inside the same parts (part 4г).
i32 add_bone_of(MotionState& ms, const Rig& rig, const char* id) {
    const i32 c = copy_of(*ms.rec, id);
    if (c < 0) return add_bone(ms, rig, rig.part(id));
    if (const i32 b = bone_of(*ms.rec, id); b >= 0) return b;
    const i32 src = rig.part(ms.rec->copies[static_cast<usize>(c)].source);
    if (src < 0) return -1;
    const i32 parent = add_bone(ms, rig, rig.parts()[static_cast<usize>(src)].parent);
    ms.rec->bones.push_back({id, rig.pivot(src), parent});
    return static_cast<i32>(ms.rec->bones.size() - 1);
}

// Whether part `inner` is inside part `outer` (or is it).
bool inside(const Rig& rig, i32 inner, i32 outer) {
    for (i32 q = inner; q >= 0; q = rig.parts()[static_cast<usize>(q)].parent)
        if (q == outer) return true;
    return false;
}

bool need_rig(const Impl& im, MotionState& ms, const char* what) {
    if (im.rig) return true;
    return call_error(ms, std::string(what) + " — только с рисунком: он читает линии, опорные точки и группы частей");
}

// ball:along("route", 0.8, "smooth", { turn = true, from = 0, to = 1, relative = false }).
bool along_task(Impl& im, lua_State* L, MotionState& ms, const char* id, u32& task) {
    if (!need_rig(im, ms, "along") || !character_only(im, ms, id, "along")) return false;
    MoveStep st;
    st.kind = MoveStep::Kind::Along;
    st.site = here(im);
    st.length_arg = 1;
    st.timing_arg = 2;
    const char* path = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    if (!path) return call_error(ms, "along(\"route\", с, сглаживание, { turn = true }): первым — имя фигуры-пути");
    if (lua_gettop(L) > 5) return call_error(ms, "along: путь, длина, сглаживание и таблица — не больше");
    if (!stage_target(im, ms, path, "along")) return false;
    // The path from the picture that has it, the part's pivot from its own (part 4д).
    const Rig& rig = im.rig->holder(path);
    const i32 pi = rig.part(path);
    if (pi < 0)
        return call_error(ms, std::string("в рисунке нет части-пути «") + path + "»" +
                                  (copy_of(*ms.rec, path) >= 0 ? ": копию зовут только через c в copies(…)" : ""));
    if (std::strcmp(path, id) == 0) return call_error(ms, std::string("часть «") + id + "» не идёт по себе");
    Along al;
    if (!lua_isnoneornil(L, 5)) {
        if (!lua_istable(L, 5)) return call_error(ms, "along: последним — таблица { turn = true, from = 0, to = 1, relative = true }");
        lua_pushnil(L);
        while (lua_next(L, 5)) {
            const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
            const std::string k = key ? key : "?";
            const bool flag = k == "turn" || k == "relative", share = k == "from" || k == "to";
            std::string bad;
            if (!flag && !share) bad = "along: поля «" + k + "» нет (есть turn, from, to, relative)";
            else if (flag && lua_type(L, -1) != LUA_TBOOLEAN) bad = "along: " + k + " — true или false";
            else if (share && (lua_type(L, -1) != LUA_TNUMBER || !(lua_tonumber(L, -1) >= 0 && lua_tonumber(L, -1) <= 1)))
                bad = "along: from и to — доли длины пути от 0 до 1";
            else if (k == "turn") al.turn = lua_toboolean(L, -1);
            else if (k == "relative") al.relative = lua_toboolean(L, -1);
            else if (k == "from") al.from = lua_tonumber(L, -1);
            else al.to = lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (!bad.empty()) {
                lua_pop(L, 1);
                return call_error(ms, bad);
            }
        }
    }
    if (!read_length(L, 3, ms, "along", st.length) || !read_timing(im, L, 4, ms, st)) return false;
    // The path once a recording, as a broken line in picture units.
    Recording& rec = *ms.rec;
    al.path = static_cast<u32>(rec.paths.size());
    for (usize i = 0; i < rec.paths.size(); ++i)
        if (rec.paths[i].id == path) al.path = static_cast<u32>(i);
    if (al.path == rec.paths.size()) {
        PathLine line;
        std::string why;
        if (!rig.path_of(pi, line, why)) return call_error(ms, why);
        rec.paths.push_back(std::move(line));
    }
    const Rig& own = rig_of(*im.rig, rec, id);
    // A copy's is its part's; the camera's the middle of the canvas (part 6б).
    al.pivot = handle_of(im, id) == Handle::Camera ? stage_middle(im) : own.pivot(rig_part(own, rec, id));
    st.along = static_cast<i32>(rec.alongs.size());
    rec.alongs.push_back(al);
    task = move_task(im, ms, id, false, Prop::X, std::move(st));
    return true;
}

int api_part_along(lua_State* L) {
    const char* id = part_id(L, 1, "along");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "along");
    u32 task = 0;
    if (!along_task(im, L, ms, id, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

// sword:snap_to("hand_tip"[, с, сглаживание]).
bool snap_task(Impl& im, lua_State* L, MotionState& ms, const char* id, u32& task) {
    if (!need_rig(im, ms, "snap_to") || !character_only(im, ms, id, "snap_to")) return false;
    MoveStep st;
    st.kind = MoveStep::Kind::Snap;
    st.site = here(im);
    st.length_arg = 1;
    st.timing_arg = 2;
    const char* target = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    if (!target) return call_error(ms, "snap_to(\"hand_tip\", с, сглаживание): первым — имя части");
    if (lua_gettop(L) > 4) return call_error(ms, "snap_to: часть, длина и сглаживание — не больше");
    if (!stage_target(im, ms, target, "snap_to")) return false;
    // Each from the picture that has it (part 4д): inside one another only in one picture.
    const Rig& rig = rig_of(*im.rig, *ms.rec, id);
    const Rig& tr = im.rig->holder(target);
    const i32 self = rig.part(id), ti = tr.part(target);
    if (ti < 0) return call_error(ms, no_part(*ms.rec, target));
    if (std::strcmp(target, id) == 0) return call_error(ms, std::string("часть «") + id + "» не встанет к самой себе");
    if (&tr == &rig && inside(rig, ti, self)) return call_error(ms, std::string("часть «") + id + "» не встанет к своей же «" + target + "»");
    if (!read_length(L, 3, ms, "snap_to", st.length) || !read_timing(im, L, 4, ms, st)) return false;
    add_bone(ms, tr, ti);
    add_bone_of(ms, rig, id);
    st.target = target;
    task = move_task(im, ms, id, false, Prop::X, std::move(st));
    return true;
}

int api_part_snap_to(lua_State* L) {
    const char* id = part_id(L, 1, "snap_to");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "snap_to");
    u32 task = 0;
    if (!snap_task(im, L, ms, id, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

// sword:attach("hand"), sword:detach(): a block «часть · крепление» without a length.
bool link_task(Impl& im, lua_State* L, MotionState& ms, const char* id, bool attach, u32& task) {
    if (!need_rig(im, ms, attach ? "attach" : "detach") || !character_only(im, ms, id, attach ? "attach" : "detach")) return false;
    MoveStep st;
    st.site = here(im);
    PropValue v;
    // Each from the picture that has it (part 4д): inside one another only in one picture.
    const Rig& rig = rig_of(*im.rig, *ms.rec, id);
    const i32 self = rig.part(id);
    if (!attach) {
        if (lua_gettop(L) > 1) return call_error(ms, "detach() — без аргументов: часть отпускает то, за что держится");
    } else {
        const char* target = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
        if (!target || lua_gettop(L) > 2) return call_error(ms, "attach(\"hand\"): одно имя части, за которой ходить");
        if (!stage_target(im, ms, target, "attach")) return false;
        const Rig& tr = im.rig->holder(target);
        const i32 ti = tr.part(target);
        if (ti < 0) return call_error(ms, no_part(*ms.rec, target));
        if (std::strcmp(target, id) == 0) return call_error(ms, std::string("часть «") + id + "» не крепится к самой себе");
        if (&tr == &rig && inside(rig, ti, self))
            return call_error(ms, std::string("часть «") + id + "» не пойдёт за «" + target + "»: «" + target + "» внутри неё");
        // A copy stands beside its part, inside the parts its part is inside (part 4г).
        const i32 src = rig_part(rig, *ms.rec, id);
        if (&tr == &rig && inside(rig, self >= 0 ? self : src >= 0 ? rig.parts()[static_cast<usize>(src)].parent : -1, ti))
            return call_error(ms, std::string("часть «") + id + "» и так ходит с «" + target + "»: она внутри неё");
        // No link round: from the target on, through the groups and the links of the whole clip, back to this part.
        std::vector<std::pair<std::string, std::string>> links = ms.links;
        links.emplace_back(id, target);
        std::vector<std::string> way{id};
        std::vector<std::string> seen;
        auto next_of = [&](const std::string& p) {
            std::vector<std::string> out;
            const Rig& pr = im.rig->holder(p);
            const i32 q = pr.part(p);
            if (q >= 0 && pr.parts()[static_cast<usize>(q)].parent >= 0) out.push_back(pr.parts()[static_cast<usize>(pr.parts()[static_cast<usize>(q)].parent)].id);
            for (const auto& [from, to] : links)
                if (from == p) out.push_back(to);
            return out;
        };
        std::function<bool(const std::string&)> reach = [&](const std::string& p) {
            way.push_back(p);
            if (p == id) return true;
            if (std::find(seen.begin(), seen.end(), p) == seen.end()) {
                seen.push_back(p);
                for (const std::string& n : next_of(p))
                    if (reach(n)) return true;
            }
            way.pop_back();
            return false;
        };
        if (reach(target)) {
            std::string text;
            for (const std::string& w : way) text += (text.empty() ? "" : " → ") + w;
            return call_error(ms, "крепление по кругу: " + text);
        }
        ms.links.emplace_back(id, target);
        add_bone(ms, tr, ti);
        add_bone_of(ms, rig, id);
        v.v = 1;
        v.target = target;
        st.value_arg = 0;
    }
    st.props.emplace_back(Prop::Link, std::move(v));
    task = move_task(im, ms, id, true, Prop::Link, std::move(st));
    return true;
}

int link_call(lua_State* L, bool attach) {
    const char* what = attach ? "attach" : "detach";
    const char* id = part_id(L, 1, what);
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, what);
    u32 task = 0;
    if (!link_task(im, L, ms, id, attach, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

int api_part_attach(lua_State* L) { return link_call(L, true); }
int api_part_detach(lua_State* L) { return link_call(L, false); }

// ---- part 5б: the start ----

// start_from("idle"): the clip starts where clip idle stands at its end. Met first with no pose given, the try stops
// and asks for idle (script.cpp records it, then this clip again); given it, the pose goes in: its parts, its links
// with their bones.
bool start_from_call(Impl& im, lua_State* L, MotionState& ms) {
    if (im.scene) return call_error(ms, "start_from — у клипа персонажа: сцена начинается с покоя, её актёры — со своих клипов");
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
        return call_error(ms, "start_from(\"idle\"): одно имя клипа, с позы конца которого начать");
    const std::string name = lua_tostring(L, 1);
    const std::string call = "start_from(\"" + name + "\")";
    if (ms.contexts.size() > 1) return call_error(ms, call + " — в самой функции клипа, не внутри другой функции");
    StartRequest* req = im.start;
    if (!ms.tasks.empty() || ms.read || !ms.rec->events.empty() || !ms.rec->start.clip.empty() || (req && !req->wanted.empty()))
        return call_error(ms, call + " — первая строка клипа: до неё уже переход, ожидание, метка или чтение значения");
    if (!std::binary_search(im.clips.begin(), im.clips.end(), name))
        return call_error(ms, "в коде нет клипа «" + name + "»: start_from начинает с позы конца другого клипа");
    if (!req) return call_error(ms, call + ": клип записывается без начала");
    if (!req->pose) {
        req->wanted = name;
        req->site = here(im);
        return call_error(ms, call + ": поза конца ещё не записана");
    }
    if (req->pose->clip != name) return call_error(ms, call + ": между записями клип начался с другого клипа");
    StartPose pose = *req->pose;
    pose.site = here(im);
    for (const StartValue& sv : pose.values) {
        part_slot(ms, sv.part.c_str());
        if (sv.prop != Prop::Link || sv.value.v <= 0) continue;
        // Held by a part: no link round through it, and both stand by their bones.
        ms.links.emplace_back(sv.part, sv.value.target);
        if (!im.rig) continue;
        const Rig& tr = im.rig->holder(sv.value.target);
        add_bone(ms, tr, tr.part(sv.value.target));
        add_bone_of(ms, rig_of(*im.rig, *ms.rec, sv.part), sv.part.c_str());
    }
    ms.rec->start = std::move(pose);
    return true;
}

int api_start_from(lua_State* L) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "start_from");
    if (!start_from_call(im, L, ms)) raise(L, ms);
    return 0;
}

int api_part_to_top(lua_State* L) { return order_call(L, OrderStep::Kind::Top, "to_top"); }
int api_part_to_bottom(lua_State* L) { return order_call(L, OrderStep::Kind::Bottom, "to_bottom"); }
int api_part_above(lua_State* L) { return order_call(L, OrderStep::Kind::Above, "above"); }
int api_part_below(lua_State* L) { return order_call(L, OrderStep::Kind::Below, "below"); }

// ---- a scene's actors (part 6а) ----

// The id of the actor handle at idx; raises when it is none.
const char* actor_id(lua_State* L, int idx, const char* what) {
    if (!lua_istable(L, idx) || !is_meta(L, idx, kActorMeta))
        luaL_error(L, "%s — у актёра через двоеточие: actor(\"ёжик\"):%s(…)", what, what);
    lua_getfield(L, idx, "id");
    const char* id = lua_tostring(L, -1);
    lua_pop(L, 1); // the handle keeps the string
    return id;
}

// actor("ёжик"):play("walk"[, { speed, params }]), :loop(…), :stop(): an actor's clip from the moment it is laid out.
// Its length is the recording's of the character's clip with the actor's parameters (and these over them): asked now,
// so a clip that does not record is an error at this line.
bool play_task(Impl& im, lua_State* L, MotionState& ms, const char* id, ActorPlay::Kind kind, u32& task) {
    const char* what = kind == ActorPlay::Kind::Play ? "play" : kind == ActorPlay::Kind::Loop ? "loop" : "stop";
    if (!im.scene_stage || !im.hooks || !im.hooks->clip_length)
        return call_error(ms, std::string(what) + " — у актёра сцены: actor(\"ёжик\"):" + what + "(…)");
    ActorPlay p;
    p.kind = kind;
    p.actor = id;
    p.site = here(im);
    if (kind == ActorPlay::Kind::Stop) {
        if (lua_gettop(L) > 1) return call_error(ms, "stop() — без аргументов: актёр стоит в позе, которую показывал");
    } else {
        const std::string call = std::string(what) + "(\"walk\"[, { speed = 1, params = { … } }])";
        if (lua_type(L, 2) != LUA_TSTRING || !*lua_tostring(L, 2)) return call_error(ms, call + ": первым — имя клипа персонажа");
        p.clip = lua_tostring(L, 2);
        if (lua_gettop(L) > 3) return call_error(ms, call + ": имя клипа и таблица — не больше");
        if (!lua_isnoneornil(L, 3)) {
            if (!lua_istable(L, 3)) return call_error(ms, call + ": вторым — таблица");
            lua_pushnil(L);
            while (lua_next(L, 3)) {
                const std::string key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "?";
                bool ok = true;
                std::string why;
                if (key == "speed") {
                    p.speed = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : 0;
                    ok = std::isfinite(p.speed) && p.speed > 0;
                    why = "speed — число больше 0: во сколько раз быстрее клипа";
                } else if (key == "params") {
                    ok = lua_istable(L, -1);
                    const int t = lua_gettop(L);
                    lua_pushnil(L);
                    while (ok && lua_next(L, t)) {
                        ok = lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TNUMBER && std::isfinite(lua_tonumber(L, -1));
                        if (ok) p.params[lua_tostring(L, -2)] = lua_tonumber(L, -1);
                        lua_pop(L, 1);
                    }
                    why = "params — таблица чисел по именам параметров персонажа: params = { height = 14 }";
                } else if (key == "blend") {
                    ok = false;
                    why = "blend — в части 6г сцен, вместе с игрой";
                } else {
                    ok = false;
                    why = "поля «" + key + "» нет (есть speed, params)";
                }
                if (!ok) {
                    lua_settop(L, 3);
                    return call_error(ms, std::string(what) + ": " + why);
                }
                lua_pop(L, 1);
            }
        }
        std::string why;
        const u64 since = time_now_ns();
        const bool got = im.hooks->clip_length(p.actor, p.clip, p.params, p.clip_length, why);
        im.hook_done(since); // the character's recording is not the scene's time
        if (!got) return call_error(ms, "актёр «" + p.actor + "»: " + why);
        p.length = snap_time(p.clip_length / p.speed);
    }
    Task t;
    t.kind = Task::Kind::Play;
    t.site = p.site;
    t.play = std::move(p);
    task = add_task(ms, std::move(t));
    return true;
}

int play_call(lua_State* L, ActorPlay::Kind kind, const char* what) {
    const char* id = actor_id(L, 1, what);
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, what);
    u32 task = 0;
    if (!play_task(im, L, ms, id, kind, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

int api_actor_play(lua_State* L) { return play_call(L, ActorPlay::Kind::Play, "play"); }
int api_actor_loop(lua_State* L) { return play_call(L, ActorPlay::Kind::Loop, "loop"); }
int api_actor_stop(lua_State* L) { return play_call(L, ActorPlay::Kind::Stop, "stop"); }

// actor("ёжик"):part("head"): the part «ёжик/head».
int api_actor_part(lua_State* L) {
    const char* id = actor_id(L, 1, "part");
    const char* sub = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    if (!sub || !*sub || lua_gettop(L) != 2) luaL_error(L, "actor(\"%s\"):part(\"head\"): одно имя части персонажа", id);
    lua_pushfstring(L, "%s/%s", id, sub);
    const char* full = lua_tostring(L, -1);
    Impl& im = impl_of(L);
    if (im.rig && im.rig->holder(full).part(full) < 0) luaL_error(L, "у актёра «%s» нет части «%s»", id, sub);
    push_part(L, full);
    return 1;
}

// ---- a scene's camera and transitions (part 6б) ----

// camera(): the scene's one camera.
int api_camera(lua_State* L) {
    Impl& im = impl_of(L);
    if (!im.scene) luaL_error(L, "camera — только в сцене: у персонажа камеры нет");
    if (im.mode == Mode::Draw) luaL_error(L, "camera — в scene = function(p) … end, а не в stage: stage ставит то, что на сцене");
    if (lua_gettop(L) > 0) luaL_error(L, "camera() — без аргументов: камера у сцены одна");
    push_camera(L);
    return 1;
}

// The target of center_on and follow at idx: a part of the scene or an actor as a whole, by name or handle; the
// camera's bone and the target's are laid in. Empty with why for none.
bool camera_target(Impl& im, lua_State* L, MotionState& ms, int idx, const char* what, std::string& out) {
    out.clear();
    if (lua_type(L, idx) == LUA_TSTRING) {
        out = lua_tostring(L, idx);
    } else if (part_handle(L, idx)) {
        lua_getfield(L, idx, "id");
        out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
    } else {
        return call_error(ms, std::string(what) + ": цель — часть сцены или актёр, \"ёжик\"");
    }
    if (camera_part(out)) return call_error(ms, std::string(what) + ": камера не идёт к самой себе — цель — часть сцены или актёр");
    if (!need_rig(im, ms, what) || !stage_target(im, ms, out, what)) return false;
    const Rig& tr = im.rig->holder(out);
    const i32 ti = tr.part(out);
    if (ti < 0) return call_error(ms, no_part(*ms.rec, out));
    add_bone(ms, tr, ti);
    camera_bone(im, ms);
    return true;
}

// camera():center_on("ёжик" | { x, y }[, с, сглаживание]): the frame's middle to where a part's or an actor's pivot
// stands then, as snap_to, or to a point of the stage.
bool center_task(Impl& im, lua_State* L, MotionState& ms, const char* id, u32& task) {
    MoveStep st;
    st.site = here(im);
    st.length_arg = 1;
    st.timing_arg = 2;
    if (lua_gettop(L) > 4) return call_error(ms, "center_on: цель, длина и сглаживание — не больше");
    if (lua_istable(L, 2) && !part_handle(L, 2)) {
        f64 xy[2] = {0, 0};
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
            const bool ok = key && (std::strcmp(key, "x") == 0 || std::strcmp(key, "y") == 0) && lua_type(L, -1) == LUA_TNUMBER &&
                            std::isfinite(lua_tonumber(L, -1));
            if (ok) xy[key[0] == 'y'] = lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (!ok) {
                lua_pop(L, 1);
                return call_error(ms, "center_on: точка — { x = …, y = … }, числа в единицах сцены от её левого верхнего угла");
            }
        }
        lua_getfield(L, 2, "x");
        lua_getfield(L, 2, "y");
        const bool both = !lua_isnil(L, -1) && !lua_isnil(L, -2);
        lua_pop(L, 2);
        if (!both) return call_error(ms, "center_on: у точки и x, и y: { x = 960, y = 540 }");
        const vector::Point mid = stage_middle(im);
        PropValue vx, vy;
        vx.v = xy[0] - mid.x;
        vy.v = xy[1] - mid.y;
        st.props.emplace_back(Prop::X, vx);
        st.props.emplace_back(Prop::Y, vy);
    } else {
        st.kind = MoveStep::Kind::Snap;
        if (!camera_target(im, L, ms, 2, "center_on", st.target)) return false;
    }
    if (!read_length(L, 3, ms, "center_on", st.length) || !read_timing(im, L, 4, ms, st)) return false;
    task = move_task(im, ms, id, false, Prop::X, std::move(st));
    return true;
}

int api_camera_center_on(lua_State* L) {
    const char* id = part_id(L, 1, "center_on");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "center_on");
    u32 task = 0;
    if (!center_task(im, L, ms, id, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

// camera():follow("ёжик"[, с, сглаживание]), camera():follow(nil).
bool follow_task(Impl& im, lua_State* L, MotionState& ms, const char* id, u32& task) {
    MoveStep st;
    st.kind = MoveStep::Kind::Follow;
    st.site = here(im);
    st.length_arg = 1;
    st.timing_arg = 2;
    if (lua_gettop(L) > 4) return call_error(ms, "follow: цель, длина и сглаживание — не больше");
    if (lua_isnoneornil(L, 2)) {
        if (lua_gettop(L) > 2) return call_error(ms, "follow(nil) — без длины: камера сразу стоит, где её оставило слежение");
        st.kind = MoveStep::Kind::Unfollow;
        st.length_arg = st.timing_arg = -1;
    } else if (!camera_target(im, L, ms, 2, "follow", st.target) || !read_length(L, 3, ms, "follow", st.length) ||
               !read_timing(im, L, 4, ms, st)) {
        return false;
    }
    task = move_task(im, ms, id, false, Prop::X, std::move(st));
    return true;
}

int api_camera_follow(lua_State* L) {
    const char* id = part_id(L, 1, "follow");
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "follow");
    u32 task = 0;
    if (!follow_task(im, L, ms, id, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

constexpr const char* kTransitionKinds[] = {"растворение", "сдвиг", "приближение"};
constexpr const char* kTransitionEdges[] = {"право", "лево", "верх", "низ"};

// transition(вид | функция, с[, { from, ease }]) and leave(…): a task of the scene's own flow, laid out by
// layout_transition. A kind's timing is its ease, else its own.
bool transition_task(Impl& im, lua_State* L, MotionState& ms, bool leave, u32& task) {
    const std::string what = leave ? "leave" : "transition";
    const std::string kinds = "«растворение», «сдвиг», «приближение» или своя функция(old, new)";
    if (ms.contexts.size() != 1)
        return call_error(ms, leave ? "leave — в самой scene, не внутри функции: после него сцена кончается" : "transition — в начале scene");
    if (!leave && ms.entered) return call_error(ms, "transition — в начале scene, один раз");
    Task t;
    t.kind = Task::Kind::Transition;
    t.leave = leave;
    t.site = here(im);
    MoveStep st;
    st.site = t.site;
    if (lua_isfunction(L, 1)) {
        if (lua_gettop(L) > 1) return call_error(ms, what + "(function(old, new) … end): длина перехода — длина записи функции, других аргументов нет");
        t.fn = ref_function(L, ms, 1);
    } else {
        if (lua_type(L, 1) != LUA_TSTRING) return call_error(ms, what + "(вид, с[, { from, ease }]): вид — " + kinds);
        t.name = lua_tostring(L, 1);
        if (std::find(std::begin(kTransitionKinds), std::end(kTransitionKinds), t.name) == std::end(kTransitionKinds))
            return call_error(ms, what + ": нет вида «" + t.name + "» (есть " + kinds + ")");
        if (lua_gettop(L) > 3) return call_error(ms, what + ": вид, длина и таблица — не больше");
        if (!read_length(L, 2, ms, what.c_str(), st.length)) return false;
        vector::timing_from_word(t.name == "растворение" ? "linear" : "in_out_cubic", st.timing);
        if (!lua_isnoneornil(L, 3)) {
            if (!lua_istable(L, 3)) return call_error(ms, what + ": третьим — таблица { from, ease }");
            lua_pushnil(L);
            while (lua_next(L, 3)) {
                const bool known = lua_type(L, -2) == LUA_TSTRING && (std::strcmp(lua_tostring(L, -2), "from") == 0 || std::strcmp(lua_tostring(L, -2), "ease") == 0);
                const std::string key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "?";
                lua_pop(L, 1);
                if (!known) {
                    lua_pop(L, 1);
                    return call_error(ms, what + ": поля «" + key + "» нет (есть from, ease)");
                }
            }
            lua_getfield(L, 3, "from");
            if (!lua_isnil(L, -1)) {
                const char* edge = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
                const bool good = std::find_if(std::begin(kTransitionEdges), std::end(kTransitionEdges),
                                               [&](const char* e) { return std::strcmp(e, edge) == 0; }) != std::end(kTransitionEdges);
                t.from = edge;
                lua_pop(L, 1);
                if (t.name != "сдвиг") return call_error(ms, what + ": from — только у «сдвиг»: с какого края въезжает новый кадр");
                if (!good) return call_error(ms, what + ": from — «право», «лево», «верх» или «низ»");
            } else {
                lua_pop(L, 1);
            }
            lua_getfield(L, 3, "ease");
            const bool eased = !lua_isnil(L, -1) && !read_timing(im, L, lua_gettop(L), ms, st);
            lua_pop(L, 1);
            if (eased) return false;
        }
        if (t.name == "сдвиг" && t.from.empty()) t.from = "право";
    }
    t.number = st.length;
    t.steps.push_back(std::move(st));
    if (leave) ms.left = true;
    else ms.entered = true;
    task = add_task(ms, std::move(t));
    return true;
}

int transition_call(lua_State* L, bool leave) {
    const char* what = leave ? "leave" : "transition";
    Impl& im = impl_of(L);
    if (!im.scene) luaL_error(L, "%s — только в сцене", what);
    MotionState& ms = motion(L, what);
    u32 task = 0;
    if (!transition_task(im, L, ms, leave, task)) raise(L, ms);
    return 0; // nothing to put in all(…) or spawn(…): it goes in the scene's own flow
}

int api_transition(lua_State* L) { return transition_call(L, false); }
int api_leave(lua_State* L) { return transition_call(L, true); }

// What later parts of a scene bring: an error that names the part, so code written ahead says so.
int later(lua_State* L, const char* what, const char* step) {
    luaL_error(L, "%s — в части %s сцен: в этой версии его ещё нет", what, step);
}
int api_music(lua_State* L) { return later(L, "music", "6в"); }
int api_wait_action(lua_State* L) { return later(L, "wait_action", "6в"); }
int api_talk(lua_State* L) { return later(L, "talk", "6в"); }

// ---- Luau: properties ----

void push_value(lua_State* L, Prop p, const PropValue& v) {
    if (prop_is_number(p)) {
        lua_pushnumber(L, v.v);
    } else if (p == Prop::Visible) {
        lua_pushboolean(L, v.v > 0);
    } else if (p == Prop::StrokeWidth && v.v > 0) {
        lua_pushnumber(L, v.n);
    } else if (prop_is_color(p) && v.v > 0) {
        const std::string hex = color_hex(v.color);
        lua_pushlstring(L, hex.data(), hex.size());
    } else if (p == Prop::Morph && v.v > 0) {
        lua_pushlstring(L, v.target.data(), v.target.size());
    } else {
        lua_pushnil(L);
    }
}

// body.y(): the value now: in a recording where its time has come to, in a frame at the frame's time.
bool get_value(Impl& im, lua_State* L, const char* id, Prop p, PropValue& out, std::string& why) {
    if (im.mode == Mode::Record && im.motion) {
        MotionState& ms = *im.motion;
        ms.read = true; // no start_from after it (part 5б)
        f64 t = ms.tween_t;
        if (ms.in_tween) {
            for (const TweenTrack& tt : ms.tween)
                if (ms.rec->parts[tt.part] == id && tt.prop == p)
                    for (u32 i = ms.tween_frame + 1; i-- > 0;)
                        if (tt.set[i]) {
                            out = tt.frames[i];
                            return true;
                        }
        } else {
            if (!flush_current(im, L)) return false;
            t = ms.contexts.back().cursor;
        }
        const i32 part = recorded_part(*ms.rec, id);
        if (part < 0) {
            out = rest_value(p);
            return true;
        }
        return value_now(im, ms, static_cast<u32>(part), p, t, out);
    }
    if (im.frame_rec) {
        const i32 part = recorded_part(*im.frame_rec, id);
        const i32 tr = part < 0 ? -1 : im.frame_rec->track(static_cast<u32>(part), p);
        if (tr < 0) {
            out = part < 0 ? rest_value(p) : first_value(*im.frame_rec, static_cast<u32>(part), p); // part 5б
            return true;
        }
        return sample(*im.frame_rec, static_cast<u32>(tr), im.frame_t, &im, out, why);
    }
    out = rest_value(p);
    return true;
}

int prop_getter(lua_State* L, Impl& im, const char* id, Prop p) {
    // The C++ objects live in this block only; an error is raised after it.
    bool ok = true;
    {
        PropValue out;
        std::string w;
        ok = get_value(im, L, id, p, out, w);
        if (ok) push_value(L, p, out);
        else if (im.motion && !im.motion->error.empty() && w.empty()) lua_pushlstring(L, im.motion->error.data(), im.motion->error.size());
        else lua_pushlstring(L, w.data(), w.size());
    }
    if (!ok) lua_error(L);
    return 1;
}

bool tween_set(Impl& im, lua_State* L, MotionState& ms, const char* id, Prop p) {
    PropValue v;
    if (!read_field(L, 2, handle_of(im, id), p, ms, v)) return false;
    if (p == Prop::Morph) return call_error(ms, "morph в tween не задаётся");
    const u32 part = part_slot(ms, id);
    TweenTrack* tt = nullptr;
    for (TweenTrack& t : ms.tween)
        if (t.part == part && t.prop == p) tt = &t;
    if (!tt) {
        ms.tween.push_back({part, p, std::vector<PropValue>(ms.tween_frames), std::vector<u8>(ms.tween_frames, 0)});
        tt = &ms.tween.back();
    }
    tt->frames[ms.tween_frame] = std::move(v);
    tt->set[ms.tween_frame] = 1;
    return true;
}

bool prop_task(Impl& im, lua_State* L, MotionState& ms, const char* id, Prop p, u32& task) {
    MoveStep st;
    st.site = here(im);
    if (lua_isfunction(L, 2)) {
        if (lua_gettop(L) > 2) return call_error(ms, std::string(prop_word(p)) + "(функция): связь ставится без длины");
        st.kind = MoveStep::Kind::Bind;
        lua_getref(L, ms.bindings);
        const int n = lua_objlen(L, -1);
        lua_pushvalue(L, 2);
        lua_rawseti(L, -2, n + 1);
        lua_pop(L, 1);
        st.binding = n;
        st.props.emplace_back(p, PropValue{});
    } else {
        st.value_arg = 0;
        st.length_arg = 1;
        st.timing_arg = 2;
        PropValue v;
        if (!read_field(L, 2, handle_of(im, id), p, ms, v)) return false;
        if (!read_length(L, 3, ms, prop_word(p), st.length) || !read_timing(im, L, 4, ms, st)) return false;
        if (!lua_isnoneornil(L, 5))
            return call_error(ms, "дуга — у x и y вместе: part(\"body\"):to({ x = …, y = … }, с, \"smooth\", \"arc\")");
        st.props.emplace_back(p, std::move(v));
    }
    task = move_task(im, ms, id, true, p, std::move(st));
    return true;
}

// body.y(…): () the value now, (v) at once, (v, s, timing) a transition, (function) a binding.
int api_prop_call(lua_State* L) {
    Prop p = Prop::X;
    const char* id = prop_of(L, 1, p);
    Impl& im = impl_of(L);
    if (lua_gettop(L) == 1) return prop_getter(L, im, id, p);
    if (im.mode == Mode::Record && im.motion && im.motion->in_tween) {
        MotionState& ms = *im.motion;
        if (lua_gettop(L) > 2) luaL_error(L, "внутри tween(с, функция) — только мгновенное значение: body.y(v)");
        if (!tween_set(im, L, ms, id, p)) raise(L, ms);
        return 0;
    }
    MotionState& ms = motion(L, prop_word(p));
    u32 task = 0;
    if (!prop_task(im, L, ms, id, p, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

int api_prop_rest(lua_State* L) {
    Prop p = Prop::X;
    const char* id = prop_of(L, 1, p);
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "rest");
    u32 task = 0;
    if (!rest_task(im, L, ms, id, true, p, 2, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

int api_prop_spring(lua_State* L) {
    Prop p = Prop::X;
    const char* id = prop_of(L, 1, p);
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "spring");
    u32 task = 0;
    if (!spring_task(im, L, ms, id, true, p, 2, task)) raise(L, ms);
    push_task(L, ms, task);
    return 1;
}

// bezier(x1, y1, x2, y2): CSS's cubic-bezier as an easing (part 4ж), anywhere a timing goes; e(x) gives y at x. It
// keeps the call that made it, so its numbers are rewritten there.
constexpr const char* kBezierWrong = "bezier: x1 и x2 — от 0 до 1, y1 и y2 — числа";

int api_bezier(lua_State* L) {
    vector::Bezier b;
    f64* n[] = {&b.x1, &b.y1, &b.x2, &b.y2};
    for (int i = 0; i < 4; ++i) {
        if (lua_type(L, i + 1) != LUA_TNUMBER) luaL_error(L, "%s", kBezierWrong);
        *n[i] = lua_tonumber(L, i + 1);
    }
    if (!vector::bezier_valid(b)) luaL_error(L, "%s", kBezierWrong);
    const BezierUse use{b, here(impl_of(L))};
    *static_cast<BezierUse*>(lua_newuserdata(L, sizeof(BezierUse))) = use;
    luaL_getmetatable(L, kBezierMeta);
    lua_setmetatable(L, -2);
    return 1;
}

int api_bezier_call(lua_State* L) {
    if (lua_type(L, 2) != LUA_TNUMBER) luaL_error(L, "bezier: кривая(x) — x число от 0 до 1");
    lua_pushnumber(L, vector::bezier(static_cast<const BezierUse*>(lua_touserdata(L, 1))->curve, lua_tonumber(L, 2)));
    return 1;
}

int api_bezier_text(lua_State* L) {
    const vector::Bezier& b = static_cast<const BezierUse*>(lua_touserdata(L, 1))->curve;
    const std::string text = "bezier(" + format_number(b.x1) + ", " + format_number(b.y1) + ", " + format_number(b.x2) + ", " +
                             format_number(b.y2) + ")";
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// spring(body.y, 2, "plop"): Motion Canvas's form.
int api_spring(lua_State* L) {
    Prop p = Prop::X;
    const char* id = prop_of(L, 1, p);
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, "spring");
    u32 task = 0;
    lua_remove(L, 1);
    lua_pushnil(L);
    lua_insert(L, 1); // the arguments where prop:spring has them
    if (!spring_task(im, L, ms, id, true, p, 2, task)) raise(L, ms);
    // Its value is the call's second argument.
    ms.tasks[task].steps.back().value_arg = 1;
    push_task(L, ms, task);
    return 1;
}

// ---- Luau: a task's chain ----

Task* move_of(lua_State* L, MotionState& ms, const char* what) {
    u32 id = 0;
    if (!task_at(L, 1, ms, id)) luaL_error(L, "%s — вызывайте у перехода через двоеточие: body.y(2, 0.1):%s(…)", what, what);
    Task& t = ms.tasks[id];
    if (t.kind != Task::Kind::Move)
        luaL_error(L, "%s — только у переходов частей; после all(…) и других — отдельной строкой", what);
    if (t.single && t.prop == Prop::Order)
        luaL_error(L, "%s — не после to_top, to_bottom, above и below: у порядка нет длины, следующий переход — отдельной строкой", what);
    if (t.single && t.prop == Prop::Mask)
        luaL_error(L, "%s — не после mask: у маски нет длины, следующий переход — отдельной строкой", what);
    if (t.single && t.prop == Prop::Link)
        luaL_error(L, "%s — не после attach и detach: у крепления нет длины, следующий переход — отдельной строкой", what);
    if (t.laid) luaL_error(L, "%s — переход уже записан: допишите цепочку раньше, чем читаете значения", what);
    return &t;
}

bool task_to(Impl& im, lua_State* L, MotionState& ms, Task& t) {
    MoveStep st;
    st.site = here(im);
    st.value_arg = 0;
    st.length_arg = 1;
    st.timing_arg = 2;
    if (t.single) {
        PropValue v;
        if (!read_field(L, 2, handle_of(im, ms.rec->parts[t.part]), t.prop, ms, v)) return false;
        if (t.prop == Prop::Morph) {
            if (v.target == ms.rec->parts[t.part]) v = {};
            if (!check_morph(im, ms, ms.rec->parts[t.part].c_str(), v)) return false;
        }
        st.props.emplace_back(t.prop, std::move(v));
        if (!lua_isnoneornil(L, 5)) return call_error(ms, "дуга — у x и y вместе: part(\"body\"):to({ x = …, y = … }, с, \"smooth\", \"arc\")");
    } else {
        st.value_field = true;
        if (!read_table(L, 2, ms, ":to", st, handle_of(im, ms.rec->parts[t.part])) || !read_path(L, 5, ms, st)) return false;
    }
    if (!read_length(L, 3, ms, ":to", st.length) || !read_timing(im, L, 4, ms, st)) return false;
    t.steps.push_back(std::move(st));
    return true;
}

int api_task_to(lua_State* L) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, ":to");
    Task* t = move_of(L, ms, "to");
    if (!task_to(im, L, ms, *t)) raise(L, ms);
    lua_settop(L, 1);
    return 1;
}

bool task_step(Impl& im, lua_State* L, MotionState& ms, Task& t, MoveStep::Kind kind) {
    MoveStep st;
    st.kind = kind;
    st.site = here(im);
    st.length_arg = 0;
    st.timing_arg = kind == MoveStep::Kind::Wait ? -1 : 1;
    if (!read_length(L, 2, ms, kind == MoveStep::Kind::Wait ? ":wait" : ":back", st.length)) return false;
    if (kind != MoveStep::Kind::Wait && !read_timing(im, L, 3, ms, st)) return false;
    t.steps.push_back(std::move(st));
    return true;
}

int api_task_back(lua_State* L) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, ":back");
    Task* t = move_of(L, ms, "back");
    if (!task_step(im, L, ms, *t, MoveStep::Kind::Back)) raise(L, ms);
    lua_settop(L, 1);
    return 1;
}

int api_task_wait(lua_State* L) {
    Impl& im = impl_of(L);
    MotionState& ms = motion(L, ":wait");
    Task* t = move_of(L, ms, "wait");
    if (!task_step(im, L, ms, *t, MoveStep::Kind::Wait)) raise(L, ms);
    lua_settop(L, 1);
    return 1;
}

// ---- metatables ----

int index_with(lua_State* L, const char* what, const char* known) {
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1)) return 1;
    const char* key = lua_tostring(L, 2);
    luaL_error(L, "у %s нет «%s» (%s)", what, key ? key : "?", known);
}

int api_part_index(lua_State* L) {
    return index_with(L, "части", "свойства: x, y, rot, scale, sx, sy, skew_x, skew_y, opacity, fill, stroke, stroke_width, dash, "
                                  "draw_from, draw_to, visible, tint, flash, flash_color, blur, shadow, shadow_x, shadow_y, "
                                  "shadow_blur; методы: to, rest, save, restore, morph, spring, set, to_top, to_bottom, above, "
                                  "below, mask, along, snap_to, attach, detach");
}
int api_actor_index(lua_State* L) {
    return index_with(L, "актёра", "методы: play, loop, stop, part и все методы части: to, rest, save, restore, spring, to_top, "
                                   "to_bottom, above, below, mask, along, snap_to, attach, detach");
}
int api_camera_index(lua_State* L) {
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1)) return 1;
    const char* key = lua_tostring(L, 2);
    luaL_error(L, "у камеры нет свойства «%s»: есть x, y, zoom, rot и методы to, center_on, along, follow, save, restore, rest", key ? key : "?");
}
int api_layer_index(lua_State* L) {
    return index_with(L, "кадра перехода", "свойства: x, y, rot, scale, opacity; методы: to, mask");
}
int api_prop_index(lua_State* L) { return index_with(L, "свойства", "методы: rest, spring; значение — body.y()"); }
int api_task_index(lua_State* L) { return index_with(L, "перехода", "методы: to, back, wait"); }

void methods_meta(lua_State* L, const char* meta, lua_CFunction index, std::initializer_list<std::pair<const char*, lua_CFunction>> methods,
                  lua_CFunction call = nullptr) {
    luaL_newmetatable(L, meta);
    lua_newtable(L);
    for (const auto& [name, fn] : methods) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
    lua_setreadonly(L, -1, 1);
    lua_pushcclosure(L, index, meta, 1);
    lua_setfield(L, -2, "__index");
    if (call) {
        lua_pushcfunction(L, call, meta);
        lua_setfield(L, -2, "__call");
    }
    lua_pushstring(L, "закрыто");
    lua_setfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
}

} // namespace

int scene_actor(lua_State* L) {
    Impl& im = impl_of(L);
    if (!im.scene) luaL_error(L, "actor — в сцене: актёр — персонаж на сцене, actor(\"ёжик\")");
    if (im.mode != Mode::Record && im.mode != Mode::Frame) luaL_error(L, "actor(\"ёжик\") — внутри scene = function(p) … end");
    const char* id = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    if (!id || lua_gettop(L) != 1) luaL_error(L, "actor(\"ёжик\"): одно имя актёра сцены");
    if (im.scene_stage && !scene_actor_id(im, id)) luaL_error(L, "на сцене нет актёра «%s»: актёров ставит stage, actor(\"%s\", \"персонажи/…\")", id, id);
    push_part(L, id, kActorMeta);
    return 1;
}

void register_motion_api(lua_State* L) {
    methods_meta(L, kPartMeta, &api_part_index,
                 {{"to", &api_part_to}, {"rest", &api_part_rest}, {"save", &api_part_save}, {"restore", &api_part_restore},
                  {"morph", &api_part_morph}, {"spring", &api_part_spring}, {"set", &api_part_set}, {"to_top", &api_part_to_top},
                  {"to_bottom", &api_part_to_bottom}, {"above", &api_part_above}, {"below", &api_part_below},
                  {"mask", &api_part_mask}, {"along", &api_part_along}, {"snap_to", &api_part_snap_to},
                  {"attach", &api_part_attach}, {"detach", &api_part_detach}});
    methods_meta(L, kActorMeta, &api_actor_index,
                 {{"to", &api_part_to}, {"rest", &api_part_rest}, {"save", &api_part_save}, {"restore", &api_part_restore},
                  {"morph", &api_part_morph}, {"spring", &api_part_spring}, {"to_top", &api_part_to_top},
                  {"to_bottom", &api_part_to_bottom}, {"above", &api_part_above}, {"below", &api_part_below},
                  {"mask", &api_part_mask}, {"along", &api_part_along}, {"snap_to", &api_part_snap_to},
                  {"attach", &api_part_attach}, {"detach", &api_part_detach}, {"play", &api_actor_play},
                  {"loop", &api_actor_loop}, {"stop", &api_actor_stop}, {"part", &api_actor_part}});
    methods_meta(L, kCameraMeta, &api_camera_index,
                 {{"to", &api_part_to}, {"center_on", &api_camera_center_on}, {"along", &api_part_along}, {"follow", &api_camera_follow},
                  {"save", &api_part_save}, {"restore", &api_part_restore}, {"rest", &api_part_rest}});
    methods_meta(L, kLayerMeta, &api_layer_index, {{"to", &api_part_to}, {"mask", &api_part_mask}});
    methods_meta(L, kPropMeta, &api_prop_index, {{"rest", &api_prop_rest}, {"spring", &api_prop_spring}}, &api_prop_call);
    methods_meta(L, kTaskMeta, &api_task_index, {{"to", &api_task_to}, {"back", &api_task_back}, {"wait", &api_task_wait}});
    luaL_newmetatable(L, kBezierMeta);
    lua_pushcfunction(L, &api_bezier_call, "bezier");
    lua_setfield(L, -2, "__call");
    lua_pushcfunction(L, &api_bezier_text, "bezier");
    lua_setfield(L, -2, "__tostring");
    lua_pushstring(L, "закрыто");
    lua_setfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
    const std::pair<const char*, lua_CFunction> globals[] = {
        {"part", &api_part},       {"parts", &api_parts},           {"all", &api_all},
        {"any", &api_any},         {"chain", &api_chain},           {"sequence", &api_sequence},
        {"delay", &api_delay},     {"at", &api_at},                 {"wait", &api_wait},
        {"wait_until", &api_wait_until}, {"duration", &api_duration}, {"now", &api_now},
        {"loop", &api_loop},       {"loop_for", &api_loop_for},     {"loop_until", &api_loop_until},
        {"run", &api_run},         {"spawn", &api_spawn},           {"join", &api_join},
        {"tween", &api_tween},     {"spring", &api_spring},         {"emit", &api_emit},
        {"sound", &api_sound},     {"log", &api_log},               {"copies", &api_copies},
        {"swap", &api_swap},       {"bezier", &api_bezier},     {"start_from", &api_start_from},
        {"camera", &api_camera},   {"transition", &api_transition}, {"leave", &api_leave},
        {"music", &api_music},     {"wait_action", &api_wait_action}, {"talk", &api_talk},
    };
    for (const auto& [name, fn] : globals) {
        lua_pushcfunction(L, fn, name);
        lua_setglobal(L, name);
    }
}

namespace {

// What the recording warns of, once a part: a skew that goes past ±kMaxSkew (the frame holds it there; a binding's
// value is held without a word), a dash on a part none of whose shapes has a pattern.
void add_notes(const Impl& im, const Rig* rig, Recording& rec) {
    std::vector<std::pair<u32, Prop>> said;
    auto note = [&](const Block& b, Prop kind, std::string text) {
        for (const auto& s : said)
            if (s.first == b.part && s.second == kind) return;
        said.emplace_back(b.part, kind);
        const CallSite* c = im.call(b.site);
        rec.notes.push_back({b.site, c ? c->line : 0, std::move(text)});
    };
    for (const Block& b : rec.blocks) {
        const std::string& part = rec.parts[b.part];
        if ((b.prop == Prop::SkewX || b.prop == Prop::SkewY) && b.to != Block::To::Binding) {
            // Along the whole block: an easing that overshoots goes past where it ends.
            constexpr int kSteps = 240;
            f64 worst = 0;
            for (int i = 0; i <= kSteps; ++i) {
                PropValue v;
                std::string why;
                if (block_value(rec, b, b.start + b.length * i / kSteps, nullptr, v, why) && std::abs(v.v) > std::abs(worst)) worst = v.v;
                if (b.length <= 0) break;
            }
            if (std::abs(worst) > kMaxSkew)
                note(b, Prop::SkewX,
                     "часть «" + part + "»: " + prop_word(b.prop) + " доходит до " + format_number(worst) +
                         "°, а наклон держится в −89…89°: дальше кадр не наклоняет");
        }
        if (b.prop == Prop::Dash && rig && !rig_of(*rig, rec, part).dashed(rig_part(rig_of(*rig, rec, part), rec, part)))
            note(b, Prop::Dash, "часть «" + part + "»: dash, а пунктира у её фигур нет");
    }
}

} // namespace

bool record_clip(Impl& im, lua_State* T, const Rig* rig, std::string_view clip, const std::map<std::string, f64>& values,
                 u64 seed, const MotionLengths& lengths, Recording& out, StartRequest* start, StartPose* ends) {
    if (im.scene) {
        lua_getfield(T, -1, "scene"); // a scene's one clip (part 6а)
    } else {
        lua_getfield(T, -1, "clips");
        lua_pushlstring(T, clip.data(), clip.size());
        lua_gettable(T, -2);
        lua_remove(T, -2);
    }
    im.motion = std::make_unique<MotionState>();
    MotionState& ms = *im.motion;
    ms.rec = &out;
    ms.lengths = &lengths;
    out.clip = clip;
    out.generation = ++im.generation;
    lua_newtable(im.L);
    ms.bindings = lua_ref(im.L, -1);
    lua_pop(im.L, 1);
    ms.contexts.push_back({});

    push_params(im, T, values);
    lua_getglobal(T, "rng");
    im.mode = Mode::Record;
    im.rig = rig;
    im.start = start;
    im.rng = frame_seed(seed, 0, clip);
    im.seed = seed;
    const u64 budget = im.budget_ns;
    im.budget_ns = budget * 10;
    im.begin_call();
    im.active = T;
    bool ok = lua_pcall(T, 2, 0, 0) == LUA_OK;
    if (!ok) {
        im.fail(lua_tostring(T, -1));
        lua_pop(T, 1);
    }
    // start_from asked for a pose not given yet, even under pcall: this try ends here (part 5б).
    if (start && !start->wanted.empty()) ok = false;
    if (ok && !flush(im, T, 0)) {
        ok = false;
        im.fail(ms.error.c_str());
    }
    if (ok) {
        out.length = snap_time(ms.contexts[0].cursor);
        ms.main_done = true;
        for (usize i = 0; ok && i < ms.deferred.size(); ++i) {
            const MotionState::Deferred d = ms.deferred[i];
            ms.tasks[d.task].laid = true;
            f64 end = 0;
            if (!layout_loop(im, T, d.task, {d.t, true, d.start_site, d.start_t}, end)) {
                ok = false;
                im.fail(ms.error.c_str());
            }
        }
    }
    if (ok) {
        std::string why;
        if (!resolve(out, &im, why)) {
            ok = false;
            im.fail(why.c_str());
        }
    }
    if (ok) add_notes(im, rig, out);
    if (ok && ends) {
        // The pose a clip that starts from this one starts with, its bindings asked while they are this recording's.
        std::string why;
        if (!end_pose(out, &im, *ends, why)) {
            ok = false;
            im.fail(why.c_str());
        }
    }
    for (const ScriptFile& f : im.files) out.files.push_back(f.name);
    for (int r : ms.refs) lua_unref(im.L, r);
    if (ok && !ends) {
        auto& slot = im.bindings[std::string(clip)];
        if (slot.first != LUA_NOREF && slot.second != 0) lua_unref(im.L, slot.first);
        slot = {ms.bindings, out.generation};
    } else {
        lua_unref(im.L, ms.bindings);
    }
    im.in_call = false;
    im.mode = Mode::None;
    im.rig = nullptr;
    im.start = nullptr;
    im.budget_ns = budget;
    im.motion.reset();
    if (!ok) out = {};
    return ok;
}

namespace {

// A binding is a function of time and the parts' values (docs/models/движение-модель.md, «Как считается кадр»): the
// same t gives the same frame whatever frames were read before, in this script or a new one. It is kept so while it
// runs: every table it can reach outside itself (through its upvalues, the functions it can call, its globals) is
// read-only, every upvalue it could set is looked at after it and put back. What would change is refused at its line.
constexpr int kMaxReach = 4096;
constexpr const char* kStateless = "у связи нет памяти между кадрами, она считает кадр только из времени и значений частей";

struct Guard {
    int seen = 0, closures = 0, frozen = 0; // stack indices of its tables
    int frozen_n = 0;
};

bool same_value(lua_State* T, int a, int b) {
    if (lua_rawequal(T, a, b)) return true;
    return lua_type(T, a) == LUA_TNUMBER && lua_type(T, b) == LUA_TNUMBER && std::isnan(lua_tonumber(T, a)) &&
           std::isnan(lua_tonumber(T, b));
}

enum class Reach { Ok, TooMuch, Thread };

// Pushes the guard's tables and walks from the function at fn: the tables, functions, buffers and userdata reachable
// through upvalues, environments, tables' keys, values and metatables (the sandbox's globals and libraries are
// read-only already). Remembers each function's upvalues (a C function keeps its state there too: string.gmatch its
// place) and each buffer's bytes, then makes the tables read-only. A coroutine's state cannot be kept, so one
// reachable is refused. Modules are not reached by require (it refuses inside a binding), only through upvalues.
Reach guard_open(lua_State* T, int fn, int globals, Guard& g) {
    lua_newtable(T);
    g.seen = lua_gettop(T);
    lua_newtable(T);
    g.closures = lua_gettop(T);
    lua_newtable(T);
    g.frozen = lua_gettop(T);
    lua_newtable(T);
    const int work = lua_gettop(T);
    int n = 0, count = 0;
    lua_pushvalue(T, fn);
    lua_rawseti(T, work, ++n);
    while (n > 0) {
        lua_rawgeti(T, work, n);
        lua_pushnil(T);
        lua_rawseti(T, work, n--);
        const int v = lua_gettop(T);
        const int type = lua_type(T, v);
        if (type == LUA_TTHREAD) {
            lua_settop(T, work - 1);
            return Reach::Thread;
        }
        lua_pushvalue(T, v);
        lua_rawget(T, g.seen);
        const bool skip = (type != LUA_TTABLE && type != LUA_TFUNCTION && type != LUA_TBUFFER && type != LUA_TUSERDATA) ||
                          lua_rawequal(T, v, globals) || !lua_isnil(T, -1);
        lua_pop(T, 1);
        if (skip) {
            lua_pop(T, 1);
            continue;
        }
        if (++count > kMaxReach) {
            lua_settop(T, work - 1);
            return Reach::TooMuch;
        }
        lua_pushvalue(T, v);
        lua_pushboolean(T, 1);
        lua_rawset(T, g.seen);
        if (type == LUA_TTABLE) {
            if (!lua_getreadonly(T, v)) {
                lua_pushvalue(T, v);
                lua_rawseti(T, g.frozen, ++g.frozen_n);
            }
            lua_pushnil(T);
            while (lua_next(T, v)) {
                lua_rawseti(T, work, ++n);
                lua_pushvalue(T, -1);
                lua_rawseti(T, work, ++n);
            }
            if (lua_getmetatable(T, v)) lua_rawseti(T, work, ++n);
        } else if (type == LUA_TBUFFER) {
            usize size = 0;
            const void* bytes = lua_tobuffer(T, v, &size);
            lua_pushvalue(T, v);
            lua_pushlstring(T, static_cast<const char*>(bytes), size);
            lua_rawset(T, g.closures);
        } else if (type == LUA_TUSERDATA) {
            if (lua_getmetatable(T, v)) lua_rawseti(T, work, ++n); // newproxy(true)'s, for one
        } else {
            lua_newtable(T);
            const int vals = lua_gettop(T);
            int i = 1;
            for (; lua_getupvalue(T, v, i); ++i) {
                lua_pushvalue(T, -1);
                lua_rawseti(T, work, ++n);
                lua_rawseti(T, vals, i);
            }
            lua_pushinteger(T, i - 1);
            lua_setfield(T, vals, "n");
            lua_pushvalue(T, v);
            lua_insert(T, -2);
            lua_rawset(T, g.closures);
            lua_getfenv(T, v);
            lua_rawseti(T, work, ++n);
        }
        lua_pop(T, 1);
    }
    lua_pop(T, 1); // work
    for (int i = 1; i <= g.frozen_n; ++i) {
        lua_rawgeti(T, g.frozen, i);
        lua_setreadonly(T, -1, 1);
        lua_pop(T, 1);
    }
    return Reach::Ok;
}

// Makes the tables writable again and puts back every upvalue and buffer the call changed; false when one changed.
// Leaves the stack as it was below the guard's tables.
bool guard_close(lua_State* T, Guard& g) {
    for (int i = 1; i <= g.frozen_n; ++i) {
        lua_rawgeti(T, g.frozen, i);
        lua_setreadonly(T, -1, 0);
        lua_pop(T, 1);
    }
    bool same = true;
    lua_pushnil(T);
    while (lua_next(T, g.closures)) {
        const int vals = lua_gettop(T), fn = vals - 1;
        if (lua_isbuffer(T, fn)) {
            usize size = 0, was_size = 0;
            void* bytes = lua_tobuffer(T, fn, &size);
            const char* was = lua_tolstring(T, vals, &was_size);
            if (size == was_size && size > 0 && std::memcmp(bytes, was, size) != 0) {
                same = false;
                std::memcpy(bytes, was, size);
            }
            lua_pop(T, 1);
            continue;
        }
        lua_getfield(T, vals, "n");
        const int count = static_cast<int>(lua_tointeger(T, -1));
        lua_pop(T, 1);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(T, vals, i);
            lua_getupvalue(T, fn, i);
            if (same_value(T, -1, -2)) {
                lua_pop(T, 2);
            } else {
                same = false;
                lua_pop(T, 1);
                lua_setupvalue(T, fn, i);
            }
        }
        lua_pop(T, 1);
    }
    lua_remove(T, g.frozen);
    lua_remove(T, g.closures);
    lua_remove(T, g.seen);
    return same;
}

} // namespace

bool PoseScript::Impl::eval(const Recording& rec, i32 binding, Prop prop, f64 t, PropValue& out, std::string& why) {
    int table = LUA_NOREF;
    if (motion && motion->rec == &rec) {
        table = motion->bindings;
    } else {
        const auto it = bindings.find(rec.clip);
        if (it != bindings.end() && it->second.second == rec.generation) table = it->second.first;
    }
    if (table == LUA_NOREF) return why = "запись клипа устарела: код загружен заново, запишите клип снова", false;
    if (binding_depth >= kMaxBindingDepth) return why = "связи ссылаются друг на друга по кругу", false;
    lua_State* T = active;
    if (!T) return why = "связь некому посчитать", false;
    if (!lua_checkstack(T, 32)) return why = "связи не хватило стека", false;
    lua_getref(T, table);
    lua_rawgeti(T, -1, binding + 1);
    lua_remove(T, -2);
    const int fn = lua_gettop(T);
    // Where it is written, for what it is refused.
    lua_Debug ar{};
    std::string at;
    if (lua_getinfo(T, -1, "s", &ar) && ar.short_src[0] && ar.linedefined > 0)
        at = std::string(ar.short_src) + ":" + std::to_string(ar.linedefined) + ": ";
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_xmove(L, T, 1);
    const int globals = lua_gettop(T);
    Guard guard;
    const Reach reach = guard_open(T, fn, globals, guard);
    if (reach != Reach::Ok) {
        lua_settop(T, fn - 1);
        if (reach == Reach::Thread)
            return why = at + "связь держит сопрограмму (coroutine), а её ход между кадрами не повторить: " + kStateless, false;
        return why = at + "связь видит больше " + std::to_string(kMaxReach) + " таблиц, функций и буферов: передайте ей только нужное",
               false;
    }
    lua_pushvalue(T, fn);
    const Mode was_mode = mode;
    const Recording* was_rec = frame_rec;
    const f64 was_t = frame_t;
    const i32 was_site = site;
    const u64 was_rng = rng;
    mode = Mode::Frame;
    frame_rec = &rec;
    frame_t = t;
    // Its chance: the seed's, the time's and its own, not what was drawn before it.
    rng = frame_seed(seed ^ (0x9E3779B97F4A7C15ull * static_cast<u64>(binding + 1)), t, rec.clip);
    ++binding_depth;
    const int status = lua_pcall(T, 0, 1, 0);
    --binding_depth;
    mode = was_mode;
    frame_rec = was_rec;
    frame_t = was_t;
    rng = was_rng;
    lua_insert(T, guard.seen); // the result or the error below the guard's tables
    ++guard.seen, ++guard.closures, ++guard.frozen;
    const bool same = guard_close(T, guard);
    lua_replace(T, fn); // over the function, the globals above it
    lua_settop(T, fn);
    if (status != LUA_OK) {
        const char* m = lua_tostring(T, -1);
        why = m ? m : "ошибка без текста";
        lua_pop(T, 1);
        const std::string kWrite = "attempt to modify a readonly table";
        if (why.size() >= kWrite.size() && why.compare(why.size() - kWrite.size(), kWrite.size(), kWrite) == 0)
            why = why.substr(0, why.size() - kWrite.size()) + "связь меняет таблицу или глобальную переменную вне себя: " + kStateless;
        return false;
    }
    if (!same) {
        lua_pop(T, 1);
        return why = at + "связь меняет переменную или буфер вне себя: " + kStateless, false;
    }
    site = was_site;
    out = {};
    bool good = true;
    if (prop_is_number(prop)) {
        good = lua_type(T, -1) == LUA_TNUMBER && std::isfinite(lua_tonumber(T, -1));
        if (good) out.v = lua_tonumber(T, -1);
        else why = at + "связь " + prop_word(prop) + " вернула не число";
    } else if (prop_is_color(prop) && !lua_isnil(T, -1)) {
        const char* text = lua_tostring(T, -1);
        good = text && parse_part_color(text, out.color);
        if (good) out.v = 1;
        else why = at + "связь " + prop_word(prop) + " вернула не цвет \"#rrggbb\" и не \"#rrggbbaa\"";
    } else if (prop == Prop::StrokeWidth && !lua_isnil(T, -1)) {
        good = lua_type(T, -1) == LUA_TNUMBER && std::isfinite(lua_tonumber(T, -1));
        if (good) out.v = 1, out.n = lua_tonumber(T, -1);
        else why = at + "связь stroke_width вернула не число и не nil";
    } else if (prop == Prop::Visible) {
        good = lua_type(T, -1) == LUA_TBOOLEAN;
        if (good) out.v = lua_toboolean(T, -1) ? 1 : 0;
        else why = at + "связь visible вернула не true и не false";
    }
    lua_pop(T, 1);
    return good;
}

} // namespace forge::pixel
