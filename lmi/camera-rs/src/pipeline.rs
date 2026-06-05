use crate::fourcc::FourCc;
use std::thread;

#[derive(Debug, Clone)]
pub struct PipelinePlan {
    pub raw_ring_depth: usize,
    pub processed_ring_depth: usize,
    pub worker_threads: usize,
    pub sink_policy: &'static str,
}

impl PipelinePlan {
    pub fn performance_default() -> Self {
        let cpus = thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(4);
        Self {
            raw_ring_depth: 4,
            processed_ring_depth: 3,
            worker_threads: cpus.saturating_sub(2).max(1),
            sink_policy: "latest-complete-frame-wins",
        }
    }
}

pub fn print_pipeline_plan() {
    let plan = PipelinePlan::performance_default();
    println!("pipeline: local loopback is primary; UVC is optional");
    println!("capture: /dev/video3 RAW pgAA via MMAP");
    println!(
        "output:  /dev/video20 v4l2loopback {}/NV12 processed node",
        FourCc::YUYV
    );
    println!("raw_ring_depth={}", plan.raw_ring_depth);
    println!("processed_ring_depth={}", plan.processed_ring_depth);
    println!("worker_threads={}", plan.worker_threads);
    println!("sink_policy={}", plan.sink_policy);
    println!(
        "backpressure: drop old frames, never let slow consumers block RAW capture indefinitely"
    );
    println!(
        "first backend: supervise existing C lmi-isp; Rust/NEON ISP grows later after benchmarks"
    );
}
