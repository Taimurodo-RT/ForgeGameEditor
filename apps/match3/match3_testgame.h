#pragma once

// The test game of the module «Три в ряд» (step 14.3b): what its self-test plays, kept in tests/data/match3 and made
// by this function, so a change of the module's data or of the level format is made again, not edited by hand.
//
//   forge_game --data tests/data/match3/game --make-test-game tests/data/match3
//
// writes <out>/game: three fields in levels.json («Первый» 6 × 6; «Второй» 7 × 7 with holes and gems put before;
// «Третий» 8 × 6 with a notch on top), four gems on the board (two with pictures of three frames, one of one frame,
// one with no picture) and one with «На поле» off, the rules of each level, the links «Поле даёт +2 хода за Топаз»,
// «Поле набирает цель», «Поле остаётся без ходов», a HUD and the windows «Уровень пройден», «Победа», «Поражение»;
// and <out>/short: a field of two moves and an unreachable target (the scene match3_lose plays it).

#include <filesystem>
#include <string>

namespace match3 {

// games: the sources' games/ folder (its modules/match3 has the module's files). out is made anew.
bool make_test_game(const std::filesystem::path& games, const std::filesystem::path& out, std::string& why);

} // namespace match3
