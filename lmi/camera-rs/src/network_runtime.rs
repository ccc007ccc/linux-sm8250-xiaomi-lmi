use crate::controls::{self, SensorControlConfig};
use crate::devices;
use crate::isp::{IspCommand, IspSupervisor, UvcIspProfile};
use crate::native_modes::{NativeMode, OV13B10_NATIVE_MODES, by_frame_index};
use crate::route::LmiRouteConfig;
use std::ffi::CString;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::os::raw::{c_char, c_int, c_uint};
use std::os::unix::fs::{FileTypeExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const SIGINT: c_int = 2;
const SIGTERM: c_int = 15;
const O_NONBLOCK: i32 = 0o4000;
const LMI_UVC_RECORD_MAGIC: u32 = 0x43564d4c;
const LMI_UVC_RECORD_VERSION_V1: u16 = 1;
const LMI_UVC_RECORD_VERSION_V2: u16 = 2;
const LMI_UVC_RECORD_BASE_HEADER_SIZE: usize = 16;
const LMI_UVC_RECORD_MAX_HEADER_SIZE: usize = 64;
const FIFO_READ_CHUNK: usize = 65536;
const HTTP_BOUNDARY: &str = "lmi-mjpeg";
const CONTROL_WRITE_GRACE: Duration = Duration::from_millis(900);
const PROCESS_GRACE: Duration = Duration::from_millis(800);
const H264_RTP_PAYLOAD_TYPE: u8 = 96;
const RTP_JPEG_PAYLOAD_TYPE: u8 = 26;
const RTP_JPEG_MAX_PAYLOAD: usize = 1200;
/* Windows Network Camera/FrameServer drops whole MJPEG frames if a 720p
 * RTP/JPEG frame arrives as one short UDP burst.  Pace small packet groups so
 * the receiver socket can drain without lowering the JPEG quality. */
const RTP_JPEG_UDP_PACKET_BURST: u32 = 2;
const RTP_JPEG_UDP_PACKET_GAP: Duration = Duration::from_micros(300);
const RTP_JPEG_RESTART_TYPE_OFFSET: u8 = 64;
const RTP_JPEG_WHOLE_FRAME_RESTART_COUNT: u16 = 0x3fff;
const RTSP_PUBLIC_METHODS: &str = "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER";
const RTSP_SETUP_MODE_PLAY_GRACE_MS: u64 = 250;
const H264_PIPELINE_STARTUP_TIMEOUT: Duration = Duration::from_secs(20);
const H264_PIPELINE_STALE_TIMEOUT: Duration = Duration::from_secs(8);

static STOP_REQUESTED: AtomicBool = AtomicBool::new(false);
static WS_DISCOVERY_INSTANCE_ID: AtomicU64 = AtomicU64::new(0);
static WS_DISCOVERY_MESSAGE_NUMBER: AtomicU64 = AtomicU64::new(1);

unsafe extern "C" {
    fn mkfifo(path: *const c_char, mode: c_uint) -> c_int;
    fn signal(signum: c_int, handler: extern "C" fn(c_int)) -> usize;
    fn kill(pid: c_int, sig: c_int) -> c_int;
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
enum NetworkCodec {
    Mjpeg,
    H264,
}

impl NetworkCodec {
    fn name(self) -> &'static str {
        match self {
            Self::Mjpeg => "mjpeg",
            Self::H264 => "h264",
        }
    }

    fn max_frame_bytes(self, mode: NativeMode) -> u32 {
        match self {
            Self::Mjpeg => mode.max_frame_bytes(),
            Self::H264 => mode.h264_max_record_bytes(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct NetworkRunConfig {
    pub raw_node: PathBuf,
    pub ctrl_node: Option<PathBuf>,
    pub isp_bin: PathBuf,
    pub fifo: PathBuf,
    pub isp_nv12_fifo: PathBuf,
    pub isp_control_fifo: PathBuf,
    pub venus_bin: PathBuf,
    pub venus_node: PathBuf,
    codec: NetworkCodec,
    pub rtsp_force_tcp: bool,
    pub listen: String,
    pub rtsp_listen: Option<String>,
    pub rtsp_path: String,
    pub onvif: bool,
    pub onvif_listen: String,
    pub onvif_uuid: String,
    pub onvif_name: String,
    pub frame_index: u32,
    pub out_width: Option<u32>,
    pub out_height: Option<u32>,
    pub fps_cap: Option<u32>,
    pub setup_route: bool,
    pub route: LmiRouteConfig,
    pub controls: SensorControlConfig,
    pub gamma: f32,
    pub tone_highlight_knee: u32,
    pub tone_highlight_max: u32,
    pub max_soft_gain: f32,
    pub auto_exposure: bool,
    pub ae_target: u32,
    pub ae_clip_target: Option<u32>,
    pub ae_clip_weight: u32,
    pub max_digital_gain: Option<u32>,
    pub flicker: Option<String>,
    pub mjpeg_quality: u32,
    pub mjpeg_sharpen: u32,
    pub mjpeg_smooth: u32,
    pub mjpeg_area_scale: u32,
    pub mjpeg_subsampling: u32,
    pub mjpeg_scale_mode: String,
    pub mjpeg_fast_threads: u32,
    pub cpu_affinity: Option<String>,
    pub h264_gop: u32,
    pub h264_profile: String,
    pub h264_level: String,
    pub h264_bitrate_mode: String,
    pub h264_bitrate: Option<u32>,
    pub h264_peak_bitrate: Option<u32>,
    pub h264_frame_rc: Option<bool>,
    pub h264_output_buffers: u32,
    pub h264_capture_buffers: u32,
    pub h264_max_qp: Option<u32>,
    pub h264_auto_restart: bool,
}

impl Default for NetworkRunConfig {
    fn default() -> Self {
        Self {
            raw_node: PathBuf::from("/dev/video3"),
            ctrl_node: None,
            isp_bin: PathBuf::from("/run/lmi-camera/lmi-isp"),
            fifo: PathBuf::from("/run/lmi-camera/lmi-netcam.fifo"),
            isp_nv12_fifo: PathBuf::from("/run/lmi-camera/lmi-netcam-nv12.fifo"),
            isp_control_fifo: PathBuf::from("/run/lmi-camera/lmi-netcam.control"),
            venus_bin: PathBuf::from("/run/lmi-camera/lmi-venus-enc"),
            venus_node: PathBuf::new(),
            codec: NetworkCodec::Mjpeg,
            rtsp_force_tcp: false,
            listen: "127.0.0.1:8080".to_string(),
            rtsp_listen: None,
            rtsp_path: "/stream.mjpg".to_string(),
            onvif: false,
            onvif_listen: "0.0.0.0:3702".to_string(),
            onvif_uuid: "2d5b7c2c-6f3a-4a3d-9b5a-000000000001".to_string(),
            onvif_name: "LMI-OV13B10".to_string(),
            frame_index: 6,
            out_width: None,
            out_height: None,
            fps_cap: None,
            setup_route: true,
            route: LmiRouteConfig::default(),
            controls: SensorControlConfig::default(),
            gamma: 3.0,
            tone_highlight_knee: 0,
            tone_highlight_max: 255,
            max_soft_gain: 3.5,
            auto_exposure: true,
            ae_target: 85,
            ae_clip_target: Some(620),
            ae_clip_weight: 50,
            max_digital_gain: Some(1024),
            flicker: Some("auto".to_string()),
            mjpeg_quality: 90,
            mjpeg_sharpen: 0,
            mjpeg_smooth: 0,
            mjpeg_area_scale: 100,
            mjpeg_subsampling: 420,
            mjpeg_scale_mode: "bayer-area-frac".to_string(),
            mjpeg_fast_threads: 4,
            cpu_affinity: Some("4-7".to_string()),
            h264_gop: 30,
            h264_profile: "high".to_string(),
            h264_level: "4.2".to_string(),
            h264_bitrate_mode: "cbr".to_string(),
            h264_bitrate: None,
            h264_peak_bitrate: None,
            h264_frame_rc: Some(true),
            h264_output_buffers: 3,
            h264_capture_buffers: 4,
            h264_max_qp: Some(28),
            h264_auto_restart: true,
        }
    }
}

#[derive(Debug)]
struct ModeSwitchState {
    inner: Mutex<ModeSwitchInner>,
}

#[derive(Debug)]
struct ModeSwitchInner {
    current: NativeMode,
    requested: Option<NativeMode>,
}

impl ModeSwitchState {
    fn new(current: NativeMode) -> Self {
        Self {
            inner: Mutex::new(ModeSwitchInner {
                current,
                requested: None,
            }),
        }
    }

    fn request(&self, mode: NativeMode) -> bool {
        let mut inner = self.inner.lock().expect("mode switch state poisoned");
        if inner.current == mode {
            inner.requested = None;
            false
        } else {
            inner.requested = Some(mode);
            true
        }
    }

    fn take_requested(&self) -> Option<NativeMode> {
        self.inner
            .lock()
            .expect("mode switch state poisoned")
            .requested
            .take()
    }
}

#[derive(Debug, Clone)]
struct NetworkServiceInfo {
    http_listen: String,
    rtsp_listen: Option<String>,
    rtsp_path: String,
    rtsp_force_tcp: bool,
    onvif: bool,
    onvif_listen: String,
    onvif_uuid: String,
    onvif_name: String,
    codec: NetworkCodec,
    mjpeg_quality: u32,
    h264_gop: u32,
    h264_profile: String,
    h264_bitrate: Option<u32>,
    h264_profile_level_id: String,
    h264_sprop_parameter_sets: String,
    isp_control_fifo: PathBuf,
    available_modes: Vec<NativeMode>,
    mode_switch: Arc<ModeSwitchState>,
}

impl NetworkServiceInfo {
    fn from_config(
        config: &NetworkRunConfig,
        mode: NativeMode,
        mode_switch: Arc<ModeSwitchState>,
    ) -> Self {
        let advertised_mode = |mut native: NativeMode| {
            if let Some(fps_cap) = config.fps_cap {
                let fps_cap = fps_cap.min(native.fps_cap).max(1);
                native.fps_cap = fps_cap;
                native.nominal_fps_milli = fps_cap.saturating_mul(1000);
            }
            native
        };
        let mut available_modes = Vec::with_capacity(OV13B10_NATIVE_MODES.len());
        available_modes.push(mode);
        if config.out_width.is_none() && config.out_height.is_none() {
            available_modes.extend(
                OV13B10_NATIVE_MODES
                    .into_iter()
                    .filter(|native| native.frame_index != mode.frame_index)
                    .map(advertised_mode),
            );
        }
        Self {
            http_listen: config.listen.clone(),
            rtsp_listen: config.rtsp_listen.clone(),
            rtsp_path: config.rtsp_path.clone(),
            rtsp_force_tcp: config.rtsp_force_tcp,
            onvif: config.onvif,
            onvif_listen: config.onvif_listen.clone(),
            onvif_uuid: config.onvif_uuid.clone(),
            onvif_name: config.onvif_name.clone(),
            codec: config.codec,
            mjpeg_quality: config.mjpeg_quality,
            h264_gop: config.h264_gop,
            h264_profile: config.h264_profile.clone(),
            h264_bitrate: config.h264_bitrate,
            h264_profile_level_id: h264_profile_level_id_from_config(config),
            h264_sprop_parameter_sets: String::new(),
            isp_control_fifo: config.isp_control_fifo.clone(),
            available_modes,
            mode_switch,
        }
    }

    fn with_h264_sdp(mut self, sps: Option<&[u8]>, pps: Option<&[u8]>) -> Self {
        if let (Some(sps), Some(pps)) = (sps, pps) {
            self.h264_profile_level_id = h264_profile_level_id_from_sps(sps)
                .unwrap_or_else(|| self.h264_profile_level_id.clone());
            self.h264_sprop_parameter_sets =
                format!("{},{}", base64_encode(sps), base64_encode(pps));
        }
        self
    }

    fn stream_uri(&self, http_host: &str) -> String {
        match &self.rtsp_listen {
            Some(listen) => format!(
                "rtsp://{}{}",
                host_with_port(http_host, listen),
                self.rtsp_path
            ),
            None => format!("http://{http_host}/stream.mjpg"),
        }
    }

    fn snapshot_uri(&self, http_host: &str) -> Option<String> {
        match self.codec {
            NetworkCodec::Mjpeg => Some(format!("http://{http_host}/snapshot.jpg")),
            NetworkCodec::H264 => None,
        }
    }

    fn available_modes(&self) -> &[NativeMode] {
        &self.available_modes
    }

    fn h264_bitrate_kbps(&self, mode: NativeMode) -> u32 {
        self.h264_bitrate
            .unwrap_or_else(|| mode.h264_bitrate())
            .div_ceil(1000)
            .max(1)
    }
}

pub fn looks_like_network_run(args: &[String]) -> bool {
    matches!(args.first().map(String::as_str), Some("network"))
        || args
            .windows(2)
            .any(|pair| pair[0] == "--output" && pair[1] == "network")
}

pub fn parse_network_run_config<I>(args: I) -> io::Result<NetworkRunConfig>
where
    I: Iterator<Item = String>,
{
    let mut config = NetworkRunConfig::default();
    let mut args = args.peekable();

    if matches!(args.peek().map(String::as_str), Some("network")) {
        args.next();
    }

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--output" => {
                let output = next_value(&mut args, "--output")?;
                if output != "network" {
                    return Err(invalid_input(format!(
                        "unsupported --output '{output}', network runtime uses --output network"
                    )));
                }
            }
            "--profile" => {
                let profile = next_value(&mut args, "--profile")?;
                if profile != "native-modes" {
                    return Err(invalid_input(format!(
                        "unsupported network profile '{profile}', only native-modes is implemented"
                    )));
                }
            }
            "--codec" | "--netcam-codec" => {
                config.codec = parse_network_codec(&next_value(&mut args, &arg)?, &arg)?
            }
            "--listen" => config.listen = next_value(&mut args, "--listen")?,
            "--rtsp" => config.rtsp_listen = Some("0.0.0.0:8554".to_string()),
            "--no-rtsp" => config.rtsp_listen = None,
            "--rtsp-listen" => config.rtsp_listen = Some(next_value(&mut args, "--rtsp-listen")?),
            "--rtsp-path" => {
                config.rtsp_path = normalize_path(&next_value(&mut args, "--rtsp-path")?)
            }
            "--rtsp-force-tcp" => config.rtsp_force_tcp = true,
            "--no-rtsp-force-tcp" => config.rtsp_force_tcp = false,
            "--onvif" | "--ws-discovery" => config.onvif = true,
            "--no-onvif" | "--no-ws-discovery" => config.onvif = false,
            "--onvif-listen" | "--ws-discovery-listen" => {
                config.onvif_listen = next_value(&mut args, &arg)?
            }
            "--onvif-uuid" => config.onvif_uuid = next_value(&mut args, "--onvif-uuid")?,
            "--onvif-name" => config.onvif_name = next_value(&mut args, "--onvif-name")?,
            "--frame-index" | "--default-frame" => {
                config.frame_index = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--out-size" | "--stream-size" => {
                let (width, height) = parse_output_size(&next_value(&mut args, &arg)?, &arg)?;
                config.out_width = Some(width);
                config.out_height = Some(height);
            }
            "--out-width" | "--stream-width" => {
                config.out_width = Some(parse_u32(&next_value(&mut args, &arg)?, &arg)?);
            }
            "--out-height" | "--stream-height" => {
                config.out_height = Some(parse_u32(&next_value(&mut args, &arg)?, &arg)?);
            }
            "--fps-cap" | "--fps" => {
                let fps_cap = parse_u32(&next_value(&mut args, &arg)?, &arg)?;
                if fps_cap == 0 {
                    return Err(invalid_input(format!("invalid {arg} value '0'")));
                }
                config.fps_cap = Some(fps_cap);
            }
            "--raw" | "--video" => {
                config.raw_node = PathBuf::from(next_value(&mut args, &arg)?);
                config.route.raw_node = Some(config.raw_node.clone());
            }
            "--ctrl" => config.ctrl_node = Some(PathBuf::from(next_value(&mut args, "--ctrl")?)),
            "--isp-bin" => config.isp_bin = PathBuf::from(next_value(&mut args, "--isp-bin")?),
            "--venus-bin" | "--h264-encoder-bin" => {
                config.venus_bin = PathBuf::from(next_value(&mut args, &arg)?)
            }
            "--venus-node" | "--h264-encoder-node" => {
                config.venus_node = PathBuf::from(next_value(&mut args, &arg)?)
            }
            "--netcam-fifo" | "--fifo" => config.fifo = PathBuf::from(next_value(&mut args, &arg)?),
            "--isp-nv12-fifo" | "--venus-input-fifo" => {
                config.isp_nv12_fifo = PathBuf::from(next_value(&mut args, &arg)?)
            }
            "--isp-control-fifo" | "--control-fifo" => {
                config.isp_control_fifo = PathBuf::from(next_value(&mut args, &arg)?)
            }
            "--no-setup-route" => config.setup_route = false,
            "--setup-route" => config.setup_route = true,
            "--media" => config.route.media = PathBuf::from(next_value(&mut args, "--media")?),
            "--sensor" => config.route.sensor = next_value(&mut args, "--sensor")?,
            "--csiphy" => config.route.csiphy = next_value(&mut args, "--csiphy")?,
            "--csid" => config.route.csid = next_value(&mut args, "--csid")?,
            "--vfe" => config.route.vfe = next_value(&mut args, "--vfe")?,
            "--video-entity" => {
                config.route.video_entity = next_value(&mut args, "--video-entity")?
            }
            "--sensor-subdev" => {
                config.route.sensor_subdev =
                    Some(PathBuf::from(next_value(&mut args, "--sensor-subdev")?))
            }
            "--csiphy-subdev" => {
                config.route.csiphy_subdev =
                    Some(PathBuf::from(next_value(&mut args, "--csiphy-subdev")?))
            }
            "--csid-subdev" => {
                config.route.csid_subdev =
                    Some(PathBuf::from(next_value(&mut args, "--csid-subdev")?))
            }
            "--vfe-subdev" => {
                config.route.vfe_subdev =
                    Some(PathBuf::from(next_value(&mut args, "--vfe-subdev")?))
            }
            "--csiphy-source-pad" => {
                config.route.csiphy_source_pad = parse_u16(
                    &next_value(&mut args, "--csiphy-source-pad")?,
                    "--csiphy-source-pad",
                )?
            }
            "--csid-source-pad" => {
                config.route.csid_source_pad = parse_u16(
                    &next_value(&mut args, "--csid-source-pad")?,
                    "--csid-source-pad",
                )?
            }
            "--mbus-code" => {
                config.route.mbus_code =
                    parse_u32_auto(&next_value(&mut args, "--mbus-code")?, "--mbus-code")?
            }
            "--keep-links" => config.route.keep_links = true,
            "--route" => {
                let route = next_value(&mut args, "--route")?;
                if route != "lmi-ov13b10" {
                    return Err(invalid_input(format!(
                        "unsupported --route '{route}', only lmi-ov13b10 is implemented"
                    )));
                }
            }
            "--gamma" | "--isp-gamma" => {
                config.gamma = parse_f32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--tone-highlight-knee" | "--isp-tone-highlight-knee" => {
                config.tone_highlight_knee = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--tone-highlight-max" | "--isp-tone-highlight-max" => {
                config.tone_highlight_max = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--max-soft-gain" | "--isp-max-soft-gain" => {
                config.max_soft_gain = parse_f32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--target" | "--target-luma" => {
                config.ae_target = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--no-ae" => config.auto_exposure = false,
            "--auto-exposure" => config.auto_exposure = true,
            "--isp-ae-clip-target" => {
                config.ae_clip_target = Some(parse_u32(&next_value(&mut args, &arg)?, &arg)?)
            }
            "--isp-ae-clip-weight" => {
                config.ae_clip_weight = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--isp-max-digital-gain" => {
                config.max_digital_gain = Some(parse_u32(&next_value(&mut args, &arg)?, &arg)?)
            }
            "--flicker" | "--power-line-frequency" => {
                let value = next_value(&mut args, &arg)?;
                match value.as_str() {
                    "off" | "50" | "60" | "auto" => config.flicker = Some(value),
                    other => {
                        return Err(invalid_input(format!(
                            "invalid {arg} value '{other}', expected off, 50, 60, or auto"
                        )));
                    }
                }
            }
            "--no-flicker" => config.flicker = Some("off".to_string()),
            "--isp-uvc-mjpeg-quality" | "--mjpeg-quality" => {
                config.mjpeg_quality = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--isp-uvc-mjpeg-sharpen" | "--mjpeg-sharpen" => {
                config.mjpeg_sharpen = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--isp-uvc-mjpeg-smooth" | "--mjpeg-smooth" => {
                config.mjpeg_smooth = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--isp-uvc-mjpeg-area-scale" | "--mjpeg-area-scale" => {
                config.mjpeg_area_scale = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--isp-uvc-mjpeg-subsampling" | "--mjpeg-subsampling" => {
                config.mjpeg_subsampling = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--isp-uvc-mjpeg-scale-mode" | "--mjpeg-scale-mode" => {
                config.mjpeg_scale_mode = next_value(&mut args, &arg)?
            }
            "--isp-uvc-mjpeg-fast-threads" | "--mjpeg-fast-threads" => {
                config.mjpeg_fast_threads = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--isp-uvc-mjpeg-cpu-affinity" | "--cpu-affinity" => {
                let affinity = next_value(&mut args, &arg)?;
                config.cpu_affinity = if affinity.is_empty() {
                    None
                } else {
                    Some(affinity)
                };
            }
            "--h264-gop" | "--venus-gop" => {
                config.h264_gop = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--h264-profile" | "--venus-profile" => {
                config.h264_profile = next_value(&mut args, &arg)?
            }
            "--h264-level" | "--venus-level" => config.h264_level = next_value(&mut args, &arg)?,
            "--h264-bitrate-mode" | "--venus-bitrate-mode" => {
                let value = next_value(&mut args, &arg)?;
                match value.as_str() {
                    "vbr" | "cbr" | "cq" => config.h264_bitrate_mode = value,
                    other => {
                        return Err(invalid_input(format!(
                            "invalid {arg} value '{other}', expected vbr, cbr, or cq"
                        )));
                    }
                }
            }
            "--h264-bitrate" | "--venus-bitrate" => {
                config.h264_bitrate = Some(parse_u32(&next_value(&mut args, &arg)?, &arg)?)
            }
            "--h264-peak-bitrate" | "--venus-peak-bitrate" => {
                config.h264_peak_bitrate = Some(parse_u32(&next_value(&mut args, &arg)?, &arg)?)
            }
            "--h264-frame-rc" | "--venus-frame-rc" => config.h264_frame_rc = Some(true),
            "--no-h264-frame-rc" | "--no-venus-frame-rc" => config.h264_frame_rc = Some(false),
            "--h264-output-buffers" | "--venus-output-buffers" => {
                config.h264_output_buffers = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--h264-capture-buffers" | "--venus-capture-buffers" => {
                config.h264_capture_buffers = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--h264-auto-restart" | "--venus-auto-restart" => config.h264_auto_restart = true,
            "--no-h264-auto-restart" | "--no-venus-auto-restart" => {
                config.h264_auto_restart = false
            }
            "--h264-max-qp" | "--venus-max-qp" => {
                let value = parse_u32(&next_value(&mut args, &arg)?, &arg)?;
                if value == 0 {
                    config.h264_max_qp = None;
                } else if value <= 51 {
                    config.h264_max_qp = Some(value);
                } else {
                    return Err(invalid_input(format!(
                        "invalid {arg} value '{value}', expected 0 or 1..51"
                    )));
                }
            }
            "--vblank"
            | "--exposure"
            | "--analogue-gain"
            | "--digital-gain"
            | "--reset-controls"
            | "--preserve-controls" => {
                controls::parse_sensor_control_option(&mut config.controls, &arg, &mut args)?;
            }
            "--help" | "-h" => {
                print_network_run_usage();
                return Ok(config);
            }
            other => {
                return Err(invalid_input(format!(
                    "unknown network run option: {other}"
                )));
            }
        }
    }

    if by_frame_index(config.frame_index).is_none() {
        return Err(invalid_input(format!(
            "invalid network frame {}, expected one of the native frame indexes 1..{}",
            config.frame_index,
            OV13B10_NATIVE_MODES.len()
        )));
    }
    if config.out_width.is_some() != config.out_height.is_some() {
        return Err(invalid_input(
            "network output size requires both width and height; use --out-size WxH",
        ));
    }
    if let (Some(width), Some(height)) = (config.out_width, config.out_height) {
        validate_network_output_size(width, height)?;
        if config.codec == NetworkCodec::Mjpeg
            && config.rtsp_listen.is_some()
            && !rtp_jpeg_dimensions_supported(width, height)
        {
            return Err(invalid_input(format!(
                "MJPEG RTSP output {width}x{height} is too large for RTP/JPEG; use --out-size <=2040x2040 or --no-rtsp for HTTP MJPEG only"
            )));
        }
    }
    if config.rtsp_force_tcp && config.rtsp_listen.is_none() {
        return Err(invalid_input("--rtsp-force-tcp requires --rtsp"));
    }
    if config.codec == NetworkCodec::H264 {
        if config.rtsp_listen.is_none() {
            return Err(invalid_input(
                "--codec h264 requires --rtsp for RTSP/ONVIF streaming",
            ));
        }
        if config.rtsp_path == "/stream.mjpg" {
            config.rtsp_path = "/stream.h264".to_string();
        }
        if config.h264_output_buffers == 0 || config.h264_output_buffers > 4 {
            return Err(invalid_input(
                "--h264-output-buffers must be in the range 1..4",
            ));
        }
        if config.h264_capture_buffers == 0 || config.h264_capture_buffers > 4 {
            return Err(invalid_input(
                "--h264-capture-buffers must be in the range 1..4",
            ));
        }
    }

    config.route.raw_node = Some(config.raw_node.clone());
    Ok(config)
}

pub fn print_network_run_usage() {
    println!("Network camera runtime options:");
    println!(
        "  run --output network --profile native-modes [--codec mjpeg|h264] [--frame-index 1..6]"
    );
    println!("  --listen ADDR:PORT  (default 127.0.0.1:8080; use 0.0.0.0:8080 for LAN testing)");
    println!("  --fps-cap FPS  (optional cap; unset keeps the selected native frame rate)");
    println!("  --out-size WxH  (debug-only scaler override; unset keeps the native UVC size)");
    println!("  --rtsp --rtsp-listen ADDR:PORT --rtsp-path PATH [--rtsp-force-tcp]");
    println!("  --onvif --onvif-listen ADDR:PORT --onvif-uuid UUID --onvif-name NAME");
    println!("  --isp-bin PATH --netcam-fifo PATH --isp-control-fifo PATH");
    println!("  --venus-bin PATH [--venus-node DEV] --isp-nv12-fifo PATH  (for --codec h264)");
    println!("  --h264-bitrate-mode vbr|cbr|cq --h264-bitrate BPS --h264-peak-bitrate BPS");
    println!(
        "  --h264-max-qp 0|1..51 --no-h264-frame-rc --h264-output-buffers 1..4 --h264-capture-buffers 1..4"
    );
    println!("  --no-h264-auto-restart  (debug: stop instead of restart-looping on Venus stalls)");
    println!("  --raw DEV --ctrl DEV --media DEV --sensor-subdev DEV --keep-links");
    println!("  endpoints: / /status /snapshot.jpg /stream.mjpg /onvif/device_service");
}

enum NetworkSessionExit {
    Shutdown,
    Restart(NativeMode),
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
enum FrameLiveness {
    Empty,
    Fresh,
    Stale { sequence: u32, age: Duration },
}

pub fn run_network(mut config: NetworkRunConfig) -> io::Result<()> {
    install_signal_handlers();
    if config.codec == NetworkCodec::H264 {
        resolve_venus_node(&mut config)?;
    }
    config.route.raw_node = Some(config.raw_node.clone());

    let mut raw_mode = network_mode_with_fps_cap(&config, config.frame_index)?;
    loop {
        if STOP_REQUESTED.load(Ordering::SeqCst) {
            return Ok(());
        }
        match run_network_session(&config, raw_mode)? {
            NetworkSessionExit::Shutdown => return Ok(()),
            NetworkSessionExit::Restart(requested) => {
                raw_mode = network_mode_with_fps_cap(&config, requested.frame_index)?;
                STOP_REQUESTED.store(false, Ordering::SeqCst);
                println!(
                    "[netcam] restarting camera pipeline for native frame {} ({}x{} @ {:.3}fps)",
                    raw_mode.frame_index,
                    raw_mode.width,
                    raw_mode.height,
                    raw_mode.nominal_fps()
                );
            }
        }
    }
}

fn network_mode_with_fps_cap(
    config: &NetworkRunConfig,
    frame_index: u32,
) -> io::Result<NativeMode> {
    let mut mode =
        by_frame_index(frame_index).ok_or_else(|| invalid_input("invalid network native frame"))?;
    if let Some(fps_cap) = config.fps_cap {
        let fps_cap = fps_cap.min(mode.fps_cap).max(1);
        mode.fps_cap = fps_cap;
        mode.nominal_fps_milli = fps_cap.saturating_mul(1000);
    }
    Ok(mode)
}

fn run_network_session(
    config: &NetworkRunConfig,
    raw_mode: NativeMode,
) -> io::Result<NetworkSessionExit> {
    let stream_mode = network_stream_mode(config, raw_mode)?;
    let shared = Arc::new(FrameStore::default());
    shared.set_startup_phase("http-status-starting");
    let mode_switch = Arc::new(ModeSwitchState::new(stream_mode));
    let mut service = NetworkServiceInfo::from_config(config, stream_mode, mode_switch);
    if service.codec == NetworkCodec::Mjpeg
        && service.rtsp_listen.is_some()
        && !rtp_jpeg_mode_supported(stream_mode)
    {
        println!(
            "[netcam] RTSP RTP/JPEG disabled for {}x{}: RFC 2435 width/height fields only cover <=2040px; HTTP MJPEG remains available",
            stream_mode.width, stream_mode.height
        );
        service.rtsp_listen = None;
    }

    let mut http_server = Some(spawn_http_server(
        config.listen.clone(),
        stream_mode,
        shared.clone(),
        service.clone(),
    )?);
    let mut fifo_reader = None;

    shared.set_startup_phase("configuring-route");
    let (raw_node, ctrl_node) = match route_for_mode(config, raw_mode) {
        Ok(route) => route,
        Err(err) => {
            shared.set_startup_phase("route-failed");
            shared.set_error(format!("route setup failed: {err}"));
            cleanup_startup_failure(&shared, &mut http_server, &mut fifo_reader);
            return Err(err);
        }
    };
    shared.set_startup_phase("route-ready");
    if !config.setup_route && config.controls.should_apply(config.auto_exposure) {
        let Some(ctrl) = ctrl_node.as_deref() else {
            let err = invalid_input(
                "sensor controls requested but no control subdev is known; pass --ctrl or enable route setup",
            );
            shared.set_startup_phase("controls-failed");
            shared.set_error(err.to_string());
            cleanup_startup_failure(&shared, &mut http_server, &mut fifo_reader);
            return Err(err);
        };
        shared.set_startup_phase("applying-controls");
        if let Err(err) =
            controls::apply_initial_sensor_controls(ctrl, &config.controls, config.auto_exposure)
        {
            shared.set_startup_phase("controls-failed");
            shared.set_error(format!("initial controls failed: {err}"));
            cleanup_startup_failure(&shared, &mut http_server, &mut fifo_reader);
            return Err(err);
        }
    }

    shared.set_startup_phase("preparing-fifos");
    if let Err(err) = prepare_network_fifos(config) {
        shared.set_startup_phase("fifos-failed");
        shared.set_error(format!("FIFO setup failed: {err}"));
        cleanup_startup_failure(&shared, &mut http_server, &mut fifo_reader);
        return Err(err);
    }
    shared.set_startup_phase("fifos-ready");

    let profile = build_network_isp_profile(config, raw_mode, stream_mode, raw_node, ctrl_node);
    shared.set_startup_phase("fifo-reader-starting");
    fifo_reader = Some(
        match spawn_fifo_reader(
            config.fifo.clone(),
            profile.max_frame_bytes,
            config.codec,
            shared.clone(),
        ) {
            Ok(reader) => reader,
            Err(err) => {
                shared.set_startup_phase("fifo-reader-failed");
                shared.set_error(format!("FIFO reader failed: {err}"));
                cleanup_startup_failure(&shared, &mut http_server, &mut fifo_reader);
                return Err(err);
            }
        },
    );

    println!(
        "[netcam] native-modes ready: codec={} frame {} -> OV13B10 mode {} raw {}x{} -> stream {}x{} @ {:.3}fps",
        config.codec.name(),
        raw_mode.frame_index,
        raw_mode.mode_index,
        raw_mode.width,
        raw_mode.height,
        stream_mode.width,
        stream_mode.height,
        stream_mode.nominal_fps()
    );
    println!(
        "[netcam] HTTP endpoints: http://{}/ /status /snapshot.jpg /stream.mjpg /onvif/device_service",
        config.listen
    );
    if let Some(rtsp) = &service.rtsp_listen {
        println!(
            "[netcam] RTSP {} endpoint: rtsp://{}{}",
            service.codec.name(),
            rtsp,
            service.rtsp_path
        );
    }
    if service.onvif {
        println!(
            "[netcam] ONVIF WS-Discovery enabled on {} uuid={}",
            service.onvif_listen, service.onvif_uuid
        );
    }
    println!(
        "[netcam] raw {} stays truthful pgAA",
        profile.raw_node.display()
    );

    shared.set_startup_phase("pipeline-starting");
    let mut pipeline = match (|| -> io::Result<PipelineSupervisor> {
        match config.codec {
            NetworkCodec::Mjpeg => {
                let command = IspCommand::for_uvc_mjpeg(&profile);
                print!("[isp] command: ");
                command.print_shell();
                Ok(PipelineSupervisor::mjpeg(command.spawn()?))
            }
            NetworkCodec::H264 => {
                let encoder_command = EncoderCommand::for_mode(config, stream_mode);
                print!("[venus] command: ");
                encoder_command.print_shell();
                let encoder = encoder_command.spawn()?;
                let command = IspCommand::for_uvc_nv12(&profile);
                print!("[isp] command: ");
                command.print_shell();
                let isp = match command.spawn() {
                    Ok(isp) => isp,
                    Err(err) => {
                        let mut encoder = encoder;
                        let _ = encoder.terminate();
                        return Err(err);
                    }
                };
                Ok(PipelineSupervisor::h264(encoder, isp))
            }
        }
    })() {
        Ok(pipeline) => pipeline,
        Err(err) => {
            shared.set_startup_phase("pipeline-failed");
            shared.set_error(format!("camera pipeline failed: {err}"));
            cleanup_startup_failure(&shared, &mut http_server, &mut fifo_reader);
            return Err(err);
        }
    };
    println!("[netcam] lmi-isp pid={}", pipeline.isp_id());
    if let Some(pid) = pipeline.encoder_id() {
        println!("[netcam] lmi-venus-enc pid={pid}");
    }
    shared.set_startup_phase("pipeline-running");

    if let Some(flicker) = &config.flicker {
        shared.set_startup_phase("applying-flicker-control");
        apply_control_command(&config.isp_control_fifo, &format!("flicker={flicker}"));
    }

    if config.codec == NetworkCodec::H264 {
        let (sps, pps) = shared.wait_for_h264_params(Duration::from_secs(4));
        if sps.is_none() || pps.is_none() {
            println!(
                "[netcam] H.264 SPS/PPS not available yet; RTSP SDP will use profile-level-id fallback and omit sprop-parameter-sets until encoder emits an IDR"
            );
        }
        service = service.with_h264_sdp(sps.as_deref(), pps.as_deref());
    }

    let mut rtsp_server = match service.rtsp_listen.clone() {
        Some(listen) => {
            shared.set_startup_phase("rtsp-starting");
            Some(spawn_rtsp_server(
                listen,
                stream_mode,
                shared.clone(),
                service.clone(),
            )?)
        }
        None => None,
    };
    let mut ws_discovery = if service.onvif {
        shared.set_startup_phase("ws-discovery-starting");
        Some(spawn_ws_discovery(stream_mode, service.clone())?)
    } else {
        None
    };
    shared.set_startup_phase("serving");

    let mut exit_error = None;
    let mut restart_mode = None;
    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        if let Some(requested) = service.mode_switch.take_requested() {
            println!(
                "[netcam] ONVIF requested native frame {} ({}x{} @ {:.3}fps); restarting local pipeline",
                requested.frame_index,
                requested.width,
                requested.height,
                requested.nominal_fps()
            );
            restart_mode = Some(requested);
            break;
        }

        if let Some(exit) = pipeline.try_wait()? {
            if service.codec == NetworkCodec::H264 && config.h264_auto_restart {
                println!(
                    "[netcam] {} exited while H.264 network camera was active: {}; restarting local pipeline",
                    exit.component, exit.status
                );
                restart_mode = Some(stream_mode);
            } else if service.codec == NetworkCodec::H264 {
                println!(
                    "[netcam] {} exited while H.264 network camera was active: {}; stopping without local restart",
                    exit.component, exit.status
                );
                STOP_REQUESTED.store(true, Ordering::SeqCst);
                shared.notify_shutdown();
                exit_error = Some(io::Error::other(format!(
                    "{} exited while H.264 network camera was active: {}",
                    exit.component, exit.status
                )));
            } else {
                STOP_REQUESTED.store(true, Ordering::SeqCst);
                shared.notify_shutdown();
                exit_error = Some(io::Error::other(format!(
                    "{} exited while network camera was active: {}",
                    exit.component, exit.status
                )));
            }
            break;
        }

        if service.codec == NetworkCodec::H264 {
            match shared.frame_liveness(H264_PIPELINE_STALE_TIMEOUT) {
                FrameLiveness::Fresh => {}
                FrameLiveness::Empty if pipeline.uptime() >= H264_PIPELINE_STARTUP_TIMEOUT => {
                    if config.h264_auto_restart {
                        println!(
                            "[netcam] H.264 pipeline produced no frames for {}ms after startup; restarting local pipeline",
                            pipeline.uptime().as_millis()
                        );
                        restart_mode = Some(stream_mode);
                    } else {
                        let age_ms = pipeline.uptime().as_millis();
                        println!(
                            "[netcam] H.264 pipeline produced no frames for {}ms after startup; stopping without local restart",
                            age_ms
                        );
                        STOP_REQUESTED.store(true, Ordering::SeqCst);
                        shared.notify_shutdown();
                        exit_error = Some(io::Error::other(format!(
                            "H.264 pipeline produced no frames for {age_ms}ms after startup"
                        )));
                    }
                    break;
                }
                FrameLiveness::Empty => {}
                FrameLiveness::Stale { sequence, age } => {
                    if config.h264_auto_restart {
                        println!(
                            "[netcam] H.264 pipeline stale: latest sequence={} age={}ms; restarting local pipeline",
                            sequence,
                            age.as_millis()
                        );
                        restart_mode = Some(stream_mode);
                    } else {
                        let age_ms = age.as_millis();
                        println!(
                            "[netcam] H.264 pipeline stale: latest sequence={} age={}ms; stopping without local restart",
                            sequence, age_ms
                        );
                        STOP_REQUESTED.store(true, Ordering::SeqCst);
                        shared.notify_shutdown();
                        exit_error = Some(io::Error::other(format!(
                            "H.264 pipeline stale: latest sequence={sequence} age={age_ms}ms"
                        )));
                    }
                    break;
                }
            }
        }

        if let Some(handle) = fifo_reader.as_ref() {
            if handle.is_finished() {
                let result = fifo_reader.take().expect("reader disappeared").join();
                STOP_REQUESTED.store(true, Ordering::SeqCst);
                shared.notify_shutdown();
                exit_error = Some(join_result("FIFO reader", result));
                break;
            }
        }

        if let Some(handle) = http_server.as_ref() {
            if handle.is_finished() {
                let result = http_server.take().expect("server disappeared").join();
                STOP_REQUESTED.store(true, Ordering::SeqCst);
                shared.notify_shutdown();
                exit_error = Some(join_result("HTTP server", result));
                break;
            }
        }

        if let Some(handle) = rtsp_server.as_ref() {
            if handle.is_finished() {
                let result = rtsp_server.take().expect("RTSP server disappeared").join();
                STOP_REQUESTED.store(true, Ordering::SeqCst);
                shared.notify_shutdown();
                exit_error = Some(join_result("RTSP server", result));
                break;
            }
        }

        if let Some(handle) = ws_discovery.as_ref() {
            if handle.is_finished() {
                let result = ws_discovery
                    .take()
                    .expect("WS-Discovery server disappeared")
                    .join();
                STOP_REQUESTED.store(true, Ordering::SeqCst);
                shared.notify_shutdown();
                exit_error = Some(join_result("WS-Discovery server", result));
                break;
            }
        }

        thread::sleep(Duration::from_millis(80));
    }

    STOP_REQUESTED.store(true, Ordering::SeqCst);
    shared.notify_shutdown();
    println!("[netcam] shutdown requested: stopping camera pipeline and network endpoints");
    pipeline.terminate()?;
    if let Some(handle) = http_server.take() {
        let _ = handle.join();
    }
    if let Some(handle) = rtsp_server.take() {
        let _ = handle.join();
    }
    if service.onvif {
        send_ws_discovery_bye(&service);
    }
    if let Some(handle) = ws_discovery.take() {
        let _ = handle.join();
    }
    if let Some(handle) = fifo_reader.take() {
        let _ = handle.join();
    }

    if let Some(requested) = restart_mode {
        Ok(NetworkSessionExit::Restart(requested))
    } else if let Some(err) = exit_error {
        Err(err)
    } else {
        Ok(NetworkSessionExit::Shutdown)
    }
}

fn cleanup_startup_failure(
    shared: &Arc<FrameStore>,
    http_server: &mut Option<JoinHandle<io::Result<()>>>,
    fifo_reader: &mut Option<JoinHandle<io::Result<()>>>,
) {
    STOP_REQUESTED.store(true, Ordering::SeqCst);
    shared.notify_shutdown();
    if let Some(handle) = http_server.take() {
        let _ = handle.join();
    }
    if let Some(handle) = fifo_reader.take() {
        let _ = handle.join();
    }
}

fn prepare_network_fifos(config: &NetworkRunConfig) -> io::Result<()> {
    prepare_parent_dir(&config.fifo)?;
    if config.codec == NetworkCodec::H264 {
        prepare_parent_dir(&config.isp_nv12_fifo)?;
    }
    prepare_parent_dir(&config.isp_control_fifo)?;
    prepare_fifo(&config.fifo)?;
    if config.codec == NetworkCodec::H264 {
        prepare_fifo(&config.isp_nv12_fifo)?;
    }
    prepare_fifo(&config.isp_control_fifo)
}

fn network_stream_mode(config: &NetworkRunConfig, raw_mode: NativeMode) -> io::Result<NativeMode> {
    match (config.out_width, config.out_height) {
        (Some(width), Some(height)) => {
            validate_network_output_size(width, height)?;
            let mut stream_mode = raw_mode;
            stream_mode.width = width;
            stream_mode.height = height;
            Ok(stream_mode)
        }
        (None, None) => Ok(raw_mode),
        _ => Err(invalid_input(
            "network output size requires both width and height; use --out-size WxH",
        )),
    }
}

fn validate_network_output_size(width: u32, height: u32) -> io::Result<()> {
    if width == 0 || height == 0 {
        return Err(invalid_input(
            "network output width/height must be non-zero",
        ));
    }
    if width % 2 != 0 || height % 2 != 0 {
        return Err(invalid_input(
            "network output width/height must be even for NV12/MJPEG/H.264",
        ));
    }
    Ok(())
}

fn build_network_isp_profile(
    config: &NetworkRunConfig,
    raw_mode: NativeMode,
    stream_mode: NativeMode,
    raw_node: PathBuf,
    ctrl_node: Option<PathBuf>,
) -> UvcIspProfile {
    let fps_cap = config
        .fps_cap
        .unwrap_or(raw_mode.fps_cap)
        .min(raw_mode.fps_cap)
        .max(1);
    UvcIspProfile {
        raw_node,
        ctrl_node,
        isp_bin: config.isp_bin.clone(),
        fifo: match config.codec {
            NetworkCodec::Mjpeg => config.fifo.clone(),
            NetworkCodec::H264 => config.isp_nv12_fifo.clone(),
        },
        control_fifo: Some(config.isp_control_fifo.clone()),
        out_width: stream_mode.width,
        out_height: stream_mode.height,
        fps_cap,
        gamma: config.gamma,
        tone_highlight_knee: config.tone_highlight_knee,
        tone_highlight_max: config.tone_highlight_max,
        max_soft_gain: config.max_soft_gain,
        auto_exposure: config.auto_exposure,
        ae_target: config.ae_target,
        ae_clip_target: config.ae_clip_target,
        ae_clip_weight: config.ae_clip_weight,
        max_digital_gain: config.max_digital_gain,
        max_frame_bytes: config.codec.max_frame_bytes(stream_mode),
        mjpeg_quality: config.mjpeg_quality,
        mjpeg_sharpen: config.mjpeg_sharpen,
        mjpeg_smooth: config.mjpeg_smooth,
        mjpeg_area_scale: config.mjpeg_area_scale,
        mjpeg_subsampling: config.mjpeg_subsampling,
        mjpeg_scale_mode: config.mjpeg_scale_mode.clone(),
        mjpeg_fast_threads: config.mjpeg_fast_threads,
        cpu_affinity: config.cpu_affinity.clone(),
    }
}

fn route_for_mode(
    config: &NetworkRunConfig,
    mode: NativeMode,
) -> io::Result<(PathBuf, Option<PathBuf>)> {
    let mut route = config.route.clone();
    route.width = mode.width;
    route.height = mode.height;
    route.raw_node = Some(config.raw_node.clone());
    route.controls = config.controls.clone();

    if config.setup_route {
        let report = crate::route::setup_lmi_ov13b10_route(&route)?;
        println!(
            "[netcam] route_ready frame={} mode={} raw_format={} {}x{} bytesperline={} sizeimage={}",
            mode.frame_index,
            mode.mode_index,
            report.format.fourcc,
            report.format.width,
            report.format.height,
            report.format.bytesperline,
            report.format.sizeimage
        );
        Ok((
            report.raw_node,
            config
                .ctrl_node
                .clone()
                .or_else(|| Some(report.control_subdev)),
        ))
    } else {
        println!(
            "[netcam] setup_route=skipped for native frame {}",
            mode.frame_index
        );
        Ok((config.raw_node.clone(), config.ctrl_node.clone()))
    }
}

struct EncoderCommand {
    program: PathBuf,
    args: Vec<String>,
}

impl EncoderCommand {
    fn for_mode(config: &NetworkRunConfig, mode: NativeMode) -> Self {
        let bitrate = config.h264_bitrate.unwrap_or_else(|| mode.h264_bitrate());
        let peak_bitrate = config
            .h264_peak_bitrate
            .unwrap_or_else(|| mode.h264_peak_bitrate());
        let mut args = vec![
            "--device".to_string(),
            config.venus_node.display().to_string(),
            "--input-fifo".to_string(),
            config.isp_nv12_fifo.display().to_string(),
            "--output-fifo".to_string(),
            config.fifo.display().to_string(),
            "--width".to_string(),
            mode.width.to_string(),
            "--height".to_string(),
            mode.height.to_string(),
            "--fps".to_string(),
            mode.fps_cap.to_string(),
            "--bitrate".to_string(),
            bitrate.to_string(),
            "--peak-bitrate".to_string(),
            peak_bitrate.to_string(),
            "--bitrate-mode".to_string(),
            config.h264_bitrate_mode.clone(),
            "--gop".to_string(),
            config.h264_gop.to_string(),
            "--profile".to_string(),
            config.h264_profile.clone(),
            "--level".to_string(),
            config.h264_level.clone(),
            "--max-record".to_string(),
            mode.h264_max_record_bytes().to_string(),
            "--output-buffers".to_string(),
            config.h264_output_buffers.to_string(),
            "--capture-buffers".to_string(),
            config.h264_capture_buffers.to_string(),
            "--fifo-write-timeout-ms".to_string(),
            "2000".to_string(),
        ];
        if let Some(frame_rc) = config.h264_frame_rc {
            args.push("--frame-rc".to_string());
            args.push(if frame_rc { "1" } else { "0" }.to_string());
        }
        if let Some(max_qp) = config.h264_max_qp {
            args.push("--h264-max-qp".to_string());
            args.push(max_qp.to_string());
            args.push("--h264-i-max-qp".to_string());
            args.push(max_qp.to_string());
            args.push("--h264-p-max-qp".to_string());
            args.push(max_qp.to_string());
            args.push("--h264-b-max-qp".to_string());
            args.push(max_qp.to_string());
        }
        Self {
            program: config.venus_bin.clone(),
            args,
        }
    }

    fn print_shell(&self) {
        println!(
            "{} {}",
            shell_quote(&self.program.display().to_string()),
            self.args
                .iter()
                .map(|arg| shell_quote(arg))
                .collect::<Vec<_>>()
                .join(" ")
        );
    }

    fn spawn(&self) -> io::Result<EncoderSupervisor> {
        let child = Command::new(&self.program)
            .args(&self.args)
            .stdin(Stdio::null())
            .stdout(Stdio::inherit())
            .stderr(Stdio::inherit())
            .spawn()?;
        Ok(EncoderSupervisor { child })
    }
}

struct EncoderSupervisor {
    child: Child,
}

impl EncoderSupervisor {
    fn id(&self) -> u32 {
        self.child.id()
    }

    fn try_wait(&mut self) -> io::Result<Option<ExitStatus>> {
        self.child.try_wait()
    }

    fn terminate(&mut self) -> io::Result<()> {
        terminate_child(&mut self.child)
    }
}

impl Drop for EncoderSupervisor {
    fn drop(&mut self) {
        let _ = self.terminate();
    }
}

struct PipelineExit {
    component: &'static str,
    status: ExitStatus,
}

struct PipelineSupervisor {
    encoder: Option<EncoderSupervisor>,
    isp: IspSupervisor,
    started_at: Instant,
}

impl PipelineSupervisor {
    fn mjpeg(isp: IspSupervisor) -> Self {
        Self {
            encoder: None,
            isp,
            started_at: Instant::now(),
        }
    }

    fn h264(encoder: EncoderSupervisor, isp: IspSupervisor) -> Self {
        Self {
            encoder: Some(encoder),
            isp,
            started_at: Instant::now(),
        }
    }

    fn isp_id(&self) -> u32 {
        self.isp.id()
    }

    fn encoder_id(&self) -> Option<u32> {
        self.encoder.as_ref().map(EncoderSupervisor::id)
    }

    fn uptime(&self) -> Duration {
        self.started_at.elapsed()
    }

    fn try_wait(&mut self) -> io::Result<Option<PipelineExit>> {
        if let Some(encoder) = self.encoder.as_mut() {
            if let Some(status) = encoder.try_wait()? {
                return Ok(Some(PipelineExit {
                    component: "lmi-venus-enc",
                    status,
                }));
            }
        }
        if let Some(status) = self.isp.try_wait()? {
            return Ok(Some(PipelineExit {
                component: "lmi-isp",
                status,
            }));
        }
        Ok(None)
    }

    fn terminate(&mut self) -> io::Result<()> {
        let mut first_err = None;
        if let Err(err) = self.isp.terminate() {
            first_err = Some(err);
        }
        if let Some(encoder) = self.encoder.as_mut() {
            if let Err(err) = encoder.terminate() {
                if first_err.is_none() {
                    first_err = Some(err);
                }
            }
        }
        if let Some(err) = first_err {
            Err(err)
        } else {
            Ok(())
        }
    }
}

impl Drop for PipelineSupervisor {
    fn drop(&mut self) {
        let _ = self.terminate();
    }
}

#[derive(Clone)]
struct FrameSnapshot {
    sequence: u32,
    data: Arc<Vec<u8>>,
    received_at: Instant,
    sync: bool,
}

#[derive(Default)]
struct FrameStore {
    inner: Mutex<FrameStoreInner>,
    changed: Condvar,
}

#[derive(Default)]
struct FrameStoreInner {
    frame: Option<FrameSnapshot>,
    h264_sps: Option<Vec<u8>>,
    h264_pps: Option<Vec<u8>>,
    clients: u32,
    stream_clients: u32,
    frames_received: u64,
    bad_records: u64,
    reader_alive: bool,
    startup_phase: String,
    last_error: Option<String>,
}

impl FrameStore {
    fn accept_frame(&self, sequence: u32, data: Vec<u8>, sync: bool) {
        let mut inner = self.inner.lock().expect("frame store poisoned");
        let snapshot = FrameSnapshot {
            sequence,
            data: Arc::new(data),
            received_at: Instant::now(),
            sync,
        };
        inner.frame = Some(snapshot);
        inner.frames_received += 1;
        self.changed.notify_all();
    }

    fn set_h264_params(&self, sps: Option<&[u8]>, pps: Option<&[u8]>) {
        let mut inner = self.inner.lock().expect("frame store poisoned");
        let mut changed = false;
        if let Some(sps) = sps {
            if inner.h264_sps.as_deref() != Some(sps) {
                inner.h264_sps = Some(sps.to_vec());
                changed = true;
            }
        }
        if let Some(pps) = pps {
            if inner.h264_pps.as_deref() != Some(pps) {
                inner.h264_pps = Some(pps.to_vec());
                changed = true;
            }
        }
        if changed {
            self.changed.notify_all();
        }
    }

    fn set_reader_alive(&self, alive: bool) {
        let mut inner = self.inner.lock().expect("frame store poisoned");
        inner.reader_alive = alive;
        self.changed.notify_all();
    }

    fn set_startup_phase(&self, phase: &'static str) {
        let mut inner = self.inner.lock().expect("frame store poisoned");
        if inner.startup_phase != phase {
            println!("[netcam] startup_phase={phase}");
            inner.startup_phase = phase.to_string();
        }
        self.changed.notify_all();
    }

    fn set_error(&self, message: impl Into<String>) {
        let message = message.into();
        let mut inner = self.inner.lock().expect("frame store poisoned");
        inner.bad_records += 1;
        if inner.last_error.as_deref() != Some(message.as_str()) {
            println!("[netcam] runtime warning: {message}");
        }
        inner.last_error = Some(message);
    }

    fn client_connected(&self, stream: bool) {
        let mut inner = self.inner.lock().expect("frame store poisoned");
        inner.clients = inner.clients.saturating_add(1);
        if stream {
            inner.stream_clients = inner.stream_clients.saturating_add(1);
        }
    }

    fn client_disconnected(&self, stream: bool) {
        let mut inner = self.inner.lock().expect("frame store poisoned");
        inner.clients = inner.clients.saturating_sub(1);
        if stream {
            inner.stream_clients = inner.stream_clients.saturating_sub(1);
        }
    }

    fn frame_liveness(&self, stale_after: Duration) -> FrameLiveness {
        let inner = self.inner.lock().expect("frame store poisoned");
        let Some(frame) = &inner.frame else {
            return FrameLiveness::Empty;
        };
        let age = frame.received_at.elapsed();
        if age >= stale_after {
            FrameLiveness::Stale {
                sequence: frame.sequence,
                age,
            }
        } else {
            FrameLiveness::Fresh
        }
    }

    fn wait_for_frame(
        &self,
        after_sequence: Option<u32>,
        timeout: Duration,
    ) -> Option<FrameSnapshot> {
        self.wait_for_frame_matching(after_sequence, timeout, |_| true)
    }

    fn latest_sequence(&self) -> Option<u32> {
        self.inner
            .lock()
            .expect("frame store poisoned")
            .frame
            .as_ref()
            .map(|frame| frame.sequence)
    }

    fn wait_for_next_h264_sync(
        &self,
        after_sequence: Option<u32>,
        timeout: Duration,
    ) -> Option<FrameSnapshot> {
        self.wait_for_frame_matching(after_sequence, timeout, |frame| frame.sync)
    }

    fn wait_for_frame_matching<F>(
        &self,
        after_sequence: Option<u32>,
        timeout: Duration,
        accept: F,
    ) -> Option<FrameSnapshot>
    where
        F: Fn(&FrameSnapshot) -> bool,
    {
        let deadline = Instant::now() + timeout;
        let mut inner = self.inner.lock().expect("frame store poisoned");
        loop {
            if let Some(frame) = &inner.frame {
                if after_sequence != Some(frame.sequence) && accept(frame) {
                    return Some(frame.clone());
                }
            }
            if STOP_REQUESTED.load(Ordering::SeqCst) {
                return None;
            }
            let now = Instant::now();
            let remaining = deadline.checked_duration_since(now)?;
            let (guard, _) = self
                .changed
                .wait_timeout(inner, remaining)
                .expect("frame store poisoned");
            inner = guard;
        }
    }

    fn h264_params(&self) -> (Option<Vec<u8>>, Option<Vec<u8>>) {
        let inner = self.inner.lock().expect("frame store poisoned");
        (inner.h264_sps.clone(), inner.h264_pps.clone())
    }

    fn wait_for_h264_params(&self, timeout: Duration) -> (Option<Vec<u8>>, Option<Vec<u8>>) {
        let Some(deadline) = Instant::now().checked_add(timeout) else {
            return self.h264_params();
        };
        let mut inner = self.inner.lock().expect("frame store poisoned");
        loop {
            if inner.h264_sps.is_some() && inner.h264_pps.is_some() {
                return (inner.h264_sps.clone(), inner.h264_pps.clone());
            }
            if STOP_REQUESTED.load(Ordering::SeqCst) {
                return (inner.h264_sps.clone(), inner.h264_pps.clone());
            }
            let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
                return (inner.h264_sps.clone(), inner.h264_pps.clone());
            };
            let (guard, _) = self
                .changed
                .wait_timeout(inner, remaining)
                .expect("frame store poisoned");
            inner = guard;
        }
    }

    fn status_json(&self, listen: &str, mode: NativeMode, codec: NetworkCodec) -> String {
        let inner = self.inner.lock().expect("frame store poisoned");
        let (have_frame, sequence, frame_bytes, frame_age_ms, sync_frame) = match &inner.frame {
            Some(frame) => (
                true,
                frame.sequence,
                frame.data.len() as u64,
                Some(frame.received_at.elapsed().as_millis() as u64),
                frame.sync,
            ),
            None => (false, 0, 0, None, false),
        };
        let age = frame_age_ms
            .map(|value| value.to_string())
            .unwrap_or_else(|| "null".to_string());
        let last_error = inner
            .last_error
            .as_deref()
            .map(json_string)
            .unwrap_or_else(|| "null".to_string());
        let h264_sps_bytes = inner.h264_sps.as_ref().map(|sps| sps.len()).unwrap_or(0);
        let h264_pps_bytes = inner.h264_pps.as_ref().map(|pps| pps.len()).unwrap_or(0);
        format!(
            concat!(
                "{{\n",
                "  \"output\": \"network\",\n",
                "  \"profile\": \"native-modes\",\n",
                "  \"codec\": {},\n",
                "  \"listen\": {},\n",
                "  \"frame_index\": {},\n",
                "  \"mode_index\": {},\n",
                "  \"width\": {},\n",
                "  \"height\": {},\n",
                "  \"fps_cap\": {},\n",
                "  \"have_frame\": {},\n",
                "  \"sync_frame\": {},\n",
                "  \"sequence\": {},\n",
                "  \"frame_bytes\": {},\n",
                "  \"frame_age_ms\": {},\n",
                "  \"h264_sps_bytes\": {},\n",
                "  \"h264_pps_bytes\": {},\n",
                "  \"frames_received\": {},\n",
                "  \"clients\": {},\n",
                "  \"stream_clients\": {},\n",
                "  \"reader_alive\": {},\n",
                "  \"startup_phase\": {},\n",
                "  \"bad_records\": {},\n",
                "  \"last_error\": {}\n",
                "}}\n"
            ),
            json_string(codec.name()),
            json_string(listen),
            mode.frame_index,
            mode.mode_index,
            mode.width,
            mode.height,
            mode.fps_cap,
            json_bool(have_frame),
            json_bool(sync_frame),
            sequence,
            frame_bytes,
            age,
            h264_sps_bytes,
            h264_pps_bytes,
            inner.frames_received,
            inner.clients,
            inner.stream_clients,
            json_bool(inner.reader_alive),
            json_string(&inner.startup_phase),
            inner.bad_records,
            last_error
        )
    }

    fn notify_shutdown(&self) {
        self.changed.notify_all();
    }
}

struct ClientGuard {
    shared: Arc<FrameStore>,
    stream: bool,
}

impl ClientGuard {
    fn new(shared: Arc<FrameStore>, stream: bool) -> Self {
        shared.client_connected(stream);
        Self { shared, stream }
    }
}

impl Drop for ClientGuard {
    fn drop(&mut self) {
        self.shared.client_disconnected(self.stream);
    }
}

fn spawn_fifo_reader(
    path: PathBuf,
    max_frame_bytes: u32,
    codec: NetworkCodec,
    shared: Arc<FrameStore>,
) -> io::Result<JoinHandle<io::Result<()>>> {
    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .custom_flags(O_NONBLOCK)
        .open(&path)?;
    println!("[netcam] FIFO reader opened {}", path.display());
    Ok(thread::spawn(move || {
        fifo_reader_loop(file, max_frame_bytes as usize, codec, shared)
    }))
}

fn fifo_reader_loop(
    mut file: File,
    max_frame_bytes: usize,
    codec: NetworkCodec,
    shared: Arc<FrameStore>,
) -> io::Result<()> {
    let mut parser = RecordParser::new(max_frame_bytes, codec, shared.clone());
    let mut buf = [0u8; FIFO_READ_CHUNK];
    shared.set_reader_alive(true);
    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        match file.read(&mut buf) {
            Ok(0) => thread::sleep(Duration::from_millis(10)),
            Ok(n) => parser.feed(&buf[..n]),
            Err(err) if err.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(10));
            }
            Err(err) => {
                shared.set_error(format!("read {} failed: {}", parser.name(), err));
                thread::sleep(Duration::from_millis(80));
            }
        }
    }
    shared.set_reader_alive(false);
    Ok(())
}

struct RecordParser {
    pending: Vec<u8>,
    max_frame_bytes: usize,
    codec: NetworkCodec,
    shared: Arc<FrameStore>,
    h264_sps: Option<Vec<u8>>,
    h264_pps: Option<Vec<u8>>,
    h264_have_sync: bool,
}

impl RecordParser {
    fn new(max_frame_bytes: usize, codec: NetworkCodec, shared: Arc<FrameStore>) -> Self {
        Self {
            pending: Vec::with_capacity(FIFO_READ_CHUNK * 2),
            max_frame_bytes,
            codec,
            shared,
            h264_sps: None,
            h264_pps: None,
            h264_have_sync: false,
        }
    }

    fn name(&self) -> &'static str {
        match self.codec {
            NetworkCodec::Mjpeg => "LMVC MJPEG FIFO",
            NetworkCodec::H264 => "LMVC H.264 FIFO",
        }
    }

    fn feed(&mut self, data: &[u8]) {
        self.pending.extend_from_slice(data);
        loop {
            if self.pending.len() < LMI_UVC_RECORD_BASE_HEADER_SIZE {
                return;
            }
            let mut header =
                match parse_record_header(&self.pending[..LMI_UVC_RECORD_BASE_HEADER_SIZE]) {
                    Some(header) => header,
                    None => {
                        self.resync("header magic/version");
                        continue;
                    }
                };
            if header.header_size < LMI_UVC_RECORD_BASE_HEADER_SIZE
                || header.header_size > LMI_UVC_RECORD_MAX_HEADER_SIZE
            {
                self.resync("header size");
                continue;
            }
            if header.payload_size == 0 || header.payload_size > self.max_frame_bytes {
                self.resync(&format!(
                    "payload size {} > max {}",
                    header.payload_size, self.max_frame_bytes
                ));
                continue;
            }
            if self.pending.len() < header.header_size {
                return;
            }
            header.flags = parse_record_flags(&self.pending[..header.header_size], header.version);
            let record_size = header.header_size + header.payload_size;
            if self.pending.len() < record_size {
                return;
            }
            let payload = self.pending[header.header_size..record_size].to_vec();
            self.pending.drain(..record_size);
            match self.codec {
                NetworkCodec::Mjpeg => self.accept_mjpeg_record(header, payload),
                NetworkCodec::H264 => self.accept_h264_record(header, payload),
            }
        }
    }

    fn accept_mjpeg_record(&mut self, header: RecordHeader, payload: Vec<u8>) {
        if !looks_like_jpeg(&payload) {
            self.shared.set_error(format!(
                "sequence {} is not a complete JPEG payload ({} bytes)",
                header.sequence,
                payload.len()
            ));
            return;
        }
        self.shared.accept_frame(header.sequence, payload, false);
    }

    fn accept_h264_record(&mut self, header: RecordHeader, payload: Vec<u8>) {
        let ranges = h264_nal_ranges(&payload);
        if ranges.is_empty() {
            self.shared.set_error(format!(
                "sequence {} has no Annex-B H.264 NAL units ({} bytes)",
                header.sequence,
                payload.len()
            ));
            return;
        }

        let mut has_vcl = false;
        let mut has_idr = false;
        let mut has_sps = false;
        let mut has_pps = false;
        for (start, end) in ranges.iter().copied() {
            let nal_type = payload[start] & 0x1f;
            if matches!(nal_type, 1..=5) {
                has_vcl = true;
            }
            match nal_type {
                5 => has_idr = true,
                7 => {
                    has_sps = true;
                    self.h264_sps = Some(payload[start..end].to_vec());
                }
                8 => {
                    has_pps = true;
                    self.h264_pps = Some(payload[start..end].to_vec());
                }
                _ => {}
            }
        }
        self.shared
            .set_h264_params(self.h264_sps.as_deref(), self.h264_pps.as_deref());

        if !has_vcl {
            return;
        }

        let sync = has_idr;
        let mut access_unit = payload;
        if (!has_sps || !has_pps) && sync {
            let (Some(sps), Some(pps)) = (self.h264_sps.as_deref(), self.h264_pps.as_deref())
            else {
                self.shared.set_error(format!(
                    "sequence {} is sync H.264 but SPS/PPS are not cached yet",
                    header.sequence
                ));
                return;
            };
            let mut prefixed = Vec::with_capacity(sps.len() + pps.len() + access_unit.len() + 8);
            h264_append_start_code_nal(&mut prefixed, sps);
            h264_append_start_code_nal(&mut prefixed, pps);
            prefixed.extend_from_slice(&access_unit);
            access_unit = prefixed;
        }

        if sync {
            self.h264_have_sync = true;
        } else if !self.h264_have_sync {
            self.shared.set_error(format!(
                "sequence {} is H.264 delta frame before first sync frame; dropping",
                header.sequence
            ));
            return;
        }

        self.shared.accept_frame(header.sequence, access_unit, sync);
    }

    fn resync(&mut self, reason: &str) {
        self.shared.set_error(reason.to_string());
        if let Some(pos) = find_magic(&self.pending[1..]) {
            self.pending.drain(..pos + 1);
        } else if self.pending.len() > 3 {
            let keep = self.pending.split_off(self.pending.len() - 3);
            self.pending = keep;
        } else {
            self.pending.clear();
        }
    }
}

#[derive(Clone, Copy)]
struct RecordHeader {
    version: u16,
    header_size: usize,
    payload_size: usize,
    sequence: u32,
    flags: u32,
}

fn parse_record_header(bytes: &[u8]) -> Option<RecordHeader> {
    if bytes.len() < LMI_UVC_RECORD_BASE_HEADER_SIZE {
        return None;
    }
    let magic = u32::from_le_bytes(bytes[0..4].try_into().ok()?);
    let version = u16::from_le_bytes(bytes[4..6].try_into().ok()?);
    let header_size = u16::from_le_bytes(bytes[6..8].try_into().ok()?);
    let payload_size = u32::from_le_bytes(bytes[8..12].try_into().ok()?);
    let sequence = u32::from_le_bytes(bytes[12..16].try_into().ok()?);
    if magic != LMI_UVC_RECORD_MAGIC
        || !matches!(
            version,
            LMI_UVC_RECORD_VERSION_V1 | LMI_UVC_RECORD_VERSION_V2
        )
    {
        return None;
    }
    Some(RecordHeader {
        version,
        header_size: header_size as usize,
        payload_size: payload_size as usize,
        sequence,
        flags: 0,
    })
}

fn parse_record_flags(bytes: &[u8], version: u16) -> u32 {
    if version >= LMI_UVC_RECORD_VERSION_V2 && bytes.len() >= 20 {
        u32::from_le_bytes(bytes[16..20].try_into().expect("LMVC v2 flags len"))
    } else {
        0
    }
}

fn find_magic(bytes: &[u8]) -> Option<usize> {
    bytes.windows(4).position(|chunk| {
        u32::from_le_bytes(chunk.try_into().expect("window len")) == LMI_UVC_RECORD_MAGIC
    })
}

fn looks_like_jpeg(payload: &[u8]) -> bool {
    payload.len() >= 4
        && payload[0] == 0xff
        && payload[1] == 0xd8
        && payload[payload.len() - 2] == 0xff
        && payload[payload.len() - 1] == 0xd9
}

fn h264_nal_ranges(payload: &[u8]) -> Vec<(usize, usize)> {
    let mut ranges = Vec::new();
    let Some((mut start, mut prefix)) = h264_find_start_code(payload, 0) else {
        return ranges;
    };
    loop {
        let nal_start = start + prefix;
        let next = h264_find_start_code(payload, nal_start);
        let nal_end = next
            .map(|(next_start, _)| next_start)
            .unwrap_or(payload.len());
        if nal_end > nal_start {
            ranges.push((nal_start, nal_end));
        }
        let Some((next_start, next_prefix)) = next else {
            break;
        };
        start = next_start;
        prefix = next_prefix;
    }
    ranges
}

fn h264_find_start_code(payload: &[u8], mut off: usize) -> Option<(usize, usize)> {
    while off + 3 <= payload.len() {
        if off + 4 <= payload.len()
            && payload[off] == 0
            && payload[off + 1] == 0
            && payload[off + 2] == 0
            && payload[off + 3] == 1
        {
            return Some((off, 4));
        }
        if payload[off] == 0 && payload[off + 1] == 0 && payload[off + 2] == 1 {
            return Some((off, 3));
        }
        off += 1;
    }
    None
}

fn h264_append_start_code_nal(out: &mut Vec<u8>, nal: &[u8]) {
    out.extend_from_slice(&[0, 0, 0, 1]);
    out.extend_from_slice(nal);
}

fn spawn_http_server(
    listen: String,
    mode: NativeMode,
    shared: Arc<FrameStore>,
    service: NetworkServiceInfo,
) -> io::Result<JoinHandle<io::Result<()>>> {
    let listener = TcpListener::bind(&listen)?;
    listener.set_nonblocking(true)?;
    println!(
        "[netcam] HTTP server listening on {}",
        listener.local_addr()?
    );
    Ok(thread::spawn(move || {
        http_server_loop(listener, mode, shared, service)
    }))
}

fn http_server_loop(
    listener: TcpListener,
    mode: NativeMode,
    shared: Arc<FrameStore>,
    service: NetworkServiceInfo,
) -> io::Result<()> {
    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        match listener.accept() {
            Ok((stream, peer)) => {
                let shared = shared.clone();
                let service = service.clone();
                thread::spawn(move || {
                    if let Err(err) = handle_client(stream, peer, mode, shared, service) {
                        println!("[netcam] HTTP client {peer} disconnected: {err}");
                    }
                });
            }
            Err(err) if err.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(30));
            }
            Err(err) => return Err(err),
        }
    }
    Ok(())
}

struct HttpRequest {
    method: String,
    path: String,
    body: Vec<u8>,
    host: Option<String>,
    content_type: Option<String>,
    soap_action: Option<String>,
    authorization: Option<String>,
}

fn handle_client(
    mut stream: TcpStream,
    peer: SocketAddr,
    mode: NativeMode,
    shared: Arc<FrameStore>,
    service: NetworkServiceInfo,
) -> io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(2)))?;
    stream.set_write_timeout(Some(Duration::from_secs(3)))?;
    let request = read_http_request(&mut stream)?;

    if request.method == "POST"
        && matches!(
            request.path.as_str(),
            "/onvif/device_service" | "/onvif/media_service" | "/onvif/imaging_service"
        )
    {
        let _guard = ClientGuard::new(shared, false);
        return write_onvif_response(&mut stream, &request, peer, mode, &service);
    }

    if request.method != "GET" {
        return write_response(
            &mut stream,
            "405 Method Not Allowed",
            "text/plain; charset=utf-8",
            b"Only GET and ONVIF POST are supported.\n",
        );
    }

    match request.path.as_str() {
        "/" => {
            let _guard = ClientGuard::new(shared, false);
            let host = request.host.as_deref().unwrap_or(&service.http_listen);
            let stream_uri = service.stream_uri(host);
            let snapshot_uri = service
                .snapshot_uri(host)
                .unwrap_or_else(|| "not available in H.264 mode".to_string());
            let onvif_state = if service.onvif { "enabled" } else { "disabled" };
            let body = format!(
                concat!(
                    "lmi network camera\n\n",
                    "RAW invariant: /dev/video3 stays truthful pgAA; this is userspace network camera support.\n",
                    "codec: {}\n",
                    "mode: native frame {} -> OV13B10 mode {} {}x{} @ {:.3}fps\n\n",
                    "Endpoints:\n",
                    "  /status\n",
                    "  /snapshot.jpg  (MJPEG mode only)\n",
                    "  /stream.mjpg   (MJPEG mode only)\n",
                    "  /onvif/device_service  (SOAP POST)\n",
                    "  /onvif/media_service   (SOAP POST)\n",
                    "  /onvif/imaging_service (SOAP POST, exposure window -> meter ROI)\n\n",
                    "Stream URI: {}\n",
                    "Snapshot URI: {}\n",
                    "WS-Discovery: {}\n\n",
                    "HTML preview:\n",
                    "  <img src=\"/stream.mjpg\" />\n"
                ),
                service.codec.name(),
                mode.frame_index,
                mode.mode_index,
                mode.width,
                mode.height,
                mode.nominal_fps(),
                stream_uri,
                snapshot_uri,
                onvif_state
            );
            write_response(
                &mut stream,
                "200 OK",
                "text/plain; charset=utf-8",
                body.as_bytes(),
            )
        }
        "/status" => {
            let _guard = ClientGuard::new(shared.clone(), false);
            let body = shared.status_json(&service.http_listen, mode, service.codec);
            write_response(
                &mut stream,
                "200 OK",
                "application/json; charset=utf-8",
                body.as_bytes(),
            )
        }
        "/snapshot.jpg" if service.codec == NetworkCodec::Mjpeg => {
            let _guard = ClientGuard::new(shared.clone(), false);
            match shared.wait_for_frame(None, Duration::from_secs(5)) {
                Some(frame) => write_jpeg_response(&mut stream, &frame),
                None => write_response(
                    &mut stream,
                    "503 Service Unavailable",
                    "text/plain; charset=utf-8",
                    b"No camera frame is available yet.\n",
                ),
            }
        }
        "/snapshot.jpg" => write_response(
            &mut stream,
            "404 Not Found",
            "text/plain; charset=utf-8",
            b"JPEG snapshots are not available while --codec h264 is active; use the RTSP H.264 stream.\n",
        ),
        "/stream.mjpg" | "/stream.mjpeg" if service.codec == NetworkCodec::Mjpeg => {
            let _guard = ClientGuard::new(shared.clone(), true);
            write_mjpeg_stream(&mut stream, shared)
        }
        "/stream.mjpg" | "/stream.mjpeg" => write_response(
            &mut stream,
            "404 Not Found",
            "text/plain; charset=utf-8",
            b"MJPEG streaming is not available while --codec h264 is active; use the RTSP H.264 stream.\n",
        ),
        "/onvif/device_service" | "/onvif/media_service" | "/onvif/imaging_service" => {
            write_response(
                &mut stream,
                "200 OK",
                "text/plain; charset=utf-8",
                b"ONVIF service is available via SOAP POST.\n",
            )
        }
        _ => write_response(
            &mut stream,
            "404 Not Found",
            "text/plain; charset=utf-8",
            b"Unknown endpoint. Try /status, /snapshot.jpg, /stream.mjpg, /onvif/device_service, or /onvif/imaging_service.\n",
        ),
    }
}

fn read_http_request(stream: &mut TcpStream) -> io::Result<HttpRequest> {
    let mut request = Vec::new();
    let mut buf = [0u8; 1024];
    let header_end = loop {
        let n = stream.read(&mut buf)?;
        if n == 0 {
            break find_header_end(&request).unwrap_or(request.len());
        }
        request.extend_from_slice(&buf[..n]);
        if let Some(end) = find_header_end(&request) {
            break end;
        }
        if request.len() >= 65536 {
            return Err(invalid_input("HTTP request headers are too large"));
        }
    };

    let header = String::from_utf8_lossy(&request[..header_end]).to_string();
    let mut lines = header.lines();
    let request_line = lines.next().unwrap_or_default().trim();
    if request_line.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "empty HTTP request",
        ));
    }
    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or_default().to_string();
    let raw_path = parts.next().unwrap_or("/");
    let path = raw_path
        .split_once('?')
        .map(|(path, _)| path)
        .unwrap_or(raw_path)
        .to_string();
    let content_length = header_value(&header, "content-length")
        .and_then(|value| value.parse::<usize>().ok())
        .unwrap_or(0);
    let host = header_value(&header, "host");
    let content_type = header_value(&header, "content-type");
    let soap_action = header_value(&header, "soapaction");
    let authorization = header_value(&header, "authorization");

    let body_start = if request.len() >= header_end + 4 {
        header_end + 4
    } else if request.len() >= header_end + 2 {
        header_end + 2
    } else {
        request.len()
    };
    let mut body = request[body_start..].to_vec();
    while body.len() < content_length {
        let n = stream.read(&mut buf)?;
        if n == 0 {
            break;
        }
        body.extend_from_slice(&buf[..n]);
        if body.len() > 1048576 {
            return Err(invalid_input("HTTP request body is too large"));
        }
    }
    body.truncate(content_length);

    Ok(HttpRequest {
        method,
        path,
        body,
        host,
        content_type,
        soap_action,
        authorization,
    })
}

fn find_header_end(bytes: &[u8]) -> Option<usize> {
    bytes
        .windows(4)
        .position(|window| window == b"\r\n\r\n")
        .or_else(|| bytes.windows(2).position(|window| window == b"\n\n"))
}

fn header_value(header: &str, name: &str) -> Option<String> {
    header.lines().skip(1).find_map(|line| {
        let (key, value) = line.split_once(':')?;
        if key.trim().eq_ignore_ascii_case(name) {
            Some(value.trim().to_string())
        } else {
            None
        }
    })
}

fn write_response(
    stream: &mut TcpStream,
    status: &str,
    content_type: &str,
    body: &[u8],
) -> io::Result<()> {
    write!(
        stream,
        "HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
        body.len()
    )?;
    stream.write_all(body)
}

fn write_jpeg_response(stream: &mut TcpStream, frame: &FrameSnapshot) -> io::Result<()> {
    write!(
        stream,
        "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: {}\r\nX-Sequence: {}\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
        frame.data.len(),
        frame.sequence
    )?;
    stream.write_all(frame.data.as_ref().as_slice())
}

fn write_mjpeg_stream(stream: &mut TcpStream, shared: Arc<FrameStore>) -> io::Result<()> {
    write!(
        stream,
        "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary={HTTP_BOUNDARY}\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n"
    )?;
    let mut last_sequence = None;
    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        let Some(frame) = shared.wait_for_frame(last_sequence, Duration::from_secs(5)) else {
            continue;
        };
        last_sequence = Some(frame.sequence);
        write!(
            stream,
            "--{HTTP_BOUNDARY}\r\nContent-Type: image/jpeg\r\nContent-Length: {}\r\nX-Sequence: {}\r\n\r\n",
            frame.data.len(),
            frame.sequence
        )?;
        stream.write_all(frame.data.as_ref().as_slice())?;
        stream.write_all(b"\r\n")?;
    }
    Ok(())
}

fn onvif_request_host(
    request: &HttpRequest,
    peer: SocketAddr,
    service: &NetworkServiceInfo,
) -> String {
    let port = listen_port(&service.http_listen).unwrap_or("8080");
    if let Some(host) = request
        .host
        .as_deref()
        .map(str::trim)
        .filter(|host| !host.is_empty())
    {
        let bare_host = strip_host_port(host).trim_matches(|ch| ch == '[' || ch == ']');
        if !host_is_unspecified_for_onvif(bare_host) {
            return host.to_string();
        }
    }

    local_ip_for_peer(peer)
        .map(|ip| format_ip_port(ip, port))
        .unwrap_or_else(|| service.http_listen.clone())
}

fn host_is_unspecified_for_onvif(host: &str) -> bool {
    matches!(
        host,
        "" | "0.0.0.0" | "::" | "localhost" | "127.0.0.1" | "::1"
    )
}

fn format_ip_port(ip: IpAddr, port: &str) -> String {
    match ip {
        IpAddr::V4(addr) => format_host_port(&addr.to_string(), port),
        IpAddr::V6(addr) => format_host_port(&addr.to_string(), port),
    }
}

fn write_onvif_response(
    stream: &mut TcpStream,
    request: &HttpRequest,
    peer: SocketAddr,
    mode: NativeMode,
    service: &NetworkServiceInfo,
) -> io::Result<()> {
    let body = String::from_utf8_lossy(&request.body);
    let host = onvif_request_host(request, peer, service);
    let device_uri = format!("http://{host}/onvif/device_service");
    let media_uri = format!("http://{host}/onvif/media_service");
    let stream_uri = service.stream_uri(&host);
    let snapshot_uri = service.snapshot_uri(&host);
    let action = onvif_request_action(request, &body);
    let uses_media_service = onvif_request_uses_media_service(request, &body);
    let uses_imaging_service = uses_imaging_service(request, &body);
    log_onvif_request(request, &host, action);
    let response_body = match action {
        Some("GetDeviceInformation") => onvif_get_device_information(service),
        Some("GetEndpointReference") => onvif_get_endpoint_reference(service),
        Some("GetServices") => onvif_get_services(&device_uri, &media_uri, service),
        Some("GetServiceCapabilities") if uses_media_service => {
            onvif_get_media_service_capabilities(service)
        }
        Some("GetServiceCapabilities") if uses_imaging_service => {
            onvif_get_imaging_service_capabilities()
        }
        Some("GetServiceCapabilities") => onvif_get_service_capabilities(),
        Some("GetCapabilities") => onvif_get_capabilities(&device_uri, &media_uri, service),
        Some("GetVideoEncoderConfigurationOptions") => {
            onvif_get_video_encoder_configuration_options(mode, service)
        }
        Some("GetVideoEncoderConfigurations") => {
            onvif_get_video_encoder_configurations(mode, service)
        }
        Some("GetVideoEncoderConfiguration") => {
            onvif_get_video_encoder_configuration(&body, mode, service)
        }
        Some("SetVideoEncoderConfiguration") => {
            onvif_set_video_encoder_configuration(&body, mode, service)
        }
        Some("GetCompatibleVideoEncoderConfigurations") => {
            onvif_get_compatible_video_encoder_configurations(mode, service)
        }
        Some("GetVideoSourceConfigurationOptions") => {
            onvif_get_video_source_configuration_options(mode, service)
        }
        Some("GetVideoSourceConfigurations") => {
            onvif_get_video_source_configurations(mode, service)
        }
        Some("GetVideoSourceConfiguration") => {
            onvif_get_video_source_configuration(&body, mode, service)
        }
        Some("SetVideoSourceConfiguration") => {
            onvif_set_video_source_configuration(&body, mode, service)
        }
        Some("GetCompatibleVideoSourceConfigurations") => {
            onvif_get_compatible_video_source_configurations(mode, service)
        }
        Some("GetGuaranteedNumberOfVideoEncoderInstances") => {
            onvif_get_guaranteed_number_of_video_encoder_instances(service)
        }
        Some("GetVideoSources") => onvif_get_video_sources(mode, service),
        Some("GetProfiles") => onvif_get_profiles(mode, service),
        Some("GetProfile") => onvif_get_profile(&body, mode, service),
        Some("GetStreamUri") | Some("GetUri") => onvif_get_stream_uri(&body, &stream_uri, service),
        Some("GetSnapshotUri") => match snapshot_uri.as_deref() {
            Some(snapshot_uri) => onvif_get_snapshot_uri(snapshot_uri),
            None => onvif_fault(
                "ter:ActionNotSupported",
                "SnapshotUri is not available in H.264 mode",
            ),
        },
        Some("GetScopes") => onvif_get_scopes(service),
        Some("GetHostname") => onvif_get_hostname(service),
        Some("GetNetworkProtocols") => onvif_get_network_protocols(service),
        Some("GetNetworkInterfaces") => onvif_get_network_interfaces(&host, peer, service),
        Some("GetDiscoveryMode") => onvif_get_discovery_mode(),
        Some("GetUsers") => onvif_get_users(service),
        Some("GetSystemDateAndTime") => onvif_get_system_date_and_time(),
        Some("GetImagingSettings") => onvif_get_imaging_settings(),
        Some("SetImagingSettings") => onvif_set_imaging_settings(&body, mode, service),
        Some("GetOptions") if uses_imaging_service => onvif_get_imaging_options(),
        _ => onvif_fault(
            "ter:ActionNotSupported",
            "Unsupported ONVIF request for lmi netcam",
        ),
    };
    let message_id = extract_xml_text(&body, "MessageID");
    let response_action_uri = action
        .map(|action| onvif_response_action_uri(action, uses_media_service, uses_imaging_service));
    let response_body = response_action_uri
        .as_deref()
        .map(|action_uri| {
            onvif_add_response_headers(
                &response_body,
                action_uri,
                message_id.as_deref(),
                &ws_message_uuid(service),
            )
        })
        .unwrap_or(response_body);
    let content_type = response_action_uri
        .as_deref()
        .map(|action_uri| format!("application/soap+xml; charset=utf-8; action=\"{action_uri}\""))
        .unwrap_or_else(|| "application/soap+xml; charset=utf-8".to_string());
    write_response(stream, "200 OK", &content_type, response_body.as_bytes())
}

const ONVIF_ACTIONS: [&str; 32] = [
    "GetDeviceInformation",
    "GetEndpointReference",
    "GetServices",
    "GetServiceCapabilities",
    "GetCapabilities",
    "GetVideoEncoderConfigurationOptions",
    "GetCompatibleVideoEncoderConfigurations",
    "GetVideoEncoderConfigurations",
    "GetVideoEncoderConfiguration",
    "SetVideoEncoderConfiguration",
    "GetVideoSourceConfigurationOptions",
    "GetCompatibleVideoSourceConfigurations",
    "GetVideoSourceConfigurations",
    "GetVideoSourceConfiguration",
    "SetVideoSourceConfiguration",
    "GetGuaranteedNumberOfVideoEncoderInstances",
    "GetVideoSources",
    "GetProfiles",
    "GetProfile",
    "GetStreamUri",
    "GetUri",
    "GetSnapshotUri",
    "GetScopes",
    "GetHostname",
    "GetNetworkProtocols",
    "GetNetworkInterfaces",
    "GetDiscoveryMode",
    "GetUsers",
    "GetSystemDateAndTime",
    "GetImagingSettings",
    "SetImagingSettings",
    "GetOptions",
];

fn onvif_request_action(request: &HttpRequest, body: &str) -> Option<&'static str> {
    if let Some(action) = request
        .soap_action
        .as_deref()
        .and_then(onvif_action_from_value)
    {
        return Some(action);
    }
    if let Some(action) = request
        .content_type
        .as_deref()
        .and_then(onvif_action_from_value)
    {
        return Some(action);
    }
    if let Some(action) = redacted_xml_element(&request.body, "Action")
        .as_deref()
        .and_then(onvif_action_from_value)
    {
        return Some(action);
    }
    onvif_body_action_name(body)
}

fn onvif_request_uses_media_service(request: &HttpRequest, body: &str) -> bool {
    request.path == "/onvif/media_service"
        || request
            .soap_action
            .as_deref()
            .is_some_and(onvif_value_uses_media_service)
        || request
            .content_type
            .as_deref()
            .is_some_and(onvif_value_uses_media_service)
        || redacted_xml_element(&request.body, "Action")
            .as_deref()
            .is_some_and(onvif_value_uses_media_service)
        || body.contains("<trt:GetServiceCapabilities")
        || body.contains(":GetProfiles")
        || body.contains(":GetStreamUri")
        || body.contains(":GetVideo")
}

fn uses_imaging_service(request: &HttpRequest, body: &str) -> bool {
    request.path == "/onvif/imaging_service"
        || request
            .soap_action
            .as_deref()
            .is_some_and(onvif_value_uses_imaging_service)
        || request
            .content_type
            .as_deref()
            .is_some_and(onvif_value_uses_imaging_service)
        || redacted_xml_element(&request.body, "Action")
            .as_deref()
            .is_some_and(onvif_value_uses_imaging_service)
        || body.contains("/ver20/imaging/wsdl")
        || body.contains("<timg:GetServiceCapabilities")
        || body.contains(":GetImagingSettings")
        || body.contains(":SetImagingSettings")
}

fn onvif_value_uses_media_service(value: &str) -> bool {
    value.contains("http://www.onvif.org/ver10/media/wsdl")
}

fn onvif_value_uses_imaging_service(value: &str) -> bool {
    value.contains("http://www.onvif.org/ver20/imaging/wsdl")
}

fn onvif_action_from_value(value: &str) -> Option<&'static str> {
    ONVIF_ACTIONS
        .iter()
        .find(|name| value.contains(**name))
        .copied()
}

fn onvif_body_action_name(body: &str) -> Option<&'static str> {
    ONVIF_ACTIONS
        .into_iter()
        .find(|name| find_xml_start_tag(body, name).is_some())
}

fn log_onvif_request(request: &HttpRequest, host: &str, action: Option<&str>) {
    println!(
        concat!(
            "[netcam] ONVIF method={} path={} action={} host={} content_type={} soap_action={} ",
            "wsa_action={} auth={} wsse={}",
        ),
        rtsp_log_value(&request.method),
        rtsp_log_value(&request.path),
        action.unwrap_or("unknown"),
        rtsp_log_value(host),
        request
            .content_type
            .as_deref()
            .map(rtsp_log_value)
            .unwrap_or_else(|| "none".to_string()),
        request
            .soap_action
            .as_deref()
            .map(redact_header_value)
            .unwrap_or_else(|| "none".to_string()),
        redacted_xml_element(&request.body, "Action").unwrap_or_else(|| "none".to_string()),
        request
            .authorization
            .as_deref()
            .map(redact_authorization)
            .unwrap_or_else(|| "none".to_string()),
        wsse_presence(&request.body)
    );
}

fn redact_header_value(value: &str) -> String {
    let value = value.trim().trim_matches('"');
    if value.is_empty() {
        "empty".to_string()
    } else {
        rtsp_log_value(value)
    }
}

fn redact_authorization(value: &str) -> String {
    let scheme = value.split_whitespace().next().unwrap_or("present");
    format!("{}:<redacted>", rtsp_log_value(scheme))
}

fn redacted_xml_element(body: &[u8], local_name: &str) -> Option<String> {
    let text = String::from_utf8_lossy(body);
    let start = find_xml_start_tag(&text, local_name)?;
    let content_start = text[start..].find('>')? + start + 1;
    let end = text[content_start..].find('<')? + content_start;
    let value = text[content_start..end].trim();
    if value.is_empty() {
        Some("empty".to_string())
    } else {
        Some(rtsp_log_value(value))
    }
}

fn find_xml_start_tag(text: &str, local_name: &str) -> Option<usize> {
    let needle = format!(":{local_name}");
    let mut search_from = 0usize;
    while let Some(offset) = text[search_from..].find(&needle) {
        let name_start = search_from + offset + 1;
        let tag_start = text[..name_start].rfind('<')?;
        let after_name = name_start + local_name.len();
        let terminator = text.as_bytes().get(after_name).copied();
        if terminator == Some(b'>') || terminator == Some(b' ') || terminator == Some(b'/') {
            return Some(tag_start);
        }
        search_from = after_name;
    }
    let needle = format!("<{local_name}");
    text.find(&needle)
}

fn wsse_presence(body: &[u8]) -> &'static str {
    let text = String::from_utf8_lossy(body);
    let has_username = text.contains("UsernameToken") || text.contains(":Username");
    let has_digest = text.contains("PasswordDigest");
    let has_password = text.contains(":Password") || text.contains("<Password");
    match (has_username, has_digest, has_password) {
        (true, true, _) => "UsernameToken+Digest:<redacted>",
        (true, false, true) => "UsernameToken+Password:<redacted>",
        (true, false, false) => "UsernameToken:<redacted>",
        (false, true, _) => "Digest:<redacted>",
        (false, false, true) => "Password:<redacted>",
        (false, false, false) => "none",
    }
}

fn onvif_name(service: &NetworkServiceInfo) -> String {
    service.onvif_name.replace(['\r', '\n'], " ")
}

fn onvif_hostname(service: &NetworkServiceInfo) -> String {
    let mut out = String::new();
    for ch in onvif_name(service).chars() {
        if ch.is_ascii_alphanumeric() || ch == '-' {
            out.push(ch);
        } else if ch.is_whitespace() || ch == '_' {
            out.push('-');
        }
    }
    while out.contains("--") {
        out = out.replace("--", "-");
    }
    out = out.trim_matches('-').to_string();
    if out.is_empty() {
        "LMI-OV13B10".to_string()
    } else {
        out
    }
}

const ONVIF_WSA_NS: &str = "http://www.w3.org/2005/08/addressing";
const ONVIF_WSA_ANONYMOUS: &str = "http://www.w3.org/2005/08/addressing/anonymous";

fn onvif_envelope(inner: &str) -> String {
    format!(
        concat!(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n",
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" ",
            "xmlns:a=\"{}\" ",
            "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" ",
            "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" ",
            "xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" ",
            "xmlns:tt=\"http://www.onvif.org/ver10/schema\" ",
            "xmlns:ter=\"http://www.onvif.org/ver10/error\">\n",
            "<s:Body>\n",
            "{}\n",
            "</s:Body>\n",
            "</s:Envelope>\n"
        ),
        ONVIF_WSA_NS, inner
    )
}

fn onvif_response_action_uri(
    action: &str,
    uses_media_service: bool,
    uses_imaging_service: bool,
) -> String {
    let media_action = matches!(
        action,
        "GetVideoEncoderConfigurationOptions"
            | "GetCompatibleVideoEncoderConfigurations"
            | "GetVideoEncoderConfigurations"
            | "GetVideoEncoderConfiguration"
            | "SetVideoEncoderConfiguration"
            | "GetVideoSourceConfigurationOptions"
            | "GetCompatibleVideoSourceConfigurations"
            | "GetVideoSourceConfigurations"
            | "GetVideoSourceConfiguration"
            | "SetVideoSourceConfiguration"
            | "GetGuaranteedNumberOfVideoEncoderInstances"
            | "GetVideoSources"
            | "GetProfiles"
            | "GetProfile"
            | "GetStreamUri"
            | "GetUri"
            | "GetSnapshotUri"
    );
    let namespace = if uses_imaging_service {
        "http://www.onvif.org/ver20/imaging/wsdl"
    } else if uses_media_service || media_action {
        "http://www.onvif.org/ver10/media/wsdl"
    } else {
        "http://www.onvif.org/ver10/device/wsdl"
    };
    let response = if action == "GetUri" {
        "GetStreamUriResponse".to_string()
    } else {
        format!("{action}Response")
    };
    format!("{namespace}/{response}")
}

fn onvif_add_response_headers(
    envelope: &str,
    action_uri: &str,
    relates_to: Option<&str>,
    message_id: &str,
) -> String {
    let relates_to = relates_to.map(str::trim).filter(|value| !value.is_empty());
    let relates_to_xml = relates_to
        .map(|value| format!("<a:RelatesTo>{}</a:RelatesTo>\n", xml_text(value)))
        .unwrap_or_default();
    let header = format!(
        concat!(
            "<s:Header>\n",
            "<a:Action>{}</a:Action>\n",
            "<a:MessageID>urn:uuid:{}</a:MessageID>\n",
            "{}",
            "<a:To>{}</a:To>\n",
            "</s:Header>\n"
        ),
        xml_text(action_uri),
        xml_text(message_id),
        relates_to_xml,
        ONVIF_WSA_ANONYMOUS
    );
    if let Some(body_pos) = envelope.find("<s:Body>") {
        let mut out = String::with_capacity(envelope.len() + header.len());
        out.push_str(&envelope[..body_pos]);
        out.push_str(&header);
        out.push_str(&envelope[body_pos..]);
        out
    } else {
        envelope.to_string()
    }
}

fn onvif_get_device_information(service: &NetworkServiceInfo) -> String {
    onvif_envelope(&format!(
        concat!(
            "<tds:GetDeviceInformationResponse>",
            "<tds:Manufacturer>LMI</tds:Manufacturer>",
            "<tds:Model>{}</tds:Model>",
            "<tds:FirmwareVersion>mainline-software-isp</tds:FirmwareVersion>",
            "<tds:SerialNumber>{}</tds:SerialNumber>",
            "<tds:HardwareId>SM8250-OV13B10</tds:HardwareId>",
            "</tds:GetDeviceInformationResponse>"
        ),
        xml_text(&onvif_name(service)),
        xml_text(&service.onvif_uuid)
    ))
}

fn onvif_get_endpoint_reference(service: &NetworkServiceInfo) -> String {
    onvif_envelope(&format!(
        concat!(
            "<tds:GetEndpointReferenceResponse>",
            "<tds:GUID>urn:uuid:{}</tds:GUID>",
            "</tds:GetEndpointReferenceResponse>"
        ),
        xml_text(&service.onvif_uuid)
    ))
}

fn onvif_get_services(device_uri: &str, media_uri: &str, service: &NetworkServiceInfo) -> String {
    let snapshot_uri = if service.codec == NetworkCodec::Mjpeg {
        "true"
    } else {
        "false"
    };
    let profile_count = onvif_profile_count(service);
    onvif_envelope(&format!(
        concat!(
            "<tds:GetServicesResponse>",
            "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>",
            "<tds:XAddr>{}</tds:XAddr>",
            "<tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version></tds:Service>",
            "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>",
            "<tds:XAddr>{}</tds:XAddr>",
            "<tds:Capabilities><trt:Capabilities SnapshotUri=\"{}\" Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\">",
            "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"{}\"/>",
            "{}",
            "</trt:Capabilities></tds:Capabilities>",
            "<tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version></tds:Service>",
            "</tds:GetServicesResponse>"
        ),
        xml_text(device_uri),
        xml_text(media_uri),
        snapshot_uri,
        profile_count,
        onvif_media_streaming_capabilities_xml(service)
    ))
}

fn onvif_get_service_capabilities() -> String {
    onvif_envelope(concat!(
        "<tds:GetServiceCapabilitiesResponse>",
        "<tds:Capabilities>",
        "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\" Dot11Configuration=\"false\" HostnameFromDHCP=\"false\" NTP=\"0\" DHCPv6=\"false\"/>",
        "<tds:Security TLS1.0=\"false\" TLS1.1=\"false\" TLS1.2=\"false\" OnboardKeyGeneration=\"false\" AccessPolicyConfig=\"false\" DefaultAccessPolicy=\"false\" Dot1X=\"false\" RemoteUserHandling=\"false\" X.509Token=\"false\" SAMLToken=\"false\" KerberosToken=\"false\" UsernameToken=\"false\" HttpDigest=\"false\" RELToken=\"false\" SupportedEAPMethods=\"0\" MaxUsers=\"1\"/>",
        "<tds:System DiscoveryResolve=\"true\" DiscoveryBye=\"true\" RemoteDiscovery=\"false\" SystemBackup=\"false\" SystemLogging=\"false\" FirmwareUpgrade=\"false\" HttpSystemBackup=\"false\" HttpSystemLogging=\"false\" HttpFirmwareUpgrade=\"false\"/>",
        "<tds:Misc AuxiliaryCommands=\"\"/>",
        "</tds:Capabilities></tds:GetServiceCapabilitiesResponse>"
    ))
}

fn onvif_get_media_service_capabilities(service: &NetworkServiceInfo) -> String {
    let snapshot_uri = if service.codec == NetworkCodec::Mjpeg {
        "true"
    } else {
        "false"
    };
    onvif_envelope(&format!(
        concat!(
            "<trt:GetServiceCapabilitiesResponse>",
            "<trt:Capabilities SnapshotUri=\"{}\" Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\">",
            "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"{}\"/>",
            "{}",
            "</trt:Capabilities></trt:GetServiceCapabilitiesResponse>"
        ),
        snapshot_uri,
        onvif_profile_count(service),
        onvif_media_streaming_capabilities_xml(service)
    ))
}

fn onvif_media_streaming_capabilities_xml(service: &NetworkServiceInfo) -> String {
    if service.rtsp_listen.is_some() {
        concat!(
            "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\" ",
            "NonAggregateControl=\"false\" NoRTSPStreaming=\"false\"/>"
        )
        .to_string()
    } else {
        concat!(
            "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"false\" RTP_RTSP_TCP=\"false\" ",
            "NonAggregateControl=\"false\" NoRTSPStreaming=\"true\"/>"
        )
        .to_string()
    }
}

fn onvif_get_imaging_service_capabilities() -> String {
    onvif_envelope(concat!(
        "<timg:GetServiceCapabilitiesResponse>",
        "<timg:Capabilities ImageStabilization=\"false\" Presets=\"false\" AdaptablePreset=\"false\"/>",
        "</timg:GetServiceCapabilitiesResponse>"
    ))
}

const ONVIF_MIN_EXPOSURE_US: f64 = 100.0;
const ONVIF_MAX_EXPOSURE_US: f64 = 33_000.0;
const ONVIF_MIN_GAIN: f64 = 1.0;
const ONVIF_MAX_GAIN: f64 = 16.0;

fn onvif_get_imaging_settings() -> String {
    onvif_envelope(concat!(
        "<timg:GetImagingSettingsResponse><timg:ImagingSettings>",
        "<tt:Exposure><tt:Mode>AUTO</tt:Mode><tt:Priority>LowNoise</tt:Priority>",
        "<tt:MinExposureTime>100</tt:MinExposureTime><tt:MaxExposureTime>33000</tt:MaxExposureTime>",
        "<tt:ExposureTime>10000</tt:ExposureTime>",
        "<tt:MinGain>1</tt:MinGain><tt:MaxGain>16</tt:MaxGain><tt:Gain>4</tt:Gain>",
        "</tt:Exposure>",
        "</timg:ImagingSettings></timg:GetImagingSettingsResponse>"
    ))
}

fn onvif_get_imaging_options() -> String {
    onvif_envelope(concat!(
        "<timg:GetOptionsResponse><timg:ImagingOptions>",
        "<tt:Exposure><tt:Mode>AUTO</tt:Mode><tt:Mode>MANUAL</tt:Mode>",
        "<tt:Priority>LowNoise</tt:Priority><tt:Priority>FrameRate</tt:Priority>",
        "<tt:MinExposureTime><tt:Min>100</tt:Min><tt:Max>33000</tt:Max></tt:MinExposureTime>",
        "<tt:MaxExposureTime><tt:Min>100</tt:Min><tt:Max>33000</tt:Max></tt:MaxExposureTime>",
        "<tt:MinGain><tt:Min>1</tt:Min><tt:Max>16</tt:Max></tt:MinGain>",
        "<tt:MaxGain><tt:Min>1</tt:Min><tt:Max>16</tt:Max></tt:MaxGain>",
        "</tt:Exposure>",
        "</timg:ImagingOptions></timg:GetOptionsResponse>"
    ))
}

fn onvif_set_imaging_settings(
    body: &str,
    mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    let mut applied = false;
    let exposure_mode = parse_onvif_exposure_mode(body);

    match exposure_mode {
        Some(OnvifExposureMode::Auto) => {
            println!("[netcam] ONVIF imaging exposure mode -> auto");
            apply_control_command(&service.isp_control_fifo, "auto_exposure=1");
            applied = true;
        }
        Some(OnvifExposureMode::Manual) => {
            println!("[netcam] ONVIF imaging exposure mode -> manual");
            apply_control_command(&service.isp_control_fifo, "auto_exposure=0");
            applied = true;
        }
        None => {}
    }

    if !matches!(exposure_mode, Some(OnvifExposureMode::Auto)) {
        if let Some(exposure_100us) = parse_onvif_exposure_time_100us(body) {
            let command = format!("exposure_absolute={exposure_100us}");
            println!("[netcam] ONVIF imaging exposure -> {command}");
            apply_control_command(&service.isp_control_fifo, &command);
            applied = true;
        }
        if let Some(gain) = parse_onvif_gain_to_uvc(body) {
            let command = format!("gain={gain}");
            println!("[netcam] ONVIF imaging gain -> {command}");
            apply_control_command(&service.isp_control_fifo, &command);
            applied = true;
        }
    }

    if let Some(roi) = parse_onvif_meter_roi(body, mode) {
        let command = format!(
            "meter_roi={},{},{},{},{}",
            roi.top, roi.left, roi.bottom, roi.right, roi.auto_controls
        );
        println!(
            "[netcam] ONVIF imaging ROI -> {} ({})",
            command,
            service.isp_control_fifo.display()
        );
        apply_control_command(&service.isp_control_fifo, &command);
        applied = true;
    }

    if !applied {
        println!("[netcam] ONVIF SetImagingSettings had no exposure/gain/window to apply");
    }
    onvif_envelope("<timg:SetImagingSettingsResponse></timg:SetImagingSettingsResponse>")
}

#[derive(Clone, Copy)]
enum OnvifExposureMode {
    Auto,
    Manual,
}

fn parse_onvif_exposure_mode(body: &str) -> Option<OnvifExposureMode> {
    let value = extract_onvif_exposure_text(body, "Mode")?;
    match value.trim().to_ascii_uppercase().as_str() {
        "AUTO" => Some(OnvifExposureMode::Auto),
        "MANUAL" => Some(OnvifExposureMode::Manual),
        _ => None,
    }
}

fn parse_onvif_exposure_time_100us(body: &str) -> Option<u32> {
    let value = extract_onvif_exposure_text(body, "ExposureTime")?
        .trim()
        .parse::<f64>()
        .ok()?;
    if !value.is_finite() {
        return None;
    }
    let us = value.clamp(ONVIF_MIN_EXPOSURE_US, ONVIF_MAX_EXPOSURE_US);
    Some(((us / 100.0).round() as u32).max(1))
}

fn parse_onvif_gain_to_uvc(body: &str) -> Option<u32> {
    let value = extract_onvif_exposure_text(body, "Gain")?
        .trim()
        .parse::<f64>()
        .ok()?;
    if !value.is_finite() {
        return None;
    }
    let gain = value.clamp(ONVIF_MIN_GAIN, ONVIF_MAX_GAIN);
    let normalized = (gain - ONVIF_MIN_GAIN) / (ONVIF_MAX_GAIN - ONVIF_MIN_GAIN);
    Some((normalized * 255.0).round() as u32)
}

fn extract_onvif_exposure_text(body: &str, local_name: &str) -> Option<String> {
    let tail = body
        .find(":Exposure")
        .or_else(|| body.find("<Exposure"))
        .map(|pos| &body[pos..])
        .unwrap_or(body);
    extract_xml_text(tail, local_name)
}

#[derive(Clone, Copy)]
struct MeterRoi {
    top: u32,
    left: u32,
    bottom: u32,
    right: u32,
    auto_controls: u32,
}

fn parse_onvif_meter_roi(body: &str, mode: NativeMode) -> Option<MeterRoi> {
    parse_onvif_meter_roi_from_attributes(body, mode).or_else(|| {
        let top = extract_xml_number_any(body, &["Top", "top"])?;
        let left = extract_xml_number_any(body, &["Left", "left"])?;
        let bottom = extract_xml_number_any(body, &["Bottom", "bottom"])?;
        let right = extract_xml_number_any(body, &["Right", "right"])?;
        meter_roi_from_rect_values(top, left, bottom, right, mode)
    })
}

fn parse_onvif_meter_roi_from_attributes(body: &str, mode: NativeMode) -> Option<MeterRoi> {
    let mut search_from = 0usize;
    while let Some(rel_start) = body[search_from..].find('<') {
        let start = search_from + rel_start;
        let Some(rel_end) = body[start..].find('>') else {
            break;
        };
        let end = start + rel_end;
        let tag = &body[start..=end];
        search_from = end + 1;
        if tag.starts_with("</") || tag.starts_with("<?") || tag.starts_with("<!") {
            continue;
        }

        let local = xml_start_tag_local_name(tag).unwrap_or_default();
        let local_lower = local.to_ascii_lowercase();
        let roi_like = matches!(
            local_lower.as_str(),
            "window"
                | "rectangle"
                | "meteringwindow"
                | "meteringrectangle"
                | "exposurewindow"
                | "bounds"
                | "roi"
                | "region"
                | "area"
        ) || (xml_attr_f64(tag, "top").is_some()
            && xml_attr_f64(tag, "left").is_some()
            && xml_attr_f64(tag, "bottom").is_some()
            && xml_attr_f64(tag, "right").is_some());
        if !roi_like {
            continue;
        }

        if let (Some(top), Some(left), Some(bottom), Some(right)) = (
            xml_attr_f64(tag, "top"),
            xml_attr_f64(tag, "left"),
            xml_attr_f64(tag, "bottom"),
            xml_attr_f64(tag, "right"),
        ) {
            if let Some(roi) = meter_roi_from_rect_values(top, left, bottom, right, mode) {
                return Some(roi);
            }
        }

        if let (Some(x), Some(y), Some(width), Some(height)) = (
            xml_attr_f64(tag, "x"),
            xml_attr_f64(tag, "y"),
            xml_attr_f64(tag, "width"),
            xml_attr_f64(tag, "height"),
        ) {
            if let Some(roi) = meter_roi_from_rect_values(y, x, y + height, x + width, mode) {
                return Some(roi);
            }
        }
    }
    None
}

fn meter_roi_from_rect_values(
    top: f64,
    left: f64,
    bottom: f64,
    right: f64,
    mode: NativeMode,
) -> Option<MeterRoi> {
    let values = [top, left, bottom, right];
    if values.iter().any(|value| !value.is_finite()) {
        return None;
    }
    if values.iter().all(|value| value.abs() < f64::EPSILON) {
        return Some(MeterRoi {
            top: 0,
            left: 0,
            bottom: 0,
            right: 0,
            auto_controls: 0,
        });
    }

    let any_negative = values.iter().any(|value| *value < 0.0);
    let all_unit = values.iter().all(|value| (0.0..=1.0).contains(value));
    let all_signed_unit = values.iter().all(|value| (-1.0..=1.0).contains(value));
    let pixel_like = !any_negative
        && right <= f64::from(mode.width).max(1.0) * 1.25
        && bottom <= f64::from(mode.height).max(1.0) * 1.25
        && (right > 1.0 || bottom > 1.0);

    let mut top = if any_negative && all_signed_unit {
        roi_coord_from_unit((top + 1.0) * 0.5)
    } else if all_unit {
        roi_coord_from_unit(top)
    } else if pixel_like {
        roi_coord_from_pixel(top, mode.height)
    } else {
        roi_coord_from_raw(top)
    };
    let mut left = if any_negative && all_signed_unit {
        roi_coord_from_unit((left + 1.0) * 0.5)
    } else if all_unit {
        roi_coord_from_unit(left)
    } else if pixel_like {
        roi_coord_from_pixel(left, mode.width)
    } else {
        roi_coord_from_raw(left)
    };
    let mut bottom = if any_negative && all_signed_unit {
        roi_coord_from_unit((bottom + 1.0) * 0.5)
    } else if all_unit {
        roi_coord_from_unit(bottom)
    } else if pixel_like {
        roi_coord_from_pixel(bottom, mode.height)
    } else {
        roi_coord_from_raw(bottom)
    };
    let mut right = if any_negative && all_signed_unit {
        roi_coord_from_unit((right + 1.0) * 0.5)
    } else if all_unit {
        roi_coord_from_unit(right)
    } else if pixel_like {
        roi_coord_from_pixel(right, mode.width)
    } else {
        roi_coord_from_raw(right)
    };

    if bottom < top {
        std::mem::swap(&mut top, &mut bottom);
    }
    if right < left {
        std::mem::swap(&mut left, &mut right);
    }
    if bottom == top {
        if bottom < 65535 {
            bottom += 1;
        } else {
            top = top.saturating_sub(1);
        }
    }
    if right == left {
        if right < 65535 {
            right += 1;
        } else {
            left = left.saturating_sub(1);
        }
    }

    Some(MeterRoi {
        top,
        left,
        bottom,
        right,
        auto_controls: 1,
    })
}

fn roi_coord_from_unit(value: f64) -> u32 {
    (value.clamp(0.0, 1.0) * 65535.0).round() as u32
}

fn roi_coord_from_pixel(value: f64, extent: u32) -> u32 {
    let extent = f64::from(extent.max(1));
    roi_coord_from_unit(value / extent)
}

fn roi_coord_from_raw(value: f64) -> u32 {
    value.round().clamp(0.0, 65535.0) as u32
}

fn extract_xml_number_any(body: &str, names: &[&str]) -> Option<f64> {
    names.iter().find_map(|name| {
        extract_xml_text(body, name).and_then(|value| value.trim().parse::<f64>().ok())
    })
}

fn xml_start_tag_local_name(tag: &str) -> Option<&str> {
    let tag = tag.trim_start_matches('<').trim_start();
    if tag.starts_with('/') || tag.starts_with('?') || tag.starts_with('!') {
        return None;
    }
    let end = tag
        .find(|ch: char| ch.is_whitespace() || ch == '/' || ch == '>')
        .unwrap_or(tag.len());
    let name = &tag[..end];
    if name.is_empty() {
        None
    } else {
        Some(
            name.rsplit_once(':')
                .map(|(_, local)| local)
                .unwrap_or(name),
        )
    }
}

fn xml_attr_f64(tag: &str, name: &str) -> Option<f64> {
    xml_attr_value(tag, name)?.trim().parse::<f64>().ok()
}

fn xml_attr_value<'a>(tag: &'a str, wanted_local: &str) -> Option<&'a str> {
    let bytes = tag.as_bytes();
    let mut i = 0usize;
    while i < bytes.len() {
        while i < bytes.len()
            && (bytes[i].is_ascii_whitespace()
                || bytes[i] == b'<'
                || bytes[i] == b'/'
                || bytes[i] == b'>')
        {
            i += 1;
        }
        let name_start = i;
        while i < bytes.len()
            && !bytes[i].is_ascii_whitespace()
            && bytes[i] != b'='
            && bytes[i] != b'/'
            && bytes[i] != b'>'
        {
            i += 1;
        }
        if name_start == i {
            i += 1;
            continue;
        }
        let name = &tag[name_start..i];
        while i < bytes.len() && bytes[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= bytes.len() || bytes[i] != b'=' {
            continue;
        }
        i += 1;
        while i < bytes.len() && bytes[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= bytes.len() {
            return None;
        }
        let quote = bytes[i];
        let value_start;
        let value_end;
        if quote == b'\'' || quote == b'\"' {
            i += 1;
            value_start = i;
            while i < bytes.len() && bytes[i] != quote {
                i += 1;
            }
            value_end = i;
            i = i.saturating_add(1);
        } else {
            value_start = i;
            while i < bytes.len()
                && !bytes[i].is_ascii_whitespace()
                && bytes[i] != b'/'
                && bytes[i] != b'>'
            {
                i += 1;
            }
            value_end = i;
        }
        let local = name
            .rsplit_once(':')
            .map(|(_, local)| local)
            .unwrap_or(name);
        if local.eq_ignore_ascii_case(wanted_local) {
            return Some(&tag[value_start..value_end]);
        }
    }
    None
}

fn onvif_get_capabilities(
    device_uri: &str,
    media_uri: &str,
    service: &NetworkServiceInfo,
) -> String {
    let (rtp_tcp, rtp_rtsp_tcp) = if service.rtsp_listen.is_some() {
        ("true", "true")
    } else {
        ("false", "false")
    };
    onvif_envelope(&format!(
        concat!(
            "<tds:GetCapabilitiesResponse><tds:Capabilities>",
            "<tt:Device><tt:XAddr>{}</tt:XAddr></tt:Device>",
            "<tt:Media><tt:XAddr>{}</tt:XAddr>",
            "<tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>",
            "<tt:RTP_TCP>{}</tt:RTP_TCP><tt:RTP_RTSP_TCP>{}</tt:RTP_RTSP_TCP></tt:StreamingCapabilities>",
            "<tt:Extension><tt:ProfileCapabilities><tt:MaximumNumberOfProfiles>{}</tt:MaximumNumberOfProfiles></tt:ProfileCapabilities></tt:Extension>",
            "</tt:Media>",
            "</tds:Capabilities></tds:GetCapabilitiesResponse>"
        ),
        xml_text(device_uri),
        xml_text(media_uri),
        rtp_tcp,
        rtp_rtsp_tcp,
        onvif_profile_count(service)
    ))
}

fn onvif_get_video_sources(mode: NativeMode, _service: &NetworkServiceInfo) -> String {
    onvif_envelope(&format!(
        concat!(
            "<trt:GetVideoSourcesResponse>",
            "<trt:VideoSources token=\"ov13b10\"><tt:Framerate>{}</tt:Framerate>",
            "<tt:Resolution><tt:Width>{}</tt:Width><tt:Height>{}</tt:Height></tt:Resolution>",
            "</trt:VideoSources></trt:GetVideoSourcesResponse>"
        ),
        mode.fps_cap, mode.width, mode.height
    ))
}

fn onvif_get_video_source_configuration_options(
    _mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    let bounds = onvif_modes(service)
        .iter()
        .map(|mode| {
            format!(
                concat!(
                    "<tt:BoundsRange><tt:XRange><tt:Min>0</tt:Min><tt:Max>0</tt:Max></tt:XRange>",
                    "<tt:YRange><tt:Min>0</tt:Min><tt:Max>0</tt:Max></tt:YRange>",
                    "<tt:WidthRange><tt:Min>{}</tt:Min><tt:Max>{}</tt:Max></tt:WidthRange>",
                    "<tt:HeightRange><tt:Min>{}</tt:Min><tt:Max>{}</tt:Max></tt:HeightRange></tt:BoundsRange>"
                ),
                mode.width, mode.width, mode.height, mode.height
            )
        })
        .collect::<String>();
    onvif_envelope(&format!(
        concat!(
            "<trt:GetVideoSourceConfigurationOptionsResponse><trt:Options MaximumNumberOfProfiles=\"{}\">",
            "{}",
            "<tt:VideoSourceTokensAvailable>ov13b10</tt:VideoSourceTokensAvailable>",
            "</trt:Options></trt:GetVideoSourceConfigurationOptionsResponse>"
        ),
        onvif_profile_count(service),
        bounds
    ))
}

fn onvif_get_video_source_configurations(
    _mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    let configurations = onvif_modes(service)
        .iter()
        .map(|mode| onvif_video_source_configuration_xml(*mode, "trt:Configurations"))
        .collect::<String>();
    onvif_envelope(&format!(
        "<trt:GetVideoSourceConfigurationsResponse>{}</trt:GetVideoSourceConfigurationsResponse>",
        configurations
    ))
}

fn onvif_get_video_source_configuration(
    body: &str,
    mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    let selected = onvif_requested_mode(body, service).unwrap_or(mode);
    onvif_envelope(&format!(
        "<trt:GetVideoSourceConfigurationResponse>{}</trt:GetVideoSourceConfigurationResponse>",
        onvif_video_source_configuration_xml(selected, "trt:Configuration")
    ))
}

fn onvif_get_compatible_video_source_configurations(
    _mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    let configurations = onvif_modes(service)
        .iter()
        .map(|mode| onvif_video_source_configuration_xml(*mode, "trt:Configurations"))
        .collect::<String>();
    onvif_envelope(&format!(
        "<trt:GetCompatibleVideoSourceConfigurationsResponse>{}</trt:GetCompatibleVideoSourceConfigurationsResponse>",
        configurations
    ))
}

fn onvif_set_video_source_configuration(
    body: &str,
    mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    if let Some(requested) = onvif_requested_mode(body, service) {
        if service.mode_switch.request(requested) {
            println!(
                "[netcam] ONVIF SetVideoSourceConfiguration requested native frame {} ({}x{} @ {:.3}fps)",
                requested.frame_index,
                requested.width,
                requested.height,
                requested.nominal_fps()
            );
        } else {
            println!(
                "[netcam] ONVIF SetVideoSourceConfiguration kept native frame {} ({}x{} @ {:.3}fps)",
                mode.frame_index,
                mode.width,
                mode.height,
                mode.nominal_fps()
            );
        }
    }
    onvif_envelope(
        "<trt:SetVideoSourceConfigurationResponse></trt:SetVideoSourceConfigurationResponse>",
    )
}

fn onvif_video_source_configuration_xml(mode: NativeMode, element: &str) -> String {
    format!(
        concat!(
            "<{} token=\"{}\"><tt:Name>OV13B10 RAW pgAA {}</tt:Name>",
            "<tt:UseCount>1</tt:UseCount><tt:SourceToken>ov13b10</tt:SourceToken>",
            "<tt:Bounds x=\"0\" y=\"0\" width=\"{}\" height=\"{}\"/>",
            "</{}>"
        ),
        element,
        onvif_video_source_token(mode),
        xml_text(&mode.frame_name()),
        mode.width,
        mode.height,
        element
    )
}

fn onvif_get_video_encoder_configurations(
    _mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    let configurations = onvif_modes(service)
        .iter()
        .map(|mode| onvif_video_encoder_configuration_xml(*mode, service, "trt:Configurations"))
        .collect::<String>();
    onvif_envelope(&format!(
        "<trt:GetVideoEncoderConfigurationsResponse>{}</trt:GetVideoEncoderConfigurationsResponse>",
        configurations
    ))
}

fn onvif_get_video_encoder_configuration(
    body: &str,
    mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    let selected = onvif_requested_mode(body, service).unwrap_or(mode);
    onvif_envelope(&format!(
        "<trt:GetVideoEncoderConfigurationResponse>{}</trt:GetVideoEncoderConfigurationResponse>",
        onvif_video_encoder_configuration_xml(selected, service, "trt:Configuration")
    ))
}

fn onvif_get_compatible_video_encoder_configurations(
    _mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    let configurations = onvif_modes(service)
        .iter()
        .map(|mode| onvif_video_encoder_configuration_xml(*mode, service, "trt:Configurations"))
        .collect::<String>();
    onvif_envelope(&format!(
        "<trt:GetCompatibleVideoEncoderConfigurationsResponse>{}</trt:GetCompatibleVideoEncoderConfigurationsResponse>",
        configurations
    ))
}

fn onvif_set_video_encoder_configuration(
    body: &str,
    mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    if let Some(requested) = onvif_requested_mode(body, service) {
        if service.mode_switch.request(requested) {
            println!(
                "[netcam] ONVIF SetVideoEncoderConfiguration requested native frame {} ({}x{} @ {:.3}fps)",
                requested.frame_index,
                requested.width,
                requested.height,
                requested.nominal_fps()
            );
        } else {
            println!(
                "[netcam] ONVIF SetVideoEncoderConfiguration kept native frame {} ({}x{} @ {:.3}fps)",
                mode.frame_index,
                mode.width,
                mode.height,
                mode.nominal_fps()
            );
        }
    } else {
        println!(
            "[netcam] ONVIF SetVideoEncoderConfiguration did not name a native LMI mode; active frame remains {}",
            mode.frame_index
        );
    }
    onvif_envelope(
        "<trt:SetVideoEncoderConfigurationResponse></trt:SetVideoEncoderConfigurationResponse>",
    )
}

fn onvif_video_encoder_configuration_xml(
    mode: NativeMode,
    service: &NetworkServiceInfo,
    element: &str,
) -> String {
    match service.codec {
        NetworkCodec::Mjpeg => format!(
            concat!(
                "<{} token=\"{}\"><tt:Name>userspace MJPEG {}</tt:Name>",
                "<tt:UseCount>1</tt:UseCount><tt:Encoding>JPEG</tt:Encoding>",
                "<tt:Resolution><tt:Width>{}</tt:Width><tt:Height>{}</tt:Height></tt:Resolution>",
                "<tt:Quality>{}.0</tt:Quality><tt:RateControl><tt:FrameRateLimit>{}</tt:FrameRateLimit>",
                "<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>20000</tt:BitrateLimit></tt:RateControl>",
                "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port><tt:TTL>1</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>",
                "<tt:SessionTimeout>PT60S</tt:SessionTimeout></{}>"
            ),
            element,
            onvif_encoder_token(mode, service),
            xml_text(&mode.frame_name()),
            mode.width,
            mode.height,
            onvif_mjpeg_quality(service.mjpeg_quality),
            mode.fps_cap,
            element
        ),
        NetworkCodec::H264 => format!(
            concat!(
                "<{} token=\"{}\"><tt:Name>userspace H.264 {}</tt:Name>",
                "<tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>",
                "<tt:Resolution><tt:Width>{}</tt:Width><tt:Height>{}</tt:Height></tt:Resolution>",
                "<tt:Quality>5.0</tt:Quality><tt:RateControl><tt:FrameRateLimit>{}</tt:FrameRateLimit>",
                "<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>{}</tt:BitrateLimit></tt:RateControl>",
                "<tt:H264><tt:GovLength>{}</tt:GovLength><tt:H264Profile>{}</tt:H264Profile></tt:H264>",
                "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port><tt:TTL>1</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>",
                "<tt:SessionTimeout>PT60S</tt:SessionTimeout></{}>"
            ),
            element,
            onvif_encoder_token(mode, service),
            xml_text(&mode.frame_name()),
            mode.width,
            mode.height,
            mode.fps_cap,
            service.h264_bitrate_kbps(mode),
            service.h264_gop.max(1),
            onvif_h264_profile(&service.h264_profile),
            element
        ),
    }
}

fn onvif_profile_count(service: &NetworkServiceInfo) -> usize {
    onvif_modes(service).len().max(1)
}

fn onvif_modes(service: &NetworkServiceInfo) -> Vec<NativeMode> {
    let mut modes = service
        .available_modes()
        .iter()
        .copied()
        .filter(|mode| service.codec != NetworkCodec::Mjpeg || rtp_jpeg_mode_supported(*mode))
        .collect::<Vec<_>>();
    if modes.is_empty() {
        if let Some(mode) = service.available_modes().first().copied() {
            modes.push(mode);
        }
    }
    modes
}

fn onvif_stream_token_suffix(mode: NativeMode) -> String {
    /* Windows Network Camera caches ONVIF profile/configuration metadata by
     * UUID and token.  A scaled stream keeps the same native frame index while
     * changing width/height/fps, so include the active stream shape in every
     * token to force a fresh capability record after --out-size experiments. */
    format!(
        "f{}-{}x{}-fps{}",
        mode.frame_index,
        mode.width,
        mode.height,
        mode.fps_cap.max(1)
    )
}

fn onvif_encoder_token(mode: NativeMode, service: &NetworkServiceInfo) -> String {
    format!(
        "lmi-{}-{}",
        service.codec.name(),
        onvif_stream_token_suffix(mode)
    )
}

fn onvif_profile_token(mode: NativeMode, service: &NetworkServiceInfo) -> String {
    format!(
        "lmi-native-{}-{}",
        service.codec.name(),
        onvif_stream_token_suffix(mode)
    )
}

fn onvif_video_source_token(mode: NativeMode) -> String {
    format!("lmi-vsrc-{}", onvif_stream_token_suffix(mode))
}

fn onvif_mjpeg_quality(mjpeg_quality: u32) -> u32 {
    mjpeg_quality.clamp(1, 100)
}

fn onvif_h264_profile(profile: &str) -> &'static str {
    match profile.to_ascii_lowercase().as_str() {
        "baseline" | "constrained-baseline" => "Baseline",
        "main" => "Main",
        "high" => "High",
        _ => "High",
    }
}

fn onvif_get_guaranteed_number_of_video_encoder_instances(service: &NetworkServiceInfo) -> String {
    let (jpeg, h264) = match service.codec {
        NetworkCodec::Mjpeg => (1, 0),
        NetworkCodec::H264 => (0, 1),
    };
    onvif_envelope(&format!(
        concat!(
            "<trt:GetGuaranteedNumberOfVideoEncoderInstancesResponse>",
            "<trt:TotalNumber>1</trt:TotalNumber>",
            "<trt:JPEG>{}</trt:JPEG><trt:H264>{}</trt:H264><trt:MPEG4>0</trt:MPEG4>",
            "</trt:GetGuaranteedNumberOfVideoEncoderInstancesResponse>"
        ),
        jpeg, h264
    ))
}

fn onvif_video_resolution_xml(mode: NativeMode) -> String {
    format!(
        "<tt:ResolutionsAvailable><tt:Width>{}</tt:Width><tt:Height>{}</tt:Height></tt:ResolutionsAvailable>",
        mode.width, mode.height
    )
}

fn onvif_int_range_xml(name: &str, min: u32, max: u32) -> String {
    format!(
        "<tt:{name}><tt:Min>{}</tt:Min><tt:Max>{}</tt:Max></tt:{name}>",
        min.min(max),
        max.max(min)
    )
}

fn onvif_h264_options_xml(mode: NativeMode, service: &NetworkServiceInfo) -> String {
    let max_fps = mode.fps_cap.max(1);
    let max_gop = service.h264_gop.max(1).max(60);
    format!(
        concat!(
            "{}",
            "{}",
            "{}",
            "{}",
            "<tt:H264ProfilesSupported>{}</tt:H264ProfilesSupported>"
        ),
        onvif_video_resolution_xml(mode),
        onvif_int_range_xml("GovLengthRange", service.h264_gop.max(1), max_gop),
        onvif_int_range_xml("FrameRateRange", 1, max_fps),
        onvif_int_range_xml("EncodingIntervalRange", 1, 1),
        onvif_h264_profile(&service.h264_profile)
    )
}

fn onvif_get_video_encoder_configuration_options(
    _mode: NativeMode,
    service: &NetworkServiceInfo,
) -> String {
    let modes = onvif_modes(service);
    let max_fps = modes
        .iter()
        .map(|mode| mode.fps_cap)
        .max()
        .unwrap_or(1)
        .max(1);
    let codec_options = match service.codec {
        NetworkCodec::Mjpeg => {
            let jpeg_options = format!(
                concat!("{}", "{}", "{}"),
                modes
                    .iter()
                    .map(|mode| onvif_video_resolution_xml(*mode))
                    .collect::<String>(),
                onvif_int_range_xml("FrameRateRange", 1, max_fps),
                onvif_int_range_xml("EncodingIntervalRange", 1, 1)
            );
            format!(
                concat!(
                    "<tt:JPEG>{}</tt:JPEG>",
                    "<tt:Extension><tt:JPEG>{}<tt:BitrateRange><tt:Min>64</tt:Min><tt:Max>20000</tt:Max></tt:BitrateRange></tt:JPEG></tt:Extension>"
                ),
                jpeg_options, jpeg_options
            )
        }
        NetworkCodec::H264 => {
            let h264_options = modes
                .iter()
                .map(|mode| onvif_h264_options_xml(*mode, service))
                .collect::<String>();
            let bitrate_min = modes
                .iter()
                .map(|mode| service.h264_bitrate_kbps(*mode))
                .min()
                .unwrap_or(64)
                .min(64)
                .max(1);
            let bitrate_max = modes
                .iter()
                .map(|mode| service.h264_bitrate_kbps(*mode))
                .max()
                .unwrap_or(20_000)
                .max(20_000);
            format!(
                concat!(
                    "<tt:H264>{}</tt:H264>",
                    "<tt:Extension><tt:H264>{}<tt:BitrateRange><tt:Min>{}</tt:Min><tt:Max>{}</tt:Max></tt:BitrateRange></tt:H264></tt:Extension>"
                ),
                h264_options, h264_options, bitrate_min, bitrate_max
            )
        }
    };
    onvif_envelope(&format!(
        concat!(
            "<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options GuaranteedFrameRateSupported=\"false\">",
            "<tt:QualityRange><tt:Min>1</tt:Min><tt:Max>100</tt:Max></tt:QualityRange>",
            "{}",
            "</trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>"
        ),
        codec_options
    ))
}

fn onvif_get_profiles(_mode: NativeMode, service: &NetworkServiceInfo) -> String {
    let profiles = onvif_modes(service)
        .iter()
        .map(|mode| onvif_profile_xml(*mode, service, "trt:Profiles"))
        .collect::<String>();
    onvif_envelope(&format!(
        "<trt:GetProfilesResponse>{}</trt:GetProfilesResponse>",
        profiles
    ))
}

fn onvif_get_profile(body: &str, mode: NativeMode, service: &NetworkServiceInfo) -> String {
    let selected = onvif_requested_mode(body, service).unwrap_or(mode);
    onvif_envelope(&format!(
        "<trt:GetProfileResponse>{}</trt:GetProfileResponse>",
        onvif_profile_xml(selected, service, "trt:Profile")
    ))
}

fn onvif_profile_xml(mode: NativeMode, service: &NetworkServiceInfo, element: &str) -> String {
    format!(
        concat!(
            "<{} token=\"{}\" fixed=\"true\">",
            "<tt:Name>{}</tt:Name>",
            "<tt:VideoSourceConfiguration token=\"{}\"><tt:Name>OV13B10 RAW pgAA {}</tt:Name>",
            "<tt:UseCount>1</tt:UseCount><tt:SourceToken>ov13b10</tt:SourceToken>",
            "<tt:Bounds x=\"0\" y=\"0\" width=\"{}\" height=\"{}\"/>",
            "</tt:VideoSourceConfiguration>",
            "{}",
            "</{}>"
        ),
        element,
        onvif_profile_token(mode, service),
        xml_text(&format!(
            "{} {}x{}@{}",
            service.onvif_name, mode.width, mode.height, mode.fps_cap
        )),
        onvif_video_source_token(mode),
        xml_text(&mode.frame_name()),
        mode.width,
        mode.height,
        onvif_video_encoder_configuration_xml(mode, service, "tt:VideoEncoderConfiguration"),
        element
    )
}

fn onvif_get_stream_uri(body: &str, stream_uri: &str, service: &NetworkServiceInfo) -> String {
    if let Some(requested) = onvif_requested_mode(body, service) {
        if service.mode_switch.request(requested) {
            println!(
                "[netcam] ONVIF GetStreamUri requested native frame {} ({}x{} @ {:.3}fps); stream URI remains stable across the local restart",
                requested.frame_index,
                requested.width,
                requested.height,
                requested.nominal_fps()
            );
        } else {
            println!(
                "[netcam] ONVIF GetStreamUri kept active native frame {} ({}x{} @ {:.3}fps)",
                requested.frame_index,
                requested.width,
                requested.height,
                requested.nominal_fps()
            );
        }
    }
    onvif_envelope(&format!(
        concat!(
            "<trt:GetStreamUriResponse><trt:MediaUri>",
            "<tt:Uri>{}</tt:Uri>",
            "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>",
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>",
            "<tt:Timeout>PT0S</tt:Timeout>",
            "</trt:MediaUri></trt:GetStreamUriResponse>"
        ),
        xml_text(stream_uri)
    ))
}

fn onvif_requested_mode(body: &str, service: &NetworkServiceInfo) -> Option<NativeMode> {
    onvif_request_tokens(body)
        .into_iter()
        .find_map(|token| onvif_mode_from_token(&token, service))
        .or_else(|| onvif_mode_from_resolution(body, service))
}

fn onvif_mode_from_token(token: &str, service: &NetworkServiceInfo) -> Option<NativeMode> {
    onvif_modes(service).into_iter().find(|mode| {
        token == onvif_encoder_token(*mode, service)
            || token == onvif_profile_token(*mode, service)
            || token == onvif_video_source_token(*mode)
    })
}

fn onvif_mode_from_resolution(body: &str, service: &NetworkServiceInfo) -> Option<NativeMode> {
    let width = extract_xml_text(body, "Width")?
        .trim()
        .parse::<u32>()
        .ok()?;
    let height = extract_xml_text(body, "Height")?
        .trim()
        .parse::<u32>()
        .ok()?;
    onvif_modes(service)
        .into_iter()
        .find(|mode| mode.width == width && mode.height == height)
}

fn onvif_request_tokens(body: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    for name in [
        "ProfileToken",
        "ConfigurationToken",
        "VideoEncoderConfigurationToken",
        "VideoSourceConfigurationToken",
        "Token",
    ] {
        if let Some(value) = extract_xml_text(body, name) {
            push_unique_token(&mut tokens, value);
        }
    }
    for local_name in [
        "Profile",
        "Profiles",
        "Configuration",
        "Configurations",
        "VideoEncoderConfiguration",
        "VideoSourceConfiguration",
    ] {
        for value in extract_xml_attr_values(body, local_name, "token") {
            push_unique_token(&mut tokens, value);
        }
    }
    tokens
}

fn push_unique_token(tokens: &mut Vec<String>, value: String) {
    let value = value.trim();
    if value.is_empty() {
        return;
    }
    if !tokens.iter().any(|existing| existing == value) {
        tokens.push(value.to_string());
    }
}

fn extract_xml_attr_values(body: &str, local_name: &str, attr_name: &str) -> Vec<String> {
    let mut values = Vec::new();
    let mut search_from = 0usize;
    while let Some(rel_start) = body[search_from..].find('<') {
        let start = search_from + rel_start;
        let Some(rel_end) = body[start..].find('>') else {
            break;
        };
        let end = start + rel_end;
        let tag = &body[start..=end];
        search_from = end + 1;
        if tag.starts_with("</") || tag.starts_with("<?") || tag.starts_with("<!") {
            continue;
        }
        if xml_start_tag_local_name(tag)
            .map(|local| local.eq_ignore_ascii_case(local_name))
            .unwrap_or(false)
        {
            if let Some(value) = xml_attr_value(tag, attr_name) {
                values.push(value.to_string());
            }
        }
    }
    values
}

fn onvif_get_snapshot_uri(snapshot_uri: &str) -> String {
    onvif_envelope(&format!(
        concat!(
            "<trt:GetSnapshotUriResponse><trt:MediaUri>",
            "<tt:Uri>{}</tt:Uri>",
            "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>",
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>",
            "<tt:Timeout>PT0S</tt:Timeout>",
            "</trt:MediaUri></trt:GetSnapshotUriResponse>"
        ),
        xml_text(snapshot_uri)
    ))
}

fn onvif_scopes(service: &NetworkServiceInfo) -> Vec<String> {
    vec![
        "onvif://www.onvif.org/type/video_encoder".to_string(),
        format!(
            "onvif://www.onvif.org/name/{}",
            scope_escape(&onvif_name(service))
        ),
        format!(
            "onvif://www.onvif.org/location/{}",
            scope_escape(&onvif_hostname(service))
        ),
        "onvif://www.onvif.org/hardware/SM8250-OV13B10".to_string(),
        "onvif://www.onvif.org/type/NetworkVideoTransmitter".to_string(),
        "onvif://www.onvif.org/Profile/Streaming".to_string(),
        "onvif://www.onvif.org/Profile/S".to_string(),
    ]
}

fn onvif_get_scopes(service: &NetworkServiceInfo) -> String {
    let scopes: String = onvif_scopes(service)
        .into_iter()
        .map(|scope| {
            format!(
                "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>{}</tt:ScopeItem></tds:Scopes>",
                xml_text(&scope)
            )
        })
        .collect();
    onvif_envelope(&format!(
        "<tds:GetScopesResponse>{scopes}</tds:GetScopesResponse>"
    ))
}

fn onvif_get_hostname(service: &NetworkServiceInfo) -> String {
    onvif_envelope(&format!(
        "<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP><tt:Name>{}</tt:Name></tds:HostnameInformation></tds:GetHostnameResponse>",
        xml_text(&onvif_hostname(service))
    ))
}

fn onvif_get_network_protocols(service: &NetworkServiceInfo) -> String {
    let http_port = listen_port(&service.http_listen).unwrap_or("8080");
    let rtsp = service.rtsp_listen.as_deref().map(|listen| {
        format!(
            "<tds:NetworkProtocols><tt:Name>RTSP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>{}</tt:Port></tds:NetworkProtocols>",
            xml_text(listen_port(listen).unwrap_or("8554"))
        )
    }).unwrap_or_default();
    onvif_envelope(&format!(
        concat!(
            "<tds:GetNetworkProtocolsResponse>",
            "<tds:NetworkProtocols><tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>{}</tt:Port></tds:NetworkProtocols>",
            "{}",
            "</tds:GetNetworkProtocolsResponse>"
        ),
        xml_text(http_port),
        rtsp
    ))
}

fn onvif_get_network_interfaces(
    host: &str,
    peer: SocketAddr,
    service: &NetworkServiceInfo,
) -> String {
    let ip = onvif_interface_ip(host, peer, service);
    let prefix = onvif_ipv4_prefix_len_from_route(ip)
        .or_else(|| onvif_ipv4_prefix_len(ip).filter(|prefix| (1..32).contains(prefix)))
        .unwrap_or(24);
    let interface = onvif_interface_name(ip).unwrap_or_else(|| "wlp1s0".to_string());
    let mac = onvif_interface_mac(&interface).unwrap_or_else(|| stable_local_mac(service));
    onvif_envelope(&format!(
        concat!(
            "<tds:GetNetworkInterfacesResponse>",
            "<tds:NetworkInterfaces token=\"{}\"><tt:Enabled>true</tt:Enabled>",
            "<tt:Info><tt:Name>{}</tt:Name><tt:HwAddress>{}</tt:HwAddress><tt:MTU>1500</tt:MTU></tt:Info>",
            "<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config>",
            "<tt:Manual><tt:Address>{}</tt:Address><tt:PrefixLength>{}</tt:PrefixLength></tt:Manual>",
            "<tt:DHCP>true</tt:DHCP></tt:Config></tt:IPv4>",
            "</tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>"
        ),
        xml_text(&interface),
        xml_text(&interface),
        xml_text(&mac),
        ip,
        prefix
    ))
}

fn onvif_interface_ip(host: &str, peer: SocketAddr, service: &NetworkServiceInfo) -> Ipv4Addr {
    let bare_host = strip_host_port(host).trim_matches(|ch| ch == '[' || ch == ']');
    if let Ok(addr) = bare_host.parse::<Ipv4Addr>() {
        if !addr.is_unspecified() && !addr.is_loopback() {
            return addr;
        }
    }
    if let Some(IpAddr::V4(addr)) = local_ip_for_peer(peer) {
        if !addr.is_unspecified() && !addr.is_loopback() {
            return addr;
        }
    }
    service
        .http_listen
        .rsplit_once(':')
        .and_then(|(host, _)| host.parse::<Ipv4Addr>().ok())
        .filter(|addr| !addr.is_unspecified() && !addr.is_loopback())
        .unwrap_or(Ipv4Addr::new(192, 168, 0, 41))
}

fn onvif_ipv4_prefix_len(ip: Ipv4Addr) -> Option<u32> {
    let contents = fs::read_to_string("/proc/net/fib_trie").ok()?;
    let needle = ip.to_string();
    let lines: Vec<&str> = contents.lines().collect();
    for (index, line) in lines.iter().enumerate() {
        if !line.contains(&needle) {
            continue;
        }
        let start = index.saturating_sub(8);
        let end = (index + 8).min(lines.len().saturating_sub(1));
        for candidate in &lines[start..=end] {
            if let Some(prefix) = parse_fib_prefix_len(candidate) {
                return Some(prefix);
            }
        }
    }
    None
}

fn parse_fib_prefix_len(line: &str) -> Option<u32> {
    let slash = line.find('/')?;
    let digits: String = line[slash + 1..]
        .chars()
        .take_while(|ch| ch.is_ascii_digit())
        .collect();
    let prefix = digits.parse::<u32>().ok()?;
    (prefix <= 32).then_some(prefix)
}

fn onvif_ipv4_prefix_len_from_route(ip: Ipv4Addr) -> Option<u32> {
    let contents = fs::read_to_string("/proc/net/route").ok()?;
    let ip = u32::from(ip);
    let mut best_prefix = None;
    for line in contents.lines().skip(1) {
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.len() < 8 {
            continue;
        }
        let Some(destination) = proc_net_route_ipv4(fields[1]) else {
            continue;
        };
        let Some(mask) = proc_net_route_ipv4(fields[7]) else {
            continue;
        };
        if mask != 0 && (ip & mask) == (destination & mask) {
            let prefix = mask.count_ones();
            if best_prefix.map(|best| prefix > best).unwrap_or(true) {
                best_prefix = Some(prefix);
            }
        }
    }
    best_prefix
}

fn onvif_interface_name(ip: Ipv4Addr) -> Option<String> {
    let contents = fs::read_to_string("/proc/net/route").ok()?;
    let ip = u32::from(ip);
    let mut best = None;
    for line in contents.lines().skip(1) {
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.len() < 8 {
            continue;
        }
        let Some(destination) = proc_net_route_ipv4(fields[1]) else {
            continue;
        };
        let Some(mask) = proc_net_route_ipv4(fields[7]) else {
            continue;
        };
        if mask != 0 && (ip & mask) == (destination & mask) {
            let prefix = mask.count_ones();
            if best
                .as_ref()
                .map(|(best_prefix, _): &(u32, String)| prefix > *best_prefix)
                .unwrap_or(true)
            {
                best = Some((prefix, fields[0].to_string()));
            }
        }
    }
    best.map(|(_, name)| name)
}

fn proc_net_route_ipv4(field: &str) -> Option<u32> {
    let raw = u32::from_str_radix(field, 16).ok()?;
    Some(u32::from_be_bytes(raw.to_le_bytes()))
}

fn onvif_interface_mac(interface: &str) -> Option<String> {
    let path = format!("/sys/class/net/{interface}/address");
    let mac = fs::read_to_string(path).ok()?.trim().to_ascii_uppercase();
    if mac == "00:00:00:00:00:00" || mac.len() != 17 {
        None
    } else {
        Some(mac)
    }
}

fn stable_local_mac(service: &NetworkServiceInfo) -> String {
    let mut bytes = [0x02u8, 0x4c, 0x4d, 0x49, 0x00, 0x00];
    for (index, byte) in service.onvif_uuid.bytes().enumerate() {
        bytes[3 + index % 3] ^= byte;
    }
    format!(
        "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]
    )
}

fn onvif_get_discovery_mode() -> String {
    onvif_envelope(
        "<tds:GetDiscoveryModeResponse><tds:DiscoveryMode>Discoverable</tds:DiscoveryMode></tds:GetDiscoveryModeResponse>",
    )
}

fn onvif_get_users(service: &NetworkServiceInfo) -> String {
    onvif_envelope(&format!(
        concat!(
            "<tds:GetUsersResponse>",
            "<tds:User><tt:Username>{}</tt:Username><tt:UserLevel>Administrator</tt:UserLevel></tds:User>",
            "</tds:GetUsersResponse>"
        ),
        xml_text(&onvif_hostname(service))
    ))
}

fn onvif_get_system_date_and_time() -> String {
    let now = current_utc_datetime();
    onvif_envelope(&format!(
        concat!(
            "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>",
            "<tt:DateTimeType>Manual</tt:DateTimeType>",
            "<tt:DaylightSavings>false</tt:DaylightSavings>",
            "<tt:TimeZone><tt:TZ>UTC</tt:TZ></tt:TimeZone>",
            "<tt:UTCDateTime>",
            "<tt:Time><tt:Hour>{}</tt:Hour><tt:Minute>{}</tt:Minute><tt:Second>{}</tt:Second></tt:Time>",
            "<tt:Date><tt:Year>{}</tt:Year><tt:Month>{}</tt:Month><tt:Day>{}</tt:Day></tt:Date>",
            "</tt:UTCDateTime>",
            "<tt:LocalDateTime>",
            "<tt:Time><tt:Hour>{}</tt:Hour><tt:Minute>{}</tt:Minute><tt:Second>{}</tt:Second></tt:Time>",
            "<tt:Date><tt:Year>{}</tt:Year><tt:Month>{}</tt:Month><tt:Day>{}</tt:Day></tt:Date>",
            "</tt:LocalDateTime>",
            "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>"
        ),
        now.hour,
        now.minute,
        now.second,
        now.year,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second,
        now.year,
        now.month,
        now.day
    ))
}

struct OnvifDateTime {
    year: i32,
    month: u32,
    day: u32,
    hour: u32,
    minute: u32,
    second: u32,
}

fn current_utc_datetime() -> OnvifDateTime {
    let seconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or_default();
    unix_seconds_to_utc(seconds)
}

fn unix_seconds_to_utc(seconds: u64) -> OnvifDateTime {
    let days = (seconds / 86_400) as i64;
    let seconds_of_day = seconds % 86_400;
    let (year, month, day) = civil_from_days(days);
    OnvifDateTime {
        year,
        month,
        day,
        hour: (seconds_of_day / 3600) as u32,
        minute: ((seconds_of_day % 3600) / 60) as u32,
        second: (seconds_of_day % 60) as u32,
    }
}

fn civil_from_days(days_since_unix_epoch: i64) -> (i32, u32, u32) {
    let z = days_since_unix_epoch + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let mut year = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = mp + if mp < 10 { 3 } else { -9 };
    year += if month <= 2 { 1 } else { 0 };
    (year as i32, month as u32, day as u32)
}

fn onvif_fault(code: &str, reason: &str) -> String {
    onvif_envelope(&format!(
        concat!(
            "<s:Fault><s:Code><s:Value>{}</s:Value></s:Code>",
            "<s:Reason><s:Text xml:lang=\"en\">{}</s:Text></s:Reason></s:Fault>"
        ),
        xml_text(code),
        xml_text(reason)
    ))
}

fn spawn_rtsp_server(
    listen: String,
    mode: NativeMode,
    shared: Arc<FrameStore>,
    service: NetworkServiceInfo,
) -> io::Result<JoinHandle<io::Result<()>>> {
    let listener = TcpListener::bind(&listen)?;
    listener.set_nonblocking(true)?;
    println!(
        "[netcam] RTSP server listening on {}",
        listener.local_addr()?
    );
    Ok(thread::spawn(move || {
        rtsp_server_loop(listener, mode, shared, service)
    }))
}

fn rtsp_server_loop(
    listener: TcpListener,
    mode: NativeMode,
    shared: Arc<FrameStore>,
    service: NetworkServiceInfo,
) -> io::Result<()> {
    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        match listener.accept() {
            Ok((stream, peer)) => {
                let shared = shared.clone();
                let service = service.clone();
                thread::spawn(move || {
                    if let Err(err) = handle_rtsp_client(stream, mode, shared, service) {
                        println!("[netcam] RTSP client {peer} disconnected: {err}");
                    }
                });
            }
            Err(err) if err.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(30));
            }
            Err(err) => return Err(err),
        }
    }
    Ok(())
}

struct RtspRequest {
    method: String,
    uri: String,
    cseq: String,
    transport: Option<String>,
}

const DEFAULT_RTP_SSRC: u32 = 0x4c4d4931;
const RTSP_TRACK_CONTROL: &str = "trackID=0";

struct RtspUdpTransport {
    rtp_socket: UdpSocket,
    _rtcp_socket: Option<UdpSocket>,
    target_rtp: SocketAddr,
    client_rtp_port: u16,
    client_rtcp_port: Option<u16>,
    server_rtp_port: u16,
    server_rtcp_port: Option<u16>,
    ssrc: u32,
}

enum RtspStreamTransport {
    TcpInterleaved { rtp_channel: u8, ssrc: u32 },
    UdpUnicast(RtspUdpTransport),
}

fn handle_rtsp_client(
    mut stream: TcpStream,
    mode: NativeMode,
    shared: Arc<FrameStore>,
    service: NetworkServiceInfo,
) -> io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_secs(30)))?;
    stream.set_write_timeout(Some(Duration::from_secs(3)))?;
    let peer = stream.peer_addr().ok();
    let session = format!("lmi{:08x}", mode.mode_index);
    let mut setup: Option<RtspStreamTransport> = None;
    let mut pending = Vec::new();
    loop {
        let request = read_rtsp_request(&mut stream, &mut pending)?;
        match request.method.as_str() {
            "OPTIONS" => write_rtsp_response(
                &mut stream,
                &request.cseq,
                "200 OK",
                &[("Public", RTSP_PUBLIC_METHODS.to_string())],
                None,
                b"",
            )?,
            "DESCRIBE" => {
                let describe_service = if service.codec == NetworkCodec::H264
                    && service.h264_sprop_parameter_sets.is_empty()
                {
                    let (sps, pps) = shared.wait_for_h264_params(Duration::from_millis(1500));
                    service
                        .clone()
                        .with_h264_sdp(sps.as_deref(), pps.as_deref())
                } else {
                    service.clone()
                };
                let sdp = rtsp_sdp(&request.uri, mode, &describe_service);
                println!(
                    "[netcam] RTSP DESCRIBE uri={} codec={} h264_sprop={}",
                    rtsp_log_value(&request.uri),
                    describe_service.codec.name(),
                    if describe_service.h264_sprop_parameter_sets.is_empty() {
                        "absent"
                    } else {
                        "present"
                    }
                );
                write_rtsp_response(
                    &mut stream,
                    &request.cseq,
                    "200 OK",
                    &[
                        ("Content-Base", rtsp_content_base(&request.uri)),
                        ("Content-Type", "application/sdp".to_string()),
                    ],
                    None,
                    sdp.as_bytes(),
                )?;
            }
            "SETUP" => {
                let Some(transport) = request.transport.as_deref() else {
                    write_rtsp_response(
                        &mut stream,
                        &request.cseq,
                        "461 Unsupported Transport",
                        &[],
                        None,
                        b"",
                    )?;
                    continue;
                };
                println!(
                    "[netcam] RTSP SETUP transport={}",
                    rtsp_log_value(transport)
                );
                let upper_transport = transport.to_ascii_uppercase();
                if upper_transport.contains("RTP/AVP/TCP") {
                    let rtp_channel = parse_interleaved_rtp_channel(transport).unwrap_or(0);
                    let ssrc = parse_transport_ssrc(transport).unwrap_or(DEFAULT_RTP_SSRC);
                    let mode_play = rtsp_transport_requests_play(transport);
                    let transport_header = format!(
                        "RTP/AVP/TCP;unicast;interleaved={}-{};ssrc={}{}",
                        rtp_channel,
                        rtp_channel.saturating_add(1),
                        rtsp_ssrc_hex(ssrc),
                        if mode_play { ";mode=PLAY" } else { "" }
                    );
                    let setup_headers = rtsp_setup_response_headers(
                        transport_header,
                        &session,
                        mode_play,
                        &request.uri,
                        shared.as_ref(),
                        &service,
                    );
                    write_rtsp_response(
                        &mut stream,
                        &request.cseq,
                        "200 OK",
                        &setup_headers,
                        None,
                        b"",
                    )?;
                    let setup_transport = RtspStreamTransport::TcpInterleaved { rtp_channel, ssrc };
                    if mode_play
                        && !rtsp_setup_mode_play_waits_for_explicit_request(&mut stream, &pending)?
                    {
                        println!(
                            "[netcam] RTSP SETUP mode=PLAY starting implicit PLAY over TCP interleaved channel={}",
                            rtp_channel
                        );
                        return stream_rtsp_transport(
                            &mut stream,
                            shared,
                            mode,
                            setup_transport,
                            &session,
                            &request.uri,
                            &service,
                        );
                    }
                    setup = Some(setup_transport);
                    continue;
                }

                if upper_transport.contains("RTP/AVP") {
                    if service.rtsp_force_tcp {
                        write_rtsp_response(
                            &mut stream,
                            &request.cseq,
                            "461 Unsupported Transport",
                            &[],
                            None,
                            b"UDP RTSP transport is disabled; use RTP/AVP/TCP interleaved.\n",
                        )?;
                        continue;
                    }
                    let Some((client_rtp_port, client_rtcp_port)) = parse_client_ports(transport)
                    else {
                        write_rtsp_response(
                            &mut stream,
                            &request.cseq,
                            "461 Unsupported Transport",
                            &[],
                            None,
                            b"UDP RTSP transport needs client_port=rtp[-rtcp].\n",
                        )?;
                        continue;
                    };
                    let Some(peer) = peer else {
                        write_rtsp_response(
                            &mut stream,
                            &request.cseq,
                            "461 Unsupported Transport",
                            &[],
                            None,
                            b"UDP RTSP transport needs a peer address.\n",
                        )?;
                        continue;
                    };
                    let ssrc = parse_transport_ssrc(transport).unwrap_or(DEFAULT_RTP_SSRC);
                    let udp =
                        prepare_udp_rtsp_transport(peer, client_rtp_port, client_rtcp_port, ssrc)?;
                    let server_port = match udp.server_rtcp_port {
                        Some(rtcp) => format!("{}-{}", udp.server_rtp_port, rtcp),
                        None => udp.server_rtp_port.to_string(),
                    };
                    let client_port = match udp.client_rtcp_port {
                        Some(rtcp) => format!("{}-{}", udp.client_rtp_port, rtcp),
                        None => udp.client_rtp_port.to_string(),
                    };
                    let transport_proto = if upper_transport.contains("RTP/AVP/UDP") {
                        "RTP/AVP/UDP"
                    } else {
                        "RTP/AVP"
                    };
                    let mode_play = rtsp_transport_requests_play(transport);
                    println!(
                        "[netcam] RTSP UDP target={} server_port={} ssrc={}",
                        udp.target_rtp,
                        server_port,
                        rtsp_ssrc_hex(ssrc)
                    );
                    let transport_header = format!(
                        "{transport_proto};unicast;client_port={client_port};server_port={server_port};ssrc={}{}",
                        rtsp_ssrc_hex(ssrc),
                        if mode_play { ";mode=PLAY" } else { "" }
                    );
                    let setup_headers = rtsp_setup_response_headers(
                        transport_header,
                        &session,
                        mode_play,
                        &request.uri,
                        shared.as_ref(),
                        &service,
                    );
                    write_rtsp_response(
                        &mut stream,
                        &request.cseq,
                        "200 OK",
                        &setup_headers,
                        None,
                        b"",
                    )?;
                    let setup_transport = RtspStreamTransport::UdpUnicast(udp);
                    if mode_play
                        && !rtsp_setup_mode_play_waits_for_explicit_request(&mut stream, &pending)?
                    {
                        println!("[netcam] RTSP SETUP mode=PLAY starting implicit PLAY over UDP");
                        return stream_rtsp_transport(
                            &mut stream,
                            shared,
                            mode,
                            setup_transport,
                            &session,
                            &request.uri,
                            &service,
                        );
                    }
                    setup = Some(setup_transport);
                    continue;
                }

                write_rtsp_response(
                    &mut stream,
                    &request.cseq,
                    "461 Unsupported Transport",
                    &[],
                    None,
                    b"Supported transports: RTP/AVP UDP unicast or RTP/AVP/TCP interleaved.\n",
                )?;
            }
            "PLAY" => {
                let Some(setup) = setup.take() else {
                    write_rtsp_response(
                        &mut stream,
                        &request.cseq,
                        "454 Session Not Found",
                        &[],
                        None,
                        b"",
                    )?;
                    continue;
                };
                let rtp_start = rtsp_rtp_start(&shared, &service);
                write_rtsp_response(
                    &mut stream,
                    &request.cseq,
                    "200 OK",
                    &[
                        ("Session", session.clone()),
                        ("Range", "npt=0-".to_string()),
                        (
                            "RTP-Info",
                            rtsp_rtp_info(
                                &rtsp_track_url(&request.uri),
                                rtp_start.sequence,
                                rtp_start.timestamp,
                            ),
                        ),
                    ],
                    None,
                    b"",
                )?;
                return stream_rtsp_transport(
                    &mut stream,
                    shared,
                    mode,
                    setup,
                    &session,
                    &request.uri,
                    &service,
                );
            }
            "GET_PARAMETER" => write_rtsp_response(
                &mut stream,
                &request.cseq,
                "200 OK",
                &[("Session", session.clone())],
                None,
                b"",
            )?,
            "TEARDOWN" => {
                write_rtsp_response(
                    &mut stream,
                    &request.cseq,
                    "200 OK",
                    &[("Session", session.clone())],
                    None,
                    b"",
                )?;
                return Ok(());
            }
            _ => write_rtsp_response(
                &mut stream,
                &request.cseq,
                "405 Method Not Allowed",
                &[("Public", RTSP_PUBLIC_METHODS.to_string())],
                None,
                b"",
            )?,
        }
    }
}

fn stream_rtsp_transport(
    stream: &mut TcpStream,
    shared: Arc<FrameStore>,
    mode: NativeMode,
    setup: RtspStreamTransport,
    session: &str,
    request_uri: &str,
    service: &NetworkServiceInfo,
) -> io::Result<()> {
    let _guard = ClientGuard::new(shared.clone(), true);
    match setup {
        RtspStreamTransport::TcpInterleaved { rtp_channel, ssrc } => stream_rtsp_tcp(
            stream,
            shared,
            mode,
            rtp_channel,
            ssrc,
            session,
            &rtsp_track_url(request_uri),
            service,
        ),
        RtspStreamTransport::UdpUnicast(udp) => stream_rtsp_udp(
            stream,
            shared,
            mode,
            udp,
            session,
            &rtsp_track_url(request_uri),
            service,
        ),
    }
}

fn rtsp_setup_response_headers(
    transport_header: String,
    session: &str,
    mode_play: bool,
    request_uri: &str,
    shared: &FrameStore,
    service: &NetworkServiceInfo,
) -> Vec<(&'static str, String)> {
    let mut headers = vec![
        ("Transport", transport_header),
        ("Session", session.to_string()),
    ];
    if mode_play {
        let rtp_start = rtsp_rtp_start(shared, service);
        headers.push(("Range", "npt=0-".to_string()));
        headers.push((
            "RTP-Info",
            rtsp_rtp_info(
                &rtsp_track_url(request_uri),
                rtp_start.sequence,
                rtp_start.timestamp,
            ),
        ));
    }
    headers
}

fn rtsp_setup_mode_play_waits_for_explicit_request(
    stream: &mut TcpStream,
    pending: &[u8],
) -> io::Result<bool> {
    if !pending.is_empty() {
        return Ok(true);
    }
    let old_timeout = stream.read_timeout()?;
    stream.set_read_timeout(Some(Duration::from_millis(RTSP_SETUP_MODE_PLAY_GRACE_MS)))?;
    let mut probe = [0u8; 1];
    let result = match stream.peek(&mut probe) {
        Ok(0) => Ok(true),
        Ok(_) => Ok(true),
        Err(err)
            if err.kind() == io::ErrorKind::WouldBlock || err.kind() == io::ErrorKind::TimedOut =>
        {
            Ok(false)
        }
        Err(err) => Err(err),
    };
    let restore = stream.set_read_timeout(old_timeout);
    match (result, restore) {
        (Err(err), _) => Err(err),
        (Ok(_), Err(err)) => Err(err),
        (Ok(value), Ok(())) => Ok(value),
    }
}

fn read_rtsp_request(stream: &mut TcpStream, pending: &mut Vec<u8>) -> io::Result<RtspRequest> {
    let mut buf = [0u8; 1024];
    loop {
        if let Some(header_end) = find_header_end(pending) {
            let delimiter_len = if pending[header_end..].starts_with(b"\r\n\r\n") {
                4
            } else {
                2
            };
            let header = String::from_utf8_lossy(&pending[..header_end]).to_string();
            let content_length = header_value(&header, "content-length")
                .and_then(|value| value.parse::<usize>().ok())
                .unwrap_or(0);
            let message_len = header_end
                .saturating_add(delimiter_len)
                .saturating_add(content_length);
            if pending.len() >= message_len {
                let message: Vec<u8> = pending.drain(..message_len).collect();
                let header = String::from_utf8_lossy(&message[..header_end]).to_string();
                let mut lines = header.lines();
                let request_line = lines.next().unwrap_or_default().trim();
                let mut parts = request_line.split_whitespace();
                let method = parts.next().unwrap_or_default().to_string();
                let uri = parts.next().unwrap_or("*").to_string();
                if method.is_empty() {
                    return Err(io::Error::new(
                        io::ErrorKind::UnexpectedEof,
                        "empty RTSP request",
                    ));
                }
                return Ok(RtspRequest {
                    method,
                    uri,
                    cseq: header_value(&header, "cseq").unwrap_or_else(|| "1".to_string()),
                    transport: header_value(&header, "transport"),
                });
            }
        }
        if pending.len() >= 65536 {
            return Err(invalid_input("RTSP request headers are too large"));
        }
        let n = stream.read(&mut buf)?;
        if n == 0 {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "RTSP client closed connection",
            ));
        }
        pending.extend_from_slice(&buf[..n]);
    }
}

fn parse_inline_rtsp_control(buf: &[u8]) -> Option<RtspRequest> {
    let header_end = find_header_end(buf)?;
    let header = String::from_utf8_lossy(&buf[..header_end]).to_string();
    let mut lines = header.lines();
    let request_line = lines.next()?.trim();
    let mut parts = request_line.split_whitespace();
    let method = parts.next()?.to_string();
    if method.is_empty() {
        return None;
    }
    Some(RtspRequest {
        method,
        uri: parts.next().unwrap_or("*").to_string(),
        cseq: header_value(&header, "cseq").unwrap_or_else(|| "1".to_string()),
        transport: header_value(&header, "transport"),
    })
}

fn service_rtsp_tcp_control(
    stream: &mut TcpStream,
    session: &str,
    _track_url: &str,
    pending: &mut Vec<u8>,
) -> io::Result<bool> {
    let mut buf = [0u8; 1024];
    loop {
        match stream.read(&mut buf) {
            Ok(0) => return Ok(false),
            Ok(n) => pending.extend_from_slice(&buf[..n]),
            Err(err)
                if err.kind() == io::ErrorKind::WouldBlock
                    || err.kind() == io::ErrorKind::TimedOut =>
            {
                break;
            }
            Err(err) => return Err(err),
        }
    }

    loop {
        if pending.is_empty() {
            return Ok(true);
        }

        if pending[0] == b'$' {
            if pending.len() < 4 {
                return Ok(true);
            }
            let frame_len = u16::from_be_bytes([pending[2], pending[3]]) as usize;
            let total_len = 4usize.saturating_add(frame_len);
            if pending.len() < total_len {
                return Ok(true);
            }
            pending.drain(..total_len);
            continue;
        }

        let Some(header_end) = find_header_end(pending) else {
            if pending.len() > 65536 {
                return Err(invalid_input("RTSP control request headers are too large"));
            }
            return Ok(true);
        };
        let delimiter_len = if pending[header_end..].starts_with(b"\r\n\r\n") {
            4
        } else {
            2
        };
        let header = String::from_utf8_lossy(&pending[..header_end]).to_string();
        let content_length = header_value(&header, "content-length")
            .and_then(|value| value.parse::<usize>().ok())
            .unwrap_or(0);
        let message_len = header_end
            .saturating_add(delimiter_len)
            .saturating_add(content_length);
        if pending.len() < message_len {
            return Ok(true);
        }
        let message: Vec<u8> = pending.drain(..message_len).collect();
        let Some(request) = parse_inline_rtsp_control(&message) else {
            continue;
        };

        if request.method.eq_ignore_ascii_case("TEARDOWN") {
            write_rtsp_response(
                stream,
                &request.cseq,
                "200 OK",
                &[("Session", session.to_string())],
                None,
                b"",
            )?;
            return Ok(false);
        }
        if request.method.eq_ignore_ascii_case("GET_PARAMETER") {
            write_rtsp_response(
                stream,
                &request.cseq,
                "200 OK",
                &[("Session", session.to_string())],
                None,
                b"",
            )?;
            continue;
        }
        if request.method.eq_ignore_ascii_case("OPTIONS") {
            write_rtsp_response(
                stream,
                &request.cseq,
                "200 OK",
                &[("Public", RTSP_PUBLIC_METHODS.to_string())],
                None,
                b"",
            )?;
            continue;
        }
        if request.method.eq_ignore_ascii_case("PLAY") {
            write_rtsp_response(
                stream,
                &request.cseq,
                "200 OK",
                &[
                    ("Session", session.to_string()),
                    ("Range", "npt=0-".to_string()),
                ],
                None,
                b"",
            )?;
            continue;
        }

        write_rtsp_response(
            stream,
            &request.cseq,
            "405 Method Not Allowed",
            &[("Public", RTSP_PUBLIC_METHODS.to_string())],
            None,
            b"",
        )?;
    }
}

fn write_rtsp_response(
    stream: &mut TcpStream,
    cseq: &str,
    status: &str,
    headers: &[(&str, String)],
    content_type: Option<&str>,
    body: &[u8],
) -> io::Result<()> {
    write!(stream, "RTSP/1.0 {status}\r\nCSeq: {cseq}\r\n")?;
    for (name, value) in headers {
        write!(stream, "{name}: {value}\r\n")?;
    }
    if let Some(content_type) = content_type {
        write!(stream, "Content-Type: {content_type}\r\n")?;
    }
    write!(stream, "Content-Length: {}\r\n\r\n", body.len())?;
    stream.write_all(body)
}

fn rtsp_sdp(uri: &str, mode: NativeMode, service: &NetworkServiceInfo) -> String {
    let name = service.onvif_name.replace(['\r', '\n'], " ");
    match service.codec {
        NetworkCodec::Mjpeg => format!(
            concat!(
                "v=0\r\n",
                "o=- 0 0 IN IP4 0.0.0.0\r\n",
                "s={}\r\n",
                "c=IN IP4 0.0.0.0\r\n",
                "t=0 0\r\n",
                "a=range:npt=0-\r\n",
                "m=video 0 RTP/AVP 26\r\n",
                "a=rtpmap:26 JPEG/90000\r\n",
                "a=framesize:26 {}-{}\r\n",
                "a=framerate:{}\r\n",
                "a=sendonly\r\n",
                "a=control:{}\r\n"
            ),
            name,
            mode.width,
            mode.height,
            mode.fps_cap,
            rtsp_track_control(uri)
        ),
        NetworkCodec::H264 => {
            let fmtp = if service.h264_sprop_parameter_sets.is_empty() {
                format!(
                    "packetization-mode=1;profile-level-id={}",
                    service.h264_profile_level_id
                )
            } else {
                format!(
                    "packetization-mode=1;profile-level-id={};sprop-parameter-sets={}",
                    service.h264_profile_level_id, service.h264_sprop_parameter_sets
                )
            };
            format!(
                concat!(
                    "v=0\r\n",
                    "o=- 0 0 IN IP4 0.0.0.0\r\n",
                    "s={}\r\n",
                    "c=IN IP4 0.0.0.0\r\n",
                    "t=0 0\r\n",
                    "a=range:npt=0-\r\n",
                    "m=video 0 RTP/AVP {}\r\n",
                    "a=rtpmap:{} H264/90000\r\n",
                    "a=fmtp:{} {}\r\n",
                    "a=framesize:{} {}-{}\r\n",
                    "a=framerate:{}\r\n",
                    "a=sendonly\r\n",
                    "a=control:{}\r\n"
                ),
                name,
                H264_RTP_PAYLOAD_TYPE,
                H264_RTP_PAYLOAD_TYPE,
                H264_RTP_PAYLOAD_TYPE,
                fmtp,
                H264_RTP_PAYLOAD_TYPE,
                mode.width,
                mode.height,
                mode.fps_cap,
                rtsp_track_control(uri)
            )
        }
    }
}

fn rtsp_content_base(uri: &str) -> String {
    let base = rtsp_base_uri(uri);
    if base.ends_with('/') {
        base
    } else {
        format!("{base}/")
    }
}

fn rtsp_track_control(_uri: &str) -> String {
    RTSP_TRACK_CONTROL.to_string()
}

fn rtsp_base_uri(uri: &str) -> String {
    uri.strip_suffix('/')
        .unwrap_or(uri)
        .strip_suffix(RTSP_TRACK_CONTROL)
        .map(|base| base.trim_end_matches('/').to_string())
        .filter(|base| !base.is_empty())
        .unwrap_or_else(|| uri.trim_end_matches('/').to_string())
}

fn rtsp_track_url(uri: &str) -> String {
    let base = rtsp_base_uri(uri);
    if base.is_empty() {
        RTSP_TRACK_CONTROL.to_string()
    } else {
        format!("{base}/{RTSP_TRACK_CONTROL}")
    }
}

fn rtsp_rtp_info(url: &str, seq: u16, rtptime: u32) -> String {
    format!(
        "url={};seq={};rtptime={}",
        rtsp_log_value(url),
        seq,
        rtptime
    )
}

#[derive(Clone, Copy)]
struct RtspRtpStart {
    sequence: u16,
    timestamp: u32,
}

fn rtsp_rtp_start(_shared: &FrameStore, _service: &NetworkServiceInfo) -> RtspRtpStart {
    /* Each RTSP session starts a fresh RTP sequence/timestamp space.  The
     * sender uses these exact values for the first packet it emits after a
     * successful SETUP/PLAY response, even when H.264 history is replayed
     * from the latest IDR. */
    RtspRtpStart {
        sequence: 1,
        timestamp: 0,
    }
}

fn rtsp_ssrc_hex(ssrc: u32) -> String {
    format!("{ssrc:08X}")
}

fn parse_interleaved_rtp_channel(transport: &str) -> Option<u8> {
    transport.split(';').find_map(|part| {
        let value = strip_transport_value(part.trim(), "interleaved")?;
        let first = value.split('-').next()?;
        first.parse::<u8>().ok()
    })
}

fn parse_client_ports(transport: &str) -> Option<(u16, Option<u16>)> {
    transport.split(';').find_map(|part| {
        let value = strip_transport_value(part.trim(), "client_port")?;
        let mut ports = value.split('-');
        let rtp = ports.next()?.parse::<u16>().ok()?;
        let rtcp = ports.next().and_then(|port| port.parse::<u16>().ok());
        Some((rtp, rtcp))
    })
}

fn parse_transport_ssrc(transport: &str) -> Option<u32> {
    transport.split(';').find_map(|part| {
        let value = strip_transport_value(part.trim(), "ssrc")?;
        u32::from_str_radix(value.trim_start_matches("0x").trim_start_matches("0X"), 16).ok()
    })
}

fn strip_transport_value<'a>(part: &'a str, key: &str) -> Option<&'a str> {
    let (name, value) = part.split_once('=')?;
    if name.trim().eq_ignore_ascii_case(key) {
        Some(value.trim())
    } else {
        None
    }
}

fn rtsp_transport_requests_play(transport: &str) -> bool {
    transport.split(';').any(|part| {
        strip_transport_value(part.trim(), "mode")
            .map(|value| value.eq_ignore_ascii_case("PLAY"))
            .unwrap_or(false)
    })
}

fn prepare_udp_rtsp_transport(
    peer: SocketAddr,
    client_rtp_port: u16,
    requested_client_rtcp_port: Option<u16>,
    ssrc: u32,
) -> io::Result<RtspUdpTransport> {
    let any_addr = if peer.is_ipv4() {
        IpAddr::V4(Ipv4Addr::UNSPECIFIED)
    } else {
        IpAddr::V6(std::net::Ipv6Addr::UNSPECIFIED)
    };
    let (rtp_socket, rtcp_socket) = bind_udp_rtp_pair(any_addr)?;
    let server_rtp_port = rtp_socket.local_addr()?.port();
    let server_rtcp_port = rtcp_socket.local_addr()?.port();
    let client_rtcp_port = requested_client_rtcp_port.or_else(|| client_rtp_port.checked_add(1));
    let target_rtp = SocketAddr::new(peer.ip(), client_rtp_port);
    Ok(RtspUdpTransport {
        rtp_socket,
        _rtcp_socket: Some(rtcp_socket),
        target_rtp,
        client_rtp_port,
        client_rtcp_port,
        server_rtp_port,
        server_rtcp_port: Some(server_rtcp_port),
        ssrc,
    })
}

fn bind_udp_rtp_pair(any_addr: IpAddr) -> io::Result<(UdpSocket, UdpSocket)> {
    let mut last_error = None;
    for _ in 0..64 {
        let rtp_socket = UdpSocket::bind(SocketAddr::new(any_addr, 0))?;
        let rtp_port = rtp_socket.local_addr()?.port();
        if rtp_port % 2 != 0 || rtp_port == u16::MAX {
            continue;
        }
        match UdpSocket::bind(SocketAddr::new(any_addr, rtp_port + 1)) {
            Ok(rtcp_socket) => return Ok((rtp_socket, rtcp_socket)),
            Err(err) => last_error = Some(err),
        }
    }
    Err(last_error.unwrap_or_else(|| invalid_input("failed to allocate even/odd UDP RTP ports")))
}

fn rtsp_log_value(value: &str) -> String {
    value.replace(['\r', '\n'], " ")
}

fn stream_rtsp_tcp(
    stream: &mut TcpStream,
    shared: Arc<FrameStore>,
    mode: NativeMode,
    channel: u8,
    ssrc: u32,
    session: &str,
    track_url: &str,
    service: &NetworkServiceInfo,
) -> io::Result<()> {
    match service.codec {
        NetworkCodec::Mjpeg => stream_rtsp_tcp_jpeg(
            stream,
            shared,
            mode,
            channel,
            ssrc,
            session,
            track_url,
            rtp_jpeg_quality(service.mjpeg_quality),
        ),
        NetworkCodec::H264 => {
            stream_rtsp_tcp_h264(stream, shared, mode, channel, ssrc, session, track_url)
        }
    }
}

fn stream_rtsp_udp(
    control_stream: &mut TcpStream,
    shared: Arc<FrameStore>,
    mode: NativeMode,
    transport: RtspUdpTransport,
    session: &str,
    track_url: &str,
    service: &NetworkServiceInfo,
) -> io::Result<()> {
    match service.codec {
        NetworkCodec::Mjpeg => stream_rtsp_udp_jpeg(
            control_stream,
            shared,
            mode,
            transport,
            session,
            track_url,
            rtp_jpeg_quality(service.mjpeg_quality),
        ),
        NetworkCodec::H264 => {
            stream_rtsp_udp_h264(control_stream, shared, mode, transport, session, track_url)
        }
    }
}

fn stream_rtsp_tcp_h264(
    stream: &mut TcpStream,
    shared: Arc<FrameStore>,
    mode: NativeMode,
    channel: u8,
    ssrc: u32,
    session: &str,
    track_url: &str,
) -> io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_millis(1)))?;
    let mut last_sequence = None;
    let mut rtp_sequence = 1u16;
    let mut rtp_timestamp = 0u32;
    let timestamp_step = rtp_frame_delta_for_fps(mode.fps_cap).max(1);
    let mut need_sync = true;
    let mut control_pending = Vec::new();
    let mut rtp_stats = RtpSenderStats::default();
    let rtcp_channel = channel.saturating_add(1);
    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        if !service_rtsp_tcp_control(stream, session, track_url, &mut control_pending)? {
            return Ok(());
        }
        let frame = if need_sync {
            let after_sequence = shared.latest_sequence();
            let Some(frame) =
                shared.wait_for_next_h264_sync(after_sequence, Duration::from_secs(2))
            else {
                continue;
            };
            frame
        } else {
            let Some(frame) = shared.wait_for_frame(last_sequence, Duration::from_millis(250))
            else {
                continue;
            };
            frame
        };
        if last_sequence == Some(frame.sequence) {
            continue;
        }
        last_sequence = Some(frame.sequence);
        need_sync = false;
        let frame_rtp_timestamp = rtp_timestamp;
        write_rtp_h264_access_unit_packets(
            &frame.data,
            frame_rtp_timestamp,
            &mut rtp_sequence,
            ssrc,
            |packet| {
                rtp_stats.record_packet(packet);
                write_interleaved_rtsp_frame(stream, channel, packet)
            },
        )?;
        if rtp_stats.rtcp_due(false) {
            write_interleaved_rtcp_sender_report(
                stream,
                rtcp_channel,
                ssrc,
                frame_rtp_timestamp,
                &rtp_stats,
            )?;
            rtp_stats.mark_rtcp_sent();
        }
        rtp_timestamp = rtp_timestamp.wrapping_add(timestamp_step);
    }
    Ok(())
}

fn stream_rtsp_udp_h264(
    control_stream: &mut TcpStream,
    shared: Arc<FrameStore>,
    mode: NativeMode,
    transport: RtspUdpTransport,
    session: &str,
    _track_url: &str,
) -> io::Result<()> {
    control_stream.set_read_timeout(Some(Duration::from_millis(20)))?;
    let mut last_sequence = None;
    let mut rtp_sequence = 1u16;
    let mut rtp_timestamp = 0u32;
    let timestamp_step = rtp_frame_delta_for_fps(mode.fps_cap).max(1);
    let mut control_buf = [0u8; 1024];
    let mut frames_sent = 0u64;
    let mut need_sync = true;
    let mut rtp_stats = RtpSenderStats::default();
    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        match control_stream.read(&mut control_buf) {
            Ok(0) => return Ok(()),
            Ok(n) => {
                if let Some(request) = parse_inline_rtsp_control(&control_buf[..n]) {
                    if request.method.eq_ignore_ascii_case("TEARDOWN") {
                        write_rtsp_response(
                            control_stream,
                            &request.cseq,
                            "200 OK",
                            &[("Session", session.to_string())],
                            None,
                            b"",
                        )?;
                        return Ok(());
                    }
                    if request.method.eq_ignore_ascii_case("GET_PARAMETER") {
                        write_rtsp_response(
                            control_stream,
                            &request.cseq,
                            "200 OK",
                            &[("Session", session.to_string())],
                            None,
                            b"",
                        )?;
                        continue;
                    }
                    if request.method.eq_ignore_ascii_case("OPTIONS") {
                        write_rtsp_response(
                            control_stream,
                            &request.cseq,
                            "200 OK",
                            &[("Public", RTSP_PUBLIC_METHODS.to_string())],
                            None,
                            b"",
                        )?;
                        continue;
                    }
                    if request.method.eq_ignore_ascii_case("PLAY") {
                        write_rtsp_response(
                            control_stream,
                            &request.cseq,
                            "200 OK",
                            &[
                                ("Session", session.to_string()),
                                ("Range", "npt=0-".to_string()),
                            ],
                            None,
                            b"",
                        )?;
                    }
                }
            }
            Err(err)
                if err.kind() == io::ErrorKind::WouldBlock
                    || err.kind() == io::ErrorKind::TimedOut => {}
            Err(err) => return Err(err),
        }
        let frame = if need_sync {
            let after_sequence = shared.latest_sequence();
            let Some(frame) =
                shared.wait_for_next_h264_sync(after_sequence, Duration::from_secs(2))
            else {
                continue;
            };
            frame
        } else {
            let Some(frame) = shared.wait_for_frame(last_sequence, Duration::from_millis(250))
            else {
                continue;
            };
            frame
        };
        if last_sequence == Some(frame.sequence) {
            continue;
        }
        last_sequence = Some(frame.sequence);
        need_sync = false;
        let frame_rtp_timestamp = rtp_timestamp;
        write_rtp_h264_access_unit_packets(
            &frame.data,
            frame_rtp_timestamp,
            &mut rtp_sequence,
            transport.ssrc,
            |packet| {
                transport.rtp_socket.send_to(packet, transport.target_rtp)?;
                rtp_stats.record_packet(packet);
                Ok(())
            },
        )?;
        if rtp_stats.rtcp_due(false) {
            send_udp_rtcp_sender_report(&transport, frame_rtp_timestamp, &rtp_stats)?;
            rtp_stats.mark_rtcp_sent();
        }
        frames_sent = frames_sent.saturating_add(1);
        if frames_sent == 1 || frames_sent == 30 {
            println!(
                "[netcam] RTSP UDP H.264 sent frame={} target={} sequence={}",
                frames_sent, transport.target_rtp, frame.sequence
            );
        }
        rtp_timestamp = rtp_timestamp.wrapping_add(timestamp_step);
    }
    Ok(())
}

fn rtp_frame_delta_for_fps(fps: u32) -> u32 {
    90_000 / fps.max(1)
}

fn rtp_timestamp_for_frame(epoch: &mut Option<Instant>, frame: &FrameSnapshot) -> u32 {
    let epoch = match *epoch {
        Some(epoch) => epoch,
        None => {
            *epoch = Some(frame.received_at);
            frame.received_at
        }
    };
    let elapsed = frame.received_at.saturating_duration_since(epoch);
    ((elapsed.as_nanos().saturating_mul(90_000)) / 1_000_000_000) as u32
}

fn write_rtp_h264_access_unit_packets<F>(
    access_unit: &[u8],
    timestamp: u32,
    sequence: &mut u16,
    ssrc: u32,
    mut send_packet: F,
) -> io::Result<()>
where
    F: FnMut(&[u8]) -> io::Result<()>,
{
    let ranges = h264_nal_ranges(access_unit);
    if ranges.is_empty() {
        return Err(invalid_input("H.264 access unit has no Annex-B NAL units"));
    }
    for (idx, (start, end)) in ranges.iter().copied().enumerate() {
        let nal = &access_unit[start..end];
        let is_last_nal = idx + 1 == ranges.len();
        write_rtp_h264_nal_packets(
            nal,
            is_last_nal,
            timestamp,
            sequence,
            ssrc,
            &mut send_packet,
        )?;
    }
    Ok(())
}

fn write_rtp_h264_nal_packets<F>(
    nal: &[u8],
    marker_on_last: bool,
    timestamp: u32,
    sequence: &mut u16,
    ssrc: u32,
    send_packet: &mut F,
) -> io::Result<()>
where
    F: FnMut(&[u8]) -> io::Result<()>,
{
    const MAX_RTP_PAYLOAD: usize = 1200;
    if nal.is_empty() {
        return Ok(());
    }
    if nal.len() <= MAX_RTP_PAYLOAD {
        let mut packet = Vec::with_capacity(12 + nal.len());
        push_rtp_header(
            &mut packet,
            H264_RTP_PAYLOAD_TYPE,
            marker_on_last,
            timestamp,
            *sequence,
            ssrc,
        );
        packet.extend_from_slice(nal);
        send_packet(&packet)?;
        *sequence = sequence.wrapping_add(1);
        return Ok(());
    }

    let nal_header = nal[0];
    let fu_indicator = (nal_header & 0xe0) | 28;
    let nal_type = nal_header & 0x1f;
    let mut offset = 1usize;
    let max_chunk = MAX_RTP_PAYLOAD - 2;
    let mut first = true;
    while offset < nal.len() {
        let chunk_len = max_chunk.min(nal.len() - offset);
        let last = offset + chunk_len >= nal.len();
        let mut packet = Vec::with_capacity(12 + 2 + chunk_len);
        push_rtp_header(
            &mut packet,
            H264_RTP_PAYLOAD_TYPE,
            marker_on_last && last,
            timestamp,
            *sequence,
            ssrc,
        );
        packet.push(fu_indicator);
        packet.push((if first { 0x80 } else { 0 }) | (if last { 0x40 } else { 0 }) | nal_type);
        packet.extend_from_slice(&nal[offset..offset + chunk_len]);
        send_packet(&packet)?;
        *sequence = sequence.wrapping_add(1);
        first = false;
        offset += chunk_len;
    }
    Ok(())
}

fn push_rtp_header(
    packet: &mut Vec<u8>,
    payload_type: u8,
    marker: bool,
    timestamp: u32,
    sequence: u16,
    ssrc: u32,
) {
    packet.push(0x80);
    packet.push((if marker { 0x80 } else { 0 }) | payload_type);
    packet.extend_from_slice(&sequence.to_be_bytes());
    packet.extend_from_slice(&timestamp.to_be_bytes());
    packet.extend_from_slice(&ssrc.to_be_bytes());
}

#[derive(Default)]
struct RtpSenderStats {
    packets: u32,
    octets: u32,
    last_rtcp: Option<Instant>,
}

impl RtpSenderStats {
    fn record_packet(&mut self, packet: &[u8]) {
        self.packets = self.packets.wrapping_add(1);
        self.octets = self
            .octets
            .wrapping_add(packet.len().saturating_sub(12) as u32);
    }

    fn rtcp_due(&self, force: bool) -> bool {
        force
            || self
                .last_rtcp
                .map(|last| last.elapsed() >= Duration::from_secs(5))
                .unwrap_or(true)
    }

    fn mark_rtcp_sent(&mut self) {
        self.last_rtcp = Some(Instant::now());
    }
}

fn rtcp_sender_report(ssrc: u32, rtp_timestamp: u32, stats: &RtpSenderStats) -> [u8; 28] {
    let (ntp_msw, ntp_lsw) = ntp_timestamp_now();
    let mut packet = [0u8; 28];
    packet[0] = 0x80;
    packet[1] = 200;
    packet[2..4].copy_from_slice(&6u16.to_be_bytes());
    packet[4..8].copy_from_slice(&ssrc.to_be_bytes());
    packet[8..12].copy_from_slice(&ntp_msw.to_be_bytes());
    packet[12..16].copy_from_slice(&ntp_lsw.to_be_bytes());
    packet[16..20].copy_from_slice(&rtp_timestamp.to_be_bytes());
    packet[20..24].copy_from_slice(&stats.packets.to_be_bytes());
    packet[24..28].copy_from_slice(&stats.octets.to_be_bytes());
    packet
}

fn ntp_timestamp_now() -> (u32, u32) {
    const NTP_UNIX_EPOCH_OFFSET: u64 = 2_208_988_800;
    let duration = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let seconds = duration.as_secs().saturating_add(NTP_UNIX_EPOCH_OFFSET) as u32;
    let fraction = ((u64::from(duration.subsec_nanos())) << 32) / 1_000_000_000u64;
    (seconds, fraction as u32)
}

fn write_interleaved_rtcp_sender_report(
    stream: &mut TcpStream,
    channel: u8,
    ssrc: u32,
    rtp_timestamp: u32,
    stats: &RtpSenderStats,
) -> io::Result<()> {
    let report = rtcp_sender_report(ssrc, rtp_timestamp, stats);
    write_interleaved_rtsp_frame(stream, channel, &report)
}

fn send_udp_rtcp_sender_report(
    transport: &RtspUdpTransport,
    rtp_timestamp: u32,
    stats: &RtpSenderStats,
) -> io::Result<()> {
    let (Some(socket), Some(client_rtcp_port)) =
        (transport._rtcp_socket.as_ref(), transport.client_rtcp_port)
    else {
        return Ok(());
    };
    let target = SocketAddr::new(transport.target_rtp.ip(), client_rtcp_port);
    let report = rtcp_sender_report(transport.ssrc, rtp_timestamp, stats);
    socket.send_to(&report, target)?;
    Ok(())
}

fn stream_rtsp_tcp_jpeg(
    stream: &mut TcpStream,
    shared: Arc<FrameStore>,
    mode: NativeMode,
    channel: u8,
    ssrc: u32,
    session: &str,
    track_url: &str,
    rtp_quality: u8,
) -> io::Result<()> {
    stream.set_read_timeout(Some(Duration::from_millis(1)))?;
    let mut last_sequence = None;
    let mut rtp_sequence = 1u16;
    let mut rtp_epoch = None;
    let mut control_pending = Vec::new();
    let mut rtp_stats = RtpSenderStats::default();
    let rtcp_channel = channel.saturating_add(1);
    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        if !service_rtsp_tcp_control(stream, session, track_url, &mut control_pending)? {
            return Ok(());
        }
        let Some(frame) = shared.wait_for_frame(last_sequence, Duration::from_millis(250)) else {
            continue;
        };
        last_sequence = Some(frame.sequence);
        let frame_rtp_timestamp = rtp_timestamp_for_frame(&mut rtp_epoch, &frame);
        write_rtp_jpeg_packets(
            &frame.data,
            mode,
            frame_rtp_timestamp,
            &mut rtp_sequence,
            ssrc,
            rtp_quality,
            |packet| {
                rtp_stats.record_packet(packet);
                write_interleaved_rtsp_frame(stream, channel, packet)
            },
        )?;
        if rtp_stats.rtcp_due(false) {
            write_interleaved_rtcp_sender_report(
                stream,
                rtcp_channel,
                ssrc,
                frame_rtp_timestamp,
                &rtp_stats,
            )?;
            rtp_stats.mark_rtcp_sent();
        }
    }
    Ok(())
}

struct RtpJpegPayload {
    jpeg_type: u8,
    quantization_tables: Vec<u8>,
    restart_interval: Option<u16>,
    scan_data: Vec<u8>,
}

fn rtp_jpeg_dimensions_supported(width: u32, height: u32) -> bool {
    width.div_ceil(8) <= u8::MAX as u32 && height.div_ceil(8) <= u8::MAX as u32
}

fn rtp_jpeg_mode_supported(mode: NativeMode) -> bool {
    rtp_jpeg_dimensions_supported(mode.width, mode.height)
}

fn rtp_jpeg_quality(mjpeg_quality: u32) -> u8 {
    mjpeg_quality.clamp(1, 99) as u8
}

fn rtp_jpeg_packet_q(_payload: &RtpJpegPayload, fallback_quality: u8) -> u8 {
    /* Windows Network Camera accepts the static RFC2435 quality field more
     * reliably than q>=128 dynamic quantization-table headers.  lmi-isp uses
     * the standard JPEG base tables, so the advertised quality is the safer
     * interoperability choice here. */
    fallback_quality
}

fn rtp_jpeg_packet_type(payload: &RtpJpegPayload) -> u8 {
    if payload.restart_interval.is_some() {
        payload
            .jpeg_type
            .saturating_add(RTP_JPEG_RESTART_TYPE_OFFSET)
    } else {
        payload.jpeg_type
    }
}

fn stream_rtsp_udp_jpeg(
    control_stream: &mut TcpStream,
    shared: Arc<FrameStore>,
    mode: NativeMode,
    transport: RtspUdpTransport,
    session: &str,
    _track_url: &str,
    rtp_quality: u8,
) -> io::Result<()> {
    control_stream.set_read_timeout(Some(Duration::from_millis(20)))?;
    let mut last_sequence = None;
    let mut rtp_sequence = 1u16;
    let mut rtp_epoch = None;
    let mut control_buf = [0u8; 1024];
    let mut frames_sent = 0u64;
    let mut rtp_stats = RtpSenderStats::default();
    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        match control_stream.read(&mut control_buf) {
            Ok(0) => return Ok(()),
            Ok(n) => {
                if let Some(request) = parse_inline_rtsp_control(&control_buf[..n]) {
                    if request.method.eq_ignore_ascii_case("TEARDOWN") {
                        write_rtsp_response(
                            control_stream,
                            &request.cseq,
                            "200 OK",
                            &[("Session", session.to_string())],
                            None,
                            b"",
                        )?;
                        return Ok(());
                    }
                    if request.method.eq_ignore_ascii_case("GET_PARAMETER") {
                        write_rtsp_response(
                            control_stream,
                            &request.cseq,
                            "200 OK",
                            &[("Session", session.to_string())],
                            None,
                            b"",
                        )?;
                        continue;
                    }
                    if request.method.eq_ignore_ascii_case("OPTIONS") {
                        write_rtsp_response(
                            control_stream,
                            &request.cseq,
                            "200 OK",
                            &[("Public", RTSP_PUBLIC_METHODS.to_string())],
                            None,
                            b"",
                        )?;
                        continue;
                    }
                    if request.method.eq_ignore_ascii_case("PLAY") {
                        write_rtsp_response(
                            control_stream,
                            &request.cseq,
                            "200 OK",
                            &[
                                ("Session", session.to_string()),
                                ("Range", "npt=0-".to_string()),
                            ],
                            None,
                            b"",
                        )?;
                    }
                }
            }
            Err(err)
                if err.kind() == io::ErrorKind::WouldBlock
                    || err.kind() == io::ErrorKind::TimedOut => {}
            Err(err) => return Err(err),
        }
        let Some(frame) = shared.wait_for_frame(last_sequence, Duration::from_millis(250)) else {
            continue;
        };
        last_sequence = Some(frame.sequence);
        let frame_rtp_timestamp = rtp_timestamp_for_frame(&mut rtp_epoch, &frame);
        let mut frame_packets = 0u32;
        write_rtp_jpeg_packets(
            &frame.data,
            mode,
            frame_rtp_timestamp,
            &mut rtp_sequence,
            transport.ssrc,
            rtp_quality,
            |packet| {
                transport.rtp_socket.send_to(packet, transport.target_rtp)?;
                rtp_stats.record_packet(packet);
                frame_packets = frame_packets.saturating_add(1);
                if frame_packets % RTP_JPEG_UDP_PACKET_BURST == 0 {
                    thread::sleep(RTP_JPEG_UDP_PACKET_GAP);
                }
                Ok(())
            },
        )?;
        if rtp_stats.rtcp_due(false) {
            send_udp_rtcp_sender_report(&transport, frame_rtp_timestamp, &rtp_stats)?;
            rtp_stats.mark_rtcp_sent();
        }
        frames_sent = frames_sent.saturating_add(1);
        if frames_sent == 1 || frames_sent == 30 {
            println!(
                "[netcam] RTSP UDP sent frame={} target={} sequence={}",
                frames_sent, transport.target_rtp, frame.sequence
            );
        }
    }
    Ok(())
}

fn write_rtp_jpeg_packets<F>(
    jpeg: &[u8],
    mode: NativeMode,
    timestamp: u32,
    sequence: &mut u16,
    ssrc: u32,
    rtp_quality: u8,
    mut send_packet: F,
) -> io::Result<()>
where
    F: FnMut(&[u8]) -> io::Result<()>,
{
    let width_blocks = ((mode.width + 7) / 8) as usize;
    let height_blocks = ((mode.height + 7) / 8) as usize;
    if !rtp_jpeg_mode_supported(mode) {
        return Err(invalid_input(format!(
            "RTSP RTP/JPEG supports at most 2040x2040; current frame is {}x{}",
            mode.width, mode.height
        )));
    }
    let payload = parse_jpeg_for_rtp(jpeg)?;
    let jpeg_type = rtp_jpeg_packet_type(&payload);
    let jpeg_q = rtp_jpeg_packet_q(&payload, rtp_quality);
    let qtable_len = payload.quantization_tables.len();
    if qtable_len > u16::MAX as usize {
        return Err(invalid_input(
            "RTP/JPEG quantization table set is too large",
        ));
    }

    let mut offset = 0usize;
    while offset < payload.scan_data.len() {
        let mut jpeg_header_len = 8usize;
        if payload.restart_interval.is_some() {
            jpeg_header_len += 4;
        }
        let include_qtables = offset == 0 && jpeg_q >= 128 && qtable_len > 0;
        if include_qtables {
            jpeg_header_len += 4 + qtable_len;
        }
        if jpeg_header_len >= RTP_JPEG_MAX_PAYLOAD {
            return Err(invalid_input("RTP/JPEG header is too large"));
        }

        let chunk_len =
            (RTP_JPEG_MAX_PAYLOAD - jpeg_header_len).min(payload.scan_data.len() - offset);
        let last_fragment = offset + chunk_len >= payload.scan_data.len();
        let mut packet = Vec::with_capacity(12 + jpeg_header_len + chunk_len);
        push_rtp_header(
            &mut packet,
            RTP_JPEG_PAYLOAD_TYPE,
            last_fragment,
            timestamp,
            *sequence,
            ssrc,
        );
        packet.push(0);
        packet.push(((offset >> 16) & 0xff) as u8);
        packet.push(((offset >> 8) & 0xff) as u8);
        packet.push((offset & 0xff) as u8);
        packet.push(jpeg_type);
        packet.push(jpeg_q);
        packet.push(width_blocks as u8);
        packet.push(height_blocks as u8);
        if let Some(restart_interval) = payload.restart_interval {
            packet.extend_from_slice(&restart_interval.to_be_bytes());
            let restart_flags = 0xc000 | RTP_JPEG_WHOLE_FRAME_RESTART_COUNT;
            packet.extend_from_slice(&restart_flags.to_be_bytes());
        }
        if include_qtables {
            packet.push(0);
            packet.push(0);
            packet.extend_from_slice(&(qtable_len as u16).to_be_bytes());
            packet.extend_from_slice(&payload.quantization_tables);
        }
        packet.extend_from_slice(&payload.scan_data[offset..offset + chunk_len]);
        send_packet(&packet)?;
        *sequence = sequence.wrapping_add(1);
        offset += chunk_len;
    }
    Ok(())
}

fn write_interleaved_rtsp_frame(
    stream: &mut TcpStream,
    channel: u8,
    packet: &[u8],
) -> io::Result<()> {
    if packet.len() > u16::MAX as usize {
        return Err(invalid_input(
            "RTP packet too large for RTSP interleaved framing",
        ));
    }
    stream.write_all(&[b'$', channel])?;
    stream.write_all(&(packet.len() as u16).to_be_bytes())?;
    stream.write_all(packet)
}

fn parse_jpeg_for_rtp(jpeg: &[u8]) -> io::Result<RtpJpegPayload> {
    if !looks_like_jpeg(jpeg) {
        return Err(invalid_input("RTSP frame is not a complete JPEG"));
    }
    let mut pos = 2usize;
    let mut jpeg_type = 1u8;
    let mut quantization_tables = Vec::new();
    let mut restart_interval = None;
    while pos + 4 <= jpeg.len() {
        if jpeg[pos] != 0xff {
            pos += 1;
            continue;
        }
        while pos < jpeg.len() && jpeg[pos] == 0xff {
            pos += 1;
        }
        if pos >= jpeg.len() {
            break;
        }
        let marker = jpeg[pos];
        pos += 1;
        if matches!(marker, 0x00 | 0x01 | 0xd0..=0xd9) {
            continue;
        }
        if pos + 2 > jpeg.len() {
            break;
        }
        let seg_len = u16::from_be_bytes([jpeg[pos], jpeg[pos + 1]]) as usize;
        if seg_len < 2 || pos + seg_len > jpeg.len() {
            return Err(invalid_input("malformed JPEG segment length"));
        }
        let seg_start = pos + 2;
        let seg_end = pos + seg_len;
        let segment = &jpeg[seg_start..seg_end];
        match marker {
            0xdb => append_rtp_jpeg_quantization_tables(segment, &mut quantization_tables)?,
            0xdd => {
                if segment.len() >= 2 {
                    let interval = u16::from_be_bytes([segment[0], segment[1]]);
                    if interval != 0 {
                        restart_interval = Some(interval);
                    }
                }
            }
            0xc0 => {
                if segment.len() >= 9 && segment[5] >= 3 {
                    let y_sampling = segment[7];
                    jpeg_type = if y_sampling == 0x22 { 1 } else { 0 };
                }
            }
            0xda => {
                /* Keep the final EOI marker in the RTP/JPEG payload.  Many
                 * depayloaders synthesize it, but Windows Network Camera can
                 * pass MJPG samples through DirectShow without appending EOI;
                 * including it keeps the exposed MJPEG frame self-contained. */
                let scan_data = jpeg[seg_end..].to_vec();
                if scan_data.is_empty() {
                    return Err(invalid_input("JPEG scan data is empty"));
                }
                return Ok(RtpJpegPayload {
                    jpeg_type,
                    quantization_tables,
                    restart_interval,
                    scan_data,
                });
            }
            _ => {}
        }
        pos = seg_end;
    }
    Err(invalid_input("JPEG SOS marker not found"))
}

fn append_rtp_jpeg_quantization_tables(segment: &[u8], out: &mut Vec<u8>) -> io::Result<()> {
    let mut pos = 0usize;
    while pos < segment.len() {
        let table_info = segment[pos];
        pos += 1;
        let precision = table_info >> 4;
        let table_len = match precision {
            0 => 64,
            1 => 128,
            _ => return Err(invalid_input("invalid JPEG quantization table precision")),
        };
        if pos + table_len > segment.len() {
            return Err(invalid_input("truncated JPEG quantization table"));
        }
        if precision != 0 {
            return Err(invalid_input(
                "RTSP RTP/JPEG only supports 8-bit JPEG quantization tables",
            ));
        }
        out.extend_from_slice(&segment[pos..pos + table_len]);
        pos += table_len;
    }
    Ok(())
}

fn spawn_ws_discovery(
    mode: NativeMode,
    service: NetworkServiceInfo,
) -> io::Result<JoinHandle<io::Result<()>>> {
    let socket = UdpSocket::bind(&service.onvif_listen)?;
    socket.set_nonblocking(true)?;
    if let Ok(SocketAddr::V4(_)) = socket.local_addr() {
        if let Err(err) = socket.set_multicast_ttl_v4(1) {
            println!("[netcam] WS-Discovery multicast ttl warning: {err}");
        }
        if let Err(err) = socket.set_multicast_loop_v4(false) {
            println!("[netcam] WS-Discovery multicast loop warning: {err}");
        }
        if let Err(err) =
            socket.join_multicast_v4(&Ipv4Addr::new(239, 255, 255, 250), &Ipv4Addr::UNSPECIFIED)
        {
            println!("[netcam] WS-Discovery multicast join warning: {err}");
        }
    }
    println!(
        "[netcam] WS-Discovery listening on {}",
        socket.local_addr()?
    );
    Ok(thread::spawn(move || {
        send_ws_discovery_hello(&socket, mode, &service);
        ws_discovery_loop(socket, mode, service)
    }))
}

fn ws_discovery_loop(
    socket: UdpSocket,
    mode: NativeMode,
    service: NetworkServiceInfo,
) -> io::Result<()> {
    let mut buf = [0u8; 8192];
    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        match socket.recv_from(&mut buf) {
            Ok((n, peer)) => {
                let request = String::from_utf8_lossy(&buf[..n]);
                let is_probe = ws_discovery_request_contains(&request, "Probe");
                let is_resolve = ws_discovery_request_contains(&request, "Resolve");
                if !is_probe && !is_resolve {
                    continue;
                }
                let relates_to = extract_xml_text(&request, "MessageID")
                    .unwrap_or_else(|| format!("urn:uuid:{}", service.onvif_uuid));
                let xaddr = ws_device_xaddr(&service, peer);
                let ws_2009 = ws_discovery_uses_2009(&request);
                let wsa_2005 = ws_discovery_uses_wsa_2005(&request) || ws_2009;
                let kind = if is_resolve { "Resolve" } else { "Probe" };
                let ws_version = if ws_2009 { "2009" } else { "2005" };
                let wsa_version = if wsa_2005 { "2005" } else { "2004" };
                println!(
                    "[netcam] WS-Discovery {kind} ws={ws_version} wsa={wsa_version} from {peer} bytes={} xaddr={}",
                    n,
                    rtsp_log_value(&xaddr)
                );
                let response = if is_resolve {
                    ws_discovery_resolve_match(
                        &service,
                        mode,
                        &xaddr,
                        &relates_to,
                        ws_2009,
                        wsa_2005,
                    )
                } else {
                    ws_discovery_probe_match(&service, mode, &xaddr, &relates_to, ws_2009, wsa_2005)
                };
                match socket.send_to(response.as_bytes(), peer) {
                    Ok(sent) => {
                        println!("[netcam] WS-Discovery {kind} reply to {peer} bytes={sent}")
                    }
                    Err(err) => println!("[netcam] WS-Discovery reply to {peer} failed: {err}"),
                }
            }
            Err(err) if err.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(60));
            }
            Err(err) => return Err(err),
        }
    }
    Ok(())
}

fn ws_discovery_request_contains(request: &str, local_name: &str) -> bool {
    request
        .split(|ch: char| !(ch.is_ascii_alphanumeric() || ch == ':' || ch == '_'))
        .any(|token| {
            token
                .rsplit_once(':')
                .map(|(_, local)| local)
                .unwrap_or(token)
                == local_name
        })
}

fn send_ws_discovery_hello(socket: &UdpSocket, mode: NativeMode, service: &NetworkServiceInfo) {
    let multicast = SocketAddr::from((Ipv4Addr::new(239, 255, 255, 250), 3702));
    let xaddr = ws_device_xaddr(service, multicast);
    let hello = ws_discovery_hello(service, mode, &xaddr);
    match socket.send_to(hello.as_bytes(), multicast) {
        Ok(sent) => println!(
            "[netcam] WS-Discovery Hello ws=2005 bytes={sent} xaddr={}",
            rtsp_log_value(&xaddr)
        ),
        Err(err) => println!("[netcam] WS-Discovery Hello ws=2005 failed: {err}"),
    }
}

fn send_ws_discovery_bye(service: &NetworkServiceInfo) {
    let Ok(socket) = UdpSocket::bind("0.0.0.0:0") else {
        return;
    };
    let _ = socket.set_multicast_ttl_v4(1);
    let _ = socket.set_multicast_loop_v4(false);
    let bye = ws_discovery_bye(service);
    match socket.send_to(bye.as_bytes(), (Ipv4Addr::new(239, 255, 255, 250), 3702)) {
        Ok(sent) => println!("[netcam] WS-Discovery Bye ws=2005 bytes={sent}"),
        Err(err) => println!("[netcam] WS-Discovery Bye ws=2005 failed: {err}"),
    }
}

fn ws_device_xaddr(service: &NetworkServiceInfo, peer: SocketAddr) -> String {
    let port = listen_port(&service.http_listen).unwrap_or("8080");
    let ip = local_ip_for_peer(peer).unwrap_or(IpAddr::V4(Ipv4Addr::LOCALHOST));
    let host = match ip {
        IpAddr::V4(addr) => addr.to_string(),
        IpAddr::V6(addr) => format!("[{addr}]"),
    };
    format!("http://{host}:{port}/onvif/device_service")
}

fn local_ip_for_peer(peer: SocketAddr) -> Option<IpAddr> {
    let bind_addr = if peer.is_ipv4() {
        "0.0.0.0:0"
    } else {
        "[::]:0"
    };
    let socket = UdpSocket::bind(bind_addr).ok()?;
    socket.connect(peer).ok()?;
    Some(socket.local_addr().ok()?.ip())
}

fn ws_discovery_probe_match(
    service: &NetworkServiceInfo,
    mode: NativeMode,
    xaddr: &str,
    relates_to: &str,
    ws_2009: bool,
    wsa_2005: bool,
) -> String {
    let action = if ws_2009 {
        "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01/ProbeMatches"
    } else {
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches"
    };
    ws_discovery_envelope(
        service,
        action,
        relates_to,
        &format!(
            concat!(
                "<d:ProbeMatches><d:ProbeMatch>",
                "{}",
                "</d:ProbeMatch></d:ProbeMatches>"
            ),
            ws_discovery_match_body(service, mode, xaddr, ws_2009)
        ),
        ws_2009,
        wsa_2005,
    )
}

fn ws_discovery_resolve_match(
    service: &NetworkServiceInfo,
    mode: NativeMode,
    xaddr: &str,
    relates_to: &str,
    ws_2009: bool,
    wsa_2005: bool,
) -> String {
    let action = if ws_2009 {
        "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01/ResolveMatches"
    } else {
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/ResolveMatches"
    };
    ws_discovery_envelope(
        service,
        action,
        relates_to,
        &format!(
            concat!(
                "<d:ResolveMatches><d:ResolveMatch>",
                "{}",
                "</d:ResolveMatch></d:ResolveMatches>"
            ),
            ws_discovery_match_body(service, mode, xaddr, ws_2009)
        ),
        ws_2009,
        wsa_2005,
    )
}

fn ws_discovery_hello(service: &NetworkServiceInfo, mode: NativeMode, xaddr: &str) -> String {
    ws_discovery_envelope(
        service,
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello",
        "",
        &format!(
            concat!("<d:Hello>", "{}", "</d:Hello>"),
            ws_discovery_match_body(service, mode, xaddr, false)
        ),
        false,
        false,
    )
}

fn ws_discovery_bye(service: &NetworkServiceInfo) -> String {
    ws_discovery_envelope(
        service,
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/Bye",
        "",
        &format!(
            concat!(
                "<d:Bye>",
                "<a:EndpointReference><a:Address>urn:uuid:{}</a:Address></a:EndpointReference>",
                "</d:Bye>"
            ),
            xml_text(&service.onvif_uuid)
        ),
        false,
        false,
    )
}

fn ws_discovery_envelope(
    service: &NetworkServiceInfo,
    action: &str,
    relates_to: &str,
    body: &str,
    ws_2009: bool,
    wsa_2005: bool,
) -> String {
    let addressing_ns = if wsa_2005 {
        "http://www.w3.org/2005/08/addressing"
    } else {
        "http://schemas.xmlsoap.org/ws/2004/08/addressing"
    };
    let discovery_ns = if ws_2009 {
        "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01"
    } else {
        "http://schemas.xmlsoap.org/ws/2005/04/discovery"
    };
    let anonymous_to = if wsa_2005 {
        "http://www.w3.org/2005/08/addressing/anonymous"
    } else {
        "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
    };
    let is_response = !relates_to.trim().is_empty();
    let to = if is_response {
        anonymous_to
    } else if ws_2009 {
        "urn:docs-oasis-open-org:ws-dd:ns:discovery:2009:01"
    } else {
        "urn:schemas-xmlsoap-org:ws:2005:04:discovery"
    };
    format!(
        concat!(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" ",
            "xmlns:a=\"{}\" ",
            "xmlns:d=\"{}\" ",
            "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" ",
            "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">",
            "<s:Header>",
            "<a:To>{}</a:To>",
            "<a:Action>{}</a:Action>",
            "<a:MessageID>urn:uuid:{}</a:MessageID>",
            "{}",
            "{}",
            "</s:Header><s:Body>{}</s:Body></s:Envelope>"
        ),
        addressing_ns,
        discovery_ns,
        xml_text(to),
        xml_text(action),
        xml_text(&ws_message_uuid(service)),
        relates_to
            .trim()
            .is_empty()
            .then(String::new)
            .unwrap_or_else(|| { format!("<a:RelatesTo>{}</a:RelatesTo>", xml_text(relates_to)) }),
        ws_discovery_app_sequence(service),
        body
    )
}

fn ws_discovery_app_sequence(service: &NetworkServiceInfo) -> String {
    let instance_id = ws_discovery_instance_id();
    let message_number = WS_DISCOVERY_MESSAGE_NUMBER.fetch_add(1, Ordering::SeqCst);
    format!(
        concat!(
            "<d:AppSequence InstanceId=\"{}\" ",
            "SequenceId=\"urn:uuid:{}\" MessageNumber=\"{}\"/>"
        ),
        instance_id,
        xml_text(&service.onvif_uuid),
        message_number
    )
}

fn ws_discovery_instance_id() -> u64 {
    let existing = WS_DISCOVERY_INSTANCE_ID.load(Ordering::SeqCst);
    if existing != 0 {
        return existing;
    }
    let seconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or(1)
        .max(1);
    match WS_DISCOVERY_INSTANCE_ID.compare_exchange(0, seconds, Ordering::SeqCst, Ordering::SeqCst)
    {
        Ok(_) => seconds,
        Err(value) => value,
    }
}

fn ws_message_uuid(service: &NetworkServiceInfo) -> String {
    let millis = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis())
        .unwrap_or_default();
    let mut seed = millis ^ 0x6c6d_6900_0d15_cafe_babe_8250_0b13_0010u128;
    for byte in service.onvif_uuid.bytes() {
        seed = seed.wrapping_mul(131).wrapping_add(byte as u128);
    }
    let a = (seed >> 96) as u32;
    let b = (seed >> 80) as u16;
    let c = ((seed >> 64) as u16) & 0x0fff;
    let d = ((seed >> 48) as u16) & 0x0fff;
    let e = seed & 0xffff_ffff_ffff;
    format!("{a:08x}-{b:04x}-4{c:03x}-8{d:03x}-{e:012x}")
}

fn ws_discovery_uses_2009(request: &str) -> bool {
    request.contains("http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01")
        || request.contains("urn:docs-oasis-open-org:ws-dd:ns:discovery:2009:01")
}

fn ws_discovery_uses_wsa_2005(request: &str) -> bool {
    request.contains("http://www.w3.org/2005/08/addressing")
}

fn ws_discovery_match_body(
    service: &NetworkServiceInfo,
    mode: NativeMode,
    xaddr: &str,
    ws_2009: bool,
) -> String {
    let scopes = ws_discovery_scopes(service).join(" ");
    let metadata_version = ws_discovery_metadata_version(service, mode);
    let scope_attr = if ws_2009 {
        " MatchBy=\"http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01/rfc3986\""
    } else {
        ""
    };
    format!(
        concat!(
            "<a:EndpointReference><a:Address>urn:uuid:{}</a:Address></a:EndpointReference>",
            "<d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types>",
            "<d:Scopes{}>{}</d:Scopes>",
            "<d:XAddrs>{}</d:XAddrs>",
            "<d:MetadataVersion>{}</d:MetadataVersion>"
        ),
        xml_text(&service.onvif_uuid),
        scope_attr,
        xml_text(&scopes),
        xml_text(xaddr),
        metadata_version
    )
}

fn ws_discovery_scopes(service: &NetworkServiceInfo) -> Vec<String> {
    vec![
        "onvif://www.onvif.org/type/video_encoder".to_string(),
        "onvif://www.onvif.org/type/NetworkVideoTransmitter".to_string(),
        "onvif://www.onvif.org/Profile/Streaming".to_string(),
        "onvif://www.onvif.org/Profile/S".to_string(),
        format!(
            "onvif://www.onvif.org/name/{}",
            scope_escape(&onvif_name(service))
        ),
        format!(
            "onvif://www.onvif.org/location/{}",
            scope_escape(&onvif_hostname(service))
        ),
        "onvif://www.onvif.org/hardware/SM8250-OV13B10".to_string(),
    ]
}

fn ws_discovery_metadata_version(service: &NetworkServiceInfo, mode: NativeMode) -> u32 {
    let mut hash = 0x811c_9dc5u32;
    for byte in service
        .onvif_uuid
        .bytes()
        .chain(service.onvif_name.bytes())
        .chain(service.rtsp_path.bytes())
        .chain(service.codec.name().bytes())
    {
        hash ^= byte as u32;
        hash = hash.wrapping_mul(0x0100_0193);
    }
    hash ^= mode.width;
    hash = hash.wrapping_mul(0x0100_0193);
    hash ^= mode.height;
    hash = hash.wrapping_mul(0x0100_0193);
    hash ^= mode.fps_cap;
    hash = hash.wrapping_mul(0x0100_0193);
    hash ^= if service.rtsp_listen.is_some() {
        0x7274_7370
    } else {
        0x6874_7470
    };
    (hash & 0x7fff_ffff).max(1)
}

fn extract_xml_text(body: &str, local_name: &str) -> Option<String> {
    let needle = format!(":{local_name}");
    let start = body
        .find(&needle)
        .or_else(|| body.find(&format!("<{local_name}")))?;
    let after_open = body[start..].find('>')? + start + 1;
    let end = body[after_open..].find('<')? + after_open;
    Some(body[after_open..end].trim().to_string())
}

fn normalize_path(value: &str) -> String {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return "/stream.mjpg".to_string();
    }
    let without_query = trimmed
        .split_once('?')
        .map(|(path, _)| path)
        .unwrap_or(trimmed);
    if without_query.starts_with('/') {
        without_query.to_string()
    } else {
        format!("/{without_query}")
    }
}

fn host_with_port(http_host: &str, listen: &str) -> String {
    let port = listen_port(listen).unwrap_or("8554");
    let host = strip_host_port(http_host.trim());
    format_host_port(host, port)
}

fn listen_port(listen: &str) -> Option<&str> {
    let trimmed = listen.trim();
    if trimmed.starts_with('[') {
        let end = trimmed.find(']')?;
        return trimmed[end + 1..]
            .strip_prefix(':')
            .filter(|port| !port.is_empty());
    }
    trimmed
        .rsplit_once(':')
        .map(|(_, port)| port)
        .filter(|port| !port.is_empty() && port.chars().all(|ch| ch.is_ascii_digit()))
}

fn strip_host_port(host: &str) -> &str {
    if host.starts_with('[') {
        if let Some(end) = host.find(']') {
            return &host[..=end];
        }
    }
    if host.matches(':').count() == 1 {
        host.rsplit_once(':').map(|(name, _)| name).unwrap_or(host)
    } else {
        host
    }
}

fn format_host_port(host: &str, port: &str) -> String {
    if host.starts_with('[') {
        format!("{host}:{port}")
    } else if host.contains(':') {
        format!("[{host}]:{port}")
    } else {
        format!("{host}:{port}")
    }
}

fn xml_text(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for ch in value.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&apos;"),
            ch => out.push(ch),
        }
    }
    out
}

fn scope_escape(value: &str) -> String {
    let mut out = String::new();
    for byte in value.bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(byte as char)
            }
            b' ' => out.push_str("%20"),
            other => out.push_str(&format!("%{other:02X}")),
        }
    }
    out
}

fn apply_control_command(path: &Path, command: &str) {
    let deadline = Instant::now() + CONTROL_WRITE_GRACE;
    loop {
        match OpenOptions::new()
            .write(true)
            .custom_flags(O_NONBLOCK)
            .open(path)
        {
            Ok(mut fifo) => {
                if let Err(err) = writeln!(fifo, "{command}") {
                    println!(
                        "[netcam] failed to write control '{}' to {}: {}",
                        command,
                        path.display(),
                        err
                    );
                }
                return;
            }
            Err(err)
                if err.kind() == io::ErrorKind::WouldBlock || err.raw_os_error() == Some(6) =>
            {
                if Instant::now() >= deadline {
                    println!(
                        "[netcam] deferred control '{}' because {} has no reader",
                        command,
                        path.display()
                    );
                    return;
                }
                thread::sleep(Duration::from_millis(20));
            }
            Err(err) => {
                println!(
                    "[netcam] failed to open control fifo {} for '{}': {}",
                    path.display(),
                    command,
                    err
                );
                return;
            }
        }
    }
}

fn prepare_parent_dir(path: &Path) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    Ok(())
}

fn prepare_fifo(path: &Path) -> io::Result<()> {
    match fs::symlink_metadata(path) {
        Ok(meta) if meta.file_type().is_fifo() => fs::remove_file(path)?,
        Ok(_) => {
            return Err(invalid_input(format!(
                "refusing to replace non-FIFO path {}",
                path.display()
            )));
        }
        Err(err) if err.kind() == io::ErrorKind::NotFound => {}
        Err(err) => return Err(err),
    }
    let c_path = CString::new(path.as_os_str().as_encoded_bytes()).map_err(|_| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("path contains NUL byte: {}", path.display()),
        )
    })?;
    let ret = unsafe { mkfifo(c_path.as_ptr(), 0o600) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn install_signal_handlers() {
    STOP_REQUESTED.store(false, Ordering::SeqCst);
    unsafe {
        signal(SIGINT, handle_signal);
        signal(SIGTERM, handle_signal);
    }
}

extern "C" fn handle_signal(_: c_int) {
    STOP_REQUESTED.store(true, Ordering::SeqCst);
}

fn join_result<T>(name: &str, result: thread::Result<io::Result<T>>) -> io::Error {
    match result {
        Ok(Ok(_)) => io::Error::other(format!("{name} exited unexpectedly")),
        Ok(Err(err)) => io::Error::other(format!("{name} failed: {err}")),
        Err(_) => io::Error::other(format!("{name} panicked")),
    }
}

fn next_value<I>(args: &mut std::iter::Peekable<I>, flag: &str) -> io::Result<String>
where
    I: Iterator<Item = String>,
{
    args.next()
        .ok_or_else(|| invalid_input(format!("missing value for {flag}")))
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

fn parse_output_size(value: &str, field: &str) -> io::Result<(u32, u32)> {
    let (width, height) = value
        .split_once('x')
        .or_else(|| value.split_once('X'))
        .ok_or_else(|| invalid_input(format!("invalid {field} value '{value}', expected WxH")))?;
    let width = parse_u32(width.trim(), field)?;
    let height = parse_u32(height.trim(), field)?;
    validate_network_output_size(width, height)?;
    Ok((width, height))
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

fn parse_f32(value: &str, field: &str) -> io::Result<f32> {
    value
        .parse::<f32>()
        .map_err(|_| invalid_input(format!("invalid {field} value '{value}'")))
}

fn parse_network_codec(value: &str, field: &str) -> io::Result<NetworkCodec> {
    match value.to_ascii_lowercase().as_str() {
        "mjpeg" | "jpeg" => Ok(NetworkCodec::Mjpeg),
        "h264" | "h.264" | "avc" => Ok(NetworkCodec::H264),
        other => Err(invalid_input(format!(
            "invalid {field} value '{other}', expected mjpeg or h264"
        ))),
    }
}

fn resolve_venus_node(config: &mut NetworkRunConfig) -> io::Result<()> {
    if !config.venus_node.as_os_str().is_empty() {
        return Ok(());
    }

    let nodes = devices::list_video_nodes()?;
    let Some(node) = devices::default_venus_encoder_node(&nodes) else {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            "could not find qcom-venus-encoder video node; load Venus modules/firmware or pass --venus-node",
        ));
    };
    config.venus_node = node.path.clone();
    println!(
        "[venus] auto-selected encoder node {}",
        config.venus_node.display()
    );
    Ok(())
}

fn h264_profile_level_id_from_config(config: &NetworkRunConfig) -> String {
    let profile_idc = match config.h264_profile.to_ascii_lowercase().as_str() {
        "baseline" | "constrained-baseline" => 0x42,
        "main" => 0x4d,
        "high" => 0x64,
        _ => 0x64,
    };
    let level_idc = h264_level_idc(&config.h264_level).unwrap_or(0x33);
    format!("{profile_idc:02X}00{level_idc:02X}")
}

fn h264_level_idc(level: &str) -> Option<u8> {
    let trimmed = level.trim();
    let (major, minor) = match trimmed.split_once('.') {
        Some((major, minor)) => (major.parse::<u8>().ok()?, minor.parse::<u8>().ok()?),
        None => {
            let raw = trimmed.parse::<u8>().ok()?;
            if raw >= 10 {
                return Some(raw);
            }
            (raw, 0)
        }
    };
    major.checked_mul(10)?.checked_add(minor)
}

fn h264_profile_level_id_from_sps(sps: &[u8]) -> Option<String> {
    if sps.len() < 4 || (sps[0] & 0x1f) != 7 {
        return None;
    }
    Some(format!("{:02X}{:02X}{:02X}", sps[1], sps[2], sps[3]))
}

fn base64_encode(input: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(input.len().div_ceil(3) * 4);
    let mut i = 0usize;
    while i < input.len() {
        let b0 = input[i];
        let b1 = input.get(i + 1).copied().unwrap_or(0);
        let b2 = input.get(i + 2).copied().unwrap_or(0);
        out.push(TABLE[(b0 >> 2) as usize] as char);
        out.push(TABLE[(((b0 & 0x03) << 4) | (b1 >> 4)) as usize] as char);
        if i + 1 < input.len() {
            out.push(TABLE[(((b1 & 0x0f) << 2) | (b2 >> 6)) as usize] as char);
        } else {
            out.push('=');
        }
        if i + 2 < input.len() {
            out.push(TABLE[(b2 & 0x3f) as usize] as char);
        } else {
            out.push('=');
        }
        i += 3;
    }
    out
}

fn shell_quote(value: &str) -> String {
    if value.is_empty() {
        return "''".to_string();
    }
    if value.bytes().all(|b| {
        matches!(
            b,
            b'A'..=b'Z'
                | b'a'..=b'z'
                | b'0'..=b'9'
                | b'/'
                | b'.'
                | b'_'
                | b'-'
                | b':'
                | b','
                | b'='
                | b'+'
        )
    }) {
        return value.to_string();
    }
    let mut quoted = String::from("'");
    for ch in value.chars() {
        if ch == '\'' {
            quoted.push_str("'\\''");
        } else {
            quoted.push(ch);
        }
    }
    quoted.push('\'');
    quoted
}

fn terminate_child(child: &mut Child) -> io::Result<()> {
    if child.try_wait()?.is_some() {
        return Ok(());
    }
    let pid = child.id() as c_int;
    let ret = unsafe { kill(pid, SIGTERM) };
    if ret < 0 {
        let err = io::Error::last_os_error();
        if child.try_wait()?.is_none() {
            child.kill()?;
        }
        let _ = child.wait();
        return Err(err);
    }
    let deadline = Instant::now() + PROCESS_GRACE;
    while Instant::now() < deadline {
        if child.try_wait()?.is_some() {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(20));
    }
    if child.try_wait()?.is_none() {
        child.kill()?;
        let _ = child.wait();
    }
    Ok(())
}

fn invalid_input(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

fn json_bool(value: bool) -> &'static str {
    if value { "true" } else { "false" }
}

fn json_string(value: &str) -> String {
    let mut out = String::with_capacity(value.len() + 2);
    out.push('"');
    for ch in value.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c.is_control() => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_network_run_before_native_profile() {
        let args: Vec<String> = ["--output", "network", "--profile", "native-modes"]
            .into_iter()
            .map(String::from)
            .collect();
        assert!(looks_like_network_run(&args));
    }

    #[test]
    fn parses_network_defaults_and_frame_index() {
        let config = parse_network_run_config(
            [
                "--output",
                "network",
                "--profile",
                "native-modes",
                "--frame-index",
                "4",
                "--listen",
                "0.0.0.0:8081",
            ]
            .into_iter()
            .map(String::from),
        )
        .unwrap();
        assert_eq!(config.frame_index, 4);
        assert_eq!(config.out_width, None);
        assert_eq!(config.out_height, None);
        assert_eq!(config.listen, "0.0.0.0:8081");
        assert_eq!(
            config.fifo,
            PathBuf::from("/run/lmi-camera/lmi-netcam.fifo")
        );
        assert_eq!(config.fps_cap, None);
        assert_eq!(config.flicker.as_deref(), Some("auto"));
        assert_eq!(config.onvif_name, "LMI-OV13B10");
        assert!(config.rtsp_listen.is_none());
        assert!(!config.onvif);
    }

    #[test]
    fn parses_network_rtsp_and_onvif_options() {
        let config = parse_network_run_config(
            [
                "network",
                "--rtsp",
                "--rtsp-listen",
                "0.0.0.0:8555",
                "--rtsp-path",
                "camera",
                "--onvif",
                "--onvif-listen",
                "0.0.0.0:3703",
                "--onvif-uuid",
                "11111111-2222-3333-4444-555555555555",
                "--onvif-name",
                "test camera",
            ]
            .into_iter()
            .map(String::from),
        )
        .unwrap();
        assert_eq!(config.rtsp_listen.as_deref(), Some("0.0.0.0:8555"));
        assert_eq!(config.rtsp_path, "/camera");
        assert!(config.onvif);
        assert_eq!(config.onvif_listen, "0.0.0.0:3703");
        assert_eq!(config.onvif_uuid, "11111111-2222-3333-4444-555555555555");
        assert_eq!(config.onvif_name, "test camera");
    }

    #[test]
    fn parses_network_fps_cap_option() {
        let config =
            parse_network_run_config(["network", "--fps-cap", "24"].into_iter().map(String::from))
                .unwrap();
        assert_eq!(config.fps_cap, Some(24));

        let err = parse_network_run_config(["network", "--fps", "0"].into_iter().map(String::from))
            .unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
    }

    #[test]
    fn parses_network_output_size_option() {
        let config = parse_network_run_config(
            ["network", "--out-size", "1280x720"]
                .into_iter()
                .map(String::from),
        )
        .unwrap();
        assert_eq!(config.out_width, Some(1280));
        assert_eq!(config.out_height, Some(720));

        let err = parse_network_run_config(
            ["network", "--out-size", "1281x720"]
                .into_iter()
                .map(String::from),
        )
        .unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);

        let http_only = parse_network_run_config(
            ["network", "--out-size", "4096x2160"]
                .into_iter()
                .map(String::from),
        )
        .unwrap();
        assert_eq!(http_only.out_width, Some(4096));
        assert_eq!(http_only.out_height, Some(2160));

        let err = parse_network_run_config(
            ["network", "--rtsp", "--out-size", "4096x2160"]
                .into_iter()
                .map(String::from),
        )
        .unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
    }

    #[test]
    fn onvif_scopes_use_clean_default_name() {
        let mode = OV13B10_NATIVE_MODES[5];
        let service = NetworkServiceInfo::from_config(
            &NetworkRunConfig::default(),
            mode,
            Arc::new(ModeSwitchState::new(mode)),
        );
        let scopes = onvif_scopes(&service);
        assert!(
            scopes
                .iter()
                .any(|scope| scope == "onvif://www.onvif.org/name/LMI-OV13B10")
        );
        assert!(
            scopes
                .iter()
                .any(|scope| scope == "onvif://www.onvif.org/type/NetworkVideoTransmitter")
        );
    }

    #[test]
    fn onvif_extra_requests_return_supported_metadata() {
        let mode = OV13B10_NATIVE_MODES[5];
        let service = NetworkServiceInfo::from_config(
            &NetworkRunConfig::default(),
            mode,
            Arc::new(ModeSwitchState::new(mode)),
        );
        assert!(onvif_get_video_sources(mode, &service).contains("GetVideoSourcesResponse"));
        assert!(
            onvif_get_video_encoder_configurations(mode, &service)
                .contains("<tt:FrameRateLimit>120</tt:FrameRateLimit>")
        );
        let users = onvif_get_users(&service);
        assert!(users.contains("GetUsersResponse"));
        assert!(users.contains("Administrator"));
    }

    #[test]
    fn onvif_native_lists_expose_advertised_modes() {
        let current = OV13B10_NATIVE_MODES[5];
        let first = OV13B10_NATIVE_MODES[0];
        let mut config = NetworkRunConfig::default();
        config.codec = NetworkCodec::H264;
        let service = NetworkServiceInfo::from_config(
            &config,
            current,
            Arc::new(ModeSwitchState::new(current)),
        );

        let profiles = onvif_get_profiles(current, &service);
        assert!(profiles.contains(&onvif_profile_token(current, &service)));
        assert!(profiles.contains(&onvif_profile_token(first, &service)));

        let encoder_configs = onvif_get_video_encoder_configurations(current, &service);
        assert!(encoder_configs.contains(&onvif_encoder_token(current, &service)));
        assert!(encoder_configs.contains(&onvif_encoder_token(first, &service)));

        let source_configs = onvif_get_video_source_configurations(current, &service);
        assert!(source_configs.contains(&onvif_video_source_token(current)));
        assert!(source_configs.contains(&onvif_video_source_token(first)));

        let options = onvif_get_video_encoder_configuration_options(current, &service);
        assert!(options.contains(&onvif_resolution_xml(current)));
        assert!(options.contains(&onvif_resolution_xml(first)));
    }

    #[test]
    fn onvif_mjpeg_lists_only_rtp_jpeg_safe_modes() {
        let current = OV13B10_NATIVE_MODES[5];
        let first = OV13B10_NATIVE_MODES[0];
        let config = NetworkRunConfig::default();
        let service = NetworkServiceInfo::from_config(
            &config,
            current,
            Arc::new(ModeSwitchState::new(current)),
        );

        let profiles = onvif_get_profiles(current, &service);
        assert!(profiles.contains(&onvif_profile_token(current, &service)));
        assert!(!profiles.contains(&onvif_profile_token(first, &service)));
        assert_eq!(onvif_profile_count(&service), 1);
    }

    fn onvif_resolution_xml(mode: NativeMode) -> String {
        format!(
            "<tt:Width>{}</tt:Width><tt:Height>{}</tt:Height>",
            mode.width, mode.height
        )
    }

    #[test]
    fn rtsp_h264_sdp_uses_windows_compatible_media_metadata() {
        let mode = OV13B10_NATIVE_MODES[5];
        let mut config = NetworkRunConfig::default();
        config.codec = NetworkCodec::H264;
        let service =
            NetworkServiceInfo::from_config(&config, mode, Arc::new(ModeSwitchState::new(mode)));

        let sdp = rtsp_sdp("rtsp://192.168.0.41:8554/stream.h264", mode, &service);

        assert!(sdp.contains("m=video 0 RTP/AVP 96\r\n"));
        assert!(sdp.contains("a=framesize:96 1364-768\r\n"));
        assert!(sdp.contains("a=control:trackID=0\r\n"));
        assert!(!sdp.contains("m=video 49170 RTP/AVP"));
        assert!(!sdp.contains("a=type:broadcast"));
    }

    #[test]
    fn rtp_jpeg_payload_keeps_eoi_marker_for_self_contained_mjpg_samples() {
        let jpeg = [
            0xff, 0xd8, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03, 0x01, 0x22,
            0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00,
            0x00, 0x3f, 0x00, 0x12, 0x34, 0xff, 0xd9,
        ];

        let payload = parse_jpeg_for_rtp(&jpeg).unwrap();

        assert_eq!(
            payload.scan_data[payload.scan_data.len() - 2..],
            [0xff, 0xd9]
        );
    }

    #[test]
    fn onvif_set_encoder_configuration_requests_mode_switch() {
        let current = OV13B10_NATIVE_MODES[5];
        let requested = OV13B10_NATIVE_MODES[0];
        let mode_switch = Arc::new(ModeSwitchState::new(current));
        let mut config = NetworkRunConfig::default();
        config.codec = NetworkCodec::H264;
        let service = NetworkServiceInfo::from_config(&config, current, mode_switch.clone());
        let token = onvif_encoder_token(requested, &service);
        let body = format!(
            concat!(
                "<trt:SetVideoEncoderConfiguration>",
                "<trt:Configuration token=\"{}\"><tt:Encoding>H264</tt:Encoding>",
                "<tt:Resolution><tt:Width>{}</tt:Width><tt:Height>{}</tt:Height></tt:Resolution>",
                "</trt:Configuration></trt:SetVideoEncoderConfiguration>"
            ),
            token, requested.width, requested.height
        );

        let response = onvif_set_video_encoder_configuration(&body, current, &service);

        assert!(response.contains("SetVideoEncoderConfigurationResponse"));
        assert_eq!(mode_switch.take_requested(), Some(requested));
    }

    #[test]
    fn onvif_get_stream_uri_requests_selected_mode() {
        let current = OV13B10_NATIVE_MODES[5];
        let requested = OV13B10_NATIVE_MODES[4];
        let mode_switch = Arc::new(ModeSwitchState::new(current));
        let mut config = NetworkRunConfig::default();
        config.codec = NetworkCodec::H264;
        let service = NetworkServiceInfo::from_config(&config, current, mode_switch.clone());
        let body = format!(
            "<trt:GetStreamUri><trt:ProfileToken>{}</trt:ProfileToken></trt:GetStreamUri>",
            onvif_profile_token(requested, &service)
        );

        let response =
            onvif_get_stream_uri(&body, "rtsp://192.168.0.41:8554/stream.h264", &service);

        assert!(response.contains("GetStreamUriResponse"));
        assert_eq!(mode_switch.take_requested(), Some(requested));
    }

    #[test]
    fn onvif_out_size_advertises_only_the_active_scaled_stream() {
        let raw = OV13B10_NATIVE_MODES[5];
        let mut stream = raw;
        stream.width = 1280;
        stream.height = 720;
        stream.fps_cap = 30;
        stream.nominal_fps_milli = 30_000;
        let mode_switch = Arc::new(ModeSwitchState::new(stream));
        let mut config = NetworkRunConfig::default();
        config.out_width = Some(1280);
        config.out_height = Some(720);
        config.fps_cap = Some(30);
        let service = NetworkServiceInfo::from_config(&config, stream, mode_switch.clone());
        let body = format!(
            "<trt:GetStreamUri><trt:ProfileToken>{}</trt:ProfileToken></trt:GetStreamUri>",
            onvif_profile_token(stream, &service)
        );

        let response =
            onvif_get_stream_uri(&body, "rtsp://192.168.0.41:8554/stream.mjpg", &service);
        let profiles = onvif_get_profiles(stream, &service);
        let encoder_configs = onvif_get_video_encoder_configurations(stream, &service);
        let encoder_options = onvif_get_video_encoder_configuration_options(stream, &service);
        let source_configs = onvif_get_video_source_configurations(stream, &service);
        let sources = onvif_get_video_sources(stream, &service);

        assert!(response.contains("GetStreamUriResponse"));
        assert_eq!(mode_switch.take_requested(), None);
        assert_eq!(onvif_profile_count(&service), 1);
        for xml in [
            profiles.as_str(),
            encoder_configs.as_str(),
            encoder_options.as_str(),
            sources.as_str(),
        ] {
            assert!(xml.contains("<tt:Width>1280</tt:Width><tt:Height>720</tt:Height>"));
            assert!(!xml.contains("<tt:Width>640</tt:Width><tt:Height>360</tt:Height>"));
            assert!(!xml.contains("<tt:Width>1364</tt:Width><tt:Height>768</tt:Height>"));
        }
        assert!(
            source_configs.contains("<tt:Bounds x=\"0\" y=\"0\" width=\"1280\" height=\"720\"/>")
        );
        assert!(!source_configs.contains("width=\"640\" height=\"360\""));
        assert!(!source_configs.contains("width=\"1364\" height=\"768\""));
        assert!(onvif_profile_token(stream, &service).contains("1280x720-fps30"));
        assert!(onvif_encoder_token(stream, &service).contains("1280x720-fps30"));
        assert!(onvif_video_source_token(stream).contains("1280x720-fps30"));
        assert!(!profiles.contains(&onvif_profile_token(OV13B10_NATIVE_MODES[4], &service)));
    }

    #[test]
    fn onvif_scaled_tokens_change_with_stream_shape() {
        let raw = OV13B10_NATIVE_MODES[5];
        let mut stream_720 = raw;
        stream_720.width = 1280;
        stream_720.height = 720;
        stream_720.fps_cap = 30;
        let mut stream_360 = raw;
        stream_360.width = 640;
        stream_360.height = 360;
        stream_360.fps_cap = 30;
        let config = NetworkRunConfig::default();
        let service_720 = NetworkServiceInfo::from_config(
            &config,
            stream_720,
            Arc::new(ModeSwitchState::new(stream_720)),
        );
        let service_360 = NetworkServiceInfo::from_config(
            &config,
            stream_360,
            Arc::new(ModeSwitchState::new(stream_360)),
        );

        assert_ne!(
            onvif_profile_token(stream_720, &service_720),
            onvif_profile_token(stream_360, &service_360)
        );
        assert_ne!(
            onvif_encoder_token(stream_720, &service_720),
            onvif_encoder_token(stream_360, &service_360)
        );
        assert_ne!(
            onvif_video_source_token(stream_720),
            onvif_video_source_token(stream_360)
        );
    }

    #[test]
    fn rtcp_sender_report_counts_rtp_payload_bytes() {
        let mut stats = RtpSenderStats::default();
        stats.record_packet(&[0u8; 12 + 100]);
        stats.record_packet(&[0u8; 12 + 25]);

        let report = rtcp_sender_report(0x4c4d4931, 9000, &stats);

        assert_eq!(
            u32::from_be_bytes(report[4..8].try_into().unwrap()),
            0x4c4d4931
        );
        assert_eq!(u32::from_be_bytes(report[16..20].try_into().unwrap()), 9000);
        assert_eq!(u32::from_be_bytes(report[20..24].try_into().unwrap()), 2);
        assert_eq!(u32::from_be_bytes(report[24..28].try_into().unwrap()), 125);
    }

    #[test]
    fn parses_onvif_manual_imaging_controls() {
        let body = concat!(
            "<timg:SetImagingSettings>",
            "<timg:ImagingSettings><tt:Exposure>",
            "<tt:Mode>MANUAL</tt:Mode>",
            "<tt:ExposureTime>12000</tt:ExposureTime>",
            "<tt:Gain>8.5</tt:Gain>",
            "</tt:Exposure></timg:ImagingSettings>",
            "</timg:SetImagingSettings>"
        );
        assert!(matches!(
            parse_onvif_exposure_mode(body),
            Some(OnvifExposureMode::Manual)
        ));
        assert_eq!(parse_onvif_exposure_time_100us(body), Some(120));
        assert_eq!(parse_onvif_gain_to_uvc(body), Some(128));
    }

    #[test]
    fn parses_onvif_metering_roi_attributes() {
        let mode = OV13B10_NATIVE_MODES[5];
        let body = concat!(
            "<timg:SetImagingSettings>",
            "<tt:Exposure><tt:Window left=\"0.25\" top=\"0.125\" right=\"0.75\" bottom=\"0.875\"/>",
            "</tt:Exposure></timg:SetImagingSettings>"
        );
        let roi = parse_onvif_meter_roi(body, mode).unwrap();
        assert_eq!(roi.left, 16384);
        assert_eq!(roi.top, 8192);
        assert_eq!(roi.right, 49151);
        assert_eq!(roi.bottom, 57343);
        assert_eq!(roi.auto_controls, 1);
    }

    #[test]
    fn parses_lmvc_v1_header() {
        let mut header = Vec::new();
        header.extend_from_slice(&LMI_UVC_RECORD_MAGIC.to_le_bytes());
        header.extend_from_slice(&LMI_UVC_RECORD_VERSION_V1.to_le_bytes());
        header.extend_from_slice(&(LMI_UVC_RECORD_BASE_HEADER_SIZE as u16).to_le_bytes());
        header.extend_from_slice(&(4u32).to_le_bytes());
        header.extend_from_slice(&(7u32).to_le_bytes());
        let parsed = parse_record_header(&header).unwrap();
        assert_eq!(parsed.header_size, LMI_UVC_RECORD_BASE_HEADER_SIZE);
        assert_eq!(parsed.payload_size, 4);
        assert_eq!(parsed.sequence, 7);
    }
}
