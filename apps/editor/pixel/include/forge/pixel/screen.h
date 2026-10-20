#pragma once

// «Отдать экрану» of «Векторная» (part 5в of docs/models/движение-модель.md): a character's clips as a game's screen
// plays them on its live SVG (forge/vector/motion_file.h). The game runs none of the author's code, so the file holds
// what the tab's preview shows, worked out: each track's pieces start where its deciding block starts and where that
// block ends uncut; a piece is a hold when the frame keeps one value through it, the block's transition when its
// formula gives the frame exactly at each 120th of a second and at the moment before the piece's end, else the frame's
// values 120 times a second.

#include "forge/pixel/doc.h"
#include "forge/pixel/draw.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/vector/motion_file.h"

#include <string>
#include <string_view>
#include <vector>

namespace forge::pixel {

// A frame's value of a part's property as the file keeps it: a colour, a width or a morph at its share.
vector::MotionValue screen_value(const Pose& pose, const std::string& id, vector::MotionProp p);

// Clip `clip` of the code as the screen plays it, at the file's values and seed and the lengths the script was given:
// its tracks of the picture's parts, its marks, what it does that the screen does not draw; the authors' easings its
// pieces use go into file.curves (each once). False with why: «клип «jump», строка 5: …».
bool screen_clip(PoseScript& script, const Rig& rig, const PixelDoc& doc, std::string_view clip, vector::MotionFile& file,
                 vector::MotionClip& out, std::string& why);

struct ScreenGift {
    vector::MotionFile motion;
    std::string svg;                 // the picture the game shows
    std::vector<std::string> notes;  // what the screen will not show as the tab does, for the tab's line
};

// Every clip of the code into a file of `character` given from `from` (its «.pixel.json» in «Ресурсы»), with the parts
// they move, and the picture: `svg` (the file's text, byte for byte) or, for a character the code draws (`drawn`),
// its drawing with the parts that morph written as <path>. False with why: a clip that fails, or no clips.
bool build_screen(PoseScript& script, const Rig& rig, const PixelDoc& doc, std::string_view character, std::string_view from,
                  std::string_view svg, const Drawing* drawn, ScreenGift& out, std::string& why);

// The drawing with the shapes of these ids written as <path> of the same outline.
Drawing drawing_with_paths(const Drawing& d, const std::vector<std::string>& ids);

} // namespace forge::pixel
