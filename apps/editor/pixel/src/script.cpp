#include "forge/pixel/script.h"

#include "forge/core/time.h"
#include "forge/vector/ease.h"

#include <lua.h>
#include <luacode.h>
#include <lualib.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace forge::pixel {

namespace {

u64 mix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

} // namespace

struct PoseScript::Impl {
    lua_State* L = nullptr;
    std::string bytecode;
    std::vector<ScriptParam> params;
    std::string error;
    u32 error_line = 0;
    bool ok = false;

    // The call running now.
    const Rig* rig = nullptr;
    Pose* pose = nullptr;
    u64 rng = 1;
    u64 budget_ns = 100'000'000;
    u64 start_ns = 0;
    u32 interrupts = 0;
    bool in_call = false, timed_out = false;

    Impl();
    ~Impl() {
        if (L) lua_close(L);
    }

    // "pose:12: message" → line 12 and "строка 12: message".
    void fail(const char* raw) {
        std::string msg = raw ? raw : "ошибка без текста";
        error_line = 0;
        for (usize i = msg.find(':'); i != std::string::npos; i = msg.find(':', i + 1)) {
            usize j = i + 1;
            while (j < msg.size() && msg[j] >= '0' && msg[j] <= '9') ++j;
            if (j > i + 1 && j < msg.size() && msg[j] == ':') {
                error_line = static_cast<u32>(std::strtoul(msg.c_str() + i + 1, nullptr, 10));
                msg = msg.substr(j + 1);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
                break;
            }
        }
        error = error_line ? "строка " + std::to_string(error_line) + ": " + msg : msg;
    }

    void begin_call() {
        in_call = true;
        timed_out = false;
        start_ns = 0;
        interrupts = 0;
    }

    // A fresh thread with the module run in it, its table on top. Null (error set, thread popped) when it fails.
    lua_State* module() {
        lua_State* T = lua_newthread(L);
        luaL_sandboxthread(T);
        if (luau_load(T, "=pose", bytecode.data(), bytecode.size(), 0) != 0) {
            fail(lua_tostring(T, -1));
            lua_pop(L, 1);
            return nullptr;
        }
        begin_call();
        const int status = lua_pcall(T, 0, 1, 0);
        in_call = false;
        if (status != LUA_OK) {
            fail(lua_tostring(T, -1));
            lua_pop(L, 1);
            return nullptr;
        }
        if (!lua_istable(T, -1)) {
            fail("скрипт позы должен вернуть таблицу: return { params = {…}, pose = function(t, p, rng, clip) … end }");
            lua_pop(L, 1);
            return nullptr;
        }
        return T;
    }
};

namespace {

PoseScript::Impl& impl_of(lua_State* L) { return *static_cast<PoseScript::Impl*>(lua_callbacks(L)->userdata); }

void on_interrupt(lua_State* L, int gc) {
    if (gc >= 0) return;
    PoseScript::Impl& im = impl_of(L);
    if (!im.in_call) return;
    if (!im.timed_out) {
        if ((++im.interrupts & 255) != 0) return;
        const u64 now = time_now_ns();
        if (im.start_ns == 0) im.start_ns = now;
        if (now - im.start_ns <= im.budget_ns) return;
        im.timed_out = true;
    }
    lua_Debug ar{};
    const bool where = lua_getinfo(L, 0, "sl", &ar) && ar.currentline > 0;
    lua_pushfstring(L, "%s:%d: скрипт позы считал дольше %.0f мс (бесконечный цикл?)", where ? ar.short_src : "?",
                    where ? ar.currentline : 0, static_cast<f64>(im.budget_ns) / 1e6);
    lua_error(L);
}

// The API raises errors with luaL_error and no object with a destructor alive in the frame: an error leaves the frame
// without running them (the editor has no exceptions).
const char* const kPartFields = "x, y, rot, scale, sx, sy, opacity, fill";

// part("id") → a handle whose :set{…} adds to the part's pose.
int api_part(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    PoseScript::Impl& im = impl_of(L);
    if (im.rig && im.rig->part(id) < 0) luaL_error(L, "в SVG нет части «%s»", id);
    lua_newtable(L);
    lua_pushstring(L, id);
    lua_setfield(L, -2, "id");
    luaL_getmetatable(L, "forge.pixel.part");
    lua_setmetatable(L, -2);
    return 1;
}

int api_part_set(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 1, "id");
    const char* id = lua_tostring(L, -1);
    if (!id) luaL_error(L, "part(…):set — вызывайте через двоеточие: part(\"head\"):set{ rot = 10 }");
    lua_pop(L, 1);
    PoseScript::Impl& im = impl_of(L);
    if (!im.pose) luaL_error(L, "part(…):set работает только внутри pose");
    PartPose& pp = im.pose->parts[id];
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
        if (!key) luaL_error(L, "part(\"%s\"):set — поля по именам: %s", id, kPartFields);
        if (std::strcmp(key, "fill") == 0) {
            const char* text = lua_tostring(L, -1);
            Rgba8 c;
            if (!text || !parse_color(text, c)) luaL_error(L, "part(\"%s\"):set — fill: нужен цвет \"#rrggbb\"", id);
            pp.has_fill = true;
            pp.fill = c;
        } else {
            f64* field = std::strcmp(key, "x") == 0         ? &pp.x
                         : std::strcmp(key, "y") == 0       ? &pp.y
                         : std::strcmp(key, "rot") == 0     ? &pp.rot
                         : std::strcmp(key, "scale") == 0   ? &pp.scale
                         : std::strcmp(key, "sx") == 0      ? &pp.sx
                         : std::strcmp(key, "sy") == 0      ? &pp.sy
                         : std::strcmp(key, "opacity") == 0 ? &pp.opacity
                                                            : nullptr;
            if (!field) luaL_error(L, "part(\"%s\"):set — поля «%s» нет (есть %s)", id, key, kPartFields);
            if (!lua_isnumber(L, -1)) luaL_error(L, "part(\"%s\"):set — %s: нужно число", id, key);
            const f64 v = lua_tonumber(L, -1);
            if (!std::isfinite(v)) luaL_error(L, "part(\"%s\"):set — %s: не число (%f)", id, key, v);
            *field = v;
        }
        lua_pop(L, 1);
    }
    return 0;
}

// shape("id") → a handle whose :morph(target, k) morphs it.
int api_shape(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    PoseScript::Impl& im = impl_of(L);
    if (im.rig) {
        const i32 p = im.rig->part(id);
        if (p < 0) luaL_error(L, "в SVG нет фигуры «%s»", id);
        if (!im.rig->parts()[static_cast<usize>(p)].shape) luaL_error(L, "«%s» — группа, морфинг бывает только у фигуры", id);
    }
    lua_newtable(L);
    lua_pushstring(L, id);
    lua_setfield(L, -2, "id");
    luaL_getmetatable(L, "forge.pixel.shape");
    lua_setmetatable(L, -2);
    return 1;
}

int api_shape_morph(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "id");
    const char* id = lua_tostring(L, -1);
    if (!id) luaL_error(L, "shape(…):morph — вызывайте через двоеточие: shape(\"mouth\"):morph(\"mouth_open\", k)");
    lua_pop(L, 1);
    const char* target = luaL_checkstring(L, 2);
    const f64 k = luaL_checknumber(L, 3);
    if (!std::isfinite(k)) luaL_error(L, "shape(\"%s\"):morph — k не число", id);
    PoseScript::Impl& im = impl_of(L);
    if (!im.pose) luaL_error(L, "shape(…):morph работает только внутри pose");
    if (im.rig && im.rig->outline().find(target) < 0) luaL_error(L, "в SVG нет фигуры «%s»", target);
    ShapeMorph& m = im.pose->morphs[id];
    m.target = target;
    m.k = k;
    return 0;
}

int api_ease_named(lua_State* L) {
    const auto e = static_cast<vector::Ease>(lua_tointeger(L, lua_upvalueindex(1)));
    lua_pushnumber(L, vector::ease(e, luaL_checknumber(L, 1)));
    return 1;
}

// ease(word, t), the table called: the words of the «Интерфейс» tab ("in" is a word Lua keeps for itself).
int api_ease_call(lua_State* L) {
    const char* word = luaL_checkstring(L, 2);
    vector::Ease e;
    if (!vector::ease_from_word(word, e)) luaL_error(L, "ease: нет слова «%s» (smooth, linear, in, out, back, bounce, elastic)", word);
    lua_pushnumber(L, vector::ease(e, luaL_checknumber(L, 3)));
    return 1;
}

// rng() 0 … 1, rng(n) 1 … n, rng(a, b) a … b: the frame's own generator.
int api_rng(lua_State* L) {
    PoseScript::Impl& im = impl_of(L);
    im.rng = mix64(im.rng);
    const f64 u = static_cast<f64>(im.rng >> 11) * (1.0 / 9007199254740992.0);
    const int n = lua_gettop(L);
    if (n == 0) {
        lua_pushnumber(L, u);
        return 1;
    }
    f64 lo = 1, hi = luaL_checknumber(L, 1);
    if (n >= 2) lo = hi, hi = luaL_checknumber(L, 2);
    lo = std::ceil(lo), hi = std::floor(hi);
    if (!(lo <= hi)) luaL_error(L, "rng: пустой промежуток");
    lua_pushnumber(L, lo + std::floor(u * (hi - lo + 1)));
    return 1;
}

} // namespace

PoseScript::Impl::Impl() {
    L = luaL_newstate();
    lua_callbacks(L)->userdata = this;
    lua_callbacks(L)->interrupt = &on_interrupt;
    luaL_openlibs(L);
    // No clock and no chance but the frame's rng: the pose is a function of time.
    lua_pushnil(L);
    lua_setglobal(L, "os");
    lua_getglobal(L, "math");
    lua_pushnil(L);
    lua_setfield(L, -2, "random");
    lua_pushnil(L);
    lua_setfield(L, -2, "randomseed");
    lua_pop(L, 1);

    luaL_newmetatable(L, "forge.pixel.part");
    lua_newtable(L);
    lua_pushcfunction(L, &api_part_set, "set");
    lua_setfield(L, -2, "set");
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "__index");
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
    luaL_newmetatable(L, "forge.pixel.shape");
    lua_newtable(L);
    lua_pushcfunction(L, &api_shape_morph, "morph");
    lua_setfield(L, -2, "morph");
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "__index");
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);

    lua_pushcfunction(L, &api_part, "part");
    lua_setglobal(L, "part");
    lua_pushcfunction(L, &api_shape, "shape");
    lua_setglobal(L, "shape");
    lua_newtable(L); // ease
    for (vector::Ease e : vector::kEases) {
        lua_pushinteger(L, static_cast<int>(e));
        lua_pushcclosure(L, &api_ease_named, vector::ease_word(e), 1);
        lua_setfield(L, -2, vector::ease_word(e));
    }
    lua_newtable(L);
    lua_pushcfunction(L, &api_ease_call, "ease");
    lua_setfield(L, -2, "__call");
    lua_setreadonly(L, -1, 1);
    lua_setmetatable(L, -2);
    lua_setglobal(L, "ease");
    luaL_sandbox(L);
}

PoseScript::PoseScript() : impl_(std::make_unique<Impl>()) {}
PoseScript::~PoseScript() = default;
PoseScript::PoseScript(PoseScript&&) noexcept = default;
PoseScript& PoseScript::operator=(PoseScript&&) noexcept = default;

bool PoseScript::load(std::string_view source) {
    Impl& im = *impl_;
    im.ok = false;
    im.params.clear();
    im.error.clear();
    im.error_line = 0;
    lua_CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    size_t size = 0;
    char* code = luau_compile(source.data(), source.size(), &opts, &size);
    im.bytecode.assign(code, size);
    std::free(code);

    lua_State* T = im.module();
    if (!T) return false;
    lua_getfield(T, -1, "pose");
    const bool has_pose = lua_isfunction(T, -1);
    lua_pop(T, 1);
    if (!has_pose) {
        im.fail("в таблице скрипта нет pose = function(t, p, rng, clip) … end");
        lua_pop(im.L, 1);
        return false;
    }
    lua_getfield(T, -1, "params");
    if (lua_istable(T, -1)) {
        lua_pushnil(T);
        while (lua_next(T, -2)) {
            if (lua_type(T, -2) != LUA_TSTRING || !lua_isnumber(T, -1)) {
                const std::string name = lua_type(T, -2) == LUA_TSTRING ? lua_tostring(T, -2) : "?";
                im.fail(("params." + name + ": параметр — число (скорость, длина шага…)").c_str());
                lua_pop(im.L, 1);
                return false;
            }
            im.params.push_back({lua_tostring(T, -2), lua_tonumber(T, -1)});
            lua_pop(T, 1);
        }
    } else if (!lua_isnil(T, -1)) {
        im.fail("params должен быть таблицей: params = { speed = 1 }");
        lua_pop(im.L, 1);
        return false;
    }
    std::sort(im.params.begin(), im.params.end(), [](const ScriptParam& a, const ScriptParam& b) { return a.name < b.name; });
    lua_pop(im.L, 1);
    im.ok = true;
    return true;
}

bool PoseScript::loaded() const { return impl_->ok; }
const std::vector<ScriptParam>& PoseScript::params() const { return impl_->params; }
const std::string& PoseScript::error() const { return impl_->error; }
u32 PoseScript::error_line() const { return impl_->error_line; }
void PoseScript::set_budget_ms(f64 ms) { impl_->budget_ns = static_cast<u64>(std::max(1.0, ms) * 1e6); }

bool PoseScript::pose(const Rig& rig, f64 t, std::string_view clip, const std::map<std::string, f64>& values, u64 seed, Pose& out) {
    Impl& im = *impl_;
    out = {};
    im.error.clear();
    im.error_line = 0;
    if (!im.ok) {
        im.error = "скрипт позы не загружен";
        return false;
    }
    lua_State* T = im.module();
    if (!T) return false;
    lua_getfield(T, -1, "pose");
    lua_pushnumber(T, t);
    lua_newtable(T);
    for (const ScriptParam& p : im.params) {
        const auto it = values.find(p.name);
        lua_pushnumber(T, it != values.end() ? it->second : p.value);
        lua_setfield(T, -2, p.name.c_str());
    }
    lua_pushcfunction(T, &api_rng, "rng");
    lua_pushlstring(T, clip.data(), clip.size());
    // The frame's chance: from the seed, the time and the clip, the same on every run.
    u64 h = mix64(seed ^ mix64(std::bit_cast<u64>(t)));
    for (char c : clip) h = mix64(h ^ static_cast<u8>(c));
    im.rng = h;
    im.rig = &rig;
    im.pose = &out;
    im.begin_call();
    const int status = lua_pcall(T, 4, 0, 0);
    im.in_call = false;
    im.rig = nullptr;
    im.pose = nullptr;
    if (status != LUA_OK) {
        im.fail(lua_tostring(T, -1));
        out = {};
    }
    lua_pop(im.L, 1);
    return status == LUA_OK;
}

} // namespace forge::pixel
