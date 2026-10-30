#include "forge/vector/scene_file.h"

#include "forge/vector/timing.h"
#include "json_text.h"

#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>

namespace forge::vector {

namespace {

using json::number;
using json::put_number;
using json::put_string;
using json::text;

f64 snap_ns(f64 t) { return std::round(t * 1e9) / 1e9; }

constexpr const char* kPlayWords[] = {"play", "loop", "stop"};

// --- writing ---------------------------------------------------------------------------------

void put_pair(std::string& out, f64 a, f64 b) {
    out += '[';
    put_number(out, a);
    out += ", ";
    put_number(out, b);
    out += ']';
}

// "key": number, after a comma.
void put_field(std::string& out, const char* key, f64 v) {
    out += ", \"";
    out += key;
    out += "\": ";
    put_number(out, v);
}

// A list a line an item: "[]" when empty.
template <class T, class Put>
void put_list(std::string& out, const char* key, const std::vector<T>& items, Put put, bool last = false) {
    out += "  \"";
    out += key;
    out += "\": [";
    for (usize i = 0; i < items.size(); ++i) {
        out += i ? ",\n    " : "\n    ";
        put(items[i]);
    }
    out += items.empty() ? "]" : "\n  ]";
    out += last ? "\n" : ",\n";
}

// --- reading ---------------------------------------------------------------------------------

// A file's path inside the scene's folder: forward slashes, no way out of it.
bool inside(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos)
        return false;
    usize from = 0;
    while (from <= path.size()) {
        const usize slash = std::min(path.find('/', from), path.size());
        const std::string_view part = path.substr(from, slash - from);
        if (part.empty() || part == "." || part == "..") return false;
        from = slash + 1;
    }
    return true;
}

bool hex_color(std::string_view s) {
    if (s.size() != 7 || s[0] != '#') return false;
    return std::all_of(s.begin() + 1, s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
}

struct Reader {
    SceneFile& f;
    std::string& why;
    u64 version = 0;

    bool fail(const std::string& where, const std::string& what) {
        why = where + ": " + what;
        return false;
    }

    static bool pair(yyjson_val* v, f64& a, f64& b) {
        return yyjson_is_arr(v) && yyjson_arr_size(v) == 2 && number(yyjson_arr_get(v, 0), a) && number(yyjson_arr_get(v, 1), b);
    }

    static bool order(yyjson_val* v, u32& out) {
        if (!yyjson_is_uint(v) || yyjson_get_uint(v) > std::numeric_limits<u32>::max()) return false;
        out = static_cast<u32>(yyjson_get_uint(v));
        return true;
    }

    // A list under `key` (none: empty), each item an object read by `item`.
    template <class Item>
    bool list(yyjson_val* r, const char* key, const char* shape, Item item) {
        yyjson_val* v = yyjson_obj_get(r, key);
        if (!v) return true;
        if (!yyjson_is_arr(v)) return fail(key, std::string("нужен список ") + shape);
        usize i, n;
        yyjson_val* x;
        yyjson_arr_foreach(v, i, n, x) {
            const std::string at = std::string(key) + "[" + std::to_string(i) + "]";
            if (!yyjson_is_obj(x)) return fail(at, std::string("нужен объект ") + shape);
            if (!item(x, at)) return false;
        }
        return true;
    }

    bool taken(const std::string& id) const {
        return std::any_of(f.pictures.begin(), f.pictures.end(), [&](const ScenePicture& p) { return p.id == id; }) ||
               std::any_of(f.actors.begin(), f.actors.end(), [&](const SceneActor& a) { return a.id == id; });
    }

    bool picture(yyjson_val* v, const std::string& at) {
        ScenePicture p;
        if (!text(yyjson_obj_get(v, "id"), p.id) || p.id.empty()) return fail(at, "id: нужно имя картинки строкой");
        if (taken(p.id)) return fail(at, "имя «" + p.id + "» дважды");
        if (!text(yyjson_obj_get(v, "file"), p.file) || !inside(p.file)) return fail(at, "file: нужен путь PNG внутри папки сцены");
        if (!number(yyjson_obj_get(v, "x"), p.x) || !number(yyjson_obj_get(v, "y"), p.y)) return fail(at, "x, y: нужен левый верхний угол числами");
        if (!number(yyjson_obj_get(v, "w"), p.w) || !number(yyjson_obj_get(v, "h"), p.h) || !(p.w > 0) || !(p.h > 0))
            return fail(at, "w, h: нужен размер больше 0");
        f.pictures.push_back(std::move(p));
        return true;
    }

    bool actor(yyjson_val* v, const std::string& at) {
        SceneActor a;
        if (!text(yyjson_obj_get(v, "id"), a.id) || a.id.empty()) return fail(at, "id: нужно имя актёра строкой");
        if (taken(a.id)) return fail(at, "имя «" + a.id + "» дважды");
        if (a.id == kSceneCamera) return fail(at, "имя camera — у камеры сцены");
        if (!text(yyjson_obj_get(v, "character"), a.character)) return fail(at, "character: нужен файл персонажа строкой");
        if (!text(yyjson_obj_get(v, "picture"), a.picture) || !inside(a.picture)) return fail(at, "picture: нужен путь SVG внутри папки сцены");
        if (yyjson_val* m = yyjson_obj_get(v, "motion"); m && (!text(m, a.motion) || !inside(a.motion)))
            return fail(at, "motion: нужен путь файла клипов внутри папки сцены (без motion — актёр клипов не играет)");
        if (!pair(yyjson_obj_get(v, "origin"), a.origin.x, a.origin.y)) return fail(at, "origin: нужен левый верхний угол рисунка [x, y]");
        if (!pair(yyjson_obj_get(v, "size"), a.w, a.h) || !(a.w > 0) || !(a.h > 0)) return fail(at, "size: нужен размер рисунка [w, h] больше 0");
        if (yyjson_val* r = yyjson_obj_get(v, "reach")) {
            const bool four = yyjson_is_arr(r) && yyjson_arr_size(r) == 4 && number(yyjson_arr_get(r, 0), a.reach.x) &&
                              number(yyjson_arr_get(r, 1), a.reach.y) && number(yyjson_arr_get(r, 2), a.reach.w) && number(yyjson_arr_get(r, 3), a.reach.h);
            if (!four || !(a.reach.w > 0) || !(a.reach.h > 0)) return fail(at, "reach: нужна рамка рисунка в сцене [x, y, w, h], w и h больше 0");
        }
        if (yyjson_val* rb = yyjson_obj_get(v, "reach_by")) {
            if (version < 3) return fail(at, "reach_by: параметры сцены — в файле версии 3");
            if (!yyjson_is_obj(rb)) return fail(at, "reach_by: нужен объект {\"параметр\": [слева, сверху, справа, снизу]}");
            usize i, n;
            yyjson_val *key, *edges;
            yyjson_obj_foreach(rb, i, n, key, edges) {
                const std::string name(yyjson_get_str(key), yyjson_get_len(key));
                const std::string where = at + ": reach_by." + name;
                if (!f.param(name)) return fail(where, "у сцены нет такого параметра");
                if (f.fixed.count(name)) return fail(where, "этот параметр из игры не меняется");
                std::array<f64, 4> e{};
                const bool four = yyjson_is_arr(edges) && yyjson_arr_size(edges) == 4 && number(yyjson_arr_get(edges, 0), e[0]) &&
                                  number(yyjson_arr_get(edges, 1), e[1]) && number(yyjson_arr_get(edges, 2), e[2]) && number(yyjson_arr_get(edges, 3), e[3]);
                if (!four || std::any_of(e.begin(), e.end(), [](f64 x) { return !(x >= 0); }))
                    return fail(where, "нужно, на сколько уходит каждый край на единицу: [слева, сверху, справа, снизу], от 0");
                a.reach_by[name] = e;
            }
        }
        f.actors.push_back(std::move(a));
        return true;
    }

    bool play(yyjson_val* v, const std::string& at) {
        ScenePlay p;
        if (!text(yyjson_obj_get(v, "actor"), p.actor) || !f.actor(p.actor)) return fail(at, "actor: нужен актёр из actors");
        std::string kind;
        if (!text(yyjson_obj_get(v, "kind"), kind)) return fail(at, "kind: нужно \"play\", \"loop\" или \"stop\"");
        const auto k = std::find(std::begin(kPlayWords), std::end(kPlayWords), kind);
        if (k == std::end(kPlayWords)) return fail(at, "kind: «" + kind + "» — нужно \"play\", \"loop\" или \"stop\"");
        p.kind = static_cast<ScenePlay::Kind>(k - std::begin(kPlayWords));
        const bool stop = p.kind == ScenePlay::Kind::Stop;
        yyjson_val* clip = yyjson_obj_get(v, "clip");
        if (stop && clip) return fail(at, "clip: у stop клипа нет");
        if (!stop && (!text(clip, p.clip) || p.clip.empty())) return fail(at, "clip: нужно имя клипа строкой");
        if (!number(yyjson_obj_get(v, "at"), p.at) || p.at < 0) return fail(at, "at: нужен миг сцены от 0");
        if (yyjson_val* s = yyjson_obj_get(v, "speed")) {
            if (stop) return fail(at, "speed: у stop скорости нет");
            if (!number(s, p.speed) || !(p.speed > 0)) return fail(at, "speed: нужно число больше 0");
        } else if (!stop) return fail(at, "speed: нужна скорость клипа");
        if (!order(yyjson_obj_get(v, "order"), p.order)) return fail(at, "order: нужен номер вызова, целое от 0");
        if (yyjson_val* b = yyjson_obj_get(v, "blend")) {
            if (stop) return fail(at, "blend: у stop смены нет");
            if (version < 2) return fail(at, "blend: смена клипов — в файле версии 2");
            if (!number(b, p.blend) || !(p.blend >= 0)) return fail(at, "blend: нужны секунды смены от 0");
        }
        f.plays.push_back(std::move(p));
        return true;
    }

    bool music(yyjson_val* v, const std::string& at) {
        SceneMusicCall m;
        if (!number(yyjson_obj_get(v, "at"), m.at) || m.at < 0) return fail(at, "at: нужен миг сцены от 0");
        if (!order(yyjson_obj_get(v, "order"), m.order)) return fail(at, "order: нужен номер вызова, целое от 0");
        if (yyjson_val* n = yyjson_obj_get(v, "name"); n && (!text(n, m.name) || m.name.empty()))
            return fail(at, "name: нужно имя звука игры строкой (без name — тишина)");
        if (!number(yyjson_obj_get(v, "fade"), m.fade) || m.fade < 0) return fail(at, "fade: нужны секунды наплыва от 0");
        if (!number(yyjson_obj_get(v, "volume"), m.volume) || m.volume < 0 || m.volume > 1) return fail(at, "volume: нужна громкость от 0 до 1");
        f.music.push_back(std::move(m));
        return true;
    }

    bool stop(yyjson_val* v, const std::string& at) {
        SceneStop s;
        if (!number(yyjson_obj_get(v, "at"), s.at) || s.at < 0) return fail(at, "at: нужен миг сцены от 0");
        if (!f.stops.empty() && s.at < f.stops.back().at) return fail(at, "at: ожидания идут по времени");
        if (!order(yyjson_obj_get(v, "order"), s.order)) return fail(at, "order: нужен номер вызова, целое от 0");
        yyjson_val* action = yyjson_obj_get(v, "action");
        yyjson_val* talk = yyjson_obj_get(v, "talk");
        if (!action == !talk) return fail(at, "нужно одно из action (действие «Ввода», \"\" — любая клавиша) и talk (разговор)");
        s.talk = talk != nullptr;
        if (!text(s.talk ? talk : action, s.name)) return fail(at, s.talk ? "talk: нужно имя разговора строкой" : "action: нужно имя действия строкой");
        if (s.talk && s.name.empty()) return fail(at, "talk: нужно имя разговора");
        f.stops.push_back(std::move(s));
        return true;
    }

    bool transition(yyjson_val* v, const std::string& at) {
        SceneTransition t;
        yyjson_val* leave = yyjson_obj_get(v, "leave");
        if (!yyjson_is_bool(leave)) return fail(at, "leave: нужно true (уход) или false (вход)");
        t.leave = yyjson_get_bool(leave);
        if (!text(yyjson_obj_get(v, "kind"), t.kind)) return fail(at, "kind: нужен вид перехода строкой (\"\" — свой)");
        if (yyjson_val* from = yyjson_obj_get(v, "from"); from && !text(from, t.from)) return fail(at, "from: нужен край строкой");
        if (!number(yyjson_obj_get(v, "at"), t.at) || t.at < 0) return fail(at, "at: нужен миг сцены от 0");
        if (!number(yyjson_obj_get(v, "length"), t.length) || t.length < 0) return fail(at, "length: нужна длина от 0");
        f.transitions.push_back(std::move(t));
        return true;
    }

    // Part 6г6: the scene's parameters and those a game does not change.
    bool params(yyjson_val* r) {
        yyjson_val* ps = yyjson_obj_get(r, "params");
        yyjson_val* fx = yyjson_obj_get(r, "fixed");
        if ((ps || fx) && version < 3) return why = std::string(ps ? "params" : "fixed") + ": параметры сцены — в файле версии 3", false;
        if (ps) {
            if (!yyjson_is_arr(ps)) return why = "params: нужен список параметров [{\"name\": …, \"value\": …}]", false;
            usize i, n;
            yyjson_val* item;
            yyjson_arr_foreach(ps, i, n, item) {
                const std::string at = "params[" + std::to_string(i) + "]";
                MotionParam prm;
                if (!yyjson_is_obj(item)) return fail(at, "нужен объект {\"name\": …, \"value\": …}");
                if (!text(yyjson_obj_get(item, "name"), prm.name) || prm.name.empty()) return fail(at, "name: нужно имя параметра строкой");
                if (!number(yyjson_obj_get(item, "value"), prm.value)) return fail(at, "value: нужно число — с ним вкладка отдала сцену");
                if (f.param(prm.name)) return fail(at, "параметр «" + prm.name + "» дважды");
                if (!f.params.empty() && !(f.params.back().name < prm.name)) return fail(at, "параметры идут по имени");
                f.params.push_back(std::move(prm));
            }
        }
        if (fx) {
            if (!yyjson_is_obj(fx)) return why = "fixed: нужен объект {\"параметр\": \"почему не меняется\"}", false;
            usize i, n;
            yyjson_val *key, *v;
            yyjson_obj_foreach(fx, i, n, key, v) {
                const std::string name(yyjson_get_str(key), yyjson_get_len(key));
                std::string reason;
                if (!f.param(name)) return fail("fixed." + name, "у сцены нет такого параметра");
                if (!text(v, reason) || reason.empty()) return fail("fixed." + name, "нужно, почему он из игры не меняется, строкой");
                f.fixed[name] = std::move(reason);
            }
        }
        return true;
    }

    bool root(yyjson_val* r) {
        if (!yyjson_is_obj(r)) return why = "в файле не объект JSON", false;
        std::string kind;
        if (!text(yyjson_obj_get(r, "forge"), kind) || kind != "scene") return why = "forge: это не файл сцены (нужно \"scene\")", false;
        yyjson_val* ver = yyjson_obj_get(r, "version");
        if (!yyjson_is_uint(ver) || yyjson_get_uint(ver) == 0) return why = "version: нужно целое число от 1", false;
        if (yyjson_get_uint(ver) > kSceneFileVersion)
            return why = "version " + std::to_string(yyjson_get_uint(ver)) + ": сцену отдал редактор новее этой игры (она читает " +
                         std::to_string(kSceneFileVersion) + ")",
                   false;
        version = yyjson_get_uint(ver);
        if (!text(yyjson_obj_get(r, "name"), f.name) || f.name.empty()) return why = "name: нужно имя сцены строкой", false;
        if (!text(yyjson_obj_get(r, "from"), f.from)) return why = "from: нужен файл сцены в «Ресурсах» строкой", false;
        if (!pair(yyjson_obj_get(r, "canvas"), f.width, f.height) || !(f.width > 0) || !(f.height > 0))
            return why = "canvas: нужен размер сцены [w, h] больше 0", false;
        if (yyjson_val* bg = yyjson_obj_get(r, "background"); bg && (!text(bg, f.background) || !hex_color(f.background)))
            return why = "background: нужен цвет \"#rrggbb\" (без background сцена прозрачна)", false;
        if (!number(yyjson_obj_get(r, "length"), f.length) || f.length < 0) return why = "length: нужна длина сцены от 0", false;
        if (!params(r)) return false;
        auto bind = [this](auto method) { return [this, method](yyjson_val* v, const std::string& at) { return (this->*method)(v, at); }; };
        if (!list(r, "pictures", "{\"id\": …, \"file\": …, \"x\": …}", bind(&Reader::picture))) return false;
        if (!list(r, "actors", "{\"id\": …, \"picture\": …, \"motion\": …}", bind(&Reader::actor))) return false;
        if (!list(r, "plays", "{\"actor\": …, \"kind\": …, \"at\": …}", bind(&Reader::play))) return false;
        if (!list(r, "music", "{\"at\": …, \"name\": …, \"fade\": …}", bind(&Reader::music))) return false;
        if (!list(r, "stops", "{\"at\": …, \"action\" или \"talk\": …}", bind(&Reader::stop))) return false;
        if (!list(r, "transitions", "{\"leave\": …, \"kind\": …, \"at\": …}", bind(&Reader::transition))) return false;
        if (yyjson_val* nd = yyjson_obj_get(r, "not_drawn")) {
            if (!yyjson_is_arr(nd)) return why = "not_drawn: нужен список слов", false;
            usize i, n;
            yyjson_val* w;
            yyjson_arr_foreach(nd, i, n, w) {
                std::string word;
                if (!text(w, word)) return fail("not_drawn[" + std::to_string(i) + "]", "нужно слово строкой");
                f.not_drawn.push_back(std::move(word));
            }
        }
        return true;
    }
};

// --- time ------------------------------------------------------------------------------------

const SceneStop* stop_of(const SceneFile& f, i32 waiting) {
    return waiting >= 0 && waiting < static_cast<i32>(f.stops.size()) ? &f.stops[static_cast<usize>(waiting)] : nullptr;
}

// Whether a stop was made before a thing of its moment numbered `order`: a play, music or another stop takes a number of
// its own, a mark the number the next of them takes (part 6г3), so one made right before the stop has the stop's.
bool made_before(const SceneStop& stop, u32 order) { return stop.order < order; }

// Whether a thing made at `at` (`order`) has begun at t: not when made after the stop the scene stands at, at its moment.
bool begun(f64 at, u32 order, f64 t, const SceneStop* stop) {
    if (at > t) return false;
    return !stop || at != stop->at || !made_before(*stop, order);
}

// The time lived by the scene when a thing made at `at` (`order`) begins: `at` and the waits made before it.
f64 lived_at(const SceneFile& f, std::span<const f64> held, f64 at, u32 order) {
    f64 x = at;
    for (usize i = 0; i < held.size() && i < f.stops.size(); ++i)
        if (f.stops[i].at < at || (f.stops[i].at == at && made_before(f.stops[i], order))) x += held[i];
    return x;
}

// A volume going from `from` to `to` over `fade` seconds from `at`: its value at t.
struct Ramp {
    f64 at = 0, fade = 0, from = 0, to = 0;
    f64 at_time(f64 t) const {
        if (!(fade > 0) || t >= at + fade) return to;
        return from + (to - from) * std::clamp((t - at) / fade, 0.0, 1.0);
    }
};

i32 actor_index(const SceneFile& f, std::string_view id) {
    for (usize i = 0; i < f.actors.size(); ++i)
        if (f.actors[i].id == id) return static_cast<i32>(i);
    return -1;
}

MotionPartPose rest_part(std::string id) {
    MotionPartPose pp;
    pp.id = std::move(id);
    for (usize p = 0; p < kMotionPropCount; ++p) pp.values[p] = motion_rest(static_cast<MotionProp>(p));
    return pp;
}

} // namespace

const SceneActor* SceneFile::actor(std::string_view id) const {
    for (const SceneActor& a : actors)
        if (a.id == id) return &a;
    return nullptr;
}

const MotionParam* SceneFile::param(std::string_view name) const {
    for (const MotionParam& p : params)
        if (p.name == name) return &p;
    return nullptr;
}

bool read_scene_file(std::string_view json, SceneFile& out, std::string& why) {
    out = {};
    why.clear();
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(json.data()), json.size(), 0, nullptr, &err);
    if (!doc) {
        why = "не JSON: " + std::string(err.msg ? err.msg : "") + " (байт " + std::to_string(err.pos) + ")";
        return false;
    }
    Reader r{out, why};
    const bool ok = r.root(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!ok) out = {};
    return ok;
}

std::string write_scene_file(const SceneFile& f) {
    // Version 3 only with parameters (part 6г6), 2 only with a blend (part 6г5а): a game made before reads the rest.
    const bool blends = std::any_of(f.plays.begin(), f.plays.end(), [](const ScenePlay& p) { return p.blend > 0; });
    const u32 version = !f.params.empty() ? 3 : blends ? 2 : 1;
    std::string out = "{\n  \"forge\": \"scene\",\n  \"version\": " + std::to_string(version) + ",\n  \"name\": ";
    put_string(out, f.name);
    out += ",\n  \"from\": ";
    put_string(out, f.from);
    out += ",\n  \"canvas\": ";
    put_pair(out, f.width, f.height);
    if (!f.background.empty()) {
        out += ",\n  \"background\": ";
        put_string(out, f.background);
    }
    out += ",\n  \"length\": ";
    put_number(out, f.length);
    if (!f.params.empty()) {
        out += ",\n  \"params\": [";
        for (usize i = 0; i < f.params.size(); ++i) {
            out += i ? ", {\"name\": " : "{\"name\": ";
            put_string(out, f.params[i].name);
            out += ", \"value\": ";
            put_number(out, f.params[i].value);
            out += '}';
        }
        out += "],\n  \"fixed\": {";
        for (const auto& [name, reason] : f.fixed) {
            if (out.back() != '{') out += ", ";
            put_string(out, name);
            out += ": ";
            put_string(out, reason);
        }
        out += '}';
    }
    out += ",\n";
    put_list(out, "pictures", f.pictures, [&](const ScenePicture& p) {
        out += "{\"id\": ";
        put_string(out, p.id);
        out += ", \"file\": ";
        put_string(out, p.file);
        put_field(out, "x", p.x);
        put_field(out, "y", p.y);
        put_field(out, "w", p.w);
        put_field(out, "h", p.h);
        out += '}';
    });
    put_list(out, "actors", f.actors, [&](const SceneActor& a) {
        out += "{\"id\": ";
        put_string(out, a.id);
        out += ", \"character\": ";
        put_string(out, a.character);
        out += ", \"picture\": ";
        put_string(out, a.picture);
        if (!a.motion.empty()) {
            out += ", \"motion\": ";
            put_string(out, a.motion);
        }
        out += ", \"origin\": ";
        put_pair(out, a.origin.x, a.origin.y);
        out += ", \"size\": ";
        put_pair(out, a.w, a.h);
        if (a.reach.w > 0) {
            out += ", \"reach\": [";
            for (const f64 v : {a.reach.x, a.reach.y, a.reach.w, a.reach.h}) {
                if (out.back() != '[') out += ", ";
                put_number(out, v);
            }
            out += ']';
        }
        if (!a.reach_by.empty()) {
            out += ", \"reach_by\": {";
            for (const auto& [name, edges] : a.reach_by) {
                if (out.back() != '{') out += ", ";
                put_string(out, name);
                out += ": [";
                for (usize i = 0; i < 4; ++i) {
                    if (i) out += ", ";
                    put_number(out, edges[i]);
                }
                out += ']';
            }
            out += '}';
        }
        out += '}';
    });
    put_list(out, "plays", f.plays, [&](const ScenePlay& p) {
        out += "{\"actor\": ";
        put_string(out, p.actor);
        out += ", \"kind\": ";
        put_string(out, kPlayWords[static_cast<usize>(p.kind)]);
        if (p.kind != ScenePlay::Kind::Stop) {
            out += ", \"clip\": ";
            put_string(out, p.clip);
        }
        put_field(out, "at", p.at);
        if (p.kind != ScenePlay::Kind::Stop) put_field(out, "speed", p.speed);
        out += ", \"order\": " + std::to_string(p.order);
        if (p.kind != ScenePlay::Kind::Stop && p.blend > 0) put_field(out, "blend", p.blend);
        out += '}';
    });
    put_list(out, "music", f.music, [&](const SceneMusicCall& m) {
        out += "{\"at\": ";
        put_number(out, m.at);
        out += ", \"order\": " + std::to_string(m.order);
        if (!m.name.empty()) {
            out += ", \"name\": ";
            put_string(out, m.name);
        }
        put_field(out, "fade", m.fade);
        put_field(out, "volume", m.volume);
        out += '}';
    });
    put_list(out, "stops", f.stops, [&](const SceneStop& s) {
        out += "{\"at\": ";
        put_number(out, s.at);
        out += ", \"order\": " + std::to_string(s.order) + (s.talk ? ", \"talk\": " : ", \"action\": ");
        put_string(out, s.name);
        out += '}';
    });
    put_list(out, "transitions", f.transitions, [&](const SceneTransition& t) {
        out += t.leave ? "{\"leave\": true, \"kind\": " : "{\"leave\": false, \"kind\": ";
        put_string(out, t.kind);
        if (!t.from.empty()) {
            out += ", \"from\": ";
            put_string(out, t.from);
        }
        put_field(out, "at", t.at);
        put_field(out, "length", t.length);
        out += '}';
    });
    out += "  \"not_drawn\": [";
    for (usize i = 0; i < f.not_drawn.size(); ++i) {
        if (i) out += ", ";
        put_string(out, f.not_drawn[i]);
    }
    out += "]\n}\n";
    return out;
}

bool check_scene(const SceneMotion& s, std::string& why) {
    if (!s.stage.clip("scene")) return why = "stage.motion.json: нет клипа «scene»", false;
    if (s.actors.size() != s.file.actors.size())
        return why = "актёров " + std::to_string(s.file.actors.size()) + ", а файлов их клипов " + std::to_string(s.actors.size()), false;
    for (const ScenePlay& p : s.file.plays) {
        if (p.kind == ScenePlay::Kind::Stop) continue;
        const i32 a = actor_index(s.file, p.actor);
        if (a < 0) return why = "нет актёра «" + p.actor + "»", false;
        if (s.file.actors[static_cast<usize>(a)].motion.empty()) return why = "актёр «" + p.actor + "» играет «" + p.clip + "», а файла клипов у него нет", false;
        if (!s.actors[static_cast<usize>(a)].clip(p.clip))
            return why = "актёр «" + p.actor + "»: в " + s.file.actors[static_cast<usize>(a)].motion + " нет клипа «" + p.clip + "»", false;
    }
    // Part 6г6: the motion files have the scene's parameters, the values the scene was given at.
    if (s.stage.params != s.file.params) return why = "stage.motion.json: параметры не те, что у сцены", false;
    for (usize a = 0; a < s.actors.size(); ++a)
        if (!s.file.actors[a].motion.empty() && s.actors[a].params != s.file.params)
            return why = s.file.actors[a].motion + ": параметры не те, что у сцены", false;
    return true;
}

bool scene_with_params(SceneMotion& s, const std::map<std::string, f64>& values, std::vector<std::string>& notes, std::string& why) {
    if (values.empty()) return true;
    const SceneFile& f = s.file;
    std::vector<MotionParam> given;
    for (const auto& [name, v] : values) {
        if (!f.param(name)) {
            if (f.params.empty()) return why = "у сцены «" + f.name + "» нет параметров", false;
            std::string have;
            for (const MotionParam& p : f.params) have += (have.empty() ? "" : ", ") + p.name;
            return why = "в сцене «" + f.name + "» нет параметра «" + name + "»: есть " + have, false;
        }
        if (!std::isfinite(v)) return why = "параметр " + name + ": нужно число", false;
    }
    for (const auto& [name, v] : values) {
        const MotionParam& own = *f.param(name);
        if (v == own.value) continue;
        if (const auto it = f.fixed.find(name); it != f.fixed.end()) {
            notes.push_back("параметр " + name + " из игры не меняется: " + it->second);
            continue;
        }
        given.push_back({name, v});
    }
    if (given.empty()) return true;
    for (MotionClip& c : s.stage.clips) c = motion_clip_with(s.stage, c, given);
    for (MotionFile& m : s.actors)
        for (MotionClip& c : m.clips) c = motion_clip_with(m, c, given);
    for (SceneActor& a : s.file.actors) {
        f64 out[4] = {};
        for (const MotionParam& g : given)
            if (const auto it = a.reach_by.find(g.name); it != a.reach_by.end())
                for (usize i = 0; i < 4; ++i) out[i] += it->second[i] * std::abs(g.value - f.param(g.name)->value);
        if (!(out[0] > 0 || out[1] > 0 || out[2] > 0 || out[3] > 0)) continue;
        const SceneBox b = a.reach.w > 0 ? a.reach : SceneBox{a.origin.x, a.origin.y, a.w, a.h};
        const f64 x0 = std::floor(b.x - out[0]), y0 = std::floor(b.y - out[1]);
        const f64 x1 = std::ceil(b.x + b.w + out[2]), y1 = std::ceil(b.y + b.h + out[3]);
        a.reach = {x0, y0, x1 - x0, y1 - y0};
    }
    return true;
}

namespace {

// An actor's calls by start, then as made, and what it shows when one of them is the last begun.
struct ActorCalls {
    const SceneMotion& s;
    std::span<const f64> held;
    i32 actor = -1;
    std::vector<i32> mine; // indices of file.plays

    const ScenePlay& play(i64 k) const { return s.file.plays[static_cast<usize>(mine[static_cast<usize>(k)])]; }

    f64 clip_length(const ScenePlay& p) const {
        const MotionClip* clip = static_cast<usize>(actor) < s.actors.size() ? s.actors[static_cast<usize>(actor)].clip(p.clip) : nullptr;
        return clip ? clip->length : 0;
    }

    // What the actor shows at t (the scene's waits up to then, of those at t made before `until`) when call k is the last
    // begun: empty at rest. A stop holds what the one before it showed at its start.
    std::vector<SceneBlendStep> shows(i64 k, f64 t, u32 until) const {
        while (k >= 0 && play(k).kind == ScenePlay::Kind::Stop) {
            t = play(k).at;
            until = play(k--).order;
        }
        if (k < 0) return {};
        const ScenePlay& p = play(k);
        // The clip went on while the scene waited after its start.
        f64 waited = 0;
        for (usize i = 0; i < held.size() && i < s.file.stops.size(); ++i) {
            const SceneStop& w = s.file.stops[i];
            const bool after = w.at > p.at || (w.at == p.at && w.order > p.order);
            const bool by_then = w.at < t || (w.at == t && w.order < until);
            if (after && by_then) waited += held[i];
        }
        const f64 length = clip_length(p);
        const f64 x = (t - p.at + waited) * p.speed;
        SceneBlendStep step;
        step.play = mine[static_cast<usize>(k)];
        if (p.kind == ScenePlay::Kind::Play) step.time = std::min(x, length);
        else step.time = length > 0 ? std::fmod(x, length) : 0;
        step.time = std::max(0.0, step.time);
        // Part 6г5а: its blend, over its clip's play, no longer than the clip when it is no loop.
        const f64 blend = !(length > 0) ? 0 : p.kind == ScenePlay::Kind::Loop ? p.blend : std::min(p.blend, length);
        if (blend > 0 && x < blend) step.k = timing(Timing::Smooth, std::max(0.0, x) / blend);
        if (step.k >= 1) return {step};
        // From what it showed at the play's start: the call before going on by its own time, or that held (a stop, a
        // blend going on then, the character at rest).
        std::vector<SceneBlendStep> out = shows(k - 1, p.at, p.order);
        if (out.size() == 1 && out[0].play == mine[static_cast<usize>(k - 1)]) out = shows(k - 1, t, until);
        if (out.empty()) out.push_back({});
        out.push_back(step);
        return out;
    }
};

} // namespace

SceneActorMoment scene_actor_moment(const SceneMotion& s, std::string_view actor, f64 t, std::span<const f64> held, i32 waiting) {
    const SceneFile& f = s.file;
    ActorCalls calls{s, held, actor_index(f, actor), {}};
    if (calls.actor < 0) return {};
    for (usize i = 0; i < f.plays.size(); ++i)
        if (f.plays[i].actor == actor) calls.mine.push_back(static_cast<i32>(i));
    std::stable_sort(calls.mine.begin(), calls.mine.end(), [&](i32 a, i32 b) {
        const ScenePlay &x = f.plays[static_cast<usize>(a)], &y = f.plays[static_cast<usize>(b)];
        return x.at != y.at ? x.at < y.at : x.order < y.order;
    });
    const SceneStop* at = stop_of(f, waiting);
    i64 k = -1;
    for (usize i = 0; i < calls.mine.size(); ++i)
        if (begun(calls.play(static_cast<i64>(i)).at, calls.play(static_cast<i64>(i)).order, t, at)) k = static_cast<i64>(i);
    std::vector<SceneBlendStep> steps = calls.shows(k, t, std::numeric_limits<u32>::max());
    if (steps.empty()) return {};
    SceneActorMoment m;
    m.play = steps.back().play;
    m.time = steps.back().time;
    if (steps.size() > 1) m.blend = std::move(steps);
    return m;
}

std::vector<SceneMusicVoice> scene_music_at(const SceneFile& f, f64 t, std::span<const f64> held, i32 waiting) {
    struct Playing {
        u32 call = 0;
        Ramp volume;
    };
    const SceneStop* at = stop_of(f, waiting);
    // The moments music lives by: a scene's time and the waits before it (a stop at the same time made before it).
    auto lived = [&](f64 time, u32 order) { return lived_at(f, held, time, order); };
    std::vector<u32> calls(f.music.size());
    for (u32 i = 0; i < calls.size(); ++i) calls[i] = i;
    std::stable_sort(calls.begin(), calls.end(), [&](u32 a, u32 b) { return f.music[a].at < f.music[b].at; });
    const f64 now_at = lived(t, std::numeric_limits<u32>::max());
    std::optional<Playing> now;
    std::vector<Playing> going; // the ones fading out
    for (const u32 i : calls) {
        const SceneMusicCall& m = f.music[i];
        if (!begun(m.at, m.order, t, at)) continue;
        const f64 when = lived(m.at, m.order);
        if (now && !m.name.empty() && f.music[now->call].name == m.name) {
            // The same music goes on: only its volume moves.
            now->volume = {when, m.fade, now->volume.at_time(when), m.volume};
            continue;
        }
        if (now) going.push_back({now->call, {when, m.fade, now->volume.at_time(when), 0}});
        now.reset();
        if (!m.name.empty()) now = Playing{i, {when, m.fade, 0, m.volume}};
    }
    auto since = [&](u32 call) { return now_at - lived(f.music[call].at, f.music[call].order); };
    std::vector<SceneMusicVoice> out;
    for (const Playing& p : going)
        if (now_at < p.volume.at + p.volume.fade) out.push_back({f.music[p.call].name, since(p.call), p.volume.at_time(now_at), p.call});
    if (now) out.push_back({f.music[now->call].name, since(now->call), now->volume.at_time(now_at), now->call});
    return out;
}

ScenePhase scene_phase(const SceneFile& f, f64 t) {
    bool entering = false;
    ScenePhase phase = ScenePhase::Scene;
    for (const SceneTransition& tr : f.transitions) {
        const f64 end = snap_ns(tr.at + tr.length);
        if (!tr.leave) entering = entering || (t >= tr.at && t < end);
        else if (t >= tr.at) phase = t < end ? ScenePhase::Leave : ScenePhase::Gone;
    }
    return entering && phase == ScenePhase::Scene ? ScenePhase::Enter : phase;
}

bool scene_actor_look(MotionProp p) {
    switch (p) {
    case MotionProp::Fill:
    case MotionProp::Stroke:
    case MotionProp::StrokeWidth:
    case MotionProp::Dash:
    case MotionProp::DrawFrom:
    case MotionProp::DrawTo:
    case MotionProp::Tint:
    case MotionProp::Flash:
    case MotionProp::FlashColor: return true;
    default: return false;
    }
}

void scene_over(MotionPartPose& clip, const MotionPartPose& scene) {
    auto has = [&](MotionProp p) { return scene.has[static_cast<usize>(p)]; };
    auto sv = [&](MotionProp p) -> const MotionValue& { return scene.values[static_cast<usize>(p)]; };
    auto cv = [&](MotionProp p) -> MotionValue& { return clip.values[static_cast<usize>(p)]; };
    auto take = [&](MotionProp p) {
        clip.has[static_cast<usize>(p)] = true;
        cv(p) = sv(p);
    };
    for (const MotionProp p : {MotionProp::X, MotionProp::Y, MotionProp::Rot, MotionProp::SkewX, MotionProp::SkewY, MotionProp::Dash})
        if (has(p)) clip.has[static_cast<usize>(p)] = true, cv(p).v += sv(p).v;
    for (const MotionProp p : {MotionProp::Scale, MotionProp::Sx, MotionProp::Sy, MotionProp::Opacity})
        if (has(p)) clip.has[static_cast<usize>(p)] = true, cv(p).v *= sv(p).v;
    if (has(MotionProp::Visible)) {
        clip.has[static_cast<usize>(MotionProp::Visible)] = true;
        cv(MotionProp::Visible).v = cv(MotionProp::Visible).v > 0 && sv(MotionProp::Visible).v > 0 ? 1 : 0;
    }
    // A share over the part's own: the scene's when it moved it from rest.
    for (const MotionProp p : {MotionProp::Fill, MotionProp::Stroke, MotionProp::StrokeWidth, MotionProp::Tint, MotionProp::FlashColor, MotionProp::Morph})
        if (has(p) && sv(p).v > 0) take(p);
    const bool drawn = (has(MotionProp::DrawFrom) && sv(MotionProp::DrawFrom).v != 0) || (has(MotionProp::DrawTo) && sv(MotionProp::DrawTo).v != 1);
    if (drawn) {
        for (const MotionProp p : {MotionProp::DrawFrom, MotionProp::DrawTo}) {
            clip.has[static_cast<usize>(p)] = true;
            cv(p) = has(p) ? sv(p) : motion_rest(p);
        }
    }
    if (has(MotionProp::Flash) && sv(MotionProp::Flash).v != 0) take(MotionProp::Flash);
    if (has(MotionProp::Mask) && sv(MotionProp::Mask).v != 0) take(MotionProp::Mask);
}

ScenePose scene_pose(const SceneMotion& s, f64 t, std::span<const f64> held, i32 waiting) {
    const SceneFile& f = s.file;
    ScenePose out;
    out.phase = scene_phase(f, t);
    out.camera = rest_part(kSceneCamera);
    for (usize i = 0; i < std::size(kSceneFrames); ++i) out.frames[i] = rest_part(kSceneFrames[i]);
    // The scene's own pose: the stage's parts, its looks of its actors and its transitions of their parts.
    std::vector<std::vector<MotionPartPose>> over(f.actors.size());
    std::vector<std::vector<MotionOrderStep>> over_order(f.actors.size());
    if (const MotionClip* clip = s.stage.clip("scene")) {
        for (MotionPartPose& pp : motion_pose(s.stage, *clip, t)) {
            if (pp.id == kSceneCamera) {
                out.camera = std::move(pp);
                continue;
            }
            const auto frame = std::find(std::begin(kSceneFrames), std::end(kSceneFrames), pp.id);
            if (frame != std::end(kSceneFrames)) {
                out.frames[frame - std::begin(kSceneFrames)] = std::move(pp);
                continue;
            }
            if (const i32 a = actor_index(f, pp.id); a >= 0) {
                // The whole actor: where it stands, its opacity, mask and link on its group of the stage; its looks on
                // its picture's group, which has its id.
                MotionPartPose look = rest_part(pp.id);
                for (usize p = 0; p < kMotionPropCount; ++p)
                    if (pp.has[p] && scene_actor_look(static_cast<MotionProp>(p))) {
                        look.has[p] = true;
                        look.values[p] = pp.values[p];
                        pp.has[p] = false;
                        pp.values[p] = motion_rest(static_cast<MotionProp>(p));
                    }
                over[static_cast<usize>(a)].push_back(std::move(look));
                out.stage.push_back(std::move(pp));
                continue;
            }
            const usize slash = pp.id.find('/');
            if (const i32 a = slash == std::string::npos ? -1 : actor_index(f, std::string_view(pp.id).substr(0, slash)); a >= 0) {
                over[static_cast<usize>(a)].push_back(std::move(pp));
                continue;
            }
            out.stage.push_back(std::move(pp));
        }
        for (const MotionCopy& c : clip->copies)
            if (motion_copy_drawn(c, t)) out.copies.push_back(c);
        for (MotionOrderStep& st : motion_order_at(*clip, t)) {
            const usize slash = st.part.find('/');
            if (const i32 a = slash == std::string::npos ? -1 : actor_index(f, std::string_view(st.part).substr(0, slash)); a >= 0)
                over_order[static_cast<usize>(a)].push_back(std::move(st));
            else out.order.push_back(std::move(st));
        }
    }
    // Each actor: its clip at its moment, the scene's over it.
    for (usize a = 0; a < f.actors.size(); ++a) {
        SceneActorPose ap;
        ap.id = f.actors[a].id;
        const MotionFile* file = a < s.actors.size() ? &s.actors[a] : nullptr;
        // Where its parts stand: by its file, or by the stage's for one that plays no clip.
        const MotionFile* records = f.actors[a].motion.empty() ? &s.stage : file;
        ap.moment = scene_actor_moment(s, ap.id, t, held, waiting);
        // A step's clip at its moment; the character at rest without one.
        auto clip_pose = [&](const SceneBlendStep& st) {
            const MotionClip* clip = file && st.play >= 0 ? file->clip(f.plays[static_cast<usize>(st.play)].clip) : nullptr;
            return clip ? motion_clip_pose(*file, *clip, st.time) : MotionClipPose{};
        };
        MotionClipPose shown = clip_pose({ap.moment.play, ap.moment.time});
        if (file && !ap.moment.blend.empty()) {
            shown = clip_pose(ap.moment.blend.front());
            for (usize i = 1; i < ap.moment.blend.size(); ++i) shown = motion_blend(*file, shown, clip_pose(ap.moment.blend[i]), ap.moment.blend[i].k);
        }
        ap.parts = std::move(shown.parts);
        ap.copies = std::move(shown.copies);
        ap.order = std::move(shown.order);
        ap.shown = std::move(shown.shown);
        ap.from_parts = std::move(shown.from_parts);
        // The scene's over the clip's, in the drawing shown and, in a swap, in the one it comes from.
        auto put_over = [&](std::vector<MotionPartPose>& parts, std::string_view drawing) {
            for (const MotionPartPose& sp : over[a]) {
                auto it = std::find_if(parts.begin(), parts.end(), [&](const MotionPartPose& pp) { return pp.id == sp.id; });
                if (it == parts.end()) it = parts.insert(parts.end(), rest_part(sp.id));
                scene_over(*it, sp);
                if (records) motion_part_pose(*records, *it, drawing);
            }
        };
        put_over(ap.parts, ap.shown.drawing);
        if (ap.shown.swap) put_over(ap.from_parts, ap.shown.from);
        ap.order.insert(ap.order.end(), over_order[a].begin(), over_order[a].end());
        out.actors.push_back(std::move(ap));
    }
    return out;
}

bool scene_marks(const SceneMotion& s, std::vector<SceneMark>& out, std::string& why) {
    constexpr usize kMax = 100000;
    out.clear();
    const SceneFile& f = s.file;
    if (const MotionClip* clip = s.stage.clip("scene"))
        for (const MotionMark& m : clip->marks) out.push_back({m.name, m.sound, {}, m.at});
    // Each actor's plays by start, then order, the actors by name (forge::pixel::plays_of).
    std::vector<const ScenePlay*> all;
    for (const ScenePlay& p : f.plays) all.push_back(&p);
    std::stable_sort(all.begin(), all.end(), [](const ScenePlay* a, const ScenePlay* b) {
        if (a->actor != b->actor) return a->actor < b->actor;
        return a->at != b->at ? a->at < b->at : a->order < b->order;
    });
    for (usize i = 0; i < all.size(); ++i) {
        const ScenePlay& p = *all[i];
        if (p.kind == ScenePlay::Kind::Stop) continue;
        const bool last = i + 1 == all.size() || all[i + 1]->actor != p.actor;
        const f64 end = last ? f.length : all[i + 1]->at;
        const i32 a = actor_index(f, p.actor);
        const MotionClip* clip = a >= 0 && static_cast<usize>(a) < s.actors.size() ? s.actors[static_cast<usize>(a)].clip(p.clip) : nullptr;
        if (!clip || clip->marks.empty()) continue;
        const bool loop = p.kind == ScenePlay::Kind::Loop;
        const f64 round = clip->length / p.speed;
        for (u64 k = 0;; ++k) {
            const f64 base = snap_ns(p.at + static_cast<f64>(k) * round);
            if (k > 0 && (!loop || !(round > 0) || base >= end)) break;
            for (const MotionMark& m : clip->marks) {
                const f64 at = snap_ns(base + m.at / p.speed);
                if (at > end || (at == end && !last)) continue;
                if (out.size() >= kMax)
                    return why = "актёр «" + p.actor + "»: меток клипов больше " + std::to_string(kMax) + " — клип по кругу слишком короткий", false;
                out.push_back({m.name, m.sound, p.actor, at});
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const SceneMark& a, const SceneMark& b) { return a.at < b.at; });
    return true;
}

f64 scene_lived(const SceneAt& at) {
    f64 x = at.t;
    for (const f64 h : at.held) x += h;
    return x;
}

f64 scene_since(const SceneFile& f, const SceneAt& at, f64 when, u32 order) {
    if (!begun(when, order, at.t, stop_of(f, at.waiting))) return -1;
    return std::max(0.0, scene_lived(at) - lived_at(f, at.held, when, order));
}

namespace {

// A mark of a scene at a moment of its play: which (the scene's own: play -1, its mark; an actor's: its play, round and
// mark in the clip) and the time lived when it comes.
struct LivedMark {
    i64 play = -1;
    u64 round = 0;
    usize mark = 0;
    f64 at = 0;
    bool operator==(const LivedMark& o) const { return play == o.play && round == o.round && mark == o.mark; }
};

// The marks the scene has had at `now`, lived in [lo, hi] (and up to now), in the order scene_marks lists them: the scene's
// own, then each actor's by its plays (by name, then start, then order), by round and mark. False past `room` of them.
bool marks_had(const SceneMotion& s, const SceneAt& now, f64 lo, f64 hi, std::vector<LivedMark>& out, usize room) {
    constexpr f64 kEps = 1e-9;
    const SceneFile& f = s.file;
    const SceneStop* stop = stop_of(f, now.waiting);
    const f64 lived = scene_lived(now);
    hi = std::min(hi, lived);
    auto keep = [&](LivedMark m) {
        if (m.at < lo - kEps || m.at > hi + kEps) return true;
        if (out.size() >= room) return false;
        out.push_back(m);
        return true;
    };
    if (const MotionClip* clip = s.stage.clip("scene"))
        for (usize i = 0; i < clip->marks.size(); ++i) {
            const MotionMark& m = clip->marks[i];
            if (!begun(m.at, m.order, now.t, stop)) continue;
            if (!keep({-1, 0, i, lived_at(f, now.held, m.at, m.order)})) return false;
        }
    std::vector<usize> all(f.plays.size());
    for (usize i = 0; i < all.size(); ++i) all[i] = i;
    std::stable_sort(all.begin(), all.end(), [&](usize a, usize b) {
        const ScenePlay &x = f.plays[a], &y = f.plays[b];
        if (x.actor != y.actor) return x.actor < y.actor;
        return x.at != y.at ? x.at < y.at : x.order < y.order;
    });
    for (usize i = 0; i < all.size(); ++i) {
        const ScenePlay& p = f.plays[all[i]];
        if (p.kind == ScenePlay::Kind::Stop || !begun(p.at, p.order, now.t, stop)) continue;
        const bool last = i + 1 == all.size() || f.plays[all[i + 1]].actor != p.actor;
        // Its end: the actor's next call when it has begun (not at it), else as far as the scene has lived.
        f64 end = lived;
        bool at_end = true;
        if (!last) {
            const ScenePlay& q = f.plays[all[i + 1]];
            if (begun(q.at, q.order, now.t, stop)) {
                end = lived_at(f, now.held, q.at, q.order);
                at_end = false;
            }
        }
        const i32 a = actor_index(f, p.actor);
        const MotionClip* clip = a >= 0 && static_cast<usize>(a) < s.actors.size() ? s.actors[static_cast<usize>(a)].clip(p.clip) : nullptr;
        if (!clip || clip->marks.empty()) continue;
        const bool loop = p.kind == ScenePlay::Kind::Loop;
        const f64 round = clip->length / p.speed;
        // What it waited before it began: its marks are its moments in the scene's time and that.
        const f64 before = lived_at(f, now.held, p.at, p.order) - p.at;
        u64 k = 0;
        if (loop && round > 0 && lo - kEps > p.at + before + round) k = static_cast<u64>(std::floor((lo - kEps - p.at - before) / round)) - 1;
        for (;; ++k) {
            const f64 base = snap_ns(p.at + static_cast<f64>(k) * round);
            if (k > 0 && (!loop || !(round > 0))) break;
            if (base + before > std::min(end, hi) + kEps) break;
            for (usize j = 0; j < clip->marks.size(); ++j) {
                const f64 in_scene = snap_ns(base + clip->marks[j].at / p.speed);
                const f64 at = before == 0 ? in_scene : in_scene + before;
                if (at > end + kEps || (!at_end && at > end - kEps)) continue;
                if (!keep({static_cast<i64>(all[i]), k, j, at})) return false;
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const LivedMark& x, const LivedMark& y) { return x.at < y.at; });
    return true;
}

} // namespace

bool scene_marks_between(const SceneMotion& s, const SceneAt* from, const SceneAt& to, std::vector<SceneMark>& out, std::string& why) {
    constexpr usize kMax = 100000;
    out.clear();
    const f64 lo = from ? scene_lived(*from) : -1;
    std::vector<LivedMark> now, had;
    if (!marks_had(s, to, lo, std::numeric_limits<f64>::infinity(), now, kMax) ||
        (from && !marks_had(s, *from, lo, lo, had, kMax)))
        return why = "меток клипов за раз больше " + std::to_string(kMax) + " — клип по кругу слишком короткий", false;
    const MotionClip* own = s.stage.clip("scene");
    for (const LivedMark& m : now) {
        if (std::find(had.begin(), had.end(), m) != had.end()) continue;
        if (m.play < 0) {
            const MotionMark& mk = own->marks[m.mark];
            out.push_back({mk.name, mk.sound, {}, m.at});
            continue;
        }
        const ScenePlay& p = s.file.plays[static_cast<usize>(m.play)];
        const MotionMark& mk = s.actors[static_cast<usize>(actor_index(s.file, p.actor))].clip(p.clip)->marks[m.mark];
        out.push_back({mk.name, mk.sound, p.actor, m.at});
    }
    return true;
}

MotionMatrix scene_camera_matrix(const MotionPartPose& camera, f64 w, f64 h) {
    auto value = [&](MotionProp p) {
        const usize i = static_cast<usize>(p);
        return camera.has[i] ? camera.values[i].v : motion_rest(p).v;
    };
    const f64 zoom = value(MotionProp::Scale), r = -value(MotionProp::Rot) * std::numbers::pi / 180, c = std::cos(r), s = std::sin(r);
    const f64 cx = w / 2 + value(MotionProp::X), cy = h / 2 + value(MotionProp::Y);
    MotionMatrix m{zoom * c, zoom * s, -zoom * s, zoom * c, 0, 0};
    m.e = w / 2 - (m.a * cx + m.c * cy);
    m.f = h / 2 - (m.b * cx + m.d * cy);
    return m;
}

} // namespace forge::vector
