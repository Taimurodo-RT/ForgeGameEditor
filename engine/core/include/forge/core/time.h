#pragma once

#include "forge/core/types.h"

namespace forge {

// Monotonic time in nanoseconds since an arbitrary point.
u64 time_now_ns();

inline f64 ns_to_ms(u64 ns) { return static_cast<f64>(ns) / 1'000'000.0; }

} // namespace forge
