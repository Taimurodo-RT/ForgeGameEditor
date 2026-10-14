#pragma once

// Live SVG pictures (step 4 of docs/models/вектор-модель.md): a script changes the parts of an SVG picture an element
// shows, found by their id in the file, and the element draws it again at its own size on the next frame. Other
// elements showing the same file keep the file as it is.
//
// Each change says where the part stands against the file, not against the last change: move(10, 0) twice leaves it
// 10 to the right, turn(90) after turn(45) is 90. A change stays until reset, which gives the element back the file.
//   move   by x, y in the units of the part's parent (what the file's own coordinates are where it stands)
//   turn   degrees clockwise round the centre of the part as the file draws it
//   color  the fill of the part, and of the shapes inside it that have a fill of their own
//   fade   the part's opacity, 0 … 1
//   morph  the part's outline on the way to another shape of the file, k = 0 the part, 1 the other shape
//          (forge::vector's morph; the part must be a <path>, the other any shape in the same coordinates)
// What does not apply (a part the file has not, a morph into nothing) is said once, in Russian, by svg_problems.

#include "forge/core/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace Rml {
class Element;
}

namespace forge::ui {

// What the scripts said of one part, against the file.
struct SvgPart {
    bool moved = false;   // move or turn was said
    f64 x = 0, y = 0;     // move
    f64 turn = 0;         // degrees
    std::string fill;     // color ("" none said)
    f64 opacity = -1;     // fade (below 0: none said)
    std::string morph;    // the shape it turns into ("" none)
    f64 k = 0;
};

bool svg_move(Rml::Element* element, std::string_view part, f64 x, f64 y);
bool svg_turn(Rml::Element* element, std::string_view part, f64 degrees);
// false (with why) when the colour does not read: "#rgb", "#rrggbb", "rgb(r, g, b)" or a few names.
bool svg_color(Rml::Element* element, std::string_view part, std::string_view color, std::string* why = nullptr);
bool svg_fade(Rml::Element* element, std::string_view part, f64 opacity);
bool svg_morph(Rml::Element* element, std::string_view part, std::string_view target, f64 k);
// The element draws its file again, as it is.
void svg_reset(Rml::Element* element);

// What a script said of the element's part (nullptr: nothing).
const SvgPart* svg_part(Rml::Element* element, std::string_view part);
// The parts of the element's pictures that did not take what was said, one line each, since the last call; a line
// comes once however often the script says it.
std::vector<std::string> svg_problems();

// Ui::init calls it: the SVG plugin asks this file what a live element's pictures are.
void install_live_svg();

} // namespace forge::ui
