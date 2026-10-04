// Forge editor shell: hierarchy, inspector, world view, log and history, with
// undo/redo of every action and Play/Stop. The window layout lives in
// ui/editor/editor.rml and editor.rcss and updates while the editor runs.
//
//   forge_editor [--scene FILE] [--ui DIR] [--theme NAME] [--objects N] [--no-vsync]
//   forge_editor --screenshot out.png [--frames N] [--select] [--play] [--theme NAME]   offscreen
//   forge_editor --bench [--frames N]   offscreen: every object listed, the hierarchy scrolling
//   forge_editor --self-test [--screenshot out.png]   offscreen: drives the controls, fails on a wrong result
//
// Without --scene the editor opens scene.forge.json in the current folder, or
// makes a sample scene of 50 000 objects when there is none.

#include "components.h"
#include "demo_art.h"

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

    bool init(SDL_GPUDevice* device, SDL_Window* window, SDL_GPUTextureFormat format, u32 width, u32 height,
              const std::filesystem::path& ui_dir, const std::string& theme, u32 objects) {
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
        log_set_sink(nullptr, nullptr);
        ui::register_list_source("hierarchy", nullptr);
        sprites_.shutdown();
        ui_.shutdown();
    }

    // --- actions (buttons, keys) ---

    void undo() {
        if (history.undo()) FORGE_INFO("Отменено");
    }
    void redo() {
        if (history.redo()) FORGE_INFO("Повторено");
    }
    void save() {
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
        refresh_drawables();
        if (play.playing() && !paused_) simulate(static_cast<f32>(std::min(dt, 0.1)));
        hierarchy.refresh();
        sync_model();
        // Bindings may fire change events while they write values into the
        // document (a slider snapping to its step, a field losing focus as it
        // is rebuilt); those are not the user's edits.
        in_ui_update_ = true;
        ui_.update();
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

        const bool ui_used = ui_.handle_event(context_, e);
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
        on("play", [this](Rml::Event&, const Rml::VariantList&) { if (!play.playing()) toggle_play(); });
        on("stop", [this](Rml::Event&, const Rml::VariantList&) { if (play.playing()) toggle_play(); });
        on("pause", [this](Rml::Event&, const Rml::VariantList&) {
            if (play.playing()) paused_ = !paused_;
            model_.DirtyVariable("paused");
        });
        on("set_theme", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { pending_theme_ = arg_str(a, 0); });
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
        model_ = model.GetModelHandle();
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
        set(m_can_undo_, history.can_undo(), "can_undo");
        set(m_can_redo_, history.can_redo(), "can_redo");
        set(m_undo_label_, history.undo_label(), "undo_label");
        set(m_dirty_, history.dirty(), "dirty");
        set(m_scene_name_, path_to_utf8(scene_path.filename()), "scene_name");
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
        if (time_ - status_time_ > 0.25) {
            status_time_ = time_;
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
        for (auto& [level, text] : lines) m_log_.push_back({std::move(text), level});
        if (m_log_.size() > 300) m_log_.erase(m_log_.begin(), m_log_.end() - 300);
        model_.DirtyVariable("log");
        scroll_log_ = 2; // after the next layout
    }

    void follow_log() {
        if (scroll_log_ == 0) return;
        if (--scroll_log_ > 0) return;
        if (Rml::Element* log = find_element("log")) log->SetScrollTop(log->GetScrollHeight());
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
        if (!view) return;
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
        SDL_EndGPURenderPass(pass);
    }

    bool handle_key(const SDL_KeyboardEvent& k) {
        const bool ctrl = (k.mod & SDL_KMOD_CTRL) != 0;
        const bool shift = (k.mod & SDL_KMOD_SHIFT) != 0;
        if (ctrl && k.key == SDLK_Z) { shift ? redo() : undo(); return true; }
        if (ctrl && k.key == SDLK_Y) { redo(); return true; }
        if (ctrl && k.key == SDLK_S) { save(); return true; }
        if (ctrl && k.key == SDLK_D) { duplicate_selection(); return true; }
        if (k.key == SDLK_F5) { toggle_play(); return true; }
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
        if (!editor_.init(gpu(), window(), swapchain_format(), static_cast<u32>(w), static_cast<u32>(h),
                          options.ui_dir, options.theme, options.objects)) {
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
// the world, undo and redo it, run the game and stop it, delete and duplicate.
// Fails when any action does not do what the user would expect.
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
        case 43: return false;
        default: break;
        }
        return true;
    }
    bool passed() const { return failures_ == 0; }

private:
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
    usize entries_ = 0, count_ = 0;
    int failures_ = 0;
};

int run_offscreen(const Options& options, const char* screenshot, u32 frames, bool select, bool play, bool bench,
                  bool self_test) {
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
        if (target && editor.init(device, nullptr, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h, options.ui_dir,
                                  options.theme, options.objects)) {
            f64 update_ms = 0, render_ms = 0, worst_ms = 0;
            u32 measured = 0;
            if (bench) editor.hierarchy.expand_all(true);
            SelfTest test(editor);
            if (self_test) frames = std::max(frames, 50u);
            bool testing = self_test;
            for (u32 f = 0; f < frames; ++f) {
                if (testing) testing = test.step(f);
                if (f == 1 && select && !editor.doc.roots().empty()) {
                    const ObjectId group = editor.doc.roots()[0];
                    const ObjectId first = editor.doc.children_of(group).empty() ? group : editor.doc.children_of(group)[0];
                    editor.select({first});
                    editor.reveal(first);
                }
                if (f == 2 && play) editor.toggle_play();
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
    bool select = false, play = false, bench = false, self_test = false;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--no-vsync") == 0) config.vsync = false;
        else if (std::strcmp(argv[i], "--ui") == 0 && has_value) app.options.ui_dir = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--scene") == 0 && has_value) app.options.scene = utf8_path(argv[++i]);
        else if (std::strcmp(argv[i], "--theme") == 0 && has_value) app.options.theme = argv[++i];
        else if (std::strcmp(argv[i], "--objects") == 0 && has_value) app.options.objects = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--screenshot") == 0 && has_value) screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && has_value) frames = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--select") == 0) select = true;
        else if (std::strcmp(argv[i], "--play") == 0) play = true;
        else if (std::strcmp(argv[i], "--bench") == 0) bench = true;
        else if (std::strcmp(argv[i], "--self-test") == 0) self_test = true;
    }
    if (screenshot || bench || self_test)
        return run_offscreen(app.options, screenshot, frames, select, play, bench, self_test);
    return app.run(config);
}
