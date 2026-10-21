// «Векторная», part 6а of docs/models/сцены-модель.md «Сцена и актёры»: a scene open in the tab in place of a character
// (pixel_editor.h). Its code is the panel's and its stage the vector's: the scene is clip «scene» of its code
// (PoseScript::set_scene), recorded, laid out on the timeline and edited by the mouse as a character's clip is; a frame
// poses the stage and its actors (pixel::Scene::pose). The rest of the mode is pixel_editor.cpp.
// Part 6б «Камера и переходы»: the camera's frame over the vector and its edge dragged, its row of the tree, the player's
// frame through it for the small view and the snapshot, the row of the transitions on the timeline.
// Part 6в «Звук, ожидание и разговор»: the rows of the music and of the stops, the music in the player, «Ждать, как в
// игре».

#include "pixel_editor.h"
#include "pixel_files.h"

#include "asset_library.h"

#include "forge/assets/asset_pipeline.h"
#include "forge/assets/image.h"
#include "forge/data/json.h"
#include "forge/pixel/motion_edit.h"
#include "forge/pixel/scene_gift.h"
#include "forge/vector/scene_file.h"

#include <algorithm>
#include <cmath>
#include <set>

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
        s.RegisterMember("blend", &PlayRow::blend);
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
    // Part 7а: a scene opened or made from «Файлы» is opened to work on: the left panel shows its «Слои».
    on("px_scene_open", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(scenes_.size()) && open(scenes_[static_cast<usize>(i)])) set_left("layers");
    });
    on("px_new_scene", [this](Rml::Event&, const Rml::VariantList&) {
        if (create_scene()) set_left("layers");
    });
    on("px_actor_clip", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        if (select_actor_clip(arg_int(a, 0)) && play_ >= 0) set_side("anim"); // part 7а
    });
    on("px_actor_clip_code", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { actor_clip_to_code(arg_int(a, 0)); });
    on("px_actor_clip_to_code", [this](Rml::Event&, const Rml::VariantList&) { actor_clip_to_code(play_); });
    on("px_actor_character", [this](Rml::Event&, const Rml::VariantList&) { open_actor_character(); });
    // Part 6б: the transitions' row.
    model.Bind("px_transitions", &m_transitions_);
    model.Bind("px_has_transition", &m_has_transition_);
    model.Bind("px_transition_info", &m_transition_info_);
    model.Bind("px_transition_call", &m_transition_call_);
    model.Bind("px_transition_can_code", &m_transition_can_code_);
    on("px_transition", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        if (select_transition(arg_int(a, 0)) && transition_ >= 0) set_side("anim"); // part 7а
    });
    on("px_transition_code", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { transition_to_code(arg_int(a, 0)); });
    on("px_transition_to_code", [this](Rml::Event&, const Rml::VariantList&) { transition_to_code(transition_); });
    // Part 6в: the music's and the stops' rows, «Ждать, как в игре» and the line under the player while it stands.
    if (auto s = model.RegisterStruct<MusicView>()) {
        s.RegisterMember("index", &MusicView::index);
        s.RegisterMember("style", &MusicView::style);
        s.RegisterMember("fade", &MusicView::fade);
        s.RegisterMember("label", &MusicView::label);
        s.RegisterMember("title", &MusicView::title);
        s.RegisterMember("missing", &MusicView::missing);
    }
    model.RegisterArray<std::vector<MusicView>>();
    if (auto s = model.RegisterStruct<MusicMark>()) {
        s.RegisterMember("index", &MusicMark::index);
        s.RegisterMember("style", &MusicMark::style);
        s.RegisterMember("title", &MusicMark::title);
    }
    model.RegisterArray<std::vector<MusicMark>>();
    if (auto s = model.RegisterStruct<StopRow>()) {
        s.RegisterMember("index", &StopRow::index);
        s.RegisterMember("style", &StopRow::style);
        s.RegisterMember("label", &StopRow::label);
        s.RegisterMember("title", &StopRow::title);
        s.RegisterMember("missing", &StopRow::missing);
        s.RegisterMember("held", &StopRow::held);
    }
    model.RegisterArray<std::vector<StopRow>>();
    model.Bind("px_music", &m_music_);
    model.Bind("px_music_marks", &m_music_marks_);
    model.Bind("px_music_wave", &m_music_wave_);
    model.Bind("px_music_wave_style", &m_music_wave_style_);
    model.Bind("px_stops", &m_stops_);
    model.Bind("px_stops_height", &m_stops_height_);
    model.Bind("px_hold", &m_hold_);
    model.Bind("px_held_words", &m_held_words_);
    on("px_music_code", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { music_to_code(arg_int(a, 0)); });
    on("px_stop_code", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { stop_to_code(arg_int(a, 0)); });
    on("px_hold_toggle", [this](Rml::Event&, const Rml::VariantList&) { set_hold(!hold_on_); });
    on("px_give_game", [this](Rml::Event&, const Rml::VariantList&) { give_game(); });
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
    if (!scene_->pose(t_, rec_ok_ ? &rec_ : nullptr, pose, held_, held_at_)) return refuse("Снимка нет: " + scene_->error());
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

// --- «Отдать игре» (part 6г1) ------------------------------------------------------------------------------------

bool PixelEditor::give_game() {
    note_.clear();
    auto refuse = [&](std::string why) {
        note_ = std::move(why);
        rebuild();
        return false;
    };
    if (!scene_mode_) return refuse("Игре отдаётся сцена; персонажа — «Отдать экрану»");
    if (game_dir.empty()) return refuse("Отдать игре можно в открытой игре");
    if (!has_ || !scene_) return refuse("Нечего отдавать: сцена не открыта");
    if (!script_problem_.empty()) return refuse("Нечего отдавать: " + script_problem_);
    if (assets_.looking()) return refuse("«Ресурсы» ещё смотрят файлы: отдайте через миг");
    const fs::path code = script_file();
    const std::string name = path_to_utf8(code.stem()), from = rel_of(assets_, code);
    pixel::SceneGift gift;
    std::string why;
    const bool ok = pixel::build_scene_gift(*scene_, name, from, scene_files(), gift, why);
    view_dirty_ = true; // the scene was recorded again: the view draws its own
    if (!ok) return refuse(why);
    const fs::path scenes = game_dir / "scenes", dir = scenes / utf8_path(name);
    std::error_code ec;
    // The folder of a scene of another file of «Ресурсы», or a folder not of a scene, is not written over.
    if (const std::optional<std::vector<u8>> was = bytes_of(dir / "scene.json")) {
        vector::SceneFile f;
        if (!vector::read_scene_file({reinterpret_cast<const char*>(was->data()), was->size()}, f, why))
            return refuse("В игре уже есть папка сцены «" + name + "», её scene.json не читается: " + why);
        if (f.from != from) return refuse("В игре уже есть сцена «" + name + "» из «" + f.from + "»: переименуйте сцену или ту");
    } else if (fs::exists(dir, ec) && !fs::is_empty(dir, ec))
        return refuse("В игре уже есть папка «scenes/" + name + "» не от сцены: переименуйте сцену или ту папку");
    std::vector<PixelFiles::File> files;
    for (const pixel::SceneGift::File& f : gift.files) {
        const fs::path path = dir / utf8_path(f.path);
        files.push_back({path, bytes_of(path), text_bytes(f.bytes)});
    }
    // The folder's files the scene writes no more (an actor taken away from it) go in the same step; a file whose name
    // only changed its capitals is the new one's where names do not tell them apart (Windows), not taken away.
    const usize written = files.size();
    if (fs::is_directory(dir, ec))
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const bool kept = std::any_of(files.begin(), files.begin() + static_cast<std::ptrdiff_t>(written), [&](const PixelFiles::File& f) {
                std::error_code e;
                return f.path == it->path() || fs::equivalent(f.path, it->path(), e);
            });
            if (!kept)
                if (std::optional<std::vector<u8>> before = bytes_of(it->path())) files.push_back({it->path(), std::move(before), std::nullopt});
        }
    if (std::all_of(files.begin(), files.end(), [](const PixelFiles::File& f) { return f.before == f.after; })) return refuse("Уже отдано так");
    std::vector<fs::path> made; // outer first: Ctrl+Z takes away those left empty
    for (const fs::path& d : {scenes, dir, dir / "actors", dir / "pictures"})
        if (!fs::exists(d, ec)) made.push_back(d);
    history_.execute(std::make_unique<PixelFiles>(*this, assets_, std::move(files), "«" + name + "»: отдана игре", std::move(made)));
    history_.seal();
    const vector::SceneFile& f = gift.motion.file;
    std::string what;
    if (!f.actors.empty()) what += ", " + count_words(static_cast<u32>(f.actors.size()), "актёр", "актёра", "актёров");
    if (!f.pictures.empty()) what += ", " + count_words(static_cast<u32>(f.pictures.size()), "картинка", "картинки", "картинок");
    note_ = "Отдано игре: «" + name + "»" + what + ", " + number_words(std::round(f.length * 100) / 100) + " с.";
    for (const std::string& n : gift.notes) note_ += " " + n + ".";
    FORGE_INFO("Векторная: %s", note_.c_str());
    rebuild();
    return true;
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
            // Part 6г5а: what its blend comes from, as the player shows it at its start.
            const f64 lasts = !(p.clip_length > 0) ? 0 : p.kind == Kind::Loop ? p.blend : std::min(p.blend, p.clip_length);
            if (lasts > 0) {
                const pixel::ActorMoment m = pixel::actor_moment(rec_, p.actor, p.start);
                std::string from = "от позы";
                if (m.play == &p && m.blend.size() == 2 && !m.blend.front().play) from = "от покоя";
                else if (m.play == &p && m.blend.size() == 2 && m.blend.front().play->kind != Kind::Stop) {
                    // The call before it going on, when no stop came between.
                    const pixel::ActorPlay* before = nullptr;
                    for (const pixel::ActorPlay& o : rec_.plays)
                        if (o.actor == p.actor && &o != &p && (o.start < p.start || (o.start == p.start && o.order < p.order)) &&
                            (!before || o.start > before->start || (o.start == before->start && o.order > before->order)))
                            before = &o;
                    if (before == m.blend.front().play) from = "от «" + before->clip + "»";
                }
                info += "\nплавно " + seconds_words(lasts / p.speed) + " с " + from;
            }
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
    // Part 7а: the canvas's shape as big as fits in the frame of «Пиксели» (its column's width before it is laid out).
    const f64 bw = pixel_box_w_ > 0 ? pixel_box_w_ : kPixelColumn * dp_, bh = pixel_box_h_ > 0 ? pixel_box_h_ : kPixelColumn * dp_;
    const f64 sw = std::max(1.0, scene_->stage().width()), sh = std::max(1.0, scene_->stage().height());
    const f64 side = sw >= sh ? std::min(bw, bh * sw / sh) : std::min(bh, bw * sh / sw);
    pixel::frame_size(sw, sh, std::max(1.0, std::floor(side)), w, h);
    return true;
}

bool PixelEditor::camera_edge(f32 x, f32 y) const {
    f32 cx[4], cy[4];
    if (!camera_corners(cx, cy)) return false;
    const f64 reach = kFrameReach * std::max(1.0f, dp_);
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

namespace forge::editor_app {

// --- music, waiting and talks (part 6в) ----------------------------------------------------------------------------

namespace {

constexpr f64 kMusicPx = 22;       // the music's row (logical px)
constexpr f64 kFlagPx = 140;       // a stop's flag, as wide as .px-flag.px-stop (logical px)
constexpr usize kLanePx = 22;      // a lane of the stops' row (logical px)
constexpr int kMostRounds = 4096;  // of a music's wave drawn round and round in sight

// The keys of an action in the player's words, «E» or «E, F»; "" when it has none (its mouse's and gamepad's buttons
// are not the tab's).
std::string key_words(const input::Action& a) {
    std::string out;
    for (const input::Bind& b : a.binds)
        if (b.device == input::Device::Key) out += (out.empty() ? "" : ", ") + input::words(b);
    return out;
}

// What the stops' row was laid with: the game's talks and its actions' words and keys.
std::string stops_key(const std::vector<std::string>& talks, const std::vector<input::Action>& actions) {
    std::string key;
    for (const std::string& t : talks) key += t + "|";
    key += "#";
    for (const input::Action& a : actions) key += a.words + ":" + key_words(a) + "|";
    return key;
}

} // namespace

const std::vector<pixel::CallSite>& PixelEditor::panel_sites() const {
    static const std::vector<pixel::CallSite> none;
    return script().loaded() ? script().code().sites() : none;
}

std::string PixelEditor::stop_words(const pixel::SceneStop& st) const {
    if (st.kind == pixel::SceneStop::Kind::Talk) return "разговор «" + st.name + "»";
    return st.name.empty() ? std::string("ждёт клавишу") : "ждёт «" + st.name + "»";
}

const input::Action* PixelEditor::stop_action(const pixel::SceneStop& st, const std::vector<input::Action>& actions) const {
    if (st.kind != pixel::SceneStop::Kind::Action || st.name.empty()) return nullptr;
    for (const input::Action& a : actions)
        if (a.words == st.name) return &a;
    return nullptr;
}

std::string PixelEditor::stop_missing(const pixel::SceneStop& st, const std::vector<input::Action>& actions) const {
    if (st.kind == pixel::SceneStop::Kind::Talk)
        return std::find(talks_.begin(), talks_.end(), st.name) == talks_.end() ? "разговора «" + st.name + "» в игре нет (её папка dialogues)"
                                                                               : std::string();
    if (st.name.empty() || stop_action(st, actions)) return {};
    return "действия «" + st.name + "» во «Вводе» игры нет";
}

void PixelEditor::lay_stops() {
    m_stops_.clear();
    const std::vector<input::Action> actions = game_actions ? game_actions() : std::vector<input::Action>{};
    stops_key_ = stops_key(talks_, actions);
    const f64 dp = std::max(1e-3f, dp_), w = tl_w_;
    const auto& sites = panel_sites();
    // Flags that would cover one another (the same moment, or near) go each a lane lower: the right edge of each lane's last.
    std::vector<f64> lanes;
    if (scene_mode_ && scene_ && rec_ok_ && tl_w_ > 0)
        for (usize i = 0; i < rec_.stops.size(); ++i) {
            const pixel::SceneStop& st = rec_.stops[i];
            const f64 x = tv_.x(st.time);
            if (x < -2 * dp || x > w + 2 * dp) continue;
            usize lane = 0;
            while (lane < lanes.size() && lanes[lane] > x - 2 * dp) ++lane;
            if (lane == lanes.size()) lanes.push_back(0);
            lanes[lane] = x - 2 * dp + kFlagPx * dp;
            const bool talk = st.kind == pixel::SceneStop::Kind::Talk;
            const std::string missing = stop_missing(st, actions);
            std::string about = missing;
            if (about.empty()) {
                if (talk) about = "разговор игры поверх сцены";
                else if (st.name.empty()) about = "любая клавиша";
                else if (const std::string keys = key_words(*stop_action(st, actions)); keys.empty()) about = "у действия нет клавиш";
                else about = "клавиши: " + keys;
            }
            const u32 line = st.site.file == 0 && st.site.site >= 0 && st.site.site < static_cast<i32>(sites.size()) ? sites[static_cast<usize>(st.site.site)].line : 0;
            const std::string call = talk ? "talk(\"" + st.name + "\")" : st.name.empty() ? std::string("wait_action()") : "wait_action(\"" + st.name + "\")";
            char style[80];
            std::snprintf(style, sizeof style, "left: %.1fdevpx; top: %dpx;", x - 2 * dp, static_cast<int>(lane * kLanePx));
            m_stops_.push_back({static_cast<int>(i), style, stop_words(st),
                                call + " в " + seconds_words(st.time) + " с" + (line ? ", строка " + std::to_string(line) : std::string()) + ": " + about +
                                    "; время сцены стоит, клипы актёров идут, длины нет; двойной щелчок — к коду",
                                !missing.empty(), static_cast<i32>(i) == held_at_});
        }
    const std::string height = "height: " + std::to_string(std::max<usize>(1, lanes.size()) * kLanePx) + "px;";
    if (height != m_stops_height_) {
        m_stops_height_ = height;
        dirty("px_stops_height");
    }
    dirty("px_stops");
}

void PixelEditor::check_stops() {
    if (!scene_mode_ || !rec_ok_ || rec_.stops.empty()) return;
    talks_ = game_dir.empty() ? std::vector<std::string>{} : talk_ids(game_dir);
    if (stops_key(talks_, game_actions ? game_actions() : std::vector<input::Action>{}) != stops_key_) lay_stops();
}

bool PixelEditor::stop_to_code(i32 index) {
    if (!rec_ok_ || index < 0 || index >= static_cast<i32>(rec_.stops.size())) return false;
    const pixel::SceneStop& st = rec_.stops[static_cast<usize>(index)];
    if (st.site.file != 0 || st.site.site < 0 || st.site.site >= static_cast<i32>(script().code().sites().size()))
        return refuse_edit("Флажок «" + stop_words(st) + "» — из модуля: его код не в панели");
    if (code_ != script_panel_) return refuse_edit("Код в панели не тот, что записан: сохраните его (Ctrl+S), и флажок покажет свой вызов");
    pixel::Block b;
    b.site = st.site;
    select_call(b);
    return true;
}

void PixelEditor::lay_music() {
    const f64 dp = std::max(1e-3f, dp_), w = tl_w_;
    const u32 cols = tl_w_ > 0 ? static_cast<u32>(std::lround(tl_w_)) : 0, high = static_cast<u32>(std::lround(kMusicPx * dp));
    std::vector<pixel::MusicRun> runs;
    if (scene_mode_ && scene_ && rec_ok_) runs = pixel::music_runs(rec_);
    // The same timeline, music and files: the row as it is.
    char buf[160];
    std::snprintf(buf, sizeof buf, "%.17g %.17g %u %u %.17g %zu|", tv_.t(0), tv_.per_second(), cols, high, rec_.length, panel_sites().size());
    std::string key = buf;
    if (scene_mode_ && rec_ok_)
        for (const pixel::SceneMusic& m : rec_.music) {
            const auto f = sound_files_.find(m.name);
            std::snprintf(buf, sizeof buf, "%.17g %.17g %.17g %u %d %u %p|", m.start, m.fade, m.volume, m.order, m.site.site, m.site.file,
                          f == sound_files_.end() ? nullptr : static_cast<const void*>(f->second.file.clip.get()));
            key += m.name + " " + buf + (f == sound_files_.end() ? std::string() : f->second.file.why) + "|";
        }
    if (key == music_key_) return;
    music_key_ = key;

    m_music_.clear();
    m_music_marks_.clear();
    const auto& sites = panel_sites();
    auto line_of = [&](const pixel::SiteRef& s) {
        return s.file == 0 && s.site >= 0 && s.site < static_cast<i32>(sites.size()) ? sites[static_cast<usize>(s.site)].line : 0u;
    };
    auto at_line = [&](const pixel::SiteRef& s) { return line_of(s) ? ", строка " + std::to_string(line_of(s)) : std::string(); };
    std::vector<f32> columns(cols, 0);
    std::vector<bool> sounding(cols, false);
    const f64 t0 = tv_.t(0), dt = 1 / tv_.per_second();
    for (const pixel::MusicRun& run : runs) {
        const auto f = sound_files_.find(run.name);
        const bool found = f != sound_files_.end() && f->second.file.clip;
        const std::string why = found ? std::string() : f != sound_files_.end() ? f->second.file.why : "музыка «" + run.name + "»: у игры нет папки sounds";
        // Its block, its fade a wedge from its start.
        const f64 x0 = tv_.x(run.start), x1 = tv_.x(run.end);
        if (x1 >= 0 && x0 <= w) {
            const f64 left = std::max(0.0, x0), right = std::min(w, std::max(x1, x0 + 2 * dp));
            char style[112], fade[112];
            std::snprintf(style, sizeof style, "left: %.1fdevpx; width: %.1fdevpx;", left, right - left);
            if (run.fade > 0) std::snprintf(fade, sizeof fade, "left: %.1fdevpx; width: %.1fdevpx;", x0 - left, run.fade * tv_.per_second());
            else std::snprintf(fade, sizeof fade, "display: none;");
            const pixel::SceneMusic& first = rec_.music[run.calls[0]];
            const std::string title = "music(\"" + run.name + "\") с " + seconds_words(run.start) + " по " + seconds_words(run.end) + " с" + at_line(first.site) +
                                      (run.fade > 0 ? ", наплыв " + seconds_words(run.fade) + " с" : std::string()) + ", громкость " +
                                      number_words(first.volume) + (found ? ": «" + path_to_utf8(f->second.file.file.filename()) + "» по кругу" : ": " + why) +
                                      "; двойной щелчок — к коду";
            m_music_.push_back({static_cast<int>(run.calls[0]), style, fade, run.name, title, !found});
        }
        // The calls of the same music on it.
        for (usize j = 1; j < run.calls.size(); ++j) {
            const pixel::SceneMusic& m = rec_.music[run.calls[j]];
            const f64 x = tv_.x(m.start);
            if (x < -4 * dp || x > w + 4 * dp) continue;
            char style[96];
            std::snprintf(style, sizeof style, "left: %.1fdevpx; width: %.1fdevpx;", x - 1.5 * dp, 3 * dp);
            m_music_marks_.push_back({static_cast<int>(run.calls[j]), style,
                                      "music(\"" + m.name + "\") в " + seconds_words(m.start) + " с" + at_line(m.site) + ": та же музыка играет дальше, громкость " +
                                          number_words(m.volume) + (m.fade > 0 ? " за " + seconds_words(m.fade) + " с" : std::string(" сразу")) +
                                          "; двойной щелчок — к коду"});
        }
        // Its wave, round and round from its start; the middle line where it plays at all.
        if (!found || cols == 0) continue;
        const audio::Clip& clip = *f->second.file.clip;
        const f64 len = clip.seconds(), a = std::max(run.start, t0), b = std::min(run.end, t0 + cols * dt);
        if (!(len > 0) || !(b > a)) continue;
        f64 s = run.start + std::floor((a - run.start) / len) * len;
        for (int n = 0; s < b && n < kMostRounds; s += len, ++n) pixel::add_wave(clip, s, run.end, t0, dt, columns);
        for (u32 c = 0; c < cols; ++c)
            if (t0 + (c + 1) * dt > run.start && t0 + c * dt < run.end) sounding[c] = true;
    }
    if (ui_) {
        if (!music_images_[1].empty()) ui_->drop_image(music_images_[1]);
        music_images_[1] = std::move(music_images_[0]);
        music_images_[0].clear();
    }
    m_music_wave_.clear();
    m_music_wave_style_ = "display: none;";
    if (ui_ && std::find(sounding.begin(), sounding.end(), true) != sounding.end()) {
        std::vector<u8> px(static_cast<usize>(cols) * high * 4, 0);
        for (u32 c = 0; c < cols; ++c) {
            if (!sounding[c]) continue;
            const f64 half = std::max(0.5, std::min<f64>(1, columns[c]) * high / 2);
            for (u32 y = 0; y < high; ++y)
                if (std::fabs(y + 0.5 - high / 2.0) <= half) std::fill_n(&px[(static_cast<usize>(y) * cols + c) * 4], 4, u8{255});
        }
        music_images_[0] = "px_m_" + std::to_string(++image_serial_);
        ui_->set_image(music_images_[0], px.data(), cols, high);
        m_music_wave_ = "/memory/" + music_images_[0];
        char style[96];
        std::snprintf(style, sizeof style, "width: %udevpx; height: %udevpx;", cols, high);
        m_music_wave_style_ = style;
    }
    for (const char* var : {"px_music", "px_music_marks", "px_music_wave", "px_music_wave_style"}) dirty(var);
}

bool PixelEditor::music_to_code(i32 call) {
    if (!rec_ok_ || call < 0 || call >= static_cast<i32>(rec_.music.size())) return false;
    const pixel::SceneMusic& m = rec_.music[static_cast<usize>(call)];
    const std::string what = m.name.empty() ? std::string("Тишина music(nil)") : "Музыка «" + m.name + "»";
    if (m.site.file != 0 || m.site.site < 0 || m.site.site >= static_cast<i32>(script().code().sites().size()))
        return refuse_edit(what + " — из модуля: её код не в панели");
    if (code_ != script_panel_) return refuse_edit("Код в панели не тот, что записан: сохраните его (Ctrl+S), и музыка покажет свой вызов");
    pixel::Block b;
    b.site = m.site;
    select_call(b);
    return true;
}

std::vector<PixelEditor::MusicNow> PixelEditor::music_now() const {
    std::vector<MusicNow> out;
    for (const MusicOn& m : music_on_) out.push_back({m.name, m.volume, m.call, m.voice});
    return out;
}

void PixelEditor::play_music(bool going) {
    std::vector<pixel::MusicVoice> want;
    if (going && sound_on_ && scene_mode_ && rec_ok_ && !rec_.music.empty())
        want = pixel::music_at(rec_, cut_.from + play_clock_, held_, held_at_);
    // What does not play now stops; what plays goes on at its volume; what begins, from where it is in its file.
    for (usize i = music_on_.size(); i-- > 0;)
        if (std::none_of(want.begin(), want.end(), [&](const pixel::MusicVoice& v) { return v.call == music_on_[i].call && v.name == music_on_[i].name; })) {
            mixer_.stop(music_on_[i].voice);
            music_on_.erase(music_on_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    for (const pixel::MusicVoice& v : want) {
        const auto on = std::find_if(music_on_.begin(), music_on_.end(), [&](const MusicOn& m) { return m.call == v.call; });
        if (on != music_on_.end()) {
            if (on->volume != static_cast<f32>(v.volume)) {
                on->volume = static_cast<f32>(v.volume);
                mixer_.set(on->voice, on->volume, 0);
            }
            continue;
        }
        const auto f = sound_files_.find(v.name);
        if (f == sound_files_.end() || !f->second.file.clip) continue; // no file: silence
        if (!mixer_tried_ && !offscreen) {
            mixer_tried_ = true;
            if (!mixer_.open()) FORGE_WARN("Звук просмотра: устройства нет (%s), звуки идут без него", SDL_GetError());
        }
        audio::Play p;
        p.volume = static_cast<f32>(v.volume);
        p.loop = true;
        p.bus = audio::Bus::Music;
        p.from = v.since;
        music_on_.push_back({v.call, v.name, mixer_.play(f->second.file.clip, p), p.volume});
        music_played_.push_back({v.name, v.since, v.call});
    }
}

void PixelEditor::hold_check() {
    if (!hold_on_ || !scene_mode_ || !rec_ok_ || held_at_ >= 0) return;
    const f64 now = cut_.from + play_clock_;
    if (next_stop_ >= rec_.stops.size()) return;
    const pixel::SceneStop& st = rec_.stops[next_stop_];
    if (st.time > cut_.to + 1e-9 || now + 1e-9 < st.time) return;
    // Reached: the player stands at its moment.
    play_clock_ = std::max(0.0, st.time - cut_.from);
    held_at_ = static_cast<i32>(next_stop_);
    held_.resize(rec_.stops.size(), 0);
    hold_actions_ = game_actions ? game_actions() : std::vector<input::Action>{};
    rebuild_held();
    lay_stops();
}

void PixelEditor::forget_holds(f64 from) {
    const bool had = held_at_ >= 0 || std::any_of(held_.begin(), held_.end(), [](f64 h) { return h != 0; });
    const bool stops = scene_mode_ && rec_ok_;
    held_.assign(stops ? rec_.stops.size() : 0, 0);
    held_at_ = -1;
    next_stop_ = 0;
    if (stops)
        while (next_stop_ < rec_.stops.size() && rec_.stops[next_stop_].time < from - 1e-9) ++next_stop_;
    if (had) {
        view_dirty_ = true; // the frames as recorded again
        rebuild_held();
        lay_stops();
    }
}

void PixelEditor::rebuild_held() {
    std::string words;
    if (held_at_ >= 0 && held_at_ < static_cast<i32>(rec_.stops.size())) {
        const pixel::SceneStop& st = rec_.stops[static_cast<usize>(held_at_)];
        if (st.kind == pixel::SceneStop::Kind::Talk) words = "Разговор «" + st.name + "»: ▶ — дальше";
        else if (st.name.empty()) words = "Ждёт клавишу: любая клавиша или ▶ — дальше";
        else if (const input::Action* a = stop_action(st, hold_actions_)) {
            const std::string keys = key_words(*a);
            words = "Ждёт «" + st.name + "»: " + (keys.empty() ? std::string("у действия нет клавиш, ▶ — дальше") : keys + " или ▶ — дальше");
        } else {
            words = "Ждёт «" + st.name + "»: такого действия во «Вводе» нет, ▶ — дальше";
        }
    }
    if (words != m_held_words_) {
        m_held_words_ = words;
        dirty("px_held_words");
    }
}

void PixelEditor::set_hold(bool on) {
    hold_on_ = m_hold_ = on;
    dirty("px_hold");
    // Off: the player goes through the flags, the frames as recorded; on: from where it is.
    forget_holds(playing_ ? cut_.from + play_clock_ : t_);
}

bool PixelEditor::go_on() {
    if (held_at_ < 0) return false;
    next_stop_ = static_cast<usize>(held_at_) + 1;
    held_at_ = -1;
    rebuild_held();
    lay_stops();
    return true;
}

void PixelEditor::toggle_play() {
    if (!go_on()) play(!playing_);
}

bool PixelEditor::hold_key(const SDL_KeyboardEvent& k) {
    if (held_at_ < 0 || held_at_ >= static_cast<i32>(rec_.stops.size()) || k.repeat || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)))
        return false;
    const pixel::SceneStop& st = rec_.stops[static_cast<usize>(held_at_)];
    if (st.kind != pixel::SceneStop::Kind::Action) return false;
    // Any key but one pressed with another (Shift, Ctrl …).
    if (st.name.empty()) return k.scancode >= SDL_SCANCODE_LCTRL && k.scancode <= SDL_SCANCODE_RGUI ? false : go_on();
    if (const input::Action* a = stop_action(st, hold_actions_))
        for (const input::Bind& b : a->binds)
            if (b.device == input::Device::Key && b.code == static_cast<i32>(k.scancode)) return go_on();
    return false;
}

} // namespace forge::editor_app
