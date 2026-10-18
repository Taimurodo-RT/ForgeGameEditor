#pragma once

// «Герой.pixel.json» (step 2 of docs/models/вектор-модель.md): everything about a vector character but its drawing and
// its pose script. The SVG stays an ordinary file of any editor; the game reads none of the three, only the baked
// picture.
//
//   {
//     "svg": "<Guid of Герой.svg>", "script": "Герой.luau",
//     "frame": [32, 32], "seed": 1234,
//     "params": {"speed": 1.0},
//     "pivots": {"leg_l": [29.5, 41]},
//     "clips": {"stand": {"frames": 4, "fps": 6, "loop": true}, "walk": {"frames": 8, "fps": 12, "loop": true},
//               "jump": {"frames": 7, "fps": 12, "loop": false, "range": [0.25, 0.75]}},
//     "waits": {"jump": {"отрыв": 0.18}}, "durations": {"spin": {"оборот": 0.6}},
//     "bake": {"palette": "svg", "outline": null, "picture": "<Guid of the baked picture>", "hash": "<its bytes>",
//              "strip": {"stand": {"first": 0, "frames": 4, "fps": 6, "loop": true}, "walk": {"first": 4, …,
//                        "events": [{"frame": 0, "sound": "шаг"}, {"frame": 2, "emit": "удар", "frames": 2}]}}}
//   }
//
// "svg" is not there when the code draws the picture (its draw section, draw.h).
// "range" is the part of a clip of the code baked (step 3 of docs/models/движение-модель.md), only when it is not all of
// it; such a clip's "frames" are counted from it (clip_cut.h), the file keeps them as they were last counted.
// "strip" is what the picture holds, as it was baked: «Отдать шаблону…» gives the template these animations, whatever
// the clips became since, with the marks of their code on the frames that show them (part 5а of
// docs/models/движение-модель.md); a clip without marks has no "events". "waits" and "durations" are the lengths of the code's wait_until and duration by clip and
// name (step 1 of docs/models/движение-модель.md): dragged on the timeline, they live here, not in the code.

#include "forge/pixel/motion.h"
#include "forge/pixel/rig.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace forge::pixel {

inline constexpr u32 kMaxFrameSide = 256;
// The widest baked strip, in pixels: the side of a texture SDL GPU promises on any video card (objects::kMaxStripWidth).
inline constexpr u32 kMaxStripWidth = 16384;
// Frames in a strip, all clips together, as a file may say: as many one-pixel frames as the widest strip holds. The
// game's own frame limit is lower (objects::kDefaultFrameLimit unless its game.json says), and baking keeps to it.
inline constexpr u32 kMaxStripFrames = kMaxStripWidth;
// The end of a clip's range, in seconds, as a file may say: an hour.
inline constexpr f64 kMaxRange = 3600;

struct ClipSpec {
    std::string state; // the animation state it plays ("walk"), its key in "clips"
    u32 frames = 4;    // a clip of the code: as counted from its range when the file was written (clip_cut.h)
    f32 fps = 10;
    bool loop = true;
    // "range": [from, to] — the part of a clip of the code that is played and baked, in seconds; none: all of it.
    bool ranged = false;
    f64 from = 0, to = 0;
    bool operator==(const ClipSpec&) const = default;
};

// A mark of a baked clip (part 5а): emit("удар"[, s]) or sound("шаг") on the frame of the clip (from 0) that first shows
// its moment, a window on `frames` frames; what objects::ClipEvent gives the game.
struct StripEvent {
    std::string name;
    bool sound = false;
    u32 frame = 0, frames = 1;
    bool operator==(const StripEvent&) const = default;
};

// A clip as baked: its frames in the strip, its marks by frame (those of one frame as the code made them).
struct StripClip {
    std::string state;
    u32 first = 0, frames = 0;
    f32 fps = 10;
    bool loop = true;
    std::vector<StripEvent> events;
    bool operator==(const StripClip&) const = default;
};

struct PixelDoc {
    std::string svg;    // the Guid of the SVG in «Ресурсы»; empty when the code draws the picture
    std::string script; // the pose script's file, beside this one
    u32 frame_w = 32, frame_h = 32;
    u64 seed = 1;
    std::map<std::string, f64> params;           // values over the script's own
    std::map<std::string, vector::Point> pivots; // by part, in picture units
    std::vector<ClipSpec> clips;                 // in the strip's order
    std::map<std::string, MotionLengths> lengths; // by clip of the code: "waits" and "durations"
    bool game_palette = false;                   // "palette": "game" (the game's colours) or "svg"
    bool outline = false;
    Rgba8 outline_color{0, 0, 0, 255};
    std::string picture; // the Guid of the baked picture; empty until baked
    std::string hash;    // its bytes' hash when baked: another hash means it was edited since
    std::vector<StripClip> strip; // what the baked picture holds
    bool operator==(const PixelDoc&) const = default;
};

// False with why (Russian, naming the field) when the text is not such a file.
bool read_pixel_doc(std::string_view json, PixelDoc& out, std::string& why);
std::string write_pixel_doc(const PixelDoc& doc);

} // namespace forge::pixel
