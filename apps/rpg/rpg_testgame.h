#pragma once

// The test game of the module «RPG сверху» (step 14.4a): what its self-test plays, kept in tests/data/rpg and made by
// this function, so a change of the module's data or of the level format is made again, not edited by hand.
//
//   forge_game --data tests/data/rpg/game --make-test-game tests/data/rpg
//
// writes <out>/game: two maps in levels.json («Деревня» 24 × 14: a house, a pond behind a fence with a key by it, the
// elder who asks for the key, the guard who lets the hero give up, a dog who says nothing, coins, the gate to the
// east; «Поляна» 12 × 9 with a chest), the hero of three frames, the quest «Ключ от ворот» and the dialogues of the
// elder and the guard, the links «Герой доходит до Ворот» and «Герой находит Сундук», a HUD with the map's name, the
// inventory and a hint to talk, and the windows «Уровень пройден», «Победа», «Поражение».

#include <filesystem>
#include <string>

namespace rpg {

// games: the sources' games/ folder (its modules/rpg has the module's files). out is made anew.
bool make_test_game(const std::filesystem::path& games, const std::filesystem::path& out, std::string& why);

} // namespace rpg
