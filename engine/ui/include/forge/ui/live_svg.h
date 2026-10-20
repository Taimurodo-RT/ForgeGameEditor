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
//
// A clip of a character plays on the element too (part 5в of docs/models/движение-модель.md): svg_pose gives the
// parts of one of its pictures as the clip has them at a moment, over what the changes above said. Each part takes
// what the tab's picture of «Векторная» gives it: its transform (the clip's matrix), its opacity times the file's,
// its shapes' fill, stroke and stroke width from their own by the clip's shares (a group first, the parts inside it
// after), hidden by display: none, its outline morphed with its colours, a fill or stroke its `style` gives changed
// there.
//
// Since part 5г the clip draws everything the tab's main picture draws but blur and shadow: dashes, outlines drawn along
// their length, tints and flashes (gradients too), masks, links, copies and the parts' order. The element then draws
// the picture from a stage of it (engine/ui/src/svg_stage.h), made again when the clip needs what it has not.
//
// Since part 5д the clip shows the character's other drawings too: the element draws the drawing shown in place of its
// file, and in a swap two documents into one picture, the old drawing and the new one over it. Parts with one id in
// both (shapes) morph from the old one's to the new one's, the other shapes fade out of the old one and into the new.

#include "forge/core/types.h"
#include "forge/vector/motion_file.h"

#include <memory>
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
// Every live element in root's tree (root and the elements under it) draws its files again, as svg_reset does.
void svg_reset_all(Rml::Element* root);

// What a script said of the element's part (nullptr: nothing).
const SvgPart* svg_part(Rml::Element* element, std::string_view part);
// The parts of the element's pictures that did not take what was said, one line each, since the last call; a line
// comes once however often the script says it.
std::vector<std::string> svg_problems();

// Another drawing of a character (part 5д): its name, its file beside the picture and the file's text.
struct SvgDrawing {
    std::string name;    // "angry"
    std::string picture; // "Ёжик.angry.svg"
    std::shared_ptr<const std::string> text;
};

// What a clip shows at a moment on an element's picture `picture` (its file name, "Ёжик.svg"): motion_pose's parts.
struct SvgPose {
    std::string picture;
    std::vector<vector::MotionPartPose> parts;   // as they stand in the drawing shown
    std::vector<vector::MotionCopy> copies;      // the copies drawn now (motion_copy_drawn)
    std::vector<vector::MotionOrderStep> order;  // the order's steps up to now (motion_order_at)
    // Part 5д: the character's other drawings, by name; the one shown (motion_shown: "draw" or "" the main one, the
    // picture's file); in a swap the one it comes from, how far, and the parts as they stand in that one.
    std::vector<SvgDrawing> drawings;
    std::string drawing;
    bool swap = false;
    std::string swap_from;
    f64 swap_k = 0;
    std::vector<vector::MotionPartPose> from_parts;
};
// What a clip of a character's file shows on its picture `picture` at a moment (a game's screen poses each frame of a
// clip so): its parts, its copies drawn, its order's steps (part 5г); the drawing shown with the others' texts, in a swap
// the one it comes from, how far and the parts as they stand in that one (part 5д).
SvgPose clip_pose(std::string picture, const vector::MotionFile& file, const vector::MotionClip& clip, f64 at, std::vector<SvgDrawing> drawings = {});
// The element shows the pose on its next frame; another pose of the same parts replaces it.
void svg_pose(Rml::Element* element, SvgPose pose);
// The pose goes and the element's pictures are made again from their files, with what the changes above said: what a
// clip changed and the next one does not is as the file has it again.
void svg_unpose(Rml::Element* element);
// The pose the element shows (nullptr: none).
const SvgPose* svg_posed(Rml::Element* element);
// A picture's text with a pose on it, drawn w × h as an element of that size draws it (stretched to it, as the SVG plugin
// does), straight RGBA: what a screen shows, without one. False with why when the text is not SVG.
bool render_posed_svg(std::string_view svg, const SvgPose& pose, u32 w, u32 h, std::vector<u8>& rgba, std::string& why);
// The attributes the pose gives an element of the picture after it is applied, for checks: "" when none was given. `over`:
// of the document drawn over it in a swap (part 5д).
std::string posed_attribute(std::string_view svg, const SvgPose& pose, std::string_view id, std::string_view name, bool over = false);
// The text of the stage a pose draws the picture from, for checks: "" when it draws the file itself. `over`: the
// stage (or the drawing's text) of the document over it in a swap.
std::string posed_stage(std::string_view svg, const SvgPose& pose, bool over = false);

// Ui::init calls it: the SVG plugin asks this file what a live element's pictures are.
void install_live_svg();

} // namespace forge::ui
