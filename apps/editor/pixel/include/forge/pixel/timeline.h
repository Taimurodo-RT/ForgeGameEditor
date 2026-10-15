#pragma once

// The timeline of «Пиксельные» and its code panel as numbers (step 3 of docs/models/движение-модель.md, «Вкладка
// (шаг 3)»): where a moment stands on it, zooming round the mouse, the ruler's step, a flag of a time event dragged, a
// block's words, the lines and characters of a call in the code.
//
//   TimeView v{.width = 800, .length = 0.75, .fps = 12};
//   v.zoom(2, 200);                         // twice as near, the moment under x = 200 stays there
//   drag_flag(rec, lengths, 0, 0.5, false)  // the first event's flag to 0.5 s: its length, the rest move on

#include "forge/pixel/code_sites.h"
#include "forge/pixel/motion.h"

#include <string>
#include <string_view>

namespace forge::pixel {

// Points of the timeline a frame at most.
inline constexpr f64 kMostPerFrame = 64;

struct TimeView {
    f64 width = 0;  // the timeline's points
    f64 length = 0; // the clip's seconds
    f32 fps = 10;
    f64 start = 0;  // the moment at its left edge
    f64 scale = 0;  // points a second; 0 until set: all of the clip in its width

    f64 fit() const;  // points a second that show all of the clip
    f64 most() const; // kMostPerFrame points a frame
    f64 per_second() const { return scale > 0 ? scale : fit(); }
    f64 x(f64 t) const { return (t - start) * per_second(); }
    f64 t(f64 x) const { return start + x / per_second(); }
    // Nearer by `factor` (farther below 1), the moment under at_x staying there; between fit() and most().
    void zoom(f64 factor, f64 at_x);
    // Along by dx points (right is later).
    void pan(f64 dx);
    // Moment t in the middle.
    void center(f64 t);
    // The scale between fit() and most(), the start so that nothing past the clip's ends shows when it fits.
    void clamp();
};

// The ruler's step in frames: 1, 2, 5, 10, 20, 50 … the least with its marks at least `gap` points apart.
u32 ruler_step(f64 points_per_frame, f64 gap);

// A time event's flag dragged to moment `to` of the clip. Its length becomes to − its start, not below 0, to thousandths;
// the events after it move on with it, as the code runs on later. With only_this the next event of another name gives
// up as much (not below 0), so the flags after it stay where they are. The lengths the file then holds.
MotionLengths drag_flag(const Recording& rec, const MotionLengths& now, usize event, f64 to, bool only_this);

// What a block does, as the timeline writes it on the block: "y → 2", "fill → #ff0000", "morph → mouth_open",
// "rot → назад", "связь", "пружина → 3", "tween".
std::string block_words(const Recording& rec, const Block& b);

// «Другое зерно»: the next seed, splitmix64 of this one below 1 000 000 (another one when they meet).
u64 other_seed(u64 seed);

// The lines (1-based) a span of the source is on: the first and the last.
void span_lines(std::string_view source, CodeSpan span, u32& first, u32& last);
// A byte of the source as a character of it (UTF-8), as text fields count.
u32 char_index(std::string_view source, u32 byte);
// A character of the source as its byte.
u32 byte_index(std::string_view source, u32 character);
// The call of the code at byte `at`: the innermost whose span holds it; -1 for none.
i32 site_at(const CodeSites& code, u32 at);

} // namespace forge::pixel
