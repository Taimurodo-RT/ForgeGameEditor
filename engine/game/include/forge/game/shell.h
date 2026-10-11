#pragma once

// The part every game has around its gameplay: the main menu, pause, save
// and load screens with slots, settings, the dialogue box, the quest journal,
// short messages ("Сохранено"), the play-time clock and autosave. The game
// itself only builds and draws its world (see Game below).
//
// Screens are one RmlUi document, <ui>/game/shell.rml with shell.rcss, bound
// to the data model "shell". Restyle or rearrange them there; the game's own
// HUD is its own document in the same context.
//
// Game data the shell reads from the game's folder:
//   game.json             {"title": "...", "org": "...", "theme": "fantasy", "autosave_minutes": 5}
//   dialogues/*.json      dialogues (dialogue.h); the file name is the id
//   quests.json           quests (quests.h)
//   ui/*.html             the game's own screens from the editor's
//                         «Интерфейс» (screens.h): a HUD, windows, a main
//                         menu in place of the usual one

#include "forge/game/dialogue.h"
#include "forge/game/quests.h"
#include "forge/game/saves.h"
#include "forge/game/screens.h"
#include "forge/game/vars.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Window;
namespace Rml {
class Context;
class Element;
} // namespace Rml
namespace forge::ui {
class Ui;
}

namespace forge::game {

class Shell;

// What a game provides. Everything is called on the main thread.
class Game {
public:
    virtual ~Game() = default;

    // Set up what lives for the whole run (renderers, art). Called once.
    virtual bool init(Shell& shell, SDL_GPUDevice* device, SDL_GPUTextureFormat format) = 0;
    // A game begins in the session folder: empty for a new game, filled from
    // a slot when loading. Build the world from it.
    virtual bool begin(const std::filesystem::path& session, bool new_game, std::string* error) = 0;
    // Write everything not yet on disk into the session folder. location is
    // shown in the slot list ("Старая шахта").
    virtual bool save(const std::filesystem::path& session, std::string& location, std::string* error) = 0;
    // Back to the main menu: let go of the world.
    virtual void end() = 0;
    virtual bool running() const = 0;
    // Whether the game may be saved now (false: why not, in words for the player). A game that is over (won or lost)
    // is never saved: the shell writes no slot, says why to the player, and keeps quiet for an autosave.
    virtual bool can_save(std::string* why) const {
        (void)why;
        return true;
    }

    // Every frame, also while a menu is open. playing: the game is on
    // screen and not paused (dialogues do not pause the world); input: the
    // player may control the hero (no menu, no dialogue).
    virtual void update(f64 dt, bool playing, bool input) = 0;
    // The world, under the UI. Also called in the main menu: a game may draw
    // a backdrop there (or leave the screen cleared).
    virtual void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height) = 0;
    // Events the UI did not use, while the player has control.
    virtual void handle_event(const SDL_Event& event) { (void)event; }
    virtual void shutdown() = 0;
};

// game.json
struct GameInfo {
    std::string title = "Forge";
    std::string org = "Forge";
    std::string theme;
    f64 autosave_minutes = 5;
    f64 zoom = 0;    // "zoom": pixels of the screen per tile of the world at a new game; 0: the game's own
    bool ok = false; // the file was there and readable
};
GameInfo read_game_info(const std::filesystem::path& game_dir);

struct ShellConfig {
    std::filesystem::path ui_dir;   // has forge-ui/, fonts/ and game/shell.rml
    std::filesystem::path game_dir; // game.json, dialogues/, quests.json
    std::filesystem::path user_dir; // empty = the system's place for game data
    std::string theme;              // empty = game.json's, then the settings'
    // The module the game runs on ("slice"): its slots say so, and a slot of another module (or of none) is not
    // loaded (slot_fits).
    std::string module;
    // Running from the source tree: UI files reload when saved, F8 inspects
    // the UI. Off in a packaged game.
    bool dev = true;
};

enum class Screen : u8 { Main, Playing, Paused, Slots, Settings, Journal, Loading };

// The game's volumes as its variables settings.master, settings.music and settings.sound (whole numbers, 0 to 100),
// both ways, so a screen shows and changes them like any other variable: what a button's «Изменить данные» or
// «Логика» sets goes to the sound and settings.json, a change of the settings shows in the variables (Shell, every
// frame and after a button). 0 is a volume like any other: the player's silence, kept.
struct VolumeVars {
    // What write put into the variables last (NaN: nothing since they were cleared or loaded, so what a save or a new
    // game has in them never changes the settings).
    std::array<f64, 3> written{NAN, NAN, NAN};
    // What the variables were set to since write, into s: 0 to 1 of a volume, whatever number it was (-10 is 0, 250
    // is 1); what is not a number leaves the volume as it was. Whether a volume changed.
    bool read(const Vars& vars, Settings& s) const;
    // The volumes of s into the variables, as whole numbers, where they are not that already.
    void write(const Settings& s, Vars& vars);
};

class Shell {
public:
    Shell();
    ~Shell();
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    // window may be null (offscreen runs).
    bool init(Game& game, SDL_GPUDevice* device, SDL_Window* window, SDL_GPUTextureFormat format, u32 width, u32 height,
              const ShellConfig& config);
    void shutdown();

    // True when the shell or the UI used the event.
    bool handle_event(const SDL_Event& event);
    void update(f64 dt);
    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, u32 width, u32 height);
    bool quit_requested() const { return quit_; }

    // --- for the game ---
    Vars& vars() { return vars_; }
    const QuestBook& quests() const { return quests_; }
    // Functions dialogues may call: has("pickaxe"), give("gold", 5)...
    void define(const std::string& name, CallFn fn);
    // Opens a dialogue by id (the file name). False when it does not exist
    // or has nothing to say right now.
    bool talk(std::string_view dialogue_id);
    bool in_dialogue() const;
    // A short message at the top of the screen for a few seconds.
    void toast(std::string text);
    // The game's own screens (ui/*.html).
    GameScreens& screens() { return *screens_; }
    // A button's «Сообщение логике»: the game passes it to its logic (the
    // schemes «При сообщении» with that name).
    std::function<void(const std::string& message)> on_message;
    f64 playtime() const { return playtime_; }

    // --- for menus, tests and the offscreen runner ---
    Screen screen() const { return screen_; }
    bool new_game();
    // A slot of another module, or one whose module cannot be read, is refused before anything changes: the game,
    // its session and its variables stay as they were, and the player is told why.
    bool load(std::string_view slot_id);
    bool save(std::string_view slot_id, std::string title, bool autosave = false);
    bool continue_game();       // the newest slot
    void pause(bool on);
    void to_main_menu();
    const SaveSlots& slots() const { return *slots_; }
    const Settings& settings() const { return settings_; }
    const std::filesystem::path& user_folder() const { return user_dir_; } // saves and settings.json
    void apply_settings(const Settings& s);
    const std::string& title() const { return title_; }
    // The game's data folder (game.json, level/...).
    const std::filesystem::path& game_dir() const { return config_.game_dir; }
    const Dialogue* dialogue(std::string_view id) const;
    DialogueRunner& dialogue_runner() { return *runner_; }
    std::vector<std::string> data_errors() const { return data_errors_; }

    ui::Ui& ui();
    Rml::Context* context() { return context_; }
    Rml::Element* find_element(const char* id);
    // The mouse is over the world, not over a window or button.
    bool over_world() const;

private:
    struct Model;
    void bind_model();
    void refresh_model();
    void show(Screen s);
    void load_game_data();
    bool begin(std::string_view slot_id);
    void end_dialogue();
    void start_loading(std::function<void()> work);
    void screen_action(const ScreenAction& action);
    // The game's volumes as variables (VolumeVars), both ways.
    void sync_settings_vars();
    Value call(std::string_view name, const std::vector<Value>& args);

    Game* game_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* window_ = nullptr;
    SDL_GPUTextureFormat format_{};
    std::unique_ptr<ui::Ui> ui_;
    Rml::Context* context_ = nullptr;
    std::unique_ptr<Model> model_;
    ShellConfig config_;
    std::filesystem::path user_dir_;
    std::unique_ptr<SaveSlots> slots_;
    Settings settings_;
    std::string title_ = "Forge";
    std::string org_ = "Forge";
    f64 autosave_s_ = 300;

    Vars vars_;
    VolumeVars volume_vars_;
    QuestBook quests_;
    std::unordered_map<std::string, std::unique_ptr<Dialogue>> dialogues_;
    std::unordered_map<std::string, CallFn> calls_;
    std::unique_ptr<DialogueRunner> runner_;
    std::unique_ptr<GameScreens> screens_;
    std::vector<std::string> data_errors_;

    Screen screen_ = Screen::Main;
    Screen back_ = Screen::Main; // where Esc on settings / slots returns
    f64 playtime_ = 0;
    f64 since_autosave_ = 0;
    std::string slot_; // the slot this game was loaded from or last saved to
    bool quit_ = false;
    u32 width_ = 0, height_ = 0;
};

} // namespace forge::game
