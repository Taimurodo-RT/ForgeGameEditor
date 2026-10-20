#include "forge/pixel/clip_cut.h"

#include "forge/pixel/script.h"

#include <algorithm>
#include <cmath>

namespace forge::pixel {

namespace {

// (to − from)·fps a hair over a whole number is that number: 0.1 · 30 is 3.0000000000000004.
constexpr f64 kFrameSlack = 1e-6;

} // namespace

f64 ClipCut::time(u32 k) const {
    const f64 t = from + k / static_cast<f64>(fps);
    if (!code) return t;
    if (!loop && k + 1 >= frames) return to;
    return std::min(t, to);
}

u32 range_frames(f64 from, f64 to, f32 fps, bool loop) {
    const f64 span = std::max(0.0, (to - from) * fps);
    const f64 n = std::ceil(span - kFrameSlack);
    const f64 frames = std::max(0.0, n) + (loop ? 0 : 1);
    return static_cast<u32>(std::clamp(frames, 1.0, static_cast<f64>(kMaxStripFrames)));
}

ClipCut whole_clip(const ClipSpec& c, bool code, f64 length) {
    ClipCut out;
    out.code = code;
    out.fps = c.fps > 0 ? c.fps : 10;
    out.loop = c.loop;
    if (!code) {
        out.frames = std::max(1u, c.frames);
        out.length = out.frames / static_cast<f64>(out.fps);
        out.to = out.length;
        return out;
    }
    out.length = std::max(0.0, length);
    out.to = out.length;
    out.frames = range_frames(0, out.length, out.fps, out.loop);
    return out;
}

ClipCut cut_clip(const ClipSpec& c, bool code, f64 length) {
    ClipCut out = whole_clip(c, code, length);
    if (!code || !c.ranged) return out;
    out.cut = c.to > out.length + 1e-9;
    out.from = std::clamp(c.from, 0.0, out.length);
    out.to = std::clamp(c.to, out.from, out.length);
    out.frames = range_frames(out.from, out.to, out.fps, out.loop);
    return out;
}

bool cut_clip(PoseScript& script, const Rig& rig, const PixelDoc& doc, const ClipSpec& c, ClipCut& out, std::string& why) {
    const std::vector<std::string>& clips = script.clips();
    if (!std::binary_search(clips.begin(), clips.end(), c.state)) {
        out = cut_clip(c, false, 0);
        return true;
    }
    script.set_lengths(doc.lengths); // every clip's: a start_from records another (part 5б)
    const Recording* rec = script.recording(rig, c.state, doc.params, doc.seed);
    if (!rec) {
        why = "клип «" + c.state + "»: " + script.error();
        return false;
    }
    out = cut_clip(c, true, rec->length);
    return true;
}

bool count_frames(PoseScript& script, const Rig& rig, PixelDoc& doc, std::string& why) {
    for (ClipSpec& c : doc.clips) {
        ClipCut cut;
        if (!cut_clip(script, rig, doc, c, cut, why)) return false;
        if (cut.code) c.frames = cut.frames;
    }
    return true;
}

} // namespace forge::pixel
