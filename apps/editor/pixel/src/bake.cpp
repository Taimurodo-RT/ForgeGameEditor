#include "forge/pixel/bake.h"

#include <algorithm>

namespace forge::pixel {

std::vector<std::string> apply_pivots(Rig& rig, const PixelDoc& doc) {
    // A part the file has no pivot for turns round the middle of its rest bounds, whatever was set before (Ctrl+Z).
    for (const Part& p : rig.parts()) {
        const auto it = doc.pivots.find(p.id);
        rig.set_pivot(p.id, it != doc.pivots.end() ? it->second : vector::Point{(p.x0 + p.x1) / 2, (p.y0 + p.y1) / 2});
    }
    std::vector<std::string> missing;
    for (const auto& [id, at] : doc.pivots)
        if (rig.part(id) < 0) missing.push_back(id);
    return missing;
}

bool frame_pose(PoseScript& script, const Rig& rig, const PixelDoc& doc, const ClipSpec& clip, u32 k, Pose& out, std::string& why) {
    ClipCut cut;
    std::string ignore;
    if (cut_clip(script, rig, doc, clip, cut, ignore) && script.pose(rig, cut.time(k), clip.state, doc.params, doc.seed, out)) return true;
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

bool bake_strip(Rig& rig, PoseScript& script, const PixelDoc& doc, const BakeOptions& options, u32 frame_limit, Strip& out,
                std::string& why) {
    out = {};
    std::vector<ClipCut> cuts(doc.clips.size());
    for (usize i = 0; i < doc.clips.size(); ++i) {
        if (!cut_clip(script, rig, doc, doc.clips[i], cuts[i], why)) return false;
        out.frames += cuts[i].frames;
    }
    if (out.frames == 0) {
        why = "нет ни одного клипа с кадрами";
        return false;
    }
    if (out.frames > frame_limit) {
        const u32 d = out.frames % 10, h = out.frames % 100;
        const u32 over = out.frames - frame_limit, od = over % 10, oh = over % 100;
        why = "в клипах " + std::to_string(out.frames) + (d == 1 && h != 11 ? " кадр" : d >= 2 && d <= 4 && (h < 12 || h > 14) ? " кадра" : " кадров") +
              ", а предел игры " + std::to_string(frame_limit) + (od == 1 && oh != 11 ? ", лишний " : ", лишних ") + std::to_string(over) +
              ": уберите кадры у клипов, сузьте диапазоны или поднимите «Предел кадров»";
        out = {};
        return false;
    }
    const u32 w = doc.frame_w, h = doc.frame_h;
    if (static_cast<u64>(w) * out.frames > kMaxStripWidth) {
        why = "полоса " + std::to_string(static_cast<u64>(w) * out.frames) + " точек шире " + std::to_string(kMaxStripWidth) +
              ": столько видеокарта не возьмёт, уменьшите кадр или число кадров";
        out = {};
        return false;
    }
    out.picture.width = w * out.frames;
    out.picture.height = h;
    out.picture.rgba8.assign(static_cast<usize>(out.picture.width) * h * 4, 0);
    u32 at = 0;
    bool ok = true;
    for (usize i = 0; i < doc.clips.size(); ++i) {
        const ClipSpec& c = doc.clips[i];
        out.clips.push_back({c.state, at, cuts[i].frames, c.fps, c.loop});
        for (u32 k = 0; k < cuts[i].frames && ok; ++k, ++at) {
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
