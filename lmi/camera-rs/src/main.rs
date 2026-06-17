mod capture;
mod config;
mod controls;
mod devices;
mod fourcc;
mod isp;
mod media;
mod native_modes;
mod network_runtime;
mod pipeline;
mod route;
mod uvc;
mod uvc_runtime;
mod v4l2;

use config::{IspPixelFormat, LocalLoopbackProfile};
use controls::SensorControlConfig;
use fourcc::FourCc;
use isp::IspCommand;
use route::LmiRouteConfig;
use std::env;
use std::io;
use std::path::{Path, PathBuf};

const RAW_NODE: &str = "/dev/video3";
const LOOPBACK_NODE: &str = "/dev/video20";

#[derive(Debug, Clone)]
struct LocalLoopbackRunConfig {
    profile: LocalLoopbackProfile,
    route: LmiRouteConfig,
    setup_route: bool,
    controls: SensorControlConfig,
}

fn main() {
    if let Err(err) = run() {
        eprintln!("lmi-camera: {err}");
        std::process::exit(1);
    }
}

fn run() -> io::Result<()> {
    let mut args = env::args().skip(1);
    match args.next().as_deref() {
        None | Some("help") | Some("--help") | Some("-h") => {
            usage();
            Ok(())
        }
        Some("probe") => probe(),
        Some("media") => media::print_media_nodes(),
        Some("route-summary") => media::print_route_summary(),
        Some("formats") => {
            let device = args
                .next()
                .map(PathBuf::from)
                .unwrap_or_else(|| PathBuf::from(RAW_NODE));
            print_formats(&device)
        }
        Some("route-check") => route_check(),
        Some("setup-route") => {
            let config = route::parse_lmi_route_config(args)?;
            route::setup_lmi_ov13b10_route(&config).map(|_| ())
        }
        Some("capture-raw") => {
            let config = capture::parse_raw_capture_config(args)?;
            capture::capture_raw(&config)
        }
        Some("profile") => {
            let profile = parse_local_loopback_profile(args)?;
            profile.print();
            Ok(())
        }
        Some("isp-command") => {
            let profile = parse_local_loopback_profile(args)?;
            IspCommand::for_local_loopback(&profile).print_shell();
            Ok(())
        }
        Some("run") => {
            let rest: Vec<String> = args.collect();
            if network_runtime::looks_like_network_run(&rest) {
                let config = network_runtime::parse_network_run_config(rest.into_iter())?;
                network_runtime::run_network(config)
            } else if uvc_runtime::looks_like_native_uvc_run(&rest) {
                let config = uvc_runtime::parse_native_uvc_run_config(rest.into_iter())?;
                uvc_runtime::run_native_uvc(config)
            } else {
                let config = parse_local_loopback_run_config(rest.into_iter())?;
                run_local_loopback(config)
            }
        }
        Some("uvc-status") => {
            let mut gadget = "lmi_uvc".to_string();
            let mut codec = uvc::UvcCodec::Mjpeg;
            let mut assert_native = false;
            let mut args = args.peekable();
            while let Some(arg) = args.next() {
                match arg.as_str() {
                    "--gadget" | "--uvc-gadget-name" => gadget = next_value(&mut args, &arg)?,
                    "--codec" | "--uvc-codec" => {
                        codec = parse_uvc_codec(&next_value(&mut args, &arg)?, &arg)?
                    }
                    "--assert-native-six" => assert_native = true,
                    "--help" | "-h" => {
                        println!(
                            "uvc-status options: --gadget NAME --codec mjpeg|h264 --assert-native-six"
                        );
                        return Ok(());
                    }
                    other => {
                        return Err(invalid_input(format!("unknown uvc-status option: {other}")));
                    }
                }
            }
            if assert_native {
                uvc::assert_native_six_codec(&gadget, codec)
            } else {
                let status = uvc::read_status_codec(&gadget, codec)?;
                println!(
                    "gadget={} codec={}",
                    status.gadget.display(),
                    status.codec.name()
                );
                uvc::print_status(&status);
                Ok(())
            }
        }
        Some("pipeline-plan") => {
            pipeline::print_pipeline_plan();
            Ok(())
        }
        Some(other) => {
            eprintln!("unknown command: {other}");
            usage();
            Ok(())
        }
    }
}

fn usage() {
    println!("lmi-camera: Rust control-plane prototype for lmi local camera runtime");
    println!();
    println!("Commands:");
    println!("  probe          list media/video nodes and highlight raw/loopback defaults");
    println!("  media          enumerate /dev/media* graph entities, pads, and links");
    println!("  route-summary  summarize lmi camera media route and default nodes");
    println!("  formats [dev]  enumerate V4L2 formats for a video node, default {RAW_NODE}");
    println!("  route-check    check primary local-camera invariants");
    println!("  setup-route    configure OV13B10 -> CSIPHY1 -> CSID1 -> VFE1 RDI0 RAW route");
    println!("  capture-raw    stream a bounded RAW pgAA frame smoke test into a null sink");
    println!("  profile        print default local-loopback runtime profile");
    println!("  isp-command    print the supervised lmi-isp command");
    println!("  run            run supervised local-loopback C ISP backend");
    println!("  run --output uvc --profile native-modes");
    println!("                 run default Rust/C UVC demand-start native six-mode backend");
    println!(
        "  run --output network --profile native-modes --frame-index 6 --listen 127.0.0.1:8080"
    );
    println!("                 run opt-in MJPEG-over-HTTP native-mode network camera backend");
    println!("  uvc-status     inspect UVC configfs frames; --codec h264 checks framebased H.264");
    println!("  pipeline-plan  print multithreaded performance pipeline plan");
    println!();
    println!("Common local-loopback options:");
    println!("  --raw DEV --loopback DEV --ctrl DEV --isp-bin PATH");
    println!("  --size WxH --fps N --format yuyv|nv12 --no-ae --target N");
    println!("  run also accepts --route-size WxH, sensor controls, and setup-route media options");
    println!();
    uvc_runtime::print_native_uvc_run_usage();
    println!();
    network_runtime::print_network_run_usage();
    println!();
    route::print_setup_route_usage();
    capture::print_capture_raw_usage();
}

fn probe() -> io::Result<()> {
    println!("primary=local-linux-camera via v4l2loopback");
    println!("secondary=usb-uvc optional backend");
    println!("raw_invariant={RAW_NODE} must remain pgAA RAW");
    println!("processed_default={LOOPBACK_NODE}");
    println!();

    media::print_media_nodes()?;
    println!();

    let nodes = devices::list_video_nodes()?;
    if nodes.is_empty() {
        println!("no /dev/video* nodes found");
    }
    for node in &nodes {
        print_video_node(node);
    }

    println!();
    match devices::default_raw_node(&nodes) {
        Some(node) => {
            let pg = v4l2::has_format(&node.path, FourCc::PGAA);
            println!(
                "raw_default {} present pgAA={}",
                node.path.display(),
                yes_no(pg)
            );
        }
        None => println!("raw_default {RAW_NODE} missing"),
    }
    match devices::default_loopback_node(&nodes) {
        Some(node) => println!(
            "loopback_default {} present name={}",
            node.path.display(),
            node.name.as_deref().unwrap_or("unknown")
        ),
        None => println!("loopback_default {LOOPBACK_NODE} missing"),
    }
    Ok(())
}

fn print_video_node(node: &devices::VideoNode) {
    let cap = v4l2::query_capability(&node.path).ok();
    let card = cap
        .as_ref()
        .map(|c| c.card.as_str())
        .or(node.name.as_deref())
        .unwrap_or("unknown");
    let driver = cap
        .as_ref()
        .map(|c| c.driver.as_str())
        .or(node.driver.as_deref())
        .unwrap_or("unknown");
    let bus = cap
        .as_ref()
        .map(|c| c.bus_info.as_str())
        .or(node.bus_info.as_deref())
        .unwrap_or("unknown");
    println!(
        "video {} card='{}' driver='{}' bus='{}'",
        node.path.display(),
        card,
        driver,
        bus
    );
}

fn print_formats(path: &Path) -> io::Result<()> {
    let cap = v4l2::query_capability(path)?;
    println!("device {}", path.display());
    println!(
        "driver='{}' card='{}' bus='{}' caps=0x{:08x} device_caps=0x{:08x}",
        cap.driver, cap.card, cap.bus_info, cap.capabilities, cap.device_caps
    );
    let formats = v4l2::enum_formats(path)?;
    if formats.is_empty() {
        println!("no formats enumerated");
    }
    for fmt in formats {
        println!(
            "type={} index={} fourcc={} desc='{}'",
            fmt.buffer_type, fmt.index, fmt.fourcc, fmt.description
        );
    }
    Ok(())
}

fn route_check() -> io::Result<()> {
    let nodes = devices::list_video_nodes()?;
    let mut ok = true;

    println!("checking local camera invariants");
    println!("primary output is local v4l2loopback; UVC is optional");

    match devices::default_raw_node(&nodes) {
        Some(raw) => {
            let has_pg = v4l2::has_format(&raw.path, FourCc::PGAA);
            println!("raw node {} present: yes", raw.path.display());
            println!("raw node supports pgAA: {}", yes_no(has_pg));
            if !has_pg {
                ok = false;
            }
        }
        None => {
            println!("raw node {RAW_NODE} present: no");
            ok = false;
        }
    }

    match devices::default_loopback_node(&nodes) {
        Some(loopback) => {
            println!("loopback node {} present: yes", loopback.path.display());
            if let Some(name) = &loopback.name {
                println!("loopback node name: {name}");
            }
        }
        None => {
            println!("loopback node {LOOPBACK_NODE} present: no");
            println!("hint: lmi kernel should have CONFIG_V4L2LOOPBACK=y with default video_nr=20");
            ok = false;
        }
    }

    println!("kernel processed YUV/RGB advertised: no");
    println!("/dev/video3 fake colour output allowed: no");

    if ok {
        println!("route-check: PASS");
    } else {
        println!("route-check: FAIL");
    }
    Ok(())
}

fn parse_local_loopback_profile<I>(args: I) -> io::Result<LocalLoopbackProfile>
where
    I: Iterator<Item = String>,
{
    let mut profile = LocalLoopbackProfile::default();
    let mut args = args.peekable();

    if matches!(args.peek().map(String::as_str), Some("local-loopback")) {
        args.next();
    }

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--raw" => profile.raw_node = PathBuf::from(next_value(&mut args, "--raw")?),
            "--loopback" => {
                profile.loopback_node = PathBuf::from(next_value(&mut args, "--loopback")?)
            }
            "--ctrl" => profile.ctrl_node = Some(PathBuf::from(next_value(&mut args, "--ctrl")?)),
            "--isp-bin" => profile.isp_bin = PathBuf::from(next_value(&mut args, "--isp-bin")?),
            "--size" => {
                let size = next_value(&mut args, "--size")?;
                let (width, height) = parse_size(&size)?;
                profile.out_width = width;
                profile.out_height = height;
            }
            "--width" => {
                profile.out_width =
                    parse_nonzero_u32(&next_value(&mut args, "--width")?, "--width")?;
            }
            "--height" => {
                profile.out_height =
                    parse_nonzero_u32(&next_value(&mut args, "--height")?, "--height")?;
            }
            "--fps" | "--fps-cap" => {
                profile.fps_cap = parse_u32(&next_value(&mut args, &arg)?, &arg)?;
            }
            "--format" => {
                let format = next_value(&mut args, "--format")?;
                profile.format = format.parse::<IspPixelFormat>().map_err(invalid_input)?;
            }
            "--gamma" => {
                profile.gamma = parse_f32(&next_value(&mut args, "--gamma")?, "--gamma")?;
            }
            "--max-soft-gain" => {
                profile.max_soft_gain = parse_f32(
                    &next_value(&mut args, "--max-soft-gain")?,
                    "--max-soft-gain",
                )?;
            }
            "--target" => {
                profile.ae_target = parse_u32(&next_value(&mut args, "--target")?, "--target")?;
            }
            "--no-ae" => profile.auto_exposure = false,
            "--auto-exposure" => profile.auto_exposure = true,
            "--help" | "-h" => {
                usage();
                return Ok(profile);
            }
            other => {
                return Err(invalid_input(format!(
                    "unknown local-loopback option: {other}"
                )));
            }
        }
    }

    Ok(profile)
}

fn parse_local_loopback_run_config<I>(args: I) -> io::Result<LocalLoopbackRunConfig>
where
    I: Iterator<Item = String>,
{
    let mut profile = LocalLoopbackProfile::default();
    let mut route = LmiRouteConfig::default();
    let mut setup_route = true;
    let mut controls = SensorControlConfig::default();
    let mut args = args.peekable();

    if matches!(args.peek().map(String::as_str), Some("local-loopback")) {
        args.next();
    }

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--raw" => {
                profile.raw_node = PathBuf::from(next_value(&mut args, "--raw")?);
                route.raw_node = Some(profile.raw_node.clone());
            }
            "--loopback" => {
                profile.loopback_node = PathBuf::from(next_value(&mut args, "--loopback")?)
            }
            "--ctrl" => profile.ctrl_node = Some(PathBuf::from(next_value(&mut args, "--ctrl")?)),
            "--isp-bin" => profile.isp_bin = PathBuf::from(next_value(&mut args, "--isp-bin")?),
            "--size" => {
                let size = next_value(&mut args, "--size")?;
                let (width, height) = parse_size(&size)?;
                profile.out_width = width;
                profile.out_height = height;
            }
            "--width" => {
                profile.out_width =
                    parse_nonzero_u32(&next_value(&mut args, "--width")?, "--width")?;
            }
            "--height" => {
                profile.out_height =
                    parse_nonzero_u32(&next_value(&mut args, "--height")?, "--height")?;
            }
            "--fps" | "--fps-cap" => {
                profile.fps_cap = parse_u32(&next_value(&mut args, &arg)?, &arg)?;
            }
            "--format" => {
                let format = next_value(&mut args, "--format")?;
                profile.format = format.parse::<IspPixelFormat>().map_err(invalid_input)?;
            }
            "--gamma" => {
                profile.gamma = parse_f32(&next_value(&mut args, "--gamma")?, "--gamma")?;
            }
            "--max-soft-gain" => {
                profile.max_soft_gain = parse_f32(
                    &next_value(&mut args, "--max-soft-gain")?,
                    "--max-soft-gain",
                )?;
            }
            "--target" => {
                profile.ae_target = parse_u32(&next_value(&mut args, "--target")?, "--target")?;
            }
            "--no-ae" => profile.auto_exposure = false,
            "--auto-exposure" => profile.auto_exposure = true,
            "--vblank"
            | "--exposure"
            | "--analogue-gain"
            | "--digital-gain"
            | "--reset-controls"
            | "--preserve-controls" => {
                controls::parse_sensor_control_option(&mut controls, &arg, &mut args)?;
            }
            "--no-setup-route" => setup_route = false,
            "--setup-route" => setup_route = true,
            "--route-size" => {
                let size = next_value(&mut args, "--route-size")?;
                let (width, height) = parse_size(&size)?;
                route.width = width;
                route.height = height;
            }
            "--route-width" => {
                route.width =
                    parse_nonzero_u32(&next_value(&mut args, "--route-width")?, "--route-width")?;
            }
            "--route-height" => {
                route.height =
                    parse_nonzero_u32(&next_value(&mut args, "--route-height")?, "--route-height")?;
            }
            "--media" => route.media = PathBuf::from(next_value(&mut args, "--media")?),
            "--sensor" => route.sensor = next_value(&mut args, "--sensor")?,
            "--csiphy" => route.csiphy = next_value(&mut args, "--csiphy")?,
            "--csid" => route.csid = next_value(&mut args, "--csid")?,
            "--vfe" => route.vfe = next_value(&mut args, "--vfe")?,
            "--video-entity" => route.video_entity = next_value(&mut args, "--video-entity")?,
            "--sensor-subdev" => {
                route.sensor_subdev = Some(PathBuf::from(next_value(&mut args, "--sensor-subdev")?))
            }
            "--csiphy-subdev" => {
                route.csiphy_subdev = Some(PathBuf::from(next_value(&mut args, "--csiphy-subdev")?))
            }
            "--csid-subdev" => {
                route.csid_subdev = Some(PathBuf::from(next_value(&mut args, "--csid-subdev")?))
            }
            "--vfe-subdev" => {
                route.vfe_subdev = Some(PathBuf::from(next_value(&mut args, "--vfe-subdev")?))
            }
            "--csiphy-source-pad" => {
                route.csiphy_source_pad = parse_u16(
                    &next_value(&mut args, "--csiphy-source-pad")?,
                    "--csiphy-source-pad",
                )?
            }
            "--csid-source-pad" => {
                route.csid_source_pad = parse_u16(
                    &next_value(&mut args, "--csid-source-pad")?,
                    "--csid-source-pad",
                )?
            }
            "--mbus-code" => {
                route.mbus_code =
                    parse_u32_auto(&next_value(&mut args, "--mbus-code")?, "--mbus-code")?
            }
            "--keep-links" => route.keep_links = true,
            "--route" => {
                let route_name = next_value(&mut args, "--route")?;
                if route_name != "lmi-ov13b10" {
                    return Err(invalid_input(format!(
                        "unsupported --route '{route_name}', only lmi-ov13b10 is implemented"
                    )));
                }
            }
            "--help" | "-h" => {
                usage();
                return Ok(LocalLoopbackRunConfig {
                    profile,
                    route,
                    setup_route,
                    controls,
                });
            }
            other => {
                return Err(invalid_input(format!(
                    "unknown local-loopback run option: {other}"
                )));
            }
        }
    }

    route.raw_node = Some(profile.raw_node.clone());
    Ok(LocalLoopbackRunConfig {
        profile,
        route,
        setup_route,
        controls,
    })
}

fn run_local_loopback(mut config: LocalLoopbackRunConfig) -> io::Result<()> {
    if config.setup_route {
        let report = route::setup_lmi_ov13b10_route(&config.route)?;
        config.profile.raw_node = report.raw_node;
        if config.profile.ctrl_node.is_none() {
            config.profile.ctrl_node = Some(report.control_subdev);
        }
        println!(
            "route_ready raw_format={} {}x{} bytesperline={} sizeimage={}",
            report.format.fourcc,
            report.format.width,
            report.format.height,
            report.format.bytesperline,
            report.format.sizeimage
        );
    } else {
        println!("setup_route=skipped");
    }

    if let Some(ctrl) = config.profile.ctrl_node.as_deref() {
        controls::apply_initial_sensor_controls(
            ctrl,
            &config.controls,
            config.profile.auto_exposure,
        )?;
    } else if config.controls.should_apply(config.profile.auto_exposure) {
        return Err(invalid_input(
            "sensor controls requested but no control subdev is known; pass --ctrl or enable route setup",
        ));
    }

    // Orientation is owned by the kernel: consume the sensor rotation metadata
    // (DTS `rotation`, exposed as V4L2_CID_CAMERA_SENSOR_ROTATION) and let the
    // ISP rotate the loopback upright.  An explicit --rotate on the CLI wins.
    if config.profile.rotate == 0 {
        if let Some(ctrl) = config.profile.ctrl_node.as_deref() {
            match controls::read_sensor_rotation(ctrl) {
                Ok(Some(deg)) => {
                    let norm = ((deg % 360) + 360) % 360;
                    if matches!(norm, 0 | 90 | 180 | 270) {
                        config.profile.rotate = norm as u32;
                        println!("sensor_rotation_metadata={} -> isp --rotate {}", deg, norm);
                    } else {
                        println!("sensor_rotation_metadata={} not a right angle; not rotating", deg);
                    }
                }
                Ok(None) => println!("sensor_rotation_metadata=absent; not rotating"),
                Err(err) => println!("sensor_rotation_metadata read failed: {err}; not rotating"),
            }
        }
    }

    println!("starting local-loopback C ISP backend");
    println!(
        "raw {} stays truthful pgAA",
        config.profile.raw_node.display()
    );
    println!(
        "processed output {} is the app-facing camera node",
        config.profile.loopback_node.display()
    );

    let command = IspCommand::for_local_loopback(&config.profile);
    print!("command: ");
    command.print_shell();

    let mut supervisor = command.spawn()?;
    println!("lmi-isp pid={}", supervisor.id());
    let status = supervisor.wait()?;
    if status.success() {
        println!("lmi-isp exited cleanly");
        Ok(())
    } else {
        Err(io::Error::other(format!("lmi-isp exited with {status}")))
    }
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

fn invalid_input(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

fn parse_uvc_codec(value: &str, field: &str) -> io::Result<uvc::UvcCodec> {
    match value {
        "mjpeg" | "MJPEG" => Ok(uvc::UvcCodec::Mjpeg),
        "h264" | "H264" | "h.264" | "H.264" => Ok(uvc::UvcCodec::H264),
        other => Err(invalid_input(format!(
            "invalid {field} value '{other}', expected mjpeg or h264"
        ))),
    }
}

fn yes_no(v: bool) -> &'static str {
    if v { "yes" } else { "no" }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn run_config_keeps_raw_route_size_separate_from_output_size() {
        let config = parse_local_loopback_run_config(
            ["--size", "640x480", "--raw", "/dev/video9"]
                .into_iter()
                .map(String::from),
        )
        .unwrap();

        assert_eq!(config.profile.out_width, 640);
        assert_eq!(config.profile.out_height, 480);
        assert_eq!(config.profile.raw_node, PathBuf::from("/dev/video9"));
        assert_eq!(config.route.raw_node, Some(PathBuf::from("/dev/video9")));
        assert_eq!(config.route.width, 1364);
        assert_eq!(config.route.height, 768);
        assert!(config.setup_route);
    }

    #[test]
    fn run_config_accepts_route_overrides_and_skip() {
        let config = parse_local_loopback_run_config(
            [
                "--no-setup-route",
                "--route-size",
                "1280x720",
                "--media",
                "/dev/media2",
                "--sensor-subdev",
                "/dev/v4l-subdev7",
                "--keep-links",
            ]
            .into_iter()
            .map(String::from),
        )
        .unwrap();

        assert!(!config.setup_route);
        assert_eq!(config.route.width, 1280);
        assert_eq!(config.route.height, 720);
        assert_eq!(config.route.media, PathBuf::from("/dev/media2"));
        assert_eq!(
            config.route.sensor_subdev,
            Some(PathBuf::from("/dev/v4l-subdev7"))
        );
        assert!(config.route.keep_links);
    }

    #[test]
    fn run_config_accepts_initial_sensor_controls() {
        let config = parse_local_loopback_run_config(
            [
                "--ctrl",
                "/dev/v4l-subdev5",
                "--vblank",
                "44",
                "--exposure",
                "120",
                "--analogue-gain",
                "2",
                "--digital-gain",
                "3",
                "--preserve-controls",
            ]
            .into_iter()
            .map(String::from),
        )
        .unwrap();

        assert_eq!(
            config.profile.ctrl_node,
            Some(PathBuf::from("/dev/v4l-subdev5"))
        );
        assert_eq!(config.controls.vblank, Some(44));
        assert_eq!(config.controls.exposure, Some(120));
        assert_eq!(config.controls.analogue_gain, Some(2));
        assert_eq!(config.controls.digital_gain, Some(3));
        assert!(config.controls.preserve_controls);
    }

    #[test]
    fn run_config_rejects_zero_output_and_route_sizes() {
        let output_err =
            parse_local_loopback_run_config(["--size", "0x480"].into_iter().map(String::from))
                .unwrap_err();
        assert_eq!(output_err.kind(), io::ErrorKind::InvalidInput);
        assert!(
            output_err
                .to_string()
                .contains("--size width must be greater than zero")
        );

        let route_err =
            parse_local_loopback_run_config(["--route-width", "0"].into_iter().map(String::from))
                .unwrap_err();
        assert_eq!(route_err.kind(), io::ErrorKind::InvalidInput);
        assert!(
            route_err
                .to_string()
                .contains("--route-width must be greater than zero")
        );
    }
}
