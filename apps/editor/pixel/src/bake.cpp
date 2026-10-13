#include "forge/pixel/bake.h"

#include <algorithm>

namespace forge::pixel {

std::vector<std::string> apply_pivots(Rig& rig, const PixelDoc& doc) {
    std::vector<std::string> missing;
    for (const auto& [id, at] : doc.pivots) {
        if (rig.part(id) < 0) missing.push_back(id);
        else rig.set_pivot(id, at);
    }
    return missing;
}

bool frame_pose(PoseScript& script, const Rig& rig, const PixelDoc& doc, const ClipSpec& clip, u32 k, Pose& out, std::string& why) {
    const f64 t = clip.fps > 0 ? k / static_cast<f64>(clip.fps) : 0;
    if (script.pose(rig, t, clip.state, doc.params, doc.seed, out)) return true;
    why = "клип «" + clip.state + "», кадр " + std::to_string(k + 1) + ": " + script.error();
    return false;
}

BakeOptions bake_options(const PixelDoc& doc, const std::vector<Rgba8>& game_colors) {
    BakeOptions o;
    if (doc.game_palette) o.palette = game_colors;
    o.outline = doc.outline;
    o.outline_color = doc.outline_color;
    return o;
}

bool bake_frame(Rig& rig, PoseScript& script, const PixelDoc& doc, const ClipSpec& clip, u32 k, const BakeOptions& options,
                assets::CookedTexture& out, std::string& why) {
    Pose pose;
    if (!frame_pose(script, rig, doc, clip, k, pose, why)) return false;
    // Moves in whole pixels of the frame, so a part does not shimmer from frame to frame.
    const f64 scale = rig.to_pixels(doc.frame_w, doc.frame_h).a;
    std::string err;
    if (!rig.pose(pose, err, scale > 0 ? 1 / scale : 0)) {
        why = "клип «" + clip.state + "», кадр " + std::to_string(k + 1) + ": " + err;
        return false;
    }
    out = rig.bake(doc.frame_w, doc.frame_h, options);
    return true;
}

bool bake_strip(Rig& rig, PoseScript& script, const PixelDoc& doc, const BakeOptions& options, Strip& out, std::string& why) {
    out = {};
    for (const ClipSpec& c : doc.clips) out.frames += c.frames;
    if (out.frames == 0) {
        why = "нет ни одного клипа с кадрами";
        return false;
    }
    const u32 w = doc.frame_w, h = doc.frame_h;
    out.picture.width = w * out.frames;
    out.picture.height = h;
    out.picture.rgba8.assign(static_cast<usize>(out.picture.width) * h * 4, 0);
    u32 at = 0;
    bool ok = true;
    for (const ClipSpec& c : doc.clips) {
        out.clips.push_back({c.state, at, c.frames, c.fps, c.loop});
        for (u32 k = 0; k < c.frames && ok; ++k, ++at) {
            assets::CookedTexture frame;
            ok = bake_frame(rig, script, doc, c, k, options, frame, why);
            if (!ok) break;
            for (u32 y = 0; y < h; ++y)
                std::copy_n(frame.rgba8.begin() + static_cast<std::ptrdiff_t>(y) * w * 4, w * 4,
                            out.picture.rgba8.begin() + (static_cast<std::ptrdiff_t>(y) * out.picture.width + at * w) * 4);
        }
        if (!ok) break;
    }
    std::string ignore;
    rig.pose({}, ignore);
    if (!ok) out = {};
    return ok;
}

} // namespace forge::pixel
