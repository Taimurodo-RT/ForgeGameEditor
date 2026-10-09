// Forge editor shell: hierarchy, inspector, world view, log and history, with
// undo/redo of every action and Play/Stop. The window layout lives in
// ui/editor/editor.rml and editor.rcss and updates while the editor runs.
//
//   forge_editor [--scene FILE] [--level DIR] [--assets DIR] [--ui DIR] [--theme NAME] [--objects N] [--no-vsync]
//   forge_editor --screenshot out.png [--frames N] [--select] [--play] [--tab N] [--theme NAME]   offscreen
//   forge_editor --bench [--frames N]   offscreen: every object listed, the hierarchy scrolling
//   forge_editor --bench-level [--frames N]   offscreen: flying over the level while painting
//   forge_editor --bench-assets [N]   offscreen: a project of N files (50 000), indexed, listed, searched
//   forge_editor --bench-scheme [N]   offscreen: a scheme of N nodes (5 000) panned and a node dragged
//   forge_editor --bench-story   offscreen: a big talk scrolled with the mouse wheel
//   forge_editor --screenshot X.png --talk NAME   the «Сюжет» tab with that talk open (cast:STORY, novel:STORY)
//   forge_editor --screenshot X.png --templates WHAT   «Интерфейс»'s template library: construction, screen, a template's
//                file, new:FILE (a new screen from it), insert:FILE (it on the open screen), check:FILE (a new screen
//                from it in «Проверить»), save (the form «Сохранить как шаблон» for the open screen), save:LAYER
//                (for its layer of that name)
//   forge_editor --self-test [--screenshot out.png]   offscreen: drives the controls, fails on a wrong result
//
// The «Уровень» tab opens «Старая шахта» from games/slice/level (or --level
// DIR); the «Сцена» tab opens scene.forge.json in the current folder (or
// --scene FILE), or makes a sample scene of 50 000 objects when there is none.

#include "asset_library.h"
#include "components.h"
#include "demo_art.h"
#include "level_editor.h"
#include "logic_editor.h"
#include "project_folder.h"
#include "story_editor.h"
#include "ui_editor.h"
#include "object_library.h"
#include "slice_level.h"

#include "forge/assets/image.h"
#include "forge/audio/audio.h"
#include "forge/audio/screen_sounds.h"
#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/data/json.h"
#include "forge/editor/commands.h"
#include "forge/editor/document.h"
#include "forge/editor/inspector.h"
#include "forge/editor/undo.h"
#include "forge/platform/app.h"
#include "forge/render/camera.h"
#include "forge/render/gpu.h"
#include "forge/render/offscreen.h"
#include "forge/render/sprite_batch.h"
#include "forge/render/sprite_renderer.h"
#include "forge/script/graph.h"
#include "forge/ui/tokens.h"
#include "forge/ui/ui.h"
#include "forge/ui/virtual_list.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef FORGE_UI_DIR
#define FORGE_UI_DIR "ui"
#endif
#ifndef SLICE_LEVEL_DIR
#define SLICE_LEVEL_DIR "level"
#endif
#ifndef FORGE_EXAMPLES_DIR
#define FORGE_EXAMPLES_DIR "games/examples"
#endif
#ifndef FORGE_SLICE_EXE
#define FORGE_SLICE_EXE ""
#endif

using namespace forge;
using namespace forge::editor;
using namespace forge::editor_app;

namespace {

constexpr f32 kPi = 3.14159265f;


struct Stopwatch {
    u64 start = time_now_ns();
    f64 elapsed_ms() const { return ns_to_ms(time_now_ns() - start); }
};

// --- log panel -------------------------------------------------------------

// Messages from any thread wait here until the frame shows them.
struct LogQueue {
    std::mutex mutex;
    std::vector<std::pair<int, std::string>> pending;
};
LogQueue g_log;

void log_sink(LogLevel level, const char* message, void*) {
    if (level < LogLevel::Info) return;
    const int kind = level == LogLevel::Error ? 2 : level == LogLevel::Warn ? 1 : 0;
    std::lock_guard lock(g_log.mutex);
    if (g_log.pending.size() < 1000) g_log.pending.emplace_back(kind, message);
}

// --- view models for the RML document --------------------------------------

struct FieldView {
    Rml::String kind; // header, text, slider, bool, enum, color, group, readonly
    Rml::String label;
    Rml::String value;
    Rml::String color = "transparent"; // swatch of colour fields
    int depth = 0;
    float min = 0, max = 0, step = 0;
};

struct LogLine {
    Rml::String text;
    int level = 0;
};

// What a field row edits.
struct FieldRef {
    const reflect::TypeInfo* type = nullptr; // component
    std::string path;                        // empty for a component header
    std::vector<std::string> options;
};

std::string short_type_name(const reflect::TypeInfo* type) {
    const usize colon = type->name.rfind("::");
    return colon == std::string::npos ? type->name : type->name.substr(colon + 2);
}

std::string component_title(const reflect::TypeInfo* type) {
    const std::string name = short_type_name(type);
    if (name == "Transform") return "Положение";
    if (name == "Look") return "Внешний вид";
    if (name == "Mover") return "Движение";
    return name;
}

std::string group_digits(u64 n) {
    std::string digits = std::to_string(n), out;
    for (usize i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += " "; // narrow space
        out += digits[i];
    }
    return out;
}

u32 pack(const Color& c) {
    auto byte = [](f32 v) { return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return render::pack_color(byte(c.r), byte(c.g), byte(c.b), byte(c.a));
}

bool parse_hex(const std::string* text, f32 out[4]) {
    if (!text || text->size() < 7 || (*text)[0] != '#') return false;
    out[3] = 1;
    for (usize i = 0; i < 4 && 1 + i * 2 + 1 < text->size(); ++i)
        out[i] = static_cast<f32>(std::strtoul(text->substr(1 + i * 2, 2).c_str(), nullptr, 16)) / 255.0f;
    return true;
}

// --- sample scene ----------------------------------------------------------

void make_sample_scene(Document& doc, u32 objects) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
    const u32 per_group = 100;
    const u32 groups = std::max(1u, objects / per_group);
    const u32 columns = static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(groups))));
    const char* group_names[] = {"Стадо", "Склад", "Поляна", "Завод"};
    for (u32 g = 0; g < groups; ++g) {
        const u32 theme = g % 4;
        const ObjectId group = doc.create(std::string(group_names[theme]) + " " + std::to_string(g + 1));
        const f32 cx = static_cast<f32>(g % columns) * 40.0f + 20.0f;
        const f32 cy = static_cast<f32>(g / columns) * 40.0f + 20.0f;
        doc.add<Transform>(group)->position = {cx, cy};
        for (u32 i = 0; i < per_group; ++i) {
            Look look;
            const char* name = "Объект";
            switch (theme) {
            case 0:
                look.picture = Picture::Critter;
                look.variant = static_cast<u32>(unit(rng) * 8) % 8;
                name = "Зверь";
                break;
            case 1:
                look.picture = unit(rng) < 0.7f ? Picture::Crate : Picture::Chest;
                name = look.picture == Picture::Crate ? "Ящик" : "Сундук";
                break;
            case 2:
                look.picture = unit(rng) < 0.6f ? Picture::Leaf : Picture::Ball;
                look.tint = {0.6f + 0.4f * unit(rng), 0.8f + 0.2f * unit(rng), 0.6f + 0.4f * unit(rng), 1};
                name = look.picture == Picture::Leaf ? "Лист" : "Мяч";
                break;
            default:
                look.picture = unit(rng) < 0.5f ? Picture::Drill : Picture::Furnace;
                name = look.picture == Picture::Drill ? "Бур" : "Печь";
                break;
            }
            const ObjectId id = doc.create(std::string(name) + " " + std::to_string(g * per_group + i + 1), group);
            Transform* t = doc.add<Transform>(id);
            t->position = {cx + (unit(rng) - 0.5f) * 34.0f, cy + (unit(rng) - 0.5f) * 34.0f};
            t->scale = theme == 3 ? Vec2{2, 2} : Vec2{1, 1};
            *doc.add<Look>(id) = look;
            if (theme == 0 || (theme == 2 && look.picture == Picture::Ball)) {
                Mover* m = doc.add<Mover>(id);
                const f32 angle = unit(rng) * 2 * kPi;
                const f32 speed = 1.0f + unit(rng) * 3.0f;
                m->velocity = {std::cos(angle) * speed, std::sin(angle) * speed};
                m->spin = theme == 2 ? 180.0f : 0.0f;
                m->bounce_radius = 6.0f + unit(rng) * 8.0f;
            }
        }
    }
}

class Editor;

// Where this computer keeps the objects shared by every game: the user's
// application data folder (on Windows %APPDATA%\Forge\Forge).
std::filesystem::path default_shared_folder() {
    char* pref = SDL_GetPrefPath("Forge", "Forge");
    if (!pref) return {};
    std::filesystem::path out = utf8_path(pref) / utf8_path("Общие объекты");
    SDL_free(pref);
    return out;
}

// The hierarchy panel: the object tree flattened to the rows that are open.
class HierarchySource final : public ui::ListSource {
public:
    explicit HierarchySource(Editor& editor) : editor_(editor) {}
    u32 count() const override;
    u64 version() const override;
    std::string field(u32 row, std::string_view name) const override;
    void on_row_event(u32 row, std::string_view event, int modifiers) override;

    void refresh();
    void set_expanded(ObjectId id, bool expanded);
    void expand_all(bool expanded);
    i64 row_of(ObjectId id) const;
    ObjectId id_at(u32 row) const { return row < rows_.size() ? rows_[row].id : kNoObject; }

private:
    struct Row {
        ObjectId id;
        u32 depth;
    };
    void add(ObjectId id, u32 depth);

    Editor& editor_;
    std::vector<Row> rows_;
    std::unordered_set<ObjectId> expanded_;
    u64 built_structure_ = 0;
    u64 expand_version_ = 1;
    u64 built_expand_ = 0;
    u32 anchor_ = 0; // for shift-click ranges
};

// One drawable object, cached while the document's structure stays the same.
struct Drawable {
    ObjectId id;
    Transform* transform;
    Look* look;
    Mover* mover;
    Vec2 home; // where it was when Play started
};

class Editor {
public:
    Document doc;
    UndoStack history{doc};
    PlaySession play;
    HierarchySource hierarchy{*this};
    std::filesystem::path scene_path = "scene.forge.json";
    // The game's object kinds and where its templates are kept.
    std::filesystem::path game_dir = utf8_path(SLICE_DATA_DIR);
    std::filesystem::path objects_folder = game_dir / "objects";
    std::filesystem::path pictures_folder = game_dir / "pictures"; // the templates' own pictures
    std::filesystem::path sounds_folder = game_dir / "sounds";     // and sounds
    std::filesystem::path logic_file = game_dir / "logic.json";     // the links («Логика»)
    std::filesystem::path story_dir = game_dir;                     // dialogues/ and quests.json («Сюжет»)
    std::filesystem::path ui_game_dir = game_dir;                   // ui/ («Интерфейс»)

    // Before init: the game's data from another folder (the project's game/).
    void use_game_dir(const std::filesystem::path& dir) {
        game_dir = dir;
        objects_folder = dir / "objects";
        pictures_folder = dir / "pictures";
        sounds_folder = dir / "sounds";
        logic_file = dir / "logic.json";
        story_dir = dir;
        ui_game_dir = dir;
    }
    // The objects shared by every game: this computer's, outside any game.
    std::filesystem::path shared_folder = default_shared_folder();
    slice::SliceLevel level_module;
    LevelEditor level{level_module};
    ObjectLibrary objects_tab{level_module};
    LogicEditor logic_tab{level_module};
    StoryEditor story_tab{level_module.library()};
    UiEditor ui_tab;
    AssetLibrary assets;

    bool init(SDL_GPUDevice* device, SDL_Window* window, SDL_GPUTextureFormat format, u32 width, u32 height,
              const std::filesystem::path& ui_dir, const std::string& theme, u32 objects, const LevelConfig& level_config,
              const AssetsConfig& assets_config) {
        device_ = device;
        window_ = window;
        log_set_sink(&log_sink, nullptr);

        std::vector<u8> bytes;
        if (std::filesystem::exists(scene_path) && read_file(scene_path, bytes)) {
            std::string error;
            const Stopwatch timer;
            if (doc.load_json(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &error))
                FORGE_INFO("Сцена %s открыта: %s объектов за %.0f мс", path_to_utf8(scene_path).c_str(),
                           group_digits(doc.object_count()).c_str(), timer.elapsed_ms());
            else
                FORGE_ERROR("Сцена %s не открылась: %s", path_to_utf8(scene_path).c_str(), error.c_str());
        }
        if (doc.object_count() == 0) {
            const Stopwatch timer;
            make_sample_scene(doc, objects);
            FORGE_INFO("Создана пробная сцена: %s объектов за %.0f мс", group_digits(doc.object_count()).c_str(),
                       timer.elapsed_ms());
        }
        history.clear();

        art_ = demo::make_sprite_sheet();
        if (!sprites_.init(device, format, art_.sheet(), 1u << 18)) return false;

        ui::UiConfig config;
        config.root = ui_dir;
        config.theme = theme;
        config.hot_reload = window != nullptr;
        if (!ui_.init(device, window, config)) return false;
        level_module.library()->set_pictures_folder(pictures_folder);
        level_module.library()->set_sounds_folder(sounds_folder);
        if (std::string error; !level_module.library()->load(game_dir / "kinds.json", objects_folder, &error))
            FORGE_ERROR("Объекты не загрузились: %s", error.c_str());
        if (!level.init(ui_, device, format, level_config)) return false;
        objects_tab.set_shared_folder(shared_folder);
        objects_tab.init(ui_);
        objects_tab.list_images = [this] { return assets.images(); };
        objects_tab.list_sounds = [this] { return assets.sounds(); };
        ui_tab.list_sounds = [this] { return assets.sounds(); };
        objects_tab.on_place = [this](u64 key) {
            open_tab("level");
            level.arm_template(key);
        };
        logic_tab.template_icon = [this](const objects::Template& t) { return objects_tab.template_icon(t); };
        logic_tab.set_settings(level_config.settings, !level_config.offscreen);
        if (!level_config.offscreen) logic_tab.set_fired_file(LevelEditor::fired_file());
        logic_tab.init(ui_, game_dir, logic_file);
        story_tab.template_icon = [this](const objects::Template& t) { return objects_tab.template_icon(t); };
        story_tab.window = window;
        story_tab.init(ui_, story_dir);
        if (!assets.init(ui_, assets_config)) return false;
        context_ = ui_.create_context("editor", width, height);
        // What screens can show: the game's values in the author's words.
        ui_tab.game_values = [this] {
            std::vector<std::pair<std::string, std::string>> out = {
                {"hero.hearts", "Сердца героя"}, {"hero.hearts_max", "Сердец всего"}, {"inv.coins", "Монеты"}, {"inv.copper", "Медь"},
                {"settings.master", "Громкость всего, 0–100"}, {"settings.music", "Громкость музыки, 0–100"},
                {"settings.sound", "Громкость звуков, 0–100"}};
            const objects::Library& lib = *level_module.library();
            for (const objects::Template& t : lib.templates()) {
                if (!lib.has_block(t, "pickup")) continue;
                const objects::PropDef* what = lib.prop_of(t, "what");
                if (!what) continue;
                std::string item = lib.value(t, *what);
                if (item.size() >= 2 && item.front() == '"') item = item.substr(1, item.size() - 2);
                if (!item.empty()) out.emplace_back("inv." + item, t.name);
            }
            for (const game::Quest& q : story_tab.quests().quests())
                if (!q.var.empty()) out.emplace_back(q.var, "Задание «" + q.title + "»");
            std::vector<std::pair<std::string, std::string>> unique;
            for (auto& [name, label] : out)
                if (std::none_of(unique.begin(), unique.end(), [&](const auto& u) { return u.first == name; }))
                    unique.emplace_back(name, label + " · " + name);
            return unique;
        };
        // Where «Логика» shows a screen: blocks «Показать экран» (or «Показать или скрыть») with its name written in.
        ui_tab.logic_openers = [this](const std::string& screen) {
            std::vector<std::string> out;
            auto shows = [&](const std::string& json) {
                script::Graph g;
                if (json.empty() || !g.from_json(json)) return false;
                for (const script::GraphNode& n : g.nodes) {
                    if (n.def != "api.ui.show" && n.def != "api.ui.toggle") continue;
                    // A wire into «Экран» wins over the name written in.
                    const bool wired = std::any_of(g.links.begin(), g.links.end(), [&](const script::GraphLink& l) {
                        return l.to_node == n.uid && l.to_pin == "name";
                    });
                    const std::string* name = n.value("name");
                    if (!wired && name && *name == screen) return true;
                }
                return false;
            };
            const logic::Logic& game = logic_tab.links();
            for (const logic::Link& l : game.links)
                if (shows(l.graph)) out.push_back("связь «" + logic_tab.phrase_of(l.id) + "»");
            const objects::Library& lib = *level_module.library();
            for (const logic::ThingScheme& t : game.schemes)
                if (shows(t.graph)) {
                    const objects::Template* of = lib.find(std::string_view(t.thing));
                    out.push_back("схема «" + (of ? of->name : t.thing) + "»");
                }
            return out;
        };
        ui_tab.game_items = [this] {
            std::vector<game::ScreenItem> out;
            const objects::Library& lib = *level_module.library();
            for (const objects::Template& t : lib.templates()) {
                const std::string item = lib.item_of(t);
                if (item.empty()) continue;
                // item.icon: the template's own picture, as the game's pages
                // find it (several pickups of one thing: the first with one).
                const std::string picture = lib.picture_in(t, ui_game_dir);
                auto have = std::find_if(out.begin(), out.end(), [&](const game::ScreenItem& i) { return i.id == item; });
                if (have == out.end()) out.push_back({item, t.name, picture, t.about});
                else if (have->picture.empty()) have->picture = picture;
            }
            return out;
        };
        ui_tab.game_quests = [this] { return &story_tab.quests(); };
        // The «Интерфейс» tab draws game screens in a context of its own (after the editor's: F8 inspects the editor).
        if (!context_ || !ui_tab.init(ui_, ui_game_dir) || !bind_model()) return false;
        ui::register_list_source("hierarchy", &hierarchy);
        // Loading fills the inputs from the model, which fires their change
        // events: not edits.
        level.set_ui_updating(true);
        objects_tab.set_ui_updating(true);
        const bool loaded = ui_.load_document(context_, "editor/editor.rml");
        level.set_ui_updating(false);
        objects_tab.set_ui_updating(false);
        if (!loaded) return false;

        // Start looking at the first group.
        if (!doc.roots().empty())
            if (const Transform* t = doc.component<Transform>(doc.roots()[0])) {
                camera_.x = t->position.x;
                camera_.y = t->position.y;
            }
        camera_.zoom = 24.0f;
        return true;
    }

    void shutdown() {
        // Closing never loses painted tiles: the level is saved.
        if (level.dirty()) level.save();
        level.shutdown();
        assets.shutdown();
        ui_tab.shutdown();
        log_set_sink(nullptr, nullptr);
        ui::register_list_source("hierarchy", nullptr);
        sprites_.shutdown();
        ui_.shutdown();
    }

    // --- actions (buttons, keys) ---

    void undo() {
        if (m_tab_ == "assets") assets.undo();
        else if (m_tab_ == "objects") objects_tab.undo();
        else if (m_tab_ == "logic") logic_tab.undo();
        else if (m_tab_ == "story") story_tab.undo();
        else if (m_tab_ == "ui") ui_tab.undo();
        else if (m_tab_ == "level") level.undo();
        else if (history.undo()) FORGE_INFO("Отменено");
    }
    void redo() {
        if (m_tab_ == "assets") assets.redo();
        else if (m_tab_ == "objects") objects_tab.redo();
        else if (m_tab_ == "logic") logic_tab.redo();
        else if (m_tab_ == "story") story_tab.redo();
        else if (m_tab_ == "ui") ui_tab.redo();
        else if (m_tab_ == "level") level.redo();
        else if (history.redo()) FORGE_INFO("Повторено");
    }
    // The open tab's history.
    UndoStack& active_history() {
        if (m_tab_ == "assets") return assets.history();
        if (m_tab_ == "objects") return objects_tab.history();
        if (m_tab_ == "logic") return logic_tab.history();
        if (m_tab_ == "story") return story_tab.history();
        if (m_tab_ == "ui") return ui_tab.history();
        return m_tab_ == "level" ? level.history() : history;
    }
    void open_tab(const std::string& key) {
        if (key == "assets" && m_tab_ != "assets") assets.opened();
        m_tab_ = key;
        model_.DirtyVariable("tab");
    }
    const std::string& tab() const { return m_tab_; }
    void save() {
        if (m_tab_ == "assets" || m_tab_ == "objects" || m_tab_ == "logic" || m_tab_ == "story" || m_tab_ == "ui") return; // files are saved as they change
        if (m_tab_ == "level") {
            level.save();
            return;
        }
        const Stopwatch timer;
        const std::string text = doc.save_json();
        if (write_file_atomic(scene_path, std::span(reinterpret_cast<const u8*>(text.data()), text.size()))) {
            history.mark_saved();
            FORGE_INFO("Сохранено: %s (%.1f МБ, %.0f мс)", path_to_utf8(scene_path).c_str(),
                       static_cast<f64>(text.size()) / (1024.0 * 1024.0), timer.elapsed_ms());
        } else {
            FORGE_ERROR("Не удалось сохранить %s", path_to_utf8(scene_path).c_str());
        }
    }
    void toggle_play() {
        const Stopwatch timer;
        if (!play.playing()) {
            play.start(doc, history);
            paused_ = false;
            for (Drawable& d : drawables_) d.home = d.transform->position;
            FORGE_INFO("Игра запущена (снимок сцены %.1f МБ за %.0f мс)", static_cast<f64>(play.snapshot_bytes()) / (1024.0 * 1024.0),
                       timer.elapsed_ms());
        } else {
            play.stop(doc, history);
            FORGE_INFO("Игра остановлена, сцена восстановлена за %.0f мс", timer.elapsed_ms());
        }
    }
    void select(std::vector<ObjectId> ids) {
        if (ids == doc.selection()) return;
        history.execute(std::make_unique<Select>(doc, std::move(ids)));
    }
    void delete_selection() {
        std::vector<ObjectId> ids = doc.selection();
        if (ids.empty()) return;
        history.begin_group(ids.size() == 1 ? "Удалить «" + doc.find(ids[0])->name + "»"
                                            : "Удалить объекты: " + std::to_string(ids.size()));
        select({});
        for (ObjectId id : ids)
            if (doc.find(id)) history.execute(std::make_unique<DeleteObject>(doc, id));
        history.end_group();
    }
    void duplicate_selection() {
        std::vector<ObjectId> ids = doc.selection(), copies;
        if (ids.empty()) return;
        history.begin_group("Копировать");
        for (ObjectId id : ids) {
            const Object* o = doc.find(id);
            if (!o) continue;
            auto create = std::make_unique<CreateObject>(doc.object_json(id, false), o->parent,
                                                         doc.index_in_parent(id) + 1, "Копировать");
            CreateObject* raw = create.get();
            history.execute(std::move(create));
            copies.push_back(raw->id());
            if (Transform* t = doc.component<Transform>(raw->id())) {
                const std::string before = doc.component_json(raw->id(), reflect::type_of<Transform>());
                Transform moved = *t;
                moved.position.x += 1;
                moved.position.y += 1;
                history.execute(std::make_unique<SetComponent>(raw->id(), reflect::type_of<Transform>(), before,
                                                               data::to_json(moved, false)));
            }
        }
        select(copies);
        history.end_group();
    }
    void add_object() {
        const ObjectId parent = doc.selection().empty() ? kNoObject : doc.find(doc.selection()[0])->parent;
        history.begin_group("Создать объект");
        auto create = CreateObject::named(doc, "Новый объект", parent);
        const ObjectId id = create->id();
        history.execute(std::move(create));
        Transform t;
        t.position = {static_cast<f32>(camera_.x), static_cast<f32>(camera_.y)};
        history.execute(std::make_unique<AddComponent>(id, reflect::type_of<Transform>(), data::to_json(t, false)));
        history.execute(std::make_unique<AddComponent>(id, reflect::type_of<Look>()));
        select({id});
        history.end_group();
        reveal(id);
    }
    void focus_selection() {
        if (doc.selection().empty()) return;
        if (const Transform* t = doc.component<Transform>(doc.selection()[0])) {
            camera_.x = t->position.x;
            camera_.y = t->position.y;
        }
    }
    // Opens the parents of an object and scrolls the hierarchy to it.
    void reveal(ObjectId id) {
        for (const Object* o = doc.find(id); o && o->parent != kNoObject; o = doc.find(o->parent))
            hierarchy.set_expanded(o->parent, true);
        hierarchy.refresh();
        const i64 row = hierarchy.row_of(id);
        if (row >= 0) ui::scroll_list_to("hierarchy", static_cast<u32>(row));
    }

    // --- frame ---

    void update(f64 dt) {
        time_ += dt;
        drain_log();
        if (!pending_theme_.empty()) {
            if (ui_.set_theme(pending_theme_)) theme_ = pending_theme_;
            model_.DirtyVariable("theme");
            pending_theme_.clear();
        }
        if (m_tab_ == "level") level.update(dt, context_);
        assets.update(dt, m_tab_ == "assets" ? context_ : nullptr);
        if (m_tab_ == "objects") objects_tab.update(context_);
        if (m_tab_ == "logic") logic_tab.update(context_);
        if (m_tab_ == "story") story_tab.update(context_);
        ui_tab.set_shown(m_tab_ == "ui");
        if (m_tab_ == "ui") ui_tab.update(context_);
        refresh_drawables();
        if (play.playing() && !paused_) simulate(static_cast<f32>(std::min(dt, 0.1)));
        hierarchy.refresh();
        sync_model();
        // Bindings may fire change events while they write values into the
        // document (a slider snapping to its step, a field losing focus as it
        // is rebuilt); those are not the user's edits.
        in_ui_update_ = true;
        level.set_ui_updating(true);
        assets.set_ui_updating(true);
        objects_tab.set_ui_updating(true);
        ui_.update();
        // The colour picker stands beside its swatch once its size is known.
        if (m_tab_ == "ui" && ui_tab.place_picker(context_)) context_->Update();
        // A key whose time was typed moved among its keys: the keyboard stays with that key's row.
        if (m_tab_ == "ui" && ui_tab.follow_moved_key(context_)) context_->Update();
        level.set_ui_updating(false);
        assets.set_ui_updating(false);
        objects_tab.set_ui_updating(false);
        in_ui_update_ = false;
        follow_log();
    }

    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUTextureFormat format, u32 w, u32 h) {
        if (context_->GetDimensions() != Rml::Vector2i(static_cast<int>(w), static_cast<int>(h)))
            context_->SetDimensions({static_cast<int>(w), static_cast<int>(h)});

        // Background in the theme's colour; the world goes into the viewport.
        f32 bg[4] = {0.07f, 0.09f, 0.1f, 1};
        parse_hex(ui_.tokens().find("surface"), bg);

        viewport_rect();
        render_world(cmd, target, bg);
        // Layout during rendering may move values into inputs too (a slider
        // takes its range only then): not the user's edits either.
        level.set_ui_updating(true);
        objects_tab.set_ui_updating(true);
        ui_.render(cmd, target, format, w, h);
        level.set_ui_updating(false);
        objects_tab.set_ui_updating(false);
    }

    bool handle_event(const SDL_Event& e) {
        const f32 density = window_ ? SDL_GetWindowPixelDensity(window_) : 1.0f;
        // The template library of «Интерфейс», even from its search field: Esc closes it, Enter takes the template selected.
        if (e.type == SDL_EVENT_KEY_DOWN && m_tab_ == "ui" && ui_tab.templates_open()) {
            if (e.key.key == SDLK_ESCAPE) {
                ui_tab.close_templates();
                return true;
            }
            if ((e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && !(e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
                if (!e.key.repeat) ui_tab.templates_enter(); // the form saved, or the selected template taken
                return true;
            }
        }
        // Keys first, unless a text field has the keyboard.
        if (e.type == SDL_EVENT_KEY_DOWN && !text_focus() && handle_key(e.key)) return true;

        // Files dropped on the window go to the project's resources.
        if (e.type == SDL_EVENT_DROP_BEGIN && m_tab_ != "assets") open_tab("assets");
        const bool ui_used = ui_.handle_event(context_, e);
        if (m_tab_ == "assets") return assets.handle_event(e, density, ui_used, context_) || ui_used;
        if (m_tab_ == "level") return level.handle_event(e, density, ui_used, context_) || ui_used;
        if (m_tab_ == "objects") return ui_used;
        if (m_tab_ == "logic") return logic_tab.handle_event(e, density, ui_used) || ui_used;
        if (m_tab_ == "story") return story_tab.handle_event(e) || ui_used;
        if (m_tab_ == "ui") return ui_tab.handle_event(e, density, context_) || ui_used;
        switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            const f32 x = e.button.x * density, y = e.button.y * density;
            if (!over_viewport(x, y)) break;
            if (e.button.button == SDL_BUTTON_LEFT) begin_drag(x, y, SDL_GetModState() & SDL_KMOD_CTRL);
            else panning_ = true;
            last_mouse_x_ = x;
            last_mouse_y_ = y;
            return true;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            const f32 x = e.motion.x * density, y = e.motion.y * density;
            if (panning_) {
                camera_.x -= (x - last_mouse_x_) / camera_.zoom;
                camera_.y -= (y - last_mouse_y_) / camera_.zoom;
            }
            if (dragging_ != kNoObject) drag_to(x, y);
            last_mouse_x_ = x;
            last_mouse_y_ = y;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            panning_ = false;
            if (dragging_ != kNoObject) {
                dragging_ = kNoObject;
                history.seal();
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            f32 mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            mx *= density;
            my *= density;
            if (!over_viewport(mx, my)) break;
            f64 before_x, before_y, after_x, after_y;
            to_tile(mx, my, before_x, before_y);
            camera_.zoom = std::clamp(camera_.zoom * std::pow(1.15f, e.wheel.y), 0.5f, 160.0f);
            to_tile(mx, my, after_x, after_y);
            camera_.x += before_x - after_x;
            camera_.y += before_y - after_y;
            return true;
        }
        default: break;
        }
        return ui_used;
    }

    // Where an object is on screen, in pixels (for the self-test).
    bool screen_of(ObjectId id, f32& x, f32& y) {
        refresh_drawables();
        viewport_rect();
        const Transform* t = doc.component<Transform>(id);
        if (!t) return false;
        x = vx_ + static_cast<f32>((t->position.x - camera_.x) * camera_.zoom) + vw_ * 0.5f;
        y = vy_ + static_cast<f32>((t->position.y - camera_.y) * camera_.zoom) + vh_ * 0.5f;
        return x >= vx_ && y >= vy_ && x < vx_ + vw_ && y < vy_ + vh_;
    }
    f32 zoom() const { return camera_.zoom; }

    ui::Ui& ui() { return ui_; }
    bool paused() const { return paused_; }
    Rml::Context* context() { return context_; }
    Rml::Element* find_element(const char* id) {
        for (int i = 0; i < context_->GetNumDocuments(); ++i)
            if (Rml::Element* e = context_->GetDocument(i)->GetElementById(id)) return e;
        return nullptr;
    }

    void list_click(u32 row, int modifiers) {
        const ObjectId id = hierarchy.id_at(row);
        if (id == kNoObject) return;
        std::vector<ObjectId> ids = doc.selection();
        if (modifiers & Rml::Input::KM_CTRL) {
            auto it = std::find(ids.begin(), ids.end(), id);
            if (it != ids.end()) ids.erase(it);
            else ids.push_back(id);
        } else if ((modifiers & Rml::Input::KM_SHIFT) && list_anchor_ != kNoObject) {
            const i64 a = hierarchy.row_of(list_anchor_);
            ids.clear();
            if (a >= 0)
                for (i64 r = std::min<i64>(a, row); r <= std::max<i64>(a, row); ++r)
                    ids.push_back(hierarchy.id_at(static_cast<u32>(r)));
        } else {
            ids = {id};
        }
        if (!(modifiers & Rml::Input::KM_SHIFT)) list_anchor_ = id;
        select(std::move(ids));
    }

private:
    // --- data model ---

    bool bind_model() {
        Rml::DataModelConstructor model = context_->CreateDataModel("editor");
        if (!model) return false;
        if (auto s = model.RegisterStruct<FieldView>()) {
            s.RegisterMember("kind", &FieldView::kind);
            s.RegisterMember("label", &FieldView::label);
            s.RegisterMember("value", &FieldView::value);
            s.RegisterMember("color", &FieldView::color);
            s.RegisterMember("depth", &FieldView::depth);
            s.RegisterMember("min", &FieldView::min);
            s.RegisterMember("max", &FieldView::max);
            s.RegisterMember("step", &FieldView::step);
        }
        if (auto s = model.RegisterStruct<LogLine>()) {
            s.RegisterMember("text", &LogLine::text);
            s.RegisterMember("level", &LogLine::level);
        }
        model.RegisterArray<std::vector<FieldView>>();
        model.RegisterArray<std::vector<LogLine>>();
        model.RegisterArray<std::vector<Rml::String>>();

        theme_ = ui_.theme();
        model.Bind("theme", &theme_);
        model.Bind("playing", &m_playing_);
        model.Bind("paused", &paused_);
        model.Bind("can_undo", &m_can_undo_);
        model.Bind("can_redo", &m_can_redo_);
        model.Bind("undo_label", &m_undo_label_);
        model.Bind("dirty", &m_dirty_);
        model.Bind("scene_name", &m_scene_name_);
        model.Bind("has_selection", &m_has_selection_);
        model.Bind("selection_count", &m_selection_count_);
        model.Bind("selected_name", &m_selected_name_);
        model.Bind("fields", &m_fields_);
        model.Bind("addable", &m_addable_);
        model.Bind("log", &m_log_);
        model.Bind("history", &m_history_);
        model.Bind("history_cursor", &m_history_cursor_);
        model.Bind("bottom_tab", &m_bottom_tab_);
        model.Bind("object_count", &m_object_count_);
        model.Bind("status", &m_status_);
        model.Bind("zoom", &m_zoom_);
        model.Bind("tab", &m_tab_);
        model.Bind("tab_title", &m_tab_title_);
        model.Bind("tab_description", &m_tab_description_);
        model.Bind("tab_step", &m_tab_step_);
        model.Bind("tab_icon", &m_tab_icon_);

        auto on = [&](const char* name, auto fn) {
            model.BindEventCallback(name, [this, fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) {
                fn(ev, args);
            });
        };
        auto arg_int = [](const Rml::VariantList& a, usize i, int fallback = 0) {
            return i < a.size() ? a[i].Get<int>() : fallback;
        };
        auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };

        on("undo", [this](Rml::Event&, const Rml::VariantList&) { undo(); });
        on("redo", [this](Rml::Event&, const Rml::VariantList&) { redo(); });
        on("save", [this](Rml::Event&, const Rml::VariantList&) { save(); });
        on("play", [this](Rml::Event&, const Rml::VariantList&) {
            if (m_tab_ == "level") level.play_here();
            else if (!play.playing()) toggle_play();
        });
        on("stop", [this](Rml::Event&, const Rml::VariantList&) { if (play.playing()) toggle_play(); });
        on("pause", [this](Rml::Event&, const Rml::VariantList&) {
            if (play.playing()) paused_ = !paused_;
            model_.DirtyVariable("paused");
        });
        on("set_theme", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { pending_theme_ = arg_str(a, 0); });
        // Editor tabs: the list, titles and texts live in editor.rml.
        on("open_tab", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
            if (arg_str(a, 0) == "assets" && m_tab_ != "assets") assets.opened();
            m_tab_ = arg_str(a, 0);
            m_tab_title_ = arg_str(a, 1);
            m_tab_description_ = arg_str(a, 2);
            m_tab_step_ = arg_str(a, 3);
            m_tab_icon_ = arg_str(a, 4);
            for (const char* name : {"tab", "tab_title", "tab_description", "tab_step", "tab_icon"})
                model_.DirtyVariable(name);
        });
        on("add_object", [this](Rml::Event&, const Rml::VariantList&) { add_object(); });
        on("delete_selection", [this](Rml::Event&, const Rml::VariantList&) { delete_selection(); });
        on("expand_all", [this](Rml::Event&, const Rml::VariantList& a) {
            hierarchy.expand_all(!a.empty() && a[0].Get<bool>());
        });
        on("history_jump", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
            const usize target = static_cast<usize>(arg_int(a, 0, -1) + 1);
            while (history.cursor() > target && history.undo()) {}
            while (history.cursor() < target && history.redo()) {}
        });
        on("rename", [this, arg_str](Rml::Event& ev, const Rml::VariantList& a) {
            const bool enter = a.size() > 1 && a[1].Get<bool>();
            if (enter) commit_rename(arg_str(a, 0));
            (void)ev;
        });
        on("rename_commit", [this](Rml::Event& ev, const Rml::VariantList&) {
            if (auto* input = rml_input(ev.GetTargetElement())) commit_rename(input->GetValue());
        });
        on("field_text", [this, arg_int, arg_str](Rml::Event&, const Rml::VariantList& a) {
            const bool enter = a.size() > 2 && a[2].Get<bool>();
            if (enter) set_field(arg_int(a, 0, -1), arg_str(a, 1), false);
        });
        on("field_commit", [this, arg_int](Rml::Event& ev, const Rml::VariantList& a) {
            if (auto* input = rml_input(ev.GetTargetElement())) set_field(arg_int(a, 0, -1), input->GetValue(), false);
        });
        on("field_slide", [this, arg_int, arg_str](Rml::Event&, const Rml::VariantList& a) {
            set_field(arg_int(a, 0, -1), arg_str(a, 1), true);
        });
        on("field_toggle", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
            const int i = arg_int(a, 0, -1);
            if (i >= 0 && i < static_cast<int>(m_fields_.size()))
                set_field(i, m_fields_[i].value == "true" ? "false" : "true", false);
        });
        on("field_cycle", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
            const int i = arg_int(a, 0, -1);
            if (i < 0 || i >= static_cast<int>(field_refs_.size())) return;
            const auto& options = field_refs_[i].options;
            if (options.empty()) return;
            auto it = std::find(options.begin(), options.end(), m_fields_[i].value);
            const i64 at = it == options.end() ? 0 : it - options.begin();
            const i64 n = static_cast<i64>(options.size());
            set_field(i, options[static_cast<usize>(((at + arg_int(a, 1, 1)) % n + n) % n)], false);
        });
        on("remove_component", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
            const int i = arg_int(a, 0, -1);
            if (i < 0 || i >= static_cast<int>(field_refs_.size()) || doc.selection().empty()) return;
            history.execute(std::make_unique<RemoveComponent>(doc, doc.selection()[0], field_refs_[i].type));
        });
        on("add_component", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
            const int i = arg_int(a, 0, -1);
            if (i < 0 || i >= static_cast<int>(addable_types_.size()) || doc.selection().empty()) return;
            history.execute(std::make_unique<AddComponent>(doc.selection()[0], addable_types_[i]));
        });
        DockView::register_types(model);
        level.bind(model);
        assets.bind(model);
        objects_tab.bind(model);
        logic_tab.bind(model);
        story_tab.bind(model);
        ui_tab.bind(model);
        model_ = model.GetModelHandle();
        level.set_model(model_);
        assets.set_model(model_);
        objects_tab.set_model(model_);
        logic_tab.set_model(model_);
        story_tab.set_model(model_);
        ui_tab.set_model(model_);
        return true;
    }

    static Rml::ElementFormControl* rml_input(Rml::Element* e) {
        if (!e || (e->GetTagName() != "input" && e->GetTagName() != "textarea")) return nullptr;
        return static_cast<Rml::ElementFormControl*>(e);
    }

    bool text_focus() const {
        const Rml::Element* focus = context_ ? context_->GetFocusElement() : nullptr;
        if (!focus || (focus->GetTagName() != "input" && focus->GetTagName() != "textarea")) return false;
        const Rml::String type = focus->GetAttribute<Rml::String>("type", "text");
        return type == "text" || type == "password" || focus->GetTagName() == "textarea";
    }

    void commit_rename(const std::string& name) {
        if (in_ui_update_ || doc.selection().empty() || name.empty()) return;
        const ObjectId id = doc.selection()[0];
        if (doc.find(id) && doc.find(id)->name != name) {
            history.execute(std::make_unique<RenameObject>(doc, id, name));
            history.seal();
        }
    }

    // Applies text to field row i of the inspector through an undoable command.
    void set_field(int i, const std::string& text, bool dragging) {
        if (in_ui_update_) return;
        if (i < 0 || i >= static_cast<int>(field_refs_.size()) || doc.selection().empty()) return;
        if (text == m_fields_[static_cast<usize>(i)].value) return; // shown rounded; not an edit
        const FieldRef& ref = field_refs_[i];
        if (ref.path.empty()) return;
        const ObjectId id = doc.selection()[0];
        const std::string before = doc.component_json(id, ref.type);
        if (before.empty()) return;
        Component scratch(ref.type);
        data::LoadReport report;
        data::from_json(ref.type, scratch.data(), before, report);
        std::string error;
        if (!set_field_text(ref.type, scratch.data(), ref.path, text, &error)) {
            FORGE_WARN("%s: %s", m_fields_[i].label.c_str(), error.c_str());
            fields_dirty_ = true; // show the old value again
            return;
        }
        const std::string after = data::to_json(ref.type, scratch.data(), false);
        if (after == before) return;
        const Object* o = doc.find(id);
        history.execute(std::make_unique<SetComponent>(id, ref.type, before, after, ref.path,
                                                       "«" + (o ? o->name : std::string()) + "»: " +
                                                           m_fields_[static_cast<usize>(i)].label));
        if (!dragging) history.seal();
    }

    void rebuild_fields() {
        m_fields_.clear();
        field_refs_.clear();
        m_addable_.clear();
        addable_types_.clear();
        const Object* o = doc.selection().empty() ? nullptr : doc.find(doc.selection()[0]);
        if (!o) return;
        std::vector<FieldRow> rows;
        for (const auto& c : o->components) {
            FieldView header;
            header.kind = "header";
            header.label = component_title(c->type());
            m_fields_.push_back(std::move(header));
            field_refs_.push_back({c->type(), {}, {}});
            rows.clear();
            describe_fields(c->type(), c->data(), rows);
            for (FieldRow& r : rows) {
                FieldView v;
                v.label = r.label;
                v.value = r.value;
                v.depth = static_cast<int>(r.depth);
                using reflect::Kind;
                const bool integer = r.kind >= Kind::I8 && r.kind <= Kind::U64;
                if (r.read_only) v.kind = "readonly";
                else if (r.kind == Kind::Bool) v.kind = "bool";
                else if (r.kind == Kind::Enum) v.kind = "enum";
                else if (r.kind == Kind::Color) v.kind = "color", v.color = r.value;
                else if (r.kind == Kind::Struct || r.kind == Kind::Array) v.kind = "group";
                else if ((integer || r.kind == Kind::F32 || r.kind == Kind::F64) && r.has_range) v.kind = "slider";
                else v.kind = "text";
                if (r.kind == Kind::Array) v.value = "элементов: " + r.value;
                v.min = static_cast<float>(r.min);
                v.max = static_cast<float>(r.max);
                v.step = integer ? 1.0f : static_cast<float>((r.max - r.min) / 200.0);
                m_fields_.push_back(std::move(v));
                field_refs_.push_back({c->type(), r.path, std::move(r.options)});
            }
        }
        for (const reflect::TypeInfo* type :
             {reflect::type_of<Transform>(), reflect::type_of<Look>(), reflect::type_of<Mover>()}) {
            if (o->find(type)) continue;
            addable_types_.push_back(type);
            m_addable_.push_back(component_title(type));
        }
    }

    template <typename T>
    void set(T& member, const T& value, const char* name) {
        if (member == value) return;
        member = value;
        model_.DirtyVariable(name);
    }

    void sync_model() {
        set(m_playing_, play.playing(), "playing");
        UndoStack& h = active_history();
        set(m_can_undo_, h.can_undo(), "can_undo");
        set(m_can_redo_, h.can_redo(), "can_redo");
        set(m_undo_label_, h.undo_label(), "undo_label");
        set(m_dirty_, m_tab_ != "assets" && m_tab_ != "objects" && m_tab_ != "logic" && m_tab_ != "story" && m_tab_ != "ui" && h.dirty(), "dirty"); // files are written at once
        set(m_scene_name_,
            m_tab_ == "level"    ? level.title()
            : m_tab_ == "assets" ? std::string("Ресурсы проекта")
            : m_tab_ == "objects" ? std::string("Объекты: ") + level.title()
            : m_tab_ == "logic"   ? std::string("Логика: ") + level.title()
            : m_tab_ == "story"   ? std::string("Сюжет: ") + level.title()
            : m_tab_ == "ui"      ? std::string("Интерфейс: ") + ui_tab.screen().title
                                 : path_to_utf8(scene_path.filename()),
            "scene_name");
        set(m_has_selection_, !doc.selection().empty() && doc.find(doc.selection()[0]) != nullptr, "has_selection");
        set(m_selection_count_, static_cast<int>(doc.selection().size()), "selection_count");
        set(m_object_count_, "Объектов: " + group_digits(doc.object_count()), "object_count");
        char zoom[32];
        std::snprintf(zoom, sizeof(zoom), "%.0f пикс/клетку", camera_.zoom);
        set(m_zoom_, std::string(zoom), "zoom");

        // Inspector: on any change of the selected object, and while playing
        // a few times a second (the game moves things without commands).
        const bool playing_refresh = play.playing() && !paused_ && time_ - fields_time_ > 0.1;
        if (doc.version() != fields_doc_version_ || doc.selection_version() != fields_selection_version_ ||
            fields_dirty_ || playing_refresh) {
            const bool selection_changed = doc.selection_version() != fields_selection_version_;
            // Do not rewrite a field the user is typing in.
            if (selection_changed || !text_focus()) {
                fields_doc_version_ = doc.version();
                fields_selection_version_ = doc.selection_version();
                fields_dirty_ = false;
                fields_time_ = time_;
                rebuild_fields();
                model_.DirtyVariable("fields");
                model_.DirtyVariable("addable");
                const Object* o = m_has_selection_ ? doc.find(doc.selection()[0]) : nullptr;
                m_selected_name_ = o ? o->name : std::string();
                model_.DirtyVariable("selected_name");
            }
        }

        if (history.version() != history_version_) {
            history_version_ = history.version();
            m_history_.clear();
            for (usize i = 0; i < history.size(); ++i) m_history_.push_back(history.label_at(i));
            m_history_cursor_ = static_cast<int>(history.cursor());
            model_.DirtyVariable("history");
            model_.DirtyVariable("history_cursor");
        }

        const ui::UiStats& s = ui_.stats();
        char status[200];
        std::snprintf(status, sizeof(status), "Интерфейс %.2f + %.2f мс · спрайтов %s · выделено %zu",
                      s.update_ms, s.render_ms, group_digits(sprites_drawn_).c_str(), doc.selection().size());
        // Timings change every frame, so they are shown four times a second;
        // a new selection is shown at once.
        if (m_tab_ == "logic") {
            set(m_status_, logic_tab.status(), "status");
        } else if (m_tab_ == "story") {
            set(m_status_, story_tab.status(), "status");
        } else if (m_tab_ == "ui") {
            set(m_status_, ui_tab.status(), "status");
        } else if (m_tab_ == "objects") {
            set(m_status_, objects_tab.status(), "status");
        } else if (m_tab_ == "assets") {
            set(m_status_, assets.status(), "status");
        } else if (m_tab_ == "level") {
            set(m_status_, level.status(), "status");
        } else if (time_ - status_time_ > 0.25 || doc.selection_version() != status_selection_version_) {
            status_time_ = time_;
            status_selection_version_ = doc.selection_version();
            set(m_status_, std::string(status), "status");
        }
    }

    void drain_log() {
        std::vector<std::pair<int, std::string>> lines;
        {
            std::lock_guard lock(g_log.mutex);
            lines.swap(g_log.pending);
        }
        if (lines.empty()) return;
        for (auto& [kind, text] : lines) m_log_.push_back({std::move(text), kind});
        if (m_log_.size() > 300) m_log_.erase(m_log_.begin(), m_log_.end() - 300);
        model_.DirtyVariable("log");
        scroll_log_ = 2; // after the next layout
    }

    void follow_log() {
        if (scroll_log_ == 0) return;
        if (--scroll_log_ > 0) return;
        for (const char* id : {"log", "lv-log", "as-log"})
            if (Rml::Element* log = find_element(id)) log->SetScrollTop(log->GetScrollHeight());
    }

    // --- world ---

    void refresh_drawables() {
        if (doc.structure_version() == drawables_version_) return;
        drawables_version_ = doc.structure_version();
        const bool keep_homes = play.playing();
        std::unordered_map<ObjectId, Vec2> homes;
        if (keep_homes)
            for (const Drawable& d : drawables_) homes[d.id] = d.home;
        drawables_.clear();
        // Depth-first, so children draw over their group in tree order.
        std::vector<ObjectId> stack(doc.roots().rbegin(), doc.roots().rend());
        while (!stack.empty()) {
            const ObjectId id = stack.back();
            stack.pop_back();
            Object* o = doc.find(id);
            if (!o) continue;
            Transform* t = static_cast<Transform*>(doc.component(id, reflect::type_of<Transform>()));
            Look* look = static_cast<Look*>(doc.component(id, reflect::type_of<Look>()));
            if (t && look) {
                Mover* m = static_cast<Mover*>(doc.component(id, reflect::type_of<Mover>()));
                Vec2 home = t->position;
                if (keep_homes)
                    if (auto it = homes.find(id); it != homes.end()) home = it->second;
                drawables_.push_back({id, t, look, m, home});
            }
            for (auto it = o->children.rbegin(); it != o->children.rend(); ++it) stack.push_back(*it);
        }
    }

    void simulate(f32 dt) {
        for (Drawable& d : drawables_) {
            if (!d.mover) continue;
            Vec2& p = d.transform->position;
            Vec2& v = d.mover->velocity;
            p.x += v.x * dt;
            p.y += v.y * dt;
            d.transform->rotation = std::fmod(d.transform->rotation + d.mover->spin * dt, 360.0f);
            const f32 r = d.mover->bounce_radius;
            if ((p.x - d.home.x > r && v.x > 0) || (d.home.x - p.x > r && v.x < 0)) v.x = -v.x;
            if ((p.y - d.home.y > r && v.y > 0) || (d.home.y - p.y > r && v.y < 0)) v.y = -v.y;
        }
    }

    void viewport_rect() {
        vx_ = vy_ = vw_ = vh_ = 0;
        Rml::Element* view = find_element("viewport");
        if (!view || !view->IsVisible(true)) return; // another editor tab is open
        const Rml::Vector2f at = view->GetAbsoluteOffset(Rml::BoxArea::Padding);
        const Rml::Vector2f size = view->GetBox().GetSize(Rml::BoxArea::Padding);
        vx_ = std::max(0.0f, at.x);
        vy_ = std::max(0.0f, at.y);
        vw_ = std::max(0.0f, size.x);
        vh_ = std::max(0.0f, size.y);
    }

    bool over_viewport(f32 x, f32 y) const {
        if (x < vx_ || y < vy_ || x >= vx_ + vw_ || y >= vy_ + vh_) return false;
        // Labels drawn over the world still belong to the world.
        const Rml::Element* hover = context_->GetHoverElement();
        for (; hover; hover = hover->GetParentNode())
            if (hover->GetId() == "viewport") return true;
        return false;
    }

    void to_tile(f32 x, f32 y, f64& tx, f64& ty) const {
        camera_.screen_to_tile(x - vx_, y - vy_, static_cast<u32>(vw_), static_cast<u32>(vh_), tx, ty);
    }

    ObjectId pick(f32 x, f32 y) const {
        f64 tx, ty;
        to_tile(x, y, tx, ty);
        // Last drawn is on top.
        ObjectId best = kNoObject;
        u32 best_layer = 0;
        for (const Drawable& d : drawables_) {
            if (!d.look->visible) continue;
            const f64 hw = std::fabs(d.transform->scale.x) * 0.5, hh = std::fabs(d.transform->scale.y) * 0.5;
            if (std::fabs(tx - d.transform->position.x) <= hw && std::fabs(ty - d.transform->position.y) <= hh &&
                (best == kNoObject || d.look->layer >= best_layer)) {
                best = d.id;
                best_layer = d.look->layer;
            }
        }
        return best;
    }

    void begin_drag(f32 x, f32 y, bool add) {
        const ObjectId hit = pick(x, y);
        if (hit == kNoObject) {
            if (!add) select({});
            return;
        }
        std::vector<ObjectId> ids = add ? doc.selection() : std::vector<ObjectId>{};
        if (std::find(ids.begin(), ids.end(), hit) == ids.end()) ids.push_back(hit);
        select(ids);
        reveal(hit);
        history.seal();
        dragging_ = hit;
        to_tile(x, y, drag_tile_x_, drag_tile_y_);
        if (const Transform* t = doc.component<Transform>(hit)) drag_start_ = t->position;
    }

    void drag_to(f32 x, f32 y) {
        f64 tx, ty;
        to_tile(x, y, tx, ty);
        const auto* type = reflect::type_of<Transform>();
        Transform* t = doc.component<Transform>(dragging_);
        if (!t) return;
        const std::string before = doc.component_json(dragging_, type);
        Transform moved = *t;
        moved.position = {drag_start_.x + static_cast<f32>(tx - drag_tile_x_),
                          drag_start_.y + static_cast<f32>(ty - drag_tile_y_)};
        if (moved.position == t->position) return;
        const Object* o = doc.find(dragging_);
        history.execute(std::make_unique<SetComponent>(dragging_, type, before, data::to_json(moved, false), "position",
                                                       "Переместить «" + (o ? o->name : std::string()) + "»"));
    }

    void render_world(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, const f32 bg[4]) {
        const u32 vw = static_cast<u32>(vw_), vh = static_cast<u32>(vh_);
        sprites_drawn_ = 0;
        if (vw > 0 && vh > 0) {
            batch_.begin(camera_.snapped_x(), camera_.snapped_y(), static_cast<u32>(drawables_.size()) * 2 + 16);
            const f64 ox = batch_.origin_x(), oy = batch_.origin_y();
            const f32 view_h = vh_ / camera_.zoom;
            const f32 top = static_cast<f32>(camera_.y - oy) - view_h * 0.5f;
            u32 accent = render::pack_color(80, 220, 200, 200);
            f32 a[4];
            if (parse_hex(ui_.tokens().find("primary"), a))
                accent = pack({a[0], a[1], a[2], 0.85f});
            for (const Drawable& d : drawables_) {
                if (!d.look->visible) continue;
                render::Sprite s;
                s.x = static_cast<f32>(d.transform->position.x - ox);
                s.y = static_cast<f32>(d.transform->position.y - oy);
                s.w = d.transform->scale.x;
                s.h = d.transform->scale.y;
                s.angle = d.transform->rotation * kPi / 180.0f;
                s.frame = sprite_frame(*d.look, time_);
                s.color = pack(d.look->tint);
                s.order = render::draw_order(static_cast<u8>(d.look->layer * 2 + 1), s.y + s.h * 0.5f - top, view_h);
                if (doc.selected(d.id)) {
                    render::Sprite glow = s;
                    glow.frame = demo::kFrameGlow;
                    glow.w = std::fabs(s.w) * 2.2f;
                    glow.h = std::fabs(s.h) * 2.2f;
                    glow.angle = 0;
                    glow.color = accent;
                    glow.order = render::draw_order(static_cast<u8>(d.look->layer * 2), s.y + s.h * 0.5f - top, view_h);
                    batch_.push(glow);
                }
                batch_.push(s);
            }
            sprites_.prepare(cmd, batch_, camera_, vw, vh, true);
            sprites_drawn_ = sprites_.stats().drawn;
        }
        if (m_tab_ == "level") level.prepare(cmd);

        SDL_GPUColorTargetInfo color{};
        color.texture = target;
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        color.clear_color = {bg[0], bg[1], bg[2], 1};
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
        if (vw > 0 && vh > 0) {
            SDL_GPUViewport viewport{vx_, vy_, vw_, vh_, 0, 1};
            SDL_SetGPUViewport(pass, &viewport);
            const SDL_Rect scissor{static_cast<int>(vx_), static_cast<int>(vy_), static_cast<int>(vw),
                                   static_cast<int>(vh)};
            SDL_SetGPUScissor(pass, &scissor);
            sprites_.draw(cmd, pass);
        }
        if (m_tab_ == "level") level.draw(cmd, pass);
        SDL_EndGPURenderPass(pass);
    }

    bool handle_key(const SDL_KeyboardEvent& k) {
        const bool ctrl = (k.mod & SDL_KMOD_CTRL) != 0;
        const bool shift = (k.mod & SDL_KMOD_SHIFT) != 0;
        if (ctrl && k.key == SDLK_Z) { shift ? redo() : undo(); return true; }
        if (ctrl && k.key == SDLK_Y) { redo(); return true; }
        if (ctrl && k.key == SDLK_S) { save(); return true; }
        if (m_tab_ == "assets") return assets.handle_key(k);
        if (m_tab_ == "objects") return objects_tab.handle_key(k);
        if (m_tab_ == "logic") return logic_tab.handle_key(k);
        if (m_tab_ == "story") return story_tab.handle_key(k);
        if (m_tab_ == "ui") return ui_tab.handle_key(k);
        if (m_tab_ == "level") return level.handle_key(k);
        if (k.key == SDLK_F5) { toggle_play(); return true; }
        if (m_tab_ != "world") return false; // the keys below act on the world view
        if (ctrl && k.key == SDLK_D) { duplicate_selection(); return true; }
        if (k.key == SDLK_DELETE) { delete_selection(); return true; }
        if (k.key == SDLK_F && !ctrl) { focus_selection(); return true; }
        if (k.key == SDLK_ESCAPE) { select({}); return true; }
        return false;
    }

    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* window_ = nullptr;
    ui::Ui ui_;
    Rml::Context* context_ = nullptr;
    Rml::DataModelHandle model_;
    demo::SheetImage art_;
    render::SpriteRenderer sprites_;
    render::SpriteBatch batch_;
    render::Camera2D camera_;
    std::vector<Drawable> drawables_;
    u64 drawables_version_ = 0;
    u32 sprites_drawn_ = 0;
    f32 vx_ = 0, vy_ = 0, vw_ = 0, vh_ = 0;
    f64 time_ = 0;
    bool paused_ = false;
    bool in_ui_update_ = false;
    bool panning_ = false;
    f32 last_mouse_x_ = 0, last_mouse_y_ = 0;
    ObjectId dragging_ = kNoObject;
    f64 drag_tile_x_ = 0, drag_tile_y_ = 0;
    Vec2 drag_start_{};
    ObjectId list_anchor_ = kNoObject;
    std::string theme_, pending_theme_;

    // Model mirrors.
    bool m_playing_ = false, m_can_undo_ = false, m_can_redo_ = false, m_dirty_ = false, m_has_selection_ = false;
    int m_selection_count_ = 0, m_history_cursor_ = 0, m_bottom_tab_ = 0;
    Rml::String m_tab_ = "level", m_tab_title_, m_tab_description_, m_tab_step_, m_tab_icon_;
    Rml::String m_undo_label_, m_scene_name_, m_selected_name_, m_object_count_, m_status_, m_zoom_;
    std::vector<FieldView> m_fields_;
    std::vector<FieldRef> field_refs_;
    std::vector<Rml::String> m_addable_;
    std::vector<const reflect::TypeInfo*> addable_types_;
    std::vector<LogLine> m_log_;
    std::vector<Rml::String> m_history_;
    u64 fields_doc_version_ = 0, fields_selection_version_ = 0, history_version_ = 0;
    bool fields_dirty_ = true;
    f64 fields_time_ = 0, status_time_ = -1;
    u64 status_selection_version_ = 0;
    int scroll_log_ = 0;
};

// --- hierarchy --------------------------------------------------------------

u32 HierarchySource::count() const { return static_cast<u32>(rows_.size()); }

u64 HierarchySource::version() const {
    return editor_.doc.version() + editor_.doc.selection_version() * 1'000'003ull + expand_version_ * 7'919ull;
}

void HierarchySource::add(ObjectId id, u32 depth) {
    rows_.push_back({id, depth});
    if (!expanded_.count(id)) return;
    for (ObjectId child : editor_.doc.children_of(id)) add(child, depth + 1);
}

void HierarchySource::refresh() {
    if (built_structure_ == editor_.doc.structure_version() && built_expand_ == expand_version_) return;
    built_structure_ = editor_.doc.structure_version();
    built_expand_ = expand_version_;
    rows_.clear();
    for (ObjectId id : editor_.doc.roots()) add(id, 0);
}

void HierarchySource::set_expanded(ObjectId id, bool expanded) {
    if (expanded ? expanded_.insert(id).second : expanded_.erase(id) > 0) ++expand_version_;
}

void HierarchySource::expand_all(bool expanded) {
    expanded_.clear();
    if (expanded)
        for (ObjectId id : editor_.doc.roots())
            if (!editor_.doc.children_of(id).empty()) expanded_.insert(id);
    ++expand_version_;
    refresh();
}

i64 HierarchySource::row_of(ObjectId id) const {
    for (usize i = 0; i < rows_.size(); ++i)
        if (rows_[i].id == id) return static_cast<i64>(i);
    return -1;
}

std::string HierarchySource::field(u32 row, std::string_view name) const {
    if (row >= rows_.size()) return {};
    const Row& r = rows_[row];
    const Object* o = editor_.doc.find(r.id);
    if (!o) return {};
    if (name == "name") return o->name;
    if (name == "indent") return std::to_string(4 + r.depth * 18) + "px";
    if (name == "selected") return editor_.doc.selected(r.id) ? "1" : "0";
    if (name == "arrow") return o->children.empty() ? "" : expanded_.count(r.id) ? "expand_more" : "chevron_right";
    const Look* look = static_cast<const Look*>(
        const_cast<Document&>(editor_.doc).component(r.id, reflect::type_of<Look>()));
    if (name == "icon") {
        if (!o->children.empty()) return "folder";
        if (!look) return "radio_button_unchecked";
        switch (look->picture) {
        case Picture::Critter: return "pets";
        case Picture::Crate:
        case Picture::Chest: return "inventory_2";
        case Picture::Ball: return "sports_baseball";
        case Picture::Leaf: return "eco";
        case Picture::Glow: return "lightbulb";
        default: return "factory";
        }
    }
    if (name == "kind") {
        if (!o->children.empty()) return std::to_string(o->children.size());
        if (look)
            if (const reflect::EnumValue* v = reflect::type_of<Picture>()->find_enum(static_cast<i64>(look->picture)))
                return v->name;
        return {};
    }
    return {};
}

void HierarchySource::on_row_event(u32 row, std::string_view event, int modifiers) {
    if (row >= rows_.size()) return;
    if (event == "click") {
        editor_.list_click(row, modifiers);
        anchor_ = row;
    } else if (event == "dblclick") {
        const ObjectId id = rows_[row].id;
        if (!editor_.doc.children_of(id).empty()) set_expanded(id, !expanded_.count(id));
        else editor_.focus_selection();
    }
}

// --- app ----------------------------------------------------------------------

struct Options {
    std::filesystem::path ui_dir = utf8_path(FORGE_UI_DIR);
    std::filesystem::path scene;
    std::filesystem::path level = utf8_path(SLICE_LEVEL_DIR);
    bool level_given = false;
    std::filesystem::path project; // empty: the working folder
    std::filesystem::path assets; // empty: assets/ in the working folder
    std::string theme = "dark";
    u32 objects = 50'000;
    std::string talk; // offscreen: the «Сюжет» tab with this talk open ("story/name"; "cast:story", "novel:story")
    // offscreen: the «Интерфейс» tab's template library open on a section ("construction", "screen") or a template
    // (its file name); "new:FILE" a new screen from it, "insert:FILE" it on the open screen (the window closed).
    std::string templates;
};

class EditorApp final : public App {
public:
    Options options;

    bool on_init() override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window(), &w, &h);
        if (!options.scene.empty()) editor_.scene_path = options.scene;
        // The author's work lives in the project folder, apart from the engine.
        const std::filesystem::path project = options.project.empty() ? std::filesystem::current_path() : options.project;
        ProjectGame pg;
        if (std::string error; !prepare_project_game(project, utf8_path(SLICE_DATA_DIR), pg, &error)) {
            FORGE_ERROR("Проект: %s", error.c_str());
            return false;
        }
        if (pg.created) FORGE_INFO("Проект: игра скопирована в %s", path_to_utf8(pg.dir).c_str());
        for (const std::string& f : pg.refreshed) FORGE_INFO("Проект: %s обновлён из движка", f.c_str());
        editor_.use_game_dir(pg.dir);
        LevelConfig lc;
        lc.folder = options.level_given ? options.level : pg.dir / "level";
        lc.game_data = pg.dir;
        lc.game_exe = utf8_path(FORGE_SLICE_EXE);
        if (char* pref = SDL_GetPrefPath("Forge", "Editor")) {
            lc.settings = utf8_path(pref);
            SDL_free(pref);
        }
        AssetsConfig ac;
        ac.folder = options.assets.empty() ? project / "assets" : options.assets;
        ac.library = ac.folder.parent_path() / ".forge" / "library";
        ac.settings = lc.settings;
        ac.window = window();
        if (!editor_.init(gpu(), window(), swapchain_format(), static_cast<u32>(w), static_cast<u32>(h),
                          options.ui_dir, options.theme, options.objects, lc, ac)) {
            FORGE_ERROR("editor: start failed (UI folder: %s)", path_to_utf8(options.ui_dir).c_str());
            return false;
        }
        return true;
    }
    void on_event(const SDL_Event& e) override { editor_.handle_event(e); }
    void on_frame(f64 dt) override {
        editor_.update(dt);
        set_status(editor_.play.playing() ? "игра идёт" : "");
    }
    void on_render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 w, u32 h) override {
        editor_.render(cmd, target, swapchain_format(), w, h);
    }
    void on_shutdown() override {
        const FrameStats& s = frame_stats();
        FORGE_INFO("frame avg %.3f ms, worst %.3f ms (last %u frames)", s.avg_ms, s.worst_ms, FrameStats::kWindow);
        if (editor_.history.dirty()) FORGE_WARN("editor: unsaved changes were not saved");
        editor_.shutdown();
    }

private:
    Editor editor_;
};

// Offscreen run for screenshots and the benchmark.
// Plays the editor's own controls through synthetic input: drag an object in
// the world, undo and redo it, run the game and stop it, delete and duplicate,
// switch editor tabs.
// Fails when any action does not do what the user would expect.
// A small project for offscreen runs: pictures, a sound, data, folders. With
// bench > 0 also that many files in 50 folders (every tenth a picture);
// kept between runs while the count is the same.
void make_sample_assets(const std::filesystem::path& root, u32 bench) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path marker = root / "bench.txt";
    std::vector<u8> was;
    const std::string want = std::to_string(bench);
    if (bench > 0 && read_file(marker, was) && std::string(was.begin(), was.end()) == want) {
        fs::remove_all(root / "assets" / "персонажи", ec); // the self-test part is always fresh
    } else {
        fs::remove_all(root, ec);
    }
    const fs::path a = root / "assets";
    auto picture = [&](const fs::path& file, u32 w, u32 h, u8 r, u8 g, u8 b) {
        assets::CookedTexture t;
        t.width = w;
        t.height = h;
        t.rgba8.resize(static_cast<usize>(w) * h * 4);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                u8* p = t.rgba8.data() + (static_cast<usize>(y) * w + x) * 4;
                const bool edge = x == 0 || y == 0 || x == w - 1 || y == h - 1;
                p[0] = edge ? 20 : r;
                p[1] = edge ? 20 : static_cast<u8>(g + (y * 40) / h);
                p[2] = edge ? 20 : b;
                p[3] = 255;
            }
        std::vector<u8> bytes;
        const std::string ext = path_to_utf8(file.extension());
        assets::encode_image(t, ext, bytes);
        fs::create_directories(file.parent_path(), ec);
        write_file_atomic(file, bytes);
    };
    auto text = [&](const fs::path& file, const std::string& body) {
        fs::create_directories(file.parent_path(), ec);
        write_file_atomic(file, {reinterpret_cast<const u8*>(body.data()), body.size()});
    };
    picture(a / "персонажи" / "кузнец.png", 16, 16, 180, 90, 40);
    picture(a / "персонажи" / "шахтёр.png", 16, 24, 200, 160, 60);
    if (fs::exists(a / "тайлы", ec)) return; // the rest is kept with the bench files
    picture(a / "тайлы" / "камень.png", 32, 32, 120, 120, 130);
    picture(a / "тайлы" / "песок.bmp", 16, 16, 220, 200, 120);
    text(a / "данные" / "предметы.json", R"({"кирка": {"цена": 10}, "факел": {"цена": 2}})");
    text(a / "readme.txt", "Ресурсы пробного проекта.");
    {
        // A quarter second of a 440 Hz tone, 16-bit mono.
        const u32 rate = 22050, n = rate / 4;
        std::vector<u8> wav(44 + n * 2);
        auto put32 = [&](usize at, u32 v) { for (int i = 0; i < 4; ++i) wav[at + i] = static_cast<u8>(v >> (8 * i)); };
        auto put16 = [&](usize at, u32 v) { for (int i = 0; i < 2; ++i) wav[at + i] = static_cast<u8>(v >> (8 * i)); };
        std::memcpy(wav.data(), "RIFF", 4);
        put32(4, 36 + n * 2);
        std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
        put32(16, 16);
        put16(20, 1);
        put16(22, 1);
        put32(24, rate);
        put32(28, rate * 2);
        put16(32, 2);
        put16(34, 16);
        std::memcpy(wav.data() + 36, "data", 4);
        put32(40, n * 2);
        for (u32 i = 0; i < n; ++i)
            put16(44 + i * 2, static_cast<u16>(static_cast<i16>(8000 * std::sin(i * 2 * 3.14159265 * 440 / rate))));
        fs::create_directories(a / "звуки", ec);
        write_file_atomic(a / "звуки" / "кирка.wav", wav);
    }
    for (u32 i = 0; i < bench; ++i) {
        char folder[16], name[48];
        std::snprintf(folder, sizeof(folder), "%02u", i % 50);
        const fs::path dir = a / "bench" / folder;
        if (i % 10 == 0) {
            std::snprintf(name, sizeof(name), "предмет_%u.png", i);
            picture(dir / utf8_path(name), 8, 8, static_cast<u8>(i * 7), static_cast<u8>(i * 13), static_cast<u8>(i * 29));
        } else {
            std::snprintf(name, sizeof(name), "предмет_%u.json", i);
            text(dir / utf8_path(name), "{\"id\": " + std::to_string(i) + "}");
        }
    }
    if (bench > 0) text(marker, want);
}

class SelfTest {
public:
    explicit SelfTest(Editor& editor) : ed_(editor) {}

    // Called before each frame's update; false when the test is over.
    bool step(u32 frame) {
        switch (frame) {
        case 3: {
            const ObjectId group = ed_.doc.roots().at(0);
            target_ = ed_.doc.children_of(group).at(0);
            start_ = ed_.doc.component<Transform>(target_)->position;
            check(ed_.screen_of(target_, x_, y_), "object is in the view");
            mouse(SDL_EVENT_MOUSE_MOTION, x_, y_);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x_, y_);
            check(ed_.doc.selection() == std::vector<ObjectId>{target_}, "click selects the object");
            entries_ = ed_.history.size();
            break;
        }
        case 4: case 5: case 6: case 7: case 8:
            x_ += 12;
            mouse(SDL_EVENT_MOUSE_MOTION, x_, y_);
            break;
        case 9: {
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, x_, y_);
            moved_ = ed_.doc.component<Transform>(target_)->position;
            const f32 expected = 60.0f / ed_.zoom();
            check(std::fabs(moved_.x - start_.x - expected) < 0.01f && moved_.y == start_.y, "drag moves the object");
            check(ed_.history.size() == entries_ + 1, "a whole drag is one history entry");
            break;
        }
        case 10:
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(ed_.doc.component<Transform>(target_)->position == start_, "Ctrl+Z puts it back");
            break;
        case 11:
            key(SDLK_Y, SDL_KMOD_CTRL);
            check(ed_.doc.component<Transform>(target_)->position == moved_, "Ctrl+Y moves it again");
            entries_ = ed_.history.size();
            break;
        case 12:
            key(SDLK_F5, SDL_KMOD_NONE);
            check(ed_.play.playing(), "F5 starts the game");
            break;
        case 40:
            check(!(ed_.doc.component<Transform>(target_)->position == moved_), "objects move while playing");
            key(SDLK_F5, SDL_KMOD_NONE);
            check(!ed_.play.playing(), "F5 stops the game");
            check(ed_.doc.component<Transform>(target_)->position == moved_, "Stop restores the scene");
            check(ed_.doc.selection() == std::vector<ObjectId>{target_}, "Stop keeps the selection");
            check(ed_.history.size() == entries_, "playing adds nothing to the history");
            break;
        case 41:
            count_ = ed_.doc.object_count();
            key(SDLK_DELETE, SDL_KMOD_NONE);
            check(!ed_.doc.find(target_) && ed_.doc.object_count() == count_ - 1, "Delete removes the object");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(ed_.doc.find(target_) && ed_.doc.component<Transform>(target_)->position == moved_,
                  "Ctrl+Z brings it back");
            break;
        case 42:
            ed_.select({target_});
            key(SDLK_D, SDL_KMOD_CTRL);
            check(ed_.doc.object_count() == count_ + 1, "Ctrl+D makes a copy");
            check(ed_.history.undo_label() == "Копировать", "the copy is one history entry");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(ed_.doc.object_count() == count_, "Ctrl+Z removes the copy");
            break;
        case 43:
            check(click_tab(6), "a click on the Logic tab");
            break;
        case 44:
            check(tab_lit(6) && !tab_lit(1), "the Logic tab is highlighted on the next frame");
            break;
        case 45: {
            check(shown("logic") && !shown("viewport"), "the tab replaces the world view");
            count_ = ed_.doc.object_count();
            key(SDLK_DELETE, SDL_KMOD_NONE);
            check(ed_.doc.object_count() == count_, "Delete does nothing outside the world tab");
            check(click_tab(1), "a click on the Scene tab");
            break;
        }
        case 46:
            check(tab_lit(1) && !tab_lit(6), "the Scene tab is highlighted again");
            break;
        case 47:
            check(shown("viewport") && !shown("placeholder"), "the world view is back");
            break;
        case 48:
            check(click_tab(0), "a click on the Level tab");
            break;
        default:
            if (frame >= 50) return level_step(frame - 50);
            break;
        }
        return true;
    }
    bool passed() const { return failures_ == 0; }

private:
    // --- the level tab ---
    LevelEditor& lv() { return ed_.level; }
    world::TileId at(i32 x, i32 y, u32 layer = 1) { return lv().level().tile(layer, x, y); }
    // Moves the mouse to the middle of a cell in the level view.
    void to_cell(i32 x, i32 y) {
        lv().screen_of(x + 0.5, y + 0.5, x_, y_);
        mouse(SDL_EVENT_MOUSE_MOTION, x_, y_);
    }
    void click_cell(i32 x, i32 y) {
        to_cell(x, y);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x_, y_);
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, x_, y_);
    }
    usize tile_named(const char* id) {
        const auto& tiles = ed_.level_module.tiles();
        for (usize i = 0; i < tiles.size(); ++i)
            if (tiles[i].id == id) return i;
        return 0;
    }
    bool element_center(const char* id, f32& x, f32& y) {
        Rml::Element* e = ed_.find_element(id);
        if (!e || !e->IsVisible(true)) return false;
        const Rml::Vector2f p = e->GetAbsoluteOffset(Rml::BoxArea::Border) + e->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
        x = p.x;
        y = p.y;
        return true;
    }
    usize stack_of(const char* panel) {
        const auto& stacks = lv().dock().stacks();
        for (usize i = 0; i < stacks.size(); ++i)
            for (const std::string& p : stacks[i].panels)
                if (p == panel) return i;
        return ~usize(0);
    }

    bool level_step(u32 f) {
        if (f >= 19) return objects_step();
        const usize stone = tile_named("stone"), sand = tile_named("sand");
        switch (f) {
        case 0: {
            check(shown("level-view") && shown("pane-palette") && !shown("viewport"), "the level tab shows the world and panels");
            check(lv().view_w() > 200 && lv().view_h() > 200, "the level view has room");
            // A spot in the sky over the village.
            cx_ = static_cast<i32>(std::floor(lv().camera().x)) - 6;
            cy_ = static_cast<i32>(std::floor(lv().camera().y)) - 4;
            check(at(cx_, cy_) == 0 && lv().level().loaded(cx_, cy_), "the sky over the village is loaded and empty");
            check(element_center("pal-" + std::to_string(stone), x_, y_), "the palette shows stone");
            if (Rml::Element* e = ed_.find_element(("pal-" + std::to_string(stone)).c_str())) e->Click();
            break;
        }
        case 1: {
            check(lv().tile_index() == stone && lv().layer() == 1, "a click on the palette picks stone on the block layer");
            lv().set_brush_radius(0);
            entries_ = lv().history().size();
            to_cell(cx_, cy_);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x_, y_);
            for (i32 i = 1; i <= 3; ++i) to_cell(cx_ + i, cy_);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, x_, y_);
            bool row = true;
            for (i32 i = 0; i <= 3; ++i) row = row && at(cx_ + i, cy_) == ed_.level_module.tiles()[stone].value;
            check(row && at(cx_ + 4, cy_) == 0 && at(cx_, cy_ + 1) == 0, "the brush paints the cells it went over, no more");
            check(lv().history().size() == entries_ + 1 && lv().history().undo_label() == "Кисть: Камень",
                  "a brush stroke is one history entry");
            check(lv().dirty(), "the level is marked unsaved");
            break;
        }
        case 2:
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(at(cx_, cy_) == 0 && at(cx_ + 3, cy_) == 0, "Ctrl+Z takes the stroke back");
            key(SDLK_Y, SDL_KMOD_CTRL);
            check(at(cx_ + 3, cy_) != 0, "Ctrl+Y paints it again");
            break;
        case 3: {
            key(SDLK_R, SDL_KMOD_NONE);
            check(lv().tool() == Tool::Rect, "R picks the rectangle");
            to_cell(cx_, cy_ + 2);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x_, y_);
            to_cell(cx_ + 4, cy_ + 3);
            to_cell(cx_ + 4, cy_ + 4);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, x_, y_);
            u32 n = 0;
            for (i32 y = cy_ + 1; y <= cy_ + 5; ++y)
                for (i32 x = cx_ - 1; x <= cx_ + 5; ++x) n += at(x, y) != 0;
            check(n == 15, "the rectangle fills 5 x 3 cells");
            break;
        }
        case 4: {
            key(SDLK_4, SDL_KMOD_NONE);
            check(lv().tile_index() == sand, "key 4 picks sand");
            key(SDLK_G, SDL_KMOD_NONE);
            click_cell(cx_ + 2, cy_ + 3);
            u32 n = 0;
            for (i32 y = cy_ + 2; y <= cy_ + 4; ++y)
                for (i32 x = cx_; x <= cx_ + 4; ++x) n += at(x, y) == ed_.level_module.tiles()[sand].value;
            check(n == 15 && at(cx_, cy_) == ed_.level_module.tiles()[stone].value,
                  "the fill turns the whole rectangle to sand and nothing else");
            break;
        }
        case 5:
            key(SDLK_I, SDL_KMOD_NONE);
            click_cell(cx_ + 1, cy_);
            check(lv().tile_index() == stone && lv().tool() == Tool::Brush, "the picker takes stone and goes back to the brush");
            key(SDLK_E, SDL_KMOD_NONE);
            click_cell(cx_ + 1, cy_);
            check(at(cx_ + 1, cy_) == 0 && at(cx_, cy_) != 0, "the eraser clears one cell");
            break;
        case 6:
            // Panels: the minimap's header dragged onto the palette's.
            stacks_ = lv().dock().stacks().size();
            check(element_center("dock-tab-minimap", x_, y_), "the minimap has a header");
            mouse(SDL_EVENT_MOUSE_MOTION, x_, y_);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x_, y_);
            break;
        case 7: {
            f32 px = 0, py = 0;
            check(element_center("dock-tab-palette", px, py), "the palette has a header");
            mouse(SDL_EVENT_MOUSE_MOTION, (x_ + px) * 0.5f, (y_ + py) * 0.5f);
            mouse(SDL_EVENT_MOUSE_MOTION, px + 30, py);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, px + 30, py);
            break;
        }
        case 8:
            check(stack_of("minimap") == stack_of("palette") && lv().dock().stacks().size() == stacks_ - 1,
                  "the minimap is a tab next to the palette");
            check(shown("pane-minimap") && !shown("pane-palette"), "the dropped panel is the open tab");
            break;
        case 9:
            check(element_center("dock-tab-palette", x_, y_), "the palette tab is there");
            mouse(SDL_EVENT_MOUSE_MOTION, x_, y_);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x_, y_);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, x_, y_);
            break;
        case 10:
            check(shown("pane-palette") && !shown("pane-minimap"), "a click on a tab opens it");
            lv().reset_layout();
            break;
        case 11:
            check(lv().dock().stacks().size() == stacks_ && shown("pane-minimap") && shown("pane-palette"),
                  "the layout resets");
            key(SDLK_S, SDL_KMOD_CTRL);
            check(!lv().dirty(), "Ctrl+S saves the level");
            {
                level::Level copy(ed_.level_module);
                check(copy.open(lv().level().folder()), "the saved level opens again");
                copy.ensure_loaded({cx_ - 2, cy_ - 2, cx_ + 8, cy_ + 8});
                check(copy.tile(1, cx_, cy_) == ed_.level_module.tiles()[stone].value &&
                          copy.tile(1, cx_ + 2, cy_ + 3) == ed_.level_module.tiles()[sand].value &&
                          copy.tile(1, cx_ + 1, cy_) == 0,
                      "the saved level has the painted tiles");
                // The villagers come back with their bodies (the game moves only those).
                const i32 vy = ed_.level_module.slice_generator().village_y();
                copy.ensure_loaded({-40, vy - 8, 24, vy + 4});
                int people = 0, with_bodies = 0;
                copy.scene().ecs().each([&](flecs::entity e, const slice::Npc&) {
                    ++people;
                    with_bodies += e.has<sim::Body>();
                });
                check(people == 2 && with_bodies == 2, "the saved villagers keep their bodies");
            }
            break;
        case 12: {
            check(lv().play_here(), "«Играть отсюда» finds a place for the hero");
            const auto cmd = lv().play_command(1.5, 2);
            check(cmd.size() >= 6 && cmd[1] == "--play" && cmd[2] == "--level" && cmd[4] == "--at" && cmd[5] == "1.50,2.00",
                  "the game is started with the level and the place");
            check(cmd.size() >= 12 && cmd[cmd.size() - 2] == "--data" && utf8_path(cmd.back()) == ed_.game_dir,
                  "and with the project's game data");
            check(lv().minimap_updates() > 0, "the minimap is drawn");
            break;
        }
        // --- objects ---
        case 13: {
            key(SDLK_O, SDL_KMOD_NONE);
            check(lv().mode() == Mode::Objects, "O opens the objects mode");
            break;
        }
        case 14: {
            const i32 coins = coins_index();
            check(coins >= 0 && shown("obj-" + std::to_string(coins)), "the palette shows the templates");
            if (Rml::Element* e = ed_.find_element(("obj-" + std::to_string(coins)).c_str())) e->Click();
            check(lv().armed_object() == coins, "a click on the palette takes coins");
            entries_ = lv().history().size();
            click_cell(cx_ + 10, cy_);
            check(lv().history().size() == entries_ + 1 && lv().history().undo_label() == "Поставить: Монеты",
                  "a click in the world places the coins, one history entry");
            check(lv().selection().size() == 1, "the placed object is selected");
            object_ = lv().selection().empty() ? 0 : lv().selection()[0];
            const flecs::entity e = placed();
            check(e.is_valid() && e.has<slice::Item>() && e.get<slice::Item>().kind == u8(slice::ItemKind::Coins) &&
                      e.get<slice::Item>().count == 10 && e.has<objects::ObjectRef>(),
                  "the coins are in the world, a copy of their template");
            break;
        }
        case 15:
            check(shown("obj-delete") && shown("lv-num-3"), "the properties show the coins and their fields");
            check(!shown("lv-reset-3"), "nothing of their own yet");
            lv().set_field(3, "7", false);
            check(count() == 7 && lv().history().undo_label() == "«Монеты»: Сколько", "«Сколько» is set to 7");
            check(own("count"), "seven is this copy's own value");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(count() == 10 && !own("count"), "Ctrl+Z gives the old count back, the template's again");
            key(SDLK_Y, SDL_KMOD_CTRL);
            check(count() == 7 && own("count"), "Ctrl+Y sets it again");
            break;
        case 16: {
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(lv().armed_object() < 0, "Esc puts the coins back on the shelf");
            entries_ = lv().history().size();
            // The coins lie on the cell's floor: grab them just above it.
            to_point(cx_ + 10.5, cy_ + 0.85);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x_, y_);
            to_point(cx_ + 11.6, cy_ + 0.85);
            to_point(cx_ + 13.5, cy_ + 0.85);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, x_, y_);
            const flecs::entity e = placed();
            check(e.is_valid() && std::abs(e.get<scene::Position>().tile_x() - (cx_ + 13.5)) < 1e-6,
                  "a drag moves the coins three cells");
            check(lv().history().size() == entries_ + 1 && lv().history().undo_label() == "Передвинуть: Монеты",
                  "the move is one history entry");
            break;
        }
        case 17: {
            key(SDLK_DELETE, SDL_KMOD_NONE);
            check(!placed().is_valid() && lv().selection().empty(), "Delete removes the coins");
            key(SDLK_Z, SDL_KMOD_CTRL);
            const flecs::entity e = placed();
            check(e.is_valid() && count() == 7 && std::abs(e.get<scene::Position>().tile_x() - (cx_ + 13.5)) < 1e-6,
                  "Ctrl+Z brings them back as they were");
            break;
        }
        case 18: {
            key(SDLK_Q, SDL_KMOD_NONE);
            to_point(cx_ + 13.5, cy_ + 0.85);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x_, y_);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, x_, y_);
            check(lv().mode() == Mode::Select && lv().selection().size() == 1 && lv().selection()[0] == object_,
                  "in the select mode a click picks the object");
            click_cell(cx_ + 6, cy_ - 3);
            check(lv().selection().empty(), "a click on empty sky clears the selection");
            key(SDLK_S, SDL_KMOD_CTRL);
            level::Level copy(ed_.level_module);
            check(copy.open(lv().level().folder()), "the level opens again");
            copy.ensure_loaded({cx_ + 8, cy_ - 4, cx_ + 18, cy_ + 4});
            const flecs::entity e = copy.find(object_);
            check(e.is_valid() && e.get<slice::Item>().count == 7 &&
                      std::abs(e.get<scene::Position>().tile_x() - (cx_ + 13.5)) < 1e-6,
                  "the saved level keeps the coins where they were put");
            lv().select_objects({object_}); // for a screenshot of the panels
            break;
        }
        default: break;
        }
        return true;
    }
    // The palette's coins (the game's "coins" template).
    i32 coins_index() const {
        const auto& defs = ed_.level_module.objects();
        for (usize i = 0; i < defs.size(); ++i)
            if (defs[i].id == "coins") return static_cast<i32>(i);
        return -1;
    }

    // --- the resources tab ---
    AssetLibrary& as() { return ed_.assets; }
    // True while the step must wait for ready (the background threads); a
    // step that waits too long fails and the test goes on.
    bool hold(bool ready, const char* what) {
        if (ready) {
            waited_ = 0;
            return false;
        }
        SDL_Delay(2);
        if (++waited_ < 1500) return true;
        waited_ = 0;
        check(false, what);
        return false;
    }
    i64 row_of(const std::string& rel) {
        for (usize i = 0; i < as().row_count(); ++i)
            if (as().row_rel(i) == rel) return static_cast<i64>(i);
        return -1;
    }
    void click_row(const std::string& rel, const char* event = "click") {
        const i64 r = row_of(rel);
        if (r >= 0) as().on_row_event(static_cast<u32>(r), event, 0);
    }
    bool exists(const std::string& rel) { return std::filesystem::exists(as().abs(rel)); }
    // The cells of the Resources list now on screen.
    std::vector<Rml::Element*> list_cells() {
        std::vector<Rml::Element*> out;
        Rml::Element* list = ed_.find_element("as-list");
        Rml::Element* content = list && list->GetNumChildren() > 0 ? list->GetChild(0) : nullptr;
        for (int i = 0; content && i < content->GetNumChildren(); ++i)
            if (Rml::Element* c = content->GetChild(i); c->IsVisible(true)) out.push_back(c);
        return out;
    }
    void right_click(f32 x, f32 y) {
        mouse(SDL_EVENT_MOUSE_MOTION, x, y);
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.x = x;
        e.button.y = y;
        e.button.button = SDL_BUTTON_RIGHT;
        e.button.down = true;
        ed_.handle_event(e);
        e.type = SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.down = false;
        ed_.handle_event(e);
    }
    void drop(const std::filesystem::path& file) {
        SDL_Event e{};
        e.type = SDL_EVENT_DROP_BEGIN;
        ed_.handle_event(e);
        drop_text_ = path_to_utf8(file);
        e.type = SDL_EVENT_DROP_FILE;
        e.drop.data = drop_text_.c_str();
        ed_.handle_event(e);
        e.type = SDL_EVENT_DROP_COMPLETE;
        e.drop.data = nullptr;
        ed_.handle_event(e);
    }

    // --- the objects tab ---
    ObjectLibrary& ol() { return ed_.objects_tab; }
    i64 card_named(const char* name) {
        for (usize i = 0; i < ol().cards(); ++i)
            if (ol().card_name(i) == name) return static_cast<i64>(i);
        return -1;
    }
    bool click(const std::string& id) {
        Rml::Element* e = ed_.find_element(id.c_str());
        if (e) e->Click();
        return e != nullptr;
    }
    // The template's value of a property, as JSON ("" without one).
    std::string tpl_value(const char* id, const char* prop) {
        objects::Library& lib = ol().library();
        const objects::Template* t = lib.find(id);
        const objects::PropDef* p = t ? lib.prop_of(*t, prop) : nullptr;
        return p ? lib.value(*t, *p) : std::string();
    }
    // Card i's centre in window pixels (false when it is not shown).
    bool card_at(i64 i, f32& x, f32& y) {
        return i >= 0 && element_center(("ol-card-" + std::to_string(i)).c_str(), x, y);
    }
    void left_click(f32 x, f32 y) {
        mouse(SDL_EVENT_MOUSE_MOTION, x, y);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
    }
    bool objects_step() {
        if (ol_step_ >= 37) return lg_step_ >= 0 ? logic_step() : st_step_ >= 0 ? story_step() : ue_step_ >= 0 ? ui_step() : asset_step();
        objects::Library& lib = ol().library();
        f32 x = 0, y = 0;
        switch (ol_step_) {
        case 0:
            check(click_tab(2) && ed_.tab() == "objects", "a click on the Objects tab");
            break;
        case 1:
            check(shown("ol-card-0") && ol().cards() == 11, "the tab shows the game's 11 templates as cards");
            check(!ol().history().can_undo() && tpl_value("crate", "density") == "0.6",
                  "showing the templates changes none of them");
            check(shown("ol-place-all") && shown("ol-genre-0") && shown("ol-kind-k:pickup") && shown("ol-kind-k:person"),
                  "the genres and the kinds are listed");
            check(!shown("ol-num-1") && !shown("ol-next-0"), "the library itself has no properties to set");
            check(click("ol-genre-1") && ol().place() == "g:RPG", "a click on «RPG»");
            break;
        case 2:
            check(ol().cards() == 3 && card_named("Шахтёр Борис") >= 0 && card_named("Кузнец") >= 0 && card_named("Ключ") >= 0,
                  "only the RPG objects are shown");
            check(click("ol-kind-k:pickup"), "a click on «Подбираемое»");
            break;
        case 3: {
            check(ol().cards() == 6, "only the pickups are shown");
            const i64 coins = card_named("Монеты");
            check(card_at(coins, x, y), "the «Монеты» card is on screen");
            right_click(x, y);
            break;
        }
        case 4:
            if (hold(shown("ctx-ol-open"), "the menu is laid out")) return true;
            check(ol().selected() && ol().selected()->id == "coins", "the right button selects «Монеты»");
            check(shown("ctx-ol-open") && shown("ctx-ol-place") && shown("ctx-ol-copy") && shown("ctx-ol-rename") &&
                      shown("ctx-ol-delete") && shown("ctx-ol-genre-0"),
                  "and opens its menu: editor, place, copy, rename, delete, genre");
            check(click("ctx-ol-open") && ol().editing() && !ol().menu_open(), "«Открыть редактор» opens the object's editor");
            break;
        case 5:
            if (hold(shown("ol-num-1"), "the editor's blocks are laid out")) return true;
            check(shown("ol-editor") && shown("ol-num-1") && shown("ol-next-0") && !shown("ol-grid"),
                  "the editor shows «Что это» and «Сколько» in place of the cards");
            click("ol-next-0"); // Монеты → Медь
            ol().set_prop(1, "15", false);
            check(tpl_value("coins", "what") == "\"copper\"" && tpl_value("coins", "count") == "15",
                  "the template is now 15 copper");
            check(objects::read_template(lib.find("coins")->file).value().value("count") &&
                      *objects::read_template(lib.find("coins")->file).value().value("count") == "15",
                  "the change is written to the template's file");
            check(ol().history().undo_label() == "«Монеты»: Сколько", "the change is in the tab's history");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!ol().editing(), "Esc goes back to the library");
            click_tab(0);
            break;
        case 6: {
            const flecs::entity e = placed();
            check(e.is_valid() && e.get<slice::Item>().kind == u8(slice::ItemKind::Copper) && count() == 7,
                  "the copy on the level follows the template, but keeps its own count of 7");
            check(lv().history().undo_label() != "«Монеты»: Сколько", "the level's history is not touched");
            click_tab(2);
            key(SDLK_Z, SDL_KMOD_CTRL);
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(tpl_value("coins", "what") == "\"coins\"" && tpl_value("coins", "count") == "10",
                  "Ctrl+Z twice gives the template back");
            click_tab(0);
            break;
        }
        case 7:
            check(placed().is_valid() && placed().get<slice::Item>().kind == u8(slice::ItemKind::Coins),
                  "and the copy is coins again");
            click_tab(2);
            break;
        case 8: {
            check(click("ol-place-all") && ol().cards() == 11, "«Все объекты» shows all 11 again");
            Rml::Element* wrap = ed_.find_element("ol-grid-wrap");
            check(wrap != nullptr, "the cards' area is there");
            if (!wrap) break;
            const Rml::Vector2f at = wrap->GetAbsoluteOffset(Rml::BoxArea::Border);
            right_click(at.x + wrap->GetOffsetWidth() - 30, at.y + wrap->GetOffsetHeight() - 30);
            break;
        }
        case 9:
            if (hold(shown("ctx-ol-new"), "the menu is laid out")) return true;
            check(shown("ctx-ol-new") && !shown("ctx-ol-open"), "the right button on empty space offers only «Создать»");
            check(click("ctx-ol-new"), "a click on «Создать объект…»");
            break;
        case 10:
            check(shown("ol-new-pickup-0") && shown("ol-new-person-0") && shown("ol-new-crate--1"),
                  "it lists every kind's presets and an empty one");
            click("ol-new-pickup-0");
            break;
        case 11: {
            const objects::Template* t = ol().selected();
            check(!ol().menu_open() && t && t->name == "Монетка" && tpl_value(t->id.c_str(), "count") == "1" &&
                      t->genre == "Платформер",
                  "the «Монетка» preset makes a template: one coin, for platformers");
            check(t && std::filesystem::exists(t->file) && t->file.parent_path() == ed_.objects_folder,
                  "the template is a file in the objects folder");
            new_template_ = t ? t->key : 0;
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(!lib.find(new_template_), "Ctrl+Z takes it away");
            key(SDLK_Y, SDL_KMOD_CTRL);
            check(lib.find(new_template_) != nullptr, "Ctrl+Y brings it back");
            ol().select(new_template_);
            key(SDLK_F2, SDL_KMOD_NONE);
            check(ol().renaming(), "F2 starts renaming on the card");
            break;
        }
        case 12:
            check(shown("ol-rename"), "the card shows a name field");
            check(ol().rename("Золотая монетка") && !ol().renaming() && lib.find(new_template_)->name == "Золотая монетка" &&
                      path_to_utf8(lib.find(new_template_)->file.filename()) == "Золотая монетка.object.json",
                  "renaming renames the file too");
            check(card_at(card_named("Золотая монетка"), x, y), "the new card is on screen");
            right_click(x, y);
            break;
        case 13:
            if (hold(shown("ctx-ol-genre-1"), "the menu is laid out")) return true;
            check(shown("ctx-ol-genre-1"), "its menu lists the genres");
            check(click("ctx-ol-genre-1") && lib.find(new_template_)->genre == "RPG" && !ol().menu_open(),
                  "a click on «RPG» moves it to RPG");
            check(objects::read_template(lib.find(new_template_)->file).value().genre == "RPG",
                  "the genre is written to the file");
            check(click("ol-genre-1") && card_named("Золотая монетка") >= 0 && ol().cards() == 4,
                  "and it is listed under «RPG»");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lib.find(new_template_)->genre == "Платформер", "Ctrl+Z gives the genre back");
            click("ol-place-all");
            break;
        case 14: {
            ol().select(new_template_);
            const usize before = lib.templates().size();
            key(SDLK_D, SDL_KMOD_CTRL);
            const objects::Template* copy = ol().selected();
            check(lib.templates().size() == before + 1 && copy && copy->key != new_template_ &&
                      tpl_value(copy->id.c_str(), "count") == "1",
                  "Ctrl+D makes a copy of the template");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lib.templates().size() == before, "Ctrl+Z takes the copy away");
            ol().select(new_template_);
            break;
        }
        case 15:
            check(card_at(card_named("Золотая монетка"), x, y), "the card is on screen");
            mouse(SDL_EVENT_MOUSE_MOTION, x, y);
            left_click(x, y);
            left_click(x, y);
            break;
        case 16:
            check(ol().editing() && ol().selected() && ol().selected()->key == new_template_,
                  "a double click opens the object's editor");
            check(click("ol-back") && !ol().editing(), "«К библиотеке» goes back");
            break;
        case 17:
            check(click("ol-place") && ed_.tab() == "level", "«Поставить на уровень» opens the level");
            break;
        case 18: {
            if (hold(lv().view_w() > 0, "the level view is laid out")) return true;
            const auto& defs = ed_.level_module.objects();
            const i32 armed = lv().armed_object();
            check(armed >= 0 && defs[static_cast<usize>(armed)].key == new_template_, "with the new template in hand");
            click_cell(cx_ + 8, cy_);
            const flecs::entity e = lv().selection().empty() ? flecs::entity() : lv().level().find(lv().selection()[0]);
            check(e.is_valid() && e.get<slice::Item>().count == 1 && lib.template_of(e) == lib.find(new_template_),
                  "a click places a copy of it");
            click_tab(2);
            break;
        }
        case 19:
            ol().show("");
            ol().set_search("кир");
            check(ol().cards() == 1 && ol().card_name(0) == "Кирка", "the search finds the pickaxe");
            ol().set_search("");
            break;
        case 20: {
            ol().select(new_template_);
            const usize before = lib.templates().size();
            check(click("ol-delete") && lib.templates().size() == before - 1, "«Удалить» deletes the template");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lib.templates().size() == before, "Ctrl+Z brings it back");
            break;
        }
        case 21: {
            // A picture of the project for the coins: a gold square.
            assets::CookedTexture gold{16, 16, std::vector<u8>(16 * 16 * 4)};
            for (usize i = 0; i < gold.rgba8.size(); i += 4) {
                gold.rgba8[i] = 250;
                gold.rgba8[i + 1] = 200;
                gold.rgba8[i + 2] = 20;
                gold.rgba8[i + 3] = 255;
            }
            std::vector<u8> png;
            const std::filesystem::path dir = std::filesystem::temp_directory_path() / "forge_editor_test_pictures";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            picture_file_ = dir / utf8_path("Звезда.png");
            check(assets::encode_image(gold, ".png", png) && write_file_atomic(picture_file_, png), "a test picture is made");
            ol().list_images = [this] { return std::vector<std::filesystem::path>{picture_file_}; };
            ol().select(lib.find("coins")->key);
            ol().open_editor();
            break;
        }
        case 22: {
            if (!ol().pictures_open()) {
                if (hold(shown("ol-picture-pick"), "the coins' editor is laid out")) return true;
                check(!shown("ol-picture-clear") && lib.find("coins")->picture.empty(),
                      "the coins' editor offers «Картинка: Выбрать…», the usual picture for now");
                check(click("ol-picture-pick") && ol().pictures_open(), "«Выбрать…» opens the picture chooser");
                return true;
            }
            if (hold(shown("ol-pic-0"), "the chooser is laid out")) return true;
            check(ol().picture_choices() == 1 && ol().picture_choice(0) == "Звезда", "it shows the project's picture");
            const u32 look = lib.find("coins")->look();
            check(click("ol-pic-0") && !ol().pictures_open() && lib.find("coins")->picture == "Звезда.png",
                  "a click on it gives the coins that picture");
            check(std::filesystem::exists(ed_.pictures_folder / utf8_path("Звезда.png")),
                  "the picture is copied into the game's pictures folder");
            check(objects::read_template(lib.find("coins")->file).value().picture == "Звезда.png",
                  "and named in the template's file");
            check(lib.find("coins")->look() != look, "the coins' icons are drawn again");
            std::vector<u8> rgba;
            ed_.level_module.object_icon({"coins", "", "", "", lib.find("coins")->key}, 32, rgba);
            const u8* mid = &rgba[(16 * 32 + 16) * 4];
            check(mid[0] == 250 && mid[1] == 200 && mid[2] == 20, "with the new picture");
            check(ol().history().undo_label() == "«Монеты»: картинка", "the change is in the tab's history");
            break;
        }
        case 23:
            if (hold(shown("ol-picture-clear"), "«Убрать» shows")) return true;
            check(click("ol-picture-clear") && lib.find("coins")->picture.empty(), "«Убрать» gives the usual picture back");
            ol().undo();
            check(lib.find("coins")->picture == "Звезда.png", "Ctrl+Z gives the new one back");
            click("ol-back");
            click_tab(0);
            break;
        case 24: {
            // The coins on the level are drawn with it (their icon in the palette too).
            const flecs::entity e = placed();
            check(e.is_valid() && lib.template_of(e) == lib.find("coins"), "the coins on the level are a copy of the template");
            click_tab(2);
            break;
        }
        case 25:
            // Blocks: the coins are «Тело» + «Подбирается».
            ol().select(lib.find("coins")->key);
            ol().open_editor();
            break;
        case 26:
            if (hold(shown("ol-block-pickup"), "the coins' blocks are shown")) return true;
            check(shown("ol-block-body") && shown("ol-block-remove-pickup") && ol().block_count() == 2,
                  "the coins' editor shows their blocks: «Тело» and «Подбирается»");
            check(click("ol-block-add") && ol().menu_open(), "«Добавить блок» opens the list of blocks");
            break;
        case 27: {
            if (hold(shown("ol-add-control"), "the blocks menu is laid out")) return true;
            check(!shown("ol-add-pickup") && shown("ol-add-rigid"), "it offers the blocks the coins do not have yet");
            check(click("ol-add-control") && lib.has_block(*lib.find("coins"), "control") && !ol().menu_open(),
                  "«Управление» is added to the coins");
            const objects::Template* t = lib.find("coins");
            const objects::PropDef* scheme = lib.prop_of(*t, "scheme");
            check(scheme && scheme->choices.size() >= 5 && lib.prop_of(*t, "speed") != nullptr &&
                      objects::read_template(t->file).value().blocks.size() == 3,
                  "with its «Схема» to choose and «Скорость», and the template's file lists the blocks");
            check(objects::Library::display(*scheme, lib.value(*t, *scheme)) == "Бродит туда-сюда",
                  "a new «Управление» wanders by default");
            click_tab(0);
            break;
        }
        case 28: {
            const flecs::entity e = placed();
            check(e.is_valid() && e.has<slice::Critter>() &&
                      e.get<slice::Critter>().scheme == static_cast<u8>(slice::Scheme::Wander) &&
                      e.get<slice::Item>().count == 7,
                  "the coins on the level now run, and keep their own count");
            click_tab(2);
            ol().undo();
            check(!lib.has_block(*lib.find("coins"), "control"), "Ctrl+Z takes the block away again");
            check(ol().add_block("rigid") && lib.has_block(*lib.find("coins"), "rigid") &&
                      !lib.has_block(*lib.find("coins"), "body") && !lib.has_block(*lib.find("coins"), "pickup"),
                  "«Физика» replaces «Тело», and «Подбирается», which needs a body, goes with it");
            ol().undo();
            check(lib.has_block(*lib.find("coins"), "pickup") && lib.has_block(*lib.find("coins"), "body"),
                  "Ctrl+Z gives them back");
            check(!ol().remove_block("body"), "the last blocks are not taken away: the object would be empty");
            click_tab(0);
            break;
        }
        case 29: {
            const flecs::entity e = placed();
            check(e.is_valid() && !e.has<slice::Critter>() && e.has<slice::Item>(), "and the coins on the level are coins again");
            click_tab(2);
            ol().close_editor();
            ol().select(lib.find("coins")->key); // for a screenshot
            ol().show("");
            break;
        }
        // «Общие»: objects go between the game and the shared library as copies.
        case 30: {
            check(ol().shared().templates().empty(), "the shared library starts empty");
            check(shown("ol-place-shared") && shown("ol-share"), "«Общие объекты» and «Сделать общим» are there");
            check(click("ol-share") && ol().shared().find("coins") &&
                      std::filesystem::exists(ed_.shared_folder / "objects" / lib.find("coins")->file.filename()),
                  "«Сделать общим» puts a copy of the coins into the shared library's folder");
            check(!ol().share_selected(), "sharing them again does nothing while they are the same");
            // Another game's object, with its own picture, shared earlier.
            objects::Template star = *lib.find("coins");
            star.id = "shared_star";
            star.name = "Звёздочка";
            star.picture = "Звезда.png";
            star.file.clear();
            std::optional<objects::Template> s = ol().shared().copy_from(lib, star);
            check(s && ol().shared().put(*s), "a shared object from another game");
            // The game's coins change after they were shared.
            shared_count_ = lib.value(*lib.find("coins"), *lib.prop_of(*lib.find("coins"), "count"));
            check(shared_count_ != "99" && lib.put(lib.with_value(*lib.find("coins"), "count", "99")), "the game's coins change");
            ol().show("s:");
            break;
        }
        case 31: {
            if (hold(shown("ol-shared-note"), "«Общие» shows where the shared objects are")) return true;
            check(ol().showing_shared() && ol().cards() == 2 && !shown("ol-open") && shown("ol-take"),
                  "«Общие» shows the 2 shared objects, to take into the game (not to edit)");
            ol().select(lib.find("coins")->key);
            check(ol().selected() && ol().selected()->name == "Монеты", "the shared coins are selected");
            check(click("ol-take") && lib.value(*lib.find("coins"), *lib.prop_of(*lib.find("coins"), "count")) == shared_count_,
                  "«Обновить из общих» brings the game's coins back to the shared ones");
            ol().undo();
            check(lib.value(*lib.find("coins"), *lib.prop_of(*lib.find("coins"), "count")) == "99", "Ctrl+Z undoes it");
            ol().select(fnv1a("shared_star"));
            check(ol().take_selected() && lib.find("shared_star") && lib.find("shared_star")->picture == "Звезда.png" &&
                      std::filesystem::exists(ed_.pictures_folder / utf8_path("Звезда.png")),
                  "«Взять в игру» copies another game's object into this one, picture and all");
            check(ol().remove_selected() && !ol().shared().find("shared_star") && lib.find("shared_star"),
                  "«Убрать из общих» leaves the game's copy");
            ol().undo();
            check(ol().shared().find("shared_star"), "and Ctrl+Z brings it back to the shared ones");
            ol().show("");
            break;
        }
        // «Звук»: the coins get a sound of their own from the project's sounds.
        case 32: {
            const std::filesystem::path dir = std::filesystem::temp_directory_path() / "forge_editor_test_sounds";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const audio::Tone tone[] = {{audio::Wave::Sine, 990, 1320, 0.2f}};
            std::vector<u8> wav;
            check(audio::encode_wav(*audio::synth(tone), wav) && write_file_atomic(dir / utf8_path("Звон.wav"), wav) &&
                      write_file_atomic(dir / utf8_path("Песня.mp3"), std::vector<u8>{'I', 'D', '3', 4, 0}),
                  "test sounds are made");
            sound_dir_ = dir;
            ol().list_sounds = [dir] { return std::vector<std::filesystem::path>{dir / utf8_path("Звон.wav"), dir / utf8_path("Песня.mp3")}; };
            ol().select(lib.find("coins")->key);
            ol().open_editor();
            check(ol().add_block("sound") && lib.has_block(*lib.find("coins"), "sound"), "«Добавить блок» → «Звук»");
            break;
        }
        case 33: {
            auto row_of_label = [this](const char* label) {
                for (usize i = 0; i < ol().prop_count(); ++i)
                    if (ol().prop_label(i) == label) return static_cast<int>(i);
                return -1;
            };
            sound_row_ = row_of_label("Подбирают");
            const std::string pick = "ol-sound-pick-" + std::to_string(sound_row_);
            if (hold(sound_row_ >= 0 && shown(pick), "the «Звук» rows are laid out")) return true;
            check(ol().prop_value(static_cast<usize>(sound_row_)) == "обычный", "«Подбирают»: обычный звук");
            const int near = row_of_label("Рядом");
            check(near >= 0 && ol().prop_value(static_cast<usize>(near)) == "нет", "«Рядом»: нет");
            check(!shown("ol-sound-clear-" + std::to_string(sound_row_)), "no «Убрать» while the sound is the usual one");
            check(click(pick) && ol().sounds_open(), "«Выбрать…» opens the sound chooser");
            break;
        }
        case 34: {
            if (hold(shown("ol-snd-0"), "the sound chooser is laid out")) return true;
            check(ol().sound_choices() == 2 && ol().sound_choice(0) == "Звон", "it lists the project's sounds");
            const u64 heard = ol().sounds_played();
            check(click("ol-snd-play-0") && ol().sounds_played() == heard + 1 && ol().sounds_open(), "▶ plays it without choosing");
            check(!ol().choose_sound(1) && ol().sounds_open(), "an MP3 is not taken: the game reads WAV and OGG");
            check(click("ol-snd-0") && !ol().sounds_open(), "a click on a sound chooses it");
            const objects::PropDef* p = lib.prop_of(*lib.find("coins"), "sound_pickup");
            check(p && lib.value(*lib.find("coins"), *p) == "\"Звон.wav\"" &&
                      std::filesystem::exists(ed_.sounds_folder / utf8_path("Звон.wav")),
                  "the coins sound «Звон», copied into the game's sounds folder");
            check(ol().history().undo_label() == "«Монеты»: Подбирают", "the change is in the tab's history");
            break;
        }
        case 35: {
            const std::string row = std::to_string(sound_row_);
            if (hold(shown("ol-sound-clear-" + row), "«Убрать» shows")) return true;
            check(ol().prop_value(static_cast<usize>(sound_row_)) == "Звон.wav", "the row names the sound");
            const u64 heard = ol().sounds_played();
            check(click("ol-sound-play-" + row) && ol().sounds_played() == heard + 1, "▶ in the row plays the object's sound");
            const objects::PropDef* p = lib.prop_of(*lib.find("coins"), "sound_pickup");
            check(click("ol-sound-clear-" + row) && lib.value(*lib.find("coins"), *p) == "\"\"", "«Убрать» gives the usual sound back");
            ol().undo();
            check(lib.value(*lib.find("coins"), *p) == "\"Звон.wav\"", "Ctrl+Z gives «Звон» back");
            check(ol().share_selected() && std::filesystem::exists(ed_.shared_folder / "sounds" / utf8_path("Звон.wav")),
                  "a shared object takes its sounds along");
            ol().close_editor();
            ol().show("");
            break;
        }
        case 36:
            click_tab(0);
            break;
        default: break;
        }
        ++ol_step_;
        return true;
    }

    // --- the logic tab ---
    LogicEditor& lg() { return ed_.logic_tab; }
    bool thing_click(const std::string& id) {
        f32 x = 0, y = 0;
        if (!element_center("lg-thing-" + id, x, y)) return false;
        left_click(x, y);
        return true;
    }
    i64 option_of(const char* phrase) {
        for (usize i = 0; i < lg().pick_options(); ++i)
            if (lg().pick_phrase(i) == phrase) return static_cast<i64>(i);
        return -1;
    }
    std::string logic_text() {
        std::vector<u8> bytes;
        read_file(ed_.logic_file, bytes);
        return {bytes.begin(), bytes.end()};
    }
    bool logic_step() {
        switch (lg_step_) {
        case 0:
            check(click_tab(6) && ed_.tab() == "logic", "a click on the Logic tab");
            break;
        case 1: {
            check(shown("lg-thing-hero") && shown("lg-thing-key") && shown("lg-thing-door"), "the board shows the hero, the key and the door");
            check(shown("lg-link-1") && lg().phrase_of(1) == "Ключ открывает Дверь" && lg().problem_of(1).empty(),
                  "the game's link is on the board and works");
            check(shown("lg-word-1"), "the right column says it in words");
            check(shown("lg-add-coins") && click("lg-add-coins"), "«Монеты» are offered in the left column");
            break;
        }
        case 2:
            check(shown("lg-thing-coins") && lg().history().can_undo() && lg().selected_thing() == "coins",
                  "«Монеты» go onto the board from the left column, selected");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(lg().selected_thing().empty(), "Esc lets go of them");
            check(thing_click("hero") && lg().selected_thing() == "hero", "a click on the hero selects it");
            break;
        case 3:
            check(thing_click("coins") && lg().picking(), "a click on the coins asks what the hero does with them");
            break;
        case 4:
            check(shown("lg-picker") && option_of("Герой собирает Монеты") >= 0, "the picker offers «Герой собирает Монеты»");
            check(lg().pick(static_cast<usize>(std::max<i64>(0, option_of("Герой собирает Монеты")))) && !lg().picking(),
                  "picking it makes the link");
            check(lg().links().links.size() == 2 && logic_text().find("\"collect\"") != std::string::npos,
                  "the new link is written to logic.json");
            break;
        case 5: {
            const u32 link = lg().selected_link();
            if (!check(link != 0 && shown("lg-refine-once"), "the new link is selected, its refinements shown")) break;
            check(click("lg-refine-once") && lg().links().find(link) && lg().links().find(link)->once, "«Только один раз» refines it");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lg().links().find(link) && !lg().links().find(link)->once, "Ctrl+Z takes the refinement back");
            break;
        }
        case 6: {
            f32 x = 0, y = 0;
            if (!check(lg().links().spot("key") != nullptr, "the key has a place on the board")) break;
            const logic::Spot before = *lg().links().spot("key");
            check(element_center("lg-thing-key", x, y), "the key is on screen");
            mouse(SDL_EVENT_MOUSE_MOTION, x, y);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
            mouse(SDL_EVENT_MOUSE_MOTION, x + 60, y + 20);
            mouse(SDL_EVENT_MOUSE_MOTION, x + 100, y + 40);
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, x + 100, y + 40);
            const logic::Spot* after = lg().links().spot("key");
            check(after && std::fabs(after->x - before.x - 100) < 1 && std::fabs(after->y - before.y - 40) < 1, "the key is dragged on the board");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(std::fabs(lg().links().spot("key")->x - before.x) < 1, "Ctrl+Z puts it back");
            break;
        }
        case 7:
            lg().select_link(1);
            break;
        case 8:
            check(shown("lg-remove-link") && click("lg-remove-link") && !lg().links().find(1), "a link is removed");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lg().links().find(1) != nullptr && logic_text().find("\"open\"") != std::string::npos, "and Ctrl+Z brings it back");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            break;
        case 9:
            check(lg().selected_link() == 0 && shown("lg-word-1"), "Esc goes back to the words");
            check(click("lg-mode-steps") && lg().mode() == "steps", "«Шаги» shows the same logic as steps");
            break;
        case 10:
            check(shown("lg-card-1") && shown("lg-card-2") && !shown("lg-board"), "each link is a card of steps");
            check(lg().steps_of(1).size() == 5 && lg().steps_of(1)[2].text == "Открыть Дверь", "«Ключ открывает Дверь» reads as steps");
            check(click("lg-step-x-1-sound") && !lg().links().find(1)->sound, "× takes the sound step away");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lg().links().find(1)->sound, "and Ctrl+Z brings it back");
            check(click("lg-step-add-1"), "«Добавить шаг» opens");
            break;
        case 11:
            check(shown("lg-add-step-1-night") && click("lg-add-step-1-night") && lg().links().find(1)->night,
                  "«Только ночью» adds the step «Если сейчас ночь»");
            break;
        case 12:
            check(lg().steps_of(1)[1].text == "Если сейчас ночь" && shown("lg-step-x-1-night"), "the new step is on the card");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(!lg().links().find(1)->night, "Ctrl+Z takes it back");
            check(click("lg-mode-code") && lg().mode() == "code", "«Код» shows the same logic as code");
            break;
        case 13: {
            check(shown("lg-code-1") && lg().code_lines() > 10, "the code is shown line by line");
            usize line = 0;
            while (line < lg().code_lines() && lg().code_link(line) != 2) ++line;
            check(line < lg().code_lines() && click("lg-code-" + std::to_string(line + 1)) && lg().selected_link() == 2,
                  "a click on a line selects its link");
            check(click("lg-mode-scheme") && lg().mode() == "scheme", "«Схема» shows the same logic as nodes");
            break;
        }
        case 14: {
            // «Ключ открывает Дверь» as nodes: «Когда» → «Если» (the key) → «Сделать»…
            SchemeView& sc = lg().scheme();
            const script::Graph* g = sc.graph(1);
            if (!check(g && logic::when_node(*g), "the link is a scheme starting at «Когда»")) break;
            usize of1 = 0;
            for (const auto& n : sc.node_views()) of1 += n.link == 1 && !n.hidden;
            // Only the nodes near the view are in the document.
            check(of1 > 0 && of1 <= g->nodes.size() && shown("sc-frame-1"), "a frame with the scheme's nodes");
            const std::string when = "sc-node-1-" + std::to_string(logic::when_node(*g));
            check(shown(when) && shown("sc-pin-1-" + std::to_string(logic::when_node(*g)) + "-o-next"), "«Когда» with its flow exit");
            check(click(when) && sc.selected_node() == logic::when_node(*g) && lg().selected_link() == 1, "a click on a node selects it");
            check(!shown("sc-remove-1-" + std::to_string(logic::when_node(*g))), "«Когда» cannot be taken away");
            check(click("sc-add-1") && sc.palette_open(), "«+ Нода» opens the list of nodes");
            const usize all = sc.palette_items();
            sc.set_search("прибав");
            check(all > 30 && sc.palette_items() >= 1 && sc.palette_items() < all, "the list is searched");
            break;
        }
        case 15: {
            SchemeView& sc = lg().scheme();
            check(click("sc-pal-std.var.add") && !sc.palette_open(), "a node is picked from it");
            const std::vector<logic::Step> st = lg().steps_of(1);
            check(st.size() == 2 && st[1].text.rfind("Уточнено в Схеме", 0) == 0, "the link has a scheme of its own: «Уточнено в Схеме»");
            check(shown("lg-scheme-reset") || lg().selected_link() == 1, "the side offers to take it away");
            break;
        }
        case 16: {
            SchemeView& sc = lg().scheme();
            const script::Graph* g = sc.graph(1);
            if (!check(g != nullptr, "the scheme is there")) break;
            u32 act = 0, added = 0, tail = 0;
            for (const script::GraphNode& n : g->nodes) {
                if (n.def == "logic.act") act = n.uid;
                if (n.def == "std.var.add") added = n.uid;
            }
            // The end of the chain: «Сделать» or the sound after it.
            tail = act;
            for (bool more = true; more;) {
                more = false;
                for (const script::GraphLink& w : g->links)
                    if (w.from_node == tail && w.from_pin == script::kFlowNext && w.to_node != added) {
                        tail = w.to_node;
                        more = true;
                        break;
                    }
            }
            if (!check(act && added, "«Сделать» and the new node")) break;
            const std::string a = std::to_string(act), b = std::to_string(added), t = std::to_string(tail);
            check(click("sc-pin-1-" + t + "-o-next") && sc.pin_pending(), "a click on the last node's exit waits for the other end");
            check(click("sc-pin-1-" + b + "-i-in") && !sc.pin_pending(), "a click on the new node's entry joins them");
            g = sc.graph(1);
            bool wired = false;
            for (const script::GraphLink& w : g->links) wired |= w.from_node == tail && w.to_node == added && w.to_pin == script::kFlowIn;
            check(wired, "a white wire between them");
            check(!sc.node_problem(1, added).empty() && !lg().problem_of(1).empty(), "a variable without a name is marked on the node");
            check(shown("sc-field-1-" + b + "-name") && sc.set_value(1, added, "name", "opened"), "the name is typed into the node");
            check(sc.node_problem(1, added).empty() && lg().problem_of(1).empty(), "and the scheme works");
            // A pick field: «Сделать»'s thing from the game's things.
            check(click("sc-pick-1-" + a + "-thing") && sc.picking(), "a pick field opens its list");
            break;
        }
        case 17: {
            SchemeView& sc = lg().scheme();
            u32 act = 0;
            for (const script::GraphNode& n : sc.graph(1)->nodes)
                if (n.def == "logic.act") act = n.uid;
            bool curves = !sc.wire_views().empty();
            for (const SchemeView::WireView& w : sc.wire_views()) curves &= w.points.size() > 8;
            check(curves && shown("sc-choice-key"), "wires are drawn as curves, the list has the game's things");
            check(click("sc-choice-key") && !sc.picking(), "a thing is picked");
            const script::GraphNode* n = sc.graph(1)->find(act);
            check(n && n->value("thing") && *n->value("thing") == "key", "and kept on the node");
            key(SDLK_Z, SDL_KMOD_CTRL);
            n = sc.graph(1)->find(act);
            check(n && n->value("thing") && *n->value("thing") == "door", "Ctrl+Z takes it back");
            lg().set_mode("code");
            break;
        }
        case 18: {
            check(lg().code_lines() > 0, "«Код» shows the scheme as code");
            bool has = false;
            for (usize i = 0; i < lg().code_lines(); ++i) has |= lg().code_link(i) == 1;
            check(has, "with lines of the link");
            lg().set_mode("scheme");
            break;
        }
        case 19: {
            SchemeView& sc = lg().scheme();
            if (!check(sc.graph(1) != nullptr, "the scheme is still there")) break;
            u32 added = 0;
            for (const script::GraphNode& n : sc.graph(1)->nodes)
                if (n.def == "std.var.add") added = n.uid;
            const std::string b = std::to_string(added);
            check(added && click("sc-node-1-" + b) && sc.selected_node() == added, "the new node is selected");
            key(SDLK_DELETE, SDL_KMOD_NONE);
            const std::vector<logic::Step> st = lg().steps_of(1);
            check(sc.graph(1) && !sc.graph(1)->find(added) && (st.size() < 2 || st[1].text.rfind("Уточнено", 0) != 0),
                  "Delete takes it away: the link is plain again");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(sc.graph(1) && sc.graph(1)->find(added) != nullptr, "Ctrl+Z brings it back");
            check(shown("sc-reset-1") && click("sc-reset-1") && sc.graph(1) && !sc.graph(1)->find(added), "«Как в Связях» takes the own scheme away");
            // Moving a node keeps the link plain, the node stays where it was put.
            const u32 when = sc.graph(1) ? logic::when_node(*sc.graph(1)) : 0;
            // A plain link is shown in order: «Когда» first, the others right of it or under it.
            bool ordered = sc.graph(1) && when;
            if (ordered)
                for (const script::GraphNode& n : sc.graph(1)->nodes)
                    if (n.uid != when) ordered &= n.x != sc.graph(1)->find(when)->x || n.y != sc.graph(1)->find(when)->y;
            check(ordered, "a plain link is laid out, no node on another");
            check(sc.move_node(1, when, -203, 41) && sc.graph(1)->find(when) && sc.graph(1)->find(when)->x == -208 &&
                      sc.graph(1)->find(when)->y == 48,
                  "a node is moved, onto the grid");
            const std::vector<logic::Step> st2 = lg().steps_of(1);
            check(st2.size() < 2 || st2[1].text.rfind("Уточнено", 0) != 0, "moving changes nothing in the link");
            check(shown("sc-arrange-1") && click("sc-arrange-1") && sc.graph(1)->find(when)->x != -208, "«Упорядочить» puts it back in order");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(sc.graph(1)->find(when)->x == -208, "Ctrl+Z undoes it");
            const script::Graph* g2 = sc.graph(2);
            // (Its frame may be out of view: only the nodes near it are in the document.)
            if (g2) sc.select_node(2, logic::when_node(*g2));
            check(g2 && lg().selected_link() == 2, "a node of the other link selects it");
            check(click("lg-mode-links") && lg().mode() == "links", "back to «Связи»");
            break;
        }
        case 20:
            check(shown("lg-board") && lg().selected_link() == 2, "the board is back, the link still selected");
            check(click("lg-mode-ideas") && lg().mode() == "ideas", "«Идеи» shows the same logic as ideas");
            break;
        case 21:
            check(shown("lg-recipe-1") && lg().idea_of(1) && lg().idea_of(1)->name == "Дверь с ключом", "«Ключ открывает Дверь» is the idea «Дверь с ключом»");
            check(shown("lg-field-1-a") && shown("lg-field-1-b"), "its things are its fields");
            check(click("lg-idea-new") && lg().gallery_open(), "«Новая идея» opens the ideas");
            break;
        case 22:
            check(shown("lg-idea-trap") && click("lg-idea-trap") && lg().links().links.size() == 3 && !lg().gallery_open(),
                  "«Ловушка» becomes a link");
            check(lg().links().links.back().verb == "hurt" && lg().links().links.back().b == "hero", "it hurts the hero");
            break;
        case 23: {
            const u32 trap = lg().links().links.back().id;
            check(shown("lg-recipe-" + std::to_string(trap)) && click("lg-field-" + std::to_string(trap) + "-a"), "a click on its thing");
            break;
        }
        case 24: {
            const u32 trap = lg().links().links.back().id;
            const std::string before = lg().links().find(trap)->a;
            usize other = 0;
            while (other < lg().choices() && lg().choice(other) == before) ++other;
            check(shown("lg-chooser") && other < lg().choices(), "offers the things that fit");
            if (other < lg().choices()) {
                const std::string id = lg().choice(other);
                check(click("lg-choice-" + id) && lg().links().find(trap)->a == id, "and another one is chosen");
            }
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lg().links().find(trap)->a == before, "Ctrl+Z gives the first one back");
            // A game started from the editor says which links happen.
            const std::filesystem::path fired = std::filesystem::temp_directory_path() / "forge_editor_test_fired.txt";
            const std::string text = "run 7\n1\n";
            write_file_atomic(fired, {reinterpret_cast<const u8*>(text.data()), text.size()});
            lg().set_fired_file(fired);
            break;
        }
        case 25:
            check(lg().lit(1) && !lg().lit(2), "a link that happened in the game lights up");
            lg().set_fired_file({});
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lg().links().links.size() == 2, "Ctrl+Z takes the idea back");
            check(click("lg-mode-links") && lg().mode() == "links", "back to «Связи»");
            break;
        case 26: {
            // The running game draws a link (F2 over the game): it writes the
            // file, the editor takes it in.
            logic::Logic game = lg().links();
            game.add({0, "critter", "flee", "hero"});
            check(game.save(ed_.logic_file), "the game writes the links");
            std::error_code ec;
            std::filesystem::last_write_time(ed_.logic_file, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2), ec);
            break;
        }
        case 27:
            if (hold(lg().links().links.size() == 3, "the editor takes in a link drawn in the game")) return true;
            check(lg().links().links.back().verb == "flee" && lg().links().spot("critter"), "the link and its new thing are on the board");
            check(lg().history().can_undo(), "as a change that can be taken back");
            break;
        case 28: {
            check(shown("lg-thing-critter"), "«Зверёк» shows on the board");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lg().links().links.size() == 2, "Ctrl+Z takes the game's link back");
            logic::Logic file;
            check(file.load(ed_.logic_file) && file.links.size() == 2, "and the file too (the game reads it again)");
            break;
        }
        case 29:
            // «Код» block: a link gets its own code instead of its verb.
            lg().select_link(1);
            check(click("lg-code-edit") && lg().editing_code() && lg().mode() == "code", "«Свой код» opens the link's code");
            break;
        case 30: {
            check(shown("lg-code-text"), "the code is in a text box");
            lg().set_code_text("if then");
            check(click("lg-code-save") && lg().editing_code() && lg().links().find(1)->code.empty(), "code that does not compile is not kept");
            lg().set_code_text("logic.act(\"open\", target, \"door\", self, hero)\nlogic.hint(hero, \"Скрип!\")\n");
            check(click("lg-code-save") && !lg().editing_code(), "good code is kept");
            const logic::Link* l = lg().links().find(1);
            check(l && l->code.find("Скрип!") != std::string::npos, "the link has its own code");
            break;
        }
        case 31: {
            const std::vector<logic::Step> st = lg().steps_of(1);
            check(st.size() == 2 && st[1].text == "Свой код: 2 строки", "in «Шаги» and «Схема» it is a «Код» block");
            check(shown("lg-code-reset"), "«Вернуть обычное действие» is offered");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lg().links().find(1)->code.empty(), "Ctrl+Z takes the code back");
            key(SDLK_Y, SDL_KMOD_CTRL);
            check(!lg().links().find(1)->code.empty(), "Ctrl+Y brings it again");
            check(lg().reset_code(1) && lg().links().find(1)->code.empty(), "the link gets its verb back");
            check(click("lg-mode-links") && lg().mode() == "links", "back to «Связи»");
            break;
        }
        case 32:
            // A thing's own scheme: what the door does by itself, no link.
            lg().select_thing("door");
            check(lg().selected_thing() == "door", "the door is selected on the board");
            break;
        case 33:
            check(shown("lg-thing-scheme") && click("lg-thing-scheme") && lg().mode() == "scheme", "«Своя схема вещи…» opens «Схема»");
            check(lg().links().scheme_for("door") != nullptr && lg().links().schemes.size() == 1, "the door has a scheme of its own");
            break;
        case 34: {
            SchemeView& sc = lg().scheme();
            const logic::ThingScheme* own = lg().links().scheme_for("door");
            if (!check(own != nullptr, "the scheme is there")) break;
            const u32 id = own->id;
            const std::string f = std::to_string(id);
            check(shown("sc-frame-" + f) && sc.own(id) && shown("sc-drop-" + f), "it is a frame of its own, one that can be taken away");
            const script::Graph* g = sc.graph(id);
            u32 start = 0, tick = 0;
            for (const script::GraphNode& n : g ? g->nodes : std::vector<script::GraphNode>{}) {
                if (n.def == "std.event.start") start = n.uid;
                if (n.def == "std.event.tick") tick = n.uid;
            }
            check(start && tick && shown("sc-node-" + f + "-" + std::to_string(tick)), "it starts with «При старте» and «Каждый шаг»");
            // Its list has the events; a link's has not.
            sc.open_palette(id, 0, 400);
            sc.set_search("удар");
            const usize events = sc.palette_items();
            check(events >= 1 && sc.palette_pick("std.event.hit"), "«При ударе» is in its list and is added");
            sc.open_palette(1, 0, 400);
            sc.set_search("удар");
            check(sc.palette_items() < events, "a link's list has no events");
            sc.close_palette();
            check(sc.add_node(1, "std.event.tick", 0, 0) == 0, "and a link takes none");
            // Каждый шаг → Сдвинуть (этот объект): it moves by itself.
            const u32 move = sc.add_node(id, "api.entity.move", 280, 190);
            check(move && sc.connect(id, tick, script::kFlowNext, move, script::kFlowIn), "«Сдвинуть» after «Каждый шаг»");
            const u32 self = sc.add_node(id, "std.self", 0, 400);
            check(self && sc.connect(id, self, "actor", move, "actor") && sc.connect(id, tick, "dt", move, "dx"), "«Этот объект» and the time are wired in");
            check(sc.node_problem(id, move).empty() && lg().problem_of(id).empty(), "and the scheme works");
            logic::Logic file;
            check(file.load(ed_.logic_file) && file.scheme_for("door") && file.scheme_for("door")->graph.find("api.entity.move") != std::string::npos,
                  "the file keeps it");
            lg().set_mode("code");
            break;
        }
        case 35: {
            bool has = false;
            const u32 id = lg().links().scheme_for("door") ? lg().links().scheme_for("door")->id : 0;
            for (usize i = 0; i < lg().code_lines(); ++i) has |= id && lg().code_link(i) == id;
            check(has, "«Код» shows the door's own scheme");
            lg().set_mode("scheme");
            break;
        }
        case 36: {
            SchemeView& sc = lg().scheme();
            check(click("sc-own-add") && sc.picking(), "«+ Своя схема вещи» lists the things");
            break;
        }
        case 37:
            check(!shown("sc-choice-door") && shown("sc-choice-crate") && !shown("sc-choice-hero"), "the ones without a scheme, not the hero");
            check(click("sc-choice-crate") && lg().links().schemes.size() == 2 && lg().links().scheme_for("crate"), "a pick starts the crate's");
            break;
        case 38: {
            check(lg().links().scheme_for("crate") && shown("sc-frame-" + std::to_string(lg().links().scheme_for("crate")->id)), "its frame is shown");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lg().links().schemes.size() == 1, "Ctrl+Z takes it back");
            const u32 id = lg().links().scheme_for("door")->id;
            check(click("sc-drop-" + std::to_string(id)) && lg().links().schemes.empty(), "«Убрать» takes the door's away");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(lg().links().scheme_for("door") != nullptr, "Ctrl+Z brings it back");
            break;
        }
        case 39: {
            // With the mouse: the view panned, then a node dragged (the world
            // under the view is moved; the node follows the mouse all the same).
            SchemeView& sc = lg().scheme();
            sc.focus(lg().links().scheme_for("door")->id);
            Rml::Element* pane = ed_.find_element("lg-scheme");
            if (!check(pane != nullptr, "the scheme pane")) break;
            const Rml::Vector2f at = pane->GetAbsoluteOffset(Rml::BoxArea::Padding);
            const f32 px = at.x + pane->GetClientWidth() - 30, py = at.y + pane->GetClientHeight() - 20;
            const f32 pan_x = sc.pan_x(), pan_y = sc.pan_y();
            mouse(SDL_EVENT_MOUSE_MOTION, px, py);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, px, py);
            for (int i = 1; i <= 4; ++i) mouse(SDL_EVENT_MOUSE_MOTION, px - 25.0f * static_cast<f32>(i), py - 10.0f * static_cast<f32>(i));
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, px - 100, py - 40);
            check(sc.pan_x() == pan_x - 100 && sc.pan_y() == pan_y - 40, "dragging the empty pane pans the view");
            break;
        }
        case 40: {
            SchemeView& sc = lg().scheme();
            const u32 id = lg().links().scheme_for("door")->id;
            const script::Graph* g = sc.graph(id);
            drag_node_ = 0;
            for (const script::GraphNode& n : g->nodes)
                if (n.def == "api.entity.move") drag_node_ = n.uid;
            const SchemeView::NodeView* v = nullptr;
            for (const SchemeView::NodeView& n : sc.node_views())
                if (!n.hidden && static_cast<u32>(n.link) == id && static_cast<u32>(n.uid) == drag_node_) v = &n;
            Rml::Element* pane = ed_.find_element("lg-scheme");
            if (!check(v && pane, "«Сдвинуть» is in view")) break;
            const Rml::Vector2f at = pane->GetAbsoluteOffset(Rml::BoxArea::Padding);
            x_ = at.x + v->x + sc.pan_x() + 40;
            y_ = at.y + v->y + sc.pan_y() + 12;
            drag_from_x_ = g->find(drag_node_)->x;
            drag_from_y_ = g->find(drag_node_)->y;
            mouse(SDL_EVENT_MOUSE_MOTION, x_, y_);
            mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x_, y_);
            for (int i = 1; i <= 3; ++i) mouse(SDL_EVENT_MOUSE_MOTION, x_ + 10.0f * static_cast<f32>(i), y_ + 7.0f * static_cast<f32>(i));
            break;
        }
        case 41: {
            SchemeView& sc = lg().scheme();
            const u32 id = lg().links().scheme_for("door")->id;
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, x_ + 30, y_ + 21);
            const script::GraphNode* n = sc.graph(id)->find(drag_node_);
            // 30, 21 px: onto the grid, 32 and 16.
            check(n && n->x == drag_from_x_ + 32 && n->y == drag_from_y_ + 16, "a node dragged with the mouse follows it, onto the grid");
            check(click("lg-mode-links") && lg().mode() == "links", "back to «Связи»");
            break;
        }
        default:
            lg_step_ = -1;
            return true;
        }
        ++lg_step_;
        return true;
    }

    // --- the story tab ---
    StoryEditor& st() { return ed_.story_tab; }
    bool lit(const std::string& id) {
        Rml::Element* e = ed_.find_element(id.c_str());
        return e && e->IsClassSet("lit");
    }
    std::string talk_text(const std::string& talk) {
        std::vector<u8> bytes;
        read_file(ed_.story_dir / "dialogues" / utf8_path(talk + ".json"), bytes);
        return {bytes.begin(), bytes.end()};
    }
    bool story_step() {
        const game::DialogueSource& src = st().source();
        switch (st_step_) {
        case 0:
            check(click_tab(7) && ed_.tab() == "story", "a click on the Story tab");
            break;
        case 1:
            check(st().talks() == std::vector<std::string>{"miner", "smith"} && st().opened() == "miner", "the game's two conversations, Boris's open");
            check(shown("st-talk-miner") && shown("st-talk-smith") && shown("st-line-hello") && shown("st-line-ask"),
                  "the script shows the lines");
            check(shown("st-topic-talk-0") && !shown("st-line-k_mine"), "topic answers are shown next to their words, not as lines");
            check(st().phrases("quest.pickaxe >= 1", false) == std::vector<std::string>{"задание «Потерянная кирка»: ищет кирку или дальше"},
                  "a condition says it in words");
            check(st().phrases("quest.pickaxe = 3; give(\"coins\", 30)", true) ==
                      std::vector<std::string>{"задание «Потерянная кирка» → выполнено", "герою: Монеты +30"},
                  "actions say it in words");
            check(st().playing() && st().play_node() == "hello" && lit("st-line-hello") && lit("st-rule-2"),
                  "the test plays the start that fits, lit in the script");
            check(click("st-say-hello") && st().editing(), "a click on a line opens it for typing");
            break;
        case 2:
            check(shown("st-edit"), "the line is a field now");
            st().set_edit_text("Эх, путник! Беда у меня.\n");
            break;
        case 3:
            check(!st().editing() && src.node("hello")->text == "Эх, путник! Беда у меня.", "Enter keeps the new text");
            check(talk_text("miner").find("Эх, путник! Беда у меня.") != std::string::npos, "it is written to the file at once");
            check(st().play_line().text == "Эх, путник! Беда у меня.", "the test shows the new text");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(src.node("hello")->text.rfind("Эх, путник... Беда", 0) == 0, "Ctrl+Z gives the old text back");
            break;
        case 4:
            check(st().play_next() && st().play_node() == "ask" && st().play_line().choices.size() == 3, "«Дальше» shows the question");
            check(st().play_choose(0) && st().var("quest.pickaxe") == 1 && st().play_node() == "thanks_ahead",
                  "an answer moves the quest");
            break;
        case 5:
            check(lit("st-choice-ask-0") && lit("st-line-thanks_ahead"), "the answer taken and the line shown are lit");
            st().step_item("pickaxe", 1);
            st().play();
            check(st().play_node() == "give_back" && st().var("quest.pickaxe") == 3 && st().var("inv.coins") == 30,
                  "with the pickaxe the talk goes through the junction to the reward");
            break;
        case 6: {
            check(click("st-rule-0-if") && st().menu_open() && st().menu_find("всегда (иначе)") >= 0, "a click on a mark opens its list");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!st().menu_open(), "Esc closes it");
            check(st().open_menu("choice", "ask", 0, "if"), "the list of an answer's condition");
            const int has = st().menu_find("у героя есть Кирка");
            check(has >= 0 && st().menu_pick(static_cast<usize>(has)) && src.node("ask")->choices[0].cond == "has(\"pickaxe\")",
                  "picked from the list, no formulas");
            check(st().open_menu("choice", "ask", 0, "if") && st().menu_pick(static_cast<usize>(st().menu_find("своё условие…"))) &&
                      st().editing(),
                  "«своё условие…» opens it for typing");
            st().set_edit_text("quest.pickaxe == 0\n");
            break;
        }
        case 7:
            check(src.node("ask")->choices[0].cond == "quest.pickaxe == 0", "a typed condition is kept");
            check(st().open_menu("node", "give_back", -1, "do", 1) && st().menu_pick(static_cast<usize>(st().menu_find("убрать"))) &&
                      src.node("give_back")->act == "quest.pickaxe = 3",
                  "a change is taken off a line");
            check(st().add_choice("thanks_ahead") && st().editing(), "«+ Ответ героя» adds one, open for typing");
            st().set_edit_text("Понял.\n");
            break;
        case 8:
            check(src.node("thanks_ahead")->choices.size() == 1 && src.node("thanks_ahead")->choices[0].text == "Понял.",
                  "the hero's new answer");
            check(st().add_topic("talk") && st().editing(), "«+ Тема» adds a topic, its words open for typing");
            st().set_edit_text("погода, дождь\n");
            break;
        case 9:
            check(src.node("talk")->keywords.back().words == std::vector<std::string>{"погода", "дождь"}, "the topic's words");
            st().set_quest("pickaxe", 0);
            st().play();
            st().play_next();
            check(st().play_choose(1) && st().play_node() == "talk", "to the questions");
            check(st().play_ask("Какая сегодня погода?") && st().play_line().text == "…", "a typed question finds the new topic");
            break;
        case 10:
            check(lit("st-topic-talk-" + std::to_string(src.node("talk")->keywords.size() - 1)), "the topic asked is lit");
            check(st().play_next() && st().play_node() == "talk", "its answer goes back to the questions");
            check(st().play_ask("где шахту искать") && st().play_line().text.rfind("Вход на западе", 0) == 0, "and the old ones");
            st_new_ = st().add_line_after("give_back");
            check(!st_new_.empty() && src.node("give_back")->next == st_new_ && st().editing(), "«+ Реплика» adds a line after");
            st().set_edit_text("Иди с миром.\n");
            break;
        case 11:
            check(src.node(st_new_) && src.node(st_new_)->text == "Иди с миром." && src.node(st_new_)->speaker == "miner",
                  "the new line, said by the same speaker");
            check(st().remove_line(st_new_) && src.node("give_back")->next.empty() && !src.node(st_new_), "«Убрать» takes it away again");
            check(st().open_menu("node", "hello", -1, "next") && st().menu_pick(static_cast<usize>(st().menu_find("конец разговора"))) &&
                      src.node("hello")->next.empty(),
                  "where the talk goes on, from the list");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(src.node("hello")->next == "ask", "Ctrl+Z");
            {
                game::Dialogue d;
                game::DialogueReport report;
                check(d.load(talk_text("miner"), report) && report.ok(), "the game reads what the editor wrote");
            }
            check(!st().new_talk().empty() && st().talks().size() == 3 && st().editing(), "«Новый разговор»");
            st().set_edit_text("Привет, путник!\n");
            break;
        case 12:
            check(st().source().nodes.size() == 1 && st().source().nodes[0].text == "Привет, путник!", "the new conversation's first line");
            check(st().playing(), "the new conversation plays at once");
            check(click("st-talk-miner") && st().opened() == "miner", "back to Boris");
            break;
        case 13:
            // A Ren'Py game comes in as a story of several talks.
            if (!st().import_renpy(utf8_path(FORGE_CONVERTERS_DIR) / "tests" / "renpy_game" / "game")) {
                FORGE_INFO("self-test: Python не найден, импорт Ren'Py не проверяется");
                st_step_ = 21;
                return true;
            }
            check(st().importing(), "«Импорт из Ren'Py…» starts in the background");
            break;
        case 14:
            if (st().importing() && ++st_wait_ < 20000) {
                SDL_Delay(2);
                return true;
            }
            check(!st().importing() && st().opened() == "dvor/script", "the game's script opens when it is in");
            check(talk_text("dvor/chapter").find("\"call\"") == std::string::npos &&
                      talk_text("dvor/script").find("\"call\": \"chapter:chapter_one\"") != std::string::npos,
                  "a talk per script file, calling into each other");
            check(st().playing() && st().play_node() == "start_1" && st().play_line().text == "Утро во дворе. Пахнет сиренью.",
                  "the story plays from its start");
            break;
        case 15:
            check(shown("st-talk-dvor/script") && shown("st-talk-dvor/chapter"), "the story's talks are listed");
            check(shown("st-stage-start_1-0") && lit("st-line-start_1"), "the staging is shown with the line");
            check(st().play_next() && st().play_line().choices.size() == 2, "an answer hidden by its condition is not offered");
            check(st().play_choose(0) && st().opened() == "dvor/chapter" && st().play_node() == "chapter_one_1",
                  "a call into another talk: the script follows the test there");
            break;
        case 16:
            check(lit("st-line-chapter_one_1"), "the line shown is lit in that talk");
            st().play_next();
            st().play_next();
            check(st().play_next() && st().play_node() == "chapter_one_6", "a while loop goes round, then on");
            check(st().play_choose(0) && st().opened() == "dvor/script" && st().play_line().text == "Ты сегодня богат!",
                  "return comes back after the call, the branch reads the variables");
            check(st().phrases("renpy(\"result = greet(player)\")", true) == std::vector<std::string>{"Ren'Py: result = greet(player)"},
                  "Ren'Py code kept as it was says so");
            break;
        case 17:
            check(shown("st-state-met_boris") && shown("st-state-coins"), "the variables the talk checks can be set");
            st().set_var("met_boris", 1);
            st().play();
            st().play_next();
            check(st().play_line().choices.size() == 3, "with the variable set, the hidden answer shows");
            check(st().begin_edit("stage", "start_1", 0) && st().editing(), "a staging step opens for typing");
            st().set_edit_text("scene bg street\n");
            break;
        case 18: {
            check(st().source().node("start_1")->stage[0] == "scene bg street" && talk_text("dvor/script").find("scene bg street") != std::string::npos,
                  "the staging step is changed and written");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(st().source().node("start_1")->stage[0] == "scene bg yard", "Ctrl+Z");
            // The story is named after the game (build.name); options.rpy is
            // settings, not a talk; the cast and the settings have pages.
            check(!shown("st-talk-dvor/options") && shown("st-cast-dvor") && shown("st-novel-dvor"),
                  "the story lists its cast and its settings, not options.rpy as a talk");
            check(click("st-cast-dvor") && st().view() == "cast", "a click on «Персонажи» shows the cast");
            break;
        }
        case 19: {
            bool boris = false;
            for (usize i = 0; i < st().cast_size(); ++i) boris |= st().cast_name(i) == "Борис";
            check(boris && shown("st-card-b") && !shown("st-line-start_1"), "the cast's cards instead of the script");
            check(click("st-novel-dvor") && st().view() == "novel" && st().setting_groups() >= 3, "«Настройки новеллы» shows the game's settings");
            break;
        }
        case 20:
            check(shown("st-novel-page"), "the settings page is shown");
            check(click("st-talk-dvor/script") && st().view().empty(), "a talk goes back to the script");
            break;
        case 21: {
            // A big talk shows a page at a time.
            game::DialogueSource big;
            big.id = "big";
            big.speakers.push_back({"n", "Рассказчик", "", ""});
            big.start.push_back({{}, "l0"});
            for (int i = 0; i < 400; ++i) {
                game::SourceNode n;
                n.id = "l" + std::to_string(i);
                n.text = "Строка " + std::to_string(i);
                if (i % 100 == 0) n.scene = "Часть " + std::to_string(i / 100 + 1);
                if (i + 1 < 400) n.next = "l" + std::to_string(i + 1);
                big.nodes.push_back(n);
            }
            const std::string json = big.json();
            write_file_atomic(ed_.story_dir / "dialogues" / "big.json", {reinterpret_cast<const u8*>(json.data()), json.size()});
            check(st().open("big") && st().opened() == "big", "a big talk opens");
            break;
        }
        case 22:
            check(st().lines_shown() == 150 && st().line_shown("l0") && !st().line_shown("l200"), "a big talk shows a page of lines");
            check(shown("st-goto-l300"), "its scenes are listed to go to");
            click("st-goto-l300");
            break;
        case 23:
            check(st().line_shown("l300") && !st().line_shown("l0"), "a scene clicked comes onto the page");
            check(click("st-page-prev") && true, "the page buttons");
            break;
        case 24:
            check(st().line_shown("l200") && !st().line_shown("l300"), "«Раньше» shows the lines before");
            check(click("st-talk-miner") && st().opened() == "miner", "back to Boris");
            break;
        default:
            st_step_ = -1;
            return true;
        }
        ++st_step_;
        return true;
    }

    // «Интерфейс»: the canvas played with the mouse and keys.
    UiEditor& ue() { return ed_.ui_tab; }
    u32 ue_named(const editor::design::Node& n, const std::string& name) {
        if (n.name == name) return n.id;
        for (const editor::design::Node& c : n.children)
            if (u32 id = ue_named(c, name)) return id;
        return 0;
    }
    u32 ue_named(const std::string& name) { return ue_named(ue().screen().root, name); }
    // The cells a list shows now (its content's children not hidden).
    static usize ue_list_cells(Rml::Element* content) {
        usize n = 0;
        for (int i = 0; content && i < content->GetNumChildren(); ++i)
            if (content->GetChild(i)->GetComputedValues().display() != Rml::Style::Display::None) ++n;
        return n;
    }
    u32 ue_list_ = 0, ue_text_ = 0, ue_cell_ = 0;
    usize ue_items_ = 0;
    u32 ue_edge_ = 0, ue_mid_ = 0; // layers stuck to the corner and to the middle
    std::string ue_json_;          // the screen before looking at other sizes
    editor::design::Rect ue_edge_box_, ue_mid_box_;
    f64 ue_coins_before_ = 0;
    std::string ue_expect_; // the thing of the clicked cell
    usize ue_log_size_ = 0;
    // The wheel over a list in «Проверить», on every size and fit.
    int ue_wheel_ = 0, ue_wheel_phase_ = 0, ue_notches_ = 0;
    f32 ue_wheel_top_ = -1, ue_pan_x_ = 0, ue_pan_y_ = 0, ue_wheel_x_ = 0, ue_wheel_y_ = 0;
    u64 ue_wheel_start_ = 0, ue_wheel_still_ = 0; // real milliseconds: the wheel's first notch, the scroll's last change
    struct UeSpot {
        std::string id;
        f32 x0, y0, x1, y1;
    };
    std::vector<UeSpot> ue_seen_; // where each cell stood before the wheel (page pixels)
    objects::Template ue_coins_;          // the coins before the list's picture check
    std::filesystem::path ue_pictures_; // and the pictures folder
    // A screen point in window pixels.
    f32 ue_wx(f32 x) { return ue().canvas_left() + ue().to_canvas_x(x); }
    f32 ue_wy(f32 y) { return ue().canvas_top() + ue().to_canvas_y(y); }
    void ue_drag(f32 x0, f32 y0, f32 x1, f32 y1) {
        mouse(SDL_EVENT_MOUSE_MOTION, x0, y0);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0);
        for (int i = 1; i <= 4; ++i) mouse(SDL_EVENT_MOUSE_MOTION, x0 + (x1 - x0) * static_cast<f32>(i) / 4, y0 + (y1 - y0) * static_cast<f32>(i) / 4);
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, x1, y1);
    }
    std::string ue_file(const char* ext, const std::string& name = "main_menu") {
        std::vector<u8> bytes;
        read_file(ed_.ui_game_dir / "ui" / utf8_path(name + std::string(ext)), bytes);
        return std::string(bytes.begin(), bytes.end());
    }
    u32 ue_inst_ = 0;
    std::string ue_other_; // a second screen, a window the menu opens
    u32 ue_bar_ = 0;
    std::string ue_moved_; // the title's movement at one moment
    const editor::design::Node* ue_node(u32 id) { return editor::design::find(ue().screen().root, id); }
    // «Простой»: the screen and history before a change, the beginner's own screen and blocks.
    std::string ue_disk_, ue_json2_, ue_simple_screen_;
    usize ue_cursor_ = 0, ue_entries_ = 0;
    u32 ue_simple_btn_ = 0;
    std::string ue_style_; // the new button's game colour
    Rml::Element* ue_panel_ = nullptr; // the simple panel's scrolling part
    bool ue_option_outside_ = false, ue_option_hovered_ = false; // the last option clicked: outside its panel, under the pointer
    std::vector<u32> ue_blocks_;
    // «Палитра цвета»: the layers, colours and panel the picker's checks work on.
    u32 ue_cp_btn_ = 0, ue_cp_title_ = 0, ue_cp_rect_ = 0;
    editor::design::Color ue_cp_color_{};
    std::string ue_cp_key_;
    usize ue_cp_overrides_ = 0;
    u32 ue_cp_frame_ = 0, ue_cp_red_ = 0, ue_cp_green_ = 0, ue_cp_linked_ = 0;
    editor::design::Color ue_cp_want_{};
    bool ue_cp_clicked_ = false; // the eyedropper's click on the live screen made; the game's frame after it is checked
    // A point of a layer on the page the canvas shows (as drawn: the player's size in «Проверить»), at fractions of
    // its box, in window pixels; then clicked.
    bool ue_page_click(u32 id, f32 fx, f32 fy) {
        Rml::ElementDocument* page = ue().page();
        Rml::Element* e = page ? page->GetElementById("n" + std::to_string(id)) : nullptr;
        Rml::Element* image = ed_.find_element("ue-screen");
        if (!e || !image || !page->GetContext()) return false;
        const Rml::Vector2i texture = page->GetContext()->GetDimensions();
        const Rml::Vector2f p = e->GetAbsoluteOffset(Rml::BoxArea::Border) + e->GetBox().GetSize(Rml::BoxArea::Border) * Rml::Vector2f(fx, fy);
        const Rml::Vector2f at = image->GetAbsoluteOffset(Rml::BoxArea::Border), size = image->GetBox().GetSize(Rml::BoxArea::Border);
        if (texture.x <= 0 || texture.y <= 0 || size.x <= 0) return false;
        left_click(at.x + p.x / static_cast<f32>(texture.x) * size.x, at.y + p.y / static_cast<f32>(texture.y) * size.y);
        return true;
    }
    // «Шкала времени» (13.5): the moving rectangle, the screen before a gesture and after the carried key.
    u32 ue_tl_rect_ = 0;
    std::string ue_tl_json_, ue_tl_moved_;
    usize ue_tl_cursor_ = 0;
    f64 ue_tl_t_ = 0;
    int ue_tl_stage_ = 0;
    // The screen's sound (13.7): the stage, the screen and history before, the sound's counts then, a file of «Ресурсы».
    int ue_sd_stage_ = 0;
    std::string ue_sd_json_;
    usize ue_sd_cursor_ = 0;
    u32 ue_sd_starts_ = 0, ue_sd_clicks_ = 0;
    std::filesystem::path ue_sd_extra_;
    std::function<std::vector<std::filesystem::path>()> ue_sd_list_;
    // A drop-down of the panel brought into sight and opened with the mouse.
    bool ue_sd_open(const char* id) {
        Rml::Element* e = ed_.find_element(id);
        if (!e) return false;
        e->ScrollIntoView(Rml::ScrollAlignment::Nearest);
        return ue_open(id);
    }
    // A key as the window gives it, through the editor's own way in.
    void ue_sd_key(SDL_Keycode k, bool down, bool repeat = false, SDL_Keymod mod = SDL_KMOD_NONE) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.key = k;
        e.key.mod = mod;
        e.key.down = down;
        e.key.repeat = repeat;
        ed_.handle_event(e);
    }
    f64 ue_sd_var(const char* name) { return ue().check_vars().get(name).number(); }
    // Only what plays on the check's music bus, as the device would hear it.
    f32 ue_sd_music_peak() {
        audio::Mixer& m = ue().check_mixer();
        m.set_volume(audio::Bus::Ui, 0);
        std::vector<f32> buf(4096);
        f32 p = 0;
        for (int i = 0; i < 2; ++i) {
            m.mix(buf.data(), 2048);
            p = 0;
            for (f32 v : buf) p = std::max(p, std::fabs(v));
        }
        m.set_volume(audio::Bus::Ui, 1);
        return p;
    }
    // «Создать +» and the right button's menus (13.8): the stage, the screen, its layers, history and the mouse's point.
    int ue_cm_stage_ = 0, ue_cm_kind_ = 0;
    std::string ue_cm_screen_;
    u32 ue_cm_btn_ = 0, ue_cm_inst_ = 0;
    usize ue_cm_cursor_ = 0, ue_cm_count_ = 0;
    f32 ue_cm_x_ = 0, ue_cm_y_ = 0;
    std::vector<u32> ue_cm_sel_;
    // An element's top left corner in window pixels (false when it is not shown).
    bool ue_cm_corner(const char* id, f32& x, f32& y) {
        Rml::Element* e = ed_.find_element(id);
        if (!e || !e->IsVisible(true)) return false;
        const Rml::Vector2f p = e->GetAbsoluteOffset(Rml::BoxArea::Border);
        x = p.x;
        y = p.y;
        return true;
    }
    // A menu's item pressed with the mouse.
    bool ue_cm_press(const char* id) {
        f32 x = 0, y = 0;
        if (!element_center(id, x, y)) return false;
        left_click(x, y);
        return true;
    }
    // The right button on a layer on the canvas.
    bool ue_cm_right(u32 id) {
        const auto b = ue().layer_box(id);
        if (!b) return false;
        ue_cm_x_ = ue_wx(b->cx());
        ue_cm_y_ = ue_wy(b->cy());
        right_click(ue_cm_x_, ue_cm_y_);
        return true;
    }
    // The right button on a layer's row in the list of layers (brought into sight).
    bool ue_cm_right_row(u32 id) {
        Rml::Element* e = ed_.find_element(("ue-layer-" + std::to_string(id)).c_str());
        if (!e) return false;
        e->ScrollIntoView(Rml::ScrollAlignment::Nearest);
        if (!ue_mv_row(id, 0.5f, ue_cm_x_, ue_cm_y_)) return false;
        right_click(ue_cm_x_, ue_cm_y_);
        return true;
    }
    void ue_cm_key(SDL_Keycode k, SDL_Keymod mod = SDL_KMOD_LCTRL) {
        ue_sd_key(k, true, false, mod);
        ue_sd_key(k, false, false, mod);
    }
    // The open menu is at the mouse, or moved just enough to stay whole in the tab.
    bool ue_cm_at_mouse(const char* menu) {
        Rml::Element* m = ed_.find_element(menu);
        Rml::Element* tab = ed_.find_element("ui-editor");
        f32 x = 0, y = 0;
        if (!m || !tab || !ue_cm_corner(menu, x, y)) return false;
        const Rml::Vector2f t = tab->GetAbsoluteOffset(Rml::BoxArea::Border);
        const f32 right = t.x + tab->GetOffsetWidth(), bottom = t.y + tab->GetOffsetHeight();
        const bool whole = x >= t.x - 0.5f && y >= t.y - 0.5f && x + m->GetOffsetWidth() <= right + 0.5f && y + m->GetOffsetHeight() <= bottom + 0.5f;
        const bool at_x = std::fabs(x - ue_cm_x_) < 1.5f || (x < ue_cm_x_ && std::fabs(x + m->GetOffsetWidth() - right) < 1.5f);
        const bool at_y = std::fabs(y - ue_cm_y_) < 1.5f || (y < ue_cm_y_ && std::fabs(y + m->GetOffsetHeight() - bottom) < 1.5f);
        return whole && at_x && at_y;
    }
    // The panel over the selected layer (13.10): the stage, the screen before, its text, button and picture, history
    // then, the option picked.
    int ue_fl_stage_ = 0;
    std::string ue_fl_json_, ue_fl_pick_;
    u32 ue_fl_text_ = 0, ue_fl_btn_ = 0, ue_fl_pic_ = 0;
    usize ue_fl_cursor_ = 0;
    // The panel's box in window pixels (false when it is not shown).
    bool ue_fl_box(f32& x, f32& y, f32& w, f32& h) {
        Rml::Element* e = ed_.find_element("ue-float");
        if (!e || !e->IsVisible(true)) return false;
        const Rml::Vector2f p = e->GetAbsoluteOffset(Rml::BoxArea::Border), size = e->GetBox().GetSize(Rml::BoxArea::Border);
        x = p.x;
        y = p.y;
        w = size.x;
        h = size.y;
        return true;
    }
    // As laid out: 10 px over the layer (under: 34 px under it, past its size), centred on it unless the canvas's
    // edge is nearer, whole on the canvas; why not, when not.
    bool ue_fl_placed(u32 id, bool above, std::string& why) {
        f32 x = 0, y = 0, w = 0, h = 0;
        const auto b = ue().layer_box(id);
        Rml::Element* canvas = ed_.find_element("ue-canvas");
        if (!b || !canvas || !ue_fl_box(x, y, w, h)) return why = "no panel", false;
        const f32 cl = ue().canvas_left(), ct = ue().canvas_top();
        const f32 cw = canvas->GetBox().GetSize(Rml::BoxArea::Border).x, ch = canvas->GetBox().GetSize(Rml::BoxArea::Border).y;
        const f32 top = ue_wy(b->y), bottom = ue_wy(b->y + b->h), middle = (ue_wx(b->x) + ue_wx(b->x + b->w)) * 0.5f;
        const f32 want_x = std::clamp(middle - w * 0.5f, cl + 4, cl + cw - w - 4);
        const f32 want_y = above ? top - 10 - h : bottom + 34;
        why = "panel " + std::to_string(x) + "," + std::to_string(y) + " " + std::to_string(w) + "x" + std::to_string(h) + ", want " +
              std::to_string(want_x) + "," + std::to_string(want_y);
        return std::fabs(x - want_x) < 1.5f && std::fabs(y - want_y) < 1.5f && x >= cl + 3 && y >= ct + 3 && x + w <= cl + cw - 3 &&
               y + h <= ct + ch - 3;
    }
    // A drop-down's first option other than what it shows (nullopt: none).
    std::optional<std::string> ue_fl_other(const char* id) {
        auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(ed_.find_element(id));
        for (int i = 0; select && i < select->GetNumOptions(); ++i)
            if (Rml::Element* o = select->GetOption(i)) {
                const Rml::String v = o->GetAttribute<Rml::String>("value", "");
                if (v != select->GetValue()) return v;
            }
        return std::nullopt;
    }
    // Typed into a field as from the keyboard and left as it is: no Enter, the field keeps the keyboard.
    bool ue_fl_type_only(const char* id, const std::string& text) {
        auto* e = rmlui_dynamic_cast<Rml::ElementFormControl*>(ed_.find_element(id));
        if (!e) return false;
        e->Focus();
        key(SDLK_END, SDL_KMOD_NONE);
        const Rml::String old = e->GetValue();
        const usize letters = static_cast<usize>(std::count_if(old.begin(), old.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
        for (usize i = 0; i < letters; ++i) key(SDLK_BACKSPACE, SDL_KMOD_NONE);
        SDL_Event t{};
        t.type = SDL_EVENT_TEXT_INPUT;
        t.text.text = text.c_str();
        ed_.handle_event(t);
        return e->GetValue() == text && e->IsPseudoClassSet("focus");
    }
    // The panel and the rest of the editor (13.10): the screen, the sound picked, the copy of the component, what
    // «Проверить» had played and said.
    std::string ue_fl_screen_, ue_fl_sound_;
    // A frame restacked together with one of its own layers (Codex's review on PR #70, in PR #69's restacking).
    int ue_rs_stage_ = 0;
    u32 ue_rs_a_ = 0, ue_rs_b_ = 0, ue_rs_c_ = 0, ue_rs_d_ = 0, ue_rs_e_ = 0, ue_rs_f_ = 0, ue_rs_g_ = 0;
    std::string ue_rs_screen_, ue_rs_json_, ue_rs_others_;
    usize ue_rs_cursor_ = 0;
    // A click on a layer's row in the list, Shift held or not; dx aside (two clicks on one spot are a double click).
    // The point is checked to be on the row (the list scrolls under the components): ue_rs_show a frame before.
    bool ue_rs_row(u32 id, bool shift, f32 dx) {
        Rml::Element* row = ed_.find_element(("ue-layer-" + std::to_string(id)).c_str());
        if (!row) return false;
        f32 x = 0, y = 0;
        if (!ue_mv_row(id, 0.5f, x, y)) return false;
        Rml::Element* under = ed_.context() ? ed_.context()->GetElementAtPoint({x + dx, y}) : nullptr;
        while (under && under != row) under = under->GetParentNode();
        if (!under) return false;
        SDL_SetModState(shift ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE);
        left_click(x + dx, y);
        SDL_SetModState(SDL_KMOD_NONE);
        return true;
    }
    void ue_rs_show(u32 id) {
        if (Rml::Element* row = ed_.find_element(("ue-layer-" + std::to_string(id)).c_str())) row->ScrollIntoView(Rml::ScrollAlignment::Nearest);
    }
    // A layer with all its own: ids, places and sizes.
    std::string ue_rs_branch(u32 id) {
        const editor::design::Node* n = ue_node(id);
        if (!n) return "?";
        std::string out = std::to_string(n->id) + ":" + std::to_string(n->x) + "," + std::to_string(n->y) + "," + std::to_string(n->w) +
                          "," + std::to_string(n->h) + "[";
        for (const editor::design::Node& c : n->children) out += ue_rs_branch(c.id) + " ";
        return out + "]";
    }
    // A window's behaviour (13.11): its place, pause, Esc and veil in both panels, what opens it, «Проверить».
    int ue_wb_stage_ = 0;
    std::string ue_wb_w1_, ue_wb_w2_, ue_wb_w3_, ue_wb_menu_, ue_wb_hud_, ue_wb_t1_, ue_wb_t2_, ue_wb_t3_, ue_wb_tm_, ue_wb_th_;
    u32 ue_wb_more_ = 0, ue_wb_close_ = 0, ue_wb_third_ = 0, ue_wb_settings_ = 0, ue_wb_open_ = 0, ue_wb_count_ = 0, ue_wb_scheme_ = 0,
        ue_wb_node_ = 0;
    bool ue_wb_scheme_new_ = false;
    usize ue_wb_log_ = 0;
    int ue_we_stage_ = 0; // games/examples/window-behaviour in the editor
    // The template library (13.12): «Главное меню» and the screens before it, the layers on it, the first copy taken.
    int ue_tp_stage_ = 0;
    std::string ue_tp_json_, ue_tp_disk_, ue_tp_html_;
    std::vector<std::string> ue_tp_screens_;
    usize ue_tp_layers_ = 0;
    u32 ue_tp_first_ = 0;
    // The author's own templates: the library and the menu's file before, a saved template's file, the history's place.
    std::string ue_tp_lib_json_, ue_tp_lib_html_, ue_tp_menu_file_, ue_tp_own_;
    usize ue_tp_steps_ = 0;
    u32 ue_tp_menu_ = 0, ue_tp_copy_ = 0;
    editor::design::Node ue_tp_menu_node_; // «Меню» as it was saved
    // games/examples/templates in the editor: its files as copied, the history's place, «Факел»'s card.
    int ue_te_stage_ = 0;
    std::vector<std::string> ue_te_files_;
    usize ue_te_steps_ = 0;
    u32 ue_te_torch_ = 0, ue_te_clicks_ = 0;
    bool ue_te_had_click_ = false;
    std::string ue_te_made_; // the screen made from the own screen template while its pause and picture are away
    std::vector<std::string> ue_we_files_; // its files in the game's folder, as they were before opening them again
    // The rows of a list of notes in the panel (#ue-openers, #ue-check-state): text, and whether it warns.
    std::vector<std::pair<std::string, bool>> ue_wb_rows(const char* id) {
        std::vector<std::pair<std::string, bool>> out;
        Rml::Element* box = ed_.find_element(id);
        for (int i = 0; box && i < box->GetNumChildren(); ++i) {
            Rml::Element* c = box->GetChild(i);
            if (c->IsClassSet("ue-note") && c->IsVisible(true)) out.emplace_back(c->GetInnerRML(), c->IsClassSet("ue-warn"));
        }
        return out;
    }
    std::string ue_wb_text(const std::vector<std::pair<std::string, bool>>& rows) {
        std::string out;
        for (const auto& [t, warn] : rows) out += (out.empty() ? "" : " | ") + std::string(warn ? "! " : "") + t;
        return out;
    }
    u32 ue_fl_inst_ = 0, ue_fl_clicks_ = 0;
    usize ue_fl_log_ = 0;
    // A layer of a screen as written on disk.
    std::optional<editor::design::Node> ue_fl_on_disk(const std::string& screen, u32 id) {
        editor::design::Screen s;
        if (!editor::design::load_screen(ue_file(".json", screen), s)) return std::nullopt;
        const editor::design::Node* n = editor::design::find(s.root, id);
        return n ? std::optional<editor::design::Node>(*n) : std::nullopt;
    }
    // A click on the canvas at a layer's centre.
    bool ue_fl_click(u32 id) {
        const auto b = ue().layer_box(id);
        if (!b) return false;
        left_click(ue_wx(b->cx()), ue_wy(b->cy()));
        return true;
    }
    // Moving layers between frames (13.6): the example screen, as it was, its history.
    int ue_mv_stage_ = 0;
    std::string ue_mv_json_, ue_mv_orig_;
    usize ue_mv_cursor_ = 0;
    f64 ue_mv_presses_ = 0;
    int ue_mv_presses_n_ = 0;
    editor::design::Rect ue_mv_box_{};
    f32 ue_mv_x_ = 0, ue_mv_y_ = 0; // where the mouse let go (window pixels)
    u64 ue_mv_t_ = 0;          // when the mouse came over a frame
    std::string ue_mv_listed_; // the screen with the picture made a list
    std::string ue_mv_fill_json_; // before a move checked on its own
    usize ue_mv_fill_cursor_ = 0;
    u32 ue_mv_inst_ = 0;
    int ue_mv_variant_ = 0; // 0: the frame stretched by a row, 1: by a column
    int ue_mv_variant2_ = 0; // neighbours: 0 one frame, 1 two layers, 2 inside a frame
    std::vector<u32> ue_mv_sources_;
    u32 ue_mv_target_ = 0;
    std::vector<std::pair<u32, editor::design::Rect>> ue_mv_boxes_;
    std::string ue_mv_kids_;
    // A point in a layer's row of the layers' list: frac 0 its top, 1 its bottom (window pixels).
    bool ue_mv_row(u32 id, f32 frac, f32& x, f32& y) {
        Rml::Element* e = ed_.find_element(("ue-layer-" + std::to_string(id)).c_str());
        if (!e || !e->IsVisible(true)) return false;
        const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border), size = e->GetBox().GetSize(Rml::BoxArea::Border);
        x = at.x + size.x * 0.5f;
        y = at.y + size.y * frac;
        return true;
    }
    // A layer's row carried onto another row (frac within it); held, or let go there.
    bool ue_mv_carry(u32 id, u32 onto, f32 frac, bool let_go = true) {
        f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!ue_mv_row(id, 0.5f, x0, y0) || !ue_mv_row(onto, frac, x1, y1)) return false;
        // Each press a little aside of the last: two presses on one spot at once would be a double click (renaming).
        x0 += static_cast<f32>(ue_mv_presses_n_++ % 6) * 8.0f - 20.0f;
        mouse(SDL_EVENT_MOUSE_MOTION, x0, y0);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0);
        for (int i = 1; i <= 4; ++i) mouse(SDL_EVENT_MOUSE_MOTION, x0 + (x1 - x0) * static_cast<f32>(i) / 4, y0 + (y1 - y0) * static_cast<f32>(i) / 4);
        if (let_go) mouse(SDL_EVENT_MOUSE_BUTTON_UP, x1, y1);
        return true;
    }
    // A layer taken on the canvas at its middle and carried to a screen point, held there.
    bool ue_mv_take(u32 id, f32 sx, f32 sy) {
        const auto b = ue().layer_box(id);
        if (!b) return false;
        const f32 x0 = ue_wx(b->cx()), y0 = ue_wy(b->cy());
        ue_mv_x_ = ue_wx(sx);
        ue_mv_y_ = ue_wy(sy);
        mouse(SDL_EVENT_MOUSE_MOTION, x0, y0);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0);
        for (int i = 1; i <= 6; ++i)
            mouse(SDL_EVENT_MOUSE_MOTION, x0 + (ue_mv_x_ - x0) * static_cast<f32>(i) / 6, y0 + (ue_mv_y_ - y0) * static_cast<f32>(i) / 6);
        return true;
    }
    std::string ue_mv_kids(u32 id) {
        std::string out;
        if (const editor::design::Node* n = ue_node(id))
            for (const editor::design::Node& c : n->children) out += (out.empty() ? "" : " ") + std::to_string(c.id);
        return out;
    }
    u32 ue_mv_parent(u32 id) {
        const std::vector<u32> path = editor::design::path_to(ue().screen().root, id);
        return path.size() >= 2 ? path[path.size() - 2] : 0;
    }
    bool ue_mv_same() { return editor::design::save_screen(ue().screen()) == ue_mv_json_ && ue().history().cursor() == ue_mv_cursor_; }
    bool ue_mv_near(const std::optional<editor::design::Rect>& b, f32 x, f32 y) {
        return b && std::fabs(b->x - x) < 0.6f && std::fabs(b->y - y) < 0.6f;
    }
    void ue_wheel(f32 dy) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.y = dy;
        ed_.handle_event(e);
    }
    // A window x on the timeline's track at a time (seconds), and a key's mark.
    // The timeline scrolled into the panel's view, as the author would see it before using it.
    void ue_tl_into_view() {
        if (Rml::Element* tl = ed_.find_element("ue-tl"); tl && tl->IsVisible(true)) {
            tl->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
            ed_.context()->Update();
        }
    }
    f32 ue_tl_x(f64 seconds) {
        ue_tl_into_view();
        Rml::Element* track = ed_.find_element("ue-tl-track");
        if (!track) return 0;
        const f32 left = track->GetAbsoluteOffset(Rml::BoxArea::Border).x, w = track->GetBox().GetSize(Rml::BoxArea::Border).x;
        return left + w * static_cast<f32>(seconds / ue().timeline_span());
    }
    f32 ue_tl_y() {
        f32 x = 0, y = 0;
        ue_point("ue-tl-track", 0.5f, 0.5f, x, y);
        return y;
    }
    // A key's mark held, carried to a window x in steps (and let go unless `hold`).
    bool ue_tl_carry(int index, f32 to_x, bool hold = false) {
        ue_tl_into_view();
        f32 x = 0, y = 0;
        if (!ue_point(("ue-tl-key-" + std::to_string(index)).c_str(), 0.5f, 0.5f, x, y)) return false;
        mouse(SDL_EVENT_MOUSE_MOTION, x, y);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        for (int i = 1; i <= 6; ++i) mouse(SDL_EVENT_MOUSE_MOTION, x + (to_x - x) * static_cast<f32>(i) / 6, y);
        if (!hold) mouse(SDL_EVENT_MOUSE_BUTTON_UP, to_x, y);
        return true;
    }
    // The page's rectangle: how far its movement has shifted it now (the x of its translate).
    f32 ue_tl_shift() {
        Rml::ElementDocument* page = ue().page();
        Rml::Element* e = page ? page->GetElementById("n" + std::to_string(ue_tl_rect_)) : nullptr;
        const Rml::Property* p = e ? e->GetProperty("transform") : nullptr;
        const Rml::TransformPtr t = p ? p->Get<Rml::TransformPtr>() : nullptr;
        if (t)
            for (const Rml::TransformPrimitive& prim : t->GetPrimitives())
                if (prim.type == Rml::TransformPrimitive::TRANSLATE2D) return prim.translate_2d.values[0].number;
        return 0;
    }
    std::vector<f32> ue_tl_ats() {
        std::vector<f32> out;
        if (const editor::design::Node* n = ue_node(ue_tl_rect_))
            for (const editor::design::MotionKey& k : n->motion.keys) out.push_back(std::round(k.at * 100));
        return out;
    }
    std::vector<f32> ue_tl_xs() {
        std::vector<f32> out;
        if (const editor::design::Node* n = ue_node(ue_tl_rect_))
            for (const editor::design::MotionKey& k : n->motion.keys) out.push_back(k.x);
        return out;
    }
    static std::string ue_list(const std::vector<f32>& v) {
        std::string out;
        for (f32 x : v) out += (out.empty() ? "" : " ") + std::to_string(static_cast<int>(std::lround(x)));
        return out;
    }
    // A pixel of the page as drawn (page pixels: «Макет», the screen's own size).
    bool ue_page_pixel(int x, int y, u8 rgba[4]) {
        Rml::ElementDocument* page = ue().page();
        return page && page->GetContext() && ed_.ui().read_pixel(page->GetContext(), static_cast<u32>(x), static_cast<u32>(y), rgba);
    }
    std::string ue_text_of(const char* id) {
        Rml::Element* e = ed_.find_element(id);
        return e && e->IsVisible(true) ? e->GetInnerRML() : std::string();
    }
    // A point of an element at fractions of its box, window pixels (false when it is not shown).
    bool ue_point(const char* id, f32 fx, f32 fy, f32& x, f32& y) {
        Rml::Element* e = ed_.find_element(id);
        if (!e || !e->IsVisible(true)) return false;
        const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border), size = e->GetBox().GetSize(Rml::BoxArea::Border);
        x = at.x + size.x * fx;
        y = at.y + size.y * fy;
        return true;
    }
    // A click with the mouse in the middle of an element, scrolled into its panel's view first (as the author would).
    bool ue_press(const std::string& id) {
        if (Rml::Element* e = ed_.find_element(id.c_str()); e && e->IsVisible(true)) {
            e->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
            ed_.context()->Update();
        }
        f32 x = 0, y = 0;
        if (!ue_point(id.c_str(), 0.5f, 0.5f, x, y)) return false;
        left_click(x, y);
        return true;
    }
    // Held down at one point of an element, moved in steps and let go at another (the picker's square and bars).
    bool ue_slide(const char* id, f32 fx0, f32 fy0, f32 fx1, f32 fy1, const std::function<void()>& between = {}) {
        f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!ue_point(id, fx0, fy0, x0, y0) || !ue_point(id, fx1, fy1, x1, y1)) return false;
        mouse(SDL_EVENT_MOUSE_MOTION, x0, y0);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x0, y0);
        for (int i = 1; i <= 4; ++i) {
            mouse(SDL_EVENT_MOUSE_MOTION, x0 + (x1 - x0) * static_cast<f32>(i) / 4, y0 + (y1 - y0) * static_cast<f32>(i) / 4);
            if (between) between();
        }
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, x1, y1);
        return true;
    }
    // Typed into a field without Enter (the text stays in it).
    bool ue_keys(const char* id, const std::string& text) {
        auto* e = rmlui_dynamic_cast<Rml::ElementFormControl*>(ed_.find_element(id));
        if (!e) return false;
        e->Focus();
        key(SDLK_END, SDL_KMOD_NONE);
        const Rml::String old = e->GetValue();
        const usize letters = static_cast<usize>(std::count_if(old.begin(), old.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
        for (usize i = 0; i < letters; ++i) key(SDLK_BACKSPACE, SDL_KMOD_NONE);
        SDL_Event t{};
        t.type = SDL_EVENT_TEXT_INPUT;
        t.text.text = text.c_str();
        ed_.handle_event(t);
        return e->GetValue() == text;
    }
    std::string ue_field(const char* id) {
        auto* e = rmlui_dynamic_cast<Rml::ElementFormControl*>(ed_.find_element(id));
        return e ? std::string(e->GetValue()) : std::string("<none>");
    }
    // The picker wholly inside the window, its «Готово» and HEX field found by the pointer where they show.
    bool ue_picker_reachable(std::string& why) {
        Rml::Element* p = ed_.find_element("ue-picker");
        Rml::Context* c = ed_.context();
        if (!p || !c || !p->IsVisible(true)) return why = "no picker", false;
        const Rml::Vector2f at = p->GetAbsoluteOffset(Rml::BoxArea::Border), size = p->GetBox().GetSize(Rml::BoxArea::Border);
        const Rml::Vector2i w = c->GetDimensions();
        why = "picker " + std::to_string(static_cast<int>(at.x)) + "," + std::to_string(static_cast<int>(at.y)) + " " +
              std::to_string(static_cast<int>(size.x)) + "x" + std::to_string(static_cast<int>(size.y)) + " in " +
              std::to_string(w.x) + "x" + std::to_string(w.y);
        if (at.x < 0 || at.y < 0 || at.x + size.x > static_cast<f32>(w.x) || at.y + size.y > static_cast<f32>(w.y)) return false;
        for (const char* id : {"ue-cp-done", "ue-cp-cancel", "ue-cp-hex", "ue-cp-sv"}) {
            f32 x = 0, y = 0;
            if (!ue_point(id, 0.5f, 0.5f, x, y)) return why += std::string(", no ") + id, false;
            bool found = false;
            for (Rml::Element* e = c->GetElementAtPoint({x, y}); e; e = e->GetParentNode()) found = found || e->GetId() == id;
            if (!found) return why += std::string(", the pointer misses ") + id, false;
        }
        return true;
    }
    // Typed into a field as from the keyboard: focus, End and Backspace over the old text, the text, Enter, then away.
    // (Ctrl+A is not used: RmlUi reads the modifiers from the real keyboard, not from the event.)
    bool ue_type(const char* id, const std::string& text) {
        auto* e = rmlui_dynamic_cast<Rml::ElementFormControl*>(ed_.find_element(id));
        if (!e) return false;
        e->Focus();
        key(SDLK_END, SDL_KMOD_NONE);
        const Rml::String old = e->GetValue();
        const usize letters = static_cast<usize>(std::count_if(old.begin(), old.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
        for (usize i = 0; i < letters; ++i) key(SDLK_BACKSPACE, SDL_KMOD_NONE);
        SDL_Event t{};
        t.type = SDL_EVENT_TEXT_INPUT;
        t.text.text = text.c_str();
        ed_.handle_event(t);
        const bool typed = e->GetValue() == text;
        key(SDLK_RETURN, SDL_KMOD_NONE);
        e->Blur();
        return typed;
    }
    // Typed with Enter, the field kept (no blur after it).
    bool ue_type_enter(const char* id, const std::string& text) {
        auto* e = rmlui_dynamic_cast<Rml::ElementFormControl*>(ed_.find_element(id));
        if (!e) return false;
        e->Focus();
        return ue_type_here(text);
    }
    // Typed into the field that has the keyboard, as it is: its text replaced, Enter.
    bool ue_type_here(const std::string& text) {
        auto* e = rmlui_dynamic_cast<Rml::ElementFormControl*>(ed_.context()->GetFocusElement());
        if (!e) return false;
        key(SDLK_END, SDL_KMOD_NONE);
        const Rml::String old = e->GetValue();
        const usize letters = static_cast<usize>(std::count_if(old.begin(), old.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
        for (usize i = 0; i < letters; ++i) key(SDLK_BACKSPACE, SDL_KMOD_NONE);
        SDL_Event t{};
        t.type = SDL_EVENT_TEXT_INPUT;
        t.text.text = text.c_str();
        ed_.handle_event(t);
        const bool typed = e->GetValue() == text;
        key(SDLK_RETURN, SDL_KMOD_NONE);
        return typed;
    }
    // A drop-down used with the mouse: a click opens it, and on a later frame (once its list is laid out) a click on the option.
    bool ue_open(const char* id) {
        f32 x = 0, y = 0;
        if (!element_center(id, x, y)) return false;
        left_click(x, y);
        return true;
    }
    bool ue_option(const char* id, const std::string& value) {
        auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(ed_.find_element(id));
        for (int i = 0; select && i < select->GetNumOptions(); ++i) {
            Rml::Element* o = select->GetOption(i);
            if (!o || o->GetAttribute<Rml::String>("value", "") != value) continue;
            if (!o->IsVisible(true)) return false;
            const Rml::Vector2f p = o->GetAbsoluteOffset(Rml::BoxArea::Border) + o->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
            // Whether the option shows outside the scrolling panel the drop-down is in (its list leaves the panel's clipping).
            ue_option_outside_ = false;
            for (Rml::Element* a = select->GetParentNode(); a; a = a->GetParentNode()) {
                const auto& c = a->GetComputedValues();
                if (c.overflow_y() != Rml::Style::Overflow::Auto && c.overflow_y() != Rml::Style::Overflow::Scroll) continue;
                const Rml::Vector2f at = a->GetAbsoluteOffset(Rml::BoxArea::Padding), sz = a->GetBox().GetSize(Rml::BoxArea::Padding);
                ue_option_outside_ = p.y < at.y || p.y > at.y + sz.y || p.x < at.x || p.x > at.x + sz.x;
                break;
            }
            mouse(SDL_EVENT_MOUSE_MOTION, p.x, p.y);
            Rml::Element* hover = ed_.context() ? ed_.context()->GetHoverElement() : nullptr;
            ue_option_hovered_ = hover == o || (hover && hover->GetParentNode() == o);
            left_click(p.x, p.y);
            return select->GetValue() == value;
        }
        return false;
    }
    bool ui_step() {
        namespace d = editor::design;
        const u32 title = ue_named("Название игры");
        switch (ue_step_) {
        case 0:
            check(click_tab(8) && ed_.tab() == "ui", "a click on the Interface tab");
            break;
        case 1: {
            check(ue().opened() == "main_menu" && ue().screen().title == "Главное меню", "a new game gets a main menu screen");
            ue_html_ = ue_file(".html");
            check(ue_html_.find("Старая шахта") != std::string::npos, "the screen is written as a page the game can show");
            check(title && shown("ue-layer-" + std::to_string(title)) && shown("ue-layer-" + std::to_string(ue_named("Новая игра"))),
                  "the layers list shows the screen's layers");
            const auto box = ue().layer_box(title);
            check(box && box->w > 300 && box->h > 50, "the engine laid the screen out");
            check(ue().zoom() > 0.2f && ue().zoom() < 1.0f, "the screen fits the canvas");
            if (box) left_click(ue_wx(box->cx()), ue_wy(box->cy()));
            break;
        }
        case 2: {
            check(ue().selection() == std::vector<u32>{title}, "a click on the title selects it");
            check(shown("ue-selection"), "the selection has its frame and handles");
            const auto box = ue().layer_box(title);
            if (box) ue_drag(ue_wx(box->cx()), ue_wy(box->cy()), ue_wx(box->cx() + 77), ue_wy(box->cy() + 333));
            break;
        }
        case 3: {
            const d::Node* n = ue_node(title);
            check(n && std::abs(n->x - 217) < 8 && std::abs(n->y - 483) < 8, "dragging moves the title");
            {
                d::Screen saved;
                const d::Node* m = d::load_screen(ue_file(".json"), saved) ? d::find(saved.root, title) : nullptr;
                check(n && m && m->x == n->x && m->y == n->y, "the move is saved");
            }
            key(SDLK_Z, SDL_KMOD_CTRL);
            n = ue_node(title);
            check(n && n->x == 140 && n->y == 150, "undo puts it back");
            key(SDLK_R, SDL_KMOD_NONE);
            check(ue().tool() == UiEditor::Tool::Rectangle, "R picks the rectangle");
            ue_drag(ue_wx(1203), ue_wy(613), ue_wx(1503), ue_wy(813));
            break;
        }
        case 4: {
            ue_rect_ = ue().selection().size() == 1 ? ue().selection()[0] : 0;
            const d::Node* r = ue_node(ue_rect_);
            check(r && r->type == d::NodeType::Rectangle && std::abs(r->w - 300) < 8 && std::abs(r->h - 200) < 8,
                  "dragging with the rectangle tool draws one");
            check(ue().tool() == UiEditor::Tool::Select, "then the tool is back to select");
            check(ue().set_property("w", "250") && r && ue_node(ue_rect_)->w == 250, "its width typed in");
            check(ue().set_property("fill.0.color", "#FF0000") && ue_file(".html").find("n" + std::to_string(ue_rect_)) != std::string::npos,
                  "its colour is set and it is on the page");
            break;
        }
        case 5: {
            // Its left edge brought near the title's: it snaps onto it.
            const auto box = ue().layer_box(ue_rect_);
            const auto t = ue().layer_box(title);
            check(box && t, "both are laid out");
            if (box && t) ue_drag(ue_wx(box->cx()), ue_wy(box->cy()), ue_wx(box->cx() - (box->x - t->x) + 3), ue_wy(box->cy()));
            break;
        }
        case 6: {
            const d::Node* r = ue_node(ue_rect_);
            const auto t = ue().layer_box(title);
            check(r && t && std::abs(r->x - t->x) < 0.01f, "a moved layer snaps to another's edge");
            const usize before = ue().screen().root.children.size();
            key(SDLK_D, SDL_KMOD_CTRL);
            check(ue().screen().root.children.size() == before + 1 && ue().selection().size() == 1 && ue().selection()[0] != ue_rect_,
                  "Ctrl+D duplicates");
            key(SDLK_DELETE, SDL_KMOD_NONE);
            check(ue().screen().root.children.size() == before && ue().selection().empty(), "Delete removes");
            ue().select({ue_rect_});
            key(SDLK_D, SDL_KMOD_CTRL);
            ue().select({ue_rect_, ue().selection()[0]});
            key(SDLK_A, SDL_KMOD_SHIFT);
            const d::Node* parent = ue().selection().size() == 1 ? ue_node(ue().selection()[0]) : nullptr;
            check(parent && parent->layout.mode != d::LayoutMode::None && parent->children.size() == 2,
                  "Shift+A puts the two into a frame with auto layout");
            break;
        }
        case 7: {
            // A guide pulled from the top ruler, then dragged back onto it.
            f32 rx = 0, ry = 0;
            check(element_center("ue-ruler-x", rx, ry), "the top ruler is there");
            ue_drag(rx, ry, rx, ue_wy(540));
            check(ue().screen().guides.size() == 1 && !ue().screen().guides[0].vertical &&
                      std::abs(ue().screen().guides[0].position - 540) < 2,
                  "a guide is pulled out of the ruler");
            const f32 gy = ue_wy(ue().screen().guides.empty() ? 0 : ue().screen().guides[0].position);
            ue_drag(rx, gy, rx, ry);
            check(ue().screen().guides.empty(), "a guide dragged back onto the ruler is gone");
            break;
        }
        case 8:
            key(SDLK_0, SDL_KMOD_SHIFT);
            check(std::abs(ue().zoom() - 1) < 0.001f, "Shift+0 shows 100%");
            key(SDLK_1, SDL_KMOD_SHIFT);
            check(ue().zoom() < 1 && ue().zoom() > 0.2f, "Shift+1 fits the screen again");
            break;
        case 9:
            // The screen's own settings, with nothing selected.
            ue().select({});
            check(ue().set_property("screen.font_size", "30") && ue().screen().text.size == 30 &&
                      ue_file(".html").find("font-size: 30px") != std::string::npos,
                  "the screen's text size goes to the page");
            check(ue().set_property("screen.safe", "40") && ue().screen().safe == 40, "the safe area is set");
            break;
        case 10: {
            check(shown("ue-screen-font"), "with nothing selected the panel shows the screen's settings");
            check(shown("ue-safe"), "the safe area is drawn");
            // Drawn art on a button: a frame picture, a mask, a tiled texture.
            const u32 button = ue_named("Кнопка «Новая игра»");
            ue().select({button});
            check(button && ue().set_property("frame.add", "") && ue_node(button)->frame.image == "pictures/интерфейс/рамка_дерево.png",
                  "a frame picture is added to a button");
            check(std::filesystem::exists(ed_.ui_game_dir / utf8_path("pictures/интерфейс/рамка_дерево.png")),
                  "the sample pictures are in the game");
            check(ue_node(button)->frame.slice[0] == 32, "the sample frame comes with its cut");
            check(ue_file(".html").find("border-image-source") != std::string::npos, "the frame is on the page");
            check(ue().set_property("mask.add", "") && ue_file(".html").find("mask-image") != std::string::npos, "a mask is added");
            const usize fill = ue_node(button)->fills.size();
            check(ue().set_property("fill.add", "") && ue().set_property("fill." + std::to_string(fill) + ".kind", "image") &&
                      ue().set_property("fill." + std::to_string(fill) + ".image", "pictures/интерфейс/камень.png") &&
                      ue().set_property("fill." + std::to_string(fill) + ".fit", "tile") &&
                      ue().set_property("fill." + std::to_string(fill) + ".tile", "32"),
                  "a tiled texture is set");
            check(ue_file(".html").find("32px auto") != std::string::npos, "the tiles' size is on the page");
            const auto box = ue().layer_box(button);
            check(box && box->w > 100, "the button is still laid out");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(ue_node(button)->fills.size() == fill + 1 && ue_node(button)->fills[fill].tile == 0, "undo takes back a step");
            ue().select({ue_named("Кнопка «Настройки»")});
            break;
        }
        case 11: {
            // A button made a component, from the panel.
            f32 bx = 0, by = 0;
            check(element_center("ue-make-component", bx, by), "a layer can be made a component");
            left_click(bx, by);
            const u32 id = ue().selection().empty() ? 0 : ue().selection()[0];
            const d::Node* n = ue_node(id);
            check(n && n->component == "Кнопка «Настройки»" && n->master != 0, "the button is now a copy of a component");
            check(d::find_component(d::components(ue().library()), "Кнопка «Настройки»"), "the component is in the library");
            check(std::filesystem::exists(ed_.ui_game_dir / "ui" / "components.json"), "the library is saved");
            ue_inst_ = id;
            break;
        }
        case 12: {
            check(shown("ue-comp-0") && shown("ue-detach-instance"), "the components list and the copy's panel show");
            // The copy's own text, then the component changes.
            const u32 label = ue_named("Настройки");
            ue().select({label});
            check(ue().set_property("text", "Опции") && ue_node(label)->overrides == std::vector<std::string>{"text"},
                  "a change on the copy is its own");
            ue().select({ue_inst_});
            check(ue().edit_component() && ue().library_open() && ue().selection().size() == 1, "the component opens in the library");
            check(ue().add_variant(true), "button states are added");
            const std::vector<d::Component> list = d::components(ue().library());
            const d::Component* c = d::find_component(list, "Кнопка «Настройки»");
            check(c && c->variants.size() == 4 && d::state_property(*c, ue().library()) == "Состояние", "four states");
            u32 hover = 0;
            for (const d::Node& v : ue().screen().root.children)
                if (d::variant_value(v.variant, "Состояние") == "Наведение") hover = v.id;
            ue().select({hover});
            check(hover && ue().set_property("fill.0.color", "#FF8800"), "the hover look is drawn");
            check(ue().set_property("component.name", "Кнопка меню"), "the component is renamed");
            check(ue().open("main_menu"), "back to the screen");
            const d::Node* n = ue_node(ue_inst_);
            check(n && n->component == "Кнопка меню" && n->variant.size() == 1, "the copy follows the component");
            check(n && n->children.size() == 1 && n->children[0].text == "Опции",
                  "and keeps its own text");
            const std::string html = ue_file(".html");
            check(html.find("#n" + std::to_string(ue_inst_) + ":hover {") != std::string::npos &&
                      html.find("#ff8800") != std::string::npos,
                  "the page shows the hover look on hover");
            check(html.find("#n" + std::to_string(ue_inst_) + ".disabled {") != std::string::npos, "and the disabled one");
            break;
        }
        case 13: {
            // Another copy from the list, its state picked.
            f32 cx = 0, cy = 0;
            check(element_center("ue-comp-0", cx, cy), "the component is listed");
            const usize before = ue().screen().root.children.size();
            left_click(cx, cy);
            check(ue().screen().root.children.size() == before + 1, "a click puts a copy on the screen");
            const u32 copy = ue().selection().empty() ? 0 : ue().selection()[0];
            check(ue().set_property("instance.Состояние", "Выключена") && ue_node(copy) && ue_node(copy)->opacity == 0.5f,
                  "the copy shows another state");
            check(ue().detach_instance() && ue_node(copy)->component.empty(), "a copy can be detached");
            key(SDLK_Z, SDL_KMOD_CTRL);
            key(SDLK_Z, SDL_KMOD_CTRL);
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(ue().screen().root.children.size() == before, "undo takes the copy away");
            ue().select({ue_inst_});
            check(ue().reset_instance() && ue_node(ue_inst_)->children[0].text == "Настройки", "reset forgets the copy's text");
            break;
        }
        case 14: {
            // The game's colours and text styles, set on the library's page.
            check(ue().open_library(), "the library opens");
            ue().select({});
            check(ue().set_property("color.add", "") && ue().set_property("color.0.name", "Золото") &&
                      ue().set_property("color.0.color", "#E8B04A") && ue().library().colors.size() == 1,
                  "a game colour is added");
            check(ue().set_property("textstyle.add", "") && ue().set_property("textstyle.0.size", "72") &&
                      ue().library().text_styles.size() == 1,
                  "a text style is added");
            break;
        }
        case 15: {
            check(shown("ue-color-add") && shown("ue-color-name-0"), "the library's page shows the game's styles");
            const std::string color = ue().library().colors.empty() ? std::string() : ue().library().colors[0].key;
            const std::string text = ue().library().text_styles.empty() ? std::string() : ue().library().text_styles[0].key;
            check(ue().open("main_menu"), "back to the screen");
            ue().select({});
            check(ue().set_property("fill.0.style", color) && ue().screen().root.fills[0].color == d::Color{0xE8, 0xB0, 0x4A, 255},
                  "the background takes the game colour");
            ue().select({ue_named("Название игры")});
            check(ue().set_property("text_style", text) && ue_node(ue_named("Название игры"))->text_style.size == 72,
                  "the title takes the text style");
            // Changed in the library: changed on the screen.
            check(ue().open_library(), "the library again");
            ue().select({});
            check(ue().set_property("color.0.color", "#123456") && ue().set_property("textstyle.0.size", "90"), "the styles change");
            check(ue().open("main_menu"), "back to the screen");
            check(ue().screen().root.fills[0].color == d::Color{0x12, 0x34, 0x56, 255} &&
                      ue_node(ue_named("Название игры"))->text_style.size == 90,
                  "the screen follows the game's styles");
            check(ue_file(".html").find("#123456") != std::string::npos, "and its page too");
            break;
        }
        case 16: {
            const std::string color = ue().library().colors[0].key;
            const std::string text = ue().library().text_styles[0].key;
            check(ue().open_library(), "open styles for deletion and recreation");
            ue().select({});
            check(ue().set_property("color.0.remove", "") && ue().set_property("color.add", ""), "replace a game colour");
            check(ue().set_property("textstyle.0.remove", "") && ue().set_property("textstyle.add", ""), "replace a text style");
            check(ue().library().colors[0].key != color && ue().library().text_styles[0].key != text,
                  "new styles never reuse deleted styles' keys");
            check(ue().open("main_menu"), "reopen the styled screen");
            check(ue().screen().root.fills[0].color == d::Color{0x12, 0x34, 0x56, 255} &&
                      ue_node(ue_named("Название игры"))->text_style.size == 90,
                  "new unrelated styles do not recolour or resize existing screens");
            ue().select({ue_inst_});
            break;
        }
        case 17: {
            const auto box = ue().layer_box(ue_inst_);
            check(box.has_value(), "the component copy is laid out for resizing");
            if (box) {
                const f32 width = ue_node(ue_inst_)->w;
                ue_drag(ue_wx(box->right()), ue_wy(box->cy()), ue_wx(box->right() + 80), ue_wy(box->cy()));
                const f32 resized = ue_node(ue_inst_)->w;
                check(resized > width + 40, "resizing a component copy with the mouse persists");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_node(ue_inst_)->w == width, "undo restores the copy's original width");
                ue().redo();
                check(ue_node(ue_inst_)->w == resized, "redo restores the copy's resized width");
                check(ue().open("main_menu") && ue_node(ue_inst_)->w == resized, "the copy's size survives reopening");
            }
            break;
        }
        case 18: {
            // The link to the game: a window the first button opens.
            ue_other_ = ue().new_screen();
            ue().select({});
            check(ue().screen().show == d::ScreenShow::Command, "a new screen is a window: it never covers the game by surprise");
            check(ue().screen().dim && ue().screen().root.fills.empty(), "a new window darkens what is under it by its switch, not by a fill");
            check(ue().set_property("screen.show", "playing") && !ue().covers_game(), "over the game it lets the game show");
            check(ue().set_property("fill.add", "") && ue().set_property("fill.0.color", "#1B2127") && ue().covers_game(),
                  "a solid screen over the game is pointed out");
            check(ue().set_property("screen.show", "command") && ue().set_property("screen.pauses", "") &&
                      ue().screen().show == d::ScreenShow::Command && ue().screen().pauses,
                  "a screen becomes a window that stops the game");
            check(ue_file(".html", ue_other_).find("forge-screen=\"command\" forge-fit=\"expand\"") != std::string::npos &&
                      ue_file(".html", ue_other_).find("forge-pauses=\"1\"") != std::string::npos,
                  "the window's page says so");
            check(ue().open("main_menu"), "back to the menu");
            ue().select({ue_named("Кнопка «Новая игра»")});
            check(click("ue-tab-game"), "the panel's «В игре» tab");
            break;
        }
        case 19: {
            check(shown("ue-click-add") && !shown("ue-fill-add"), "the tab shows what the layer does in the game");
            // The game's own menu may come without actions; a new one's button starts a game.
            while (!ue_node(ue_named("Кнопка «Новая игра»"))->on_click.empty())
                if (!ue().set_property("click.0.remove", "")) break;
            check(ue_node(ue_named("Кнопка «Новая игра»"))->on_click.empty(), "the button's actions go");
            check(click("ue-click-add"), "an action is added");
            const d::Node* exit = ue_node(ue_named("Кнопка «Новая игра»"));
            check(exit && exit->on_click.size() == 1 && exit->on_click[0].kind == d::ActionKind::Show &&
                      exit->on_click[0].target == ue_other_,
                  "a new action opens the other screen");
            check(ue().set_property("click.add", "") && ue().set_property("click.1.kind", "change") &&
                      ue().set_property("click.1.target", "inv.coins += 5"),
                  "a second action changes the coins");
            ue().select({ue_named("Название игры")});
            check(ue().set_property("text.insert", "inv.coins") && ue_node(ue_named("Название игры"))->text == "Старая шахта {inv.coins}",
                  "a value of the game goes into the text");
            ue_bar_ = ue().add_layer(d::NodeType::Rectangle, 100, 950, 400, 30);
            check(ue().set_property("bar.add", "") && ue().set_property("show_if", "inv.coins > 7") &&
                      ue_node(ue_bar_)->bar.value == "hero.hearts",
                  "a rectangle becomes a bar shown with enough coins");
            const std::string html = ue_file(".html");
            check(html.find("forge-click=\"[[&quot;show&quot;,&quot;" + ue_other_ + "&quot;],[&quot;change&quot;,&quot;inv.coins += 5&quot;]]\"") !=
                          std::string::npos &&
                      html.find("forge-text=\"Старая шахта {inv.coins}\"") != std::string::npos &&
                      html.find("forge-bar-value=\"hero.hearts\"") != std::string::npos &&
                      html.find("forge-show-if=\"inv.coins &gt; 7\"") != std::string::npos,
                  "the page carries the clicks, the text, the bar and the condition");
            ue().select({});
            check(click("ue-check"), "«Проверить»");
            break;
        }
        case 20: {
            check(ue().checking() && shown("ue-check-var-0"), "the check shows the values the screen reads");
            Rml::ElementDocument* page = ue().page();
            Rml::Element* title_el = page ? page->GetElementById(Rml::String("n") + std::to_string(title)) : nullptr;
            check(title_el && title_el->GetInnerRML() == "Старая шахта 5", "the title shows the coins");
            Rml::Element* bar = page ? page->GetElementById(Rml::String("n") + std::to_string(ue_bar_)) : nullptr;
            check(bar && !bar->IsVisible(true), "the bar waits for enough coins");
            const auto box = ue().layer_box(ue_named("Кнопка «Новая игра»"));
            check(box.has_value(), "the button is on the canvas");
            if (box) left_click(ue_wx(box->cx()), ue_wy(box->cy()));
            break;
        }
        case 21: {
            check(ue().check_vars().get("inv.coins").number() == 10, "the click changed the coins");
            check(ue().opened() == "main_menu" && ue().check_windows() == std::vector<std::string>{ue_other_},
                  "and opened the window over the menu, as the game does");
            check(ue().check_log().size() == 2, "the panel lists what the button did");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(ue().checking() && ue().check_windows().empty(), "Esc closes the window first");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!ue().checking(), "Esc again ends the check");
            check(ue().open("main_menu"), "back to the menu");
            // Movement: the title floats, the button eases into its looks, the window rises.
            ue().select({ue_named("Название игры")});
            check(click("ue-tab-motion"), "the panel's «Движение» tab");
            break;
        }
        case 22: {
            check(shown("ue-motion-kind") && !shown("ue-motion-duration"), "a layer stands still at first");
            check(ue().set_property("motion.kind", "float") && ue().set_property("motion.duration", "0.4") &&
                      ue_node(title)->motion.kind == d::MotionKind::Float,
                  "the title floats");
            check(ue().set_property("motion.kind", "custom") && ue_node(title)->motion.keys.size() == 3 &&
                      ue_node(title)->motion.keys[1].y < 0,
                  "its own keys start from the float");
            check(ue().set_property("motion.key.1.color.add", "") && ue().set_property("motion.key.1.color", "#FF0000") &&
                      ue_node(title)->motion.keys[1].tint,
                  "a key changes the colour too, like the web");
            check(ue_file(".html").find("@keyframes m" + std::to_string(title)) != std::string::npos, "the game's page moves it");
            ue().select({ue_named("Кнопка «Новая игра»")});
            check(ue().set_property("smooth", "0.15") && ue_file(".html").find("transition: all 0.15s") != std::string::npos,
                  "the button's looks change smoothly");
            check(ue().open(ue_other_), "the window");
            ue().select({});
            check(ue().set_property("screen.appear", "rise") && ue_file(".html", ue_other_).find("forge-appear=\"rise\"") != std::string::npos,
                  "the window rises when it opens");
            check(ue().open("main_menu"), "back to the menu");
            ue().set_checking(true);
            break;
        }
        case 23: {
            Rml::ElementDocument* page = ue().page();
            Rml::Element* t = page ? page->GetElementById(Rml::String("n") + std::to_string(title)) : nullptr;
            const Rml::Property* p = t ? t->GetProperty("transform") : nullptr;
            ue_moved_ = p ? p->ToString() : std::string();
            check(t && !ue_moved_.empty(), "in the check the title moves");
            SDL_Delay(120);
            break;
        }
        case 24: {
            Rml::ElementDocument* page = ue().page();
            Rml::Element* t = page ? page->GetElementById(Rml::String("n") + std::to_string(title)) : nullptr;
            const Rml::Property* p = t ? t->GetProperty("transform") : nullptr;
            check(p && p->ToString() != ue_moved_, "and keeps moving");
            ue().set_checking(false);
            ue().set_panel("design");
            break;
        }
        case 25: {
            // A list: a component's copy becomes the cell of the hero's things.
            const u32 cell = ue().place_component("Кнопка меню");
            check(cell != 0, "a copy of the button for the cell");
            ue().select({cell});
            ue().set_panel("game");
            check(ue().set_property("list", "items"), "«Список: Вещи героя» on the copy");
            const u32 list = ue().selection().empty() ? 0 : ue().selection()[0];
            const d::Node* l = ue_node(list);
            check(l && l->list == d::ListSource::Items && l->children.size() == 2 && l->children[0].id == cell &&
                      !l->children[0].component.empty(),
                  "a list frame stands around the copy, with an «empty» text after it");
            ue().undo();
            check(!ue_node(list) && ue_node(cell) && d::path_to(ue().screen().root, cell).size() == 2,
                  "Ctrl+Z takes the list away in one step");
            ue().redo();
            check(ue_node(list) && ue_node(list)->list == d::ListSource::Items, "and Ctrl+Y brings it back");
            ue_list_ = list;
            // The cell's text shows the element's name and count.
            const d::Node* inst = ue_node(cell);
            u32 text = 0;
            if (inst)
                for (const d::Node& c : inst->children)
                    if (c.type == d::NodeType::Text) text = c.id;
            check(text != 0, "the copy has a text");
            ue_text_ = text;
            ue_cell_ = cell;
            ue().select({text});
            break;
        }
        case 26: {
            const u32 text = ue_text_, cell = ue_cell_, list = ue_list_;
            check(shown("ue-picture-from"), "inside the cell the panel offers the element's picture");
            check(ue().set_property("text", "") && ue().set_property("text.insert", "item.name") &&
                      ue().set_property("text.insert", "item.count"),
                  "the element's name and count go into the cell's text");
            check(ue_node(text) && ue_node(text)->text == "{item.name} {item.count}", "the cell's text has the element's name and count");
            ue().select({list});
            check(ue().set_property("list_gap", "6") && ue_node(list)->list_gap == 6, "the space between cells");
            const std::string html = ue_file(".html");
            check(html.find("forge-list=\"items\" forge-list-gap=\"6\"") != std::string::npos &&
                      html.find("forge-text=\"{item.name} {item.count}\"") != std::string::npos,
                  "the game's page carries the list and the cell's text");
            // Saved: it is there when the screen opens again.
            check(ue().open(ue_other_) && ue().open("main_menu"), "the screen opens again");
            const d::Node* again = ue_node(list);
            check(again && again->list == d::ListSource::Items && again->list_gap == 6 && !again->children.empty() &&
                      again->children[0].id == cell && ue_node(text) && ue_node(text)->text == "{item.name} {item.count}",
                  "the list, its cell and the cell's text are kept");
            // «Картинка предмета» on the cell's text: the coins get a picture
            // of the game's (a sample under pictures/интерфейс), the rest have none.
            ue().select({text});
            check(ue().set_property("picture_from", "item.icon") && ue_node(text)->picture_from == "item.icon",
                  "the cell's text shows the element's picture");
            check(ue_file(".html").find("forge-picture=\"item.icon\"") != std::string::npos, "the game's page carries it");
            objects::Library& lib = *ed_.level_module.library();
            ue_pictures_ = lib.pictures_folder();
            lib.set_pictures_folder(ed_.ui_game_dir / "pictures");
            const objects::Template* coins = nullptr; // the first pickup of coins: the list's name for them
            for (const objects::Template& t : lib.templates())
                if (!coins && lib.item_of(t) == "coins") coins = &t;
            check(coins && std::filesystem::exists(ed_.ui_game_dir / utf8_path("pictures/интерфейс/камень.png")), "a picture to give");
            if (coins) {
                ue_coins_ = *coins;
                objects::Template with = *coins;
                with.picture = "интерфейс/камень.png";
                check(lib.put(with), "the coins get the picture");
            }
            ue().set_checking(true);
            break;
        }
        case 27: {
            // «Проверить»: a cell for every thing the game has, showing it.
            Rml::ElementDocument* page = ue().page();
            Rml::Element* box = page ? page->GetElementById(Rml::String("n") + std::to_string(ue_list_)) : nullptr;
            Rml::Element* content = box ? box->GetFirstChild() : nullptr;
            usize items = 0;
            for (const auto& [name, value] : ue().check_vars().all())
                if (name.rfind("inv.", 0) == 0 && value.number() > 0) ++items;
            check(items > 0 && ue_list_cells(content) == items,
                  "every thing the hero carries has a cell");
            ue_items_ = items;
            // item.icon: a cell shows its thing's picture (the coins' now), or none.
            const std::vector<game::ScreenItem> things = ed_.ui_tab.game_items();
            int right = 0, with = 0, without = 0;
            for (int i = 0; content && i < content->GetNumChildren(); ++i) {
                Rml::Element* c = content->GetChild(i);
                if (c->GetComputedValues().display() == Rml::Style::Display::None) continue;
                Rml::ElementList pics;
                c->QuerySelectorAll(pics, "[forge-picture]");
                if (pics.empty()) continue;
                const Rml::Property* p = pics[0]->GetLocalProperty("decorator");
                const std::string look = p ? p->ToString() : std::string("none");
                const std::string text = pics[0]->GetInnerRML();
                for (const game::ScreenItem& t : things) {
                    // "{item.name} {item.count}": the name, a space, digits.
                    if (text.size() <= t.name.size() + 1 || text.compare(0, t.name.size() + 1, t.name + " ") != 0 ||
                        text.find_first_not_of("0123456789", t.name.size() + 1) != std::string::npos)
                        continue;
                    const bool ok = t.picture.empty() ? look == "none" : look.find("../" + t.picture) != std::string::npos;
                    if (!ok) FORGE_ERROR("«%s» (%s) shows %s", t.name.c_str(), t.picture.c_str(), look.c_str());
                    right += ok;
                    (t.picture.empty() ? without : with) += 1;
                    break;
                }
            }
            const auto coins = std::find_if(things.begin(), things.end(), [](const game::ScreenItem& t) { return t.id == "coins"; });
            check(coins != things.end() && coins->picture == "pictures/интерфейс/камень.png",
                  "the game's coins have the picture as a path in the game's folder");
            check(right == static_cast<int>(items) && with > 0 && without > 0,
                  "every cell shows its thing's picture, or none without one");
            for (const auto& [name, value] : ue().check_vars().all())
                if (name.rfind("inv.", 0) == 0 && value.number() > 0) {
                    ue().check_vars().set(name, 0);
                    break;
                }
            break;
        }
        case 28: {
            Rml::ElementDocument* page = ue().page();
            Rml::Element* box = page ? page->GetElementById(Rml::String("n") + std::to_string(ue_list_)) : nullptr;
            Rml::Element* content = box ? box->GetFirstChild() : nullptr;
            check(ue_list_cells(content) + 1 == ue_items_, "a thing set to 0 leaves the list");
            std::vector<std::string> names;
            for (const auto& [name, value] : ue().check_vars().all())
                if (name.rfind("inv.", 0) == 0) names.push_back(name);
            for (const std::string& n : names) ue().check_vars().set(n, 0);
            break;
        }
        case 29: {
            Rml::ElementDocument* page = ue().page();
            Rml::Element* box = page ? page->GetElementById(Rml::String("n") + std::to_string(ue_list_)) : nullptr;
            Rml::Element* content = box ? box->GetFirstChild() : nullptr;
            check(content && ue_list_cells(content) == 0, "with nothing left, no cells");
            const d::Node* l = ue_node(ue_list_);
            Rml::Element* empty = l && l->children.size() > 1 && page
                                      ? page->GetElementById(Rml::String("n") + std::to_string(l->children[1].id))
                                      : nullptr;
            check(empty && empty->GetComputedValues().visibility() == Rml::Style::Visibility::Visible, "and the «empty» text shows");
            ue().set_checking(false);
            objects::Library& lib = *ed_.level_module.library();
            if (!ue_coins_.id.empty()) check(lib.put(ue_coins_), "the coins' picture goes back");
            lib.set_pictures_folder(ue_pictures_);
            ue().set_panel("design");
            break;
        }
        // --- the player's screen sizes (13.2) ---
        case 30: {
            check(ue().view() == 0 && !ue().previewing() && !shown("ue-view-note"), "the canvas starts at the screen's own size");
            // A layer stuck to the bottom right corner and one to the middle.
            ue_edge_ = ue().add_layer(d::NodeType::Rectangle, 1720, 980, 160, 60);
            check(ue().set_property("horizontal", "end") && ue().set_property("vertical", "end"), "a layer stuck to the corner");
            ue_mid_ = ue().add_layer(d::NodeType::Rectangle, 900, 20, 120, 60);
            check(ue().set_property("horizontal", "center"), "a layer stuck to the middle");
            check(ue().set_property("click.add", "") && ue().set_property("click.0.kind", "change") &&
                      ue().set_property("click.0.target", "inv.coins += 5"),
                  "its click adds coins");
            // A click on a cell of the list says which thing it shows.
            // The cell is a copy of the menu's «Настройки» button and came with
            // its «Настройки» action: the cell does one thing, so that one goes.
            ue().select({ue_cell_});
            while (!ue_node(ue_cell_)->on_click.empty())
                if (!ue().set_property("click.0.remove", "")) break;
            check(ue_node(ue_cell_)->on_click.empty(), "the cell's copied actions go");
            check(ue().set_property("click.add", "") && ue_node(ue_cell_)->on_click.size() == 1, "the cell gets an action");
            check(ue().set_property("click.0.kind", "message") && ue().set_property("click.0.target", "взял {item.id}"),
                  "the cell's click tells the logic which thing");
            check(ue_file(".html").find("forge-click=\"[[&quot;message&quot;,&quot;взял {item.id}&quot;]]\"") != std::string::npos,
                  "the game's page gives the cell just that action");
            ue().select({});
            break;
        }
        case 31: {
            // Laid out: where they stand on «Макет».
            ue_json_ = d::save_screen(ue().screen());
            ue_edge_box_ = ue().layer_box(ue_edge_).value_or(d::Rect{});
            ue_mid_box_ = ue().layer_box(ue_mid_).value_or(d::Rect{});
            check(ue_edge_box_.x == 1720 && ue_edge_box_.y == 980 && ue_mid_box_.x == 900 && ue_mid_box_.y == 20, "they stand where they were drawn");
            check(shown("ue-view-4") && click("ue-view-4"), "the switch over the canvas: 21:9");
            break;
        }
        case 32: {
            // 21:9 (2560×1080) against a 16:9 screen that grows: 640 wider, the same scale.
            check(ue().view() == 4 && ue().previewing(), "21:9 is chosen");
            check(d::save_screen(ue().screen()) == ue_json_, "choosing a size changes nothing of the screen");
            Rml::ElementDocument* page = ue().page();
            const Rml::Vector2i dims = page ? page->GetContext()->GetDimensions() : Rml::Vector2i();
            check(dims.x == 2560 && dims.y == 1080, ("the page is laid out on a 2560×1080 screen: " + std::to_string(dims.x) + "×" +
                                                          std::to_string(dims.y)).c_str());
            const auto edge = ue().layer_box(ue_edge_), mid = ue().layer_box(ue_mid_);
            check(edge && edge->x == ue_edge_box_.x + 640 && edge->y == ue_edge_box_.y, "the corner layer follows the right edge");
            check(mid && mid->x == ue_mid_box_.x + 320, "the middle layer stays in the middle");
            check(ue().layer_box(ue().screen().root.id)->w == 2560, "the screen is 2560 wide");
            Rml::Element* img = ed_.find_element("ue-screen");
            const Rml::Vector2f size = img ? img->GetBox().GetSize() : Rml::Vector2f();
            check(size.y > 0 && std::fabs(size.x / size.y - 2560.0f / 1080.0f) < 0.01f, "the canvas shows 21:9 proportions");
            check(shown("ue-view-note") && ue().view_note().find("2560×1080") != std::string::npos, ("and says which screen: " + ue().view_note()).c_str());
            // A click picks the layer; a drag does not move it (layers move in «Макет»).
            const f32 cx = ue_wx(edge ? edge->cx() : 0), cy = ue_wy(edge ? edge->cy() : 0);
            left_click(cx, cy);
            check(ue().selection().size() == 1 && ue().selection()[0] == ue_edge_, "a click on the canvas picks the layer where it shows");
            ue_drag(cx, cy, cx + 120, cy + 60);
            check(d::save_screen(ue().screen()) == ue_json_, "a drag on another size does not move it");
            // Stretched, a 16:9 screen on 21:9 is drawn wider, its layout stays 1920.
            ue().select({});
            check(ue().set_property("screen.fit", "stretch"), "the screen is stretched");
            break;
        }
        case 33: {
            Rml::Element* img = ed_.find_element("ue-screen");
            const Rml::Vector2f size = img ? img->GetBox().GetSize() : Rml::Vector2f();
            check(size.y > 0 && std::fabs(size.x / size.y - 2560.0f / 1080.0f) < 0.01f, "stretched, the canvas is still 21:9");
            const auto edge = ue().layer_box(ue_edge_);
            check(edge && edge->x == ue_edge_box_.x && ue().layer_box(ue().screen().root.id)->w == 1920,
                  "stretched, the layout keeps the drawn size");
            const game::ScreenFit f = ue().view_fit();
            check(std::fabs(f.sx - 2560.0f / 1920.0f) < 1e-4f && f.sy == 1, "and its sides are scaled apart");
            ue().undo();
            check(ue().screen().fit == d::ScreenFit::Expand && d::save_screen(ue().screen()) == ue_json_, "Ctrl+Z brings the growing back");
            check(click("ue-view-5"), "4:3");
            break;
        }
        case 34: {
            // 4:3 (1440×1080): three quarters the size, 360 page pixels taller.
            Rml::ElementDocument* page = ue().page();
            const Rml::Vector2i dims = page ? page->GetContext()->GetDimensions() : Rml::Vector2i();
            check(dims.x == 1440 && dims.y == 1080, "the page is laid out on a 1440×1080 screen");
            const game::ScreenFit f = ue().view_fit();
            check(std::fabs(f.sx - 0.75f) < 1e-4f && f.sx == f.sy && std::fabs(f.height - 1440) < 0.01f, "one scale, 0.75");
            const auto edge = ue().layer_box(ue_edge_), mid = ue().layer_box(ue_mid_);
            check(edge && edge->x == ue_edge_box_.x && edge->y == ue_edge_box_.y + 360, "the corner layer follows the bottom edge");
            check(mid && mid->x == ue_mid_box_.x && mid->y == ue_mid_box_.y, "the middle one stays");
            Rml::Element* img = ed_.find_element("ue-screen");
            const Rml::Vector2f size = img ? img->GetBox().GetSize() : Rml::Vector2f();
            check(size.y > 0 && std::fabs(size.x / size.y - 4.0f / 3.0f) < 0.01f, "the canvas shows 4:3 proportions");
            ue().select({});
            check(ue().set_property("screen.fit", "fit"), "whole, with bars");
            break;
        }
        case 35: {
            // Whole on 4:3: bars of 135 above and below, the layout as drawn.
            Rml::ElementDocument* page = ue().page();
            Rml::Element* root = page ? page->GetElementById("n" + std::to_string(ue().screen().root.id)) : nullptr;
            const Rml::Vector2f at = root ? root->GetAbsoluteOffset(Rml::BoxArea::Border) : Rml::Vector2f(-1, -1);
            check(std::fabs(at.x) < 0.01f && std::fabs(at.y - 135) < 0.01f, "the page sits between bars of 135");
            const auto edge = ue().layer_box(ue_edge_);
            check(edge && edge->x == ue_edge_box_.x && edge->y == ue_edge_box_.y, "and its layers stand as drawn");
            ue().undo();
            check(d::save_screen(ue().screen()) == ue_json_, "Ctrl+Z: growing again");
            check(click("ue-check"), "«Проверить» on 4:3");
            break;
        }
        case 36: {
            check(ue().checking() && ue().view() == 5, "the check runs on the chosen screen");
            Rml::ElementDocument* page = ue().page();
            const Rml::Vector2i dims = page ? page->GetContext()->GetDimensions() : Rml::Vector2i();
            check(dims.x == 1440 && dims.y == 1080, "on 1440×1080");
            ue_coins_before_ = ue().check_vars().get("inv.coins").number();
            // The middle layer where it shows: the click goes through the 0.75 scale to the page.
            const auto box = ue().layer_box(ue_mid_);
            check(box.has_value(), "the layer is on the canvas");
            if (box) left_click(ue_wx(box->cx()), ue_wy(box->cy()));
            break;
        }
        case 37: {
            check(ue().check_vars().get("inv.coins").number() == ue_coins_before_ + 5, "the click on 4:3 hit the layer where it shows");
            break;
        }
        case 38: {
            // The wheel over the list, as the player turns it: on every size,
            // growing, whole with bars and stretched where the sizes differ.
            static const std::pair<int, const char*> kRuns[] = {{5, "expand"}, {5, "fit"},    {5, "stretch"}, {4, "expand"}, {4, "fit"},
                                                                {4, "stretch"}, {1, "expand"}, {2, "expand"},  {3, "expand"}};
            if (ue_wheel_ >= static_cast<int>(std::size(kRuns))) break;
            const auto [view, fit] = kRuns[ue_wheel_];
            const std::string run = std::string(editor_app::UiEditor::view_sizes()[static_cast<usize>(view)].label) + " " + fit + ": ";
            Rml::ElementDocument* page = ue().page();
            Rml::Element* box = page ? page->GetElementById(Rml::String("n") + std::to_string(ue_list_)) : nullptr;
            Rml::Element* content = box ? box->GetFirstChild() : nullptr;
            const game::ScreenFit f = ue().view_fit();
            // The thing a cell shows, by its text.
            auto thing_of = [&](Rml::Element* c) {
                Rml::ElementList texts;
                c->QuerySelectorAll(texts, "[forge-text]");
                const std::string text = texts.empty() ? std::string() : texts[0]->GetInnerRML();
                for (const game::ScreenItem& t : ed_.ui_tab.game_items())
                    if (text.rfind(t.name + " ", 0) == 0 && text.find_first_not_of("0123456789", t.name.size() + 1) == std::string::npos)
                        return t.id;
                return std::string();
            };
            // Whether the page's topmost element at a page point is (inside) this one.
            auto on_top = [&](Rml::Element* el, f32 px, f32 py) {
                for (Rml::Element* e = page->GetContext()->GetElementAtPoint({game::fit_to_view_x(f, px), game::fit_to_view_y(f, py)}); e;
                     e = e->GetParentNode())
                    if (e == el) return true;
                return false;
            };
            // The cells (wholly in sight, or all), with their boxes in page pixels.
            struct Seen {
                Rml::Element* cell;
                UeSpot spot;
            };
            auto cells = [&](bool whole) {
                std::vector<Seen> out;
                const Rml::Vector2f top = box->GetAbsoluteOffset(Rml::BoxArea::Padding);
                for (int i = 0; content && i < content->GetNumChildren(); ++i) {
                    Rml::Element* c = content->GetChild(i);
                    if (c->GetComputedValues().display() == Rml::Style::Display::None) continue;
                    const Rml::Vector2f at = c->GetAbsoluteOffset(Rml::BoxArea::Border), size = c->GetBox().GetSize(Rml::BoxArea::Border);
                    if (whole && (at.y < top.y - 0.5f || at.y + size.y > top.y + box->GetClientHeight() + 0.5f)) continue;
                    const std::string id = thing_of(c);
                    const f32 x = at.x - f.left, y = at.y - f.top;
                    if (!id.empty()) out.push_back({c, {id, x, y, x + size.x, y + size.y}});
                }
                return out;
            };
            auto wheel = [&](f32 x, f32 y) {
                SDL_Event e{};
                e.type = SDL_EVENT_MOUSE_WHEEL;
                e.wheel.x = x;
                e.wheel.y = y;
                ed_.handle_event(e);
            };
            switch (ue_wheel_phase_) {
            case 0: // the size and fit, then «Проверить» with more things than the list shows
                ue().set_checking(false);
                ue().select({});
                if (std::string(fit) != "expand") check(ue().set_property("screen.fit", fit), (run + "the fit is set").c_str());
                check(click("ue-view-" + std::to_string(view)) && ue().view() == view, (run + "the size is chosen").c_str());
                ue().set_checking(true);
                for (const game::ScreenItem& t : ed_.ui_tab.game_items()) ue().check_vars().set("inv." + t.id, 1);
                ue_wheel_phase_ = 1;
                return true;
            case 1: {
                check(box && box->GetScrollHeight() > box->GetClientHeight() + 1 && box->GetScrollTop() == 0,
                      (run + "the list scrolls, from the top").c_str());
                if (!box) break;
                ue_seen_.clear();
                for (const Seen& c : cells(false)) ue_seen_.push_back(c.spot);
                // A point of the list no other layer covers.
                const Rml::Vector2f at = box->GetAbsoluteOffset(Rml::BoxArea::Padding);
                f32 px = -1, py = -1;
                for (int j = 1; j < 8 && px < 0; ++j)
                    for (int i = 1; i < 8 && px < 0; ++i) {
                        const f32 x = at.x - f.left + box->GetClientWidth() * static_cast<f32>(i) / 8;
                        const f32 y = at.y - f.top + box->GetClientHeight() * static_cast<f32>(j) / 8;
                        if (on_top(box, x, y)) px = x, py = y;
                    }
                check(px >= 0, (run + "a point of the list in sight").c_str());
                ue_pan_x_ = ue().to_canvas_x(0);
                ue_pan_y_ = ue().to_canvas_y(0);
                ue_wheel_x_ = ue_wx(px);
                ue_wheel_y_ = ue_wy(py);
                mouse(SDL_EVENT_MOUSE_MOTION, ue_wheel_x_, ue_wheel_y_);
                wheel(1, 0);  // sideways: the list has nowhere to go, and the canvas must not move
                wheel(0, -1); // one notch down
                check(ue().to_canvas_x(0) == ue_pan_x_ && ue().to_canvas_y(0) == ue_pan_y_,
                      (run + "the wheel does not move the canvas").c_str());
                ue_notches_ = 1;
                ue_wheel_top_ = -1;
                ue_wheel_start_ = ue_wheel_still_ = SDL_GetTicks();
                ue_wheel_phase_ = 2;
                return true;
            }
            case 2: {
                // The page scrolls smoothly, by the clock, not by frames: it has
                // stopped when its scroll has not changed for 300 real ms.
                const f32 top = box ? box->GetScrollTop() : 0;
                const u64 now = SDL_GetTicks();
                if (top != ue_wheel_top_) {
                    ue_wheel_top_ = top;
                    ue_wheel_still_ = now;
                }
                const bool timeout = now - ue_wheel_start_ > 8000;
                if (now - ue_wheel_still_ < 300 && !timeout) {
                    SDL_Delay(5);
                    return true;
                }
                const std::string state = "прокрутка " + std::to_string(top) + " px, щелчков колеса " + std::to_string(ue_notches_) +
                                          ", прошло " + std::to_string(now - ue_wheel_start_) + " мс";
                check(!timeout, (run + "the scroll comes to a stop: " + state).c_str());
                // A cell in sight where, before the wheel, another thing stood:
                // clicked where the canvas shows it now.
                ue_expect_.clear();
                f32 cx = 0, cy = 0;
                for (const Seen& c : cells(true)) {
                    const f32 x = (c.spot.x0 + c.spot.x1) * 0.5f, y = (c.spot.y0 + c.spot.y1) * 0.5f;
                    std::string before;
                    for (const UeSpot& s : ue_seen_)
                        if (x >= s.x0 && x < s.x1 && y >= s.y0 && y < s.y1) before = s.id;
                    if (before != c.spot.id && on_top(c.cell, x, y)) ue_expect_ = c.spot.id, cx = x, cy = y; // the lowest
                }
                // Not far enough for another thing to stand there: the wheel
                // turns one more notch, as a player would (up to 6).
                const bool bottom = box && box->GetScrollTop() + box->GetClientHeight() >= box->GetScrollHeight() - 0.5f;
                if (ue_expect_.empty() && !timeout && !bottom && ue_notches_ < 6) {
                    mouse(SDL_EVENT_MOUSE_MOTION, ue_wheel_x_, ue_wheel_y_);
                    wheel(0, -1);
                    ++ue_notches_;
                    ue_wheel_still_ = now;
                    return true;
                }
                check(top > 0, (run + "the wheel scrolled the list: " + state).c_str());
                check(box && box->GetScrollLeft() == 0, (run + "and not sideways").c_str());
                check(ue().to_canvas_x(0) == ue_pan_x_ && ue().to_canvas_y(0) == ue_pan_y_, (run + "the canvas stood still").c_str());
                check(!ue_expect_.empty(), (run + "a cell in sight where another thing stood before the wheel: " + state).c_str());
                ue_log_size_ = ue().check_log().size();
                if (!ue_expect_.empty()) left_click(ue_wx(cx), ue_wy(cy));
                ue_wheel_phase_ = 3;
                return true;
            }
            default: {
                const std::vector<std::string>& log = ue().check_log();
                check(log.size() == ue_log_size_ + 1 && log.back() == "Сообщение «Логике»: взял " + ue_expect_,
                      (run + "the click after the wheel acts on its own thing: " + ue_log_since(ue_log_size_) + " (ждали " +
                       ue_expect_ + ")")
                          .c_str());
                check(ue().to_canvas_x(0) == ue_pan_x_ && ue().to_canvas_y(0) == ue_pan_y_, (run + "the canvas stood still").c_str());
                ue().set_checking(false);
                if (std::string(fit) != "expand") ue().undo();
                check(d::save_screen(ue().screen()) == ue_json_, (run + "the screen is as it was").c_str());
                ++ue_wheel_;
                ue_wheel_phase_ = 0;
                return true;
            }
            }
            break;
        }
        case 39: {
            // On «Макет» the wheel pans the canvas, as before.
            check(!ue().checking() && click("ue-view-0"), "back to «Макет»");
            const f32 y0 = ue().to_canvas_y(0);
            mouse(SDL_EVENT_MOUSE_MOTION, ue_wx(960), ue_wy(540));
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.y = -1;
            ed_.handle_event(e);
            check(ue().to_canvas_y(0) != y0, "on «Макет» the wheel pans the canvas");
            e.wheel.y = 1;
            ed_.handle_event(e);
            check(ue().to_canvas_y(0) == y0, "and back");
            break;
        }
        case 40: {
            check(ue().view() == 0 && !shown("ue-view-note"), "«Макет» again");
            Rml::ElementDocument* page = ue().page();
            const Rml::Vector2i dims = page ? page->GetContext()->GetDimensions() : Rml::Vector2i();
            check(dims.x == 1920 && dims.y == 1080, "the page at its own size");
            const auto edge = ue().layer_box(ue_edge_);
            check(edge && edge->x == ue_edge_box_.x && edge->y == ue_edge_box_.y, "the corner layer back where it was drawn");
            check(d::save_screen(ue().screen()) == ue_json_, "the screen as it was: layers, ids, copies' own values");
            // Saved and opened again: the same.
            check(ue().open(ue_other_) && ue().open("main_menu") && d::save_screen(ue().screen()) == ue_json_,
                  "saved and opened again, the same");
            // A real edit on «Макет» is undone and redone whole.
            ue().select({ue_edge_});
            check(ue().set_property("x", "1700") && ue_node(ue_edge_)->x == 1700, "the corner layer moved on «Макет»");
            check(click("ue-view-3"), "2560×1440");
            break;
        }
        case 41: {
            // 2560×1440: the same proportions, 4/3 the size: the layout is the drawn one.
            const game::ScreenFit f = ue().view_fit();
            check(std::fabs(f.sx - 4.0f / 3.0f) < 1e-4f && std::fabs(f.width - 1920) < 0.01f, "2560×1440 is the screen at 4/3");
            check(ue().layer_box(ue_edge_)->x == 1700, "the moved layer shows moved");
            ue().undo();
            check(ue_node(ue_edge_)->x == 1720 && d::save_screen(ue().screen()) == ue_json_, "Ctrl+Z on another size undoes the move");
            ue().redo();
            check(ue_node(ue_edge_)->x == 1700, "Ctrl+Y brings it back");
            ue().undo();
            check(click("ue-view-1"), "1280×720");
            break;
        }
        case 42: {
            const game::ScreenFit f = ue().view_fit();
            Rml::ElementDocument* page = ue().page();
            const Rml::Vector2i dims = page ? page->GetContext()->GetDimensions() : Rml::Vector2i();
            check(dims.x == 1280 && dims.y == 720 && std::fabs(f.sx - 2.0f / 3.0f) < 1e-4f, "1280×720: two thirds");
            const auto edge = ue().layer_box(ue_edge_);
            check(edge && edge->x == ue_edge_box_.x && edge->y == ue_edge_box_.y, "the layout is the drawn one");
            check(click("ue-view-0"), "back to «Макет»");
            break;
        }
        // --- «Простой / Полный» (13.3) ---
        case 43: {
            // The menu as the tests left it: copies of components, a list, movement, drawn art.
            check(!ue().simple() && shown("ue-mode-simple") && shown("ue-tool-frame"), "the switch shows «Полный» chosen");
            ue().select({});
            ue_json_ = d::save_screen(ue().screen());
            ue_disk_ = ue_file(".json");
            ue_cursor_ = ue().history().cursor();
            ue_entries_ = ue().history().size();
            check(click("ue-mode-simple") && ue().simple(), "«Простой» is chosen");
            check(d::save_screen(ue().screen()) == ue_json_ && ue_file(".json") == ue_disk_, "choosing it changes nothing of the screen");
            check(ue().history().cursor() == ue_cursor_ && ue().history().size() == ue_entries_, "and is no step of the history");
            break;
        }
        case 44: {
            check(!shown("ue-tool-frame") && !shown("ue-tool-ellipse") && shown("ue-block-button") && shown("ue-block-list"),
                  "the drawing tools give way to the blocks");
            check(shown("ue-simple") && shown("ue-s-show") && !shown("ue-tab-design"), "the screen in plain words: when it shows");
            // Every layer looked at in plain words: nothing changes.
            std::vector<u32> ids;
            auto visit = [&](auto&& self, const d::Node& n) -> void {
                ids.push_back(n.id);
                for (const d::Node& c : n.children) self(self, c);
            };
            visit(visit, ue().screen().root);
            for (u32 id : ids) ue().select({id});
            check(ids.size() > 10 && d::save_screen(ue().screen()) == ue_json_ && ue().history().cursor() == ue_cursor_,
                  ("every one of the " + std::to_string(ids.size()) + " layers seen in «Простой», nothing changed").c_str());
            // A layer with movement: what is not shown is named, kept and a click away.
            u32 moving = 0;
            for (u32 id : ids)
                if (const d::Node* n = ue_node(id); n && n->motion.kind != d::MotionKind::None && !moving) moving = id;
            check(moving != 0, "the menu has a moving layer");
            ue().select({moving});
            break;
        }
        case 45: {
            Rml::Element* hidden = ed_.find_element("ue-s-hidden");
            const std::string text = hidden ? std::string(hidden->GetInnerRML()) : std::string();
            check(shown("ue-s-hidden") && text.find("движение") != std::string::npos && shown("ue-s-to-full"),
                  ("the moving layer says its movement is in «Полный»: " + text).c_str());
            check(click("ue-s-to-full") && !ue().simple() && d::save_screen(ue().screen()) == ue_json_,
                  "its link opens «Полный», the screen as it was");
            check(click("ue-mode-simple") && ue().simple() && ue().history().cursor() == ue_cursor_, "and back to «Простой»");
            // The list and its cell's text.
            ue().select({ue_list_});
            break;
        }
        case 46: {
            check(shown("ue-s-list") && shown("ue-s-name"), "a list in plain words: what it lists");
            ue().select({ue_text_});
            break;
        }
        case 47: {
            check(shown("ue-s-picture-from") && shown("ue-s-text"), "the cell's text: its words and the thing's picture");
            // The new game button: drawn art and a frame picture the simple panel does not show.
            ue_simple_btn_ = ue_named("Кнопка «Новая игра»");
            ue().select({ue_simple_btn_});
            break;
        }
        case 48: {
            Rml::Element* word = ed_.find_element("ue-s-block");
            check(word && std::string(word->GetInnerRML()) == "Кнопка", "the menu's button reads as a button");
            Rml::Element* hidden = ed_.find_element("ue-s-hidden");
            const std::string text = hidden ? std::string(hidden->GetInnerRML()) : std::string();
            check(text.find("рисованная рамка") != std::string::npos && text.find("маска") != std::string::npos,
                  ("and names its frame picture and mask: " + text).c_str());
            // Typed into the label's field, as the author would.
            ue_json_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            check(ue_type("ue-s-text", "Играть"), "the label typed in «Простой»");
            break;
        }
        case 49: {
            const d::Node* button = ue_node(ue_simple_btn_);
            const d::Node* label = button ? d::block_label(*button) : nullptr;
            check(label && label->text == "Играть", "the button says «Играть»");
            check(ue().history().cursor() == ue_cursor_ + 1 && ue().history().undo_label() == "Изменено: Надпись",
                  ("one step of the history: " + ue().history().undo_label()).c_str());
            // Nothing else changed: the label put back gives the screen as it was.
            d::Screen back;
            check(d::load_screen(d::save_screen(ue().screen()), back), "the screen reads back");
            if (d::Node* b = d::find(back.root, ue_simple_btn_))
                if (d::Node* l = d::block_label(*b)) l->text = "Новая игра";
            check(d::save_screen(back) == ue_json_, "only the label changed: ids, art, copies, list, movement kept");
            ue_json2_ = d::save_screen(ue().screen());
            check(click("ue-mode-full") && !ue().simple(), "back to «Полный»");
            check(d::save_screen(ue().screen()) == ue_json2_ && ue().history().cursor() == ue_cursor_ + 1,
                  "the switch changes nothing and keeps the history");
            break;
        }
        case 50: {
            check(shown("ue-tab-design") && shown("ue-tool-frame") && !shown("ue-simple"), "«Полный» shows its panel and tools");
            ue().undo();
            check(d::save_screen(ue().screen()) == ue_json_, "Ctrl+Z after the switch takes back the simple change");
            ue().redo();
            check(d::save_screen(ue().screen()) == ue_json2_, "Ctrl+Y brings it again");
            check(click("ue-mode-simple") && ue().simple(), "«Простой» again");
            ue().undo();
            check(d::save_screen(ue().screen()) == ue_json_, "Ctrl+Z in «Простой»");
            ue().redo();
            check(d::save_screen(ue().screen()) == ue_json2_, "Ctrl+Y in «Простой»");
            check(ue().open(ue_other_) && ue().open("main_menu") && d::save_screen(ue().screen()) == ue_json2_,
                  "closed and opened again: the same, ids and all");
            // A screen of a beginner's own, from the blocks.
            check(click("ue-new-screen"), "a new screen");
            break;
        }
        case 51: {
            check(ue().simple() && ue().screen().root.children.empty(), "the new screen is empty and still «Простой»");
            ue_simple_screen_ = ue().opened();
            ue_blocks_.clear();
            static const std::pair<const char*, d::Block> blocks[] = {{"button", d::Block::Button}, {"text", d::Block::Text},
                                                                       {"bar", d::Block::Bar}, {"list", d::Block::List},
                                                                       {"picture", d::Block::Picture}};
            for (const auto& [key, block] : blocks) {
                const usize at = ue().history().cursor();
                check(click(std::string("ue-block-") + key), (std::string("a click on the block ") + key).c_str());
                const u32 id = ue().selection().size() == 1 ? ue().selection()[0] : 0;
                const d::Node* n = ue_node(id);
                check(n && d::block_of(*n) == block && ue().history().cursor() == at + 1 &&
                          ue().history().undo_label() == std::string("Добавлено: ") + d::block_word(block),
                      (std::string("one step adds a ") + d::block_word(block)).c_str());
                ue_blocks_.push_back(id);
            }
            // Each block where there is room, none over another.
            bool apart = true;
            for (usize i = 0; i < ue_blocks_.size(); ++i)
                for (usize j = i + 1; j < ue_blocks_.size(); ++j) {
                    const d::Node* a = ue_node(ue_blocks_[i]);
                    const d::Node* b = ue_node(ue_blocks_[j]);
                    if (!a || !b || (a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h)) apart = false;
                }
            check(apart, "the new blocks lie side by side");
            ue().select({ue_blocks_[0]});
            break;
        }
        case 52: {
            // Looking is no change: fields focused and left (and Enter) with what they show write nothing. The
            // colour shows the game colour's HEX and the size the label's: written back, they would cut the links.
            const d::Node* button = ue_node(ue_blocks_[0]);
            ue_json_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            ue_style_ = button && !button->fills.empty() ? button->fills[0].style : std::string();
            check(!ue_style_.empty(), "the new button wears the game colour " + ue_style_);
            for (const char* id : {"ue-s-color", "ue-s-size", "ue-s-text-color", "ue-s-text", "ue-s-name", "ue-s-x", "ue-s-w", "ue-s-h"}) {
                Rml::Element* e = ed_.find_element(id);
                check(e && shown(id), std::string("the field is shown: ") + id);
                if (!e) continue;
                e->Focus();
                e->Blur();
                e->Focus();
                key(SDLK_RETURN, SDL_KMOD_NONE);
                e->Blur();
                check(d::save_screen(ue().screen()) == ue_json_ && ue().history().cursor() == ue_cursor_,
                      std::string("focused and left, Enter on it: nothing written, no step: ") + id);
            }
            // The mode switched with the mouse while the colour field has the keyboard.
            if (Rml::Element* e = ed_.find_element("ue-s-color")) e->Focus();
            f32 x = 0, y = 0;
            check(element_center("ue-mode-full", x, y), "«Полный» is on the toolbar");
            left_click(x, y);
            break;
        }
        case 53: {
            const d::Node* button = ue_node(ue_blocks_[0]);
            check(!ue().simple() && d::save_screen(ue().screen()) == ue_json_ && ue().history().cursor() == ue_cursor_ && button &&
                      button->fills[0].style == ue_style_,
                  "switched with a field focused: the screen, its link to the game colour and the history as they were");
            check(click("ue-mode-simple") && ue().simple(), "«Простой» again");
            // The link still works: the game colour changed, the button follows.
            check(ue().open_library(), "the library");
            ue().select({});
            check(ue().set_property("color.0.color", "#225588"), "the game colour changes");
            check(ue().open(ue_simple_screen_), "back to the new screen");
            button = ue_node(ue_blocks_[0]);
            check(button && button->fills[0].style == ue_style_ && button->fills[0].color == d::Color{0x22, 0x55, 0x88, 255},
                  "the looked-at button follows the game colour");
            ue().select({ue_blocks_[0]});
            break;
        }
        case 54: {
            Rml::Element* word = ed_.find_element("ue-s-block");
            check(word && std::string(word->GetInnerRML()) == "Кнопка" && shown("ue-s-action") && !shown("ue-s-hidden"),
                  "the new button: its action, nothing hidden");
            ue_cursor_ = ue().history().cursor();
            const std::string untyped = d::save_screen(ue().screen());
            check(ue_type("ue-s-text", "Настройки"), "its label typed");
            check(ue().history().cursor() == ue_cursor_ + 1, "Enter keeps it: one step");
            ue().undo();
            check(d::save_screen(ue().screen()) == untyped && d::block_label(*ue_node(ue_blocks_[0]))->text == "Кнопка",
                  "one Ctrl+Z takes the whole typed label back");
            ue().redo();
            check(d::block_label(*ue_node(ue_blocks_[0]))->text == "Настройки" && ue().history().cursor() == ue_cursor_ + 1,
                  "Ctrl+Y brings it again");
            check(ue_open("ue-s-action"), "a click opens the action's list");
            break;
        }
        case 55:
            check(ue_option("ue-s-action", "settings") && ue_option_hovered_, "a click on «Настройки» in it, the pointer finds it");
            break;
        case 56: {
            // The panel cut short right under «Держится» (a test-only height), measured once the panel shows what the
            // action brought (its «Звук»): the drop-down's list then opens past the panel's bottom, where the panel
            // clips its own content but not the list.
            ue_panel_ = nullptr;
            if (Rml::Element* sel = ed_.find_element("ue-s-anchor-h"))
                for (Rml::Element* a = sel->GetParentNode(); a; a = a->GetParentNode()) {
                    const auto o = a->GetComputedValues().overflow_y();
                    if (o != Rml::Style::Overflow::Auto && o != Rml::Style::Overflow::Scroll) continue;
                    const f32 bottom = sel->GetAbsoluteOffset(Rml::BoxArea::Border).y + sel->GetBox().GetSize(Rml::BoxArea::Border).y;
                    const f32 top = a->GetAbsoluteOffset(Rml::BoxArea::Border).y;
                    a->SetProperty("max-height", std::to_string(static_cast<int>(bottom - top + a->GetScrollTop() + 4)) + "px");
                    ue_panel_ = a;
                    break;
                }
            check(ue_panel_ != nullptr, "the panel scrolls");
            const d::Node* button = ue_node(ue_blocks_[0]);
            check(button && button->on_click.size() == 1 && button->on_click[0].kind == d::ActionKind::Settings &&
                      button->on_click[0].target.empty() && d::block_label(*button)->text == "Настройки",
                  "the button says «Настройки» and opens the game's settings");
            check(ue().history().cursor() == ue_cursor_ + 2 && ue().history().undo_label() == "Изменено: При нажатии",
                  ("two changes, two steps: " + std::to_string(ue().history().cursor() - ue_cursor_) + ", last " +
                   ue().history().undo_label()).c_str());
            check(ue_open("ue-s-anchor-h"), "a click opens where it keeps");
            break;
        }
        case 57:
            check(ue_option("ue-s-anchor-h", "end") && ue_option_hovered_, "a click on «Справа», the pointer finds it");
            check(ue_option_outside_, "and «Справа» shows outside the scrolling panel it opened from");
            // Now cut short above «Место и размер»: the X field under the panel's bottom is clipped.
            if (Rml::Element* x = ed_.find_element("ue-s-x"); x && ue_panel_) {
                const f32 top = ue_panel_->GetAbsoluteOffset(Rml::BoxArea::Border).y;
                const f32 field = x->GetAbsoluteOffset(Rml::BoxArea::Border).y;
                ue_panel_->SetProperty("max-height", std::to_string(static_cast<int>(field - top + ue_panel_->GetScrollTop() - 8)) + "px");
            }
            break;
        case 58: {
            // What the panel clips takes no pointer.
            if (Rml::Element* x = ed_.find_element("ue-s-x"); x && ue_panel_) {
                const Rml::Vector2f p = x->GetAbsoluteOffset(Rml::BoxArea::Border) + x->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
                const f32 bottom = ue_panel_->GetAbsoluteOffset(Rml::BoxArea::Border).y + ue_panel_->GetBox().GetSize(Rml::BoxArea::Border).y;
                mouse(SDL_EVENT_MOUSE_MOTION, p.x, p.y);
                Rml::Element* hover = ed_.context() ? ed_.context()->GetHoverElement() : nullptr;
                bool on_x = false;
                for (Rml::Element* e = hover; e; e = e->GetParentNode()) on_x = on_x || e == x;
                check(p.y > bottom && !on_x, "the X field, cut off under the panel's bottom, takes no pointer");
            } else check(false, "the X field and the panel are there");
            if (ue_panel_) ue_panel_->RemoveProperty("max-height");
            break;
        }
        case 59: {
            check(ue_node(ue_blocks_[0])->horizontal == d::Constraint::End && ue().history().cursor() == ue_cursor_ + 3,
                  "it keeps to the right edge: one more step");
            check(ue_type("ue-s-color", "#3366cc") && ue_node(ue_blocks_[0])->fills[0].color == d::Color{0x33, 0x66, 0xcc, 255} &&
                      ue().history().cursor() == ue_cursor_ + 4,
                  "its colour typed: one more step");
            ue().select({ue_blocks_[1]});
            break;
        }
        case 60: {
            check(ue_type("ue-s-text", "Монеты: {inv.coins}") && ue_node(ue_blocks_[1])->text == "Монеты: {inv.coins}",
                  "the text shows the coins");
            // Everything kept, on the page the game reads.
            const std::string html = ue_file(".html", ue_simple_screen_);
            check(html.find("forge-click=\"[[&quot;settings&quot;,&quot;&quot;]]\"") != std::string::npos &&
                      html.find("forge-bar-value=\"hero.hearts\"") != std::string::npos &&
                      html.find("forge-list=\"items\"") != std::string::npos && html.find("{inv.coins}") != std::string::npos,
                  "the game's page has the button, the bar, the list and the text");
            ue_json_ = d::save_screen(ue().screen());
            ue().undo();
            check(ue_node(ue_blocks_[1])->text == "Текст", "Ctrl+Z: the text as it was");
            ue().redo();
            check(d::save_screen(ue().screen()) == ue_json_, "Ctrl+Y: as typed");
            check(ue().open("main_menu") && ue().open(ue_simple_screen_) && d::save_screen(ue().screen()) == ue_json_,
                  "saved, opened again: the same");
            check(click("ue-check"), "«Проверить»");
            break;
        }
        case 61: {
            check(ue().checking(), "the beginner's screen comes alive");
            Rml::ElementDocument* page = ue().page();
            Rml::Element* list = page ? page->GetElementById("n" + std::to_string(ue_blocks_[3])) : nullptr;
            Rml::Element* content = list ? list->GetFirstChild() : nullptr;
            usize carried = 0;
            for (const auto& [name, value] : ue().check_vars().all())
                if (name.rfind("inv.", 0) == 0 && value.number() > 0) ++carried;
            check(content && ue_list_cells(content) == carried, ("the list has a cell for every thing carried: " +
                                                                 std::to_string(carried)).c_str());
            const auto box = ue().layer_box(ue_blocks_[0]);
            check(box.has_value(), "the button is on the canvas");
            ue_log_size_ = ue().check_log().size();
            if (box) left_click(ue_wx(box->cx()), ue_wy(box->cy()));
            break;
        }
        case 62: {
            const std::vector<std::string>& log = ue().check_log();
            check(log.size() == ue_log_size_ + 1 && log.back() == "Кнопка: Настройки",
                  ("the button pressed does what was picked: " + ue_log_since(ue_log_size_)).c_str());
            ue().set_checking(false);
            check(click("ue-mode-full") && !ue().simple() && d::save_screen(ue().screen()) == ue_json_,
                  "«Полный» shows the same screen");
            check(ue().open("main_menu"), "back to the menu");
            break;
        }
        // --- «Палитра цвета» (13.4): one picker for every colour, used with the mouse and keys ---
        case 63:
            check(!ue().simple() && ue().opened() == "main_menu", "the menu, in «Полный»");
            ue_cp_btn_ = ue_named("Кнопка «Новая игра»");
            ue().select({ue_cp_btn_});
            break;
        case 64: {
            const d::Node* b = ue_node(ue_cp_btn_);
            check(b && !b->fills.empty() && b->fills[0].kind == d::PaintKind::Solid, "the button has a colour fill");
            ue_json_ = d::save_screen(ue().screen());
            ue_disk_ = ue_file(".json");
            ue_cursor_ = ue().history().cursor();
            ue_entries_ = ue().history().size();
            ue_cp_color_ = b ? b->fills[0].color : d::Color{};
            check(ue_press("ue-sw-fill-0") && ue().picker_open() && ue().picker_field() == "fill.0.color",
                  "a click on the fill's swatch opens the picker on the fill");
            check(ue().picker_color() == ue_cp_color_, "it starts from the fill's colour, alpha too: " + d::color_hex(ue_cp_color_));
            check(d::save_screen(ue().screen()) == ue_json_ && ue_file(".json") == ue_disk_ && ue().history().cursor() == ue_cursor_,
                  "opening it changes nothing");
            break;
        }
        case 65: {
            check(shown("ue-picker") && shown("ue-cp-sv") && shown("ue-cp-hue") && shown("ue-cp-alpha") && shown("ue-cp-hex") &&
                      shown("ue-cp-alpha-field") && shown("ue-cp-dropper"),
                  "the picker shows its square, hue, alpha over the checkerboard, HEX, alpha % and eyedropper");
            std::string hex = d::color_hex(ue_cp_color_).substr(1);
            for (char& ch : hex) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            check(ue_field("ue-cp-hex") == hex, "the HEX field shows " + hex + ": " + ue_field("ue-cp-hex"));
            std::string why;
            const bool reachable = ue_picker_reachable(why);
            check(reachable, "the picker is inside the window, its controls under the pointer: " + why);
            // Looked at: focused, left, Enter, without typing.
            for (const char* id : {"ue-cp-hex", "ue-cp-alpha-field"}) {
                Rml::Element* e = ed_.find_element(id);
                if (!e) continue;
                e->Focus();
                e->Blur();
                e->Focus();
                key(SDLK_RETURN, SDL_KMOD_NONE);
                e->Blur();
            }
            check(ue().picker_open() && ue().picker_color() == ue_cp_color_ && d::save_screen(ue().screen()) == ue_json_,
                  "its fields focused, left and Enter without typing: nothing changes");
            check(ue_press("ue-cp-cancel") && !ue().picker_open(), "«Отмена» closes it");
            check(d::save_screen(ue().screen()) == ue_json_ && ue_file(".json") == ue_disk_ && ue().history().cursor() == ue_cursor_ &&
                      ue().history().size() == ue_entries_,
                  "opened and closed: the screen, its file and the history as they were");
            check(ue_press("ue-sw-fill-0") && ue().picker_open(), "opened again");
            break;
        }
        case 66: {
            check(ue_press("ue-cp-done") && !ue().picker_open(), "«Готово» without a change closes it");
            check(d::save_screen(ue().screen()) == ue_json_ && ue().history().cursor() == ue_cursor_ && ue().history().size() == ue_entries_,
                  "and makes no step of the history");
            check(ue_press("ue-sw-fill-0") && ue().picker_open(), "opened again");
            break;
        }
        case 67: {
            // Dragged on the square, the hue and the alpha: the canvas shows it, nothing is written or kept yet.
            bool quiet = true;
            auto still = [&] { quiet = quiet && ue().history().cursor() == ue_cursor_ && ue_file(".json") == ue_disk_; };
            check(ue_slide("ue-cp-sv", 0.2f, 0.7f, 0.8f, 0.1f, still), "the square dragged");
            const d::Hsva sv = d::color_to_hsva(ue().picker_color());
            check(std::abs(sv.s - 0.8f) < 0.03f && std::abs(sv.v - 0.9f) < 0.03f,
                  "saturation and brightness where the mouse let go: " + std::to_string(sv.s) + " " + std::to_string(sv.v));
            check(ue_slide("ue-cp-hue", 0.1f, 0.5f, 0.55f, 0.5f, still), "the hue dragged");
            const d::Hsva hue = d::color_to_hsva(ue().picker_color());
            check(std::abs(hue.h - 198) < 4 && std::abs(hue.s - 0.8f) < 0.03f, "the hue changes, the square's place kept: " + std::to_string(hue.h));
            check(ue_slide("ue-cp-alpha", 0.9f, 0.5f, 0.5f, 0.5f, still), "the alpha dragged");
            check(std::abs(ue().picker_color().a - 128) <= 3, "alpha in the middle: " + std::to_string(ue().picker_color().a));
            check(quiet, "while dragging: no step of the history, the file not written");
            const d::Node* b = ue_node(ue_cp_btn_);
            check(b && b->fills[0].color == ue().picker_color(), "the canvas shows the picked colour on the button");
            check(ue().selection() == std::vector<u32>{ue_cp_btn_}, "the button stays selected");
            ue_cp_color_ = ue().picker_color();
            check(ue_press("ue-cp-done") && !ue().picker_open(), "«Готово»");
            break;
        }
        case 68: {
            check(ue().history().cursor() == ue_cursor_ + 1 && ue().history().undo_label() == "Изменено: Заливка",
                  "one finished pick, one step: " + ue().history().undo_label());
            const d::Node* b = ue_node(ue_cp_btn_);
            check(b && b->fills[0].color == ue_cp_color_, "the fill has the picked colour, alpha too");
            d::Screen saved;
            check(d::load_screen(ue_file(".json"), saved) && d::find(saved.root, ue_cp_btn_) &&
                      d::find(saved.root, ue_cp_btn_)->fills[0].color == ue_cp_color_,
                  "written into the screen's file");
            // Nothing else changed: put the colour back and it is the screen as it was (ids, other fills, stroke, text).
            d::Screen back;
            check(d::load_screen(d::save_screen(ue().screen()), back), "the screen reads back");
            if (d::Node* n = d::find(back.root, ue_cp_btn_)) {
                d::Screen was;
                d::load_screen(ue_json_, was);
                n->fills[0].color = d::find(was.root, ue_cp_btn_)->fills[0].color;
            }
            check(d::save_screen(back) == ue_json_, "only the fill's colour changed");
            ue_json2_ = d::save_screen(ue().screen());
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == ue_json_, "Ctrl+Z takes the whole pick back");
            key(SDLK_Y, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == ue_json2_, "Ctrl+Y brings it again");
            ue_disk_ = ue_file(".json");
            ue_cursor_ = ue().history().cursor();
            check(ue_press("ue-sw-fill-0") && ue().picker_open(), "the picker again");
            break;
        }
        case 69: {
            // A HEX typed and Enter shows it; Esc puts everything back.
            check(ue_type("ue-cp-hex", "#3366cc") && ue_node(ue_cp_btn_)->fills[0].color == d::Color{0x33, 0x66, 0xcc, 255},
                  "#3366cc typed: the canvas shows it");
            check(ue_file(".json") == ue_disk_ && ue().history().cursor() == ue_cursor_, "not written, no step");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!ue().picker_open() && d::save_screen(ue().screen()) == ue_json2_ && ue_file(".json") == ue_disk_ &&
                      ue().history().cursor() == ue_cursor_,
                  "Esc: the colour as it was, nothing written, no step");
            check(ue_press("ue-sw-fill-0") && ue().picker_open(), "the picker again");
            break;
        }
        case 70: {
            // Esc while the HEX field has the keyboard, with a colour typed but not entered.
            check(ue_keys("ue-cp-hex", "ff0000"), "ff0000 typed, no Enter");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!ue().picker_open() && d::save_screen(ue().screen()) == ue_json2_ && ue().history().cursor() == ue_cursor_,
                  "Esc in the field: closed, nothing changed");
            check(ue_press("ue-sw-fill-0") && ue().picker_open(), "the picker again");
            break;
        }
        case 71:
            // A part of a colour, a typo: the document stays as it is, the field says why.
            check(ue_type("ue-cp-hex", "3366c"), "3366c typed");
            check(d::save_screen(ue().screen()) == ue_json2_ && ue().picker_color() == ue_cp_color_,
                  "an incomplete HEX changes nothing");
            break;
        case 72: {
            check(shown("ue-cp-hex-note"), "and the picker says what is missing");
            check(ue_type("ue-cp-hex", "zz66cc") && d::save_screen(ue().screen()) == ue_json2_, "a typo changes nothing");
            break;
        }
        case 73: {
            // (A frame later: the field shows the picker's HEX again after the typo.)
            const bool typed = ue_type("ue-cp-hex", "36c");
            check(typed && ue().picker_color() == d::Color{0x33, 0x66, 0xcc, 255},
                  "three digits: #3366CC: " + std::to_string(typed) + " " + d::color_hex(ue().picker_color()) + " field " + ue_field("ue-cp-hex"));
            check(ue_type("ue-cp-alpha-field", "50") && ue().picker_color() == d::Color{0x33, 0x66, 0xcc, 128}, "alpha 50 %");
            break;
        }
        case 74: {
            check(ue_field("ue-cp-hex") == "3366CC80", "the HEX shows the alpha too: " + ue_field("ue-cp-hex"));
            check(ue_press("ue-cp-done"), "«Готово»");
            check(!shown("ue-cp-hex-note") && ue_node(ue_cp_btn_)->fills[0].color == d::Color{0x33, 0x66, 0xcc, 128} &&
                      ue().history().cursor() == ue_cursor_ + 1,
                  "#3366CC at 50 %: one step");
            // A colour of the game's: the fill follows it.
            ue_cp_key_ = ue().library().colors.empty() ? std::string() : ue().library().colors[0].key;
            check(!ue_cp_key_.empty(), "the game has a colour");
            ue_cursor_ = ue().history().cursor();
            check(ue_press("ue-sw-fill-0") && ue().picker_open(), "the picker again");
            break;
        }
        case 75: {
            check(shown("ue-cp-theme-0"), "the game's colours are in the picker");
            check(ue_press("ue-cp-theme-0") && ue().picker_link() == ue_cp_key_ && ue_node(ue_cp_btn_)->fills[0].style == ue_cp_key_ &&
                      ue_node(ue_cp_btn_)->fills[0].color == ue().library().colors[0].color,
                  "a game colour picked: the fill follows it");
            check(ue_press("ue-cp-done") && ue().history().cursor() == ue_cursor_ + 1, "«Готово»: one step");
            ue_json2_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            break;
        }
        case 76: {
            // Looked at again, in the picker and in the panel's HEX field: the link stays.
            check(ue_press("ue-sw-fill-0") && ue().picker_link() == ue_cp_key_, "opened on a linked fill, the link shows");
            break;
        }
        case 77: {
            Rml::Element* hex = ed_.find_element("ue-cp-hex");
            if (hex) {
                hex->Focus();
                key(SDLK_RETURN, SDL_KMOD_NONE);
                hex->Blur();
            }
            check(ue_press("ue-cp-done") && d::save_screen(ue().screen()) == ue_json2_ && ue().history().cursor() == ue_cursor_,
                  "Enter on the shown HEX and «Готово»: link kept, no step");
            Rml::Element* field = ed_.find_element("ue-fill-hex-0");
            check(field && shown("ue-fill-hex-0"), "the panel's HEX field of the fill");
            if (field) {
                field->Focus();
                field->Blur();
                field->Focus();
                key(SDLK_RETURN, SDL_KMOD_NONE);
                field->Blur();
            }
            check(d::save_screen(ue().screen()) == ue_json2_ && ue_node(ue_cp_btn_)->fills[0].style == ue_cp_key_ &&
                      ue().history().cursor() == ue_cursor_,
                  "the panel's HEX field focused, left, Enter: the link to the game colour kept, no step");
            check(ue_press("ue-sw-fill-0"), "the picker again");
            break;
        }
        case 78:
            // A colour of one's own dragged: the link goes in the preview only; Esc brings it back.
            check(ue_slide("ue-cp-sv", 0.5f, 0.5f, 0.3f, 0.2f) && ue().picker_link().empty() && ue_node(ue_cp_btn_)->fills[0].style.empty(),
                  "dragged: its own colour on the canvas");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!ue().picker_open() && d::save_screen(ue().screen()) == ue_json2_ && ue_node(ue_cp_btn_)->fills[0].style == ue_cp_key_ &&
                      ue().history().cursor() == ue_cursor_,
                  "Esc: the link to the game colour is back, no step");
            check(ue_press("ue-sw-fill-0"), "the picker again");
            break;
        case 79: {
            check(ue_type("ue-cp-hex", "#20C060") && ue_press("ue-cp-done"), "#20C060 typed, «Готово»");
            const d::Node* b = ue_node(ue_cp_btn_);
            check(b && b->fills[0].style.empty() && b->fills[0].color == d::Color{0x20, 0xc0, 0x60, 255} &&
                      ue().history().cursor() == ue_cursor_ + 1,
                  "its own colour now: the link gone, one step");
            d::Screen back;
            d::load_screen(d::save_screen(ue().screen()), back);
            d::Screen was;
            d::load_screen(ue_json2_, was);
            d::find(back.root, ue_cp_btn_)->fills[0] = d::find(was.root, ue_cp_btn_)->fills[0];
            check(d::save_screen(back) == ue_json2_, "only this fill changed");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == ue_json2_ && ue_node(ue_cp_btn_)->fills[0].style == ue_cp_key_,
                  "Ctrl+Z: the link back");
            // The game colour changed with the picker on the library's page: the button follows.
            check(ue().open_library(), "the library");
            ue().select({});
            break;
        }
        case 80:
            check(ue_press("ue-sw-color-0") && ue().picker_field() == "color.0.color", "the game colour's swatch opens the picker");
            break;
        case 81:
            check(ue_type("ue-cp-hex", "#AA3311") && ue_press("ue-cp-done"), "#AA3311 for the game colour");
            check(ue().library().colors[0].color == d::Color{0xaa, 0x33, 0x11, 255}, "the game colour is #AA3311");
            check(ue().open("main_menu"), "back to the menu");
            check(ue_node(ue_cp_btn_)->fills[0].style == ue_cp_key_ && ue_node(ue_cp_btn_)->fills[0].color == d::Color{0xaa, 0x33, 0x11, 255},
                  "the linked button follows the game colour");
            check(ue_file(".html").find("#aa3311") != std::string::npos, "and its page too");
            // The eyedropper: the title's colour taken from the canvas, from a rectangle in the game colour (the
            // menu's buttons show drawn frame pictures over their fills: what is seen there is the picture).
            ue_cp_rect_ = ue().add_layer(d::NodeType::Rectangle, 1500, 100, 200, 120);
            ue().select({ue_cp_rect_});
            check(ue().set_property("fill.0.style", ue_cp_key_), "a rectangle in the game colour");
            ue_cp_title_ = ue_named("Название игры");
            ue().select({ue_cp_title_});
            break;
        case 82:
            ue_json_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            check(ue_press("ue-sw-text") && ue().picker_field() == "text_color", "the title's text colour in the picker");
            break;
        case 83: {
            check(ue_press("ue-cp-dropper") && ue().picker_dropping(), "the eyedropper picked up");
            // Away with Esc, then with the right button, then a click off the screen: nothing taken, the picker stays.
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!ue().picker_dropping() && ue().picker_open(), "Esc puts the eyedropper away, the picker stays");
            check(ue_press("ue-cp-dropper") && ue().picker_dropping(), "picked up again");
            const auto box = ue().layer_box(ue_cp_rect_);
            check(box.has_value(), "the rectangle is on the canvas");
            if (box) right_click(ue_wx(box->cx()), ue_wy(box->cy()));
            check(!ue().picker_dropping() && ue().picker_open() && d::save_screen(ue().screen()) == ue_json_,
                  "the right button puts it away: nothing taken");
            check(ue_press("ue-cp-dropper"), "picked up again");
            left_click(ue().canvas_left() + 6, ue().canvas_top() + 6); // the pasteboard, off the screen
            check(!ue().picker_dropping() && d::save_screen(ue().screen()) == ue_json_ && ue().selection() == std::vector<u32>{ue_cp_title_},
                  "a click off the screen puts it away: nothing taken, nothing selected");
            check(ue_press("ue-cp-dropper"), "picked up again");
            break;
        }
        case 84: {
            // On the canvas over the rectangle: its colour, exactly.
            const auto box = ue().layer_box(ue_cp_rect_);
            if (box) left_click(ue_wx(box->cx()), ue_wy(box->cy()));
            check(!ue().picker_dropping() && ue().picker_color() == d::Color{0xaa, 0x33, 0x11, 255},
                  "the eyedropper takes the colour seen on the canvas: " + d::color_hex(ue().picker_color()));
            check(ue().selection() == std::vector<u32>{ue_cp_title_}, "the title stays selected: the click selected nothing");
            d::Screen now, was;
            d::load_screen(d::save_screen(ue().screen()), now);
            d::load_screen(ue_json_, was);
            check(d::save_screen(now) != ue_json_ && d::find(now.root, ue_cp_title_)->text_style.color == d::Color{0xaa, 0x33, 0x11, 255},
                  "the title shows it");
            d::find(now.root, ue_cp_title_)->text_style = d::find(was.root, ue_cp_title_)->text_style;
            check(d::save_screen(now) == ue_json_, "no other layer changed (the rectangle under the click neither)");
            check(ue_press("ue-cp-done") && ue().history().cursor() == ue_cursor_ + 1, "«Готово»: one step");
            ue().select({ue_cp_btn_});
            break;
        }
        case 85: {
            // At the bottom of the scrolled panel: the stroke's swatch scrolled down to the panel's edge.
            Rml::Element* swatch = ed_.find_element("ue-sw-stroke");
            Rml::Element* panel = nullptr;
            for (Rml::Element* a = swatch ? swatch->GetParentNode() : nullptr; a && !panel; a = a->GetParentNode()) {
                const auto o = a->GetComputedValues().overflow_y();
                if (o == Rml::Style::Overflow::Auto || o == Rml::Style::Overflow::Scroll) panel = a;
            }
            check(swatch && panel, "the stroke's swatch in the scrolling design panel");
            if (swatch && panel) {
                const f32 bottom = panel->GetAbsoluteOffset(Rml::BoxArea::Padding).y + panel->GetClientHeight();
                const f32 y = swatch->GetAbsoluteOffset(Rml::BoxArea::Border).y;
                panel->SetScrollTop(panel->GetScrollTop() + (y - (bottom - 26)));
            }
            break;
        }
        case 86: {
            f32 x = 0, y = 0;
            check(ue_point("ue-sw-stroke", 0.5f, 0.5f, x, y) && y > ed_.context()->GetDimensions().y * 0.6f,
                  "the swatch is low in the window: " + std::to_string(static_cast<int>(y)));
            check(ue_press("ue-sw-stroke") && ue().picker_field() == "stroke.color", "a click opens the picker on the stroke");
            ue_json_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            break;
        }
        case 87: {
            std::string why;
            const bool reachable = ue_picker_reachable(why);
            check(reachable, "opened from the panel's bottom edge it stays inside the window: " + why);
            // A click outside the picker, on the canvas over another layer: it closes, the canvas is not clicked.
            const auto box = ue().layer_box(ue_cp_title_);
            if (box) left_click(ue_wx(box->cx()), ue_wy(box->cy()));
            check(!ue().picker_open() && ue().selection() == std::vector<u32>{ue_cp_btn_} && d::save_screen(ue().screen()) == ue_json_ &&
                      ue().history().cursor() == ue_cursor_,
                  "a click outside closes it: the title under it is not selected, nothing moves, no step");
            // At the window's very corner.
            const Rml::Vector2i w = ed_.context()->GetDimensions();
            check(ue().open_picker("stroke.color", static_cast<f32>(w.x) - 4, static_cast<f32>(w.y) - 4, 2, 2), "opened at the corner");
            break;
        }
        case 88: {
            std::string why;
            const bool reachable = ue_picker_reachable(why);
            check(reachable, "at the window's corner it stays inside: " + why);
            ue().close_picker(false);
            check(d::save_screen(ue().screen()) == ue_json_ && ue().history().cursor() == ue_cursor_, "cancelled: nothing changed");
            // A copy of a component: its colour becomes its own; Esc adds nothing.
            ue().select({ue_inst_});
            break;
        }
        case 89: {
            const d::Node* copy = ue_node(ue_inst_);
            check(copy && copy->master && !copy->fills.empty(), "the copy of «Кнопка «Настройки»» has a fill");
            ue_json_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            ue_cp_overrides_ = copy ? copy->overrides.size() : 0;
            check(ue_press("ue-sw-fill-0") && ue().picker_open(), "the copy's fill in the picker");
            break;
        }
        case 90: {
            check(ue_type("ue-cp-hex", "#5544AA"), "#5544AA typed");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(d::save_screen(ue().screen()) == ue_json_ && ue_node(ue_inst_)->overrides.size() == ue_cp_overrides_,
                  "Esc: nothing kept, no change of its own added");
            check(ue_press("ue-sw-fill-0"), "the picker again");
            break;
        }
        case 91: {
            check(ue_type("ue-cp-hex", "#5544AA") && ue_press("ue-cp-done"), "#5544AA, «Готово»");
            const d::Node* copy = ue_node(ue_inst_);
            check(copy && copy->fills[0].color == d::Color{0x55, 0x44, 0xaa, 255} &&
                      std::find(copy->overrides.begin(), copy->overrides.end(), "fills") != copy->overrides.end() &&
                      ue().history().cursor() == ue_cursor_ + 1,
                  "the copy keeps its own fill colour, one step");
            const d::Node* master = copy ? d::find_variant(ue().library(), copy->component, copy->variant) : nullptr;
            check(!master || master->fills.empty() || master->fills[0].color != d::Color{0x55, 0x44, 0xaa, 255},
                  "the component itself is not recoloured");
            // A gradient's stops: the rectangle with a linear fill.
            ue().select({ue_cp_rect_});
            check(ue().set_property("fill.0.kind", "linear") && ue().set_property("effect.add", ""), "a gradient and a shadow");
            break;
        }
        case 92: {
            const d::Node* r = ue_node(ue_cp_rect_);
            check(r && r->fills[0].stops.size() == 2, "the gradient has two stops");
            ue_json_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            check(ue_press("ue-sw-fill2-0") && ue().picker_field() == "fill.0.color2" && r && ue().picker_color() == r->fills[0].stops.back().color,
                  "the last stop's swatch opens the picker on it");
            break;
        }
        case 93: {
            const d::GradientStop first = ue_node(ue_cp_rect_)->fills[0].stops.front();
            const f32 last_at = ue_node(ue_cp_rect_)->fills[0].stops.back().position;
            check(ue_type("ue-cp-hex", "#00FF0080") && ue_press("ue-cp-done"), "#00FF0080 for the last stop");
            const d::Paint& f = ue_node(ue_cp_rect_)->fills[0];
            check(f.stops.size() == 2 && f.stops.front() == first && f.stops.back().color == d::Color{0, 0xff, 0, 0x80} &&
                      f.stops.back().position == last_at && ue().history().cursor() == ue_cursor_ + 1,
                  "only the last stop's colour changed, its alpha kept: one step");
            check(ue_press("ue-sw-effect-0") && ue().picker_field() == "effect.0.color", "the shadow's swatch opens the picker");
            break;
        }
        case 94: {
            check(ue_type("ue-cp-alpha-field", "50") && ue_press("ue-cp-done"), "the shadow at 50 % (it was 25 %)");
            check(ue_node(ue_cp_rect_)->effects[0].color.a == 128 && ue().history().cursor() == ue_cursor_ + 2,
                  "the shadow's alpha: one step: " + d::color_hex(ue_node(ue_cp_rect_)->effects[0].color) + " " +
                      std::to_string(ue().history().cursor() - ue_cursor_));
            // «Простой»: the same picker on the beginner's button.
            check(ue().open(ue_simple_screen_) && click("ue-mode-simple") && ue().simple(), "the beginner's screen in «Простой»");
            ue().select({ue_blocks_[0]});
            break;
        }
        case 95: {
            ue_json_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            check(ue_press("ue-s-swatch") && ue().picker_field() == "simple.color" &&
                      ue().picker_color() == ue_node(ue_blocks_[0])->fills[0].color,
                  "the simple panel's colour swatch opens the same picker");
            break;
        }
        case 96: {
            check(shown("ue-picker") && shown("ue-cp-theme-0"), "with the game's colours");
            check(ue_press("ue-cp-theme-0") && ue_press("ue-cp-done"), "a game colour, «Готово»");
            const d::Node* b = ue_node(ue_blocks_[0]);
            check(b && b->fills[0].style == ue_cp_key_ && ue().history().cursor() == ue_cursor_ + 1 &&
                      ue().history().undo_label() == "Изменено: Цвет",
                  "the button follows the game colour: one step «" + ue().history().undo_label() + "»");
            ue_json2_ = d::save_screen(ue().screen());
            // A click on the mode switch while the picker is open goes to the picker's outside: kept, mode as it was.
            check(ue_press("ue-s-swatch"), "the picker again");
            break;
        }
        case 97: {
            check(ue_type("ue-cp-hex", "#FF8800"), "#FF8800 typed");
            check(ue_press("ue-mode-full") && !ue().picker_open() && ue().simple(), "a click on «Полный» closes the picker, the mode stays");
            check(ue_node(ue_blocks_[0])->fills[0].color == d::Color{0xff, 0x88, 0, 255} && ue().history().cursor() == ue_cursor_ + 2,
                  "the colour kept: one step");
            // Switched by code while open: kept first, then the switch; Ctrl+Z in «Полный» takes it back.
            check(ue_press("ue-s-swatch"), "the picker again");
            break;
        }
        case 98: {
            check(ue_type("ue-cp-hex", "#0088FF"), "#0088FF typed");
            ue().set_simple(false);
            check(!ue().picker_open() && !ue().simple() && ue_node(ue_blocks_[0])->fills[0].color == d::Color{0, 0x88, 0xff, 255} &&
                      ue().history().cursor() == ue_cursor_ + 3,
                  "«Полный» while it is open: the colour kept as one step");
            key(SDLK_Z, SDL_KMOD_CTRL);
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == ue_json2_, "Ctrl+Z twice in «Полный»: back to the game colour");
            check(click("ue-mode-simple") && ue().simple(), "«Простой» again");
            ue().select({ue_blocks_[0]});
            break;
        }
        case 99:
            check(ue_press("ue-s-text-swatch") && ue().picker_field() == "simple.text_color", "the label's colour swatch");
            break;
        case 100: {
            check(ue_type("ue-cp-hex", "#102030") && ue_press("ue-cp-done"), "#102030 for the label");
            const d::Node* label = d::block_label(*ue_node(ue_blocks_[0]));
            check(label && label->text_style.color == d::Color{0x10, 0x20, 0x30, 255} && ue().history().undo_label() == "Изменено: Цвет надписи",
                  "the label's colour: " + ue().history().undo_label());
            ue_json2_ = d::save_screen(ue().screen());
            check(ue().open("main_menu") && ue().open(ue_simple_screen_) && d::save_screen(ue().screen()) == ue_json2_,
                  "saved, opened again: the same colours and link");
            check(click("ue-check"), "«Проверить»");
            break;
        }
        case 101: {
            check(ue().checking(), "the screen comes alive");
            Rml::ElementDocument* page = ue().page();
            Rml::Element* button = page ? page->GetElementById("n" + std::to_string(ue_blocks_[0])) : nullptr;
            const d::Color want = ue().library().colors[0].color;
            const Rml::Colourb c = button ? button->GetProperty<Rml::Colourb>("background-color") : Rml::Colourb();
            check(button && c.red == want.r && c.green == want.g && c.blue == want.b && c.alpha == want.a,
                  "«Проверить» shows the button in the game colour");
            ue().set_checking(false);
            // An empty frame in «Простой»: its swatch shows white (no fill yet); white picked must not get lost.
            ue_cp_frame_ = ue().add_layer(d::NodeType::Frame, 40, 300, 200, 120);
            ue().select({ue_cp_frame_});
            check(ue().set_property("fill.0.remove", ""), "its fill taken away (a frame starts white)");
            break;
        }
        case 102: {
            const d::Node* f = ue_node(ue_cp_frame_);
            check(f && f->fills.empty(), "a frame without a fill");
            ue_json_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            check(ue_press("ue-s-swatch") && ue().picker_color() == d::Color{255, 255, 255, 255}, "its swatch: white, as a fill would start");
            break;
        }
        case 103:
            check(ue_press("ue-cp-done") && d::save_screen(ue().screen()) == ue_json_ && ue().history().cursor() == ue_cursor_,
                  "opened and «Готово» untouched: no fill added, no step");
            check(ue_press("ue-s-swatch"), "the picker again");
            break;
        case 104: {
            check(ue_type("ue-cp-hex", "#ffffff") && ue_press("ue-cp-done"), "white picked by its HEX, «Готово»");
            const d::Node* f = ue_node(ue_cp_frame_);
            check(f && f->fills.size() == 1 && f->fills[0].color == d::Color{255, 255, 255, 255} &&
                      ue().history().cursor() == ue_cursor_ + 1,
                  "the frame is white now (the white picked was not taken for «nothing changed»): one step");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == ue_json_, "Ctrl+Z: no fill again");
            // The eyedropper on the live screen («Проверить»): the coins' text takes the button's colour.
            ue().select({ue_blocks_[1]});
            break;
        }
        case 105:
        case 114:
        case 117:
            if (ue_step_ == 105) {
                ue_json_ = d::save_screen(ue().screen());
                ue_cursor_ = ue().history().cursor();
            } else {
                check(ue().view() == (ue_step_ == 114 ? 5 : 4), "the player's size: " + std::to_string(ue().view()));
            }
            check(ue_press("ue-s-swatch") && ue().picker_field() == "simple.color", "the text block's colour in the picker");
            break;
        case 106:
        case 115:
        case 118:
            check(shown("ue-cp-live") && ue_press("ue-cp-live"), "the picker's «Проверить» eyedropper");
            check(ue().checking() && ue().picker_open() && ue().picker_living() && ue().picker_dropping(),
                  "the screen comes alive, the picker stays open with its eyedropper");
            check(ue().selection() == std::vector<u32>{ue_blocks_[1]}, "the text stays its target");
            break;
        case 107: {
            check(!shown("ue-simple") && !shown("ue-selection") && shown("ue-picker"), "no panel and no selection box over the live screen");
            if (!ue_cp_clicked_) {
                ue_cp_want_ = ue().library().colors[0].color;
                check(ue().page() && ue().check_log().empty(), "the live page, nothing pressed yet");
                check(ue_page_click(ue_blocks_[0], 0.08f, 0.5f), "a click on the button as the game draws it");
                ue_cp_clicked_ = true;
                return true; // the game's next frame: a pressed button would act there
            }
            ue_cp_clicked_ = false;
            check(!ue().picker_dropping() && ue().picker_color() == ue_cp_want_,
                  "the eyedropper takes the drawn pixel: " + d::color_hex(ue().picker_color()) + " (the button: " + d::color_hex(ue_cp_want_) + ")");
            check(ue().check_log().empty(), "the button under the click is not pressed: " + ue_log_since(0));
            check(ue().checking() && ue().selection() == std::vector<u32>{ue_blocks_[1]}, "still live, the target kept");
            check(ue_press("ue-cp-done"), "«Готово»");
            check(!ue().checking() && !ue().picker_open() && ue().selection() == std::vector<u32>{ue_blocks_[1]},
                  "back to the layers, the text still selected");
            check(ue_node(ue_blocks_[1])->text_style.color == ue_cp_want_ && ue().history().cursor() == ue_cursor_ + 1,
                  "the text in the button's colour: one step");
            ue_json_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            break;
        }
        case 108:
        case 111:
            check(ue_press("ue-s-swatch"), "the picker again");
            break;
        case 109:
        case 112:
            check(ue_press("ue-cp-live") && ue().checking(), "live again");
            break;
        case 110:
            if (!ue_cp_clicked_) {
                check(ue_page_click(ue_blocks_[0], 0.08f, 0.5f), "a click on the button");
                ue_cp_clicked_ = true;
                return true;
            }
            ue_cp_clicked_ = false;
            check(!ue().picker_dropping() && ue().picker_color() == ue_cp_want_ && ue().check_log().empty(), "a colour taken, nothing pressed");
            check(ue_press("ue-cp-cancel"), "«Отмена»");
            check(!ue().checking() && d::save_screen(ue().screen()) == ue_json_ && ue().history().cursor() == ue_cursor_ &&
                      ue().check_log().empty(),
                  "«Отмена»: back to the layers, all as it was, nothing pressed, no step");
            break;
        case 113:
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!ue().picker_dropping() && ue().picker_open() && ue().checking(), "Esc: the eyedropper away, still live");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!ue().picker_open() && !ue().checking() && d::save_screen(ue().screen()) == ue_json_ && ue().history().cursor() == ue_cursor_,
                  "Esc again: closed, back to the layers, nothing changed");
            // The same on the player's 4:3 screen, then on 21:9.
            ue().set_view(5);
            ue().select({ue_blocks_[1]});
            break;
        case 116:
        case 119: {
            const std::string size = ue_step_ == 116 ? "4:3" : "21:9";
            if (!ue_cp_clicked_) {
                check(ue_page_click(ue_blocks_[0], 0.08f, 0.5f), size + ": a click on the button");
                ue_cp_clicked_ = true;
                return true;
            }
            ue_cp_clicked_ = false;
            check(ue().picker_color() == ue_cp_want_, size + ": the drawn pixel " + d::color_hex(ue().picker_color()));
            check(ue().check_log().empty() && ue().selection() == std::vector<u32>{ue_blocks_[1]}, size + ": nothing pressed, the target kept");
            check(ue_press("ue-cp-cancel") && !ue().checking() && d::save_screen(ue().screen()) == ue_json_, size + ": «Отмена»");
            if (ue_step_ == 116) {
                ue().set_view(4);
                ue().select({ue_blocks_[1]});
            } else {
                ue().set_view(0);
                // Several layers with different colours: what is picked goes to all of them.
                check(click("ue-mode-full") && !ue().simple(), "«Полный»");
                check(ue().open("main_menu"), "back to the menu");
                ue_cp_red_ = ue().add_layer(d::NodeType::Rectangle, 100, 860, 120, 80);
                ue_cp_green_ = ue().add_layer(d::NodeType::Rectangle, 240, 860, 120, 80);
                ue_cp_linked_ = ue().add_layer(d::NodeType::Rectangle, 380, 860, 120, 80);
                ue().select({ue_cp_red_});
                ue().set_property("fill.0.color", "#ff0000");
                ue().select({ue_cp_green_});
                ue().set_property("fill.0.color", "#00ff00");
                ue().select({ue_cp_linked_});
                ue().set_property("fill.0.style", ue_cp_key_);
                ue().select({ue_cp_red_, ue_cp_green_, ue_cp_linked_});
            }
            break;
        }
        case 120:
            check(ue_node(ue_cp_red_)->fills[0].color == d::Color{255, 0, 0, 255} && ue_node(ue_cp_green_)->fills[0].color == d::Color{0, 255, 0, 255} &&
                      ue_node(ue_cp_linked_)->fills[0].style == ue_cp_key_,
                  "red, green and one in the game colour, all three selected");
            ue_json_ = d::save_screen(ue().screen());
            ue_cursor_ = ue().history().cursor();
            check(ue_press("ue-sw-fill-0") && ue().picker_color() == d::Color{255, 0, 0, 255}, "the picker shows the first one's red");
            break;
        case 121:
            check(ue_text_of("ue-cp-note").find("разные цвета") != std::string::npos, "and says the layers differ: " + ue_text_of("ue-cp-note"));
            check(ue_press("ue-cp-done") && d::save_screen(ue().screen()) == ue_json_ && ue().history().cursor() == ue_cursor_,
                  "opened and «Готово» untouched: nothing changed, no step");
            check(ue_press("ue-sw-fill-0"), "the picker again");
            break;
        case 122: {
            if (Rml::Element* hex = ed_.find_element("ue-cp-hex")) {
                hex->Focus();
                hex->Blur();
            }
            check(d::save_screen(ue().screen()) == ue_json_, "the HEX field focused and left: nothing changes");
            check(ue_type("ue-cp-hex", "#ff0000"), "red picked by its HEX (the first one's own colour)");
            check(ue_node(ue_cp_green_)->fills[0].color == d::Color{255, 0, 0, 255} && ue_node(ue_cp_linked_)->fills[0].style.empty(),
                  "the canvas shows all three red");
            check(ue_press("ue-cp-done") && ue().history().cursor() == ue_cursor_ + 1, "«Готово»: one step");
            for (u32 id : {ue_cp_red_, ue_cp_green_, ue_cp_linked_}) {
                const d::Node* n = ue_node(id);
                check(n && n->fills[0].color == d::Color{255, 0, 0, 255} && n->fills[0].style.empty(), "every one red, with no link");
            }
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == ue_json_ && ue_node(ue_cp_green_)->fills[0].color == d::Color{0, 255, 0, 255} &&
                      ue_node(ue_cp_linked_)->fills[0].style == ue_cp_key_,
                  "one Ctrl+Z: green and the game colour's link back");
            ue().select({ue_cp_red_, ue_cp_green_, ue_cp_linked_});
            key(SDLK_DELETE, SDL_KMOD_NONE);
            check(!ue_node(ue_cp_red_) && !ue_node(ue_cp_green_) && !ue_node(ue_cp_linked_), "the three layers deleted again");
            // --- «Шкала времени» (13.5): the keys of a movement over time, carried by the mouse, and played ---
            // A blue rectangle with four keys told apart by x (and the last by y): 0 %, 25 %, 50 % red, 100 %.
            ue_tl_rect_ = ue().add_layer(d::NodeType::Rectangle, 1400, 900, 100, 60);
            ue().select({ue_tl_rect_});
            ue().set_property("fill.0.color", "#3355ff");
            ue().set_property("motion.kind", "custom"); // starts from «Пульсирует»: keys at 0, 50, 100 %
            ue().set_property("motion.key.1.scale", "100");
            ue().set_property("motion.key.1.at", "25");
            ue().set_property("motion.key.1.x", "100");
            ue().set_property("motion.key.add", ""); // halfway between 25 and 100 %
            ue().set_property("motion.key.2.at", "50");
            ue().set_property("motion.key.2.x", "200");
            ue().set_property("motion.key.2.color.add", "");
            ue().set_property("motion.key.2.color", "#ff0000");
            ue().set_property("motion.key.3.y", "30");
            ue().set_property("motion.duration", "2");
            ue().set_property("motion.delay", "1");
            ue().set_property("motion.easing", "linear");
            ue().set_property("motion.loop", ""); // once
            check(click("ue-tab-motion") && ue().panel() == "motion", "the «Движение» tab");
            break;
        }
        case 123: {
            check(ue_list(ue_tl_ats()) == "0 25 50 100" && ue_list(ue_tl_xs()) == "0 100 200 0", "four keys: " + ue_list(ue_tl_ats()));
            check(shown("ue-tl") && shown("ue-tl-key-0") && shown("ue-tl-key-3") && ue().timeline_shown(), "the timeline shows the four keys");
            check(std::abs(ue().timeline_span() - 3) < 1e-4f, "it spans the delay and one pass: 3 s");
            ue_tl_json_ = d::save_screen(ue().screen());
            ue_tl_cursor_ = ue().history().cursor();
            // A click on a key: picked, nothing changed.
            check(ue_press("ue-tl-key-1") && ue().timeline_key() == 1, "a click on the second key picks it");
            check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_ && !ue().motion_preview(),
                  "a click without a move: no change, no step, no preview");
            break;
        }
        case 124: {
            check(shown("ue-key-row-1") && ed_.find_element("ue-key-row-1")->IsClassSet("selected"), "its row in the panel is marked");
            // Carried past the red key at 50 % to 75 %: held first, then let go.
            check(ue_tl_carry(1, ue_tl_x(1 + 0.75 * 2), true) && ue().key_dragging(), "the key carried with the mouse held");
            check(ue().history().cursor() == ue_tl_cursor_, "while held: nothing in the history");
            check(ue().timeline_key() == 2, "past the red key it is third, and still the picked one");
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, ue_tl_x(1 + 0.75 * 2), ue_tl_y());
            check(!ue().key_dragging() && ue_list(ue_tl_ats()) == "0 50 75 100" && ue_list(ue_tl_xs()) == "0 200 100 0",
                  "let go at 75 %: " + ue_list(ue_tl_ats()) + " / x " + ue_list(ue_tl_xs()));
            check(ue_node(ue_tl_rect_)->motion.keys[1].tint && ue_node(ue_tl_rect_)->motion.keys[1].color == d::Color{255, 0, 0, 255} &&
                      ue_node(ue_tl_rect_)->motion.keys[3].y == 30,
                  "the neighbours keep their colour and place");
            check(ue().history().cursor() == ue_tl_cursor_ + 1 && ue().history().undo_label() == "Изменено: Время ключа",
                  "one gesture, one step: " + ue().history().undo_label());
            ue_tl_moved_ = d::save_screen(ue().screen());
            break;
        }
        case 125: {
            check(ed_.find_element("ue-key-row-2") && ed_.find_element("ue-key-row-2")->IsClassSet("selected"), "the carried key's row is marked");
            Rml::Element* at = ed_.find_element("ue-key-at-2");
            check(at && rmlui_dynamic_cast<Rml::ElementFormControl*>(at)->GetValue() == "75%", "its time field says 75%");
            // The carried key edited in its row, and its colour by the picker: the right key changes.
            check(ue_type("ue-key-x-2", "150") && ue_list(ue_tl_xs()) == "0 200 150 0", "its X typed: " + ue_list(ue_tl_xs()));
            check(ue_press("ue-key-color-add-2") && ue_node(ue_tl_rect_)->motion.keys[2].tint, "its colour on");
            break;
        }
        case 126:
            check(ue_press("ue-sw-key-2") && ue().picker_field() == "motion.key.2.color", "its swatch opens the picker on that key");
            break;
        case 127: {
            check(ue_type("ue-cp-hex", "#00ff00") && ue_press("ue-cp-done"), "green, «Готово»");
            const d::Motion& m = ue_node(ue_tl_rect_)->motion;
            check(m.keys[2].color == d::Color{0, 255, 0, 255} && m.keys[1].color == d::Color{255, 0, 0, 255}, "the carried key green, the red one red");
            check(ue().history().cursor() == ue_tl_cursor_ + 4, "four steps: carried, X, colour on, green");
            const std::string green = d::save_screen(ue().screen());
            for (int i = 0; i < 3; ++i) key(SDLK_Z, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == ue_tl_moved_, "Ctrl+Z three times: as just after the carry");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == ue_tl_json_ && ue_list(ue_tl_ats()) == "0 25 50 100", "Ctrl+Z: the key back at 25 %");
            key(SDLK_Y, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == ue_tl_moved_, "Ctrl+Y: at 75 % again");
            for (int i = 0; i < 3; ++i) key(SDLK_Y, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == green, "Ctrl+Y: X and green again");
            ue_tl_json_ = green;
            ue_tl_cursor_ = ue().history().cursor();
            break;
        }
        case 128: {
            // Esc while carrying: all of it undone, no step; letting go after changes nothing.
            check(ue_tl_carry(0, ue_tl_x(1 + 0.4 * 2), true) && ue().key_dragging() && ue_list(ue_tl_ats()) != "0 50 75 100",
                  "the first key carried to 40 %: " + ue_list(ue_tl_ats()));
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!ue().key_dragging() && d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_ &&
                      ue().timeline_key() == 0,
                  "Esc: back where it was, no step, the key still picked");
            mouse(SDL_EVENT_MOUSE_BUTTON_UP, ue_tl_x(1 + 0.4 * 2), ue_tl_y());
            check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_, "letting go after Esc: nothing");
            // The ends: the last key past the right end stays at 100 %: no change, no step.
            check(ue_tl_carry(3, ue_tl_x(3) + 200), "the last key carried past the right end");
            check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_, "already at 100 %: no change, no step");
            // The first key past the right end: at 100 %, before the key there (which is still itself, y 30).
            check(ue_tl_carry(0, ue_tl_x(3) + 200), "the first key carried past the right end");
            const d::Motion& m = ue_node(ue_tl_rect_)->motion;
            check(ue_list(ue_tl_ats()) == "50 75 100 100" && m.keys[2].y == 0 && m.keys[3].y == 30 && ue().timeline_key() == 2,
                  "at 100 %, before the last key: " + ue_list(ue_tl_ats()) + ", picked " + std::to_string(ue().timeline_key()));
            check(ue().history().cursor() == ue_tl_cursor_ + 1, "one step");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(d::save_screen(ue().screen()) == ue_tl_json_, "Ctrl+Z: back");
            break;
        }
        case 129: {
            // Each stage on its own frame: the panel is laid out (and rebuilt after a key moves) in between.
            const std::string focused = ed_.context()->GetFocusElement() ? ed_.context()->GetFocusElement()->GetId() : std::string();
            switch (ue_tl_stage_++) {
            case 0: {
                // The third key past the left end: at 0 %, after the key there.
                check(ue_tl_carry(2, ue_tl_x(0) - 200), "the green key carried past the left end");
                const d::Motion& m = ue_node(ue_tl_rect_)->motion;
                check(ue_list(ue_tl_ats()) == "0 0 50 100" && m.keys[1].color == d::Color{0, 255, 0, 255} && ue().timeline_key() == 1,
                      "at 0 %, after the first key: " + ue_list(ue_tl_ats()));
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_, "Ctrl+Z: back");
                left_click(ue_tl_x(1), ue_tl_y());
                check(ue().timeline_key() == 0, "the first key picked on the timeline");
                return true;
            }
            case 1: {
                // The time typed in the picked key's field, past the red key: the pick follows it.
                // Enter moves it, and the blur at once after it (the field still named for the old place) leaves the neighbour be.
                const bool typed = ue_type("ue-key-at-0", "60");
                check(typed && ue_list(ue_tl_ats()) == "50 60 75 100" && ue().timeline_key() == 1 && ue().history().cursor() == ue_tl_cursor_ + 1,
                      "60 typed: second now, still picked, one step: " + ue_list(ue_tl_ats()) + ", picked " + std::to_string(ue().timeline_key()));
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_, "Ctrl+Z: back again");
                left_click(ue_tl_x(1), ue_tl_y());
                return true;
            }
            case 2: {
                // Typed again and Enter, the field kept: the keyboard stays with the key once the panel is laid out.
                check(ue().timeline_key() == 0, "the first key picked again");
                const bool typed = ue_type_enter("ue-key-at-0", "60");
                check(typed && ue_list(ue_tl_ats()) == "50 60 75 100" && ue().timeline_key() == 1 && ue().history().cursor() == ue_tl_cursor_ + 1,
                      "60 and Enter, the field kept: " + ue_list(ue_tl_ats()));
                return true;
            }
            case 3: {
                auto* at = rmlui_dynamic_cast<Rml::ElementFormControl*>(ed_.find_element("ue-key-at-1"));
                check(focused == "ue-key-at-1" && at && at->GetValue() == "60%",
                      "a frame on: the keyboard in the moved key's field (60%), not its neighbour's: " + focused);
                // More typed without picking another field: the same key, not the red one (x 200) now in its old row.
                const bool typed = ue_type_here("70");
                check(typed && ue_list(ue_tl_ats()) == "50 70 75 100" && ue_list(ue_tl_xs()) == "200 0 150 0" &&
                          ue().timeline_key() == 1 && ue().history().cursor() == ue_tl_cursor_ + 2,
                      "70 typed on: the same key: " + ue_list(ue_tl_ats()) + " / x " + ue_list(ue_tl_xs()));
                return true;
            }
            case 4: {
                // Tab: on to that key's X; and leaving the field changes nothing.
                key(SDLK_TAB, SDL_KMOD_NONE);
                const std::string next = ed_.context()->GetFocusElement() ? ed_.context()->GetFocusElement()->GetId() : std::string();
                check(next == "ue-key-x-1", "Tab: the moved key's X: " + next);
                const bool typed = ue_type_here("300");
                check(typed && ue_list(ue_tl_xs()) == "200 300 150 0" && ue().history().cursor() == ue_tl_cursor_ + 3,
                      "its X typed: " + ue_list(ue_tl_xs()));
                const std::string before = d::save_screen(ue().screen());
                if (Rml::Element* f = ed_.context()->GetFocusElement()) f->Blur();
                check(d::save_screen(ue().screen()) == before && ue().history().cursor() == ue_tl_cursor_ + 3, "leaving the field: nothing more");
                for (int i = 0; i < 3; ++i) key(SDLK_Z, SDL_KMOD_CTRL);
                check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_, "Ctrl+Z three times: as before");
                left_click(ue_tl_x(2.5), ue_tl_y()); // the 75 % key (green)
                return true;
            }
            case 5: {
                // Backwards past a neighbour: 75 % typed to 25 %, the field kept.
                check(ue().timeline_key() == 2, "the green key picked");
                const bool typed = ue_type_enter("ue-key-at-2", "25");
                check(typed && ue_list(ue_tl_ats()) == "0 25 50 100" && ue_list(ue_tl_xs()) == "0 150 200 0" && ue().timeline_key() == 1,
                      "25 and Enter: second now: " + ue_list(ue_tl_ats()) + " / x " + ue_list(ue_tl_xs()));
                return true;
            }
            case 6: {
                check(focused == "ue-key-at-1", "a frame on: the keyboard with it: " + focused);
                const bool typed = ue_type_here("10");
                check(typed && ue_list(ue_tl_ats()) == "0 10 50 100" && ue_list(ue_tl_xs()) == "0 150 200 0" &&
                          ue_node(ue_tl_rect_)->motion.keys[1].color == d::Color{0, 255, 0, 255} && ue().history().cursor() == ue_tl_cursor_ + 2,
                      "10 typed on: the same green key: " + ue_list(ue_tl_ats()) + " / x " + ue_list(ue_tl_xs()));
                if (Rml::Element* f = ed_.context()->GetFocusElement()) f->Blur();
                key(SDLK_Z, SDL_KMOD_CTRL);
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_, "Ctrl+Z twice: as before");
                key(SDLK_Y, SDL_KMOD_CTRL);
                check(ue_list(ue_tl_ats()) == "0 25 50 100" && ue_list(ue_tl_xs()) == "0 150 200 0", "Ctrl+Y: at 25 % again, the same key");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(d::save_screen(ue().screen()) == ue_tl_json_, "Ctrl+Z: back");
                break;
            }
            default: break;
            }
            ue_tl_stage_ = 0;
            // The preview: a click on the track before the delay is over.
            ue().select({ue_tl_rect_});
            left_click(ue_tl_x(0.5), ue_tl_y());
            check(ue().motion_preview() && std::abs(ue().preview_time() - 0.5) < 0.05, "a click on the track: the canvas at 0.5 s");
            check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_, "the preview changes nothing");
            break;
        }
        case 130: {
            check(std::abs(ue_tl_shift()) < 0.5f, "before the delay is over it stands at its place: " + std::to_string(ue_tl_shift()));
            u8 at[4] = {}, beside[4] = {};
            check(ue_page_pixel(1410, 930, at) && ue_page_pixel(1380, 930, beside) && at[2] > 200 && at[0] < 100,
                  "drawn blue at its place: " + std::to_string(at[0]) + " " + std::to_string(at[1]) + " " + std::to_string(at[2]));
            // Further on: between the first two keys (x 0 → 200 by 50 %): at 25 % of the pass, 100.
            left_click(ue_tl_x(1.5), ue_tl_y());
            break;
        }
        case 131: {
            check(std::abs(ue_tl_shift() - 100) < 1, "at 1.5 s: shifted 100: " + std::to_string(ue_tl_shift()));
            u8 old_place[4] = {}, new_place[4] = {}, bg[4] = {};
            ue_page_pixel(1300, 1000, bg);
            check(ue_page_pixel(1410, 930, old_place) && ue_page_pixel(1550, 930, new_place) &&
                      std::memcmp(new_place, bg, 3) != 0 && std::memcmp(old_place, new_place, 3) != 0,
                  "the canvas draws it 100 to the right");
            // Back to 2.75 s (from the start again, played to there): between 75 % (150) and 100 % (0): 75.
            left_click(ue_tl_x(2.75), ue_tl_y());
            break;
        }
        case 132:
            check(std::abs(ue_tl_shift() - 75) < 1, "at 2.75 s: 75: " + std::to_string(ue_tl_shift()));
            left_click(ue_tl_x(2.25), ue_tl_y()); // back in time: between 50 % (200) and 75 % (150): 175
            break;
        case 133:
            check(std::abs(ue_tl_shift() - 175) < 1, "back at 2.25 s: 175: " + std::to_string(ue_tl_shift()));
            {
                // Scrubbed: held on the track between the 75 % and 100 % marks and carried past its right end.
                const f32 y = ue_tl_y();
                mouse(SDL_EVENT_MOUSE_MOTION, ue_tl_x(2.75), y);
                mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, ue_tl_x(2.75), y);
                for (int i = 1; i <= 4; ++i) mouse(SDL_EVENT_MOUSE_MOTION, ue_tl_x(2.75) + 60.0f * static_cast<f32>(i), y);
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, ue_tl_x(3) + 240, y);
            }
            break;
        case 134: {
            check(std::abs(ue().preview_time() - 3) < 1e-6, "scrubbed past the right end: the moment stops at the end, 3 s: " +
                                                                 std::to_string(ue().preview_time()));
            check(std::abs(ue_tl_shift()) < 0.5f, "at its end: the last key's place (x 0): " + std::to_string(ue_tl_shift()));
            check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_, "the preview wrote nothing");
            // Over and over, back and forth: the second pass backwards.
            check(click("ue-motion-loop") && click("ue-motion-back"), "«Повторять» and «Туда и обратно»");
            check(ue().motion_preview() && std::abs(ue().timeline_span() - 5) < 1e-4f, "still previewing; the timeline spans two passes: 5 s");
            ue_tl_json_ = d::save_screen(ue().screen());
            ue_tl_cursor_ = ue().history().cursor();
            left_click(ue_tl_x(3.5), ue_tl_y()); // 0.5 s into the pass backwards: at 75 % again, 150
            break;
        }
        case 135:
            check(std::abs(ue_tl_shift() - 150) < 1, "at 3.5 s, coming back: 150: " + std::to_string(ue_tl_shift()));
            check(ue_press("ue-tl-play") && ue().preview_playing(), "«Проиграть»");
            ue_tl_t_ = ue().preview_time();
            break;
        case 136:
        case 138:
            if (hold(ue().preview_time() > ue_tl_t_ + 0.05, "it plays on")) return true;
            if (ue_step_ == 136) check(ue_press("ue-tl-play") && !ue().preview_playing(), "«Пауза»");
            else {
                key(SDLK_SPACE, SDL_KMOD_NONE);
                check(!ue().preview_playing(), "the space bar pauses it");
            }
            ue_tl_t_ = ue().preview_time();
            break;
        case 137:
            check(ue().preview_time() == ue_tl_t_, "paused: the moment stays");
            key(SDLK_SPACE, SDL_KMOD_NONE);
            check(ue().preview_playing(), "the space bar plays it again");
            ue_tl_t_ = ue().preview_time();
            break;
        case 139:
            check(ue().preview_time() == ue_tl_t_, "paused again");
            check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_, "playing wrote nothing");
            check(ue_press("ue-tl-stop") && !ue().motion_preview(), "«К редактированию»: the preview ends");
            break;
        case 140:
            check(std::abs(ue_tl_shift()) < 0.5f, "the canvas stands still again: " + std::to_string(ue_tl_shift()));
            // Another layer ends the preview; so does Esc (the layer stays selected).
            left_click(ue_tl_x(2.25), ue_tl_y());
            check(ue().motion_preview(), "previewing again");
            ue().select({ue_cp_btn_});
            check(!ue().motion_preview() && ue().timeline_key() == -1, "another layer: the preview ends");
            ue().select({ue_tl_rect_});
            break;
        case 141:
            left_click(ue_tl_x(1.5), ue_tl_y());
            check(ue().motion_preview(), "previewing");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            check(!ue().motion_preview() && ue().selection() == std::vector<u32>{ue_tl_rect_}, "Esc: the preview ends, the layer still selected");
            check(d::save_screen(ue().screen()) == ue_tl_json_ && ue().history().cursor() == ue_tl_cursor_, "nothing written");
            // Saved, opened again; «Простой» and back.
            check(ue().open(ue_simple_screen_) && ue().open("main_menu") && d::save_screen(ue().screen()) == ue_tl_json_,
                  "saved, opened again: the same keys");
            ue().select({ue_tl_rect_});
            check(click("ue-mode-simple") && ue().simple(), "«Простой»");
            break;
        case 142:
            check(!ue().timeline_shown() && d::save_screen(ue().screen()) == ue_tl_json_, "«Простой» has no timeline; nothing changed");
            check(click("ue-mode-full") && !ue().simple(), "«Полный» again");
            break;
        case 143: {
            check(ue().timeline_shown() && ue_list(ue_tl_ats()) == "0 50 75 100", "the timeline with the same keys");
            const std::string id = std::to_string(ue_tl_rect_);
            const std::string html = ue_file(".html");
            check(html.find("animation: 2s linear 1s infinite alternate m" + id) != std::string::npos && html.find("75% {") != std::string::npos,
                  "the game's page plays it: 2 s after 1 s, over and over, back and forth, a key at 75 %");
            check(click("ue-check") && ue().checking(), "«Проверить»");
            break;
        }
        case 144: {
            Rml::ElementDocument* page = ue().page();
            Rml::Element* e = page ? page->GetElementById("n" + std::to_string(ue_tl_rect_)) : nullptr;
            const Rml::Property* a = e ? e->GetProperty("animation") : nullptr;
            check(a && a->ToString().find("m" + std::to_string(ue_tl_rect_)) != std::string::npos, "«Проверить» plays the same movement");
            ue().set_checking(false);
            ue().select({ue_tl_rect_});
            key(SDLK_DELETE, SDL_KMOD_NONE);
            check(!ue_node(ue_tl_rect_), "the rectangle deleted again");
            check(click("ue-tab-design"), "the «Дизайн» tab again");
            break;
        }
        case 145: {
            // Moving layers between frames (13.6) on games/examples/layer-move, with the mouse and keys as the author
            // does: ids 2 title, 3 «Рамка А» (4 button «Кнопка» with 5 its text, 6 «Картинка»), 7 «Рамка Б» (8 caption,
            // 9 «Ряд» in a row with 10 and 11, 12 «Окошко»). Each stage on its own frame.
            const f32 kHalf = 0.5f;
            // Waiting (the mouse resting) repeats the same stage.
            auto wait = [&](bool ready, const char* what) {
                if (!hold(ready, what)) return false;
                --ue_mv_stage_;
                return true;
            };
            switch (ue_mv_stage_++) {
            case 0: {
                const std::filesystem::path from = utf8_path(FORGE_EXAMPLES_DIR) / "layer-move";
                std::error_code ec;
                for (const char* ext : {".json", ".html"})
                    std::filesystem::copy_file(from / utf8_path(std::string("перенос_пример") + ext),
                                               ed_.ui_game_dir / "ui" / utf8_path(std::string("перенос_пример") + ext),
                                               std::filesystem::copy_options::overwrite_existing, ec);
                check(!ec && ue().open("перенос_пример"), "the example screen with two frames opens");
                check(ue().view() == 0 && !ue().simple(), "in «Макет», «Полный»");
                ue().select({});
                return true;
            }
            case 1: {
                check(ue_mv_kids(3) == "4 6" && ue_mv_kids(7) == "8 9 12" && ue_mv_kids(9) == "10 11", "the frames as drawn");
                ue_mv_json_ = ue_mv_orig_ = d::save_screen(ue().screen());
                ue_mv_cursor_ = ue().history().cursor();
                f32 x = 0, y = 0;
                check(ue_mv_row(4, kHalf, x, y), "the button's row in the layers' list");
                left_click(x, y);
                check(ue().selection() == std::vector<u32>{4} && ue_mv_same(), "a click on its row picks it, nothing changes");
                return true;
            }
            case 2:
                // The list: the button carried onto the middle of «Рамка Б»'s row, held there.
                check(ue_mv_carry(4, 7, kHalf, false) && ue().layer_dragging() && ue().drop_parent() == 7,
                      "carried onto «Рамка Б»: it would go in");
                check(ue_mv_same(), "while held nothing changes");
                ue_mv_box_ = ue().layer_box(4).value_or(d::Rect{});
                return true;
            case 3: {
                Rml::Element* row = ed_.find_element("ue-layer-7");
                check(row && row->IsClassSet("drop-into"), "the row of «Рамка Б» shows it goes inside");
                f32 x = 0, y = 0;
                ue_mv_row(7, kHalf, x, y);
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
                const d::Node* b = ue_node(4);
                check(ue_mv_parent(4) == 7 && ue_mv_kids(7) == "8 9 12 4" && ue_mv_kids(3) == "6", "let go: in «Рамка Б», on top");
                check(b && b->x == ue_mv_box_.x - 1000 && b->y == ue_mv_box_.y - 200 && b->w == 240 && b->h == 80 &&
                          ue_mv_kids(4) == "5" && b->on_click.size() == 1,
                      "its place counted from «Рамка Б», its size, text and click kept");
                check(ue().history().cursor() == ue_mv_cursor_ + 1 && ue().history().undo_label() == "Перенесено в «Рамка Б»",
                      "one step: " + ue().history().undo_label());
                check(ue().selection() == std::vector<u32>{4} && ue().move_note().find("за краем") != std::string::npos,
                      "still picked; outside the frame's edge it is said not to show: " + ue().move_note());
                return true;
            }
            case 4:
                check(ue_mv_near(ue().layer_box(4), ue_mv_box_.x, ue_mv_box_.y), "on the screen it has not jumped");
                check(shown("ue-move-note"), "the note under the layers");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same() && ue_mv_parent(4) == 3, "Ctrl+Z: back in «Рамка А», where it was");
                key(SDLK_Y, SDL_KMOD_CTRL);
                check(ue_mv_parent(4) == 7 && ue().history().cursor() == ue_mv_cursor_ + 1, "Ctrl+Y: in «Рамка Б» again");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z");
                return true;
            case 5: {
                // Over a row's top part: above it in the list, so over it in the drawing; into a row it joins the flow.
                check(ue_mv_carry(6, 11, 0.1f), "the picture carried above «Ячейка 2»");
                check(ue_mv_parent(6) == 9 && ue_mv_kids(9) == "10 11 6" && !ue_node(6)->absolute && ue().history().cursor() == ue_mv_cursor_ + 1,
                      "in «Ряд», after «Ячейка 2»: " + ue_mv_kids(9));
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z");
                return true; // the list laid out again
            }
            case 6: {
                // Esc while carrying: nothing.
                check(ue_mv_carry(4, 7, kHalf, false) && ue().layer_dragging(), "carried again");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().layer_dragging() && ue_mv_same(), "Esc: not carried, nothing changed");
                f32 x = 0, y = 0;
                ue_mv_row(7, kHalf, x, y);
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
                check(ue_mv_same() && ue_mv_parent(4) == 3, "letting go after Esc: nothing");
                // A frame into what is inside it: refused.
                check(ue_mv_carry(7, 9, kHalf, false) && ue().layer_dragging() && ue().drop_parent() == 0, "«Рамка Б» onto its own «Ряд»");
                return true;
            }
            case 7: {
                Rml::Element* row = ed_.find_element("ue-layer-9");
                check(row && row->IsClassSet("drop-no") && ue().move_note().find("в саму себя") != std::string::npos,
                      "refused, and why: " + ue().move_note());
                f32 x = 0, y = 0;
                ue_mv_row(9, kHalf, x, y);
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
                check(ue_mv_same(), "let go there: nothing changes");
                // A frame goes with all inside it: «Рамка А» into «Окошко».
                ue_mv_box_ = ue().layer_box(3).value_or(d::Rect{});
                check(ue_mv_carry(3, 12, kHalf) && ue_mv_parent(3) == 12 && ue_mv_kids(3) == "4 6" && ue_mv_kids(4) == "5" &&
                          ue().history().cursor() == ue_mv_cursor_ + 1,
                      "«Рамка А» with its layers into «Окошко»");
                return true;
            }
            case 8: {
                check(ue_mv_near(ue().layer_box(3), ue_mv_box_.x, ue_mv_box_.y) && ue_mv_near(ue().layer_box(4), 220, 280),
                      "it and what is inside stand where they stood");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z");
                // Out of the row onto the screen: above the title's row (the title is the screen's lowest).
                ue_mv_box_ = ue().layer_box(10).value_or(d::Rect{});
                check(ue_mv_carry(10, 2, 0.1f) && ue_mv_parent(10) == 1 && ue_mv_kids(1) == "2 10 3 7",
                      "«Ячейка 1» onto the screen, over the title: " + ue_mv_kids(1));
                return true;
            }
            case 9:
                check(ue_mv_near(ue().layer_box(10), ue_mv_box_.x, ue_mv_box_.y), "where the row had put it: " +
                                                                                       std::to_string(ue().layer_box(10).value_or(d::Rect{}).x));
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z");
                // Where it already is (under the picture, in «Рамка А»): no change, no step.
                check(ue_mv_carry(4, 6, 0.9f) && ue_mv_same(), "let go where it was: nothing changes");
                return true;
            case 10:
                // A cell that fills its row (484 of 680 px: the row's padding, gap and the other cell) taken out onto
                // the screen keeps the size it shows, not its 140 px written for a fixed width.
                ue().select({10});
                check(ue().set_property("width_sizing", "fill"), "«Ячейка 1» fills its row's width");
                ue_mv_fill_json_ = d::save_screen(ue().screen());
                ue_mv_fill_cursor_ = ue().history().cursor();
                return true;
            case 11:
                ue_mv_box_ = ue().layer_box(10).value_or(d::Rect{});
                check(std::fabs(ue_mv_box_.w - 484) < 0.6f && std::fabs(ue_mv_box_.h - 140) < 0.6f,
                      "in the row it is 484 by 140: " + std::to_string(ue_mv_box_.w) + " by " + std::to_string(ue_mv_box_.h));
                check(ue_mv_carry(10, 2, 0.1f) && ue_mv_parent(10) == 1 && ue().history().cursor() == ue_mv_fill_cursor_ + 1,
                      "carried onto the screen over the title, one step");
                return true;
            case 12: {
                const auto b = ue().layer_box(10);
                const d::Node* n = ue_node(10);
                check(b && ue_mv_near(b, ue_mv_box_.x, ue_mv_box_.y) && std::fabs(b->w - ue_mv_box_.w) < 0.6f && std::fabs(b->h - ue_mv_box_.h) < 0.6f,
                      "on the screen it stands and measures as in the row: " +
                          (b ? std::to_string(b->x) + " " + std::to_string(b->y) + " " + std::to_string(b->w) + " " + std::to_string(b->h) : std::string("нет")));
                check(n && n->width_sizing == d::Sizing::Fixed && n->w == 484 && n->h == 140, "its width fixed at what it showed");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(d::save_screen(ue().screen()) == ue_mv_fill_json_ && ue().history().cursor() == ue_mv_fill_cursor_,
                      "Ctrl+Z: back in the row, filling it");
                key(SDLK_Y, SDL_KMOD_CTRL);
                check(ue_mv_parent(10) == 1 && ue_node(10) && ue_node(10)->w == 484, "Ctrl+Y: on the screen again, 484 wide");
                return true;
            }
            case 13: {
                const auto b = ue().layer_box(10);
                check(b && std::fabs(b->w - ue_mv_box_.w) < 0.6f && ue_mv_near(b, ue_mv_box_.x, ue_mv_box_.y), "after Ctrl+Y the same box");
                // Saved and opened again under its Russian name.
                const std::string moved = d::save_screen(ue().screen());
                check(ue().open("main_menu") && ue().open("перенос_пример") && d::save_screen(ue().screen()) == moved,
                      "saved, opened again: as it was moved");
                d::Screen on_disk;
                check(d::load_screen(ue_file(".json", "перенос_пример"), on_disk) && d::save_screen(on_disk) == moved,
                      "the file «перенос_пример.json» holds it");
                usize named = 0;
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(ed_.ui_game_dir / "ui", ec))
                    if (path_to_utf8(e.path().filename()).rfind("перенос_пример.", 0) == 0) ++named;
                check(named == 2, "the screen's files under the Russian name, no others: " + std::to_string(named));
                return true;
            }
            case 14: {
                const auto b = ue().layer_box(10);
                check(b && std::fabs(b->w - ue_mv_box_.w) < 0.6f && ue_mv_near(b, ue_mv_box_.x, ue_mv_box_.y), "reopened: the same box");
                // Back to the example as it came.
                const std::filesystem::path from = utf8_path(FORGE_EXAMPLES_DIR) / "layer-move";
                std::error_code ec;
                for (const char* ext : {".json", ".html"})
                    std::filesystem::copy_file(from / utf8_path(std::string("перенос_пример") + ext),
                                               ed_.ui_game_dir / "ui" / utf8_path(std::string("перенос_пример") + ext),
                                               std::filesystem::copy_options::overwrite_existing, ec);
                check(!ec && ue().open("main_menu") && ue().open("перенос_пример") && d::save_screen(ue().screen()) == ue_mv_orig_,
                      "the example as it came");
                ue_mv_json_ = ue_mv_orig_;
                ue_mv_cursor_ = ue().history().cursor();
                // «Вне раскладки» in the row: let go where it is, it stays so.
                ue().select({10});
                check(ue().set_property("absolute", "true") && ue_node(10)->absolute, "«Ячейка 1» out of the row's flow");
                ue_mv_fill_json_ = d::save_screen(ue().screen());
                ue_mv_fill_cursor_ = ue().history().cursor();
                return true;
            }
            case 15:
                // The list shows the row's cells the other way round: «Ячейка 2» above, «Ячейка 1» under it.
                check(ue_mv_carry(10, 11, 0.9f) && d::save_screen(ue().screen()) == ue_mv_fill_json_ &&
                          ue().history().cursor() == ue_mv_fill_cursor_ && ue_node(10)->absolute,
                      "under «Ячейка 2», where it is: nothing changes, still out of the flow");
                check(ue_mv_carry(11, 10, 0.1f) && d::save_screen(ue().screen()) == ue_mv_fill_json_ &&
                          ue().history().cursor() == ue_mv_fill_cursor_,
                      "«Ячейка 2» over «Ячейка 1», where it is: nothing changes");
                // Truly moved: after «Ячейка 2» it joins the row's flow.
                check(ue_mv_carry(10, 11, 0.1f) && ue_mv_kids(9) == "11 10" && !ue_node(10)->absolute &&
                          ue().history().cursor() == ue_mv_fill_cursor_ + 1,
                      "over «Ячейка 2»: after it, in the flow: " + ue_mv_kids(9));
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(d::save_screen(ue().screen()) == ue_mv_fill_json_ && ue_node(10)->absolute, "Ctrl+Z: back, out of the flow");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z: in the flow as it came");
                return true;
            case 16:
                // A copy of a component filling the row, taken out: the size it showed becomes its own (an override),
                // so the component's size does not come back when the copy is brought up to date (opening again).
                // Made a component while filling (the component 484 wide, the copy no change of its own), then the
                // row narrowed: the copy shows 284.
                ue().select({11});
                check(ue().set_property("width_sizing", "fill") && ue().make_component() && ue().selection().size() == 1,
                      "«Ячейка 2» filling the row made a component, a copy in its place");
                ue_mv_inst_ = ue().selection().empty() ? 0 : ue().selection()[0];
                check(ue_node(ue_mv_inst_) && ue_node(ue_mv_inst_)->overrides.empty(), "the copy has no changes of its own");
                ue().select({9});
                check(ue().set_property("w", "480"), "«Ряд» narrowed to 480");
                return true;
            case 17:
                ue_mv_box_ = ue().layer_box(ue_mv_inst_).value_or(d::Rect{});
                check(std::fabs(ue_mv_box_.w - 284) < 0.6f, "the copy is 284 wide in the row: " + std::to_string(ue_mv_box_.w));
                check(ue_mv_carry(ue_mv_inst_, 2, 0.1f) && ue_mv_parent(ue_mv_inst_) == 1, "the copy carried onto the screen");
                return true;
            case 18: {
                const auto b = ue().layer_box(ue_mv_inst_);
                const d::Node* n = ue_node(ue_mv_inst_);
                check(b && std::fabs(b->w - ue_mv_box_.w) < 0.6f && ue_mv_near(b, ue_mv_box_.x, ue_mv_box_.y), "the copy stands and measures as in the row");
                check(n && std::find(n->overrides.begin(), n->overrides.end(), "size") != n->overrides.end(), "its size kept as its own");
                const std::string moved = d::save_screen(ue().screen());
                check(ue().open("main_menu") && ue().open("перенос_пример") && d::save_screen(ue().screen()) == moved,
                      "opened again (the copy brought up to date): as it was moved");
                return true;
            }
            case 19: {
                const auto b = ue().layer_box(ue_mv_inst_);
                check(b && std::fabs(b->w - ue_mv_box_.w) < 0.6f, "the copy still 284 wide: " + std::to_string(b ? b->w : 0.0f));
                const std::filesystem::path from = utf8_path(FORGE_EXAMPLES_DIR) / "layer-move";
                std::error_code ec;
                for (const char* ext : {".json", ".html"})
                    std::filesystem::copy_file(from / utf8_path(std::string("перенос_пример") + ext),
                                               ed_.ui_game_dir / "ui" / utf8_path(std::string("перенос_пример") + ext),
                                               std::filesystem::copy_options::overwrite_existing, ec);
                check(!ec && ue().open("main_menu") && ue().open("перенос_пример") && d::save_screen(ue().screen()) == ue_mv_orig_,
                      "the example as it came");
                ue_mv_cursor_ = ue().history().cursor();
                ue().select({});
                return true;
            }
            case 20: {
                // Anchored «Пропорционально» into a frame that a row (then a column) stretches: «Окошко» in «Ряд»,
                // its own 140 by 100, filling both ways. The button keeps its box on the screen; its place and size
                // are counted in the frame's shown size, its anchoring stays.
                const bool column = ue_mv_variant_ == 1;
                if (column) {
                    ue().select({9});
                    check(ue().set_property("layout.mode", "column") && ue().set_property("y", "20") && ue().set_property("h", "600"),
                          "«Ряд» a column 600 high");
                }
                check(ue_mv_carry(12, 9, kHalf) && ue_mv_parent(12) == 9 && ue_mv_kids(9) == "10 11 12", "«Окошко» into «Ряд», after the cells");
                ue().select({12});
                check(ue().set_property("w", "140") && ue().set_property("width_sizing", "fill") && ue().set_property("h", "100") &&
                          ue().set_property("height_sizing", "fill"),
                      "«Окошко» 140 by 100 of its own, filling");
                ue().select({4});
                check(ue().set_property("horizontal", "scale") && ue().set_property("vertical", "scale"), "the button anchored «Пропорционально» both ways");
                ue_mv_fill_json_ = d::save_screen(ue().screen());
                ue_mv_fill_cursor_ = ue().history().cursor();
                return true;
            }
            case 21: {
                const bool column = ue_mv_variant_ == 1;
                ue_mv_box_ = ue().layer_box(4).value_or(d::Rect{});
                const d::Rect frame = ue().layer_box(12).value_or(d::Rect{});
                check(ue_mv_near(ue_mv_box_, 220, 280) && std::fabs(ue_mv_box_.w - 240) < 0.6f && std::fabs(ue_mv_box_.h - 80) < 0.6f,
                      "the button at 220, 280, 240 by 80");
                check(column ? frame.w > 141 && frame.h > 101 : frame.w > 141,
                      std::string(column ? "in the column" : "in the row") + " «Окошко» shows " + std::to_string(frame.w) + " by " +
                          std::to_string(frame.h) + " (its own 140 by 100)");
                FORGE_INFO("self-test: «Окошко» %s shows %.2f, %.2f, %.2f by %.2f", column ? "in the column" : "in the row", frame.x, frame.y,
                           frame.w, frame.h);
                check(ue_mv_carry(4, 12, kHalf) && ue_mv_parent(4) == 12 && ue().history().cursor() == ue_mv_fill_cursor_ + 1,
                      "the button carried into «Окошко», one step");
                return true;
            }
            case 22: {
                const auto b = ue().layer_box(4);
                const d::Node* n = ue_node(4);
                const std::string got = b ? std::to_string(b->x) + " " + std::to_string(b->y) + " " + std::to_string(b->w) + " " + std::to_string(b->h)
                                          : std::string("нет");
                check(b && ue_mv_near(b, ue_mv_box_.x, ue_mv_box_.y) && std::fabs(b->w - ue_mv_box_.w) < 0.6f && std::fabs(b->h - ue_mv_box_.h) < 0.6f,
                      "in «Окошко» it stands and measures as before: " + got);
                if (b && n)
                    FORGE_INFO("self-test: the button in «Окошко» shows %.2f, %.2f, %.2f by %.2f; its own x %.2f y %.2f w %.2f h %.2f", b->x, b->y,
                               b->w, b->h, n->x, n->y, n->w, n->h);
                check(n && n->w > 0 && n->h > 0 && n->horizontal == d::Constraint::Scale && n->vertical == d::Constraint::Scale,
                      "its own size above zero, still «Пропорционально»: " + (n ? std::to_string(n->x) + " " + std::to_string(n->w) : std::string()));
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(d::save_screen(ue().screen()) == ue_mv_fill_json_ && ue().history().cursor() == ue_mv_fill_cursor_, "Ctrl+Z: as before");
                key(SDLK_Y, SDL_KMOD_CTRL);
                check(ue_mv_parent(4) == 12, "Ctrl+Y: in «Окошко» again");
                return true;
            }
            case 23: {
                const auto b = ue().layer_box(4);
                check(b && ue_mv_near(b, ue_mv_box_.x, ue_mv_box_.y) && std::fabs(b->w - ue_mv_box_.w) < 0.6f && std::fabs(b->h - ue_mv_box_.h) < 0.6f,
                      "after Ctrl+Y the same box");
                const std::string moved = d::save_screen(ue().screen());
                check(ue().open("main_menu") && ue().open("перенос_пример") && d::save_screen(ue().screen()) == moved, "saved, opened again: the same");
                return true;
            }
            case 24: {
                const auto b = ue().layer_box(4);
                check(b && ue_mv_near(b, ue_mv_box_.x, ue_mv_box_.y) && std::fabs(b->w - ue_mv_box_.w) < 0.6f && std::fabs(b->h - ue_mv_box_.h) < 0.6f,
                      "opened again: the same box");
                const std::filesystem::path from = utf8_path(FORGE_EXAMPLES_DIR) / "layer-move";
                std::error_code ec;
                for (const char* ext : {".json", ".html"})
                    std::filesystem::copy_file(from / utf8_path(std::string("перенос_пример") + ext),
                                               ed_.ui_game_dir / "ui" / utf8_path(std::string("перенос_пример") + ext),
                                               std::filesystem::copy_options::overwrite_existing, ec);
                check(!ec && ue().open("main_menu") && ue().open("перенос_пример") && d::save_screen(ue().screen()) == ue_mv_orig_,
                      "the example as it came");
                ue_mv_cursor_ = ue().history().cursor();
                ue().select({});
                if (ue_mv_variant_ == 0) {
                    ue_mv_variant_ = 1; // again, the frame in a column
                    ue_mv_stage_ = 20;
                } else {
                    ue_mv_variant_ = 0;
                }
                return true;
            }
            case 25: {
                // Frames anchored «Пропорционально» carried into a frame with another layer of another size after it
                // among the same children: taking the carried out of their list moves what follows, and the frame
                // must still be the one they go into. 0: «Рамка А» into «Рамка Б» (on the screen «Окошко» after Б);
                // 1: the title and «Рамка А» together; 2: inside «Рамка Б», «Подпись» into «Ряд» (made free) with
                // «Окошко» after it.
                ue_mv_sources_.clear();
                if (ue_mv_variant2_ <= 1) {
                    check(ue().move_into({12}, 1, 3) && ue_mv_kids(1) == "2 3 7 12", "«Окошко» onto the screen after «Рамка Б»");
                    ue_mv_sources_ = ue_mv_variant2_ == 0 ? std::vector<u32>{3} : std::vector<u32>{2, 3};
                    ue_mv_target_ = 7;
                } else {
                    ue().select({9});
                    check(ue().set_property("layout.mode", "none"), "«Ряд» a free frame");
                    ue_mv_sources_ = {8};
                    ue_mv_target_ = 9;
                }
                for (u32 id : ue_mv_sources_) {
                    ue().select({id});
                    check(ue().set_property("horizontal", "scale") && ue().set_property("vertical", "scale"), "anchored «Пропорционально» both ways");
                }
                ue().select(ue_mv_sources_);
                ue_mv_fill_json_ = d::save_screen(ue().screen());
                ue_mv_fill_cursor_ = ue().history().cursor();
                return true;
            }
            case 26: {
                ue_mv_boxes_.clear();
                for (u32 id : ue_mv_sources_) {
                    ue_mv_boxes_.emplace_back(id, ue().layer_box(id).value_or(d::Rect{}));
                    if (const d::Node* n = ue_node(id))
                        for (const d::Node& c : n->children) ue_mv_boxes_.emplace_back(c.id, ue().layer_box(c.id).value_or(d::Rect{}));
                }
                ue_mv_kids_ = ue_mv_kids(ue_mv_sources_.back());
                check(ue_mv_carry(ue_mv_sources_.back(), ue_mv_target_, kHalf), "carried into the frame");
                bool in = true;
                for (u32 id : ue_mv_sources_) in = in && ue_mv_parent(id) == ue_mv_target_;
                check(in && ue().history().cursor() == ue_mv_fill_cursor_ + 1, "all in, one step: " + ue_mv_kids(ue_mv_target_));
                return true;
            }
            case 27:
            case 28:
            case 29: {
                const int at = ue_mv_stage_ - 1;
                std::string off;
                for (const auto& [id, was] : ue_mv_boxes_) {
                    const auto b = ue().layer_box(id);
                    if (!b || !ue_mv_near(b, was.x, was.y) || std::fabs(b->w - was.w) > 0.6f || std::fabs(b->h - was.h) > 0.6f)
                        off += " " + std::to_string(id) + (b ? " at " + std::to_string(b->x) + " " + std::to_string(b->y) + " " + std::to_string(b->w) +
                                                                   " " + std::to_string(b->h)
                                                             : std::string(" none"));
                }
                bool kept = ue_mv_kids(ue_mv_sources_.back()) == ue_mv_kids_;
                for (u32 id : ue_mv_sources_)
                    if (const d::Node* n = ue_node(id))
                        kept = kept && n->horizontal == d::Constraint::Scale && n->vertical == d::Constraint::Scale && n->w > 0 && n->h > 0;
                    else
                        kept = false;
                check(off.empty() && kept, std::string(at == 27 ? "carried" : at == 28 ? "after Ctrl+Y" : "opened again") +
                                               ": where and as big as before, with its layers and anchoring" + off);
                if (at == 27) {
                    key(SDLK_Z, SDL_KMOD_CTRL);
                    check(d::save_screen(ue().screen()) == ue_mv_fill_json_ && ue().history().cursor() == ue_mv_fill_cursor_, "Ctrl+Z: as before");
                    key(SDLK_Y, SDL_KMOD_CTRL);
                    check(ue_mv_parent(ue_mv_sources_.back()) == ue_mv_target_, "Ctrl+Y: in again");
                } else if (at == 28) {
                    const std::string moved = d::save_screen(ue().screen());
                    check(ue().open("main_menu") && ue().open("перенос_пример") && d::save_screen(ue().screen()) == moved, "saved, opened again: the same");
                } else {
                    const std::filesystem::path from = utf8_path(FORGE_EXAMPLES_DIR) / "layer-move";
                    std::error_code ec;
                    for (const char* ext : {".json", ".html"})
                        std::filesystem::copy_file(from / utf8_path(std::string("перенос_пример") + ext),
                                                   ed_.ui_game_dir / "ui" / utf8_path(std::string("перенос_пример") + ext),
                                                   std::filesystem::copy_options::overwrite_existing, ec);
                    check(!ec && ue().open("main_menu") && ue().open("перенос_пример") && d::save_screen(ue().screen()) == ue_mv_orig_,
                          "the example as it came");
                    ue_mv_cursor_ = ue().history().cursor();
                    ue().select({});
                    if (++ue_mv_variant2_ <= 2) ue_mv_stage_ = 25;
                    else ue_mv_variant2_ = 0;
                }
                return true;
            }
            case 30:
                // Two picked: both go, both stay picked.
                ue().select({4, 6});
                check(ue_mv_carry(4, 7, kHalf) && ue_mv_parent(4) == 7 && ue_mv_parent(6) == 7 && ue_mv_kids(7) == "8 9 12 4 6" &&
                          ue().selection() == std::vector<u32>{4, 6} && ue().history().cursor() == ue_mv_cursor_ + 1,
                      "two picked layers carried into «Рамка Б» together, still picked: " + ue_mv_kids(7));
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z");
                // Carried away and back onto its own row, let go: nothing changes, both stay picked (no click on the row).
                ue().select({4, 6});
                {
                    f32 x = 0, y = 0;
                    check(ue_mv_carry(4, 8, kHalf, false) && ue_mv_row(4, kHalf, x, y), "carried towards «Подпись»");
                    mouse(SDL_EVENT_MOUSE_MOTION, x, y);
                    mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
                    check(ue_mv_same() && ue().selection() == std::vector<u32>{4, 6}, "back onto its own row: nothing changes, both still picked");
                }
                // The canvas, zoomed and moved.
                ue().set_zoom(0.62f, 300, 200);
                mouse(SDL_EVENT_MOUSE_MOTION, ue_wx(900), ue_wy(900));
                ue_wheel(-1);
                return true;
            case 31:
                check(std::fabs(ue().zoom() - 0.62f) < 1e-4f, "zoomed to 62 %");
                // Passing over frames without resting: it only moves. Over «Рамка Б» for a frame or two (the editor's
                // update runs between), shorter than the rest that takes it in.
                ue().select({3});
                check(ue_mv_take(4, 1300, 300), "the button taken on the canvas, over «Рамка Б»");
                ue_mv_t_ = time_now_ns();
                return true;
            case 32: return true;
            case 33: {
                const f64 passed = ns_to_ms(time_now_ns() - ue_mv_t_) / 1000.0;
                if (passed < 0.45)
                    check(ue().drop_parent() == 0 && !shown("ue-drop"),
                          "over «Рамка Б» for " + std::to_string(passed) + " s: not taken in, no outline");
                else
                    FORGE_INFO("self-test: frames too slow (%.2f s) to pass over «Рамка Б» quicker than the rest", passed);
                for (int i = 1; i <= 4; ++i) mouse(SDL_EVENT_MOUSE_MOTION, ue_wx(1300 - 175.0f * static_cast<f32>(i)), ue_wy(300 + 75.0f * static_cast<f32>(i)));
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, ue_wx(600), ue_wy(600));
                if (passed < 0.45)
                    check(ue_mv_parent(4) == 3 && ue().history().undo_label() == "Сдвинуто" && ue().history().cursor() == ue_mv_cursor_ + 1,
                          "passed over «Рамка Б»: still in «Рамка А», moved: " + ue().history().undo_label());
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z");
                return true;
            }
            case 34:
                // Rested over «Окошко» (inside «Рамка Б»): it would go there.
                ue().select({3}); // a click in «Рамка А» picks what is in it
                check(ue_mv_take(4, 1500, 380), "taken to «Окошко»");
                return true;
            case 35:
                if (wait(ue().drop_parent() == 12, "resting over «Окошко»: it would go in")) return true;
                check(ue().canvas_moving() && ue().history().cursor() == ue_mv_cursor_, "held: nothing in the history");
                return true;
            case 36: {
                Rml::Element* label = ed_.find_element("ue-drop-label");
                check(shown("ue-drop") && label && label->GetInnerRML() == "В рамку «Окошко»",
                      "«Окошко» outlined and named: " + (label ? label->GetInnerRML() : std::string("нет")));
                ue_mv_box_ = ue().layer_box(4).value_or(d::Rect{});
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, ue_mv_x_, ue_mv_y_);
                check(ue_mv_parent(4) == 12 && ue().history().cursor() == ue_mv_cursor_ + 1 && ue().history().undo_label() == "Перенесено в «Окошко»",
                      "let go: in «Окошко», one step");
                const d::Node* b = ue_node(4);
                check(b && b->x == ue_mv_box_.x - 1420 && b->y == ue_mv_box_.y - 300, "its place counted from «Окошко» (in «Рамка Б»)");
                return true;
            }
            case 37:
                check(ue_mv_near(ue().layer_box(4), ue_mv_box_.x, ue_mv_box_.y), "where it was let go, no jump");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z: back in «Рамка А», where it was");
                key(SDLK_Y, SDL_KMOD_CTRL);
                check(ue_mv_parent(4) == 12, "Ctrl+Y");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z");
                return true;
            case 38:
                ue().select({3}); // a click in «Рамка А» picks what is in it
                check(ue_mv_take(4, 1500, 380), "taken to «Окошко» again");
                return true;
            case 39:
                if (wait(ue().drop_parent() == 12, "resting over «Окошко» again")) return true;
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().canvas_moving() && ue_mv_same(), "Esc: all of it back, no step");
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, ue_mv_x_, ue_mv_y_);
                check(ue_mv_same(), "letting go after Esc: nothing");
                return true;
            case 40:
                // A layer of a list stays in it: resting over another frame does not offer it, it only moves.
                check(ue().make_list(6, d::ListSource::Items), "the picture made a list");
                ue_mv_listed_ = d::save_screen(ue().screen());
                return true;
            case 41: {
                const d::Node* list = ue_node(ue_mv_parent(6));
                const u32 empty = list && list->children.size() == 2 ? list->children[1].id : 0;
                ue().select({list ? list->id : 0});
                check(empty && ue_mv_take(empty, 1300, 300) && ue().selection() == std::vector<u32>{empty},
                      "the empty list's text taken on the canvas, over «Рамка Б»");
                ue_mv_t_ = time_now_ns();
                return true;
            }
            case 42:
                if (ns_to_ms(time_now_ns() - ue_mv_t_) < 1000.0 * (UiEditor::drop_wait_seconds() + 0.3)) {
                    --ue_mv_stage_;
                    return true;
                }
                check(ue().canvas_moving() && ue().drop_parent() == 0 && !shown("ue-drop"), "rested over «Рамка Б»: a list's layer is not offered");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, ue_mv_x_, ue_mv_y_);
                check(d::save_screen(ue().screen()) == ue_mv_listed_, "Esc: the list as it was");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z: no list");
                return true;
            case 43:
                // Into the row, between its cells.
                ue().select({3});
                check(ue_mv_take(6, 1200, 670), "the picture taken between the row's cells");
                return true;
            case 44:
                if (wait(ue().drop_parent() == 9, "resting over «Ряд»")) return true;
                check(ue().drop_index() == 1, "it would go between the cells: " + std::to_string(ue().drop_index()));
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, ue_mv_x_, ue_mv_y_);
                check(ue_mv_kids(9) == "10 6 11" && ue().history().cursor() == ue_mv_cursor_ + 1, "in «Ряд», second: " + ue_mv_kids(9));
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue_mv_same(), "Ctrl+Z");
                return true;
            case 45:
                // What a frame hides is not a place: «Ряд» lower, sticking out of «Рамка Б» below.
                ue().select({9});
                check(ue().set_property("y", "560"), "«Ряд» lower, past «Рамка Б»'s edge");
                ue_mv_json_ = d::save_screen(ue().screen());
                ue_mv_cursor_ = ue().history().cursor();
                return true;
            case 46:
                ue().select({3}); // a click in «Рамка А» picks what is in it
                check(ue_mv_take(4, 1300, 900), "the button taken to the hidden part of «Ряд»");
                return true;
            case 47: {
                if (wait(ue().drop_parent() != 0, "resting there")) return true;
                check(ue().drop_parent() == 1, "there it would go onto the screen, not into «Ряд»: " + std::to_string(ue().drop_parent()));
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, ue_mv_x_, ue_mv_y_);
                check(ue_mv_same(), "Esc");
                ue().select({});
                left_click(ue_wx(1300), ue_wy(900));
                const std::vector<u32>& sel = ue().selection();
                check(std::find(sel.begin(), sel.end(), 9u) == sel.end() && std::find(sel.begin(), sel.end(), 7u) == sel.end(),
                      "a click on the hidden part picks neither «Ряд» nor «Рамка Б»");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(d::save_screen(ue().screen()) == ue_mv_orig_, "Ctrl+Z: «Ряд» back");
                ue_mv_json_ = ue_mv_orig_;
                ue_mv_cursor_ = ue().history().cursor();
                return true;
            }
            case 48:
                // At last: the button into «Рамка Б» on the canvas, kept.
                ue().select({3}); // a click in «Рамка А» picks what is in it
                check(ue_mv_take(4, 1200, 320), "the button taken into «Рамка Б»");
                return true;
            case 49:
                if (wait(ue().drop_parent() == 7, "resting over «Рамка Б»")) return true;
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, ue_mv_x_, ue_mv_y_);
                check(ue_mv_parent(4) == 7, "in «Рамка Б»");
                return true;
            case 50: {
                ue_mv_box_ = ue().layer_box(4).value_or(d::Rect{});
                ue_mv_json_ = d::save_screen(ue().screen());
                check(ue().open("main_menu") && ue().open("перенос_пример") && d::save_screen(ue().screen()) == ue_mv_json_ &&
                          ue_mv_parent(4) == 7,
                      "saved: reopened, the button in «Рамка Б»");
                check(ue_file(".html", "перенос_пример").find("id=\"n4\"") != std::string::npos, "and on the page");
                check(click("ue-mode-simple") && ue().simple() && d::save_screen(ue().screen()) == ue_mv_json_, "«Простой»: nothing changes");
                return true;
            }
            case 51:
                check(shown("ue-layer-4") && ue_mv_parent(4) == 7, "in «Простой» the button is in «Рамка Б» too");
                check(click("ue-mode-full") && !ue().simple() && d::save_screen(ue().screen()) == ue_mv_json_, "«Полный» again");
                check(click("ue-check") && ue().checking(), "«Проверить»");
                return true;
            case 52:
                ue_mv_presses_ = ue().check_vars().get("demo.presses").number();
                left_click(ue_wx(ue_mv_box_.cx()), ue_wy(ue_mv_box_.cy()));
                return true;
            case 53:
                check(ue().check_vars().get("demo.presses").number() == ue_mv_presses_ + 1, "the button pressed where it is now");
                left_click(ue_wx(220 + 120), ue_wy(280 + 40));
                return true;
            case 54:
                check(ue().check_vars().get("demo.presses").number() == ue_mv_presses_ + 1, "nothing at its old place");
                ue().set_checking(false);
                ue().set_view(5);
                check(click("ue-check") && ue().checking() && ue().view() == 5, "«Проверить» on 4:3");
                return true;
            case 55: {
                const auto b = ue().layer_box(4);
                check(b.has_value(), "the button on 4:3");
                if (b) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return true;
            }
            case 56:
                check(ue().check_vars().get("demo.presses").number() == ue_mv_presses_ + 2, "pressed on 4:3");
                ue().set_checking(false);
                ue().set_view(4);
                check(click("ue-check") && ue().checking() && ue().view() == 4, "«Проверить» on 21:9");
                return true;
            case 57: {
                const auto b = ue().layer_box(4);
                if (b) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return true;
            }
            case 58:
                check(ue().check_vars().get("demo.presses").number() == ue_mv_presses_ + 3, "pressed on 21:9");
                ue().set_checking(false);
                ue().set_view(4);
                // On a player's screen of another size layers are not moved between frames: said, nothing changes.
                check(!ue().move_into({6}, 7, 0) && ue().move_note().find("«Макет»") != std::string::npos &&
                          d::save_screen(ue().screen()) == ue_mv_json_,
                      "on 21:9 not moved: " + ue().move_note());
                ue().set_view(0);
                check(ue().open("main_menu"), "back to the menu");
                break;
            default: break;
            }
            ue_mv_stage_ = 0;
            break;
        }
        case 146: {
            // The screen's sound (13.7) on games/examples/screen-sound, as the author does it: a music and the buttons'
            // sound picked in the panel («Полный» and «Простой»), a file of «Ресурсы» copied into the game's sounds,
            // Ctrl+Z and Ctrl+Y, the page written; in «Проверить» the music plays once and stops when left, a button
            // sounds once per press; selecting and dragging on the canvas sound nothing.
            namespace d = editor::design;
            auto wait = [&](bool ready, const char* what) {
                if (!hold(ready, what)) return false;
                --ue_sd_stage_;
                return true;
            };
            auto choices = [&](const std::string& value) {
                for (const auto& [v, name] : ue().sound_choices())
                    if (v == value) return name;
                return std::string("<none>");
            };
            const audio::ScreenSounds& snd = ue().check_sound();
            audio::Mixer& mixer = ue().check_mixer();
            switch (ue_sd_stage_++) {
            case 0: {
                const std::filesystem::path from = utf8_path(FORGE_EXAMPLES_DIR) / "screen-sound";
                std::error_code ec;
                for (const char* name : {"звук_меню", "звук_игра", "звук_окно"})
                    for (const char* ext : {".json", ".html"})
                        std::filesystem::copy_file(from / utf8_path(std::string(name) + ext), ed_.ui_game_dir / "ui" / utf8_path(std::string(name) + ext),
                                                   std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::create_directories(ue().sounds_folder(), ec);
                for (const char* name : {"мелодия меню.wav", "мелодия игры.wav", "мелодия окна.wav", "щелчок.wav", "звон.wav"})
                    std::filesystem::copy_file(from / "sounds" / utf8_path(name), ue().sounds_folder() / utf8_path(name),
                                               std::filesystem::copy_options::overwrite_existing, ec);
                // A sound of «Ресурсы», not yet the game's.
                ue_sd_extra_ = std::filesystem::temp_directory_path() / utf8_path("forge_editor_ресурсы") / utf8_path("колокол.wav");
                std::filesystem::create_directories(ue_sd_extra_.parent_path(), ec);
                std::filesystem::copy_file(from / "sounds" / utf8_path("звон.wav"), ue_sd_extra_, std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(ue().sounds_folder() / utf8_path("колокол.wav"), ec);
                ue_sd_list_ = ue().list_sounds;
                const std::filesystem::path extra = ue_sd_extra_;
                ue().list_sounds = [extra] { return std::vector<std::filesystem::path>{extra}; };
                check(!ec && ue().open("звук_меню"), "the example's menu opens");
                check(ue().view() == 0 && !ue().simple() && !ue().checking(), "in «Макет», «Полный»");
                check(ue().screen().music == "мелодия меню.wav" && ue().screen().button_sound == "щелчок.wav", "its music and buttons' sound");
                check(choices("мелодия окна.wav") == "мелодия окна" && choices("звон.wav") == "звон" &&
                          choices("add:" + path_to_utf8(ue_sd_extra_)) == "колокол — из «Ресурсов»",
                      "the drop-downs offer the game's sounds and those of «Ресурсы»");
                ue().select({});
                check(click("ue-tab-game"), "the screen's «В игре»");
                ue_sd_json_ = d::save_screen(ue().screen());
                ue_sd_cursor_ = ue().history().cursor();
                ue_sd_starts_ = snd.music_starts();
                ue_sd_clicks_ = snd.clicks();
                return true;
            }
            case 1:
                if (wait(ue_field("ue-screen-music") == "мелодия меню.wav", "the panel's drop-downs filled")) return true;
                check(ue_field("ue-screen-music") == "мелодия меню.wav" && ue_field("ue-screen-button-sound") == "щелчок.wav",
                      "the panel shows the music and the buttons' sound: " + ue_field("ue-screen-music") + " / " + ue_field("ue-screen-button-sound"));
                check(ue_sd_open("ue-screen-music"), "a click opens «Музыка»");
                return true;
            case 2:
                check(ue_option("ue-screen-music", "мелодия окна.wav") && ue().screen().music == "мелодия окна.wav", "another music picked");
                check(ue().history().cursor() == ue_sd_cursor_ + 1 && ue().history().undo_label() == "Изменено: Музыка экрана",
                      "one step: " + ue().history().undo_label());
                check(ue_file(".html", "звук_меню").find("forge-music=\"мелодия окна.wav\"") != std::string::npos, "the page says it");
                ue().undo();
                check(d::save_screen(ue().screen()) == ue_sd_json_, "Ctrl+Z: the music as it was");
                ue().redo();
                check(ue().screen().music == "мелодия окна.wav", "Ctrl+Y: picked again");
                ue().undo();
                return true;
            case 3: {
                const u32 quiet = ue_named("Тихо");
                ue().select({quiet});
                check(ue_node(quiet) && ue_node(quiet)->click_sound == "none", "«Тихо» has no sound");
                return true;
            }
            case 4:
                check(ue_field("ue-click-sound") == "none", "its «Звук»: «Без звука»");
                check(ue_sd_open("ue-click-sound"), "a click opens its «Звук»");
                return true;
            case 5: {
                const u32 quiet = ue_named("Тихо");
                check(ue_option("ue-click-sound", "add:" + path_to_utf8(ue_sd_extra_)), "a sound of «Ресурсы» picked");
                std::error_code ec;
                check(ue_node(quiet)->click_sound == "колокол.wav" && std::filesystem::exists(ue().sounds_folder() / utf8_path("колокол.wav"), ec),
                      "copied into the game's sounds under its Russian name, the button's now");
                check(ue().history().cursor() == ue_sd_cursor_ + 1 && ue().history().undo_label() == "Изменено: Звук нажатия",
                      "one step: " + ue().history().undo_label());
                check(choices("колокол.wav") == "колокол" && choices("add:" + path_to_utf8(ue_sd_extra_)) == "<none>",
                      "the list has it as the game's own now");
                ue().undo();
                check(ue_node(quiet)->click_sound == "none" && d::save_screen(ue().screen()) == ue_sd_json_, "Ctrl+Z: no sound again");
                ue().redo();
                check(ue_node(quiet)->click_sound == "колокол.wav", "Ctrl+Y: the bell");
                // Taken off: «Как у экрана».
                check(ue().set_property("click_sound", "") && ue_node(quiet)->click_sound.empty(), "back to the screen's sound");
                ue().undo();
                ue().undo();
                check(d::save_screen(ue().screen()) == ue_sd_json_ && ue().history().cursor() == ue_sd_cursor_, "all taken back");
                // What the game does not read is not taken.
                check(!ue().set_property("click_sound", "песня.mp3") && !ue().set_property("click_sound", "../звон.wav") &&
                          d::save_screen(ue().screen()) == ue_sd_json_,
                      "an MP3 or a path out of the sounds folder is not taken");
                ue().set_simple(true);
                return true;
            }
            case 6: {
                // «Простой»: the same fields, in plain words.
                check(ue().simple() && d::save_screen(ue().screen()) == ue_sd_json_, "«Простой»: nothing changed");
                ue().select({ue_named("Звон")});
                return true;
            }
            case 7:
                check(ue_field("ue-s-click-sound") == "звон.wav", "«Звон»'s own sound shown in «Простой»");
                ue().select({});
                return true;
            case 8:
                check(ue_field("ue-s-music") == "мелодия меню.wav" && ue_field("ue-s-button-sound") == "щелчок.wav",
                      "the screen's music and buttons' sound in «Простой»");
                check(ue_sd_open("ue-s-button-sound"), "a click opens «Кнопки»");
                return true;
            case 9:
                check(ue_option("ue-s-button-sound", "звон.wav") && ue().screen().button_sound == "звон.wav", "picked in «Простой»");
                check(ue().history().cursor() == ue_sd_cursor_ + 1, "one step");
                ue().undo();
                check(d::save_screen(ue().screen()) == ue_sd_json_, "Ctrl+Z");
                ue().set_simple(false);
                check(d::save_screen(ue().screen()) == ue_sd_json_, "back in «Полный»: everything kept");
                // Saved and opened again: the same.
                check(ue().open("main_menu") && ue().open("звук_меню") && d::save_screen(ue().screen()) == ue_sd_json_ &&
                          ue_file(".json", "звук_меню").find("\"click_sound\": \"none\"") != std::string::npos,
                      "opened again: the music and sounds as saved");
                return true;
            case 10: {
                // Selecting and dragging a button on the canvas: no sound.
                const u32 button = ue_named("Нажми");
                const auto b = ue().layer_box(button);
                check(b.has_value(), "the button on the canvas");
                if (b) {
                    left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                    ue_drag(ue_wx(b->cx()), ue_wy(b->cy()), ue_wx(b->cx() + 40), ue_wy(b->cy() + 20));
                }
                return true;
            }
            case 11:
                check(ue().selection().size() == 1 && ue().history().cursor() == ue_sd_cursor_ + 1,
                      "picked and dragged on the canvas: one step: " + std::to_string(ue().history().cursor() - ue_sd_cursor_) + " " +
                          std::to_string(ue().selection().size()));
                check(snd.clicks() == ue_sd_clicks_ && snd.music_starts() == ue_sd_starts_ && mixer.voices() == 0,
                      "no sound while editing");
                ue().undo();
                check(d::save_screen(ue().screen()) == ue_sd_json_, "Ctrl+Z puts it back");
                ue().select({});
                check(click("ue-check") && ue().checking(), "«Проверить»");
                for (const char* v : {"demo.presses", "demo.rings", "demo.quiet"}) ue().check_vars().set(v, 0);
                return true;
            case 12:
                check(snd.music_name() == "мелодия меню.wav" && snd.music_starts() == ue_sd_starts_ + 1 && mixer.voices(audio::Bus::Music) == 1,
                      "«Проверить» plays the menu's music once: " + snd.music_name());
                check(ue_sd_music_peak() > 0.05f, "and it is heard (the mix is not silent)");
                if (const auto b = ue().layer_box(ue_named("Нажми"))) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return true;
            case 13:
                check(ue().check_vars().get("demo.presses").number() == 1 && snd.clicks() == ue_sd_clicks_ + 1 && snd.last_click() == "щелчок.wav",
                      "«Нажми» in «Проверить»: once, with the screen's click: " + std::to_string(ue().check_vars().get("demo.presses").number()) +
                          " " + std::to_string(snd.clicks() - ue_sd_clicks_) + " " + snd.last_click());
                if (const auto b = ue().layer_box(ue_named("Звон"))) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return true;
            case 14:
                check(ue().check_vars().get("demo.rings").number() == 1 && snd.clicks() == ue_sd_clicks_ + 2 && snd.last_click() == "звон.wav",
                      "«Звон»: its own sound");
                if (const auto b = ue().layer_box(ue_named("Тихо"))) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return true;
            case 15:
                check(ue().check_vars().get("demo.quiet").number() == 1 && snd.clicks() == ue_sd_clicks_ + 2, "«Тихо»: no sound");
                // The page built again (another player's screen): the music goes on, not twice.
                ue().set_view(5);
                return true;
            case 16:
                check(snd.music_starts() == ue_sd_starts_ + 1 && mixer.voices(audio::Bus::Music) == 1, "the page built again: the same music, one");
                ue().set_view(0);
                ue().set_checking(false);
                return true;
            case 17:
                check(!ue().checking() && snd.music_name().empty() && mixer.voices(audio::Bus::Music) == 0, "out of «Проверить»: the music stops");
                // The window over the game: its music while it is open, the game's when it closes.
                check(ue().open("звук_игра") && click("ue-check") && ue().checking(), "«Проверить» on the screen over the game");
                return true;
            case 18:
                check(snd.music_name() == "мелодия игры.wav" && snd.music_starts() == ue_sd_starts_ + 2, "the game's music");
                if (const auto b = ue().layer_box(ue_named("Окно"))) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return true;
            case 19:
                if (wait(ue().check_windows() == std::vector<std::string>{"звук_окно"} && snd.music_name() == "мелодия окна.wav",
                         "the window opens over the game with its music"))
                    return true;
                check(snd.music_starts() == ue_sd_starts_ + 3 && mixer.voices(audio::Bus::Music) == 1 && snd.last_click() == "щелчок.wav",
                      "«Окно»: a click, then the window's music instead of the game's");
                ue_sd_clicks_ = snd.clicks();
                if (const auto b = ue().check_box("звук_окно", "Закрыть")) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return true;
            case 20:
                if (wait(ue().opened() == "звук_игра" && ue().check_windows().empty(), "«Закрыть» closes the window")) return true;
                check(snd.clicks() == ue_sd_clicks_ && snd.music_name() == "мелодия игры.wav" && mixer.voices(audio::Bus::Music) == 1,
                      "«Закрыть» without a sound, the game's music again");
                ue().set_checking(false);
                return true;
            case 21:
                check(snd.music_name().empty() && mixer.voices(audio::Bus::Music) == 0, "out of «Проверить»: silence");
                // The keyboard in «Проверить», as in the game: on the menu, through the editor's way in.
                check(ue().open("звук_меню") && click("ue-check") && ue().checking(), "«Проверить» on the menu again");
                for (const char* v : {"demo.presses", "demo.rings", "demo.quiet"}) ue().check_vars().set(v, 0);
                return true;
            case 22:
                if (wait(snd.music_name() == "мелодия меню.wav", "the menu's music")) return true;
                if (const auto b = ue().layer_box(ue_named("Нажми"))) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return true;
            case 23:
                check(ue_sd_var("demo.presses") == 1, "the mouse presses «Нажми» and gives it the focus");
                ue_sd_clicks_ = snd.clicks();
                ue_sd_key(SDLK_RETURN, true);
                ue_sd_key(SDLK_RETURN, false);
                return true;
            case 24:
                check(ue_sd_var("demo.presses") == 2 && snd.clicks() == ue_sd_clicks_ + 1 && snd.last_click() == "щелчок.wav",
                      "Enter presses the focused button once, with its sound: " + std::to_string(ue_sd_var("demo.presses")) + " " +
                          std::to_string(snd.clicks() - ue_sd_clicks_));
                // Held: the repeats and the key going up press nothing more.
                ue_sd_key(SDLK_RETURN, true);
                ue_sd_key(SDLK_RETURN, true, true);
                ue_sd_key(SDLK_RETURN, true, true);
                ue_sd_key(SDLK_RETURN, false);
                return true;
            case 25:
                check(ue_sd_var("demo.presses") == 3 && snd.clicks() == ue_sd_clicks_ + 2,
                      "Enter held: once, not with its repeats nor going up: " + std::to_string(ue_sd_var("demo.presses")));
                ue_sd_key(SDLK_SPACE, true);
                ue_sd_key(SDLK_SPACE, false);
                return true;
            case 26:
                check(ue_sd_var("demo.presses") == 4 && snd.clicks() == ue_sd_clicks_ + 3, "Space presses once, with its sound");
                ue_sd_key(SDLK_KP_ENTER, true);
                ue_sd_key(SDLK_KP_ENTER, false);
                return true;
            case 27:
                check(ue_sd_var("demo.presses") == 5 && snd.clicks() == ue_sd_clicks_ + 4, "keypad Enter presses once, with its sound");
                // With Ctrl it is the editor's key, not the button's.
                ue_sd_key(SDLK_RETURN, true, false, SDL_KMOD_LCTRL);
                ue_sd_key(SDLK_RETURN, false, false, SDL_KMOD_LCTRL);
                // Tab: the next button has the focus.
                ue_sd_key(SDLK_TAB, true);
                ue_sd_key(SDLK_TAB, false);
                return true;
            case 28:
                check(ue_sd_var("demo.presses") == 5 && snd.clicks() == ue_sd_clicks_ + 4, "Ctrl+Enter does not press");
                ue_sd_key(SDLK_RETURN, true);
                ue_sd_key(SDLK_RETURN, false);
                return true;
            case 29:
                check(ue_sd_var("demo.rings") == 1 && ue_sd_var("demo.presses") == 5 && snd.last_click() == "звон.wav",
                      "Tab gives «Звон» the focus, Enter rings it: " + std::to_string(ue_sd_var("demo.rings")));
                // Another tab: the hidden check is silent, its keys go nowhere.
                ue_sd_starts_ = snd.music_starts();
                ue_sd_clicks_ = snd.clicks();
                check(click_tab(0) && ed_.tab() == "level", "the Level tab");
                return true;
            case 30:
                if (wait(snd.music_name().empty(), "on another tab the check's music stops")) return true;
                check(mixer.voices(audio::Bus::Music) == 0, "no music voice on another tab");
                check(ue_sd_music_peak() < 1e-4f, "and nothing is heard: " + std::to_string(ue_sd_music_peak()));
                ue_sd_key(SDLK_RETURN, true);
                ue_sd_key(SDLK_RETURN, false);
                check(ue_sd_var("demo.rings") == 1 && snd.clicks() == ue_sd_clicks_, "Enter on another tab presses nothing");
                check(click_tab(8) && ed_.tab() == "ui" && ue().checking(), "back on «Интерфейс», still in «Проверить»");
                return true;
            case 31:
                if (wait(mixer.voices(audio::Bus::Music) == 1, "the music comes back")) return true;
                check(snd.music_name() == "мелодия меню.wav" && snd.music_starts() == ue_sd_starts_ + 1 && ue_sd_music_peak() > 0.05f,
                      "back: the menu's music, one, heard");
                // Again: away and back.
                check(click_tab(0), "away again");
                return true;
            case 32:
                check(mixer.voices(audio::Bus::Music) == 0 && ue_sd_music_peak() < 1e-4f, "away again: silent");
                check(click_tab(8), "back again");
                return true;
            case 33:
                if (wait(mixer.voices(audio::Bus::Music) == 1, "the music comes back again")) return true;
                check(snd.music_starts() == ue_sd_starts_ + 2 && mixer.voices(audio::Bus::Music) == 1, "back again: one music, not two");
                check(click("ue-check") && !ue().checking(), "out of «Проверить» with its button");
                return true;
            case 34:
                check(mixer.voices(audio::Bus::Music) == 0 && snd.music_name().empty(), "out of «Проверить»: the music stops");
                ue_sd_clicks_ = snd.clicks();
                ue_sd_key(SDLK_RETURN, true);
                ue_sd_key(SDLK_RETURN, false);
                check(ue_sd_var("demo.rings") == 1 && ue_sd_var("demo.presses") == 5 && snd.clicks() == ue_sd_clicks_,
                      "out of «Проверить»: Enter does not press the page's button");
                // Over the game no button takes the keys: Space and Enter stay the game's.
                check(ue().open("звук_игра") && click("ue-check") && ue().checking(), "«Проверить» on the screen over the game");
                return true;
            case 35:
                if (const auto b = ue().layer_box(ue_named("Окно"))) {
                    // The mouse over the button without pressing it, then the keys.
                    SDL_Event e{};
                    e.type = SDL_EVENT_MOUSE_MOTION;
                    e.motion.x = ue_wx(b->cx());
                    e.motion.y = ue_wy(b->cy());
                    ed_.handle_event(e);
                }
                for (SDL_Keycode k : {SDLK_SPACE, SDLK_RETURN, SDLK_KP_ENTER}) {
                    ue_sd_key(k, true);
                    ue_sd_key(k, false);
                }
                return true;
            case 36:
                check(ue().opened() == "звук_игра" && snd.clicks() == ue_sd_clicks_, "over the game Space and Enter press no button");
                ue().set_checking(false);
                // «В главное меню» on a window in «Проверить»: the game's menu comes back, as in the game.
                check(ue().open("звук_окно"), "the window");
                ue().select({ue_named("Закрыть")});
                check(ue().set_property("click.0.kind", "menu") && ue().set_property("click_sound", "none"), "«Закрыть» goes to the main menu");
                ue().select({});
                check(click("ue-check") && ue().checking(), "«Проверить» on the window");
                return true;
            case 37:
                if (const auto b = ue().layer_box(ue_named("Закрыть"))) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return true;
            case 38:
                if (wait(ue().opened() == "main_menu", "«В главное меню» opens the main menu")) return true;
                check(ue().checking(), "still in «Проверить», now on the menu");
                ue().set_checking(false);
                ue().list_sounds = ue_sd_list_;
                check(ue().open("main_menu"), "back to the menu");
                break;
            default: break;
            }
            ue_sd_stage_ = 0;
            break;
        }
        case 147: {
            // «Создать +» and the right button's menus (13.8), with the mouse and keys through the editor's own way in:
            // each kind made from «Создать» (one step, selected, where nothing is yet), the layer's menu on the canvas
            // and on its row in the list, the screen's menu and «Вставить сюда», the order of the layers (refused in a
            // list and in a copy of a component), hide and lock, Esc and a click aside closing a menu, «Простой» and
            // «Проверить».
            namespace d = editor::design;
            auto wait = [&](bool ready, const char* what) {
                if (!hold(ready, what)) return false;
                --ue_cm_stage_;
                return true;
            };
            const d::Node& root = ue().screen().root;
            auto index_of = [&](u32 id) {
                for (usize i = 0; i < root.children.size(); ++i)
                    if (root.children[i].id == id) return static_cast<int>(i);
                return -1;
            };
            auto top_named = [&](const char* prefix) -> u32 {
                for (const d::Node& c : root.children)
                    if (c.name.rfind(prefix, 0) == 0) return c.id;
                return 0;
            };
            auto steps = [&] { return ue().history().cursor(); };
            static const char* const kKinds[] = {"text", "picture", "bar", "list", "frame", "rectangle", "ellipse"};
            switch (ue_cm_stage_++) {
            case 0: {
                ue().set_simple(false);
                ue_cm_screen_ = ue().new_screen();
                ue().select({});
                check(ue().opened() == ue_cm_screen_ && root.children.empty() && ue().view() == 0 && !ue().checking() && ue().menu().empty(),
                      "a new empty screen, «Макет», «Полный», no menu");
                ue_cm_cursor_ = steps();
                f32 x = 0, y = 0;
                check(shown("ue-create") && !shown("ue-create-menu") && element_center("ue-create", x, y), "«Создать» over the canvas");
                left_click(x, y);
                return true;
            }
            case 1:
                if (wait(shown("ue-new-button"), "the create menu is laid out")) return true;
                return true; // its place is settled a frame later
            case 2: {
                Rml::Element* b = ed_.find_element("ue-create");
                check(ue().menu() == "create" && b && b->IsClassSet("on"), "a click on «Создать» opens its menu, the button lit");
                bool all = true;
                for (const char* id : {"ue-new-button", "ue-new-text", "ue-new-picture", "ue-new-bar", "ue-new-list", "ue-new-frame",
                                       "ue-new-rectangle", "ue-new-ellipse"})
                    all = all && shown(id);
                check(all, "elements (button, text, picture, bar, list) and shapes (frame, rectangle, ellipse)");
                const usize comps = d::components(ue().library()).size();
                check(comps > 0 ? shown("ue-new-comp-" + std::to_string(comps - 1)) && !shown("ue-new-comp-" + std::to_string(comps)) &&
                                      !shown("ue-create-no-comps")
                                : shown("ue-create-no-comps"),
                      "and each of the game's components: " + std::to_string(comps));
                f32 mx = 0, my = 0, bx = 0, by = 0;
                const bool placed = b && ue_cm_corner("ue-create-menu", mx, my) && ue_cm_corner("ue-create", bx, by);
                check(placed && std::fabs(mx - bx) < 1.5f && my >= by + b->GetOffsetHeight() && my <= by + b->GetOffsetHeight() + 8,
                      "under the button: menu " + std::to_string(mx) + "," + std::to_string(my) + ", button " + std::to_string(bx) + "," +
                          std::to_string(by) + " +" + std::to_string(b ? b->GetOffsetHeight() : 0));
                check(ue_cm_press("ue-new-button"), "a click on «Кнопка»");
                return true;
            }
            case 3: {
                const d::Node* n = root.children.size() == 1 ? &root.children[0] : nullptr;
                check(ue().menu().empty(), "the menu closes");
                check(n && d::block_of(*n) == d::Block::Button && n->name == "Кнопка 1" && ue().selection() == std::vector<u32>{n->id},
                      "a button made, selected");
                check(steps() == ue_cm_cursor_ + 1 && ue().history().undo_label() == "Добавлено: Кнопка", "one step: " + ue().history().undo_label());
                // As a block of «Простой»: the free place nearest the middle, on a grid of 40.
                check(n && std::fabs(n->x + n->w * 0.5f - ue().screen().width * 0.5f) <= 40 && std::fabs(n->y + n->h * 0.5f - ue().screen().height * 0.5f) <= 40,
                      "in the middle of the screen");
                ue_cm_btn_ = n ? n->id : 0;
                ue_cm_key(SDLK_Z);
                check(root.children.empty(), "Ctrl+Z: gone");
                ue_cm_key(SDLK_Y);
                check(root.children.size() == 1 && root.children[0].id == ue_cm_btn_, "Ctrl+Y: back");
                ue_cm_kind_ = 0;
                return true;
            }
            case 4: {
                // Each other kind, «Создать» opened again by a click.
                const std::string id = std::string("ue-new-") + kKinds[ue_cm_kind_];
                if (ue().menu() != "create") {
                    check(click("ue-create") && ue().menu() == "create", "«Создать» again");
                    --ue_cm_stage_;
                    return true;
                }
                if (wait(shown(id), "the create menu is laid out")) return true;
                ue_cm_count_ = root.children.size();
                ue_cm_cursor_ = steps();
                check(ue_cm_press(id.c_str()), "a click on " + id);
                return true;
            }
            case 5: {
                const std::string kind = kKinds[ue_cm_kind_];
                const d::Node* n = root.children.size() == ue_cm_count_ + 1 ? &root.children.back() : nullptr;
                bool ok = n && ue().menu().empty() && ue().selection() == std::vector<u32>{n->id} && steps() == ue_cm_cursor_ + 1 &&
                          ue().history().undo_label() == "Добавлено: " + (n->name.rfind(' ') != std::string::npos ? n->name.substr(0, n->name.rfind(' ')) : n->name);
                if (n) {
                    if (kind == "frame") ok = ok && n->type == d::NodeType::Frame && n->children.empty() && !n->fills.empty() && n->name == "Рамка 1";
                    else if (kind == "rectangle") ok = ok && n->type == d::NodeType::Rectangle && n->name == "Прямоугольник 1";
                    else if (kind == "ellipse") ok = ok && n->type == d::NodeType::Ellipse && n->name == "Эллипс 1";
                    else ok = ok && kind == d::block_key(d::block_of(*n));
                    for (usize i = 0; ok && i + 1 < root.children.size(); ++i) {
                        const d::Node& c = root.children[i];
                        ok = !(n->x < c.x + c.w && c.x < n->x + n->w && n->y < c.y + c.h && c.y < n->y + n->h);
                    }
                }
                check(ok, "«" + kind + "» from «Создать»: one step (" + ue().history().undo_label() + "), selected, where nothing is yet, the menu closed");
                if (++ue_cm_kind_ < static_cast<int>(std::size(kKinds))) ue_cm_stage_ = 4;
                return true;
            }
            case 6:
                // The layer's menu on the canvas.
                ue().select({});
                ue_cm_cursor_ = steps();
                check(ue_cm_right(ue_cm_btn_), "the right button on the button");
                return true;
            case 7:
                if (wait(shown("ue-ctx-cut"), "the layer's menu is laid out")) return true;
                return true;
            case 8: {
                check(ue().menu() == "layer" && ue().selection() == std::vector<u32>{ue_cm_btn_}, "it selects the button and opens the layer's menu");
                bool all = true;
                for (const char* id : {"ue-ctx-cut", "ue-ctx-copy", "ue-ctx-paste", "ue-ctx-duplicate", "ue-ctx-delete", "ue-ctx-up", "ue-ctx-down",
                                       "ue-ctx-top", "ue-ctx-bottom", "ue-ctx-frame", "ue-ctx-hide", "ue-ctx-lock", "ue-ctx-component"})
                    all = all && shown(id);
                check(all && !shown("ue-ctx-edit-component") && !shown("ue-ctx-detach") && !shown("ue-ctx-all") && !shown("ue-create-menu"),
                      "cut, copy, paste, duplicate, delete; the order; frame, hide, lock; make a component");
                Rml::Element* paste = ed_.find_element("ue-ctx-paste");
                check(paste && paste->IsClassSet("disabled") == !ue().can_paste(), "«Вставить» greyed only while nothing is copied");
                check(ue_cm_at_mouse("ue-ctx"), "the menu is at the mouse");
                check(ue_cm_press("ue-ctx-copy"), "«Копировать»");
                return true;
            }
            case 9:
                check(ue().menu().empty() && ue().can_paste() && steps() == ue_cm_cursor_, "copied: the menu closed, no step");
                // On nothing: the screen's menu.
                ue_cm_x_ = ue_wx(12);
                ue_cm_y_ = ue_wy(12);
                right_click(ue_cm_x_, ue_cm_y_);
                return true;
            case 10:
                if (wait(shown("ue-ctx-all"), "the screen's menu is laid out")) return true;
                return true;
            case 11: {
                check(ue().menu() == "empty" && ue().selection().empty(), "on nothing: the screen's menu, nothing selected");
                check(shown("ue-ctx-create") && shown("ue-ctx-paste-here") && shown("ue-ctx-all") && !shown("ue-ctx-cut") && !shown("ue-ctx-hide"),
                      "«Создать…», «Вставить сюда», «Выделить всё»");
                Rml::Element* here = ed_.find_element("ue-ctx-paste-here");
                check(here && !here->IsClassSet("disabled") && ue_cm_at_mouse("ue-ctx"), "«Вставить сюда» lit, the menu at the mouse");
                ue_cm_count_ = root.children.size();
                check(ue_cm_press("ue-ctx-paste-here"), "«Вставить сюда»");
                return true;
            }
            case 12: {
                const d::Node* n = root.children.size() == ue_cm_count_ + 1 ? &root.children.back() : nullptr;
                const d::Node* b = ue_node(ue_cm_btn_);
                check(n && b && n->id != b->id && n->name == b->name && n->w == b->w && n->h == b->h && n->x == 12 && n->y == 12 &&
                          ue().selection() == std::vector<u32>{n->id},
                      "a copy of the button with its corner where the right button was, selected");
                check(steps() == ue_cm_cursor_ + 1 && ue().history().undo_label() == "Вставлено", "one step: " + ue().history().undo_label());
                ue_cm_cursor_ = steps();
                // «Создать…» from the screen's menu, at the screen's right edge.
                ue_cm_x_ = ue_wx(ue().screen().width - 8);
                ue_cm_y_ = ue_wy(8);
                right_click(ue_cm_x_, ue_cm_y_);
                return true;
            }
            case 13:
                if (wait(shown("ue-ctx-create"), "the screen's menu is laid out")) return true;
                check(ue().menu() == "empty" && ue_cm_press("ue-ctx-create"), "«Создать…»");
                return true;
            case 14:
                if (wait(shown("ue-new-ellipse"), "the create menu is laid out")) return true;
                return true;
            case 15:
                check(ue().menu() == "create" && !shown("ue-ctx") && ue_cm_at_mouse("ue-create-menu"),
                      "«Создать…» opens the create menu in its place, whole in the tab");
                ue_cm_key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(ue().menu().empty() && ue().selection().empty() && steps() == ue_cm_cursor_, "Esc closes it, nothing else");
                return true;
            case 16:
                check(!shown("ue-create-menu") && !shown("ue-ctx"), "the menus are gone");
                // The order: the button is the lowest layer.
                check(index_of(ue_cm_btn_) == 0, "the button is the lowest layer");
                ue().select({});
                check(ue_cm_right(ue_cm_btn_), "its menu");
                return true;
            case 17:
                if (wait(shown("ue-ctx-top"), "the layer's menu is laid out")) return true;
                check(ue_cm_press("ue-ctx-top"), "«На самый верх»");
                return true;
            case 18:
                check(index_of(ue_cm_btn_) == static_cast<int>(root.children.size()) - 1 && steps() == ue_cm_cursor_ + 1 &&
                          ue().history().undo_label() == "На самый верх",
                      "the button on top, one step: " + ue().history().undo_label());
                ue_cm_cursor_ = steps();
                check(ue_cm_right(ue_cm_btn_), "its menu again");
                return true;
            case 19:
                if (wait(shown("ue-ctx-up"), "the layer's menu is laid out")) return true;
                check(ue_cm_press("ue-ctx-up"), "«Выше» on the top layer");
                return true;
            case 20: {
                const int top = static_cast<int>(root.children.size()) - 1;
                check(index_of(ue_cm_btn_) == top && steps() == ue_cm_cursor_ && ue().menu().empty(), "already on top: no step");
                ue_cm_key(SDLK_LEFTBRACKET);
                check(index_of(ue_cm_btn_) == top - 1 && ue().history().undo_label() == "Ниже", "Ctrl+[: one lower");
                ue_cm_key(SDLK_LEFTBRACKET, static_cast<SDL_Keymod>(SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT));
                check(index_of(ue_cm_btn_) == 0 && ue().history().undo_label() == "В самый низ", "Ctrl+Shift+[: the lowest");
                ue_cm_key(SDLK_RIGHTBRACKET);
                check(index_of(ue_cm_btn_) == 1 && ue().history().undo_label() == "Выше" && steps() == ue_cm_cursor_ + 3, "Ctrl+]: one higher");
                for (int i = 0; i < 3; ++i) ue_cm_key(SDLK_Z);
                check(index_of(ue_cm_btn_) == top && steps() == ue_cm_cursor_, "Ctrl+Z three times: on top again");
                // Two layers together keep their order.
                const u32 text = top_named("Текст"), picture = top_named("Картинка");
                check(index_of(text) == 0 && index_of(picture) == 1, "the text and the picture are the two lowest");
                ue().select({text, picture});
                ue_cm_key(SDLK_RIGHTBRACKET);
                check(index_of(text) == 1 && index_of(picture) == 2 && steps() == ue_cm_cursor_ + 1, "both one higher together, in their order");
                ue_cm_key(SDLK_Z);
                check(index_of(text) == 0 && index_of(picture) == 1 && steps() == ue_cm_cursor_, "Ctrl+Z: back");
                // In a list the order is the list's.
                const d::Node* list = ue_node(top_named("Список"));
                check(list && !list->children.empty(), "the list has its cell");
                if (list && !list->children.empty()) ue().select({list->children[0].id});
                ue_cm_key(SDLK_RIGHTBRACKET);
                check(steps() == ue_cm_cursor_ && ue().move_note().find("В списке") != std::string::npos, "refused in a list: " + ue().move_note());
                // A copy of a component from «Создать».
                ue().select({});
                check(click("ue-create"), "«Создать»");
                return true;
            }
            case 21: {
                const usize comps = d::components(ue().library()).size();
                if (comps == 0) {
                    ue_cm_stage_ = 25; // no component in this game: nothing to place
                    return true;
                }
                if (wait(shown("ue-new-comp-0"), "the create menu is laid out")) return true;
                ue_cm_count_ = root.children.size();
                ue_cm_cursor_ = steps();
                check(ue_cm_press("ue-new-comp-0"), "a click on the first component");
                return true;
            }
            case 22: {
                const std::string first = d::components(ue().library())[0].name;
                const d::Node* n = root.children.size() == ue_cm_count_ + 1 ? &root.children.back() : nullptr;
                check(n && n->component == first && ue().selection() == std::vector<u32>{n->id} && steps() == ue_cm_cursor_ + 1 && ue().menu().empty(),
                      "a copy of «" + first + "» placed, selected, one step");
                ue_cm_inst_ = n ? n->id : 0;
                ue_cm_cursor_ = steps();
                if (n && !n->children.empty()) {
                    ue().select({n->children[0].id});
                    ue_cm_key(SDLK_RIGHTBRACKET);
                    check(steps() == ue_cm_cursor_ && ue().move_note().find("компонента") != std::string::npos,
                          "the copy's layers keep the component's order: " + ue().move_note());
                }
                ue().select({});
                check(ue_cm_right(ue_cm_inst_), "the right button on the copy");
                return true;
            }
            case 23:
                if (wait(shown("ue-ctx-detach"), "the layer's menu is laid out")) return true;
                check(ue().selection() == std::vector<u32>{ue_cm_inst_} && shown("ue-ctx-edit-component") && !shown("ue-ctx-component"),
                      "a copy's menu: «Открыть компонент», «Отвязать от компонента»");
                check(ue_cm_press("ue-ctx-detach"), "«Отвязать от компонента»");
                return true;
            case 24: {
                const d::Node* n = ue_node(ue_cm_inst_);
                check(n && n->component.empty() && steps() == ue_cm_cursor_ + 1, "detached, one step: " + ue().history().undo_label());
                ue_cm_key(SDLK_Z);
                n = ue_node(ue_cm_inst_);
                check(n && !n->component.empty() && steps() == ue_cm_cursor_, "Ctrl+Z: a copy again");
                return true;
            }
            case 25: {
                // Hide and lock from the row's menu in the list of layers.
                ue().select({ue_cm_btn_});
                ue_cm_cursor_ = steps();
                check(ue_cm_right_row(top_named("Прямоугольник")), "the right button on the rectangle's row");
                return true;
            }
            case 26:
                if (wait(shown("ue-ctx-hide"), "the layer's menu is laid out")) return true;
                return true;
            case 27: {
                const u32 rect = top_named("Прямоугольник");
                check(ue().menu() == "layer" && ue().selection() == std::vector<u32>{rect}, "the row's layer selected, its menu open");
                check(ue_cm_at_mouse("ue-ctx"), "the menu at the mouse");
                check(ue_cm_press("ue-ctx-hide"), "«Скрыть»");
                return true;
            }
            case 28: {
                const d::Node* r = ue_node(top_named("Прямоугольник"));
                check(r && !r->visible && steps() == ue_cm_cursor_ + 1 && ue().history().undo_label() == "Скрыт слой", "hidden, one step");
                check(ue_cm_right_row(top_named("Прямоугольник")), "its menu again");
                return true;
            }
            case 29: {
                if (wait(shown("ue-ctx-hide"), "the layer's menu is laid out")) return true;
                Rml::Element* hide = ed_.find_element("ue-ctx-hide");
                check(hide && hide->GetInnerRML().find("Показать") != std::string::npos, "now it offers «Показать»");
                check(ue_cm_press("ue-ctx-lock"), "«Закрепить»");
                return true;
            }
            case 30: {
                const d::Node* r = ue_node(top_named("Прямоугольник"));
                check(r && r->locked && steps() == ue_cm_cursor_ + 2 && ue().history().undo_label() == "Слой закреплён", "locked, one step");
                ue_cm_key(SDLK_Z);
                ue_cm_key(SDLK_Z);
                r = ue_node(top_named("Прямоугольник"));
                check(r && r->visible && !r->locked && steps() == ue_cm_cursor_, "Ctrl+Z twice: shown and free");
                // A click aside closes a menu and does nothing else.
                ue().select({ue_cm_btn_});
                ue_cm_sel_ = ue().selection();
                check(click("ue-create") && ue().menu() == "create", "«Создать» open");
                return true;
            }
            case 31: {
                if (wait(shown("ue-new-ellipse"), "the create menu is laid out")) return true;
                const auto t = ue().layer_box(top_named("Текст"));
                check(t.has_value(), "the text on the canvas");
                if (t) left_click(ue_wx(t->cx()), ue_wy(t->cy()));
                return true;
            }
            case 32:
                check(ue().menu().empty() && ue().selection() == ue_cm_sel_ && steps() == ue_cm_cursor_,
                      "a click on the canvas beside the menu closes it: the text not selected, no step");
                // The wheel moves the view: the menu would point at nothing.
                check(ue_cm_right(ue_cm_btn_) && ue().menu() == "layer", "the button's menu");
                {
                    SDL_Event w{};
                    w.type = SDL_EVENT_MOUSE_WHEEL;
                    w.wheel.y = 1;
                    w.wheel.mouse_x = ue_cm_x_;
                    w.wheel.mouse_y = ue_cm_y_;
                    ed_.handle_event(w);
                }
                check(ue().menu().empty() && steps() == ue_cm_cursor_, "the wheel over the canvas closes it");
                ue().set_simple(true);
                return true;
            case 33:
                check(ue().simple() && !shown("ue-create"), "«Простой»: no «Создать» (its own blocks are there)");
                ue().select({});
                check(ue_cm_right(ue_cm_btn_), "the right button on the button");
                return true;
            case 34:
                if (wait(shown("ue-ctx-hide"), "the layer's menu is laid out")) return true;
                check(shown("ue-ctx-copy") && shown("ue-ctx-top") && !shown("ue-ctx-frame") && !shown("ue-ctx-component"),
                      "«Простой»: the layer's menu without frames and components");
                ue_cm_key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(ue().menu().empty() && steps() == ue_cm_cursor_, "Esc closes it");
                ue().set_simple(false);
                ue().select({});
                check(click("ue-check") && ue().checking(), "«Проверить»");
                return true;
            case 35:
                ue_cm_right(ue_cm_btn_);
                click("ue-create");
                return true;
            case 36:
                check(ue().checking() && ue().menu().empty() && !shown("ue-ctx") && !shown("ue-create-menu") && steps() == ue_cm_cursor_,
                      "«Проверить»: neither the right button nor «Создать» opens a menu");
                ue().set_checking(false);
                check(ue().open("main_menu"), "back to the menu");
                break;
            default: break;
            }
            ue_cm_stage_ = 0;
            break;
        }
        case 148: {
            // The panel over the selected layer (13.10), with the mouse and keys through the editor's own way in: over
            // one text, button or picture in «Полный», 10 px above it (under it with no room above, gone when it is out
            // of sight); not for several layers, the screen, «Простой», «Проверить», a player's size or a carried
            // layer. Each change is one step Ctrl+Z takes back, a field given what it shows makes none, and the
            // panel's clicks, keys and hover stay its own.
            namespace d = editor::design;
            auto wait = [&](bool ready, const char* what) {
                if (!hold(ready, what)) return false;
                --ue_fl_stage_;
                return true;
            };
            const d::Node& root = ue().screen().root;
            auto steps = [&] { return ue().history().cursor(); };
            auto only = [&](u32 id) { return ue().selection() == std::vector<u32>{id}; };
            // The wheel over the canvas's far corner (away from the panel): the view moves by dy × 60 px.
            auto wheel = [&](f32 dy) {
                Rml::Element* canvas = ed_.find_element("ue-canvas");
                const Rml::Vector2f size = canvas ? canvas->GetBox().GetSize(Rml::BoxArea::Border) : Rml::Vector2f(0, 0);
                const f32 x = ue().canvas_left() + size.x - 30, y = ue().canvas_top() + size.y - 30;
                mouse(SDL_EVENT_MOUSE_MOTION, x, y);
                SDL_Event w{};
                w.type = SDL_EVENT_MOUSE_WHEEL;
                w.wheel.y = dy;
                w.wheel.mouse_x = x;
                w.wheel.mouse_y = y;
                ed_.handle_event(w);
            };
            std::string why;
            f32 px = 0, py = 0, pw = 0, ph = 0;
            switch (ue_fl_stage_++) {
            case 0: {
                ue().set_simple(false);
                ue().set_view(0);
                ue().new_screen();
                ue_fl_text_ = ue().create("text");
                ue_fl_btn_ = ue().create("button");
                ue_fl_pic_ = ue().create("picture");
                check(ue_fl_text_ && ue_fl_btn_ && ue_fl_pic_ && root.children.size() == 3, "a new screen with a text, a button and a picture");
                // The text just over the button (the button's panel will lie over it), the picture away from both.
                for (const auto& [id, x, y] : {std::tuple{ue_fl_btn_, "200", "500"}, std::tuple{ue_fl_text_, "200", "430"},
                                               std::tuple{ue_fl_pic_, "1300", "500"}}) {
                    ue().select({id});
                    ue().set_property("x", x);
                    ue().set_property("y", y);
                }
                // The text follows a text style of the game's: a field given what it shows keeps it so.
                const std::vector<d::NamedTextStyle>& styles = ue().library().text_styles;
                ue().select({ue_fl_text_});
                check(!styles.empty() && ue().set_property("text_style", styles[0].key), "the text takes the game's text style");
                ue().select({});
                return true;
            }
            case 1: {
                const auto t = ue().layer_box(ue_fl_text_), b = ue().layer_box(ue_fl_btn_);
                check(t && b && t->y + t->h <= b->y && t->x == b->x, "the text lies just over the button");
                check(ue().float_panel().empty() && !shown("ue-float"), "nothing selected: no panel");
                ue_fl_json_ = d::save_screen(ue().screen());
                ue_fl_cursor_ = steps();
                check(ue_fl_click(ue_fl_text_), "a click on the text");
                return true;
            }
            case 2: {
                if (wait(shown("ue-f-family"), "the text's panel is laid out")) return true;
                const d::Node* n = ue_node(ue_fl_text_);
                check(only(ue_fl_text_) && ue().float_panel() == "text" && ue_fl_box(px, py, pw, ph) && std::fabs(pw - 320) < 0.5f &&
                          std::fabs(ph - (2 * 7 + 2 * 32 + 6)) < 0.5f,
                      "the text selected: its panel, 320 px wide, two rows");
                check(shown("ue-f-size") && shown("ue-f-weight") && shown("ue-f-color") && shown("ue-f-align-justify") && !shown("ue-f-action") &&
                          !shown("ue-f-picture") && !shown("ue-f-sound"),
                      "font, size, weight, colour and alignment; nothing of a button's or a picture's");
                {
                    const bool placed = ue_fl_placed(ue_fl_text_, true, why);
                    check(placed, "10 px above the text, centred on it: " + why);
                }
                const f32 size = n ? n->text_style.size : 0;
                const std::string shown_size = size == std::floor(size) ? std::to_string(static_cast<int>(size)) : std::to_string(size);
                check(n && !n->text_style.style.empty() && ue_field("ue-f-size") == shown_size,
                      "the size field shows the text's size, its text style's: " + ue_field("ue-f-size") + " / " + shown_size);
                check(steps() == ue_fl_cursor_, "selecting is no step");
                check(ue_type("ue-f-size", shown_size) && steps() == ue_fl_cursor_ && n && !ue_node(ue_fl_text_)->text_style.style.empty(),
                      "its size typed as it is: no step, the text still follows its style");
                check(ue_open("ue-f-family"), "a click opens the fonts");
                return true;
            }
            case 3: {
                const std::optional<std::string> font = ue_fl_other("ue-f-family");
                ue_fl_pick_ = font.value_or("?");
                check(font && ue_option("ue-f-family", *font) && ue_option_hovered_, "a click on the font «" + ue_fl_pick_ + "», the pointer finds it");
                const d::Node* n = ue_node(ue_fl_text_);
                check(n && n->text_style.family == ue_fl_pick_ && steps() == ue_fl_cursor_ + 1 && ue().history().undo_label() == "Изменено: Шрифт",
                      "the text's font, one step: " + ue().history().undo_label());
                check(only(ue_fl_text_) && root.children.size() == 3, "the click is the panel's: the text still selected");
                return true; // the fonts' list goes on the next frame
            }
            case 4:
                check(ue_open("ue-f-weight"), "a click opens the weights");
                return true;
            case 5: {
                const d::Node* n = ue_node(ue_fl_text_);
                const std::string weight = n && n->text_style.weight == 700 ? "400" : "700";
                check(ue_option("ue-f-weight", weight) && ue_option_hovered_, "a click on the weight " + weight);
                n = ue_node(ue_fl_text_);
                check(n && std::to_string(n->text_style.weight) == weight && steps() == ue_fl_cursor_ + 2 &&
                          ue().history().undo_label() == "Изменено: Жирность",
                      "the weight, one step: " + ue().history().undo_label());
                // Typed: Backspace and the digits are the field's (the text stays), Enter keeps the size.
                check(ue_type("ue-f-size", "48"), "48 typed into the size");
                n = ue_node(ue_fl_text_);
                check(n && n->text_style.size == 48 && steps() == ue_fl_cursor_ + 3 && ue().history().undo_label() == "Изменено: Размер текста" &&
                          root.children.size() == 3 && only(ue_fl_text_),
                      "the size 48, one step; the keys did nothing else: " + ue().history().undo_label());
                return true;
            }
            case 6: {
                check(ue_field("ue-f-size") == "48", "the field shows 48: " + ue_field("ue-f-size"));
                check(ue_type("ue-f-size", "48") && steps() == ue_fl_cursor_ + 3, "48 typed again: no step");
                check(ue_cm_press("ue-f-align-center"), "a click on «По центру»");
                const d::Node* n = ue_node(ue_fl_text_);
                check(n && n->text_style.align == d::TextAlign::Center && steps() == ue_fl_cursor_ + 4 &&
                          ue().history().undo_label() == "Изменено: Выравнивание текста" && only(ue_fl_text_),
                      "centred, one step, the text still selected: " + ue().history().undo_label());
                return true;
            }
            case 7: {
                Rml::Element* center = ed_.find_element("ue-f-align-center");
                Rml::Element* left = ed_.find_element("ue-f-align-left");
                check(center && left && center->IsClassSet("selected") && !left->IsClassSet("selected"), "«По центру» lit");
                check(ue_cm_press("ue-f-color") && ue().picker_open() && ue().picker_field() == "text_color", "the colour opens the picker on the text's colour");
                ue_cm_key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().picker_open() && steps() == ue_fl_cursor_ + 4 && only(ue_fl_text_), "Esc puts it away: no step");
                for (int i = 0; i < 4; ++i) ue_cm_key(SDLK_Z);
                check(d::save_screen(ue().screen()) == ue_fl_json_ && steps() == ue_fl_cursor_, "Ctrl+Z four times: the text as it was");
                // Where it does not show.
                ue().select({ue_fl_text_, ue_fl_btn_});
                check(ue().float_panel().empty(), "two layers: no panel");
                ue().select({root.id});
                check(ue().float_panel().empty(), "the screen: no panel");
                ue().select({ue_fl_text_});
                check(ue().float_panel() == "text", "the text again: its panel");
                ue().set_simple(true);
                check(only(ue_fl_text_) && ue().float_panel().empty(), "«Простой»: no panel (its own panel has all of it)");
                ue().set_simple(false);
                check(ue().float_panel() == "text", "«Полный» again: the panel");
                ue().set_view(1);
                check(ue().float_panel().empty(), "a player's size: no panel");
                ue().set_view(0);
                check(ue().float_panel() == "text", "«Макет» again: the panel");
                check(click("ue-check") && ue().checking() && ue().float_panel().empty(), "«Проверить»: no panel");
                ue().set_checking(false);
                ue().select({ue_fl_text_});
                check(ue().float_panel() == "text", "back from «Проверить»: the panel");
                return true;
            }
            case 8: {
                if (wait(shown("ue-f-family"), "the text's panel is laid out")) return true;
                // Carried: gone while it moves, back over it where it is let go.
                const auto t = ue().layer_box(ue_fl_text_);
                check(t.has_value(), "the text's box");
                if (!t) break;
                const f32 x = ue_wx(t->cx()), y = ue_wy(t->cy());
                mouse(SDL_EVENT_MOUSE_MOTION, x, y);
                mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
                check(ue().float_panel() == "text", "pressed, not moved yet: the panel stays");
                {
                    // The view moved while it is held: still not carried, the panel stays.
                    SDL_Event w{};
                    w.type = SDL_EVENT_MOUSE_WHEEL;
                    w.wheel.y = -0.5f;
                    w.wheel.mouse_x = x;
                    w.wheel.mouse_y = y;
                    ed_.handle_event(w);
                }
                check(!ue().canvas_moving() && ue().float_panel() == "text", "the view moved while pressed: the panel stays");
                mouse(SDL_EVENT_MOUSE_MOTION, x - 40, y - 40);
                check(ue().canvas_moving() && ue().float_panel().empty(), "carried: no panel");
                mouse(SDL_EVENT_MOUSE_BUTTON_UP, x - 40, y - 40);
                check(!ue().canvas_moving() && ue().float_panel() == "text" && steps() == ue_fl_cursor_ + 1, "let go: one step, the panel back");
                return true;
            }
            case 9: {
                if (wait(shown("ue-f-family"), "the text's panel is laid out")) return true;
                {
                    const bool placed = ue_fl_placed(ue_fl_text_, true, why);
                    check(placed, "over the text where it is now: " + why);
                }
                ue_cm_key(SDLK_Z);
                check(d::save_screen(ue().screen()) == ue_fl_json_ && steps() == ue_fl_cursor_, "Ctrl+Z: the text back");
                // No room above: the view moved until the text's top is 30 px under the canvas's top.
                const auto t = ue().layer_box(ue_fl_text_);
                check(t.has_value(), "the text's box");
                if (t) wheel(-(ue().to_canvas_y(t->y) - 30) / 60);
                return true;
            }
            case 10: {
                if (wait(shown("ue-f-family"), "the text's panel is laid out")) return true;
                const auto t = ue().layer_box(ue_fl_text_);
                check(t && ue_fl_box(px, py, pw, ph) && ue().to_canvas_y(t->y) >= 0 && ue().to_canvas_y(t->y) < 10 + ph + 4,
                      "the text's top near the canvas's top: " + std::to_string(t ? ue().to_canvas_y(t->y) : -1));
                {
                    const bool placed = ue_fl_placed(ue_fl_text_, false, why);
                    check(placed, "no room above: under the text, past its size: " + why);
                }
                // Out of sight above the canvas: none.
                if (t) wheel(-(ue().to_canvas_y(t->y + t->h) + 40) / 60);
                check(only(ue_fl_text_) && ue().float_panel().empty(), "the text out of sight: no panel");
                ue().zoom_to_fit();
                check(ue().float_panel() == "text", "fitted again: the panel");
                ue_fl_json_ = d::save_screen(ue().screen());
                ue_fl_cursor_ = steps();
                check(ue_fl_click(ue_fl_btn_), "a click on the button");
                return true;
            }
            case 11: {
                if (wait(shown("ue-f-action"), "the button's panel is laid out")) return true;
                check(only(ue_fl_btn_) && ue().float_panel() == "button" && ue_fl_box(px, py, pw, ph) && std::fabs(pw - 300) < 0.5f &&
                          std::fabs(ph - (2 * 7 + 32)) < 0.5f,
                      "the button: its panel, 300 px wide, one row");
                check(ue_field("ue-f-action") == "none" && !shown("ue-f-sound") && !shown("ue-f-target") && !shown("ue-f-message") &&
                          !shown("ue-f-family") && !shown("ue-f-many"),
                      "«Ничего» on a click; no sound, screen or message yet");
                {
                    const bool placed = ue_fl_placed(ue_fl_btn_, true, why);
                    check(placed, "above the button: " + why);
                }
                const auto p = ue().layer_box(ue_fl_pic_);
                if (p) mouse(SDL_EVENT_MOUSE_MOTION, ue_wx(p->cx()), ue_wy(p->cy()));
                return true;
            }
            case 12: {
                check(shown("ue-hover"), "the pointer over the picture: it is outlined");
                // Over the panel, the text under it is not.
                const auto t = ue().layer_box(ue_fl_text_);
                check(t && ue_fl_box(px, py, pw, ph), "the text and the panel");
                if (!t) break;
                const f32 l = std::max(px, ue_wx(t->x)), r = std::min(px + pw, ue_wx(t->x + t->w));
                const f32 top = std::max(py, ue_wy(t->y)), bottom = std::min(py + ph, ue_wy(t->y + t->h));
                check(l + 4 < r && top + 4 < bottom, "the panel lies over the text");
                mouse(SDL_EVENT_MOUSE_MOTION, (l + r) * 0.5f, (top + bottom) * 0.5f);
                return true;
            }
            case 13: {
                check(!shown("ue-hover"), "the pointer over the panel: the text under it is not outlined");
                // The panel's own clicks: its edge, the right button on it.
                check(ue_fl_box(px, py, pw, ph), "the panel");
                left_click(px + 3, py + 3);
                check(only(ue_fl_btn_) && steps() == ue_fl_cursor_ && root.children.size() == 3, "a click on the panel's edge: the button stays selected");
                right_click(px + 3, py + 3);
                check(ue().menu().empty() && only(ue_fl_btn_), "the right button on the panel: no menu");
                check(ue_open("ue-f-action"), "a click opens the actions");
                return true;
            }
            case 14: {
                check(ue_option("ue-f-action", "show") && ue_option_hovered_, "a click on «Открыть экран»");
                const d::Node* n = ue_node(ue_fl_btn_);
                check(n && n->on_click.size() == 1 && n->on_click[0].kind == d::ActionKind::Show && !n->on_click[0].target.empty() &&
                          n->on_click[0].target != ue().opened() && steps() == ue_fl_cursor_ + 1 && ue().history().undo_label() == "Изменено: При нажатии",
                      "the button opens another screen, one step: " + ue().history().undo_label());
                return true;
            }
            case 15: {
                if (wait(shown("ue-f-target") && shown("ue-f-sound"), "the screen and the sound under the action")) return true;
                const d::Node* n = ue_node(ue_fl_btn_);
                check(ue_fl_box(px, py, pw, ph) && std::fabs(ph - (2 * 7 + 3 * 32 + 2 * 6)) < 0.5f && n && ue_field("ue-f-target") == n->on_click[0].target,
                      "three rows: the action, its screen, the sound");
                {
                    const bool placed = ue_fl_placed(ue_fl_btn_, true, why);
                    check(placed, "still 10 px above the button: " + why);
                }
                check(ue_open("ue-f-sound"), "a click opens the sounds");
                return true;
            }
            case 16: {
                check(ue_option("ue-f-sound", "none") && ue_option_hovered_, "a click on «Без звука»");
                const d::Node* n = ue_node(ue_fl_btn_);
                check(n && n->click_sound == "none" && steps() == ue_fl_cursor_ + 2 && ue().history().undo_label() == "Изменено: Звук нажатия",
                      "no sound on its click, one step: " + ue().history().undo_label());
                check(ue_open("ue-f-action"), "the actions again");
                return true;
            }
            case 17: {
                check(ue_option("ue-f-action", "message"), "a click on «Сообщение «Логике»»");
                const d::Node* n = ue_node(ue_fl_btn_);
                check(n && n->on_click.size() == 1 && n->on_click[0].kind == d::ActionKind::Message && n->on_click[0].target.empty() &&
                          steps() == ue_fl_cursor_ + 3,
                      "a message, one step");
                return true;
            }
            case 18: {
                if (wait(shown("ue-f-message") && !shown("ue-f-target"), "the message's field instead of the screen")) return true;
                check(ue_field("ue-f-sound") == "none", "the sound still «Без звука»: " + ue_field("ue-f-sound"));
                check(ue_type("ue-f-message", "открыть_дверь"), "the message typed");
                const d::Node* n = ue_node(ue_fl_btn_);
                check(n && n->on_click[0].target == "открыть_дверь" && steps() == ue_fl_cursor_ + 4 && root.children.size() == 3 && only(ue_fl_btn_),
                      "the message kept, one step; the keys did nothing else");
                for (int i = 0; i < 4; ++i) ue_cm_key(SDLK_Z);
                check(d::save_screen(ue().screen()) == ue_fl_json_ && steps() == ue_fl_cursor_, "Ctrl+Z four times: the button as it was");
                // Two actions: the panel names them and keeps the sound.
                ue().select({ue_fl_btn_});
                check(ue().set_property("click.add", "") && ue().set_property("click.add", ""), "two actions from «В игре»");
                return true;
            }
            case 19: {
                if (wait(shown("ue-f-many"), "the note on several actions")) return true;
                const bool placed = ue_fl_placed(ue_fl_btn_, true, why);
                check(!shown("ue-f-action") && shown("ue-f-sound") && placed, "no action list, the sound; above it: " + why);
                ue_cm_key(SDLK_Z);
                ue_cm_key(SDLK_Z);
                check(d::save_screen(ue().screen()) == ue_fl_json_ && steps() == ue_fl_cursor_, "Ctrl+Z twice: no actions again");
                check(ue_fl_click(ue_fl_pic_), "a click on the picture");
                return true;
            }
            case 20: {
                if (wait(shown("ue-f-picture"), "the picture's panel is laid out")) return true;
                check(only(ue_fl_pic_) && ue().float_panel() == "picture" && ue_fl_box(px, py, pw, ph) && std::fabs(pw - 280) < 0.5f &&
                          !shown("ue-f-action") && !shown("ue-f-family"),
                      "the picture: its panel, 280 px wide, its file only");
                {
                    const bool placed = ue_fl_placed(ue_fl_pic_, true, why);
                    check(placed, "above the picture: " + why);
                }
                check(ue_open("ue-f-picture"), "a click opens the game's pictures");
                return true;
            }
            case 21: {
                // «Нет»: the picture keeps its fill and names no file (case 149 picks another file).
                ue_fl_pick_ = "";
                check(ue_option("ue-f-picture", "") && ue_option_hovered_, "a click on «Нет»");
                const d::Node* n = ue_node(ue_fl_pic_);
                check(n && !n->fills.empty() && n->fills[0].image.empty() && steps() == ue_fl_cursor_ + 1 &&
                          ue().history().undo_label() == "Изменено: Картинка",
                      "the picture shows it, one step: " + ue().history().undo_label());
                return true; // drawn on the next frame
            }
            case 22: {
                // «Нет» too is drawn (as nothing: the page names no file, and no folder is read as a picture).
                {
                    const std::string html = ue_file(".html", ue().opened());
                    check(html.find("url(\"../\")") == std::string::npos && html.find("url('../')") == std::string::npos,
                          "no picture: the page names no folder as one");
                }
                ue_cm_key(SDLK_Z);
                check(d::save_screen(ue().screen()) == ue_fl_json_ && steps() == ue_fl_cursor_, "Ctrl+Z: as it was");
                check(ue().open("main_menu"), "back to the menu");
                break;
            }
            default: break;
            }
            ue_fl_stage_ = 0;
            break;
        }
        case 149: {
            // The panel over the layer with the rest of the editor (13.10), the whole way: one selection and the same
            // values as the panel on the right both ways, one history for both, a field left mid-typing kept by its
            // own layer when another layer, screen or tab is clicked, a copy of a component keeping what the panel
            // changed on it while its component changes, the screen read again from disk, and the button in
            // «Проверить» doing what was picked, with its sound.
            namespace d = editor::design;
            auto wait = [&](bool ready, const char* what) {
                if (!hold(ready, what)) return false;
                --ue_fl_stage_;
                return true;
            };
            const d::Node& root = ue().screen().root;
            auto steps = [&] { return ue().history().cursor(); };
            auto only = [&](u32 id) { return ue().selection() == std::vector<u32>{id}; };
            auto label_of = [&](u32 id) -> const d::Node* {
                const d::Node* n = ue_node(id);
                return n ? d::block_label(*n) : nullptr;
            };
            switch (ue_fl_stage_++) {
            case 0: {
                ue().set_simple(false);
                ue().set_view(0);
                ue_fl_screen_ = ue().new_screen();
                ue_fl_btn_ = ue().create("button");
                ue_fl_text_ = ue().create("text");
                ue_fl_pic_ = ue().create("picture");
                check(ue_fl_btn_ && ue_fl_text_ && ue_fl_pic_ && root.children.size() == 3, "a new screen with a button, a text and a picture");
                for (const auto& [id, x, y] : {std::tuple{ue_fl_btn_, "200", "500"}, std::tuple{ue_fl_text_, "200", "300"},
                                               std::tuple{ue_fl_pic_, "1300", "500"}}) {
                    ue().select({id});
                    ue().set_property("x", x);
                    ue().set_property("y", y);
                }
                ue().select({});
                return true;
            }
            case 1:
                ue_fl_json_ = d::save_screen(ue().screen());
                ue_fl_cursor_ = steps();
                check(ue_fl_click(ue_fl_btn_), "a click on the button");
                return true;
            case 2:
                if (wait(shown("ue-f-action"), "the button's panel is laid out")) return true;
                check(click("ue-tab-game") && ue().panel() == "game", "the right panel's «В игре»");
                check(ue_open("ue-f-action"), "a click opens the panel's actions");
                return true;
            case 3: {
                check(ue_option("ue-f-action", "settings") && ue_option_hovered_, "«Настройки» picked on the panel over the button");
                const d::Node* n = ue_node(ue_fl_btn_);
                check(n && n->on_click.size() == 1 && n->on_click[0].kind == d::ActionKind::Settings && steps() == ue_fl_cursor_ + 1,
                      "the button opens the settings, one step");
                return true;
            }
            case 4: {
                if (wait(shown("ue-click-kind-0") && shown("ue-f-sound"), "both panels show the action")) return true;
                check(ue_field("ue-click-kind-0") == "settings" && ue_field("ue-f-action") == "settings",
                      "the right panel shows what the panel over the button picked: " + ue_field("ue-click-kind-0"));
                check(ue_sd_open("ue-click-sound"), "a click opens the right panel's sounds");
                return true;
            }
            case 5: {
                // A sound of the game's on the right.
                ue_fl_sound_.clear();
                if (auto* sel = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(ed_.find_element("ue-click-sound")))
                    for (int i = 0; i < sel->GetNumOptions() && ue_fl_sound_.empty(); ++i) {
                        const Rml::String v = sel->GetOption(i)->GetAttribute<Rml::String>("value", "");
                        if (!v.empty() && v != "none" && v.rfind("add:", 0) != 0) ue_fl_sound_ = v;
                    }
                check(!ue_fl_sound_.empty() && ue_option("ue-click-sound", ue_fl_sound_) && ue_option_hovered_,
                      "the game's sound «" + ue_fl_sound_ + "» picked on the right");
                const d::Node* n = ue_node(ue_fl_btn_);
                check(n && n->click_sound == ue_fl_sound_ && steps() == ue_fl_cursor_ + 2, "the button's sound, one step");
                return true;
            }
            case 6: {
                if (wait(ue_field("ue-f-sound") == ue_fl_sound_, "the panel over the button shows the sound")) return true;
                check(only(ue_fl_btn_) && ue().float_panel() == "button", "one selection for both panels");
                // One history: Ctrl+Z takes back the right panel's change, then the other panel's.
                ue_cm_key(SDLK_Z);
                const d::Node* n = ue_node(ue_fl_btn_);
                check(n && n->click_sound.empty() && n->on_click.size() == 1 && steps() == ue_fl_cursor_ + 1, "Ctrl+Z: the sound as the screen's");
                ue_cm_key(SDLK_Z);
                check(d::save_screen(ue().screen()) == ue_fl_json_ && steps() == ue_fl_cursor_, "Ctrl+Z: no action, as it was");
                ue_cm_key(SDLK_Y);
                ue_cm_key(SDLK_Y);
                n = ue_node(ue_fl_btn_);
                check(n && n->on_click.size() == 1 && n->on_click[0].kind == d::ActionKind::Settings && n->click_sound == ue_fl_sound_ &&
                          steps() == ue_fl_cursor_ + 2,
                      "Ctrl+Y twice: both back");
                return true;
            }
            case 7: {
                if (wait(ue_field("ue-f-sound") == ue_fl_sound_ && ue_field("ue-click-sound") == ue_fl_sound_, "both panels show the sound again"))
                    return true;
                check(ue_field("ue-f-action") == "settings" && ue_field("ue-click-kind-0") == "settings", "and the action, on both");
                check(ue_fl_click(ue_fl_pic_), "a click on the picture");
                return true;
            }
            case 8:
                if (wait(shown("ue-f-picture"), "the picture's panel is laid out")) return true;
                check(click("ue-tab-design") && ue().panel() == "design", "the right panel's «Дизайн»");
                ue_fl_cursor_ = steps();
                check(ue_open("ue-f-picture"), "a click opens the panel's pictures");
                return true;
            case 9: {
                // Another of the game's pictures (not «Нет»).
                std::optional<std::string> other;
                if (auto* sel = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(ed_.find_element("ue-f-picture")))
                    for (int i = 0; i < sel->GetNumOptions() && !other; ++i) {
                        const Rml::String v = sel->GetOption(i)->GetAttribute<Rml::String>("value", "");
                        if (!v.empty() && v != sel->GetValue()) other = v;
                    }
                ue_fl_pick_ = other.value_or("?");
                check(other && ue_option("ue-f-picture", *other), "the picture «" + ue_fl_pick_ + "» picked on the panel over it");
                const d::Node* n = ue_node(ue_fl_pic_);
                check(n && n->fills[0].image == ue_fl_pick_ && steps() == ue_fl_cursor_ + 1, "one step");
                return true;
            }
            case 10:
                if (wait(shown("ue-fill-image-0"), "the right panel's fill")) return true;
                check(ue_field("ue-fill-image-0") == ue_fl_pick_, "the right panel shows the same picture: " + ue_field("ue-fill-image-0"));
                check(ue_fl_click(ue_fl_text_), "a click on the text");
                return true;
            case 11: {
                if (wait(shown("ue-f-size"), "the text's panel is laid out")) return true;
                // Left mid-typing, then another layer clicked: the size is the text's, not the button's.
                const d::Node* label = label_of(ue_fl_btn_);
                const f32 label_size = label ? label->text_style.size : 0;
                ue_fl_cursor_ = steps();
                check(ue_fl_type_only("ue-f-size", "56"), "56 typed into the size, no Enter");
                check(ue_fl_click(ue_fl_btn_), "a click on the button while typing");
                const d::Node* t = ue_node(ue_fl_text_);
                label = label_of(ue_fl_btn_);
                check(t && t->text_style.size == 56 && label && label->text_style.size == label_size && only(ue_fl_btn_) &&
                          steps() == ue_fl_cursor_ + 1 && ue().history().undo_label() == "Изменено: Размер текста",
                      "the text keeps its 56, the button's label its size; one step; the button selected");
                check(ue_fl_click(ue_fl_text_), "the text again");
                return true;
            }
            case 12: {
                if (wait(shown("ue-f-size"), "the text's panel is laid out")) return true;
                check(ue_field("ue-f-size") == "56" && ue_field("ue-font-size") == "56", "both panels show 56");
                // Then another screen clicked in the list.
                ue_fl_json_ = ue_file(".json", "main_menu");
                check(ue_fl_type_only("ue-f-size", "60"), "60 typed, no Enter");
                f32 x = 0, y = 0;
                check(element_center("ue-screen-main_menu", x, y), "the menu's row in the list of screens");
                left_click(x, y);
                const std::optional<d::Node> t = ue_fl_on_disk(ue_fl_screen_, ue_fl_text_);
                check(ue().opened() == "main_menu" && t && t->text_style.size == 60 && ue_file(".json", "main_menu") == ue_fl_json_,
                      "the menu open; 60 written to the text of the screen left, the menu untouched");
                check(ue().open(ue_fl_screen_), "back to the screen");
                ue().select({ue_fl_text_});
                return true;
            }
            case 13: {
                if (wait(shown("ue-f-size"), "the text's panel is laid out")) return true;
                // Then another tab clicked.
                check(ue_field("ue-f-size") == "60" && ue_fl_type_only("ue-f-size", "64"), "60 shown; 64 typed, no Enter");
                Rml::Element* bar = ed_.find_element("editor-tabs");
                f32 x = 0, y = 0;
                if (bar && bar->GetNumChildren() > 0) {
                    Rml::Element* level = bar->GetChild(0);
                    const Rml::Vector2f p = level->GetAbsoluteOffset(Rml::BoxArea::Border) + level->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
                    x = p.x;
                    y = p.y;
                }
                check(x > 0, "the «Уровень» tab");
                left_click(x, y);
                const d::Node* t = ue_node(ue_fl_text_);
                check(ed_.tab() != "ui" && t && t->text_style.size == 64 && ue().opened() == ue_fl_screen_, "another tab; 64 kept by the text");
                check(click_tab(8) && ed_.tab() == "ui", "back to «Интерфейс»");
                return true;
            }
            case 14: {
                // Read again from disk: what both panels changed is there.
                check(ue().open("main_menu") && ue().open(ue_fl_screen_), "the screen closed and opened again");
                const d::Node* b = ue_node(ue_fl_btn_);
                const d::Node* t = ue_node(ue_fl_text_);
                const d::Node* p = ue_node(ue_fl_pic_);
                check(b && b->on_click.size() == 1 && b->on_click[0].kind == d::ActionKind::Settings && b->click_sound == ue_fl_sound_ && t &&
                          t->text_style.size == 64 && p && p->fills[0].image == ue_fl_pick_,
                      "the action, the sound, the size and the picture as left");
                check(ue_file(".html", ue_fl_screen_).find(ue_fl_sound_) != std::string::npos, "the game's page has the button's sound");
                // A component made of the button: its copy takes the button's place.
                ue().select({ue_fl_btn_});
                check(ue().make_component(), "the button made a component");
                ue_fl_inst_ = ue().selection().size() == 1 ? ue().selection()[0] : 0;
                const d::Node* inst = ue_node(ue_fl_inst_);
                check(inst && !inst->component.empty(), "a copy of it selected");
                return true;
            }
            case 15:
                if (wait(shown("ue-f-action"), "the copy's panel is laid out")) return true;
                check(ue().float_panel() == "button" && ue_field("ue-f-action") == "settings", "the copy: the button's panel, «Настройки»");
                ue_fl_cursor_ = steps();
                check(ue_open("ue-f-action"), "a click opens the actions");
                return true;
            case 16: {
                check(ue_option("ue-f-action", "quit"), "«Выйти из игры» picked for this copy");
                const d::Node* inst = ue_node(ue_fl_inst_);
                check(inst && inst->on_click.size() == 1 && inst->on_click[0].kind == d::ActionKind::Quit &&
                          std::find(inst->overrides.begin(), inst->overrides.end(), "game") != inst->overrides.end() && steps() == ue_fl_cursor_ + 1,
                      "the copy's own action, kept as its own change; one step");
                // The component changes: its label bigger, another action.
                const std::string component = inst ? inst->component : std::string();
                check(ue().open_library(), "the components");
                u32 master = 0;
                for (const d::Node& c : ue().screen().root.children)
                    if (c.component == component) master = c.id;
                const d::Node* m = ue_node(master);
                const d::Node* ml = m ? d::block_label(*m) : nullptr;
                const u32 master_label = ml ? ml->id : 0; // a change loads the screen anew: ids, not pointers
                ue().select({master});
                check(master && ue().set_property("click.0.kind", "pause"), "the component's action: pause");
                ue().select({master_label});
                check(master_label && ue().set_property("size", "40"), "the component's label: 40");
                check(ue().open(ue_fl_screen_), "back to the screen");
                inst = ue_node(ue_fl_inst_);
                const d::Node* il = label_of(ue_fl_inst_);
                check(inst && inst->on_click.size() == 1 && inst->on_click[0].kind == d::ActionKind::Quit && il && il->text_style.size == 40,
                      "the copy keeps its own action and follows its component's label");
                // The copy's label through the panel.
                if (il) ue().select({il->id});
                return true;
            }
            case 17: {
                if (wait(shown("ue-f-size"), "the copy's label's panel is laid out")) return true;
                check(ue_field("ue-f-size") == "40", "its size 40 on the panel");
                ue_fl_cursor_ = steps();
                check(ue_type("ue-f-size", "44"), "44 typed");
                const d::Node* il = label_of(ue_fl_inst_);
                check(il && il->text_style.size == 44 && std::find(il->overrides.begin(), il->overrides.end(), "text_style") != il->overrides.end() &&
                          steps() == ue_fl_cursor_ + 1,
                      "the copy's label 44, its own change; one step");
                return true;
            }
            case 18: {
                if (wait(ue_field("ue-font-size") == "44", "the right panel shows 44")) return true;
                // The component's label changes again: this copy keeps 44.
                const d::Node* inst = ue_node(ue_fl_inst_);
                const std::string component = inst ? inst->component : std::string();
                check(ue().open_library(), "the components");
                u32 master_label = 0;
                for (const d::Node& c : ue().screen().root.children)
                    if (const d::Node* ml = c.component == component ? d::block_label(c) : nullptr) master_label = ml->id;
                ue().select({master_label});
                check(master_label && ue().set_property("size", "50"), "the component's label: 50");
                check(ue().open(ue_fl_screen_), "back to the screen");
                const d::Node* il = label_of(ue_fl_inst_);
                inst = ue_node(ue_fl_inst_);
                check(il && il->text_style.size == 44 && inst && inst->on_click[0].kind == d::ActionKind::Quit, "the copy keeps 44 and its action");
                const std::optional<d::Node> disk = ue_fl_on_disk(ue_fl_screen_, ue_fl_inst_);
                check(disk && disk->on_click.size() == 1 && disk->on_click[0].kind == d::ActionKind::Quit, "and so on disk");
                // «Проверить»: the copy pressed as the game would.
                check(click("ue-check") && ue().checking(), "«Проверить»");
                return true;
            }
            case 19: {
                if (wait(ue().page() != nullptr, "the live page")) return true;
                ue_fl_log_ = ue().check_log().size();
                ue_fl_clicks_ = ue().check_sound().clicks();
                const auto box = ue().layer_box(ue_fl_inst_);
                check(box.has_value(), "the copy on the canvas");
                if (box) left_click(ue_wx(box->cx()), ue_wy(box->cy()));
                return true;
            }
            case 20: {
                const std::vector<std::string>& log = ue().check_log();
                check(log.size() == ue_fl_log_ + 1 && log.back() == "Кнопка: Выйти из игры", "the copy pressed does its own action: " + ue_log_since(ue_fl_log_));
                check(ue().check_sound().clicks() == ue_fl_clicks_ + 1, "with its sound once");
                ue().set_checking(false);
                check(ue().open("main_menu"), "back to the menu");
                break;
            }
            default: break;
            }
            ue_fl_stage_ = 0;
            break;
        }
        case 150: {
            // A frame and one of its own layers restacked together (PR #69; Codex's review on PR #70): each moves among
            // its own neighbours whichever was selected first, by keys and by the layer's menu; one step each, undone
            // and redone whole (order, ids, selection); the other branches untouched; the order kept on disk.
            // The screen: A (C, D, E), B (G), F; A and B are frames.
            namespace d = editor::design;
            auto wait = [&](bool ready, const char* what) {
                if (!hold(ready, what)) return false;
                --ue_rs_stage_;
                return true;
            };
            const d::Node& root = ue().screen().root;
            auto steps = [&] { return ue().history().cursor(); };
            auto ids = [](std::initializer_list<u32> list) {
                std::string out;
                for (u32 id : list) out += (out.empty() ? "" : " ") + std::to_string(id);
                return out;
            };
            const u32 a = ue_rs_a_, b = ue_rs_b_, c = ue_rs_c_, dd = ue_rs_d_, e = ue_rs_e_, f = ue_rs_f_;
            auto order = [&](std::initializer_list<u32> top, std::initializer_list<u32> in_a) {
                return ue_mv_kids(root.id) == ids(top) && ue_mv_kids(a) == ids(in_a);
            };
            auto others = [&] { return ue_rs_branch(b) + ue_rs_branch(f); };
            auto picked = [&] {
                std::string out;
                for (u32 id : ue().selection()) out += (out.empty() ? "" : " ") + std::to_string(id);
                return "selected " + out + "; layers " + ue_mv_kids(root.id) + " / " + ue_mv_kids(a) + "; last step " + ue().history().undo_label();
            };
            const auto top_both = static_cast<SDL_Keymod>(SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
            switch (ue_rs_stage_++) {
            case 0: {
                ue().set_simple(false);
                ue().set_view(0);
                ue_rs_screen_ = ue().new_screen();
                ue_rs_c_ = ue().create("rectangle");
                ue_rs_d_ = ue().create("rectangle");
                ue_rs_e_ = ue().create("rectangle");
                ue().select({ue_rs_c_, ue_rs_d_, ue_rs_e_});
                ue_cm_key(SDLK_G);
                ue_rs_a_ = ue().selection().size() == 1 ? ue().selection()[0] : 0;
                ue_rs_g_ = ue().create("rectangle");
                ue().select({ue_rs_g_});
                ue_cm_key(SDLK_G);
                ue_rs_b_ = ue().selection().size() == 1 ? ue().selection()[0] : 0;
                ue_rs_f_ = ue().create("ellipse");
                const d::Node* fa = ue_node(ue_rs_a_);
                const d::Node* fb = ue_node(ue_rs_b_);
                check(fa && fb && fa->type == d::NodeType::Frame && fb->type == d::NodeType::Frame &&
                          ue_mv_kids(root.id) == ids({ue_rs_a_, ue_rs_b_, ue_rs_f_}) && ue_mv_kids(ue_rs_a_) == ids({ue_rs_c_, ue_rs_d_, ue_rs_e_}) &&
                          ue_mv_kids(ue_rs_b_) == ids({ue_rs_g_}),
                      "a new screen: frame A with C, D, E, frame B with G, then F");
                ue().select({});
                return true;
            }
            case 1:
                ue_rs_show(a);
                return true;
            case 2: {
                ue_rs_json_ = d::save_screen(ue().screen());
                ue_rs_others_ = others();
                ue_rs_cursor_ = steps();
                const bool clicked = ue_rs_row(a, false, -20);
                check(clicked && ue().selection() == std::vector<u32>{a}, "a click on A's row in the list: " + picked());
                ue_rs_show(c);
                return true;
            }
            case 3: {
                {
                    const bool clicked = ue_rs_row(c, true, 20);
                    check(clicked && ue().selection() == std::vector<u32>{a, c}, "Shift and a click on C's row, inside A: A, then C, selected: " + picked());
                }
                ue_cm_key(SDLK_RIGHTBRACKET);
                check(order({b, a, f}, {dd, c, e}) && steps() == ue_rs_cursor_ + 1 && ue().history().undo_label() == "Выше" &&
                          ue().selection() == std::vector<u32>{a, c},
                      "Ctrl+]: A one higher among the screen's layers and C among A's, one step: " + ue_mv_kids(root.id) + " / " + ue_mv_kids(a));
                check(others() == ue_rs_others_, "B with G and F untouched");
                const std::string up = d::save_screen(ue().screen());
                ue_cm_key(SDLK_Z);
                check(d::save_screen(ue().screen()) == ue_rs_json_ && steps() == ue_rs_cursor_ && ue().selection() == std::vector<u32>{a, c},
                      "Ctrl+Z: the order, ids and selection as before");
                ue_cm_key(SDLK_Y);
                check(d::save_screen(ue().screen()) == up && steps() == ue_rs_cursor_ + 1 && ue().selection() == std::vector<u32>{a, c},
                      "Ctrl+Y: as moved, both still selected");
                ue_cm_key(SDLK_RIGHTBRACKET, top_both);
                check(order({b, f, a}, {dd, e, c}) && ue().history().undo_label() == "На самый верх",
                      "Ctrl+Shift+]: both on top of their own neighbours: " + ue_mv_kids(root.id) + " / " + ue_mv_kids(a));
                ue_cm_key(SDLK_LEFTBRACKET);
                check(order({b, a, f}, {dd, c, e}) && ue().history().undo_label() == "Ниже",
                      "Ctrl+[: both one lower: " + ue_mv_kids(root.id) + " / " + ue_mv_kids(a));
                ue_cm_key(SDLK_LEFTBRACKET, top_both);
                check(order({a, b, f}, {c, dd, e}) && ue().history().undo_label() == "В самый низ" && steps() == ue_rs_cursor_ + 4,
                      "Ctrl+Shift+[: both the lowest, four steps in all: " + ue_mv_kids(root.id) + " / " + ue_mv_kids(a));
                check(others() == ue_rs_others_, "B with G and F still untouched");
                for (int i = 0; i < 4; ++i) ue_cm_key(SDLK_Z);
                check(d::save_screen(ue().screen()) == ue_rs_json_ && steps() == ue_rs_cursor_ && ue().selection() == std::vector<u32>{a, c},
                      "four Ctrl+Z: as it was, both selected");
                ue().select({});
                ue_rs_show(c);
                return true;
            }
            case 4: {
                // The other way round: the layer first, then its frame.
                const bool clicked = ue_rs_row(c, false, -20);
                check(clicked && ue().selection() == std::vector<u32>{c}, "a click on C's row: " + picked());
                ue_rs_show(a);
                return true;
            }
            case 5: {
                const bool clicked = ue_rs_row(a, true, 20);
                check(clicked && ue().selection() == std::vector<u32>{c, a}, "Shift and a click on A's row: C, then A, selected: " + picked());
                check(ue_cm_right_row(a), "the right button on A's row");
                return true;
            }
            case 6:
                if (wait(shown("ue-ctx-up"), "the layer's menu is laid out")) return true;
                check(ue().selection() == std::vector<u32>{c, a}, "the menu keeps both selected");
                check(ue_cm_press("ue-ctx-up"), "«Выше»");
                return true;
            case 7: {
                check(order({b, a, f}, {dd, c, e}) && steps() == ue_rs_cursor_ + 1 && ue().history().undo_label() == "Выше" && ue().menu().empty(),
                      "«Выше» from the menu: the same order, whichever was selected first: " + ue_mv_kids(root.id) + " / " + ue_mv_kids(a));
                check(others() == ue_rs_others_, "B with G and F untouched");
                const std::string up = d::save_screen(ue().screen());
                check(ue().open("main_menu") && ue().open(ue_rs_screen_), "the screen closed and opened again");
                check(order({b, a, f}, {dd, c, e}) && d::save_screen(ue().screen()) == up, "from disk: the order as left");
                check(ue().open("main_menu"), "back to the menu");
                break;
            }
            default: break;
            }
            ue_rs_stage_ = 0;
            break;
        }
        case 151: {
            // A window's behaviour shown and changed in the screen's panel (13.11): where it comes up (anywhere, only
            // over the game, only over the main menu; elsewhere it does not open), its pause, Esc and veil, what
            // opens it and from where; in «Проверить» windows come up over the screen as in the game: the veil keeps
            // clicks off what is under it, Esc closes only the window on top (once while held), the keyboard comes back
            // to the menu; a window only for the menu checked by itself is checked as over the menu.
            namespace d = editor::design;
            auto wait = [&](bool ready, const char* what) {
                if (!hold(ready, what)) return false;
                --ue_wb_stage_;
                return true;
            };
            auto rect = [&](f32 x, f32 y, const char* name) {
                const u32 id = ue().add_layer(d::NodeType::Rectangle, x, y, 200, 80);
                ue().select({id});
                ue().set_property("name", name);
                return id;
            };
            // One action on the selected layer (a new action shows some screen: kind and target as wanted).
            auto act = [&](const char* kind, const std::string& target) {
                ue().set_property("click.add", "");
                ue().set_property("click.0.kind", kind);
                if (!target.empty()) ue().set_property("click.0.target", target);
                const d::Node* n = ue().selection().size() == 1 ? ue_node(ue().selection()[0]) : nullptr;
                return n && n->on_click.size() == 1 && std::string(d::action_word(n->on_click[0].kind)) == kind &&
                       (target.empty() || n->on_click[0].target == target);
            };
            auto said = [&]() { return ue().check_log().empty() ? std::string() : ue().check_log().back(); };
            auto has = [&](const std::vector<std::pair<std::string, bool>>& rows, const std::string& text, bool warn) {
                return std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.first == text && r.second == warn; });
            };
            auto press = [&](SDL_Keycode k) {
                ue_sd_key(k, true);
                ue_sd_key(k, false);
            };
            // Esc held: what it does when it goes down, then the repeats the keyboard sends, which do nothing more.
            auto esc_held = [&]() {
                ue_sd_key(SDLK_ESCAPE, true);
                const auto windows = ue().check_windows();
                const bool on = ue().checking();
                const usize lines = ue().check_log().size();
                const std::vector<u32> sel = ue().selection();
                bool same = true;
                for (int i = 0; i < 3; ++i) {
                    ue_sd_key(SDLK_ESCAPE, true, true);
                    same = same && ue().check_windows() == windows && ue().checking() == on && ue().check_log().size() == lines &&
                           ue().selection() == sel;
                }
                ue_sd_key(SDLK_ESCAPE, false);
                return same;
            };
            auto at = [&](const std::optional<d::Rect>& b) {
                if (b) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return b.has_value();
            };
            const std::string w1 = ue_wb_w1_, w2 = ue_wb_w2_, w3 = ue_wb_w3_;
            const std::string over_menu_w1 = "Кнопка «Настройки» на экране «" + ue_wb_tm_ + "»: над главным меню";
            const std::string over_game_w1 = "Кнопка «Окно» на экране «" + ue_wb_th_ + "»: над игрой";
            switch (ue_wb_stage_++) {
            case 0: {
                ue().set_simple(false);
                ue().set_view(0);
                // W2: a window Esc does not close, without a veil, only over the game; W1 opens it.
                ue_wb_w2_ = ue().new_screen();
                ue_wb_t2_ = ue().screen().title;
                rect(1200, 300, "Закрыть2");
                check(act("close", ""), "W2: «Закрыть2» closes it");
                ue().select({});
                check(ue().set_property("screen.esc", "") && ue().set_property("screen.dim", "") && ue().set_property("screen.over", "game") &&
                          !ue().screen().esc_closes && !ue().screen().dim && ue().screen().over == d::WindowOver::Game,
                      "W2: no Esc, no veil, only over the game");
                // W3: a window as made, closed by Esc; W1 opens it too.
                ue_wb_w3_ = ue().new_screen();
                ue_wb_t3_ = ue().screen().title;
                rect(1200, 600, "Закрыть3");
                check(act("close", ""), "W3: «Закрыть3» closes it");
                // W1: a new window (a veil by its switch), a button that opens W2 and one that closes it.
                ue_wb_w1_ = ue().new_screen();
                ue_wb_t1_ = ue().screen().title;
                check(ue().screen().dim && ue().screen().esc_closes && !ue().screen().pauses && ue().screen().over == d::WindowOver::Any,
                      "W1: a new window darkens what is under it, closes by Esc, comes up anywhere");
                ue_wb_more_ = rect(800, 300, "Ещё");
                check(act("show", ue_wb_w2_), "W1: «Ещё» opens W2");
                ue_wb_close_ = rect(800, 450, "Закрыть");
                check(act("close", ""), "W1: «Закрыть» closes it");
                ue_wb_third_ = rect(800, 600, "Третье");
                check(act("show", ue_wb_w3_), "W1: «Третье» opens W3");
                // A main menu and a screen over the game, each with a button that opens W1.
                ue_wb_menu_ = ue().new_screen();
                ue_wb_tm_ = ue().screen().title;
                ue().select({});
                check(ue().set_property("screen.show", "menu"), "M: the main menu");
                ue_wb_settings_ = rect(100, 100, "Настройки");
                check(act("show", ue_wb_w1_), "M: «Настройки» opens W1");
                ue_wb_hud_ = ue().new_screen();
                ue_wb_th_ = ue().screen().title;
                ue().select({});
                check(ue().set_property("screen.show", "playing"), "H: over the game");
                ue_wb_open_ = rect(100, 100, "Окно");
                check(act("show", ue_wb_w1_), "H: «Окно» opens W1");
                ue_wb_count_ = rect(100, 600, "Счёт");
                check(act("change", "test.count += 1"), "H: «Счёт» counts");
                check(ue_file(".html", ue_wb_w1_).find("<div id=\"forge-dim\"></div>") != std::string::npos &&
                          ue_file(".html", ue_wb_w2_).find("forge-esc=\"0\" forge-over=\"game\"") != std::string::npos,
                      "the windows' pages: W1's veil, W2 without Esc only over the game");
                check(ue().open(ue_wb_w1_), "W1 opened");
                ue().select({ue().screen().root.id});
                click("ue-tab-game");
                return true;
            }
            case 1: {
                check(shown("ue-screen-over") && shown("ue-screen-pauses") && shown("ue-screen-esc") && shown("ue-screen-dim") &&
                          shown("ue-window-notes") && shown("ue-openers"),
                      "«В игре» of a window: where it comes up, the switches, what they do, what opens it");
                const auto rows = ue_wb_rows("ue-openers");
                check(rows.size() == 2 && has(rows, over_menu_w1, false) && has(rows, over_game_w1, false),
                      "what opens W1: the menu's button over the menu, the game's over the game: " + ue_wb_text(rows));
                const auto notes = ue_wb_rows("ue-window-notes");
                check(notes.size() == 3 && notes[0].first.find("Игра идёт под окном") == 0 && notes[2].first.find("щелчки туда не проходят") != std::string::npos,
                      "the notes say the game goes on and the veil stops clicks: " + ue_wb_text(notes));
                check(click("ue-screen-pauses") && ue().screen().pauses && ue().history().undo_label().find("Пауза окна") != std::string::npos,
                      "a click on «Пока открыто, игра стоит»: one step «Пауза окна»: " + ue().history().undo_label());
                return true;
            }
            case 2: {
                const auto notes = ue_wb_rows("ue-window-notes");
                check(!notes.empty() && notes[0].first.find("Игра стоит") == 0, "the note now says the game stands: " + ue_wb_text(notes));
                check(ue_open("ue-screen-over"), "the drop-down «Где появляется» opens");
                return true;
            }
            case 3:
                check(ue_option("ue-screen-over", "game") && ue().screen().over == d::WindowOver::Game, "«Только над игрой» picked with the mouse");
                return true;
            case 4: {
                const auto rows = ue_wb_rows("ue-openers");
                check(has(rows, over_menu_w1 + ". Не откроется: окно только над игрой", true) && has(rows, over_game_w1, false),
                      "the menu's button is pointed out: W1 does not open there: " + ue_wb_text(rows));
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue().screen().over == d::WindowOver::Any, "Ctrl+Z: anywhere again");
                return true;
            }
            case 5: {
                const auto rows = ue_wb_rows("ue-openers");
                check(has(rows, over_menu_w1, false) && has(rows, over_game_w1, false), "and nothing is pointed out: " + ue_wb_text(rows));
                key(SDLK_Y, SDL_KMOD_CTRL);
                check(ue().screen().over == d::WindowOver::Game, "Ctrl+Y: only over the game");
                check(ue().set_property("screen.over", "menu") && ue().screen().over == d::WindowOver::Menu, "only over the main menu");
                // «Логика» shows W1 too: a block «Показать экран» in the door's own scheme.
                for (const logic::ThingScheme& t : lg().links().schemes)
                    if (t.thing == "door") ue_wb_scheme_ = t.id;
                ue_wb_scheme_new_ = ue_wb_scheme_ == 0;
                if (!ue_wb_scheme_) ue_wb_scheme_ = lg().thing_scheme("door");
                ue_wb_node_ = ue_wb_scheme_ ? lg().scheme().add_node(ue_wb_scheme_, "api.ui.show", 400, 400) : 0;
                check(ue_wb_node_ && lg().scheme().set_value(ue_wb_scheme_, ue_wb_node_, "name", ue_wb_w1_),
                      "«Логика»: the door's scheme shows W1");
                // The screen closed and opened again: from disk, with «Логика» read anew.
                check(ue().open(ue_wb_hud_) && ue().open(ue_wb_w1_), "W1 closed and opened again");
                ue().select({ue().screen().root.id});
                return true;
            }
            case 6: {
                check(ue().screen().over == d::WindowOver::Menu && ue().screen().pauses && ue().screen().dim && ue().screen().esc_closes,
                      "from disk: only over the menu, stops the game, darkens, closes by Esc");
                check(ue_file(".html", ue_wb_w1_).find("forge-pauses=\"1\" forge-dim=\"1\" forge-over=\"menu\"") != std::string::npos,
                      "and its page says so");
                const auto rows = ue_wb_rows("ue-openers");
                const std::string logic_row = "«Логика», схема «Дверь»: над игрой. Не откроется: окно только над главным меню";
                check(rows.size() == 3 && has(rows, over_menu_w1, false) &&
                          has(rows, over_game_w1 + ". Не откроется: окно только над главным меню", true) && has(rows, logic_row, true),
                      "now the game's button and «Логика»'s block are pointed out: " + ue_wb_text(rows));
                // «Логика» as it was.
                check(lg().scheme().remove_node(ue_wb_scheme_, ue_wb_node_) && (!ue_wb_scheme_new_ || lg().remove_thing_scheme(ue_wb_scheme_)),
                      "the block taken away again");
                // «Простой»: the same choice for the screen (W1 opened again: the list is read anew, as when the tab comes back).
                check(ue().open(ue_wb_hud_) && ue().open(ue_wb_w1_), "W1 opened again");
                ue().set_simple(true);
                ue().select({ue().screen().root.id});
                return true;
            }
            case 7:
                check(shown("ue-s-over") && shown("ue-s-pauses") && shown("ue-s-esc") && shown("ue-s-dim") && shown("ue-s-openers"),
                      "«Простой» shows where the window comes up, its switches and what opens it");
                check(ue_wb_rows("ue-s-openers").size() == 2, "«Логика»'s block is gone from the list: " + ue_wb_text(ue_wb_rows("ue-s-openers")));
                check(ue_open("ue-s-over"), "its drop-down opens");
                return true;
            case 8:
                check(ue_option("ue-s-over", "any") && ue().screen().over == d::WindowOver::Any &&
                          ue().history().undo_label().find("Где появляется окно") != std::string::npos,
                      "«Везде» picked in «Простой»: one step «Где появляется окно»: " + ue().history().undo_label());
                ue().set_simple(false);
                // «Проверить» on the menu: W1 over it, W2 (only over the game) not; the keyboard comes back to the menu.
                check(ue().open(ue_wb_menu_) && click("ue-check") && ue().checking(), "«Проверить» on the menu");
                return true;
            case 9:
                press(SDLK_TAB);
                check(ue().check_focus() == ue_wb_menu_ + ":n" + std::to_string(ue_wb_settings_), "Tab: «Настройки» has the keyboard: " + ue().check_focus());
                press(SDLK_RETURN);
                return true;
            case 10: {
                check(ue().check_windows() == std::vector<std::string>{w1} && ue().opened() == ue_wb_menu_,
                      "Enter: W1 comes up over the menu, the menu stays");
                const std::string focus = ue().check_focus();
                check(focus.rfind(w1 + ":", 0) == 0, "the window that stops the game takes the keyboard: " + focus);
                check(said() == "Открыто окно «" + ue_wb_t1_ + "» поверх экрана", "the panel says so: " + said());
                const auto state = ue_wb_rows("ue-check-state");
                check(has(state, "Сверху окно «" + ue_wb_t1_ + "»: Esc его закроет", false) &&
                          has(state, "Как над главным меню: игра ещё не идёт, пауза окна ничего не останавливает", false),
                      "«Окна» in the check: W1 on top, over the menu: " + ue_wb_text(state));
                press(SDLK_TAB);
                check(ue().check_focus() == w1 + ":n" + std::to_string(ue_wb_more_), "Tab: «Ещё»: " + ue().check_focus());
                press(SDLK_RETURN);
                return true;
            }
            case 11:
                check(ue().check_windows() == std::vector<std::string>{w1} &&
                          said() == "Окно «" + ue_wb_t2_ + "» не открылось: оно появляется только над игрой, а проверка идёт как над главным меню",
                      "W2 only over the game does not come up over the menu, and the panel says why: " + said());
                press(SDLK_TAB);
                check(ue().check_focus() == w1 + ":n" + std::to_string(ue_wb_close_), "Tab: «Закрыть»: " + ue().check_focus());
                press(SDLK_RETURN);
                return true;
            case 12:
                check(ue().check_windows().empty(), "Enter: W1 closed");
                check(ue().check_focus() == ue_wb_menu_ + ":n" + std::to_string(ue_wb_settings_),
                      "the keyboard is back on «Настройки», the button that opened it: " + ue().check_focus());
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().checking(), "no window up: Esc ends the check");
                // «Проверить» on the screen over the game.
                check(ue().open(ue_wb_hud_) && click("ue-check") && ue().checking(), "«Проверить» on H");
                ue().check_vars().set("test.count", 0);
                return true;
            case 13:
                check(at(ue().layer_box(ue_wb_count_)), "«Счёт» clicked");
                return true;
            case 14:
                check(ue_sd_var("test.count") == 1, "with no window the click reaches H");
                check(at(ue().layer_box(ue_wb_open_)), "«Окно» clicked");
                return true;
            case 15: {
                if (wait(ue().check_windows() == std::vector<std::string>{w1}, "W1 comes up over H")) return true;
                const auto state = ue_wb_rows("ue-check-state");
                check(has(state, "Сверху окно «" + ue_wb_t1_ + "»: Esc его закроет", false) &&
                          has(state, "Окно «" + ue_wb_t1_ + "» затемняет то, что под ним: щелчки туда не проходят", false) &&
                          has(state, "Как в игре: мир и герой ждали бы, окно «" + ue_wb_t1_ + "» ставит игру на паузу", false),
                      "«Окна»: W1 on top, darkens, stops the game: " + ue_wb_text(state));
                check(ue().check_focus().rfind(w1 + ":", 0) == 0, "W1 has the keyboard: " + ue().check_focus());
                ue_wb_log_ = ue().check_log().size();
                check(at(ue().layer_box(ue_wb_count_)), "«Счёт» clicked under the veil");
                return true;
            }
            case 16:
                check(ue_sd_var("test.count") == 1 && ue().check_log().size() == ue_wb_log_, "the veil keeps the click off «Счёт»");
                check(ue().check_windows() == std::vector<std::string>{w1}, "and a click on the veil does not close W1");
                check(at(ue().check_box(w1, "Ещё")), "W1's «Ещё» clicked");
                return true;
            case 17: {
                check(ue().check_windows() == std::vector<std::string>{w1, w2}, "W2 comes up over W1 (over the game it may)");
                const auto state = ue_wb_rows("ue-check-state");
                check(has(state, "Сверху окно «" + ue_wb_t2_ + "»: по Esc не закрывается, окна под ним тоже", false) &&
                          has(state, "Под ним: «" + ue_wb_t1_ + "», ниже экран «" + ue_wb_th_ + "»", false),
                      "«Окна»: W2 on top, W1 under it: " + ue_wb_text(state));
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(ue().checking() && ue().check_windows() == std::vector<std::string>{w1, w2},
                      "Esc closes neither W2 (no Esc) nor W1 under it, nor the check");
                check(said() == "Esc: окно «" + ue_wb_t2_ + "» по Esc не закрывается; в игре Esc открыл бы паузу",
                      "the panel says what Esc does: " + said());
                check(esc_held() && ue().checking() && ue().check_windows() == std::vector<std::string>{w1, w2},
                      "Esc held over W2: its repeats close neither window nor end the check");
                check(at(ue().check_box(w1, "Ещё")), "W1's «Ещё» clicked beside W2's button");
                return true;
            }
            case 18:
                check(said() == "Окно уже открыто: «" + ue_wb_t2_ + "»" && ue().check_windows() == std::vector<std::string>{w1, w2},
                      "W2 does not darken: the click beside its button reaches W1 under it, and W2 stays where it is: " + said());
                check(at(ue().check_box(w2, "Закрыть2")), "W2's «Закрыть2» clicked");
                return true;
            case 19:
                check(ue().check_windows() == std::vector<std::string>{w1} && said() == "Закрыто окно «" + ue_wb_t2_ + "»",
                      "«Закрыть2» closed only W2");
                check(at(ue().check_box(w1, "Третье")), "W1's «Третье» clicked");
                return true;
            case 20: {
                if (wait(ue().check_windows() == std::vector<std::string>{w1, w3}, "W3 comes up over W1")) return true;
                const auto state = ue_wb_rows("ue-check-state");
                check(has(state, "Сверху окно «" + ue_wb_t3_ + "»: Esc его закроет", false) &&
                          has(state, "Под ним: «" + ue_wb_t1_ + "», ниже экран «" + ue_wb_th_ + "»", false),
                      "«Окна»: W3 on top, closed by Esc, W1 under it: " + ue_wb_text(state));
                // Two windows Esc closes, Esc held: one goes with the press, the repeats leave the other; a new press
                // closes it, and held again the check goes on.
                check(esc_held() && ue().check_windows() == std::vector<std::string>{w1} && said() == "Esc: закрыто окно «" + ue_wb_t3_ + "»",
                      "Esc held: W3 closed when it went down, W1 stays through the repeats and the key going up");
                check(esc_held() && ue().checking() && ue().check_windows().empty() && said() == "Esc: закрыто окно «" + ue_wb_t1_ + "»",
                      "a new press closes W1, and held its repeats do not end the check");
                return true;
            }
            case 21: {
                const auto state = ue_wb_rows("ue-check-state");
                check(state.size() == 1 && state[0].first == "Как в игре: мир шёл бы, ни одно открытое окно не ставит игру на паузу",
                      "«Окна»: in the game the world would go on: " + ue_wb_text(state));
                check(at(ue().layer_box(ue_wb_count_)), "«Счёт» clicked again");
                return true;
            }
            case 22:
                check(ue_sd_var("test.count") == 2, "with W1 gone the click reaches H again");
                ue_sd_key(SDLK_ESCAPE, true);
                check(!ue().checking(), "a new press of Esc ends the check");
                // Still held: the repeats do not go on to the editor (Esc there takes the selection up a level).
                ue().select({ue_wb_count_});
                for (int i = 0; i < 3; ++i) ue_sd_key(SDLK_ESCAPE, true, true);
                ue_sd_key(SDLK_ESCAPE, false);
                check(ue().selection() == std::vector<u32>{ue_wb_count_}, "the Esc that ended the check, held, does nothing in the editor");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(ue().selection().empty(), "a new Esc in the editor works as before: the selection goes up a level");
                // A window only for the main menu checked by itself: checked as over the menu, whatever its pause.
                check(ue().open(ue_wb_w3_) && ue().set_property("screen.esc", "") && !ue().screen().esc_closes, "W3 no longer closed by Esc");
                check(ue().open(ue_wb_w1_) && ue().set_property("screen.over", "menu") && ue().screen().pauses, "W1 only over the main menu");
                check(click("ue-check") && ue().checking(), "«Проверить» on W1 itself");
                return true;
            case 23: {
                const auto state = ue_wb_rows("ue-check-state");
                check(state.size() == 1 && state[0].first == "Как над главным меню: игра ещё не идёт, пауза окна ничего не останавливает",
                      "«Окна»: checked as over the main menu, not as in the game: " + ue_wb_text(state));
                check(at(ue().layer_box(ue_wb_more_)), "«Ещё» clicked");
                return true;
            }
            case 24:
                check(ue().check_windows().empty() &&
                          said() == "Окно «" + ue_wb_t2_ + "» не открылось: оно появляется только над игрой, а проверка идёт как над главным меню",
                      "W2 only over the game does not come up, and the panel says why: " + said());
                check(at(ue().layer_box(ue_wb_third_)), "«Третье» clicked");
                return true;
            case 25: {
                if (wait(ue().check_windows() == std::vector<std::string>{w3}, "W3 comes up over W1")) return true;
                const auto state = ue_wb_rows("ue-check-state");
                check(state.size() == 3 && has(state, "Сверху окно «" + ue_wb_t3_ + "»: по Esc не закрывается, окна под ним тоже", false) &&
                          has(state, "Как над главным меню: игра ещё не идёт, пауза окна ничего не останавливает", false),
                      "«Окна»: W3 on top without Esc, over the main menu: " + ue_wb_text(state));
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(ue().checking() && ue().check_windows() == std::vector<std::string>{w3} &&
                          said() == "Esc: окно «" + ue_wb_t3_ + "» по Esc не закрывается; над главным меню Esc больше ничего не делает",
                      "Esc: W3 stays, and the panel says what Esc does over the main menu: " + said());
                check(at(ue().check_box(w3, "Закрыть3")), "W3's «Закрыть3» clicked");
                return true;
            }
            case 26:
                check(ue().check_windows().empty() && said() == "Закрыто окно «" + ue_wb_t3_ + "»", "«Закрыть3» closed W3");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().checking(), "no window up: Esc ends the check");
                check(ue().open("main_menu"), "back to the menu");
                break;
            default: break;
            }
            ue_wb_stage_ = 0;
            break;
        }
        case 152: {
            // games/examples/window-behaviour (13.11), the files the game and its package read: opened from disk, its
            // windows show their behaviour in the panel; a change written and taken back writes the example's
            // content again (line ends aside: a Windows checkout has CRLF, the editor writes LF); opening them again
            // and «Проверить» write nothing; «Проверить» on its screen over the game stacks its windows as the game
            // does.
            namespace d = editor::design;
            const std::filesystem::path from = utf8_path(FORGE_EXAMPLES_DIR) / "window-behaviour" / "ui";
            static const char* const kPages[] = {"окна_меню", "окна_игра", "окна_настройки", "окна_справка"};
            auto lf = [](std::string text) {
                std::erase(text, '\r');
                return text;
            };
            auto example = [&](const char* name, const char* ext) {
                std::vector<u8> bytes;
                read_file(from / utf8_path(std::string(name) + ext), bytes);
                return std::string(bytes.begin(), bytes.end());
            };
            // The same content as the example's, whichever line ends either side has.
            auto same = [&](const char* name) {
                return lf(ue_file(".json", name)) == lf(example(name, ".json")) && lf(ue_file(".html", name)) == lf(example(name, ".html"));
            };
            // The files in the game's folder, byte for byte.
            auto files = [&]() {
                std::vector<std::string> out;
                for (const char* name : kPages)
                    for (const char* ext : {".json", ".html"}) out.push_back(ue_file(ext, name));
                return out;
            };
            auto said = [&]() { return ue().check_log().empty() ? std::string() : ue().check_log().back(); };
            auto at = [&](const std::optional<d::Rect>& b) {
                if (b) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return b.has_value();
            };
            const std::vector<std::string> one{"окна_настройки"}, two{"окна_настройки", "окна_справка"};
            switch (ue_we_stage_++) {
            case 0: {
                std::error_code ec;
                for (const char* name : kPages)
                    for (const char* ext : {".json", ".html"})
                        std::filesystem::copy_file(from / utf8_path(std::string(name) + ext), ed_.ui_game_dir / "ui" / utf8_path(std::string(name) + ext),
                                                   std::filesystem::copy_options::overwrite_existing, ec);
                check(!ec && ue().open("окна_справка"), "the example's help window opens from disk");
                ue().set_simple(false);
                ue().set_view(0);
                ue().select({ue().screen().root.id});
                click("ue-tab-game");
                return true;
            }
            case 1: {
                const d::Screen& s = ue().screen();
                check(s.show == d::ScreenShow::Command && s.over == d::WindowOver::Game && !s.esc_closes && !s.dim && !s.pauses,
                      "«Окна: справка» from disk: only over the game, no Esc, no veil, no pause");
                const auto rows = ue_wb_rows("ue-openers");
                check(rows.size() == 1 && !rows[0].second &&
                          rows[0].first == "Кнопка «Справка» на экране «Окна: настройки»: над окном «Окна: настройки»; откроется, только если и то окно над игрой",
                      "what opens it: the settings window's «Справка»: " + ue_wb_text(rows));
                // Written and taken back: the same files as the example's.
                check(click("ue-screen-dim") && ue().screen().dim && ue_file(".html", "окна_справка").find("forge-dim") != std::string::npos,
                      "«Затемнять» by a click: written on the page");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(!ue().screen().dim && same("окна_справка"), "Ctrl+Z: the files say the example's again (line ends aside)");
                check(ue().open("окна_настройки"), "«Окна: настройки» opened");
                ue().select({ue().screen().root.id});
                return true;
            }
            case 2: {
                const d::Screen& s = ue().screen();
                check(s.show == d::ScreenShow::Command && s.over == d::WindowOver::Any && s.esc_closes && s.dim && s.pauses,
                      "«Окна: настройки»: anywhere, closed by Esc, darkens, stops the game");
                const auto rows = ue_wb_rows("ue-openers");
                check(rows.size() == 2 && !rows[0].second && !rows[1].second &&
                          ue_wb_text(rows).find("Кнопка «Настройки» на экране «Окна: меню»: над главным меню") != std::string::npos &&
                          ue_wb_text(rows).find("Кнопка «Настройки» на экране «Окна: игра»: над игрой") != std::string::npos,
                      "what opens it: the menu's button over the menu, the game's over the game: " + ue_wb_text(rows));
                check(click("ue-screen-pauses") && !ue().screen().pauses, "«Пока открыто, игра стоит» off by a click");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue().screen().pauses, "Ctrl+Z: on again");
                // Opened again from disk: the same, and nothing written by opening.
                ue_we_files_ = files();
                check(ue().open("окна_справка") && ue().open("окна_настройки") && ue().screen().pauses && ue().screen().dim, "opened again: as it was");
                check(files() == ue_we_files_, "opening them again wrote nothing: the files are byte for byte as they were");
                bool all = true;
                for (const char* name : kPages) all = all && same(name);
                check(all, "the four screens still say what the example's files do, which the game and its package read (line ends aside)");
                // «Проверить» on the screen over the game.
                check(ue().open("окна_игра") && click("ue-check") && ue().checking(), "«Проверить» on «Окна: игра»");
                return true;
            }
            case 3:
                check(at(ue().layer_box(ue_named("Настройки"))), "«Настройки» clicked");
                return true;
            case 4:
                if (hold(ue().check_windows() == one, "the settings window comes up over the game")) {
                    --ue_we_stage_;
                    return true;
                }
                check(at(ue().check_box("окна_настройки", "Справка")), "«Справка» clicked");
                return true;
            case 5:
                check(ue().check_windows() == two, "the help window comes up over it (over the game it may)");
                ue().check_vars().set("demo.volume", 0);
                check(at(ue().check_box("окна_настройки", "Громче")), "«Громче» clicked beside the help window");
                return true;
            case 6:
                check(ue_sd_var("demo.volume") == 1 && ue().check_windows() == two, "the help window does not darken: the click reached the window under it");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(ue().check_windows() == two && said() == "Esc: окно «Окна: справка» по Esc не закрывается; в игре Esc открыл бы паузу",
                      "Esc: the help window stays, so does the one under it: " + said());
                check(at(ue().check_box("окна_справка", "Понятно")), "«Понятно» clicked (still on top after the click under it)");
                return true;
            case 7:
                check(ue().check_windows() == one, "«Понятно» closed the help window");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(ue().check_windows().empty() && ue().checking(), "Esc closed the settings window");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().checking(), "Esc ends the check");
                check(files() == ue_we_files_, "«Проверить» wrote nothing: the files are byte for byte as they were");
                {
                    bool all = true;
                    for (const char* name : kPages) all = all && same(name);
                    check(all, "and they still say what the example's files do (line ends aside)");
                }
                check(ue().open("main_menu"), "back to the menu");
                {
                    std::error_code ec;
                    for (const char* name : kPages)
                        for (const char* ext : {".json", ".html"}) std::filesystem::remove(ed_.ui_game_dir / "ui" / utf8_path(std::string(name) + ext), ec);
                }
                break;
            default: break;
            }
            ue_we_stage_ = 0;
            break;
        }
        case 153: {
            // The template library (13.12): the window from the bar, its groups and search (in both sections, any case,
            // every word), the templates' pictures drawn by the UI engine with the game's values; a construction taken
            // onto the screen as one step of the history (plain layers, fresh ids, a second copy apart from the first),
            // from the button, a double click and the empty canvas's menu; a screen template as a new screen with its
            // settings that «Проверить» runs; a second main menu as a window; nothing taken into «Компоненты»; Esc closes
            // the window, from its search too, and «Проверить» keeps it shut.
            namespace d = editor::design;
            const auto& all = ue().templates();
            auto index_of = [&](const char* file) -> usize {
                for (usize i = 0; i < all.size(); ++i)
                    if (all[i].info.file == file) return i;
                return all.size();
            };
            auto files_listed = [&]() {
                std::vector<std::string> out;
                for (usize i : ue().templates_listed()) out.push_back(all[i].info.file);
                return out;
            };
            auto joined = [](const std::vector<std::string>& v) {
                std::string out;
                for (const std::string& x : v) out += " " + x;
                return out;
            };
            auto wait = [&](bool ready, const char* what) {
                if (!hold(ready, what)) return false;
                --ue_tp_stage_;
                return true;
            };
            const std::filesystem::path own_dir = ed_.ui_game_dir / "ui" / "templates";
            auto own_file = [&](const std::string& file) {
                std::vector<u8> bytes;
                read_file(own_dir / utf8_path(file + ".json"), bytes);
                return std::string(bytes.begin(), bytes.end());
            };
            auto own_index = [&]() {
                std::vector<d::TemplateInfo> list;
                std::vector<u8> bytes;
                if (read_file(own_dir / "templates.json", bytes))
                    d::load_template_index(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), list);
                return list;
            };
            auto own_titled = [&](const std::string& title) -> usize {
                for (usize i = 0; i < all.size(); ++i)
                    if (all[i].info.own && all[i].info.title == title) return i;
                return all.size();
            };
            auto input_value = [&](const char* id) {
                auto* e = rmlui_dynamic_cast<Rml::ElementFormControl*>(ed_.find_element(id));
                return e ? std::string(e->GetValue()) : std::string("<нет поля>");
            };
            // A copy of the menu made from the own template: its copy of the game's component, linked as the original.
            // On the same screen every layer has an id of its own; a new screen keeps the ids of the one it was saved from.
            auto linked_like_menu = [&](const d::Node& copy, bool same_screen = true) {
                const d::Node& original = ue_tp_menu_node_;
                if (copy.children.size() != original.children.size()) return false;
                for (usize i = 0; i < copy.children.size(); ++i) {
                    const d::Node& a = copy.children[i];
                    const d::Node& b = original.children[i];
                    if (a.component != b.component || a.master != b.master || a.on_click != b.on_click || a.variant != b.variant ||
                        (same_screen && a.id == b.id))
                        return false;
                }
                return std::any_of(copy.children.begin(), copy.children.end(), [](const d::Node& c) { return !c.component.empty() && c.master; });
            };
            auto steps = [&] { return ue().history().cursor(); };
            auto select_shop = [&] {
                ue().templates_show("screen", "Магазин");
                return ue().select_template(index_of("screen_shop_dark"));
            };
            // A point of a template's picture, in its screen's pixels (the picture is the screen fitted whole).
            auto pixel = [&](const char* file, f32 x, f32 y, u8 px[4]) {
                const usize i = index_of(file);
                const f32 k = static_cast<f32>(UiEditor::kTemplateThumbW) / 1920.0f;
                return i < all.size() && ue().read_template_pixel(i, static_cast<u32>(x * k), static_cast<u32>(y * k), px);
            };
            auto dblclick = [&](const std::string& id) {
                f32 x = 0, y = 0;
                if (!element_center(id, x, y)) return false;
                left_click(x, y);
                left_click(x, y);
                return true;
            };
            // Plain layers with ids of their own on the screen, none twice.
            auto plain = [&](const d::Node& n) {
                bool ok = true;
                auto visit = [&](auto&& self, const d::Node& m) -> void {
                    ok = ok && m.component.empty() && m.master == 0 && m.text_style.style.empty();
                    for (const d::Paint& f : m.fills) ok = ok && f.style.empty();
                    for (const d::Node& c : m.children) self(self, c);
                };
                visit(visit, n);
                return ok;
            };
            auto ids_unique = [&]() {
                std::set<u32> seen;
                bool ok = true;
                auto visit = [&](auto&& self, const d::Node& m) -> void {
                    ok = ok && seen.insert(m.id).second && m.id < ue().screen().next_id;
                    for (const d::Node& c : m.children) self(self, c);
                };
                visit(visit, ue().screen().root);
                return ok;
            };
            switch (ue_tp_stage_++) {
            case 0:
                check(ue().open("main_menu") && !ue().checking(), "«Главное меню» open");
                ue_tp_disk_ = ue_file(".json");
                ue_tp_html_ = ue_file(".html");
                ue_tp_screens_ = ue().screens();
                ue_tp_layers_ = ue().screen().root.children.size();
                check(!ue().templates_open() && !shown("ue-tpl"), "the template library is closed");
                check(click("ue-templates") && ue().templates_open(), "«Шаблоны» on the bar opens it");
                return true;
            case 1: {
                check(shown("ue-tpl") && shown("ue-tpl-search") && shown("ue-tpl-sec-construction") && shown("ue-tpl-sec-screen"),
                      "the window shows its sections and search");
                check(all.size() == 29, "29 templates: " + std::to_string(all.size()));
                const std::vector<usize> c = ue().templates_listed();
                bool constructions = c.size() == 15, cards = true;
                for (usize i : c) {
                    constructions = constructions && all[i].info.kind == d::TemplateKind::Construction;
                    cards = cards && shown("ue-tpl-card-" + std::to_string(i));
                }
                check(constructions && cards, "it opens on the 15 constructions, a card each");
                check(!c.empty() && ue().selected_template() == static_cast<int>(c.front()) && shown("ue-tpl-insert") &&
                          !shown("ue-tpl-new-screen"),
                      "the first one selected; a construction is put on the screen («Вставить в экран»), not made a screen");
                check(shown("ue-tpl-group-0") && shown("ue-tpl-group-10"), "the groups of both sections are listed");
                check(ed_.find_element("ue-tpl-group-0")->IsClassSet("off") && ed_.find_element("ue-tpl-group-5")->IsClassSet("off"),
                      "«Мои» first in both, empty: the game has no templates of its own yet");
                return true;
            }
            case 2: {
                // The pictures: a few made each frame, each drawn a few frames, then kept.
                bool drawn = true;
                for (usize i = 0; i < all.size(); ++i) drawn = drawn && ue().template_drawn(i);
                if (hold(drawn, "every template's picture is drawn")) {
                    --ue_tp_stage_;
                    return true;
                }
                Rml::Element* img = ed_.find_element("ue-tpl-preview");
                Rml::Element* card = ed_.find_element(("ue-tpl-card-" + std::to_string(ue().selected_template())).c_str());
                Rml::Element* thumb = card ? card->QuerySelector("img") : nullptr;
                check(img && thumb && img->GetAttribute<Rml::String>("src", "").rfind("/gpu/ui-tpl-", 0) == 0 &&
                          img->GetAttribute<Rml::String>("src", "") == thumb->GetAttribute<Rml::String>("src", ""),
                      "the selected one's picture is beside the cards");
                u8 corner[4] = {}, middle[4] = {}, gold[4] = {}, hearts[4] = {}, hearts_left[4] = {};
                // «Сетка предметов. Дерево»: on the editor's dark, the parchment in its middle (the game's picture).
                const usize wood = index_of("grid_items_wood");
                check(wood < all.size() && ue().read_template_pixel(wood, 3, 3, corner) && ue().read_template_pixel(wood, 200, 112, middle),
                      "the wooden grid's picture is read");
                check(corner[0] == 0x23 && corner[1] == 0x27 && corner[2] == 0x2e, "a construction stands on the editor's dark");
                check(middle[0] > 150 && middle[1] > 120 && middle[0] > middle[2] + 30,
                      "the parchment in its middle: the picture of the game's folder is drawn (" + std::to_string(middle[0]) + "," +
                          std::to_string(middle[1]) + "," + std::to_string(middle[2]) + ")");
                // «Пауза. Тёмная»: its third button, gold.
                check(pixel("screen_pause_dark", 960, 580, gold) && gold[0] > 200 && gold[1] > 150 && gold[1] < 200 && gold[2] < 110,
                      "the dark pause's buttons are gold in its picture");
                // «Над игрой. Тёмная»: the hearts' bar shows the game's value, 7 of 10: red near its start, not at its end.
                check(pixel("screen_hud_dark", 300, 115, hearts) && hearts[0] > 150 && hearts[1] < 100,
                      "the HUD's hearts are red where the value reaches");
                check(pixel("screen_hud_dark", 420, 115, hearts_left) && hearts_left[0] < 100,
                      "and dark past 7 of 10: the picture has the game's values");
                // The search: in both sections, any case, every word.
                check(ue_fl_type_only("ue-tpl-search", "предмет"), "typed «предмет» into the search");
                return true;
            }
            case 3: {
                const std::vector<std::string> found = files_listed();
                check(found == std::vector<std::string>{"grid_items_dark", "grid_items_wood", "card_item_dark", "card_item_wood",
                                                        "screen_inventory_dark", "screen_inventory_wood"},
                      "«предмет» finds grids, cards and inventories, constructions and screens:" + joined(found));
                check(!ed_.find_element("ue-tpl-group-0")->IsClassSet("selected") && !ed_.find_element("ue-tpl-sec-construction")->IsClassSet("selected"),
                      "while searching no group is lit");
                check(ue_fl_type_only("ue-tpl-search", "ПАУЗА тёмная"), "typed «ПАУЗА тёмная»");
                return true;
            }
            case 4: {
                const std::vector<std::string> found = files_listed();
                check(found == std::vector<std::string>{"screen_pause_dark"}, "any case, every word: only the dark pause:" + joined(found));
                check(ue_fl_type_only("ue-tpl-search", "жираф"), "typed «жираф»");
                return true;
            }
            case 5:
                check(ue().templates_listed().empty() && shown("ue-tpl-empty"), "nothing found: said so, with words to try");
                // Esc in the search field closes the window (the field has the keyboard).
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().templates_open(), "Esc from the search closes the library");
                check(!ed_.context()->GetFocusElement() || ed_.context()->GetFocusElement()->GetId() != "ue-tpl-search",
                      "the hidden search field gives the keyboard back (Ctrl+Z, Delete, the arrows are the screen's again)");
                check(click("ue-templates") && ue().templates_open(), "opened again");
                return true;
            case 6: {
                check(shown("ue-tpl") && ue().templates_listed().empty(), "it opens as it was left: the search kept");
                check(click("ue-tpl-group-3"), "«Карточки» clicked");
                return true;
            }
            case 7: {
                const std::vector<std::string> found = files_listed();
                check(found == std::vector<std::string>{"card_item_dark", "card_item_wood", "card_quest_dark", "card_portrait_dark",
                                                        "card_portrait_wood"} &&
                          ed_.find_element("ue-tpl-group-3")->IsClassSet("selected"),
                      "a group shows its own, lit, the search cleared:" + joined(found));
                const usize wood = index_of("card_item_wood");
                check(click("ue-tpl-card-" + std::to_string(wood)) && ue().selected_template() == static_cast<int>(wood), "a click selects «Карточка предмета. Дерево»");
                return true;
            }
            case 8: {
                const usize wood = index_of("card_item_wood"), dark = index_of("card_item_dark");
                check(shown("ue-tpl-variant-" + std::to_string(wood)) && shown("ue-tpl-variant-" + std::to_string(dark)) &&
                          ed_.find_element(("ue-tpl-variant-" + std::to_string(wood)).c_str())->IsClassSet("selected"),
                      "its looks beside it, «Дерево» lit");
                check(click("ue-tpl-variant-" + std::to_string(dark)) && ue().selected_template() == static_cast<int>(dark), "«Тёмная» picked");
                ue_tp_json_ = d::save_screen(ue().screen()); // the screen before (as the editor holds it)
                check(click("ue-tpl-insert"), "«Вставить в экран»");
                return true;
            }
            case 9: {
                const d::Screen& s = ue().screen();
                check(!ue().templates_open(), "taking it closes the window");
                check(s.root.children.size() == ue_tp_layers_ + 1 && ue().selection().size() == 1 &&
                          ue().selection()[0] == s.root.children.back().id && s.root.children.back().name == "Карточка «Меч»",
                      "the card is on the screen, on top, selected");
                const d::Node& card = s.root.children.back();
                check(card.x >= 0 && card.y >= 0 && card.x + card.w <= s.width && card.y + card.h <= s.height, "inside the screen");
                check(plain(card) && ids_unique(), "plain layers with ids of their own");
                check(ue().history().undo_label() == "Из шаблонов: Карточка предмета", "one step of the history: " + ue().history().undo_label());
                check(ue_file(".json").find("Карточка «Меч»") != std::string::npos && ue_file(".html").find("Карточка «Меч»") != std::string::npos,
                      "written with the page");
                ue_tp_first_ = card.id;
                key(SDLK_Z, SDL_KMOD_CTRL);
                const std::string undone = d::save_screen(ue().screen());
                check(undone == ue_tp_json_, "Ctrl+Z: the screen as before, all of it in one step");
                if (undone != ue_tp_json_) {
                    usize at = 0;
                    while (at < undone.size() && at < ue_tp_json_.size() && undone[at] == ue_tp_json_[at]) ++at;
                    FORGE_INFO("self-test: after Ctrl+Z «%s» / before «%s»", undone.substr(at > 80 ? at - 80 : 0, 200).c_str(),
                               ue_tp_json_.substr(at > 80 ? at - 80 : 0, 200).c_str());
                }
                key(SDLK_Y, SDL_KMOD_CTRL);
                check(ue().screen().root.children.size() == ue_tp_layers_ + 1 && ue().screen().root.children.back().id == ue_tp_first_,
                      "Ctrl+Y: the card back, the same layers");
                // The empty canvas's menu: «Из шаблонов…».
                ue().select({});
                right_click(ue_wx(1900), ue_wy(1060));
                return true;
            }
            case 10:
                check(ue().menu() == "empty" && shown("ue-ctx-templates"), "the right button on nothing: the screen's menu has «Из шаблонов…»");
                check(click("ue-ctx-templates") && ue().templates_open() && ue().menu().empty(), "it opens the library");
                return true;
            case 11: {
                const usize i = index_of("card_item_dark");
                check(shown("ue-tpl-card-" + std::to_string(i)) && dblclick("ue-tpl-card-" + std::to_string(i)), "a double click on the dark card");
                return true;
            }
            case 12: {
                const d::Screen& s = ue().screen();
                check(!ue().templates_open() && s.root.children.size() == ue_tp_layers_ + 2, "a second card on the screen");
                const d::Node& a = *d::find(s.root, ue_tp_first_);
                const d::Node& b = s.root.children.back();
                check(b.id != a.id && ids_unique() && (b.x != a.x || b.y != a.y), "a copy apart: its own ids, its own place");
                // One copy changed: the other is as it was (no link between copies, nor to the template).
                const u32 label = ue_named(b, "Надпись");
                ue().select({label});
                check(label != 0 && ue().set_property("text", "Купить") && ue_node(label)->text == "Купить", "the second card's button says «Купить»");
                check(ue_named(*d::find(ue().screen().root, ue_tp_first_), "Надпись") != 0 &&
                          ue_node(ue_named(*d::find(ue().screen().root, ue_tp_first_), "Надпись"))->text == "Взять" &&
                          all[index_of("card_item_dark")].screen.root.children.front().children.back().children.front().text == "Взять",
                      "the first card and the template still say «Взять»");
                // A screen from a template: under the list of screens.
                check(click("ue-new-screen-template") && ue().templates_open(), "«Из шаблона» opens the library");
                return true;
            }
            case 13: {
                const std::vector<usize> listed = ue().templates_listed();
                bool screens = listed.size() == 14;
                for (usize i : listed) screens = screens && all[i].info.kind == d::TemplateKind::Screen;
                check(screens && ed_.find_element("ue-tpl-sec-screen")->IsClassSet("selected"), "on the screens, 14 of them");
                check(click("ue-tpl-card-" + std::to_string(index_of("screen_pause_dark"))), "«Пауза. Тёмная» clicked");
                return true;
            }
            case 14:
                check(ue().selected_template() == static_cast<int>(index_of("screen_pause_dark")) && shown("ue-tpl-new-screen") &&
                          shown("ue-tpl-insert") && ed_.find_element("ue-tpl-insert")->IsClassSet("strong") == false,
                      "a screen template: «Новый экран» first, and «Вставить рамкой в экран»");
                // Enter in the search field takes the one selected.
                check(ue_fl_type_only("ue-tpl-search", "пауза") && ue().templates_listed().size() == 3 &&
                          ue().selected_template() == static_cast<int>(index_of("screen_pause_dark")),
                      "«пауза» typed: three pauses, the dark one still selected");
                key(SDLK_RETURN, SDL_KMOD_NONE);
                return true;
            case 15: {
                const d::Screen& s = ue().screen();
                check(!ue().templates_open() && ue().opened() == "пауза" && s.title == "Пауза", "Enter: a new screen «Пауза», open");
                check(!ed_.context()->GetFocusElement() || ed_.context()->GetFocusElement()->GetId() != "ue-tpl-search",
                      "the keyboard is the editor's again");
                check(s.show == d::ScreenShow::Command && s.pauses && s.dim && s.esc_closes && s.appear == d::Appear::Zoom,
                      "with the template's settings: a window that stops the game, darkens, closes by Esc, grows in");
                check(!ue_file(".json", "пауза").empty() && !ue_file(".html", "пауза").empty() && shown("ue-screen-пауза"),
                      "its files written, it is in the list of screens");
                check(plain(s.root) && ids_unique(), "plain layers with ids of their own");
                check(click("ue-check") && ue().checking(), "«Проверить» on it");
                return true;
            }
            case 16: {
                ue_log_size_ = ue().check_log().size();
                check(ue().layer_box(ue_named("Кнопка «Продолжить»")).has_value(), "its «Продолжить» is on the page");
                if (auto b = ue().layer_box(ue_named("Кнопка «Продолжить»"))) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return true;
            }
            case 17:
                check(ue().check_log().size() == ue_log_size_ + 1 && ue().check_log().back() == "Экран закрыт",
                      "a click on it closes the pause, as in the game: " + ue_log_since(ue_log_size_));
                // «Проверить» keeps the library shut, as «Создать».
                check(click("ue-templates") && !ue().templates_open(), "«Шаблоны» does nothing while the screen is checked");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().checking(), "Esc ends the check");
                // A second main menu: the game has one, so it comes as a window, and the author is told how to switch.
                check(click("ue-templates") && ue().templates_open(), "the library again");
                return true;
            case 18:
                check(click("ue-tpl-sec-screen") && ue().templates_listed().size() == 14, "«Экраны»");
                return true;
            case 19:
                check(click("ue-tpl-card-" + std::to_string(index_of("screen_menu_wood"))) &&
                          ue().selected_template() == static_cast<int>(index_of("screen_menu_wood")),
                      "«Главное меню. Дерево»");
                return true;
            case 20:
                check(click("ue-tpl-new-screen"), "«Новый экран»");
                return true;
            case 21: {
                const d::Screen& s = ue().screen();
                check(ue().opened() == "главное_меню" && s.title == "Главное меню 2", "a new screen «Главное меню 2»: " + ue().opened() + ", " + s.title);
                check(s.show == d::ScreenShow::Command && ue().move_note().find("«Главное меню игры»") != std::string::npos,
                      "the game has a main menu: this one is a window, and the note says how to make it the menu: " + ue().move_note());
                // Nothing goes into «Компоненты»: no «Шаблоны» on its bar; «Из шаблона» makes a screen, puts nothing there.
                check(ue().open_library(), "«Компоненты» open");
                return true;
            }
            case 22:
                check(!shown("ue-templates") && shown("ue-new-screen-template"), "«Компоненты»: no «Шаблоны» on the bar");
                check(click("ue-new-screen-template") && ue().templates_open(), "«Из шаблона» from «Компоненты»");
                return true;
            case 23: {
                const std::string before = d::save_screen(ue().library());
                check(ed_.find_element("ue-tpl-insert") && ed_.find_element("ue-tpl-insert")->IsClassSet("disabled"), "«Вставить в экран» is off there");
                const u32 put = ue().insert_template(static_cast<usize>(ue().selected_template()));
                check(put == 0 && d::save_screen(ue().library()) == before && ue().templates_note().find("«Компоненты»") != std::string::npos,
                      "and takes nothing, saying why: " + ue().templates_note());
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().templates_open(), "Esc closes it");
                // Back as it was: the new screens gone, «Главное меню» as before the cards.
                std::error_code ec;
                for (const std::string& name : ue().screens())
                    if (std::find(ue_tp_screens_.begin(), ue_tp_screens_.end(), name) == ue_tp_screens_.end())
                        for (const char* ext : {".json", ".html"}) std::filesystem::remove(ed_.ui_game_dir / "ui" / utf8_path(name + ext), ec);
                check(ue().screens() == ue_tp_screens_, "the screens made here removed");
                for (const auto& [ext, text] : {std::pair{".json", &ue_tp_disk_}, std::pair{".html", &ue_tp_html_}})
                    write_file_atomic(ed_.ui_game_dir / "ui" / utf8_path(std::string("main_menu") + ext),
                                      {reinterpret_cast<const u8*>(text->data()), text->size()});
                check(ue().open("main_menu") && ue().screen().root.children.size() == ue_tp_layers_ && ue_file(".json") == ue_tp_disk_,
                      "«Главное меню» as before the cards");
                return true;
            }
            // --- «Сохранить как шаблон»: the author's own, with a copy of the game's component inside ---
            case 24: {
                std::error_code ec;
                check(!std::filesystem::exists(own_dir, ec), "the game has no templates of its own yet");
                ue_tp_lib_json_ = ue_file(".json", "components");
                ue_tp_lib_html_ = ue_file(".html", "components");
                ue_tp_menu_ = ue_named("Меню");
                auto linked = [&] {
                    const d::Node* menu = ue_node(ue_tp_menu_);
                    return menu && std::any_of(menu->children.begin(), menu->children.end(),
                                               [](const d::Node& c) { return !c.component.empty() && c.master; });
                };
                if (!linked()) {
                    ue().select({ue_named("Кнопка «Новая игра»")});
                    ue().make_component();
                }
                check(ue_tp_menu_ && linked(), "«Меню» holds a copy of the game's component");
                // The note about «Главное меню 2» stayed with that screen: another screen opened, it is gone.
                check(ue().move_note().empty(), "no note about another screen over the list of layers: " + ue().move_note());
                ue_tp_menu_node_ = *ue_node(ue_tp_menu_);
                ue_tp_json_ = d::save_screen(ue().screen());
                ue_tp_menu_file_ = ue_file(".json");
                ue_tp_steps_ = steps();
                // The full list of layers, «Меню»'s row brought into sight before the right button goes to it.
                ue().set_simple(false);
                ue().set_view(0);
                if (Rml::Element* row = ed_.find_element(("ue-layer-" + std::to_string(ue_tp_menu_)).c_str()))
                    row->ScrollIntoView(Rml::ScrollAlignment::Nearest);
                return true;
            }
            case 25:
                if (wait(shown("ue-layer-" + std::to_string(ue_tp_menu_)), "«Меню»'s row is laid out")) return true;
                {
                    // The game has many screens by now: they scroll in their own place, the layers keep room.
                    Rml::Element* list = ed_.find_element(("ue-layer-" + std::to_string(ue_tp_menu_)).c_str());
                    list = list ? list->GetParentNode() : nullptr;
                    Rml::Element* screens = ed_.find_element("ue-screens");
                    check(list && list->GetClientHeight() >= 3 * 28 && screens && screens->GetOffsetHeight() <= 241,
                          "the list of layers keeps room among " + std::to_string(ue().screens().size()) + " screens: " +
                              std::to_string(list ? list->GetClientHeight() : -1.0f) + " px");
                }
                check(ue_cm_right_row(ue_tp_menu_), "the right button on «Меню»'s row");
                return true;
            case 26:
                if (wait(shown("ue-ctx-save-template"), "the layer's menu is laid out")) return true;
                check(ue().menu() == "layer" && ue().selection() == std::vector<u32>{ue_tp_menu_}, "«Меню» selected, its menu open");
                check(click("ue-ctx-save-template") && ue().saving_template() && ue().menu().empty(), "«Сохранить как шаблон…»");
                return true;
            case 27:
                if (wait(shown("ue-tpl-save-form"), "the form is laid out")) return true;
                check(input_value("ue-tpl-save-title") == "Меню" && ed_.find_element("ue-tpl-save-group-0")->IsClassSet("selected"),
                      "the form: the layer's name, the group «Мои»: " + input_value("ue-tpl-save-title"));
                check(ue().templates_listed().empty() && shown("ue-tpl-empty"), "beside it the game's own constructions: none yet, and how to make one");
                check(d::save_screen(ue().screen()) == ue_tp_json_ && steps() == ue_tp_steps_, "the form changes nothing on the screen, no step");
                check(ue_fl_type_only("ue-tpl-save-title", "Моё меню"), "a title typed");
                return true;
            case 28: {
                key(SDLK_RETURN, SDL_KMOD_NONE);
                const std::vector<d::TemplateInfo> index = own_index();
                check(!ue().saving_template() && ue().templates_open() && index.size() == 1 && index[0].file == "моё_меню" &&
                          index[0].title == "Моё меню" && index[0].group == d::kOwnGroup && index[0].kind == d::TemplateKind::Construction,
                      "Enter saves it into the game's ui/templates: its line in the index, «Мои»");
                ue_tp_own_ = own_file("моё_меню");
                check(!ue_tp_own_.empty() && ue_tp_own_.find("Кнопка «Настройки»") != std::string::npos, "and its file");
                const usize mine = own_titled("Моё меню");
                check(mine < all.size() && ue().selected_template() == static_cast<int>(mine) && ue().templates_note().find("ui/templates/моё_меню.json") != std::string::npos,
                      "it comes up selected among the templates, the note says where it lies: " + ue().templates_note());
                check(d::save_screen(ue().screen()) == ue_tp_json_ && ue_file(".json") == ue_tp_menu_file_ && steps() == ue_tp_steps_,
                      "the screen and its file as they were, no step of the history");
                return true;
            }
            case 29: {
                const usize mine = own_titled("Моё меню");
                Rml::Element* card = ed_.find_element(("ue-tpl-card-" + std::to_string(mine)).c_str());
                Rml::Element* badge = card ? card->QuerySelector(".ue-tpl-own") : nullptr;
                check(badge && badge->IsVisible(), "its card is marked as the game's own");
                check(!shown("ue-tpl-needs") && ue().template_needs(mine).empty(), "it needs nothing this game lacks: " + joined(ue().template_needs(mine)));
                // The same title again, in other letters: said, and saved beside, never over it.
                check(click("ue-tpl-save-open") && ue().saving_template(), "«Сохранить свой…» in the window: the form again");
                return true;
            }
            case 30:
                if (wait(shown("ue-tpl-save-form"), "the form again")) return true;
                check(ue_fl_type_only("ue-tpl-save-title", "моё МЕНЮ"), "the same title in other letters");
                return true;
            case 31:
                if (wait(shown("ue-tpl-save-clash"), "the clash is said")) return true;
                check(shown("ue-tpl-replace") && ed_.find_element("ue-tpl-save")->GetInnerRML().find("Сохранить рядом") != std::string::npos,
                      "«Заменить» or «Сохранить рядом»");
                check(click("ue-tpl-save") && own_index().size() == 2 && own_index()[1].title == "моё МЕНЮ 2" &&
                          own_index()[1].file == "моё_меню_2" && own_file("моё_меню") == ue_tp_own_,
                      "beside it: «моё МЕНЮ 2» in a file of its own, the first not touched");
                check(click("ue-tpl-save-open"), "the form once more");
                return true;
            case 32:
                if (wait(shown("ue-tpl-save-form"), "the form once more")) return true;
                check(ue_fl_type_only("ue-tpl-save-title", "Моё меню"), "the first one's title");
                return true;
            case 33:
                if (wait(shown("ue-tpl-save-group-4"), "the groups are laid out")) return true;
                check(click("ue-tpl-save-group-4"), "group «Списки»");
                return true;
            case 34: {
                if (wait(ed_.find_element("ue-tpl-save-group-4")->IsClassSet("selected") && shown("ue-tpl-replace"), "«Списки» lit, «Заменить» offered"))
                    return true;
                check(click("ue-tpl-replace"), "«Заменить»");
                const std::vector<d::TemplateInfo> index = own_index();
                check(index.size() == 2 && index[0].file == "моё_меню" && index[0].group == "Списки" && index[1].file == "моё_меню_2" &&
                          !std::filesystem::exists(own_dir / "моё_меню_3.json"),
                      "replaced in its place: the same file, its new group, nothing added");
                check(d::save_screen(ue().screen()) == ue_tp_json_ && ue_file(".json") == ue_tp_menu_file_ && steps() == ue_tp_steps_,
                      "the screen still as it was, no step");
                // As when the project is opened again: the folder read anew.
                std::vector<std::string> errors;
                const std::vector<d::Template> read = d::load_templates(own_dir, &errors);
                check(errors.empty() && read.size() == 2 && read[0].info.title == "Моё меню" && read[1].info.title == "моё МЕНЮ 2",
                      "read from the game's folder: both");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().templates_open() && click("ue-templates") && ue().templates_open(), "closed and opened again");
                return true;
            }
            case 35: {
                if (wait(shown("ue-tpl-search"), "the window again")) return true;
                check(ue_fl_type_only("ue-tpl-search", "моё меню"), "searched «моё меню»");
                return true;
            }
            case 36: {
                const usize a = own_titled("Моё меню"), b = own_titled("моё МЕНЮ 2");
                const std::vector<usize> listed = ue().templates_listed();
                check(a < all.size() && b < all.size() && listed == std::vector<usize>{a, b}, "the search finds both own, read from the folder");
                check(click("ue-tpl-card-" + std::to_string(a)) && ue().selected_template() == static_cast<int>(a), "«Моё меню» picked");
                ue_tp_steps_ = steps();
                return true;
            }
            case 37: {
                key(SDLK_RETURN, SDL_KMOD_NONE);
                const d::Screen& sc = ue().screen();
                const d::Node& copy = sc.root.children.back();
                check(!ue().templates_open() && steps() == ue_tp_steps_ + 1 && ue().history().undo_label() == "Из шаблонов: Моё меню",
                      "Enter puts it on the screen: one step");
                check(linked_like_menu(copy) && ids_unique(), "the copy: ids of its own, its copy of the game's component linked as the menu's");
                d::Screen synced = sc;
                check(!d::sync_instances(synced, ue().library()), "and in step with the component: syncing changes nothing");
                ue_tp_copy_ = copy.id;
                check(click("ue-templates"), "the library again");
                return true;
            }
            case 38: {
                const usize a = own_titled("Моё меню");
                if (wait(shown("ue-tpl-card-" + std::to_string(a)), "its card")) return true;
                check(dblclick("ue-tpl-card-" + std::to_string(a)), "a double click: a second copy");
                return true;
            }
            case 39: {
                const d::Screen& sc = ue().screen();
                const d::Node& second = sc.root.children.back();
                check(steps() == ue_tp_steps_ + 2 && second.id != ue_tp_copy_ && linked_like_menu(second) && ids_unique(),
                      "two copies, apart: every id its own, both linked to the component");
                // One changed: the other and the template not.
                const u32 label = ue_named(second, "Новая игра");
                auto first_text = [&]() {
                    const d::Node* first = d::find(ue().screen().root, ue_tp_copy_);
                    const d::Node* t = first ? ue_node(ue_named(*first, "Новая игра")) : nullptr;
                    return t ? t->text : std::string("?");
                };
                const std::string was = first_text();
                ue().select({label});
                check(label && ue().set_property("text", "Начать") && ue_node(label) && ue_node(label)->text == "Начать" && first_text() == was &&
                          own_file("моё_меню").find("Начать") == std::string::npos,
                      "the second copy's text changed: the first and the template not (" + was + ")");
                key(SDLK_Z, SDL_KMOD_CTRL);
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue().screen().root.children.back().id == ue_tp_copy_ && steps() == ue_tp_steps_ + 1, "Ctrl+Z twice: the text, then the second copy");
                key(SDLK_Y, SDL_KMOD_CTRL);
                check(ue().screen().root.children.back().id != ue_tp_copy_ && linked_like_menu(ue().screen().root.children.back()),
                      "Ctrl+Y: the second copy back, whole");
                // Left without saving: nothing written, no step.
                ue().select({});
                ue_tp_steps_ = steps();
                right_click(ue_wx(1900), ue_wy(1060));
                return true;
            }
            case 40:
                if (wait(shown("ue-ctx-save-screen-template"), "the screen's menu")) return true;
                check(click("ue-ctx-save-screen-template") && ue().saving_template(), "«Сохранить экран как шаблон…»");
                return true;
            case 41:
                if (wait(shown("ue-tpl-save-form"), "the screen's form")) return true;
                check(input_value("ue-tpl-save-title") == "Главное меню" && ed_.find_element("ue-tpl-save-what")->GetInnerRML().find("целиком") != std::string::npos,
                      "the whole screen, under its title");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().templates_open() && own_index().size() == 2 && steps() == ue_tp_steps_, "Esc: nothing saved, no step");
                // From the window with nothing selected: the whole screen.
                check(click("ue-templates"), "the library");
                return true;
            case 42:
                if (wait(shown("ue-tpl-save-open"), "the window")) return true;
                check(click("ue-tpl-save-open") && ue().saving_template(), "«Сохранить свой…», nothing selected");
                return true;
            case 43:
                if (wait(shown("ue-tpl-save-form"), "the screen's form")) return true;
                check(ue_fl_type_only("ue-tpl-save-title", "Мой главный экран"), "a title");
                return true;
            case 44: {
                ue_tp_menu_file_ = ue_file(".json");
                key(SDLK_RETURN, SDL_KMOD_NONE);
                const usize mine = own_titled("Мой главный экран");
                check(mine < all.size() && all[mine].info.kind == d::TemplateKind::Screen && own_index().size() == 3 &&
                          ue().selected_template() == static_cast<int>(mine) && ue_file(".json") == ue_tp_menu_file_ && steps() == ue_tp_steps_,
                      "a screen template of its own, the screen as it was");
                // Taken as a new screen: Enter again.
                key(SDLK_RETURN, SDL_KMOD_NONE);
                const d::Screen& sc = ue().screen();
                check(ue().opened() == "мой_главный_экран" && sc.title == "Мой главный экран" && sc.show == d::ScreenShow::Command,
                      "a new screen from it; the game has a main menu, so a window: " + ue().opened());
                const d::Node* menu = nullptr;
                for (const d::Node& c : sc.root.children)
                    if (c.name == "Меню") menu = &c;
                check(menu && linked_like_menu(*menu, false), "with the menu and its copy of the component linked");
                check(ue().open("main_menu") && ue_file(".json") == ue_tp_menu_file_, "«Главное меню» not touched");
                check(click("ue-templates"), "the library");
                ue().templates_show("construction", d::kOwnGroup); // as the chips «Конструкции» › «Мои»
                return true;
            }
            case 45: {
                const usize b = own_titled("моё МЕНЮ 2");
                if (wait(shown("ue-tpl-card-" + std::to_string(b)), "the own card")) return true;
                check(click("ue-tpl-card-" + std::to_string(b)), "«моё МЕНЮ 2» picked");
                return true;
            }
            case 46:
                if (wait(shown("ue-tpl-remove"), "«Удалить шаблон»")) return true;
                check(!shown("ue-tpl-confirm") && click("ue-tpl-remove"), "«Удалить шаблон»");
                return true;
            case 47: {
                if (wait(shown("ue-tpl-confirm"), "the question")) return true;
                const std::string menu = ue_file(".json");
                check(click("ue-tpl-remove-yes"), "«Удалить»");
                const std::vector<d::TemplateInfo> index = own_index();
                check(index.size() == 2 && index[0].file == "моё_меню" && index[1].title == "Мой главный экран" &&
                          !std::filesystem::exists(own_dir / utf8_path("моё_меню_2.json")) && own_titled("моё МЕНЮ 2") == all.size(),
                      "its file and its line gone, the others kept");
                check(ue_file(".json") == menu, "the copies on the screen not touched");
                // What a template needs of the game: said beside it.
                check(select_shop(), "«Магазин. Тёмная»");
                return true;
            }
            case 48: {
                if (wait(shown("ue-tpl-needs"), "what it needs")) return true;
                const std::vector<std::string> needs = ue().template_needs(index_of("screen_shop_dark"));
                check(!needs.empty() && needs[0].find("В игре нет вещей") != std::string::npos && needs[0].find("«sword»") != std::string::npos,
                      "the shop: the things this game lacks, and what happens:" + joined(needs));
                const std::vector<std::string> hud = ue().template_needs(index_of("screen_hud_dark"));
                check(std::any_of(hud.begin(), hud.end(), [](const std::string& n) { return n.find("«hero.xp»") != std::string::npos; }),
                      "the HUD: the values the game does not keep:" + joined(hud));
                const std::vector<std::string> form = ue().template_needs(index_of("form_confirm_dark"));
                check(std::any_of(form.begin(), form.end(), [](const std::string& n) { return n.find("«подтвердил»") != std::string::npos; }),
                      "the confirmation: its messages only «Логика» answers:" + joined(form));
                check(ue().template_needs(index_of("screen_pause_dark")).empty(), "the pause needs nothing");
                // Taken, it says so over the canvas.
                check(click("ue-tpl-new-screen") && ue().move_note().find("«sword»") != std::string::npos,
                      "made a screen: the note over the canvas says it too: " + ue().move_note());
                return true;
            }
            case 49: {
                // Back as it was: the screens, the menu, the components, no templates of its own.
                std::error_code ec;
                for (const std::string& name : ue().screens())
                    if (std::find(ue_tp_screens_.begin(), ue_tp_screens_.end(), name) == ue_tp_screens_.end())
                        for (const char* ext : {".json", ".html"}) std::filesystem::remove(ed_.ui_game_dir / "ui" / utf8_path(name + ext), ec);
                std::filesystem::remove_all(own_dir, ec);
                for (const auto& [name, text] : {std::pair{"main_menu.json", &ue_tp_disk_}, std::pair{"main_menu.html", &ue_tp_html_},
                                                 std::pair{"components.json", &ue_tp_lib_json_}, std::pair{"components.html", &ue_tp_lib_html_}})
                    write_file_atomic(ed_.ui_game_dir / "ui" / utf8_path(name), {reinterpret_cast<const u8*>(text->data()), text->size()});
                check(ue().open_library() && ue().open("main_menu") && ue_file(".json") == ue_tp_disk_ && ue().screens() == ue_tp_screens_ &&
                          d::components(ue().library()).empty() == (ue_tp_lib_json_.find("\"component\"") == std::string::npos),
                      "the screens, the menu and the components as before");
                break;
            }
            default: break;
            }
            ue_tp_stage_ = 0;
            break;
        }
        case 154: {
            // games/examples/templates (13.12), the files the game and its package read: a game made of templates,
            // with its own template «Находка» put twice. Opened from disk: the library lists the game's own template
            // (it needs nothing the game lacks) and puts a third copy as one step, taken back to the example's
            // files; «Проверить» on its screen over the game does what the game does: the pause button opens the
            // author's pause, its «Настройки» the author's settings, whose buttons change the volume, «Готово» and
            // «Продолжить» close them (with the pause's click sound), each «Взять» gives its own thing; nothing is
            // written by opening or checking. Then the screen saved as the game's own template needs nothing here,
            // but taken where its pause and a picture are not, the library and the note say so (the button stays).
            namespace d = editor::design;
            const std::filesystem::path from = utf8_path(FORGE_EXAMPLES_DIR) / "templates";
            static const char* const kPages[] = {"шаблоны_меню", "шаблоны_игра", "шаблоны_пауза", "шаблоны_настройки"};
            static const char* const kOwn[] = {"templates/templates.json", "templates/находка.json"};
            auto lf = [](std::string text) {
                std::erase(text, '\r');
                return text;
            };
            auto read = [](const std::filesystem::path& p) {
                std::vector<u8> bytes;
                read_file(p, bytes);
                return std::string(bytes.begin(), bytes.end());
            };
            // The game's files now, byte for byte: the pages and its own template.
            auto files = [&]() {
                std::vector<std::string> out;
                for (const char* name : kPages)
                    for (const char* ext : {".json", ".html"}) out.push_back(ue_file(ext, name));
                for (const char* own : kOwn) out.push_back(read(ed_.ui_game_dir / "ui" / utf8_path(own)));
                return out;
            };
            // The same content as the example's, whichever line ends either side has.
            auto same = [&]() {
                bool ok = true;
                for (const char* name : kPages)
                    for (const char* ext : {".json", ".html"})
                        ok = ok && lf(ue_file(ext, name)) == lf(read(from / "ui" / utf8_path(std::string(name) + ext)));
                for (const char* own : kOwn) ok = ok && lf(read(ed_.ui_game_dir / "ui" / utf8_path(own))) == lf(read(from / "ui" / utf8_path(own)));
                return ok;
            };
            auto said = [&]() { return ue().check_log().empty() ? std::string() : ue().check_log().back(); };
            auto at = [&](const std::optional<d::Rect>& b) {
                if (b) left_click(ue_wx(b->cx()), ue_wy(b->cy()));
                return b.has_value();
            };
            auto own_index = [&]() -> usize {
                const auto& all = ue().templates();
                for (usize i = 0; i < all.size(); ++i)
                    if (all[i].info.own && all[i].info.title == "Находка") return i;
                return all.size();
            };
            auto wait = [&](bool ready, const char* what) {
                if (!hold(ready, what)) return false;
                --ue_te_stage_;
                return true;
            };
            const std::vector<std::string> pause{"шаблоны_пауза"}, both{"шаблоны_пауза", "шаблоны_настройки"};
            switch (ue_te_stage_++) {
            case 0: {
                std::error_code ec;
                std::filesystem::create_directories(ed_.ui_game_dir / "ui" / "templates", ec);
                for (const char* name : kPages)
                    for (const char* ext : {".json", ".html"})
                        std::filesystem::copy_file(from / "ui" / utf8_path(std::string(name) + ext), ed_.ui_game_dir / "ui" / utf8_path(std::string(name) + ext),
                                                   std::filesystem::copy_options::overwrite_existing, ec);
                for (const char* own : kOwn)
                    std::filesystem::copy_file(from / "ui" / utf8_path(own), ed_.ui_game_dir / "ui" / utf8_path(own),
                                               std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::create_directories(ed_.ui_game_dir / "sounds", ec);
                ue_te_had_click_ = std::filesystem::exists(ed_.ui_game_dir / "sounds" / utf8_path("щелчок.wav"), ec);
                std::filesystem::copy_file(from / "sounds" / utf8_path("щелчок.wav"), ed_.ui_game_dir / "sounds" / utf8_path("щелчок.wav"),
                                           std::filesystem::copy_options::overwrite_existing, ec);
                ue_te_files_ = files();
                check(!ec && same(), "the example's pages, its own template and sound copied into the game");
                check(ue().open("шаблоны_игра") && ue().screen().show == d::ScreenShow::Playing, "«Шаблоны: игра» opens from disk");
                const d::Node* key = nullptr;
                const d::Node* torch = nullptr;
                for (const d::Node& c : ue().screen().root.children) {
                    if (c.name == "Карточка «Ключ»") key = &c;
                    if (c.name == "Карточка «Факел»") torch = &c;
                }
                check(key && torch && key->children.size() == torch->children.size() && key->id != torch->id,
                      "the own template's two copies on it, «Ключ» and «Факел»");
                ue_te_torch_ = torch ? torch->id : 0;
                check(files() == ue_te_files_, "opening wrote nothing");
                ue().set_simple(false);
                ue().select({});
                check(click("ue-templates") && ue().templates_open(), "the library");
                return true;
            }
            case 1:
                if (wait(shown("ue-tpl-search"), "the library is laid out")) return true;
                check(ue_fl_type_only("ue-tpl-search", "находка"), "searched «находка»");
                return true;
            case 2: {
                const usize own = own_index();
                check(own < ue().templates().size() && ue().templates_listed() == std::vector<usize>{own},
                      "the game's own template is found, read from its ui/templates");
                check(ue().template_needs(own).empty(), "it needs nothing the game lacks (the key and the torches are the game's things)");
                ue_te_steps_ = ue().history().cursor();
                const usize before = ue().screen().root.children.size();
                key(SDLK_RETURN, SDL_KMOD_NONE);
                check(ue().screen().root.children.size() == before + 1 && ue().history().cursor() == ue_te_steps_ + 1 &&
                          ue().history().undo_label() == "Из шаблонов: Находка",
                      "Enter: a third copy, one step");
                key(SDLK_Z, SDL_KMOD_CTRL);
                check(ue().screen().root.children.size() == before && same(), "Ctrl+Z: the example's files again (line ends aside)");
                check(click("ue-check") && ue().checking(), "«Проверить» on «Шаблоны: игра»");
                return true;
            }
            case 3:
                check(at(ue().layer_box(ue_named("Кнопка «Пауза»"))), "the pause button clicked");
                return true;
            case 4:
                if (wait(ue().check_windows() == pause, "the author's pause comes up")) return true;
                check(said() == "Открыто окно «Шаблоны: пауза» поверх экрана", "the author's pause over the game: " + said());
                check(at(ue().check_box("шаблоны_пауза", "Кнопка «Настройки»")), "its «Настройки» clicked");
                return true;
            case 5:
                if (wait(ue().check_windows() == both, "the settings come up over the pause")) return true;
                check(ue_sd_var("settings.master") == 100, "the volume as a new game has it: 100");
                check(at(ue().check_box("шаблоны_настройки", "Кнопка «−»")), "«−» clicked");
                return true;
            case 6:
                check(ue_sd_var("settings.master") == 90 && said() == "Данные: settings.master -= 10", "the volume 90: " + said());
                check(at(ue().check_box("шаблоны_настройки", "Кнопка «Готово»")), "«Готово» clicked");
                return true;
            case 7:
                check(ue().check_windows() == pause && said() == "Закрыто окно «Шаблоны: настройки»", "«Готово» closed the settings: " + said());
                // At once, while the settings still fade away over it: a closed window takes no clicks.
                ue_te_clicks_ = ue().check_sound().clicks();
                check(at(ue().check_box("шаблоны_пауза", "Кнопка «Продолжить»")), "«Продолжить» clicked");
                return true;
            case 8:
                check(ue().check_windows().empty() && said() == "Закрыто окно «Шаблоны: пауза»", "«Продолжить» closed the pause: " + said());
                check(ue().check_sound().clicks() == ue_te_clicks_ + 1 && ue().check_sound().last_click() == "щелчок.wav",
                      "with the pause's click sound: " + ue().check_sound().last_click());
                if (const d::Node* torch = ue_node(ue_te_torch_)) at(ue().layer_box(ue_named(*torch, "Кнопка «Взять»")));
                return true;
            case 9:
                check(said() == "Данные: inv.torch += 1", "the second copy's «Взять» gives a torch: " + said());
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().checking(), "Esc ends the check");
                check(files() == ue_te_files_, "«Проверить» wrote nothing: the files byte for byte as copied");
                check(ue().open("шаблоны_меню") && ue().open("шаблоны_игра") && files() == ue_te_files_, "opened again: nothing written");
                // The whole screen as the game's own template, then taken where its pause and a picture are not.
                ue().select({});
                check(ue().begin_save_template(0) && ue().saving_template(), "«Сохранить экран как шаблон…»");
                return true;
            case 10:
                if (wait(shown("ue-tpl-save-title"), "the form is laid out")) return true;
                check(ue_fl_type_only("ue-tpl-save-title", "Над игрой с находками"), "a title typed");
                return true;
            case 11: {
                const usize n = ue().templates().size();
                key(SDLK_RETURN, SDL_KMOD_NONE);
                const int i = ue().selected_template();
                check(ue().templates().size() == n + 1 && i >= 0 && ue().templates()[static_cast<usize>(i)].info.own &&
                          ue().templates()[static_cast<usize>(i)].info.kind == d::TemplateKind::Screen,
                      "Enter: the screen is the game's own template");
                check(i >= 0 && ue().template_needs(static_cast<usize>(i)).empty(),
                      "here it needs nothing: the pause it opens, its pictures and things are the game's");
                // Another game: no «Шаблоны: пауза», no golden button picture.
                const std::filesystem::path away = std::filesystem::temp_directory_path() / "forge_templates_away";
                std::error_code ec;
                std::filesystem::remove_all(away, ec);
                std::filesystem::create_directories(away, ec);
                for (const char* ext : {".json", ".html"})
                    std::filesystem::rename(ed_.ui_game_dir / "ui" / utf8_path(std::string("шаблоны_пауза") + ext),
                                            away / utf8_path(std::string("шаблоны_пауза") + ext), ec);
                std::filesystem::rename(ed_.ui_game_dir / "pictures" / utf8_path("интерфейс/кнопка_золото.png"), away / "button.png", ec);
                check(!ec, "the pause and the picture taken away");
                key(SDLK_ESCAPE, SDL_KMOD_NONE);
                check(!ue().templates_open() && click("ue-templates") && ue().templates_open(), "the library opened again");
                return true;
            }
            case 12: {
                if (wait(shown("ue-tpl-search"), "the library is laid out again")) return true;
                int i = -1;
                for (usize k = 0; k < ue().templates().size(); ++k)
                    if (ue().templates()[k].info.own && ue().templates()[k].info.title == "Над игрой с находками") i = static_cast<int>(k);
                check(i >= 0 && ue().select_template(static_cast<usize>(i)), "the own screen template picked");
                const std::vector<std::string> needs = i >= 0 ? ue().template_needs(static_cast<usize>(i)) : std::vector<std::string>{};
                auto said_of = [&](const char* what) {
                    return std::any_of(needs.begin(), needs.end(), [&](const std::string& l) { return l.find(what) != std::string::npos; });
                };
                std::string all;
                for (const std::string& l : needs) all += " " + l;
                check(said_of("Нет картинок «pictures/интерфейс/кнопка_золото.png»") && said_of("Кнопки открывают экраны «шаблоны_пауза»"),
                      "the window says what it lacks now, the picture and the pause:" + all);
                check(needs.size() == 2, "and nothing else");
                check(click("ue-tpl-new-screen"), "«Новый экран» from it all the same");
                return true;
            }
            case 13: {
                ue_te_made_ = ue().opened();
                const d::Node* button = ue_node(ue_named("Кнопка «Пауза»"));
                check(ue_te_made_ != "шаблоны_игра" && ue().screen().title == "Над игрой с находками" && button &&
                          button->on_click == std::vector<d::Action>{{d::ActionKind::Show, "шаблоны_пауза"}},
                      "a new screen, its pause button as it was: " + ue_te_made_);
                check(ue().move_note().find("кнопка_золото.png") != std::string::npos && ue().move_note().find("«шаблоны_пауза»") != std::string::npos,
                      "the note over the list says the same: " + ue().move_note());
                // Back as it was: the pause and the picture, the own templates as in the example.
                const std::filesystem::path away = std::filesystem::temp_directory_path() / "forge_templates_away";
                std::error_code ec;
                for (const char* ext : {".json", ".html"})
                    std::filesystem::rename(away / utf8_path(std::string("шаблоны_пауза") + ext),
                                            ed_.ui_game_dir / "ui" / utf8_path(std::string("шаблоны_пауза") + ext), ec);
                std::filesystem::rename(away / "button.png", ed_.ui_game_dir / "pictures" / utf8_path("интерфейс/кнопка_золото.png"), ec);
                std::filesystem::remove_all(away, ec);
                for (const char* ext : {".json", ".html"}) std::filesystem::remove(ed_.ui_game_dir / "ui" / utf8_path(ue_te_made_ + ext), ec);
                int i = -1;
                for (usize k = 0; k < ue().templates().size(); ++k)
                    if (ue().templates()[k].info.own && ue().templates()[k].info.title == "Над игрой с находками") i = static_cast<int>(k);
                check(i >= 0 && ue().remove_template(static_cast<usize>(i)), "the own screen template taken out");
                check(ue().open("шаблоны_игра") && files() == ue_te_files_, "the example's files byte for byte again");
                {
                    for (const char* name : kPages)
                        for (const char* ext : {".json", ".html"}) std::filesystem::remove(ed_.ui_game_dir / "ui" / utf8_path(std::string(name) + ext), ec);
                    std::filesystem::remove_all(ed_.ui_game_dir / "ui" / "templates", ec);
                    if (!ue_te_had_click_) std::filesystem::remove(ed_.ui_game_dir / "sounds" / utf8_path("щелчок.wav"), ec);
                }
                check(ue().open("main_menu"), "back to the menu");
                break;
            }
            default: break;
            }
            ue_te_stage_ = 0;
            break;
        }
        default:
            ue_step_ = -1;
            return true;
        }
        ++ue_step_;
        return true;
    }

    std::string shared_count_; // the shared coins' count, for the «Общие» checks
    std::filesystem::path sound_dir_;
    int sound_row_ = -1; // the coins' «Подбирают» row

    bool asset_step() {
        const bool idle = !as().busy();
        switch (as_step_) {
        case 0:
            check(click_tab(10), "a click on the Resources tab");
            break;
        case 1:
            if (hold(idle && as().record_count() >= 7, "the resources are indexed")) return true;
            check(shown("as-list") && shown("as-pane-folders") && shown("as-pane-preview"), "the tab shows the list and panels");
            check(as().record_count() == 7, "7 files found");
            check(as().row_count() == 5 && as().row_rel(0) == "данные" && as().row_rel(4) == "readme.txt",
                  "the top folder lists 4 folders, then the file");
            check(shown("folder-4"), "the folder tree has the folders");
            click_row("тайлы", "dblclick");
            check(as().folder() == "тайлы" && as().row_count() == 2, "a double click opens a folder");
            click_row("тайлы/камень.png");
            break;
        case 2:
            if (hold(as().preview_width() == 32 && as().row_has_thumb(static_cast<usize>(std::max<i64>(0, row_of("тайлы/камень.png")))),
                     "the picture's preview and thumbnail are made"))
                return true;
            check(as().preview_height() == 32 && shown("as-preview"), "the preview shows the 32 × 32 picture");
            break;
        case 3:
            if (!typed_) {
                // Typed, as from the keyboard.
                if (Rml::Element* e = ed_.find_element("as-search")) e->Focus();
                SDL_Event t{};
                t.type = SDL_EVENT_TEXT_INPUT;
                t.text.text = "шах";
                ed_.handle_event(t);
                typed_ = true;
                return true;
            }
            if (hold(as().row_count() == 1, "the search answers")) return true;
            check(as().row_count() == 1 && as().row_rel(0) == "персонажи/шахтёр.png", "typing «шах» finds the miner everywhere");
            if (Rml::Element* e = ed_.find_element("as-search")) e->Blur();
            click_row("персонажи/шахтёр.png");
            check(as().edit_image("cw") && as().history().undo_label() == "Повернуть вправо: шахтёр.png", "the picture turns");
            break;
        case 4:
            if (hold(idle && as().preview_width() == 24, "the turned picture is shown")) return true;
            check(as().preview_height() == 16, "turned: 24 × 16");
            key(SDLK_Z, SDL_KMOD_CTRL);
            break;
        case 5:
            if (hold(idle && as().preview_width() == 16, "the turn is taken back")) return true;
            check(as().preview_height() == 24, "Ctrl+Z gives the picture back: 16 × 24");
            as().set_search("");
            as().open_folder("персонажи");
            click_row("персонажи/шахтёр.png");
            check(as().rename_selected("Борис"), "rename");
            check(exists("персонажи/Борис.png") && !exists("персонажи/шахтёр.png"), "the file is renamed, the extension kept");
            break;
        case 6:
            if (hold(idle, "the rename is indexed")) return true;
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(exists("персонажи/шахтёр.png") && !exists("персонажи/Борис.png"), "Ctrl+Z gives the old name back");
            key(SDLK_Y, SDL_KMOD_CTRL);
            check(exists("персонажи/Борис.png"), "Ctrl+Y renames again");
            break;
        case 7:
            if (hold(idle && row_of("персонажи/Борис.png") >= 0, "the list shows the new name")) return true;
            click_row("персонажи/Борис.png");
            key(SDLK_DELETE, SDL_KMOD_NONE);
            check(!exists("персонажи/Борис.png") && std::filesystem::exists(as().trash()), "Delete moves the file to the trash");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(exists("персонажи/Борис.png"), "Ctrl+Z brings it back");
            if (Rml::Element* e = ed_.find_element("as-new-folder")) e->Click();
            check(std::filesystem::is_directory(as().abs("персонажи/Новая папка")), "a new folder");
            check(as().selection().size() == 1 && as().rename_selected("враги"), "the new folder renamed");
            check(std::filesystem::is_directory(as().abs("персонажи/враги")), "the folder is called «враги»");
            if (Rml::Element* e = ed_.find_element("as-name")) e->Blur();
            break;
        case 8:
            if (hold(idle && row_of("персонажи/враги") >= 0, "the list shows the folder")) return true;
            click_row("персонажи/кузнец.png");
            key(SDLK_X, SDL_KMOD_CTRL);
            click_row("персонажи/враги", "dblclick");
            check(as().folder() == "персонажи/враги", "into the new folder");
            key(SDLK_V, SDL_KMOD_CTRL);
            check(exists("персонажи/враги/кузнец.png") && !exists("персонажи/кузнец.png"), "cut and paste move the file");
            {
                // A file from outside the project, dropped on the window.
                const auto outside = std::filesystem::temp_directory_path() / "forge_editor_drop";
                std::error_code ec;
                std::filesystem::remove_all(outside, ec);
                std::filesystem::create_directories(outside, ec);
                std::vector<u8> png;
                assets::CookedTexture t;
                t.width = t.height = 4;
                t.rgba8.assign(64, 200);
                assets::encode_image(t, ".png", png);
                write_file_atomic(outside / "жук.png", png);
                drop(outside / "жук.png");
            }
            break;
        case 9:
            if (hold(idle && exists("персонажи/враги/жук.png") && !as().selection().empty(), "the dropped file is imported"))
                return true;
            check(as().history().undo_label() == "Импорт: жук.png" && as().selection()[0] == "персонажи/враги/жук.png",
                  "the drop is one history entry and the new file is selected");
            break;
        case 10:
            if (hold(idle && row_of("персонажи/враги/жук.png") >= 0, "the imported file is listed")) return true;
            check(as().set_tags("враг летает"), "tags are set");
            break;
        case 11:
            if (hold(idle, "the tags are indexed")) return true;
            as().set_search("летает");
            check(as().row_count() == 1 && as().row_rel(0) == "персонажи/враги/жук.png", "search finds the file by its tag");
            as().set_search("");
            as().open_folder("тайлы");
            click_row("тайлы/песок.bmp");
            check(as().convert_image(".png") && exists("тайлы/песок.png"), "a PNG copy of the BMP");
            break;
        case 12:
            if (hold(idle && as().row_count() == 3, "the copy is listed")) return true;
            check(as().row_field(0, "kind") == "Картинка PNG" || as().row_field(0, "kind") == "Картинка BMP",
                  "the Type column names the kind and the format");
            check(as().row_field(0, "tint") == "ft-image" && !as().row_field(0, "date").empty(), "an image colour and a date");
            as().sort_by("size");
            check(as().row_rel(0) == "тайлы/песок.png" && as().row_rel(2) == "тайлы/песок.bmp", "sorted by size, smallest first");
            as().sort_by("size");
            check(as().row_rel(0) == "тайлы/песок.bmp", "the same column again: largest first");
            as().sort_by("name");
            if (Rml::Element* e = ed_.find_element("nav-audio")) e->Click();
            check(as().filter() == "audio" && as().row_count() == 1 && as().row_rel(0) == "звуки/кирка.wav",
                  "«Все звуки» lists the project's sounds from every folder");
            check(as().row_field(0, "kind") == "Звук WAV" && as().row_field(0, "where") == "звуки", "with the folder column");
            key(SDLK_LEFT, SDL_KMOD_ALT);
            check(as().filter().empty() && as().folder() == "тайлы" && as().row_count() == 3, "Alt+← goes back to the folder");
            key(SDLK_RIGHT, SDL_KMOD_ALT);
            check(as().filter() == "audio", "Alt+→ goes forward again");
            key(SDLK_BACKSPACE, SDL_KMOD_NONE);
            check(as().filter().empty() && as().folder() == "тайлы", "Backspace leaves the collection");
            key(SDLK_BACKSPACE, SDL_KMOD_NONE);
            check(as().folder().empty(), "Backspace goes up a folder");
            as().open_folder("тайлы");
            click_row("тайлы/камень.png");
            key(SDLK_C, SDL_KMOD_CTRL);
            as().open_folder("данные");
            key(SDLK_V, SDL_KMOD_CTRL);
            break;
        case 13:
            if (hold(idle && exists("данные/камень.png"), "the copy is made")) return true;
            check(exists("тайлы/камень.png") && as().history().undo_label() == "Копия: камень.png", "Ctrl+C, Ctrl+V copies the file");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(!exists("данные/камень.png") && exists("тайлы/камень.png"), "Ctrl+Z takes the copy away");
            as().open_folder("тайлы"); // for a screenshot: a picture in the preview
            click_row("тайлы/камень.png");
            break;
        case 14:
            if (hold(as().preview_width() == 32 && idle, "the stone is shown again")) return true;
            if (Rml::Element* e = ed_.find_element("as-sort-menu")) e->Click();
            break;
        case 15:
            check(shown("sort-date"), "«Сортировка» opens its menu");
            if (Rml::Element* e = ed_.find_element("sort-date")) e->Click();
            check(as().sort_key() == "date", "a menu item sorts");
            break;
        case 16:
            check(!shown("sort-date"), "the menu closes after a choice");
            as().sort_by("name");
            as().open_folder("");
            as().set_filter("image");
            if (Rml::Element* e = ed_.find_element("as-view-icons")) e->Click();
            break;
        case 17: {
            if (hold(idle && as().view() == "icons" && as().row_has_thumb(0) && as().row_has_thumb(1), "big thumbnails are made"))
                return true;
            std::vector<Rml::Element*> cells = list_cells();
            bool side_by_side = false;
            for (Rml::Element* a : cells)
                for (Rml::Element* b : cells)
                    if (a != b && a->GetAbsoluteOffset().y == b->GetAbsoluteOffset().y && a->GetAbsoluteOffset().x < b->GetAbsoluteOffset().x)
                        side_by_side = true;
            check(as().row_field(0, "thumb").rfind("/memory/tb_", 0) == 0, "«Значки» uses big thumbnails");
            check(side_by_side, "«Значки» puts pictures side by side in a grid");
            // A right click on a picture, as from the mouse.
            Rml::Element* cell = cells.empty() ? nullptr : cells.front();
            if (cell) {
                const Rml::Vector2f at = cell->GetAbsoluteOffset(Rml::BoxArea::Border);
                right_click(at.x + cell->GetOffsetWidth() / 2, at.y + 40);
            }
            break;
        }
        case 18:
            check(as().selection().size() == 1 && shown("ctx-delete") && shown("ctx-copy") && shown("ctx-rotate"),
                  "a right click on a picture chooses it and opens its menu");
            if (Rml::Element* e = ed_.find_element("ctx-copy")) e->Click();
            check(as().status().find("скопировано 1") != std::string::npos, "«Копировать» from the menu");
            break;
        case 19: {
            check(!shown("ctx-copy"), "the menu closes after a choice");
            // A right click on the empty space under the pictures.
            if (Rml::Element* list = ed_.find_element("as-list")) {
                const Rml::Vector2f at = list->GetAbsoluteOffset(Rml::BoxArea::Border);
                right_click(at.x + list->GetOffsetWidth() - 30, at.y + list->GetOffsetHeight() - 30);
            }
            break;
        }
        case 20:
            check(as().selection().empty() && shown("ctx-paste") && shown("ctx-new-folder"),
                  "a right click on empty space opens the folder's menu");
            key(SDLK_ESCAPE, SDL_KMOD_NONE);
            break;
        case 21:
            check(!shown("ctx-paste"), "Esc closes the menu");
            key(SDLK_ESCAPE, SDL_KMOD_NONE); // forget the copied file
            as().set_filter("");
            as().open_folder("персонажи/враги");
            {
                // A WebP picture (stb cannot read WebP; libwebp does).
                const auto outside = std::filesystem::temp_directory_path() / "forge_editor_drop";
                std::vector<u8> webp;
                assets::CookedTexture t;
                t.width = 6;
                t.height = 4;
                t.rgba8.assign(6 * 4 * 4, 120);
                assets::encode_image(t, ".webp", webp);
                write_file_atomic(outside / "слизень.webp", webp);
                drop(outside / "слизень.webp");
            }
            break;
        case 22: {
            const i64 r = row_of("персонажи/враги/слизень.webp");
            if (hold(idle && r >= 0 && as().row_has_thumb(static_cast<usize>(r)), "the WebP picture gets a thumbnail")) return true;
            check(as().row_field(static_cast<usize>(r), "kind") == "Картинка WEBP", "a WebP file is a picture");
            // «Конвертировать…» on a sound (needs Python with the converters' libraries).
            if (as().converters().python().empty()) {
                FORGE_INFO("self-test: Python не найден, конвертеры не проверяются");
                as_step_ = 25;
                as().open_folder("тайлы");
                click_row("тайлы/камень.png");
                return true;
            }
            as().set_filter("");
            as().open_folder("звуки");
            click_row("звуки/кирка.wav");
            check(as().converters().find("audio") && as().converters().find("psd_layers") && as().converters().find("aseprite_sheet"),
                  "the converters are found");
            as().open_convert();
            check(as().convert_open() && as().convert_picked() == "audio" && as().convert_choices() == 1,
                  "«Конвертировать…» offers the sound converter for a WAV");
            check(as().set_convert_setting("format", "\"ogg\"") && as().set_convert_setting("channels", "\"mono\""),
                  "settings are chosen");
            check(!as().set_convert_setting("format", "\"mp4\""), "a wrong choice is refused");
            break;
        }
        case 23:
            check(shown("as-conv-run"), "the window is shown");
            check(as().run_convert() && !as().convert_open() && as().converting(), "the conversion starts and the window closes");
            break;
        case 24: {
            // Python starts slowly on a cold machine: a longer wait than hold().
            if (as().converting() && ++conv_wait_ < 20000) {
                SDL_Delay(2);
                return true;
            }
            if (hold(idle && row_of("звуки/кирка.ogg") >= 0, "the OGG copy is listed")) return true;
            check(exists("звуки/кирка.ogg") && exists("звуки/кирка.wav"), "the OGG copy is next to the WAV");
            check(as().history().undo_label() == "Звук → OGG, WAV или FLAC: кирка.ogg", "the conversion is one history entry");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(!exists("звуки/кирка.ogg"), "Ctrl+Z takes the OGG away");
            as().open_folder("тайлы"); // for a screenshot: the grid with a menu
            click_row("тайлы/камень.png");
            break;
        }
        case 25: {
            if (hold(idle && as().row_has_thumb(0), "the stones are shown")) return true;
            std::vector<Rml::Element*> cells = list_cells();
            for (Rml::Element* c : cells)
                if (c->GetAbsoluteOffset().x < 99999 && c->IsClassSet("vl-row") && c->GetChild(0) && c->GetChild(0)->IsClassSet("selected")) {
                    const Rml::Vector2f at = c->GetAbsoluteOffset(Rml::BoxArea::Border);
                    right_click(at.x + c->GetOffsetWidth() / 2, at.y + 40);
                }
            return false;
        }
        default: break;
        }
        ++as_step_;
        return true;
    }
    flecs::entity placed() { return lv().level().find(object_); }
    bool own(const char* prop) {
        const flecs::entity e = placed();
        return e.is_valid() && e.has<objects::ObjectRef>() && e.get<objects::ObjectRef>().overrides_prop(prop);
    }
    u32 count() {
        const flecs::entity e = placed();
        return e.is_valid() ? e.get<slice::Item>().count : 0;
    }
    void to_point(f64 x, f64 y) {
        lv().screen_of(x, y, x_, y_);
        mouse(SDL_EVENT_MOUSE_MOTION, x_, y_);
    }

    bool shown(const std::string& id) { return shown(id.c_str()); }
    bool element_center(const std::string& id, f32& x, f32& y) { return element_center(id.c_str(), x, y); }

    bool shown(const char* id) {
        Rml::Element* e = ed_.find_element(id);
        if (e && e->IsVisible(true)) return true;
        // Several elements may share an id while only one is shown.
        Rml::ElementList all;
        if (e && e->GetOwnerDocument()) e->GetOwnerDocument()->QuerySelectorAll(all, std::string("#") + id);
        for (Rml::Element* x : all)
            if (x->IsVisible(true)) return true;
        return false;
    }
    bool tab_lit(int index) {
        Rml::Element* bar = ed_.find_element("editor-tabs");
        return bar && index < bar->GetNumChildren() && bar->GetChild(index)->IsClassSet("selected");
    }
    bool click_tab(int index) {
        Rml::Element* bar = ed_.find_element("editor-tabs");
        if (!bar || index >= bar->GetNumChildren()) return false;
        bar->GetChild(index)->Click();
        return true;
    }
    // What «Проверить» wrote since a count of lines, joined (for a failure's message).
    std::string ue_log_since(usize from) {
        const std::vector<std::string>& log = ue().check_log();
        std::string out = std::to_string(log.size() - std::min(from, log.size())) + " строк:";
        for (usize i = std::min(from, log.size()); i < log.size(); ++i) out += " [" + log[i] + "]";
        return out;
    }
    bool check(bool ok, const std::string& what) { return check(ok, what.c_str()); }
    bool check(bool ok, const char* what) {
        if (ok) {
            FORGE_INFO("self-test: %s", what);
        } else {
            FORGE_ERROR("self-test FAILED: %s", what);
            ++failures_;
        }
        return ok;
    }
    void mouse(SDL_EventType type, f32 x, f32 y) {
        SDL_Event e{};
        e.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            e.motion.x = x;
            e.motion.y = y;
        } else {
            e.button.x = x;
            e.button.y = y;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        }
        ed_.handle_event(e);
    }
    void key(SDL_Keycode k, SDL_Keymod mod) {
        SDL_Event e{};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = k;
        e.key.mod = mod;
        e.key.down = true;
        ed_.handle_event(e);
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        ed_.handle_event(e);
    }

    Editor& ed_;
    ObjectId target_ = kNoObject;
    Vec2 start_{}, moved_{};
    f32 x_ = 0, y_ = 0;
    usize entries_ = 0, count_ = 0, stacks_ = 0;
    i32 cx_ = 0, cy_ = 0;
    u64 object_ = 0;
    u32 as_step_ = 0, ol_step_ = 0, waited_ = 0, conv_wait_ = 0;
    int lg_step_ = 0;
    int st_step_ = 0, st_wait_ = 0;
    int ue_step_ = 0;
    u32 ue_rect_ = 0;
    std::string ue_html_;
    std::string st_new_; // a line the test added
    std::filesystem::path picture_file_;
    u64 new_template_ = 0;
    std::string drop_text_;
    bool typed_ = false;
    int failures_ = 0;
    u32 drag_node_ = 0;
    f32 drag_from_x_ = 0, drag_from_y_ = 0;
};

// The project folder: the first start copies the game, later ones keep the
// author's files and refresh only the engine's.
int check_project_folder() {
    namespace fs = std::filesystem;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) ++failures;
        FORGE_INFO("%s project: %s", ok ? "ok  " : "FAIL", what);
    };
    std::error_code ec;
    const fs::path project = fs::temp_directory_path() / utf8_path("forge_editor_проект");
    fs::remove_all(project, ec);
    const fs::path tmpl = utf8_path(SLICE_DATA_DIR);
    ProjectGame pg;
    std::string error;
    check(prepare_project_game(project, tmpl, pg, &error) && pg.created && pg.dir == project / "game",
          "the first start copies the game into game/");
    check(fs::exists(pg.dir / "logic.json") && fs::exists(pg.dir / "dialogues") && fs::exists(pg.dir / "objects") &&
              !fs::exists(project / "game.copying"),
          "with its links, conversations and objects");
    const std::string mine = "{\"links\": []}";
    write_file_atomic(pg.dir / "logic.json", {reinterpret_cast<const u8*>(mine.data()), mine.size()});
    write_file_atomic(pg.dir / "verbs.json", {reinterpret_cast<const u8*>(mine.data()), mine.size()});
    fs::remove(pg.dir / "quests.json", ec);
    fs::remove_all(pg.dir / "dialogues", ec);
    check(prepare_project_game(project, tmpl, pg, &error) && !pg.created, "the next start keeps the project");
    std::vector<u8> bytes;
    check(read_file(pg.dir / "logic.json", bytes) && std::string(bytes.begin(), bytes.end()) == mine,
          "the author's links stay as they are");
    std::vector<u8> engine;
    check(read_file(pg.dir / "verbs.json", bytes) && read_file(tmpl / "verbs.json", engine) && bytes == engine,
          "the engine's verbs are refreshed");
    check(fs::exists(pg.dir / "quests.json") && !fs::exists(pg.dir / "dialogues"),
          "a missing data file comes back, a removed folder does not");
    check(prepare_project_game(project, tmpl, pg, &error) && pg.refreshed.empty(), "nothing to refresh the third time");
    fs::remove_all(project, ec);
    return failures;
}

int run_offscreen(const Options& options, const char* screenshot, u32 frames, bool select, bool play, bool bench,
                  bool self_test, int tab, bool bench_level, u32 bench_assets, u32 bench_scheme, bool bench_story) {
    jobs::init();
    SDL_GPUDevice* device = render::create_offscreen_device();
    if (!device) {
        jobs::shutdown();
        return 1;
    }
    const u32 w = 1600, h = 900;
    SDL_GPUTexture* target = render::create_render_target(device, w, h);
    int result = 1;
    {
        Editor editor;
        // Never touch a scene file from an offscreen run.
        editor.scene_path = options.scene.empty() ? std::filesystem::path("__offscreen_no_scene__.json") : options.scene;
        // Nor the game's templates: a copy of them.
        editor.objects_folder = std::filesystem::temp_directory_path() / "forge_editor_objects";
        {
            std::error_code ec;
            std::filesystem::remove_all(editor.objects_folder, ec);
            std::filesystem::copy(editor.game_dir / "objects", editor.objects_folder, std::filesystem::copy_options::recursive, ec);
            editor.pictures_folder = std::filesystem::temp_directory_path() / "forge_editor_pictures";
            std::filesystem::remove_all(editor.pictures_folder, ec);
            if (std::filesystem::exists(editor.game_dir / "pictures", ec))
                std::filesystem::copy(editor.game_dir / "pictures", editor.pictures_folder, std::filesystem::copy_options::recursive, ec);
            editor.sounds_folder = std::filesystem::temp_directory_path() / "forge_editor_sounds";
            editor.objects_tab.silent = true;
            editor.ui_tab.silent = true;
            std::filesystem::remove_all(editor.sounds_folder, ec);
            // Nor this computer's shared objects: an empty library of its own.
            editor.shared_folder = std::filesystem::temp_directory_path() / "forge_editor_shared";
            std::filesystem::remove_all(editor.shared_folder, ec);
            // Nor the game's links: a copy of them.
            editor.logic_file = std::filesystem::temp_directory_path() / "forge_editor_logic.json";
            std::filesystem::remove(editor.logic_file, ec);
            std::filesystem::copy_file(editor.game_dir / "logic.json", editor.logic_file, ec);
            // Nor its conversations.
            editor.story_dir = std::filesystem::temp_directory_path() / "forge_editor_story";
            std::filesystem::remove_all(editor.story_dir, ec);
            std::filesystem::create_directories(editor.story_dir, ec);
            std::filesystem::copy(editor.game_dir / "dialogues", editor.story_dir / "dialogues", std::filesystem::copy_options::recursive, ec);
            std::filesystem::copy_file(editor.game_dir / "quests.json", editor.story_dir / "quests.json", ec);
            // Nor its screens.
            editor.ui_game_dir = std::filesystem::temp_directory_path() / "forge_editor_ui";
            std::filesystem::remove_all(editor.ui_game_dir, ec);
            std::filesystem::create_directories(editor.ui_game_dir, ec);
            if (std::filesystem::exists(editor.game_dir / "ui", ec))
                std::filesystem::copy(editor.game_dir / "ui", editor.ui_game_dir / "ui", std::filesystem::copy_options::recursive, ec);
        }
        // Nor the game's level, unless one is given.
        LevelConfig lc;
        lc.offscreen = true;
        lc.game_data = editor.game_dir;
        lc.folder = options.level_given ? options.level : std::filesystem::temp_directory_path() / "forge_editor_level";
        if (!options.level_given) {
            std::error_code ec;
            std::filesystem::remove_all(lc.folder, ec);
        }
        // Resources: a fresh sample folder, unless one is given.
        AssetsConfig ac;
        ac.offscreen = true;
        const std::filesystem::path sample = std::filesystem::temp_directory_path() / "forge_editor_assets";
        ac.folder = options.assets.empty() ? sample / "assets" : options.assets;
        ac.library = options.assets.empty() ? sample / "library" : ac.folder.parent_path() / ".forge" / "library";
        if (options.assets.empty()) make_sample_assets(sample, bench_assets);
        if (target && editor.init(device, nullptr, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h, options.ui_dir,
                                  options.theme, options.objects, lc, ac)) {
            f64 update_ms = 0, render_ms = 0, worst_ms = 0;
            u32 measured = 0;
            if (bench) {
                editor.open_tab("world");
                editor.hierarchy.expand_all(true);
            }
            SelfTest test(editor);
            if (self_test) {
                frames = 100'000; // until the steps end (some wait for background work)
                editor.open_tab("world"); // the scene part first, then the level, then the resources
            }
            bool testing = self_test;
            if (bench_level) {
                editor.open_tab("level");
                frames = std::max(frames, 600u);
            }
            if (bench_assets) {
                // The first look at a big project, then a look that finds nothing new.
                editor.open_tab("assets");
                for (int pass = 0; pass < 2; ++pass) {
                    const Stopwatch scan;
                    if (pass == 1) editor.assets.refresh();
                    do {
                        editor.update(1.0 / 60.0);
                        SDL_Delay(2);
                    } while (editor.assets.busy() && scan.elapsed_ms() < 30 * 60 * 1000.0);
                    FORGE_INFO("assets: %s look at %s files took %.0f ms", pass == 0 ? "first" : "second",
                               group_digits(editor.assets.record_count()).c_str(), scan.elapsed_ms());
                }
                editor.assets.open_folder("bench/10"); // 1 000 pictures: a thumbnail on every row
                frames = std::max(frames, 600u);
            }
            // A thing scheme of bench_scheme nodes: «Каждый шаг», then a long
            // chain of «Сдвинуть», each with «Этот объект» wired in. The view
            // is panned, then a node dragged, with the mouse.
            u32 sc_frame = 0, sc_node = 0;
            f32 sc_x = 0, sc_y = 0, sc_node_x = 0, sc_node_y = 0, sc_press_x = 0, sc_press_y = 0;
            if (bench_scheme) {
                editor.open_tab("logic");
                SchemeView& sc = editor.logic_tab.scheme();
                editor.logic_tab.set_mode("scheme");
                sc_frame = editor.logic_tab.thing_scheme("door");
                script::Graph g;
                g.name = "bench";
                u32 prev = g.add("std.event.tick", 0, 0).uid;
                const char* prev_pin = script::kFlowNext;
                for (u32 i = 1; i + 1 < bench_scheme; i += 2) {
                    const u32 move = g.add("api.entity.move", 0, 0).uid;
                    const u32 self = g.add("std.self", 0, 0).uid;
                    g.link(prev, prev_pin, move, script::kFlowIn);
                    g.link(self, "actor", move, "actor");
                    prev = move;
                }
                const Stopwatch put;
                sc.set_graph(sc_frame, g, "bench", true);
                const f64 put_ms = put.elapsed_ms();
                const Stopwatch order;
                sc.arrange(sc_frame);
                const f64 order_ms = order.elapsed_ms();
                sc.focus(sc_frame);
                FORGE_INFO("scheme: %s nodes, %s wires; put in and laid out %.1f ms, «Упорядочить» %.1f ms",
                           group_digits(static_cast<u64>(sc.graph(sc_frame)->nodes.size())).c_str(), group_digits(static_cast<u64>(sc.graph(sc_frame)->links.size())).c_str(),
                           put_ms, order_ms);
                frames = std::max(frames, 120u);
            }
            auto sc_mouse = [&](SDL_EventType type, f32 x, f32 y) {
                SDL_Event e{};
                e.type = type;
                if (type == SDL_EVENT_MOUSE_MOTION) {
                    e.motion.x = x;
                    e.motion.y = y;
                } else {
                    e.button.x = x;
                    e.button.y = y;
                    e.button.button = SDL_BUTTON_LEFT;
                    e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                }
                editor.handle_event(e);
            };
            // A talk of 600 lines like an imported novel (speakers, staging,
            // changes, a choice every 30 lines), scrolled by the wheel with the
            // mouse over the lines, as a person reads.
            f32 st_x = 0, st_y = 0;
            f64 wheel_ms = 0, wheel_worst = 0;
            u32 wheels = 0;
            if (bench_story) {
                game::DialogueSource big;
                big.id = "bench";
                big.speakers.push_back({"m", "Моника", "#7bd389", ""});
                big.speakers.push_back({"s", "Сайори", "#f2a0b8", ""});
                big.start.push_back({{}, "l0"});
                const int n = 600;
                for (int i = 0; i < n; ++i) {
                    game::SourceNode node;
                    node.id = "l" + std::to_string(i);
                    node.speaker = i % 3 == 0 ? "" : i % 3 == 1 ? "m" : "s";
                    node.text = "Реплика номер " + std::to_string(i) + ": сегодня в клубе снова читают стихи, и все ждут, кто начнёт первым.";
                    if (i % 50 == 0) node.scene = "Сцена " + std::to_string(i / 50 + 1);
                    if (i % 7 == 0) node.stage = {"scene bg club_day", "show monika 1a at t11", "play music t2"};
                    if (i % 11 == 0) node.act = "affection += 1";
                    if (i % 30 == 29 && i + 2 < n) {
                        node.choices.push_back({"Остаться", "", "stay = true", "l" + std::to_string(i + 1)});
                        node.choices.push_back({"Уйти домой", "affection > 2", "", "l" + std::to_string(i + 2)});
                    } else if (i + 1 < n) {
                        node.next = "l" + std::to_string(i + 1);
                    }
                    big.nodes.push_back(node);
                }
                const std::string json = big.json();
                write_file_atomic(editor.story_dir / "dialogues" / "bench.json", {reinterpret_cast<const u8*>(json.data()), json.size()});
                editor.open_tab("story");
                editor.story_tab.open("bench");
                const Stopwatch first;
                editor.update(1.0 / 60.0);
                FORGE_INFO("story: the talk opened, laid out %.1f ms", first.elapsed_ms());
                frames = std::max(frames, 160u);
            }
            f64 pan_ms = 0, drag_ms = 0, pan_worst = 0, drag_worst = 0;
            u32 pans = 0, drags = 0;
            for (u32 f = 0; f < frames; ++f) {
                if (testing) testing = test.step(f);
                else if (self_test) break;
                // Pan: press on the empty pane under the frame, move; then drag
                // a «Сдвинуть» in view about.
                if (bench_scheme && f == 10) {
                    if (Rml::Element* pane = editor.find_element("lg-scheme")) {
                        const Rml::Vector2f at = pane->GetAbsoluteOffset(Rml::BoxArea::Border);
                        sc_x = at.x + pane->GetClientWidth() - 40;
                        sc_y = at.y + pane->GetClientHeight() - 20;
                    }
                    sc_mouse(SDL_EVENT_MOUSE_MOTION, sc_x, sc_y);
                    sc_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, sc_x, sc_y);
                }
                if (bench_scheme && f > 10 && f < 60) {
                    sc_x += f < 35 ? -12.0f : 12.0f;
                    sc_y += f < 35 ? -4.0f : 4.0f;
                    const Stopwatch t;
                    sc_mouse(SDL_EVENT_MOUSE_MOTION, sc_x, sc_y);
                    const f64 ms = t.elapsed_ms();
                    pan_ms += ms;
                    pan_worst = std::max(pan_worst, ms);
                    ++pans;
                }
                if (bench_scheme && f == 60) {
                    sc_mouse(SDL_EVENT_MOUSE_BUTTON_UP, sc_x, sc_y);
                    // A «Сдвинуть» in view.
                    for (const SchemeView::NodeView& n : editor.logic_tab.scheme().node_views())
                        if (static_cast<u32>(n.link) == sc_frame && !n.hidden && n.title == "Сдвинуть" &&
                            n.x + editor.logic_tab.scheme().pan_x() > 300 && n.y + editor.logic_tab.scheme().pan_y() > 100) {
                            sc_node = static_cast<u32>(n.uid);
                            break;
                        }
                }
                if (bench_scheme && f == 62) {
                    // Its place on screen (the world sits at the pan already).
                    Rml::Element* node = editor.find_element(("sc-node-" + std::to_string(sc_frame) + "-" + std::to_string(sc_node)).c_str());
                    if (const script::GraphNode* n = editor.logic_tab.scheme().graph(sc_frame)->find(sc_node)) {
                        sc_node_x = n->x;
                        sc_node_y = n->y;
                    }
                    if (node) {
                        const Rml::Vector2f at = node->GetAbsoluteOffset(Rml::BoxArea::Border);
                        sc_x = at.x + 40;
                        sc_y = at.y + 12;
                        sc_press_x = sc_x;
                        sc_press_y = sc_y;
                        sc_mouse(SDL_EVENT_MOUSE_MOTION, sc_x, sc_y);
                        sc_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, sc_x, sc_y);
                    } else {
                        FORGE_WARN("scheme: the node to drag is not on screen");
                    }
                }
                if (bench_scheme && f > 62 && f < 110) {
                    sc_x += f < 90 ? 8.0f : -8.0f;
                    sc_y += f < 90 ? 5.0f : -5.0f;
                    const Stopwatch t;
                    sc_mouse(SDL_EVENT_MOUSE_MOTION, sc_x, sc_y);
                    const f64 ms = t.elapsed_ms();
                    drag_ms += ms;
                    drag_worst = std::max(drag_worst, ms);
                    ++drags;
                }
                if (bench_scheme && f == 110) {
                    const Stopwatch t;
                    sc_mouse(SDL_EVENT_MOUSE_BUTTON_UP, sc_x, sc_y);
                    const f64 ms = t.elapsed_ms();
                    const script::GraphNode* now = editor.logic_tab.scheme().graph(sc_frame)->find(sc_node);
                    FORGE_INFO("scheme: letting the node go (one step of the history) %.1f ms; it went %.0f, %.0f px (the mouse %.0f, %.0f)",
                               ms, now ? now->x - sc_node_x : 0.0f, now ? now->y - sc_node_y : 0.0f, sc_x - sc_press_x, sc_y - sc_press_y);
                }
                if (bench_story && f == 5)
                    if (Rml::Element* list = editor.find_element("st-script")) {
                        const Rml::Vector2f at = list->GetAbsoluteOffset(Rml::BoxArea::Border);
                        st_x = at.x + list->GetClientWidth() * 0.5f;
                        st_y = at.y + list->GetClientHeight() * 0.5f;
                        sc_mouse(SDL_EVENT_MOUSE_MOTION, st_x, st_y);
                    }
                if (bench_story && f > 5) {
                    SDL_Event e{};
                    e.type = SDL_EVENT_MOUSE_WHEEL;
                    e.wheel.y = (f / 80) % 2 == 0 ? -1.0f : 1.0f; // down, then back up
                    e.wheel.mouse_x = st_x;
                    e.wheel.mouse_y = st_y;
                    const Stopwatch t;
                    editor.handle_event(e);
                    editor.update(1.0 / 60.0);
                    const f64 ms = t.elapsed_ms();
                    wheel_ms += ms;
                    wheel_worst = std::max(wheel_worst, ms);
                    ++wheels;
                }
                if (bench_assets && f == frames / 2) {
                    // Search across everything, then scroll the results.
                    const Stopwatch search;
                    editor.assets.set_search("предмет");
                    FORGE_INFO("assets: search «предмет» %.2f ms, %s rows", search.elapsed_ms(),
                               group_digits(editor.assets.row_count()).c_str());
                }
                if (bench_assets)
                    if (Rml::Element* list = editor.find_element("as-list")) {
                        float top = list->GetScrollTop() + 53.0f;
                        if (top >= list->GetScrollHeight() - list->GetClientHeight()) top = 0;
                        list->SetScrollTop(top);
                    }
                // Flies over the world fast while painting: new chunks load and
                // edited ones go out of view all the time.
                if (bench_level && f > 10) {
                    LevelEditor& lv = editor.level;
                    lv.camera().x += 24;
                    lv.camera().y = 40 * std::sin(f * 0.05);
                    level::TileStroke stroke(lv.level(), "bench");
                    stroke.paint_disc(1, static_cast<i32>(lv.camera().x), static_cast<i32>(lv.camera().y), 4, 3);
                }
                if (f == 1 && select && !editor.doc.roots().empty()) {
                    const ObjectId group = editor.doc.roots()[0];
                    const ObjectId first = editor.doc.children_of(group).empty() ? group : editor.doc.children_of(group)[0];
                    editor.select({first});
                    editor.reveal(first);
                }
                if (f == 2 && play) editor.toggle_play();
                if (f == 3 && select && editor.tab() == "ui") { // a button of the menu, for its design
                    const editor::design::Node& root = editor.ui_tab.screen().root;
                    if (!root.children.empty() && !root.children.back().children.empty())
                        editor.ui_tab.select({root.children.back().children.front().id});
                }
                if (f == 1 && !options.talk.empty()) {
                    editor.open_tab("story");
                    const std::string& t = options.talk;
                    const bool ok = t.rfind("cast:", 0) == 0    ? editor.story_tab.show_cast(t.substr(5))
                                    : t.rfind("novel:", 0) == 0 ? editor.story_tab.show_novel(t.substr(6))
                                                                : editor.story_tab.open(t);
                    if (!ok) FORGE_WARN("no talk «%s»", t.c_str());
                }
                if (!options.templates.empty() && f == 1) editor.open_tab("ui");
                if (!options.templates.empty() && f == 2) {
                    const std::string& t = options.templates;
                    const usize colon = t.find(':');
                    const std::string file = colon == std::string::npos ? t : t.substr(colon + 1);
                    editor.ui_tab.open_templates(t == "screen" ? "screen" : "construction");
                    // save: the form for the whole screen; save:<layer>: for a layer at the top named so.
                    if (t == "save" || t.rfind("save:", 0) == 0) {
                        u32 layer = 0;
                        for (const editor::design::Node& n : editor.ui_tab.screen().root.children)
                            if (colon != std::string::npos && n.name == file) layer = n.id;
                        editor.ui_tab.begin_save_template(layer);
                    }
                    const auto& all = editor.ui_tab.templates();
                    for (usize i = 0; i < all.size(); ++i) {
                        if (all[i].info.file != file) continue;
                        editor.ui_tab.templates_show(editor::design::template_kind_word(all[i].info.kind), all[i].info.group);
                        editor.ui_tab.select_template(i);
                        if (t.rfind("new:", 0) == 0 || t.rfind("check:", 0) == 0) editor.ui_tab.screen_from_template(i);
                        if (t.rfind("check:", 0) == 0) editor.ui_tab.set_checking(true);
                        if (t.rfind("insert:", 0) == 0) editor.ui_tab.insert_template(i);
                    }
                }
                if (f == 1 && tab > 0)
                    if (Rml::Element* bar = editor.find_element("editor-tabs"); bar && tab < bar->GetNumChildren())
                        bar->GetChild(tab)->Click();
                // Offscreen frames take microseconds: give the tab highlight's colour
                // transition (started by the frame after the click) time to finish.
                if (f == 2 && tab > 0) SDL_Delay(300);
                if (bench) {
                    if (Rml::Element* list = editor.find_element("hierarchy")) {
                        float top = list->GetScrollTop() + 37.0f;
                        if (top >= list->GetScrollHeight() - list->GetClientHeight()) top = 0;
                        list->SetScrollTop(top);
                    }
                }
                const Stopwatch frame_timer;
                editor.update(1.0 / 60.0);
                const f64 cpu_update = frame_timer.elapsed_ms();
                if (bench_scheme && cpu_update > 100) FORGE_INFO("scheme: frame %u update %.0f ms", f, cpu_update);
                SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
                const Stopwatch render_timer;
                editor.render(cmd, target, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h);
                const f64 cpu_render = render_timer.elapsed_ms();
                SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
                SDL_WaitForGPUFences(device, true, &fence, 1);
                SDL_ReleaseGPUFence(device, fence);
                if (f < 3) continue;
                update_ms += cpu_update;
                render_ms += cpu_render;
                worst_ms = std::max(worst_ms, cpu_update + cpu_render);
                ++measured;
            }
            measured = std::max(measured, 1u);
            if (bench_scheme)
                FORGE_INFO("scheme: pan %.2f ms avg / %.2f worst per mouse move (%u), node drag %.2f ms avg / %.2f worst (%u)",
                           pan_ms / std::max(pans, 1u), pan_worst, pans, drag_ms / std::max(drags, 1u), drag_worst, drags);
            if (bench_story)
                FORGE_INFO("story: a wheel step and the frame after it %.2f ms avg / %.2f worst (%u); %zu lines shown",
                           wheel_ms / std::max(wheels, 1u), wheel_worst, wheels, editor.story_tab.lines_shown());
            if (bench_assets)
                FORGE_INFO("assets: %s rows listed, %s previews made", group_digits(editor.assets.row_count()).c_str(),
                           group_digits(editor.assets.thumbs_made()).c_str());
            if (bench_level)
                FORGE_INFO("level: flew %.0f tiles painting; edits %llu; chunks in memory %u", editor.level.camera().x,
                           static_cast<unsigned long long>(editor.level.level().edits()), editor.level.level().world().stats().resident);
            FORGE_INFO("editor (CPU, %u frames, %s objects, %u rows listed%s): update avg %.3f ms, render avg %.3f ms, "
                       "worst frame %.3f ms",
                       measured, group_digits(editor.doc.object_count()).c_str(), editor.hierarchy.count(),
                       bench ? ", hierarchy scrolling" : "", update_ms / measured, render_ms / measured, worst_ms);
            result = screenshot ? (render::save_png(device, target, w, h, screenshot) ? 0 : 1) : 0;
            if (self_test) {
                const bool passed = test.passed() && check_project_folder() == 0;
                FORGE_INFO("self-test %s", passed ? "passed" : "FAILED");
                if (!passed) result = 1;
            }
        }
        editor.shutdown();
    }
    if (target) SDL_ReleaseGPUTexture(device, target);
    render::destroy_offscreen_device(device);
    jobs::shutdown();
    return result;
}

} // namespace

int main(int argc, char** argv) {
    AppConfig config;
    config.title = "Forge — редактор";
    config.background_fps = 30; // leave the PC to the game the user starts next to the editor
    config.shader_formats = render::supported_shader_formats();
    EditorApp app;
    const char* screenshot = nullptr;
    u32 frames = 10;
    bool select = false, play = false, bench = false, self_test = false, bench_level = false;
    u32 bench_assets = 0, bench_scheme = 0;
    bool bench_story = false;
    int tab = 0;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--no-vsync") == 0) config.vsync = false;
        else if (std::strcmp(argv[i], "--ui") == 0 && has_value) app.options.ui_dir = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--scene") == 0 && has_value) app.options.scene = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--assets") == 0 && has_value) app.options.assets = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--project") == 0 && has_value) app.options.project = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--level") == 0 && has_value) {
            app.options.level = utf8_path(argv[++i]);
            app.options.level_given = true;
        }
        else if (std::strcmp(argv[i], "--theme") == 0 && has_value) app.options.theme = argv[++i];
        else if (std::strcmp(argv[i], "--objects") == 0 && has_value) app.options.objects = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--screenshot") == 0 && has_value) screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && has_value) frames = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--select") == 0) select = true;
        else if (std::strcmp(argv[i], "--play") == 0) play = true;
        else if (std::strcmp(argv[i], "--bench") == 0) bench = true;
        else if (std::strcmp(argv[i], "--bench-level") == 0) bench_level = true;
        else if (std::strcmp(argv[i], "--bench-assets") == 0) {
            bench_assets = 50'000;
            if (has_value && argv[i + 1][0] != '-') bench_assets = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (std::strcmp(argv[i], "--bench-scheme") == 0) {
            bench_scheme = 5'000;
            if (has_value && argv[i + 1][0] != '-') bench_scheme = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        }
        else if (std::strcmp(argv[i], "--bench-story") == 0) bench_story = true;
        else if (std::strcmp(argv[i], "--talk") == 0 && has_value) app.options.talk = argv[++i];
        else if (std::strcmp(argv[i], "--templates") == 0 && has_value) app.options.templates = argv[++i];
        else if (std::strcmp(argv[i], "--self-test") == 0) self_test = true;
        else if (std::strcmp(argv[i], "--tab") == 0 && has_value) tab = std::atoi(argv[++i]);
    }
    if (screenshot || bench || self_test || bench_level || bench_assets || bench_scheme || bench_story)
        return run_offscreen(app.options, screenshot, frames, select, play, bench, self_test, tab, bench_level, bench_assets,
                             bench_scheme, bench_story);
    return app.run(config);
}
