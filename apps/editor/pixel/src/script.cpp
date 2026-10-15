#include "forge/pixel/script.h"

#include "script_impl.h"

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/vector/ease.h"

#include <lua.h>
#include <luacode.h>
#include <lualib.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
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

// Every call of the code goes through these (code_sites.h): the call's id is noted, then it is made. In Luau, so a
// deep call does not nest C calls.
constexpr const char* kPrelude = R"(local site, method = ...
return function(id, f, ...) site(id, f) return f(...) end,
  function(id, o, m, ...) return method(id, o, m)(o, ...) end)";
constexpr const char* kPreludeChunk = "=__forge";
constexpr const char* kMainChunk = "=pose";

} // namespace

PoseScript::Impl& impl_of(lua_State* L) { return *static_cast<PoseScript::Impl*>(lua_callbacks(L)->userdata); }

const char* value_text(lua_State* L, int idx, size_t* len) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TTABLE:
    case LUA_TFUNCTION:
    case LUA_TUSERDATA:
    case LUA_TLIGHTUSERDATA:
    case LUA_TTHREAD:
    case LUA_TBUFFER:
        if (!luaL_getmetafield(L, idx, "__tostring")) {
            lua_pushstring(L, luaL_typename(L, idx));
            return lua_tolstring(L, -1, len);
        }
        lua_pop(L, 1);
        break;
    default:
        break;
    }
    return luaL_tolstring(L, idx, len);
}

namespace {

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
    // The author's line, not the prelude's.
    lua_Debug ar{};
    bool where = false;
    for (int level = 0; lua_getinfo(L, level, "sl", &ar); ++level)
        if (ar.currentline > 0 && std::strcmp(ar.short_src, "__forge") != 0) {
            where = true;
            break;
        }
    lua_pushfstring(L, "%s:%d: скрипт позы считал дольше %.0f мс (бесконечный цикл?)", where ? ar.short_src : "?",
                    where ? ar.currentline : 0, static_cast<f64>(im.budget_ns) / 1e6);
    lua_error(L);
}

bool callable(lua_State* L, int idx) {
    if (lua_isfunction(L, idx)) return true;
    if (!luaL_getmetafield(L, idx, "__call")) return false;
    lua_pop(L, 1);
    return true;
}

// The text of a call's callee, for messages: "body.z", "o:m".
void push_callee(lua_State* L, const PoseScript::Impl& im, i32 id) {
    const SiteRef s = im.site_ref(id);
    const CallSite* c = im.call(s);
    if (!c) {
        lua_pushstring(L, "?");
        return;
    }
    const std::string& text = im.files[s.file].text;
    usize end = c->callee_end;
    if (c->method) end = c->parens ? c->open : c->args.front().span.begin;
    while (end > c->span.begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n')) --end;
    const usize n = std::min<usize>(end - c->span.begin, 60);
    lua_pushlstring(L, text.data() + c->span.begin, n);
}

// site(id, f): the call `id` runs now; f must be callable.
int api_site(lua_State* L) {
    PoseScript::Impl& im = impl_of(L);
    im.site = static_cast<i32>(lua_tointeger(L, 1));
    if (!callable(L, 2)) {
        push_callee(L, im, im.site);
        luaL_error(L, "%s — не функция (%s)", lua_tostring(L, -1), luaL_typename(L, 2));
    }
    return 0;
}

// method(id, o, m): o's method m for the call `id`.
int api_method(lua_State* L) {
    PoseScript::Impl& im = impl_of(L);
    im.site = static_cast<i32>(lua_tointeger(L, 1));
    const int t = lua_type(L, 2);
    if (t != LUA_TTABLE && t != LUA_TUSERDATA && t != LUA_TSTRING && t != LUA_TVECTOR) {
        push_callee(L, im, im.site);
        luaL_error(L, "%s — у %s нет методов", lua_tostring(L, -1), luaL_typename(L, 2));
    }
    lua_pushvalue(L, 3);
    lua_gettable(L, 2);
    if (!callable(L, -1)) {
        push_callee(L, im, im.site);
        luaL_error(L, "%s — нет такого метода", lua_tostring(L, -1));
    }
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
    if (!im.pose) luaL_error(L, "shape(…):morph работает только внутри pose (в клипе — part(…):morph(цель, с))");
    if (im.rig && im.rig->outline().find(target) < 0) luaL_error(L, "в рисунке нет фигуры «%s»", target);
    ShapeMorph& m = im.pose->morphs[id];
    m.target = target;
    m.k = k;
    return 0;
}

// shape("id") → a handle whose :morph(target, k) morphs it.
int api_shape(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    PoseScript::Impl& im = impl_of(L);
    if (im.rig) {
        const i32 p = im.rig->part(id);
        if (p < 0) luaL_error(L, "в рисунке нет фигуры «%s»", id);
        if (!im.rig->parts()[static_cast<usize>(p)].shape) luaL_error(L, "«%s» — группа, морфинг бывает только у фигуры", id);
    }
    lua_newtable(L);
    lua_pushstring(L, id);
    lua_setfield(L, -2, "id");
    luaL_getmetatable(L, "forge.pixel.shape");
    lua_setmetatable(L, -2);
    return 1;
}

int api_ease_named(lua_State* L) {
    const auto e = static_cast<vector::Ease>(lua_tointeger(L, lua_upvalueindex(1)));
    lua_pushnumber(L, vector::ease(e, luaL_checknumber(L, 1)));
    return 1;
}

// ease(word, t), the table called: the words of the «Интерфейс» tab and Motion Canvas's ("in" is a word Lua keeps for
// itself).
int api_ease_call(lua_State* L) {
    const char* word = luaL_checkstring(L, 2);
    vector::Timing t;
    if (!vector::timing_from_word(word, t)) luaL_error(L, "ease: нет слова «%s» (smooth, linear, in, out, back, bounce, elastic, in_sine … in_out_bounce)", word);
    lua_pushnumber(L, vector::timing(t, luaL_checknumber(L, 3)));
    return 1;
}

int api_tostring(lua_State* L) {
    luaL_checkany(L, 1);
    value_text(L, 1, nullptr);
    return 1;
}

// string.format: what %* writes is value_text's; the rest is Luau's own (upvalue 1), %s still a string or a number.
int api_format(lua_State* L) {
    size_t n = 0;
    const char* p = luaL_checklstring(L, 1, &n);
    const char* end = p + n;
    const int top = lua_gettop(L);
    int arg = 1;
    auto digits = [&] {
        while (p < end && *p >= '0' && *p <= '9') ++p;
    };
    while (p < end) {
        if (*p++ != '%') continue;
        if (p < end && *p == '%') {
            ++p;
            continue;
        }
        ++arg;
        if (p < end && *p == '*') {
            ++p;
            if (arg <= top) {
                value_text(L, arg, nullptr);
                lua_replace(L, arg);
            }
            continue;
        }
        while (p < end && (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0')) ++p;
        digits();
        if (p < end && *p == '.') {
            ++p;
            digits();
        }
        if (p < end) ++p;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, top, 1);
    return 1;
}

// rng() 0 … 1, rng(n) 1 … n, rng(a, b) a … b: the frame's or the recording's own generator.
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

// require("name"): animations/name.luau of the project, run once per recording or frame.
bool load_module(PoseScript::Impl& im, std::string_view name, u32& file) {
    std::string text, why;
    if (!im.reader) return im.error = "require: модули проекта здесь не читаются", false;
    if (!im.reader(name, text, why)) return im.error = "require(\"" + std::string(name) + "\"): " + why, false;
    for (u32 i = 1; i < im.files.size(); ++i)
        if (im.files[i].name == name && im.files[i].text == text) {
            file = i;
            return true;
        }
    ScriptFile f;
    f.name = name;
    f.text = std::move(text);
    if (!im.compile(f)) return false;
    file = static_cast<u32>(im.files.size());
    im.files.push_back(std::move(f));
    return true;
}

int api_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    PoseScript::Impl& im = impl_of(L);
    // A module lives as long as the run it was loaded in, longer than a frame and shorter than a recording's bindings:
    // a binding takes it from the code around it, as an upvalue its guard looks after.
    if (im.binding_depth > 0)
        luaL_error(L, "require(\"%s\") внутри связи: модуль живёт дольше кадра, возьмите его в начале кода или клипа "
                      "(local m = require(\"%s\")) и зовите из связи", name, name);
    for (const char* c = name; *c; ++c)
        if (*c == '/' || *c == '\\' || *c == ':' || *c == '.' || static_cast<u8>(*c) < 0x20)
            luaL_error(L, "require(\"%s\"): имя модуля — без папок и точек, как «движения»", name);
    if (!*name) luaL_error(L, "require: нужно имя модуля");
    lua_getref(L, im.modules);
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    u32 file = 0;
    if (!load_module(im, name, file)) {
        lua_pushlstring(L, im.error.data(), im.error.size());
        lua_error(L);
    }
    const ScriptFile& f = im.files[file];
    lua_pushfstring(L, "=%s.luau", name);
    if (luau_load(L, lua_tostring(L, -1), f.bytecode.data(), f.bytecode.size(), 0) != 0) lua_error(L);
    lua_remove(L, -2);
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
    return 1;
}

} // namespace

// part(…):set{…}: adds to the part's pose inside pose().
int api_part_set(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 1, "id");
    const char* id = lua_tostring(L, -1);
    if (!id) luaL_error(L, "part(…):set — вызывайте через двоеточие: part(\"head\"):set{ rot = 10 }");
    lua_pop(L, 1);
    PoseScript::Impl& im = impl_of(L);
    if (!im.pose) luaL_error(L, "part(…):set работает только внутри pose (в клипе — part(…):to{…} или part(…).x(…))");
    // The table entry for the part is made before any error: errors leave without destructors.
    PartPose& pp = im.pose->parts[id];
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
        if (!key) luaL_error(L, "part(\"%s\"):set — поля по именам: x, y, rot, scale, sx, sy, opacity, fill", id);
        if (std::strcmp(key, "fill") == 0) {
            const char* text = lua_tostring(L, -1);
            Rgba8 c;
            if (!text || !parse_color(text, c)) luaL_error(L, "part(\"%s\"):set — fill: нужен цвет \"#rrggbb\"", id);
            pp.has_fill = true;
            pp.fill = c;
            pp.fill_k = 1;
        } else {
            f64* field = std::strcmp(key, "x") == 0         ? &pp.x
                         : std::strcmp(key, "y") == 0       ? &pp.y
                         : std::strcmp(key, "rot") == 0     ? &pp.rot
                         : std::strcmp(key, "scale") == 0   ? &pp.scale
                         : std::strcmp(key, "sx") == 0      ? &pp.sx
                         : std::strcmp(key, "sy") == 0      ? &pp.sy
                         : std::strcmp(key, "opacity") == 0 ? &pp.opacity
                                                            : nullptr;
            if (!field) luaL_error(L, "part(\"%s\"):set — поля «%s» нет (есть x, y, rot, scale, sx, sy, opacity, fill)", id, key);
            if (!lua_isnumber(L, -1)) luaL_error(L, "part(\"%s\"):set — %s: нужно число", id, key);
            const f64 v = lua_tonumber(L, -1);
            if (!std::isfinite(v)) luaL_error(L, "part(\"%s\"):set — %s: не число (%f)", id, key, v);
            *field = v;
        }
        lua_pop(L, 1);
    }
    return 0;
}

u64 frame_seed(u64 seed, f64 t, std::string_view clip) {
    u64 h = mix64(seed ^ mix64(std::bit_cast<u64>(t)));
    for (char c : clip) h = mix64(h ^ static_cast<u8>(c));
    return h;
}

void push_params(PoseScript::Impl& im, lua_State* T, const std::map<std::string, f64>& values) {
    lua_newtable(T);
    for (const ScriptParam& p : im.params) {
        const auto it = values.find(p.name);
        lua_pushnumber(T, it != values.end() ? it->second : p.value);
        lua_setfield(T, -2, p.name.c_str());
    }
}

PoseScript::Impl::Impl() {
    L = luaL_newstate();
    lua_callbacks(L)->userdata = this;
    lua_callbacks(L)->interrupt = &on_interrupt;
    luaL_openlibs(L);
    // No clock and no chance but the frame's rng: the pose is a function of time. No heap size (gcinfo) either, and no
    // getfenv and setfenv, through which a function would change its own environment or reach its thread's.
    for (const char* name : {"os", "gcinfo", "getfenv", "setfenv"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    // Nor an address: tostring and string.format's %* tell a table or a function by its kind.
    lua_pushcfunction(L, &api_tostring, "tostring");
    lua_setglobal(L, "tostring");
    lua_getglobal(L, "string");
    lua_getfield(L, -1, "format");
    lua_pushcclosure(L, &api_format, "format", 1);
    lua_setfield(L, -2, "format");
    lua_pop(L, 1);
    lua_getglobal(L, "math");
    lua_pushnil(L);
    lua_setfield(L, -2, "random");
    lua_pushnil(L);
    lua_setfield(L, -2, "randomseed");
    lua_pop(L, 1);

    luaL_newmetatable(L, "forge.pixel.shape");
    lua_newtable(L);
    lua_pushcfunction(L, &api_shape_morph, "morph");
    lua_setfield(L, -2, "morph");
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "__index");
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);

    lua_pushcfunction(L, &api_shape, "shape");
    lua_setglobal(L, "shape");
    lua_pushcfunction(L, &api_rng, "rng");
    lua_setglobal(L, "rng");
    lua_pushcfunction(L, &api_require, "require");
    lua_setglobal(L, "require");
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
    register_motion_api(L);
    register_draw_api(L);

    // __fc and __fm, through which the instrumented code calls.
    lua_CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    size_t size = 0;
    char* code = luau_compile(kPrelude, std::strlen(kPrelude), &opts, &size);
    const int loaded = luau_load(L, kPreludeChunk, code, size, 0);
    std::free(code);
    if (loaded == 0) {
        lua_pushcfunction(L, &api_site, "site");
        lua_pushcfunction(L, &api_method, "method");
        if (lua_pcall(L, 2, 2, 0) == 0) {
            lua_setglobal(L, "__fm");
            lua_setglobal(L, "__fc");
        } else {
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    luaL_sandbox(L);
}

PoseScript::Impl::~Impl() {
    if (L) lua_close(L);
}

SiteRef PoseScript::Impl::site_ref(i32 id) const {
    if (id < 0) return {};
    for (u32 f = 0; f < files.size(); ++f) {
        const ScriptFile& file = files[f];
        if (static_cast<u32>(id) >= file.base && static_cast<u32>(id) < file.base + file.code.sites().size())
            return {f, static_cast<i32>(static_cast<u32>(id) - file.base)};
    }
    return {};
}

const CallSite* PoseScript::Impl::call(SiteRef s) const {
    if (s.site < 0 || s.file >= files.size()) return nullptr;
    const auto& sites = files[s.file].code.sites();
    return static_cast<usize>(s.site) < sites.size() ? &sites[static_cast<usize>(s.site)] : nullptr;
}

std::string PoseScript::Impl::where(SiteRef s) const {
    const CallSite* c = call(s);
    if (!c) return {};
    const std::string line = "строка " + std::to_string(c->line);
    return s.file == 0 ? line : files[s.file].name + ".luau, " + line;
}

// "pose:12: message" → line 12 and "строка 12: message"; "moves.luau:3: …" → "moves.luau, строка 3: …"; an error the
// prelude or a C function raised has no line of its own: the call running then gives it.
void PoseScript::Impl::fail(const char* raw) {
    std::string msg = raw ? raw : "ошибка без текста";
    error_line = 0;
    std::string chunk;
    u32 line = 0;
    for (usize i = msg.find(':'); i != std::string::npos; i = msg.find(':', i + 1)) {
        usize j = i + 1;
        while (j < msg.size() && msg[j] >= '0' && msg[j] <= '9') ++j;
        if (j > i + 1 && j < msg.size() && msg[j] == ':') {
            chunk = msg.substr(0, i);
            line = static_cast<u32>(std::strtoul(msg.c_str() + i + 1, nullptr, 10));
            msg = msg.substr(j + 1);
            if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
            break;
        }
    }
    if (chunk.empty() || chunk == "__forge") {
        const SiteRef s = site_ref(site);
        const CallSite* c = call(s);
        if (c) {
            error = where(s) + ": " + msg;
            if (s.file == 0) error_line = c->line;
            return;
        }
        if (chunk.empty()) {
            error = msg;
            return;
        }
        line = 0;
    }
    if (chunk == "pose") {
        error_line = line;
        error = line ? "строка " + std::to_string(line) + ": " + msg : msg;
    } else if (line) {
        error = chunk + ", строка " + std::to_string(line) + ": " + msg;
    } else {
        error = msg;
    }
}

void PoseScript::Impl::begin_call() {
    in_call = true;
    timed_out = false;
    start_ns = 0;
    interrupts = 0;
    site = -1;
}

bool PoseScript::Impl::compile(ScriptFile& f) {
    std::string why;
    u32 line = 0;
    if (!f.code.parse(f.text, why, line)) {
        error_line = f.name.empty() ? line : 0;
        error = (f.name.empty() ? "" : f.name + ".luau, ") + "строка " + std::to_string(line) + ": " + why;
        return false;
    }
    f.base = next_base;
    next_base += static_cast<u32>(f.code.sites().size());
    const std::string text = f.code.instrumented(f.base);
    lua_CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    size_t size = 0;
    char* code = luau_compile(text.data(), text.size(), &opts, &size);
    f.bytecode.assign(code, size);
    std::free(code);
    return true;
}

lua_State* PoseScript::Impl::module() {
    lua_State* T = lua_newthread(L);
    luaL_sandboxthread(T);
    if (luau_load(T, kMainChunk, files[0].bytecode.data(), files[0].bytecode.size(), 0) != 0) {
        fail(lua_tostring(T, -1));
        lua_pop(L, 1);
        return nullptr;
    }
    if (modules != LUA_NOREF) lua_unref(L, modules);
    lua_newtable(L);
    modules = lua_ref(L, -1);
    lua_pop(L, 1);
    const Mode was = mode;
    mode = Mode::Load;
    begin_call();
    active = T;
    const int status = lua_pcall(T, 0, 1, 0);
    in_call = false;
    mode = was;
    if (status != LUA_OK) {
        fail(lua_tostring(T, -1));
        lua_pop(L, 1);
        return nullptr;
    }
    if (!lua_istable(T, -1)) {
        error = "скрипт должен вернуть таблицу: return { params = {…}, clips = { walk = function(p) … end } }";
        error_line = 0;
        lua_pop(L, 1);
        return nullptr;
    }
    return T;
}

PoseScript::PoseScript() : impl_(std::make_unique<Impl>()) {}
PoseScript::~PoseScript() = default;
PoseScript::PoseScript(PoseScript&&) noexcept = default;
PoseScript& PoseScript::operator=(PoseScript&&) noexcept = default;

bool PoseScript::load(std::string_view source) {
    Impl& im = *impl_;
    im.ok = false;
    im.params.clear();
    im.clips.clear();
    im.has_pose = false;
    im.has_draw = false;
    im.error.clear();
    im.error_line = 0;
    im.cache.clear();
    for (auto& [clip, b] : im.bindings) lua_unref(im.L, b.first);
    im.bindings.clear();
    im.files.clear();
    im.files.push_back({});
    im.files[0].text.assign(source);
    if (!im.compile(im.files[0])) return false;

    lua_State* T = im.module();
    if (!T) return false;
    lua_getfield(T, -1, "pose");
    im.has_pose = lua_isfunction(T, -1);
    const bool bad_pose = !lua_isnil(T, -1) && !im.has_pose;
    lua_pop(T, 1);
    if (bad_pose) {
        im.fail("pose должна быть функцией: pose = function(t, p, rng, clip) … end");
        lua_pop(im.L, 1);
        return false;
    }
    lua_getfield(T, -1, "draw");
    im.has_draw = lua_isfunction(T, -1);
    const bool bad_draw = !lua_isnil(T, -1) && !im.has_draw;
    lua_pop(T, 1);
    if (bad_draw) {
        im.fail("draw должна быть функцией: draw = function(p, rng) … end");
        lua_pop(im.L, 1);
        return false;
    }
    lua_getfield(T, -1, "clips");
    if (lua_istable(T, -1)) {
        lua_pushnil(T);
        while (lua_next(T, -2)) {
            if (lua_type(T, -2) != LUA_TSTRING || !lua_isfunction(T, -1)) {
                const std::string name = lua_type(T, -2) == LUA_TSTRING ? lua_tostring(T, -2) : "?";
                lua_pop(T, 2);
                im.clips.clear();
                im.error = "clips." + name + ": клип — функция, clips = { walk = function(p) … end }";
                lua_pop(im.L, 1);
                return false;
            }
            im.clips.push_back(lua_tostring(T, -2));
            lua_pop(T, 1);
        }
    } else if (!lua_isnil(T, -1)) {
        im.error = "clips должен быть таблицей: clips = { walk = function(p) … end }";
        lua_pop(im.L, 1);
        return false;
    }
    lua_pop(T, 1);
    std::sort(im.clips.begin(), im.clips.end());
    if (!im.has_pose && im.clips.empty()) {
        im.error = "в таблице скрипта нет ни clips = { walk = function(p) … end }, ни pose = function(t, p, rng, clip) … end";
        lua_pop(im.L, 1);
        return false;
    }
    lua_getfield(T, -1, "params");
    if (lua_istable(T, -1)) {
        lua_pushnil(T);
        while (lua_next(T, -2)) {
            if (lua_type(T, -2) != LUA_TSTRING || !lua_isnumber(T, -1)) {
                const std::string name = lua_type(T, -2) == LUA_TSTRING ? lua_tostring(T, -2) : "?";
                im.error = "params." + name + ": параметр — число (скорость, длина шага…)";
                lua_pop(im.L, 1);
                return false;
            }
            im.params.push_back({lua_tostring(T, -2), lua_tonumber(T, -1)});
            lua_pop(T, 1);
        }
    } else if (!lua_isnil(T, -1)) {
        im.error = "params должен быть таблицей: params = { speed = 1 }";
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
const std::vector<std::string>& PoseScript::clips() const { return impl_->clips; }
bool PoseScript::has_pose() const { return impl_->has_pose; }
const CodeSites& PoseScript::code() const { return impl_->files[0].code; }
const CodeSites* PoseScript::module_code(std::string_view name) const {
    for (usize i = impl_->files.size(); i-- > 1;)
        if (impl_->files[i].name == name) return &impl_->files[i].code;
    return nullptr;
}
void PoseScript::set_module_reader(ModuleReader reader) { impl_->reader = std::move(reader); }

ModuleReader animations_reader(std::filesystem::path game_dir) {
    return [dir = std::move(game_dir)](std::string_view name, std::string& text, std::string& why) {
        if (name.empty() || name.find_first_of("/\\:") != std::string_view::npos || name == "." || name == "..") {
            why = "имя модуля без папок: модуль — файл animations/имя.luau в папке игры";
            return false;
        }
        std::vector<u8> bytes;
        if (dir.empty() || !read_file(dir / "animations" / utf8_path(std::string(name) + ".luau"), bytes)) {
            why = "нет файла animations/" + std::string(name) + ".luau в папке игры";
            return false;
        }
        text.assign(bytes.begin(), bytes.end());
        return true;
    };
}
void PoseScript::set_lengths(std::string_view clip, const MotionLengths& lengths) {
    impl_->lengths[std::string(clip)] = lengths;
}
const std::string& PoseScript::error() const { return impl_->error; }
u32 PoseScript::error_line() const { return impl_->error_line; }
void PoseScript::set_budget_ms(f64 ms) { impl_->budget_ns = static_cast<u64>(std::max(1.0, ms) * 1e6); }

bool PoseScript::record(const Rig* rig, std::string_view clip, const std::map<std::string, f64>& values, u64 seed,
                        const MotionLengths& lengths, Recording& out) {
    Impl& im = *impl_;
    out = {};
    im.error.clear();
    im.error_line = 0;
    if (!im.ok) {
        im.error = "скрипт не загружен";
        return false;
    }
    if (!std::binary_search(im.clips.begin(), im.clips.end(), clip)) {
        im.error = "в коде нет клипа «" + std::string(clip) + "»: clips = { " + std::string(clip) + " = function(p) … end }";
        return false;
    }
    lua_State* T = im.module();
    if (!T) return false;
    const bool ok = record_clip(im, T, rig, clip, values, seed, lengths, out);
    lua_pop(im.L, 1);
    return ok;
}

namespace {

// pose(t, p, rng, clip) of the module on T's stack, into `out`.
bool run_pose(PoseScript::Impl& im, lua_State* T, const Rig* rig, f64 t, std::string_view clip,
              const std::map<std::string, f64>& values, u64 seed, Pose& out) {
    lua_getfield(T, -1, "pose");
    lua_pushnumber(T, t);
    push_params(im, T, values);
    lua_pushcfunction(T, &api_rng, "rng");
    lua_pushlstring(T, clip.data(), clip.size());
    // The frame's chance: from the seed, the time and the clip, the same on every run.
    im.rng = frame_seed(seed, t, clip);
    im.rig = rig;
    im.pose = &out;
    const auto was = im.mode;
    im.mode = PoseScript::Impl::Mode::Pose;
    im.begin_call();
    im.active = T;
    const int status = lua_pcall(T, 4, 0, 0);
    im.in_call = false;
    im.mode = was;
    im.rig = nullptr;
    im.pose = nullptr;
    if (status != LUA_OK) {
        im.fail(lua_tostring(T, -1));
        lua_pop(T, 1);
        return false;
    }
    return true;
}

} // namespace

bool PoseScript::frame(const Recording& rec, const Rig* rig, f64 t, const std::map<std::string, f64>& values, u64 seed,
                       Pose& out) {
    Impl& im = *impl_;
    out = {};
    im.error.clear();
    im.error_line = 0;
    if (!im.ok) {
        im.error = "скрипт не загружен";
        return false;
    }
    lua_State* T = nullptr;
    if (im.has_pose) {
        T = im.module();
        if (!T) return false;
    } else {
        T = lua_newthread(im.L);
        luaL_sandboxthread(T);
    }
    std::string why;
    im.active = T;
    im.seed = seed;
    im.frame_rec = &rec;
    im.frame_t = t;
    im.mode = Impl::Mode::Frame;
    im.begin_call();
    bool ok = sample_pose(rec, t, &im, out, why);
    im.in_call = false;
    im.mode = Impl::Mode::None;
    if (!ok) im.fail(why.c_str());
    if (ok && im.has_pose) {
        Pose over;
        ok = run_pose(im, T, rig, t, rec.clip, values, seed, over);
        if (ok) layer_pose(out, over);
    }
    im.frame_rec = nullptr;
    if (!ok) out = {};
    lua_pop(im.L, 1);
    return ok;
}

const Recording* PoseScript::recording(const Rig& rig, std::string_view clip, const std::map<std::string, f64>& values, u64 seed) {
    Impl& im = *impl_;
    im.error.clear();
    im.error_line = 0;
    if (!im.ok) {
        im.error = "скрипт позы не загружен";
        return nullptr;
    }
    if (!std::binary_search(im.clips.begin(), im.clips.end(), clip)) {
        im.error = "в коде нет клипа «" + std::string(clip) + "»: clips = { " + std::string(clip) + " = function(p) … end }";
        return nullptr;
    }
    const std::string name(clip);
    const MotionLengths& lengths = im.lengths[name];
    // What the recording depends on besides the code: the rig, the values, the seed, the lengths.
    std::string key = std::to_string(reinterpret_cast<uintptr_t>(&rig)) + '\n' + std::to_string(seed);
    for (const ScriptParam& p : im.params) {
        const auto it = values.find(p.name);
        key += '\n' + p.name + '=' + format_number(it != values.end() ? it->second : p.value) +
               std::to_string(it != values.end() ? std::bit_cast<u64>(it->second) : 0);
    }
    for (const auto& [n, v] : lengths.waits) key += "\nw" + n + '=' + std::to_string(std::bit_cast<u64>(v));
    for (const auto& [n, v] : lengths.durations) key += "\nd" + n + '=' + std::to_string(std::bit_cast<u64>(v));
    Impl::Cached& c = im.cache[name];
    if (c.key != key) {
        c.key = key;
        c.ok = record(&rig, clip, values, seed, lengths, c.rec);
        c.error = im.error;
        c.error_line = im.error_line;
    }
    if (!c.ok) {
        im.error = c.error;
        im.error_line = c.error_line;
        return nullptr;
    }
    return &c.rec;
}

void PoseScript::forget() { impl_->cache.clear(); }

bool PoseScript::pose(const Rig& rig, f64 t, std::string_view clip, const std::map<std::string, f64>& values, u64 seed, Pose& out) {
    Impl& im = *impl_;
    out = {};
    im.error.clear();
    im.error_line = 0;
    if (!im.ok) {
        im.error = "скрипт позы не загружен";
        return false;
    }
    if (std::binary_search(im.clips.begin(), im.clips.end(), clip)) {
        const Recording* rec = recording(rig, clip, values, seed);
        return rec && frame(*rec, &rig, t, values, seed, out);
    }
    if (!im.has_pose) {
        im.error = "в коде нет клипа «" + std::string(clip) + "»: clips = { " + std::string(clip) + " = function(p) … end }";
        return false;
    }
    lua_State* T = im.module();
    if (!T) return false;
    const bool ok = run_pose(im, T, &rig, t, clip, values, seed, out);
    if (!ok) out = {};
    lua_pop(im.L, 1);
    return ok;
}

} // namespace forge::pixel
