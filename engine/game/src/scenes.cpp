#include "forge/game/scenes.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/game/screens.h"
#include "forge/ui/live_svg.h"
#include "forge/vector/motion_file.h"
#include "forge/vector/svg.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

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
                               "div, img { display: block; position: absolute; left: 0px; top: 0px; pointer-events: none;"
                               " transform-origin: 0px 0px 0px; }"
                               // Always: its layers are moved by transforms, which the layout does not count as overflow (a scene
                               // fitted larger than its canvas spilled over the bars).
                               "#frame { overflow: hidden; clip: always; }"
                               // The bars over it: the frame's edge on whole pixels of the window (the software Vulkan lets
                               // a column of a scissor's edge through with MSAA).
                               "#bars { width: 100%; height: 100%; box-sizing: border-box; border-width: 0px; }";

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
    std::vector<vector::SceneMark> marks;
    usize next_mark = 0;
    // Its moment: the clock (the frames' steps summed) read to the nanosecond, so 90 steps of 1/60 are 1.5, where the tab's
    // frame 90 has a clip begun, and not 1.4999999999999996, a hair before it.
    f64 t = 0, clock = 0;
    bool going = false, fresh = false;
    u64 serial = 0, serials = 0;
    std::map<u64, bool> ends; // the plays over: skipped?

    Rml::ElementDocument* doc = nullptr;
    Rml::Element* frame = nullptr;
    Rml::Element* camera = nullptr;
    Rml::Element* bars = nullptr;
    ui::SvgPlaces places;
    std::vector<Layer> layers;
    std::string structure;
    int fit_w = -1, fit_h = -1;
    f32 fit_dp = 0;
    f64 fit_scale = 1;
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

    void close() {
        if (doc && context) context->UnloadDocument(doc);
        doc = nullptr;
        frame = camera = bars = nullptr;
        layers.clear();
        structure.clear();
        camera_transform.clear();
        fit_w = fit_h = -1;
    }

    void hear(std::vector<SceneTune> want, f64 game_share) {
        if (want == tunes && game_share == share) return;
        tunes = std::move(want);
        share = game_share;
        if (self->on_music) self->on_music(tunes, share);
    }

    // The scene's tunes at its moment t and how loud the game's music is under them: as it is until the first call of
    // `music`, then going out over that call's fade.
    void music_now() {
        const vector::SceneFile& f = scene.file;
        if (f.music.empty()) return;
        std::vector<SceneTune> want;
        for (const vector::SceneMusicVoice& v : vector::scene_music_at(f, t)) {
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
        if (t >= first->at) game = first->fade > 0 ? std::clamp(1 - (t - first->at) / first->fade, 0.0, 1.0) : 0.0;
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

    // The scene is over: its document goes, its music goes out while the game's comes back.
    void finish(bool skipped) {
        const u64 me = serial;
        const std::string was = name;
        going = false;
        close();
        ends[me] = skipped;
        while (ends.size() > 64) ends.erase(ends.begin());
        if (!scene.file.music.empty()) {
            tail = true;
            tail_t = 0;
            tail_share = share;
            tail_tunes = tunes;
        }
        if (skipped) {
            // What it had left to say, in order; its sounds are not heard.
            std::vector<std::string> left;
            for (usize i = next_mark; i < marks.size(); ++i)
                if (!marks[i].sound) left.push_back(marks[i].name);
            next_mark = marks.size();
            for (const std::string& m : left)
                if (self->on_message) self->on_message(m);
        }
        if (self->on_end) self->on_end(was, skipped, me);
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
        camera->SetProperty("width", px(cw));
        camera->SetProperty("height", px(ch));
        camera_transform.clear();
    }

    Rml::Element* add(const char* tag, const std::string& src, f64 w, f64 h) {
        Rml::ElementPtr e = doc->CreateElement(tag);
        if (!src.empty()) e->SetAttribute("src", src);
        e->SetProperty("width", px(w));
        e->SetProperty("height", px(h));
        return camera->AppendChild(std::move(e));
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
        while (camera->GetNumChildren() > 0) camera->RemoveChild(camera->GetChild(camera->GetNumChildren() - 1));
        for (Layer& l : want) {
            if (l.kind == Layer::Kind::Run) l.element = add("img", "stage.svg", f.width, f.height);
            else if (l.kind == Layer::Kind::Picture) {
                const vector::ScenePicture& pic = f.pictures[l.item];
                l.element = add("img", pic.file, pic.w, pic.h);
            } else {
                const vector::SceneActor& a = f.actors[l.item];
                const vector::SceneBox b = box_of(a);
                l.element = add("img", a.picture, b.w, b.h);
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

    // The scene's frame at its moment t.
    void draw(int width, int height) {
        if (!doc) return;
        fit(width, height);
        const vector::SceneFile& f = scene.file;
        const vector::ScenePose pose = vector::scene_pose(scene, t);
        const vector::MotionMatrix cam = vector::scene_camera_matrix(pose.camera, f.width, f.height);
        char buf[220];
        std::snprintf(buf, sizeof buf, "scale(%.7f) translate(%.4fpx, %.4fpx) matrix(%.7f, %.7f, %.7f, %.7f, 0, 0)", fit_scale, cam.e, cam.f, cam.a,
                      cam.b, cam.c, cam.d);
        if (camera_transform != buf) {
            camera_transform = buf;
            camera->SetProperty("transform", camera_transform);
        }

        ui::SvgPose stage;
        stage.picture = "stage.svg";
        stage.parts = pose.stage;
        stage.copies = pose.copies;
        stage.order = pose.order;
        // A mask of a picture or an actor by a shape of the stage: part 6г4.
        for (const vector::MotionPartPose& pp : pose.stage) {
            const usize mask = static_cast<usize>(vector::MotionProp::Mask);
            if (!pp.has[mask] || !(pp.values[mask].v > 0)) continue;
            const bool item = std::any_of(f.pictures.begin(), f.pictures.end(), [&](const vector::ScenePicture& p) { return p.id == pp.id; }) ||
                              std::any_of(f.actors.begin(), f.actors.end(), [&](const vector::SceneActor& a) { return a.id == pp.id; });
            if (item) say("сцена «" + name + "»: маска «" + pp.id + "» по фигуре сцены в игре пока не рисуется, «" + pp.id + "» виден целиком");
        }
        const std::vector<ui::SvgPlace> top = places.place(stage);
        build(top);
        for (Layer& l : layers) {
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
    }
};

GameScene::GameScene() : impl_(std::make_unique<Impl>()) { impl_->self = this; }

GameScene::~GameScene() { impl_->close(); }

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
    im.close();
    std::string url = path_to_utf8(dir / "scene.rml");
    std::replace(url.begin(), url.end(), '\\', '/');
    std::replace(url.begin(), url.end(), ':', '|');
    const std::string rml = std::string("<rml><head><title>Сцена</title><style>") + kStyle +
                            "</style></head><body><div id=\"frame\"><div id=\"camera\"></div></div><div id=\"bars\"></div></body></rml>";
    im.context = context;
    im.doc = context ? context->LoadDocumentFromMemory(rml, url) : nullptr;
    if (!im.doc) return why = "сцена «" + n + "»: её документ не построился", false;
    im.frame = im.doc->GetElementById("frame");
    im.camera = im.doc->GetElementById("camera");
    im.bars = im.doc->GetElementById("bars");
    if (!scene.file.background.empty()) {
        im.doc->SetProperty("background-color", scene.file.background);
        im.bars->SetProperty("border-color", scene.file.background);
    } else {
        im.bars->SetProperty("display", "none");
    }
    im.doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);

    im.dir = dir;
    im.sounds = game_dir / "sounds";
    im.name = n;
    im.how = how;
    im.scene = std::move(scene);
    im.drawings = std::move(drawings);
    im.marks = std::move(marks);
    im.next_mark = 0;
    im.t = im.clock = 0;
    im.going = true;
    im.fresh = true;
    im.serial = ++im.serials;
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
    if (im.fresh) {
        im.fresh = false;
        dt = 0;
    }
    const f64 length = im.scene.file.length;
    im.clock = std::min(im.clock + dt, length);
    im.t = im.clock < length ? std::min(std::round(im.clock * 1e9) / 1e9, length) : length;
    const u64 me = im.serial;
    while (im.going && im.serial == me && im.next_mark < im.marks.size() && im.marks[im.next_mark].at <= im.t + 1e-9) {
        const vector::SceneMark mk = im.marks[im.next_mark++];
        im.fire(mk);
    }
    if (!im.going || im.serial != me) return; // its logic stopped it, or began another
    im.music_now();
    im.draw(width, height);
    if (im.t >= im.scene.file.length) im.finish(false);
}

void GameScene::skip() {
    if (impl_->going) impl_->finish(true);
}

void GameScene::drop() {
    Impl& im = *impl_;
    if (im.going) {
        im.ends[im.serial] = false;
        im.going = false;
    }
    im.close();
    im.tail = false;
    im.hear({}, 1);
}

bool GameScene::going() const { return impl_->going; }
const std::string& GameScene::name() const { return impl_->name; }
const SceneHow& GameScene::how() const { return impl_->how; }
f64 GameScene::time() const { return impl_->t; }
u64 GameScene::serial() const { return impl_->serial; }
bool GameScene::over(u64 serial) const { return !(impl_->going && impl_->serial == serial); }
bool GameScene::skipped(u64 serial) const {
    const auto it = impl_->ends.find(serial);
    return it != impl_->ends.end() && it->second;
}
Rml::ElementDocument* GameScene::document() const { return impl_->doc; }
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
