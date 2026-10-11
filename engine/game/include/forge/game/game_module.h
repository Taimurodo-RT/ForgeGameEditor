#pragma once

// Which module a game is made with: the "module" of its game.json (step 14.3a). One rule for the editor and for the
// game launcher, so a game opens and plays on the same module or on none.
//
//   no "module" in game.json      "slice": every game before step 14.3a is one of «Старая шахта»
//   "module": "<id>"              that module (the registry says whether the build has it)
//   "module" empty or no string,  no module, the error says why: such a game is never taken for "slice"
//   game.json unreadable or gone
//
// With several modules in one game (14.3d) this stays the game's module, the one its levels use unless a level names
// its own.

#include "forge/core/types.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace forge::game {

// What a game.json without "module" means.
inline constexpr std::string_view kOldGameModule = "slice";

enum class GameModule : u8 {
    Named,  // game.json names it
    Old,    // game.json names none: a game from before, kOldGameModule
    Broken, // no module (error says why)
};

GameModule game_module(const std::filesystem::path& game_dir, std::string& id, std::string* error = nullptr);
// The same for the text of a game.json.
GameModule game_module_of(std::string_view json, std::string& id, std::string* error = nullptr);

} // namespace forge::game
