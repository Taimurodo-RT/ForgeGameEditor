#include "forge/vector/ease.h"

#include "forge/vector/trig.h"

#include <iterator>

namespace forge::vector {

namespace {

// The "in" halves, as RmlUi's TweenFunctions (third_party/rmlui/Source/Core/Tween.cpp), in double.
f64 cubic(f64 t) { return t * t * t; }

f64 back(f64 t) { return t * t * (2.70158 * t - 1.70158); }

f64 square(f64 t) { return t * t; }

f64 bounce(f64 t) {
    if (t > 1 - 1 / 2.75) return 1 - 7.5625 * square(1 - t);
    if (t > 1 - 2 / 2.75) return 1 - (7.5625 * square(1 - t - 1.5 / 2.75) + 0.75);
    if (t > 1 - 2.5 / 2.75) return 1 - (7.5625 * square(1 - t - 2.25 / 2.75) + 0.9375);
    return 1 - (7.5625 * square(1 - t - 2.625 / 2.75) + 0.984375);
}

f64 elastic(f64 t) {
    if (t == 0 || t == 1) return t;
    return -trig::exp(7.24 * (t - 1)) * trig::sin((t - 1.1) * 2 * trig::kPi / 0.4);
}

// The "out" of a curve is its "in" turned round; "in and out" is the "in" on the first half and the "out" on the
// second.
template <typename F> f64 out(F f, f64 t) { return 1 - f(1 - t); }

template <typename F> f64 in_out(F f, f64 t) { return t < 0.5 ? f(2 * t) * 0.5 : 0.5 + out(f, 2 * t - 1) * 0.5; }

constexpr const char* kWords[] = {"smooth", "linear", "in", "out", "back", "bounce", "elastic"};

} // namespace

f64 ease(Ease e, f64 t) {
    if (!(t > 0)) return 0; // also NaN
    if (t >= 1) return 1;
    switch (e) {
    case Ease::Smooth: return in_out(cubic, t);
    case Ease::Linear: return t;
    case Ease::In: return cubic(t);
    case Ease::Out: return out(cubic, t);
    case Ease::Back: return out(back, t);
    case Ease::Bounce: return out(bounce, t);
    case Ease::Elastic: return out(elastic, t);
    }
    return t;
}

const char* ease_word(Ease e) {
    const auto i = static_cast<usize>(e);
    return i < std::size(kWords) ? kWords[i] : kWords[0];
}

bool ease_from_word(std::string_view word, Ease& out) {
    for (Ease e : kEases)
        if (word == ease_word(e)) {
            out = e;
            return true;
        }
    return false;
}

} // namespace forge::vector
