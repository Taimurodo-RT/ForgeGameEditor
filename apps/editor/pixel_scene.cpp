// «Векторная», part 6а of docs/models/сцены-модель.md «Сцена и актёры»: a scene open in the tab in place of a character
// (pixel_editor.h). Its code is the panel's and its stage the vector's: the scene is clip «scene» of its code
// (PoseScript::set_scene), recorded, laid out on the timeline and edited by the mouse as a character's clip is; a frame
// poses the stage and its actors (pixel::Scene::pose). The rest of the mode is pixel_editor.cpp.
// Part 6б «Камера и переходы»: the camera's frame over the vector and its edge dragged, its row of the tree, the player's
// frame through it for the small view and the snapshot, the row of the transitions on the timeline.

#include "pixel_editor.h"
#include "pixel_files.h"

#include "asset_library.h"

#include "forge/assets/asset_pipeline.h"
#include "forge/assets/image.h"
#include "forge/data/json.h"
#include "forge/pixel/motion_edit.h"

#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

namespace forge::editor_app {

using namespace pixel_files;

namespace {

constexpr const char* kSceneSuffix = ".scene.json";
constexpr f64 kMaxSnapshotSide = 4096; // a snapshot of a scene at its own size
constexpr f32 kFrameReach = 4;         // how near the camera's frame's edge a press takes it (view points)
constexpr u8 kFrameLine[4] = {255, 255, 255, 255}, kFrameEdge[4] = {30, 30, 36, 255}, kFrameChosen[4] = {40, 120, 255, 255};

// "Вступление.scene.json" → "Вступление".
std::string scene_stem(const std::string& file_name) {
    const std::string suffix = kSceneSuffix;
    if (file_name.size() > suffix.size() && file_name.compare(file_name.size() - suffix.size(), suffix.size(), suffix) == 0)
        return file_name.substr(0, file_name.size() - suffix.size());
    return path_to_utf8(utf8_path(file_name).stem());
}

// The scene's file as the tab's machinery reads a character's: one clip «scene» of the code, its range and lengths.
pixel::PixelDoc as_pixel_doc(const pixel::SceneDoc& s) {
    pixel::PixelDoc d;
    d.script = s.script;
    d.seed = s.seed;
    d.params = s.params;
    pixel::ClipSpec c;
    c.state = "scene";
    c.fps = static_cast<f32>(s.fps);
    c.loop = false;
    c.ranged = s.ranged;
    c.from = s.from;
    c.to = s.to;
    d.clips.push_back(c);
    if (!(s.lengths == pixel::MotionLengths{})) d.lengths["scene"] = s.lengths;
    return d;
}

// What the tab changed in that, back into the scene's file.
pixel::SceneDoc as_scene_doc(const pixel::PixelDoc& d, pixel::SceneDoc s) {
    s.seed = d.seed;
    s.params = d.params;
    for (const pixel::ClipSpec& c : d.clips)
        if (c.state == "scene") {
            s.ranged = c.ranged;
            s.from = c.ranged ? c.from : 0;
            s.to = c.ranged ? c.to : 0;
        }
    const auto it = d.lengths.find("scene");
    s.lengths = it != d.lengths.end() ? it->second : pixel::MotionLengths{};
    return s;
}

std::string rel_of(const AssetLibrary& assets, const fs::path& file) {
    std::string rel = path_to_utf8(file.lexically_relative(assets.root()));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return rel;
}

} // namespace

bool PixelEditor::is_scene_file(const std::string& rel) {
    const std::string suffix = kSceneSuffix;
    return rel.size() > suffix.size() && rel.compare(rel.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// --- the model ---------------------------------------------------------------------------------------------

void PixelEditor::bind_scene(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<PlayRow>()) {
        s.RegisterMember("index", &PlayRow::index);
        s.RegisterMember("style", &PlayRow::style);
        s.RegisterMember("label", &PlayRow::label);
        s.RegisterMember("title", &PlayRow::title);
        s.RegisterMember("selected", &PlayRow::selected);
        s.RegisterMember("loop", &PlayRow::loop);
        s.RegisterMember("stop", &PlayRow::stop);
    }
    model.RegisterArray<std::vector<PlayRow>>();
    model.Bind("px_scenes", &m_scenes_);
    model.Bind("px_scene", &m_scene_);
    model.Bind("px_actor_rows", &m_actor_rows_);
    model.Bind("px_actor_rows_height", &m_actor_rows_height_);
    model.Bind("px_plays", &m_plays_);
    model.Bind("px_has_play", &m_has_play_);
    model.Bind("px_play_info", &m_play_info_);
    model.Bind("px_play_call", &m_play_call_);
    model.Bind("px_play_can_code", &m_play_can_code_);
    model.Bind("px_play_why", &m_play_why_);
    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_int = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<int>(-1) : -1; };
    on("px_scene_open", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(scenes_.size())) open(scenes_[static_cast<usize>(i)]);
    });
    on("px_new_scene", [this](Rml::Event&, const Rml::VariantList&) { create_scene(); });
    on("px_actor_clip", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { select_actor_clip(arg_int(a, 0)); });
    on("px_actor_clip_code", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { actor_clip_to_code(arg_int(a, 0)); });
    on("px_actor_clip_to_code", [this](Rml::Event&, const Rml::VariantList&) { actor_clip_to_code(play_); });
    on("px_actor_character", [this](Rml::Event&, const Rml::VariantList&) { open_actor_character(); });
    // Part 6б: the transitions' row.
    model.Bind("px_transitions", &m_transitions_);
    model.Bind("px_has_transition", &m_has_transition_);
    model.Bind("px_transition_info", &m_transition_info_);
    model.Bind("px_transition_call", &m_transition_call_);
    model.Bind("px_transition_can_code", &m_transition_can_code_);
    on("px_transition", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { select_transition(arg_int(a, 0)); });
    on("px_transition_code", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { transition_to_code(arg_int(a, 0)); });
    on("px_transition_to_code", [this](Rml::Event&, const Rml::VariantList&) { transition_to_code(transition_); });
}

// --- what a scene reads ------------------------------------------------------------------------------------

pixel::SceneFiles PixelEditor::scene_files() {
    pixel::SceneFiles f;
    // «Ресурсы» by path; each file asked for is watched, there or not.
    f.read = [this](const std::string& path, std::vector<u8>& bytes, std::string&) {
        const fs::path file = assets_.abs(path);
        scene_read_.push_back(file);
        std::optional<std::vector<u8>> b = bytes_of(file);
        if (!b) return false; // the scene says which file is not there
        bytes = std::move(*b);
        return true;
    };
    f.svg = [this](const std::string& guid, std::string& text, std::string& why) {
        const std::optional<Guid> id = Guid::parse(guid);
        const assets::AssetRecord* r = id ? assets_.find_id(*id) : nullptr;
        if (!r) return why = "его рисунка SVG нет в «Ресурсах» (его удалили?)", false;
        const fs::path file = assets_.abs(r->path);
        scene_read_.push_back(file);
        if (!read_text(file, text)) return why = "рисунок «" + r->path + "» не читается", false;
        return true;
    };
    // The game's modules: animations/имя.luau, watched by their files.
    f.modules = [this, reader = pixel::animations_reader(game_dir)](std::string_view name, std::string& text, std::string& why) {
        if (!game_dir.empty() && name.find_first_of("/\\:") == std::string_view::npos)
            scene_read_.push_back(game_dir / "animations" / utf8_path(std::string(name) + ".luau"));
        return reader(name, text, why);
    };
    return f;
}

bool PixelEditor::scene_files_changed() {
    for (const Watched& w : scene_watch_)
        if (stamp_of(w.file) != w.stamp) return true;
    return false;
}

// --- the scene read again ------------------------------------------------------------------------------------

void PixelEditor::reload_scene() {
    doc_problem_.clear();
    svg_problem_.clear();
    draw_problem_.clear();
    picture_note_.clear();
    draw_line_ = 0;
    drawn_ = false;
    const std::optional<std::vector<u8>> bytes = file_.empty() ? std::nullopt : bytes_of(file_);
    doc_watch_ = {file_, stamp_of(file_)};
    has_ = bytes.has_value();
    if (!has_) {
        rebuild();
        view_dirty_ = true;
        return;
    }
    pixel::SceneDoc d;
    std::string why;
    if (pixel::read_scene_doc({reinterpret_cast<const char*>(bytes->data()), bytes->size()}, d, why)) scene_doc_ = std::move(d);
    else doc_problem_ = "«" + path_to_utf8(file_.filename()) + "»: " + why;
    doc_ = as_pixel_doc(scene_doc_);
    clip_ = "scene";

    script_problem_.clear();
    const fs::path code_file = script_file();
    script_watch_ = {code_file, stamp_of(code_file)};
    std::string text;
    scene_read_.clear();
    if (code_file.empty() || !read_text(code_file, text)) {
        script_problem_ = "Кода сцены «" + doc_.script + "» нет рядом с «" + path_to_utf8(file_.filename()) + "»";
        adopt_code({});
    } else {
        adopt_code(text);
        // Read anew every time: the stage runs with the file's parameters and seed, and what it names may have changed.
        auto fresh = std::make_unique<pixel::Scene>();
        if (fresh->load(text, scene_doc_, scene_files())) {
            if (text != script_text_) drop_pen(), tooling_ = false;
            scene_ = std::move(fresh);
            script_text_ = text;
            bool crlf = false;
            script_panel_ = pixel::panel_text(script_text_, crlf);
            caret_ = ~0u;
            error_line_ = 0;
        } else {
            // The views stay on the last scene that worked.
            script_problem_ = "Сцена «" + doc_.script + "»: " + fresh->error() + (scene_ ? " (показана последняя удачная)" : "");
            error_line_ = fresh->error_line();
        }
    }
    scene_watch_.clear();
    for (const fs::path& f : scene_read_)
        if (std::none_of(scene_watch_.begin(), scene_watch_.end(), [&](const Watched& w) { return w.file == f; })) scene_watch_.push_back({f, stamp_of(f)});
    if (!part_.empty() && !has_part(part_)) part_.clear();
    recount();
    if (block_ >= 0) select_block(block_);
    if (play_ >= static_cast<i32>(rec_.plays.size()) || !rec_ok_) play_ = -1;
    if (transition_ >= static_cast<i32>(rec_.transitions.size()) || !rec_ok_) transition_ = -1;
    view_dirty_ = true;
    rebuild_scene_list(); // its row: its size and length as read now
    rebuild();
}

bool PixelEditor::change_scene(const pixel::PixelDoc& changed, std::string label) {
    const pixel::SceneDoc after = as_scene_doc(changed, scene_doc_);
    if (after == scene_doc_) return false;
    std::optional<std::vector<u8>> before = bytes_of(file_);
    std::vector<u8> bytes = text_bytes(pixel::write_scene_doc(after));
    if (before && *before == bytes) return false;
    note_.clear();
    std::vector<PixelFiles::File> files;
    files.push_back({file_, std::move(before), std::move(bytes)});
    history_.execute(std::make_unique<PixelFiles>(*this, assets_, std::move(files), std::move(label)));
    history_.seal();
    return true;
}

bool PixelEditor::create_scene() {
    note_.clear();
    if (code_unsaved()) {
        note_ = "Код «" + doc_.script + "» не сохранён: сохраните его (Ctrl+S в панели) или отмените набор (Ctrl+Z в панели), потом делайте новую сцену";
        rebuild();
        return false;
    }
    const fs::path dir = assets_.root() / utf8_path("сцены");
    std::error_code ec;
    const bool had_dir = fs::is_directory(dir, ec);
    std::string base = "Сцена";
    for (int n = 2; fs::exists(dir / utf8_path(base + kSceneSuffix), ec) || fs::exists(dir / utf8_path(base + ".luau"), ec); ++n)
        base = "Сцена " + std::to_string(n);
    pixel::SceneDoc d;
    d.script = base + ".luau";
    const fs::path doc_file = dir / utf8_path(base + kSceneSuffix);
    std::vector<PixelFiles::File> files;
    files.push_back({dir / utf8_path(d.script), std::nullopt, text_bytes(pixel::new_scene_code(base))});
    files.push_back({doc_file, std::nullopt, text_bytes(pixel::write_scene_doc(d))});
    open_new(doc_file);
    // The folder it made goes with the files on Ctrl+Z.
    history_.execute(std::make_unique<PixelFiles>(*this, assets_, std::move(files), "Векторная: сцена «" + base + "»", had_dir ? fs::path() : dir));
    history_.seal();
    note_ = "Сделаны «" + base + kSceneSuffix + "» и «" + base + ".luau» в «сцены»: что на сцене (stage) и что происходит (scene) — в коде.";
    rebuild();
    return has_;
}

// --- the list of scenes -----------------------------------------------------------------------------------------

void PixelEditor::rebuild_scene_list() {
    std::vector<std::string> listed;
    std::vector<Row> rows;
    std::map<std::string, SceneInfo> info;
    for (const assets::AssetRecord& r : assets_.ending(kSceneSuffix)) {
        listed.push_back(r.path);
        const fs::path file = assets_.abs(r.path);
        std::string sub;
        if (scene_mode_ && r.path == rel_ && scene_) {
            // The one open: as it shows now.
            const pixel::Stage& st = scene_->stage();
            sub = number_words(st.width()) + " × " + number_words(st.height()) + (rec_ok_ ? " · " + seconds_words(rec_.length) + " с" : std::string());
        } else {
            // The others as they were read, again when their file or code changed.
            pixel::SceneDoc d;
            std::string text, why;
            const bool doc_ok = read_text(file, text) && pixel::read_scene_doc(text, d, why);
            const fs::path code = doc_ok && !d.script.empty() ? file.parent_path() / utf8_path(d.script) : fs::path();
            SceneInfo now{stamp_of(file), stamp_of(code), {}};
            const auto was = scene_info_.find(r.path);
            if (was != scene_info_.end() && was->second.doc == now.doc && was->second.code == now.code) {
                now.sub = was->second.sub;
            } else if (!doc_ok || !read_text(code, text)) {
                now.sub = "не читается";
            } else {
                pixel::Scene probe;
                std::vector<fs::path> read;
                read.swap(scene_read_);
                if (probe.load(text, d, scene_files())) {
                    const pixel::Stage& st = probe.stage();
                    now.sub = number_words(st.width()) + " × " + number_words(st.height()) +
                              (probe.record() ? " · " + seconds_words(probe.recording()->length) + " с" : std::string(" · с ошибкой"));
                } else {
                    now.sub = "с ошибкой";
                }
                read.swap(scene_read_);
            }
            sub = now.sub;
            info[r.path] = now;
        }
        rows.push_back({scene_stem(path_to_utf8(utf8_path(r.path).filename())), sub, scene_mode_ && r.path == rel_});
    }
    scenes_ = std::move(listed);
    scene_info_ = std::move(info);
    m_scenes_ = std::move(rows);
    dirty("px_scenes");
}

// --- parts of the frame shown ------------------------------------------------------------------------------------

PixelEditor::ShownPart PixelEditor::shown_part(const std::string& id) {
    ShownPart s;
    if (id.empty()) return s;
    pixel::Rig& stage = rig().shown();
    if (const i32 i = stage.part(id); i >= 0) {
        s.rig = &stage;
        s.index = i;
        s.swapping = rig().swapping();
        return s;
    }
    // An actor's part «ёжик/head»: in the drawing its actor shows, from its units to the stage's.
    const usize slash = id.find('/');
    if (!scene_mode_ || slash == std::string::npos) return s;
    const std::string actor = id.substr(0, slash);
    const i32 host = rig().part(actor);
    pixel::Rig* a = host >= 0 ? rig().hosted(actor) : nullptr;
    if (!a) return s;
    pixel::Rig& shown = a->shown();
    const i32 j = shown.part(id);
    if (j < 0) return s;
    s.rig = &shown;
    s.index = j;
    s.outer = rig().global_matrix(host);
    s.swapping = a->swapping();
    return s;
}

bool PixelEditor::has_part(const std::string& id) const {
    if (scene_mode_ && id == pixel::kCameraPart) return scene_ != nullptr; // the camera: of the recording, no drawing's (part 6б)
    return const_cast<PixelEditor*>(this)->shown_part(id).rig != nullptr;
}

void PixelEditor::scene_rows() {
    // The camera first (part 6б); the stage's parts as they nest; an actor's parts inside it, of the drawing it shows.
    m_parts_.push_back({pixel::kCameraPart, "кадр игрока", 0, false, part_ == pixel::kCameraPart, "Камера", true});
    const pixel::Rig& stage = rig().shown();
    auto pivot_words = [](vector::Point pv) { return number_words(std::round(pv.x * 10) / 10) + "; " + number_words(std::round(pv.y * 10) / 10); };
    for (const pixel::Part& p : stage.parts()) {
        int depth = 0;
        for (i32 q = p.parent; q >= 0; q = stage.parts()[static_cast<usize>(q)].parent) ++depth;
        m_parts_.push_back({p.id, pivot_words(stage.pivot(stage.part(p.id))), depth, p.shape, p.id == part_, p.id});
        const pixel::Rig* actor = stage.hosted(p.id);
        if (!actor) continue;
        const pixel::Rig& shown = actor->shown();
        for (const pixel::Part& q : shown.parts()) {
            if (q.id == p.id) continue; // its root group: the actor itself
            int d = depth + 1;
            for (i32 r = q.parent; r >= 0 && shown.parts()[static_cast<usize>(r)].id != p.id; r = shown.parts()[static_cast<usize>(r)].parent) ++d;
            m_parts_.push_back({q.id, pivot_words(shown.pivot(shown.part(q.id))), d, q.shape, q.id == part_, q.id});
        }
    }
}

std::string PixelEditor::scene_part_at(const vector::Point& v, const pixel::Affine& view) {
    // What is chosen stays chosen when pressed anywhere on it: an actor chosen in the tree is dragged by any of its
    // points. Else the topmost thing of the stage under the mouse (the stage's last drawn first): an actor's smallest
    // part there, else the actor; of a picture or the stage's shapes the smallest part there.
    if (const ShownPart s = shown_part(part_); s.rig && s.rig->has_shapes(s.index)) {
        f64 b[4];
        s.rig->bounds(s.index, view * s.outer, b);
        if (v.x >= b[0] && v.x <= b[2] && v.y >= b[1] && v.y <= b[3]) return part_;
    }
    pixel::Rig& stage = rig().shown();
    const std::span<const pixel::Part> parts = stage.parts();
    auto inside = [&](pixel::Rig& r, i32 i, const pixel::Affine& px, f64& area) {
        f64 b[4];
        r.bounds(i, px, b);
        area = (b[2] - b[0]) * (b[3] - b[1]);
        return v.x >= b[0] && v.x <= b[2] && v.y >= b[1] && v.y <= b[3] && area > 0;
    };
    auto under = [&](i32 part, i32 top) {
        for (i32 q = part; q >= 0; q = parts[static_cast<usize>(q)].parent)
            if (q == top) return true;
        return false;
    };
    for (i32 top = static_cast<i32>(parts.size()); top-- > 0;) {
        if (parts[static_cast<usize>(top)].parent >= 0 || !stage.has_shapes(top)) continue;
        f64 area = 0;
        if (!inside(stage, top, view, area)) continue;
        const std::string& id = parts[static_cast<usize>(top)].id;
        std::string best = id;
        f64 best_area = 1e300;
        if (pixel::Rig* actor = stage.hosted(id)) {
            pixel::Rig& shown = actor->shown();
            const pixel::Affine px = view * stage.global_matrix(top);
            for (i32 i = 0; i < static_cast<i32>(shown.parts().size()); ++i) {
                const std::string& pid = shown.parts()[static_cast<usize>(i)].id;
                if (pid == id || !shown.has_shapes(i)) continue;
                if (inside(shown, i, px, area) && area < best_area) best_area = area, best = pid;
            }
            return best;
        }
        for (i32 i = top; i < static_cast<i32>(parts.size()); ++i)
            if (under(i, top) && stage.has_shapes(i) && inside(stage, i, view, area) && area < best_area) best_area = area, best = parts[static_cast<usize>(i)].id;
        return best;
    }
    return {};
}

// --- the snapshot ------------------------------------------------------------------------------------------------

std::string PixelEditor::scene_snapshot() {
    auto refuse = [&](std::string why) {
        note_ = std::move(why);
        rebuild();
        return std::string();
    };
    const pixel::ClipSpec* c = selected_clip();
    if (!has_ || !scene_ || !c) return refuse("Снимка нет: сцена не открыта");
    if (assets_.looking()) return refuse("«Ресурсы» ещё смотрят файлы: снимите через миг");
    pixel::Pose pose;
    std::string why;
    if (!pose_at(*c, t_, frame_, pose, why)) return refuse("Снимка нет: " + why);
    if (!scene_->pose(t_, rec_ok_ ? &rec_ : nullptr, pose)) return refuse("Снимка нет: " + scene_->error());
    // The player's frame at the scene's size, as «Через камеру» draws it (part 6б).
    const pixel::Stage& st = scene_->stage();
    const f64 sw = std::max<f64>(1, std::lround(st.width())), sh = std::max<f64>(1, std::lround(st.height()));
    if (sw > kMaxSnapshotSide || sh > kMaxSnapshotSide)
        return refuse("Снимка нет: сцена больше " + std::to_string(kMaxSnapshotSide) + " точек по стороне");
    const u32 w = static_cast<u32>(sw), h = static_cast<u32>(sh);
    const assets::CookedTexture frame = scene_->draw_frame(w, h, pixel::Affine{});
    view_dirty_ = true;
    std::vector<u8> png;
    if (!assets::encode_image(frame, ".png", png)) return refuse("Кадр не кодируется в PNG");
    const fs::path dir = file_.parent_path();
    const std::string stem = name() + " " + std::to_string(frame_ + 1);
    fs::path target = dir / utf8_path(stem + ".png");
    std::error_code ec;
    for (int n = 2; fs::exists(target, ec); ++n) target = dir / utf8_path(stem + " (" + std::to_string(n) + ").png");
    assets::AssetMeta meta;
    meta.id = Guid::generate();
    fs::path meta_file = target;
    meta_file += ".meta";
    std::vector<PixelFiles::File> files;
    files.push_back({target, std::nullopt, png});
    files.push_back({meta_file, std::nullopt, text_bytes(data::to_json(meta))});
    const std::string file_name = path_to_utf8(target.filename());
    history_.execute(std::make_unique<PixelFiles>(*this, assets_, std::move(files), "«" + name() + "»: снимок «" + file_name + "»"));
    history_.seal();
    note_ = "Снимок кадра: «" + file_name + "» рядом с «" + path_to_utf8(file_.filename()) + "»";
    rebuild();
    return rel_of(assets_, target);
}

// --- the actors' clips on the timeline ------------------------------------------------------------------------

bool PixelEditor::select_actor_clip(i32 index) {
    if (!scene_mode_ || !rec_ok_ || index < -1 || index >= static_cast<i32>(rec_.plays.size())) return false;
    play_ = index;
    if (index >= 0) select_block(-1); // one thing of the timeline in the inspector
    if (index >= 0 && transition_ >= 0) {
        transition_ = -1;
        rebuild_transition();
        lay_transitions();
    }
    rebuild_actor_clip();
    lay_timeline();
    return true;
}

bool PixelEditor::set_actor_clip_start(i32 index, f64 to) {
    note_.clear();
    bool saved = false;
    if (!begin_edit(saved)) return false;
    if (index < 0 || index >= static_cast<i32>(rec_.plays.size())) return refuse_edit("На таймлайне нет такого клипа актёра");
    const pixel::ActorPlay& p = rec_.plays[static_cast<usize>(index)];
    const std::string what = "«" + p.actor + "» · " + (p.kind == pixel::ActorPlay::Kind::Stop ? std::string("стоп") : p.clip);
    std::string out, why;
    if (!pixel::edit_play_start(rec_, p, to, script().code(), script_text_, out, why)) return refuse_edit("Начало " + what + " не записать: " + why);
    if (out == script_text_) return rebuild(), false;
    return write_script(out, "«" + name() + "»: " + what + ", начало " + seconds_words(to) + " с");
}

bool PixelEditor::actor_clip_to_code(i32 index) {
    if (!rec_ok_ || index < 0 || index >= static_cast<i32>(rec_.plays.size())) return false;
    const pixel::ActorPlay& p = rec_.plays[static_cast<usize>(index)];
    if (p.site.file != 0 || p.site.site < 0 || p.site.site >= static_cast<i32>(script().code().sites().size()))
        return refuse_edit("Клип актёра «" + p.actor + "» — из модуля: его код не в панели");
    if (code_ != script_panel_) return refuse_edit("Код в панели не тот, что записан: сохраните его (Ctrl+S), и клип покажет свой вызов");
    pixel::Block b;
    b.site = p.site;
    select_call(b);
    return true;
}

bool PixelEditor::open_actor_character() {
    if (!scene_ || !rec_ok_ || play_ < 0 || play_ >= static_cast<i32>(rec_.plays.size())) return false;
    const pixel::StageItem* it = scene_->stage().item(rec_.plays[static_cast<usize>(play_)].actor);
    if (!it) return false;
    return open(it->file + ".pixel.json");
}

void PixelEditor::rebuild_actor_clip() {
    const bool has = scene_mode_ && rec_ok_ && play_ >= 0 && play_ < static_cast<i32>(rec_.plays.size());
    std::string info, call, why;
    bool can_code = false;
    if (has) {
        const pixel::ActorPlay& p = rec_.plays[static_cast<usize>(play_)];
        using Kind = pixel::ActorPlay::Kind;
        if (p.kind == Kind::Stop) {
            info = "«" + p.actor + "» стоит в позе, что показывал в " + seconds_words(p.start) + " с";
        } else {
            info = "«" + p.actor + "» · клип «" + p.clip + "»" + (p.kind == Kind::Loop ? " по кругу" : "") + "\nначало " + seconds_words(p.start) +
                   " с, длина " + seconds_words(p.length) + " с" + (p.speed != 1 ? " (клип " + seconds_words(p.clip_length) + " с, speed " + number_words(p.speed) + ")" : "");
            if (p.kind == Kind::Loop) info += "\nидёт до следующего клипа актёра или конца сцены";
            std::string params;
            for (const auto& [k, v] : p.params) params += (params.empty() ? "" : ", ") + k + " = " + number_words(v);
            if (!params.empty()) info += "\nпараметры: " + params;
        }
        if (p.site.file == 0 && p.site.site >= 0 && p.site.site < static_cast<i32>(script().code().sites().size())) {
            const pixel::CallSite& site = script().code().sites()[static_cast<usize>(p.site.site)];
            const usize from = std::min<usize>(site.span.begin, script_text_.size()), to = std::min<usize>(site.span.end, script_text_.size());
            call = "строка " + std::to_string(site.line) + ": " + call_words(std::string_view(script_text_).substr(from, to - from));
            can_code = true;
        }
        std::string w;
        if (!pixel::can_edit_play_start(rec_, p, script().code(), w)) why = "Начало не тянется: " + w;
    }
    m_has_play_ = has;
    m_play_info_ = info;
    m_play_call_ = call;
    m_play_can_code_ = can_code;
    m_play_why_ = why;
    for (const char* var : {"px_has_play", "px_play_info", "px_play_call", "px_play_can_code", "px_play_why"}) dirty(var);
}

} // namespace forge::editor_app

namespace forge::editor_app {

// --- the camera's frame (part 6б) ----------------------------------------------------------------------------

namespace {

// The camera's frame on the stage: the canvas's corners back through the camera, clockwise from the top left.
void frame_corners(const pixel::Scene& scene, vector::Point out[4]) {
    const pixel::Affine back = pixel::inverse(scene.camera_matrix());
    const f64 w = scene.stage().width(), h = scene.stage().height();
    const vector::Point c[4] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    for (int i = 0; i < 4; ++i) out[i] = back.apply(c[i]);
}

// How far p is from the segment a–b.
f64 segment_distance(vector::Point p, vector::Point a, vector::Point b) {
    const f64 dx = b.x - a.x, dy = b.y - a.y, len = dx * dx + dy * dy;
    const f64 k = len > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len, 0.0, 1.0) : 0;
    return std::hypot(p.x - (a.x + k * dx), p.y - (a.y + k * dy));
}

} // namespace

bool PixelEditor::camera_corners(f32 x[4], f32 y[4]) const {
    if (!scene_mode_ || !scene_ || !rig().loaded()) return false;
    vector::Point c[4];
    frame_corners(*scene_, c);
    for (int i = 0; i < 4; ++i)
        if (!screen_of(c[i], x[i], y[i])) return false;
    return true;
}

bool PixelEditor::through_size(u32& w, u32& h) const {
    if (!scene_mode_ || !scene_) return false;
    pixel::frame_size(scene_->stage().width(), scene_->stage().height(), view_px(), w, h);
    return true;
}

bool PixelEditor::camera_edge(f32 x, f32 y) const {
    f32 cx[4], cy[4];
    if (!camera_corners(cx, cy)) return false;
    const f64 reach = kFrameReach * std::max(1.0f, view_w_ / static_cast<f32>(kViewPx));
    for (int i = 0; i < 4; ++i)
        if (segment_distance({x, y}, {cx[i], cy[i]}, {cx[(i + 1) % 4], cy[(i + 1) % 4]}) <= reach) return true;
    return false;
}

void PixelEditor::draw_camera_frame(assets::CookedTexture& t, const pixel::Affine& texels) const {
    if (!scene_ || t.rgba8.empty()) return;
    vector::Point c[4];
    frame_corners(*scene_, c);
    for (vector::Point& p : c) p = texels.apply(p);
    const i64 k = std::max<i64>(1, std::lround(dp_));
    // A line k texels wide, edged k texels on each side, so it shows on light and dark scenes alike: squares of k + 2 ·
    // grow texels along each side.
    auto line = [&](i64 grow, const u8 (&color)[4]) {
        for (int i = 0; i < 4; ++i) {
            const vector::Point a = c[i], b = c[(i + 1) % 4];
            const i64 n = static_cast<i64>(std::ceil(std::hypot(b.x - a.x, b.y - a.y))) + 1;
            for (i64 s = 0; s <= n; ++s) {
                const f64 u = static_cast<f64>(s) / static_cast<f64>(n);
                const i64 px = static_cast<i64>(std::floor(a.x + (b.x - a.x) * u)) - k / 2, py = static_cast<i64>(std::floor(a.y + (b.y - a.y) * u)) - k / 2;
                for (i64 yy = py - grow; yy < py + k + grow; ++yy)
                    for (i64 xx = px - grow; xx < px + k + grow; ++xx) {
                        if (xx < 0 || yy < 0 || xx >= t.width || yy >= t.height) continue;
                        std::copy(color, color + 4, &t.rgba8[(static_cast<usize>(yy) * t.width + static_cast<usize>(xx)) * 4]);
                    }
            }
        }
    };
    line(k, kFrameEdge);
    line(0, camera_chosen() ? kFrameChosen : kFrameLine);
}

// --- the transitions on the timeline (part 6б) --------------------------------------------------------------------

namespace {

std::string transition_words(const pixel::SceneTransition& tr) {
    return std::string(tr.leave ? "уход: " : "вход: ") + (tr.kind.empty() ? std::string("свой") : tr.kind);
}

} // namespace

void PixelEditor::lay_transitions() {
    m_transitions_.clear();
    const f64 dp = std::max(1e-3f, dp_), w = tl_w_;
    if (scene_mode_ && scene_ && rec_ok_ && tl_w_ > 0)
        for (usize i = 0; i < rec_.transitions.size(); ++i) {
            const pixel::SceneTransition& tr = rec_.transitions[i];
            f64 x0 = tv_.x(tr.start), x1 = tv_.x(tr.start + tr.length);
            if (x1 < 0 || x0 > w) continue;
            x0 = std::max(0.0, x0);
            x1 = std::min(w, std::max(x1, x0 + 2 * dp));
            char style[112];
            std::snprintf(style, sizeof style, "display: block; left: %.1fdevpx; width: %.1fdevpx;", x0, x1 - x0);
            const std::string title = std::string(tr.leave ? "leave" : "transition") + "(" + (tr.kind.empty() ? std::string("function") : "\"" + tr.kind + "\"") +
                                      ") с " + seconds_words(tr.start) + " по " + seconds_words(tr.start + tr.length) +
                                      " с: длина правится в коде, двойной щелчок — к коду";
            m_transitions_.push_back({static_cast<int>(i), style, transition_words(tr), title, static_cast<i32>(i) == transition_, false, false});
        }
    dirty("px_transitions");
}

bool PixelEditor::select_transition(i32 index) {
    if (!scene_mode_ || !rec_ok_ || index < -1 || index >= static_cast<i32>(rec_.transitions.size())) return false;
    transition_ = index;
    if (index >= 0) {
        select_block(-1); // one thing of the timeline in the inspector
        if (play_ >= 0) select_actor_clip(-1);
    }
    rebuild_transition();
    lay_transitions();
    return true;
}

bool PixelEditor::transition_to_code(i32 index) {
    if (!rec_ok_ || index < 0 || index >= static_cast<i32>(rec_.transitions.size())) return false;
    select_transition(index);
    const pixel::SceneTransition& tr = rec_.transitions[static_cast<usize>(index)];
    if (tr.site.file != 0 || tr.site.site < 0 || tr.site.site >= static_cast<i32>(script().code().sites().size()))
        return refuse_edit("Переход «" + transition_words(tr) + "» — из модуля: его код не в панели");
    if (code_ != script_panel_) return refuse_edit("Код в панели не тот, что записан: сохраните его (Ctrl+S), и переход покажет свой вызов");
    pixel::Block b;
    b.site = tr.site;
    select_call(b);
    return true;
}

void PixelEditor::rebuild_transition() {
    const bool has = scene_mode_ && rec_ok_ && transition_ >= 0 && transition_ < static_cast<i32>(rec_.transitions.size());
    std::string info, call;
    bool can_code = false;
    if (has) {
        const pixel::SceneTransition& tr = rec_.transitions[static_cast<usize>(transition_)];
        info = transition_words(tr) + (tr.kind == "сдвиг" ? ", с края «" + tr.from + "»" : std::string()) + "\nс " + seconds_words(tr.start) + " по " +
               seconds_words(tr.start + tr.length) + " с, длина " + seconds_words(tr.length) + " с" +
               (tr.kind.empty() ? "\nсвой: длина — длина записи его функции" : std::string()) +
               (tr.leave ? "\nпосле него сцена кончается: дальше игра" : "\nсцена идёт, пока входит") + "\nдлина и вид правятся в коде";
        if (tr.site.file == 0 && tr.site.site >= 0 && tr.site.site < static_cast<i32>(script().code().sites().size())) {
            const pixel::CallSite& site = script().code().sites()[static_cast<usize>(tr.site.site)];
            const usize from = std::min<usize>(site.span.begin, script_text_.size()), to = std::min<usize>(site.span.end, script_text_.size());
            call = "строка " + std::to_string(site.line) + ": " + call_words(std::string_view(script_text_).substr(from, to - from));
            can_code = true;
        }
    }
    m_has_transition_ = has;
    m_transition_info_ = info;
    m_transition_call_ = call;
    m_transition_can_code_ = can_code;
    for (const char* var : {"px_has_transition", "px_transition_info", "px_transition_call", "px_transition_can_code"}) dirty(var);
}

} // namespace forge::editor_app
