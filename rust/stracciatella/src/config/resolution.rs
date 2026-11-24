use std::default::Default;
use std::fmt;
use std::fmt::Display;
use std::str::FromStr;

use serde::{Deserialize, Deserializer, Serialize, Serializer};

/// Struct that contains a specific resolution for the game
///
/// `Resolution::AUTO` (written as `auto`) leaves the choice to the game, which
/// picks a size that fills the display at a whole-number scale.
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub struct Resolution(pub u16, pub u16);

impl Resolution {
    /// Let the game pick the resolution from the display it starts on
    pub const AUTO: Resolution = Resolution(0, 0);

    /// Whether the game should pick the resolution itself
    pub fn is_auto(&self) -> bool {
        *self == Resolution::AUTO
    }
}

impl FromStr for Resolution {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        if s.trim().eq_ignore_ascii_case("auto") {
            return Ok(Resolution::AUTO);
        }
        let mut resolutions = s.split('x').filter_map(|r_str| r_str.parse::<u16>().ok());

        match (resolutions.next(), resolutions.next()) {
            (Some(x), Some(y)) => Ok(Resolution(x, y)),
            _ => Err(String::from(
                "Incorrect resolution format, should be WIDTHxHEIGHT or auto.",
            )),
        }
    }
}

impl Display for Resolution {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        if self.is_auto() {
            return write!(f, "auto");
        }
        write!(f, "{}x{}", self.0, self.1)
    }
}

impl Serialize for Resolution {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_str(&format!("{}", self))
    }
}

impl<'de> Deserialize<'de> for Resolution {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let deserialized_string = String::deserialize(deserializer)?;
        Resolution::from_str(&deserialized_string).map_err(serde::de::Error::custom)
    }
}

impl Default for Resolution {
    fn default() -> Self {
        Resolution(640, 480)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_auto() {
        assert_eq!(Resolution::from_str("auto"), Ok(Resolution::AUTO));
        assert_eq!(Resolution::from_str("AUTO"), Ok(Resolution::AUTO));
        assert_eq!(format!("{}", Resolution::AUTO), "auto");
        assert_eq!(Resolution::from_str("1280x720"), Ok(Resolution(1280, 720)));
    }
}
