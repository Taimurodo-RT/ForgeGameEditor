// Forge editor shell: hierarchy, inspector, world view, log and history, with
// undo/redo of every action and Play/Stop. The window layout lives in
// ui/editor/editor.rml and editor.rcss and updates while the editor runs.
//
//   forge_editor [--scene FILE] [--level DIR] [--assets DIR] [--ui DIR] [--theme NAME] [--objects N] [--no-vsync]
//   forge_editor --screenshot out.png [--frames N] [--select] [--play] [--tab N] [--theme NAME]   offscreen
//   forge_editor --bench [--frames N]   offscreen: every object listed, the hierarchy scrolling
//   forge_editor --bench-level [--frames N]   offscreen: flying over the level while painting
//   forge_editor --bench-assets [N]   offscreen: a project of N files (50 000), indexed, listed, searched
//   forge_editor --self-test [--screenshot out.png]   offscreen: drives the controls, fails on a wrong result
//
// The «Уровень» tab opens «Старая шахта» from games/slice/level (or --level
// DIR); the «Сцена» tab opens scene.forge.json in the current folder (or
// --scene FILE), or makes a sample scene of 50 000 objects when there is none.

#include "asset_library.h"
#include "components.h"
#include "demo_art.h"
#include "level_editor.h"
#include "slice_level.h"

#include "forge/assets/image.h"
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
#include "forge/ui/tokens.h"
#include "forge/ui/ui.h"
#include "forge/ui/virtual_list.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <SDL3/SDL.h>

#include <algorithm>
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
    slice::SliceLevel level_module;
    LevelEditor level{level_module};
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
        if (!level.init(ui_, device, format, level_config)) return false;
        if (!assets.init(ui_, assets_config)) return false;
        context_ = ui_.create_context("editor", width, height);
        if (!context_ || !bind_model()) return false;
        ui::register_list_source("hierarchy", &hierarchy);
        if (!ui_.load_document(context_, "editor/editor.rml")) return false;

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
        log_set_sink(nullptr, nullptr);
        ui::register_list_source("hierarchy", nullptr);
        sprites_.shutdown();
        ui_.shutdown();
    }

    // --- actions (buttons, keys) ---

    void undo() {
        if (m_tab_ == "assets") assets.undo();
        else if (m_tab_ == "level") level.undo();
        else if (history.undo()) FORGE_INFO("Отменено");
    }
    void redo() {
        if (m_tab_ == "assets") assets.redo();
        else if (m_tab_ == "level") level.redo();
        else if (history.redo()) FORGE_INFO("Повторено");
    }
    // The open tab's history.
    UndoStack& active_history() {
        if (m_tab_ == "assets") return assets.history();
        return m_tab_ == "level" ? level.history() : history;
    }
    void open_tab(const std::string& key) {
        if (key == "assets" && m_tab_ != "assets") assets.opened();
        m_tab_ = key;
        model_.DirtyVariable("tab");
    }
    const std::string& tab() const { return m_tab_; }
    void save() {
        if (m_tab_ == "assets") return; // files are saved as they change
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
        ui_.update();
        level.set_ui_updating(false);
        assets.set_ui_updating(false);
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
        ui_.render(cmd, target, format, w, h);
    }

    bool handle_event(const SDL_Event& e) {
        const f32 density = window_ ? SDL_GetWindowPixelDensity(window_) : 1.0f;
        // Keys first, unless a text field has the keyboard.
        if (e.type == SDL_EVENT_KEY_DOWN && !text_focus() && handle_key(e.key)) return true;

        // Files dropped on the window go to the project's resources.
        if (e.type == SDL_EVENT_DROP_BEGIN && m_tab_ != "assets") open_tab("assets");
        const bool ui_used = ui_.handle_event(context_, e);
        if (m_tab_ == "assets") return assets.handle_event(e, density, ui_used, context_) || ui_used;
        if (m_tab_ == "level") return level.handle_event(e, density, ui_used, context_) || ui_used;
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
        model_ = model.GetModelHandle();
        level.set_model(model_);
        assets.set_model(model_);
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
        set(m_dirty_, m_tab_ != "assets" && h.dirty(), "dirty"); // files are written at once
        set(m_scene_name_,
            m_tab_ == "level"    ? level.title()
            : m_tab_ == "assets" ? std::string("Ресурсы проекта")
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
        if (m_tab_ == "assets") {
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
    std::filesystem::path assets; // empty: assets/ in the working folder
    std::string theme = "dark";
    u32 objects = 50'000;
};

class EditorApp final : public App {
public:
    Options options;

    bool on_init() override {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window(), &w, &h);
        if (!options.scene.empty()) editor_.scene_path = options.scene;
        LevelConfig lc;
        lc.folder = options.level;
        lc.game_exe = utf8_path(FORGE_SLICE_EXE);
        if (char* pref = SDL_GetPrefPath("Forge", "Editor")) {
            lc.settings = utf8_path(pref);
            SDL_free(pref);
        }
        AssetsConfig ac;
        ac.folder = options.assets.empty() ? std::filesystem::current_path() / "assets" : options.assets;
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
            check(shown("placeholder") && !shown("viewport"), "the tab replaces the world view");
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
        if (f >= 19) return asset_step();
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
            check(shown("obj-" + std::to_string(kCoins)), "the palette shows the objects");
            if (Rml::Element* e = ed_.find_element(("obj-" + std::to_string(kCoins)).c_str())) e->Click();
            check(lv().armed_object() == kCoins, "a click on the palette takes coins");
            entries_ = lv().history().size();
            click_cell(cx_ + 10, cy_);
            check(lv().history().size() == entries_ + 1 && lv().history().undo_label() == "Поставить: Монеты",
                  "a click in the world places the coins, one history entry");
            check(lv().selection().size() == 1, "the placed object is selected");
            object_ = lv().selection().empty() ? 0 : lv().selection()[0];
            const flecs::entity e = placed();
            check(e.is_valid() && e.has<slice::Item>() && e.get<slice::Item>().kind == u8(slice::ItemKind::Coins),
                  "the coins are in the world");
            break;
        }
        case 15:
            check(shown("obj-delete") && shown("lv-num-2"), "the properties show the coins and their fields");
            lv().set_field(2, "7", false);
            check(count() == 7 && lv().history().undo_label() == "«Монеты»: Сколько", "«Сколько» is set to 7");
            key(SDLK_Z, SDL_KMOD_CTRL);
            check(count() == 10, "Ctrl+Z gives the old count back");
            key(SDLK_Y, SDL_KMOD_CTRL);
            check(count() == 7, "Ctrl+Y sets it again");
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
    static constexpr i32 kCoins = 4;

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
            return false;
        default: break;
        }
        ++as_step_;
        return true;
    }
    flecs::entity placed() { return lv().level().find(object_); }
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
        return e && e->IsVisible(true);
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
    void check(bool ok, const char* what) {
        if (ok) {
            FORGE_INFO("self-test: %s", what);
        } else {
            FORGE_ERROR("self-test FAILED: %s", what);
            ++failures_;
        }
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
    u32 as_step_ = 0, waited_ = 0;
    std::string drop_text_;
    bool typed_ = false;
    int failures_ = 0;
};

int run_offscreen(const Options& options, const char* screenshot, u32 frames, bool select, bool play, bool bench,
                  bool self_test, int tab, bool bench_level, u32 bench_assets) {
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
        // Nor the game's level, unless one is given.
        LevelConfig lc;
        lc.offscreen = true;
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
            for (u32 f = 0; f < frames; ++f) {
                if (testing) testing = test.step(f);
                else if (self_test) break;
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
                FORGE_INFO("self-test %s", test.passed() ? "passed" : "FAILED");
                if (!test.passed()) result = 1;
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
    config.shader_formats = render::supported_shader_formats();
    EditorApp app;
    const char* screenshot = nullptr;
    u32 frames = 10;
    bool select = false, play = false, bench = false, self_test = false, bench_level = false;
    u32 bench_assets = 0;
    int tab = 0;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--no-vsync") == 0) config.vsync = false;
        else if (std::strcmp(argv[i], "--ui") == 0 && has_value) app.options.ui_dir = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--scene") == 0 && has_value) app.options.scene = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--assets") == 0 && has_value) app.options.assets = utf8_path(argv[++i]);
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
        else if (std::strcmp(argv[i], "--self-test") == 0) self_test = true;
        else if (std::strcmp(argv[i], "--tab") == 0 && has_value) tab = std::atoi(argv[++i]);
    }
    if (screenshot || bench || self_test || bench_level || bench_assets)
        return run_offscreen(app.options, screenshot, frames, select, play, bench, self_test, tab, bench_level, bench_assets);
    return app.run(config);
}
