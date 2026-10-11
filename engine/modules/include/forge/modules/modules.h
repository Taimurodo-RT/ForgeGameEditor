#pragma once

// The genre modules built into Forge, by name (step 14.3a).
//
// A module is the C++ of one kind of game: its world (forge::game::Game), the blocks its objects are made of
// (kinds.json), the events its verbs happen on (verbs.json "when"), the values its screens may show, and its view of
// a level in the editor (forge::level::LevelModule). The engine, the editor and the game launcher name no module:
// each binary's list of built-in modules (its builtin.cpp) adds them here, and everything else finds a module by the
// id a game names (game.json and project.forge "module"). A game made of levels of several modules (14.3d) finds each
// level's module the same way, so all of them are here at once.
//
//   forge::modules::Registry modules;
//   if (std::string error; !modules.add(slice::module_def(), &error)) ...
//   const forge::modules::ModuleDef* m = modules.find("slice");
//   if (!m) ... forge::modules::missing_module("slice")
//   m->run(argc, argv);                   // the game
//   auto view = m->make_level_module();   // the editor's «Уровень», «Объекты», «Логика»

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace forge::objects {
class Library;
}
namespace forge::level {
class LevelModule;
}

namespace forge::modules {

// A value of the game its screens may show ({hero.hearts}), in the author's words.
struct Value {
    std::string var;   // "hero.hearts"
    std::string words; // "Сердца героя"
};

// An event a verb happens on (verbs.json "when"), in the author's words.
struct Event {
    std::string id;    // "touch"
    std::string words; // "герой касается"
};

// A maker of one of the module's templates: the template's game made in out from its sources in the repository's
// games/ folder (`forge_editor --make-template <id> <out>`). Only the editor has these.
struct TemplateMaker {
    std::string id; // "platformer"
    std::function<bool(const std::filesystem::path& games, const std::filesystem::path& out, std::string& why)> make;
};

struct ModuleDef {
    std::string id;   // "slice": what game.json and project.forge name
    std::string name; // "Старая шахта"
    // The module's own files every game of it has (kinds.json, verbs.json, ideas.json), where the sources keep them
    // (games/modules/<id>). Not there (a package): games keep their copies.
    std::filesystem::path files;
    std::vector<Event> events;
    // The values its screens may show. library: the game's objects (a module may offer a value for a template, as
    // slice does for what a pickup gives); null: the module's own only.
    std::function<std::vector<Value>(const objects::Library* library)> values;
    // The game: the launcher hands its command line over as it got it; the exit code.
    std::function<int(int argc, char** argv)> run;
    // The editor's view of a level of this module: one for each editor, living as long as it.
    std::function<std::unique_ptr<level::LevelModule>()> make_level_module;
    std::vector<TemplateMaker> templates;
    // A module for the self-tests only: binaries add it with --test or --self-test, never for a player.
    bool test_only = false;

    std::vector<std::string> event_ids() const;
};

class Registry {
public:
    // False (error says why, in words): no id, an id the registry has already, no run or no make_level_module, an
    // event without an id or twice.
    bool add(ModuleDef def, std::string* error = nullptr);
    // Null when the build has no such module.
    const ModuleDef* find(std::string_view id) const;
    // In the order added.
    std::vector<const ModuleDef*> all() const;

private:
    std::vector<std::unique_ptr<ModuleDef>> modules_;
};

// What a game that needs a module this build does not have says: «игре нужен модуль «X», его нет в этой сборке Forge».
std::string missing_module(std::string_view id);

} // namespace forge::modules
