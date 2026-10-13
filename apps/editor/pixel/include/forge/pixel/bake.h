#pragma once

// A character's clips baked into one strip of frames, as the game cuts a picture («Кадров в картинке»): every clip's
// frames side by side in the order of the clips (step 2 of docs/models/вектор-модель.md). Frame k of a clip is the
// pose script at t = k / fps.

#include "forge/pixel/doc.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"

#include <string>
#include <vector>

namespace forge::pixel {

// The pivots of the file put on the rig; the ids it has no part for, in order.
std::vector<std::string> apply_pivots(Rig& rig, const PixelDoc& doc);

// The pose of frame k of a clip. False with why: "клип «walk», кадр 3: строка 5: …".
bool frame_pose(PoseScript& script, const Rig& rig, const PixelDoc& doc, const ClipSpec& clip, u32 k, Pose& out, std::string& why);

// The baking of the file's settings: its palette (the game's colours when it says so) and outline.
BakeOptions bake_options(const PixelDoc& doc, const std::vector<Rgba8>& game_colors);

// One frame as baked: posed (moves in whole pixels of the frame) and baked at the file's frame size.
bool bake_frame(Rig& rig, PoseScript& script, const PixelDoc& doc, const ClipSpec& clip, u32 k, const BakeOptions& options,
                assets::CookedTexture& out, std::string& why);

struct StripClip {
    std::string state;
    u32 first = 0, frames = 0; // its frames in the strip
    f32 fps = 10;
    bool loop = true;
};
struct Strip {
    assets::CookedTexture picture;
    std::vector<StripClip> clips;
    u32 frames = 0;
};
// Every clip, frame by frame. The rig is left at rest. False with why at the first frame that fails.
bool bake_strip(Rig& rig, PoseScript& script, const PixelDoc& doc, const BakeOptions& options, Strip& out, std::string& why);

} // namespace forge::pixel
