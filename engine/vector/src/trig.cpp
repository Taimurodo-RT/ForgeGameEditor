#include "forge/vector/trig.h"

#include <cmath>
#include <limits>

namespace forge::vector::trig {

namespace {

// π/2 in two parts (as fdlibm): the first has 33 bits, so k × it is exact for every k below 2^20 and the angle keeps
// its digits when the turns are taken away.
constexpr f64 kHalfPiHi = 1.57079632673412561417e+00;
constexpr f64 kHalfPiLo = 6.07710050650619224932e-11;
constexpr f64 kTwoOverPi = 6.36619772367581382433e-01;

// ln 2 in two parts, the same way for the exponent.
constexpr f64 kLn2Hi = 6.93147180369123816490e-01;
constexpr f64 kLn2Lo = 1.90821492927058770002e-10;
constexpr f64 kInvLn2 = 1.44269504088896338700e+00;

// Horner's rule: c[0] + x (c[1] + x (c[2] + …)).
template <usize N> f64 poly(const f64 (&c)[N], f64 x) {
    f64 s = c[N - 1];
    for (usize i = N - 1; i-- > 0;) s = s * x + c[i];
    return s;
}

// Taylor series in r² on |r| ≤ π/4: the first term left out is below 1e-17 of the result.
constexpr f64 kSin[] = {1, -1.0 / 6, 1.0 / 120, -1.0 / 5040, 1.0 / 362880, -1.0 / 39916800, 1.0 / 6227020800.0,
                        -1.0 / 1307674368000.0, 1.0 / 355687428096000.0};
constexpr f64 kCos[] = {1, -1.0 / 2, 1.0 / 24, -1.0 / 720, 1.0 / 40320, -1.0 / 3628800, 1.0 / 479001600.0,
                        -1.0 / 87178291200.0, 1.0 / 20922789888000.0};
// atan t = t − t³/3 + t⁵/5 − …, in t², for |t| ≤ tan(π/32).
constexpr f64 kAtan[] = {1, -1.0 / 3, 1.0 / 5, -1.0 / 7, 1.0 / 9, -1.0 / 11, 1.0 / 13, -1.0 / 15, 1.0 / 17};
// eʳ = 1 + r + r²/2! + …, for |r| ≤ ln 2 / 2: the first term left out (r¹⁴/14!) is below 5e-18.
constexpr f64 kExp[] = {1, 1, 1.0 / 2, 1.0 / 6, 1.0 / 24, 1.0 / 120, 1.0 / 720, 1.0 / 5040, 1.0 / 40320, 1.0 / 362880,
                        1.0 / 3628800, 1.0 / 39916800, 1.0 / 479001600.0, 1.0 / 6227020800.0};

// atan of 0 ≤ t ≤ 1: the angle halved three times (atan t = 2 atan(t / (1 + √(1 + t²)))) brings t below tan(π/32),
// where the first term the series leaves out (t¹⁹/19) is below 1e-20.
f64 atan_unit(f64 t) {
    for (int i = 0; i < 3; ++i) t = t / (1 + std::sqrt(1 + t * t));
    return 8 * (t * poly(kAtan, t * t));
}

} // namespace

void sin_cos(f64 a, f64& s, f64& c) {
    if (!(std::fabs(a) <= 1e9)) { // also NaN
        s = c = std::numeric_limits<f64>::quiet_NaN();
        return;
    }
    const f64 k = std::floor(a * kTwoOverPi + 0.5); // a = k quarter turns + r
    const f64 r = (a - k * kHalfPiHi) - k * kHalfPiLo;
    const f64 r2 = r * r;
    const f64 sr = r * poly(kSin, r2), cr = poly(kCos, r2);
    switch (static_cast<i64>(k) & 3) { // two's complement: −1 & 3 is 3, as −1 ≡ 3 (mod 4)
    case 0: s = sr, c = cr; break;
    case 1: s = cr, c = -sr; break;
    case 2: s = -sr, c = -cr; break;
    default: s = -cr, c = sr; break;
    }
}

f64 sin(f64 a) {
    f64 s, c;
    sin_cos(a, s, c);
    return s;
}

f64 cos(f64 a) {
    f64 s, c;
    sin_cos(a, s, c);
    return c;
}

f64 atan2(f64 y, f64 x) {
    if (y != y || x != x) return std::numeric_limits<f64>::quiet_NaN();
    const f64 ax = std::fabs(x), ay = std::fabs(y);
    if (ax == 0 && ay == 0) return 0;
    f64 a = ay <= ax ? atan_unit(ay / ax) : kPi / 2 - atan_unit(ax / ay);
    if (x < 0) a = kPi - a;
    return y < 0 ? -a : a;
}

f64 exp(f64 x) {
    if (x != x) return x;
    if (x < -708) return 0;
    if (x > 709) return std::numeric_limits<f64>::infinity();
    const f64 n = std::floor(x * kInvLn2 + 0.5); // x = n ln 2 + r, |r| ≤ ln 2 / 2
    const f64 r = (x - n * kLn2Hi) - n * kLn2Lo;
    return std::ldexp(poly(kExp, r), static_cast<int>(n));
}

} // namespace forge::vector::trig
