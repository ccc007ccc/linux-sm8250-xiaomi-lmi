#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub struct NativeMode {
    pub frame_index: u32,
    pub mode_index: u32,
    pub width: u32,
    pub height: u32,
    pub interval_100ns: u32,
    pub fps_cap: u32,
    pub nominal_fps_milli: u32,
}

impl NativeMode {
    pub fn max_frame_bytes(self) -> u32 {
        native_mjpeg_max_frame_bytes(self.width, self.height)
    }

    pub fn h264_max_record_bytes(self) -> u32 {
        native_h264_max_record_bytes(self.width, self.height)
    }

    pub fn h264_bitrate(self) -> u32 {
        native_h264_bitrate(self.width, self.height, self.fps_cap)
    }

    pub fn h264_peak_bitrate(self) -> u32 {
        native_h264_peak_bitrate(self.width, self.height, self.fps_cap)
    }

    pub fn frame_name(self) -> String {
        format!("f{:02}_{}x{}", self.frame_index, self.width, self.height)
    }

    pub fn mjpeg_feeder_frame_arg(self) -> String {
        format!(
            "{}:{}:{}:{}:{}",
            self.frame_index,
            self.width,
            self.height,
            self.max_frame_bytes(),
            self.interval_100ns
        )
    }

    pub fn h264_feeder_frame_arg(self) -> String {
        format!(
            "{}:{}:{}:{}:{}",
            self.frame_index,
            self.width,
            self.height,
            self.h264_max_record_bytes(),
            self.interval_100ns
        )
    }

    pub fn nominal_fps(self) -> f32 {
        self.nominal_fps_milli as f32 / 1000.0
    }
}

pub const OV13B10_NATIVE_MODES: [NativeMode; 6] = [
    NativeMode {
        frame_index: 1,
        mode_index: 0,
        width: 4208,
        height: 3120,
        interval_100ns: 335_582,
        fps_cap: 30,
        nominal_fps_milli: 29_799,
    },
    NativeMode {
        frame_index: 2,
        mode_index: 1,
        width: 4160,
        height: 3120,
        interval_100ns: 335_582,
        fps_cap: 30,
        nominal_fps_milli: 29_799,
    },
    NativeMode {
        frame_index: 3,
        mode_index: 2,
        width: 4160,
        height: 2340,
        interval_100ns: 335_582,
        fps_cap: 30,
        nominal_fps_milli: 29_799,
    },
    NativeMode {
        frame_index: 4,
        mode_index: 3,
        width: 2104,
        height: 1560,
        interval_100ns: 167_791,
        fps_cap: 60,
        nominal_fps_milli: 59_598,
    },
    NativeMode {
        frame_index: 5,
        mode_index: 4,
        width: 2080,
        height: 1170,
        interval_100ns: 167_791,
        fps_cap: 60,
        nominal_fps_milli: 59_598,
    },
    NativeMode {
        frame_index: 6,
        mode_index: 5,
        width: 1364,
        height: 768,
        interval_100ns: 83_285,
        fps_cap: 120,
        nominal_fps_milli: 120_069,
    },
];

pub fn native_mjpeg_max_frame_bytes(width: u32, height: u32) -> u32 {
    let rawish = (u64::from(width) * u64::from(height) * 3) / 2;
    let aligned = (rawish + 4095) & !4095;
    aligned.max(1_048_576) as u32
}

pub fn native_h264_bitrate(width: u32, height: u32, fps_cap: u32) -> u32 {
    let pixels = u64::from(width) * u64::from(height);
    let fps = u64::from(fps_cap.max(1));
    let bpp_milli = if pixels >= 8_000_000 {
        250
    } else if fps_cap >= 100 {
        350
    } else if fps_cap >= 50 {
        500
    } else {
        600
    };
    let bps = pixels.saturating_mul(fps).saturating_mul(bpp_milli) / 1000;
    bps.clamp(20_000_000, 180_000_000) as u32
}

pub fn native_h264_peak_bitrate(width: u32, height: u32, fps_cap: u32) -> u32 {
    let target = u64::from(native_h264_bitrate(width, height, fps_cap));
    ((target * 3) / 2).min(220_000_000) as u32
}

pub fn native_h264_max_record_bytes(width: u32, height: u32) -> u32 {
    let rawish = (u64::from(width) * u64::from(height) * 3) / 2;
    let fps = OV13B10_NATIVE_MODES
        .iter()
        .find(|mode| mode.width == width && mode.height == height)
        .map(|mode| mode.fps_cap)
        .unwrap_or(30)
        .max(1);
    let peak = u64::from(native_h264_peak_bitrate(width, height, fps));
    let peak_frame = (peak / u64::from(fps)) / 8;
    let burst_record = peak_frame
        .saturating_mul(8)
        .saturating_add(2 * 1024 * 1024);
    let safe = burst_record.max(4 * 1024 * 1024).min(rawish.max(1_048_576));
    ((safe + 4095) & !4095) as u32
}

pub fn by_frame_index(frame_index: u32) -> Option<NativeMode> {
    OV13B10_NATIVE_MODES
        .iter()
        .copied()
        .find(|mode| mode.frame_index == frame_index)
}

#[allow(dead_code)]
pub fn by_mode_index(mode_index: u32) -> Option<NativeMode> {
    OV13B10_NATIVE_MODES
        .iter()
        .copied()
        .find(|mode| mode.mode_index == mode_index)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_modes_are_the_only_public_uvc_frames() {
        let sizes: Vec<(u32, u32)> = OV13B10_NATIVE_MODES
            .iter()
            .map(|mode| (mode.width, mode.height))
            .collect();

        assert_eq!(
            sizes,
            vec![
                (4208, 3120),
                (4160, 3120),
                (4160, 2340),
                (2104, 1560),
                (2080, 1170),
                (1364, 768),
            ]
        );
        assert!(!sizes.contains(&(1280, 720)));
        assert!(!sizes.contains(&(1920, 1080)));
        assert!(!sizes.contains(&(640, 480)));
    }

    #[test]
    fn frame_index_maps_one_to_one_to_sensor_mode() {
        for (idx, mode) in OV13B10_NATIVE_MODES.iter().enumerate() {
            assert_eq!(mode.frame_index, idx as u32 + 1);
            assert_eq!(mode.mode_index, idx as u32);
            assert_eq!(by_frame_index(mode.frame_index), Some(*mode));
            assert_eq!(by_mode_index(mode.mode_index), Some(*mode));
        }
    }

    #[test]
    fn max_frame_bytes_matches_python_native_descriptor_rule() {
        assert_eq!(native_mjpeg_max_frame_bytes(1364, 768), 1_572_864);
        assert_eq!(native_mjpeg_max_frame_bytes(4208, 3120), 19_693_568);
    }

    #[test]
    fn h264_record_budget_is_smaller_than_mjpeg_rawish_budget() {
        for mode in OV13B10_NATIVE_MODES {
            assert!(mode.h264_bitrate() <= mode.h264_peak_bitrate());
            assert!(mode.h264_max_record_bytes() >= 1_048_576);
            assert!(mode.h264_max_record_bytes() <= mode.max_frame_bytes());
        }
        assert!(
            OV13B10_NATIVE_MODES[0].h264_max_record_bytes()
                < OV13B10_NATIVE_MODES[0].max_frame_bytes()
        );
    }
}
