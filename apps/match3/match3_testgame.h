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

// The template «Три в ряд» (step 14.3c, 14.3c-шаблон-модель.md), games/match3: made from its sources in games/ (the
// module's files, the template's «Ресурсы» games/templates/match3/assets and its cells games/templates/match3/tiles.png)
// into out, a folder of its own, made beside it first: a source missing (a picture an object names) makes nothing and
// leaves out as it was.
//
//   forge_editor --make-template match3 games/match3
//
// Six gems of four frames, «Поле» (the kind's rules: 25 moves, 1000 points), three levels of own cells («Первый» 7 × 7;
// «Второй» 8 × 8 with corners off, stones in the middle and two diamonds put before; «Третий» 8 × 8 with a notch on
// top, its moves the kind's), the links «Поле набирает цель» and «Поле остаётся без ходов», a HUD with icons, the
// windows «Уровень пройден», «Победа», «Поражение».
bool make_template(const std::filesystem::path& games, const std::filesystem::path& out, std::string& why);

} // namespace match3
