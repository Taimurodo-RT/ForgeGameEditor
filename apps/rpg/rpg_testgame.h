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

//
// The game of the template «RPG сверху» (step 14.4b) is made here too, of the same maps, story and screens:
//
//   forge_editor --make-template rpg games/rpg
//
// writes the game folder itself, every thing with its picture from the template's «Ресурсы»
// (games/templates/rpg/assets/картинки/*.png with their .meta, copied in as the editor copies them: sources.json), the
// coins with the sound «звон.wav» (assets/звуки) and the dog with a line of its own. The editor's self-test
// (rpg-template) makes it again and compares it with the repository byte for byte.

#include <filesystem>
#include <string>

namespace rpg {

// games: the sources' games/ folder (its modules/rpg has the module's files; templates/rpg the template's «Ресурсы»).
// out is made beside it first and takes its place when it is whole: what does not make it leaves out as it was.
bool make_test_game(const std::filesystem::path& games, const std::filesystem::path& out, std::string& why);
bool make_template(const std::filesystem::path& games, const std::filesystem::path& out, std::string& why);

} // namespace rpg
