use crate::fourcc::FourCc;
use std::fs::OpenOptions;
use std::io;
use std::mem;
use std::os::fd::AsRawFd;
use std::os::raw::{c_int, c_long, c_short, c_ulong, c_void};
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;
use std::time::{Duration, Instant};

const VIDIOC_QUERYCAP: c_ulong = 0x80685600;
const VIDIOC_ENUM_FMT: c_ulong = 0xc0405602;
const VIDIOC_S_FMT: c_ulong = 0xc0d05605;
const VIDIOC_REQBUFS: c_ulong = 0xc0145608;
const VIDIOC_QUERYBUF: c_ulong = 0xc0585609;
const VIDIOC_QBUF: c_ulong = 0xc058560f;
const VIDIOC_DQBUF: c_ulong = 0xc0585611;
const VIDIOC_STREAMON: c_ulong = 0x40045612;
const VIDIOC_STREAMOFF: c_ulong = 0x40045613;
const VIDIOC_SUBDEV_S_FMT: c_ulong = 0xc0585605;

const V4L2_BUF_TYPE_VIDEO_CAPTURE: u32 = 1;
const V4L2_BUF_TYPE_VIDEO_OUTPUT: u32 = 2;
const V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE: u32 = 9;
const V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE: u32 = 10;
const V4L2_MEMORY_MMAP: u32 = 1;
const V4L2_FIELD_NONE: u32 = 1;
const V4L2_SUBDEV_FORMAT_ACTIVE: u32 = 1;

const O_NONBLOCK: c_int = 0o4000;
const PROT_READ: c_int = 0x1;
const PROT_WRITE: c_int = 0x2;
const MAP_SHARED: c_int = 0x01;
const POLLIN: c_short = 0x0001;
const EAGAIN: i32 = 11;

unsafe extern "C" {
    fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
    fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: c_long,
    ) -> *mut c_void;
    fn munmap(addr: *mut c_void, length: usize) -> c_int;
    fn poll(fds: *mut PollFdRaw, nfds: c_ulong, timeout: c_int) -> c_int;
}

#[repr(C)]
#[derive(Clone, Copy)]
struct PollFdRaw {
    fd: c_int,
    events: c_short,
    revents: c_short,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2Capability {
    driver: [u8; 16],
    card: [u8; 32],
    bus_info: [u8; 32],
    version: u32,
    capabilities: u32,
    device_caps: u32,
    reserved: [u32; 3],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2Fmtdesc {
    index: u32,
    type_: u32,
    flags: u32,
    description: [u8; 32],
    pixelformat: u32,
    mbus_code: u32,
    reserved: [u32; 3],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MbusFrameFormatRaw {
    width: u32,
    height: u32,
    code: u32,
    field: u32,
    colorspace: u32,
    ycbcr_enc: u16,
    quantization: u16,
    xfer_func: u16,
    flags: u16,
    reserved: [u16; 10],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SubdevFormatRaw {
    which: u32,
    pad: u32,
    format: MbusFrameFormatRaw,
    stream: u32,
    reserved: [u32; 7],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2PlanePixFormatRaw {
    sizeimage: u32,
    bytesperline: u32,
    reserved: [u16; 6],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2PixFormatMplaneRaw {
    width: u32,
    height: u32,
    pixelformat: u32,
    field: u32,
    colorspace: u32,
    plane_fmt: [V4l2PlanePixFormatRaw; 8],
    num_planes: u8,
    flags: u8,
    ycbcr_enc: u8,
    quantization: u8,
    xfer_func: u8,
    reserved: [u8; 7],
}

#[repr(C)]
#[derive(Clone, Copy)]
union V4l2FormatUnionRaw {
    pix_mp: V4l2PixFormatMplaneRaw,
    raw_data: [u8; 200],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2FormatRaw {
    type_: u32,
    pad0: u32,
    fmt: V4l2FormatUnionRaw,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2RequestBuffersRaw {
    count: u32,
    type_: u32,
    memory: u32,
    capabilities: u32,
    flags: u8,
    reserved: [u8; 3],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2TimevalRaw {
    tv_sec: c_long,
    tv_usec: c_long,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2TimecodeRaw {
    type_: u32,
    flags: u32,
    frames: u8,
    seconds: u8,
    minutes: u8,
    hours: u8,
    userbits: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy)]
union V4l2PlaneMemoryRaw {
    mem_offset: u32,
    userptr: c_ulong,
    fd: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2PlaneRaw {
    bytesused: u32,
    length: u32,
    m: V4l2PlaneMemoryRaw,
    data_offset: u32,
    reserved: [u32; 11],
}

#[repr(C)]
#[derive(Clone, Copy)]
union V4l2BufferMemoryRaw {
    offset: u32,
    userptr: c_ulong,
    planes: *mut V4l2PlaneRaw,
    fd: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct V4l2BufferRaw {
    index: u32,
    type_: u32,
    bytesused: u32,
    flags: u32,
    field: u32,
    timestamp: V4l2TimevalRaw,
    timecode: V4l2TimecodeRaw,
    sequence: u32,
    memory: u32,
    m: V4l2BufferMemoryRaw,
    length: u32,
    reserved2: u32,
    request_fd: c_int,
}

#[derive(Debug, Clone)]
pub struct Capability {
    pub driver: String,
    pub card: String,
    pub bus_info: String,
    pub capabilities: u32,
    pub device_caps: u32,
}

#[derive(Debug, Clone)]
pub struct FormatDescription {
    pub index: u32,
    pub buffer_type: u32,
    pub description: String,
    pub fourcc: FourCc,
}

#[derive(Debug, Clone)]
pub struct VideoFormat {
    pub width: u32,
    pub height: u32,
    pub fourcc: FourCc,
    pub bytesperline: u32,
    pub sizeimage: u32,
    pub num_planes: u8,
}

impl VideoFormat {
    pub fn validate_requested(&self, requested: FourCc) -> io::Result<()> {
        if self.fourcc != requested {
            return Err(io::Error::other(format!(
                "driver returned {} instead of requested {}",
                self.fourcc, requested
            )));
        }
        Ok(())
    }
}

#[derive(Debug, Clone)]
pub struct CapturedFrame {
    pub index: u32,
    pub sequence: u32,
    pub bytesused: u32,
}

#[derive(Debug, Clone)]
pub struct CaptureReport {
    pub format: VideoFormat,
    pub buffers: u32,
    pub frames: Vec<CapturedFrame>,
}

pub fn query_capability(path: &Path) -> io::Result<Capability> {
    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .or_else(|_| OpenOptions::new().read(true).open(path))?;
    let mut cap: V4l2Capability = unsafe { mem::zeroed() };
    let ret = unsafe {
        ioctl(
            file.as_raw_fd(),
            VIDIOC_QUERYCAP,
            &mut cap as *mut V4l2Capability,
        )
    };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(Capability {
        driver: c_array_to_string(&cap.driver),
        card: c_array_to_string(&cap.card),
        bus_info: c_array_to_string(&cap.bus_info),
        capabilities: cap.capabilities,
        device_caps: cap.device_caps,
    })
}

pub fn enum_formats(path: &Path) -> io::Result<Vec<FormatDescription>> {
    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .or_else(|_| OpenOptions::new().read(true).open(path))?;
    let mut out = Vec::new();
    for buffer_type in [
        V4L2_BUF_TYPE_VIDEO_CAPTURE,
        V4L2_BUF_TYPE_VIDEO_OUTPUT,
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    ] {
        let mut index = 0;
        loop {
            let mut desc: V4l2Fmtdesc = unsafe { mem::zeroed() };
            desc.index = index;
            desc.type_ = buffer_type;
            let ret = unsafe {
                ioctl(
                    file.as_raw_fd(),
                    VIDIOC_ENUM_FMT,
                    &mut desc as *mut V4l2Fmtdesc,
                )
            };
            if ret < 0 {
                let err = io::Error::last_os_error();
                if index == 0 {
                    break;
                }
                if err.raw_os_error() == Some(22) {
                    break;
                }
                break;
            }
            out.push(FormatDescription {
                index,
                buffer_type,
                description: c_array_to_string(&desc.description),
                fourcc: FourCc(desc.pixelformat),
            });
            index += 1;
        }
    }
    Ok(out)
}

pub fn has_format(path: &Path, fourcc: FourCc) -> bool {
    enum_formats(path)
        .map(|formats| formats.iter().any(|f| f.fourcc == fourcc))
        .unwrap_or(false)
}

pub fn set_subdev_format(
    path: &Path,
    pad: u32,
    width: u32,
    height: u32,
    mbus_code: u32,
) -> io::Result<()> {
    let file = OpenOptions::new().read(true).write(true).open(path)?;
    let mut fmt = SubdevFormatRaw {
        which: V4L2_SUBDEV_FORMAT_ACTIVE,
        pad,
        format: MbusFrameFormatRaw {
            width,
            height,
            code: mbus_code,
            field: V4L2_FIELD_NONE,
            colorspace: 0,
            ycbcr_enc: 0,
            quantization: 0,
            xfer_func: 0,
            flags: 0,
            reserved: [0; 10],
        },
        stream: 0,
        reserved: [0; 7],
    };
    let ret = unsafe {
        ioctl(
            file.as_raw_fd(),
            VIDIOC_SUBDEV_S_FMT,
            &mut fmt as *mut SubdevFormatRaw,
        )
    };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

pub fn set_capture_mplane_format(
    path: &Path,
    width: u32,
    height: u32,
    fourcc: FourCc,
) -> io::Result<VideoFormat> {
    let file = OpenOptions::new().read(true).write(true).open(path)?;
    let format = set_capture_mplane_format_fd(file.as_raw_fd(), width, height, fourcc)?;
    format.validate_requested(fourcc)?;
    Ok(format)
}

pub fn capture_mplane_mmap(
    path: &Path,
    width: u32,
    height: u32,
    fourcc: FourCc,
    buffer_count: u32,
    frame_count: u32,
    frame_timeout: Duration,
) -> io::Result<CaptureReport> {
    if buffer_count == 0 {
        return Err(invalid_input("buffer count must be greater than zero"));
    }
    if frame_count == 0 {
        return Err(invalid_input("frame count must be greater than zero"));
    }

    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .custom_flags(O_NONBLOCK)
        .open(path)?;
    let fd = file.as_raw_fd();
    let format = set_capture_mplane_format_fd(fd, width, height, fourcc)?;
    format.validate_requested(fourcc)?;

    let allocated = request_mmap_buffers(fd, buffer_count)?;
    if allocated == 0 {
        return Err(io::Error::other("driver allocated zero capture buffers"));
    }
    let mut pool = BufferPoolGuard::new(fd);

    let mut mapped = Vec::with_capacity(allocated as usize);
    for index in 0..allocated {
        mapped.push(query_and_mmap_buffer(fd, index)?);
    }

    for index in 0..allocated {
        enqueue_buffer(fd, index)?;
    }

    let mut stream = StreamGuard::new(fd);
    stream.start()?;

    let mut frames = Vec::with_capacity(frame_count as usize);
    for _ in 0..frame_count {
        let (buffer, plane) = dequeue_buffer_with_timeout(fd, frame_timeout)?;
        let buffer_index = buffer.index;
        frames.push(CapturedFrame {
            index: buffer_index,
            sequence: buffer.sequence,
            bytesused: plane.bytesused,
        });
        if let Err(err) = enqueue_buffer(fd, buffer_index) {
            let _ = stream.stop();
            return Err(err);
        }
    }

    stream.stop()?;
    drop(mapped);
    pool.release()?;

    Ok(CaptureReport {
        format,
        buffers: allocated,
        frames,
    })
}

fn set_capture_mplane_format_fd(
    fd: c_int,
    width: u32,
    height: u32,
    fourcc: FourCc,
) -> io::Result<VideoFormat> {
    let mut fmt = V4l2FormatRaw {
        type_: V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        pad0: 0,
        fmt: V4l2FormatUnionRaw { raw_data: [0; 200] },
    };
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = fourcc.0;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    let ret = unsafe { ioctl(fd, VIDIOC_S_FMT, &mut fmt as *mut V4l2FormatRaw) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    let pix = unsafe { fmt.fmt.pix_mp };
    Ok(VideoFormat {
        width: pix.width,
        height: pix.height,
        fourcc: FourCc(pix.pixelformat),
        bytesperline: pix.plane_fmt[0].bytesperline,
        sizeimage: pix.plane_fmt[0].sizeimage,
        num_planes: pix.num_planes,
    })
}

fn request_mmap_buffers(fd: c_int, count: u32) -> io::Result<u32> {
    let mut req = V4l2RequestBuffersRaw {
        count,
        type_: V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        memory: V4L2_MEMORY_MMAP,
        capabilities: 0,
        flags: 0,
        reserved: [0; 3],
    };
    let ret = unsafe { ioctl(fd, VIDIOC_REQBUFS, &mut req as *mut V4l2RequestBuffersRaw) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(req.count)
}

fn release_mmap_buffers(fd: c_int) -> io::Result<()> {
    let mut req = V4l2RequestBuffersRaw {
        count: 0,
        type_: V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        memory: V4L2_MEMORY_MMAP,
        capabilities: 0,
        flags: 0,
        reserved: [0; 3],
    };
    let ret = unsafe { ioctl(fd, VIDIOC_REQBUFS, &mut req as *mut V4l2RequestBuffersRaw) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn query_and_mmap_buffer(fd: c_int, index: u32) -> io::Result<MappedBuffer> {
    let mut planes = [unsafe { mem::zeroed::<V4l2PlaneRaw>() }; 1];
    let mut buffer = make_buffer(index, &mut planes);
    let ret = unsafe { ioctl(fd, VIDIOC_QUERYBUF, &mut buffer as *mut V4l2BufferRaw) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }

    let length = planes[0].length as usize;
    if length == 0 {
        return Err(io::Error::other(format!(
            "capture buffer {index} has zero mmap length"
        )));
    }
    let offset = unsafe { planes[0].m.mem_offset } as c_long;
    let ptr = unsafe {
        mmap(
            std::ptr::null_mut(),
            length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            offset,
        )
    };
    if ptr == map_failed() {
        return Err(io::Error::last_os_error());
    }
    Ok(MappedBuffer { ptr, length })
}

fn enqueue_buffer(fd: c_int, index: u32) -> io::Result<()> {
    let mut planes = [unsafe { mem::zeroed::<V4l2PlaneRaw>() }; 1];
    let mut buffer = make_buffer(index, &mut planes);
    let ret = unsafe { ioctl(fd, VIDIOC_QBUF, &mut buffer as *mut V4l2BufferRaw) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn dequeue_buffer_with_timeout(
    fd: c_int,
    timeout: Duration,
) -> io::Result<(V4l2BufferRaw, V4l2PlaneRaw)> {
    let deadline = Instant::now() + timeout;
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "timed out waiting for capture frame",
            ));
        }
        wait_readable(fd, remaining)?;

        let mut planes = [unsafe { mem::zeroed::<V4l2PlaneRaw>() }; 1];
        let mut buffer = make_buffer(0, &mut planes);
        let ret = unsafe { ioctl(fd, VIDIOC_DQBUF, &mut buffer as *mut V4l2BufferRaw) };
        if ret >= 0 {
            return Ok((buffer, planes[0]));
        }
        let err = io::Error::last_os_error();
        if err.raw_os_error() == Some(EAGAIN) {
            continue;
        }
        return Err(err);
    }
}

fn wait_readable(fd: c_int, timeout: Duration) -> io::Result<()> {
    let mut pfd = PollFdRaw {
        fd,
        events: POLLIN,
        revents: 0,
    };
    let ret = unsafe { poll(&mut pfd as *mut PollFdRaw, 1, poll_timeout_ms(timeout)) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    if ret == 0 {
        return Err(io::Error::new(
            io::ErrorKind::TimedOut,
            "timed out waiting for capture fd",
        ));
    }
    Ok(())
}

fn streamon(fd: c_int) -> io::Result<()> {
    let mut type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE as c_int;
    let ret = unsafe { ioctl(fd, VIDIOC_STREAMON, &mut type_ as *mut c_int) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn streamoff(fd: c_int) -> io::Result<()> {
    let mut type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE as c_int;
    let ret = unsafe { ioctl(fd, VIDIOC_STREAMOFF, &mut type_ as *mut c_int) };
    if ret < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn make_buffer(index: u32, planes: &mut [V4l2PlaneRaw; 1]) -> V4l2BufferRaw {
    let mut buffer: V4l2BufferRaw = unsafe { mem::zeroed() };
    buffer.index = index;
    buffer.type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.m.planes = planes.as_mut_ptr();
    buffer.length = planes.len() as u32;
    buffer
}

fn poll_timeout_ms(timeout: Duration) -> c_int {
    let millis = timeout.as_millis().min(c_int::MAX as u128);
    millis as c_int
}

fn map_failed() -> *mut c_void {
    !0usize as *mut c_void
}

struct StreamGuard {
    fd: c_int,
    streaming: bool,
}

impl StreamGuard {
    fn new(fd: c_int) -> Self {
        Self {
            fd,
            streaming: false,
        }
    }

    fn start(&mut self) -> io::Result<()> {
        streamon(self.fd)?;
        self.streaming = true;
        Ok(())
    }

    fn stop(&mut self) -> io::Result<()> {
        if self.streaming {
            streamoff(self.fd)?;
            self.streaming = false;
        }
        Ok(())
    }
}

impl Drop for StreamGuard {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

struct BufferPoolGuard {
    fd: c_int,
    active: bool,
}

impl BufferPoolGuard {
    fn new(fd: c_int) -> Self {
        Self { fd, active: true }
    }

    fn release(&mut self) -> io::Result<()> {
        if self.active {
            release_mmap_buffers(self.fd)?;
            self.active = false;
        }
        Ok(())
    }
}

impl Drop for BufferPoolGuard {
    fn drop(&mut self) {
        let _ = self.release();
    }
}

struct MappedBuffer {
    ptr: *mut c_void,
    length: usize,
}

impl Drop for MappedBuffer {
    fn drop(&mut self) {
        if !self.ptr.is_null() && self.length > 0 {
            let _ = unsafe { munmap(self.ptr, self.length) };
        }
    }
}

fn c_array_to_string(bytes: &[u8]) -> String {
    let nul = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..nul]).trim().to_string()
}

fn invalid_input(message: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.into())
}

#[allow(dead_code)]
fn _assert_layouts() {
    let _ = mem::size_of::<PollFdRaw>();
    let _ = mem::size_of::<V4l2Capability>();
    let _ = mem::size_of::<V4l2Fmtdesc>();
    let _ = mem::size_of::<SubdevFormatRaw>();
    let _ = mem::size_of::<V4l2FormatRaw>();
    let _ = mem::size_of::<V4l2RequestBuffersRaw>();
    let _ = mem::size_of::<V4l2PlaneRaw>();
    let _ = mem::size_of::<V4l2BufferRaw>();
}
