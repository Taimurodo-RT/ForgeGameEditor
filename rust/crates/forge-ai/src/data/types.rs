//! Base types of the data model (docs/ai/data-model.md, «Базовые типы»).
//!
//! Each type checks its own format while deserializing. A failure carries the
//! validator code in front of the message (`E_RANGE|...`), so the loader can turn
//! a serde error into a diagnostic with the right code and JSON Pointer.

use std::borrow::Cow;
use std::fmt;

use schemars::{JsonSchema, Schema, SchemaGenerator, json_schema};
use serde::de::{self, Deserializer, Visitor};
use serde::{Deserialize, Serialize, Serializer};

use crate::num::Dec4;

/// Builds a serde error that the loader maps to `code`.
pub(crate) fn coded<E: de::Error>(code: &str, text: impl fmt::Display) -> E {
    E::custom(format_args!("{code}|{text}"))
}

fn is_lower(c: char) -> bool {
    c.is_ascii_lowercase()
}

fn is_word(c: char) -> bool {
    c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_'
}

/// `^[a-z][a-z0-9_]*$`
pub fn is_local_id(s: &str) -> bool {
    let mut it = s.chars();
    it.next().is_some_and(is_lower) && it.all(is_word)
}

/// `^[a-z]+\.[a-z][a-z0-9_]*(\.[a-z0-9_]+)*$`
pub fn is_id(s: &str) -> bool {
    let mut parts = s.split('.');
    let (Some(kind), Some(first)) = (parts.next(), parts.next()) else {
        return false;
    };
    !kind.is_empty()
        && kind.chars().all(is_lower)
        && is_local_id(first)
        && parts.all(|p| !p.is_empty() && p.chars().all(is_word))
}

macro_rules! string_type {
    ($(#[$m:meta])* $name:ident, $max:expr, $check:expr, $code:literal, $pattern:literal) => {
        $(#[$m])*
        #[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize)]
        #[serde(transparent)]
        pub struct $name(pub String);

        impl $name {
            pub fn as_str(&self) -> &str {
                &self.0
            }
        }

        impl<'de> Deserialize<'de> for $name {
            fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
                let s = String::deserialize(d)?;
                let check: fn(&str) -> bool = $check;
                if s.len() > $max || !check(&s) {
                    return Err(coded($code, format_args!("bad {} `{}`", stringify!($name), s)));
                }
                Ok(Self(s))
            }
        }

        impl JsonSchema for $name {
            fn schema_name() -> Cow<'static, str> {
                stringify!($name).into()
            }
            fn json_schema(_: &mut SchemaGenerator) -> Schema {
                json_schema!({ "type": "string", "pattern": $pattern, "maxLength": $max })
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str(&self.0)
            }
        }
    };
}

string_type!(
    /// Resource id `kind.name`.
    Id, 64, is_id, "E_ID_FORMAT", "^[a-z]+\\.[a-z][a-z0-9_]*(\\.[a-z0-9_]+)*$"
);
string_type!(
    /// Reference to a resource. Kind and existence are checked across files.
    Ref, 64, is_id, "E_ID_FORMAT", "^[a-z]+\\.[a-z][a-z0-9_]*(\\.[a-z0-9_]+)*$"
);
string_type!(
    /// Id unique inside its parent: node, port, phase, slot, step, group.
    LocalId, 32, is_local_id, "E_ID_FORMAT", "^[a-z][a-z0-9_]*$"
);
string_type!(
    /// Tag from `project.json` `tags`.
    Tag, 32, is_local_id, "E_ID_FORMAT", "^[a-z][a-z0-9_]*$"
);
string_type!(
    /// Name of an animation, sound or effect from the manifest.
    LogicalName, 64, is_local_id, "E_ID_FORMAT", "^[a-z][a-z0-9_]*$"
);
string_type!(
    /// Localisation key `loc.…`.
    LocKey, 128, |s| s.strip_prefix("loc.").is_some_and(|r| r.split('.').all(|p| !p.is_empty() && p.chars().all(is_word))),
    "E_ID_FORMAT", "^loc(\\.[a-z0-9_]+)+$"
);

impl Ref {
    /// The kind prefix: `need` for `need.food`.
    pub fn kind(&self) -> &str {
        self.0.split('.').next().unwrap_or("")
    }
}

impl Id {
    pub fn kind(&self) -> &str {
        self.0.split('.').next().unwrap_or("")
    }

    /// The part after the kind: `eat` for `action.eat`.
    pub fn name(&self) -> &str {
        self.0.split_once('.').map(|(_, n)| n).unwrap_or("")
    }
}

/// Time of day `HH:MM`, stored as minutes from midnight.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TimeOfDay(pub u16);

impl TimeOfDay {
    pub const DAY: u16 = 24 * 60;
}

impl<'de> Deserialize<'de> for TimeOfDay {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let s = String::deserialize(d)?;
        let parse = || -> Option<u16> {
            let (h, m) = s.split_once(':')?;
            if h.len() != 2 || m.len() != 2 || !(h.bytes().chain(m.bytes()).all(|b| b.is_ascii_digit())) {
                return None;
            }
            let (h, m): (u16, u16) = (h.parse().ok()?, m.parse().ok()?);
            (h < 24 && m < 60).then_some(h * 60 + m)
        };
        parse().map(TimeOfDay).ok_or_else(|| coded("E_RANGE", format_args!("bad time of day `{s}`")))
    }
}

impl Serialize for TimeOfDay {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&format!("{:02}:{:02}", self.0 / 60, self.0 % 60))
    }
}

impl JsonSchema for TimeOfDay {
    fn schema_name() -> Cow<'static, str> {
        "TimeOfDay".into()
    }
    fn json_schema(_: &mut SchemaGenerator) -> Schema {
        json_schema!({ "type": "string", "pattern": "^([01][0-9]|2[0-3]):[0-5][0-9]$" })
    }
}

/// Parses a JSON number into exact ten-thousandths. Integers are exact; a number
/// with a fraction must be within 1/1000 of a ten-thousandth, which rejects a
/// fifth decimal and absorbs the last-bit error of the decimal-to-binary parse.
struct Dec4Visitor;

impl Visitor<'_> for Dec4Visitor {
    type Value = Dec4;

    fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("a number")
    }

    fn visit_i64<E: de::Error>(self, v: i64) -> Result<Dec4, E> {
        v.checked_mul(Dec4::SCALE).map(Dec4).ok_or_else(|| coded("E_RANGE", format_args!("{v} is too large")))
    }

    fn visit_u64<E: de::Error>(self, v: u64) -> Result<Dec4, E> {
        i64::try_from(v)
            .ok()
            .and_then(|v| v.checked_mul(Dec4::SCALE))
            .map(Dec4)
            .ok_or_else(|| coded("E_RANGE", format_args!("{v} is too large")))
    }

    fn visit_f64<E: de::Error>(self, v: f64) -> Result<Dec4, E> {
        let scaled = v * Dec4::SCALE as f64;
        if !scaled.is_finite() || scaled.abs() > 9.0e15 {
            return Err(coded("E_RANGE", format_args!("{v} is too large")));
        }
        let whole = scaled.round();
        if (scaled - whole).abs() > 1.0e-3 {
            return Err(coded("E_RANGE", format_args!("{v} has more than 4 decimals")));
        }
        Ok(Dec4(whole as i64))
    }
}

/// A number with at most 4 decimals in `[MIN, MAX]`, both given in ten-thousandths.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Num<const MIN: i64, const MAX: i64>(pub Dec4);

impl<'de, const MIN: i64, const MAX: i64> Deserialize<'de> for Num<MIN, MAX> {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let v = d.deserialize_any(Dec4Visitor)?;
        if v.0 < MIN || v.0 > MAX {
            return Err(coded(
                "E_RANGE",
                format_args!(
                    "{} is outside {}..{}",
                    v.0 as f64 / 1e4,
                    MIN as f64 / Dec4::SCALE as f64,
                    MAX as f64 / Dec4::SCALE as f64
                ),
            ));
        }
        Ok(Self(v))
    }
}

impl<const MIN: i64, const MAX: i64> JsonSchema for Num<MIN, MAX> {
    fn schema_name() -> Cow<'static, str> {
        format!("Num_{MIN}_{MAX}").replace('-', "m").into()
    }
    fn json_schema(_: &mut SchemaGenerator) -> Schema {
        json_schema!({
            "type": "number",
            "minimum": MIN as f64 / Dec4::SCALE as f64,
            "maximum": MAX as f64 / Dec4::SCALE as f64,
            "multipleOf": 0.0001
        })
    }
}

/// An integer in `[MIN, MAX]`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Int<const MIN: i64, const MAX: i64>(pub i64);

impl<'de, const MIN: i64, const MAX: i64> Deserialize<'de> for Int<MIN, MAX> {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let v = d.deserialize_any(Dec4Visitor)?;
        if v.0 % Dec4::SCALE != 0 {
            return Err(coded("E_SCHEMA", "expected an integer"));
        }
        let v = v.0 / Dec4::SCALE;
        if v < MIN || v > MAX {
            return Err(coded("E_RANGE", format_args!("{v} is outside {MIN}..{MAX}")));
        }
        Ok(Self(v))
    }
}

impl<const MIN: i64, const MAX: i64> JsonSchema for Int<MIN, MAX> {
    fn schema_name() -> Cow<'static, str> {
        format!("Int_{MIN}_{MAX}").replace('-', "m").into()
    }
    fn json_schema(_: &mut SchemaGenerator) -> Schema {
        json_schema!({ "type": "integer", "minimum": MIN, "maximum": MAX })
    }
}

const I32_MAX: i64 = i32::MAX as i64;
const I32_MIN: i64 = i32::MIN as i64;

/// `Real`: from −32768 to 32767.
pub type Real = Num<-327_680_000, 327_670_000>;
/// `Real` ≥ 0.
pub type RealNonNeg = Num<0, 327_670_000>;
/// `Real` > 0.
pub type RealPos = Num<1, 327_670_000>;
/// `Unit`: from 0 to 1.
pub type Unit = Num<0, 10_000>;
/// `NeedValue`: from 0 to 100.
pub type NeedValue = Num<0, 1_000_000>;
/// Category weight: from 0 to 4.
pub type Weight = Num<0, 40_000>;
/// `Tiles`: distance in tiles, ≥ 0.
pub type Tiles = RealNonNeg;
/// `Int`.
pub type IntAny = Int<I32_MIN, I32_MAX>;
/// `Minutes`: game minutes, ≥ 0.
pub type Minutes = Int<0, I32_MAX>;
/// Positive integer.
pub type IntPos = Int<1, I32_MAX>;

#[cfg(test)]
mod tests {
    use super::*;

    fn code_of(json: &str) -> Option<String> {
        serde_json::from_str::<Unit>(json).err().map(|e| e.to_string().split('|').next().unwrap().to_owned())
    }

    #[test]
    fn ids() {
        assert!(is_id("action.eat"));
        assert!(is_id("need.food.extra_1"));
        assert!(!is_id("action"));
        assert!(!is_id("Action.eat"));
        assert!(!is_id("action.1eat"));
        assert!(!is_id("action.eat."));
        assert!(is_local_id("n_hunger_curve"));
        assert!(!is_local_id("_x"));
    }

    #[test]
    fn numbers() {
        assert_eq!(serde_json::from_str::<Unit>("0.1").unwrap().0, Dec4(1000));
        assert_eq!(serde_json::from_str::<Unit>("1").unwrap().0, Dec4(10_000));
        assert_eq!(serde_json::from_str::<Unit>("0.0001").unwrap().0, Dec4(1));
        assert_eq!(code_of("0.00001").as_deref(), Some("E_RANGE"));
        assert_eq!(code_of("1.5").as_deref(), Some("E_RANGE"));
        assert_eq!(code_of("-0.1").as_deref(), Some("E_RANGE"));
        assert_eq!(serde_json::from_str::<Real>("-32768").unwrap().0, Dec4(-327_680_000));
        assert!(serde_json::from_str::<Real>("32767.5").is_err());
        assert_eq!(serde_json::from_str::<Minutes>("43200").unwrap().0, 43200);
        assert!(serde_json::from_str::<Minutes>("1.5").is_err());
    }

    #[test]
    fn time_of_day() {
        assert_eq!(serde_json::from_str::<TimeOfDay>("\"18:30\"").unwrap().0, 18 * 60 + 30);
        assert!(serde_json::from_str::<TimeOfDay>("\"24:00\"").is_err());
        assert!(serde_json::from_str::<TimeOfDay>("\"7:00\"").is_err());
    }
}
