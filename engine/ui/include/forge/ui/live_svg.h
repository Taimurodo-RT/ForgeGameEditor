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
    bool operator==(const SvgDrawing&) const = default;
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
    // Part 6г2 of docs/models/сцены-модель.md: the elements of the picture's top level it draws, by SvgPlace::key (none:
    // all); the others are display: none and still stand where they would for what hangs on them. A box of the
    // picture's units it shows in place of its own (its viewBox, width and height; view_w 0: its own).
    std::vector<std::string> only;
    f64 view_x = 0, view_y = 0, view_w = 0, view_h = 0;
    bool operator==(const SvgPose&) const = default;
};
// What a clip of a character's file shows on its picture `picture` at a moment (a game's screen poses each frame of a
// clip so): its parts, its copies drawn, its order's steps (part 5г); the drawing shown with the others' texts, in a swap
// the one it comes from, how far and the parts as they stand in that one (part 5д).
SvgPose clip_pose(std::string picture, const vector::MotionFile& file, const vector::MotionClip& clip, f64 at, std::vector<SvgDrawing> drawings = {});
// A blend of clips (part 5е): the pose `k` of the way from `from` (the old clip's pose, or what the layer showed) to
// `to` (the new clip's), the parts posed in `file`. Each prop of each part mixed as the file's transitions mix it
// (vector::motion_mix; one only one side has is at rest on the other), held props and the order's steps as `from`'s
// until k is 1; a copy only one side draws fades out by 1 − k or in by k where that side has it; of the drawings,
// each weighed (1 − k) of its share in `from` and k in `to`, the one `to` shows on top of the heaviest other, as a
// swap. k 0 and 1 give `from` and `to` themselves.
SvgPose blend_pose(const vector::MotionFile& file, const SvgPose& from, const SvgPose& to, f64 k);
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

// An element of a picture's top level as a pose draws it (part 6г2): where it stands and how. A scene of a game draws
// its stage's shapes, pictures and actors as layers of their own, each where the stage's picture in that pose puts it.
struct SvgPlace {
    // SvgPose::only's name of it: "e<n>" the n-th element of the file (scan_svg's), "c<id>" the copy `id` of a part
    // (part 5г); "" what a stage adds (masks, gradients), which a run never hides.
    std::string key;
    std::string id;
    vector::MotionMatrix m; // its units to the picture's (the root's viewBox's)
    f64 opacity = 1;        // its own
    bool shown = true;      // its display is not none
    bool draws = true;      // a group, a <use>, a shape, an image or a text, not <defs>, <style>, …
};

// A picture posed as a live element poses it, to read where its top level stands: the same code draws it on the screen.
class SvgPlaces {
public:
    SvgPlaces();
    ~SvgPlaces();
    SvgPlaces(const SvgPlaces&) = delete;
    SvgPlaces& operator=(const SvgPlaces&) = delete;
    // The picture at `path` (the path an element showing it has) and its text; false with why when it is not SVG.
    bool load(std::string path, std::string text, std::string& why);
    // Its top level in the order it draws it, in the pose (its `only` and view are not looked at); empty before load.
    // `areas`: the parts whose areas area() then gives.
    const std::vector<SvgPlace>& place(const SvgPose& pose, const std::vector<std::string>& areas = {});
    // Part 6г4 of docs/models/сцены-модель.md: the area of a part the last place() asked for, as the pose had it: each of
    // its shapes opaque white as a mask's copy of it (its fill when it has one or no stroke, its stroke when it has one;
    // hidden or faded, whole: the tab's Rig::area), drawn w × h through m (the picture's units to the pixels). A byte of
    // cover for each pixel, rows top to bottom; false (none) when the picture has no such part or it has no shapes.
    bool area(std::string_view id, const vector::MotionMatrix& m, u32 w, u32 h, std::vector<u8>& cover) const;
    // Those shapes as SVG in the picture's units ("" none): what area() draws, which changes when its picture does.
    const std::string& area_shapes(std::string_view id) const;
    // A box in the picture's units the area is inside (round its outlines' control points and its strokes' miters):
    // false for none.
    bool area_box(std::string_view id, f64& x, f64& y, f64& w, f64& h) const;
    // The pose a run of its top level draws (`only` the elements `keys` names): the parts inside them and the parts
    // their links and masks name, and theirs; the copies and the order whole, as every run has them.
    SvgPose run_pose(const SvgPose& pose, std::vector<std::string> keys);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Ui::init calls it: the SVG plugin asks this file what a live element's pictures are.
void install_live_svg();

} // namespace forge::ui
