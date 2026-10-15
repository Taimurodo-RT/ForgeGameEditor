#pragma once

// Motion Canvas's timing functions, springs and arc (docs/models/движение-модель.md, step 1), for the clips of
// «Пиксельные» and later the game's own playback of them. The formulas are theirs (packages/core/src/tweening at
// commit 7b91435), with trig in place of Math, so the same bits come out on every system.
//
//   timing(Timing::OutQuad, 0.5)                       → 0.75
//   spring_steps(spring_preset("plop"), 0, 10, 0.001)  → the positions 120 times a second until it settles

#include "forge/core/types.h"

#include <string_view>
#include <vector>

namespace forge::vector {

enum class Timing : u8 {
    // The seven words of ease.h with their own curves (the interface's): «smooth» is Motion Canvas's easeInOutCubic,
    // «back» its easeOutBack; «bounce» and «elastic» are RmlUi's, not theirs.
    Smooth, Linear, In, Out, Back, Bounce, Elastic,
    // Motion Canvas's, as in_sine for easeInSine.
    InSine, OutSine, InOutSine,
    InQuad, OutQuad, InOutQuad,
    InCubic, OutCubic, InOutCubic,
    InQuart, OutQuart, InOutQuart,
    InQuint, OutQuint, InOutQuint,
    InExpo, OutExpo, InOutExpo,
    InCirc, OutCirc, InOutCirc,
    InBack, OutBack, InOutBack,
    InElastic, OutElastic, InOutElastic,
    InBounce, OutBounce, InOutBounce,
};
inline constexpr usize kTimingCount = static_cast<usize>(Timing::InOutBounce) + 1;

// The share of the change at t, t held to 0 … 1; 0 gives 0 and 1 gives 1 exactly. Back and Elastic go past the ends
// for a while, as theirs do.
f64 timing(Timing t, f64 x);
// "smooth", …, "in_out_bounce".
const char* timing_word(Timing t);
bool timing_from_word(std::string_view word, Timing& out);

// A spring as Motion Canvas's: mass, stiffness, damping, the speed it starts with.
struct Spring {
    f64 mass = 0.05, stiffness = 10, damping = 0.5, velocity = 0;
};
// Their presets by our words: "beat", "plop", "bounce", "swing", "jump", "strike", "smooth". False for another word.
bool spring_preset(std::string_view word, Spring& out);
// The words, in that order, for messages.
inline constexpr const char* kSpringWords = "beat, plop, bounce, swing, jump, strike, smooth";

// Motion Canvas's spring from `from` to `to`: steps of 1/120 s by Hooke's law until both the distance to `to` and the
// speed are below `tolerance`. Out: the position after each step, the last one the step it settled on, so it settles
// at out.size() / 120 s. False with why (Russian) when the spring is not one (mass ≤ 0, stiffness or damping < 0) or
// has not settled in `max_seconds`.
bool spring_steps(const Spring& s, f64 from, f64 to, f64 tolerance, std::vector<f64>& out, const char*& why,
                  f64 max_seconds = 60);
inline constexpr f64 kSpringRate = 120;

// Motion Canvas's arcLerp: the shares of the way along x and along y at `value`, so the point goes on a quarter of an
// ellipse rather than straight. `ratio` is (from − to).x / (from − to).y, as theirs takes it by default.
void arc_shares(f64 value, bool reverse, f64 ratio, f64& x, f64& y);

} // namespace forge::vector
