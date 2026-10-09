#pragma once

// The game the editor has open, and the ways to another (step 14.1).
//
// The button with the game's title in the top bar opens a menu:
// «Новая игра из шаблона…» and «Открыть игру…». The window «Новая игра»
// shows the catalog's templates (a picture, what it is, the module it needs,
// and why one cannot be used), the title and where to make the game, the
// folder it will get and whether it can. «Создать и открыть» makes the game
// (forge::editor::project::create: a copy of its own, put together aside and
// only then given its name) and opens it.
//
// Another game is opened the way Godot's project manager does it: a new
// editor process for it, and this one closes. Before that the editor asks
// about what would otherwise be saved or lost without asking (the level is
// saved on closing; «Сцена» is not): «Сохранить», «Не сохранять», «Отмена».
// Nothing is thrown away until the new editor has started.

#include "forge/editor/project.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace forge::editor_app {

struct ProjectConfig {
    std::filesystem::path root;    // the open game's folder
    std::string title;             // its title (game.json)
    std::filesystem::path catalog; // games/templates.json
    std::vector<editor::project::Module> modules;
    std::filesystem::path settings;   // the editor's own folder: where the last game was made
    std::filesystem::path editor_exe; // starts the editor for another game; empty: nothing is started
    SDL_Window* window = nullptr;     // for the system's folder windows; null: offscreen, none
};

// Where the game «Играть» starts keeps its player's files: one folder for each game, outside it.
std::filesystem::path play_folder(const std::filesystem::path& root);

class ProjectWindow {
public:
    void init(ui::Ui& ui, ProjectConfig config);
    // Another game open or another catalog (the self-test): as init, the same UI.
    void configure(ProjectConfig config) { init(*ui_, std::move(config)); }
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle model) { model_ = model; }
    // Each frame: what the system's folder windows answered.
    void update();
    // A window or the menu is over the editor: the keyboard and mouse are theirs.
    bool shown() const { return !m_view_.empty() || m_menu_; }
    // Esc and Enter of the window (true: taken).
    bool handle_key(const SDL_KeyboardEvent& k);

    // --- the editor's part ---
    // What would be saved or lost without asking, in words ("Уровень «level»: правки не сохранены").
    std::function<std::vector<std::string>()> unsaved;
    // Saves all of it (false: error says what did not save).
    std::function<bool(std::string& error)> save;
    // The editor will close without saving it.
    std::function<void()> discard;
    // The editor closes (the one for the other game has started).
    std::function<void()> quit;
    // The editor's theme now, given to the next one.
    std::function<std::string()> theme;
    // For the self-test: a write that fails.
    editor::project::Hooks create_hooks;

    // --- actions (the menu, the window, the self-test) ---
    void set_menu(bool open);
    void show_new();
    void choose(int index);
    void set_title(const std::string& title);
    void set_where(const std::string& where);
    void browse_where(); // the system's folder window for «Где создать»
    void browse_open();  // the system's folder window for «Открыть игру…»
    // «Создать и открыть»: false when the game was not made (note() says why).
    bool create();
    // «Открыть игру…» with a folder or a project.forge picked.
    void open_game(const std::filesystem::path& path);
    // The answer to the window about unsaved changes ("save", "discard", "stay") or to a note ("ok").
    void answer(const std::string& what);
    void close();

    // --- for the self-test ---
    const std::string& view() const { return m_view_; } // "", "new", "unsaved", "message"
    const std::string& note() const { return m_note_; }
    const std::string& problem() const { return m_problem_; }
    const std::string& folder() const { return m_folder_; }
    bool can_create() const { return m_can_; }
    int chosen() const { return chosen_; }
    const std::vector<editor::project::Template>& templates() const { return templates_; }
    const std::vector<std::string>& unsaved_list() const { return m_unsaved_; }
    // The command that starts the editor for another game, the last one given (offscreen: never run).
    const std::vector<std::string>& launched() const { return launched_; }
    const std::filesystem::path& made() const { return made_; }
    const ProjectConfig& config() const { return config_; }

private:
    struct TemplateView {
        Rml::String name, about, module, picture, problem;
        bool selected = false;
    };
    void refresh();
    void ask_or_go(const editor::project::Game& game);
    void go(bool discarding);
    bool launch(const std::filesystem::path& root, std::string& error);
    void message(std::string text);
    void set_view(const std::string& view);
    std::filesystem::path last_where() const;
    template <typename T>
    void set(T& member, const T& value, const char* name) {
        if (member == value) return;
        member = value;
        if (model_) model_.DirtyVariable(name);
    }

    ProjectConfig config_;
    ui::Ui* ui_ = nullptr;
    Rml::DataModelHandle model_;
    std::vector<editor::project::Template> templates_;
    std::vector<std::vector<std::string>> problems_; // of each template
    int chosen_ = 0;
    std::string title_ = "Новая игра", where_;
    editor::project::Game pending_; // the game to open once the editor's changes are settled
    bool pending_made_ = false;     // it was just made
    std::filesystem::path made_;
    std::vector<std::string> launched_;
    // The system's folder windows answer on a thread of their own.
    std::mutex mutex_;
    std::string asking_; // "where", "open"
    std::vector<std::pair<std::string, std::filesystem::path>> answers_;

    Rml::String m_game_, m_root_, m_view_, m_title_, m_where_, m_folder_, m_problem_, m_note_, m_next_;
    bool m_menu_ = false, m_can_ = false;
    std::vector<TemplateView> m_templates_;
    std::vector<Rml::String> m_unsaved_;
};

} // namespace forge::editor_app
