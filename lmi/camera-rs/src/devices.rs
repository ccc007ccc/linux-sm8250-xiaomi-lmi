use std::fs;
use std::io;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone)]
pub struct VideoNode {
    pub path: PathBuf,
    pub name: Option<String>,
    pub driver: Option<String>,
    pub bus_info: Option<String>,
}

#[derive(Debug, Clone)]
pub struct MediaNode {
    pub path: PathBuf,
}

pub fn list_video_nodes() -> io::Result<Vec<VideoNode>> {
    let mut out = Vec::new();
    for entry in fs::read_dir("/dev")? {
        let entry = entry?;
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if !name.starts_with("video") {
            continue;
        }
        let path = entry.path();
        let sys_path = sys_video_path(&path);
        out.push(VideoNode {
            path,
            name: sys_path
                .as_deref()
                .and_then(|sys| read_trimmed_optional(&sys.join("name"))),
            driver: sys_path.as_deref().and_then(sys_video_driver),
            bus_info: sys_path.as_deref().and_then(sys_video_bus_info),
        });
    }
    out.sort_by_key(|n| video_number(&n.path).unwrap_or(u32::MAX));
    Ok(out)
}

pub fn list_media_nodes() -> io::Result<Vec<MediaNode>> {
    let mut out = Vec::new();
    for entry in fs::read_dir("/dev")? {
        let entry = entry?;
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if name.starts_with("media") {
            out.push(MediaNode { path: entry.path() });
        }
    }
    out.sort_by_key(|n| media_number(&n.path).unwrap_or(u32::MAX));
    Ok(out)
}

pub fn default_raw_node(nodes: &[VideoNode]) -> Option<&VideoNode> {
    nodes.iter().find(|n| n.path == Path::new("/dev/video3"))
}

pub fn default_loopback_node(nodes: &[VideoNode]) -> Option<&VideoNode> {
    nodes.iter().find(|n| n.path == Path::new("/dev/video20"))
}

pub fn default_venus_encoder_node(nodes: &[VideoNode]) -> Option<&VideoNode> {
    nodes.iter().find(|n| {
        n.name.as_deref().is_some_and(|name| {
            let name = name.to_ascii_lowercase();
            name == "qcom-venus-encoder" || name.contains("venus-encoder")
        })
    })
}

fn sys_video_path(path: &Path) -> Option<PathBuf> {
    let name = path.file_name()?.to_string_lossy();
    Some(PathBuf::from("/sys/class/video4linux").join(name.as_ref()))
}

fn sys_video_driver(sys: &Path) -> Option<String> {
    let target = fs::read_link(sys.join("device/driver")).ok()?;
    target.file_name().map(|s| s.to_string_lossy().into_owned())
}

fn sys_video_bus_info(sys: &Path) -> Option<String> {
    read_trimmed_optional(&sys.join("device/modalias"))
}

fn read_trimmed_optional(path: &Path) -> Option<String> {
    fs::read_to_string(path)
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
}

fn video_number(path: &Path) -> Option<u32> {
    let file = path.file_name()?.to_string_lossy();
    file.strip_prefix("video")?.parse().ok()
}

fn media_number(path: &Path) -> Option<u32> {
    let file = path.file_name()?.to_string_lossy();
    file.strip_prefix("media")?.parse().ok()
}
