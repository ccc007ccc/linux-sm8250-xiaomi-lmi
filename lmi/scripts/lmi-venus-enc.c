// SPDX-License-Identifier: GPL-2.0
//
// lmi-venus-enc.c -- bridge lmi software-ISP NV12 frames into Qualcomm Venus H.264.
//
// Input is tight NV12 from lmi-isp over a FIFO. Output is Annex-B H.264 access
// units wrapped in the small LMVC record format consumed by lmi-uvc-gadget.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#ifndef v4l2_fourcc
#define v4l2_fourcc(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#endif
#ifndef V4L2_PIX_FMT_NV12
#define V4L2_PIX_FMT_NV12 v4l2_fourcc('N', 'V', '1', '2')
#endif
#ifndef V4L2_PIX_FMT_H264
#define V4L2_PIX_FMT_H264 v4l2_fourcc('H', '2', '6', '4')
#endif
#ifndef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 9
#endif
#ifndef V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE 10
#endif
#ifndef VIDEO_MAX_PLANES
#define VIDEO_MAX_PLANES 8
#endif
#ifndef V4L2_CID_MPEG_VIDEO_BITRATE_MODE
#define V4L2_CID_MPEG_VIDEO_BITRATE_MODE (V4L2_CID_CODEC_BASE + 206)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_BITRATE
#define V4L2_CID_MPEG_VIDEO_BITRATE (V4L2_CID_CODEC_BASE + 207)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_BITRATE_PEAK
#define V4L2_CID_MPEG_VIDEO_BITRATE_PEAK (V4L2_CID_CODEC_BASE + 208)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_HEADER_MODE
#define V4L2_CID_MPEG_VIDEO_HEADER_MODE (V4L2_CID_CODEC_BASE + 216)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME
#define V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME (V4L2_CID_CODEC_BASE + 229)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_AU_DELIMITER
#define V4L2_CID_MPEG_VIDEO_AU_DELIMITER (V4L2_CID_CODEC_BASE + 231)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE
#define V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE (V4L2_CID_CODEC_BASE + 357)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_H264_LEVEL
#define V4L2_CID_MPEG_VIDEO_H264_LEVEL (V4L2_CID_CODEC_BASE + 359)
#endif
#ifndef V4L2_CID_MPEG_VIDEO_H264_PROFILE
#define V4L2_CID_MPEG_VIDEO_H264_PROFILE (V4L2_CID_CODEC_BASE + 363)
#endif
#ifndef V4L2_MPEG_VIDEO_BITRATE_MODE_VBR
#define V4L2_MPEG_VIDEO_BITRATE_MODE_VBR 0
#endif
#ifndef V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME
#define V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME 1
#endif
#ifndef V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC
#define V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC 1
#endif
#ifndef V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE
#define V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE 0
#endif
#ifndef V4L2_MPEG_VIDEO_H264_PROFILE_MAIN
#define V4L2_MPEG_VIDEO_H264_PROFILE_MAIN 2
#endif
#ifndef V4L2_MPEG_VIDEO_H264_PROFILE_HIGH
#define V4L2_MPEG_VIDEO_H264_PROFILE_HIGH 4
#endif
#ifndef V4L2_MPEG_VIDEO_H264_LEVEL_4_2
#define V4L2_MPEG_VIDEO_H264_LEVEL_4_2 13
#endif
#ifndef V4L2_MPEG_VIDEO_H264_LEVEL_5_0
#define V4L2_MPEG_VIDEO_H264_LEVEL_5_0 14
#endif
#ifndef V4L2_MPEG_VIDEO_H264_LEVEL_5_1
#define V4L2_MPEG_VIDEO_H264_LEVEL_5_1 15
#endif
#ifndef V4L2_MPEG_VIDEO_H264_LEVEL_5_2
#define V4L2_MPEG_VIDEO_H264_LEVEL_5_2 16
#endif

#define MAX_BUFS 4
#define LMI_UVC_RECORD_MAGIC 0x43564d4cU /* "LMVC" little-endian */
#define LMI_UVC_RECORD_VERSION 2
#define LMI_UVC_RECORD_FLAG_KEYFRAME 0x00000001U
#define LMI_UVC_RECORD_FLAG_PFRAME   0x00000002U

struct lmi_uvc_record_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t payload_size;
	uint32_t sequence;
	uint32_t flags;
};

struct mmap_buffer {
	void *start;
	size_t length;
	int queued;
};

static const char *g_device = "/dev/video15";
static const char *g_input_fifo = "/run/lmi-camera/lmi-isp-nv12.fifo";
static const char *g_output_fifo = "/run/lmi-camera/lmi-uvc.fifo";
static unsigned int g_width;
static unsigned int g_height;
static unsigned int g_fps = 30;
static unsigned int g_bitrate = 80000000;
static unsigned int g_peak_bitrate = 120000000;
static unsigned int g_gop = 30;
static unsigned int g_profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
static unsigned int g_level = V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
static unsigned int g_max_record = 4 * 1024 * 1024;
static unsigned int g_out_count = 3;
static unsigned int g_cap_count = 4;
static unsigned int g_fifo_write_timeout_ms = 2000;
static int g_verbose;

static int g_vfd = -1;
static int g_infd = -1;
static int g_outfd = -1;
static int g_streaming_out;
static int g_streaming_cap;
static volatile sig_atomic_t g_run = 1;

static unsigned int g_coded_w;
static unsigned int g_coded_h;
static unsigned int g_raw_stride;
static unsigned int g_raw_height;
static size_t g_raw_sizeimage;
static size_t g_uv_offset;
static size_t g_tight_size;
static unsigned char *g_tight;
static size_t g_tight_off;
static uint32_t g_sequence;
static unsigned int g_queued_input;
static unsigned int g_encoded_output;

static struct mmap_buffer g_out_bufs[MAX_BUFS];
static struct mmap_buffer g_cap_bufs[MAX_BUFS];

static void ilog(const char *fmt, ...)
{
	va_list ap;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[venus-enc %ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	fflush(stderr);
}

static void on_sig(int sig)
{
	(void)sig;
	g_run = 0;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;
	do {
		ret = ioctl(fd, req, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static unsigned int align_up(unsigned int value, unsigned int align)
{
	return (value + align - 1) & ~(align - 1);
}

static const char *fourcc(uint32_t f)
{
	static char s[5];
	s[0] = (char)(f & 0xff);
	s[1] = (char)((f >> 8) & 0xff);
	s[2] = (char)((f >> 16) & 0xff);
	s[3] = (char)((f >> 24) & 0xff);
	s[4] = '\0';
	return s;
}

static int parse_uint(const char *s, unsigned int *out)
{
	char *end;
	unsigned long v;
	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || end == s || *end || v > 0xffffffffUL)
		return -1;
	*out = (unsigned int)v;
	return 0;
}

static int parse_profile(const char *s, unsigned int *out)
{
	if (!strcmp(s, "baseline"))
		*out = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
	else if (!strcmp(s, "main"))
		*out = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
	else if (!strcmp(s, "high"))
		*out = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
	else
		return parse_uint(s, out);
	return 0;
}

static int parse_level(const char *s, unsigned int *out)
{
	if (!strcmp(s, "4.2"))
		*out = V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
	else if (!strcmp(s, "5.0") || !strcmp(s, "5"))
		*out = V4L2_MPEG_VIDEO_H264_LEVEL_5_0;
	else if (!strcmp(s, "5.1"))
		*out = V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
	else if (!strcmp(s, "5.2"))
		*out = V4L2_MPEG_VIDEO_H264_LEVEL_5_2;
	else
		return parse_uint(s, out);
	return 0;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s --device /dev/video15 --input-fifo PATH --output-fifo PATH\n"
		"          --width W --height H --fps N --bitrate B --peak-bitrate B\n"
		"          [--gop N] [--profile high|main|baseline] [--level 5.1]\n"
		"          [--max-record BYTES] [--fifo-write-timeout-ms MS] [-v]\n",
		p);
}

static void set_ctrl_warn(unsigned int id, int value, const char *name)
{
	struct v4l2_control ctrl;
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.id = id;
	ctrl.value = value;
	if (xioctl(g_vfd, VIDIOC_S_CTRL, &ctrl) < 0) {
		if (g_verbose || errno != EINVAL)
			ilog("control %s=0x%x/%d failed: %s", name, id, value, strerror(errno));
	} else if (g_verbose) {
		ilog("control %s=%d", name, value);
	}
}

static int configure_formats(void)
{
	struct v4l2_format fmt;
	struct v4l2_selection sel;
	struct v4l2_streamparm parm;

	g_coded_w = align_up(g_width, 128);
	g_coded_h = align_up(g_height, 32);

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.width = g_coded_w;
	fmt.fmt.pix_mp.height = g_coded_h;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	fmt.fmt.pix_mp.plane_fmt[0].bytesperline = g_coded_w;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = g_coded_w * g_coded_h * 3 / 2;
	if (xioctl(g_vfd, VIDIOC_S_FMT, &fmt) < 0) {
		ilog("S_FMT OUTPUT_MPLANE NV12 %ux%u failed: %s", g_coded_w, g_coded_h, strerror(errno));
		return -1;
	}
	if (fmt.fmt.pix_mp.num_planes < 1 || fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12) {
		ilog("unexpected OUTPUT format: planes=%u fourcc=%s", fmt.fmt.pix_mp.num_planes,
		     fourcc(fmt.fmt.pix_mp.pixelformat));
		return -1;
	}
	g_raw_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	g_raw_height = fmt.fmt.pix_mp.height;
	g_raw_sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	if (!g_raw_stride)
		g_raw_stride = align_up(fmt.fmt.pix_mp.width, 128);
	if (g_raw_stride < g_width || g_raw_height < g_height) {
		ilog("OUTPUT format too small: stride=%u height=%u visible=%ux%u",
		     g_raw_stride, g_raw_height, g_width, g_height);
		return -1;
	}
	g_uv_offset = (size_t)g_raw_stride * g_raw_height;
	if (g_raw_sizeimage < g_uv_offset + (size_t)g_raw_stride * ((g_raw_height + 1) / 2)) {
		ilog("OUTPUT sizeimage too small: %zu stride=%u height=%u", g_raw_sizeimage,
		     g_raw_stride, g_raw_height);
		return -1;
	}
	g_coded_w = fmt.fmt.pix_mp.width;
	g_coded_h = fmt.fmt.pix_mp.height;
	g_uv_offset = (size_t)g_raw_stride * g_raw_height;
	ilog("OUTPUT NV12 coded=%ux%u visible=%ux%u stride=%u sizeimage=%zu",
	     g_coded_w, g_coded_h, g_width, g_height, g_raw_stride, g_raw_sizeimage);

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = g_coded_w;
	fmt.fmt.pix_mp.height = g_coded_h;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = g_max_record;
	if (xioctl(g_vfd, VIDIOC_S_FMT, &fmt) < 0) {
		ilog("S_FMT CAPTURE_MPLANE H264 failed: %s", strerror(errno));
		return -1;
	}
	if (fmt.fmt.pix_mp.num_planes < 1 || fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_H264) {
		ilog("unexpected CAPTURE format: planes=%u fourcc=%s", fmt.fmt.pix_mp.num_planes,
		     fourcc(fmt.fmt.pix_mp.pixelformat));
		return -1;
	}
	ilog("CAPTURE H264 coded=%ux%u sizeimage=%u max-record=%u",
	     fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	     fmt.fmt.pix_mp.plane_fmt[0].sizeimage, g_max_record);

	if (g_coded_w != g_width || g_coded_h != g_height) {
		memset(&sel, 0, sizeof(sel));
		sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		sel.target = V4L2_SEL_TGT_CROP;
		sel.r.left = 0;
		sel.r.top = 0;
		sel.r.width = g_width;
		sel.r.height = g_height;
		if (xioctl(g_vfd, VIDIOC_S_SELECTION, &sel) < 0)
			ilog("S_SELECTION OUTPUT_MPLANE crop %ux%u failed: %s", g_width, g_height, strerror(errno));
		else
			ilog("OUTPUT_MPLANE crop visible=%ux%u after capture format", sel.r.width, sel.r.height);
	}

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	parm.parm.output.timeperframe.numerator = 1;
	parm.parm.output.timeperframe.denominator = g_fps ? g_fps : 30;
	if (xioctl(g_vfd, VIDIOC_S_PARM, &parm) < 0)
		ilog("S_PARM OUTPUT %ufps failed: %s", g_fps, strerror(errno));

	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, "bitrate_mode");
	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_BITRATE, (int)g_bitrate, "bitrate");
	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, (int)g_peak_bitrate, "peak_bitrate");
	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_GOP_SIZE, (int)g_gop, "gop");
	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "b_frames");
	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_HEADER_MODE,
		      V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, "header_mode");
	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_H264_PROFILE, (int)g_profile, "h264_profile");
	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_H264_LEVEL, (int)g_level, "h264_level");
	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
		      V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC, "h264_entropy");
	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_AU_DELIMITER, 1, "au_delimiter");
	return 0;
}

static int reqbufs_mmap(enum v4l2_buf_type type, struct mmap_buffer *bufs, unsigned int *count)
{
	struct v4l2_requestbuffers rb;
	unsigned int i;

	memset(&rb, 0, sizeof(rb));
	rb.count = *count;
	rb.type = type;
	rb.memory = V4L2_MEMORY_MMAP;
	if (xioctl(g_vfd, VIDIOC_REQBUFS, &rb) < 0) {
		ilog("REQBUFS type=%u count=%u failed: %s", type, *count, strerror(errno));
		return -1;
	}
	if (rb.count == 0 || rb.count > MAX_BUFS) {
		ilog("REQBUFS type=%u returned invalid count=%u", type, rb.count);
		return -1;
	}
	*count = rb.count;
	for (i = 0; i < rb.count; i++) {
		struct v4l2_buffer b;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof(b));
		memset(planes, 0, sizeof(planes));
		b.type = type;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.length = VIDEO_MAX_PLANES;
		b.m.planes = planes;
		if (xioctl(g_vfd, VIDIOC_QUERYBUF, &b) < 0) {
			ilog("QUERYBUF type=%u index=%u failed: %s", type, i, strerror(errno));
			return -1;
		}
		if (planes[0].length == 0) {
			ilog("QUERYBUF type=%u index=%u returned zero plane", type, i);
			return -1;
		}
		bufs[i].length = planes[0].length;
		bufs[i].start = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
					     MAP_SHARED, g_vfd, planes[0].m.mem_offset);
		if (bufs[i].start == MAP_FAILED) {
			ilog("mmap type=%u index=%u failed: %s", type, i, strerror(errno));
			return -1;
		}
		bufs[i].queued = 0;
	}
	return 0;
}

static void release_bufs(enum v4l2_buf_type type, struct mmap_buffer *bufs, unsigned int count)
{
	struct v4l2_requestbuffers rb;
	unsigned int i;

	for (i = 0; i < count && i < MAX_BUFS; i++) {
		if (bufs[i].start && bufs[i].start != MAP_FAILED)
			munmap(bufs[i].start, bufs[i].length);
		bufs[i].start = NULL;
		bufs[i].length = 0;
		bufs[i].queued = 0;
	}
	memset(&rb, 0, sizeof(rb));
	rb.type = type;
	rb.memory = V4L2_MEMORY_MMAP;
	if (g_vfd >= 0)
		xioctl(g_vfd, VIDIOC_REQBUFS, &rb);
}

static int queue_capture(unsigned int index)
{
	struct v4l2_buffer b;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	memset(&b, 0, sizeof(b));
	memset(planes, 0, sizeof(planes));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = index;
	b.length = 1;
	b.m.planes = planes;
	if (xioctl(g_vfd, VIDIOC_QBUF, &b) < 0) {
		ilog("QBUF CAPTURE %u failed: %s", index, strerror(errno));
		return -1;
	}
	g_cap_bufs[index].queued = 1;
	return 0;
}

static void fill_nv12_buffer(void *dst, size_t dst_len, const unsigned char *src)
{
	unsigned int y;
	unsigned char *d = dst;
	const unsigned char *src_uv = src + (size_t)g_width * g_height;

	memset(d, 0x80, dst_len);
	for (y = 0; y < g_raw_height; y++)
		memset(d + (size_t)y * g_raw_stride, 16, g_raw_stride);
	for (y = 0; y < g_height; y++)
		memcpy(d + (size_t)y * g_raw_stride, src + (size_t)y * g_width, g_width);
	for (y = 0; y < g_height / 2; y++)
		memcpy(d + g_uv_offset + (size_t)y * g_raw_stride,
		       src_uv + (size_t)y * g_width, g_width);
}

static int queue_output(unsigned int index)
{
	struct v4l2_buffer b;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];

	if (g_out_bufs[index].length < g_raw_sizeimage) {
		ilog("OUTPUT buffer %u too small: %zu < %zu", index, g_out_bufs[index].length, g_raw_sizeimage);
		return -1;
	}
	fill_nv12_buffer(g_out_bufs[index].start, g_out_bufs[index].length, g_tight);
	memset(&b, 0, sizeof(b));
	memset(planes, 0, sizeof(planes));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = index;
	b.field = V4L2_FIELD_NONE;
	b.length = 1;
	b.m.planes = planes;
	planes[0].bytesused = (unsigned int)g_raw_sizeimage;
	if (xioctl(g_vfd, VIDIOC_QBUF, &b) < 0) {
		ilog("QBUF OUTPUT %u failed: %s", index, strerror(errno));
		return -1;
	}
	g_out_bufs[index].queued = 1;
	g_queued_input++;
	g_tight_off = 0;
	return 0;
}

static int output_free_index(void)
{
	unsigned int i;
	for (i = 0; i < g_out_count; i++) {
		if (!g_out_bufs[i].queued)
			return (int)i;
	}
	return -1;
}

static int write_all(int fd, const void *data, size_t len)
{
	const unsigned char *p = data;
	unsigned int blocked_ms = 0;

	while (len && g_run) {
		ssize_t n = write(fd, p, len);
		if (n > 0) {
			p += n;
			len -= (size_t)n;
			blocked_ms = 0;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd pfd = { fd, POLLOUT | POLLERR | POLLHUP, 0 };
			int r = poll(&pfd, 1, 100);
			if (r < 0 && errno == EINTR)
				continue;
			if (r < 0)
				return -1;
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				errno = EPIPE;
				return -1;
			}
			blocked_ms += 100;
			if (g_fifo_write_timeout_ms && blocked_ms >= g_fifo_write_timeout_ms) {
				errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		return -1;
	}

	if (len) {
		errno = EINTR;
		return -1;
	}
	return 0;
}

static int write_record(const unsigned char *payload, unsigned int size, uint32_t flags)
{
	struct lmi_uvc_record_header hdr;

	if (size == 0)
		return 0;
	if (size > g_max_record) {
		ilog("encoded access unit %u exceeds max-record %u; dropping", size, g_max_record);
		return 0;
	}
	hdr.magic = LMI_UVC_RECORD_MAGIC;
	hdr.version = LMI_UVC_RECORD_VERSION;
	hdr.header_size = sizeof(hdr);
	hdr.payload_size = size;
	hdr.sequence = ++g_sequence;
	hdr.flags = flags;
	if (write_all(g_outfd, &hdr, sizeof(hdr)) < 0 || write_all(g_outfd, payload, size) < 0) {
		ilog("write output fifo failed: %s", strerror(errno));
		return -1;
	}
	g_encoded_output++;
	return 0;
}

static int drain_capture(void)
{
	int did = 0;
	for (;;) {
		struct v4l2_buffer b;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		unsigned int bytes, off;
		memset(&b, 0, sizeof(b));
		memset(planes, 0, sizeof(planes));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.length = VIDEO_MAX_PLANES;
		b.m.planes = planes;
		if (xioctl(g_vfd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN)
				return did;
			ilog("DQBUF CAPTURE failed: %s", strerror(errno));
			return -1;
		}
		did = 1;
		if (b.index >= g_cap_count) {
			ilog("DQBUF CAPTURE invalid index=%u", b.index);
			return -1;
		}
		g_cap_bufs[b.index].queued = 0;
		bytes = planes[0].bytesused;
		off = planes[0].data_offset;
		if (!(b.flags & V4L2_BUF_FLAG_ERROR) && bytes > off && bytes <= g_cap_bufs[b.index].length) {
			uint32_t flags = 0;

			if (b.flags & V4L2_BUF_FLAG_KEYFRAME)
				flags |= LMI_UVC_RECORD_FLAG_KEYFRAME;
			else if (b.flags & V4L2_BUF_FLAG_PFRAME)
				flags |= LMI_UVC_RECORD_FLAG_PFRAME;
			if (write_record((unsigned char *)g_cap_bufs[b.index].start + off, bytes - off, flags) < 0) {
				if (errno == EPIPE || errno == ETIMEDOUT)
					g_run = 0;
				return -1;
			}
		} else if (b.flags & V4L2_BUF_FLAG_ERROR) {
			ilog("CAPTURE buffer %u flagged error", b.index);
		}
		if (queue_capture(b.index) < 0)
			return -1;
	}
}

static int drain_output(void)
{
	int did = 0;
	for (;;) {
		struct v4l2_buffer b;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		memset(&b, 0, sizeof(b));
		memset(planes, 0, sizeof(planes));
		b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.length = VIDEO_MAX_PLANES;
		b.m.planes = planes;
		if (xioctl(g_vfd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN)
				return did;
			ilog("DQBUF OUTPUT failed: %s", strerror(errno));
			return -1;
		}
		did = 1;
		if (b.index >= g_out_count) {
			ilog("DQBUF OUTPUT invalid index=%u", b.index);
			return -1;
		}
		g_out_bufs[b.index].queued = 0;
	}
}

static int read_input_some(void)
{
	while (g_tight_off < g_tight_size) {
		ssize_t n = read(g_infd, g_tight + g_tight_off, g_tight_size - g_tight_off);
		if (n > 0) {
			g_tight_off += (size_t)n;
			continue;
		}
		if (n == 0)
			return 0;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		ilog("read input fifo failed: %s", strerror(errno));
		return -1;
	}
	return 1;
}

static int wait_first_frame(void)
{
	int last_log = 0;
	while (g_run) {
		int r = read_input_some();
		if (r < 0)
			return -1;
		if (g_tight_off >= g_tight_size)
			return 0;
		{
			struct timespec ts;
			struct pollfd pfd = { g_infd, POLLIN | POLLHUP, 0 };
			clock_gettime(CLOCK_MONOTONIC, &ts);
			if ((int)ts.tv_sec - last_log >= 2) {
				last_log = (int)ts.tv_sec;
				ilog("waiting for first NV12 frame: %zu/%zu", g_tight_off, g_tight_size);
			}
			poll(&pfd, 1, 250);
		}
	}
	return -1;
}

static int streamon(enum v4l2_buf_type type, int *flag, const char *name)
{
	if (xioctl(g_vfd, VIDIOC_STREAMON, &type) < 0) {
		ilog("STREAMON %s failed: %s", name, strerror(errno));
		return -1;
	}
	*flag = 1;
	return 0;
}

static void streamoff(enum v4l2_buf_type type, int *flag)
{
	if (*flag && g_vfd >= 0)
		xioctl(g_vfd, VIDIOC_STREAMOFF, &type);
	*flag = 0;
}

static int open_paths(void)
{
	struct v4l2_capability cap;

	g_vfd = open(g_device, O_RDWR | O_NONBLOCK);
	if (g_vfd < 0) {
		ilog("open %s failed: %s", g_device, strerror(errno));
		return -1;
	}
	memset(&cap, 0, sizeof(cap));
	if (xioctl(g_vfd, VIDIOC_QUERYCAP, &cap) == 0)
		ilog("device %s driver=%s card=%s", g_device, cap.driver, cap.card);

	g_infd = open(g_input_fifo, O_RDONLY | O_NONBLOCK);
	if (g_infd < 0) {
		ilog("open input fifo %s failed: %s", g_input_fifo, strerror(errno));
		return -1;
	}
	g_outfd = open(g_output_fifo, O_WRONLY | O_NONBLOCK);
	if (g_outfd < 0) {
		ilog("open output fifo %s failed: %s", g_output_fifo, strerror(errno));
		return -1;
	}
	return 0;
}

static void cleanup(void)
{
	enum v4l2_buf_type out = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	enum v4l2_buf_type cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	streamoff(out, &g_streaming_out);
	streamoff(cap, &g_streaming_cap);
	release_bufs(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, g_out_bufs, g_out_count);
	release_bufs(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, g_cap_bufs, g_cap_count);
	if (g_infd >= 0) close(g_infd);
	if (g_outfd >= 0) close(g_outfd);
	if (g_vfd >= 0) close(g_vfd);
	free(g_tight);
}

int main(int argc, char **argv)
{
	unsigned int i;
	int ret = 1;

	for (i = 1; i < (unsigned int)argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < (unsigned int)argc) g_device = argv[++i];
		else if (!strcmp(argv[i], "--input-fifo") && i + 1 < (unsigned int)argc) g_input_fifo = argv[++i];
		else if (!strcmp(argv[i], "--output-fifo") && i + 1 < (unsigned int)argc) g_output_fifo = argv[++i];
		else if (!strcmp(argv[i], "--width") && i + 1 < (unsigned int)argc && parse_uint(argv[++i], &g_width) == 0) {}
		else if (!strcmp(argv[i], "--height") && i + 1 < (unsigned int)argc && parse_uint(argv[++i], &g_height) == 0) {}
		else if (!strcmp(argv[i], "--fps") && i + 1 < (unsigned int)argc && parse_uint(argv[++i], &g_fps) == 0) {}
		else if (!strcmp(argv[i], "--bitrate") && i + 1 < (unsigned int)argc && parse_uint(argv[++i], &g_bitrate) == 0) {}
		else if (!strcmp(argv[i], "--peak-bitrate") && i + 1 < (unsigned int)argc && parse_uint(argv[++i], &g_peak_bitrate) == 0) {}
		else if (!strcmp(argv[i], "--gop") && i + 1 < (unsigned int)argc && parse_uint(argv[++i], &g_gop) == 0) {}
		else if (!strcmp(argv[i], "--profile") && i + 1 < (unsigned int)argc && parse_profile(argv[++i], &g_profile) == 0) {}
		else if (!strcmp(argv[i], "--level") && i + 1 < (unsigned int)argc && parse_level(argv[++i], &g_level) == 0) {}
		else if (!strcmp(argv[i], "--max-record") && i + 1 < (unsigned int)argc && parse_uint(argv[++i], &g_max_record) == 0) {}
		else if (!strcmp(argv[i], "--fifo-write-timeout-ms") && i + 1 < (unsigned int)argc && parse_uint(argv[++i], &g_fifo_write_timeout_ms) == 0) {}
		else if (!strcmp(argv[i], "-v")) g_verbose = 1;
		else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
		else { usage(argv[0]); return 2; }
	}

	if (g_width == 0 || g_height == 0 || (g_width & 1) || (g_height & 1) || g_fps == 0) {
		usage(argv[0]);
		return 2;
	}
	if (g_out_count > MAX_BUFS) g_out_count = MAX_BUFS;
	if (g_cap_count > MAX_BUFS) g_cap_count = MAX_BUFS;
	g_tight_size = (size_t)g_width * g_height * 3 / 2;
	g_tight = malloc(g_tight_size);
	if (!g_tight) {
		ilog("alloc tight NV12 frame failed: %zu bytes", g_tight_size);
		return 1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	if (open_paths() < 0)
		goto out;
	if (configure_formats() < 0)
		goto out;
	if (reqbufs_mmap(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, g_out_bufs, &g_out_count) < 0)
		goto out;
	if (reqbufs_mmap(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, g_cap_bufs, &g_cap_count) < 0)
		goto out;
	for (i = 0; i < g_cap_count; i++) {
		if (queue_capture(i) < 0)
			goto out;
	}
	if (wait_first_frame() < 0)
		goto out;
	if (queue_output(0) < 0)
		goto out;
	if (streamon(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &g_streaming_cap, "CAPTURE") < 0)
		goto out;
	if (streamon(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &g_streaming_out, "OUTPUT") < 0)
		goto out;
	set_ctrl_warn(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, "force_key_frame");
	ilog("streaming H264: visible=%ux%u fps=%u bitrate=%u peak=%u gop=%u max-record=%u",
	     g_width, g_height, g_fps, g_bitrate, g_peak_bitrate, g_gop, g_max_record);

	while (g_run) {
		struct pollfd pfds[2];
		int free_idx;
		if (drain_capture() < 0 || drain_output() < 0)
			goto out;
		free_idx = output_free_index();
		if (free_idx >= 0 && g_tight_off >= g_tight_size) {
			if (queue_output((unsigned int)free_idx) < 0)
				goto out;
		}
		if (g_tight_off < g_tight_size) {
			if (read_input_some() < 0)
				goto out;
			free_idx = output_free_index();
			if (free_idx >= 0 && g_tight_off >= g_tight_size) {
				if (queue_output((unsigned int)free_idx) < 0)
					goto out;
			}
		}
		if (g_verbose && g_encoded_output && !(g_encoded_output % 120))
			ilog("stats queued=%u encoded=%u", g_queued_input, g_encoded_output);
		pfds[0].fd = g_vfd;
		pfds[0].events = POLLIN | POLLOUT | POLLERR;
		pfds[0].revents = 0;
		pfds[1].fd = g_infd;
		pfds[1].events = POLLIN | POLLHUP | POLLERR;
		pfds[1].revents = 0;
		poll(pfds, 2, 250);
		if (pfds[0].revents & (POLLERR | POLLNVAL))
			ilog("device poll revents=0x%x", pfds[0].revents);
	}
	ret = 0;

out:
	cleanup();
	return ret;
}
