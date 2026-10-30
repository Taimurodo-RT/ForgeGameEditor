#include "forge/game/scenes.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/game/screens.h"
#include "forge/ui/live_svg.h"
#include "forge/ui/ui.h"
#include "forge/vector/motion_file.h"
#include "forge/vector/svg.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/PictureScale.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace forge::game {

namespace fs = std::filesystem;

namespace {

bool read_text_file(const fs::path& path, std::string& out) {
    std::vector<u8> bytes;
    if (!read_file(path, bytes)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

// A path of the scene's folder as scene.json gives it: inside the folder.
bool inside(std::string_view rel) {
    if (rel.empty() || rel.find("..") != std::string_view::npos || rel.find(':') != std::string_view::npos) return false;
    return rel.front() != '/' && rel.front() != '\\';
}

std::string px(f64 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.4fpx", std::isfinite(v) ? v : 0.0);
    return buf;
}

// A matrix of the stage's units as a transform of a layer whose px are those units (its origin at its top left).
std::string transform_of(const vector::MotionMatrix& m) {
    auto n = [](f64 v) { return std::isfinite(v) ? v : 0.0; };
    char buf[200];
    std::snprintf(buf, sizeof buf, "translate(%.4fpx, %.4fpx) matrix(%.7f, %.7f, %.7f, %.7f, 0, 0)", n(m.e), n(m.f), n(m.a), n(m.b), n(m.c),
                  n(m.d));
    return buf;
}

constexpr const char* kStyle = "body { position: absolute; left: 0px; top: 0px; width: 100%; height: 100%; pointer-events: none; }"
                               "div, img, backdrop { display: block; position: absolute; left: 0px; top: 0px; pointer-events: none;"
                               " transform-origin: 0px 0px 0px; }"
                               // Part 6г4: the frames of a transition, each the whole screen, posed round its middle; their
                               // masks and a picture's or an actor's stretched over them.
                               ".full { width: 100%; height: 100%; }"
                               ".move { transform-origin: 50% 50% 0px; }"
                               ".wrap, .item { mask-size: 100% 100%; }"
                               "#black { background-color: #000000; }"
                               // Always: its layers are moved by transforms, which the layout does not count as overflow (a scene
                               // fitted larger than its canvas spilled over the bars).
                               "#frame { overflow: hidden; clip: always; }"
                               // The bars over it: the frame's edge on whole pixels of the window (the software Vulkan lets
                               // a column of a scissor's edge through with MSAA).
                               "#bars { width: 100%; height: 100%; box-sizing: border-box; border-width: 0px; }";

// The picture a play's <backdrop> takes of the game's frame under it (part 6г4), and the masks it makes.
std::string game_picture(u64 serial) { return "scene-" + std::to_string(serial) + "-game"; }

// How long the scene's music goes out, and the game's comes back, after its end.
constexpr f64 kMusicBack = 1.0;

} // namespace

bool read_scene_folder(const fs::path& dir, vector::SceneMotion& out, std::string& why) {
    out = {};
    std::error_code ec;
    auto read = [&](const std::string& rel, std::string& text) {
        if (!inside(rel)) return why = "путь «" + rel + "» выходит из папки сцены", false;
        if (!fs::is_regular_file(dir / utf8_path(rel), ec)) return why = "нет «" + rel + "»", false;
        if (!read_text_file(dir / utf8_path(rel), text)) return why = "«" + rel + "» не читается", false;
        return true;
    };
    auto there = [&](const std::string& rel) {
        if (!inside(rel)) return why = "путь «" + rel + "» выходит из папки сцены", false;
        if (!fs::is_regular_file(dir / utf8_path(rel), ec)) return why = "нет «" + rel + "»", false;
        return true;
    };
    std::string text;
    if (!read("scene.json", text)) return false;
    if (!vector::read_scene_file(text, out.file, why)) return why = "scene.json: " + why, false;
    if (!read("stage.motion.json", text)) return false;
    if (!vector::read_motion_file(text, out.stage, why)) return why = "stage.motion.json: " + why, false;
    if (!there("stage.svg")) return false;
    for (const vector::SceneActor& a : out.file.actors) {
        const std::string whose = "актёр «" + a.id + "»: ";
        vector::MotionFile m;
        if (!there(a.picture)) return why = whose + why, false;
        if (!a.motion.empty()) {
            if (!read(a.motion, text)) return why = whose + why, false;
            if (!vector::read_motion_file(text, m, why)) return why = whose + "«" + a.motion + "»: " + why, false;
            const std::string beside = path_to_utf8(utf8_path(a.picture).parent_path());
            for (const vector::MotionDrawing& d : m.drawings)
                if (!there(beside.empty() ? d.picture : beside + "/" + d.picture)) return why = whose + why, false;
        }
        out.actors.push_back(std::move(m));
    }
    for (const vector::ScenePicture& p : out.file.pictures)
        if (!there(p.file)) return why = "картинка «" + p.id + "»: " + why, false;
    return vector::check_scene(out, why);
}

std::vector<std::string> scene_names(const fs::path& game_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(game_dir / "scenes", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && fs::is_regular_file(it->path() / "scene.json", ec)) out.push_back(path_to_utf8(it->path().filename()));
    std::sort(out.begin(), out.end());
    return out;
}

std::string scene_sound_file(const fs::path& sounds, std::string_view name) {
    if (name.empty() || sounds.empty() || name.find_first_of("/\\") != std::string_view::npos) return {};
    const std::string n(name);
    for (const std::string& f : {n, n + ".wav", n + ".ogg"}) {
        std::error_code ec;
        if (fs::is_regular_file(sounds / utf8_path(f), ec)) return f;
    }
    return {};
}

struct GameScene::Impl {
    // A layer of the scene in its order: a run of the stage's shapes, a picture, an actor.
    struct Layer {
        enum class Kind : u8 { Run, Picture, Actor };
        Kind kind = Kind::Run;
        std::vector<std::string> keys; // a run's elements of the stage's top level (ui::SvgPlace::key)
        std::string key;               // a picture's or an actor's group
        usize item = 0;                // in the scene's pictures or actors
        Rml::Element* element = nullptr;
        // Part 6г4: a picture's or an actor's own layer, its mask by a shape of the stage on it (its box, the picture, or
        // hidden: what was set); its element's place in it.
        Rml::Element* wrap = nullptr;
        std::string wrapped, inside;
        ui::SvgPose posed; // what the run or the actor shows
        bool has_pose = false;
        std::string transform;
        f64 opacity = -1;
        bool shown = true;
    };

    Rml::Context* context = nullptr;
    fs::path dir, sounds;
    std::string name;
    SceneHow how;
    vector::SceneMotion scene;
    std::vector<std::vector<ui::SvgDrawing>> drawings; // each actor's other drawings, by scene.file.actors
    // Its moment: the clock (the frames' steps summed) read to the nanosecond, so 90 steps of 1/60 are 1.5, where the tab's
    // frame 90 has a clip begun, and not 1.4999999999999996, a hair before it.
    f64 t = 0, clock = 0;
    // Part 6г3: how long it stood at each of its stops (the frames' steps summed), the one it stands at (-1: none), the
    // next it comes to and whether its talk is open; where it was a frame ago, its marks had up to there.
    std::vector<f64> held;
    i32 waiting = -1;
    usize next_stop = 0;
    bool talk_begun = false;
    vector::SceneAt was;
    bool has_was = false;
    bool going = false, fresh = false;
    u64 serial = 0, serials = 0;
    std::map<u64, bool> ends; // the plays over: skipped?

    // A frame of a transition (part 6г4): its wrap the whole screen, faded and masked; in it the frame in its pose. What was
    // set, so that only a change is set.
    struct Frame {
        Rml::Element* wrap = nullptr;
        Rml::Element* move = nullptr;
        std::string transform, filter, mask;
        bool shown = false;
    };
    // A play's document on the screen: the snapshot of the game's frame under it, black, the game's frame below the
    // scene's, the scene's (its background all round, the camera's frame cut at its edge, the bars) and the game's above.
    struct Shown {
        Rml::ElementDocument* doc = nullptr;
        u64 serial = 0;
        Rml::Element* backdrop = nullptr;
        Rml::Element* black = nullptr;
        Rml::Element* bg = nullptr;
        Rml::Element* frame = nullptr;
        Rml::Element* camera = nullptr;
        Rml::Element* bars = nullptr;
        Frame below, look, above;
        bool backdrop_on = false, black_on = false;
        vector::ScenePhase phase = vector::ScenePhase::Scene;
        // Its pictures made by code (/gpu/…) and what each was made of.
        std::map<std::string, std::string> pictures;
    };
    // The one playing; the one before it standing as its old frame while it comes in; the last frame of the one just over,
    // held till the logic has run after its end (and the one standing under it then).
    Shown cur, prev, last, last_prev;
    // How many times the game's logic has run (logic_ran; untold, the updates), and how many at the last end.
    u64 runs = 0, end_runs = 0;
    bool runs_told = false;
    ui::Ui* ui = nullptr;
    ui::SvgPlaces places;
    std::vector<Layer> layers;
    std::string structure;
    int fit_w = -1, fit_h = -1;
    f32 fit_dp = 0;
    f64 fit_scale = 1, fit_left = 0, fit_top = 0;
    std::string camera_transform;
    u64 run_poses = 0;

    // The music: what on_music last heard; after the end, the scene's going out over kMusicBack.
    std::vector<SceneTune> tunes;
    f64 share = 1;
    bool tail = false;
    f64 tail_t = 0, tail_share = 1;
    std::vector<SceneTune> tail_tunes;

    std::set<std::string> said;
    std::vector<std::string> problems;
    GameScene* self = nullptr;

    void say(const std::string& line) {
        if (!said.insert(line).second) return;
        FORGE_WARN("%s", line.c_str());
        problems.push_back(line);
    }

    const std::string& scene_word() const { return name; }

    // A picture made by code goes from the GPU and from RmlUi's textures (an element shows it by either path).
    void forget(const std::string& picture) {
        if (ui) ui->drop_gpu_image(picture);
        Rml::ReleaseTexture("/gpu/" + picture);
        Rml::ReleaseTexture("gpu/" + picture);
    }

    void close(Shown& s) {
        if (s.doc) {
            if (context) context->UnloadDocument(s.doc);
            for (const auto& [picture, made] : s.pictures) forget(picture);
            forget(game_picture(s.serial));
        }
        s = {};
    }

    // What the play drew goes with it (its document stays as the last frame, held by finish).
    void forget_play() {
        layers.clear();
        structure.clear();
        camera_transform.clear();
        fit_w = fit_h = -1;
    }

    void close_all() {
        close(cur);
        close(prev);
        close(last);
        close(last_prev);
        forget_play();
    }

    void hear(std::vector<SceneTune> want, f64 game_share) {
        if (want == tunes && game_share == share) return;
        tunes = std::move(want);
        share = game_share;
        if (self->on_music) self->on_music(tunes, share);
    }

    // Where it is in its time, its waits read to the nanosecond as its moment is.
    vector::SceneAt at() const {
        vector::SceneAt a;
        a.t = t;
        a.held.reserve(held.size());
        for (const f64 h : held) a.held.push_back(std::round(h * 1e9) / 1e9);
        a.waiting = waiting;
        return a;
    }

    // Its time goes on from the stop it stands at.
    void go_on() {
        if (waiting < 0) return;
        next_stop = static_cast<usize>(waiting) + 1;
        waiting = -1;
        talk_begun = false;
    }

    // dt seconds of it pass: its time goes to the next stop and stands there, the rest of the frame waiting at it; a talk
    // opens when it comes to one (after a dialogue open already ends), and it goes on when that ends or did not begin.
    void advance(f64 dt) {
        const std::vector<vector::SceneStop>& stops = scene.file.stops;
        const f64 length = scene.file.length;
        f64 rest = dt;
        for (;;) {
            if (waiting >= 0) {
                const vector::SceneStop& s = stops[static_cast<usize>(waiting)];
                if (s.talk) {
                    const bool open = self->talking && self->talking();
                    if (talk_begun && !open) {
                        go_on();
                        continue;
                    }
                    if (!talk_begun && !open) {
                        talk_begun = self->on_talk && self->on_talk(s.name);
                        if (!talk_begun) {
                            say("сцена «" + name + "»: разговор «" + s.name + "» не начался (его нет в игре или ни одно начало не подошло) — сцена идёт дальше");
                            go_on();
                            continue;
                        }
                    }
                }
                held[static_cast<usize>(waiting)] += rest;
                return;
            }
            const f64 ahead = std::min(clock + rest, length);
            if (next_stop < stops.size() && stops[next_stop].at <= std::round(ahead * 1e9) / 1e9 + 1e-9) {
                const f64 to = stops[next_stop].at;
                rest = std::max(0.0, clock + rest - to);
                clock = std::max(clock, to);
                t = to;
                waiting = static_cast<i32>(next_stop);
                talk_begun = false;
                continue;
            }
            clock = ahead;
            t = clock < length ? std::min(std::round(clock * 1e9) / 1e9, length) : length;
            return;
        }
    }

    // The scene's tunes at its moment t and how loud the game's music is under them: as it is until the first call of
    // `music`, then going out over that call's fade (its waits counted: music goes on while it waits).
    void music_now() {
        const vector::SceneFile& f = scene.file;
        if (f.music.empty()) return;
        const vector::SceneAt now = at();
        std::vector<SceneTune> want;
        for (const vector::SceneMusicVoice& v : vector::scene_music_at(f, t, now.held, now.waiting)) {
            const std::string file = scene_sound_file(sounds, v.name);
            if (file.empty()) {
                say("сцена «" + name + "»: музыки «" + v.name + "» нет в звуках игры");
                continue;
            }
            want.push_back({v.call, file, v.since, v.volume});
        }
        const vector::SceneMusicCall* first = &f.music.front();
        for (const vector::SceneMusicCall& c : f.music)
            if (c.at < first->at || (c.at == first->at && c.order < first->order)) first = &c;
        f64 game = 1;
        if (const f64 since = vector::scene_since(f, now, first->at, first->order); since >= 0)
            game = first->fade > 0 ? std::clamp(1 - since / first->fade, 0.0, 1.0) : 0.0;
        hear(std::move(want), game);
    }

    void fire(const vector::SceneMark& mk) {
        if (!mk.sound) {
            if (self->on_message) self->on_message(mk.name);
            return;
        }
        const std::string file = scene_sound_file(sounds, mk.name);
        if (file.empty()) say("сцена «" + name + "»: звука «" + mk.name + "» нет в звуках игры");
        else if (self->on_sound) self->on_sound(file);
    }

    // Its talk open now closes (it is skipped or dropped).
    void close_talk() {
        if (talk_begun && self->talking && self->talking() && self->end_talk) self->end_talk();
        talk_begun = false;
    }

    // The scene is over: its document goes, its music goes out while the game's comes back.
    void finish(bool skipped) {
        const u64 me = serial;
        const std::string whose = name;
        // What it had left to say, in order, as its time would have gone on from here without waiting; its sounds are not
        // heard.
        std::vector<std::string> left;
        if (skipped) {
            vector::SceneAt end = at();
            end.t = scene.file.length;
            end.waiting = -1;
            std::vector<vector::SceneMark> rest;
            std::string why;
            if (!vector::scene_marks_between(scene, has_was ? &was : nullptr, end, rest, why)) say("сцена «" + name + "»: " + why);
            for (const vector::SceneMark& m : rest)
                if (!m.sound) left.push_back(m.name);
        }
        close_talk();
        going = false;
        // Its last frame stays (part 6г4) till the logic has heard its end, at its next run: the next scene comes in over
        // it.
        close(last);
        close(last_prev);
        last = std::exchange(cur, {});
        last_prev = std::exchange(prev, {});
        end_runs = runs;
        forget_play();
        ends[me] = skipped;
        while (ends.size() > 64) ends.erase(ends.begin());
        if (!scene.file.music.empty()) {
            tail = true;
            tail_t = 0;
            tail_share = share;
            tail_tunes = tunes;
        }
        for (const std::string& m : left)
            if (self->on_message) self->on_message(m);
        if (self->on_end) self->on_end(whose, skipped, me);
    }

    void fit(int w, int h) {
        const f32 dp = context ? context->GetDensityIndependentPixelRatio() : 1.0f;
        if (w == fit_w && h == fit_h && dp == fit_dp) return;
        fit_w = w;
        fit_h = h;
        fit_dp = dp;
        const f64 cw = scene.file.width, ch = scene.file.height;
        const ScreenFit f = fit_screen("fit", static_cast<f32>(cw), static_cast<f32>(ch), static_cast<f32>(w) / dp, static_cast<f32>(h) / dp);
        fit_scale = f.sx;
        fit_left = f.left;
        fit_top = f.top;
        Rml::Element* frame = cur.frame;
        Rml::Element* bars = cur.bars;
        frame->SetProperty("left", px(f.left));
        frame->SetProperty("top", px(f.top));
        frame->SetProperty("width", px(cw * f.sx));
        frame->SetProperty("height", px(ch * f.sy));
        // The scissor's edges: the frame's, rounded to the window's pixels.
        const f64 x0 = std::round(f64(f.left) * dp), y0 = std::round(f64(f.top) * dp), x1 = std::round((f.left + cw * f.sx) * dp),
                  y1 = std::round((f.top + ch * f.sy) * dp);
        bars->SetProperty("border-left-width", px(x0 / dp));
        bars->SetProperty("border-top-width", px(y0 / dp));
        bars->SetProperty("border-right-width", px(std::max(0.0, w - x1) / dp));
        bars->SetProperty("border-bottom-width", px(std::max(0.0, h - y1) / dp));
        cur.camera->SetProperty("width", px(cw));
        cur.camera->SetProperty("height", px(ch));
        camera_transform.clear();
    }

    Rml::Element* add(Rml::Element* into, const std::string& src, f64 w, f64 h) {
        Rml::ElementPtr e = cur.doc->CreateElement("img");
        e->SetAttribute("src", src);
        e->SetProperty("width", px(w));
        e->SetProperty("height", px(h));
        return into->AppendChild(std::move(e));
    }
    // A picture or an actor in its own layer (part 6г4: its mask by a shape of the stage).
    Rml::Element* add_item(Layer& l, const std::string& src, f64 w, f64 h) {
        Rml::ElementPtr wrap = cur.doc->CreateElement("div");
        wrap->SetClass("item", true);
        l.wrap = cur.camera->AppendChild(std::move(wrap));
        return add(l.wrap, src, w, h);
    }

    // The box an actor is drawn in: where its drawing reaches, else its picture's own.
    static vector::SceneBox box_of(const vector::SceneActor& a) {
        if (a.reach.w > 0 && a.reach.h > 0) return a.reach;
        return {a.origin.x, a.origin.y, a.w, a.h};
    }

    // The layers the stage's top level makes, in its order.
    void build(const std::vector<ui::SvgPlace>& top) {
        std::vector<Layer> want;
        std::string words;
        const vector::SceneFile& f = scene.file;
        for (const ui::SvgPlace& p : top) {
            if (!p.draws || p.key.empty()) continue;
            Layer l;
            bool item = false;
            for (usize i = 0; i < f.pictures.size() && !item; ++i)
                if (f.pictures[i].id == p.id) {
                    item = true;
                    l.kind = Layer::Kind::Picture;
                    l.item = i;
                }
            for (usize i = 0; i < f.actors.size() && !item; ++i)
                if (f.actors[i].id == p.id) {
                    item = true;
                    if (p.key[0] == 'c') {
                        say("сцена «" + name + "»: копия актёра «" + p.id + "» в игре не рисуется");
                        l.item = ~usize(0);
                    } else {
                        l.kind = Layer::Kind::Actor;
                        l.item = i;
                    }
                }
            if (item) {
                if (l.item == ~usize(0)) continue;
                l.key = p.key;
                words += (l.kind == Layer::Kind::Picture ? "|p" : "|a") + p.key;
                want.push_back(std::move(l));
                continue;
            }
            if (want.empty() || want.back().kind != Layer::Kind::Run) {
                want.emplace_back();
                words += "|r";
            }
            want.back().keys.push_back(p.key);
            words += " " + p.key;
        }
        if (words == structure) return;
        structure = words;
        Rml::Element* camera = cur.camera;
        while (camera->GetNumChildren() > 0) camera->RemoveChild(camera->GetChild(camera->GetNumChildren() - 1));
        for (const auto& [picture, made] : cur.pictures)
            if (picture.find("-item") != std::string::npos) forget(picture);
        std::erase_if(cur.pictures, [](const auto& p) { return p.first.find("-item") != std::string::npos; });
        for (Layer& l : want) {
            if (l.kind == Layer::Kind::Run) l.element = add(camera, "stage.svg", f.width, f.height);
            else if (l.kind == Layer::Kind::Picture) {
                const vector::ScenePicture& pic = f.pictures[l.item];
                l.element = add_item(l, pic.file, pic.w, pic.h);
            } else {
                const vector::SceneActor& a = f.actors[l.item];
                const vector::SceneBox b = box_of(a);
                l.element = add_item(l, a.picture, b.w, b.h);
            }
        }
        layers = std::move(want);
    }

    void place_item(Layer& l, const ui::SvgPlace& p, const vector::MotionMatrix& m) {
        const std::string tr = transform_of(m);
        if (tr != l.transform) {
            l.transform = tr;
            l.element->SetProperty("transform", tr);
        }
        if (p.opacity != l.opacity) {
            l.opacity = p.opacity;
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.5f", p.opacity);
            l.element->SetProperty("opacity", buf);
        }
        if (p.shown != l.shown) {
            l.shown = p.shown;
            if (p.shown) l.element->RemoveProperty("display");
            else l.element->SetProperty("display", "none");
        }
    }

    static void show(Rml::Element* e, bool on, bool& was) {
        if (!e || on == was) return;
        was = on;
        if (on) e->RemoveProperty("display");
        else e->SetProperty("display", "none");
    }

    bool is_item(std::string_view id) const {
        const vector::SceneFile& f = scene.file;
        return std::any_of(f.pictures.begin(), f.pictures.end(), [&](const vector::ScenePicture& p) { return p.id == id; }) ||
               std::any_of(f.actors.begin(), f.actors.end(), [&](const vector::SceneActor& a) { return a.id == id; });
    }

    // A part's mask as the game draws it (part 6г4): by a shape of the stage, its shape and whether it shows outside it.
    // None by a picture or an actor («маска по актёру или картинке», named once at play).
    bool mask_of(const vector::MotionPartPose& pp, std::string& target, bool& invert) const {
        const usize m = static_cast<usize>(vector::MotionProp::Mask);
        if (!pp.has[m] || !(pp.values[m].v > 0) || pp.values[m].target.empty() || is_item(pp.values[m].target)) return false;
        target = pp.values[m].target;
        invert = pp.values[m].v > 1.5;
        return true;
    }

    // Picture `name` of code: shape `target`'s area as the tab's mask takes it, drawn w × h through m (outside it when
    // invert), white with its alpha premultiplied; made again only when what it shows changed. It is used this frame.
    void make_mask(const std::string& name, const std::string& target, const vector::MotionMatrix& m, u32 w, u32 h, bool invert,
                   std::set<std::string>& used) {
        used.insert(name);
        char buf[240];
        std::snprintf(buf, sizeof buf, "%.9g %.9g %.9g %.9g %.9g %.9g %u %u %d|", m.a, m.b, m.c, m.d, m.e, m.f, w, h, invert ? 1 : 0);
        const std::string made = buf + places.area_shapes(target);
        std::string& was = cur.pictures[name];
        if (was == made) return;
        was = made;
        if (!ui) return;
        std::vector<u8> cover;
        if (!places.area(target, m, w, h, cover)) cover.assign(static_cast<usize>(w) * h, 0); // no shape: nothing inside it
        std::vector<u8> rgba(cover.size() * 4);
        for (usize i = 0; i < cover.size(); ++i) {
            const u8 c = invert ? static_cast<u8>(255 - cover[i]) : cover[i];
            std::fill_n(rgba.begin() + static_cast<std::ptrdiff_t>(i * 4), 4, c);
        }
        ui->set_gpu_image(name, rgba.data(), w, h);
    }

    // A frame of a transition, shown or not, in its track's pose (pp null: where it stands, whole): moved by its x, y in
    // the canvas's units as the screen is to the canvas, turned and scaled round the screen's middle; faded and masked
    // as a whole on its wrap, so the mask stands while the frame moves.
    void set_frame(Frame& fr, bool shown, const vector::MotionPartPose* pp, const std::string& mask) {
        show(fr.wrap, shown, fr.shown);
        if (!shown) return;
        using vector::MotionProp;
        auto value = [&](MotionProp p) {
            const usize i = static_cast<usize>(p);
            const f64 v = pp && pp->has[i] ? pp->values[i].v : vector::motion_rest(p).v;
            return std::isfinite(v) ? v : vector::motion_rest(p).v;
        };
        const f64 x = value(MotionProp::X), y = value(MotionProp::Y), rot = value(MotionProp::Rot), scale = value(MotionProp::Scale);
        const f64 opacity = std::clamp(value(MotionProp::Opacity), 0.0, 1.0);
        std::string tr;
        if (x != 0 || y != 0 || rot != 0 || scale != 1) {
            const f64 dp = fit_dp > 0 ? fit_dp : 1.0;
            char buf[200];
            std::snprintf(buf, sizeof buf, "translate(%.4fpx, %.4fpx) rotate(%.6fdeg) scale(%.7f)", x * fit_w / dp / scene.file.width,
                          y * fit_h / dp / scene.file.height, rot, scale);
            tr = buf;
        }
        if (tr != fr.transform) {
            fr.transform = tr;
            if (tr.empty()) fr.move->RemoveProperty("transform");
            else fr.move->SetProperty("transform", tr);
        }
        std::string filter;
        if (opacity < 1) {
            char buf[40];
            std::snprintf(buf, sizeof buf, "opacity(%.5f)", opacity);
            filter = buf;
        }
        // A filter fades it as one picture (an element's own opacity would fade each of its layers over the others).
        if (filter != fr.filter) {
            fr.filter = filter;
            if (filter.empty()) fr.wrap->RemoveProperty("filter");
            else fr.wrap->SetProperty("filter", filter);
        }
        if (mask != fr.mask) {
            fr.mask = mask;
            if (mask.empty()) fr.wrap->RemoveProperty("mask-image");
            else fr.wrap->SetProperty("mask-image", mask);
        }
    }

    // A picture's or an actor's mask by a shape of the stage (part 6г4), in the camera's layer: its wrap the shape's box
    // as far as the camera sees (outside the shape: all the camera sees), the area's picture over it at its size on the
    // screen (in steps of the raster's scale, as a picture of live SVG), the layer in its place inside. Unmasked, as 6г2.
    void mask_item(Layer& l, usize index, const vector::MotionPartPose* pp, const f64 view[4], f64 zoom, std::set<std::string>& used) {
        std::string target, wrapped, inside;
        bool invert = false;
        if (pp && mask_of(*pp, target, invert)) {
            f64 bx = view[0], by = view[1], bw = view[2] - view[0], bh = view[3] - view[1];
            if (!invert) {
                f64 x = 0, y = 0, w = 0, h = 0;
                if (places.area_box(target, x, y, w, h)) {
                    bx = std::max(x, view[0]), by = std::max(y, view[1]);
                    bw = std::min(x + w, view[2]) - bx, bh = std::min(y + h, view[3]) - by;
                } else bw = bh = 0;
            }
            if (bw > 0 && bh > 0 && std::isfinite(bw) && std::isfinite(bh)) {
                f64 k = Rml::PictureScale::Step(static_cast<float>(fit_scale * (fit_dp > 0 ? fit_dp : 1.0f) * zoom));
                if (std::max(bw, bh) * k > 4096) k = 4096 / std::max(bw, bh);
                const u32 rw = static_cast<u32>(std::clamp(std::ceil(bw * k - 1e-6), 1.0, 4096.0));
                const u32 rh = static_cast<u32>(std::clamp(std::ceil(bh * k - 1e-6), 1.0, 4096.0));
                bw = rw / k, bh = rh / k;
                const std::string name = "scene-" + std::to_string(cur.serial) + "-item" + std::to_string(index);
                make_mask(name, target, {k, 0, 0, k, -bx * k, -by * k}, rw, rh, invert, used);
                wrapped = px(bx) + " " + px(by) + " " + px(bw) + " " + px(bh) + " " + name;
                inside = px(-bx) + " " + px(-by);
            } else wrapped = "none"; // nothing of it is seen
        }
        if (wrapped != l.wrapped) {
            l.wrapped = wrapped;
            Rml::Element* w = l.wrap;
            if (wrapped == "none") w->SetProperty("display", "none");
            else w->RemoveProperty("display");
            if (wrapped.empty() || wrapped == "none") {
                for (const char* p : {"left", "top", "width", "height", "mask-image"}) w->RemoveProperty(p);
            } else {
                std::istringstream in(wrapped);
                std::string x, y, ww, hh, name;
                in >> x >> y >> ww >> hh >> name;
                w->SetProperty("left", x);
                w->SetProperty("top", y);
                w->SetProperty("width", ww);
                w->SetProperty("height", hh);
                w->SetProperty("mask-image", "url(/gpu/" + name + ")");
            }
        }
        if (inside != l.inside) {
            l.inside = inside;
            if (inside.empty()) {
                l.element->RemoveProperty("left");
                l.element->RemoveProperty("top");
            } else {
                const usize at = inside.find(' ');
                l.element->SetProperty("left", inside.substr(0, at));
                l.element->SetProperty("top", inside.substr(at + 1));
            }
        }
    }

    // The scene's frame at its moment t.
    void draw(int width, int height) {
        if (!cur.doc) return;
        fit(width, height);
        const vector::SceneFile& f = scene.file;
        const vector::SceneAt now = at();
        const vector::ScenePose pose = vector::scene_pose(scene, t, now.held, now.waiting);
        const vector::MotionMatrix cam = vector::scene_camera_matrix(pose.camera, f.width, f.height);
        char buf[220];
        std::snprintf(buf, sizeof buf, "scale(%.7f) translate(%.4fpx, %.4fpx) matrix(%.7f, %.7f, %.7f, %.7f, 0, 0)", fit_scale, cam.e, cam.f, cam.a,
                      cam.b, cam.c, cam.d);
        if (camera_transform != buf) {
            camera_transform = buf;
            cur.camera->SetProperty("transform", camera_transform);
        }

        ui::SvgPose stage;
        stage.picture = "stage.svg";
        stage.parts = pose.stage;
        stage.copies = pose.copies;
        stage.order = pose.order;
        // Part 6г4: the areas of shapes this frame masks by, its transition's frames' and its pictures' and actors' (and
        // their copies').
        std::map<std::string, const vector::MotionPartPose*> parts;
        for (const vector::MotionPartPose& pp : pose.stage) parts[pp.id] = &pp;
        std::vector<std::string> areas;
        auto want = [&](const vector::MotionPartPose& pp) {
            std::string target;
            bool invert = false;
            if (mask_of(pp, target, invert) && std::find(areas.begin(), areas.end(), target) == areas.end()) areas.push_back(target);
        };
        for (const vector::MotionPartPose& fr : pose.frames) want(fr);
        for (const vector::MotionPartPose& pp : pose.stage) {
            bool item = is_item(pp.id);
            for (const vector::MotionCopy& c : pose.copies)
                if (c.id == pp.id) item = item || is_item(c.source);
            if (item) want(pp);
        }
        const std::vector<ui::SvgPlace> top = places.place(stage, areas);
        build(top);
        // What the camera sees, in the stage's units: the box round the canvas brought back through it.
        const vector::MotionMatrix back = vector::inverse(cam);
        f64 view[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
        for (const vector::Point c : {vector::Point{0, 0}, vector::Point{f.width, 0}, vector::Point{0, f.height}, vector::Point{f.width, f.height}}) {
            const vector::Point q = back.apply(c);
            view[0] = std::min(view[0], q.x), view[1] = std::min(view[1], q.y);
            view[2] = std::max(view[2], q.x), view[3] = std::max(view[3], q.y);
        }
        const f64 zoom = std::hypot(cam.a, cam.b);
        std::set<std::string> used;
        for (usize li = 0; li < layers.size(); ++li) {
            Layer& l = layers[li];
            if (l.kind == Layer::Kind::Run) {
                ui::SvgPose run = places.run_pose(stage, l.keys);
                if (!l.has_pose || run != l.posed) {
                    l.posed = run;
                    l.has_pose = true;
                    ui::svg_pose(l.element, std::move(run));
                    ++run_poses;
                }
                continue;
            }
            const auto at = std::find_if(top.begin(), top.end(), [&](const ui::SvgPlace& p) { return p.key == l.key; });
            if (at == top.end()) continue;
            const std::string id = l.key[0] == 'c'                     ? l.key.substr(1)
                                   : l.kind == Layer::Kind::Picture ? f.pictures[l.item].id
                                                                    : f.actors[l.item].id;
            const auto part = parts.find(id);
            mask_item(l, li, part != parts.end() ? part->second : nullptr, view, zoom, used);
            if (l.kind == Layer::Kind::Picture) {
                place_item(l, *at, at->m);
                continue;
            }
            const vector::SceneActor& a = f.actors[l.item];
            const vector::SceneBox b = box_of(a);
            vector::MotionMatrix m = at->m;
            m.e += m.a * b.x + m.c * b.y;
            m.f += m.b * b.x + m.d * b.y;
            place_item(l, *at, m);
            const vector::SceneActorPose& ap = pose.actors[l.item];
            ui::SvgPose sp;
            sp.picture = path_to_utf8(utf8_path(a.picture).filename());
            sp.parts = ap.parts;
            sp.copies = ap.copies;
            sp.order = ap.order;
            sp.drawings = drawings[l.item];
            sp.drawing = ap.shown.drawing;
            if (ap.shown.swap) {
                sp.swap = true;
                sp.swap_from = ap.shown.from;
                sp.swap_k = ap.shown.k;
                sp.from_parts = ap.from_parts;
            }
            sp.view_x = b.x, sp.view_y = b.y, sp.view_w = b.w, sp.view_h = b.h;
            if (!l.has_pose || sp != l.posed) {
                l.posed = sp;
                l.has_pose = true;
                ui::svg_pose(l.element, std::move(sp));
            }
        }

        // Part 6г4: the frames of its transition by its phase. Coming in, over the game's frame (its snapshot) or over the
        // last frame of the scene before it, which stands as the old frame till it is in; going out, the game's live frame
        // over it; after its leave, nothing of it.
        using Phase = vector::ScenePhase;
        const Phase phase = pose.phase;
        cur.phase = phase;
        if (prev.doc && phase != Phase::Enter) close(prev);
        const bool over_prev = prev.doc != nullptr;
        const bool enter = phase == Phase::Enter, leave = phase == Phase::Leave;
        const bool game = (enter && !over_prev) || leave;
        auto mask = [&](usize i) -> std::string {
            std::string target;
            bool invert = false;
            if (fit_w <= 0 || fit_h <= 0 || !mask_of(pose.frames[i], target, invert)) return {};
            // The shape as the camera sees it on the screen, in its pixels.
            const f64 dp = fit_dp > 0 ? fit_dp : 1.0, k = fit_scale * dp;
            const vector::MotionMatrix screen = vector::MotionMatrix{k, 0, 0, k, fit_left * dp, fit_top * dp} * cam;
            const std::string name = "scene-" + std::to_string(cur.serial) + "-frame" + std::to_string(i);
            make_mask(name, target, screen, static_cast<u32>(fit_w), static_cast<u32>(fit_h), invert, used);
            return "url(/gpu/" + name + ")";
        };
        show(cur.backdrop, game, cur.backdrop_on);
        show(cur.black, game, cur.black_on);
        set_frame(cur.below, enter && !over_prev, &pose.frames[0], enter && !over_prev ? mask(0) : std::string());
        set_frame(cur.look, phase != Phase::Gone, enter ? &pose.frames[1] : leave ? &pose.frames[2] : nullptr,
                  enter ? mask(1) : leave ? mask(2) : std::string());
        set_frame(cur.above, leave, &pose.frames[3], leave ? mask(3) : std::string());
        if (over_prev) {
            show(prev.backdrop, false, prev.backdrop_on);
            show(prev.black, true, prev.black_on);
            set_frame(prev.below, false, nullptr, {});
            set_frame(prev.above, false, nullptr, {});
            set_frame(prev.look, true, &pose.frames[0], mask(0));
        }
        // The pictures of code this frame does not show go.
        for (auto it = cur.pictures.begin(); it != cur.pictures.end();) {
            if (used.count(it->first)) {
                ++it;
                continue;
            }
            forget(it->first);
            it = cur.pictures.erase(it);
        }
    }
};

GameScene::GameScene() : impl_(std::make_unique<Impl>()) { impl_->self = this; }

GameScene::~GameScene() { impl_->close_all(); }

bool GameScene::play(Rml::Context* context, const fs::path& game_dir, std::string_view name, const SceneHow& how, std::string& why) {
    Impl& im = *impl_;
    if (im.going) return why = "идёт сцена «" + im.name + "»", false;
    const std::string n(name);
    const fs::path dir = game_dir / "scenes" / utf8_path(n);
    std::error_code ec;
    if (n.empty() || n == "." || n == ".." || n.find_first_of("/\\:") != std::string::npos || !fs::is_regular_file(dir / "scene.json", ec))
        return why = "нет сцены «" + n + "»: отдайте её игре во вкладке «Векторная»", false;
    vector::SceneMotion scene;
    std::string err;
    if (!read_scene_folder(dir, scene, err)) return why = "сцена «" + n + "»: " + err, false;
    std::vector<std::vector<ui::SvgDrawing>> drawings(scene.actors.size());
    for (usize a = 0; a < scene.actors.size(); ++a) {
        const fs::path beside = (dir / utf8_path(scene.file.actors[a].picture)).parent_path();
        for (const vector::MotionDrawing& d : scene.actors[a].drawings) {
            std::string text;
            if (!read_text_file(beside / utf8_path(d.picture), text))
                return why = "сцена «" + n + "»: актёр «" + scene.file.actors[a].id + "»: «" + d.picture + "» не читается", false;
            drawings[a].push_back({d.name, d.picture, std::make_shared<const std::string>(std::move(text))});
        }
    }
    std::vector<vector::SceneMark> marks;
    if (!vector::scene_marks(scene, marks, err)) return why = "сцена «" + n + "»: " + err, false;
    std::string stage;
    if (!read_text_file(dir / "stage.svg", stage)) return why = "сцена «" + n + "»: «stage.svg» не читается", false;
    ui::SvgPlaces& places = im.places;
    if (!places.load(path_to_utf8(dir / "stage.svg"), std::move(stage), err)) return why = "сцена «" + n + "»: stage.svg: " + err, false;

    // The music of the one before going out after its end goes now.
    im.tail = false;
    im.close(im.cur);
    im.close(im.prev);
    im.forget_play();
    std::string url = path_to_utf8(dir / "scene.rml");
    std::replace(url.begin(), url.end(), '\\', '/');
    std::replace(url.begin(), url.end(), ':', '|');
    // Part 6г4: the snapshot of the game's frame under it, black, the game's frame below the scene's, the scene's (its
    // background all round, the camera's frame, the bars) and the game's frame above it: only the scene's is shown out of
    // a transition.
    const u64 serial = im.serials + 1;
    const std::string game = game_picture(serial);
    const std::string game_frame = "<div class=\"full move\"><img class=\"full\" src=\"/gpu/" + game + "\"></img></div>";
    const std::string rml = std::string("<rml><head><title>Сцена</title><style>") + kStyle + "</style></head><body>" +
                            "<backdrop id=\"backdrop\" class=\"full\" name=\"" + game + "\" style=\"display: none;\"></backdrop>" +
                            "<div id=\"black\" class=\"full\" style=\"display: none;\"></div>" +
                            "<div id=\"below\" class=\"full wrap\" style=\"display: none;\">" + game_frame + "</div>" +
                            "<div id=\"look-wrap\" class=\"full wrap\"><div id=\"look\" class=\"full move\"><div id=\"bg\" class=\"full\"></div>" +
                            "<div id=\"frame\"><div id=\"camera\"></div></div><div id=\"bars\"></div></div></div>" +
                            "<div id=\"above\" class=\"full wrap\" style=\"display: none;\">" + game_frame + "</div></body></rml>";
    im.context = context;
    Impl::Shown& s = im.cur;
    s.doc = context ? context->LoadDocumentFromMemory(rml, url) : nullptr;
    if (!s.doc) return why = "сцена «" + n + "»: её документ не построился", false;
    s.serial = serial;
    s.backdrop = s.doc->GetElementById("backdrop");
    s.black = s.doc->GetElementById("black");
    s.bg = s.doc->GetElementById("bg");
    s.frame = s.doc->GetElementById("frame");
    s.camera = s.doc->GetElementById("camera");
    s.bars = s.doc->GetElementById("bars");
    s.below = {s.doc->GetElementById("below"), s.doc->GetElementById("below")->GetChild(0)};
    s.look = {s.doc->GetElementById("look-wrap"), s.doc->GetElementById("look")};
    s.look.shown = true;
    s.above = {s.doc->GetElementById("above"), s.doc->GetElementById("above")->GetChild(0)};
    if (!scene.file.background.empty()) {
        s.bg->SetProperty("background-color", scene.file.background);
        s.bars->SetProperty("border-color", scene.file.background);
    } else {
        s.bg->SetProperty("display", "none");
        s.bars->SetProperty("display", "none");
    }
    s.doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    // The last frame of the one just over is its old frame when it comes in from its start and that one did not go out to
    // the game; else it goes now.
    im.close(im.last_prev);
    if (im.last.doc && vector::scene_phase(scene.file, 0) == vector::ScenePhase::Enter && im.last.phase != vector::ScenePhase::Gone)
        im.prev = std::exchange(im.last, {});
    else im.close(im.last);

    im.dir = dir;
    im.sounds = game_dir / "sounds";
    im.name = n;
    im.how = how;
    im.scene = std::move(scene);
    im.drawings = std::move(drawings);
    im.t = im.clock = 0;
    im.held.assign(im.scene.file.stops.size(), 0);
    im.waiting = -1;
    im.next_stop = 0;
    im.talk_begun = false;
    im.has_was = false;
    im.going = true;
    im.fresh = true;
    im.serial = ++im.serials; // the document's

    im.run_poses = 0;
    // What the game will not draw of it, once.
    std::string left;
    for (const std::string& w : im.scene.file.not_drawn)
        left += (left.empty() ? "" : ", ") + std::string(w == "mask_by_item" ? "маска по актёру или картинке" : vector::motion_not_drawn_words(w));
    if (!left.empty()) im.say("сцена «" + n + "»: в игре не будет: " + left);
    return true;
}

void GameScene::update(f64 dt, int width, int height) {
    Impl& im = *impl_;
    if (!std::isfinite(dt) || dt < 0) dt = 0;
    // Part 6г4: the last frame of the one over was held till the logic ran; nothing came in over it.
    if (!im.runs_told) ++im.runs;
    if (im.last.doc && im.runs != im.end_runs) {
        im.close(im.last);
        im.close(im.last_prev);
    }
    if (im.tail) {
        im.tail_t += dt;
        const f64 k = std::clamp(im.tail_t / kMusicBack, 0.0, 1.0);
        std::vector<SceneTune> out = im.tail_tunes;
        for (SceneTune& t : out) t.volume *= 1 - k;
        if (k >= 1) {
            im.tail = false;
            out.clear();
        }
        im.hear(std::move(out), im.tail_share + (1 - im.tail_share) * k);
    }
    if (!im.going) return;
    const bool first = im.fresh;
    if (im.fresh) {
        im.fresh = false;
        dt = 0;
    }
    const u64 me = im.serial;
    // A frame of no time (the pause menu up) does not come to the next stop either: a talk could not open over the menu.
    if (first || dt > 0) im.advance(dt);
    // Its marks passed since the last frame (part 6г3: by the time it lived, its waits counted).
    const vector::SceneAt now = im.at();
    std::vector<vector::SceneMark> passed;
    std::string why;
    if (!vector::scene_marks_between(im.scene, im.has_was ? &im.was : nullptr, now, passed, why)) {
        im.say("сцена «" + im.name + "»: " + why);
        passed.clear();
    }
    im.was = now;
    im.has_was = true;
    for (const vector::SceneMark& mk : passed) {
        im.fire(mk);
        if (!im.going || im.serial != me) return; // its logic stopped it, or began another
    }
    im.music_now();
    im.draw(width, height);
    if (im.t >= im.scene.file.length && im.waiting < 0 && im.next_stop >= im.scene.file.stops.size()) im.finish(false);
}

void GameScene::skip() {
    if (impl_->going) impl_->finish(true);
}

void GameScene::drop() {
    Impl& im = *impl_;
    if (im.going) {
        im.close_talk();
        im.ends[im.serial] = false;
        im.going = false;
    }
    im.close_all();
    im.tail = false;
    im.hear({}, 1);
}

bool GameScene::going() const { return impl_->going; }
const std::string& GameScene::name() const { return impl_->name; }
const SceneHow& GameScene::how() const { return impl_->how; }
f64 GameScene::time() const { return impl_->t; }
const vector::SceneStop* GameScene::waiting() const {
    const Impl& im = *impl_;
    return im.going && im.waiting >= 0 ? &im.scene.file.stops[static_cast<usize>(im.waiting)] : nullptr;
}
i32 GameScene::waiting_index() const { return impl_->going ? impl_->waiting : -1; }
void GameScene::go_on() {
    if (impl_->going) impl_->go_on();
}
std::vector<f64> GameScene::held() const { return impl_->at().held; }
f64 GameScene::lived() const { return vector::scene_lived(impl_->at()); }
void GameScene::note(const std::string& line) { impl_->say(line); }
u64 GameScene::serial() const { return impl_->serial; }
bool GameScene::over(u64 serial) const { return !(impl_->going && impl_->serial == serial); }
bool GameScene::skipped(u64 serial) const {
    const auto it = impl_->ends.find(serial);
    return it != impl_->ends.end() && it->second;
}
Rml::ElementDocument* GameScene::document() const { return impl_->cur.doc ? impl_->cur.doc : impl_->last.doc; }
std::vector<Rml::ElementDocument*> GameScene::documents() const {
    std::vector<Rml::ElementDocument*> out;
    for (const Impl::Shown* s : {&impl_->last_prev, &impl_->last, &impl_->prev, &impl_->cur})
        if (s->doc) out.push_back(s->doc);
    return out;
}
bool GameScene::wants_backdrop() const {
    for (const Impl::Shown* s : {&impl_->last_prev, &impl_->last, &impl_->prev, &impl_->cur})
        if (s->doc && s->backdrop_on) return true;
    return false;
}
void GameScene::set_ui(ui::Ui* ui) { impl_->ui = ui; }
void GameScene::logic_ran(u64 runs) {
    impl_->runs_told = true;
    impl_->runs = runs;
}
const std::vector<std::string>& GameScene::problems() const { return impl_->problems; }
u64 GameScene::run_poses() const { return impl_->run_poses; }

std::vector<std::string> GameScene::layers() const {
    std::vector<std::string> out;
    for (const Impl::Layer& l : impl_->layers) {
        if (l.kind == Impl::Layer::Kind::Run) {
            std::string s = "run";
            for (const std::string& k : l.keys) s += " " + k;
            out.push_back(std::move(s));
        } else if (l.kind == Impl::Layer::Kind::Picture) out.push_back("picture " + impl_->scene.file.pictures[l.item].id + " " + l.key);
        else out.push_back("actor " + impl_->scene.file.actors[l.item].id);
    }
    return out;
}

} // namespace forge::game
