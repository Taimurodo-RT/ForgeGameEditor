#pragma once

// The author's game: a folder of its own, made from a template of the
// catalog and opened by its folder or by its description.
//
//   Мои игры/Игра А/
//     project.forge   what it is: the description's format, its module, its template
//     game/           everything the game reads (game.json, objects/, level/…)
//     assets/         «Ресурсы»: the author's sources; the game never reads them
//
// Nothing in it names a path outside it, so a game is copied, moved or packed
// as its folder. A game is made by copying, as a template of Godot's Asset
// Library is: no link to the template stays. A folder from before
// (game/game.json and no description) opens as it always did.

#include "forge/core/types.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace forge::editor::project {

inline constexpr std::string_view kDescription = "project.forge";
inline constexpr int kFormat = 1;

// project.forge: {"forge_project": 1, "module": "slice", "template": "old-mine"}.
struct Description {
    int format = kFormat;
    std::string module; // the module the game needs ("slice")
    std::string from;   // the template it was made of ("old-mine"): only for the record
};
// False (error says why) when it is no description or one of a newer format.
bool read_description(std::string_view json, Description& out, std::string* error = nullptr);
std::string description_json(const Description& d);

// A module built into this editor: the part of the engine that makes games of
// a kind ("slice": «Старая шахта»), and the folder of its own files, the ones
// every game of it has and the editors never change.
struct Module {
    std::string id;   // "slice"
    std::string name; // "Старая шахта"
    std::filesystem::path files;
};
inline constexpr const char* kModuleFiles[] = {"kinds.json", "verbs.json", "ideas.json"};
const Module* find_module(const std::vector<Module>& modules, std::string_view id);

// A template of the catalog: what the window «Новая игра» shows, and what a
// new game is copied from.
struct Template {
    std::string id;     // "old-mine": stable, written into the games made of it
    std::string name;   // "Старая шахта"
    std::string about;  // a sentence or two
    std::string module; // "slice"
    std::filesystem::path game;    // its game/ (full path)
    std::filesystem::path assets;  // its «Ресурсы» (empty: none)
    std::filesystem::path picture; // its card's picture (empty: none)
    std::string bad;               // a path of its line that leads out of the catalog's folder (not taken)
};
// The catalog (games/templates.json): its folders and pictures are paths from
// the catalog's folder; one that leads out of it is refused. False when the
// file cannot be read.
bool read_catalog(const std::filesystem::path& file, std::vector<Template>& out, std::string* error = nullptr);
// What keeps a game from being made of t, in words; empty when nothing does:
// a module this editor does not have, no game folder or game.json, no
// picture or «Ресурсы» the catalog names, an object of a kind the module does
// not have or with a picture or a sound the template does not have, an object
// file that cannot be read.
std::vector<std::string> template_problems(const Template& t, const std::vector<Module>& modules);

// The folder a game of this title gets: the title as typed, its letters and
// spaces kept; signs no file name on Windows may have (< > : " / \ | ? * and
// control characters) left out, runs of spaces as one, no dots or spaces at
// the ends, at most 100 bytes, "_1" after a name Windows keeps for a device
// (CON, NUL, COM1…). Empty when nothing is left.
std::string folder_name(std::string_view title);

// Where a new game of title in parent goes, and why it cannot (empty: it
// can). Parent must be a full path to a folder that is there; the game's
// folder must not be there, or be empty.
struct Target {
    std::filesystem::path folder;
    std::string problem;
};
Target target(const std::filesystem::path& parent, std::string_view title);

// For tests: a file write that fails, someone taking the folder just before
// the copy becomes it.
struct Hooks {
    std::function<bool(const std::filesystem::path& file)> write;
    std::function<void(const std::filesystem::path& folder)> before_rename;
};
// Makes a game of t named title in parent. The copy is put together in a
// side folder of parent (template's game/ and assets/, project.forge, the
// title in game.json) and becomes the game's folder only whole; an empty
// folder there makes way, anything else is never written over. On any error
// nothing is left and error says what did not work. made: the game's folder.
bool create(const Template& t, const std::filesystem::path& parent, std::string_view title, std::filesystem::path& made,
            std::string* error = nullptr, const Hooks& hooks = {});

// A game to open: path is its folder or its project.forge.
struct Game {
    std::filesystem::path root; // its folder
    std::filesystem::path game; // root/game
    Description description;    // a folder from before: module "slice", no template
    bool described = false;     // it has project.forge
    std::string title;          // game.json's
};
enum class Found {
    Game,   // a game, out has it
    NoGame, // a folder with no game in it (or no folder): nothing to open
    Broken, // a game this editor cannot open (error says why)
};
Found find(const std::filesystem::path& path, const std::vector<Module>& modules, Game& out, std::string* error = nullptr);

// Opening a described game: the module's files that differ from its own are
// copied over them (names in refreshed). When the module's folder is not
// there, the game keeps its copies (note says so); nothing else is touched.
bool refresh_module_files(const std::filesystem::path& game, const Module& module, std::vector<std::string>& refreshed,
                          std::string* note = nullptr);

// The title in game_dir/game.json ("" when there is none).
std::string game_title(const std::filesystem::path& game_dir);

} // namespace forge::editor::project
