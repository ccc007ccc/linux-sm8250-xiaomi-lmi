use crate::pipeline::PipelinePlan;
use std::fmt;
use std::path::PathBuf;
use std::str::FromStr;

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum IspPixelFormat {
    Yuyv,
    Nv12,
}

impl IspPixelFormat {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Yuyv => "yuyv",
            Self::Nv12 => "nv12",
        }
    }
}

impl fmt::Display for IspPixelFormat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

impl FromStr for IspPixelFormat {
    type Err = String;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "yuyv" | "YUYV" => Ok(Self::Yuyv),
            "nv12" | "NV12" => Ok(Self::Nv12),
            _ => Err(format!("unsupported ISP output format '{value}'")),
        }
    }
}

#[derive(Debug, Clone)]
pub struct LocalLoopbackProfile {
    pub raw_node: PathBuf,
    pub loopback_node: PathBuf,
    pub ctrl_node: Option<PathBuf>,
    pub isp_bin: PathBuf,
    pub out_width: u32,
    pub out_height: u32,
    pub fps_cap: u32,
    pub format: IspPixelFormat,
    pub gamma: f32,
    pub tone_highlight_knee: u32,
    pub tone_highlight_max: u32,
    pub max_soft_gain: f32,
    pub auto_exposure: bool,
    pub ae_target: u32,
    pub pipeline: PipelinePlan,
}

impl Default for LocalLoopbackProfile {
    fn default() -> Self {
        Self {
            raw_node: PathBuf::from("/dev/video3"),
            loopback_node: PathBuf::from("/dev/video20"),
            ctrl_node: None,
            isp_bin: PathBuf::from("/run/lmi-camera/lmi-isp"),
            out_width: 1280,
            out_height: 720,
            fps_cap: 30,
            format: IspPixelFormat::Yuyv,
            gamma: 2.2,
            tone_highlight_knee: 0,
            tone_highlight_max: 255,
            max_soft_gain: 4.0,
            auto_exposure: true,
            ae_target: 110,
            pipeline: PipelinePlan::performance_default(),
        }
    }
}

impl LocalLoopbackProfile {
    pub fn print(&self) {
        println!("profile=local-loopback");
        println!("primary=local Linux camera applications");
        println!("secondary=USB UVC optional backend");
        println!("raw_node={}", self.raw_node.display());
        println!("raw_fourcc=pgAA");
        println!("processed_node={}", self.loopback_node.display());
        println!("processed_format={}", self.format);
        println!("isp_bin={}", self.isp_bin.display());
        println!("out_size={}x{}", self.out_width, self.out_height);
        println!("fps_cap={}", self.fps_cap);
        println!("gamma={:.2}", self.gamma);
        println!("tone_highlight_knee={}", self.tone_highlight_knee);
        println!("tone_highlight_max={}", self.tone_highlight_max);
        println!("max_soft_gain={:.2}", self.max_soft_gain);
        println!("auto_exposure_requested={}", yes_no(self.auto_exposure));
        println!("ae_target={}", self.ae_target);
        match &self.ctrl_node {
            Some(ctrl) => println!("ctrl_node={}", ctrl.display()),
            None => println!("ctrl_node=auto-discovery-planned"),
        }
        println!("raw_ring_depth={}", self.pipeline.raw_ring_depth);
        println!(
            "processed_ring_depth={}",
            self.pipeline.processed_ring_depth
        );
        println!("worker_threads={}", self.pipeline.worker_threads);
        println!("sink_policy={}", self.pipeline.sink_policy);
        println!("backpressure=drop-old-frames-keep-latest");
    }
}

fn yes_no(value: bool) -> &'static str {
    if value { "yes" } else { "no" }
}
