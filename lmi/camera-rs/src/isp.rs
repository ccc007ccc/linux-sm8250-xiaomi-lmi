use crate::config::{IspPixelFormat, LocalLoopbackProfile};
use std::io;
use std::os::raw::c_int;
use std::path::PathBuf;
use std::process::{Child, Command, ExitStatus, Stdio};
use std::thread;
use std::time::{Duration, Instant};

const SIGTERM: c_int = 15;
const TERMINATE_GRACE: Duration = Duration::from_millis(800);

unsafe extern "C" {
    fn kill(pid: c_int, sig: c_int) -> c_int;
}

#[derive(Debug, Clone)]
pub struct IspCommand {
    pub program: PathBuf,
    pub args: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct UvcIspProfile {
    pub raw_node: PathBuf,
    pub ctrl_node: Option<PathBuf>,
    pub isp_bin: PathBuf,
    pub fifo: PathBuf,
    pub control_fifo: Option<PathBuf>,
    pub out_width: u32,
    pub out_height: u32,
    pub fps_cap: u32,
    pub gamma: f32,
    pub tone_highlight_knee: u32,
    pub tone_highlight_max: u32,
    pub max_soft_gain: f32,
    pub auto_exposure: bool,
    pub ae_target: u32,
    pub ae_clip_target: Option<u32>,
    pub ae_clip_weight: u32,
    pub max_digital_gain: Option<u32>,
    pub max_frame_bytes: u32,
    pub mjpeg_quality: u32,
    pub mjpeg_sharpen: u32,
    pub mjpeg_smooth: u32,
    pub mjpeg_area_scale: u32,
    pub mjpeg_subsampling: u32,
    pub mjpeg_scale_mode: String,
    pub mjpeg_fast_threads: u32,
    pub cpu_affinity: Option<String>,
}

impl IspCommand {
    pub fn for_local_loopback(profile: &LocalLoopbackProfile) -> Self {
        let mut args = vec![
            "--raw".to_string(),
            profile.raw_node.display().to_string(),
            "--out-width".to_string(),
            profile.out_width.to_string(),
            "--out-height".to_string(),
            profile.out_height.to_string(),
            "--gamma".to_string(),
            format_float(profile.gamma),
            "--tone-highlight-knee".to_string(),
            profile.tone_highlight_knee.to_string(),
            "--tone-highlight-max".to_string(),
            profile.tone_highlight_max.to_string(),
            "--fps-cap".to_string(),
            profile.fps_cap.to_string(),
            "--max-soft-gain".to_string(),
            format_float(profile.max_soft_gain),
            "--loopback".to_string(),
            profile.loopback_node.display().to_string(),
        ];

        if profile.format == IspPixelFormat::Nv12 {
            args.push("--nv12".to_string());
        }

        if let Some(ctrl) = &profile.ctrl_node {
            args.push("--ctrl".to_string());
            args.push(ctrl.display().to_string());
        }

        if profile.auto_exposure {
            args.push("--auto-exposure".to_string());
            args.push("--target".to_string());
            args.push(profile.ae_target.to_string());
        }

        Self {
            program: profile.isp_bin.clone(),
            args,
        }
    }

    pub fn for_uvc_mjpeg(profile: &UvcIspProfile) -> Self {
        let mut args = uvc_base_args(profile);
        args.push("--mjpeg".to_string());
        args.push("--mjpeg-quality".to_string());
        args.push(profile.mjpeg_quality.to_string());
        args.push("--mjpeg-sharpen".to_string());
        args.push(profile.mjpeg_sharpen.to_string());
        args.push("--mjpeg-smooth".to_string());
        args.push(profile.mjpeg_smooth.to_string());
        args.push("--mjpeg-area-scale".to_string());
        args.push(profile.mjpeg_area_scale.to_string());
        args.push("--mjpeg-subsampling".to_string());
        args.push(profile.mjpeg_subsampling.to_string());
        args.push("--mjpeg-scale-mode".to_string());
        args.push(profile.mjpeg_scale_mode.clone());
        args.push("--mjpeg-fast-threads".to_string());
        args.push(profile.mjpeg_fast_threads.to_string());
        args.push("--max-frame-bytes".to_string());
        args.push(profile.max_frame_bytes.to_string());
        add_uvc_control_args(profile, &mut args);

        Self {
            program: profile.isp_bin.clone(),
            args,
        }
    }

    pub fn for_uvc_nv12(profile: &UvcIspProfile) -> Self {
        let mut args = uvc_base_args(profile);
        args.push("--nv12".to_string());
        add_uvc_control_args(profile, &mut args);

        Self {
            program: profile.isp_bin.clone(),
            args,
        }
    }

    pub fn print_shell(&self) {
        println!("{}", self.shell_words().join(" "));
    }

    pub fn spawn(&self) -> io::Result<IspSupervisor> {
        let child = Command::new(&self.program)
            .args(&self.args)
            .stdin(Stdio::null())
            .stdout(Stdio::inherit())
            .stderr(Stdio::inherit())
            .spawn()?;
        Ok(IspSupervisor { child })
    }

    fn shell_words(&self) -> Vec<String> {
        std::iter::once(shell_quote(&self.program.display().to_string()))
            .chain(self.args.iter().map(|arg| shell_quote(arg)))
            .collect()
    }
}

pub struct IspSupervisor {
    child: Child,
}

impl IspSupervisor {
    pub fn id(&self) -> u32 {
        self.child.id()
    }

    pub fn try_wait(&mut self) -> io::Result<Option<ExitStatus>> {
        self.child.try_wait()
    }

    pub fn wait(&mut self) -> io::Result<ExitStatus> {
        self.child.wait()
    }

    pub fn terminate(&mut self) -> io::Result<()> {
        if self.try_wait()?.is_some() {
            return Ok(());
        }

        let pid = self.child.id() as c_int;
        let ret = unsafe { kill(pid, SIGTERM) };
        if ret < 0 {
            let err = io::Error::last_os_error();
            if self.try_wait()?.is_none() {
                self.child.kill()?;
            }
            let _ = self.child.wait();
            return Err(err);
        }

        let deadline = Instant::now() + TERMINATE_GRACE;
        while Instant::now() < deadline {
            if self.try_wait()?.is_some() {
                return Ok(());
            }
            thread::sleep(Duration::from_millis(20));
        }

        if self.try_wait()?.is_none() {
            self.child.kill()?;
            let _ = self.child.wait();
        }
        Ok(())
    }
}

impl Drop for IspSupervisor {
    fn drop(&mut self) {
        let _ = self.terminate();
    }
}

fn uvc_base_args(profile: &UvcIspProfile) -> Vec<String> {
    vec![
        "--raw".to_string(),
        profile.raw_node.display().to_string(),
        "--out-width".to_string(),
        profile.out_width.to_string(),
        "--out-height".to_string(),
        profile.out_height.to_string(),
        "--source-aspect".to_string(),
        "preserve".to_string(),
        "--gamma".to_string(),
        format_float(profile.gamma),
        "--tone-highlight-knee".to_string(),
        profile.tone_highlight_knee.to_string(),
        "--tone-highlight-max".to_string(),
        profile.tone_highlight_max.to_string(),
        "--fps-cap".to_string(),
        profile.fps_cap.to_string(),
        "--max-soft-gain".to_string(),
        format_float(profile.max_soft_gain),
        "--fifo".to_string(),
        profile.fifo.display().to_string(),
    ]
}

fn add_uvc_control_args(profile: &UvcIspProfile, args: &mut Vec<String>) {
    if let Some(ctrl) = &profile.ctrl_node {
        args.push("--ctrl".to_string());
        args.push(ctrl.display().to_string());
    }

    if let Some(control_fifo) = &profile.control_fifo {
        args.push("--control-fifo".to_string());
        args.push(control_fifo.display().to_string());
    }

    if profile.auto_exposure {
        args.push("--auto-exposure".to_string());
        args.push("--target".to_string());
        args.push(profile.ae_target.to_string());
        if let Some(target) = profile.ae_clip_target {
            args.push("--ae-clip-target".to_string());
            args.push(target.to_string());
            args.push("--ae-clip-weight".to_string());
            args.push(profile.ae_clip_weight.to_string());
        }
        if let Some(max_gain) = profile.max_digital_gain {
            args.push("--max-digital-gain".to_string());
            args.push(max_gain.to_string());
        }
    }

    if let Some(cpu_affinity) = &profile.cpu_affinity {
        args.push("--cpu-affinity".to_string());
        args.push(cpu_affinity.clone());
    }
}

fn format_float(value: f32) -> String {
    let mut out = format!("{value:.3}");
    while out.contains('.') && out.ends_with('0') {
        out.pop();
    }
    if out.ends_with('.') {
        out.push('0');
    }
    out
}

fn shell_quote(value: &str) -> String {
    if value
        .bytes()
        .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'/' | b'.' | b'_' | b'-' | b':' | b'='))
    {
        return value.to_string();
    }

    let escaped = value.replace("'", "'\\''");
    format!("'{escaped}'")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn local_loopback_command_uses_loopback_not_fifo() {
        let profile = LocalLoopbackProfile::default();
        let command = IspCommand::for_local_loopback(&profile);

        assert_eq!(command.program, PathBuf::from("/run/lmi-camera/lmi-isp"));
        assert!(has_pair(&command.args, "--raw", "/dev/video3"));
        assert!(has_pair(&command.args, "--loopback", "/dev/video20"));
        assert!(has_pair(&command.args, "--out-width", "1280"));
        assert!(has_pair(&command.args, "--out-height", "720"));
        assert!(has_arg(&command.args, "--auto-exposure"));
        assert!(!has_arg(&command.args, "--fifo"));
        assert!(!has_arg(&command.args, "--mjpeg"));
    }

    #[test]
    fn local_loopback_command_respects_nv12_and_no_ae() {
        let profile = LocalLoopbackProfile {
            out_width: 640,
            out_height: 480,
            fps_cap: 15,
            format: IspPixelFormat::Nv12,
            auto_exposure: false,
            ..LocalLoopbackProfile::default()
        };
        let command = IspCommand::for_local_loopback(&profile);

        assert!(has_pair(&command.args, "--out-width", "640"));
        assert!(has_pair(&command.args, "--out-height", "480"));
        assert!(has_pair(&command.args, "--fps-cap", "15"));
        assert!(has_arg(&command.args, "--nv12"));
        assert!(!has_arg(&command.args, "--auto-exposure"));
        assert!(!has_arg(&command.args, "--target"));
    }

    #[test]
    fn uvc_mjpeg_command_passes_control_fifo() {
        let profile = UvcIspProfile {
            raw_node: PathBuf::from("/dev/video3"),
            ctrl_node: Some(PathBuf::from("/dev/v4l-subdev13")),
            isp_bin: PathBuf::from("/run/lmi-camera/lmi-isp"),
            fifo: PathBuf::from("/run/lmi-camera/lmi-uvc.fifo"),
            control_fifo: Some(PathBuf::from("/run/lmi-camera/lmi-isp.control")),
            out_width: 1364,
            out_height: 768,
            fps_cap: 120,
            gamma: 3.0,
            tone_highlight_knee: 0,
            tone_highlight_max: 255,
            max_soft_gain: 3.5,
            auto_exposure: true,
            ae_target: 85,
            ae_clip_target: Some(620),
            ae_clip_weight: 50,
            max_digital_gain: Some(1024),
            max_frame_bytes: 4 * 1024 * 1024,
            mjpeg_quality: 90,
            mjpeg_sharpen: 0,
            mjpeg_smooth: 0,
            mjpeg_area_scale: 100,
            mjpeg_subsampling: 420,
            mjpeg_scale_mode: "bayer-area-frac".to_string(),
            mjpeg_fast_threads: 4,
            cpu_affinity: Some("4-7".to_string()),
        };
        let command = IspCommand::for_uvc_mjpeg(&profile);

        assert!(has_pair(
            &command.args,
            "--fifo",
            "/run/lmi-camera/lmi-uvc.fifo"
        ));
        assert!(has_pair(&command.args, "--ctrl", "/dev/v4l-subdev13"));
        assert!(has_pair(
            &command.args,
            "--control-fifo",
            "/run/lmi-camera/lmi-isp.control"
        ));
        assert!(has_arg(&command.args, "--auto-exposure"));
    }

    #[test]
    fn shell_quote_preserves_copyable_command_lines() {
        assert_eq!(
            shell_quote("/run/lmi-camera/lmi-isp"),
            "/run/lmi-camera/lmi-isp"
        );
        assert_eq!(shell_quote("/tmp/with space"), "'/tmp/with space'");
        assert_eq!(shell_quote("a'b"), "'a'\\''b'");
    }

    fn has_arg(args: &[String], key: &str) -> bool {
        args.iter().any(|arg| arg == key)
    }

    fn has_pair(args: &[String], key: &str, value: &str) -> bool {
        args.windows(2)
            .any(|pair| pair[0] == key && pair[1] == value)
    }
}
