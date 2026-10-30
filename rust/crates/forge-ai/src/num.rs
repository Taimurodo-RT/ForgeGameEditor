//! Fixed-point numbers. The only place in the crate that multiplies or divides them.
//!
//! Rules (docs/ai/data-model.md, «Правила счёта»): every operation rounds to the
//! nearest value, ties to even; overflow saturates at the type bounds; values from
//! JSON are converted once, from exact ten-thousandths ([`Dec4`]).

use std::cmp::Ordering;
use std::ops::{Add, Mul, Sub};

/// A JSON number with at most 4 decimals, stored exactly as value × 10 000.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Dec4(pub i64);

impl Dec4 {
    pub const SCALE: i64 = 10_000;

    pub fn from_int(v: i64) -> Self {
        Self(v * Self::SCALE)
    }
}

/// Q16.16: needs, curves, weights, everything except B and S.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Fx(pub i32);

/// Q32.32: the product B and the score S.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Fx32(pub i64);

/// `n / d` rounded to nearest, ties to even. `d` must not be zero.
fn div_round(n: i128, d: i128) -> i128 {
    debug_assert!(d != 0);
    let (n, d) = if d < 0 { (-n, -d) } else { (n, d) };
    let q = n.div_euclid(d);
    let r = n.rem_euclid(d);
    match (2 * r).cmp(&d) {
        Ordering::Less => q,
        Ordering::Greater => q + 1,
        Ordering::Equal => q + (q & 1),
    }
}

fn sat_i32(v: i128) -> i32 {
    v.clamp(i32::MIN as i128, i32::MAX as i128) as i32
}

fn sat_i64(v: i128) -> i64 {
    v.clamp(i64::MIN as i128, i64::MAX as i128) as i64
}

impl Fx {
    pub const FRAC: u32 = 16;
    pub const ONE: Fx = Fx(1 << Self::FRAC);
    pub const ZERO: Fx = Fx(0);
    pub const MIN: Fx = Fx(i32::MIN);
    pub const MAX: Fx = Fx(i32::MAX);

    pub fn from_dec4(d: Dec4) -> Self {
        Fx(sat_i32(div_round(d.0 as i128 * (1 << Self::FRAC), Dec4::SCALE as i128)))
    }

    pub fn from_int(v: i32) -> Self {
        Fx(sat_i32((v as i128) << Self::FRAC))
    }

    /// Division; `None` on a zero divisor (the caller saturates and logs a warning).
    pub fn checked_div(self, o: Fx) -> Option<Fx> {
        (o.0 != 0).then(|| Fx(sat_i32(div_round((self.0 as i128) << Self::FRAC, o.0 as i128))))
    }

    /// Division with the rule for a zero divisor: saturate towards the sign of the dividend.
    pub fn div_sat(self, o: Fx) -> Fx {
        self.checked_div(o).unwrap_or(if self.0 < 0 { Fx::MIN } else { Fx::MAX })
    }

    /// `a + (b − a)·num/den` with one rounding: the interpolation of a point curve.
    pub fn lerp_ratio(a: Fx, b: Fx, num: Fx, den: Fx) -> Fx {
        if den.0 == 0 {
            return a;
        }
        let delta = div_round((b.0 as i128 - a.0 as i128) * num.0 as i128, den.0 as i128);
        Fx(sat_i32(a.0 as i128 + delta))
    }
}

impl Fx32 {
    pub const FRAC: u32 = 32;
    pub const ONE: Fx32 = Fx32(1 << Self::FRAC);

    pub fn from_dec4(d: Dec4) -> Self {
        Fx32(sat_i64(div_round(d.0 as i128 * (1i128 << Self::FRAC), Dec4::SCALE as i128)))
    }

    pub fn from_fx(v: Fx) -> Self {
        Fx32((v.0 as i64) << (Self::FRAC - Fx::FRAC))
    }

    pub fn checked_div(self, o: Fx32) -> Option<Fx32> {
        (o.0 != 0).then(|| Fx32(sat_i64(div_round((self.0 as i128) << Self::FRAC, o.0 as i128))))
    }
}

/// Saturating.
impl Add for Fx {
    type Output = Fx;
    fn add(self, o: Fx) -> Fx {
        Fx(self.0.saturating_add(o.0))
    }
}

/// Saturating.
impl Sub for Fx {
    type Output = Fx;
    fn sub(self, o: Fx) -> Fx {
        Fx(self.0.saturating_sub(o.0))
    }
}

/// Rounded to nearest, ties to even; saturating.
impl Mul for Fx {
    type Output = Fx;
    fn mul(self, o: Fx) -> Fx {
        Fx(sat_i32(div_round(self.0 as i128 * o.0 as i128, 1 << Fx::FRAC)))
    }
}

/// Rounded to nearest, ties to even; saturating.
impl Mul for Fx32 {
    type Output = Fx32;
    fn mul(self, o: Fx32) -> Fx32 {
        Fx32(sat_i64(div_round(self.0 as i128 * o.0 as i128, 1i128 << Fx32::FRAC)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ties_go_to_even() {
        assert_eq!(div_round(5, 2), 2);
        assert_eq!(div_round(7, 2), 4);
        assert_eq!(div_round(-5, 2), -2);
        assert_eq!(div_round(-7, 2), -4);
        assert_eq!(div_round(5, -2), -2);
        assert_eq!(div_round(9, 4), 2);
        assert_eq!(div_round(11, 4), 3);
    }

    #[test]
    fn decimals_convert_exactly() {
        assert_eq!(Fx::from_dec4(Dec4(10_000)), Fx::ONE);
        assert_eq!(Fx::from_dec4(Dec4(1_000)).0, 6554); // 0.1 -> 6553.6
        assert_eq!(Fx::from_dec4(Dec4(-1_000)).0, -6554);
        assert_eq!(Fx::from_dec4(Dec4(5_000)).0, 32768);
        assert_eq!(Fx32::from_dec4(Dec4(1)).0, 429497); // 0.0001 * 2^32 = 429496.7296
        // With 4 decimals a tie is impossible: 10 000 = 2^4 · 625 and 625 is odd.
        for d in 0..10_000 {
            let exact = d as i128 * 65536;
            assert_ne!(exact % 10_000, 5_000);
        }
    }

    #[test]
    fn multiply_rounds_once_and_saturates() {
        let half = Fx(32768);
        assert_eq!((half * half).0, 16384);
        // 1/65536 * 1/2 = tie between 0 and 1 ulp -> 0 (even).
        assert_eq!((Fx(1) * half).0, 0);
        assert_eq!((Fx(3) * half).0, 2);
        assert_eq!(Fx::MAX * Fx::from_int(2), Fx::MAX);
        assert_eq!(Fx::MIN * Fx::from_int(2), Fx::MIN);
        assert_eq!(Fx::MAX + Fx::ONE, Fx::MAX);
        assert_eq!(Fx32::ONE * Fx32::ONE, Fx32::ONE);
    }

    #[test]
    fn divide_by_zero_saturates() {
        assert_eq!(Fx::ONE.checked_div(Fx::ZERO), None);
        assert_eq!(Fx::ONE.div_sat(Fx::ZERO), Fx::MAX);
        assert_eq!(Fx(-5).div_sat(Fx::ZERO), Fx::MIN);
        assert_eq!(Fx::ONE.div_sat(Fx::from_int(4)).0, 16384);
        assert_eq!(Fx(1).div_sat(Fx::from_int(2)).0, 0);
        assert_eq!(Fx(3).div_sat(Fx::from_int(2)).0, 2);
    }

    #[test]
    fn lerp_has_one_rounding() {
        // y0 = 0, y1 = 1, x at 1/3 of the segment.
        let y = Fx::lerp_ratio(Fx::ZERO, Fx::ONE, Fx::from_int(1), Fx::from_int(3));
        assert_eq!(y.0, 21845); // 65536 / 3 = 21845.33
        let y = Fx::lerp_ratio(Fx::ONE, Fx::ZERO, Fx::from_int(1), Fx::from_int(3));
        assert_eq!(y.0, 65536 - 21845);
    }
}
