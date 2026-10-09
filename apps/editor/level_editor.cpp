#include "level_editor.h"

#include "forge/audio/audio.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/data/json.h"
#include "forge/editor/inspector.h"
#include "forge/render/sprite_batch.h"
#include "forge/sim/gravity.h"

#include "object_library.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

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
// The palette's group of the level's own tiles (forge/level/own_tiles.h).
constexpr const char* kOwnTilesGroup = "Тайлы уровня";

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
    {Mode::Physics, "physics", "Физика",
     "Гравитация мира, точки гравитации, вода и песок. «Проба» пускает воду и песок прямо здесь, «Сбросить» возвращает уровень.", ""},
    {Mode::Light, "light", "Свет",
     "Источники света: цвет, яркость и радиус, свет кончается ровно на окружности. Время суток уровня. Вид — как в игре.", ""},
    {Mode::Zones, "zones", "Зоны",
     "Именованные области уровня («Шахта», «Деревня»), их музыка и точка появления героя. Вход героя в зону — событие "
     "«Логики»: «Герой входит в Шахту».",
     ""},
};

struct PhysToolInfo {
    PhysTool tool;
    const char* id;
    const char* name;
    const char* help;
};
const PhysToolInfo kPhysTools[] = {
    {PhysTool::Select, "select", "Выбор точки",
     "Щёлкните по центру точки, чтобы выбрать её. Тяните центр, чтобы передвинуть; окружность выбранной точки — чтобы изменить "
     "радиус. Пустое место двигает вид, Delete удаляет точку."},
    {PhysTool::Point, "point", "Точка гравитации",
     "Нажмите там, где будет центр, и тяните до нужного радиуса; простой щелчок даёт радиус 8. Esc или правая кнопка во время "
     "перетаскивания отменяют: точки не будет."},
    {PhysTool::Water, "water", "Вода",
     "Протяните прямоугольник: вода нальётся только в пустые клетки (без блока и без жидкости), занятые останутся как были."},
    {PhysTool::Sand, "sand", "Песок",
     "Протяните прямоугольник: песок ляжет только в пустые клетки (без блока и без жидкости), занятые останутся как были."},
};
const PhysToolInfo& phys_info(PhysTool t) {
    for (const PhysToolInfo& i : kPhysTools)
        if (i.tool == t) return i;
    return kPhysTools[0];
}
struct LightToolInfo {
    LightTool tool;
    const char* id;
    const char* name;
    const char* help;
};
const LightToolInfo kLightTools[] = {
    {LightTool::Select, "select", "Выбор источника",
     "Щёлкните по центру источника, чтобы выбрать его. Тяните центр, чтобы передвинуть; окружность выбранного — чтобы "
     "изменить радиус. Пустое место двигает вид, Delete удаляет источник."},
    {LightTool::Source, "source", "Источник света",
     "Нажмите там, где будет центр, и тяните до нужного радиуса; простой щелчок даёт радиус 8. Свет кончается ровно на "
     "окружности. Esc или правая кнопка во время перетаскивания отменяют: источника не будет."},
};
const LightToolInfo& light_info(LightTool t) {
    for (const LightToolInfo& i : kLightTools)
        if (i.tool == t) return i;
    return kLightTools[0];
}
struct AreaToolInfo {
    AreaTool tool;
    const char* id;
    const char* name;
    const char* help;
};
const AreaToolInfo kAreaTools[] = {
    {AreaTool::Select, "select", "Выбор зоны",
     "Щёлкните зону или точку появления, чтобы выбрать. Тяните зону, чтобы передвинуть; край или угол выбранной — чтобы "
     "изменить границы. Пустое место двигает вид, Delete удаляет."},
    {AreaTool::Draw, "draw", "Новая зона",
     "Протяните прямоугольник от угла до угла в любую сторону: клетки обоих углов войдут в зону. Esc или правая кнопка во "
     "время протяжки отменяют: зоны не будет."},
    {AreaTool::Spawn, "spawn", "Точка появления",
     "Щёлкните клетку, где новая игра поставит героя (ноги на нижнем краю клетки), и тяните, чтобы уточнить. Точка одна на "
     "уровень; «Играть отсюда» важнее её."},
};
const AreaToolInfo& area_info(AreaTool t) {
    for (const AreaToolInfo& i : kAreaTools)
        if (i.tool == t) return i;
    return kAreaTools[0];
}
// The colours areas are drawn in (by id, so an area keeps its colour).
const u8 kAreaHues[][3] = {{90, 200, 255}, {130, 230, 120}, {255, 150, 90}, {220, 130, 255}, {255, 220, 90}, {90, 230, 210}};

const char* const kLightName = "Источник света";
constexpr f64 kLightNewRadius = 8;
// The hours the time slider goes through (a quarter of an hour a step).
constexpr f32 kLastHour = 23.75f;

// A colour as the panel shows it: #RRGGBB.
std::string hex_color(f32 r, f32 g, f32 b) {
    auto byte = [](f32 v) { return static_cast<int>(std::lround(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f) * 255)); };
    char text[16];
    std::snprintf(text, sizeof(text), "#%02X%02X%02X", byte(r), byte(g), byte(b));
    return text;
}
// "#RRGGBB" (or without #, any case) as 0..1 each; false otherwise.
bool parse_hex(const std::string& text, f32& r, f32& g, f32& b) {
    std::string t = text;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    if (!t.empty() && t[0] == '#') t.erase(t.begin());
    if (t.size() != 6) return false;
    int v[3];
    for (int i = 0; i < 3; ++i) {
        int n = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = t[static_cast<usize>(i * 2 + k)];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            n = n * 16 + d;
        }
        v[i] = n;
    }
    r = static_cast<f32>(v[0]) / 255.0f;
    g = static_cast<f32>(v[1]) / 255.0f;
    b = static_cast<f32>(v[2]) / 255.0f;
    return true;
}

const char* const kPullNames[] = {"вниз", "влево", "вверх", "вправо", "нет"};
// A gravity point: what the panel lets the author set.
constexpr f64 kMinRadius = 1, kMaxRadius = 128, kNewRadius = 8;
constexpr f64 kMaxStrength = level::kMaxGravity;
const char* const kPointName = "Точка гравитации";

// Numbers as the panel shows them: no trailing zeros.
std::string number(f64 v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", v);
    return text;
}

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
const char* phys_tool_id(PhysTool t) { return phys_info(t).id; }
const char* light_tool_id(LightTool t) { return light_info(t).id; }
const char* area_tool_id(AreaTool t) { return area_info(t).id; }

void LevelEditor::set_mode(Mode m) {
    if (gesture()) return;
    // Gravity points are picked only in «Физика», light sources only in
    // «Свет», objects only outside them.
    auto picks = [](Mode x) { return x == Mode::Physics ? 1 : x == Mode::Light ? 2 : x == Mode::Zones ? 3 : 0; };
    if (picks(m) != picks(mode_)) {
        reset_trial();
        select_objects({});
    }
    if (m == Mode::Zones && mode_ != Mode::Zones) scan_sounds();
    if (m != Mode::Zones) zn_ask_ = 0;
    if (m == Mode::Light && mode_ != Mode::Light && !view_.game_light) {
        view_.game_light = true; // what the light does is seen only as in the game
        FORGE_INFO("Вид: свет как в игре");
    }
    if (m != Mode::Light) view_.preview_time = -1; // the preview is the mode's
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
    look_around_level();
    camera_.zoom = 16;
    history_.clear();

    if (!module_.init_view(device, format)) return false;
    art_ = demo::make_sprite_sheet();
    if (!back_.init(device, format, art_.sheet(), 16)) return false;
    if (!front_.init(device, format, art_.sheet(), 1u << 15)) return false;
    view_ready_ = true;

    build_tiles();
    for (const std::string& name : module_.layer_names()) m_layers_.push_back(name);
    m_ph_can_trial_ = !module_.physics_fills().empty();
    build_objects();
    if (!tiles_.empty()) select_tile(0);

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

void LevelEditor::build_tiles() {
    own_tiles_version_ = level_->own_tiles_version();
    // The tile in hand stays in hand if it is still there.
    const std::string held = tile_ < tiles_.size() ? tiles_[tile_].id : std::string();
    const std::vector<std::string> old_images = own_images_;
    own_images_.clear();
    tiles_ = module_.tiles();
    // The level's own tiles after the game's: ids "own_256"..., their pictures
    // named anew each time (a picture may change under the same id).
    const level::LevelTiles& own = level_->own_tiles();
    const std::string serial = std::to_string(++own_serial_);
    own_colors_.clear();
    std::vector<std::string> images(tiles_.size());
    std::vector<u8> icon;
    for (usize i = 0; i < own.tiles.size(); ++i) {
        const level::OwnTile& o = own.tiles[i];
        std::string hint = o.solid ? "твёрдый; " : "";
        hint += o.from.empty() ? "тайл этого уровня" : o.from;
        tiles_.push_back({"own_" + std::to_string(o.id), o.name, kOwnTilesGroup, hint, o.layer, o.id, ""});
        // The icon: the picture, each pixel the nearest; the minimap: its average.
        icon.assign(static_cast<usize>(kIconPx) * kIconPx * 4, 0);
        const u8* pic = own.picture(i);
        for (u32 y = 0; y < kIconPx; ++y)
            for (u32 x = 0; x < kIconPx; ++x)
                std::memcpy(&icon[(static_cast<usize>(y) * kIconPx + x) * 4],
                            pic + (static_cast<usize>(y * own.px / kIconPx) * own.px + x * own.px / kIconPx) * 4, 4);
        u64 sum[4] = {};
        for (u32 k = 0; k < own.px * own.px; ++k)
            for (u32 c = 0; c < 4; ++c) sum[c] += pic[static_cast<usize>(k) * 4 + c];
        const u64 n = static_cast<u64>(own.px) * own.px;
        const usize slot = o.id - level::kFirstOwnTile;
        if (own_colors_.size() <= slot) own_colors_.resize(slot + 1, 0);
        // Mostly see-through: the layer below shows on the minimap.
        if (sum[3] >= n * 128)
            own_colors_[slot] = render::pack_color(static_cast<u8>(sum[0] / n), static_cast<u8>(sum[1] / n), static_cast<u8>(sum[2] / n), 255);
        const std::string image = "tile_own" + serial + "_" + std::to_string(o.id);
        ui_->set_image(image, icon.data(), kIconPx, kIconPx);
        own_images_.push_back(image);
        images.push_back(image);
    }
    // Palette: icons as pictures the documents can show, grouped.
    m_palette_.clear();
    for (usize i = 0; i < tiles_.size(); ++i) {
        const level::TileDef& t = tiles_[i];
        if (images[i].empty()) {
            images[i] = "tile_" + t.id;
            module_.tile_icon(t, kIconPx, icon);
            ui_->set_image(images[i], icon.data(), kIconPx, kIconPx);
        }
        auto g = std::find_if(m_palette_.begin(), m_palette_.end(), [&](const PaletteGroup& pg) { return pg.name == t.group; });
        if (g == m_palette_.end()) {
            m_palette_.push_back({t.group, {}});
            g = m_palette_.end() - 1;
        }
        g->tiles.push_back({static_cast<int>(i), t.name, "/memory/" + images[i], t.key, static_cast<int>(t.layer)});
    }
    tile_images_ = std::move(images);
    if (model_) model_.DirtyVariable("lv_palette");
    tile_ = 0;
    for (usize i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].id == held) tile_ = i;
    if (tile_ < tiles_.size()) layer_ = tiles_[tile_].layer;
    map_edits_ = ~0ull; // the minimap shows them too
    // The old pictures go after the palette shows the new ones.
    for (const std::string& image : old_images) ui_->drop_image(image);
}

void LevelEditor::look_around_level() {
    // Nothing around the level: the game's start is nothing to look at; its
    // spawn point is, else the corner where a map put into it begins.
    if (!level_->around().empty_around) return;
    const level::LevelAreas& a = level_->areas();
    camera_.x = a.spawn ? a.spawn_x : 0;
    camera_.y = a.spawn ? a.spawn_y - 2 : 0;
}

u32 LevelEditor::own_color(world::TileId value) const {
    const usize slot = value - level::kFirstOwnTile;
    return value >= level::kFirstOwnTile && slot < own_colors_.size() ? own_colors_[slot] : 0;
}

std::string LevelEditor::place_name(f64 x, f64 y) const {
    // The game's places are where its generator made them.
    return level_->around().empty_around ? std::string() : module_.place(x, y);
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
        // A changed template may look different: a new picture name.
        const objects::Template* t = module_.library() ? module_.library()->find(o.key) : nullptr;
        const std::string image = "obj_" + o.id + "_" + std::to_string(t ? t->look() : 0);
        ui_->set_image(image, icon.data(), kIconPx, kIconPx);
        auto g = std::find_if(m_objects_.begin(), m_objects_.end(), [&](const PaletteGroup& pg) { return pg.name == o.group; });
        if (g == m_objects_.end()) {
            m_objects_.push_back({o.group, {}});
            g = m_objects_.end() - 1;
        }
        g->tiles.push_back({static_cast<int>(i), o.name, "/memory/" + image, "", 0});
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
    reset_trial();
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
    if (auto s = model.RegisterStruct<SoundRow>()) {
        s.RegisterMember("value", &SoundRow::value);
        s.RegisterMember("name", &SoundRow::name);
    }
    model.RegisterArray<std::vector<SoundRow>>();
    if (auto s = model.RegisterStruct<AreaRow>()) {
        s.RegisterMember("id", &AreaRow::id);
        s.RegisterMember("name", &AreaRow::name);
        s.RegisterMember("about", &AreaRow::about);
        s.RegisterMember("selected", &AreaRow::selected);
    }
    model.RegisterArray<std::vector<AreaRow>>();
    if (auto s = model.RegisterStruct<AreaLabel>()) {
        s.RegisterMember("id", &AreaLabel::id);
        s.RegisterMember("name", &AreaLabel::name);
        s.RegisterMember("x", &AreaLabel::x);
        s.RegisterMember("y", &AreaLabel::y);
        s.RegisterMember("selected", &AreaLabel::selected);
        s.RegisterMember("spawn", &AreaLabel::spawn);
    }
    model.RegisterArray<std::vector<AreaLabel>>();
    model.RegisterArray<std::vector<Rml::String>>();
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
    model.Bind("lv_ph_tool", &m_ph_tool_);
    model.Bind("lv_ph_tool_name", &m_ph_tool_name_);
    model.Bind("lv_ph_tool_help", &m_ph_tool_help_);
    model.Bind("lv_ph_dir", &m_ph_dir_);
    model.Bind("lv_ph_strength", &m_ph_strength_);
    model.Bind("lv_ph_world", &m_ph_world_);
    model.Bind("lv_ph_error", &m_ph_error_);
    model.Bind("lv_ph_trial", &m_ph_trial_);
    model.Bind("lv_ph_trial_text", &m_ph_trial_text_);
    model.Bind("lv_ph_can_trial", &m_ph_can_trial_);
    model.Bind("lv_ph_note", &m_ph_note_);
    model.Bind("lv_lt_tool", &m_lt_tool_);
    model.Bind("lv_lt_tool_name", &m_lt_tool_name_);
    model.Bind("lv_lt_tool_help", &m_lt_tool_help_);
    model.Bind("lv_lt_time", &m_lt_time_);
    model.Bind("lv_lt_hours", &m_lt_hours_);
    model.Bind("lv_lt_sky", &m_lt_sky_);
    model.Bind("lv_lt_error", &m_lt_error_);
    model.Bind("lv_lt_preview", &m_lt_preview_);
    model.Bind("lv_lt_preview_hours", &m_lt_preview_hours_);
    model.Bind("lv_lt_previewing", &m_lt_previewing_);
    model.Bind("lv_lt_note", &m_lt_note_);
    model.Bind("lv_zn_tool", &m_zn_tool_);
    model.Bind("lv_zn_tool_name", &m_zn_tool_name_);
    model.Bind("lv_zn_tool_help", &m_zn_tool_help_);
    model.Bind("lv_zn_has_area", &m_zn_has_area_);
    model.Bind("lv_zn_name", &m_zn_name_);
    model.Bind("lv_zn_bounds", &m_zn_bounds_);
    model.Bind("lv_zn_music_note", &m_zn_music_note_);
    model.Bind("lv_zn_links", &m_zn_links_);
    model.Bind("lv_zn_asking", &m_zn_asking_);
    model.Bind("lv_zn_ask_text", &m_zn_ask_text_);
    model.Bind("lv_zn_has_spawn", &m_zn_has_spawn_);
    model.Bind("lv_zn_spawn_sel", &m_zn_spawn_sel_);
    model.Bind("lv_zn_spawn", &m_zn_spawn_);
    model.Bind("lv_zn_spawn_note", &m_zn_spawn_note_);
    model.Bind("lv_zn_spawn_bad", &m_zn_spawn_bad_);
    model.Bind("lv_zn_error", &m_zn_error_);
    model.Bind("lv_zn_note", &m_zn_note_);
    model.Bind("lv_zn_areas", &m_zn_areas_);
    model.Bind("lv_zn_labels", &m_zn_labels_);
    model.Bind("lv_zn_sounds", &m_zn_sounds_);

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
    on("lv_ph_tool", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string id = arg_str(a, 0);
        set_mode(Mode::Physics);
        for (const PhysToolInfo& t : kPhysTools)
            if (id == t.id) set_phys_tool(t.tool);
    });
    on("lv_ph_dir", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int n = static_cast<int>(std::size(kPullNames));
        set_pull_dir(static_cast<PullDir>(((static_cast<int>(pull_dir()) + arg_int(a, 0, 1)) % n + n) % n));
    });
    on("lv_ph_strength_text", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (a.size() > 1 && a[1].Get<bool>()) set_pull_strength(arg_str(a, 0), false);
    });
    on("lv_ph_strength_commit", [this](Rml::Event& ev, const Rml::VariantList&) {
        Rml::Element* e = ev.GetTargetElement();
        if (e && e->GetTagName() == "input") set_pull_strength(static_cast<Rml::ElementFormControl*>(e)->GetValue(), false);
    });
    on("lv_ph_strength_slide", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { set_pull_strength(arg_str(a, 0), true); });
    on("lv_ph_trial", [this](Rml::Event&, const Rml::VariantList&) {
        if (trial_.running()) reset_trial();
        else start_trial();
    });
    on("lv_lt_tool", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string id = arg_str(a, 0);
        set_mode(Mode::Light);
        for (const LightToolInfo& t : kLightTools)
            if (id == t.id) set_light_tool(t.tool);
    });
    on("lv_lt_time_text", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (a.size() > 1 && a[1].Get<bool>()) set_level_time(arg_str(a, 0), false);
    });
    on("lv_lt_time_commit", [this](Rml::Event& ev, const Rml::VariantList&) {
        Rml::Element* e = ev.GetTargetElement();
        if (e && e->GetTagName() == "input") set_level_time(static_cast<Rml::ElementFormControl*>(e)->GetValue(), false);
    });
    on("lv_lt_time_slide", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { set_level_time(arg_str(a, 0), true); });
    on("lv_lt_preview_text", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (a.size() > 1 && a[1].Get<bool>()) set_preview_time(arg_str(a, 0));
    });
    on("lv_lt_preview_commit", [this](Rml::Event& ev, const Rml::VariantList&) {
        Rml::Element* e = ev.GetTargetElement();
        if (e && e->GetTagName() == "input") set_preview_time(static_cast<Rml::ElementFormControl*>(e)->GetValue());
    });
    on("lv_lt_preview_slide", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { set_preview_time(arg_str(a, 0)); });
    on("lv_lt_preview_off", [this](Rml::Event&, const Rml::VariantList&) { set_preview_time(""); });
    on("lv_zn_tool", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string id = arg_str(a, 0);
        set_mode(Mode::Zones);
        for (const AreaToolInfo& t : kAreaTools)
            if (id == t.id) set_area_tool(t.tool);
    });
    on("lv_zn_pick", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        // A row of the areas' list: selected, and the view goes to it.
        u64 id = 0;
        if (!level::parse_area_id(arg_str(a, 0), id) || gesture()) return;
        select_area(id);
        if (const level::Area* z = level_->areas().find(id)) {
            camera_.x = (z->x0 + z->x1) * 0.5;
            camera_.y = (z->y0 + z->y1) * 0.5;
        }
    });
    on("lv_zn_pick_spawn", [this](Rml::Event&, const Rml::VariantList&) {
        if (gesture() || !level_->areas().spawn) return;
        select_spawn();
        camera_.x = level_->areas().spawn_x;
        camera_.y = level_->areas().spawn_y - 1;
    });
    on("lv_zn_name_text", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        if (a.size() > 1 && a[1].Get<bool>() && !ui_updating_) set_area_name(arg_str(a, 0));
    });
    on("lv_zn_name_commit", [this](Rml::Event& ev, const Rml::VariantList&) {
        Rml::Element* e = ev.GetTargetElement();
        if (e && e->GetTagName() == "input" && !ui_updating_) set_area_name(static_cast<Rml::ElementFormControl*>(e)->GetValue());
    });
    on("lv_zn_music", [this](Rml::Event& ev, const Rml::VariantList&) {
        // Only the author's pick counts: the panel filling the drop-down fires
        // changes too, and those never have the focus.
        auto* input = rmlui_dynamic_cast<Rml::ElementFormControl*>(ev.GetTargetElement());
        if (ui_updating_ || !input || !input->IsPseudoClassSet("focus")) return;
        set_area_music(input->GetValue());
        input->Blur(); // the drop-down then shows what the area has
    });
    on("lv_zn_delete", [this](Rml::Event&, const Rml::VariantList&) { delete_area(false); });
    on("lv_zn_delete_confirm", [this](Rml::Event&, const Rml::VariantList&) { delete_area(true); });
    on("lv_zn_delete_cancel", [this](Rml::Event&, const Rml::VariantList&) {
        zn_ask_ = 0;
        zn_note_ = "Зона осталась";
    });
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
    bind_tiled(model);
}

// --- tools -------------------------------------------------------------------

void LevelEditor::set_tool(Tool t) {
    if (stroke_) return; // not in the middle of a stroke
    tool_ = t;
}

void LevelEditor::select_tile(usize index) {
    const auto& tiles = tiles_;
    if (index >= tiles.size()) return;
    tile_ = index;
    layer_ = tiles[index].layer;
}

void LevelEditor::set_brush_radius(i32 r) { radius_ = std::clamp(r, 0, 16); }

world::TileId LevelEditor::paint_value() const {
    if (tool_ == Tool::Eraser) return 0;
    const auto& tiles = tiles_;
    return tile_ < tiles.size() ? tiles[tile_].value : 0;
}

std::string LevelEditor::stroke_label() const {
    const auto& tiles = tiles_;
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
        const u32 layer = tile_ < tiles_.size() ? tiles_[tile_].layer : layer_;
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
    const auto& tiles = tiles_;
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

void LevelEditor::arm_template(u64 key) {
    if (module_.objects_version() != objects_version_) build_objects();
    const auto& defs = module_.objects();
    for (usize i = 0; i < defs.size(); ++i)
        if (defs[i].key == key) {
            arm_object(static_cast<i32>(i));
            FORGE_INFO("«%s»: щёлкни по уровню, чтобы поставить", defs[i].name.c_str());
            return;
        }
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
    edit_begins();
    std::string label = "Удалить: ";
    const auto& defs = module_.objects();
    const flecs::entity first = level_->find(gone[0].id);
    const i32 kind = module_.object_kind(first);
    label += gone.size() == 1 && kind >= 0                          ? defs[static_cast<usize>(kind)].name
             : gone.size() == 1 && first.has<sim::GravitySource>()     ? std::string(kPointName)
             : gone.size() == 1 && first.has<level::LightSource>()     ? std::string(kLightName)
                                                                      : std::to_string(gone.size()) + " объектов";
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
    const u64 version = (level_->object_edits() + ph_preview_) * 1'000'003ull + selection_version_;
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
    if (mode_ == Mode::Physics && e.is_valid() && e.has<sim::GravitySource>()) {
        physics_fields(e);
    } else if (mode_ == Mode::Light && e.is_valid() && e.has<level::LightSource>()) {
        light_fields(e);
    } else if (kind >= 0) {
        const level::ObjectDef& def = module_.objects()[static_cast<usize>(kind)];
        m_sel_name_ = def.name;
        m_sel_hint_ = def.hint;
        const objects::Template* st = module_.library() ? module_.library()->find(def.key) : nullptr;
        m_sel_icon_ = "/memory/obj_" + def.id + "_" + std::to_string(st ? st->look() : 0);
        const scene::Position& p = e.get<scene::Position>();
        f64 x0, y0, x1, y1;
        place_limits(x0, y0, x1, y1);
        char v[32];
        std::snprintf(v, sizeof(v), "%.2f", p.tile_x());
        m_fields_.push_back({"text", "X", v, "", 0, 0, 0});
        field_refs_.push_back({nullptr, "x", {}, nullptr, {}, true, x0, x1, true});
        std::snprintf(v, sizeof(v), "%.2f", p.tile_y());
        m_fields_.push_back({"text", "Y", v, "", 0, 0, 0});
        field_refs_.push_back({nullptr, "y", {}, nullptr, {}, true, y0, y1, true});
        // The template's properties in plain words; what the copy sets its
        // own way is marked and can go back to the template's.
        objects::Library* lib = module_.library();
        const objects::Template* tmpl = lib ? lib->template_of(e) : nullptr;
        const objects::KindDef* kd = tmpl ? lib->kind_of(*tmpl) : nullptr;
        const objects::ObjectRef* ref = e.try_get<objects::ObjectRef>();
        m_sel_kind_ = kd ? kd->name : "";
        if (kd)
            for (const objects::PropDef* pp : lib->props_of(*tmpl)) {
                const objects::PropDef& prop = *pp;
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

void LevelEditor::physics_fields(flecs::entity e) {
    const sim::GravitySource& g = e.get<sim::GravitySource>();
    const scene::Position& p = e.get<scene::Position>();
    m_sel_name_ = kPointName;
    m_sel_hint_ = !g.toward_center ? "тянет в одну сторону (так задано не в редакторе)"
                  : g.strength > 0 ? "притягивает тела к центру"
                  : g.strength < 0 ? "отталкивает тела от центра"
                                   : "сила 0: не действует";
    m_sel_icon_.clear();
    m_sel_kind_.clear();
    f64 x0, y0, x1, y1;
    place_limits(x0, y0, x1, y1);
    auto row = [&](const char* kind, const char* label, std::string value, const char* hint, const reflect::TypeInfo* type,
                   const char* path, bool limited, f64 min, f64 max, f32 step) {
        FieldView f;
        f.kind = kind;
        f.label = label;
        f.value = std::move(value);
        f.hint = hint;
        f.min = static_cast<float>(min);
        f.max = static_cast<float>(max);
        f.step = step;
        m_fields_.push_back(std::move(f));
        FieldRef r;
        r.type = type;
        r.path = path;
        r.limited = limited;
        r.min = min;
        r.max = max;
        r.refuse = !type; // the centre
        field_refs_.push_back(std::move(r));
    };
    char v[32];
    std::snprintf(v, sizeof(v), "%.2f", p.tile_x());
    row("text", "X", v, "центр, в тайлах", nullptr, "x", true, x0, x1, 0);
    std::snprintf(v, sizeof(v), "%.2f", p.tile_y());
    row("text", "Y", v, "центр, в тайлах", nullptr, "y", true, y0, y1, 0);
    const reflect::TypeInfo* type = reflect::type_of<sim::GravitySource>();
    row("slider", "Сила", number(g.strength), "тайлов/с²; отрицательная отталкивает", type, "strength", true, -kMaxStrength,
        kMaxStrength, 1);
    row("slider", "Радиус", number(g.radius), "тайлов; на самой окружности ещё действует", type, "radius", true, kMinRadius,
        kMaxRadius, 0.5f);
    row("bool", "Слабеет к краю", g.fade ? "true" : "false", "в центре полная сила, у края ноль", type, "fade", false, 0, 0, 0);
    row("bool", "Выключает мировую", g.replace ? "true" : "false",
        "внутри радиуса гравитация мира не действует, другие точки действуют", type, "replace", false, 0, 0, 0);
}

void LevelEditor::light_fields(flecs::entity e) {
    const level::LightSource& s = e.get<level::LightSource>();
    const scene::Position& p = e.get<scene::Position>();
    m_sel_name_ = kLightName;
    m_sel_hint_ = s.brightness <= 0 || (s.r <= 0 && s.g <= 0 && s.b <= 0)
                      ? "не светит: яркость или цвет 0"
                      : "светит на " + number(s.radius) + " тайлов, в центре ярче всего";
    m_sel_icon_.clear();
    m_sel_kind_.clear();
    f64 x0, y0, x1, y1;
    place_limits(x0, y0, x1, y1);
    const reflect::TypeInfo* type = reflect::type_of<level::LightSource>();
    auto row = [&](const char* kind, const char* label, std::string value, const char* hint, const reflect::TypeInfo* t,
                   const char* path, bool limited, f64 min, f64 max, f32 step) {
        FieldView f;
        f.kind = kind;
        f.label = label;
        f.value = std::move(value);
        f.hint = hint;
        f.min = static_cast<float>(min);
        f.max = static_cast<float>(max);
        f.step = step;
        m_fields_.push_back(std::move(f));
        FieldRef r;
        r.type = t;
        r.path = path;
        r.limited = limited;
        r.min = min;
        r.max = max;
        r.refuse = !t; // the centre
        field_refs_.push_back(std::move(r));
    };
    char v[32];
    std::snprintf(v, sizeof(v), "%.2f", p.tile_x());
    row("text", "X", v, "центр, в тайлах", nullptr, "x", true, x0, x1, 0);
    std::snprintf(v, sizeof(v), "%.2f", p.tile_y());
    row("text", "Y", v, "центр, в тайлах", nullptr, "y", true, y0, y1, 0);
    row("color", "Цвет", hex_color(s.r, s.g, s.b), "#RRGGBB: оттенок; яркость и радиус от него не меняются", type, "color", false,
        0, 0, 0);
    row("slider", "Яркость", number(s.brightness), "свет в центре: 1 — как днём, до 4; 0 — не светит; на дальность не влияет",
        type, "brightness", true, 0, level::kMaxBrightness, 0.05f);
    row("slider", "Радиус", number(s.radius), "тайлов: дальше окружности света нет; от 1 до 40", type, "radius", true,
        level::kMinLightRadius, level::kMaxLightRadius, 0.5f);
}

void LevelEditor::place_limits(f64& x0, f64& y0, f64& x1, f64& y1) const {
    // Inside the world, the outer tiles' centres at most; a world without
    // edges: a million tiles each way, far past any level.
    const world::Rect b = module_.world_desc().bounds;
    const bool edges = b.x1 > b.x0 && b.y1 > b.y0;
    const f64 n = world::kChunkSize;
    x0 = edges ? b.x0 * n + 0.5 : -1e6;
    x1 = edges ? b.x1 * n - 0.5 : 1e6;
    y0 = edges ? b.y0 * n + 0.5 : -1e6;
    y1 = edges ? b.y1 * n - 0.5 : 1e6;
}

bool LevelEditor::parse_number(const FieldRef& ref, const std::string& label, const std::string& text, f64& out) const {
    // A comma as in Russian decimals.
    std::string t = text;
    std::replace(t.begin(), t.end(), ',', '.');
    const char* start = t.c_str();
    char* end = nullptr;
    f64 v = std::strtod(start, &end);
    while (end && (*end == ' ' || *end == '\t')) ++end;
    if (end == start || !end || *end != 0 || !std::isfinite(v)) {
        FORGE_WARN("%s: «%s» — не число; оставлено как было", label.c_str(), text.c_str());
        return false;
    }
    if (ref.limited && ref.refuse && (v < ref.min || v > ref.max)) {
        FORGE_WARN("%s: %s — за краем мира (можно от %s до %s); оставлено как было", label.c_str(), text.c_str(),
                   number(ref.min).c_str(), number(ref.max).c_str());
        return false;
    }
    if (ref.limited && (v < ref.min || v > ref.max)) {
        const f64 kept = std::clamp(v, ref.min, ref.max);
        FORGE_WARN("%s: можно от %s до %s; поставлено %s", label.c_str(), number(ref.min).c_str(), number(ref.max).c_str(),
                   number(kept).c_str());
        v = kept;
    }
    out = v;
    return true;
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
    const bool point = e.has<sim::GravitySource>() && kind < 0;
    const bool lamp = e.has<level::LightSource>() && kind < 0;
    const std::string name = kind >= 0 ? module_.objects()[static_cast<usize>(kind)].name
                             : point   ? kPointName
                             : lamp    ? kLightName
                                       : std::string();
    const std::string label = m_fields_[static_cast<usize>(i)].label;
    if (lamp && ref.type == reflect::type_of<level::LightSource>()) {
        const level::LightSource before = e.get<level::LightSource>();
        level::LightSource after = before;
        if (ref.path == "color") {
            if (!parse_hex(text, after.r, after.g, after.b)) {
                lt_note_ = label + ": «" + text + "» — не цвет; нужен вид #RRGGBB, оставлено как было";
                FORGE_WARN("%s", lt_note_.c_str());
                fields_built_ = 0;
                return;
            }
        } else {
            f64 v = 0;
            if (!parse_number(ref, label, text, v)) {
                fields_built_ = 0; // show the old value again
                return;
            }
            (ref.path == "brightness" ? after.brightness : after.radius) = static_cast<f32>(v);
        }
        const std::string a = data::to_json(ref.type, &before, false), b = data::to_json(ref.type, &after, false);
        if (a == b) {
            fields_built_ = 0;
            return;
        }
        history_.execute(std::make_unique<level::SetObjectComponent>(*level_, selection_[0], p.tile_x(), p.tile_y(), ref.type, a,
                                                                     b, ref.path, "«" + name + "»: " + label));
        if (!dragging) history_.seal();
        return;
    }
    if (point && ref.type == reflect::type_of<sim::GravitySource>()) {
        const sim::GravitySource before = e.get<sim::GravitySource>();
        sim::GravitySource after = before;
        if (ref.path == "fade" || ref.path == "replace") {
            (ref.path == "fade" ? after.fade : after.replace) = text == "true";
        } else {
            f64 v = 0;
            if (!parse_number(ref, label, text, v)) {
                fields_built_ = 0; // show the old value again
                return;
            }
            (ref.path == "strength" ? after.strength : after.radius) = static_cast<f32>(v);
        }
        const std::string a = data::to_json(ref.type, &before, false), b = data::to_json(ref.type, &after, false);
        if (a == b) {
            fields_built_ = 0;
            return;
        }
        edit_begins();
        history_.execute(std::make_unique<level::SetObjectComponent>(*level_, selection_[0], p.tile_x(), p.tile_y(), ref.type, a,
                                                                     b, ref.path, "«" + name + "»: " + label));
        if (!dragging) history_.seal();
        return;
    }
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
        f64 v = 0;
        if (!parse_number(ref, label, text, v)) {
            fields_built_ = 0; // show the old value again
            return;
        }
        level::MoveObjects::Move m{selection_[0], p.tile_x(), p.tile_y(), p.tile_x(), p.tile_y()};
        (ref.path == "x" ? m.to_x : m.to_y) = v;
        if (m.to_x == m.from_x && m.to_y == m.from_y) {
            fields_built_ = 0;
            return;
        }
        edit_begins();
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

// --- physics ---------------------------------------------------------------

void LevelEditor::set_phys_tool(PhysTool t) {
    if (gesture()) return;
    ph_tool_ = t;
}

PullDir LevelEditor::pull_dir() const {
    const level::LevelPhysics& p = level_->physics();
    const u32 down = sim::cells_down(p.gravity_x, p.gravity_y);
    return down == ~0u ? PullDir::None : static_cast<PullDir>(down);
}

f32 LevelEditor::pull_strength() const {
    const level::LevelPhysics& p = level_->physics();
    return std::hypot(p.gravity_x, p.gravity_y);
}

void LevelEditor::set_pull_dir(PullDir d) {
    if (gesture()) return;
    const level::LevelPhysics before = level_->physics();
    f32 s = pull_strength();
    if (s > level::kMaxGravity) {
        // A slanted pull from physics.json (each part up to 200) is longer
        // than any one way may be.
        ph_note_ = "Сила гравитации мира " + number(s) + " больше " + number(level::kMaxGravity) + ": в новом направлении " +
                   number(level::kMaxGravity);
        FORGE_WARN("%s", ph_note_.c_str());
        s = level::kMaxGravity;
    }
    if (s > 0) ph_strength_ = s;
    else s = ph_strength_;
    level::LevelPhysics after;
    switch (d) {
    case PullDir::Down: after.gravity_y = s; break;
    case PullDir::Up: after.gravity_y = -s; break;
    case PullDir::Left: after.gravity_x = -s; break;
    case PullDir::Right: after.gravity_x = s; break;
    case PullDir::None: break;
    }
    if (after == before) return;
    edit_begins();
    history_.seal();
    history_.execute(std::make_unique<level::SetPhysics>(*level_, before, after, "Гравитация мира: направление"));
    history_.seal();
}

void LevelEditor::set_pull_strength(const std::string& text, bool dragging) {
    // Bindings fill the inputs during the UI's update and fire change events then.
    if (ui_updating_ || gesture() || text == m_ph_strength_) return;
    const PullDir d = pull_dir();
    if (d == PullDir::None) return; // no pull: set a direction first
    FieldRef range;
    range.limited = true;
    range.min = 1;
    range.max = level::kMaxGravity;
    f64 v = 0;
    if (!parse_number(range, "Сила гравитации мира", text, v)) {
        m_ph_strength_.clear(); // shows the old value again
        return;
    }
    const level::LevelPhysics before = level_->physics();
    const f32 s = static_cast<f32>(v);
    level::LevelPhysics after;
    switch (d) {
    case PullDir::Down: after.gravity_y = s; break;
    case PullDir::Up: after.gravity_y = -s; break;
    case PullDir::Left: after.gravity_x = -s; break;
    case PullDir::Right: after.gravity_x = s; break;
    case PullDir::None: break;
    }
    ph_strength_ = s;
    if (after == before) {
        m_ph_strength_.clear();
        return;
    }
    edit_begins();
    history_.execute(std::make_unique<level::SetPhysics>(*level_, before, after, "Гравитация мира: сила"));
    if (!dragging) history_.seal();
}

// --- light -----------------------------------------------------------------

void LevelEditor::set_light_tool(LightTool t) {
    if (gesture()) return;
    lt_tool_ = t;
}

void LevelEditor::set_level_time(const std::string& text, bool dragging) {
    // Bindings fill the inputs during the UI's update and fire change events then.
    if (ui_updating_ || gesture() || text == m_lt_time_ || text == m_lt_hours_) return;
    f64 h = 0;
    if (!level::parse_clock(text, h) || h < 0 || h >= 24) {
        lt_note_ = "Время суток: «" + text + "» — нужно от 0:00 до 23:59 (например 18:30); оставлено как было";
        FORGE_WARN("%s", lt_note_.c_str());
        m_lt_time_.clear(); // shows the old value again
        return;
    }
    const level::LevelLight before = level_->light();
    level::LevelLight after = before;
    after.time = static_cast<f32>(h);
    if (after == before) {
        m_lt_time_.clear();
        return;
    }
    edit_begins();
    history_.execute(std::make_unique<level::SetLight>(*level_, before, after, "Время суток"));
    if (!dragging) history_.seal();
}

void LevelEditor::set_preview_time(const std::string& text) {
    if (ui_updating_) return;
    if (text.empty()) {
        view_.preview_time = -1;
        return;
    }
    f64 h = 0;
    if (!level::parse_clock(text, h) || h < 0 || h >= 24) {
        lt_note_ = "Просмотр: «" + text + "» — нужно от 0:00 до 23:59; вид не изменился";
        FORGE_WARN("%s", lt_note_.c_str());
        m_lt_preview_.clear();
        return;
    }
    view_.preview_time = static_cast<f32>(h);
    if (!view_.game_light) view_.game_light = true;
}

bool LevelEditor::start_trial() {
    if (gesture()) return false;
    if (trial_.running()) return true;
    if (vw_ < 1 || vh_ < 1) return false;
    history_.seal();
    std::string error;
    const world::Rect view = camera_.visible_tiles(static_cast<u32>(vw_), static_cast<u32>(vh_));
    if (!trial_.start(*level_, view, &error)) {
        FORGE_WARN("Проба не началась: %s", error.c_str());
        return false;
    }
    if (trial_.still()) FORGE_INFO("Проба: гравитации мира нет, вода и песок стоят на месте");
    else
        FORGE_INFO("Проба: вода и песок текут %s по гравитации мира; точки гравитации на них не действуют. «Сбросить» вернёт уровень",
                   kPullNames[trial_.down()]);
    return true;
}

void LevelEditor::reset_trial() {
    if (!trial_.running()) return;
    trial_.reset();
    map_edits_ = ~0ull;
    FORGE_INFO("Проба сброшена: вода и песок снова как в уровне");
}

void LevelEditor::edit_begins() { reset_trial(); }

bool LevelEditor::cancel_gesture() {
    if (zn_drag_ != AreaDrag::None) {
        // The level never changed during the drag: only the preview goes.
        zn_drag_ = AreaDrag::None;
        zn_note_ = "Отменено: зоны и точка появления не изменились";
        FORGE_INFO("Отменено: уровень не изменился");
        return true;
    }
    const PhysDrag d = ph_drag_;
    if (d == PhysDrag::None) return false;
    ph_drag_ = PhysDrag::None;
    flecs::entity e = selection_.empty() ? flecs::entity() : level_->find(selection_[0]);
    if (d == PhysDrag::Move && e.is_valid()) e.set<scene::Position>(scene::Position::at_tile(ph_from_x_, ph_from_y_));
    if (d == PhysDrag::Radius && e.is_valid()) level::set_component_json(*level_, e, ring_type(), ph_before_);
    ++ph_preview_;
    FORGE_INFO("Отменено: уровень не изменился");
    return true;
}

bool LevelEditor::ring_of(flecs::entity e) const {
    return e.is_valid() && (mode_ == Mode::Light ? e.has<level::LightSource>() : e.has<sim::GravitySource>());
}

f32 LevelEditor::ring_radius(flecs::entity e) const {
    if (mode_ == Mode::Light) return e.get<level::LightSource>().radius;
    return e.get<sim::GravitySource>().radius;
}

void LevelEditor::set_ring_radius(flecs::entity e, f32 r) {
    if (mode_ == Mode::Light) {
        level::LightSource s = e.get<level::LightSource>();
        s.radius = r;
        e.set<level::LightSource>(s);
    } else {
        sim::GravitySource g = e.get<sim::GravitySource>();
        g.radius = r;
        e.set<sim::GravitySource>(g);
    }
}

const reflect::TypeInfo* LevelEditor::ring_type() const {
    return mode_ == Mode::Light ? reflect::type_of<level::LightSource>() : reflect::type_of<sim::GravitySource>();
}

const char* LevelEditor::ring_name() const { return mode_ == Mode::Light ? kLightName : kPointName; }

f64 LevelEditor::ring_max() const { return mode_ == Mode::Light ? level::kMaxLightRadius : kMaxRadius; }

template <typename F>
void LevelEditor::each_ring(F&& f) {
    if (mode_ == Mode::Light)
        level_->scene().ecs().each([&](flecs::entity e, const scene::Position& p, const level::LightSource& s) {
            f(e, p.tile_x(), p.tile_y(), static_cast<f64>(s.radius));
        });
    else
        level_->scene().ecs().each([&](flecs::entity e, const scene::Position& p, const sim::GravitySource& g) {
            f(e, p.tile_x(), p.tile_y(), static_cast<f64>(g.radius));
        });
}

flecs::entity LevelEditor::point_at(f64 tx, f64 ty, bool ring) {
    const f64 grab = std::max(0.6, 9.0 / camera_.zoom), edge = std::max(0.4, 7.0 / camera_.zoom);
    const u64 selected = selection_.empty() ? 0 : selection_[0];
    flecs::entity best;
    f64 best_d = 0;
    each_ring([&](flecs::entity e, f64 x, f64 y, f64 radius) {
        const f64 d = std::hypot(x - tx, y - ty);
        f64 off = d;
        if (ring) {
            if (!selected || level_->id_of(e, false) != selected) return;
            off = std::fabs(d - radius);
            if (off > edge) return;
        } else if (d > grab) {
            return;
        }
        if (!best.is_valid() || off < best_d) {
            best = e;
            best_d = off;
        }
    });
    return best;
}

bool LevelEditor::press_ring(f64 tx, f64 ty, bool place) {
    if (place) {
        // The centre of the cell: a place the author can name.
        ph_cx_ = std::floor(tx) + 0.5;
        ph_cy_ = std::floor(ty) + 0.5;
        ph_r_ = 0;
        ph_drag_ = PhysDrag::Place;
        return true;
    }
    flecs::entity hit = point_at(tx, ty, false);
    bool ring = false;
    if (!hit.is_valid()) {
        hit = point_at(tx, ty, true);
        ring = hit.is_valid();
    }
    if (!hit.is_valid()) {
        select_objects({});
        return false; // the drag moves the view
    }
    select_objects({level_->id_of(hit, true)});
    ph_grab_x_ = tx;
    ph_grab_y_ = ty;
    const scene::Position& p = hit.get<scene::Position>();
    ph_from_x_ = p.tile_x();
    ph_from_y_ = p.tile_y();
    if (ring) {
        ph_before_ = level::component_json(*level_, hit, ring_type());
        ph_r0_ = ring_radius(hit);
        ph_drag_ = PhysDrag::Radius;
    } else {
        ph_drag_ = PhysDrag::Move;
    }
    return true;
}

bool LevelEditor::press_light(f32 x, f32 y) {
    f64 tx, ty;
    to_tile(x, y, tx, ty);
    history_.seal();
    return press_ring(tx, ty, lt_tool_ == LightTool::Source);
}

bool LevelEditor::press_physics(f32 x, f32 y) {
    f64 tx, ty;
    to_tile(x, y, tx, ty);
    history_.seal();
    switch (ph_tool_) {
    case PhysTool::Select: return press_ring(tx, ty, false);
    case PhysTool::Point: return press_ring(tx, ty, true);
    case PhysTool::Water:
    case PhysTool::Sand:
        cell_at(x, y, ph_ax_, ph_ay_);
        ph_bx_ = ph_ax_;
        ph_by_ = ph_ay_;
        ph_drag_ = PhysDrag::Area;
        return true;
    }
    return false;
}

void LevelEditor::drag_physics(f32 x, f32 y) {
    f64 tx, ty;
    to_tile(x, y, tx, ty);
    switch (ph_drag_) {
    case PhysDrag::None: return;
    case PhysDrag::Place: ph_r_ = std::hypot(tx - ph_cx_, ty - ph_cy_); return;
    case PhysDrag::Area: cell_at(x, y, ph_bx_, ph_by_); return;
    case PhysDrag::Move:
    case PhysDrag::Radius: break;
    }
    flecs::entity e = selection_.empty() ? flecs::entity() : level_->find(selection_[0]);
    if (!e.is_valid()) {
        ph_drag_ = PhysDrag::None;
        return;
    }
    if (ph_drag_ == PhysDrag::Move) {
        // Whole tiles: the centre stays where it was in its cell.
        const f64 nx = ph_from_x_ + std::round(tx - ph_grab_x_), ny = ph_from_y_ + std::round(ty - ph_grab_y_);
        const scene::Position& p = e.get<scene::Position>();
        if (p.tile_x() == nx && p.tile_y() == ny) return;
        if (!level_->loaded(static_cast<i32>(std::floor(nx)), static_cast<i32>(std::floor(ny)))) return;
        edit_begins();
        e.set<scene::Position>(scene::Position::at_tile(nx, ny));
    } else {
        const scene::Position& p = e.get<scene::Position>();
        const f64 r = std::clamp(std::round(std::hypot(tx - p.tile_x(), ty - p.tile_y()) * 2) / 2, kMinRadius, ring_max());
        if (ring_radius(e) == static_cast<f32>(r)) return;
        edit_begins();
        set_ring_radius(e, static_cast<f32>(r));
    }
    ++ph_preview_;
}

void LevelEditor::release_physics() {
    const PhysDrag d = ph_drag_;
    ph_drag_ = PhysDrag::None;
    if (d == PhysDrag::Area) {
        pour(ph_ax_, ph_ay_, ph_bx_, ph_by_);
        return;
    }
    if (d == PhysDrag::Place) {
        const bool light = mode_ == Mode::Light;
        const f64 r = ph_r_ < 0.5 ? (light ? kLightNewRadius : kNewRadius) : std::clamp(std::round(ph_r_ * 2) / 2, kMinRadius, ring_max());
        std::string& note = light ? lt_note_ : ph_note_;
        edit_begins();
        flecs::entity e = level_->scene().spawn(scene::Position::at_tile(ph_cx_, ph_cy_));
        if (!e.is_valid()) {
            note = std::string("Здесь уровень ещё не загрузился: ") + (light ? "источник" : "точка") + " не поставлен" + (light ? "" : "а");
            FORGE_WARN("%s", note.c_str());
            return;
        }
        if (light) {
            level::LightSource ls;
            ls.radius = static_cast<f32>(r);
            e.set<level::LightSource>(ls);
        } else {
            sim::GravitySource g;
            g.radius = static_cast<f32>(r);
            e.set<sim::GravitySource>(g);
        }
        level::ObjectSnapshot snap = level::snapshot(*level_, e);
        const u64 id = snap.id;
        history_.execute(std::make_unique<level::ObjectsCommand>(*level_, std::vector<level::ObjectSnapshot>{std::move(snap)},
                                                                 true, std::string("Поставить: ") + ring_name()));
        history_.seal();
        select_objects({id});
        note = std::string(ring_name()) + ": центр " + number(ph_cx_) + ", " + number(ph_cy_) + ", радиус " + number(r);
        FORGE_INFO("%s", note.c_str());
        return;
    }
    flecs::entity e = selection_.empty() ? flecs::entity() : level_->find(selection_[0]);
    if (!e.is_valid() || (d != PhysDrag::Move && d != PhysDrag::Radius)) return;
    const scene::Position& p = e.get<scene::Position>();
    if (d == PhysDrag::Move) {
        if (p.tile_x() == ph_from_x_ && p.tile_y() == ph_from_y_) return; // a click: only selected
        history_.execute(std::make_unique<level::MoveObjects>(
            *level_, std::vector<level::MoveObjects::Move>{{selection_[0], ph_from_x_, ph_from_y_, p.tile_x(), p.tile_y()}},
            std::string("Передвинуть: ") + ring_name()));
    } else {
        const reflect::TypeInfo* type = ring_type();
        const std::string after = level::component_json(*level_, e, type);
        if (after == ph_before_) return;
        history_.execute(std::make_unique<level::SetObjectComponent>(*level_, selection_[0], p.tile_x(), p.tile_y(), type,
                                                                     ph_before_, after, "radius",
                                                                     std::string("«") + ring_name() + "»: Радиус"));
    }
    history_.seal();
}

void LevelEditor::pour(i32 x0, i32 y0, i32 x1, i32 y1) {
    const char* id = ph_tool_ == PhysTool::Water ? "water" : "sand";
    const auto& tiles = module_.tiles();
    const level::TileDef* fill = nullptr;
    std::vector<u32> layers; // a cell is free when all of these are empty there
    for (const std::string& f : module_.physics_fills())
        for (const level::TileDef& t : tiles)
            if (t.id == f) {
                if (f == id) fill = &t;
                if (std::find(layers.begin(), layers.end(), t.layer) == layers.end()) layers.push_back(t.layer);
            }
    if (!fill) {
        FORGE_WARN("В этой игре нет плитки «%s»", id);
        return;
    }
    const i64 w = std::abs(static_cast<i64>(x1) - x0) + 1, h = std::abs(static_cast<i64>(y1) - y0) + 1;
    if (w * h > static_cast<i64>(kFillLimit)) {
        ph_note_ = fill->name + ": область " + std::to_string(w) + " × " + std::to_string(h) + " больше " +
                   group_digits(kFillLimit) + " клеток, ничего не налито";
        FORGE_WARN("%s", ph_note_.c_str());
        return;
    }
    // The author's level, not the trial's: a cell the trial emptied is still
    // taken, one it filled is free.
    edit_begins();
    std::vector<level::Cell> rect, free;
    level::rect_cells(x0, y0, x1, y1, true, rect);
    usize taken = 0, away = 0;
    for (const level::Cell& c : rect) {
        if (!level_->loaded(c.x, c.y)) {
            ++away;
            continue;
        }
        bool empty = true;
        for (u32 l : layers) empty = empty && level_->tile(l, c.x, c.y) == 0;
        if (empty) free.push_back(c);
        else ++taken;
    }
    const std::string size = std::to_string(w) + " × " + std::to_string(h);
    if (free.empty()) {
        ph_note_ = fill->name + " " + size + ": свободных клеток нет, ничего не налито (занято " + std::to_string(taken) + ")";
        FORGE_INFO("%s", ph_note_.c_str());
        return;
    }
    auto stroke = std::make_unique<level::TileStroke>(*level_, fill->name + ": " + size);
    stroke->paint(fill->layer, free, fill->value);
    history_.execute(std::move(stroke));
    history_.seal();
    ph_note_ = fill->name + " " + size + ": клеток залито " + std::to_string(free.size()) + ", занятые не тронуты: " +
               std::to_string(taken);
    if (away) ph_note_ += ", не загружено: " + std::to_string(away);
    FORGE_INFO("%s", ph_note_.c_str());
}

// --- zones -------------------------------------------------------------------

namespace {

std::string area_bounds(const level::Area& z) {
    return "x " + std::to_string(z.x0) + "…" + std::to_string(z.x1 - 1) + ", y " + std::to_string(z.y0) + "…" + std::to_string(z.y1 - 1) +
           " · " + std::to_string(z.x1 - z.x0) + " × " + std::to_string(z.y1 - z.y0) + " клеток";
}

std::string in_quotes(const std::string& s) { return "«" + s + "»"; }

} // namespace

void LevelEditor::set_area_tool(AreaTool t) {
    if (gesture()) return;
    zn_tool_ = t;
    zn_ask_ = 0;
}

void LevelEditor::select_area(u64 id) {
    zn_area_ = level_->areas().find(id) ? id : 0;
    zn_spawn_ = false;
    if (zn_ask_ != zn_area_) zn_ask_ = 0;
}

void LevelEditor::select_spawn() {
    zn_area_ = 0;
    zn_spawn_ = level_->areas().spawn || zn_drag_ == AreaDrag::Spawn;
    zn_ask_ = 0;
}

u64 LevelEditor::area_at(const level::LevelAreas& a, f64 tx, f64 ty) const {
    const level::Area* best = nullptr;
    for (const level::Area& z : a.areas)
        if (z.contains(tx, ty) && (!best || z.size() <= best->size())) best = &z;
    return best ? best->id : 0;
}

u8 LevelEditor::edges_at(f64 tx, f64 ty) const {
    const level::Area* z = level_->areas().find(zn_area_);
    if (!z) return 0;
    // Near an edge: a few pixels, but never more than a quarter of the area
    // (a thin one is still moved by its middle).
    const f64 e = std::max(0.3, 7.0 / camera_.zoom);
    const f64 ex = std::min(e, (z->x1 - z->x0) * 0.25), ey = std::min(e, (z->y1 - z->y0) * 0.25);
    if (tx < z->x0 - ex || tx > z->x1 + ex || ty < z->y0 - ey || ty > z->y1 + ey) return 0;
    const f64 l = std::fabs(tx - z->x0), r = std::fabs(tx - z->x1), t = std::fabs(ty - z->y0), b = std::fabs(ty - z->y1);
    u8 out = 0;
    if (std::min(l, r) <= ex) out |= l <= r ? 1 : 4;
    if (std::min(t, b) <= ey) out |= t <= b ? 2 : 8;
    return out;
}

bool LevelEditor::spawn_at(f64 tx, f64 ty) const {
    const level::LevelAreas& a = level_->areas();
    if (!a.spawn) return false;
    // The hero standing there: a cell wide, two high, feet on the point.
    const f64 t = std::max(0.2, 4.0 / camera_.zoom);
    return tx >= a.spawn_x - 0.5 - t && tx <= a.spawn_x + 0.5 + t && ty >= a.spawn_y - 2 - t && ty <= a.spawn_y + t;
}

void LevelEditor::change_areas(const level::LevelAreas& after, std::string label) {
    if (after == level_->areas()) return;
    edit_begins();
    history_.seal();
    history_.execute(std::make_unique<level::SetAreas>(*level_, level_->areas(), after, std::move(label)));
    history_.seal();
}

bool LevelEditor::press_areas(f32 x, f32 y) {
    f64 tx, ty;
    to_tile(x, y, tx, ty);
    history_.seal();
    zn_ask_ = 0;
    const level::LevelAreas& a = level_->areas();
    zn_preview_ = a;
    zn_grab_x_ = tx;
    zn_grab_y_ = ty;
    switch (zn_tool_) {
    case AreaTool::Draw:
        cell_at(x, y, zn_ax_, zn_ay_);
        zn_bx_ = zn_ax_;
        zn_by_ = zn_ay_;
        zn_drag_ = AreaDrag::Draw;
        return true;
    case AreaTool::Spawn:
        // Feet on the bottom of the cell pressed.
        zn_preview_.spawn = true;
        zn_preview_.spawn_x = std::floor(tx) + 0.5;
        zn_preview_.spawn_y = std::floor(ty) + 1;
        zn_drag_ = AreaDrag::Spawn;
        select_spawn();
        return true;
    case AreaTool::Select: break;
    }
    if (spawn_at(tx, ty)) {
        zn_drag_ = AreaDrag::Spawn;
        select_spawn();
        return true;
    }
    if (const u8 e = edges_at(tx, ty)) {
        zn_edges_ = e;
        zn_drag_ = AreaDrag::Edges;
        return true;
    }
    if (const u64 id = area_at(a, tx, ty)) {
        select_area(id);
        zn_drag_ = AreaDrag::Move;
        return true;
    }
    select_area(0);
    return false; // the drag moves the view
}

void LevelEditor::drag_areas(f32 x, f32 y) {
    f64 tx, ty;
    to_tile(x, y, tx, ty);
    const level::LevelAreas& a = level_->areas();
    // Whole cells: an area's edges are on the grid lines, the spawn point keeps its place in its cell.
    const i64 dx = std::clamp<i64>(std::llround(tx - zn_grab_x_), -(1 << 24), 1 << 24);
    const i64 dy = std::clamp<i64>(std::llround(ty - zn_grab_y_), -(1 << 24), 1 << 24);
    switch (zn_drag_) {
    case AreaDrag::None: return;
    case AreaDrag::Draw: cell_at(x, y, zn_bx_, zn_by_); return;
    case AreaDrag::Spawn:
        if (zn_tool_ == AreaTool::Spawn) {
            zn_preview_.spawn_x = std::floor(tx) + 0.5;
            zn_preview_.spawn_y = std::floor(ty) + 1;
        } else {
            zn_preview_.spawn_x = a.spawn_x + static_cast<f64>(dx);
            zn_preview_.spawn_y = a.spawn_y + static_cast<f64>(dy);
        }
        return;
    case AreaDrag::Move:
    case AreaDrag::Edges: break;
    }
    const level::Area* from = a.find(zn_area_);
    level::Area* to = zn_preview_.find(zn_area_);
    if (!from || !to) {
        zn_drag_ = AreaDrag::None;
        return;
    }
    if (zn_drag_ == AreaDrag::Move) {
        to->x0 = static_cast<i32>(from->x0 + dx);
        to->x1 = static_cast<i32>(from->x1 + dx);
        to->y0 = static_cast<i32>(from->y0 + dy);
        to->y1 = static_cast<i32>(from->y1 + dy);
        return;
    }
    // An edge goes to the grid line nearest the mouse; the area stays at least a cell and at most kMaxAreaSide.
    const i32 gx = static_cast<i32>(std::clamp<f64>(std::round(tx), -1e7, 1e7));
    const i32 gy = static_cast<i32>(std::clamp<f64>(std::round(ty), -1e7, 1e7));
    *to = *from;
    if (zn_edges_ & 1) to->x0 = std::clamp(gx, from->x1 - level::kMaxAreaSide, from->x1 - 1);
    if (zn_edges_ & 4) to->x1 = std::clamp(gx, from->x0 + 1, from->x0 + level::kMaxAreaSide);
    if (zn_edges_ & 2) to->y0 = std::clamp(gy, from->y1 - level::kMaxAreaSide, from->y1 - 1);
    if (zn_edges_ & 8) to->y1 = std::clamp(gy, from->y0 + 1, from->y0 + level::kMaxAreaSide);
}

void LevelEditor::release_areas() {
    const AreaDrag d = zn_drag_;
    zn_drag_ = AreaDrag::None;
    const level::LevelAreas& a = level_->areas();
    if (d == AreaDrag::Draw) {
        if (zn_ax_ == zn_bx_ && zn_ay_ == zn_by_) {
            zn_note_ = "Зона не нарисована: протяните прямоугольник от угла до угла";
            return;
        }
        if (a.areas.size() >= level::kMaxAreas) {
            zn_note_ = "Зон уже " + std::to_string(level::kMaxAreas) + ": больше уровень не держит, новая не нарисована";
            FORGE_WARN("%s", zn_note_.c_str());
            return;
        }
        level::Area z;
        do z.id = level::Level::new_id();
        while (a.find(z.id));
        // From the corner pressed, at most kMaxAreaSide cells either way.
        const i32 bx = std::clamp(zn_bx_, zn_ax_ - level::kMaxAreaSide + 1, zn_ax_ + level::kMaxAreaSide - 1);
        const i32 by = std::clamp(zn_by_, zn_ay_ - level::kMaxAreaSide + 1, zn_ay_ + level::kMaxAreaSide - 1);
        z.x0 = std::min(zn_ax_, bx);
        z.x1 = std::max(zn_ax_, bx) + 1;
        z.y0 = std::min(zn_ay_, by);
        z.y1 = std::max(zn_ay_, by) + 1;
        for (usize n = 1;; ++n) {
            z.name = "Зона " + std::to_string(n);
            if (std::none_of(a.areas.begin(), a.areas.end(), [&](const level::Area& o) { return o.name == z.name; })) break;
        }
        level::LevelAreas after = a;
        after.areas.push_back(z);
        change_areas(after, "Нарисовать зону " + in_quotes(z.name));
        select_area(z.id);
        zn_note_ = "Нарисована зона " + in_quotes(z.name) + ": " + area_bounds(z) + ". Имя и музыка — в панели справа.";
        FORGE_INFO("%s", zn_note_.c_str());
        return;
    }
    if (d == AreaDrag::None || zn_preview_ == a) return; // a click: only selected
    std::string label;
    if (d == AreaDrag::Spawn) {
        label = a.spawn ? "Передвинуть точку появления" : "Точка появления";
    } else {
        const level::Area* z = zn_preview_.find(zn_area_);
        if (!z) return;
        label = (d == AreaDrag::Move ? "Передвинуть зону " : "Границы зоны ") + in_quotes(z->name);
        zn_note_ = "Зона " + in_quotes(z->name) + ": " + area_bounds(*z);
    }
    change_areas(zn_preview_, label);
    if (d == AreaDrag::Spawn) {
        f64 gx = 0, gy = 0;
        const level::LevelAreas& now = level_->areas();
        char at[64];
        std::snprintf(at, sizeof(at), "%.1f, %.0f", now.spawn_x, now.spawn_y);
        zn_note_ = std::string("Точка появления: ") + at;
        if (!spawn_ground(gx, gy)) zn_note_ += ". Рядом нет пола с местом для героя: новая игра начнётся со старта игры";
        else if (gx != now.spawn_x || gy != now.spawn_y) {
            std::snprintf(at, sizeof(at), "%.1f, %.0f", gx, gy);
            zn_note_ += std::string(". Там нет пола под двумя свободными клетками: герой встанет на ближайший, в ") + at;
        }
        FORGE_INFO("%s", zn_note_.c_str());
    }
}

bool LevelEditor::spawn_ground(f64& x, f64& y) {
    const level::LevelAreas& a = level_->areas();
    if (!a.spawn) return false;
    // Found again only when the spawn point or the level changed.
    const u64 key = level_->areas_version() * 1000003ull + level_->edits();
    if (key != zn_spawn_key_) {
        zn_spawn_key_ = key;
        zn_spawn_ok_ = module_.play_spot(*level_, a.spawn_x, a.spawn_y, zn_spawn_x_, zn_spawn_y_);
    }
    x = zn_spawn_x_;
    y = zn_spawn_y_;
    return zn_spawn_ok_;
}

bool LevelEditor::set_area_name(const std::string& text) {
    if (gesture()) return false;
    const level::LevelAreas& a = level_->areas();
    const level::Area* z = a.find(zn_area_);
    if (!z) return false;
    const std::string name = level::clean_area_name(text);
    if (name == z->name) {
        m_zn_name_.clear(); // shows it as it is
        return true;
    }
    level::Area test = *z;
    test.name = name;
    std::string problem = name.empty() ? "имя не может быть пустым" : level::area_problem(test);
    for (const level::Area& o : a.areas)
        if (problem.empty() && o.id != z->id && o.name == name) problem = "так уже зовут другую зону, в «Логике» их было бы не различить";
    if (!problem.empty()) {
        zn_note_ = "Имя зоны " + in_quotes(text) + ": " + problem + "; осталось " + in_quotes(z->name);
        FORGE_WARN("%s", zn_note_.c_str());
        m_zn_name_.clear();
        return false;
    }
    const std::string old = z->name;
    level::LevelAreas after = a;
    after.find(zn_area_)->name = name;
    change_areas(after, "Имя зоны: " + in_quotes(name));
    zn_note_ = "Зона " + in_quotes(old) + " теперь " + in_quotes(name) + ": связи «Логики» с ней остались, у них новые слова";
    return true;
}

bool LevelEditor::set_area_music(const std::string& value) {
    if (gesture()) return false;
    const level::Area* z = level_->areas().find(zn_area_);
    if (!z) return false;
    std::string name = value;
    if (value.rfind("add:", 0) == 0) {
        // A sound of «Ресурсы»: into the game's sounds first, as the screens' music.
        const fs::path source = utf8_path(value.substr(4));
        const std::string shown = path_to_utf8(source.filename());
        std::string error;
        if (!audio::readable(source)) zn_note_ = in_quotes(shown) + ": игра читает WAV и OGG. Переведите его в «Ресурсах»: «Конвертировать…» → OGG.";
        else if (!audio::load(source, &error)) zn_note_ = in_quotes(shown) + " не звучит: " + error;
        else if ((name = copy_into(source, sounds_folder_, "звук")).empty())
            zn_note_ = in_quotes(shown) + " не скопировался в звуки игры (" + path_to_utf8(sounds_folder_) + ")";
        if (name.empty() || name == value) {
            FORGE_WARN("%s", zn_note_.c_str());
            return false;
        }
        scan_sounds();
    }
    if (!name.empty() && !level::valid_music_name(name)) {
        zn_note_ = "Музыка " + in_quotes(name) + " — не имя файла из звуков игры";
        FORGE_WARN("%s", zn_note_.c_str());
        return false;
    }
    if (name == z->music) return true;
    const std::string area = z->name;
    level::LevelAreas after = level_->areas();
    after.find(zn_area_)->music = name;
    change_areas(after, name.empty() ? "Без музыки: " + in_quotes(area) : "Музыка зоны " + in_quotes(area) + ": " + in_quotes(name));
    std::error_code ec;
    if (name.empty()) zn_note_ = "В зоне " + in_quotes(area) + " своей музыки нет: там играет музыка экранов поверх игры или тишина";
    else if (!fs::is_regular_file(sounds_folder_ / utf8_path(name), ec)) zn_note_ = "Музыка " + in_quotes(name) + ": нет файла в звуках игры";
    else zn_note_ = "В зоне " + in_quotes(area) + " играет " + in_quotes(name);
    return true;
}

bool LevelEditor::delete_area(bool confirm) {
    if (gesture()) return false;
    const level::LevelAreas& a = level_->areas();
    if (zn_spawn_ && a.spawn) {
        level::LevelAreas after = a;
        after.spawn = false;
        after.spawn_x = after.spawn_y = 0;
        change_areas(after, "Убрать точку появления");
        zn_spawn_ = false;
        zn_note_ = "Точки появления нет: новая игра начнётся со старта игры";
        return true;
    }
    const level::Area* z = a.find(zn_area_);
    if (!z) return false;
    const std::vector<std::string> links = area_links ? area_links(z->id) : std::vector<std::string>{};
    const std::string name = z->name;
    if (!links.empty() && !confirm) {
        zn_ask_ = z->id;
        zn_note_ = "Зона " + in_quotes(name) + " есть в связях «Логики» (" + std::to_string(links.size()) + "). Удалить её всё равно?";
        return false;
    }
    level::LevelAreas after = a;
    std::erase_if(after.areas, [&](const level::Area& o) { return o.id == zn_area_; });
    change_areas(after, "Удалить зону " + in_quotes(name));
    zn_ask_ = 0;
    zn_area_ = 0;
    zn_note_ = links.empty() ? "Зона " + in_quotes(name) + " удалена"
                             : "Зона " + in_quotes(name) + " удалена. Её связи (" + std::to_string(links.size()) +
                                   ") остались в «Логике» с пометкой «нет такой зоны» и не работают; Ctrl+Z вернёт зону с ними";
    FORGE_INFO("%s", zn_note_.c_str());
    return true;
}

// The game's sounds (WAV and OGG), the areas' music the folder lacks («нет
// файла»), then the sounds of «Ресурсы» not there yet.
void LevelEditor::scan_sounds() {
    std::vector<SoundRow> rows;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(sounds_folder_, ec)) {
        if (!e.is_regular_file(ec) || !audio::readable(e.path())) continue;
        rows.push_back({path_to_utf8(e.path().filename()), path_to_utf8(e.path().stem())});
        if (rows.size() >= 1000) break;
    }
    std::sort(rows.begin(), rows.end(), [](const SoundRow& a, const SoundRow& b) { return a.value < b.value; });
    auto listed = [&](const std::string& name) {
        return std::any_of(rows.begin(), rows.end(), [&](const SoundRow& r) { return r.value == name; });
    };
    for (const level::Area& z : level_->areas().areas)
        if (!z.music.empty() && !listed(z.music)) rows.push_back({z.music, z.music + " (нет файла)"});
    if (list_sounds) {
        usize added = 0;
        for (const fs::path& file : list_sounds()) {
            if (!audio::readable(file) || listed(path_to_utf8(file.filename()))) continue;
            rows.push_back({"add:" + path_to_utf8(file), path_to_utf8(file.stem()) + " — из «Ресурсов»"});
            if (++added >= 300) break;
        }
    }
    if (rows == m_zn_sounds_) return;
    m_zn_sounds_ = std::move(rows);
    if (model_) model_.DirtyVariable("lv_zn_sounds");
}

std::vector<std::pair<std::string, std::string>> LevelEditor::music_choices() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const SoundRow& r : m_zn_sounds_) out.emplace_back(r.value, r.name);
    return out;
}

void LevelEditor::sync_areas(Rml::Context* context) {
    const AreaToolInfo& t = area_info(zn_tool_);
    set(m_zn_tool_, Rml::String(t.id), "lv_zn_tool");
    set(m_zn_tool_name_, Rml::String(t.name), "lv_zn_tool_name");
    set(m_zn_tool_help_, Rml::String(t.help), "lv_zn_tool_help");
    const level::LevelAreas& saved = level_->areas();
    // An area undone away is no longer selected.
    if (zn_drag_ == AreaDrag::None && zn_area_ && !saved.find(zn_area_)) zn_area_ = 0;
    if (zn_drag_ == AreaDrag::None && zn_spawn_ && !saved.spawn) zn_spawn_ = false;
    if (zn_ask_ && zn_ask_ != zn_area_) zn_ask_ = 0;
    if (mode_ != Mode::Zones) return;
    const level::LevelAreas& a = shown_areas();
    const level::Area* z = a.find(zn_area_);
    set(m_zn_has_area_, z != nullptr, "lv_zn_has_area");
    set(m_zn_name_, Rml::String(z ? z->name : std::string()), "lv_zn_name");
    set(m_zn_bounds_, Rml::String(z ? area_bounds(*z) : std::string()), "lv_zn_bounds");
    {
        std::string note;
        std::error_code ec;
        if (z && !z->music.empty() && !fs::is_regular_file(sounds_folder_ / utf8_path(z->music), ec))
            note = "Нет файла " + in_quotes(z->music) + " в звуках игры: в игре здесь будет тихо, и журнал игры скажет об этом.";
        set(m_zn_music_note_, Rml::String(note), "lv_zn_music_note");
    }
    {
        std::vector<Rml::String> links;
        if (z && area_links)
            for (const std::string& l : area_links(z->id)) links.push_back(l);
        if (links != m_zn_links_) {
            m_zn_links_ = std::move(links);
            model_.DirtyVariable("lv_zn_links");
        }
    }
    set(m_zn_asking_, zn_ask_ != 0 && z != nullptr, "lv_zn_asking");
    set(m_zn_ask_text_,
        Rml::String(z ? "Связи останутся в «Логике» с пометкой «нет такой зоны» и не будут работать, пока зону не вернут (Ctrl+Z)."
                      : ""),
        "lv_zn_ask_text");
    set(m_zn_has_spawn_, a.spawn, "lv_zn_has_spawn");
    set(m_zn_spawn_sel_, zn_spawn_, "lv_zn_spawn_sel");
    {
        std::string text = "нет: новая игра начинается со старта игры", note;
        bool bad = false;
        if (a.spawn) {
            char at[96];
            std::snprintf(at, sizeof(at), "x %.1f, y %.0f", a.spawn_x, a.spawn_y);
            text = at;
            f64 gx = 0, gy = 0;
            if (zn_drag_ == AreaDrag::Spawn) {
                note = "Отпустите кнопку, чтобы поставить точку здесь.";
            } else if (!spawn_ground(gx, gy)) {
                note = "Вне мира или рядом нет пола с местом для героя (96 клеток вверх и вниз): новая игра начнётся со старта игры.";
                bad = true;
            } else if (gx != a.spawn_x || gy != a.spawn_y) {
                std::snprintf(at, sizeof(at), "x %.1f, y %.0f", gx, gy);
                note = std::string("Здесь нет пола под двумя свободными клетками: герой встанет на ближайший, в ") + at + ".";
                bad = true;
            } else {
                note = "Новая игра поставит героя сюда. «Играть отсюда» важнее: оно ставит героя в центр вида.";
            }
        }
        set(m_zn_spawn_, Rml::String(text), "lv_zn_spawn");
        set(m_zn_spawn_note_, Rml::String(note), "lv_zn_spawn_note");
        set(m_zn_spawn_bad_, bad, "lv_zn_spawn_bad");
    }
    set(m_zn_error_,
        Rml::String(level_->areas_error().empty()
                        ? std::string()
                        : level_->areas_error() + ". Зон нет, файл не тронут; когда вы их измените, он сохранится как areas.broken.json"),
        "lv_zn_error");
    set(m_zn_note_, Rml::String(zn_note_), "lv_zn_note");
    // The list of areas, as drawn.
    std::vector<AreaRow> rows;
    for (const level::Area& r : a.areas) {
        std::string about = std::to_string(r.x1 - r.x0) + " × " + std::to_string(r.y1 - r.y0);
        if (!r.music.empty()) about += " · ♪ " + r.music;
        rows.push_back({level::area_id_text(r.id), r.name, about, r.id == zn_area_});
    }
    if (rows != m_zn_areas_) {
        m_zn_areas_ = std::move(rows);
        model_.DirtyVariable("lv_zn_areas");
    }
    // Names over the view: at each area's top left corner, the spawn point's over the hero.
    std::vector<AreaLabel> labels;
    auto label = [&](const std::string& id, const std::string& name, f64 tx, f64 ty, bool sel, bool spawn) {
        f32 sx = 0, sy = 0;
        screen_of(tx, ty, sx, sy);
        const f32 lx = sx - vx_, ly = sy - vy_;
        if (lx < -200 || ly < -40 || lx > vw_ || ly > vh_) return;
        labels.push_back({id, name, std::round(lx), std::round(ly), sel, spawn});
    };
    if (view_shown_ && vw_ > 0) {
        const world::Rect view = camera_.visible_tiles(static_cast<u32>(vw_), static_cast<u32>(vh_));
        for (const level::Area& r : a.areas)
            if (r.x1 >= view.x0 && r.x0 <= view.x1 && r.y1 >= view.y0 && r.y0 <= view.y1)
                label(level::area_id_text(r.id), r.name, std::max<f64>(r.x0, camera_.x - vw_ / camera_.zoom * 0.5), r.y0, r.id == zn_area_, false);
        if (a.spawn) label("spawn", "Старт героя", a.spawn_x - 0.5, a.spawn_y - 2, zn_spawn_, true);
    }
    if (labels != m_zn_labels_) {
        m_zn_labels_ = std::move(labels);
        model_.DirtyVariable("lv_zn_labels");
    }
    // The music drop-down is not bound: its options come from a list, and a
    // select whose value is not (yet) among them picks another, which a
    // binding would write over the area. It is set from here, not while its
    // list is open.
    const Rml::String music = z ? Rml::String(z->music) : Rml::String();
    if (m_zn_music_ != music) m_zn_music_ = music;
    for (int i = 0; context && i < context->GetNumDocuments(); ++i)
        if (auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(context->GetDocument(i)->GetElementById("zn-music"))) {
            if (!select->IsSelectBoxVisible() && select->GetValue() != m_zn_music_) {
                const bool was = ui_updating_;
                ui_updating_ = true; // not the author's pick
                select->SetValue(m_zn_music_);
                ui_updating_ = was;
            }
            break;
        }
}

void LevelEditor::push_areas(f64 ox, f64 oy, f64 px) {
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
    auto frame = [&](f64 x0, f64 y0, f64 x1, f64 y1, f64 t, u32 color, u32 order) {
        quad(x0, y0, x1 - x0, t, color, order);
        quad(x0, y1 - t, x1 - x0, t, color, order);
        quad(x0, y0, t, y1 - y0, color, order);
        quad(x1 - t, y0, t, y1 - y0, color, order);
    };
    auto handle = [&](f64 x, f64 y, f64 size, u32 color, u32 order) { quad(x - size * 0.5, y - size * 0.5, size, size, color, order); };
    const world::Rect view = camera_.visible_tiles(static_cast<u32>(vw_), static_cast<u32>(vh_));
    const u32 gold = render::pack_color(255, 210, 90, 255), white = render::pack_color(255, 255, 255, 230);
    const level::LevelAreas& a = shown_areas();
    f64 hx = 0, hy = 0;
    const bool hovering = hover_ && !panning_ && zn_drag_ == AreaDrag::None;
    if (hovering) to_tile(mouse_x_, mouse_y_, hx, hy);
    const u64 hovered = hovering && zn_tool_ == AreaTool::Select ? area_at(a, hx, hy) : 0;
    // Each area: a light fill in its own colour and its outline; the selected
    // one in gold, with handles on its corners and edges.
    for (const level::Area& z : a.areas) {
        if (z.x1 < view.x0 || z.x0 > view.x1 || z.y1 < view.y0 || z.y0 > view.y1) continue;
        const u8* c = kAreaHues[z.id % std::size(kAreaHues)];
        const bool sel = z.id == zn_area_;
        // Only what is in view is drawn: an area can be thousands of cells.
        const f64 x0 = std::max<f64>(z.x0, view.x0 - 1), y0 = std::max<f64>(z.y0, view.y0 - 1);
        const f64 x1 = std::min<f64>(z.x1, view.x1 + 1), y1 = std::min<f64>(z.y1, view.y1 + 1);
        quad(x0, y0, x1 - x0, y1 - y0, render::pack_color(c[0], c[1], c[2], sel ? 56 : 36), 6);
        const u32 line = sel ? gold : render::pack_color(c[0], c[1], c[2], z.id == hovered ? 255 : 200);
        frame(z.x0, z.y0, z.x1, z.y1, px * (sel || z.id == hovered ? 3 : 2), line, 7);
        if (sel) {
            const f64 size = std::max(0.25, px * 9);
            const f64 mx = (z.x0 + z.x1) * 0.5, my = (z.y0 + z.y1) * 0.5;
            for (const auto& [x, y] : {std::pair<f64, f64>{z.x0, z.y0}, {z.x1, z.y0}, {z.x0, z.y1}, {z.x1, z.y1}, {mx, z.y0}, {mx, z.y1},
                                       {z.x0, my}, {z.x1, my}}) {
                handle(x, y, size + px * 4, gold, 8);
                handle(x, y, size, white, 8);
            }
        }
    }
    // What the tool is about to make.
    if (zn_drag_ == AreaDrag::Draw) {
        const f64 x0 = std::min(zn_ax_, zn_bx_), x1 = std::max(zn_ax_, zn_bx_) + 1;
        const f64 y0 = std::min(zn_ay_, zn_by_), y1 = std::max(zn_ay_, zn_by_) + 1;
        quad(x0, y0, x1 - x0, y1 - y0, render::pack_color(255, 210, 90, 50), 6);
        frame(x0, y0, x1, y1, px * 2, gold, 7);
    } else if (hovering && zn_tool_ == AreaTool::Draw) {
        frame(std::floor(hx), std::floor(hy), std::floor(hx) + 1, std::floor(hy) + 1, px * 2, render::pack_color(255, 210, 90, 200), 7);
    }
    // The spawn point: the hero standing there (a cell wide, two high), its
    // feet on the point; where it would really stand, if elsewhere, faintly.
    auto hero = [&](f64 x, f64 y, u32 fill, u32 line, f64 t) {
        quad(x - 0.5, y - 2, 1, 2, fill, 9);
        frame(x - 0.5, y - 2, x + 0.5, y, t, line, 9);
        quad(x - 0.8, y - t, 1.6, t * 2, line, 9);
    };
    if (a.spawn) {
        hero(a.spawn_x, a.spawn_y, render::pack_color(255, 120, 200, 110), zn_spawn_ ? gold : render::pack_color(255, 120, 200, 255),
             px * (zn_spawn_ ? 3 : 2));
        f64 gx = 0, gy = 0;
        if (zn_drag_ != AreaDrag::Spawn && spawn_ground(gx, gy) && (gx != a.spawn_x || gy != a.spawn_y))
            hero(gx, gy, render::pack_color(255, 120, 200, 40), render::pack_color(255, 120, 200, 140), px);
    }
    if (hovering && zn_tool_ == AreaTool::Spawn)
        hero(std::floor(hx) + 0.5, std::floor(hy) + 1, render::pack_color(255, 120, 200, 50), render::pack_color(255, 120, 200, 160), px);
}

// --- actions -----------------------------------------------------------------

void LevelEditor::undo() {
    if (gesture() || tm_open_) return; // the import window shows the level as it is
    if (!history_.can_undo()) return;
    edit_begins();
    if (history_.undo()) FORGE_INFO("Отменено");
}

void LevelEditor::redo() {
    if (gesture() || tm_open_) return;
    if (!history_.can_redo()) return;
    edit_begins();
    if (history_.redo()) FORGE_INFO("Повторено");
}

bool LevelEditor::save() {
    if (stroke_) release();
    cancel_gesture();
    // What the trial poured is not the author's level.
    edit_begins();
    const level::Level::SaveReport r = level_->save();
    if (!r.ok) {
        FORGE_ERROR("Уровень не сохранился: %s. Изменения остались в редакторе, попробуйте ещё раз", r.error.c_str());
        return false;
    }
    history_.mark_saved();
    FORGE_INFO("Уровень сохранён: участков с плитками %u, с объектами %u%s%s (%.0f мс)", r.tile_chunks, r.object_chunks,
               r.physics ? ", гравитация мира в physics.json" : "", r.areas ? ", зоны в areas.json" : "", r.ms);
    return true;
}

bool LevelEditor::open_folder(const fs::path& folder) {
    cancel_tiled(); // it was worked out for the level that goes
    if (stroke_) release();
    cancel_gesture();
    reset_trial();
    moving_ = false;
    select_objects({});
    const fs::path before = level_->folder();
    std::string error;
    if (!level_->open(folder, &error)) {
        FORGE_ERROR("Уровень %s не открылся: %s", path_to_utf8(folder).c_str(), error.c_str());
        level_->open(before, nullptr);
        return false;
    }
    config_.folder = folder;
    look_around_level();
    history_.clear();
    zn_area_ = zn_ask_ = 0;
    zn_spawn_ = false;
    zn_spawn_key_ = ~0ull;
    zn_note_.clear();
    if (mode_ == Mode::Zones) scan_sounds();
    view_.preview_time = -1;
    fields_built_ = 0;
    map_edits_ = ~0ull;
    FORGE_INFO("Уровень открыт: %s", path_to_utf8(folder).c_str());
    return true;
}

std::filesystem::path LevelEditor::play_dir() const {
    return config_.play_dir.empty() ? fs::temp_directory_path() / "forge_editor_play" : config_.play_dir;
}

std::vector<std::string> LevelEditor::play_command(f64 x, f64 y) const {
    char at[64];
    std::snprintf(at, sizeof(at), "%.2f,%.2f", x, y);
    std::vector<std::string> cmd = {path_to_utf8(config_.game_exe), "--play", "--level", path_to_utf8(level_->folder()),
                                    "--at", at, "--user", path_to_utf8(play_dir()),
                                    "--fired", path_to_utf8(fired_file())};
    if (!config_.game_data.empty()) {
        cmd.push_back("--data");
        cmd.push_back(path_to_utf8(config_.game_data));
    }
    return cmd;
}

bool LevelEditor::play_here() {
    if (stroke_) release();
    if (!save()) {
        FORGE_ERROR("Игра не запущена: уровень не сохранился, а игра начинает с сохранённого");
        return false;
    }
    f64 x = 0, y = 0;
    if (!module_.play_spot(*level_, camera_.x, camera_.y, x, y)) {
        FORGE_WARN("Рядом с центром вида нет места для героя: сдвиньте вид");
        return false;
    }
    last_play_ = play_command(x, y);
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
    const std::string place = place_name(camera_.x, camera_.y);
    const char* tool = mode_ == Mode::Physics ? phys_info(ph_tool_).name
                       : mode_ == Mode::Light ? light_info(lt_tool_).name
                       : mode_ == Mode::Zones ? area_info(zn_tool_).name
                                              : tool_name(tool_);
    if (hover_)
        std::snprintf(text, sizeof(text), "Клетка %d, %d%s%s · %s", hover_x_, hover_y_, place.empty() ? "" : " · ",
                      place.c_str(), tool);
    else
        std::snprintf(text, sizeof(text), "Центр %.0f, %.0f%s%s · %s", camera_.x, camera_.y, place.empty() ? "" : " · ",
                      place.c_str(), tool);
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
        if (trial_.running()) {
            // What the trial remembered stays loaded, wherever the view goes.
            const world::Rect both[2] = {focus, trial_.keep()};
            level_->update({both, 2});
            trial_.update(dt);
            map_edits_ = ~0ull; // the water moves on the minimap too
        } else {
            level_->update({&focus, 1});
        }
        update_minimap();
    }
    if (level_->own_tiles_version() != own_tiles_version_) build_tiles();
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
    sync_areas(context);
    sync_tiled();
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
                for (u32 l = layers; l-- > 0 && c == 0;) {
                    const world::TileId v = w.tile(l, tx, ty);
                    c = v >= level::kFirstOwnTile && static_cast<i32>(l) != module_.liquids_layer() ? own_color(v) : module_.map_color(l, v);
                }
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

bool LevelEditor::minimap_at(i32 x, i32 y, u8 rgb[3]) const {
    if (minimap_updates_ == 0) return false;
    const i32 half = static_cast<i32>(kMapPx / 2) * kMapTilesPerPx;
    const i32 dx = x - (static_cast<i32>(map_cx_) - half), dy = y - (static_cast<i32>(map_cy_) - half);
    if (dx < 0 || dy < 0 || dx % kMapTilesPerPx != 0 || dy % kMapTilesPerPx != 0) return false;
    const u32 px = static_cast<u32>(dx / kMapTilesPerPx), py = static_cast<u32>(dy / kMapTilesPerPx);
    if (px >= kMapPx || py >= kMapPx) return false;
    std::memcpy(rgb, &map_rgba_[(static_cast<usize>(py) * kMapPx + px) * 4], 3);
    return true;
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
    const auto& tiles = tiles_;
    if (tile_ < tiles.size()) {
        const level::TileDef& t = tiles[tile_];
        set(m_tile_name_, Rml::String(t.name), "lv_tile_name");
        set(m_tile_hint_, Rml::String(t.hint), "lv_tile_hint");
        set(m_tile_icon_, Rml::String("/memory/" + (tile_ < tile_images_.size() ? tile_images_[tile_] : "tile_" + t.id)), "lv_tile_icon");
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
    const PhysToolInfo& pt = phys_info(ph_tool_);
    set(m_ph_tool_, Rml::String(pt.id), "lv_ph_tool");
    set(m_ph_tool_name_, Rml::String(pt.name), "lv_ph_tool_name");
    set(m_ph_tool_help_, Rml::String(pt.help), "lv_ph_tool_help");
    const PullDir pull = pull_dir();
    set(m_ph_dir_, Rml::String(kPullNames[static_cast<int>(pull)]), "lv_ph_dir");
    set(m_ph_strength_, Rml::String(number(pull_strength())), "lv_ph_strength");
    {
        const level::LevelPhysics& p = level_->physics();
        const std::string world =
            pull == PullDir::None
                ? "Гравитации мира нет: тела парят, вода и песок стоят на месте. Точки гравитации тянут только тела."
                : "Тела: " + number(p.gravity_x) + ", " + number(p.gravity_y) + " тайлов/с². Вода и песок падают " +
                      kPullNames[static_cast<int>(pull)] +
                      " — по главной оси гравитации мира; точки гравитации на них не действуют, только на тела (героя, "
                      "предметы, ящики, зверьков).";
        set(m_ph_world_, Rml::String(world), "lv_ph_world");
    }
    set(m_ph_error_,
        Rml::String(level_->physics_error().empty() ? std::string()
                                                    : level_->physics_error() + ". Действует гравитация игры; файл "
                                                                                "перепишется, когда вы её измените"),
        "lv_ph_error");
    set(m_ph_trial_, trial_.running(), "lv_ph_trial");
    {
        std::string t;
        if (trial_.running())
            t = trial_.still() ? "Идёт проба: гравитации мира нет, ничего не течёт."
                               : "Идёт проба: " + std::to_string(trial_.ticks()) + " тиков, клеток сдвинулось за тик: " +
                                     std::to_string(trial_.moved()) + ". «Сбросить» вернёт уровень как был.";
        else
            t = "Вода и песок поплывут прямо здесь, по гравитации мира. История и сохранённый уровень не меняются.";
        set(m_ph_trial_text_, Rml::String(t), "lv_ph_trial_text");
    }
    set(m_ph_note_, Rml::String(ph_note_), "lv_ph_note");
    const LightToolInfo& lt = light_info(lt_tool_);
    set(m_lt_tool_, Rml::String(lt.id), "lv_lt_tool");
    set(m_lt_tool_name_, Rml::String(lt.name), "lv_lt_tool_name");
    set(m_lt_tool_help_, Rml::String(lt.help), "lv_lt_tool_help");
    {
        const f64 h = level_->light().time;
        set(m_lt_time_, Rml::String(level::clock_text(h)), "lv_lt_time");
        set(m_lt_hours_, Rml::String(number(std::min<f64>(h, kLastHour))), "lv_lt_hours");
        set(m_lt_sky_, Rml::String(module_.hour_words(h)), "lv_lt_sky"); // the game's words, its night among them
        const bool previewing = view_.preview_time >= 0;
        const f64 ph = previewing ? view_.preview_time : h;
        set(m_lt_previewing_, previewing, "lv_lt_previewing");
        set(m_lt_preview_, Rml::String(level::clock_text(ph)), "lv_lt_preview");
        set(m_lt_preview_hours_, Rml::String(number(std::min<f64>(ph, kLastHour))), "lv_lt_preview_hours");
    }
    set(m_lt_error_,
        Rml::String(level_->light_error().empty() ? std::string()
                                                  : level_->light_error() + ". Действует полдень; файл перепишется, когда вы "
                                                                            "измените время"),
        "lv_lt_error");
    set(m_lt_note_, Rml::String(lt_note_), "lv_lt_note");
    if (time_ - info_time_ > 0.25) {
        info_time_ = time_;
        char coords[96];
        std::snprintf(coords, sizeof(coords), "%.0f, %.0f", camera_.x, camera_.y);
        const std::string place = place_name(camera_.x, camera_.y);
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
    if (mode_ == Mode::Physics) push_physics(ox, oy, px);
    if (mode_ == Mode::Light) push_light(ox, oy, px);
    if (mode_ == Mode::Zones) push_areas(ox, oy, px);
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

void LevelEditor::push_physics(f64 ox, f64 oy, f64 px) {
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
    auto line = [&](f64 x0, f64 y0, f64 x1, f64 y1, f64 t, u32 color, u32 order) {
        const f64 dx = x1 - x0, dy = y1 - y0, len = std::hypot(dx, dy);
        if (len <= 0) return;
        render::Sprite s;
        s.x = static_cast<f32>((x0 + x1) * 0.5 - ox);
        s.y = static_cast<f32>((y0 + y1) * 0.5 - oy);
        s.w = static_cast<f32>(len);
        s.h = static_cast<f32>(t);
        s.angle = static_cast<f32>(std::atan2(dy, dx));
        s.frame = demo::kFrameSolid;
        s.color = color;
        s.order = order;
        front_batch_.push(s);
    };
    auto circle = [&](f64 cx, f64 cy, f64 r, f64 t, u32 color, u32 order) {
        const i32 n = std::clamp(static_cast<i32>(r / px / 6), 24, 160);
        for (i32 i = 0; i < n; ++i) {
            const f64 a0 = 6.283185307179586 * i / n, a1 = 6.283185307179586 * (i + 1) / n;
            line(cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1), cy + r * std::sin(a1), t, color, order);
        }
    };
    auto handle = [&](f64 x, f64 y, f64 size, u32 color, u32 order) { quad(x - size * 0.5, y - size * 0.5, size, size, color, order); };
    const world::Rect view = camera_.visible_tiles(static_cast<u32>(vw_), static_cast<u32>(vh_));
    const u64 selected = selection_.empty() ? 0 : selection_[0];

    // The points: a circle where they act, the centre to grab, and for the
    // selected one a handle on the circle for the radius.
    std::vector<sim::GravityField::Placed> placed;
    f64 hx = 0, hy = 0;
    const bool hovering = hover_ && !panning_ && ph_drag_ == PhysDrag::None;
    if (hovering) to_tile(mouse_x_, mouse_y_, hx, hy);
    const flecs::entity hovered = hovering && ph_tool_ == PhysTool::Select ? point_at(hx, hy, false) : flecs::entity();
    level_->scene().ecs().each([&](flecs::entity e, const scene::Position& p, const sim::GravitySource& g) {
        const f64 x = p.tile_x(), y = p.tile_y(), r = g.radius;
        placed.push_back({x, y, g});
        if (x + r < view.x0 || x - r > view.x1 || y + r < view.y0 || y - r > view.y1) return;
        const bool sel = selected && level_->id_of(e, false) == selected;
        const u32 ring = sel ? render::pack_color(255, 210, 90, 255)
                         : g.strength < 0 ? render::pack_color(255, 140, 70, 220)
                                          : render::pack_color(90, 220, 255, 220);
        if (g.replace) {
            // The world's pull is off inside: the area is tinted.
            const u32 tint = render::pack_color(90, 220, 255, 40);
            for (f64 yy = -r; yy < r; yy += 0.5) {
                const f64 half = std::sqrt(std::max(0.0, r * r - (yy + 0.25) * (yy + 0.25)));
                quad(x - half, y + yy, half * 2, 0.5, tint, 6);
            }
        }
        circle(x, y, r, px * (sel ? 3 : 2), ring, 7);
        if (g.fade) circle(x, y, r * 0.5, px, render::pack_color(255, 255, 255, 70), 7);
        handle(x, y, std::max(0.3, px * 9), ring, 8);
        if (e == hovered) circle(x, y, std::max(0.6, px * 9), px * 2, render::pack_color(255, 255, 255, 200), 8);
        if (sel) handle(x + r, y, std::max(0.3, px * 9), render::pack_color(255, 255, 255, 255), 8);
    });

    // What a body feels inside the points: the world's pull and theirs, as
    // GravityField adds them (the same field the game builds).
    if (!placed.empty()) {
        sim::GravityField field;
        const level::LevelPhysics& lp = level_->physics();
        field.set_world(lp.gravity_x, lp.gravity_y);
        field.build(placed);
        const f64 step = std::max(1.0, std::ceil(30.0 * px));
        const f64 x0 = std::floor(view.x0 / step) * step, y0 = std::floor(view.y0 / step) * step;
        u32 drawn = 0;
        for (f64 y = y0; y <= view.y1 && drawn < 3000; y += step)
            for (f64 x = x0; x <= view.x1 && drawn < 3000; x += step) {
                const f64 cx = x + 0.5, cy = y + 0.5;
                bool inside = false;
                for (const auto& s : placed) inside = inside || std::hypot(s.x - cx, s.y - cy) <= s.source.radius;
                if (!inside) continue;
                f32 gx = 0, gy = 0;
                field.at(cx, cy, gx, gy);
                const f64 g = std::hypot(gx, gy);
                if (g < 1e-3) {
                    handle(cx, cy, px * 3, render::pack_color(255, 255, 255, 120), 9); // no pull here
                    ++drawn;
                    continue;
                }
                const f64 len = step * 0.85 * std::min(1.0, 0.25 + g / level::kMaxGravity);
                const f64 ux = gx / g, uy = gy / g;
                const f64 ex = cx + ux * len, ey = cy + uy * len;
                const u32 c = render::pack_color(255, 255, 255, 170);
                line(cx, cy, ex, ey, px * 1.5, c, 9);
                const f64 head = std::min(len * 0.4, px * 8);
                line(ex, ey, ex - (ux * 0.87 - uy * 0.5) * head, ey - (uy * 0.87 + ux * 0.5) * head, px * 1.5, c, 9);
                line(ex, ey, ex - (ux * 0.87 + uy * 0.5) * head, ey - (uy * 0.87 - ux * 0.5) * head, px * 1.5, c, 9);
                ++drawn;
            }
    }

    // What the tool is about to make.
    if (ph_drag_ == PhysDrag::Place) {
        const f64 r = ph_r_ < 0.5 ? kNewRadius : std::clamp(std::round(ph_r_ * 2) / 2, kMinRadius, kMaxRadius);
        circle(ph_cx_, ph_cy_, r, px * 2, render::pack_color(255, 210, 90, 255), 7);
        handle(ph_cx_, ph_cy_, std::max(0.3, px * 9), render::pack_color(255, 210, 90, 255), 8);
    } else if (hovering && ph_tool_ == PhysTool::Point) {
        handle(std::floor(hx) + 0.5, std::floor(hy) + 0.5, std::max(0.3, px * 9), render::pack_color(255, 210, 90, 200), 8);
    }
    if (ph_drag_ == PhysDrag::Area || (hovering && (ph_tool_ == PhysTool::Water || ph_tool_ == PhysTool::Sand))) {
        const i32 ax = ph_drag_ == PhysDrag::Area ? std::min(ph_ax_, ph_bx_) : hover_x_;
        const i32 ay = ph_drag_ == PhysDrag::Area ? std::min(ph_ay_, ph_by_) : hover_y_;
        const i32 bx = ph_drag_ == PhysDrag::Area ? std::max(ph_ax_, ph_bx_) : hover_x_;
        const i32 by = ph_drag_ == PhysDrag::Area ? std::max(ph_ay_, ph_by_) : hover_y_;
        const bool water = ph_tool_ == PhysTool::Water;
        const u32 fill = water ? render::pack_color(60, 130, 255, 90) : render::pack_color(230, 200, 110, 100);
        const u32 edge = water ? render::pack_color(120, 180, 255, 230) : render::pack_color(255, 220, 140, 230);
        const f64 w = bx - ax + 1, h = by - ay + 1, t = px * 2;
        quad(ax, ay, w, h, fill, 6);
        quad(ax, ay, w, t, edge, 7);
        quad(ax, ay + h - t, w, t, edge, 7);
        quad(ax, ay, t, h, edge, 7);
        quad(ax + w - t, ay, t, h, edge, 7);
    }
}

void LevelEditor::push_light(f64 ox, f64 oy, f64 px) {
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
    auto line = [&](f64 x0, f64 y0, f64 x1, f64 y1, f64 t, u32 color, u32 order) {
        const f64 dx = x1 - x0, dy = y1 - y0, len = std::hypot(dx, dy);
        if (len <= 0) return;
        render::Sprite s;
        s.x = static_cast<f32>((x0 + x1) * 0.5 - ox);
        s.y = static_cast<f32>((y0 + y1) * 0.5 - oy);
        s.w = static_cast<f32>(len);
        s.h = static_cast<f32>(t);
        s.angle = static_cast<f32>(std::atan2(dy, dx));
        s.frame = demo::kFrameSolid;
        s.color = color;
        s.order = order;
        front_batch_.push(s);
    };
    auto circle = [&](f64 cx, f64 cy, f64 r, f64 t, u32 color, u32 order) {
        const i32 n = std::clamp(static_cast<i32>(r / px / 6), 24, 160);
        for (i32 i = 0; i < n; ++i) {
            const f64 a0 = 6.283185307179586 * i / n, a1 = 6.283185307179586 * (i + 1) / n;
            line(cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1), cy + r * std::sin(a1), t, color, order);
        }
    };
    auto handle = [&](f64 x, f64 y, f64 size, u32 color, u32 order) { quad(x - size * 0.5, y - size * 0.5, size, size, color, order); };
    auto byte = [](f32 v) { return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    const world::Rect view = camera_.visible_tiles(static_cast<u32>(vw_), static_cast<u32>(vh_));
    const u64 selected = selection_.empty() ? 0 : selection_[0];
    f64 hx = 0, hy = 0;
    const bool hovering = hover_ && !panning_ && ph_drag_ == PhysDrag::None;
    if (hovering) to_tile(mouse_x_, mouse_y_, hx, hy);
    const flecs::entity hovered = hovering && lt_tool_ == LightTool::Select ? point_at(hx, hy, false) : flecs::entity();
    // Each source: its circle (where its light ends) in its own colour, the
    // centre to grab in it, and for the selected one a handle for the radius.
    level_->scene().ecs().each([&](flecs::entity e, const scene::Position& p, const level::LightSource& s) {
        const render::PointLight l = level::point_light(p.tile_x(), p.tile_y(), s);
        const f64 x = l.x, y = l.y, r = l.radius;
        if (x + r < view.x0 || x - r > view.x1 || y + r < view.y0 || y - r > view.y1) return;
        const bool sel = selected && level_->id_of(e, false) == selected;
        // A dark colour still shows: the ring takes its hue at full strength.
        const f32 top = std::max({s.r, s.g, s.b, 0.0f});
        const u32 hue = top > 0.02f ? render::pack_color(byte(s.r / top), byte(s.g / top), byte(s.b / top), 230)
                                    : render::pack_color(160, 160, 160, 230);
        const u32 ring = sel ? render::pack_color(255, 210, 90, 255) : hue;
        circle(x, y, r, px * (sel ? 3 : 2), ring, 7);
        if (sel) circle(x, y, r - px * 3, px, hue, 7);
        const f64 size = std::max(0.3, px * 11);
        handle(x, y, size + px * 4, render::pack_color(255, 255, 255, 230), 8);
        handle(x, y, size, hue | 0xff000000u, 8);
        if (e == hovered) circle(x, y, std::max(0.6, px * 9), px * 2, render::pack_color(255, 255, 255, 200), 8);
        if (sel) handle(x + r, y, std::max(0.3, px * 9), render::pack_color(255, 255, 255, 255), 8);
    });
    // What the tool is about to make.
    if (ph_drag_ == PhysDrag::Place) {
        const f64 r = ph_r_ < 0.5 ? kLightNewRadius : std::clamp(std::round(ph_r_ * 2) / 2, kMinRadius, ring_max());
        circle(ph_cx_, ph_cy_, r, px * 2, render::pack_color(255, 210, 90, 255), 7);
        handle(ph_cx_, ph_cy_, std::max(0.3, px * 9), render::pack_color(255, 210, 90, 255), 8);
    } else if (hovering && lt_tool_ == LightTool::Source) {
        handle(std::floor(hx) + 0.5, std::floor(hy) + 0.5, std::max(0.3, px * 9), render::pack_color(255, 210, 90, 200), 8);
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
        hover_ = over_view(x, y, context) || stroke_ != nullptr || moving_ || ph_drag_ != PhysDrag::None || zn_drag_ != AreaDrag::None;
        if (hover_) cell_at(x, y, hover_x_, hover_y_);
        if (stroke_) drag(x, y);
        if (moving_) drag_objects(x, y);
        if (ph_drag_ != PhysDrag::None) drag_physics(x, y);
        if (zn_drag_ != AreaDrag::None) drag_areas(x, y);
        return hover_;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const f32 x = e.button.x * density, y = e.button.y * density;
        mouse_x_ = x;
        mouse_y_ = y;
        if (dock_.busy()) return true;
        // The right button takes back a drag of the physics, light and zones tools.
        if (e.button.button == SDL_BUTTON_RIGHT && (ph_drag_ != PhysDrag::None || zn_drag_ != AreaDrag::None)) return cancel_gesture();
        if (!over_view(x, y, context)) return false;
        if (e.button.button == SDL_BUTTON_LEFT && !panning_ && mode_ == Mode::Zones) {
            if (zn_drag_ == AreaDrag::None && !press_areas(x, y)) {
                panning_ = true; // empty space: the drag moves the view
                pan_button_ = SDL_BUTTON_LEFT;
            }
        } else if (e.button.button == SDL_BUTTON_LEFT && !panning_ && (mode_ == Mode::Physics || mode_ == Mode::Light)) {
            if (ph_drag_ == PhysDrag::None && !(mode_ == Mode::Light ? press_light(x, y) : press_physics(x, y))) {
                panning_ = true; // empty space: the drag moves the view
                pan_button_ = SDL_BUTTON_LEFT;
            }
        } else if (e.button.button == SDL_BUTTON_LEFT && !panning_ && mode_ == Mode::Tiles) {
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
        // A slider's drag in the panels ends with the button: the next drag is an entry of its own.
        if (e.button.button == SDL_BUTTON_LEFT) history_.seal();
        if (e.button.button == SDL_BUTTON_LEFT && stroke_) {
            release();
            used = true;
        }
        if (e.button.button == SDL_BUTTON_LEFT && moving_) {
            moving_ = false;
            history_.seal();
            used = true;
        }
        if (e.button.button == SDL_BUTTON_LEFT && ph_drag_ != PhysDrag::None) {
            release_physics();
            used = true;
        }
        if (e.button.button == SDL_BUTTON_LEFT && zn_drag_ != AreaDrag::None) {
            release_areas();
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
    // The import window holds the keys: Esc is its «Отмена».
    if (tm_open_) {
        if (k.key == SDLK_ESCAPE) cancel_tiled();
        return true;
    }
    const bool ctrl = (k.mod & SDL_KMOD_CTRL) != 0;
    if (ctrl) return false;
    if (k.key == SDLK_F5) {
        play_here();
        return true;
    }
    if (k.key == SDLK_Q) { set_mode(Mode::Select); return true; }
    if (k.key == SDLK_T) { set_mode(Mode::Tiles); return true; }
    if (k.key == SDLK_O) { set_mode(Mode::Objects); return true; }
    if (k.key == SDLK_P) { set_mode(Mode::Physics); return true; }
    if (k.key == SDLK_C) { set_mode(Mode::Light); return true; } // С: «Свет» on a Russian keyboard
    if (k.key == SDLK_Z) { set_mode(Mode::Zones); return true; }
    if (mode_ == Mode::Zones) {
        if (k.key == SDLK_ESCAPE) {
            if (cancel_gesture()) return true;
            if (zn_ask_) {
                zn_ask_ = 0;
                zn_note_ = "Зона осталась";
            } else if (zn_tool_ != AreaTool::Select) {
                set_area_tool(AreaTool::Select);
            } else {
                select_area(0);
            }
            return true;
        }
        if (gesture()) return false;
        if (k.key == SDLK_DELETE || k.key == SDLK_BACKSPACE) {
            delete_area(false);
            return true;
        }
        return false;
    }
    if (mode_ == Mode::Light) {
        if (k.key == SDLK_ESCAPE) {
            if (cancel_gesture()) return true;
            if (lt_tool_ != LightTool::Select) set_light_tool(LightTool::Select);
            else select_objects({});
            return true;
        }
        if (gesture()) return false;
        if (k.key == SDLK_DELETE || k.key == SDLK_BACKSPACE) {
            delete_selection();
            return true;
        }
        return false;
    }
    if (mode_ == Mode::Physics) {
        if (k.key == SDLK_ESCAPE) {
            if (cancel_gesture()) return true;
            if (ph_tool_ != PhysTool::Select) set_phys_tool(PhysTool::Select);
            else select_objects({});
            return true;
        }
        if (gesture()) return false;
        if (k.key == SDLK_DELETE || k.key == SDLK_BACKSPACE) {
            delete_selection();
            return true;
        }
        return false;
    }
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
        const auto& tiles = tiles_;
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
