// «Импорт карты Tiled» of the «Уровень» tab (level_editor.h): the window that
// tells everything an import would do before anything changes, and its
// buttons. The work is forge/level/tiled_apply.h's.

#include "level_editor.h"

#include "forge/core/log.h"
#include "forge/core/path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge::editor_app {

namespace fs = std::filesystem;
namespace tl = level::tiled;

namespace {

std::string q(std::string_view s) { return "«" + std::string(s) + "»"; }

std::string digits(u64 n) {
    std::string d = std::to_string(n), out;
    for (usize i = 0; i < d.size(); ++i) {
        if (i > 0 && (d.size() - i) % 3 == 0) out += " ";
        out += d[i];
    }
    return out;
}

// -16 as "−16" (a minus, not a hyphen).
std::string coord(f64 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", std::fabs(v));
    return (v < 0 ? "−" : "") + std::string(b);
}

const char* target_id(tl::Target t) {
    switch (t) {
    case tl::Target::Walls: return "walls";
    case tl::Target::Blocks: return "blocks";
    case tl::Target::Skip: return "skip";
    }
    return "skip";
}

// "Земля и камень.tsx" from "наборы/Земля и камень.tsx".
std::string file_name(const std::string& path) { return path_to_utf8(utf8_path(path).filename()); }

} // namespace

void LevelEditor::bind_tiled(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<TmLine>()) {
        s.RegisterMember("text", &TmLine::text);
        s.RegisterMember("count", &TmLine::count);
        s.RegisterMember("warn", &TmLine::warn);
    }
    model.RegisterArray<std::vector<TmLine>>();
    if (auto s = model.RegisterStruct<TmLayer>()) {
        s.RegisterMember("index", &TmLayer::index);
        s.RegisterMember("name", &TmLayer::name);
        s.RegisterMember("about", &TmLayer::about);
        s.RegisterMember("target", &TmLayer::target);
    }
    model.RegisterArray<std::vector<TmLayer>>();
    model.Bind("lv_tm_open", &m_tm_open_);
    model.Bind("lv_tm_around", &m_tm_around_);
    model.Bind("lv_tm_can", &m_tm_can_);
    model.Bind("lv_tm_title", &m_tm_title_);
    model.Bind("lv_tm_where", &m_tm_where_);
    model.Bind("lv_tm_refusal", &m_tm_refusal_);
    model.Bind("lv_tm_note", &m_tm_note_);
    model.Bind("lv_tm_sets", &m_tm_sets_);
    model.Bind("lv_tm_layers", &m_tm_layers_);
    model.Bind("lv_tm_objects", &m_tm_objects_);
    model.Bind("lv_tm_game", &m_tm_game_);
    model.Bind("lv_tm_skipped", &m_tm_skipped_);
    model.Bind("lv_tm_changes", &m_tm_changes_);
    model.Bind("lv_tm_back", &m_tm_back_);
    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    on("lv_tm_pick", [this](Rml::Event&, const Rml::VariantList&) { pick_tiled(); });
    on("lv_tm_target", [this](Rml::Event&, const Rml::VariantList& a) {
        if (a.size() < 2) return;
        const int i = a[0].Get<int>();
        const std::string id = a[1].Get<Rml::String>();
        if (i < 0) return;
        set_tiled_target(static_cast<usize>(i), id == "blocks" ? tl::Target::Blocks : id == "walls" ? tl::Target::Walls : tl::Target::Skip);
    });
    on("lv_tm_around", [this](Rml::Event&, const Rml::VariantList&) { set_tiled_around(!tm_options_.empty_around); });
    on("lv_tm_cancel", [this](Rml::Event&, const Rml::VariantList&) { cancel_tiled(); });
    on("lv_tm_import", [this](Rml::Event&, const Rml::VariantList&) { import_tiled(); });
    on("lv_tm_note_close", [this](Rml::Event&, const Rml::VariantList&) {
        tm_note_.clear();
        ++tm_serial_;
    });
}

void LevelEditor::pick_tiled() {
    if (config_.offscreen) return;
    static const SDL_DialogFileFilter filters[] = {{"Карта Tiled (.tmx)", "tmx"}};
    auto done = [](void* self, const char* const* list, int) {
        if (list && *list) static_cast<LevelEditor*>(self)->tiled_picked(utf8_path(*list));
    };
    SDL_ShowOpenFileDialog(done, this, window, filters, 1, nullptr, false);
}

void LevelEditor::tiled_picked(const fs::path& tmx) {
    std::lock_guard lock(tm_mutex_);
    tm_picked_.push_back(tmx);
}

bool LevelEditor::open_tiled(const fs::path& tmx) {
    const std::string name = path_to_utf8(tmx.filename());
    tl::Map m;
    std::string why;
    if (!tl::read_map(tmx, m, &why)) {
        tm_note_ = "Карта " + q(name) + " не открывается: " + why;
        FORGE_ERROR("%s", tm_note_.c_str());
        ++tm_serial_;
        return false;
    }
    if (stroke_) release();
    cancel_gesture();
    edit_begins();
    tm_map_ = std::move(m);
    tm_targets_.clear();
    for (const tl::Layer& l : tm_map_.layers) tm_targets_.push_back(tl::default_target(tm_map_, l));
    tm_options_ = {};
    tm_open_ = false;
    refigure_tiled();
    if (!tm_refusal_.empty()) {
        tm_note_ = "Карту " + q(name) + " нельзя импортировать: " + tm_refusal_;
        FORGE_ERROR("%s", tm_note_.c_str());
        tm_map_ = {};
        tm_plan_ = {};
        ++tm_serial_;
        return false;
    }
    tm_open_ = true;
    tm_note_.clear();
    FORGE_INFO("Карта %s прочитана: %s; ничего не изменено до «Импортировать»", name.c_str(), path_to_utf8(tmx).c_str());
    ++tm_serial_;
    return true;
}

void LevelEditor::refigure_tiled() {
    tl::Options o;
    o.targets = tm_targets_;
    o.walls = tm_options_.walls;
    o.blocks = tm_options_.blocks;
    o.layer_count = static_cast<u32>(module_.layer_names().size());
    o.liquids = module_.liquids_layer();
    objects::Library* lib = module_.library();
    o.template_taken = [lib](const tl::Picture& pic) { return lib && tl::template_taken(*lib, pic); };
    std::string why;
    tm_refusal_.clear();
    tm_preview_ = {};
    if (!tl::plan(tm_map_, o, level_->own_tiles(), level_->areas(), level_->tiled_record(), tm_plan_, &why)) {
        tm_refusal_ = why;
        tm_plan_ = {};
    } else if (!lib) {
        tm_refusal_ = "у игры нет библиотеки объектов: шаблонам «Картинка» негде быть";
    } else {
        tm_preview_ = tl::preview(*level_, *lib, tm_plan_, tm_options_);
        tm_refusal_ = tm_preview_.refusal;
    }
    ++tm_serial_;
}

void LevelEditor::set_tiled_target(usize layer, tl::Target t) {
    if (!tm_open_ || layer >= tm_targets_.size() || tm_map_.layers[layer].kind != tl::Layer::Kind::Tiles || tm_targets_[layer] == t) return;
    tm_targets_[layer] = t;
    refigure_tiled();
}

void LevelEditor::set_tiled_around(bool empty) {
    if (!tm_open_ || tm_options_.empty_around == empty) return;
    tm_options_.empty_around = empty;
    refigure_tiled();
}

void LevelEditor::cancel_tiled() {
    if (!tm_open_) return;
    tm_open_ = false;
    tm_note_ = "Импорт карты " + q(tm_plan_.map_name) + " отменён: ничего не изменилось";
    FORGE_INFO("%s", tm_note_.c_str());
    tm_map_ = {};
    tm_plan_ = {};
    tm_preview_ = {};
    ++tm_serial_;
}

bool LevelEditor::import_tiled() {
    if (!tm_open_ || !tm_refusal_.empty()) return false;
    objects::Library* lib = module_.library();
    if (!lib) return false;
    tl::Plan plan = tm_plan_;
    tl::Resources made;
    std::string why;
    const fs::path sounds = sounds_folder_.empty() ? lib->sounds_folder() : sounds_folder_;
    if (!tl::write_resources(plan, *lib, sounds, made, &why)) {
        // The window stays: the author sees why and may cancel.
        tm_note_ = "Импорт не сделан: " + why;
        FORGE_ERROR("%s", tm_note_.c_str());
        ++tm_serial_;
        return false;
    }
    const usize entries = history_.cursor();
    if (!tl::apply(*level_, history_, *lib, plan, tm_options_, &why)) {
        tm_note_ = "Импорт не сделан: " + why + ". Уровень не изменился; шаблоны «Картинка», записанные для него, остались в «Объектах»";
        FORGE_ERROR("%s", tm_note_.c_str());
        ++tm_serial_;
        return false;
    }
    tm_open_ = false;
    const bool level_changed = history_.cursor() != entries;
    std::string game;
    if (!made.templates.empty() || !made.sounds.empty())
        game = "в игре записано: шаблонов «Картинка» " + std::to_string(made.templates.size()) + ", музыки " + std::to_string(made.sounds.size());
    if (!level_changed) {
        // The level already has everything as the map says; the game may have got newer pictures.
        tm_note_ = "Карта " + q(plan.map_name) + " уже в уровне такой, как есть: " + (game.empty() ? "ничего не изменилось" : "уровень не изменился, " + game);
    } else {
        select_objects({});
        // The view goes to the map: its spawn point, else its middle.
        if (plan.areas.spawn) {
            camera_.x = plan.areas.spawn_x;
            camera_.y = plan.areas.spawn_y - 2;
        } else if (plan.x1 > plan.x0) {
            camera_.x = (plan.x0 + plan.x1) * 0.5;
            camera_.y = (plan.y0 + plan.y1) * 0.5;
        }
        tm_note_ = "Карта " + q(plan.map_name) + " в уровне: клеток изменено " + digits(tm_preview_.cells) + ", объектов " +
                   std::to_string(plan.objects.size()) + ", зон " + std::to_string(plan.record.zones.size()) +
                   ". Ctrl+Z отменит всё сразу, Ctrl+S запишет уровень" + (game.empty() ? "" : "; " + game);
    }
    FORGE_INFO("%s", tm_note_.c_str());
    tm_map_ = {};
    tm_plan_ = {};
    ++tm_serial_;
    return true;
}

void LevelEditor::sync_tiled() {
    {
        std::vector<fs::path> picked;
        {
            std::lock_guard lock(tm_mutex_);
            picked.swap(tm_picked_);
        }
        for (const fs::path& p : picked) open_tiled(p);
    }
    if (tm_synced_ == tm_serial_) return;
    tm_synced_ = tm_serial_;
    const tl::Plan& p = tm_plan_;
    const tl::Preview& pv = tm_preview_;
    m_tm_open_ = tm_open_;
    m_tm_around_ = tm_options_.empty_around;
    m_tm_can_ = tm_open_ && tm_refusal_.empty();
    m_tm_refusal_ = tm_refusal_;
    m_tm_note_ = tm_note_;
    m_tm_title_ = "Импорт карты Tiled " + q(p.map_name.empty() ? path_to_utf8(tm_map_.file.filename()) : p.map_name);
    // A closed window keeps its rows (they go with it, not before it).
    if (tm_open_) {
        m_tm_sets_.clear();
        m_tm_layers_.clear();
        m_tm_objects_.clear();
        m_tm_game_.clear();
        m_tm_skipped_.clear();
        m_tm_changes_.clear();
        m_tm_back_.clear();
        const tl::Map& m = tm_map_;
        // Where it goes.
        if (p.x1 > p.x0)
            m_tm_where_ = std::string(m.infinite ? "Бесконечная карта" : "Карта") + ", клетка " + std::to_string(p.px) + " px: " +
                          std::to_string(p.x1 - p.x0) + " × " + std::to_string(p.y1 - p.y0) + " клеток лягут в уровень в x " +
                          coord(p.x0) + "…" + coord(p.x1 - 1) + ", y " + coord(p.y0) + "…" + coord(p.y1 - 1) +
                          " (клетка карты — клетка уровня)";
        else
            m_tm_where_ = "В карте нет клеток, которые переносятся";
        for (const tl::Tileset& ts : m.tilesets) {
            TmLine l;
            l.text = (ts.name.empty() ? file_name(ts.source) : ts.name) + (ts.source.empty() ? " — в карте" : " — " + ts.source);
            if (!ts.missing.empty()) {
                l.count = "нет файла";
                l.warn = true;
            } else {
                l.count = ts.collection() ? std::to_string(ts.tiles.size()) + " картинок" : std::to_string(ts.count) + " тайлов";
            }
            m_tm_sets_.push_back(std::move(l));
        }
        for (usize i = 0; i < m.layers.size(); ++i) {
            const tl::Layer& l = m.layers[i];
            TmLayer row;
            row.index = static_cast<int>(i);
            row.name = l.name;
            if (l.kind == tl::Layer::Kind::Tiles) {
                u64 n = 0;
                for (const tl::Chunk& c : l.chunks)
                    n += static_cast<u64>(std::count_if(c.gids.begin(), c.gids.end(), [](u32 g) { return (g & ~tl::kGidFlags) != 0; }));
                row.about = digits(n) + " клеток" + (l.visible ? "" : ", скрытый");
                row.target = target_id(i < tm_targets_.size() ? tm_targets_[i] : tl::Target::Skip);
            } else if (l.kind == tl::Layer::Kind::Objects) {
                row.about = "объектов: " + std::to_string(l.objects.size()) + (l.visible ? "" : ", скрытый");
            } else {
                row.about = "слой-картинка: не переносится";
            }
            m_tm_layers_.push_back(std::move(row));
        }
        // What the objects become.
        for (const auto& [tid, zid] : p.record.zones)
            if (const level::Area* a = p.areas.find(zid))
                m_tm_objects_.push_back({"Зона " + q(a->name) + (a->music.empty() ? "" : ", музыка " + a->music),
                                         "x " + coord(a->x0) + "…" + coord(a->x1 - 1) + ", y " + coord(a->y0) + "…" + coord(a->y1 - 1), false});
        if (p.record.spawn)
            m_tm_objects_.push_back({"Точка появления героя", "x " + coord(p.areas.spawn_x) + ", y " + coord(p.areas.spawn_y), false});
        if (!p.objects.empty())
            m_tm_objects_.push_back({"Объекты-тайлы → «Картинка»", std::to_string(p.objects.size()) + " из " +
                                                                        std::to_string(p.pictures.size()) + " шаблонов", false});
        // What the game gets, and keeps after Ctrl+Z of the level.
        const objects::Library* lib = module_.library();
        for (const tl::Picture& pic : p.pictures) {
            const objects::Template* t = lib ? lib->find(std::string_view(pic.template_id)) : nullptr;
            const char* state = !t ? "новый" : t->picture != tl::picture_file(pic) ? "обновится" : "уже есть";
            m_tm_game_.push_back({"Шаблон «Картинка» " + q(t ? t->name : pic.name), state, false});
        }
        for (const tl::Music& mu : p.music) {
            const fs::path there = (sounds_folder_.empty() && lib ? lib->sounds_folder() : sounds_folder_) / utf8_path(mu.name);
            std::error_code ec;
            m_tm_game_.push_back({"Музыка " + mu.name + " → звуки игры", fs::exists(there, ec) ? "есть с таким именем" : "новая", false});
        }
        for (const tl::Note& n : p.missing) m_tm_skipped_.push_back({n.what, n.count > 1 ? digits(n.count) : "", true});
        for (const tl::Note& n : p.skipped) m_tm_skipped_.push_back({n.what, n.count > 1 ? digits(n.count) : "", false});
        // What changes in the level.
        m_tm_changes_.push_back({"Клеток станет других", digits(pv.cells), false});
        if (pv.author_cells) m_tm_changes_.push_back({"из них ваших правок (не как сделала игра)", digits(pv.author_cells), true});
        if (p.tiles_new || p.tiles_updated)
            m_tm_changes_.push_back({"Свои тайлы уровня: новых, обновится", std::to_string(p.tiles_new) + ", " + std::to_string(p.tiles_updated), false});
        m_tm_changes_.push_back({"Объектов новых", std::to_string(pv.objects_new), false});
        if (!pv.objects_back.empty()) m_tm_changes_.push_back({"вернутся как в карте", std::to_string(pv.objects_back.size()), true});
        if (pv.objects_same) m_tm_changes_.push_back({"останутся как есть", std::to_string(pv.objects_same), false});
        if (pv.objects_removed) m_tm_changes_.push_back({"уберутся (в карте их больше нет)", std::to_string(pv.objects_removed), true});
        m_tm_changes_.push_back({"Зон новых, обновится, уберётся",
                                 std::to_string(p.zones_new) + ", " + std::to_string(p.zones_updated) + ", " + std::to_string(p.zones_removed),
                                 p.zones_removed > 0});
        if (p.areas.spawn != level_->areas().spawn || p.areas.spawn_x != level_->areas().spawn_x || p.areas.spawn_y != level_->areas().spawn_y)
            m_tm_changes_.push_back({"Точка появления героя", p.areas.spawn ? "x " + coord(p.areas.spawn_x) + ", y " + coord(p.areas.spawn_y) : "уберётся",
                                     false});
        if (pv.around)
            m_tm_changes_.push_back({tm_options_.empty_around ? "Вокруг карты станет пусто: мира игры не будет" : "Вокруг карты снова мир игры", "", false});
        if (pv.game_objects) m_tm_changes_.push_back({"Уйдут жители и звери игры", std::to_string(pv.game_objects), false});
        for (const std::string& b : pv.objects_back) m_tm_back_.push_back(b);
    }
    for (const char* name : {"lv_tm_open", "lv_tm_around", "lv_tm_can", "lv_tm_title", "lv_tm_where", "lv_tm_refusal", "lv_tm_note",
                             "lv_tm_sets", "lv_tm_layers", "lv_tm_objects", "lv_tm_game", "lv_tm_skipped", "lv_tm_changes", "lv_tm_back"})
        model_.DirtyVariable(name);
}

} // namespace forge::editor_app
