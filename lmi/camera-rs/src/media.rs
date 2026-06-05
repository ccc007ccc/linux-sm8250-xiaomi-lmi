use crate::devices;
use std::collections::HashMap;
use std::fs::{self, OpenOptions};
use std::io;
use std::mem;
use std::os::fd::AsRawFd;
use std::os::raw::{c_int, c_ulong};
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::ptr;

const MEDIA_IOC_DEVICE_INFO: c_ulong = 0xc1007c00;
const MEDIA_IOC_ENUM_ENTITIES: c_ulong = 0xc1007c01;
const MEDIA_IOC_ENUM_LINKS: c_ulong = 0xc0287c02;
const MEDIA_IOC_SETUP_LINK: c_ulong = 0xc0347c03;

const MEDIA_ENT_ID_FLAG_NEXT: u32 = 1 << 31;
pub const MEDIA_LNK_FL_ENABLED: u32 = 1 << 0;
pub const MEDIA_LNK_FL_IMMUTABLE: u32 = 1 << 1;
pub const MEDIA_LNK_FL_DYNAMIC: u32 = 1 << 2;

const MEDIA_ENT_F_UNKNOWN: u32 = 0x0000_0000;
const MEDIA_ENT_F_IO_V4L: u32 = 0x0001_0001;
const MEDIA_ENT_F_CAM_SENSOR: u32 = 0x0002_0001;
const MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN: u32 = 0x0002_0000;
const MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER: u32 = 0x0000_4002;
const MEDIA_ENT_F_PROC_VIDEO_ISP: u32 = 0x0000_4009;
const MEDIA_ENT_F_VID_IF_BRIDGE: u32 = 0x0000_5002;

unsafe extern "C" {
    fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MediaDeviceInfoRaw {
    driver: [u8; 16],
    model: [u8; 32],
    serial: [u8; 40],
    bus_info: [u8; 32],
    media_version: u32,
    hw_revision: u32,
    driver_version: u32,
    reserved: [u32; 31],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MediaEntityDescRaw {
    id: u32,
    name: [u8; 32],
    type_: u32,
    revision: u32,
    flags: u32,
    group_id: u32,
    pads: u16,
    links: u16,
    reserved: [u32; 4],
    raw: [u8; 184],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
struct MediaPadDescRaw {
    entity: u32,
    index: u16,
    flags: u32,
    reserved: [u32; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
struct MediaLinkDescRaw {
    source: MediaPadDescRaw,
    sink: MediaPadDescRaw,
    flags: u32,
    reserved: [u32; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
struct MediaSetupLinkDescRaw {
    source: MediaPadDescRaw,
    sink: MediaPadDescRaw,
    flags: u32,
    reserved: [u32; 2],
}

#[repr(C)]
struct MediaLinksEnumRaw {
    entity: u32,
    pads: *mut MediaPadDescRaw,
    links: *mut MediaLinkDescRaw,
    reserved: [u32; 4],
}

#[derive(Debug, Clone)]
pub struct MediaDeviceInfo {
    pub driver: String,
    pub model: String,
    pub bus_info: String,
    pub media_version: u32,
    pub hw_revision: u32,
    pub driver_version: u32,
}

#[derive(Debug, Clone)]
pub struct MediaEntity {
    pub id: u32,
    pub name: String,
    pub function: u32,
    pub flags: u32,
    pub pads: u16,
    pub links: u16,
    pub dev_major: u32,
    pub dev_minor: u32,
    pub devnode: Option<PathBuf>,
}

#[derive(Debug, Clone)]
pub struct MediaPad {
    pub entity: u32,
    pub index: u16,
    pub flags: u32,
}

#[derive(Debug, Clone)]
pub struct MediaLink {
    pub source_entity: u32,
    pub source_pad: u16,
    pub sink_entity: u32,
    pub sink_pad: u16,
    pub flags: u32,
}

#[derive(Debug, Clone)]
pub struct MediaGraph {
    pub info: MediaDeviceInfo,
    pub entities: Vec<MediaEntity>,
    pub pads: Vec<MediaPad>,
    pub links: Vec<MediaLink>,
}

pub fn print_media_nodes() -> io::Result<()> {
    let media = devices::list_media_nodes()?;
    if media.is_empty() {
        println!("no /dev/media* nodes found");
        return Ok(());
    }

    for node in media {
        println!("media {}", node.path.display());
        match read_media_graph(&node.path) {
            Ok(graph) => print_media_graph(&graph),
            Err(err) => println!("  graph probe failed: {err}"),
        }
    }
    Ok(())
}

pub fn print_route_summary() -> io::Result<()> {
    let media_nodes = devices::list_media_nodes()?;
    if media_nodes.is_empty() {
        println!("no /dev/media* nodes found");
    }

    for node in media_nodes {
        println!("media {}", node.path.display());
        match read_media_graph(&node.path) {
            Ok(graph) => print_graph_route_summary(&graph),
            Err(err) => println!("  route summary failed: {err}"),
        }
    }

    let video_nodes = devices::list_video_nodes()?;
    match devices::default_raw_node(&video_nodes) {
        Some(raw) => println!(
            "default_raw={} name={}",
            raw.path.display(),
            raw.name.as_deref().unwrap_or("unknown")
        ),
        None => println!("default_raw=/dev/video3 missing"),
    }
    match devices::default_loopback_node(&video_nodes) {
        Some(loopback) => println!(
            "default_loopback={} name={}",
            loopback.path.display(),
            loopback.name.as_deref().unwrap_or("unknown")
        ),
        None => println!("default_loopback=/dev/video20 missing"),
    }
    println!("processed_kernel_yuv=unsupported");
    println!("uvc=optional-secondary-backend");
    Ok(())
}

pub fn read_media_graph(path: &Path) -> io::Result<MediaGraph> {
    let file = OpenOptions::new().read(true).open(path)?;
    let info = query_device_info(file.as_raw_fd())?;
    let entities = enum_entities(file.as_raw_fd())?;
    let (pads, links) = enum_all_links(file.as_raw_fd(), &entities)?;

    Ok(MediaGraph {
        info,
        entities,
        pads,
        links,
    })
}

pub fn setup_link(
    media: &Path,
    source_entity: u32,
    source_pad: u16,
    sink_entity: u32,
    sink_pad: u16,
    flags: u32,
) -> io::Result<()> {
    let file = OpenOptions::new().read(true).write(true).open(media)?;
    let mut raw = MediaSetupLinkDescRaw {
        source: MediaPadDescRaw {
            entity: source_entity,
            index: source_pad,
            flags: 0,
            reserved: [0; 2],
        },
        sink: MediaPadDescRaw {
            entity: sink_entity,
            index: sink_pad,
            flags: 0,
            reserved: [0; 2],
        },
        flags,
        reserved: [0; 2],
    };
    let ret = unsafe {
        ioctl(
            file.as_raw_fd(),
            MEDIA_IOC_SETUP_LINK,
            &mut raw as *mut MediaSetupLinkDescRaw,
        )
    };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn query_device_info(fd: c_int) -> io::Result<MediaDeviceInfo> {
    let mut raw: MediaDeviceInfoRaw = unsafe { mem::zeroed() };
    let ret = unsafe {
        ioctl(
            fd,
            MEDIA_IOC_DEVICE_INFO,
            &mut raw as *mut MediaDeviceInfoRaw,
        )
    };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(MediaDeviceInfo {
        driver: c_array_to_string(&raw.driver),
        model: c_array_to_string(&raw.model),
        bus_info: c_array_to_string(&raw.bus_info),
        media_version: raw.media_version,
        hw_revision: raw.hw_revision,
        driver_version: raw.driver_version,
    })
}

fn enum_entities(fd: c_int) -> io::Result<Vec<MediaEntity>> {
    let mut out = Vec::new();
    let mut next_id = MEDIA_ENT_ID_FLAG_NEXT;

    loop {
        let mut raw: MediaEntityDescRaw = unsafe { mem::zeroed() };
        raw.id = next_id;
        let ret = unsafe {
            ioctl(
                fd,
                MEDIA_IOC_ENUM_ENTITIES,
                &mut raw as *mut MediaEntityDescRaw,
            )
        };
        if ret < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(22) {
                break;
            }
            if out.is_empty() {
                return Err(err);
            }
            break;
        }

        let dev_major = u32::from_le_bytes([raw.raw[0], raw.raw[1], raw.raw[2], raw.raw[3]]);
        let dev_minor = u32::from_le_bytes([raw.raw[4], raw.raw[5], raw.raw[6], raw.raw[7]]);
        let entity = MediaEntity {
            id: raw.id,
            name: c_array_to_string(&raw.name),
            function: raw.type_,
            flags: raw.flags,
            pads: raw.pads,
            links: raw.links,
            dev_major,
            dev_minor,
            devnode: find_devnode(dev_major, dev_minor),
        };
        next_id = entity.id | MEDIA_ENT_ID_FLAG_NEXT;
        out.push(entity);
    }

    Ok(out)
}

fn enum_all_links(
    fd: c_int,
    entities: &[MediaEntity],
) -> io::Result<(Vec<MediaPad>, Vec<MediaLink>)> {
    let mut all_pads = Vec::new();
    let mut all_links = Vec::new();

    for entity in entities {
        if entity.pads == 0 && entity.links == 0 {
            continue;
        }

        let mut pads = vec![unsafe { mem::zeroed() }; entity.pads as usize];
        let mut links = vec![unsafe { mem::zeroed() }; entity.links as usize];
        let mut raw = MediaLinksEnumRaw {
            entity: entity.id,
            pads: if pads.is_empty() {
                ptr::null_mut()
            } else {
                pads.as_mut_ptr()
            },
            links: if links.is_empty() {
                ptr::null_mut()
            } else {
                links.as_mut_ptr()
            },
            reserved: [0; 4],
        };

        let ret = unsafe { ioctl(fd, MEDIA_IOC_ENUM_LINKS, &mut raw as *mut MediaLinksEnumRaw) };
        if ret < 0 {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(22) {
                continue;
            }
            return Err(err);
        }

        all_pads.extend(pads.iter().map(|pad| MediaPad {
            entity: pad.entity,
            index: pad.index,
            flags: pad.flags,
        }));
        all_links.extend(links.iter().map(|link| MediaLink {
            source_entity: link.source.entity,
            source_pad: link.source.index,
            sink_entity: link.sink.entity,
            sink_pad: link.sink.index,
            flags: link.flags,
        }));
    }

    all_pads.sort_by_key(|pad| (pad.entity, pad.index));
    all_pads.dedup_by_key(|pad| (pad.entity, pad.index));
    all_links.sort_by_key(|link| {
        (
            link.source_entity,
            link.source_pad,
            link.sink_entity,
            link.sink_pad,
            link.flags,
        )
    });
    all_links.dedup_by_key(|link| {
        (
            link.source_entity,
            link.source_pad,
            link.sink_entity,
            link.sink_pad,
            link.flags,
        )
    });

    Ok((all_pads, all_links))
}

fn print_media_graph(graph: &MediaGraph) {
    println!(
        "  driver='{}' model='{}' bus='{}' media=0x{:08x} hw=0x{:08x} drv=0x{:08x}",
        graph.info.driver,
        graph.info.model,
        graph.info.bus_info,
        graph.info.media_version,
        graph.info.hw_revision,
        graph.info.driver_version
    );
    println!(
        "  entities={} pads={} links={}",
        graph.entities.len(),
        graph.pads.len(),
        graph.links.len()
    );

    for entity in &graph.entities {
        let marker = if is_lmi_camera_entity(entity) {
            " *"
        } else {
            "  "
        };
        println!(
            "{marker} entity id={} name='{}' function={} pads={} links={} dev={} node={} flags=0x{:08x}",
            entity.id,
            entity.name,
            function_label(entity.function),
            entity.pads,
            entity.links,
            dev_label(entity),
            devnode_label(entity),
            entity.flags
        );
    }

    let names: HashMap<u32, &str> = graph
        .entities
        .iter()
        .map(|entity| (entity.id, entity.name.as_str()))
        .collect();
    let enabled: Vec<&MediaLink> = graph
        .links
        .iter()
        .filter(|link| link.flags & MEDIA_LNK_FL_ENABLED != 0)
        .collect();
    let flagged_pads: Vec<&MediaPad> = graph.pads.iter().filter(|pad| pad.flags != 0).collect();
    if !flagged_pads.is_empty() {
        println!("  pads:");
        for pad in flagged_pads {
            println!(
                "    {}:{} flags=0x{:08x}",
                entity_name(&names, pad.entity),
                pad.index,
                pad.flags
            );
        }
    }

    if !enabled.is_empty() {
        println!("  enabled links:");
        for link in enabled {
            println!(
                "    {}:{} -> {}:{} flags={}",
                entity_name(&names, link.source_entity),
                link.source_pad,
                entity_name(&names, link.sink_entity),
                link.sink_pad,
                link_flags_label(link.flags)
            );
        }
    }
}

fn is_lmi_camera_entity(entity: &MediaEntity) -> bool {
    let name = entity.name.to_ascii_lowercase();
    name.contains("ov13b10")
        || name.contains("camss")
        || name.contains("csiphy")
        || name.contains("csid")
        || name.contains("vfe")
        || name.contains("msm_vfe")
}

fn function_label(function: u32) -> String {
    let label = match function {
        MEDIA_ENT_F_UNKNOWN => "unknown",
        MEDIA_ENT_F_IO_V4L => "v4l-io",
        MEDIA_ENT_F_CAM_SENSOR => "camera-sensor",
        MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN => "v4l2-subdev",
        MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER => "pixel-formatter",
        MEDIA_ENT_F_PROC_VIDEO_ISP => "video-isp",
        MEDIA_ENT_F_VID_IF_BRIDGE => "video-interface-bridge",
        _ => return format!("0x{function:08x}"),
    };
    format!("{label}/0x{function:08x}")
}

fn find_devnode(dev_major: u32, dev_minor: u32) -> Option<PathBuf> {
    if dev_major == 0 && dev_minor == 0 {
        return None;
    }

    let entries = fs::read_dir("/dev").ok()?;
    let mut matches: Vec<PathBuf> = entries
        .filter_map(Result::ok)
        .filter_map(|entry| {
            let path = entry.path();
            let name = path.file_name()?.to_string_lossy();
            if !name.starts_with("video") && !name.starts_with("v4l-subdev") {
                return None;
            }
            let metadata = entry.metadata().ok()?;
            let rdev = metadata.rdev();
            if device_major(rdev) == dev_major && device_minor(rdev) == dev_minor {
                Some(path)
            } else {
                None
            }
        })
        .collect();
    matches.sort();
    matches.into_iter().next()
}

fn device_major(dev: u64) -> u32 {
    (((dev >> 8) & 0x0fff) | ((dev >> 32) & !0x0fff)) as u32
}

fn device_minor(dev: u64) -> u32 {
    ((dev & 0x00ff) | ((dev >> 12) & !0x00ff)) as u32
}

fn print_graph_route_summary(graph: &MediaGraph) {
    println!(
        "  graph driver='{}' model='{}' entities={} links={}",
        graph.info.driver,
        graph.info.model,
        graph.entities.len(),
        graph.links.len()
    );

    print_entity_group(
        "  sensors",
        graph
            .entities
            .iter()
            .filter(|entity| is_sensor_entity(entity)),
    );
    print_entity_group(
        "  camss_subdevs",
        graph
            .entities
            .iter()
            .filter(|entity| is_camss_entity(entity) && !is_video_node_entity(entity)),
    );
    print_entity_group(
        "  video_nodes",
        graph
            .entities
            .iter()
            .filter(|entity| is_video_node_entity(entity)),
    );

    let names: HashMap<u32, &str> = graph
        .entities
        .iter()
        .map(|entity| (entity.id, entity.name.as_str()))
        .collect();
    let camera_ids: Vec<u32> = graph
        .entities
        .iter()
        .filter(|entity| is_lmi_camera_entity(entity) || is_video_node_entity(entity))
        .map(|entity| entity.id)
        .collect();
    let enabled: Vec<&MediaLink> = graph
        .links
        .iter()
        .filter(|link| link.flags & MEDIA_LNK_FL_ENABLED != 0)
        .filter(|link| {
            camera_ids.contains(&link.source_entity) || camera_ids.contains(&link.sink_entity)
        })
        .collect();

    if enabled.is_empty() {
        println!("  enabled_camera_links=none");
    } else {
        println!("  enabled_camera_links:");
        for link in enabled {
            println!(
                "    {}:{} -> {}:{} flags={}",
                entity_name(&names, link.source_entity),
                link.source_pad,
                entity_name(&names, link.sink_entity),
                link.sink_pad,
                link_flags_label(link.flags)
            );
        }
    }
}

fn print_entity_group<'a>(title: &str, entities: impl Iterator<Item = &'a MediaEntity>) {
    let mut printed = false;
    for entity in entities {
        if !printed {
            println!("{title}:");
            printed = true;
        }
        println!(
            "    id={} name='{}' node={} dev={} function={}",
            entity.id,
            entity.name,
            devnode_label(entity),
            dev_label(entity),
            function_label(entity.function)
        );
    }
    if !printed {
        println!("{title}=none");
    }
}

fn is_sensor_entity(entity: &MediaEntity) -> bool {
    entity.function == MEDIA_ENT_F_CAM_SENSOR
        || entity.name.to_ascii_lowercase().contains("ov13b10")
}

fn is_camss_entity(entity: &MediaEntity) -> bool {
    let name = entity.name.to_ascii_lowercase();
    name.contains("camss")
        || name.contains("csiphy")
        || name.contains("csid")
        || name.contains("vfe")
        || name.contains("msm_vfe")
}

fn is_video_node_entity(entity: &MediaEntity) -> bool {
    entity.function == MEDIA_ENT_F_IO_V4L
        || entity
            .devnode
            .as_ref()
            .and_then(|path| path.file_name())
            .map(|name| name.to_string_lossy().starts_with("video"))
            .unwrap_or(false)
}

fn dev_label(entity: &MediaEntity) -> String {
    if entity.dev_major == 0 && entity.dev_minor == 0 {
        "-".to_string()
    } else {
        format!("{}:{}", entity.dev_major, entity.dev_minor)
    }
}

fn devnode_label(entity: &MediaEntity) -> String {
    entity
        .devnode
        .as_ref()
        .map(|path| path.display().to_string())
        .unwrap_or_else(|| "-".to_string())
}

fn entity_name<'a>(names: &'a HashMap<u32, &str>, id: u32) -> &'a str {
    names.get(&id).copied().unwrap_or("unknown")
}

fn link_flags_label(flags: u32) -> String {
    let mut out = Vec::new();
    if flags & MEDIA_LNK_FL_ENABLED != 0 {
        out.push("enabled");
    }
    if flags & MEDIA_LNK_FL_IMMUTABLE != 0 {
        out.push("immutable");
    }
    if flags & MEDIA_LNK_FL_DYNAMIC != 0 {
        out.push("dynamic");
    }
    if out.is_empty() {
        format!("0x{flags:08x}")
    } else {
        format!("{} 0x{flags:08x}", out.join("|"))
    }
}

fn c_array_to_string(bytes: &[u8]) -> String {
    let nul = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..nul]).trim().to_string()
}

#[allow(dead_code)]
fn _assert_layouts() {
    let _ = mem::size_of::<MediaDeviceInfoRaw>();
    let _ = mem::size_of::<MediaEntityDescRaw>();
    let _ = mem::size_of::<MediaPadDescRaw>();
    let _ = mem::size_of::<MediaLinkDescRaw>();
    let _ = mem::size_of::<MediaLinksEnumRaw>();
}
