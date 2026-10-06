#pragma once

// The author's project folder: everything they make lives there, apart from
// the engine, so updating the engine (git pull + build) never touches it.
//
//   <project>/game/     the game's data: level/, dialogues/, objects/,
//                       pictures/, sounds/, logic.json, quests.json…
//   <project>/assets/   «Ресурсы»
//
// The project is the editor's working folder (or --project DIR). On the
// first start game/ is a copy of the engine's games/slice, the author's edits
// in it included, so work made before projects existed moves over. Later
// starts keep everything the author has and only bring in what the engine
// owns: the definitions the editor never writes (verbs, ideas, object kinds)
// are refreshed, and data files a newer engine added appear.

#include <filesystem>
#include <string>
#include <vector>

namespace forge::editor_app {

struct ProjectGame {
    std::filesystem::path dir;          // <project>/game
    bool created = false;               // copied from the template just now
    std::vector<std::string> refreshed; // engine files brought in (names)
};

// The engine's files in game/: refreshed from the template on every start.
inline constexpr const char* kEngineGameFiles[] = {"verbs.json", "ideas.json", "kinds.json"};

bool prepare_project_game(const std::filesystem::path& project, const std::filesystem::path& template_dir,
                          ProjectGame& out, std::string* error);

} // namespace forge::editor_app
