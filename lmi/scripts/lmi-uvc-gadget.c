// SPDX-License-Identifier: GPL-2.0
//
// lmi-uvc-gadget.c -- minimal UVC gadget feeder for the lmi software-ISP camera.
//
// Exposes a single YUYV, MJPEG, or H.264 frame size over the f_uvc gadget
// V4L2 OUTPUT device, taking each complete frame from a FIFO that
// lmi-camera-web-preview.py (--isp-sink uvc) writes. Handles the UVC
// PROBE/COMMIT streaming-control handshake and the OUTPUT buffer queue.
//
// The PROBE/COMMIT logic follows the canonical Linux uvc-gadget reference
// (Laurent Pinchart); only the frame source (a FIFO) and the fixed
// single-format negotiation are lmi-specific.
//
// Cross-compiled static with the Android NDK clang and run from /tmp on the
// device; nothing is installed into the rootfs.

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

#include <linux/usb/ch9.h>
#include <linux/usb/g_uvc.h>
#include <linux/usb/video.h>
#include <linux/videodev2.h>

#ifndef V4L2_BUF_TYPE_VIDEO_OUTPUT
#define V4L2_BUF_TYPE_VIDEO_OUTPUT 2
#endif
#ifndef V4L2_PIX_FMT_H264
#define V4L2_PIX_FMT_H264 v4l2_fourcc('H', '2', '6', '4')
#endif

#define MAX_NBUF 4
#define LMI_UVC_RECORD_MAGIC 0x43564d4cU /* "LMVC" little-endian */
#define LMI_UVC_RECORD_VERSION 1

static const char *g_dev = "/dev/video0";
static const char *g_fifo = "/tmp/lmi-uvc.fifo";
static const char *g_ready_file = "";
static const char *g_event_fifo = "";
static unsigned int g_width = 640;
static unsigned int g_height = 480;
static unsigned int g_fps = 15;
static unsigned int g_maxpkt = 1024;
static unsigned int g_mult = 0;
static unsigned int g_burst = 0;
static int g_bulk = 0;
static unsigned int g_streaming_intf = 1; /* VS interface of a standalone UVC gadget */
static unsigned int g_reqbufs = 2;
static int g_verbose = 0;
static int g_exit_on_disconnect;
static int g_connected;

enum lmi_uvc_format { LMI_UVC_YUYV, LMI_UVC_MJPEG, LMI_UVC_H264 };
static enum lmi_uvc_format g_format = LMI_UVC_YUYV;
static unsigned int g_framesize; /* YUYV bytes or compressed max frame bytes */
static unsigned int g_interval;  /* 100ns units */

static int vfd = -1;
static int fifofd = -1;
static int eventfd = -1;
static int streaming = 0;
static int g_stream_requested;
static int control_sel = 0; /* PROBE or COMMIT, for the SET_CUR DATA that follows */
static unsigned int g_dequeued_frames;
static unsigned int g_requeued_frames;
static unsigned int g_received_frames;
static int g_last_stream_log;
static int g_last_record_log;
static volatile sig_atomic_t g_run = 1;

static struct uvc_streaming_control g_probe;
static struct uvc_streaming_control g_commit;

struct buffer {
	void *start;
	size_t length;
	int queued;
};
static struct buffer g_buffers[MAX_NBUF];
static unsigned int g_nbuffers;

/* FIFO frame accumulation: acc fills up, latest holds the most recent complete frame. */
struct lmi_uvc_record_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t payload_size;
	uint32_t sequence;
};

static unsigned char *g_acc;
static unsigned int g_acc_off;
static unsigned char *g_latest;
static unsigned int g_latest_size;
static int g_have_frame;
static unsigned char g_hdr_buf[sizeof(struct lmi_uvc_record_header)];
static unsigned int g_hdr_off;
static struct lmi_uvc_record_header g_cur_hdr;

static void ilog(const char *fmt, ...)
{
	va_list ap;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[uvc-gadget %ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	fflush(stderr);
}

static const char *format_name(void)
{
	switch (g_format) {
	case LMI_UVC_MJPEG:
		return "MJPEG";
	case LMI_UVC_H264:
		return "H264";
	case LMI_UVC_YUYV:
	default:
		return "YUYV";
	}
}

static unsigned int format_fourcc(void)
{
	switch (g_format) {
	case LMI_UVC_MJPEG:
		return V4L2_PIX_FMT_MJPEG;
	case LMI_UVC_H264:
		return V4L2_PIX_FMT_H264;
	case LMI_UVC_YUYV:
	default:
		return V4L2_PIX_FMT_YUYV;
	}
}

static int compressed_format(void)
{
	return g_format != LMI_UVC_YUYV;
}

static int valid_jpeg_frame(const unsigned char *p, unsigned int len)
{
	return len >= 4 && p[0] == 0xff && p[1] == 0xd8 &&
		p[len - 2] == 0xff && p[len - 1] == 0xd9;
}

static int compressed_frame_ready(void)
{
	if (!compressed_format())
		return 1;
	if (!g_have_frame || g_latest_size == 0)
		return 0;
	if (g_format == LMI_UVC_MJPEG)
		return valid_jpeg_frame(g_latest, g_latest_size);
	return 1;
}

static void on_sig(int sig)
{
	(void)sig;
	g_run = 0;
}

static int fatal_device_errno(int err)
{
	return err == ENODEV || err == ENXIO || err == EIO || err == EPIPE ||
		err == ESHUTDOWN;
}

static int fatal_poll_revents(short revents)
{
	return revents & (POLLHUP | POLLNVAL);
}

static void emit_event(const char *name)
{
	char line[64];
	int len;
	ssize_t n;

	if (eventfd < 0)
		return;
	len = snprintf(line, sizeof(line), "%s\n", name);
	if (len <= 0 || len >= (int)sizeof(line))
		return;
	n = write(eventfd, line, (size_t)len);
	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EPIPE)
		ilog("event-fifo write %s failed: %s", name, strerror(errno));
}

static void fill_streaming_control(struct uvc_streaming_control *ctrl)
{
	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->bmHint = 1;
	ctrl->bFormatIndex = 1;
	ctrl->bFrameIndex = 1;
	ctrl->dwFrameInterval = g_interval;
	ctrl->dwMaxVideoFrameSize = g_framesize;
	if (g_bulk)
		ctrl->dwMaxPayloadTransferSize = g_framesize;
	else
		ctrl->dwMaxPayloadTransferSize = g_maxpkt * (g_mult + 1) * (g_burst + 1);
	ctrl->bmFramingInfo = 3;
	ctrl->bPreferedVersion = 1;
	ctrl->bMinVersion = 1;
	ctrl->bMaxVersion = 1;
}

/* ---- UVC PROBE/COMMIT streaming-control request handling ---- */

static void process_streaming(uint8_t req, uint8_t cs, struct uvc_request_data *resp)
{
	struct uvc_streaming_control *ctrl;

	if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL)
		return;

	ctrl = (struct uvc_streaming_control *)&resp->data;
	resp->length = sizeof(*ctrl);

	switch (req) {
	case UVC_SET_CUR:
		control_sel = cs;        /* the next UVC_EVENT_DATA carries the payload */
		resp->length = 34;
		break;
	case UVC_GET_CUR:
		if (cs == UVC_VS_PROBE_CONTROL)
			memcpy(ctrl, &g_probe, sizeof(*ctrl));
		else
			memcpy(ctrl, &g_commit, sizeof(*ctrl));
		break;
	case UVC_GET_MIN:
	case UVC_GET_MAX:
	case UVC_GET_DEF:
		fill_streaming_control(ctrl);
		break;
	case UVC_GET_RES:
		memset(ctrl, 0, sizeof(*ctrl));
		break;
	case UVC_GET_LEN:
		resp->data[0] = 0x00;
		resp->data[1] = 34;
		resp->length = 2;
		break;
	case UVC_GET_INFO:
		resp->data[0] = 0x03; /* GET and SET supported */
		resp->length = 1;
		break;
	default:
		resp->length = -EL2HLT;
		break;
	}
}

static void process_setup(struct usb_ctrlrequest *ctrl, struct uvc_request_data *resp)
{
	uint8_t cs;
	uint8_t intf;

	control_sel = 0;
	resp->length = -EL2HLT; /* stall optional/unhandled controls */

	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS)
		return;

	cs = ctrl->wValue >> 8;
	intf = ctrl->wIndex & 0xff;

	/* Only the streaming (VS) interface PROBE/COMMIT controls are handled;
	 * optional VC unit/terminal controls are stalled so the host falls back
	 * to defaults, which is sufficient for a basic webcam. */
	if (intf == g_streaming_intf &&
	    (cs == UVC_VS_PROBE_CONTROL || cs == UVC_VS_COMMIT_CONTROL))
		process_streaming(ctrl->bRequest, cs, resp);

	if (g_verbose)
		ilog("setup type=0x%02x req=0x%02x cs=0x%02x intf=%u len=%u -> resp.len=%d",
		     ctrl->bRequestType, ctrl->bRequest, cs, intf, ctrl->wLength, resp->length);
}

static int start_streaming(void);
static void stop_streaming(void);

static void resync_record_header(void)
{
	memmove(g_hdr_buf, g_hdr_buf + 1, sizeof(g_hdr_buf) - 1);
	g_hdr_off = sizeof(g_hdr_buf) - 1;
	g_acc_off = 0;
	memset(&g_cur_hdr, 0, sizeof(g_cur_hdr));
}

static void release_buffers(void)
{
	struct v4l2_requestbuffers rb;
	unsigned int i;

	for (i = 0; i < g_nbuffers; i++) {
		if (g_buffers[i].start && g_buffers[i].start != MAP_FAILED)
			munmap(g_buffers[i].start, g_buffers[i].length);
		g_buffers[i].start = NULL;
		g_buffers[i].length = 0;
		g_buffers[i].queued = 0;
	}

	memset(&rb, 0, sizeof(rb));
	rb.count = 0;
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;
	if (vfd >= 0)
		ioctl(vfd, VIDIOC_REQBUFS, &rb);
	g_nbuffers = 0;
}

static void process_data(struct uvc_request_data *data)
{
	struct uvc_streaming_control *target;

	if (control_sel == 0 || data->length <= 0)
		return;

	target = (control_sel == UVC_VS_COMMIT_CONTROL) ? &g_commit : &g_probe;
	{
		unsigned int len = (unsigned int)data->length;
		if (len > sizeof(*target))
			len = sizeof(*target);
		memcpy(target, data->data, len);
	}

	if (control_sel == UVC_VS_COMMIT_CONTROL && g_verbose)
		ilog("commit: fmt=%u frame=%u interval=%u maxframe=%u",
		     g_commit.bFormatIndex, g_commit.bFrameIndex,
		     g_commit.dwFrameInterval, g_commit.dwMaxVideoFrameSize);

	control_sel = 0;
}

/* ---- OUTPUT buffer queue ---- */

static void log_stream_progress(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	if (!g_last_stream_log)
		g_last_stream_log = (int)ts.tv_sec;
	if ((int)ts.tv_sec - g_last_stream_log >= 2) {
		ilog("streaming stats: fmt=%s dq=%u q=%u rx=%u have=%d latest=%u",
		     format_name(), g_dequeued_frames, g_requeued_frames,
		     g_received_frames, g_have_frame, g_latest_size);
		g_last_stream_log = (int)ts.tv_sec;
	}
}

static int fill_buffer_from_latest(unsigned int i, unsigned int *bytesused)
{
	unsigned int need = compressed_format() ? g_latest_size : g_framesize;

	if (g_buffers[i].length < (compressed_format() ? g_framesize : need)) {
		ilog("buffer %u too small: %zu < %u", i, g_buffers[i].length,
		     compressed_format() ? g_framesize : need);
		return -1;
	}
	if (compressed_format()) {
		if (!compressed_frame_ready())
			return 1;
		memcpy(g_buffers[i].start, g_latest, need);
		*bytesused = need;
		return 0;
	}
	if (g_have_frame)
		memcpy(g_buffers[i].start, g_latest, g_framesize);
	else
		memset(g_buffers[i].start, 16, g_framesize); /* dim grey-ish until first frame */
	*bytesused = g_framesize;
	return 0;
}

static int qbuf(unsigned int i, unsigned int bytesused)
{
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = i;
	buf.bytesused = bytesused;
	buf.length = g_buffers[i].length;
	if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
		ilog("VIDIOC_QBUF(%u) failed: %s", i, strerror(errno));
		if (fatal_device_errno(errno))
			g_run = 0;
		return -1;
	}
	g_buffers[i].queued = 1;
	return 0;
}

static int queue_buffer_from_latest(unsigned int i)
{
	unsigned int bytesused = 0;
	int ret = fill_buffer_from_latest(i, &bytesused);

	if (ret > 0)
		return ret;
	if (ret < 0)
		return ret;
	return qbuf(i, bytesused);
}


static int start_streaming(void)
{
	struct v4l2_requestbuffers rb;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	unsigned int i;

	if (streaming)
		return 0;

	memset(&rb, 0, sizeof(rb));
	rb.count = g_reqbufs;
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;
	if (ioctl(vfd, VIDIOC_REQBUFS, &rb) < 0) {
		ilog("VIDIOC_REQBUFS failed: %s", strerror(errno));
		if (fatal_device_errno(errno))
			g_run = 0;
		return -1;
	}
	g_nbuffers = rb.count;
	if (g_nbuffers == 0 || g_nbuffers > MAX_NBUF) {
		ilog("VIDIOC_REQBUFS returned invalid count %u", g_nbuffers);
		release_buffers();
		return -1;
	}

	for (i = 0; i < g_nbuffers; i++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (ioctl(vfd, VIDIOC_QUERYBUF, &buf) < 0) {
			ilog("VIDIOC_QUERYBUF(%u) failed: %s", i, strerror(errno));
			if (fatal_device_errno(errno))
				g_run = 0;
			release_buffers();
			return -1;
		}
		g_buffers[i].length = buf.length;
		if (g_buffers[i].length < g_framesize) {
			ilog("VIDIOC_QUERYBUF(%u) length too small: %zu < %u", i,
			     g_buffers[i].length, g_framesize);
			release_buffers();
			return -1;
		}
		g_buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
					  MAP_SHARED, vfd, buf.m.offset);
		if (g_buffers[i].start == MAP_FAILED) {
			ilog("mmap(%u) failed: %s", i, strerror(errno));
			release_buffers();
			return -1;
		}
	}

	g_requeued_frames = 0;
	for (i = 0; i < g_nbuffers; i++) {
		int ret = queue_buffer_from_latest(i);
		if (ret > 0)
			break;
		if (ret < 0) {
			release_buffers();
			return -1;
		}
		g_requeued_frames++;
	}
	if (i == 0) {
		release_buffers();
		streaming = 0;
		return 1;
	}

	if (ioctl(vfd, VIDIOC_STREAMON, &type) < 0) {
		ilog("VIDIOC_STREAMON failed: %s", strerror(errno));
		if (fatal_device_errno(errno))
			g_run = 0;
		release_buffers();
		return -1;
	}
	streaming = 1;
	g_dequeued_frames = 0;
	g_last_stream_log = 0;
	ilog("streaming ON (%ux%u %s sizeimage=%u, %u/%u buffers queued)",
	     g_width, g_height, format_name(), g_framesize, g_requeued_frames, g_nbuffers);
	return 0;
}

static void stop_streaming(void)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	if (!streaming)
		return;
	ioctl(vfd, VIDIOC_STREAMOFF, &type);
	release_buffers();
	streaming = 0;
	ilog("streaming OFF");
}

static int process_event(void)
{
	struct v4l2_event ev;
	struct uvc_event *uvc;
	struct uvc_request_data resp;

	if (ioctl(vfd, VIDIOC_DQEVENT, &ev) < 0) {
		if (fatal_device_errno(errno)) {
			ilog("VIDIOC_DQEVENT fatal: %s", strerror(errno));
			return -1;
		}
		return 0;
	}
	uvc = (struct uvc_event *)&ev.u.data;

	switch (ev.type) {
	case UVC_EVENT_CONNECT:
		g_connected = 1;
		ilog("event CONNECT");
		emit_event("CONNECT");
		break;
	case UVC_EVENT_DISCONNECT:
		g_connected = 0;
		ilog("event DISCONNECT");
		emit_event("DISCONNECT");
		g_stream_requested = 0;
		stop_streaming();
		if (g_exit_on_disconnect) {
			ilog("disconnect requested feeder exit for parent cleanup");
			return -1;
		}
		ilog("disconnect: keeping feeder alive for host reconnect");
		break;
	case UVC_EVENT_SETUP:
		memset(&resp, 0, sizeof(resp));
		process_setup(&uvc->req, &resp);
		if (ioctl(vfd, UVCIOC_SEND_RESPONSE, &resp) < 0) {
			if (fatal_device_errno(errno)) {
				ilog("UVCIOC_SEND_RESPONSE fatal: %s", strerror(errno));
				return -1;
			}
			ilog("UVCIOC_SEND_RESPONSE failed: %s", strerror(errno));
		}
		break;
	case UVC_EVENT_DATA:
		process_data(&uvc->data);
		break;
	case UVC_EVENT_STREAMON:
		g_stream_requested = 1;
		emit_event("STREAMON");
		if (start_streaming() > 0)
			ilog("streaming deferred until first valid %s frame", format_name());
		break;
	case UVC_EVENT_STREAMOFF:
		g_stream_requested = 0;
		emit_event("STREAMOFF");
		stop_streaming();
		break;
	default:
		break;
	}
	return 0;
}

static int process_dequeue(void)
{
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
		if (errno == EAGAIN)
			return 0;
		if (fatal_device_errno(errno)) {
			ilog("VIDIOC_DQBUF fatal: %s", strerror(errno));
			return -1;
		}
		ilog("VIDIOC_DQBUF failed: %s", strerror(errno));
		return 0;
	}
	if (buf.index >= g_nbuffers) {
		ilog("VIDIOC_DQBUF returned invalid index %u", buf.index);
		return 0;
	}
	g_dequeued_frames++;
	if (queue_buffer_from_latest(buf.index) < 0)
		return -1;
	g_requeued_frames++;
	log_stream_progress();
	return 0;
}

static void record_error_once(const char *why, unsigned int a, unsigned int b)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	if ((int)ts.tv_sec != g_last_record_log) {
		g_last_record_log = (int)ts.tv_sec;
		ilog("bad compressed FIFO record: %s (%u/%u)", why, a, b);
	}
}

static void reset_record_parser(void)
{
	g_hdr_off = 0;
	g_acc_off = 0;
	memset(&g_cur_hdr, 0, sizeof(g_cur_hdr));
}

static void accept_latest_frame(unsigned int size)
{
	memcpy(g_latest, g_acc, size);
	g_latest_size = size;
	g_acc_off = 0;
	g_received_frames++;
	if (!g_have_frame) {
		g_have_frame = 1;
		ilog("first %s frame received from FIFO (%u bytes)", format_name(), size);
	}
	if (g_stream_requested && !streaming) {
		if (start_streaming() == 0)
			ilog("deferred streaming started after first %s frame", format_name());
	}
}

static void feed_record_bytes(const unsigned char *buf, unsigned int len)
{
	unsigned int off = 0;

	while (off < len) {
		if (g_hdr_off < sizeof(g_hdr_buf)) {
			unsigned int need = sizeof(g_hdr_buf) - g_hdr_off;
			unsigned int chunk = len - off;
			if (chunk > need)
				chunk = need;
			memcpy(g_hdr_buf + g_hdr_off, buf + off, chunk);
			g_hdr_off += chunk;
			off += chunk;
			if (g_hdr_off < sizeof(g_hdr_buf))
				continue;
			memcpy(&g_cur_hdr, g_hdr_buf, sizeof(g_cur_hdr));
			if (g_cur_hdr.magic != LMI_UVC_RECORD_MAGIC ||
			    g_cur_hdr.version != LMI_UVC_RECORD_VERSION ||
			    g_cur_hdr.header_size != sizeof(g_cur_hdr)) {
				record_error_once("header", g_cur_hdr.magic, g_cur_hdr.payload_size);
				resync_record_header();
				continue;
			}
			if (g_cur_hdr.payload_size == 0 || g_cur_hdr.payload_size > g_framesize) {
				record_error_once("payload size", g_cur_hdr.payload_size, g_framesize);
				reset_record_parser();
				continue;
			}
		}
		if (g_hdr_off == sizeof(g_hdr_buf)) {
			unsigned int need = g_cur_hdr.payload_size - g_acc_off;
			unsigned int chunk = len - off;
			if (chunk > need)
				chunk = need;
			memcpy(g_acc + g_acc_off, buf + off, chunk);
			g_acc_off += chunk;
			off += chunk;
			if (g_acc_off >= g_cur_hdr.payload_size) {
				if (g_format != LMI_UVC_MJPEG || valid_jpeg_frame(g_acc, g_cur_hdr.payload_size)) {
					accept_latest_frame(g_cur_hdr.payload_size);
					reset_record_parser();
				} else {
					record_error_once("jpeg markers", g_cur_hdr.payload_size, g_framesize);
					resync_record_header();
				}
			}
		}
	}
}

static void drain_fifo(void)
{
	unsigned char tmp[8192];

	for (;;) {
		ssize_t n;
		if (!compressed_format()) {
			n = read(fifofd, g_acc + g_acc_off, g_framesize - g_acc_off);
			if (n > 0) {
				g_acc_off += (unsigned int)n;
				if (g_acc_off >= g_framesize) {
					accept_latest_frame(g_framesize);
				}
				continue;
			}
		} else {
			n = read(fifofd, tmp, sizeof(tmp));
			if (n > 0) {
				feed_record_bytes(tmp, (unsigned int)n);
				continue;
			}
		}
		if (n == 0) {
			/* writer closed: reopen below via poll re-arm; just stop draining */
			break;
		}
		break; /* EAGAIN / EWOULDBLOCK */
	}
}


static int open_fifo(void)
{
	struct stat st;
	if (stat(g_fifo, &st) != 0) {
		if (mkfifo(g_fifo, 0600) != 0 && errno != EEXIST) {
			ilog("mkfifo(%s) failed: %s", g_fifo, strerror(errno));
			return -1;
		}
	}
	/* O_RDWR so the FIFO has a permanent reader+writer end and poll never
	 * sees permanent EOF when the daemon momentarily closes. */
	fifofd = open(g_fifo, O_RDWR | O_NONBLOCK);
	if (fifofd < 0) {
		ilog("open(%s) failed: %s", g_fifo, strerror(errno));
		return -1;
	}
	return 0;
}

static int open_device(void)
{
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_event_subscription sub;
	unsigned int pixfmt = format_fourcc();
	int t;

	vfd = open(g_dev, O_RDWR | O_NONBLOCK);
	if (vfd < 0) {
		ilog("open(%s) failed: %s", g_dev, strerror(errno));
		return -1;
	}
	memset(&cap, 0, sizeof(cap));
	if (ioctl(vfd, VIDIOC_QUERYCAP, &cap) == 0)
		ilog("device %s driver=%s card=%s", g_dev, cap.driver, cap.card);

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = g_width;
	fmt.fmt.pix.height = g_height;
	fmt.fmt.pix.pixelformat = pixfmt;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = compressed_format() ? 0 : g_width * 2;
	fmt.fmt.pix.sizeimage = g_framesize;
	if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
		ilog("VIDIOC_S_FMT(output %s) failed: %s", format_name(), strerror(errno));
		return -1;
	}
	if (fmt.fmt.pix.pixelformat != pixfmt ||
	    fmt.fmt.pix.width != g_width || fmt.fmt.pix.height != g_height) {
		ilog("unexpected output format: %ux%u fourcc=%c%c%c%c wanted=%s",
		     fmt.fmt.pix.width, fmt.fmt.pix.height,
		     (char)(fmt.fmt.pix.pixelformat & 0xff),
		     (char)((fmt.fmt.pix.pixelformat >> 8) & 0xff),
		     (char)((fmt.fmt.pix.pixelformat >> 16) & 0xff),
		     (char)((fmt.fmt.pix.pixelformat >> 24) & 0xff),
		     format_name());
		return -1;
	}
	if (fmt.fmt.pix.sizeimage < g_framesize) {
		ilog("output sizeimage too small: %u < %u", fmt.fmt.pix.sizeimage, g_framesize);
		return -1;
	}
	if (fmt.fmt.pix.sizeimage > g_framesize)
		g_framesize = fmt.fmt.pix.sizeimage;

	static const int evs[] = { UVC_EVENT_SETUP, UVC_EVENT_DATA,
				   UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF,
				   UVC_EVENT_DISCONNECT, UVC_EVENT_CONNECT };
	for (t = 0; t < (int)(sizeof(evs) / sizeof(evs[0])); t++) {
		memset(&sub, 0, sizeof(sub));
		sub.type = evs[t];
		if (ioctl(vfd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
			ilog("subscribe event 0x%x failed: %s", evs[t], strerror(errno));
	}
	return 0;
}

static void write_ready_file(void)
{
	int fd;

	if (!g_ready_file[0])
		return;
	fd = open(g_ready_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		ilog("ready-file %s failed: %s", g_ready_file, strerror(errno));
		return;
	}
	if (write(fd, "ready\n", 6) != 6)
		ilog("ready-file %s short write: %s", g_ready_file, strerror(errno));
	close(fd);
}

static int open_event_fifo(void)
{
	struct stat st;

	if (!g_event_fifo[0])
		return 0;
	if (stat(g_event_fifo, &st) != 0) {
		if (mkfifo(g_event_fifo, 0600) != 0 && errno != EEXIST) {
			ilog("mkfifo(%s) failed: %s", g_event_fifo, strerror(errno));
			return -1;
		}
	}
	eventfd = open(g_event_fifo, O_RDWR | O_NONBLOCK);
	if (eventfd < 0) {
		ilog("open event fifo %s failed: %s", g_event_fifo, strerror(errno));
		return -1;
	}
	return 0;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [--device /dev/videoN] [--fifo PATH] [--width W] [--height H]\n"
		"          [--fps N] [--format yuyv|mjpeg|h264] [--max-frame BYTES]\n"
		"          [--maxpkt N] [--mult N] [--burst N] [--bulk]\n"
		"          [--intf N] [--buffers N] [--ready-file PATH] [--event-fifo PATH]\n"
		"          [--exit-on-disconnect] [--keep-after-disconnect] [-v]\n",
		p);
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) g_dev = argv[++i];
		else if (!strcmp(argv[i], "--fifo") && i + 1 < argc) g_fifo = argv[++i];
		else if (!strcmp(argv[i], "--ready-file") && i + 1 < argc) g_ready_file = argv[++i];
		else if (!strcmp(argv[i], "--event-fifo") && i + 1 < argc) g_event_fifo = argv[++i];
		else if (!strcmp(argv[i], "--width") && i + 1 < argc) g_width = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--height") && i + 1 < argc) g_height = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--fps") && i + 1 < argc) g_fps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
			const char *fmt = argv[++i];
			if (!strcmp(fmt, "yuyv")) g_format = LMI_UVC_YUYV;
			else if (!strcmp(fmt, "mjpeg")) g_format = LMI_UVC_MJPEG;
			else if (!strcmp(fmt, "h264")) g_format = LMI_UVC_H264;
			else { usage(argv[0]); return 2; }
		}
		else if (!strcmp(argv[i], "--max-frame") && i + 1 < argc) g_framesize = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--maxpkt") && i + 1 < argc) g_maxpkt = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mult") && i + 1 < argc) g_mult = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--burst") && i + 1 < argc) g_burst = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bulk")) g_bulk = 1;
		else if (!strcmp(argv[i], "--intf") && i + 1 < argc) g_streaming_intf = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--buffers") && i + 1 < argc) g_reqbufs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--exit-on-disconnect")) g_exit_on_disconnect = 1;
		else if (!strcmp(argv[i], "--keep-after-disconnect")) g_exit_on_disconnect = 0;
		else if (!strcmp(argv[i], "-v")) g_verbose = 1;
		else { usage(argv[0]); return 2; }
	}

	if (g_fps == 0) g_fps = 15;
	if (g_reqbufs < 2) g_reqbufs = 2;
	if (g_reqbufs > MAX_NBUF) g_reqbufs = MAX_NBUF;
	if (!compressed_format())
		g_framesize = g_width * g_height * 2;
	else if (g_framesize == 0)
		g_framesize = g_width * g_height;
	g_interval = 10000000u / g_fps;

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	if (open_fifo() < 0) return 1;
	if (open_event_fifo() < 0) return 1;
	if (open_device() < 0) return 1;

	g_acc = malloc(g_framesize);
	g_latest = malloc(g_framesize);
	if (!g_acc || !g_latest) { ilog("oom"); return 1; }
	memset(g_latest, 16, g_framesize);
	g_latest_size = compressed_format() ? 0 : g_framesize;

	fill_streaming_control(&g_probe);
	fill_streaming_control(&g_commit);
	write_ready_file();

	ilog("ready: dev=%s fifo=%s %ux%u@%u format=%s sizeimage=%u intf=%u buffers=%u exit_on_disconnect=%d",
	     g_dev, g_fifo, g_width, g_height, g_fps, format_name(),
	     g_framesize, g_streaming_intf, g_reqbufs, g_exit_on_disconnect);

	while (g_run) {
		struct pollfd fds[2];
		int n;

		fds[0].fd = vfd;
		fds[0].events = POLLPRI | (streaming ? POLLOUT : 0);
		fds[0].revents = 0;
		fds[1].fd = fifofd;
		fds[1].events = POLLIN;
		fds[1].revents = 0;

		n = poll(fds, 2, 1000);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			ilog("poll failed: %s", strerror(errno));
			break;
		}
		if (fatal_poll_revents(fds[0].revents)) {
			ilog("device poll fatal revents=0x%x connected=%d streaming=%d",
			     fds[0].revents, g_connected, streaming);
			break;
		}
		if (fatal_poll_revents(fds[1].revents)) {
			ilog("fifo poll fatal revents=0x%x", fds[1].revents);
			break;
		}
		if (fds[1].revents & POLLIN)
			drain_fifo();
		if (fds[0].revents & POLLPRI) {
			if (process_event() < 0)
				break;
		}
		if (streaming && (fds[0].revents & POLLOUT)) {
			if (process_dequeue() < 0)
				break;
		}
	}

	stop_streaming();
	if (fifofd >= 0)
		close(fifofd);
	if (eventfd >= 0)
		close(eventfd);
	if (vfd >= 0)
		close(vfd);
	return 0;
}
