#pragma once

// SVG in «Ресурсы» (step 3 of docs/models/вектор-модель.md): the file goes to the game as written and lunasvg draws it
// there. What lunasvg leaves out, or draws otherwise, is named to the author when the file comes in.

#include "forge/core/types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge::assets {

// Whether lunasvg reads the bytes as an SVG: a picture of the game's «pictures» the game draws so.
bool svg_readable(std::span<const u8> bytes);

// One Russian clause per kind of thing, in the order met: «фильтр «glow» (размытие) в строке 4 lunasvg не рисует».
// Empty when lunasvg draws all of the file.
std::vector<std::string> svg_gaps(std::string_view text);

// The clauses as the one line «Ресурсы» show, with where blur and shadow come from instead; empty when there are none.
std::string svg_gaps_line(std::string_view text);

} // namespace forge::assets
