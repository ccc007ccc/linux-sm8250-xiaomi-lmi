use std::collections::BTreeMap;
use std::fs::OpenOptions;
use std::io;
use std::mem;
use std::os::fd::AsRawFd;
use std::os::raw::{c_int, c_ulong};
use std::path::Path;

const VIDIOC_QUERYCTRL: c_ulong = 0xc0445624;
const VIDIOC_G_CTRL: c_ulong = 0xc008561b;
const VIDIOC_S_CTRL: c_ulong = 0xc008561c;

pub const V4L2_CID_EXPOSURE: u32 = 0x0098_0911;
pub const V4L2_CID_VBLANK: u32 = 0x009e_0901;
pub const V4L2_CID_ANALOGUE_GAIN: u32 = 0x009e_0903;
pub const V4L2_CID_DIGITAL_GAIN: u32 = 0x009f_0905;

const V4L2_CTRL_FLAG_DISABLED: u32 = 0x0001;
const V4L2_CTRL_FLAG_READ_ONLY: u32 = 0x0004;

const EINVAL: i32 = 22;
const ENOTTY: i32 = 25;

unsafe extern "C" {
    fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2QueryCtrlRaw {
    id: u32,
    type_: u32,
    name: [u8; 32],
    minimum: i32,
    maximum: i32,
    step: i32,
    default_value: i32,
    flags: u32,
    reserved: [u32; 2],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2ControlRaw {
    id: u32,
    value: i32,
}

#[derive(Debug, Clone, Default, Eq, PartialEq)]
pub struct SensorControlConfig {
    pub vblank: Option<i32>,
    pub exposure: Option<i32>,
    pub analogue_gain: Option<i32>,
    pub digital_gain: Option<i32>,
    pub reset_controls: bool,
    pub preserve_controls: bool,
}

impl SensorControlConfig {
    pub fn has_value_overrides(&self) -> bool {
        self.vblank.is_some()
            || self.exposure.is_some()
            || self.analogue_gain.is_some()
            || self.digital_gain.is_some()
    }

    pub fn has_user_settings(&self) -> bool {
        self.reset_controls || self.has_value_overrides()
    }

    pub fn should_apply(&self, auto_reset: bool) -> bool {
        self.has_user_settings() || (auto_reset && !self.preserve_controls)
    }
}

#[derive(Debug, Clone, Copy)]
struct KnownControl {
    id: u32,
    key: &'static str,
    label: &'static str,
}

const KNOWN_CONTROLS: [KnownControl; 4] = [
    KnownControl {
        id: V4L2_CID_VBLANK,
        key: "vblank",
        label: "VBLANK",
    },
    KnownControl {
        id: V4L2_CID_EXPOSURE,
        key: "exposure",
        label: "EXPOSURE",
    },
    KnownControl {
        id: V4L2_CID_ANALOGUE_GAIN,
        key: "analogue_gain",
        label: "ANALOGUE_GAIN",
    },
    KnownControl {
        id: V4L2_CID_DIGITAL_GAIN,
        key: "digital_gain",
        label: "DIGITAL_GAIN",
    },
];

#[derive(Debug, Clone)]
pub struct ControlInfo {
    pub id: u32,
    pub key: &'static str,
    pub label: &'static str,
    pub driver_name: String,
    pub minimum: i32,
    pub maximum: i32,
    pub step: i32,
    pub default_value: i32,
    pub current: i32,
    pub flags: u32,
}

pub struct SensorControls {
    file: std::fs::File,
    infos: BTreeMap<u32, ControlInfo>,
}

impl SensorControls {
    pub fn open(path: &Path) -> io::Result<Self> {
        let file = OpenOptions::new().read(true).write(true).open(path)?;
        let fd = file.as_raw_fd();
        let mut infos = BTreeMap::new();
        for known in KNOWN_CONTROLS {
            if let Some(info) = maybe_query_control(fd, known)? {
                infos.insert(known.id, info);
            }
        }
        Ok(Self { file, infos })
    }

    pub fn reset_defaults(&mut self) -> io::Result<()> {
        for known in KNOWN_CONTROLS {
            let Some(default_value) = self.infos.get(&known.id).map(|info| info.default_value)
            else {
                continue;
            };
            self.set_clamped(known.id, default_value)?;
        }
        Ok(())
    }

    pub fn apply_initial(&mut self, config: &SensorControlConfig) -> io::Result<()> {
        for (value, id) in [
            (config.vblank, V4L2_CID_VBLANK),
            (config.exposure, V4L2_CID_EXPOSURE),
            (config.analogue_gain, V4L2_CID_ANALOGUE_GAIN),
            (config.digital_gain, V4L2_CID_DIGITAL_GAIN),
        ] {
            if let Some(value) = value {
                self.set_clamped(id, value)?;
            }
        }
        Ok(())
    }

    pub fn print_status(&mut self) -> io::Result<()> {
        if self.infos.is_empty() {
            println!("sensor_controls supported=none");
            return Ok(());
        }
        for known in KNOWN_CONTROLS {
            if self.infos.contains_key(&known.id) {
                self.refresh(known.id)?;
                let info = self
                    .infos
                    .get(&known.id)
                    .expect("refreshed control missing");
                println!(
                    "control {} id=0x{:08x} name='{}' current={} range={}..{} step={} default={}",
                    info.key,
                    info.id,
                    info.driver_name,
                    info.current,
                    info.minimum,
                    info.maximum,
                    info.step,
                    info.default_value
                );
            }
        }
        Ok(())
    }

    pub fn set_clamped(&mut self, id: u32, requested: i32) -> io::Result<()> {
        let info = self.infos.get(&id).cloned().ok_or_else(|| {
            invalid_input(format!(
                "sensor control {} is not supported",
                control_label(id)
            ))
        })?;
        if info.flags & V4L2_CTRL_FLAG_DISABLED != 0 {
            return Err(invalid_input(format!(
                "sensor control {} is disabled",
                info.label
            )));
        }
        if info.flags & V4L2_CTRL_FLAG_READ_ONLY != 0 {
            return Err(invalid_input(format!(
                "sensor control {} is read-only",
                info.label
            )));
        }
        let value = requested.clamp(info.minimum, info.maximum);
        set_control_raw(self.file.as_raw_fd(), id, value)?;
        self.refresh(id)?;
        if id == V4L2_CID_VBLANK {
            self.refresh(V4L2_CID_EXPOSURE)?;
        }
        let applied = self
            .infos
            .get(&id)
            .map(|info| info.current)
            .unwrap_or(value);
        println!(
            "control {} requested={} applied={} range={}..{} default={}",
            info.key, requested, applied, info.minimum, info.maximum, info.default_value
        );
        Ok(())
    }

    fn refresh(&mut self, id: u32) -> io::Result<()> {
        let Some(known) = known_control(id) else {
            return Ok(());
        };
        if let Some(info) = maybe_query_control(self.file.as_raw_fd(), known)? {
            self.infos.insert(id, info);
        }
        Ok(())
    }
}

pub fn apply_initial_sensor_controls(
    path: &Path,
    config: &SensorControlConfig,
    auto_reset: bool,
) -> io::Result<()> {
    if !config.should_apply(auto_reset) {
        return Ok(());
    }

    println!("sensor_controls ctrl={}", path.display());
    let mut controls = SensorControls::open(path)?;
    if config.reset_controls || (auto_reset && !config.preserve_controls) {
        controls.reset_defaults()?;
    }
    controls.apply_initial(config)?;
    controls.print_status()
}

pub fn parse_sensor_control_option<I>(
    config: &mut SensorControlConfig,
    arg: &str,
    args: &mut std::iter::Peekable<I>,
) -> io::Result<bool>
where
    I: Iterator<Item = String>,
{
    match arg {
        "--vblank" => config.vblank = Some(parse_i32(&next_value(args, "--vblank")?, "--vblank")?),
        "--exposure" => {
            config.exposure = Some(parse_i32(&next_value(args, "--exposure")?, "--exposure")?)
        }
        "--analogue-gain" => {
            config.analogue_gain = Some(parse_i32(
                &next_value(args, "--analogue-gain")?,
                "--analogue-gain",
            )?)
        }
        "--digital-gain" => {
            config.digital_gain = Some(parse_i32(
                &next_value(args, "--digital-gain")?,
                "--digital-gain",
            )?)
        }
        "--reset-controls" => config.reset_controls = true,
        "--preserve-controls" => config.preserve_controls = true,
        _ => return Ok(false),
    }
    Ok(true)
}

pub fn print_sensor_control_usage() {
    println!("Sensor control options:");
    println!("  --vblank N --exposure N --analogue-gain N --digital-gain N");
    println!("  --reset-controls --preserve-controls");
}

fn maybe_query_control(fd: c_int, known: KnownControl) -> io::Result<Option<ControlInfo>> {
    match query_control_raw(fd, known) {
        Ok(info) => Ok(Some(info)),
        Err(err) if matches!(err.raw_os_error(), Some(EINVAL | ENOTTY)) => Ok(None),
        Err(err) => Err(err),
    }
}

fn query_control_raw(fd: c_int, known: KnownControl) -> io::Result<ControlInfo> {
    let mut query: V4l2QueryCtrlRaw = unsafe { mem::zeroed() };
    query.id = known.id;
    let ret = unsafe { ioctl(fd, VIDIOC_QUERYCTRL, &mut query as *mut V4l2QueryCtrlRaw) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }

    let current = get_control_raw(fd, known.id).unwrap_or(query.default_value);
    Ok(ControlInfo {
        id: known.id,
        key: known.key,
        label: known.label,
        driver_name: c_array_to_string(&query.name),
        minimum: query.minimum,
        maximum: query.maximum,
        step: query.step,
        default_value: query.default_value,
        current,
        flags: query.flags,
    })
}

fn get_control_raw(fd: c_int, id: u32) -> io::Result<i32> {
    let mut control = V4l2ControlRaw { id, value: 0 };
    let ret = unsafe { ioctl(fd, VIDIOC_G_CTRL, &mut control as *mut V4l2ControlRaw) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(control.value)
}

fn set_control_raw(fd: c_int, id: u32, value: i32) -> io::Result<()> {
    let mut control = V4l2ControlRaw { id, value };
    let ret = unsafe { ioctl(fd, VIDIOC_S_CTRL, &mut control as *mut V4l2ControlRaw) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

/// V4L2_CID_CAMERA_SENSOR_ROTATION: kernel-declared sensor mounting rotation
/// (from DTS `rotation`).  Orientation is owned by the kernel; the loopback ISP
/// only consumes this value.
pub const V4L2_CID_CAMERA_SENSOR_ROTATION: u32 = 0x009a_0923;

/// Read the sensor rotation metadata from a sensor subdev.  Returns Ok(None) if
/// the control is not implemented (so callers fall back to no rotation).
pub fn read_sensor_rotation(path: &Path) -> io::Result<Option<i32>> {
    let file = OpenOptions::new().read(true).write(true).open(path)?;
    match get_control_raw(file.as_raw_fd(), V4L2_CID_CAMERA_SENSOR_ROTATION) {
        Ok(value) => Ok(Some(value)),
        Err(err) => match err.raw_os_error() {
            Some(EINVAL) | Some(ENOTTY) => Ok(None),
            _ => Err(err),
        },
    }
}

fn known_control(id: u32) -> Option<KnownControl> {
    KNOWN_CONTROLS.iter().copied().find(|known| known.id == id)
}

fn control_label(id: u32) -> &'static str {
    known_control(id)
        .map(|known| known.label)
        .unwrap_or("UNKNOWN")
}

fn next_value<I>(args: &mut std::iter::Peekable<I>, flag: &str) -> io::Result<String>
where
    I: Iterator<Item = String>,
{
    args.next()
        .ok_or_else(|| invalid_input(format!("missing value for {flag}")))
}

fn parse_i32(value: &str, field: &str) -> io::Result<i32> {
    value
        .parse::<i32>()
        .map_err(|_| invalid_input(format!("invalid {field} value '{value}'")))
}

fn c_array_to_string(bytes: &[u8]) -> String {
    let nul = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..nul]).trim().to_string()
}

fn invalid_input(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_sensor_control_options() {
        let mut config = SensorControlConfig::default();
        let mut args = ["12", "100", "3", "4"]
            .into_iter()
            .map(String::from)
            .peekable();

        assert!(parse_sensor_control_option(&mut config, "--vblank", &mut args).unwrap());
        assert!(parse_sensor_control_option(&mut config, "--exposure", &mut args).unwrap());
        assert!(parse_sensor_control_option(&mut config, "--analogue-gain", &mut args).unwrap());
        assert!(parse_sensor_control_option(&mut config, "--digital-gain", &mut args).unwrap());
        config.reset_controls = true;

        assert_eq!(config.vblank, Some(12));
        assert_eq!(config.exposure, Some(100));
        assert_eq!(config.analogue_gain, Some(3));
        assert_eq!(config.digital_gain, Some(4));
        assert!(config.has_user_settings());
        assert!(config.should_apply(false));
    }

    #[test]
    fn preserve_controls_suppresses_auto_reset_only() {
        let config = SensorControlConfig {
            preserve_controls: true,
            ..SensorControlConfig::default()
        };
        assert!(!config.should_apply(true));

        let config = SensorControlConfig {
            preserve_controls: true,
            exposure: Some(50),
            ..SensorControlConfig::default()
        };
        assert!(config.should_apply(true));
    }
}
