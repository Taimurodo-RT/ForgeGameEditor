#pragma once

#include "forge/core/types.h"

namespace forge {

struct Vec2 {
    f32 x = 0;
    f32 y = 0;
    friend bool operator==(const Vec2&, const Vec2&) = default;
};

// Linear RGBA, 0..1 per channel.
struct Color {
    f32 r = 1;
    f32 g = 1;
    f32 b = 1;
    f32 a = 1;
    friend bool operator==(const Color&, const Color&) = default;
};

} // namespace forge
