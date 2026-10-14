#include "forge/pixel/doc.h"

#include <yyjson.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace forge::pixel {

namespace {

bool number(yyjson_val* v, f64& out) {
    if (!yyjson_is_num(v)) return false;
    out = yyjson_get_num(v);
    return std::isfinite(out);
}

bool whole(yyjson_val* v, u64 lo, u64 hi, u64& out) {
    if (!yyjson_is_uint(v)) return false;
    out = yyjson_get_uint(v);
    return out >= lo && out <= hi;
}

std::string hex(const Rgba8& c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

bool read_root(yyjson_val* root, PixelDoc& d, std::string& why) {
    if (!yyjson_is_obj(root)) {
        why = "в файле не объект JSON";
        return false;
    }
    if (yyjson_val* v = yyjson_obj_get(root, "svg")) {
        if (!yyjson_is_str(v)) return why = "svg: нужна строка (Guid рисунка)", false;
        d.svg = yyjson_get_str(v);
    }
    if (yyjson_val* v = yyjson_obj_get(root, "script")) {
        if (!yyjson_is_str(v)) return why = "script: нужно имя файла строкой", false;
        d.script = yyjson_get_str(v);
    }
    if (yyjson_val* v = yyjson_obj_get(root, "frame")) {
        u64 w = 0, h = 0;
        if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 2 || !whole(yyjson_arr_get(v, 0), 1, kMaxFrameSide, w) ||
            !whole(yyjson_arr_get(v, 1), 1, kMaxFrameSide, h))
            return why = "frame: нужны ширина и высота кадра, целые от 1 до " + std::to_string(kMaxFrameSide), false;
        d.frame_w = static_cast<u32>(w), d.frame_h = static_cast<u32>(h);
    }
    if (yyjson_val* v = yyjson_obj_get(root, "seed")) {
        if (!yyjson_is_uint(v)) return why = "seed: нужно целое число от 0", false;
        d.seed = yyjson_get_uint(v);
    }
    if (yyjson_val* v = yyjson_obj_get(root, "params")) {
        if (!yyjson_is_obj(v)) return why = "params: нужен объект {\"имя\": число}", false;
        usize i, n;
        yyjson_val *key, *val;
        yyjson_obj_foreach(v, i, n, key, val) {
            f64 x;
            if (!number(val, x)) return why = std::string("params.") + yyjson_get_str(key) + ": нужно число", false;
            d.params[yyjson_get_str(key)] = x;
        }
    }
    if (yyjson_val* v = yyjson_obj_get(root, "pivots")) {
        if (!yyjson_is_obj(v)) return why = "pivots: нужен объект {\"часть\": [x, y]}", false;
        usize i, n;
        yyjson_val *key, *val;
        yyjson_obj_foreach(v, i, n, key, val) {
            vector::Point p;
            if (!yyjson_is_arr(val) || yyjson_arr_size(val) != 2 || !number(yyjson_arr_get(val, 0), p.x) || !number(yyjson_arr_get(val, 1), p.y))
                return why = std::string("pivots.") + yyjson_get_str(key) + ": нужна точка [x, y]", false;
            d.pivots[yyjson_get_str(key)] = p;
        }
    }
    if (yyjson_val* v = yyjson_obj_get(root, "clips")) {
        if (!yyjson_is_obj(v)) return why = "clips: нужен объект {\"состояние\": {\"frames\": …}}", false;
        usize i, n;
        yyjson_val *key, *val;
        yyjson_obj_foreach(v, i, n, key, val) {
            const std::string name = yyjson_get_str(key);
            if (!yyjson_is_obj(val)) return why = "clips." + name + ": нужен объект {\"frames\": 4, \"fps\": 10, \"loop\": true}", false;
            ClipSpec c;
            c.state = name;
            u64 frames = c.frames;
            if (yyjson_val* f = yyjson_obj_get(val, "frames"); f && !whole(f, 1, kMaxStripFrames, frames))
                return why = "clips." + name + ".frames: нужно целое от 1 до " + std::to_string(kMaxStripFrames), false;
            c.frames = static_cast<u32>(frames);
            f64 fps = c.fps;
            if (yyjson_val* f = yyjson_obj_get(val, "fps"); f && (!number(f, fps) || fps <= 0 || fps > 60))
                return why = "clips." + name + ".fps: нужно число больше 0 и не больше 60", false;
            c.fps = static_cast<f32>(fps);
            if (yyjson_val* f = yyjson_obj_get(val, "loop")) {
                if (!yyjson_is_bool(f)) return why = "clips." + name + ".loop: нужно true или false", false;
                c.loop = yyjson_get_bool(f);
            }
            d.clips.push_back(std::move(c));
        }
    }
    for (const bool wait : {true, false}) {
        const char* field = wait ? "waits" : "durations";
        yyjson_val* v = yyjson_obj_get(root, field);
        if (!v) continue;
        const std::string form = std::string(field) + ": нужен объект {\"клип\": {\"имя\": секунды}}";
        if (!yyjson_is_obj(v)) return why = form, false;
        usize i, n;
        yyjson_val *clip, *names;
        yyjson_obj_foreach(v, i, n, clip, names) {
            if (!yyjson_is_obj(names)) return why = form, false;
            std::map<std::string, f64>& m = wait ? d.lengths[yyjson_get_str(clip)].waits : d.lengths[yyjson_get_str(clip)].durations;
            usize j, k;
            yyjson_val *name, *len;
            yyjson_obj_foreach(names, j, k, name, len) {
                f64 s = 0;
                if (!number(len, s) || s < 0)
                    return why = std::string(field) + "." + yyjson_get_str(clip) + "." + yyjson_get_str(name) +
                                 ": нужно число секунд не меньше 0",
                           false;
                m[yyjson_get_str(name)] = s;
            }
        }
    }
    if (yyjson_val* v = yyjson_obj_get(root, "bake")) {
        if (!yyjson_is_obj(v)) return why = "bake: нужен объект", false;
        if (yyjson_val* p = yyjson_obj_get(v, "palette")) {
            const char* s = yyjson_get_str(p);
            if (!s || (std::string_view(s) != "svg" && std::string_view(s) != "game"))
                return why = "bake.palette: \"svg\" (цвета рисунка) или \"game\" (цвета игры)", false;
            d.game_palette = std::string_view(s) == "game";
        }
        if (yyjson_val* o = yyjson_obj_get(v, "outline"); o && !yyjson_is_null(o)) {
            const char* s = yyjson_get_str(o);
            if (!s || !parse_color(s, d.outline_color)) return why = "bake.outline: null или цвет \"#rrggbb\"", false;
            d.outline = true;
        }
        if (yyjson_val* p = yyjson_obj_get(v, "picture")) {
            if (!yyjson_is_str(p)) return why = "bake.picture: нужна строка (Guid картинки)", false;
            d.picture = yyjson_get_str(p);
        }
        if (yyjson_val* h = yyjson_obj_get(v, "hash")) {
            if (!yyjson_is_str(h)) return why = "bake.hash: нужна строка", false;
            d.hash = yyjson_get_str(h);
        }
        if (yyjson_val* st = yyjson_obj_get(v, "strip")) {
            if (!yyjson_is_obj(st)) return why = "bake.strip: нужен объект {\"состояние\": {\"first\": 0, \"frames\": 4, …}}", false;
            usize i, n;
            yyjson_val *key, *val;
            yyjson_obj_foreach(st, i, n, key, val) {
                const std::string name = yyjson_get_str(key);
                StripClip c;
                c.state = name;
                u64 first = 0, frames = 0;
                f64 fps = 0;
                yyjson_val* l = yyjson_is_obj(val) ? yyjson_obj_get(val, "loop") : nullptr;
                if (!yyjson_is_obj(val) || !whole(yyjson_obj_get(val, "first"), 0, kMaxStripFrames - 1, first) ||
                    !whole(yyjson_obj_get(val, "frames"), 1, kMaxStripFrames, frames) || first + frames > kMaxStripFrames ||
                    !number(yyjson_obj_get(val, "fps"), fps) || fps <= 0 || fps > 60 || !yyjson_is_bool(l))
                    return why = "bake.strip." + name + ": нужно {\"first\": 0, \"frames\": 4, \"fps\": 10, \"loop\": true} в пределах " +
                                 std::to_string(kMaxStripFrames) + " кадров",
                           false;
                c.first = static_cast<u32>(first), c.frames = static_cast<u32>(frames), c.fps = static_cast<f32>(fps);
                c.loop = yyjson_get_bool(l);
                d.strip.push_back(std::move(c));
            }
        }
    }
    return true;
}

} // namespace

bool read_pixel_doc(std::string_view json, PixelDoc& out, std::string& why) {
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(json.data()), json.size(), 0, nullptr, &err);
    if (!doc) {
        why = std::string("это не JSON: ") + err.msg + " (знак " + std::to_string(err.pos) + ")";
        return false;
    }
    PixelDoc d;
    const bool ok = read_root(yyjson_doc_get_root(doc), d, why);
    yyjson_doc_free(doc);
    if (ok) out = std::move(d);
    return ok;
}

std::string write_pixel_doc(const PixelDoc& d) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "svg", d.svg.c_str());
    yyjson_mut_obj_add_strcpy(doc, root, "script", d.script.c_str());
    yyjson_mut_val* frame = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_uint(doc, frame, d.frame_w);
    yyjson_mut_arr_add_uint(doc, frame, d.frame_h);
    yyjson_mut_obj_add_val(doc, root, "frame", frame);
    yyjson_mut_obj_add_uint(doc, root, "seed", d.seed);
    yyjson_mut_val* params = yyjson_mut_obj(doc);
    for (const auto& [k, v] : d.params) yyjson_mut_obj_add(params, yyjson_mut_strcpy(doc, k.c_str()), yyjson_mut_real(doc, v));
    yyjson_mut_obj_add_val(doc, root, "params", params);
    yyjson_mut_val* pivots = yyjson_mut_obj(doc);
    for (const auto& [k, p] : d.pivots) {
        yyjson_mut_val* pt = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_real(doc, pt, p.x);
        yyjson_mut_arr_add_real(doc, pt, p.y);
        yyjson_mut_obj_add(pivots, yyjson_mut_strcpy(doc, k.c_str()), pt);
    }
    yyjson_mut_obj_add_val(doc, root, "pivots", pivots);
    yyjson_mut_val* clips = yyjson_mut_obj(doc);
    for (const ClipSpec& c : d.clips) {
        yyjson_mut_val* o = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, o, "frames", c.frames);
        yyjson_mut_obj_add_real(doc, o, "fps", c.fps);
        yyjson_mut_obj_add_bool(doc, o, "loop", c.loop);
        yyjson_mut_obj_add(clips, yyjson_mut_strcpy(doc, c.state.c_str()), o);
    }
    yyjson_mut_obj_add_val(doc, root, "clips", clips);
    // Only when some clip has them: a file of a character without time events stays as it was.
    for (const bool wait : {true, false}) {
        yyjson_mut_val* field = yyjson_mut_obj(doc);
        for (const auto& [clip, l] : d.lengths) {
            const std::map<std::string, f64>& m = wait ? l.waits : l.durations;
            if (m.empty()) continue;
            yyjson_mut_val* names = yyjson_mut_obj(doc);
            for (const auto& [name, s] : m) yyjson_mut_obj_add(names, yyjson_mut_strcpy(doc, name.c_str()), yyjson_mut_real(doc, s));
            yyjson_mut_obj_add(field, yyjson_mut_strcpy(doc, clip.c_str()), names);
        }
        if (yyjson_mut_obj_size(field) > 0) yyjson_mut_obj_add_val(doc, root, wait ? "waits" : "durations", field);
    }
    yyjson_mut_val* bake = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, bake, "palette", d.game_palette ? "game" : "svg");
    if (d.outline) yyjson_mut_obj_add_strcpy(doc, bake, "outline", hex(d.outline_color).c_str());
    else yyjson_mut_obj_add_null(doc, bake, "outline");
    if (!d.picture.empty()) yyjson_mut_obj_add_strcpy(doc, bake, "picture", d.picture.c_str());
    if (!d.hash.empty()) yyjson_mut_obj_add_strcpy(doc, bake, "hash", d.hash.c_str());
    if (!d.strip.empty()) {
        yyjson_mut_val* strip = yyjson_mut_obj(doc);
        for (const StripClip& c : d.strip) {
            yyjson_mut_val* o = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_uint(doc, o, "first", c.first);
            yyjson_mut_obj_add_uint(doc, o, "frames", c.frames);
            yyjson_mut_obj_add_real(doc, o, "fps", c.fps);
            yyjson_mut_obj_add_bool(doc, o, "loop", c.loop);
            yyjson_mut_obj_add(strip, yyjson_mut_strcpy(doc, c.state.c_str()), o);
        }
        yyjson_mut_obj_add_val(doc, bake, "strip", strip);
    }
    yyjson_mut_obj_add_val(doc, root, "bake", bake);
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
    std::string out(text, len);
    out += '\n';
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

} // namespace forge::pixel
