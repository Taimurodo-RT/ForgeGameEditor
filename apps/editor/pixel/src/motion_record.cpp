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

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace forge::pixel {

int api_part_set(lua_State* L); // script.cpp

namespace {

using Impl = PoseScript::Impl;
using Mode = Impl::Mode;

constexpr const char* kPartMeta = "forge.motion.part";
constexpr const char* kPropMeta = "forge.motion.prop";
constexpr const char* kTaskMeta = "forge.motion.task";
constexpr Prop kHandleProps[] = {Prop::X, Prop::Y, Prop::Rot, Prop::Scale, Prop::Sx, Prop::Sy, Prop::Opacity, Prop::Fill};
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
    if (im.mode != Mode::Record || !im.motion)
        luaL_error(L, "%s — внутри функции клипа: clips = { walk = function(p) … end }", what);
    if (im.motion->in_tween)
        luaL_error(L, "%s — внутри tween(с, функция) только мгновенные значения: body.y(v)", what);
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

void push_part(lua_State* L, const char* id) {
    lua_newtable(L);
    lua_pushstring(L, id);
    lua_setfield(L, -2, "id");
    for (Prop p : kHandleProps) {
        lua_newtable(L);
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, "part");
        lua_pushinteger(L, static_cast<int>(p));
        lua_setfield(L, -2, "prop");
        luaL_getmetatable(L, kPropMeta);
        lua_setmetatable(L, -2);
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, prop_word(p));
    }
    luaL_getmetatable(L, kPartMeta);
    lua_setmetatable(L, -2);
    lua_setreadonly(L, -1, 1);
}

// The id of the part handle at idx; raises when it is none.
const char* part_id(lua_State* L, int idx, const char* what) {
    if (!lua_istable(L, idx) || !is_meta(L, idx, kPartMeta))
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
        return true;
    }
    if (p == Prop::Fill) {
        if (lua_isnil(L, idx)) return true; // its own colour
        const char* text = lua_type(L, idx) == LUA_TSTRING ? lua_tostring(L, idx) : nullptr;
        if (!text || !parse_color(text, out.color)) return call_error(ms, "fill: нужен цвет \"#rrggbb\"");
        out.v = 1;
        return true;
    }
    // morph: a shape of the SVG, or nil (or the part itself) for its own shape
    if (lua_isnil(L, idx)) return true;
    if (lua_type(L, idx) != LUA_TSTRING) return call_error(ms, "morph: нужно имя фигуры");
    out.target = lua_tostring(L, idx);
    out.v = 1;
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
    if (!lua_isfunction(L, idx)) return call_error(ms, "сглаживание — слово или функция(k)");
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

// {y = 2, sy = 0.9}: the properties in a fixed order, so the recording is the same each time.
bool read_table(lua_State* L, int idx, MotionState& ms, const char* what, MoveStep& st) {
    if (!lua_istable(L, idx)) return call_error(ms, std::string(what) + ": нужна таблица, как { y = 2, sy = 0.9 }");
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        Prop p;
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
        if (!key || !prop_from_word(key, p) || p == Prop::Morph) {
            const std::string k = key ? key : "?";
            lua_pop(L, 2);
            return call_error(ms, std::string(what) + ": поля «" + k + "» нет (есть x, y, rot, scale, sx, sy, opacity, fill)");
        }
        lua_pop(L, 1);
    }
    for (Prop p : kHandleProps) {
        lua_getfield(L, idx, prop_word(p));
        if (!lua_isnil(L, -1)) {
            PropValue v;
            if (!read_value(L, lua_gettop(L), p, ms, v)) {
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

bool check_morph(const Impl& im, MotionState& ms, const char* id, const PropValue& v) {
    if (!im.rig) return true;
    const i32 p = im.rig->part(id);
    if (p >= 0 && !im.rig->parts()[static_cast<usize>(p)].shape)
        return call_error(ms, std::string("«") + id + "» — группа, морфинг бывает только у фигуры");
    if (v.v > 0 && im.rig->outline().find(v.target) < 0)
        return call_error(ms, "в SVG нет фигуры «" + v.target + "»");
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
        out = rest_value(p);
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

bool layout_move(Impl& im, MotionState& ms, u32 id, const Place& at, f64& end) {
    const u32 part = ms.tasks[id].part;
    const usize steps = ms.tasks[id].steps.size();
    std::vector<std::pair<Prop, u32>> first; // the chain's first block of each property, for back
    auto note_first = [&](Prop p, i32 b) {
        for (const auto& f : first)
            if (f.first == p) return;
        first.emplace_back(p, static_cast<u32>(b));
    };
    f64 t = at.t;
    for (usize si = 0; si < steps; ++si) {
        const MoveStep st = ms.tasks[id].steps[si];
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
        }
    }
    end = t;
    return true;
}

// f(arg) in a context of its own from at.t: what it leaves runs from there; end is where it ends.
bool run_function(Impl& im, lua_State* L, int fn, const f64* arg, const Place& at, f64& end) {
    MotionState& ms = *im.motion;
    const usize ci = ms.contexts.size();
    ms.contexts.push_back({{}, at.t, at.start_site, at.start_t});
    lua_getref(L, fn);
    int n = 0;
    if (arg) {
        lua_pushnumber(L, *arg);
        n = 1;
    }
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
        ms.rec->marks.push_back({task.mark, task.name, t, snap_time(task.number), task.site});
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
        const int n = lua_istable(L, i) && !is_meta(L, i, kPartMeta) && !is_meta(L, i, kPropMeta) ? lua_objlen(L, i) : -1;
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
        const char* s = luaL_tolstring(L, i, &n);
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
    if (!log_line(im, L)) raise(L, *im.motion);
    return 0;
}

// ---- Luau: parts ----

int api_part(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    Impl& im = impl_of(L);
    if (im.rig && im.rig->part(id) < 0) luaL_error(L, "в SVG нет части «%s»", id);
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
    if (!im.rig) luaL_error(L, "parts(…): рисунка нет");
    lua_newtable(L);
    int n = 0;
    for (const Part& p : im.rig->parts())
        if (glob(mask, p.id.c_str())) {
            push_part(L, p.id.c_str());
            lua_rawseti(L, -2, ++n);
        }
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

bool part_to(Impl& im, lua_State* L, MotionState& ms, const char* id, u32& task) {
    MoveStep st;
    st.site = here(im);
    st.value_arg = 0;
    st.value_field = true;
    st.length_arg = 1;
    st.timing_arg = 2;
    if (!read_table(L, 2, ms, "part:to", st) || !read_length(L, 3, ms, "part:to", st.length) ||
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
    if (single) st.props.emplace_back(p, rest_value(p));
    else
        for (usize i = 0; i < kPropCount; ++i) st.props.emplace_back(static_cast<Prop>(i), rest_value(static_cast<Prop>(i)));
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
        if (!prop_is_number(p)) return call_error(ms, "пружина — только у чисел: x, y, rot, scale, sx, sy, opacity");
        PropValue v;
        if (!read_value(L, to, p, ms, v)) return false;
        st.props.emplace_back(p, std::move(v));
    } else {
        if (!read_table(L, to, ms, "part:spring", st)) return false;
        for (const auto& [q, v] : st.props)
            if (!prop_is_number(q)) return call_error(ms, "пружина — только у чисел: x, y, rot, scale, sx, sy, opacity");
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

// ---- Luau: properties ----

void push_value(lua_State* L, Prop p, const PropValue& v) {
    if (prop_is_number(p)) {
        lua_pushnumber(L, v.v);
    } else if (p == Prop::Fill && v.v > 0) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x", v.color.r, v.color.g, v.color.b);
        lua_pushstring(L, buf);
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
            out = rest_value(p);
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
    if (!read_value(L, 2, p, ms, v)) return false;
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
    (void)im;
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
        if (!read_value(L, 2, p, ms, v)) return false;
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
        if (!read_value(L, 2, t.prop, ms, v)) return false;
        if (t.prop == Prop::Morph) {
            if (v.target == ms.rec->parts[t.part]) v = {};
            if (!check_morph(im, ms, ms.rec->parts[t.part].c_str(), v)) return false;
        }
        st.props.emplace_back(t.prop, std::move(v));
        if (!lua_isnoneornil(L, 5)) return call_error(ms, "дуга — у x и y вместе: part(\"body\"):to({ x = …, y = … }, с, \"smooth\", \"arc\")");
    } else {
        st.value_field = true;
        if (!read_table(L, 2, ms, ":to", st) || !read_path(L, 5, ms, st)) return false;
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
    return index_with(L, "части", "свойства: x, y, rot, scale, sx, sy, opacity, fill; методы: to, rest, save, restore, morph, spring, set");
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

void register_motion_api(lua_State* L) {
    methods_meta(L, kPartMeta, &api_part_index,
                 {{"to", &api_part_to}, {"rest", &api_part_rest}, {"save", &api_part_save}, {"restore", &api_part_restore},
                  {"morph", &api_part_morph}, {"spring", &api_part_spring}, {"set", &api_part_set}});
    methods_meta(L, kPropMeta, &api_prop_index, {{"rest", &api_prop_rest}, {"spring", &api_prop_spring}}, &api_prop_call);
    methods_meta(L, kTaskMeta, &api_task_index, {{"to", &api_task_to}, {"back", &api_task_back}, {"wait", &api_task_wait}});
    const std::pair<const char*, lua_CFunction> globals[] = {
        {"part", &api_part},       {"parts", &api_parts},           {"all", &api_all},
        {"any", &api_any},         {"chain", &api_chain},           {"sequence", &api_sequence},
        {"delay", &api_delay},     {"at", &api_at},                 {"wait", &api_wait},
        {"wait_until", &api_wait_until}, {"duration", &api_duration}, {"now", &api_now},
        {"loop", &api_loop},       {"loop_for", &api_loop_for},     {"loop_until", &api_loop_until},
        {"run", &api_run},         {"spawn", &api_spawn},           {"join", &api_join},
        {"tween", &api_tween},     {"spring", &api_spring},         {"emit", &api_emit},
        {"sound", &api_sound},     {"log", &api_log},
    };
    for (const auto& [name, fn] : globals) {
        lua_pushcfunction(L, fn, name);
        lua_setglobal(L, name);
    }
}

bool record_clip(Impl& im, lua_State* T, const Rig* rig, std::string_view clip, const std::map<std::string, f64>& values,
                 u64 seed, const MotionLengths& lengths, Recording& out) {
    lua_getfield(T, -1, "clips");
    lua_pushlstring(T, clip.data(), clip.size());
    lua_gettable(T, -2);
    lua_remove(T, -2);
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
    im.rng = frame_seed(seed, 0, clip);
    const u64 budget = im.budget_ns;
    im.budget_ns = budget * 10;
    im.begin_call();
    im.active = T;
    bool ok = lua_pcall(T, 2, 0, 0) == LUA_OK;
    if (!ok) {
        im.fail(lua_tostring(T, -1));
        lua_pop(T, 1);
    }
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
    for (const ScriptFile& f : im.files) out.files.push_back(f.name);
    for (int r : ms.refs) lua_unref(im.L, r);
    if (ok) {
        auto& slot = im.bindings[std::string(clip)];
        if (slot.first != LUA_NOREF && slot.second != 0) lua_unref(im.L, slot.first);
        slot = {ms.bindings, out.generation};
    } else {
        lua_unref(im.L, ms.bindings);
    }
    im.in_call = false;
    im.mode = Mode::None;
    im.rig = nullptr;
    im.budget_ns = budget;
    im.motion.reset();
    if (!ok) out = {};
    return ok;
}

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
    lua_getref(T, table);
    lua_rawgeti(T, -1, binding + 1);
    lua_remove(T, -2);
    const Mode was_mode = mode;
    const Recording* was_rec = frame_rec;
    const f64 was_t = frame_t;
    const i32 was_site = site;
    mode = Mode::Frame;
    frame_rec = &rec;
    frame_t = t;
    ++binding_depth;
    const int status = lua_pcall(T, 0, 1, 0);
    --binding_depth;
    mode = was_mode;
    frame_rec = was_rec;
    frame_t = was_t;
    if (status != LUA_OK) {
        const char* m = lua_tostring(T, -1);
        why = m ? m : "ошибка без текста";
        lua_pop(T, 1);
        return false;
    }
    site = was_site;
    out = {};
    bool good = true;
    if (prop_is_number(prop)) {
        good = lua_type(T, -1) == LUA_TNUMBER && std::isfinite(lua_tonumber(T, -1));
        if (good) out.v = lua_tonumber(T, -1);
        else why = std::string("связь ") + prop_word(prop) + " вернула не число";
    } else if (prop == Prop::Fill && !lua_isnil(T, -1)) {
        const char* text = lua_tostring(T, -1);
        good = text && parse_color(text, out.color);
        if (good) out.v = 1;
        else why = "связь fill вернула не цвет \"#rrggbb\"";
    }
    lua_pop(T, 1);
    return good;
}

} // namespace forge::pixel
