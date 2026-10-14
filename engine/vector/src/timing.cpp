#include "forge/vector/timing.h"

#include "forge/vector/ease.h"
#include "forge/vector/trig.h"

#include <cmath>
#include <iterator>

namespace forge::vector {

namespace {

constexpr f64 kLn2 = 0.69314718055994530942;

// 2^x as Math.pow(2, x), from trig's exponent.
f64 pow2(f64 x) { return trig::exp(x * kLn2); }

f64 sq(f64 x) { return x * x; }

f64 asin(f64 x) { return trig::atan2(x, std::sqrt(1 - x * x)); }

f64 acos(f64 x) { return trig::atan2(std::sqrt(1 - x * x), x); }

// Theirs keeps 1.505 where the textbook has 1.5 (createEaseOutBounce): kept, so the curve is theirs.
f64 out_bounce(f64 v) {
    constexpr f64 n = 7.5625, d = 2.75;
    if (v < 1 / d) return n * v * v;
    if (v < 2 / d) {
        v -= 1.505 / d;
        return n * v * v + 0.75;
    }
    if (v < 2.5 / d) {
        v -= 2.25 / d;
        return n * v * v + 0.9375;
    }
    v -= 2.625 / d;
    return n * v * v + 0.984375;
}

constexpr f64 kBack = 1.70158;

constexpr const char* kWords[] = {
    "smooth",   "linear",    "in",           "out",      "back",      "bounce",       "elastic",
    "in_sine",  "out_sine",  "in_out_sine",  "in_quad",  "out_quad",  "in_out_quad",  "in_cubic",
    "out_cubic", "in_out_cubic", "in_quart", "out_quart", "in_out_quart", "in_quint", "out_quint",
    "in_out_quint", "in_expo", "out_expo",   "in_out_expo", "in_circ", "out_circ",    "in_out_circ",
    "in_back",  "out_back",  "in_out_back",  "in_elastic", "out_elastic", "in_out_elastic", "in_bounce",
    "out_bounce", "in_out_bounce",
};
static_assert(std::size(kWords) == kTimingCount);

struct Preset {
    const char* word;
    Spring spring;
};
// makeSpring(mass, stiffness, damping, initialVelocity) of spring.ts.
constexpr Preset kPresets[] = {
    {"beat", {0.13, 5.7, 1.2, 10.0}},   {"plop", {0.2, 20.0, 0.68, 0.0}},  {"bounce", {0.08, 4.75, 0.05, 0.0}},
    {"swing", {0.39, 19.85, 2.82, 0.0}}, {"jump", {0.04, 10.0, 0.7, 8.0}}, {"strike", {0.03, 20.0, 0.9, 4.8}},
    {"smooth", {0.16, 15.35, 1.88, 0.0}},
};

} // namespace

f64 timing(Timing t, f64 v) {
    if (!(v > 0)) return 0; // also NaN
    if (v >= 1) return 1;
    switch (t) {
    case Timing::Smooth: return ease(Ease::Smooth, v);
    case Timing::Linear: return v;
    case Timing::In: return ease(Ease::In, v);
    case Timing::Out: return ease(Ease::Out, v);
    case Timing::Back: return ease(Ease::Back, v);
    case Timing::Bounce: return ease(Ease::Bounce, v);
    case Timing::Elastic: return ease(Ease::Elastic, v);

    case Timing::InSine: return 1 - trig::cos(v * trig::kPi / 2);
    case Timing::OutSine: return trig::sin(v * trig::kPi / 2);
    case Timing::InOutSine: return -(trig::cos(trig::kPi * v) - 1) / 2;

    case Timing::InQuad: return v * v;
    case Timing::OutQuad: return 1 - sq(1 - v);
    case Timing::InOutQuad: return v < 0.5 ? 2 * v * v : 1 - sq(-2 * v + 2) / 2;

    case Timing::InCubic: return v * v * v;
    case Timing::OutCubic: return 1 - sq(1 - v) * (1 - v);
    case Timing::InOutCubic: return v < 0.5 ? 4 * v * v * v : 1 - sq(-2 * v + 2) * (-2 * v + 2) / 2;

    case Timing::InQuart: return sq(v * v);
    case Timing::OutQuart: return 1 - sq(sq(1 - v));
    case Timing::InOutQuart: return v < 0.5 ? 8 * sq(v * v) : 1 - sq(sq(-2 * v + 2)) / 2;

    case Timing::InQuint: return sq(v * v) * v;
    case Timing::OutQuint: return 1 - sq(sq(1 - v)) * (1 - v);
    case Timing::InOutQuint: return v < 0.5 ? 16 * sq(v * v) * v : 1 - sq(sq(-2 * v + 2)) * (-2 * v + 2) / 2;

    case Timing::InExpo: return pow2(10 * v - 10);
    case Timing::OutExpo: return 1 - pow2(-10 * v);
    case Timing::InOutExpo: return v < 0.5 ? pow2(20 * v - 10) / 2 : (2 - pow2(-20 * v + 10)) / 2;

    case Timing::InCirc: return 1 - std::sqrt(1 - v * v);
    case Timing::OutCirc: return std::sqrt(1 - sq(v - 1));
    case Timing::InOutCirc:
        return v < 0.5 ? (1 - std::sqrt(1 - sq(2 * v))) / 2 : (std::sqrt(1 - sq(-2 * v + 2)) + 1) / 2;

    case Timing::InBack: return (kBack + 1) * v * v * v - kBack * v * v;
    case Timing::OutBack: return 1 + (kBack + 1) * sq(v - 1) * (v - 1) + kBack * sq(v - 1);
    case Timing::InOutBack: {
        constexpr f64 s = kBack, k = 1.525;
        return v < 0.5 ? (sq(2 * v) * ((s * k + 1) * 2 * v - s * k)) / 2
                       : (sq(2 * v - 2) * ((s * k + 1) * (v * 2 - 2) + s * k) + 2) / 2;
    }

    case Timing::InElastic: return -pow2(10 * v - 10) * trig::sin((v * 10 - 10.75) * 2.094395);
    case Timing::OutElastic: return pow2(-10 * v) * trig::sin((v * 10 - 0.75) * 2.094395) + 1;
    case Timing::InOutElastic:
        return v < 0.5 ? -(pow2(20 * v - 10) * trig::sin((20 * v - 11.125) * 1.39626)) / 2
                       : (pow2(-20 * v + 10) * trig::sin((20 * v - 11.125) * 1.39626)) / 2 + 1;

    case Timing::InBounce: return 1 - out_bounce(1 - v);
    case Timing::OutBounce: return out_bounce(v);
    case Timing::InOutBounce: return v < 0.5 ? (1 - out_bounce(1 - 2 * v)) / 2 : (1 + out_bounce(2 * v - 1)) / 2;
    }
    return v;
}

const char* timing_word(Timing t) {
    const auto i = static_cast<usize>(t);
    return i < kTimingCount ? kWords[i] : kWords[0];
}

bool timing_from_word(std::string_view word, Timing& out) {
    for (usize i = 0; i < kTimingCount; ++i)
        if (word == kWords[i]) {
            out = static_cast<Timing>(i);
            return true;
        }
    return false;
}

bool spring_preset(std::string_view word, Spring& out) {
    for (const Preset& p : kPresets)
        if (word == p.word) {
            out = p.spring;
            return true;
        }
    return false;
}

bool spring_steps(const Spring& s, f64 from, f64 to, f64 tolerance, std::vector<f64>& out, const char*& why,
                  f64 max_seconds) {
    out.clear();
    why = nullptr;
    if (!(s.mass > 0)) return why = "у пружины масса должна быть больше 0", false;
    if (!(s.stiffness >= 0)) return why = "у пружины жёсткость не меньше 0", false;
    if (!(s.damping >= 0)) return why = "у пружины затухание не меньше 0", false;
    if (!std::isfinite(s.velocity) || !std::isfinite(from) || !std::isfinite(to) || !(tolerance > 0))
        return why = "у пружины нужны числа, а допуск больше 0", false;
    // spring.ts: update(dt) as Hooke's law, the check after every step.
    constexpr f64 dt = 1 / kSpringRate;
    const auto steps = static_cast<usize>(max_seconds * kSpringRate);
    f64 position = from, velocity = s.velocity;
    for (usize i = 0; i < steps; ++i) {
        const f64 force = -s.stiffness * (position - to) - s.damping * velocity;
        velocity += (force / s.mass) * dt;
        position += velocity * dt;
        out.push_back(position);
        if (std::fabs(to - position) < tolerance && std::fabs(velocity) < tolerance) return true;
    }
    out.clear();
    return why = "пружина не успокаивается за минуту: больше затухания", false;
}

void arc_shares(f64 value, bool reverse, f64 ratio, f64& x, f64& y) {
    // interpolationFunctions.ts arcLerp. No move at all (0 / 0): straight. A move along x only has an infinite ratio,
    // which theirs turns into 0 (1 / ∞); −∞ gives them NaN, here the same as +∞.
    if (ratio != ratio) {
        x = y = value;
        return;
    }
    bool flip = reverse;
    if (std::isinf(ratio))
        ratio = 0;
    else if (ratio > 1)
        ratio = 1 / ratio;
    else
        flip = !flip;
    f64 c = 1 - value;
    if (c < -1) c = -1;
    if (c > 1) c = 1;
    const f64 normalized = flip ? acos(c) : asin(value);
    const f64 radians = normalized + (value * trig::kPi / 2 - normalized) * ratio;
    f64 s = 0, co = 0;
    trig::sin_cos(radians, s, co);
    x = s;
    y = 1 - co;
    if (reverse) {
        const f64 t = x;
        x = y;
        y = t;
    }
}

} // namespace forge::vector
