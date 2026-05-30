// SPDX-License-Identifier: GPL-2.0
//
// lmi-uvc-gadget.c -- minimal UVC gadget feeder for the lmi software-ISP camera.
//
// Exposes a single uncompressed YUYV (YUY2) frame size over the f_uvc gadget
// V4L2 OUTPUT device, taking each full frame from a FIFO that
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

#define NBUF 4

static const char *g_dev = "/dev/video0";
static const char *g_fifo = "/tmp/lmi-uvc.fifo";
static unsigned int g_width = 640;
static unsigned int g_height = 480;
static unsigned int g_fps = 15;
static unsigned int g_maxpkt = 1024;
static unsigned int g_mult = 0;
static unsigned int g_burst = 0;
static int g_bulk = 0;
static unsigned int g_streaming_intf = 1; /* VS interface of a standalone UVC gadget */
static int g_verbose = 0;

static unsigned int g_framesize; /* width*height*2 (YUYV) */
static unsigned int g_interval;  /* 100ns units */

static int vfd = -1;
static int fifofd = -1;
static int streaming = 0;
static int control_sel = 0; /* PROBE or COMMIT, for the SET_CUR DATA that follows */

static struct uvc_streaming_control g_probe;
static struct uvc_streaming_control g_commit;

struct buffer {
	void *start;
	size_t length;
	int queued;
};
static struct buffer g_buffers[NBUF];
static unsigned int g_nbuffers;

/* FIFO frame accumulation: acc fills up, latest holds the most recent complete frame. */
static unsigned char *g_acc;
static unsigned int g_acc_off;
static unsigned char *g_latest;
static int g_have_frame;

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

static int fill_buffer_from_latest(unsigned int i)
{
	if (g_buffers[i].length < g_framesize) {
		ilog("buffer %u too small: %zu < %u", i, g_buffers[i].length, g_framesize);
		return -1;
	}
	if (g_have_frame)
		memcpy(g_buffers[i].start, g_latest, g_framesize);
	else
		memset(g_buffers[i].start, 16, g_framesize); /* dim grey-ish until first frame */
	return 0;
}

static int qbuf(unsigned int i)
{
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = i;
	buf.bytesused = g_framesize;
	buf.length = g_buffers[i].length;
	if (ioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
		ilog("VIDIOC_QBUF(%u) failed: %s", i, strerror(errno));
		return -1;
	}
	g_buffers[i].queued = 1;
	return 0;
}

static int start_streaming(void)
{
	struct v4l2_requestbuffers rb;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	unsigned int i;

	if (streaming)
		return 0;

	memset(&rb, 0, sizeof(rb));
	rb.count = NBUF;
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	rb.memory = V4L2_MEMORY_MMAP;
	if (ioctl(vfd, VIDIOC_REQBUFS, &rb) < 0) {
		ilog("VIDIOC_REQBUFS failed: %s", strerror(errno));
		return -1;
	}
	g_nbuffers = rb.count;
	if (g_nbuffers == 0 || g_nbuffers > NBUF) {
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
		if (fill_buffer_from_latest(i) < 0) {
			release_buffers();
			return -1;
		}
		if (qbuf(i) < 0) {
			release_buffers();
			return -1;
		}
	}

	if (ioctl(vfd, VIDIOC_STREAMON, &type) < 0) {
		ilog("VIDIOC_STREAMON failed: %s", strerror(errno));
		release_buffers();
		return -1;
	}
	streaming = 1;
	ilog("streaming ON (%ux%u YUYV, %u buffers)", g_width, g_height, g_nbuffers);
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

static void process_event(void)
{
	struct v4l2_event ev;
	struct uvc_event *uvc;
	struct uvc_request_data resp;

	if (ioctl(vfd, VIDIOC_DQEVENT, &ev) < 0)
		return;
	uvc = (struct uvc_event *)&ev.u.data;

	switch (ev.type) {
	case UVC_EVENT_CONNECT:
		ilog("event CONNECT");
		break;
	case UVC_EVENT_DISCONNECT:
		ilog("event DISCONNECT");
		stop_streaming();
		break;
	case UVC_EVENT_SETUP:
		memset(&resp, 0, sizeof(resp));
		process_setup(&uvc->req, &resp);
		if (ioctl(vfd, UVCIOC_SEND_RESPONSE, &resp) < 0)
			ilog("UVCIOC_SEND_RESPONSE failed: %s", strerror(errno));
		break;
	case UVC_EVENT_DATA:
		process_data(&uvc->data);
		break;
	case UVC_EVENT_STREAMON:
		start_streaming();
		break;
	case UVC_EVENT_STREAMOFF:
		stop_streaming();
		break;
	default:
		break;
	}
}

static void process_dequeue(void)
{
	struct v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
		if (errno != EAGAIN)
			ilog("VIDIOC_DQBUF failed: %s", strerror(errno));
		return;
	}
	if (buf.index >= g_nbuffers) {
		ilog("VIDIOC_DQBUF returned invalid index %u", buf.index);
		return;
	}
	if (fill_buffer_from_latest(buf.index) < 0)
		return;
	qbuf(buf.index);
}

static void drain_fifo(void)
{
	for (;;) {
		ssize_t n = read(fifofd, g_acc + g_acc_off, g_framesize - g_acc_off);
		if (n > 0) {
			g_acc_off += (unsigned int)n;
			if (g_acc_off >= g_framesize) {
				memcpy(g_latest, g_acc, g_framesize);
				g_acc_off = 0;
				if (!g_have_frame) {
					g_have_frame = 1;
					ilog("first frame received from FIFO");
				}
			}
			continue;
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
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = g_width * 2;
	fmt.fmt.pix.sizeimage = g_framesize;
	if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
		ilog("VIDIOC_S_FMT(output) failed: %s", strerror(errno));
		return -1;
	}
	if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV ||
	    fmt.fmt.pix.width != g_width || fmt.fmt.pix.height != g_height) {
		ilog("unexpected output format: %ux%u fourcc=%c%c%c%c",
		     fmt.fmt.pix.width, fmt.fmt.pix.height,
		     (char)(fmt.fmt.pix.pixelformat & 0xff),
		     (char)((fmt.fmt.pix.pixelformat >> 8) & 0xff),
		     (char)((fmt.fmt.pix.pixelformat >> 16) & 0xff),
		     (char)((fmt.fmt.pix.pixelformat >> 24) & 0xff));
		return -1;
	}
	if (fmt.fmt.pix.sizeimage < g_framesize) {
		ilog("output sizeimage too small: %u < %u", fmt.fmt.pix.sizeimage, g_framesize);
		return -1;
	}

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

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [--device /dev/videoN] [--fifo PATH] [--width W] [--height H]\n"
		"          [--fps N] [--maxpkt N] [--mult N] [--burst N] [--bulk] [--intf N] [-v]\n",
		p);
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) g_dev = argv[++i];
		else if (!strcmp(argv[i], "--fifo") && i + 1 < argc) g_fifo = argv[++i];
		else if (!strcmp(argv[i], "--width") && i + 1 < argc) g_width = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--height") && i + 1 < argc) g_height = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--fps") && i + 1 < argc) g_fps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--maxpkt") && i + 1 < argc) g_maxpkt = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mult") && i + 1 < argc) g_mult = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--burst") && i + 1 < argc) g_burst = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bulk")) g_bulk = 1;
		else if (!strcmp(argv[i], "--intf") && i + 1 < argc) g_streaming_intf = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-v")) g_verbose = 1;
		else { usage(argv[0]); return 2; }
	}

	if (g_fps == 0) g_fps = 15;
	g_framesize = g_width * g_height * 2;
	g_interval = 10000000u / g_fps;

	g_acc = malloc(g_framesize);
	g_latest = malloc(g_framesize);
	if (!g_acc || !g_latest) { ilog("oom"); return 1; }
	memset(g_latest, 16, g_framesize);

	fill_streaming_control(&g_probe);
	fill_streaming_control(&g_commit);

	if (open_fifo() < 0) return 1;
	if (open_device() < 0) return 1;

	ilog("ready: dev=%s fifo=%s %ux%u@%u YUYV framesize=%u intf=%u",
	     g_dev, g_fifo, g_width, g_height, g_fps, g_framesize, g_streaming_intf);

	for (;;) {
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
			if (errno == EINTR) continue;
			ilog("poll failed: %s", strerror(errno));
			break;
		}
		if (fds[1].revents & POLLIN)
			drain_fifo();
		if (fds[0].revents & POLLPRI)
			process_event();
		if (streaming && (fds[0].revents & POLLOUT))
			process_dequeue();
	}

	stop_streaming();
	return 0;
}
