use crate::controls::{self, SensorControlConfig};
use crate::fourcc::FourCc;
use crate::route::{self, LmiRouteConfig};
use crate::v4l2;
use std::io;
use std::path::PathBuf;
use std::time::Duration;

const DEFAULT_RAW: &str = "/dev/video3";
const DEFAULT_FRAMES: u32 = 30;
const DEFAULT_BUFFERS: u32 = 4;
const DEFAULT_TIMEOUT_MS: u64 = 2_000;

#[derive(Debug, Clone)]
pub struct RawCaptureConfig {
    pub raw_node: PathBuf,
    pub width: u32,
    pub height: u32,
    pub pixelformat: FourCc,
    pub frames: u32,
    pub buffers: u32,
    pub sink: RawCaptureSink,
    pub setup_route: bool,
    pub route: LmiRouteConfig,
    pub controls: SensorControlConfig,
    pub ctrl_node: Option<PathBuf>,
    pub timeout: Duration,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum RawCaptureSink {
    Null,
}

impl Default for RawCaptureConfig {
    fn default() -> Self {
        let route = LmiRouteConfig::default();
        Self {
            raw_node: PathBuf::from(DEFAULT_RAW),
            width: route.width,
            height: route.height,
            pixelformat: FourCc::PGAA,
            frames: DEFAULT_FRAMES,
            buffers: DEFAULT_BUFFERS,
            sink: RawCaptureSink::Null,
            setup_route: true,
            route,
            controls: SensorControlConfig::default(),
            ctrl_node: None,
            timeout: Duration::from_millis(DEFAULT_TIMEOUT_MS),
        }
    }
}

pub fn parse_raw_capture_config<I>(args: I) -> io::Result<RawCaptureConfig>
where
    I: Iterator<Item = String>,
{
    let mut config = RawCaptureConfig::default();
    let mut args = args.peekable();

    if matches!(args.peek().map(String::as_str), Some("raw")) {
        args.next();
    }

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--raw" | "--video" => {
                config.raw_node = PathBuf::from(next_value(&mut args, &arg)?);
                config.route.raw_node = Some(config.raw_node.clone());
            }
            "--width" => {
                config.width = parse_nonzero_u32(&next_value(&mut args, "--width")?, "--width")?;
                config.route.width = config.width;
            }
            "--height" => {
                config.height = parse_nonzero_u32(&next_value(&mut args, "--height")?, "--height")?;
                config.route.height = config.height;
            }
            "--size" => {
                let size = next_value(&mut args, "--size")?;
                let (width, height) = parse_size(&size)?;
                config.width = width;
                config.height = height;
                config.route.width = width;
                config.route.height = height;
            }
            "--frames" => {
                config.frames = parse_nonzero_u32(&next_value(&mut args, "--frames")?, "--frames")?
            }
            "--buffers" => {
                config.buffers =
                    parse_nonzero_u32(&next_value(&mut args, "--buffers")?, "--buffers")?
            }
            "--pixelformat" | "--fourcc" => {
                config.pixelformat = parse_fourcc(&next_value(&mut args, &arg)?)?;
                config.route.pixelformat = config.pixelformat;
            }
            "--sink" => {
                config.sink = parse_sink(&next_value(&mut args, "--sink")?)?;
            }
            "--timeout-ms" => {
                let millis =
                    parse_nonzero_u64(&next_value(&mut args, "--timeout-ms")?, "--timeout-ms")?;
                config.timeout = Duration::from_millis(millis);
            }
            "--no-setup-route" => config.setup_route = false,
            "--setup-route" => config.setup_route = true,
            "--ctrl" | "--control-subdev" => {
                config.ctrl_node = Some(PathBuf::from(next_value(&mut args, &arg)?));
            }
            "--vblank"
            | "--exposure"
            | "--analogue-gain"
            | "--digital-gain"
            | "--reset-controls"
            | "--preserve-controls" => {
                controls::parse_sensor_control_option(&mut config.controls, &arg, &mut args)?;
            }
            "--media"
            | "--sensor"
            | "--csiphy"
            | "--csid"
            | "--vfe"
            | "--video-entity"
            | "--mbus-code"
            | "--csiphy-source-pad"
            | "--csid-source-pad"
            | "--sensor-subdev"
            | "--csiphy-subdev"
            | "--csid-subdev"
            | "--vfe-subdev"
            | "--route" => apply_route_option(&mut config, arg, &mut args)?,
            "--keep-links" => config.route.keep_links = true,
            other => {
                return Err(invalid_input(format!(
                    "unknown capture-raw option: {other}"
                )));
            }
        }
    }

    validate_lmi_raw_config(config.route.mbus_code, config.pixelformat)?;
    config.route.raw_node = Some(config.raw_node.clone());
    config.route.width = config.width;
    config.route.height = config.height;
    config.route.pixelformat = config.pixelformat;

    Ok(config)
}

pub fn print_capture_raw_usage() {
    println!("capture-raw options:");
    println!("  --frames N --sink null --raw DEV --size WxH --buffers N");
    println!("  --pixelformat pgAA --timeout-ms N --no-setup-route --ctrl DEV");
    println!("  plus setup-route media options for the OV13B10 route");
    controls::print_sensor_control_usage();
}

pub fn capture_raw(config: &RawCaptureConfig) -> io::Result<()> {
    validate_lmi_raw_config(config.route.mbus_code, config.pixelformat)?;
    println!("capture-raw raw={} sink=null", config.raw_node.display());
    println!(
        "raw_invariant {} remains truthful {}",
        config.raw_node.display(),
        config.pixelformat
    );

    let mut ctrl_node = config.ctrl_node.clone();
    if config.setup_route {
        let report = route::setup_lmi_ov13b10_route(&config.route)?;
        ctrl_node = Some(report.control_subdev);
    } else {
        println!("setup_route=skipped");
    }

    if let Some(ctrl) = ctrl_node.as_deref() {
        controls::apply_initial_sensor_controls(ctrl, &config.controls, false)?;
    } else if config.controls.should_apply(false) {
        return Err(invalid_input(
            "sensor controls requested but no control subdev is known; pass --ctrl or enable route setup",
        ));
    }

    let report = v4l2::capture_mplane_mmap(
        &config.raw_node,
        config.width,
        config.height,
        config.pixelformat,
        config.buffers,
        config.frames,
        config.timeout,
    )?;

    println!(
        "capture format {} {}x{} planes={} bytesperline={} sizeimage={}",
        report.format.fourcc,
        report.format.width,
        report.format.height,
        report.format.num_planes,
        report.format.bytesperline,
        report.format.sizeimage
    );
    println!("capture buffers={}", report.buffers);

    let mut total_bytes = 0u64;
    let mut last_sequence = None;
    for frame in &report.frames {
        total_bytes += u64::from(frame.bytesused);
        last_sequence = Some(frame.sequence);
        println!(
            "frame index={} sequence={} bytesused={}",
            frame.index, frame.sequence, frame.bytesused
        );
    }

    println!(
        "capture complete frames={} total_bytes={} last_sequence={}",
        report.frames.len(),
        total_bytes,
        last_sequence
            .map(|seq| seq.to_string())
            .unwrap_or_else(|| "none".to_string())
    );
    Ok(())
}

fn apply_route_option<I>(
    config: &mut RawCaptureConfig,
    arg: String,
    args: &mut std::iter::Peekable<I>,
) -> io::Result<()>
where
    I: Iterator<Item = String>,
{
    match arg.as_str() {
        "--media" => config.route.media = PathBuf::from(next_value(args, "--media")?),
        "--sensor" => config.route.sensor = next_value(args, "--sensor")?,
        "--csiphy" => config.route.csiphy = next_value(args, "--csiphy")?,
        "--csid" => config.route.csid = next_value(args, "--csid")?,
        "--vfe" => config.route.vfe = next_value(args, "--vfe")?,
        "--video-entity" => config.route.video_entity = next_value(args, "--video-entity")?,
        "--mbus-code" => {
            config.route.mbus_code =
                parse_u32_auto(&next_value(args, "--mbus-code")?, "--mbus-code")?
        }
        "--csiphy-source-pad" => {
            config.route.csiphy_source_pad = parse_u16(
                &next_value(args, "--csiphy-source-pad")?,
                "--csiphy-source-pad",
            )?
        }
        "--csid-source-pad" => {
            config.route.csid_source_pad =
                parse_u16(&next_value(args, "--csid-source-pad")?, "--csid-source-pad")?
        }
        "--sensor-subdev" => {
            config.route.sensor_subdev = Some(PathBuf::from(next_value(args, "--sensor-subdev")?))
        }
        "--csiphy-subdev" => {
            config.route.csiphy_subdev = Some(PathBuf::from(next_value(args, "--csiphy-subdev")?))
        }
        "--csid-subdev" => {
            config.route.csid_subdev = Some(PathBuf::from(next_value(args, "--csid-subdev")?))
        }
        "--vfe-subdev" => {
            config.route.vfe_subdev = Some(PathBuf::from(next_value(args, "--vfe-subdev")?))
        }
        "--route" => {
            let route = next_value(args, "--route")?;
            if route != "lmi-ov13b10" {
                return Err(invalid_input(format!(
                    "unsupported --route '{route}', only lmi-ov13b10 is implemented"
                )));
            }
        }
        _ => unreachable!(),
    }
    Ok(())
}

fn parse_sink(value: &str) -> io::Result<RawCaptureSink> {
    match value {
        "null" => Ok(RawCaptureSink::Null),
        other => Err(invalid_input(format!(
            "unsupported --sink '{other}', only null is implemented"
        ))),
    }
}

fn validate_lmi_raw_config(mbus_code: u32, fourcc: FourCc) -> io::Result<()> {
    if mbus_code != route::MEDIA_BUS_FMT_SGRBG10_1X10 {
        return Err(invalid_input(format!(
            "unsupported lmi RAW mbus code 0x{mbus_code:04x}; OV13B10 route must remain SGRBG10_1X10"
        )));
    }
    if fourcc != FourCc::PGAA {
        return Err(invalid_input(format!(
            "unsupported lmi RAW fourcc {fourcc}; /dev/video3 must remain truthful pgAA"
        )));
    }
    Ok(())
}

fn next_value<I>(args: &mut std::iter::Peekable<I>, flag: &str) -> io::Result<String>
where
    I: Iterator<Item = String>,
{
    args.next()
        .ok_or_else(|| invalid_input(format!("missing value for {flag}")))
}

fn parse_size(value: &str) -> io::Result<(u32, u32)> {
    let (width, height) = value
        .split_once('x')
        .or_else(|| value.split_once('X'))
        .ok_or_else(|| invalid_input(format!("invalid --size '{value}', expected WxH")))?;
    Ok((
        parse_nonzero_u32(width, "--size width")?,
        parse_nonzero_u32(height, "--size height")?,
    ))
}

fn parse_fourcc(value: &str) -> io::Result<FourCc> {
    let bytes = value.as_bytes();
    if bytes.len() != 4 {
        return Err(invalid_input(format!(
            "invalid fourcc '{value}', expected exactly four bytes"
        )));
    }
    let mut raw = [0u8; 4];
    raw.copy_from_slice(bytes);
    Ok(FourCc::from_bytes(raw))
}

fn parse_u16(value: &str, field: &str) -> io::Result<u16> {
    value
        .parse::<u16>()
        .map_err(|_| invalid_input(format!("invalid {field} value '{value}'")))
}

fn parse_u32(value: &str, field: &str) -> io::Result<u32> {
    value
        .parse::<u32>()
        .map_err(|_| invalid_input(format!("invalid {field} value '{value}'")))
}

fn parse_nonzero_u32(value: &str, field: &str) -> io::Result<u32> {
    let parsed = parse_u32(value, field)?;
    if parsed == 0 {
        Err(invalid_input(format!("{field} must be greater than zero")))
    } else {
        Ok(parsed)
    }
}

fn parse_nonzero_u64(value: &str, field: &str) -> io::Result<u64> {
    let parsed = value
        .parse::<u64>()
        .map_err(|_| invalid_input(format!("invalid {field} value '{value}'")))?;
    if parsed == 0 {
        Err(invalid_input(format!("{field} must be greater than zero")))
    } else {
        Ok(parsed)
    }
}

fn parse_u32_auto(value: &str, field: &str) -> io::Result<u32> {
    let trimmed = value.trim();
    if let Some(hex) = trimmed
        .strip_prefix("0x")
        .or_else(|| trimmed.strip_prefix("0X"))
    {
        u32::from_str_radix(hex, 16)
            .map_err(|_| invalid_input(format!("invalid {field} value '{value}'")))
    } else {
        parse_u32(trimmed, field)
    }
}

fn invalid_input(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_default_raw_capture_config() {
        let config = parse_raw_capture_config(std::iter::empty()).unwrap();
        assert_eq!(config.raw_node, PathBuf::from(DEFAULT_RAW));
        assert_eq!(config.width, 1364);
        assert_eq!(config.height, 768);
        assert_eq!(config.pixelformat, FourCc::PGAA);
        assert_eq!(config.frames, DEFAULT_FRAMES);
        assert_eq!(config.buffers, DEFAULT_BUFFERS);
        assert_eq!(config.sink, RawCaptureSink::Null);
        assert!(config.setup_route);
        assert_eq!(config.controls, SensorControlConfig::default());
        assert_eq!(config.ctrl_node, None);
    }

    #[test]
    fn parses_raw_capture_overrides() {
        let config = parse_raw_capture_config(
            [
                "--raw",
                "/dev/video9",
                "--size",
                "1280x720",
                "--frames",
                "7",
                "--buffers",
                "3",
                "--sink",
                "null",
                "--pixelformat",
                "pgAA",
                "--timeout-ms",
                "500",
                "--no-setup-route",
                "--ctrl",
                "/dev/v4l-subdev5",
                "--exposure",
                "96",
                "--preserve-controls",
            ]
            .into_iter()
            .map(String::from),
        )
        .unwrap();
        assert_eq!(config.raw_node, PathBuf::from("/dev/video9"));
        assert_eq!(config.route.raw_node, Some(PathBuf::from("/dev/video9")));
        assert_eq!(config.width, 1280);
        assert_eq!(config.height, 720);
        assert_eq!(config.frames, 7);
        assert_eq!(config.buffers, 3);
        assert_eq!(config.timeout, Duration::from_millis(500));
        assert!(!config.setup_route);
        assert_eq!(config.ctrl_node, Some(PathBuf::from("/dev/v4l-subdev5")));
        assert_eq!(config.controls.exposure, Some(96));
        assert!(config.controls.preserve_controls);
    }

    #[test]
    fn rejects_non_pg_raw_fourcc() {
        let err = parse_raw_capture_config(["--pixelformat", "YUYV"].into_iter().map(String::from))
            .unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
        assert!(err.to_string().contains("must remain truthful pgAA"));
    }

    #[test]
    fn rejects_non_ov13b10_raw_mbus_code() {
        let err = parse_raw_capture_config(["--mbus-code", "0x3017"].into_iter().map(String::from))
            .unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
        assert!(err.to_string().contains("must remain SGRBG10_1X10"));
    }
}
