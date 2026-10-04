#include "level_editor.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/data/json.h"
#include "forge/editor/inspector.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge::editor_app {

namespace fs = std::filesystem;

namespace {

// The panels, their titles and icons. The default places them as in Unreal:
// the palette on the left, the minimap and properties on the right, the log
// and history under the view.
struct PanelInfo {
    const char* id;
    const char* title;
    const char* icon;
};
const PanelInfo kPanels[] = {
    {"palette", "Палитра", "palette"},
    {"minimap", "Мини-карта", "map"},
    {"props", "Свойства", "tune"},
    {"history", "История", "history"},
    {"log", "Журнал", "terminal"},
};
const char* kDefaultLayout =
    R"({"row":0.17,"a":{"panels":["palette"]},"b":{"row":0.77,"a":{"column":0.76,"a":{"view":true},"b":{"panels":["log","history"]}},"b":{"column":0.42,"a":{"panels":["minimap"]},"b":{"panels":["props"]}}}})";
constexpr usize kFillLimit = 100'000;
constexpr u32 kIconPx = 32;

const char* tool_name(Tool t) {
    switch (t) {
    case Tool::Brush: return "Кисть";
    case Tool::Line: return "Линия";
    case Tool::Rect: return "Прямоугольник";
    case Tool::Fill: return "Заливка";
    case Tool::Eraser: return "Ластик";
    case Tool::Picker: return "Пипетка";
    }
    return "";
}

const char* tool_help(Tool t) {
    switch (t) {
    case Tool::Brush: return "Левая кнопка рисует выбранной плиткой. [ и ] меняют размер.";
    case Tool::Line: return "Потяните от начала к концу линии и отпустите.";
    case Tool::Rect: return "Потяните от угла к углу и отпустите.";
    case Tool::Fill: return "Заполняет соседние клетки того же вида на слое плитки.";
    case Tool::Eraser: return "Стирает клетки на выбранном слое. [ и ] меняют размер.";
    case Tool::Picker: return "Щёлкните по клетке, чтобы взять её плитку.";
    }
    return "";
}

struct ModeInfo {
    Mode mode;
    const char* id;
    const char* name;
    const char* help;
    const char* when; // empty: works now
};
const ModeInfo kModes[] = {
    {Mode::Select, "select", "Выбор",
     "Щёлкните объект, чтобы выбрать и двигать его; Ctrl добавляет к выбору. Пустое место двигает вид, колесо приближает.", ""},
    {Mode::Tiles, "tiles", "Тайлы", "Рисуйте мир плитками по слоям: стены, блоки, жидкости.", ""},
    {Mode::Objects, "objects", "Объекты", "Выберите объект слева и щёлкайте по миру, чтобы ставить. Правая кнопка или Esc: снова выбор, Delete удаляет.",
     ""},
    {Mode::Physics, "physics", "Физика", "Гравитация мира и её точки, зоны воды и песка, проба течения воды прямо в редакторе.",
     "позже, после библиотеки объектов"},
    {Mode::Light, "light", "Свет", "Факелы и другие источники, время суток, вид «как в игре».", "позже, после библиотеки объектов"},
    {Mode::Zones, "zones", "Зоны", "Области («Деревня», «Шахта»), точка появления героя, триггеры квестов, звук и музыка мест.",
     "позже, вместе с редактором сюжета"},
};

const ModeInfo& mode_info(Mode m) {
    for (const ModeInfo& i : kModes)
        if (i.mode == m) return i;
    return kModes[0];
}

std::string group_digits(u64 n) {
    std::string digits = std::to_string(n), out;
    for (usize i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += " ";
        out += digits[i];
    }
    return out;
}

} // namespace

const char* tool_id(Tool t) {
    switch (t) {
    case Tool::Brush: return "brush";
    case Tool::Line: return "line";
    case Tool::Rect: return "rect";
    case Tool::Fill: return "fill";
    case Tool::Eraser: return "eraser";
    case Tool::Picker: return "picker";
    }
    return "";
}

const char* mode_id(Mode m) { return mode_info(m).id; }

void LevelEditor::set_mode(Mode m) {
    if (stroke_ || moving_) return;
    mode_ = m;
    if (m != Mode::Objects) object_ = -1;
}

LevelEditor::LevelEditor(level::LevelModule& module) : module_(module) {}

LevelEditor::~LevelEditor() { shutdown(); }

bool LevelEditor::init(ui::Ui& ui, SDL_GPUDevice* device, SDL_GPUTextureFormat format, const LevelConfig& config) {
    ui_ = &ui;
    config_ = config;
    level_ = std::make_unique<level::Level>(module_);
    std::error_code ec;
    fs::create_directories(config.folder, ec);
    std::string error;
    if (!level_->open(config.folder, &error)) {
        FORGE_ERROR("Уровень %s не открылся: %s", path_to_utf8(config.folder).c_str(), error.c_str());
        return false;
    }
    module_.start(camera_.x, camera_.y);
    camera_.zoom = 16;
    history_.clear();

    if (!module_.init_view(device, format)) return false;
    art_ = demo::make_sprite_sheet();
    if (!back_.init(device, format, art_.sheet(), 16)) return false;
    if (!front_.init(device, format, art_.sheet(), 1u << 15)) return false;
    view_ready_ = true;

    // Palette: icons as pictures the documents can show, grouped.
    const auto& tiles = module_.tiles();
    std::vector<u8> icon;
    for (usize i = 0; i < tiles.size(); ++i) {
        const level::TileDef& t = tiles[i];
        module_.tile_icon(t, kIconPx, icon);
        ui.set_image("tile_" + t.id, icon.data(), kIconPx, kIconPx);
        auto g = std::find_if(m_palette_.begin(), m_palette_.end(), [&](const PaletteGroup& pg) { return pg.name == t.group; });
        if (g == m_palette_.end()) {
            m_palette_.push_back({t.group, {}});
            g = m_palette_.end() - 1;
        }
        g->tiles.push_back({static_cast<int>(i), t.name, "/memory/tile_" + t.id, t.key, static_cast<int>(t.layer)});
    }
    for (const std::string& name : module_.layer_names()) m_layers_.push_back(name);
    build_objects();
    if (!tiles.empty()) select_tile(0);

    // The panel layout: the user's, else the default.
    DockConfig dc;
    dc.area = "dock";
    dc.view = "level-view";
    dc.pane_prefix = "pane-";
    for (const PanelInfo& p : kPanels) dc.panels.push_back({p.id, p.title, p.icon});
    dc.default_layout = kDefaultLayout;
    dc.file = "level_layout.json";
    dock_.init(std::move(dc), config.settings, !config.offscreen);
    map_rgba_.assign(static_cast<usize>(kMapPx) * kMapPx * 4, 0);
    FORGE_INFO("Уровень «%s» открыт: %s", module_.title().c_str(), path_to_utf8(config.folder).c_str());
    return true;
}

void LevelEditor::build_objects() {
    objects_version_ = module_.objects_version();
    // The armed object stays armed if its template is still there.
    const auto& objects = module_.objects();
    const u64 armed = armed_key_;
    object_ = -1;
    m_objects_.clear();
    std::vector<u8> icon;
    for (usize i = 0; i < objects.size(); ++i) {
        const level::ObjectDef& o = objects[i];
        module_.object_icon(o, kIconPx, icon);
        ui_->set_image("obj_" + o.id, icon.data(), kIconPx, kIconPx);
        auto g = std::find_if(m_objects_.begin(), m_objects_.end(), [&](const PaletteGroup& pg) { return pg.name == o.group; });
        if (g == m_objects_.end()) {
            m_objects_.push_back({o.group, {}});
            g = m_objects_.end() - 1;
        }
        g->tiles.push_back({static_cast<int>(i), o.name, "/memory/obj_" + o.id, "", 0});
        if (armed && o.key == armed) object_ = static_cast<i32>(i);
    }
    if (object_ < 0) armed_key_ = 0;
    if (model_) model_.DirtyVariable("lv_objects");
}

void LevelEditor::shutdown() {
    if (game_) {
        SDL_DestroyProcess(game_); // the game keeps running on its own
        game_ = nullptr;
    }
    if (view_ready_) {
        front_.shutdown();
        back_.shutdown();
        module_.shutdown_view();
        view_ready_ = false;
    }
    level_.reset();
}

void LevelEditor::bind(Rml::DataModelConstructor& model) {
    dock_.bind(model);
    if (auto s = model.RegisterStruct<PaletteTile>()) {
        s.RegisterMember("index", &PaletteTile::index);
        s.RegisterMember("name", &PaletteTile::name);
        s.RegisterMember("icon", &PaletteTile::icon);
        s.RegisterMember("key", &PaletteTile::key);
        s.RegisterMember("layer", &PaletteTile::layer);
    }
    model.RegisterArray<std::vector<PaletteTile>>();
    if (auto s = model.RegisterStruct<PaletteGroup>()) {
        s.RegisterMember("name", &PaletteGroup::name);
        s.RegisterMember("tiles", &PaletteGroup::tiles);
    }
    model.RegisterArray<std::vector<PaletteGroup>>();

    if (auto s = model.RegisterStruct<FieldView>()) {
        s.RegisterMember("kind", &FieldView::kind);
        s.RegisterMember("label", &FieldView::label);
        s.RegisterMember("value", &FieldView::value);
        s.RegisterMember("min", &FieldView::min);
        s.RegisterMember("max", &FieldView::max);
        s.RegisterMember("step", &FieldView::step);
        s.RegisterMember("hint", &FieldView::hint);
        s.RegisterMember("own", &FieldView::own);
        s.RegisterMember("advanced", &FieldView::advanced);
    }
    model.RegisterArray<std::vector<FieldView>>();
    model.Bind("lv_objects", &m_objects_);
    model.Bind("lv_object", &m_object_);
    model.Bind("lv_sel_count", &m_sel_count_);
    model.Bind("lv_sel_name", &m_sel_name_);
    model.Bind("lv_sel_hint", &m_sel_hint_);
    model.Bind("lv_sel_icon", &m_sel_icon_);
    model.Bind("lv_sel_kind", &m_sel_kind_);
    model.Bind("lv_details", &m_details_);
    model.Bind("lv_fields", &m_fields_);
    model.Bind("lv_palette", &m_palette_);
    model.Bind("lv_layers", &m_layers_);
    model.Bind("lv_mode", &m_mode_);
    model.Bind("lv_mode_name", &m_mode_name_);
    model.Bind("lv_mode_help", &m_mode_help_);
    model.Bind("lv_mode_when", &m_mode_when_);
    model.Bind("lv_tool", &m_tool_);
    model.Bind("lv_tool_name", &m_tool_name_);
    model.Bind("lv_tool_help", &m_tool_help_);
    model.Bind("lv_tile", &m_tile_);
    model.Bind("lv_layer", &m_layer_);
    model.Bind("lv_radius", &m_radius_);
    model.Bind("lv_tile_name", &m_tile_name_);
    model.Bind("lv_tile_hint", &m_tile_hint_);
    model.Bind("lv_tile_icon", &m_tile_icon_);
    model.Bind("lv_tile_layer", &m_tile_layer_);
    model.Bind("lv_light", &m_light_);
    model.Bind("lv_grid", &m_grid_);
    model.Bind("lv_chunks", &m_chunks_);
    model.Bind("lv_outline", &m_outline_);
    model.Bind("lv_dirty", &m_dirty_);
    model.Bind("lv_minimap", &m_minimap_);
    model.Bind("lv_place", &m_place_);
    model.Bind("lv_info", &m_info_);
    model.Bind("lv_history", &m_history_);
    model.Bind("lv_history_cursor", &m_history_cursor_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_int = [](const Rml::VariantList& a, usize i, int fallback = 0) { return i < a.size() ? a[i].Get<int>() : fallback; };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };

    on("lv_tool", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string id = arg_str(a, 0);
        set_mode(Mode::Tiles);
        for (Tool t : {Tool::Brush, Tool::Line, Tool::Rect, Tool::Fill, Tool::Eraser, Tool::Picker})
            if (id == tool_id(t)) set_tool(t);
    });
    on("lv_mode", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string id = arg_str(a, 0);
        for (const ModeInfo& m : kModes)
            if (id == m.id) set_mode(m.mode);
    });
    on("lv_object", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { arm_object(arg_int(a, 0, -1)); });
    on("lv_delete", [this](Rml::Event&, const Rml::VariantList&) { delete_selection(); });
    on("lv_field_text", [this, arg_int, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (a.size() > 2 && a[2].Get<bool>()) set_field(arg_int(a, 0, -1), arg_str(a, 1), false);
    });
    on("lv_field_commit", [this, arg_int](Rml::Event& ev, const Rml::VariantList& a) {
        Rml::Element* e = ev.GetTargetElement();
        if (e && e->GetTagName() == "input")
            set_field(arg_int(a, 0, -1), static_cast<Rml::ElementFormControl*>(e)->GetValue(), false);
    });
    on("lv_field_slide", [this, arg_int, arg_str](Rml::Event&, const Rml::VariantList& a) {
        set_field(arg_int(a, 0, -1), arg_str(a, 1), true);
    });
    on("lv_field_toggle", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0, -1);
        if (i >= 0 && i < static_cast<int>(m_fields_.size())) set_field(i, m_fields_[i].value == "true" ? "false" : "true", false);
    });
    on("lv_field_reset", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0, -1);
        if (i >= 0 && i < static_cast<int>(field_refs_.size()) && field_refs_[i].prop)
            set_field(i, field_refs_[i].template_value, false);
    });
    on("lv_details", [this](Rml::Event&, const Rml::VariantList&) {
        m_details_ = !m_details_;
        model_.DirtyVariable("lv_details");
    });
    on("lv_field_cycle", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0, -1);
        if (i < 0 || i >= static_cast<int>(field_refs_.size())) return;
        const auto& options = field_refs_[i].options;
        if (options.empty()) return;
        auto it = std::find(options.begin(), options.end(), m_fields_[i].value);
        const i64 at = it == options.end() ? 0 : it - options.begin();
        const i64 n = static_cast<i64>(options.size());
        set_field(i, options[static_cast<usize>(((at + arg_int(a, 1, 1)) % n + n) % n)], false);
    });
    on("lv_tile", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        select_tile(static_cast<usize>(arg_int(a, 0)));
        set_mode(Mode::Tiles);
        if (tool_ == Tool::Eraser || tool_ == Tool::Picker) set_tool(Tool::Brush);
    });
    on("lv_layer", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int l = arg_int(a, 0);
        if (l >= 0 && l < static_cast<int>(m_layers_.size())) layer_ = static_cast<u32>(l);
    });
    on("lv_radius", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { set_brush_radius(radius_ + arg_int(a, 0)); });
    on("lv_toggle", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string what = arg_str(a, 0);
        if (what == "light") view_.game_light = !view_.game_light;
        else if (what == "grid") view_.grid = !view_.grid;
        else if (what == "chunks") view_.chunks = !view_.chunks;
        else if (what == "outline") rect_outline_ = !rect_outline_;
    });
    on("lv_save", [this](Rml::Event&, const Rml::VariantList&) { save(); });
    on("lv_play", [this](Rml::Event&, const Rml::VariantList&) { play_here(); });
    on("lv_history_jump", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        if (stroke_) return;
        const usize target = static_cast<usize>(arg_int(a, 0, -1) + 1);
        while (history_.cursor() > target && history_.undo()) {}
        while (history_.cursor() < target && history_.redo()) {}
    });
    on("lv_minimap_click", [this](Rml::Event& ev, const Rml::VariantList&) {
        Rml::Element* e = ev.GetCurrentElement();
        if (!e) return;
        const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Content);
        const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Content);
        if (size.x <= 0 || size.y <= 0) return;
        const f32 fx = (ev.GetParameter<float>("mouse_x", 0) - at.x) / size.x;
        const f32 fy = (ev.GetParameter<float>("mouse_y", 0) - at.y) / size.y;
        const f64 span = static_cast<f64>(kMapPx) * kMapTilesPerPx;
        camera_.x = map_cx_ + (std::clamp(fx, 0.0f, 1.0f) - 0.5) * span;
        camera_.y = map_cy_ + (std::clamp(fy, 0.0f, 1.0f) - 0.5) * span;
    });
}

// --- tools -------------------------------------------------------------------

void LevelEditor::set_tool(Tool t) {
    if (stroke_) return; // not in the middle of a stroke
    tool_ = t;
}

void LevelEditor::select_tile(usize index) {
    const auto& tiles = module_.tiles();
    if (index >= tiles.size()) return;
    tile_ = index;
    layer_ = tiles[index].layer;
}

void LevelEditor::set_brush_radius(i32 r) { radius_ = std::clamp(r, 0, 16); }

world::TileId LevelEditor::paint_value() const {
    if (tool_ == Tool::Eraser) return 0;
    const auto& tiles = module_.tiles();
    return tile_ < tiles.size() ? tiles[tile_].value : 0;
}

std::string LevelEditor::stroke_label() const {
    const auto& tiles = module_.tiles();
    const std::string tile = tile_ < tiles.size() ? tiles[tile_].name : std::string();
    if (tool_ == Tool::Eraser) {
        const auto& layers = module_.layer_names();
        return std::string("Ластик: ") + (layer_ < layers.size() ? layers[layer_] : std::string());
    }
    return std::string(tool_name(tool_)) + ": " + tile;
}

void LevelEditor::shape_cells(std::vector<level::Cell>& out) const {
    out.clear();
    if (tool_ == Tool::Line) level::line_cells(start_x_, start_y_, last_x_, last_y_, out);
    else if (tool_ == Tool::Rect) level::rect_cells(start_x_, start_y_, last_x_, last_y_, !rect_outline_, out);
}

void LevelEditor::press(f32 x, f32 y) {
    i32 cx, cy;
    cell_at(x, y, cx, cy);
    start_x_ = last_x_ = cx;
    start_y_ = last_y_ = cy;
    switch (tool_) {
    case Tool::Picker: pick(cx, cy); return;
    case Tool::Fill: {
        std::vector<level::Cell> cells;
        bool capped = false;
        const u32 layer = tile_ < module_.tiles().size() ? module_.tiles()[tile_].layer : layer_;
        level::fill_cells(*level_, layer, cx, cy, kFillLimit, cells, &capped);
        auto stroke = std::make_unique<level::TileStroke>(*level_, stroke_label() + " (" + group_digits(cells.size()) + ")");
        if (stroke->paint(layer, cells, paint_value()) == 0) return;
        if (capped) FORGE_WARN("Заливка остановлена на %s клетках: область слишком большая", group_digits(kFillLimit).c_str());
        history_.execute(std::move(stroke));
        history_.seal();
        return;
    }
    case Tool::Line:
    case Tool::Rect:
        shaping_ = true;
        stroke_ = std::make_unique<level::TileStroke>(*level_, stroke_label());
        return;
    case Tool::Brush:
    case Tool::Eraser:
        stroke_ = std::make_unique<level::TileStroke>(*level_, stroke_label());
        stroke_->paint_disc(layer_, cx, cy, radius_, paint_value());
        return;
    }
}

void LevelEditor::drag(f32 x, f32 y) {
    if (!stroke_) return;
    i32 cx, cy;
    cell_at(x, y, cx, cy);
    if (cx == last_x_ && cy == last_y_) return;
    if (!shaping_) {
        // The brush leaves no gaps when the mouse jumps.
        std::vector<level::Cell> path;
        level::line_cells(last_x_, last_y_, cx, cy, path);
        for (usize i = 1; i < path.size(); ++i) stroke_->paint_disc(layer_, path[i].x, path[i].y, radius_, paint_value());
    }
    last_x_ = cx;
    last_y_ = cy;
}

void LevelEditor::release() {
    if (!stroke_) return;
    if (shaping_) {
        std::vector<level::Cell> cells;
        shape_cells(cells);
        stroke_->paint(layer_, cells, paint_value());
        shaping_ = false;
    }
    std::unique_ptr<level::TileStroke> stroke = std::move(stroke_);
    if (stroke->empty()) return;
    // Already painted: execute() sets the same values again and records it.
    history_.execute(std::move(stroke));
    history_.seal();
}

void LevelEditor::pick(i32 x, i32 y) {
    const auto& tiles = module_.tiles();
    // The topmost layer with something there.
    for (i32 layer = static_cast<i32>(module_.layer_names().size()) - 1; layer >= 0; --layer) {
        const world::TileId v = level_->tile(static_cast<u32>(layer), x, y);
        if (v == 0) continue;
        for (usize i = 0; i < tiles.size(); ++i)
            if (tiles[i].layer == static_cast<u32>(layer) && tiles[i].value == v) {
                select_tile(i);
                tool_ = Tool::Brush;
                return;
            }
        // A value the palette does not have (a half-full water cell): the
        // palette's first tile of that layer.
        for (usize i = 0; i < tiles.size(); ++i)
            if (tiles[i].layer == static_cast<u32>(layer)) {
                select_tile(i);
                tool_ = Tool::Brush;
                return;
            }
    }
}

// --- objects -----------------------------------------------------------------

void LevelEditor::arm_object(i32 index) {
    if (index >= static_cast<i32>(module_.objects().size())) return;
    set_mode(Mode::Objects);
    object_ = index;
    armed_key_ = index >= 0 ? module_.objects()[static_cast<usize>(index)].key : 0;
}

void LevelEditor::select_objects(std::vector<u64> ids) {
    if (ids == selection_) return;
    selection_ = std::move(ids);
    ++selection_version_;
}

void LevelEditor::delete_selection() {
    if (selection_.empty() || moving_) return;
    std::vector<level::ObjectSnapshot> gone;
    for (u64 id : selection_) {
        flecs::entity e = level_->find(id);
        if (e.is_valid()) gone.push_back(level::snapshot(*level_, e));
    }
    if (gone.empty()) return;
    std::string label = "Удалить: ";
    const auto& defs = module_.objects();
    const i32 kind = module_.object_kind(level_->find(gone[0].id));
    label += gone.size() == 1 && kind >= 0 ? defs[static_cast<usize>(kind)].name : std::to_string(gone.size()) + " объектов";
    history_.execute(std::make_unique<level::ObjectsCommand>(*level_, std::move(gone), false, label));
    history_.seal();
    select_objects({});
}

void LevelEditor::press_objects(f32 x, f32 y, bool add) {
    f64 tx, ty;
    to_tile(x, y, tx, ty);
    history_.seal();
    if (object_ >= 0) {
        // Place the armed object standing on the cell line under the mouse.
        const f64 px = std::floor(tx) + 0.5, py = std::floor(ty) + 1.0;
        flecs::entity e = module_.place_object(*level_, static_cast<usize>(object_), px, py);
        if (!e.is_valid()) return;
        level::ObjectSnapshot snap = level::snapshot(*level_, e);
        const u64 id = snap.id;
        history_.execute(std::make_unique<level::ObjectsCommand>(
            *level_, std::vector<level::ObjectSnapshot>{std::move(snap)}, true,
            "Поставить: " + module_.objects()[static_cast<usize>(object_)].name));
        history_.seal();
        select_objects({id});
        return;
    }
    flecs::entity hit = level_->pick(tx, ty);
    if (!hit.is_valid()) {
        if (!add) select_objects({});
        // Nothing there: the drag moves the view.
        panning_ = true;
        pan_button_ = SDL_BUTTON_LEFT;
        return;
    }
    const u64 id = level_->id_of(hit, true);
    std::vector<u64> ids = selection_;
    const auto it = std::find(ids.begin(), ids.end(), id);
    if (add) {
        if (it != ids.end()) ids.erase(it);
        else ids.push_back(id);
    } else if (it == ids.end()) {
        ids = {id};
    }
    select_objects(ids);
    if (std::find(selection_.begin(), selection_.end(), id) == selection_.end()) return;
    moving_ = true;
    move_x_ = tx;
    move_y_ = ty;
    move_start_.clear();
    for (u64 s : selection_) {
        flecs::entity e = level_->find(s);
        if (!e.is_valid()) continue;
        const scene::Position& p = e.get<scene::Position>();
        move_start_.push_back({s, p.tile_x(), p.tile_y(), p.tile_x(), p.tile_y()});
    }
}

void LevelEditor::drag_objects(f32 x, f32 y) {
    f64 tx, ty;
    to_tile(x, y, tx, ty);
    // Whole-tile steps keep things on the grid the tiles make.
    const f64 dx = std::round(tx - move_x_), dy = std::round(ty - move_y_);
    std::vector<level::MoveObjects::Move> moves = move_start_;
    bool any = false;
    for (auto& m : moves) {
        m.to_x = m.from_x + dx;
        m.to_y = m.from_y + dy;
        flecs::entity e = level_->find(m.id);
        if (const scene::Position* p = e.is_valid() ? e.try_get<scene::Position>() : nullptr)
            any |= p->tile_x() != m.to_x || p->tile_y() != m.to_y;
    }
    if (!any) return;
    const auto& defs = module_.objects();
    const i32 kind = moves.size() == 1 ? module_.object_kind(level_->find(moves[0].id)) : -1;
    const std::string label = kind >= 0 ? "Передвинуть: " + defs[static_cast<usize>(kind)].name
                                        : "Передвинуть объекты: " + std::to_string(moves.size());
    history_.execute(std::make_unique<level::MoveObjects>(*level_, std::move(moves), label));
}

void LevelEditor::rebuild_fields(Rml::Context* context) {
    const u64 version = level_->object_edits() * 1'000'003ull + selection_version_;
    if (version == fields_built_) return;
    // Do not rewrite a field while the user types in it.
    const Rml::Element* focus = context ? context->GetFocusElement() : nullptr;
    if (focus && focus->GetTagName() == "input" && focus->GetAttribute<Rml::String>("type", "text") == "text" &&
        fields_built_ % 1'000'003ull == selection_version_)
        return;
    fields_built_ = version;
    m_fields_.clear();
    field_refs_.clear();
    flecs::entity e = selection_.empty() ? flecs::entity() : level_->find(selection_[0]);
    const i32 kind = e.is_valid() ? module_.object_kind(e) : -1;
    if (kind >= 0) {
        const level::ObjectDef& def = module_.objects()[static_cast<usize>(kind)];
        m_sel_name_ = def.name;
        m_sel_hint_ = def.hint;
        m_sel_icon_ = "/memory/obj_" + def.id;
        const scene::Position& p = e.get<scene::Position>();
        char v[32];
        std::snprintf(v, sizeof(v), "%.2f", p.tile_x());
        m_fields_.push_back({"text", "X", v, "", 0, 0, 0});
        field_refs_.push_back({nullptr, "x", {}});
        std::snprintf(v, sizeof(v), "%.2f", p.tile_y());
        m_fields_.push_back({"text", "Y", v, "", 0, 0, 0});
        field_refs_.push_back({nullptr, "y", {}});
        // The template's properties in plain words; what the copy sets its
        // own way is marked and can go back to the template's.
        objects::Library* lib = module_.library();
        const objects::Template* tmpl = lib ? lib->template_of(e) : nullptr;
        const objects::KindDef* kd = tmpl ? lib->kind_of(*tmpl) : nullptr;
        const objects::ObjectRef* ref = e.try_get<objects::ObjectRef>();
        m_sel_kind_ = kd ? kd->name : "";
        if (kd)
            for (const objects::PropDef& prop : kd->props) {
                using reflect::Kind;
                const Kind k = prop.info->type->kind;
                const bool boolean = k == Kind::Bool;
                auto shown = [&](const std::string& json) { return boolean ? json : objects::Library::display(prop, json); };
                FieldView f;
                f.label = prop.name;
                f.hint = prop.hint;
                f.value = shown(lib->value(level_->scene(), e, prop));
                FieldRef r;
                r.prop = &prop;
                r.template_value = shown(lib->value(*tmpl, prop));
                if (!prop.choices.empty()) {
                    f.kind = "enum";
                    for (const objects::Choice& c : prop.choices) r.options.push_back(c.name);
                } else if (boolean) {
                    f.kind = "bool";
                } else if (k >= Kind::I8 && k <= Kind::F64 && prop.has_range) {
                    const bool integer = k <= Kind::U64;
                    f.kind = "slider";
                    f.min = static_cast<float>(prop.min);
                    f.max = static_cast<float>(prop.max);
                    f.step = integer ? 1.0f : static_cast<float>((prop.max - prop.min) / 200.0);
                } else {
                    f.kind = "text";
                }
                f.own = ref && ref->overrides_prop(prop.id);
                f.advanced = prop.advanced;
                m_fields_.push_back(std::move(f));
                field_refs_.push_back(std::move(r));
            }
        std::vector<editor::FieldRow> rows;
        for (const auto& c : level_->scene().saved_components()) {
            if (!module_.object_component_shown(c.type)) continue;
            const void* data = ecs_get_id(level_->scene().ecs().c_ptr(), e, c.id);
            if (!data) continue;
            rows.clear();
            editor::describe_fields(c.type, data, rows);
            for (editor::FieldRow& r : rows) {
                using reflect::Kind;
                FieldView f;
                f.label = r.label;
                f.value = r.value;
                const bool integer = r.kind >= Kind::I8 && r.kind <= Kind::U64;
                if (r.read_only || r.kind == Kind::Struct || r.kind == Kind::Array) f.kind = "readonly";
                else if (r.kind == Kind::Bool) f.kind = "bool";
                else if (r.kind == Kind::Enum) f.kind = "enum";
                else if ((integer || r.kind == Kind::F32 || r.kind == Kind::F64) && r.has_range) f.kind = "slider";
                else f.kind = "text";
                f.min = static_cast<float>(r.min);
                f.max = static_cast<float>(r.max);
                f.step = integer ? 1.0f : static_cast<float>((r.max - r.min) / 200.0);
                f.advanced = kd != nullptr; // the components themselves: «Подробно»
                m_fields_.push_back(std::move(f));
                field_refs_.push_back({c.type, r.path, std::move(r.options)});
            }
        }
    } else {
        m_sel_name_.clear();
        m_sel_hint_.clear();
        m_sel_icon_.clear();
        m_sel_kind_.clear();
    }
    model_.DirtyVariable("lv_sel_kind");
    model_.DirtyVariable("lv_fields");
    model_.DirtyVariable("lv_sel_name");
    model_.DirtyVariable("lv_sel_hint");
    model_.DirtyVariable("lv_sel_icon");
}

void LevelEditor::set_field(int i, const std::string& text, bool dragging) {
    // Bindings fill the inputs during the UI's update and fire change events then.
    if (ui_updating_ || i < 0 || i >= static_cast<int>(field_refs_.size()) || selection_.empty()) return;
    if (text == m_fields_[static_cast<usize>(i)].value) return; // shown rounded: not an edit
    flecs::entity e = level_->find(selection_[0]);
    if (!e.is_valid()) return;
    const FieldRef& ref = field_refs_[static_cast<usize>(i)];
    const scene::Position& p = e.get<scene::Position>();
    const i32 kind = module_.object_kind(e);
    const std::string name = kind >= 0 ? module_.objects()[static_cast<usize>(kind)].name : std::string();
    if (ref.prop) {
        objects::Library* lib = module_.library();
        const std::optional<std::string> json = lib ? objects::Library::parse(*ref.prop, text) : std::nullopt;
        if (!json) {
            fields_built_ = 0; // show the old value again
            return;
        }
        if (*json == lib->value(level_->scene(), e, *ref.prop)) {
            // The same value; only a reset ends the copy's own way.
            const objects::ObjectRef* r = e.try_get<objects::ObjectRef>();
            if (!r || !r->overrides_prop(ref.prop->id) || text != ref.template_value) return;
        }
        history_.execute(std::make_unique<level::SetObjectProp>(*level_, *lib, selection_[0], *ref.prop, *json,
                                                                "«" + name + "»: " + ref.prop->name));
        if (!dragging) history_.seal();
        return;
    }
    if (!ref.type) {
        char* end = nullptr;
        const f64 v = std::strtod(text.c_str(), &end);
        if (end == text.c_str()) {
            fields_built_ = 0; // show the old value again
            return;
        }
        level::MoveObjects::Move m{selection_[0], p.tile_x(), p.tile_y(), p.tile_x(), p.tile_y()};
        (ref.path == "x" ? m.to_x : m.to_y) = v;
        history_.execute(std::make_unique<level::MoveObjects>(*level_, std::vector<level::MoveObjects::Move>{m},
                                                              "Передвинуть: " + name));
        history_.seal();
        return;
    }
    const std::string before = level::component_json(*level_, e, ref.type);
    if (before.empty()) return;
    std::vector<std::max_align_t> scratch((ref.type->size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) + 1);
    void* object = scratch.data();
    ref.type->construct(object);
    data::LoadReport report;
    data::from_json(ref.type, object, before, report);
    std::string error;
    const bool ok = editor::set_field_text(ref.type, object, ref.path, text, &error);
    const std::string after = ok ? data::to_json(ref.type, object, false) : std::string();
    ref.type->destruct(object);
    if (!ok) {
        FORGE_WARN("%s: %s", m_fields_[static_cast<usize>(i)].label.c_str(), error.c_str());
        fields_built_ = 0;
        return;
    }
    if (after == before) return;
    history_.execute(std::make_unique<level::SetObjectComponent>(*level_, selection_[0], p.tile_x(), p.tile_y(), ref.type,
                                                                 before, after, ref.path,
                                                                 "«" + name + "»: " + m_fields_[static_cast<usize>(i)].label));
    if (!dragging) history_.seal();
}

// --- actions -----------------------------------------------------------------

void LevelEditor::undo() {
    if (stroke_ || moving_) return;
    if (history_.undo()) FORGE_INFO("Отменено");
}

void LevelEditor::redo() {
    if (stroke_ || moving_) return;
    if (history_.redo()) FORGE_INFO("Повторено");
}

void LevelEditor::save() {
    if (stroke_) release();
    const level::Level::SaveReport r = level_->save();
    if (!r.ok) {
        FORGE_ERROR("Уровень не сохранился в %s", path_to_utf8(level_->folder()).c_str());
        return;
    }
    history_.mark_saved();
    FORGE_INFO("Уровень сохранён: участков с плитками %u, с объектами %u (%.0f мс)", r.tile_chunks, r.object_chunks, r.ms);
}

std::vector<std::string> LevelEditor::play_command(f64 x, f64 y) const {
    char at[64];
    std::snprintf(at, sizeof(at), "%.2f,%.2f", x, y);
    return {path_to_utf8(config_.game_exe), "--play", "--level", path_to_utf8(level_->folder()), "--at", at,
            "--user", path_to_utf8(fs::temp_directory_path() / "forge_editor_play")};
}

bool LevelEditor::play_here() {
    if (stroke_) release();
    save();
    f64 x = 0, y = 0;
    if (!module_.play_spot(*level_, camera_.x, camera_.y, x, y)) {
        FORGE_WARN("Рядом с центром вида нет места для героя: сдвиньте вид");
        return false;
    }
    if (config_.game_exe.empty() || config_.offscreen) return true;
    std::error_code ec;
    if (!fs::exists(config_.game_exe, ec)) {
        FORGE_ERROR("Игра не найдена: %s (соберите forge_slice)", path_to_utf8(config_.game_exe).c_str());
        return false;
    }
    const std::vector<std::string> args = play_command(x, y);
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    if (game_) SDL_DestroyProcess(game_);
    game_ = SDL_CreateProcess(argv.data(), false);
    if (!game_) {
        FORGE_ERROR("Игра не запустилась: %s", SDL_GetError());
        return false;
    }
    FORGE_INFO("Игра запущена с точки %.0f, %.0f", x, y);
    return true;
}

void LevelEditor::reset_layout() { dock_.reset(); }

std::string LevelEditor::status() const {
    char text[200];
    const std::string place = module_.place(camera_.x, camera_.y);
    if (hover_)
        std::snprintf(text, sizeof(text), "Клетка %d, %d%s%s · %s", hover_x_, hover_y_, place.empty() ? "" : " · ",
                      place.c_str(), tool_name(tool_));
    else
        std::snprintf(text, sizeof(text), "Центр %.0f, %.0f%s%s · %s", camera_.x, camera_.y, place.empty() ? "" : " · ",
                      place.c_str(), tool_name(tool_));
    return text;
}

// --- frame -------------------------------------------------------------------

void LevelEditor::update(f64 dt, Rml::Context* context) {
    time_ += dt;
    view_shown_ = dock_.update(context);
    vx_ = dock_.view_x();
    vy_ = dock_.view_y();
    vw_ = view_shown_ ? dock_.view_w() : 0;
    vh_ = view_shown_ ? dock_.view_h() : 0;
    if (view_shown_ && vw_ > 0 && vh_ > 0) {
        const world::Rect focus = camera_.visible_tiles(static_cast<u32>(vw_), static_cast<u32>(vh_));
        level_->update({&focus, 1});
        update_minimap();
    }
    if (module_.objects_version() != objects_version_) {
        // Templates changed in the library: the palette, and the copies here.
        build_objects();
        if (objects::Library* lib = module_.library()) {
            if (const u32 n = lib->refresh(level_->scene())) FORGE_INFO("Шаблоны изменились: обновлено копий на уровне: %u", n);
        }
        fields_built_ = 0;
    }
    rebuild_fields(context);
    sync_model();
}

void LevelEditor::update_minimap() {
    // A few times a second, and only when something changed.
    const world::WorldStats ws = level_->world().stats();
    const u32 resident = ws.resident - ws.loading; // ready chunks
    const bool changed = level_->edits() != map_edits_ || resident != map_resident_ || std::fabs(camera_.x - map_cx_) >= kMapTilesPerPx ||
                         std::fabs(camera_.y - map_cy_) >= kMapTilesPerPx;
    if (!changed || time_ - map_time_ < 0.25) return;
    map_time_ = time_;
    map_edits_ = level_->edits();
    map_resident_ = resident;
    map_cx_ = std::floor(camera_.x);
    map_cy_ = std::floor(camera_.y);
    const world::World& w = level_->world();
    const u32 layers = static_cast<u32>(module_.layer_names().size());
    const Color bg = module_.background();
    const u8 sky[3] = {static_cast<u8>(bg.r * 255), static_cast<u8>(bg.g * 255), static_cast<u8>(bg.b * 255)};
    const i32 half = static_cast<i32>(kMapPx / 2) * kMapTilesPerPx;
    const i32 x0 = static_cast<i32>(map_cx_) - half, y0 = static_cast<i32>(map_cy_) - half;
    const world::Rect view = camera_.visible_tiles(static_cast<u32>(vw_), static_cast<u32>(vh_));
    for (u32 py = 0; py < kMapPx; ++py) {
        const i32 ty = y0 + static_cast<i32>(py) * kMapTilesPerPx;
        for (u32 px = 0; px < kMapPx; ++px) {
            const i32 tx = x0 + static_cast<i32>(px) * kMapTilesPerPx;
            u8* p = &map_rgba_[(static_cast<usize>(py) * kMapPx + px) * 4];
            u32 c = 0;
            const bool loaded = level_->loaded(tx, ty);
            if (loaded)
                for (u32 l = layers; l-- > 0 && c == 0;) c = module_.map_color(l, w.tile(l, tx, ty));
            if (c) {
                p[0] = static_cast<u8>(c), p[1] = static_cast<u8>(c >> 8), p[2] = static_cast<u8>(c >> 16);
            } else if (loaded) {
                p[0] = sky[0], p[1] = sky[1], p[2] = sky[2];
            } else {
                p[0] = p[1] = p[2] = 40; // not loaded yet
            }
            p[3] = 255;
            // The view's frame.
            const bool edge_x = tx >= view.x0 - 1 && tx <= view.x1 && (std::abs(ty - view.y0) < kMapTilesPerPx || std::abs(ty - view.y1) < kMapTilesPerPx);
            const bool edge_y = ty >= view.y0 - 1 && ty <= view.y1 && (std::abs(tx - view.x0) < kMapTilesPerPx || std::abs(tx - view.x1) < kMapTilesPerPx);
            if (edge_x || edge_y) p[0] = p[1] = p[2] = 255;
        }
    }
    const std::string old = map_name_;
    map_name_ = "minimap_" + std::to_string(++map_serial_);
    ui_->set_image(map_name_, map_rgba_.data(), kMapPx, kMapPx);
    m_minimap_ = "/memory/" + map_name_;
    model_.DirtyVariable("lv_minimap");
    // The old picture goes after the element shows the new one.
    if (!old.empty()) ui_->drop_image(old);
    ++minimap_updates_;
}

void LevelEditor::sync_model() {
    const ModeInfo& mode = mode_info(mode_);
    set(m_mode_, Rml::String(mode.id), "lv_mode");
    set(m_mode_name_, Rml::String(mode.name), "lv_mode_name");
    set(m_mode_help_, Rml::String(mode.help), "lv_mode_help");
    set(m_mode_when_, Rml::String(mode.when), "lv_mode_when");
    set(m_tool_, Rml::String(tool_id(tool_)), "lv_tool");
    set(m_tool_name_, Rml::String(tool_name(tool_)), "lv_tool_name");
    set(m_tool_help_, Rml::String(tool_help(tool_)), "lv_tool_help");
    set(m_tile_, static_cast<int>(tile_), "lv_tile");
    set(m_layer_, static_cast<int>(layer_), "lv_layer");
    set(m_radius_, radius_ + 1, "lv_radius");
    const auto& tiles = module_.tiles();
    if (tile_ < tiles.size()) {
        const level::TileDef& t = tiles[tile_];
        set(m_tile_name_, Rml::String(t.name), "lv_tile_name");
        set(m_tile_hint_, Rml::String(t.hint), "lv_tile_hint");
        set(m_tile_icon_, Rml::String("/memory/tile_" + t.id), "lv_tile_icon");
        const auto& layers = module_.layer_names();
        set(m_tile_layer_, Rml::String(t.layer < layers.size() ? layers[t.layer] : std::string()), "lv_tile_layer");
    }
    set(m_object_, static_cast<int>(object_), "lv_object");
    set(m_sel_count_, static_cast<int>(selection_.size()), "lv_sel_count");
    set(m_light_, view_.game_light, "lv_light");
    set(m_grid_, view_.grid, "lv_grid");
    set(m_chunks_, view_.chunks, "lv_chunks");
    set(m_outline_, rect_outline_, "lv_outline");
    set(m_dirty_, history_.dirty(), "lv_dirty");
    if (time_ - info_time_ > 0.25) {
        info_time_ = time_;
        char coords[96];
        std::snprintf(coords, sizeof(coords), "%.0f, %.0f", camera_.x, camera_.y);
        const std::string place = module_.place(camera_.x, camera_.y);
        set(m_place_, Rml::String(place.empty() ? coords : place + " · " + coords), "lv_place");
        char info[160];
        std::snprintf(info, sizeof(info), "Изменений: %s · участков загружено: %u",
                      group_digits(level_->edits()).c_str(), level_->world().stats().resident);
        set(m_info_, Rml::String(info), "lv_info");
    }
    if (history_.version() != history_version_) {
        history_version_ = history_.version();
        m_history_.clear();
        for (usize i = 0; i < history_.size(); ++i) m_history_.push_back(history_.label_at(i));
        m_history_cursor_ = static_cast<int>(history_.cursor());
        model_.DirtyVariable("lv_history");
        model_.DirtyVariable("lv_history_cursor");
    }
}

// --- drawing -----------------------------------------------------------------

void LevelEditor::push_overlay(f64 ox, f64 oy) {
    const u32 white = render::pack_color(255, 255, 255, 255);
    auto quad = [&](f64 x, f64 y, f64 w, f64 h, u32 color, u32 order) {
        render::Sprite s;
        s.x = static_cast<f32>(x + w * 0.5 - ox);
        s.y = static_cast<f32>(y + h * 0.5 - oy);
        s.w = static_cast<f32>(w);
        s.h = static_cast<f32>(h);
        s.frame = demo::kFrameSolid;
        s.color = color;
        s.order = order;
        front_batch_.push(s);
    };
    (void)white;
    const world::Rect view = camera_.visible_tiles(static_cast<u32>(vw_), static_cast<u32>(vh_));
    const f64 px = 1.0 / camera_.zoom;
    if (view_.grid && camera_.zoom >= 6) {
        const u32 c = render::pack_color(255, 255, 255, 40);
        for (i32 x = view.x0; x <= view.x1; ++x) quad(x - px * 0.5, view.y0, px, view.y1 - view.y0, c, 1);
        for (i32 y = view.y0; y <= view.y1; ++y) quad(view.x0, y - px * 0.5, view.x1 - view.x0, px, c, 1);
    }
    if (view_.chunks) {
        const u32 c = render::pack_color(255, 200, 80, 170);
        const i32 n = static_cast<i32>(world::kChunkSize);
        auto first = [n](i32 v) { return static_cast<i32>(std::floor(static_cast<f64>(v) / n)) * n; };
        for (i32 x = first(view.x0); x <= view.x1; x += n) quad(x - px, view.y0, px * 2, view.y1 - view.y0, c, 2);
        for (i32 y = first(view.y0); y <= view.y1; y += n) quad(view.x0, y - px, view.x1 - view.x0, px * 2, c, 2);
    }
    // What the tool would paint.
    std::vector<level::Cell> cells;
    if (shaping_) {
        shape_cells(cells);
    } else if (hover_ && !panning_ && mode_ == Mode::Tiles) {
        if (tool_ == Tool::Brush || tool_ == Tool::Eraser) level::disc_cells(hover_x_, hover_y_, radius_, cells);
        else cells.push_back({hover_x_, hover_y_});
    }
    const u32 fill = tool_ == Tool::Eraser ? render::pack_color(255, 90, 80, 90) : render::pack_color(255, 255, 255, 70);
    for (const level::Cell& c : cells) quad(c.x, c.y, 1, 1, fill, 3);
    // Objects: the selected ones framed, the one under the mouse lightly.
    if (objects_mode()) {
        auto frame = [&](flecs::entity e, u32 c, f64 t) {
            f64 x0, y0, x1, y1;
            if (!e.is_valid() || !module_.object_box(e, x0, y0, x1, y1)) return;
            quad(x0, y0, x1 - x0, t, c, 5);
            quad(x0, y1 - t, x1 - x0, t, c, 5);
            quad(x0, y0, t, y1 - y0, c, 5);
            quad(x1 - t, y0, t, y1 - y0, c, 5);
        };
        for (u64 id : selection_) frame(level_->find(id), render::pack_color(255, 210, 90, 255), px * 2);
        if (hover_ && !panning_ && !moving_ && object_ < 0) {
            f64 tx, ty;
            to_tile(mouse_x_, mouse_y_, tx, ty);
            frame(level_->pick(tx, ty), render::pack_color(255, 255, 255, 160), px);
        }
        if (hover_ && object_ >= 0) {
            // Where the armed object would stand.
            quad(hover_x_, hover_y_ + 1 - px * 2, 1, px * 2, render::pack_color(255, 210, 90, 255), 5);
            quad(hover_x_ + 0.5 - px, hover_y_, px * 2, 1, render::pack_color(255, 210, 90, 160), 5);
        }
    }
    if (hover_ && !panning_ && mode_ == Mode::Tiles) {
        // An outline around the cell under the mouse.
        const u32 c = render::pack_color(255, 255, 255, 220);
        const f64 t = px * 2;
        quad(hover_x_, hover_y_, 1, t, c, 4);
        quad(hover_x_, hover_y_ + 1 - t, 1, t, c, 4);
        quad(hover_x_, hover_y_, t, 1, c, 4);
        quad(hover_x_ + 1 - t, hover_y_, t, 1, c, 4);
    }
}

void LevelEditor::prepare(SDL_GPUCommandBuffer* cmd) {
    if (!view_ready_ || !view_shown_ || vw_ < 1 || vh_ < 1) return;
    const u32 w = static_cast<u32>(vw_), h = static_cast<u32>(vh_);
    module_.prepare_view(cmd, *level_, camera_, w, h, view_, time_);

    back_batch_.begin(camera_.snapped_x(), camera_.snapped_y(), 4);
    const Color bg = module_.background();
    auto byte = [](f32 v) { return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    render::Sprite sky;
    sky.w = vw_ / camera_.zoom + 4;
    sky.h = vh_ / camera_.zoom + 4;
    sky.x = static_cast<f32>(camera_.x - back_batch_.origin_x());
    sky.y = static_cast<f32>(camera_.y - back_batch_.origin_y());
    sky.frame = demo::kFrameSolid;
    sky.color = render::pack_color(byte(bg.r), byte(bg.g), byte(bg.b), 255);
    back_batch_.push(sky);
    back_.prepare(cmd, back_batch_, camera_, w, h, false);

    front_batch_.begin(camera_.snapped_x(), camera_.snapped_y(), front_.max_sprites());
    push_overlay(front_batch_.origin_x(), front_batch_.origin_y());
    front_.prepare(cmd, front_batch_, camera_, w, h, true);
}

void LevelEditor::draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
    if (!view_ready_ || !view_shown_ || vw_ < 1 || vh_ < 1) return;
    const SDL_GPUViewport viewport{vx_, vy_, std::floor(vw_), std::floor(vh_), 0, 1};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_Rect scissor{static_cast<int>(vx_), static_cast<int>(vy_), static_cast<int>(vw_), static_cast<int>(vh_)};
    SDL_SetGPUScissor(pass, &scissor);
    back_.draw(cmd, pass);
    module_.draw_view(cmd, pass);
    front_.draw(cmd, pass);
}

// --- input -------------------------------------------------------------------

bool LevelEditor::over_view(f32 x, f32 y, Rml::Context* context) const {
    if (!view_shown_ || x < vx_ || y < vy_ || x >= vx_ + vw_ || y >= vy_ + vh_) return false;
    // Labels drawn over the world still belong to the world.
    for (const Rml::Element* e = context ? context->GetHoverElement() : nullptr; e; e = e->GetParentNode())
        if (e->GetId() == "level-view") return true;
    return false;
}

void LevelEditor::to_tile(f32 x, f32 y, f64& tx, f64& ty) const {
    camera_.screen_to_tile(x - vx_, y - vy_, static_cast<u32>(vw_), static_cast<u32>(vh_), tx, ty);
}

void LevelEditor::cell_at(f32 x, f32 y, i32& cx, i32& cy) const {
    f64 tx, ty;
    to_tile(x, y, tx, ty);
    cx = static_cast<i32>(std::floor(tx));
    cy = static_cast<i32>(std::floor(ty));
}

bool LevelEditor::screen_of(f64 tx, f64 ty, f32& x, f32& y) const {
    x = vx_ + static_cast<f32>((tx - camera_.snapped_x()) * camera_.zoom) + std::floor(vw_) * 0.5f;
    y = vy_ + static_cast<f32>((ty - camera_.snapped_y()) * camera_.zoom) + std::floor(vh_) * 0.5f;
    return x >= vx_ && y >= vy_ && x < vx_ + vw_ && y < vy_ + vh_;
}

bool LevelEditor::handle_event(const SDL_Event& e, f32 density, bool ui_used, Rml::Context* context) {
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const f32 x = e.motion.x * density, y = e.motion.y * density;
        if (panning_) {
            camera_.x -= (x - mouse_x_) / camera_.zoom;
            camera_.y -= (y - mouse_y_) / camera_.zoom;
        }
        mouse_x_ = x;
        mouse_y_ = y;
        if (dock_.mouse_move(x, y)) return true;
        hover_ = over_view(x, y, context) || stroke_ != nullptr || moving_;
        if (hover_) cell_at(x, y, hover_x_, hover_y_);
        if (stroke_) drag(x, y);
        if (moving_) drag_objects(x, y);
        return hover_;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const f32 x = e.button.x * density, y = e.button.y * density;
        mouse_x_ = x;
        mouse_y_ = y;
        if (dock_.busy()) return true;
        if (!over_view(x, y, context)) return false;
        if (e.button.button == SDL_BUTTON_LEFT && !panning_ && mode_ == Mode::Tiles) {
            history_.seal();
            press(x, y);
        } else if (e.button.button == SDL_BUTTON_LEFT && !panning_ && objects_mode()) {
            press_objects(x, y, (SDL_GetModState() & SDL_KMOD_CTRL) != 0);
        } else if (e.button.button == SDL_BUTTON_RIGHT && object_ >= 0) {
            object_ = -1; // right click puts the armed object away
        } else if (!panning_) {
            panning_ = true;
            pan_button_ = e.button.button;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        bool used = dock_.mouse_up();
        if (e.button.button == SDL_BUTTON_LEFT && stroke_) {
            release();
            used = true;
        }
        if (e.button.button == SDL_BUTTON_LEFT && moving_) {
            moving_ = false;
            history_.seal();
            used = true;
        }
        if (panning_ && e.button.button == pan_button_) {
            panning_ = false;
            used = true;
        }
        return used;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!over_view(mouse_x_, mouse_y_, context)) return false;
        f64 bx, by, ax, ay;
        to_tile(mouse_x_, mouse_y_, bx, by);
        camera_.zoom = std::clamp(camera_.zoom * std::pow(1.15f, e.wheel.y), 1.0f, 96.0f);
        to_tile(mouse_x_, mouse_y_, ax, ay);
        camera_.x += bx - ax;
        camera_.y += by - ay;
        return true;
    }
    default: break;
    }
    (void)ui_used;
    return false;
}

bool LevelEditor::handle_key(const SDL_KeyboardEvent& k) {
    const bool ctrl = (k.mod & SDL_KMOD_CTRL) != 0;
    if (ctrl) return false;
    if (k.key == SDLK_F5) {
        play_here();
        return true;
    }
    if (k.key == SDLK_Q) { set_mode(Mode::Select); return true; }
    if (k.key == SDLK_T) { set_mode(Mode::Tiles); return true; }
    if (k.key == SDLK_O) { set_mode(Mode::Objects); return true; }
    if (objects_mode()) {
        if (k.key == SDLK_DELETE || k.key == SDLK_BACKSPACE) {
            delete_selection();
            return true;
        }
        if (k.key == SDLK_ESCAPE) {
            if (object_ >= 0) object_ = -1;
            else select_objects({});
            return true;
        }
        return false;
    }
    if (mode_ != Mode::Tiles) return false;
    switch (k.key) {
    case SDLK_B: set_tool(Tool::Brush); return true;
    case SDLK_L: set_tool(Tool::Line); return true;
    case SDLK_R: set_tool(Tool::Rect); return true;
    case SDLK_G: set_tool(Tool::Fill); return true;
    case SDLK_E: set_tool(Tool::Eraser); return true;
    case SDLK_I: set_tool(Tool::Picker); return true;
    case SDLK_LEFTBRACKET: set_brush_radius(radius_ - 1); return true;
    case SDLK_RIGHTBRACKET: set_brush_radius(radius_ + 1); return true;
    default: break;
    }
    // Palette shortcuts: "1".."9".
    if (k.key >= SDLK_1 && k.key <= SDLK_9) {
        const std::string key(1, static_cast<char>('1' + (k.key - SDLK_1)));
        const auto& tiles = module_.tiles();
        for (usize i = 0; i < tiles.size(); ++i)
            if (tiles[i].key == key) {
                select_tile(i);
                if (tool_ == Tool::Eraser || tool_ == Tool::Picker) set_tool(Tool::Brush);
                return true;
            }
    }
    return false;
}

} // namespace forge::editor_app
