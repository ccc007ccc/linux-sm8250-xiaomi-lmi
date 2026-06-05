// SPDX-License-Identifier: GPL-2.0
//
// lmi-uvc-gadget.c -- minimal UVC gadget feeder for the lmi software-ISP camera.
//
// Exposes one YUYV, MJPEG, or H.264 format with one or more frame sizes over
// the f_uvc gadget V4L2 OUTPUT device, taking each complete frame from the FIFO
// written by the lmi-camera Rust runtime's software-ISP backend. Handles the UVC
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

#include "lmi-jpeg.h"

#ifndef V4L2_BUF_TYPE_VIDEO_OUTPUT
#define V4L2_BUF_TYPE_VIDEO_OUTPUT 2
#endif
#ifndef V4L2_PIX_FMT_H264
#define V4L2_PIX_FMT_H264 v4l2_fourcc('H', '2', '6', '4')
#endif
#ifndef V4L2_BUF_FLAG_KEYFRAME
#define V4L2_BUF_FLAG_KEYFRAME 0x00000008
#endif
#ifndef V4L2_BUF_FLAG_PFRAME
#define V4L2_BUF_FLAG_PFRAME 0x00000010
#endif

#define MAX_NBUF 4
#define MAX_UVC_FRAMES 8
#define FIFO_READ_CHUNK 65536
#define LMI_UVC_RECORD_MAGIC 0x43564d4cU /* "LMVC" little-endian */
#define LMI_UVC_RECORD_VERSION_V1 1
#define LMI_UVC_RECORD_VERSION_V2 2
#define LMI_UVC_RECORD_BASE_HEADER_SIZE 16
#define LMI_UVC_RECORD_MAX_HEADER_SIZE 64
#define LMI_UVC_RECORD_FLAG_KEYFRAME 0x00000001U
#define LMI_UVC_RECORD_FLAG_PFRAME   0x00000002U

static const char *g_dev = "/dev/video0";
static const char *g_fifo = "/tmp/lmi-uvc.fifo";
static const char *g_ready_file = "";
static const char *g_event_fifo = "";
static unsigned int g_width = 640;
static unsigned int g_height = 480;
struct frame_desc {
	unsigned int index;
	unsigned int width;
	unsigned int height;
	unsigned int framesize;
	unsigned int interval;
};
static struct frame_desc g_frames[MAX_UVC_FRAMES];
static unsigned int g_frame_count;
static unsigned int g_max_framesize;
static unsigned int g_maxpkt = 1024;
static unsigned int g_mult = 0;
static unsigned int g_burst = 0;
static int g_bulk = 0;
static unsigned int g_fps = 15;
static unsigned int g_streaming_intf = 1; /* VS interface of a standalone UVC gadget */
static unsigned int g_format_index = 1;
static unsigned int g_frame_index = 1;
static unsigned int g_reqbufs = 2;
static unsigned int g_control_len_override;
static unsigned int g_control_len = 34;
static int g_h264_v4l2_frame_flags;
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
enum control_data_kind {
	CONTROL_DATA_NONE = 0,
	CONTROL_DATA_STREAMING,
	CONTROL_DATA_UVC_CONTROL,
};
static enum control_data_kind control_data_kind;
static uint8_t control_sel;  /* VS PROBE/COMMIT or VC CT/PU selector for following SET_CUR DATA */
static uint8_t control_unit;
static unsigned int control_data_len;
static unsigned int g_dequeued_frames;
static unsigned int g_requeued_frames;
static unsigned int g_received_frames;
static uint64_t g_next_pts_us;
static int g_last_stream_log;
static int g_last_record_log;
static volatile sig_atomic_t g_run = 1;

static struct uvc_streaming_control g_probe;
static struct uvc_streaming_control g_commit;

#define UVC_UNIT_CAMERA_TERMINAL 1
#define UVC_UNIT_PROCESSING      2

struct uvc_control_state {
	uint8_t unit;
	uint8_t selector;
	uint8_t len;
	int32_t cur;
	int32_t min;
	int32_t max;
	int32_t res;
	int32_t def;
	const char *name;
};

static struct uvc_control_state g_uvc_controls[] = {
	{ UVC_UNIT_CAMERA_TERMINAL, UVC_CT_AE_MODE_CONTROL, 1, 0x02, 0x01, 0x08, 0x0f, 0x02, "ae_mode" },
	{ UVC_UNIT_CAMERA_TERMINAL, UVC_CT_EXPOSURE_TIME_ABSOLUTE_CONTROL, 4, 333, 1, 10000, 1, 333, "exposure_time_absolute" },
	{ UVC_UNIT_PROCESSING, UVC_PU_GAIN_CONTROL, 2, 0, 0, 255, 1, 0, "gain" },
	{ UVC_UNIT_PROCESSING, UVC_PU_POWER_LINE_FREQUENCY_CONTROL, 1, 0, 0, 3, 1, 0, "power_line_frequency" },
};

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
	uint32_t flags;
};

static unsigned char *g_acc;
static unsigned int g_acc_off;
static unsigned char *g_latest;
static unsigned int g_latest_size;
static int g_have_frame;
static int g_latest_validated;
static int g_placeholder_frame;
static unsigned int g_latest_sequence;
static unsigned int g_latest_v4l2_flags;
static unsigned char g_hdr_buf[LMI_UVC_RECORD_MAX_HEADER_SIZE];
static unsigned int g_hdr_off;
static unsigned int g_hdr_need = LMI_UVC_RECORD_BASE_HEADER_SIZE;
static struct lmi_uvc_record_header g_cur_hdr;

#define H264_PARAM_MAX 65536
#define H264_LOG_LIMIT 16
#define H264_QUEUE_SLOTS 8
static unsigned char *g_h264_sps;
static unsigned int g_h264_sps_len;
static unsigned char *g_h264_pps;
static unsigned int g_h264_pps_len;
static unsigned int g_h264_record_logs;
struct h264_queue_entry {
	unsigned char *data;
	unsigned int size;
	unsigned int flags;
	unsigned int sequence;
};
static struct h264_queue_entry g_h264_queue[H264_QUEUE_SLOTS];
static unsigned int g_h264_queue_head;
static unsigned int g_h264_queue_count;
static unsigned int g_h264_queue_drops;

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

static int jpeg_dimensions_match(const unsigned char *p, unsigned int len,
				 unsigned int width, unsigned int height)
{
	unsigned int off;

	if (!valid_jpeg_frame(p, len))
		return 0;

	off = 2;
	while (off + 4 < len) {
		unsigned int marker;
		unsigned int seglen;

		if (p[off] != 0xff) {
			off++;
			continue;
		}
		while (off < len && p[off] == 0xff)
			off++;
		if (off >= len)
			break;
		marker = p[off++];
		if (marker == 0xd9 || marker == 0xda)
			break;
		if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
			continue;
		if (off + 2 > len)
			break;
		seglen = ((unsigned int)p[off] << 8) | p[off + 1];
		if (seglen < 2 || off + seglen > len)
			break;
		if ((marker >= 0xc0 && marker <= 0xc3) ||
		    (marker >= 0xc5 && marker <= 0xc7) ||
		    (marker >= 0xc9 && marker <= 0xcb) ||
		    (marker >= 0xcd && marker <= 0xcf)) {
			unsigned int h;
			unsigned int w;

			if (seglen < 7)
				return 0;
			h = ((unsigned int)p[off + 3] << 8) | p[off + 4];
			w = ((unsigned int)p[off + 5] << 8) | p[off + 6];
			return w == width && h == height;
		}
		off += seglen;
	}
	return 0;
}

struct h264_info {
	int saw_nal;
	int has_vcl;
	int has_idr;
	int has_sps;
	int has_pps;
	char types[96];
};

static int h264_find_start_code(const unsigned char *p, unsigned int len,
					unsigned int off, unsigned int *start,
					unsigned int *prefix)
{
	while (off + 3 < len) {
		if (p[off] == 0x00 && p[off + 1] == 0x00 && p[off + 2] == 0x01) {
			*start = off;
			*prefix = 3;
			return 1;
		}
		if (off + 4 < len && p[off] == 0x00 && p[off + 1] == 0x00 &&
		    p[off + 2] == 0x00 && p[off + 3] == 0x01) {
			*start = off;
			*prefix = 4;
			return 1;
		}
		off++;
	}
	return 0;
}

static int h264_append_param(unsigned char **dst, unsigned int *dst_len,
			     const unsigned char *src, unsigned int len)
{
	unsigned char *tmp;

	if (len == 0 || len > H264_PARAM_MAX)
		return 0;
	if (*dst_len == len && *dst && !memcmp(*dst, src, len))
		return 0;
	tmp = malloc(len);
	if (!tmp) {
		ilog("H264 parameter-set cache allocation failed: %u bytes", len);
		return -1;
	}
	memcpy(tmp, src, len);
	free(*dst);
	*dst = tmp;
	*dst_len = len;
	return 0;
}

static void h264_parse_payload(const unsigned char *p, unsigned int len,
			       struct h264_info *info, int cache_params)
{
	unsigned int off = 0;
	unsigned int start;
	unsigned int prefix;
	unsigned int types_len = 0;

	memset(info, 0, sizeof(*info));
	while (h264_find_start_code(p, len, off, &start, &prefix)) {
		unsigned int nal_off = start + prefix;
		unsigned int next_start;
		unsigned int next_prefix;
		unsigned int nal_end = len;
		unsigned int nal;

		if (nal_off >= len)
			break;
		if (h264_find_start_code(p, len, nal_off + 1, &next_start, &next_prefix))
			nal_end = next_start;
		nal = p[nal_off] & 0x1f;
		info->saw_nal = 1;
		if (types_len < sizeof(info->types)) {
			int n = snprintf(info->types + types_len, sizeof(info->types) - types_len,
					 types_len ? ",%u" : "%u", nal);
			if (n > 0) {
				types_len += (unsigned int)n;
				if (types_len >= sizeof(info->types))
					info->types[sizeof(info->types) - 1] = '\0';
			}
		}
		if (nal >= 1 && nal <= 5)
			info->has_vcl = 1;
		if (nal == 5)
			info->has_idr = 1;
		else if (nal == 7) {
			info->has_sps = 1;
			if (cache_params)
				h264_append_param(&g_h264_sps, &g_h264_sps_len, p + start, nal_end - start);
		} else if (nal == 8) {
			info->has_pps = 1;
			if (cache_params)
				h264_append_param(&g_h264_pps, &g_h264_pps_len, p + start, nal_end - start);
		}
		off = nal_end;
	}
}

static void h264_log_record(const struct h264_info *info, unsigned int size,
			    unsigned int seq, const char *action)
{
	if (g_h264_record_logs >= H264_LOG_LIMIT)
		return;
	g_h264_record_logs++;
	ilog("H264 record seq=%u size=%u nal=[%s] sps=%d pps=%d idr=%d vcl=%d action=%s",
	     seq, size, info->types[0] ? info->types : "none",
	     info->has_sps, info->has_pps, info->has_idr, info->has_vcl, action);
}

static void h264_queue_clear(void)
{
	unsigned int i;

	for (i = 0; i < H264_QUEUE_SLOTS; i++) {
		free(g_h264_queue[i].data);
		g_h264_queue[i].data = NULL;
		g_h264_queue[i].size = 0;
		g_h264_queue[i].flags = 0;
		g_h264_queue[i].sequence = 0;
	}
	g_h264_queue_head = 0;
	g_h264_queue_count = 0;
}

static int h264_queue_push(const unsigned char *data, unsigned int size,
			       unsigned int flags, unsigned int sequence, int is_sync)
{
	struct h264_queue_entry *entry;
	unsigned int idx;
	unsigned char *copy;

	if (size == 0 || size > g_framesize)
		return 0;
	if (g_h264_queue_count == H264_QUEUE_SLOTS) {
		h264_queue_clear();
		g_h264_queue_drops++;
		if (!is_sync) {
			g_have_frame = 0;
			ilog("H264 output queue full; dropped queued AUs and waiting for next sync AU");
			return 0;
		}
		ilog("H264 output queue full; dropped queued AUs and kept new sync AU");
	}

	copy = malloc(size);
	if (!copy) {
		ilog("H264 output queue allocation failed: %u bytes", size);
		return -1;
	}
	memcpy(copy, data, size);
	idx = (g_h264_queue_head + g_h264_queue_count) % H264_QUEUE_SLOTS;
	entry = &g_h264_queue[idx];
	entry->data = copy;
	entry->size = size;
	entry->flags = flags;
	entry->sequence = sequence;
	g_h264_queue_count++;
	return 1;
}

static int h264_queue_pop(unsigned char *dst, unsigned int *size, unsigned int *flags)
{
	struct h264_queue_entry *entry;

	if (g_h264_queue_count == 0)
		return 1;
	entry = &g_h264_queue[g_h264_queue_head];
	if (!entry->data || entry->size == 0 || entry->size > g_framesize)
		return -1;
	memcpy(dst, entry->data, entry->size);
	*size = entry->size;
	*flags = entry->flags;
	free(entry->data);
	entry->data = NULL;
	entry->size = 0;
	entry->flags = 0;
	entry->sequence = 0;
	g_h264_queue_head = (g_h264_queue_head + 1) % H264_QUEUE_SLOTS;
	g_h264_queue_count--;
	return 0;
}

static int h264_prefix_param_sets(unsigned int *size)
{
	unsigned int prefix_len;
	unsigned int out = 0;

	if (!g_h264_sps_len || !g_h264_pps_len)
		return 0;
	prefix_len = g_h264_sps_len + g_h264_pps_len;
	if (*size > g_framesize || prefix_len > g_framesize - *size)
		return -1;
	memmove(g_acc + prefix_len, g_acc, *size);
	memcpy(g_acc + out, g_h264_sps, g_h264_sps_len);
	out += g_h264_sps_len;
	memcpy(g_acc + out, g_h264_pps, g_h264_pps_len);
	*size += prefix_len;
	return 1;
}

static int h264_prepare_access_unit(unsigned int *size, unsigned int *flags, int *is_sync)
{
	struct h264_info info;
	int prefixed = 0;

	*flags = 0;
	*is_sync = 0;
	h264_parse_payload(g_acc, *size, &info, 1);
	if (!info.saw_nal) {
		h264_log_record(&info, *size, g_cur_hdr.sequence, "drop-no-nal");
		return 0;
	}
	if (!info.has_vcl) {
		h264_log_record(&info, *size, g_cur_hdr.sequence, "cache-param-only");
		return 0;
	}
	if (!info.has_sps || !info.has_pps) {
		prefixed = h264_prefix_param_sets(size);
		if (prefixed < 0) {
			h264_log_record(&info, *size, g_cur_hdr.sequence, "drop-param-prefix-overflow");
			return 0;
		}
		if (!prefixed && (info.has_idr ||
				    (g_cur_hdr.version >= LMI_UVC_RECORD_VERSION_V2 &&
				     (g_cur_hdr.flags & LMI_UVC_RECORD_FLAG_KEYFRAME)))) {
			h264_log_record(&info, *size, g_cur_hdr.sequence,
					info.has_idr ? "drop-idr-no-param-cache" : "drop-sync-no-param-cache");
			return 0;
		}
	}
	if (info.has_idr || (g_cur_hdr.version >= LMI_UVC_RECORD_VERSION_V2 &&
				(g_cur_hdr.flags & LMI_UVC_RECORD_FLAG_KEYFRAME))) {
		*is_sync = 1;
		*flags = g_h264_v4l2_frame_flags ? V4L2_BUF_FLAG_KEYFRAME : 0;
		h264_log_record(&info, *size, g_cur_hdr.sequence,
				info.has_idr ? (prefixed > 0 ? "accept-idr-prefixed" : "accept-idr") :
				(prefixed > 0 ? "accept-sync-prefixed" : "accept-sync"));
		return 1;
	}
	if (!g_have_frame) {
		h264_log_record(&info, *size, g_cur_hdr.sequence, "drop-wait-sync");
		return 0;
	}
	*flags = g_h264_v4l2_frame_flags ? V4L2_BUF_FLAG_PFRAME : 0;
	h264_log_record(&info, *size, g_cur_hdr.sequence,
			prefixed > 0 ? "accept-delta-prefixed" : "accept-delta");
	return 1;
}

static int compressed_frame_ready(void)
{
	if (!compressed_format())
		return 1;
	return g_have_frame && g_latest_size != 0 && g_latest_validated;
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
	char line[128];
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

static struct frame_desc *find_frame(unsigned int index)
{
	unsigned int i;
	for (i = 0; i < g_frame_count; i++) {
		if (g_frames[i].index == index)
			return &g_frames[i];
	}
	return NULL;
}

static void add_frame_desc(unsigned int index, unsigned int width, unsigned int height,
				   unsigned int framesize, unsigned int interval)
{
	if (index == 0 || width == 0 || height == 0 || framesize == 0 || g_frame_count >= MAX_UVC_FRAMES)
		return;
	if (find_frame(index))
		return;
	g_frames[g_frame_count].index = index;
	g_frames[g_frame_count].width = width;
	g_frames[g_frame_count].height = height;
	g_frames[g_frame_count].framesize = framesize;
	g_frames[g_frame_count].interval = interval;
	if (framesize > g_max_framesize)
		g_max_framesize = framesize;
	g_frame_count++;
}

static unsigned int streaming_control_len(void)
{
	if (g_control_len_override)
		return g_control_len_override;
	return g_format == LMI_UVC_H264 ? 48 : 34;
}

static void write_streaming_control_response(struct uvc_request_data *resp,
					     const struct uvc_streaming_control *ctrl)
{
	unsigned int len = g_control_len;

	if (len > sizeof(resp->data))
		len = sizeof(resp->data);
	memset(resp->data, 0, sizeof(resp->data));
	memcpy(resp->data, ctrl, sizeof(*ctrl));
	if (len >= 48) {
		/* UVC 1.5 extends PROBE/COMMIT from 34 to 48 bytes.  Keep the
		 * H.264-specific tail conservative: real-time usage, 8-bit luma,
		 * no optional temporal/layout flags, and CBR for the first stream.
		 */
		resp->data[34] = 1;      /* bUsage: real-time */
		resp->data[35] = 8;      /* bBitDepthLuma */
		resp->data[36] = 0;      /* bmSettings */
		resp->data[37] = 2;      /* bMaxNumberOfRefFramesPlus1 */
		resp->data[38] = 2;      /* bmRateControlModes: stream 0 CBR */
		resp->data[39] = 0;
		resp->data[40] = 0;      /* bmLayoutPerStream */
	}
	resp->length = len;
}

static void fill_streaming_control_frame(struct uvc_streaming_control *ctrl, unsigned int frame_index)
{
	struct frame_desc *frame = find_frame(frame_index);
	unsigned int max_frame = frame ? frame->framesize : g_framesize;
	unsigned int interval = frame && frame->interval ? frame->interval : g_interval;

	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->bmHint = 1;
	ctrl->bFormatIndex = g_format_index;
	ctrl->bFrameIndex = frame ? frame->index : g_frame_index;
	ctrl->dwFrameInterval = interval;
	ctrl->wKeyFrameRate = 1;
	ctrl->wPFrameRate = 1;
	ctrl->wCompQuality = 10000;
	ctrl->wDelay = 0;
	ctrl->dwMaxVideoFrameSize = max_frame;
	if (g_bulk)
		ctrl->dwMaxPayloadTransferSize = max_frame;
	else
		ctrl->dwMaxPayloadTransferSize = g_maxpkt * (g_mult + 1) * (g_burst + 1);
	ctrl->dwClockFrequency = 48000000;
	ctrl->bmFramingInfo = 3;
	ctrl->bPreferedVersion = 1;
	ctrl->bMinVersion = 1;
	ctrl->bMaxVersion = 1;
}

static void fill_streaming_control(struct uvc_streaming_control *ctrl)
{
	fill_streaming_control_frame(ctrl, g_frame_index);
}

static struct uvc_control_state *find_uvc_control(uint8_t unit, uint8_t selector)
{
	unsigned int i;
	for (i = 0; i < sizeof(g_uvc_controls) / sizeof(g_uvc_controls[0]); i++) {
		if (g_uvc_controls[i].unit == unit && g_uvc_controls[i].selector == selector)
			return &g_uvc_controls[i];
	}
	return NULL;
}

static int control_value_valid(const struct uvc_control_state *ctrl, int32_t value)
{
	if (ctrl->selector == UVC_CT_AE_MODE_CONTROL)
		return value == 0x01 || value == 0x02 || value == 0x04 || value == 0x08;
	return value >= ctrl->min && value <= ctrl->max;
}

static void write_le_value(unsigned char *dst, unsigned int len, int32_t value)
{
	unsigned int i;
	uint32_t v = (uint32_t)value;
	for (i = 0; i < len; i++)
		dst[i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

static int32_t read_le_value(const unsigned char *src, unsigned int len)
{
	unsigned int i;
	uint32_t v = 0;
	for (i = 0; i < len && i < 4; i++)
		v |= (uint32_t)src[i] << (i * 8);
	return (int32_t)v;
}

static void write_uvc_control_value(struct uvc_request_data *resp,
					const struct uvc_control_state *ctrl,
					int32_t value)
{
	memset(resp->data, 0, sizeof(resp->data));
	write_le_value(resp->data, ctrl->len, value);
	resp->length = ctrl->len;
}

static void process_uvc_control(uint8_t req, uint8_t unit, uint8_t cs,
					struct uvc_request_data *resp)
{
	struct uvc_control_state *ctrl = find_uvc_control(unit, cs);

	if (!ctrl)
		return;

	switch (req) {
	case UVC_SET_CUR:
		control_data_kind = CONTROL_DATA_UVC_CONTROL;
		control_unit = unit;
		control_sel = cs;
		control_data_len = ctrl->len;
		resp->length = ctrl->len;
		break;
	case UVC_GET_CUR:
		write_uvc_control_value(resp, ctrl, ctrl->cur);
		break;
	case UVC_GET_MIN:
		write_uvc_control_value(resp, ctrl, ctrl->min);
		break;
	case UVC_GET_MAX:
		write_uvc_control_value(resp, ctrl, ctrl->max);
		break;
	case UVC_GET_RES:
		write_uvc_control_value(resp, ctrl, ctrl->res);
		break;
	case UVC_GET_DEF:
		write_uvc_control_value(resp, ctrl, ctrl->def);
		break;
	case UVC_GET_LEN:
		resp->data[0] = ctrl->len;
		resp->data[1] = 0;
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

/* ---- UVC PROBE/COMMIT streaming-control request handling ---- */

static void process_streaming(uint8_t req, uint8_t cs, struct uvc_request_data *resp)
{
	struct uvc_streaming_control ctrl;

	if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL)
		return;

	resp->length = g_control_len;

	switch (req) {
	case UVC_SET_CUR:
		control_data_kind = CONTROL_DATA_STREAMING;
		control_unit = 0;
		control_sel = cs;        /* the next UVC_EVENT_DATA carries the payload */
		control_data_len = g_control_len;
		resp->length = g_control_len;
		break;
	case UVC_GET_CUR:
		if (cs == UVC_VS_PROBE_CONTROL)
			write_streaming_control_response(resp, &g_probe);
		else
			write_streaming_control_response(resp, &g_commit);
		break;
	case UVC_GET_MIN:
	case UVC_GET_MAX:
	case UVC_GET_DEF:
		fill_streaming_control(&ctrl);
		write_streaming_control_response(resp, &ctrl);
		break;
	case UVC_GET_RES:
		memset(resp->data, 0, sizeof(resp->data));
		resp->length = g_control_len;
		break;
	case UVC_GET_LEN:
		resp->data[0] = g_control_len & 0xff;
		resp->data[1] = g_control_len >> 8;
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
	uint8_t unit;

	control_data_kind = CONTROL_DATA_NONE;
	control_sel = 0;
	control_unit = 0;
	control_data_len = 0;
	resp->length = -EL2HLT; /* stall optional/unhandled controls */

	if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS)
		return;

	cs = ctrl->wValue >> 8;
	intf = ctrl->wIndex & 0xff;
	unit = ctrl->wIndex >> 8;

	if (intf == g_streaming_intf &&
	    (cs == UVC_VS_PROBE_CONTROL || cs == UVC_VS_COMMIT_CONTROL))
		process_streaming(ctrl->bRequest, cs, resp);
	else if (intf == 0 && (unit == UVC_UNIT_CAMERA_TERMINAL || unit == UVC_UNIT_PROCESSING))
		process_uvc_control(ctrl->bRequest, unit, cs, resp);

	if (g_verbose)
		ilog("setup type=0x%02x req=0x%02x cs=0x%02x intf=%u unit=%u len=%u -> resp.len=%d",
		     ctrl->bRequestType, ctrl->bRequest, cs, intf, unit, ctrl->wLength, resp->length);
}

static int start_streaming(void);
static void stop_streaming(void);
static int configure_output_format(void);

static void reset_record_parser(void)
{
	g_hdr_off = 0;
	g_hdr_need = LMI_UVC_RECORD_BASE_HEADER_SIZE;
	g_acc_off = 0;
	memset(&g_cur_hdr, 0, sizeof(g_cur_hdr));
}

static void resync_record_header(void)
{
	if (g_hdr_off > 1)
		memmove(g_hdr_buf, g_hdr_buf + 1, g_hdr_off - 1);
	g_hdr_off = g_hdr_off ? g_hdr_off - 1 : 0;
	g_hdr_need = LMI_UVC_RECORD_BASE_HEADER_SIZE;
	g_acc_off = 0;
	memset(&g_cur_hdr, 0, sizeof(g_cur_hdr));
}

static int seed_mjpeg_placeholder(void)
{
	size_t pixels;
	size_t rgb_size;
	unsigned char *rgb;
	int size;

	if (g_format != LMI_UVC_MJPEG || !g_latest)
		return 0;
	if (g_width == 0 || g_height == 0 || g_framesize == 0)
		return -1;
	pixels = (size_t)g_width * g_height;
	if (pixels > ((size_t)-1) / 3) {
		ilog("placeholder MJPEG dimensions overflow: %ux%u", g_width, g_height);
		return -1;
	}
	rgb_size = pixels * 3;
	rgb = malloc(rgb_size);
	if (!rgb) {
		ilog("placeholder MJPEG allocation failed: %zu bytes", rgb_size);
		return -1;
	}
	memset(rgb, 16, rgb_size);
	size = lmi_jpeg_encode_rgb420(g_latest, g_framesize, rgb,
					 g_width, g_height, g_width * 3, 60);
	free(rgb);
	if (size <= 0) {
		ilog("placeholder MJPEG encode failed: max-frame=%u %ux%u", g_framesize, g_width, g_height);
		return -1;
	}
	g_have_frame = 1;
	g_latest_validated = 1;
	g_placeholder_frame = 1;
	g_latest_size = (unsigned int)size;
	g_latest_sequence = 0;
	g_latest_v4l2_flags = V4L2_BUF_FLAG_KEYFRAME;
	ilog("seeded MJPEG placeholder frame (%u bytes for %ux%u)",
	     g_latest_size, g_width, g_height);
	return 0;
}

static void clear_latest_frame(void)
{
	reset_record_parser();
	g_have_frame = 0;
	g_latest_size = compressed_format() ? 0 : g_framesize;
	g_latest_validated = compressed_format() ? 0 : 1;
	g_placeholder_frame = 0;
	g_latest_sequence = 0;
	g_latest_v4l2_flags = 0;
	if (g_format == LMI_UVC_H264) {
		free(g_h264_sps);
		free(g_h264_pps);
		g_h264_sps = NULL;
		g_h264_pps = NULL;
		g_h264_sps_len = 0;
		g_h264_pps_len = 0;
		g_h264_record_logs = 0;
		h264_queue_clear();
	}
	if (!compressed_format() && g_latest)
		memset(g_latest, 16, g_framesize);
	else if (g_format == LMI_UVC_MJPEG && g_latest)
		seed_mjpeg_placeholder();
}

static void clear_h264_session_frame(const char *why)
{
	if (g_format != LMI_UVC_H264)
		return;
	clear_latest_frame();
	ilog("cleared H264 frame cache on %s to avoid stale inter-session frames", why);
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

	if (control_data_kind == CONTROL_DATA_NONE || control_sel == 0 || data->length <= 0)
		return;

	if (control_data_kind == CONTROL_DATA_UVC_CONTROL) {
		struct uvc_control_state *ctrl = find_uvc_control(control_unit, control_sel);
		if (ctrl) {
			if ((unsigned int)data->length < ctrl->len) {
				ilog("ignored short control payload: unit=%u selector=0x%02x %s len=%d expected=%u",
				     ctrl->unit, ctrl->selector, ctrl->name, data->length, ctrl->len);
			} else {
				int32_t value = read_le_value(data->data, ctrl->len);
				if (control_value_valid(ctrl, value)) {
					char event[128];
					ctrl->cur = value;
					snprintf(event, sizeof(event), "CTRL unit=%u selector=%u name=%s value=%d",
						 ctrl->unit, ctrl->selector, ctrl->name, ctrl->cur);
					emit_event(event);
					if (g_verbose)
						ilog("control set: unit=%u selector=0x%02x %s=%d",
						     ctrl->unit, ctrl->selector, ctrl->name, ctrl->cur);
				} else {
					ilog("ignored invalid control value: unit=%u selector=0x%02x %s=%d range=%d..%d",
					     ctrl->unit, ctrl->selector, ctrl->name, value, ctrl->min, ctrl->max);
				}
			}
		}
		control_data_kind = CONTROL_DATA_NONE;
		control_sel = 0;
		control_unit = 0;
		control_data_len = 0;
		return;
	}

	target = (control_sel == UVC_VS_COMMIT_CONTROL) ? &g_commit : &g_probe;
	{
		unsigned int len = (unsigned int)data->length;
		if (len > sizeof(*target))
			len = sizeof(*target);
		memcpy(target, data->data, len);
	}

	if (control_sel == UVC_VS_PROBE_CONTROL) {
		if (!find_frame(g_probe.bFrameIndex))
			g_probe.bFrameIndex = g_frame_index;
		fill_streaming_control_frame(&g_probe, g_probe.bFrameIndex);
	} else if (control_sel == UVC_VS_COMMIT_CONTROL) {
		struct frame_desc *frame;
		char event[128];

		if (g_verbose)
			ilog("commit: fmt=%u frame=%u interval=%u maxframe=%u",
			     g_commit.bFormatIndex, g_commit.bFrameIndex,
			     g_commit.dwFrameInterval, g_commit.dwMaxVideoFrameSize);
		g_commit.bFormatIndex = g_format_index;
		if (!find_frame(g_commit.bFrameIndex))
			g_commit.bFrameIndex = g_frame_index;
		fill_streaming_control_frame(&g_commit, g_commit.bFrameIndex);
		frame = find_frame(g_commit.bFrameIndex);
		if (frame) {
			unsigned int next_interval = frame->interval ? frame->interval : 10000000u / g_fps;

			if (g_commit.bFrameIndex != g_frame_index ||
			    g_width != frame->width || g_height != frame->height ||
			    g_framesize != frame->framesize || g_interval != next_interval) {
				if (streaming)
					stop_streaming();
				g_frame_index = frame->index;
				g_width = frame->width;
				g_height = frame->height;
				g_framesize = frame->framesize;
				g_interval = next_interval;
				clear_latest_frame();
				if (vfd >= 0)
					configure_output_format();
				ilog("commit selected frame %u: %ux%u sizeimage=%u interval100ns=%u; cleared stale frame cache",
				     g_frame_index, g_width, g_height, g_framesize, g_interval);
			}
		}
		frame = find_frame(g_commit.bFrameIndex);
		if (frame) {
			snprintf(event, sizeof(event), "COMMIT frame=%u width=%u height=%u size=%u",
				 frame->index, frame->width, frame->height, frame->framesize);
			emit_event(event);
		}
	}

	control_data_kind = CONTROL_DATA_NONE;
	control_sel = 0;
	control_unit = 0;
	control_data_len = 0;
}

/* ---- OUTPUT buffer queue ---- */

static void log_stream_progress(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	if (!g_last_stream_log)
		g_last_stream_log = (int)ts.tv_sec;
	if ((int)ts.tv_sec - g_last_stream_log >= 2) {
		ilog("streaming stats: fmt=%s dq=%u q=%u rx=%u have=%d latest=%u seq=%u",
		     format_name(), g_dequeued_frames, g_requeued_frames,
		     g_received_frames, g_have_frame, g_latest_size, g_latest_sequence);
		g_last_stream_log = (int)ts.tv_sec;
	}
}

static int fill_buffer_from_latest(unsigned int i, unsigned int *bytesused,
				       unsigned int *flags)
{
	unsigned int need = compressed_format() ? g_latest_size : g_framesize;

	*flags = 0;
	if (g_buffers[i].length < (compressed_format() ? g_framesize : need)) {
		ilog("buffer %u too small: %zu < %u", i, g_buffers[i].length,
		     compressed_format() ? g_framesize : need);
		return -1;
	}
	if (g_format == LMI_UVC_H264)
		return h264_queue_pop(g_buffers[i].start, bytesused, flags);
	if (compressed_format()) {
		if (!compressed_frame_ready())
			return 1;
		memcpy(g_buffers[i].start, g_latest, need);
		*bytesused = need;
		*flags = g_latest_v4l2_flags;
		return 0;
	}
	if (g_have_frame)
		memcpy(g_buffers[i].start, g_latest, g_framesize);
	else
		memset(g_buffers[i].start, 16, g_framesize); /* dim grey-ish until first frame */
	*bytesused = g_framesize;
	return 0;
}

static int qbuf(unsigned int i, unsigned int bytesused, unsigned int flags)
{
	struct v4l2_buffer buf;
	uint64_t pts_us;
	uint64_t step_us;

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = i;
	buf.bytesused = bytesused;
	buf.length = g_buffers[i].length;
	buf.flags = flags;
	pts_us = g_next_pts_us;
	step_us = g_interval ? g_interval / 10 : 1000000ull / (g_fps ? g_fps : 30);
	if (!step_us)
		step_us = 1;
	g_next_pts_us += step_us;
	buf.timestamp.tv_sec = pts_us / 1000000ull;
	buf.timestamp.tv_usec = pts_us % 1000000ull;
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
	unsigned int flags = 0;
	int ret = fill_buffer_from_latest(i, &bytesused, &flags);

	if (ret > 0)
		return ret;
	if (ret < 0)
		return ret;
	return qbuf(i, bytesused, flags);
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

	/* f_uvc only emits UVC PTS when tv_sec is non-zero; start at 1s so
	 * DirectShow sees timestamps from the first queued H.264 access unit. */
	g_next_pts_us = 1000000ull;
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
		clear_h264_session_frame("DISCONNECT");
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
		clear_h264_session_frame("STREAMON");
		emit_event("STREAMON");
		if (start_streaming() > 0)
			ilog("streaming deferred until first valid %s frame", format_name());
		break;
	case UVC_EVENT_STREAMOFF:
		g_stream_requested = 0;
		emit_event("STREAMOFF");
		stop_streaming();
		clear_h264_session_frame("STREAMOFF");
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
		return 1;
	}
	g_buffers[buf.index].queued = 0;
	g_dequeued_frames++;
	{
		int ret = queue_buffer_from_latest(buf.index);

		if (ret < 0)
			return -1;
		if (ret == 0)
			g_requeued_frames++;
	}
	log_stream_progress();
	return 1;
}

static int drain_dequeue(void)
{
	unsigned int drained = 0;

	/* f_uvc can make more than one OUTPUT buffer available before poll wakes us.
	 * Requeue all currently completed buffers so the host endpoint is not left
	 * half-empty while we drain FIFO data from the producer. */
	while (drained < g_nbuffers) {
		int ret = process_dequeue();
		if (ret <= 0)
			return ret;
		drained++;
	}
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

static void accept_latest_frame(unsigned int size)
{
	int was_placeholder = g_placeholder_frame;
	unsigned int flags = g_format == LMI_UVC_MJPEG ? V4L2_BUF_FLAG_KEYFRAME : 0;

	if (g_format == LMI_UVC_H264) {
		int queued;
		int h264_sync = 0;

		if (!h264_prepare_access_unit(&size, &flags, &h264_sync)) {
			g_acc_off = 0;
			return;
		}
		queued = h264_queue_push(g_acc, size, flags, g_cur_hdr.sequence, h264_sync);
		if (queued <= 0) {
			if (queued < 0) {
				h264_queue_clear();
				g_have_frame = 0;
				ilog("H264 output queue failed; cleared queued AUs and waiting for next sync AU");
			}
			g_acc_off = 0;
			return;
		}
	}

	g_latest_validated = !compressed_format() || g_format != LMI_UVC_MJPEG ||
		jpeg_dimensions_match(g_acc, size, g_width, g_height);
	if (compressed_format()) {
		unsigned char *tmp = g_latest;
		g_latest = g_acc;
		g_acc = tmp;
	} else {
		memcpy(g_latest, g_acc, size);
	}
	g_latest_size = size;
	g_latest_sequence = g_cur_hdr.sequence;
	g_latest_v4l2_flags = flags;
	g_placeholder_frame = 0;
	g_acc_off = 0;
	g_received_frames++;
	if (!g_have_frame || was_placeholder) {
		g_have_frame = 1;
		ilog("first real %s frame received from FIFO (%u bytes)", format_name(), size);
	}
	if (g_stream_requested && !streaming) {
		if (start_streaming() == 0)
			ilog("deferred streaming started after first %s frame", format_name());
	} else if (g_format == LMI_UVC_H264 && streaming) {
		unsigned int i;

		for (i = 0; i < g_nbuffers && g_h264_queue_count; i++) {
			if (!g_buffers[i].queued) {
				int ret = queue_buffer_from_latest(i);

				if (ret < 0) {
					g_run = 0;
					break;
				}
				if (ret == 0)
					g_requeued_frames++;
			}
		}
	}
}

static void feed_record_bytes(const unsigned char *buf, unsigned int len)
{
	unsigned int off = 0;

	while (off < len) {
		if (g_hdr_off < g_hdr_need) {
			unsigned int need = g_hdr_need - g_hdr_off;
			unsigned int chunk = len - off;
			if (chunk > need)
				chunk = need;
			memcpy(g_hdr_buf + g_hdr_off, buf + off, chunk);
			g_hdr_off += chunk;
			off += chunk;
			if (g_hdr_off < LMI_UVC_RECORD_BASE_HEADER_SIZE)
				continue;
			memset(&g_cur_hdr, 0, sizeof(g_cur_hdr));
			memcpy(&g_cur_hdr, g_hdr_buf, LMI_UVC_RECORD_BASE_HEADER_SIZE);
			if (g_cur_hdr.magic != LMI_UVC_RECORD_MAGIC ||
			    !((g_cur_hdr.version == LMI_UVC_RECORD_VERSION_V1 &&
			       g_cur_hdr.header_size == LMI_UVC_RECORD_BASE_HEADER_SIZE) ||
			      (g_cur_hdr.version == LMI_UVC_RECORD_VERSION_V2 &&
			       g_cur_hdr.header_size >= sizeof(g_cur_hdr) &&
			       g_cur_hdr.header_size <= LMI_UVC_RECORD_MAX_HEADER_SIZE))) {
				record_error_once("header", g_cur_hdr.magic, g_cur_hdr.payload_size);
				resync_record_header();
				continue;
			}
			if (g_hdr_need != g_cur_hdr.header_size) {
				g_hdr_need = g_cur_hdr.header_size;
				continue;
			}
			if (g_cur_hdr.header_size >= sizeof(g_cur_hdr))
				memcpy(&g_cur_hdr, g_hdr_buf, sizeof(g_cur_hdr));
			if (g_cur_hdr.payload_size == 0 || g_cur_hdr.payload_size > g_framesize) {
				record_error_once("payload size", g_cur_hdr.payload_size, g_framesize);
				reset_record_parser();
				continue;
			}
		}
		if (g_hdr_off >= g_hdr_need) {
			unsigned int need = g_cur_hdr.payload_size - g_acc_off;
			unsigned int chunk = len - off;
			if (chunk > need)
				chunk = need;
			memcpy(g_acc + g_acc_off, buf + off, chunk);
			g_acc_off += chunk;
			off += chunk;
			if (g_acc_off >= g_cur_hdr.payload_size) {
				if (g_format != LMI_UVC_MJPEG ||
				    jpeg_dimensions_match(g_acc, g_cur_hdr.payload_size, g_width, g_height)) {
					accept_latest_frame(g_cur_hdr.payload_size);
					reset_record_parser();
				} else {
					record_error_once("jpeg dimensions", g_cur_hdr.payload_size, g_framesize);
					reset_record_parser();
				}
			}
		}
	}
}

static void drain_fifo(void)
{
	unsigned char tmp[FIFO_READ_CHUNK];

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

static int configure_output_format(void)
{
	struct v4l2_format fmt;
	struct v4l2_streamparm parm;
	unsigned int pixfmt = format_fourcc();

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = g_width;
	fmt.fmt.pix.height = g_height;
	fmt.fmt.pix.pixelformat = pixfmt;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = compressed_format() ? 0 : g_width * 2;
	fmt.fmt.pix.sizeimage = g_framesize;
	if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) {
		ilog("VIDIOC_S_FMT(output %s %ux%u) failed: %s",
		     format_name(), g_width, g_height, strerror(errno));
		return -1;
	}
	if (fmt.fmt.pix.pixelformat != pixfmt ||
	    fmt.fmt.pix.width != g_width || fmt.fmt.pix.height != g_height) {
		ilog("unexpected output format: %ux%u fourcc=%c%c%c%c wanted=%s %ux%u",
		     fmt.fmt.pix.width, fmt.fmt.pix.height,
		     (char)(fmt.fmt.pix.pixelformat & 0xff),
		     (char)((fmt.fmt.pix.pixelformat >> 8) & 0xff),
		     (char)((fmt.fmt.pix.pixelformat >> 16) & 0xff),
		     (char)((fmt.fmt.pix.pixelformat >> 24) & 0xff),
		     format_name(), g_width, g_height);
		return -1;
	}
	if (fmt.fmt.pix.sizeimage < g_framesize) {
		ilog("output sizeimage too small: %u < %u", fmt.fmt.pix.sizeimage, g_framesize);
		return -1;
	}
	if (fmt.fmt.pix.sizeimage > g_framesize)
		g_framesize = fmt.fmt.pix.sizeimage;

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	parm.parm.output.timeperframe.numerator = 1;
	parm.parm.output.timeperframe.denominator = g_fps;
	if (ioctl(vfd, VIDIOC_S_PARM, &parm) < 0) {
		ilog("VIDIOC_S_PARM(output %ufps) failed: %s", g_fps, strerror(errno));
	} else {
		ilog("output interval set: requested=1/%u got=%u/%u interval100ns=%u",
		     g_fps, parm.parm.output.timeperframe.numerator,
		     parm.parm.output.timeperframe.denominator, g_interval);
	}

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(vfd, VIDIOC_G_PARM, &parm) < 0) {
		ilog("VIDIOC_G_PARM(output) failed: %s", strerror(errno));
	} else {
		unsigned int num = parm.parm.output.timeperframe.numerator;
		unsigned int den = parm.parm.output.timeperframe.denominator;
		unsigned int interval100ns = den ? (unsigned int)((10000000ULL * num) / den) : 0;

		ilog("output interval active: %u/%u interval100ns=%u",
		     num, den, interval100ns);
	}
	return 0;
}

static int open_device(void)
{
	struct v4l2_capability cap;
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

	if (configure_output_format() < 0)
		return -1;

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
		"          [--format-index N] [--frame-index N] [--frame IDX:W:H:BYTES[:INTERVAL100NS]]\n"
		"          [--maxpkt N] [--mult N] [--burst N] [--bulk]\n"
		"          [--intf N] [--buffers N] [--control-len 34|48] [--h264-keyframe-flags]\n"
		"          [--ready-file PATH] [--event-fifo PATH]\n"
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
		else if (!strcmp(argv[i], "--format-index") && i + 1 < argc) g_format_index = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--frame-index") && i + 1 < argc) g_frame_index = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--frame") && i + 1 < argc) {
			unsigned int idx, width, height, size, interval = 0;
			int consumed = 0;
			const char *spec = argv[++i];

			if (sscanf(spec, "%u:%u:%u:%u%n", &idx, &width, &height, &size, &consumed) != 4) {
				usage(argv[0]);
				return 2;
			}
			if (spec[consumed] == ':') {
				int extra = 0;

				if (sscanf(spec + consumed + 1, "%u%n", &interval, &extra) != 1 || spec[consumed + 1 + extra] != '\0') {
					usage(argv[0]);
					return 2;
				}
			} else if (spec[consumed] != '\0') {
				usage(argv[0]);
				return 2;
			}
			add_frame_desc(idx, width, height, size, interval);
		}
		else if (!strcmp(argv[i], "--maxpkt") && i + 1 < argc) g_maxpkt = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mult") && i + 1 < argc) g_mult = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--burst") && i + 1 < argc) g_burst = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--control-len") && i + 1 < argc) g_control_len_override = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--h264-keyframe-flags")) g_h264_v4l2_frame_flags = 1;
		else if (!strcmp(argv[i], "--h264-no-keyframe-flags")) g_h264_v4l2_frame_flags = 0;
		else if (!strcmp(argv[i], "--bulk")) g_bulk = 1;
		else if (!strcmp(argv[i], "--intf") && i + 1 < argc) g_streaming_intf = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--buffers") && i + 1 < argc) g_reqbufs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--exit-on-disconnect")) g_exit_on_disconnect = 1;
		else if (!strcmp(argv[i], "--keep-after-disconnect")) g_exit_on_disconnect = 0;
		else if (!strcmp(argv[i], "-v")) g_verbose = 1;
		else { usage(argv[0]); return 2; }
	}

	if (g_fps == 0) g_fps = 15;
	if (g_format_index == 0) g_format_index = 1;
	if (g_frame_index == 0) g_frame_index = 1;
	if (g_control_len_override && g_control_len_override != 34 && g_control_len_override != 48) {
		usage(argv[0]);
		return 2;
	}
	g_control_len = streaming_control_len();
	if (g_reqbufs < 2) g_reqbufs = 2;
	if (g_reqbufs > MAX_NBUF) g_reqbufs = MAX_NBUF;
	if (!compressed_format())
		g_framesize = g_width * g_height * 2;
	else if (g_framesize == 0)
		g_framesize = g_width * g_height;
	if (g_frame_count == 0)
		add_frame_desc(g_frame_index, g_width, g_height, g_framesize, 0);
	{
		struct frame_desc *frame = find_frame(g_frame_index);
		if (!frame)
			frame = &g_frames[0];
		g_frame_index = frame->index;
		g_width = frame->width;
		g_height = frame->height;
		g_framesize = frame->framesize;
		g_interval = frame->interval ? frame->interval : 10000000u / g_fps;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	if (open_fifo() < 0) return 1;
	if (open_event_fifo() < 0) return 1;
	if (open_device() < 0) return 1;

	if (g_max_framesize < g_framesize)
		g_max_framesize = g_framesize;
	g_acc = malloc(g_max_framesize);
	g_latest = malloc(g_max_framesize);
	if (!g_acc || !g_latest) { ilog("oom"); return 1; }
	if (g_format == LMI_UVC_H264)
		h264_queue_clear();
	clear_latest_frame();

	fill_streaming_control(&g_probe);
	fill_streaming_control(&g_commit);
	write_ready_file();

	ilog("ready: dev=%s fifo=%s %ux%u@%u format=%s fmtidx=%u frameidx=%u sizeimage=%u intf=%u buffers=%u control_len=%u h264_v4l2_frame_flags=%d placeholder=%s exit_on_disconnect=%d",
	     g_dev, g_fifo, g_width, g_height, g_fps, format_name(),
	     g_format_index, g_frame_index, g_framesize, g_streaming_intf,
	     g_reqbufs, g_control_len, g_h264_v4l2_frame_flags,
	     g_format == LMI_UVC_MJPEG ? "mjpeg-q60-gray" : "none",
	     g_exit_on_disconnect);
	for (i = 0; i < (int)g_frame_count; i++)
		ilog("frame table[%d]: index=%u %ux%u sizeimage=%u%s",
		     i, g_frames[i].index, g_frames[i].width, g_frames[i].height,
		     g_frames[i].framesize,
		     g_frames[i].index == g_frame_index ? " active" : "");

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
			if (drain_dequeue() < 0)
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
	free(g_h264_sps);
	free(g_h264_pps);
	return 0;
}
