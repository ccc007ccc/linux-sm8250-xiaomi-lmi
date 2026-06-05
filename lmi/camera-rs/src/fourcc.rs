use std::fmt;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FourCc(pub u32);

impl FourCc {
    pub const PGAA: Self = Self::from_bytes(*b"pgAA");
    pub const YUYV: Self = Self::from_bytes(*b"YUYV");

    pub const fn from_bytes(bytes: [u8; 4]) -> Self {
        Self(u32::from_le_bytes(bytes))
    }

    pub fn as_bytes(self) -> [u8; 4] {
        self.0.to_le_bytes()
    }

    pub fn as_lossy_string(self) -> String {
        self.as_bytes()
            .iter()
            .map(|b| {
                if b.is_ascii_graphic() || *b == b' ' {
                    *b as char
                } else {
                    '.'
                }
            })
            .collect()
    }
}

impl fmt::Display for FourCc {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.as_lossy_string())
    }
}
