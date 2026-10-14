#pragma once

// An SVG picture as lunasvg draws it straight at a size, for tests to compare what the UI drew with (the editor's
// self-test ui-scale, the game's scene project with --edits vs_*). Premultiplied RGBA, rows top down, as the UI draws.

#include "forge/core/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace forge::edits {

// The picture at w × h; color: its currentColor ("#rrggbb"; empty: none). Empty when it does not read.
std::vector<u8> svg_pixels(std::string_view svg, int w, int h, const std::string& color = {});
// The picture as a frame of nine parts over a w × h box (border-image, slice c of a picture drawn at r × r, the
// border as wide as the slice): the corners as drawn at r × r, the edges and the middle the picture's middle row or
// column (a frame whose edges are one colour along them). Empty when it does not read.
std::vector<u8> svg_frame(std::string_view svg, int r, int c, int w, int h);
// The largest difference of a channel of two pictures of a size; 999 when the sizes differ. rgb: alpha not compared
// (a picture over the game's world).
int pixels_diff(const std::vector<u8>& a, const std::vector<u8>& b, bool rgb = false);
// "#rrggbb" of a colour.
std::string hex_color(int r, int g, int b);

} // namespace forge::edits
