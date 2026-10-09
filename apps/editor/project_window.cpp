#include "project_window.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/hash.h"
#include "forge/core/log.h"
#include "forge/core/path.h"

#include <cstdio>

namespace forge::editor_app {

namespace fs = std::filesystem;
namespace pj = editor::project;

namespace {

Rml::String input_value(Rml::Event& ev) {
    Rml::Element* e = ev.GetTargetElement();
    return e && e->GetTagName() == "input" ? static_cast<Rml::ElementFormControl*>(e)->GetValue() : Rml::String();
}

std::string joined(const std::vector<std::string>& list) {
    std::string out;
    for (const std::string& s : list) out += (out.empty() ? "" : "; ") + s;
    return out;
}

} // namespace

fs::path play_folder(const fs::path& root) {
    std::error_code ec;
    const fs::path full = fs::weakly_canonical(fs::absolute(root, ec), ec);
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(fnv1a(path_to_utf8(full))));
    return fs::temp_directory_path() / "forge_editor_play" / name;
}

void ProjectWindow::init(ui::Ui& ui, ProjectConfig config) {
    ui_ = &ui;
    config_ = std::move(config);
    m_game_ = config_.title.empty() ? path_to_utf8(config_.root.filename()) : config_.title;
    m_root_ = path_to_utf8(config_.root);
    std::string error;
    if (!pj::read_catalog(config_.catalog, templates_, &error)) FORGE_WARN("Новая игра: %s", error.c_str());
    problems_.clear();
    m_templates_.clear();
    for (usize i = 0; i < templates_.size(); ++i) {
        const pj::Template& t = templates_[i];
        problems_.push_back(pj::template_problems(t, config_.modules));
        TemplateView v;
        v.name = t.name;
        v.about = t.about;
        const pj::Module* m = pj::find_module(config_.modules, t.module);
        v.module = m ? m->name : t.module;
        v.problem = joined(problems_.back());
        // The card's picture, made small once.
        std::vector<u8> bytes;
        assets::CookedTexture image;
        if (!t.picture.empty() && read_file(t.picture, bytes) && assets::decode_image(bytes, image)) {
            const assets::CookedTexture small = assets::fit_image(image, 240);
            const std::string name = "pj_template_" + std::to_string(i);
            ui_->set_image(name, small.rgba8.data(), small.width, small.height);
            v.picture = "/memory/" + name;
        }
        m_templates_.push_back(std::move(v));
    }
    // The first one that can be used.
    chosen_ = 0;
    for (usize i = 0; i < problems_.size(); ++i)
        if (problems_[i].empty()) {
            chosen_ = static_cast<int>(i);
            break;
        }
    for (usize i = 0; i < m_templates_.size(); ++i) m_templates_[i].selected = static_cast<int>(i) == chosen_;
    where_ = path_to_utf8(last_where());
    m_view_.clear();
    m_menu_ = false;
    if (model_)
        for (const char* name : {"pj_game", "pj_root", "pj_templates", "pj_view", "pj_menu"}) model_.DirtyVariable(name);
}

fs::path ProjectWindow::last_where() const {
    std::vector<u8> bytes;
    std::error_code ec;
    if (!config_.settings.empty() && read_file(config_.settings / "new_game_where.txt", bytes)) {
        const fs::path p = utf8_path(std::string(bytes.begin(), bytes.end()));
        if (p.is_absolute() && fs::is_directory(p, ec)) return p;
    }
    // The system's folders end with a separator: shown without it.
    auto folder = [](const char* f) {
        const fs::path p = utf8_path(f);
        return p.has_filename() || p.parent_path() == p ? p : p.parent_path();
    };
    if (const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) return folder(docs);
    if (const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME)) return folder(home);
    return {};
}

void ProjectWindow::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<TemplateView>()) {
        s.RegisterMember("name", &TemplateView::name);
        s.RegisterMember("about", &TemplateView::about);
        s.RegisterMember("module", &TemplateView::module);
        s.RegisterMember("picture", &TemplateView::picture);
        s.RegisterMember("problem", &TemplateView::problem);
        s.RegisterMember("selected", &TemplateView::selected);
    }
    model.RegisterArray<std::vector<TemplateView>>();
    model.Bind("pj_game", &m_game_);
    model.Bind("pj_root", &m_root_);
    model.Bind("pj_menu", &m_menu_);
    model.Bind("pj_view", &m_view_);
    model.Bind("pj_templates", &m_templates_);
    model.Bind("pj_title", &m_title_);
    model.Bind("pj_where", &m_where_);
    model.Bind("pj_folder", &m_folder_);
    model.Bind("pj_problem", &m_problem_);
    model.Bind("pj_can", &m_can_);
    model.Bind("pj_note", &m_note_);
    model.Bind("pj_next", &m_next_);
    model.Bind("pj_unsaved", &m_unsaved_);
    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    on("pj_menu", [this](Rml::Event&, const Rml::VariantList&) { set_menu(!m_menu_); });
    on("pj_menu_close", [this](Rml::Event&, const Rml::VariantList&) { set_menu(false); });
    on("pj_new", [this](Rml::Event&, const Rml::VariantList&) { show_new(); });
    on("pj_open", [this](Rml::Event&, const Rml::VariantList&) { browse_open(); });
    on("pj_pick", [this](Rml::Event&, const Rml::VariantList& a) { choose(a.empty() ? 0 : a[0].Get<int>()); });
    on("pj_title", [this](Rml::Event& ev, const Rml::VariantList&) { set_title(input_value(ev)); });
    on("pj_where", [this](Rml::Event& ev, const Rml::VariantList&) { set_where(input_value(ev)); });
    on("pj_browse", [this](Rml::Event&, const Rml::VariantList&) { browse_where(); });
    on("pj_create", [this](Rml::Event&, const Rml::VariantList&) { create(); });
    on("pj_cancel", [this](Rml::Event&, const Rml::VariantList&) { close(); });
    on("pj_answer", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { answer(arg_str(a, 0)); });
}

void ProjectWindow::set_view(const std::string& view) { set(m_view_, Rml::String(view), "pj_view"); }

void ProjectWindow::set_menu(bool open) { set(m_menu_, open, "pj_menu"); }

void ProjectWindow::show_new() {
    set_menu(false);
    set(m_note_, Rml::String(), "pj_note");
    // The fields as the window shows them: set from here, not from what was typed last time.
    m_title_ = title_;
    m_where_ = where_;
    if (model_) {
        model_.DirtyVariable("pj_title");
        model_.DirtyVariable("pj_where");
    }
    set_view("new");
    refresh();
}

void ProjectWindow::choose(int index) {
    if (index < 0 || index >= static_cast<int>(templates_.size())) return;
    chosen_ = index;
    for (usize i = 0; i < m_templates_.size(); ++i) m_templates_[i].selected = static_cast<int>(i) == chosen_;
    if (model_) model_.DirtyVariable("pj_templates");
    refresh();
}

// Typed in the field: the field keeps what it has (not set again from here, the caret stays).
void ProjectWindow::set_title(const std::string& title) {
    title_ = title;
    m_title_ = title;
    refresh();
}

void ProjectWindow::set_where(const std::string& where) {
    where_ = where;
    m_where_ = where;
    refresh();
}

void ProjectWindow::refresh() {
    const pj::Target t = pj::target(utf8_path(where_), title_);
    set(m_folder_, Rml::String(t.folder.empty() ? std::string() : path_to_utf8(t.folder)), "pj_folder");
    std::string problem;
    if (templates_.empty()) problem = "в каталоге шаблонов ничего нет (" + path_to_utf8(config_.catalog) + ")";
    else if (!problems_[static_cast<usize>(chosen_)].empty())
        problem = "из шаблона «" + templates_[static_cast<usize>(chosen_)].name + "» игру не создать: " +
                  joined(problems_[static_cast<usize>(chosen_)]);
    else problem = t.problem;
    set(m_problem_, Rml::String(problem), "pj_problem");
    set(m_can_, problem.empty(), "pj_can");
}

bool ProjectWindow::create() {
    refresh();
    if (!m_can_ || m_view_ != "new") return false;
    const pj::Template& t = templates_[static_cast<usize>(chosen_)];
    const fs::path parent = utf8_path(where_);
    std::string error;
    fs::path made;
    if (!pj::create(t, parent, title_, made, &error, create_hooks)) {
        set(m_note_, Rml::String("Игра не создана: " + error), "pj_note");
        FORGE_WARN("Новая игра не создана: %s", error.c_str());
        refresh();
        return false;
    }
    made_ = made;
    FORGE_INFO("Новая игра «%s» из шаблона «%s»: %s", title_.c_str(), t.name.c_str(), path_to_utf8(made).c_str());
    if (!config_.settings.empty()) {
        const std::string text = path_to_utf8(parent);
        std::error_code ec;
        fs::create_directories(config_.settings, ec);
        write_file_atomic(config_.settings / "new_game_where.txt", std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
    }
    pj::Game game;
    if (pj::find(made, config_.modules, game, &error) != pj::Found::Game) {
        message("Игра создана в " + path_to_utf8(made) + ", но не открывается: " + error);
        return true;
    }
    pending_made_ = true;
    ask_or_go(game);
    return true;
}

void ProjectWindow::open_game(const fs::path& path) {
    set_menu(false);
    pj::Game game;
    std::string error;
    if (pj::find(path, config_.modules, game, &error) != pj::Found::Game) {
        message("Игра не открыта: " + error + ".");
        return;
    }
    pending_made_ = false;
    ask_or_go(game);
}

void ProjectWindow::ask_or_go(const pj::Game& game) {
    std::error_code ec;
    if (fs::equivalent(game.root, config_.root, ec)) {
        message("Игра «" + game.title + "» уже открыта.");
        return;
    }
    pending_ = game;
    set(m_next_, Rml::String(game.title), "pj_next");
    const std::vector<std::string> list = unsaved ? unsaved() : std::vector<std::string>();
    if (list.empty()) {
        go(false);
        return;
    }
    m_unsaved_.assign(list.begin(), list.end());
    if (model_) model_.DirtyVariable("pj_unsaved");
    set(m_note_, Rml::String(), "pj_note");
    set_view("unsaved");
}

void ProjectWindow::go(bool discarding) {
    std::string error;
    if (!launch(pending_.root, error)) {
        message("Игра «" + pending_.title + "» не открыта: " + error + ". Эта игра осталась открытой, ничего не потеряно.");
        return;
    }
    FORGE_INFO("Открывается игра «%s»: %s", pending_.title.c_str(), path_to_utf8(pending_.root).c_str());
    set_view("");
    if (discarding && discard) discard();
    if (quit) quit();
}

bool ProjectWindow::launch(const fs::path& root, std::string& error) {
    launched_ = {path_to_utf8(config_.editor_exe), "--project", path_to_utf8(root)};
    const std::string look = theme ? theme() : std::string();
    if (!look.empty()) {
        launched_.push_back("--theme");
        launched_.push_back(look);
    }
    if (!config_.window || config_.editor_exe.empty()) return true; // offscreen: the self-test reads launched()
    std::error_code ec;
    if (!fs::is_regular_file(config_.editor_exe, ec)) {
        error = "нет редактора " + path_to_utf8(config_.editor_exe);
        return false;
    }
    std::vector<const char*> argv;
    for (const std::string& a : launched_) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true); // it outlives this one
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) {
        error = std::string("редактор не запустился: ") + SDL_GetError();
        return false;
    }
    SDL_DestroyProcess(process); // only the handle: the editor runs on
    return true;
}

void ProjectWindow::answer(const std::string& what) {
    if (m_view_ == "unsaved") {
        if (what == "save") {
            std::string error;
            if (save && !save(error)) {
                message("Не сохранилось: " + error + ". Игра «" + config_.title + "» осталась открытой.");
                return;
            }
            go(false);
        } else if (what == "discard") {
            go(true);
        } else if (pending_made_) {
            message("Игра «" + pending_.title + "» создана в " + path_to_utf8(pending_.root) +
                    ", но не открыта. Открыть её можно позже: «Открыть игру…».");
        } else {
            close();
        }
        return;
    }
    close();
}

void ProjectWindow::message(std::string text) {
    set(m_note_, Rml::String(std::move(text)), "pj_note");
    set_view("message");
}

void ProjectWindow::close() {
    set_menu(false);
    set_view("");
}

bool ProjectWindow::handle_key(const SDL_KeyboardEvent& k) {
    if (m_menu_ && m_view_.empty()) {
        if (k.key != SDLK_ESCAPE) return false;
        set_menu(false);
        return true;
    }
    const bool enter = k.key == SDLK_RETURN || k.key == SDLK_KP_ENTER;
    if (k.key != SDLK_ESCAPE && !enter) return false;
    if (k.repeat) return true;
    if (m_view_ == "new") {
        if (enter) create();
        else close();
    } else if (m_view_ == "unsaved") {
        answer(enter ? "save" : "stay");
    } else {
        answer("ok");
    }
    return true;
}

void ProjectWindow::browse_where() {
    if (!config_.window) return;
    {
        std::lock_guard lock(mutex_);
        asking_ = "where";
    }
    auto done = [](void* self, const char* const* list, int) {
        auto* w = static_cast<ProjectWindow*>(self);
        std::lock_guard lock(w->mutex_);
        if (list && *list) w->answers_.emplace_back(w->asking_, utf8_path(*list));
    };
    const std::string at = where_;
    SDL_ShowOpenFolderDialog(done, this, config_.window, at.empty() ? nullptr : at.c_str(), false);
}

void ProjectWindow::browse_open() {
    set_menu(false);
    if (!config_.window) return;
    {
        std::lock_guard lock(mutex_);
        asking_ = "open";
    }
    auto done = [](void* self, const char* const* list, int) {
        auto* w = static_cast<ProjectWindow*>(self);
        std::lock_guard lock(w->mutex_);
        if (list && *list) w->answers_.emplace_back(w->asking_, utf8_path(*list));
    };
    const std::string at = path_to_utf8(config_.root.parent_path());
    SDL_ShowOpenFolderDialog(done, this, config_.window, at.c_str(), false);
}

void ProjectWindow::update() {
    std::vector<std::pair<std::string, fs::path>> got;
    {
        std::lock_guard lock(mutex_);
        got.swap(answers_);
    }
    for (const auto& [what, path] : got) {
        if (what == "where") {
            set_where(path_to_utf8(path));
            m_where_ = where_;
            if (model_) model_.DirtyVariable("pj_where");
        } else {
            open_game(path);
        }
    }
}

} // namespace forge::editor_app
