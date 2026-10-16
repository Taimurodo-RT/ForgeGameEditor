#pragma once

// How a clip of a character is cut into frames (step 3 of docs/models/движение-модель.md, «Вкладка (шаг 3)»). A clip of
// the code is as long as its recording; its range (all of it unless the file says) is played and baked, and the frames
// are counted from it: frame k at from + k / fps, as many as the range holds by the clip's «Повторять». A clip of
// pose(t) alone has its «Кадров», frame k at k / fps, as before.
//
//   cut_clip(jump, true, 0.75)   // fps 12, no «Повторять»: 10 frames, 0, 1/12 … 0.75; with it 9, the last at 8/12

#include "forge/pixel/doc.h"

#include <string>

namespace forge::pixel {

class PoseScript;
class Rig;

struct ClipCut {
    bool code = false;    // the code has a clip of that name: its frames are counted
    f64 length = 0;       // the clip: its recording's length, or «Кадров» / fps
    f64 from = 0, to = 0; // what is played and baked: the file's range cut to the length, else all of the clip
    bool cut = false;     // the file's range reached past the clip's end
    u32 frames = 1;
    f32 fps = 10;
    bool loop = true;
    // The moment of frame k: from + k / fps; of the code without «Повторять» the last one at `to` exactly.
    f64 time(u32 k) const;
};

// How many frames [from, to] holds at fps: with loop ⌈(to − from)·fps⌉, the frame at `to` being the one at `from`
// again; without it one more, the last at `to`. At least one.
u32 range_frames(f64 from, f64 to, f32 fps, bool loop);
// Clip c of the file: of the code, `length` seconds long, or not of the code.
ClipCut cut_clip(const ClipSpec& c, bool code, f64 length);
// All of the clip, its range aside: the frames the timeline and «кадр 3 из 8» count.
ClipCut whole_clip(const ClipSpec& c, bool code, f64 length);
// Clip c as the script records it now, its waits and durations from the file. False with why when the code fails.
bool cut_clip(PoseScript& script, const Rig& rig, const PixelDoc& doc, const ClipSpec& c, ClipCut& out, std::string& why);
// The file with every clip of the code's "frames" counted as cut_clip counts them now.
bool count_frames(PoseScript& script, const Rig& rig, PixelDoc& doc, std::string& why);

} // namespace forge::pixel
