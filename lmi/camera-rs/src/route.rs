use crate::controls::{self, SensorControlConfig};
use crate::fourcc::FourCc;
use crate::media::{self, MediaEntity, MediaGraph, MediaLink};
use crate::v4l2;
use std::collections::HashMap;
use std::io;
use std::path::PathBuf;

pub const MEDIA_BUS_FMT_SGRBG10_1X10: u32 = 0x300a;

const DEFAULT_MEDIA: &str = "/dev/media0";
const DEFAULT_RAW: &str = "/dev/video3";
const DEFAULT_SENSOR: &str = "ov13b10";
const DEFAULT_CSIPHY: &str = "msm_csiphy1";
const DEFAULT_CSID: &str = "msm_csid1";
const DEFAULT_VFE: &str = "msm_vfe1_rdi0";
const DEFAULT_VIDEO_ENTITY: &str = "msm_vfe1_video0";

#[derive(Debug, Clone)]
pub struct LmiRouteSetupReport {
    pub raw_node: PathBuf,
    pub control_subdev: PathBuf,
    pub format: v4l2::VideoFormat,
}

#[derive(Debug, Clone)]
pub struct LmiRouteConfig {
    pub media: PathBuf,
    pub raw_node: Option<PathBuf>,
    pub width: u32,
    pub height: u32,
    pub mbus_code: u32,
    pub pixelformat: FourCc,
    pub sensor: String,
    pub csiphy: String,
    pub csid: String,
    pub vfe: String,
    pub video_entity: String,
    pub csiphy_source_pad: u16,
    pub csid_source_pad: u16,
    pub sensor_subdev: Option<PathBuf>,
    pub csiphy_subdev: Option<PathBuf>,
    pub csid_subdev: Option<PathBuf>,
    pub vfe_subdev: Option<PathBuf>,
    pub keep_links: bool,
    pub controls: SensorControlConfig,
}

impl Default for LmiRouteConfig {
    fn default() -> Self {
        Self {
            media: PathBuf::from(DEFAULT_MEDIA),
            raw_node: None,
            width: 1364,
            height: 768,
            mbus_code: MEDIA_BUS_FMT_SGRBG10_1X10,
            pixelformat: FourCc::PGAA,
            sensor: DEFAULT_SENSOR.to_string(),
            csiphy: DEFAULT_CSIPHY.to_string(),
            csid: DEFAULT_CSID.to_string(),
            vfe: DEFAULT_VFE.to_string(),
            video_entity: DEFAULT_VIDEO_ENTITY.to_string(),
            csiphy_source_pad: 1,
            csid_source_pad: 1,
            sensor_subdev: None,
            csiphy_subdev: None,
            csid_subdev: None,
            vfe_subdev: None,
            keep_links: false,
            controls: SensorControlConfig::default(),
        }
    }
}

pub fn parse_lmi_route_config<I>(args: I) -> io::Result<LmiRouteConfig>
where
    I: Iterator<Item = String>,
{
    let mut config = LmiRouteConfig::default();
    let mut args = args.peekable();

    if matches!(args.peek().map(String::as_str), Some("lmi-ov13b10")) {
        args.next();
    }

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--media" => config.media = PathBuf::from(next_value(&mut args, "--media")?),
            "--raw" | "--video" => {
                config.raw_node = Some(PathBuf::from(next_value(&mut args, &arg)?))
            }
            "--width" => {
                config.width = parse_nonzero_u32(&next_value(&mut args, "--width")?, "--width")?
            }
            "--height" => {
                config.height = parse_nonzero_u32(&next_value(&mut args, "--height")?, "--height")?
            }
            "--size" => {
                let size = next_value(&mut args, "--size")?;
                let (width, height) = parse_size(&size)?;
                config.width = width;
                config.height = height;
            }
            "--mbus-code" => {
                config.mbus_code =
                    parse_u32_auto(&next_value(&mut args, "--mbus-code")?, "--mbus-code")?
            }
            "--pixelformat" | "--fourcc" => {
                config.pixelformat = parse_fourcc(&next_value(&mut args, &arg)?)?
            }
            "--sensor" => config.sensor = next_value(&mut args, "--sensor")?,
            "--csiphy" => config.csiphy = next_value(&mut args, "--csiphy")?,
            "--csid" => config.csid = next_value(&mut args, "--csid")?,
            "--vfe" => config.vfe = next_value(&mut args, "--vfe")?,
            "--video-entity" => config.video_entity = next_value(&mut args, "--video-entity")?,
            "--csiphy-source-pad" => {
                config.csiphy_source_pad = parse_u16(
                    &next_value(&mut args, "--csiphy-source-pad")?,
                    "--csiphy-source-pad",
                )?
            }
            "--csid-source-pad" => {
                config.csid_source_pad = parse_u16(
                    &next_value(&mut args, "--csid-source-pad")?,
                    "--csid-source-pad",
                )?
            }
            "--sensor-subdev" => {
                config.sensor_subdev =
                    Some(PathBuf::from(next_value(&mut args, "--sensor-subdev")?))
            }
            "--csiphy-subdev" => {
                config.csiphy_subdev =
                    Some(PathBuf::from(next_value(&mut args, "--csiphy-subdev")?))
            }
            "--csid-subdev" => {
                config.csid_subdev = Some(PathBuf::from(next_value(&mut args, "--csid-subdev")?))
            }
            "--vfe-subdev" => {
                config.vfe_subdev = Some(PathBuf::from(next_value(&mut args, "--vfe-subdev")?))
            }
            "--keep-links" => config.keep_links = true,
            "--vblank"
            | "--exposure"
            | "--analogue-gain"
            | "--digital-gain"
            | "--reset-controls"
            | "--preserve-controls" => {
                controls::parse_sensor_control_option(&mut config.controls, &arg, &mut args)?;
            }
            "--route" => {
                let route = next_value(&mut args, "--route")?;
                if route != "lmi-ov13b10" {
                    return Err(invalid_input(format!(
                        "unsupported --route '{route}', only lmi-ov13b10 is implemented"
                    )));
                }
            }
            other => {
                return Err(invalid_input(format!(
                    "unknown setup-route option: {other}"
                )));
            }
        }
    }

    validate_lmi_raw_config(config.mbus_code, config.pixelformat)?;
    Ok(config)
}

pub fn print_setup_route_usage() {
    println!("setup-route options:");
    println!("  --media DEV --size WxH --raw DEV");
    println!("  --sensor NAME --csiphy NAME --csid NAME --vfe NAME --video-entity NAME");
    println!("  --sensor-subdev DEV --csiphy-subdev DEV --csid-subdev DEV --vfe-subdev DEV");
    println!("  --mbus-code N --pixelformat pgAA --csiphy-source-pad N --csid-source-pad N");
    println!("  --keep-links");
    controls::print_sensor_control_usage();
}

pub fn setup_lmi_ov13b10_route(config: &LmiRouteConfig) -> io::Result<LmiRouteSetupReport> {
    validate_lmi_raw_config(config.mbus_code, config.pixelformat)?;
    let graph = media::read_media_graph(&config.media)?;
    let sensor = find_entity(&graph, &config.sensor)?.clone();
    let csiphy = find_entity(&graph, &config.csiphy)?.clone();
    let csid = find_entity(&graph, &config.csid)?.clone();
    let vfe = find_entity(&graph, &config.vfe)?.clone();
    let video = find_entity(&graph, &config.video_entity)?.clone();

    let sensor_node = config
        .sensor_subdev
        .clone()
        .or_else(|| sensor.devnode.clone())
        .ok_or_else(|| missing_devnode(&sensor))?;
    let csiphy_node = config
        .csiphy_subdev
        .clone()
        .or_else(|| csiphy.devnode.clone())
        .ok_or_else(|| missing_devnode(&csiphy))?;
    let csid_node = config
        .csid_subdev
        .clone()
        .or_else(|| csid.devnode.clone())
        .ok_or_else(|| missing_devnode(&csid))?;
    let vfe_node = config
        .vfe_subdev
        .clone()
        .or_else(|| vfe.devnode.clone())
        .ok_or_else(|| missing_devnode(&vfe))?;
    let raw_node = config
        .raw_node
        .clone()
        .or_else(|| video.devnode.clone())
        .unwrap_or_else(|| PathBuf::from(DEFAULT_RAW));

    println!("media {}", config.media.display());
    println!(
        "route {} -> {} -> {} -> {} -> {}",
        sensor.name, csiphy.name, csid.name, vfe.name, video.name
    );

    if config.keep_links {
        println!("keeping existing mutable CAMSS links");
    } else {
        disable_existing_camss_routes(config, &graph)?;
    }

    enable_link(config, &csiphy, config.csiphy_source_pad, &csid, 0)?;
    enable_link(config, &csid, config.csid_source_pad, &vfe, 0)?;

    set_subdev_pads(&sensor_node, &[0], config)?;
    set_subdev_pads(&csiphy_node, &[0, config.csiphy_source_pad], config)?;
    set_subdev_pads(&csid_node, &[0, config.csid_source_pad], config)?;
    set_subdev_pads(&vfe_node, &[0], config)?;

    let fmt = v4l2::set_capture_mplane_format(
        &raw_node,
        config.width,
        config.height,
        config.pixelformat,
    )?;
    println!(
        "raw {} format {} {}x{} planes={} bytesperline={} sizeimage={}",
        raw_node.display(),
        fmt.fourcc,
        fmt.width,
        fmt.height,
        fmt.num_planes,
        fmt.bytesperline,
        fmt.sizeimage
    );
    println!("control_subdev {}", sensor_node.display());
    println!(
        "raw_invariant {} remains truthful {}",
        raw_node.display(),
        config.pixelformat
    );
    controls::apply_initial_sensor_controls(&sensor_node, &config.controls, false)?;
    Ok(LmiRouteSetupReport {
        raw_node,
        control_subdev: sensor_node,
        format: fmt,
    })
}

fn disable_existing_camss_routes(config: &LmiRouteConfig, graph: &MediaGraph) -> io::Result<()> {
    let names: HashMap<u32, &str> = graph
        .entities
        .iter()
        .map(|entity| (entity.id, entity.name.as_str()))
        .collect();

    for link in &graph.links {
        if link.flags & media::MEDIA_LNK_FL_ENABLED == 0 {
            continue;
        }
        if link.flags & media::MEDIA_LNK_FL_IMMUTABLE != 0 {
            continue;
        }
        if !is_mutable_camss_route(link, &names) {
            continue;
        }
        let source = names.get(&link.source_entity).copied().unwrap_or("unknown");
        let sink = names.get(&link.sink_entity).copied().unwrap_or("unknown");
        media::setup_link(
            &config.media,
            link.source_entity,
            link.source_pad,
            link.sink_entity,
            link.sink_pad,
            0,
        )?;
        println!(
            "disabled link {}:{} -> {}:{}",
            source, link.source_pad, sink, link.sink_pad
        );
    }
    Ok(())
}

fn enable_link(
    config: &LmiRouteConfig,
    source: &MediaEntity,
    source_pad: u16,
    sink: &MediaEntity,
    sink_pad: u16,
) -> io::Result<()> {
    media::setup_link(
        &config.media,
        source.id,
        source_pad,
        sink.id,
        sink_pad,
        media::MEDIA_LNK_FL_ENABLED,
    )?;
    println!(
        "enabled link {}:{} -> {}:{}",
        source.name, source_pad, sink.name, sink_pad
    );
    Ok(())
}

fn set_subdev_pads(path: &PathBuf, pads: &[u16], config: &LmiRouteConfig) -> io::Result<()> {
    for pad in pads {
        v4l2::set_subdev_format(
            path,
            u32::from(*pad),
            config.width,
            config.height,
            config.mbus_code,
        )?;
        println!(
            "subdev {} pad{} mbus=0x{:04x} {}x{}",
            path.display(),
            pad,
            config.mbus_code,
            config.width,
            config.height
        );
    }
    Ok(())
}

fn find_entity<'a>(graph: &'a MediaGraph, needle: &str) -> io::Result<&'a MediaEntity> {
    let matches: Vec<&MediaEntity> = graph
        .entities
        .iter()
        .filter(|entity| entity.name.contains(needle))
        .collect();
    match matches.as_slice() {
        [entity] => Ok(entity),
        [] => Err(invalid_input(format!("media entity not found: {needle}"))),
        many => Err(invalid_input(format!(
            "multiple media entities match '{needle}': {}",
            many.iter()
                .map(|entity| entity.name.as_str())
                .collect::<Vec<_>>()
                .join(", ")
        ))),
    }
}

fn is_mutable_camss_route(link: &MediaLink, names: &HashMap<u32, &str>) -> bool {
    let source = names
        .get(&link.source_entity)
        .copied()
        .unwrap_or_default()
        .to_ascii_lowercase();
    let sink = names
        .get(&link.sink_entity)
        .copied()
        .unwrap_or_default()
        .to_ascii_lowercase();
    (source.starts_with("msm_csiphy") && sink.starts_with("msm_csid"))
        || (source.starts_with("msm_csid") && sink.starts_with("msm_vfe"))
}

fn missing_devnode(entity: &MediaEntity) -> io::Error {
    invalid_input(format!(
        "media entity '{}' has no resolved /dev node; pass the matching --*-subdev or --raw option",
        entity.name
    ))
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

fn validate_lmi_raw_config(mbus_code: u32, fourcc: FourCc) -> io::Result<()> {
    if mbus_code != MEDIA_BUS_FMT_SGRBG10_1X10 {
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

fn invalid_input(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_default_route_config() {
        let config = parse_lmi_route_config(std::iter::empty()).unwrap();
        assert_eq!(config.media, PathBuf::from(DEFAULT_MEDIA));
        assert_eq!(config.width, 1364);
        assert_eq!(config.height, 768);
        assert_eq!(config.pixelformat, FourCc::PGAA);
        assert!(!config.keep_links);
        assert_eq!(config.controls, SensorControlConfig::default());
    }

    #[test]
    fn parses_route_overrides() {
        let config = parse_lmi_route_config(
            [
                "--media",
                "/dev/media2",
                "--size",
                "1280x720",
                "--raw",
                "/dev/video9",
                "--mbus-code",
                "0x300a",
                "--pixelformat",
                "pgAA",
                "--keep-links",
            ]
            .into_iter()
            .map(String::from),
        )
        .unwrap();
        assert_eq!(config.media, PathBuf::from("/dev/media2"));
        assert_eq!(config.raw_node, Some(PathBuf::from("/dev/video9")));
        assert_eq!(config.width, 1280);
        assert_eq!(config.height, 720);
        assert_eq!(config.mbus_code, MEDIA_BUS_FMT_SGRBG10_1X10);
        assert!(config.keep_links);
    }

    #[test]
    fn parses_route_control_overrides() {
        let config = parse_lmi_route_config(
            [
                "--vblank",
                "44",
                "--exposure",
                "120",
                "--analogue-gain",
                "2",
                "--digital-gain",
                "3",
                "--reset-controls",
            ]
            .into_iter()
            .map(String::from),
        )
        .unwrap();
        assert_eq!(config.controls.vblank, Some(44));
        assert_eq!(config.controls.exposure, Some(120));
        assert_eq!(config.controls.analogue_gain, Some(2));
        assert_eq!(config.controls.digital_gain, Some(3));
        assert!(config.controls.reset_controls);
    }

    #[test]
    fn rejects_zero_route_size() {
        let err =
            parse_lmi_route_config(["--size", "0x768"].into_iter().map(String::from)).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
        assert!(
            err.to_string()
                .contains("--size width must be greater than zero")
        );
    }

    #[test]
    fn rejects_non_pg_raw_fourcc() {
        let err = parse_lmi_route_config(["--pixelformat", "YUYV"].into_iter().map(String::from))
            .unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
        assert!(err.to_string().contains("must remain truthful pgAA"));
    }

    #[test]
    fn rejects_non_ov13b10_raw_mbus_code() {
        let err = parse_lmi_route_config(["--mbus-code", "0x3017"].into_iter().map(String::from))
            .unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
        assert!(err.to_string().contains("must remain SGRBG10_1X10"));
    }
}
