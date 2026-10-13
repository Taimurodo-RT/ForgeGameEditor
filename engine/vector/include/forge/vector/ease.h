#pragma once

// How a change speeds up and slows down: a share t of the time gone (0 … 1) gives the share of the change. The same
// seven words as movement in the «Интерфейс» tab (ui_design.h, Easing) and the same curves the interface draws them
// with (RmlUi's tweens), so a morph keyed beside a layer's movement keeps in step with it. Same bits on every system.
//
//   vector::mix(pair, vector::ease(vector::Ease::Smooth, t), shape);

#include "forge/core/types.h"

#include <string_view>

namespace forge::vector {

enum class Ease : u8 {
    Smooth,  // starts and ends gently (cubic in and out)
    Linear,  // even
    In,      // speeds up (cubic)
    Out,     // slows down (cubic)
    Back,    // overshoots a little and comes back
    Bounce,  // bounces at the end
    Elastic, // springs at the end
};
inline constexpr Ease kEases[] = {Ease::Smooth, Ease::Linear, Ease::In, Ease::Out, Ease::Back, Ease::Bounce, Ease::Elastic};

// The share of the change at t. t is held to 0 … 1 and the ends are exact: 0 gives 0, 1 gives 1. Between them Back and
// Elastic go past 1 for a while (up to about 1.1 and 1.26).
f64 ease(Ease e, f64 t);

// "smooth", "linear", "in", "out", "back", "bounce", "elastic": the interface tab's words.
const char* ease_word(Ease e);
bool ease_from_word(std::string_view word, Ease& out);

} // namespace forge::vector
