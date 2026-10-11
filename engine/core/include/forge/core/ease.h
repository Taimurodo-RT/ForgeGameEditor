#pragma once

// How a movement goes from its start to its end over its time (step 14.3b): a share t of the time gone (0 … 1) gives
// the share of the way. For what a game moves by itself in its world (a gem swapped, falling, fading), the same few
// curves for every module.
//
//   const f64 t = ease::progress(elapsed, 0.15);
//   x = ease::lerp(from, to, ease::in_out(t));

#include "forge/core/types.h"

#include <cmath>

namespace forge::ease {

// elapsed of duration as 0 … 1 (a duration of 0 or less: done).
inline f64 progress(f64 elapsed, f64 duration) {
    if (duration <= 0 || elapsed >= duration) return 1;
    return elapsed <= 0 ? 0 : elapsed / duration;
}

inline f64 clamp01(f64 t) { return t < 0 ? 0 : t > 1 ? 1 : t; }

// Even.
inline f64 linear(f64 t) { return clamp01(t); }

// Starts and ends gently (smoothstep): half the way at half the time.
inline f64 in_out(f64 t) {
    t = clamp01(t);
    return t * t * (3 - 2 * t);
}

// Falling from rest: faster and faster, as under gravity (the way grows as the square of the time).
inline f64 fall(f64 t) {
    t = clamp01(t);
    return t * t;
}

// How long a fall of distance takes from rest with acceleration g (both in the same units: tiles and tiles / s²).
inline f64 fall_time(f64 distance, f64 g) { return distance <= 0 || g <= 0 ? 0 : std::sqrt(2 * distance / g); }

inline f64 lerp(f64 a, f64 b, f64 t) { return a + (b - a) * t; }

} // namespace forge::ease
