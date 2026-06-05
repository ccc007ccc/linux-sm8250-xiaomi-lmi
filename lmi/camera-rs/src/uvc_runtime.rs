use crate::controls::{self, SensorControlConfig};
use crate::devices;
use crate::isp::{IspCommand, IspSupervisor, UvcIspProfile};
use crate::native_modes::{NativeMode, OV13B10_NATIVE_MODES, by_frame_index};
use crate::route::LmiRouteConfig;
use crate::uvc::{UvcCodec, UvcGadget, UvcGadgetConfig};
use std::ffi::CString;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read};
use std::os::raw::{c_char, c_int, c_uint};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

const SIGINT: c_int = 2;
const SIGTERM: c_int = 15;
const O_NONBLOCK: i32 = 0o4000;
const PROCESS_GRACE: Duration = Duration::from_millis(800);

static STOP_REQUESTED: AtomicBool = AtomicBool::new(false);

unsafe extern "C" {
    fn kill(pid: c_int, sig: c_int) -> c_int;
    fn mkfifo(path: *const c_char, mode: c_uint) -> c_int;
    fn signal(signum: c_int, handler: extern "C" fn(c_int)) -> usize;
}

#[derive(Debug, Clone)]
pub struct NativeUvcRunConfig {
    pub raw_node: PathBuf,
    pub ctrl_node: Option<PathBuf>,
    pub isp_bin: PathBuf,
    pub feeder_bin: PathBuf,
    pub venus_bin: PathBuf,
    pub venus_node: PathBuf,
    pub fifo: PathBuf,
    pub isp_nv12_fifo: PathBuf,
    pub event_fifo: PathBuf,
    pub ready_file: PathBuf,
    pub setup_route: bool,
    pub route: LmiRouteConfig,
    pub controls: SensorControlConfig,
    pub gadget: UvcGadgetConfig,
    pub codec: UvcCodec,
    pub idle_stop_delay: Duration,
    pub restart_delay: Duration,
    pub recovery_retries: u32,
    pub buffers: u32,
    pub gamma: f32,
    pub tone_highlight_knee: u32,
    pub tone_highlight_max: u32,
    pub max_soft_gain: f32,
    pub auto_exposure: bool,
    pub ae_target: u32,
    pub ae_clip_target: Option<u32>,
    pub ae_clip_weight: u32,
    pub max_digital_gain: Option<u32>,
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
}

impl Default for NativeUvcRunConfig {
    fn default() -> Self {
        Self {
            raw_node: PathBuf::from("/dev/video3"),
            ctrl_node: None,
            isp_bin: PathBuf::from("/run/lmi-camera/lmi-isp"),
            feeder_bin: PathBuf::from("/run/lmi-camera/lmi-uvc-gadget"),
            venus_bin: PathBuf::from("/run/lmi-camera/lmi-venus-enc"),
            venus_node: PathBuf::new(),
            fifo: PathBuf::from("/run/lmi-camera/lmi-uvc.fifo"),
            isp_nv12_fifo: PathBuf::from("/run/lmi-camera/lmi-isp-nv12.fifo"),
            event_fifo: PathBuf::from("/run/lmi-camera/lmi-uvc.events"),
            ready_file: PathBuf::from("/run/lmi-camera/lmi-uvc.ready"),
            setup_route: true,
            route: LmiRouteConfig::default(),
            controls: SensorControlConfig::default(),
            gadget: UvcGadgetConfig::default(),
            codec: UvcCodec::Mjpeg,
            idle_stop_delay: Duration::from_secs(2),
            restart_delay: Duration::from_secs(1),
            recovery_retries: 3,
            buffers: 3,
            gamma: 3.0,
            tone_highlight_knee: 0,
            tone_highlight_max: 255,
            max_soft_gain: 3.5,
            auto_exposure: true,
            ae_target: 85,
            ae_clip_target: Some(620),
            ae_clip_weight: 50,
            max_digital_gain: Some(1024),
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
            h264_level: "5.1".to_string(),
        }
    }
}

pub fn looks_like_native_uvc_run(args: &[String]) -> bool {
    matches!(
        args.first().map(String::as_str),
        Some("uvc") | Some("native-modes")
    ) || args.windows(2).any(|pair| {
        (pair[0] == "--output" && pair[1] == "uvc")
            || (pair[0] == "--profile" && pair[1] == "native-modes")
    })
}

pub fn parse_native_uvc_run_config<I>(args: I) -> io::Result<NativeUvcRunConfig>
where
    I: Iterator<Item = String>,
{
    let mut config = NativeUvcRunConfig::default();
    let mut args = args.peekable();

    if matches!(
        args.peek().map(String::as_str),
        Some("uvc") | Some("native-modes")
    ) {
        args.next();
    }

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--output" => {
                let output = next_value(&mut args, "--output")?;
                if output != "uvc" {
                    return Err(invalid_input(format!(
                        "unsupported --output '{output}', native-modes uses --output uvc"
                    )));
                }
            }
            "--profile" => {
                let profile = next_value(&mut args, "--profile")?;
                if profile != "native-modes" {
                    return Err(invalid_input(format!(
                        "unsupported UVC profile '{profile}', only native-modes is public/default"
                    )));
                }
            }
            "--codec" | "--uvc-codec" => {
                config.codec = parse_codec(&next_value(&mut args, &arg)?, &arg)?
            }
            "--raw" | "--video" => {
                config.raw_node = PathBuf::from(next_value(&mut args, &arg)?);
                config.route.raw_node = Some(config.raw_node.clone());
            }
            "--ctrl" => config.ctrl_node = Some(PathBuf::from(next_value(&mut args, "--ctrl")?)),
            "--isp-bin" => config.isp_bin = PathBuf::from(next_value(&mut args, "--isp-bin")?),
            "--uvc-feeder-bin" | "--isp-uvc-feeder-bin" => {
                config.feeder_bin = PathBuf::from(next_value(&mut args, &arg)?)
            }
            "--venus-bin" | "--h264-encoder-bin" => {
                config.venus_bin = PathBuf::from(next_value(&mut args, &arg)?)
            }
            "--venus-node" | "--h264-encoder-node" => {
                config.venus_node = PathBuf::from(next_value(&mut args, &arg)?)
            }
            "--uvc-fifo" | "--isp-uvc-fifo" => {
                config.fifo = PathBuf::from(next_value(&mut args, &arg)?)
            }
            "--isp-nv12-fifo" | "--venus-input-fifo" => {
                config.isp_nv12_fifo = PathBuf::from(next_value(&mut args, &arg)?)
            }
            "--uvc-event-fifo" | "--isp-uvc-event-fifo" => {
                config.event_fifo = PathBuf::from(next_value(&mut args, &arg)?)
            }
            "--uvc-ready-file" => {
                config.ready_file = PathBuf::from(next_value(&mut args, "--uvc-ready-file")?)
            }
            "--uvc-gadget-name" | "--isp-uvc-gadget-name" => {
                config.gadget.gadget_name = next_value(&mut args, &arg)?
            }
            "--uvc-udc" | "--isp-uvc-udc" => config.gadget.udc = Some(next_value(&mut args, &arg)?),
            "--uvc-no-restore-prev-gadget" | "--isp-uvc-no-restore-prev-gadget" => {
                config.gadget.restore_previous_gadget = false
            }
            "--uvc-restore-prev-gadget" => config.gadget.restore_previous_gadget = true,
            "--uvc-keep" | "--isp-uvc-keep" => config.gadget.keep = true,
            "--uvc-maxpkt" | "--isp-uvc-maxpkt" => {
                config.gadget.maxpacket = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--uvc-default-frame" | "--uvc-default-frame-index" | "--default-frame" => {
                config.gadget.default_frame_index = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--uvc-buffers" | "--isp-uvc-buffers" => {
                config.buffers = parse_u32(&next_value(&mut args, &arg)?, &arg)?
            }
            "--uvc-idle-stop-delay" | "--isp-uvc-idle-stop-delay" => {
                config.idle_stop_delay = parse_duration_secs(&next_value(&mut args, &arg)?, &arg)?
            }
            "--uvc-restart-delay" | "--isp-uvc-restart-delay" => {
                config.restart_delay = parse_duration_secs(&next_value(&mut args, &arg)?, &arg)?
            }
            "--uvc-recovery-retries" | "--isp-uvc-recovery-retries" => {
                config.recovery_retries = parse_u32(&next_value(&mut args, &arg)?, &arg)?
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
            "--vblank"
            | "--exposure"
            | "--analogue-gain"
            | "--digital-gain"
            | "--reset-controls"
            | "--preserve-controls" => {
                controls::parse_sensor_control_option(&mut config.controls, &arg, &mut args)?;
            }
            "--help" | "-h" => {
                print_native_uvc_run_usage();
                return Ok(config);
            }
            other => {
                return Err(invalid_input(format!(
                    "unknown native UVC run option: {other}"
                )));
            }
        }
    }

    if by_frame_index(config.gadget.default_frame_index).is_none() {
        return Err(invalid_input(format!(
            "invalid UVC default frame {}, expected one of the native frame indexes 1..{}",
            config.gadget.default_frame_index,
            OV13B10_NATIVE_MODES.len()
        )));
    }

    config.route.raw_node = Some(config.raw_node.clone());
    Ok(config)
}

pub fn print_native_uvc_run_usage() {
    println!("Native UVC runtime options:");
    println!("  run --output uvc --profile native-modes [--codec mjpeg|h264]");
    println!("  --isp-bin PATH --uvc-feeder-bin PATH --uvc-fifo PATH --uvc-event-fifo PATH");
    println!("  --venus-bin PATH [--venus-node DEV] --isp-nv12-fifo PATH  (for --codec h264)");
    println!("  --uvc-gadget-name NAME --uvc-udc NAME --uvc-default-frame INDEX --uvc-no-restore-prev-gadget");
    println!("  --raw DEV --ctrl DEV --media DEV --sensor-subdev DEV --keep-links");
    println!(
        "  native-modes exposes exactly six OV13B10 RAW-size frames; default codec remains MJPEG"
    );
}

fn resolve_venus_node(config: &mut NativeUvcRunConfig) -> io::Result<()> {
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

pub fn run_native_uvc(mut config: NativeUvcRunConfig) -> io::Result<()> {
    install_signal_handlers();
    config.route.raw_node = Some(config.raw_node.clone());
    if config.codec == UvcCodec::H264 {
        resolve_venus_node(&mut config)?;
    }

    prepare_parent_dir(&config.fifo)?;
    prepare_parent_dir(&config.isp_nv12_fifo)?;
    prepare_parent_dir(&config.event_fifo)?;
    prepare_parent_dir(&config.ready_file)?;
    prepare_fifo(&config.fifo)?;
    if config.codec == UvcCodec::H264 {
        prepare_fifo(&config.isp_nv12_fifo)?;
    }
    prepare_fifo(&config.event_fifo)?;
    let mut event_reader = EventReader::open(&config.event_fifo)?;

    let mut active_mode = by_frame_index(config.gadget.default_frame_index)
        .ok_or_else(|| invalid_input("invalid native UVC default frame"))?;
    let mut gadget = UvcGadget::new(config.gadget.clone());
    let node = gadget.bind_native(config.codec)?;
    let mut feeder = FeederSupervisor::spawn(&config, &node, active_mode)?;
    let mut pipeline: Option<PipelineSupervisor> = None;
    let mut stream_requested = false;
    let mut pending_stop_at: Option<Instant> = None;
    let mut recoveries = 0;

    println!(
        "[uvc] native-modes ready: codec={} {} public frames, default frame {} -> OV13B10 mode {} {}x{} @ {:.3}fps",
        config.codec.name(),
        OV13B10_NATIVE_MODES.len(),
        active_mode.frame_index,
        active_mode.mode_index,
        active_mode.width,
        active_mode.height,
        active_mode.nominal_fps()
    );

    while !STOP_REQUESTED.load(Ordering::SeqCst) {
        for event in event_reader.drain_events()? {
            handle_event(
                &config,
                &event,
                &mut active_mode,
                &mut stream_requested,
                &mut pending_stop_at,
                &mut pipeline,
            )?;
        }

        if pending_stop_at.is_some_and(|deadline| Instant::now() >= deadline) {
            if let Some(mut child) = pipeline.take() {
                println!("[uvc] demand: idle grace elapsed, stopping camera pipeline");
                child.terminate()?;
            }
            pending_stop_at = None;
        }

        if let Some(status) = feeder.try_wait()? {
            println!("[uvc] feeder exited with {status}");
            if STOP_REQUESTED.load(Ordering::SeqCst) {
                break;
            }
            if recoveries >= config.recovery_retries {
                return Err(io::Error::other(format!(
                    "UVC feeder exited after {} recovery attempts",
                    recoveries
                )));
            }
            recoveries += 1;
            if let Some(mut child) = pipeline.take() {
                println!("[uvc] stopping camera pipeline before feeder recovery");
                child.terminate()?;
            }
            thread::sleep(config.restart_delay);
            let node = gadget.rebind()?;
            feeder = FeederSupervisor::spawn(&config, &node, active_mode)?;
            stream_requested = false;
            pending_stop_at = None;
            println!(
                "[uvc] feeder recovery {}/{} complete",
                recoveries, config.recovery_retries
            );
        }

        let pipeline_exit = if let Some(child) = pipeline.as_mut() {
            child.try_wait()?
        } else {
            None
        };
        if let Some(exit) = pipeline_exit {
            println!("[uvc] {} exited with {}", exit.component, exit.status);
            if let Some(mut child) = pipeline.take() {
                child.terminate()?;
            }
            if stream_requested && exit.status.success() {
                println!(
                    "[uvc] stream still requested after clean pipeline exit, restarting after delay"
                );
                thread::sleep(config.restart_delay);
                start_pipeline(&config, active_mode, &mut pipeline)?;
            } else if stream_requested {
                return Err(io::Error::other(format!(
                    "{} exited while streaming: {}",
                    exit.component, exit.status
                )));
            }
        }

        thread::sleep(Duration::from_millis(40));
    }

    println!("[uvc] shutdown requested: stopping camera pipeline then feeder before UDC unbind");
    if let Some(mut child) = pipeline.take() {
        child.terminate()?;
    }
    feeder.terminate()?;
    gadget.close()?;
    Ok(())
}

fn handle_event(
    config: &NativeUvcRunConfig,
    event: &str,
    active_mode: &mut NativeMode,
    stream_requested: &mut bool,
    pending_stop_at: &mut Option<Instant>,
    pipeline: &mut Option<PipelineSupervisor>,
) -> io::Result<()> {
    match parse_event(event) {
        UvcEvent::Commit {
            frame,
            width,
            height,
            size,
        } => {
            let Some(mode) = by_frame_index(frame) else {
                println!("[uvc] ignored COMMIT for non-native frame {frame}: {event}");
                return Ok(());
            };
            if width.is_some_and(|w| w != mode.width) || height.is_some_and(|h| h != mode.height) {
                println!(
                    "[uvc] ignored COMMIT frame={} with non-native size {:?}x{:?}; expected {}x{}",
                    frame, width, height, mode.width, mode.height
                );
                return Ok(());
            }
            let max_frame = config.codec.max_frame_bytes(mode);
            if size.is_some_and(|bytes| bytes > max_frame) {
                println!(
                    "[uvc] ignored COMMIT frame={} with size {} > native {} max {}",
                    frame,
                    size.unwrap_or_default(),
                    config.codec.name(),
                    max_frame
                );
                return Ok(());
            }

            let changed = active_mode.frame_index != mode.frame_index;
            *active_mode = mode;
            println!(
                "[uvc] demand: host committed {} native frame {} -> OV13B10 mode {}: {}x{} max={} interval100ns={}",
                config.codec.name(),
                mode.frame_index,
                mode.mode_index,
                mode.width,
                mode.height,
                max_frame,
                mode.interval_100ns
            );
            if changed {
                if let Some(mut child) = pipeline.take() {
                    println!("[uvc] demand: frame changed, stopping old camera pipeline");
                    child.terminate()?;
                }
                if *stream_requested {
                    start_pipeline(config, mode, pipeline)?;
                }
            } else if *stream_requested && pipeline.is_none() {
                start_pipeline(config, mode, pipeline)?;
            }
        }
        UvcEvent::StreamOn => {
            *stream_requested = true;
            *pending_stop_at = None;
            if pipeline.is_none() {
                start_pipeline(config, *active_mode, pipeline)?;
            }
        }
        UvcEvent::StreamOff | UvcEvent::Disconnect => {
            *stream_requested = false;
            *pending_stop_at = Some(Instant::now() + config.idle_stop_delay);
            println!(
                "[uvc] demand: {event}, stopping camera pipeline after {:.2}s idle grace",
                config.idle_stop_delay.as_secs_f32()
            );
        }
        UvcEvent::Connect => println!("[uvc] host connected"),
        UvcEvent::Other(line) => println!("[uvc] event: {line}"),
    }
    Ok(())
}

fn build_uvc_isp_profile(
    config: &NativeUvcRunConfig,
    mode: NativeMode,
    raw_node: PathBuf,
    ctrl_node: Option<PathBuf>,
    fifo: PathBuf,
) -> UvcIspProfile {
    UvcIspProfile {
        raw_node,
        ctrl_node,
        isp_bin: config.isp_bin.clone(),
        fifo,
        out_width: mode.width,
        out_height: mode.height,
        fps_cap: mode.fps_cap,
        gamma: config.gamma,
        tone_highlight_knee: config.tone_highlight_knee,
        tone_highlight_max: config.tone_highlight_max,
        max_soft_gain: config.max_soft_gain,
        auto_exposure: config.auto_exposure,
        ae_target: config.ae_target,
        ae_clip_target: config.ae_clip_target,
        ae_clip_weight: config.ae_clip_weight,
        max_digital_gain: config.max_digital_gain,
        max_frame_bytes: config.codec.max_frame_bytes(mode),
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
    config: &NativeUvcRunConfig,
    mode: NativeMode,
) -> io::Result<(PathBuf, Option<PathBuf>)> {
    let mut route = config.route.clone();
    route.width = mode.width;
    route.height = mode.height;
    route.raw_node = Some(config.raw_node.clone());
    route.controls = config.controls.clone();

    let (raw_node, ctrl_node) = if config.setup_route {
        let report = crate::route::setup_lmi_ov13b10_route(&route)?;
        println!(
            "[isp] route_ready frame={} mode={} raw_format={} {}x{} bytesperline={} sizeimage={}",
            mode.frame_index,
            mode.mode_index,
            report.format.fourcc,
            report.format.width,
            report.format.height,
            report.format.bytesperline,
            report.format.sizeimage
        );
        (
            report.raw_node,
            config
                .ctrl_node
                .clone()
                .or_else(|| Some(report.control_subdev)),
        )
    } else {
        println!(
            "[isp] setup_route=skipped for native frame {}",
            mode.frame_index
        );
        (config.raw_node.clone(), config.ctrl_node.clone())
    };

    Ok((raw_node, ctrl_node))
}

fn start_pipeline(
    config: &NativeUvcRunConfig,
    mode: NativeMode,
    slot: &mut Option<PipelineSupervisor>,
) -> io::Result<()> {
    let (raw_node, ctrl_node) = route_for_mode(config, mode)?;
    let profile = build_uvc_isp_profile(
        config,
        mode,
        raw_node,
        ctrl_node,
        match config.codec {
            UvcCodec::Mjpeg => config.fifo.clone(),
            UvcCodec::H264 => config.isp_nv12_fifo.clone(),
        },
    );

    let pipeline = match config.codec {
        UvcCodec::Mjpeg => {
            let command = IspCommand::for_uvc_mjpeg(&profile);
            print!("[isp] command: ");
            command.print_shell();
            PipelineSupervisor::mjpeg(command.spawn()?)
        }
        UvcCodec::H264 => {
            let encoder = EncoderCommand::for_mode(config, mode);
            print!("[venus] command: ");
            encoder.print_shell();
            let encoder = encoder.spawn()?;
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
            PipelineSupervisor::h264(encoder, isp)
        }
    };

    println!(
        "[uvc] started {} pipeline native frame={} mode={} {}x{} isp_pid={}{}",
        config.codec.name(),
        mode.frame_index,
        mode.mode_index,
        mode.width,
        mode.height,
        pipeline.isp_id(),
        pipeline
            .encoder_id()
            .map(|pid| format!(" venus_pid={pid}"))
            .unwrap_or_default()
    );
    *slot = Some(pipeline);
    Ok(())
}

#[derive(Debug)]
enum UvcEvent {
    Connect,
    Disconnect,
    StreamOn,
    StreamOff,
    Commit {
        frame: u32,
        width: Option<u32>,
        height: Option<u32>,
        size: Option<u32>,
    },
    Other(String),
}

fn parse_event(line: &str) -> UvcEvent {
    let trimmed = line.trim();
    match trimmed {
        "CONNECT" => return UvcEvent::Connect,
        "DISCONNECT" => return UvcEvent::Disconnect,
        "STREAMON" => return UvcEvent::StreamOn,
        "STREAMOFF" => return UvcEvent::StreamOff,
        _ => {}
    }
    if let Some(rest) = trimmed.strip_prefix("COMMIT") {
        let mut frame = None;
        let mut width = None;
        let mut height = None;
        let mut size = None;
        for item in rest.split_whitespace() {
            let Some((key, value)) = item.split_once('=') else {
                continue;
            };
            match key {
                "frame" => frame = value.parse().ok(),
                "width" => width = value.parse().ok(),
                "height" => height = value.parse().ok(),
                "size" => size = value.parse().ok(),
                _ => {}
            }
        }
        if let Some(frame) = frame {
            return UvcEvent::Commit {
                frame,
                width,
                height,
                size,
            };
        }
    }
    UvcEvent::Other(trimmed.to_string())
}

struct EventReader {
    file: File,
    pending: Vec<u8>,
}

impl EventReader {
    fn open(path: &Path) -> io::Result<Self> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .custom_flags(O_NONBLOCK)
            .open(path)?;
        Ok(Self {
            file,
            pending: Vec::new(),
        })
    }

    fn drain_events(&mut self) -> io::Result<Vec<String>> {
        let mut buf = [0u8; 4096];
        loop {
            match self.file.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => self.pending.extend_from_slice(&buf[..n]),
                Err(err) if err.kind() == io::ErrorKind::WouldBlock => break,
                Err(err) => return Err(err),
            }
        }

        let mut events = Vec::new();
        while let Some(pos) = self.pending.iter().position(|b| *b == b'\n') {
            let line: Vec<u8> = self.pending.drain(..=pos).collect();
            let text = String::from_utf8_lossy(&line).trim().to_string();
            if !text.is_empty() {
                events.push(text);
            }
        }
        Ok(events)
    }
}

struct EncoderCommand {
    program: PathBuf,
    args: Vec<String>,
}

impl EncoderCommand {
    fn for_mode(config: &NativeUvcRunConfig, mode: NativeMode) -> Self {
        let args = vec![
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
            mode.h264_bitrate().to_string(),
            "--peak-bitrate".to_string(),
            mode.h264_peak_bitrate().to_string(),
            "--gop".to_string(),
            config.h264_gop.to_string(),
            "--profile".to_string(),
            config.h264_profile.clone(),
            "--level".to_string(),
            config.h264_level.clone(),
            "--max-record".to_string(),
            mode.h264_max_record_bytes().to_string(),
            "--fifo-write-timeout-ms".to_string(),
            "2000".to_string(),
        ];
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
}

impl PipelineSupervisor {
    fn mjpeg(isp: IspSupervisor) -> Self {
        Self { encoder: None, isp }
    }

    fn h264(encoder: EncoderSupervisor, isp: IspSupervisor) -> Self {
        Self {
            encoder: Some(encoder),
            isp,
        }
    }

    fn isp_id(&self) -> u32 {
        self.isp.id()
    }

    fn encoder_id(&self) -> Option<u32> {
        self.encoder.as_ref().map(EncoderSupervisor::id)
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

struct FeederSupervisor {
    child: Child,
}

impl FeederSupervisor {
    fn spawn(config: &NativeUvcRunConfig, node: &Path, mode: NativeMode) -> io::Result<Self> {
        let _ = fs::remove_file(&config.ready_file);
        let mut args = vec![
            "--device".to_string(),
            node.display().to_string(),
            "--fifo".to_string(),
            config.fifo.display().to_string(),
            "--width".to_string(),
            mode.width.to_string(),
            "--height".to_string(),
            mode.height.to_string(),
            "--fps".to_string(),
            mode.fps_cap.to_string(),
            "--maxpkt".to_string(),
            config.gadget.maxpacket.to_string(),
            "--intf".to_string(),
            "1".to_string(),
            "--buffers".to_string(),
            config.buffers.to_string(),
            "--format".to_string(),
            config.codec.feeder_format().to_string(),
            "--max-frame".to_string(),
            config.codec.max_frame_bytes(mode).to_string(),
            "--ready-file".to_string(),
            config.ready_file.display().to_string(),
            "--event-fifo".to_string(),
            config.event_fifo.display().to_string(),
            "--frame-index".to_string(),
            mode.frame_index.to_string(),
        ];
        for native in OV13B10_NATIVE_MODES {
            args.push("--frame".to_string());
            args.push(config.codec.frame_arg(native));
        }

        println!(
            "[uvc] feeder command: {} {}",
            shell_quote(&config.feeder_bin.display().to_string()),
            args.iter()
                .map(|arg| shell_quote(arg))
                .collect::<Vec<_>>()
                .join(" ")
        );
        let child = Command::new(&config.feeder_bin)
            .args(&args)
            .stdin(Stdio::null())
            .stdout(Stdio::inherit())
            .stderr(Stdio::inherit())
            .spawn()?;
        println!("[uvc] feeder pid={}", child.id());
        Ok(Self { child })
    }

    fn try_wait(&mut self) -> io::Result<Option<ExitStatus>> {
        self.child.try_wait()
    }

    fn terminate(&mut self) -> io::Result<()> {
        terminate_child(&mut self.child)
    }
}

impl Drop for FeederSupervisor {
    fn drop(&mut self) {
        let _ = self.terminate();
    }
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

fn prepare_parent_dir(path: &Path) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    Ok(())
}

fn prepare_fifo(path: &Path) -> io::Result<()> {
    let _ = fs::remove_file(path);
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

fn parse_f32(value: &str, field: &str) -> io::Result<f32> {
    value
        .parse::<f32>()
        .map_err(|_| invalid_input(format!("invalid {field} value '{value}'")))
}

fn parse_duration_secs(value: &str, field: &str) -> io::Result<Duration> {
    let secs = parse_f32(value, field)?;
    if secs < 0.0 {
        return Err(invalid_input(format!("{field} must be non-negative")));
    }
    Ok(Duration::from_millis((secs * 1000.0).round() as u64))
}

fn parse_codec(value: &str, field: &str) -> io::Result<UvcCodec> {
    match value {
        "mjpeg" | "MJPEG" => Ok(UvcCodec::Mjpeg),
        "h264" | "H264" | "h.264" | "H.264" => Ok(UvcCodec::H264),
        other => Err(invalid_input(format!(
            "invalid {field} value '{other}', expected mjpeg or h264"
        ))),
    }
}

fn invalid_input(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

fn shell_quote(value: &str) -> String {
    if value
        .bytes()
        .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'/' | b'.' | b'_' | b'-' | b':' | b'='))
    {
        return value.to_string();
    }
    let escaped = value.replace('"', "\\\"");
    format!("\"{escaped}\"")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_native_uvc_run_selector() {
        assert!(looks_like_native_uvc_run(&[
            "--output".to_string(),
            "uvc".to_string(),
            "--profile".to_string(),
            "native-modes".to_string(),
        ]));
        assert!(looks_like_native_uvc_run(&[
            "--profile".to_string(),
            "native-modes".to_string(),
        ]));
        assert!(!looks_like_native_uvc_run(&["local-loopback".to_string()]));
    }

    #[test]
    fn native_uvc_defaults_match_public_profile() {
        let config = parse_native_uvc_run_config(
            ["--output", "uvc", "--profile", "native-modes"]
                .into_iter()
                .map(String::from),
        )
        .unwrap();
        assert_eq!(config.raw_node, PathBuf::from("/dev/video3"));
        assert_eq!(config.mjpeg_quality, 90);
        assert_eq!(config.mjpeg_scale_mode, "bayer-area-frac");
        assert_eq!(config.ae_target, 85);
        assert_eq!(config.max_digital_gain, Some(1024));
        assert_eq!(config.gadget.default_frame_index, 1);
    }

    #[test]
    fn native_uvc_accepts_low_resolution_default_frame() {
        let config = parse_native_uvc_run_config(
            [
                "--output",
                "uvc",
                "--profile",
                "native-modes",
                "--uvc-default-frame",
                "6",
            ]
            .into_iter()
            .map(String::from),
        )
        .unwrap();
        assert_eq!(config.gadget.default_frame_index, 6);
    }
}
