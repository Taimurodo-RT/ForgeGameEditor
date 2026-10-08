#pragma once

// The colour picker's arithmetic («Интерфейс», 13.4): hue, saturation and
// brightness with alpha, what an author types as HEX, and a pixel read back
// from a drawn picture.

#include "forge/editor/ui_design.h"

#include <optional>
#include <string>
#include <string_view>

namespace forge::editor::design {

// h: 0..360, s, v, a: 0..1.
struct Hsva {
    f32 h = 0, s = 0, v = 0, a = 1;
};

Color hsva_to_color(const Hsva& c);
// A colour as hue, saturation, brightness and alpha. A grey has no hue and
// black no saturation either: they keep keep's, so the picker's hue bar and
// square do not jump when the colour passes through them.
Hsva color_to_hsva(Color c, const Hsva& keep = {});

// What an author types in the HEX field: "#3366cc", "3366CC", " 36c ",
// "#3366cc80"; nullopt for anything else (empty, a part, a letter not 0–9 A–F).
std::optional<Color> parse_hex_input(std::string_view text);
// Why the text is not a colour, in the author's words ("" when it is one).
std::string hex_problem(std::string_view text);
// "50" or "50%" as alpha 0..255; nullopt when not a number 0–100.
std::optional<u8> parse_alpha_input(std::string_view text);

// A pixel as the UI draws it (alpha premultiplied) back to its colour.
Color unpremultiply(u8 r, u8 g, u8 b, u8 a);

} // namespace forge::editor::design
