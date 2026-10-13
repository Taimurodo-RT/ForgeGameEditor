#pragma once

// Sine, cosine, arctangent and exponent that give the same bits on every system (step 1 of the vector model). The C
// library's own differ between Windows and Linux in the last bit, and a morph or an arc read from SVG must come out
// the same on both. Only + − × ÷, the square root and rounding are used: IEEE 754 makes those exact everywhere.
// Accurate to about one unit of the last place (2e-16 relative), plenty for shapes.

#include "forge/core/types.h"

namespace forge::vector::trig {

inline constexpr f64 kPi = 3.14159265358979323846;

// sin and cos of a (radians) together: accurate for |a| up to about 1e6, still the same bits everywhere up to 1e9,
// NaN beyond.
void sin_cos(f64 a, f64& s, f64& c);
f64 sin(f64 a);
f64 cos(f64 a);
// The angle of (x, y) from the x axis, −π … π, as std::atan2; (0, 0) gives 0.
f64 atan2(f64 y, f64 x);
// e to the power x; 0 below −708, +∞ above 709.
f64 exp(f64 x);

} // namespace forge::vector::trig
