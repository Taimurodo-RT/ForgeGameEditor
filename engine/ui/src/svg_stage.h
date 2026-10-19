#pragma once

// The stage of a live SVG picture (part 5г of docs/models/движение-модель.md). lunasvg can neither add an element to a
// document nor move one, and <use> copies its target once, when it reads it. So a picture that plays a clip with copies
// of parts, masks, outlines drawn along their length, morphs of shapes that are not a <path>, tinted gradients or steps
// of the parts' order is drawn from a text of its own, made from the file's: the stage. Each frame then writes the
// stage's elements as it writes the file's.
//
// In the stage:
//   a copy      is its source's text again (ids and all), right after the source and its copies with smaller numbers
//   an outline  is a shape the clip draws along its length or morphs: written as a <path> of the same outline
//   a mask      is a <mask> in a <defs> at the end, a <path> in it for each shape of the mask part (and a white
//               rectangle under them when it masks outside)
//   a gradient  of a shape the clip tints gets a copy of its own there: <linearGradient href="#its own"> with stops
//   the order   of a group's elements is what the clip's order steps make of the file's, the copies with them

#include "forge/vector/motion_file.h"
#include "forge/vector/svg.h"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace forge::ui {

// A mask of a stage: the part masked (an id of the file or a copy's), the mask part and whether outside.
using StageMask = std::tuple<std::string, std::string, bool>;

// What a stage carries beyond the file. A shape is named by the copy it is in ("" for the file's own) and its element in
// the file.
struct StageNeeds {
    std::vector<vector::MotionCopy> copies; // each once
    std::set<i32> outlines;                 // shapes written as <path>, in the file and every copy
    std::set<std::pair<std::string, i32>> fill_gradients, stroke_gradients;
    std::set<StageMask> masks;
    std::vector<vector::MotionOrderStep> order;

    bool empty() const;
    // Every copy, outline, gradient and mask of `other` is here, and the same order.
    bool covers(const StageNeeds& other) const;
    // The copies, outlines, gradients and masks of both, and other's order.
    void add(const StageNeeds& other);
};

struct Stage {
    std::string text;
    // Each element of the stage's text, in the order scan_svg reads them: the element of the file it is (its own or in a
    // copy; -1 for one the stage adds) and the copy it is in ("" none).
    std::vector<i32> file_of;
    std::vector<std::string> copy_of;
    std::vector<i32> of_file;                          // each element of the file: its own in the stage; -1 none
    std::map<std::string, i32, std::less<>> copy_root; // a copy's id: its element
    std::map<StageMask, i32> mask;                     // the <mask>
    std::map<std::pair<StageMask, i32>, i32> mask_path; // the <path> in it of a shape of the mask part (file element)
    std::map<std::pair<std::string, i32>, i32> fill_gradient, stroke_gradient; // a shape's gradient copy
};

// The shapes an element draws: itself when it is one, else the shapes inside it, not those of <defs>, <clipPath>,
// <mask>, <pattern>, <symbol> and <marker>, which draw nothing where they are.
bool stage_shape_tag(std::string_view tag);
std::vector<i32> stage_leaves(const vector::SvgOutline& outline, i32 index);

// The part of the file an id names (forge::pixel::Rig::part): the first element with it that draws (g, use or a
// shape); -1 for none.
i32 stage_part(const vector::SvgOutline& outline, std::string_view id);

// The stage of a file (`text`, read into `outline`) for these needs. What does not make (a copy of a part the file has
// not, an order step of a part in another group) is left out with a line for the author in `notes`. False when the
// file has no root element.
bool make_stage(std::string_view text, const vector::SvgOutline& outline, const StageNeeds& needs, Stage& out,
                std::vector<std::string>& notes);

} // namespace forge::ui
