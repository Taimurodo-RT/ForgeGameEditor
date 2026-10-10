// The game's levels in the «Уровень» tab (level_editor.h): the menu at the left of the level bar. A level of the
// list opens in place of this one after the window about unsaved changes; a new one is made, the open one renamed or
// made the start level, each written into game/levels.json at once (forge/level/levels.h). They are not edits of a
// level: not in its history, not waiting for «Сохранить».

#include "level_editor.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"

#include <yyjson.h>

#include <cstdlib>

namespace forge::editor_app {

namespace fs = std::filesystem;

namespace {

std::string q(std::string_view s) { return "«" + std::string(s) + "»"; }

fs::path remembered_file(const fs::path& root) { return root / ".forge" / "editor.json"; }

// The level's id in <root>/.forge/editor.json; empty without one.
std::string remembered_level(const fs::path& root) {
    std::vector<u8> bytes;
    if (root.empty() || !read_file(remembered_file(root), bytes)) return {};
    yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(bytes.data()), bytes.size(), 0);
    yyjson_val* id = yyjson_obj_get(yyjson_doc_get_root(doc), "level");
    std::string out = yyjson_is_str(id) ? std::string(yyjson_get_str(id), yyjson_get_len(id)) : std::string();
    yyjson_doc_free(doc);
    return out;
}

} // namespace

fs::path level_to_open(const fs::path& root, const fs::path& game) {
    const level::LevelList list = level::read_levels(game);
    const std::string last = remembered_level(root);
    if (!last.empty() && list.find(last)) return level::level_folder(game, last);
    return level::level_folder(game, list.start_level().id);
}

std::string LevelEditor::level_name() const {
    if (const level::LevelEntry* e = levels_.find(level_id_); e && !level_id_.empty()) return e->name;
    return path_to_utf8(level_->folder().filename());
}

void LevelEditor::bind_levels(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<LevelRow>()) {
        s.RegisterMember("id", &LevelRow::id);
        s.RegisterMember("name", &LevelRow::name);
        s.RegisterMember("current", &LevelRow::current);
        s.RegisterMember("start", &LevelRow::start);
    }
    model.RegisterArray<std::vector<LevelRow>>();
    model.Bind("lv_levels", &m_levels_);
    model.Bind("lv_levels_open", &m_levels_open_);
    model.Bind("lv_levels_can", &m_levels_can_);
    model.Bind("lv_levels_note", &m_levels_note_);
    model.Bind("lv_level_name", &m_level_name_);
    model.Bind("lv_level_listed", &m_level_listed_);
    model.Bind("lv_level_is_start", &m_level_is_start_);
    model.Bind("lv_level_naming", &m_level_naming_);
    model.Bind("lv_level_naming_title", &m_level_naming_title_);
    model.Bind("lv_level_name_field", &m_level_name_field_);
    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    on("lv_levels_menu", [this](Rml::Event&, const Rml::VariantList&) { set_levels_menu(!levels_open_); });
    on("lv_levels_close", [this](Rml::Event&, const Rml::VariantList&) {
        set_levels_menu(false);
        cancel_level_name();
    });
    on("lv_levels_note_close", [this](Rml::Event&, const Rml::VariantList&) {
        levels_note_.clear();
        ++levels_serial_;
    });
    on("lv_level_open", [this](Rml::Event&, const Rml::VariantList& a) {
        const int i = a.empty() ? -1 : a[0].Get<int>();
        if (i >= 0 && static_cast<usize>(i) < levels_.levels.size()) choose_level(levels_.levels[static_cast<usize>(i)].id);
    });
    on("lv_level_new", [this](Rml::Event&, const Rml::VariantList&) { begin_new_level(); });
    on("lv_level_rename", [this](Rml::Event&, const Rml::VariantList&) { begin_rename_level(); });
    on("lv_level_start", [this](Rml::Event&, const Rml::VariantList&) { make_start_level(); });
    on("lv_level_play_start", [this](Rml::Event&, const Rml::VariantList&) { play_start(); });
    // The name field: Enter takes it, «Отмена» (and Esc, the editor's) closes it.
    on("lv_level_name_text", [this](Rml::Event& ev, const Rml::VariantList& a) {
        if (ui_updating_ || a.size() < 2 || !a[1].Get<bool>()) return;
        if (Rml::Element* e = ev.GetTargetElement()) e->Blur(); // the keys are the tab's again
        finish_level_name(a[0].Get<Rml::String>());
    });
    on("lv_level_name_ok", [this](Rml::Event& ev, const Rml::VariantList&) {
        Rml::Element* doc = ev.GetTargetElement() ? ev.GetTargetElement()->GetOwnerDocument() : nullptr;
        auto* field = doc ? rmlui_dynamic_cast<Rml::ElementFormControl*>(doc->GetElementById("lv-level-name")) : nullptr;
        finish_level_name(field ? field->GetValue() : std::string(m_level_name_field_));
    });
    on("lv_level_name_cancel", [this](Rml::Event&, const Rml::VariantList&) { cancel_level_name(); });
}

void LevelEditor::sync_levels() {
    if (levels_synced_ == levels_serial_ || !model_) return;
    levels_synced_ = levels_serial_;
    std::vector<LevelRow> rows;
    for (const level::LevelEntry& e : levels_.levels)
        rows.push_back({Rml::String(e.id), Rml::String(e.name), e.id == level_id_, e.id == levels_.start});
    if (rows != m_levels_) {
        m_levels_ = std::move(rows);
        model_.DirtyVariable("lv_levels");
    }
    set(m_levels_open_, levels_open_, "lv_levels_open");
    set(m_levels_can_, !levels_.broken && !config_.game_data.empty(), "lv_levels_can");
    set(m_levels_note_, Rml::String(levels_note_), "lv_levels_note");
    set(m_level_name_, Rml::String(level_name()), "lv_level_name");
    set(m_level_listed_, !level_id_.empty(), "lv_level_listed");
    set(m_level_is_start_, !level_id_.empty() && level_id_ == levels_.start, "lv_level_is_start");
    set(m_level_naming_, Rml::String(naming_), "lv_level_naming");
    set(m_level_naming_title_,
        Rml::String(naming_ == "new" ? "Новый уровень" : naming_ == "rename" ? "Переименовать уровень " + q(level_name()) : ""),
        "lv_level_naming_title");
}

void LevelEditor::set_levels_menu(bool open) {
    if (open) {
        // The list as it is on disk now (another editor of the game, a hand edit).
        if (!config_.game_data.empty()) {
            const bool was_broken = levels_.broken;
            levels_ = level::read_levels(config_.game_data);
            const level::LevelEntry* e = level::level_of_folder(config_.game_data, levels_, level_->folder());
            level_id_ = e ? e->id : std::string();
            if (levels_.broken) levels_note_ = levels_.problem + ": список уровней не меняется, пока его не исправят";
            else if (was_broken) levels_note_.clear(); // put right: what was said of it holds no more
        }
        naming_.clear();
    }
    levels_open_ = open;
    ++levels_serial_;
}

void LevelEditor::leave_level(const std::string& to, std::function<void()> then) {
    if (!dirty()) {
        then();
        return;
    }
    if (!ask_unsaved) { // nobody to ask: as «Сохранить»
        if (save()) then();
        else levels_note_ = "Уровень " + q(level_name()) + " не сохранился: " + save_error_ + "; он остался открытым";
        ++levels_serial_;
        return;
    }
    const std::string from = level_name();
    ask_unsaved(from, to, {"Уровень " + q(from) + ": правки не сохранены"},
                [this](std::string& error) {
                    if (save()) return true;
                    error = "уровень " + q(level_name()) + " не записан: " + save_error_;
                    ++levels_serial_;
                    return false;
                },
                [then](bool) { then(); });
}

bool LevelEditor::open_level(const std::string& id) {
    const fs::path folder = level::level_folder(config_.game_data, id);
    std::error_code ec;
    fs::create_directories(folder, ec);
    const std::string was = level_name();
    if (!open_folder(folder)) {
        levels_note_ = "Уровень не открылся: " + path_to_utf8(folder) + "; открыт " + q(was);
        ++levels_serial_;
        return false;
    }
    // The view where the game starts (or at the level's spawn point when nothing is around it).
    module_.start(camera_.x, camera_.y);
    look_around_level();
    levels_note_.clear();
    remember_level();
    ++levels_serial_;
    return true;
}

void LevelEditor::remember_level() const {
    if (config_.project_root.empty() || level_id_.empty()) return;
    const std::string text = "{\n  \"level\": \"" + level_id_ + "\"\n}\n";
    if (!write_file_atomic(remembered_file(config_.project_root), {reinterpret_cast<const u8*>(text.data()), text.size()}))
        FORGE_WARN("Не записан %s: в следующий раз откроется стартовый уровень",
                   path_to_utf8(remembered_file(config_.project_root)).c_str());
}

void LevelEditor::choose_level(const std::string& id) {
    set_levels_menu(false);
    const level::LevelEntry* e = levels_.find(id);
    if (!e || id == level_id_) return;
    const std::string name = e->name;
    leave_level(name, [this, id] {
        if (open_level(id)) FORGE_INFO("Открыт уровень %s", q(level_name()).c_str());
    });
}

void LevelEditor::begin_new_level() {
    levels_open_ = false;
    if (levels_.broken || config_.game_data.empty()) {
        levels_note_ = levels_.broken ? levels_.problem + ": список уровней не меняется, пока его не исправят" : "у уровня нет игры";
        ++levels_serial_;
        return;
    }
    naming_ = "new";
    set(m_level_name_field_, Rml::String(level::free_level_name(levels_)), "lv_level_name_field");
    levels_note_.clear();
    ++levels_serial_;
}

void LevelEditor::begin_rename_level() {
    levels_open_ = false;
    if (level_id_.empty() || levels_.broken) {
        levels_note_ = levels_.broken ? levels_.problem + ": список уровней не меняется, пока его не исправят"
                                      : "Этот уровень не из списка уровней игры: переименовать его нельзя";
        ++levels_serial_;
        return;
    }
    naming_ = "rename";
    set(m_level_name_field_, Rml::String(level_name()), "lv_level_name_field");
    levels_note_.clear();
    ++levels_serial_;
}

void LevelEditor::cancel_level_name() {
    naming_.clear();
    ++levels_serial_;
}

bool LevelEditor::finish_level_name(const std::string& typed) {
    const std::string what = naming_;
    if (what.empty()) return false;
    const std::string problem = level::level_name_problem(levels_, typed, what == "rename" ? level_id_ : std::string());
    if (!problem.empty()) {
        levels_note_ = "Имя не подходит: " + problem;
        ++levels_serial_;
        return false;
    }
    naming_.clear();
    ++levels_serial_;
    if (what == "rename") {
        const std::string was = level_name();
        std::string error;
        if (!level::rename_level(config_.game_data, levels_, level_id_, typed, &error)) {
            levels_note_ = "Уровень не переименован: " + error;
            return false;
        }
        levels_note_ = "Уровень " + q(was) + " теперь " + q(level_name());
        FORGE_INFO("%s", levels_note_.c_str());
        return true;
    }
    // A new level: this one is settled first (the window about unsaved changes), then the level is made and opened.
    leave_level(typed, [this, typed] {
        std::string id, error;
        if (!level::add_level(config_.game_data, levels_, typed, id, &error)) {
            levels_note_ = "Уровень не создан: " + error + ". Открыт " + q(level_name()) + ", как был";
            FORGE_ERROR("%s", levels_note_.c_str());
            ++levels_serial_;
            return;
        }
        if (open_level(id)) {
            levels_note_ = "Создан уровень " + q(level_name());
            FORGE_INFO("%s (%s)", levels_note_.c_str(), path_to_utf8(level_->folder()).c_str());
        }
    });
    return true;
}

bool LevelEditor::make_start_level() {
    levels_open_ = false;
    ++levels_serial_;
    if (level_id_.empty()) {
        levels_note_ = "Этот уровень не из списка уровней игры: стартовым он быть не может";
        return false;
    }
    if (level_id_ == levels_.start) { // nothing to write (a game without levels.json stays without it)
        levels_note_ = "Игра и так начинается с уровня " + q(level_name());
        return true;
    }
    std::string error;
    if (!level::set_start_level(config_.game_data, levels_, level_id_, &error)) {
        levels_note_ = "Стартовый уровень не изменён: " + error;
        return false;
    }
    levels_note_ = "Игра начинается с уровня " + q(level_name());
    FORGE_INFO("%s", levels_note_.c_str());
    return true;
}

std::vector<std::string> LevelEditor::play_start_command() const {
    std::vector<std::string> cmd = {path_to_utf8(config_.game_exe), "--play", "--user", path_to_utf8(play_dir()), "--fired",
                                    path_to_utf8(fired_file())};
    if (!config_.game_data.empty()) {
        cmd.push_back("--data");
        cmd.push_back(path_to_utf8(config_.game_data));
    }
    return cmd;
}

bool LevelEditor::play_start() {
    levels_open_ = false;
    ++levels_serial_;
    if (stroke_) release();
    if (!save()) {
        FORGE_ERROR("Игра не запущена: уровень не сохранился, а игра начинает с сохранённого");
        return false;
    }
    last_play_ = play_start_command();
    if (!launch(last_play_)) return false;
    if (!config_.game_exe.empty() && !config_.offscreen)
        FORGE_INFO("Игра запущена со стартового уровня %s", q(levels_.start_level().name).c_str());
    return true;
}

} // namespace forge::editor_app
