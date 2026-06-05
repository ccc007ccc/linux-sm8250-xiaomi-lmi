use crate::devices;
use crate::native_modes::{NativeMode, OV13B10_NATIVE_MODES};
use std::collections::BTreeSet;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::thread;
use std::time::{Duration, Instant};

const CONFIGFS: &str = "/sys/kernel/config/usb_gadget";
const DEFAULT_GADGET: &str = "lmi_uvc";
const DEFAULT_VENDOR: &str = "0x1d6b";
const DEFAULT_PRODUCT: &str = "0x0102";
const DEFAULT_BCD_USB: &str = "0x0200";
const RUST_MJPEG_BCD_DEVICE: &str = "0x0013";
const RUST_MJPEG_SERIAL: &str = "lmi0004";
const RUST_H264_BCD_DEVICE: &str = "0x0015";
const RUST_H264_SERIAL: &str = "lmi0006";

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum UvcCodec {
    Mjpeg,
    H264,
}

impl UvcCodec {
    pub fn name(self) -> &'static str {
        match self {
            UvcCodec::Mjpeg => "mjpeg",
            UvcCodec::H264 => "h264",
        }
    }

    pub fn feeder_format(self) -> &'static str {
        match self {
            UvcCodec::Mjpeg => "mjpeg",
            UvcCodec::H264 => "h264",
        }
    }

    pub fn frame_arg(self, mode: NativeMode) -> String {
        match self {
            UvcCodec::Mjpeg => mode.mjpeg_feeder_frame_arg(),
            UvcCodec::H264 => mode.h264_feeder_frame_arg(),
        }
    }

    pub fn max_frame_bytes(self, mode: NativeMode) -> u32 {
        match self {
            UvcCodec::Mjpeg => mode.max_frame_bytes(),
            UvcCodec::H264 => mode.h264_max_record_bytes(),
        }
    }

    fn bcd_device(self) -> &'static str {
        match self {
            UvcCodec::Mjpeg => RUST_MJPEG_BCD_DEVICE,
            UvcCodec::H264 => RUST_H264_BCD_DEVICE,
        }
    }

    fn serial(self) -> &'static str {
        match self {
            UvcCodec::Mjpeg => RUST_MJPEG_SERIAL,
            UvcCodec::H264 => RUST_H264_SERIAL,
        }
    }

    fn format_dir(self, func: &Path) -> PathBuf {
        match self {
            UvcCodec::Mjpeg => func.join("streaming/mjpeg/mjpg"),
            UvcCodec::H264 => func.join("streaming/framebased/h264"),
        }
    }

    fn header_name(self) -> &'static str {
        match self {
            UvcCodec::Mjpeg => "mjpg",
            UvcCodec::H264 => "h264",
        }
    }
}

#[derive(Debug, Clone)]
pub struct UvcGadgetConfig {
    pub configfs: PathBuf,
    pub gadget_name: String,
    pub udc: Option<String>,
    pub restore_previous_gadget: bool,
    pub keep: bool,
    pub maxpacket: u32,
    pub default_frame_index: u32,
}

impl Default for UvcGadgetConfig {
    fn default() -> Self {
        Self {
            configfs: PathBuf::from(CONFIGFS),
            gadget_name: DEFAULT_GADGET.to_string(),
            udc: None,
            restore_previous_gadget: true,
            keep: false,
            maxpacket: 1024,
            default_frame_index: 1,
        }
    }
}

#[derive(Debug)]
pub struct UvcGadget {
    config: UvcGadgetConfig,
    gadget: PathBuf,
    func: PathBuf,
    built: bool,
    udc_name: Option<String>,
    prev_gadget: Option<PathBuf>,
    node: Option<PathBuf>,
}

#[derive(Debug, Clone)]
pub struct UvcStatus {
    pub gadget: PathBuf,
    pub codec: UvcCodec,
    pub frames: Vec<UvcFrameStatus>,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct UvcFrameStatus {
    pub name: String,
    pub width: u32,
    pub height: u32,
    pub interval_100ns: u32,
    pub max_frame_bytes: u32,
}

impl UvcGadget {
    pub fn new(config: UvcGadgetConfig) -> Self {
        let gadget = config.configfs.join(&config.gadget_name);
        let func = gadget.join("functions/uvc.0");
        Self {
            config,
            gadget,
            func,
            built: false,
            udc_name: None,
            prev_gadget: None,
            node: None,
        }
    }

    pub fn bind_native(&mut self, codec: UvcCodec) -> io::Result<PathBuf> {
        if !self.config.configfs.is_dir() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("configfs not mounted at {}", self.config.configfs.display()),
            ));
        }

        if self.gadget.is_dir() {
            self.teardown()?;
        }

        let before = video_node_set();
        self.build_native_descriptor(codec)?;
        let udc = self.detect_udc()?;
        self.udc_name = Some(udc.clone());
        self.prev_gadget = self.gadget_on_udc(&udc)?;
        if let Some(prev) = &self.prev_gadget {
            self.unbind_path(prev);
            if !self.wait_udc_free(&udc, Duration::from_secs(3))? {
                return Err(io::Error::other(format!(
                    "UDC {udc} did not become free after unbinding {}",
                    prev.display()
                )));
            }
            let action = if self.config.restore_previous_gadget {
                "will restore on exit"
            } else {
                "will leave unbound on exit"
            };
            println!(
                "[uvc] temporarily unbound {} from {} ({})",
                prev.file_name()
                    .map(|v| v.to_string_lossy())
                    .unwrap_or_else(|| prev.display().to_string().into()),
                udc,
                action
            );
        }

        self.write_gadget("UDC", &udc)?;
        let node = self.find_uvc_node(&before, Duration::from_secs(5))?;
        println!(
            "[uvc] gadget {} bound to UDC {}, codec={}, node={}",
            self.config.gadget_name,
            udc,
            codec.name(),
            node.display()
        );
        self.node = Some(node.clone());
        Ok(node)
    }

    pub fn rebind(&mut self) -> io::Result<PathBuf> {
        let udc = self
            .udc_name
            .clone()
            .ok_or_else(|| io::Error::other("cannot rebind before initial UDC bind"))?;
        println!("[uvc] rebind {} on {}", self.config.gadget_name, udc);
        self.unbind_path(&self.gadget);
        if !self.wait_udc_free(&udc, Duration::from_secs(3))? {
            println!("[uvc] warning: UDC {udc} still busy before rebind");
        }
        let before = video_node_set();
        self.write_gadget("UDC", &udc)?;
        let node = self.find_uvc_node(&before, Duration::from_secs(5))?;
        self.node = Some(node.clone());
        println!("[uvc] rebound node={}", node.display());
        Ok(node)
    }

    pub fn teardown(&mut self) -> io::Result<()> {
        self.unbind_path(&self.gadget);
        if let Some(udc) = &self.udc_name {
            let _ = self.wait_udc_free(udc, Duration::from_secs(3));
        }
        remove_configfs_tree_best_effort(&self.gadget);
        self.built = false;
        self.node = None;
        Ok(())
    }

    pub fn close(&mut self) -> io::Result<()> {
        if !self.config.keep {
            self.teardown()?;
            if self.config.restore_previous_gadget {
                self.restore_previous_gadget();
            } else if let Some(prev) = &self.prev_gadget {
                println!(
                    "[uvc] leaving previous gadget {} unbound on exit",
                    prev.file_name()
                        .map(|v| v.to_string_lossy())
                        .unwrap_or_else(|| prev.display().to_string().into())
                );
            }
        }
        Ok(())
    }

    fn build_native_descriptor(&mut self, codec: UvcCodec) -> io::Result<()> {
        let g = &self.gadget;
        let f = &self.func;
        fs::create_dir(g)?;
        self.built = true;

        self.write_gadget("idVendor", DEFAULT_VENDOR)?;
        self.write_gadget("idProduct", DEFAULT_PRODUCT)?;
        self.write_gadget("bcdUSB", DEFAULT_BCD_USB)?;
        self.write_gadget("bcdDevice", codec.bcd_device())?;
        self.write_gadget("bDeviceClass", "0xEF")?;
        self.write_gadget("bDeviceSubClass", "0x02")?;
        self.write_gadget("bDeviceProtocol", "0x01")?;

        fs::create_dir_all(g.join("strings/0x409"))?;
        self.write_gadget("strings/0x409/manufacturer", "lmi")?;
        self.write_gadget("strings/0x409/product", "lmi OV13B10 camera")?;
        self.write_gadget("strings/0x409/serialnumber", codec.serial())?;

        fs::create_dir_all(g.join("configs/c.1/strings/0x409"))?;
        self.write_gadget("configs/c.1/strings/0x409/configuration", "uvc")?;
        self.write_gadget("configs/c.1/MaxPower", "500")?;

        fs::create_dir_all(f)?;
        fs::create_dir_all(f.join("control/class/fs"))?;
        fs::create_dir_all(f.join("control/class/ss"))?;
        self.try_write(
            &f.join("streaming_maxpacket"),
            self.config.maxpacket.to_string(),
        );

        self.build_native_frames(codec)?;

        for speed in ["fs", "hs", "ss"] {
            symlink_force(
                f.join("streaming/header/h"),
                f.join(format!("streaming/class/{speed}/h")),
            )?;
        }
        fs::create_dir_all(f.join("control/header/h"))?;
        if codec == UvcCodec::H264 {
            self.try_write(&f.join("control/header/h/bcdUVC"), "0x0150");
        }
        for speed in ["fs", "ss"] {
            symlink_force(
                f.join("control/header/h"),
                f.join(format!("control/class/{speed}/h")),
            )?;
        }
        symlink_force(f, g.join("configs/c.1/uvc.0"))?;
        Ok(())
    }

    fn build_native_frames(&self, codec: UvcCodec) -> io::Result<()> {
        let fmt_dir = codec.format_dir(&self.func);
        fs::create_dir_all(&fmt_dir)?;
        self.try_write(&fmt_dir.join("bFormatIndex"), "1");
        self.try_write(
            &fmt_dir.join("bDefaultFrameIndex"),
            self.config.default_frame_index.to_string(),
        );
        if codec == UvcCodec::H264 {
            self.try_write(&fmt_dir.join("bBitsPerPixel"), "0");
            self.try_write(&fmt_dir.join("bmInterfaceFlags"), "0");
        }

        for mode in OV13B10_NATIVE_MODES {
            let dir = fmt_dir.join(mode.frame_name());
            fs::create_dir_all(&dir)?;
            let (min_bitrate, max_bitrate, max_frame) = match codec {
                UvcCodec::Mjpeg => {
                    let bitrate = u64::from(mode.max_frame_bytes()) * 8 * u64::from(mode.fps_cap);
                    ((bitrate / 3).max(1), bitrate.max(1), mode.max_frame_bytes())
                }
                UvcCodec::H264 => (
                    u64::from(mode.h264_bitrate() / 2).max(1),
                    u64::from(mode.h264_peak_bitrate()).max(1),
                    mode.h264_max_record_bytes(),
                ),
            };
            self.try_write(&dir.join("wWidth"), mode.width.to_string());
            self.try_write(&dir.join("wHeight"), mode.height.to_string());
            self.try_write(
                &dir.join("dwMaxVideoFrameBufferSize"),
                max_frame.to_string(),
            );
            self.try_write(
                &dir.join("dwFrameInterval"),
                mode.interval_100ns.to_string(),
            );
            self.try_write(
                &dir.join("dwDefaultFrameInterval"),
                mode.interval_100ns.to_string(),
            );
            if codec == UvcCodec::H264 {
                self.try_write(&dir.join("dwBytesPerLine"), "0");
            }
            self.try_write(&dir.join("dwMinBitRate"), min_bitrate.to_string());
            self.try_write(&dir.join("dwMaxBitRate"), max_bitrate.to_string());
        }

        if codec == UvcCodec::Mjpeg {
            let color = self.func.join("streaming/color_matching/mjpg");
            if fs::create_dir_all(&color).is_ok() {
                self.try_write(&color.join("bColorPrimaries"), "1");
                self.try_write(&color.join("bTransferCharacteristics"), "1");
                self.try_write(&color.join("bMatrixCoefficients"), "4");
                let _ = symlink_force(&color, fmt_dir.join("mjpg"));
            }
        }

        fs::create_dir_all(self.func.join("streaming/header/h"))?;
        symlink_force(
            &fmt_dir,
            self.func
                .join(format!("streaming/header/h/{}", codec.header_name())),
        )?;
        fs::create_dir_all(self.func.join("streaming/class/fs"))?;
        fs::create_dir_all(self.func.join("streaming/class/hs"))?;
        fs::create_dir_all(self.func.join("streaming/class/ss"))?;
        Ok(())
    }

    fn write_gadget(&self, rel: &str, value: &str) -> io::Result<()> {
        fs::write(self.gadget.join(rel), value)
    }

    fn try_write(&self, path: &Path, value: impl AsRef<str>) {
        let _ = fs::write(path, value.as_ref());
    }

    fn detect_udc(&self) -> io::Result<String> {
        if let Some(udc) = &self.config.udc {
            return Ok(udc.clone());
        }
        let mut udcs = Vec::new();
        for entry in fs::read_dir("/sys/class/udc")? {
            let entry = entry?;
            udcs.push(entry.file_name().to_string_lossy().into_owned());
        }
        udcs.sort();
        udcs.into_iter().next().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                "no UDC available in /sys/class/udc",
            )
        })
    }

    fn gadget_on_udc(&self, udc: &str) -> io::Result<Option<PathBuf>> {
        for entry in fs::read_dir(&self.config.configfs)? {
            let entry = entry?;
            let path = entry.path();
            if path == self.gadget {
                continue;
            }
            if read_trimmed(path.join("UDC")).as_deref() == Some(udc) {
                return Ok(Some(path));
            }
        }
        Ok(None)
    }

    fn unbind_path(&self, gadget: &Path) {
        let _ = fs::write(gadget.join("UDC"), "\n");
    }

    fn wait_udc_free(&self, udc: &str, timeout: Duration) -> io::Result<bool> {
        let deadline = Instant::now() + timeout;
        loop {
            let mut busy = false;
            for entry in fs::read_dir(&self.config.configfs)? {
                let entry = entry?;
                if read_trimmed(entry.path().join("UDC")).as_deref() == Some(udc) {
                    busy = true;
                    break;
                }
            }
            if !busy {
                return Ok(true);
            }
            if Instant::now() >= deadline {
                return Ok(false);
            }
            thread::sleep(Duration::from_millis(50));
        }
    }

    fn restore_previous_gadget(&self) {
        let Some(prev) = &self.prev_gadget else {
            return;
        };
        let Some(udc) = &self.udc_name else {
            return;
        };
        match fs::write(prev.join("UDC"), udc) {
            Ok(()) => println!("[uvc] restored {} on {}", prev.display(), udc),
            Err(err) => println!(
                "[uvc] warning: could not restore {} on {}: {}",
                prev.display(),
                udc,
                err
            ),
        }
    }

    fn find_uvc_node(&self, before: &BTreeSet<PathBuf>, timeout: Duration) -> io::Result<PathBuf> {
        let deadline = Instant::now() + timeout;
        loop {
            let after = video_node_set();
            for node in after.difference(before) {
                return Ok(node.clone());
            }
            if let Ok(nodes) = devices::list_video_nodes() {
                for node in nodes {
                    let name = node.name.unwrap_or_default().to_lowercase();
                    let driver = node.driver.unwrap_or_default().to_lowercase();
                    let sys = PathBuf::from("/sys/class/video4linux")
                        .join(node.path.file_name().unwrap_or_default());
                    let devpath = fs::read_link(sys.join("device"))
                        .map(|p| p.to_string_lossy().to_lowercase())
                        .unwrap_or_default();
                    if name.contains("uvc")
                        || name.contains("gadget")
                        || driver.contains("configfs-gadget")
                        || devpath.contains("gadget")
                    {
                        return Ok(node.path);
                    }
                }
            }
            if Instant::now() >= deadline {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "UVC gadget video node did not appear after UDC bind",
                ));
            }
            thread::sleep(Duration::from_millis(50));
        }
    }
}

impl Drop for UvcGadget {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

pub fn read_status_codec(gadget_name: &str, codec: UvcCodec) -> io::Result<UvcStatus> {
    let gadget = PathBuf::from(CONFIGFS).join(gadget_name);
    let frame_dir = codec.format_dir(&gadget.join("functions/uvc.0"));
    let mut frames = Vec::new();
    for entry in fs::read_dir(&frame_dir)? {
        let entry = entry?;
        let file_type = entry.file_type()?;
        if !file_type.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        if !name.starts_with('f') {
            continue;
        }
        let path = entry.path();
        frames.push(UvcFrameStatus {
            name,
            width: read_u32(path.join("wWidth"))?,
            height: read_u32(path.join("wHeight"))?,
            interval_100ns: read_u32(path.join("dwFrameInterval"))?,
            max_frame_bytes: read_u32(path.join("dwMaxVideoFrameBufferSize"))?,
        });
    }
    frames.sort_by_key(|frame| frame.name.clone());
    Ok(UvcStatus {
        gadget,
        codec,
        frames,
    })
}

pub fn assert_native_six_codec(gadget_name: &str, codec: UvcCodec) -> io::Result<()> {
    let status = read_status_codec(gadget_name, codec)?;
    println!(
        "gadget={} codec={}",
        status.gadget.display(),
        status.codec.name()
    );
    if status.frames.len() != OV13B10_NATIVE_MODES.len() {
        print_status(&status);
        return Err(io::Error::other(format!(
            "expected exactly {} native {} frames, found {}",
            OV13B10_NATIVE_MODES.len(),
            codec.name(),
            status.frames.len()
        )));
    }
    for (actual, expected) in status.frames.iter().zip(OV13B10_NATIVE_MODES) {
        let expected_status = expected_status(expected, codec);
        println!(
            "frame {} {}x{} interval={} max={} mode_index={}",
            actual.name,
            actual.width,
            actual.height,
            actual.interval_100ns,
            actual.max_frame_bytes,
            expected.mode_index
        );
        if *actual != expected_status {
            return Err(io::Error::other(format!(
                "native UVC frame mismatch: expected {:?}, got {:?}",
                expected_status, actual
            )));
        }
    }
    println!("uvc-status: PASS native-six {}", codec.name());
    Ok(())
}

pub fn print_status(status: &UvcStatus) {
    for frame in &status.frames {
        println!(
            "frame {} {}x{} interval={} max={}",
            frame.name, frame.width, frame.height, frame.interval_100ns, frame.max_frame_bytes
        );
    }
}

fn expected_status(mode: NativeMode, codec: UvcCodec) -> UvcFrameStatus {
    UvcFrameStatus {
        name: mode.frame_name(),
        width: mode.width,
        height: mode.height,
        interval_100ns: mode.interval_100ns,
        max_frame_bytes: codec.max_frame_bytes(mode),
    }
}

fn video_node_set() -> BTreeSet<PathBuf> {
    fs::read_dir("/dev")
        .ok()
        .into_iter()
        .flat_map(|iter| iter.filter_map(Result::ok))
        .filter_map(|entry| {
            let name = entry.file_name();
            let name = name.to_string_lossy();
            if name.starts_with("video") {
                Some(entry.path())
            } else {
                None
            }
        })
        .collect()
}

fn read_trimmed(path: impl AsRef<Path>) -> Option<String> {
    fs::read_to_string(path)
        .ok()
        .map(|v| v.trim().to_string())
        .filter(|v| !v.is_empty())
}

fn read_u32(path: impl AsRef<Path>) -> io::Result<u32> {
    let path = path.as_ref();
    let value = fs::read_to_string(path)?;
    value.trim().parse::<u32>().map_err(|_| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("invalid integer in {}", path.display()),
        )
    })
}

fn symlink_force(src: impl AsRef<Path>, dst: impl AsRef<Path>) -> io::Result<()> {
    let dst = dst.as_ref();
    let _ = fs::remove_file(dst);
    std::os::unix::fs::symlink(src, dst)
}

fn remove_configfs_tree_best_effort(path: &Path) {
    if !path.exists() {
        return;
    }
    if let Ok(entries) = fs::read_dir(path) {
        let mut entries: Vec<PathBuf> = entries
            .filter_map(|entry| entry.ok().map(|e| e.path()))
            .collect();
        entries.sort_by_key(|p| std::cmp::Reverse(p.components().count()));
        for entry in entries {
            if fs::symlink_metadata(&entry)
                .map(|m| m.file_type().is_symlink())
                .unwrap_or(false)
            {
                let _ = fs::remove_file(&entry);
            } else if entry.is_dir() {
                remove_configfs_tree_best_effort(&entry);
            }
        }
    }
    let _ = fs::remove_dir(path);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn expected_native_status_is_exactly_six_mjpeg_frames() {
        let frames: Vec<UvcFrameStatus> = OV13B10_NATIVE_MODES
            .iter()
            .copied()
            .map(|mode| expected_status(mode, UvcCodec::Mjpeg))
            .collect();

        assert_eq!(frames.len(), 6);
        assert_eq!(frames[0].name, "f01_4208x3120");
        assert_eq!(frames[5].name, "f06_1364x768");
        assert!(!frames.iter().any(|f| f.width == 1280 && f.height == 720));
    }

    #[test]
    fn h264_native_status_uses_smaller_record_budget() {
        let mjpeg = expected_status(OV13B10_NATIVE_MODES[0], UvcCodec::Mjpeg);
        let h264 = expected_status(OV13B10_NATIVE_MODES[0], UvcCodec::H264);

        assert_eq!(h264.name, "f01_4208x3120");
        assert_eq!(h264.width, mjpeg.width);
        assert_eq!(h264.height, mjpeg.height);
        assert!(h264.max_frame_bytes < mjpeg.max_frame_bytes);
    }
}
