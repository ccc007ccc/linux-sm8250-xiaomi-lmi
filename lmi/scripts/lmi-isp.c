// SPDX-License-Identifier: GPL-2.0
//
// lmi-isp.c -- fast C software ISP + streamer for the lmi OV13B10 camera.
//
// Captures RAW10 (MIPI packed GRBG, V4L2 fourcc "pgAA") from a CAMSS RDI video
// node whose media pipeline + sensor mode were already configured by the
// Rust lmi-camera runtime, runs a real software ISP in C
//   10-bit unpack -> black-level -> gray-world AWB -> bilinear demosaic
//   -> gamma tone curve (10-bit linear -> 8-bit display) -> scale -> YUYV/NV12/MJPEG
// and writes processed frames to a v4l2loopback OUTPUT node (write()), a FIFO
// consumed by lmi-uvc-gadget, or a finite dump file for regression checks. Optional auto-exposure drives the sensor
// subdev's exposure/analogue/digital-gain controls, and software auto-tone
// can lift the processed output when those controls saturate.
//
// This is the performance/quality path: full-resolution demosaic and a proper
// tone curve at far higher fps than the pure-Python ISP. Cross-compiled static
// with the Android NDK and run from /tmp; nothing touches the rootfs, and the
// kernel /dev/video3 stays truthful RAW.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#ifndef V4L2_CID_PIXEL_RATE
#include <linux/v4l2-controls.h>
#endif

#include "lmi-jpeg.h"

#ifndef F_GETPIPE_SZ
#define F_GETPIPE_SZ 1032
#endif
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif
#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN (V4L2_CID_IMAGE_PROC_CLASS_BASE + 5)
#endif
#ifndef V4L2_CID_PIXEL_RATE
#define V4L2_CID_PIXEL_RATE (V4L2_CID_IMAGE_PROC_CLASS_BASE + 2)
#endif
#ifndef V4L2_CID_HBLANK
#define V4L2_CID_HBLANK (V4L2_CID_IMAGE_SOURCE_CLASS_BASE + 2)
#endif
#ifndef V4L2_CID_VBLANK
#define V4L2_CID_VBLANK (V4L2_CID_IMAGE_SOURCE_CLASS_BASE + 1)
#endif

#define NBUF 4
#define MAXPLANES 1
#define LMI_UVC_RECORD_MAGIC 0x43564d4cU /* "LMVC" little-endian */
#define LMI_UVC_RECORD_VERSION 1
#define LMI_UVC_ROI_MAX_COORD 65535
#define LMI_UVC_ROI_COORD_DEN 65536
#define LMI_FLICKER_BANDS 48
#define LMI_FLICKER_MIN_BANDS 16

/* ---- config ---- */
static const char *g_raw = "/dev/video3";
static const char *g_ctrl = "";		  /* sensor subdev for AE (optional) */
static const char *g_loopback = "";	   /* v4l2loopback OUTPUT node, or "" */
static const char *g_fifo = "";		   /* FIFO for the UVC feeder, or "" */
static const char *g_control_fifo = ""; /* FIFO for live UVC control updates, or "" */
static const char *g_dump = "";		   /* processed frame dump for regression, or "" */
static int g_dump_frames = 1;
static int g_out_w = 1280, g_out_h = 720;
/* Orientation is owned by the kernel: camera-rs reads V4L2_CID_CAMERA_SENSOR_ROTATION
 * (DTS `rotation`) and passes it as --rotate so the YUYV loopback output is presented
 * upright, exactly like libcamera would.  The ISP never invents an angle. */
static int g_rotate;			   /* CCW degrees to apply to the YUYV loopback: 0/90/180/270 */
static int g_logical_w, g_logical_h;   /* pre-rotation output size used for source mapping */
static int g_denoise;			   /* 0..100 motion-gated temporal denoise for the YUYV loopback */
static int g_denoise_thresh = 18;	  /* per-sample motion threshold; above = keep current (no ghost) */
static uint8_t *g_prev_frame;		  /* previous YUYV frame for temporal denoise */
enum source_aspect_policy {
	SOURCE_ASPECT_STRETCH = 0,
	SOURCE_ASPECT_PRESERVE,
};
static enum source_aspect_policy g_source_aspect = SOURCE_ASPECT_STRETCH;
static int g_src_x, g_src_y, g_src_w, g_src_h;
static int g_src_qx, g_src_qy, g_src_qw, g_src_qh;
static int g_nv12 = 0;					 /* 0 = YUYV, 1 = NV12 */
static int g_mjpeg = 0;
static int g_mjpeg_quality = 60;
static int g_max_frame_bytes;
static unsigned int g_frame_seq;
static int g_jpeg_drop_log_sec;
static double g_gamma = 2.2;
static int g_tone_highlight_knee;        /* optional gamma-space shoulder threshold */
static int g_tone_highlight_max = 255;   /* optional compressed highlight ceiling */
static int g_blacklevel = 64;			  /* 10-bit black level */
static int g_fps_cap = 30;
static int g_auto_exposure = 0;
static int g_target = 105;
static int g_ae_clip_target;				/* optional percentile cap for bright highlights */
static int g_ae_clip_weight = 50;			/* percent penalty applied when highlights exceed cap */
static int g_awb = 1;					  /* gray-world AWB */
static int g_verbose = 0;
static int g_motion_overlay = 0;		  /* moving test graphics for live UVC checks */
static int g_motion_overlay_size = 96;
static unsigned int g_motion_overlay_seq;

/* ---- raw capture state ---- */
static int g_rawfd = -1;
static int g_raw_w, g_raw_h, g_raw_stride, g_raw_sizeimage;
struct rbuf { void *start; size_t len; };
static struct rbuf g_rb[NBUF];
static int g_nrb;

/* ---- output sink state ---- */
static int g_outfd = -1;				   /* loopback OR fifo OR dump fd */
static int g_out_stride, g_out_size;
static int g_pipe_cap = 1 << 20;
static int g_mjpeg_pipe_short_log_sec;
static int g_fifo_backpressure_log_sec;
static unsigned int g_fifo_backpressure_drops;

/* ---- ISP buffers ---- */
static uint8_t *g_raw_copy;				  /* packed RAW10 work copy for early QBUF */
static uint16_t *g_bayer;				  /* raw_w * raw_h, 10-bit values */
static uint8_t *g_gamma_lut;			   /* 1024 -> 8-bit */
static uint8_t *g_frame;				   /* output YUYV/NV12 or RGB before MJPEG */
static uint16_t *g_luma10;				 /* optional pre-gamma luma for anti-bloom */
static uint8_t *g_filter;				  /* optional filtered RGB before MJPEG */
static uint8_t *g_filter2;				 /* second RGB filter stage when needed */
static uint8_t *g_sharp;				   /* optional sharpened RGB before MJPEG */
static uint32_t *g_hi_r, *g_hi_g, *g_hi_b, *g_hi_mask; /* fast highlight-local box filter */
static int g_hi_w, g_hi_h, g_hi_stride;
static uint8_t *g_jpeg;
static int g_jpeg_size;
static int g_sharpen;					   /* MJPEG unsharp amount, percent */
static int g_mjpeg_smooth;				  /* MJPEG post-downscale smoothing amount, percent */
static int g_mjpeg_highlight_smooth;		/* MJPEG local smoothing around bright high-contrast edges */
static int g_mjpeg_highlight_threshold = 150;
static int g_mjpeg_highlight_radius = 2;
static int g_mjpeg_desaturate_highlights;	/* reduce chroma fringing around clipped highlights */
static int g_mjpeg_antibloom;				/* gray out pixels bordering strongly clipped source highlights */
static int g_mjpeg_antibloom_threshold = 930;
static int g_mjpeg_antibloom_radius = 3;
static int g_mjpeg_edge_despeckle;			/* blend Bayer-fringe spikes into the local halo */
static int g_mjpeg_edge_despeckle_threshold = 24;
static int g_mjpeg_edge_despeckle_radius = 2;
static int g_mjpeg_area_scale = 100;		 /* percent of source footprint used for MJPEG area downscale */
static int g_mjpeg_subsampling = 420;		/* JPEG chroma sampling: 420 or 444 */
static int g_mjpeg_bayer_despeckle;		   /* blend the area sample toward the Bayer-quad center */
static int g_mjpeg_blend_frac;				 /* blend Bayer area with fractional sampling near highlights */
static int g_mjpeg_blend_threshold = 180;
static int g_mjpeg_highlight_area;			 /* blend high-contrast highlights toward the Bayer footprint area */
static int g_mjpeg_highlight_area_threshold = 140;
static int g_mjpeg_highlight_area_delta = 18;
static int g_mjpeg_highlight_box;			  /* blend high-contrast highlights toward a Bayer-quad box */
static int g_mjpeg_highlight_box_threshold = 140;
static int g_mjpeg_highlight_box_delta = 18;
static int g_mjpeg_highlight_box_area = 300;
enum mjpeg_scale_mode {
	MJPEG_SCALE_BAYER_AREA = 0,
	MJPEG_SCALE_BAYER_AREA_FRAC,
	MJPEG_SCALE_BAYER_AREA_FRAC_4TAP,
	MJPEG_SCALE_BAYER_AREA_BOX,
	MJPEG_SCALE_BAYER_QUAD_BOX,
	MJPEG_SCALE_BAYER_QUAD4,
	MJPEG_SCALE_BAYER_CENTER,
	MJPEG_SCALE_DEMOSAIC_CENTER,
	MJPEG_SCALE_DEMOSAIC_4TAP,
	MJPEG_SCALE_DEMOSAIC_9TAP,
};
static enum mjpeg_scale_mode g_mjpeg_scale_mode = MJPEG_SCALE_BAYER_AREA;
static uint32_t *g_int_r, *g_int_gr, *g_int_gb, *g_int_b;
static int g_int_qw, g_int_qh, g_int_stride;
static int *g_center_qx, *g_center_qy;
static int g_center_map_src_x, g_center_map_src_y, g_center_map_src_w, g_center_map_src_h;
static int g_center_map_out_w, g_center_map_out_h, g_center_map_area_scale;
static int *g_frac_qx0, *g_frac_qx1, *g_frac_fx;
static int *g_frac_qy0, *g_frac_qy1, *g_frac_fy;
static int g_frac_map_src_x, g_frac_map_src_y, g_frac_map_src_w, g_frac_map_src_h;
static int g_frac_map_out_w, g_frac_map_out_h;
static int g_mjpeg_fast_threads = 4;
static int g_cpu_first = -1, g_cpu_last = -1;
static double g_stage_jpeg_ms;
static int g_stage_direct_mjpeg;

/* ---- AE state ---- */
static int g_exposure = -1, g_again = -1, g_dgain = -1;
static int g_exp_min, g_exp_max, g_gain_min, g_gain_max, g_dgain_min, g_dgain_max;
static int g_ae_last_log;
static int g_ae_last_hi;
static int g_ae_changed;
static double g_ae_filtered_mean = -1.0;
static int g_ae_last_dir;
static int g_ae_dir_count;
static int g_ae_mean_window[8];
static int g_ae_mean_window_count;
static int g_ae_mean_window_pos;
static int g_auto_tone = 1;
static int g_dgain_limit = -1;

enum flicker_mode {
	FLICKER_MODE_OFF = 0,
	FLICKER_MODE_FIXED_50,
	FLICKER_MODE_FIXED_60,
	FLICKER_MODE_AUTO,
};

struct flicker_stats {
	int bands;
	double score_50;
	double score_60;
	double score;
	int candidate_hz;
	int active;
};

static enum flicker_mode g_flicker_mode = FLICKER_MODE_OFF;
static int g_flicker_hz;
static int g_flicker_detected_hz;
static int g_flicker_pending_hz;
static int g_flicker_active;
static double g_flicker_score_50;
static double g_flicker_score_60;
static double g_flicker_score;
static int g_flicker_lock_count;
static int g_flicker_clear_count;
static int g_flicker_last_log_sec;
static int g_timing_ready;
static int g_line_time_ns;
static int g_timing_hblank = -1;
static int g_timing_vblank = -1;
static int64_t g_timing_pixel_rate;
static int g_meter_roi_enabled;
static int g_meter_left, g_meter_top, g_meter_right, g_meter_bottom, g_meter_auto_controls;
static double g_soft_gain = 1.0;
static double g_max_soft_gain = 4.0;

static void ilog(const char *fmt, ...)
{
	va_list ap;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[lmi-isp %ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	fflush(stderr);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
	return r;
}

static double ts_ms(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1e3 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

static double ts_sec(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) + (b->tv_nsec - a->tv_nsec) / 1e9;
}

struct perf_stats {
	unsigned int frames;
	unsigned int poll_timeouts;
	unsigned int dq_eagain;
	unsigned int qbuf_errors;
	unsigned int raw_seq_lost;
	unsigned int fifo_drops;
	unsigned int over_budget;
	unsigned int direct_mjpeg;
	double poll_sum, poll_max;
	double dq_sum, dq_max;
	double hold_sum, hold_max;
	double copy_sum, copy_max;
	double qbuf_sum, qbuf_max;
	double unpack_sum, unpack_max;
	double awb_sum, awb_max;
	double pack_sum, pack_max;
	double jpeg_sum, jpeg_max;
	double out_sum, out_max;
	double ae_sum, ae_max;
	double sleep_sum, sleep_max;
	double work_sum, work_max;
};

static void perf_add(double *sum, double *max, double value)
{
	*sum += value;
	if (value > *max)
		*max = value;
}

static double perf_avg(double sum, unsigned int frames)
{
	return frames ? sum / frames : 0.0;
}

/* ---------------- sensor controls / AE ---------------- */

static int g_ctrlfd = -1;
static int g_controlfd = -1;
static char g_control_line[256];
static unsigned int g_control_line_len;

static void ae_compensate_gain_for_exposure(int before, int after);

static int get_ctrl_range(int fd, unsigned int id, int *vmin, int *vmax, int *cur)
{
	struct v4l2_queryctrl q;
	struct v4l2_control c;
	memset(&q, 0, sizeof(q));
	q.id = id;
	if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0)
		return -1;
	*vmin = q.minimum;
	*vmax = q.maximum;
	memset(&c, 0, sizeof(c));
	c.id = id;
	if (xioctl(fd, VIDIOC_G_CTRL, &c) == 0)
		*cur = c.value;
	else
		*cur = q.default_value;
	return 0;
}

static int get_ctrl_i64(int fd, unsigned int id, int64_t *value)
{
	struct v4l2_ext_controls ctrls;
	struct v4l2_ext_control ctrl;
	struct v4l2_queryctrl q;

	memset(&q, 0, sizeof(q));
	q.id = id;
	if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0)
		return -1;
	memset(&ctrls, 0, sizeof(ctrls));
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.id = id;
	ctrls.count = 1;
	ctrls.controls = &ctrl;
	if (xioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) < 0)
		return -1;
	if (q.type == V4L2_CTRL_TYPE_INTEGER64)
		*value = ctrl.value64;
	else
		*value = ctrl.value;
	return 0;
}

static void controls_update_timing(void)
{
	int minv, maxv, hblank = 0, vblank = -1;
	int64_t pixel_rate;

	if (g_timing_ready || g_ctrlfd < 0 || g_raw_w <= 0)
		return;
	if (get_ctrl_i64(g_ctrlfd, V4L2_CID_PIXEL_RATE, &pixel_rate) < 0 || pixel_rate <= 0)
		return;
	if (get_ctrl_range(g_ctrlfd, V4L2_CID_HBLANK, &minv, &maxv, &hblank) < 0)
		hblank = 0;
	if (get_ctrl_range(g_ctrlfd, V4L2_CID_VBLANK, &minv, &maxv, &vblank) < 0)
		vblank = -1;
	g_line_time_ns = (int)(((int64_t)g_raw_w + hblank) * 1000000000LL / pixel_rate);
	if (g_line_time_ns <= 0)
		return;
	g_timing_ready = 1;
	g_timing_pixel_rate = pixel_rate;
	g_timing_hblank = hblank;
	g_timing_vblank = vblank;
	if (g_verbose || g_auto_exposure || g_flicker_mode != FLICKER_MODE_OFF)
		ilog("controls: timing pixel_rate=%lld hblank=%d vblank=%d line_time_ns=%d",
		     (long long)g_timing_pixel_rate, g_timing_hblank, g_timing_vblank,
		     g_line_time_ns);
}

static int set_ctrl(int fd, unsigned int id, int val)
{
	struct v4l2_control c;
	memset(&c, 0, sizeof(c));
	c.id = id;
	c.value = val;
	return xioctl(fd, VIDIOC_S_CTRL, &c);
}

static int controls_init(void)
{
	if (g_ctrlfd >= 0)
		return 0;
	if (!g_ctrl[0]) {
		ilog("controls: no ctrl subdev configured");
		return -1;
	}
	g_ctrlfd = open(g_ctrl, O_RDWR);
	if (g_ctrlfd < 0) {
		ilog("controls: cannot open ctrl subdev %s: %s", g_ctrl, strerror(errno));
		return -1;
	}
	if (get_ctrl_range(g_ctrlfd, V4L2_CID_EXPOSURE, &g_exp_min, &g_exp_max, &g_exposure) < 0) {
		ilog("controls: EXPOSURE is not available on %s", g_ctrl);
		close(g_ctrlfd);
		g_ctrlfd = -1;
		return -1;
	}
	if (get_ctrl_range(g_ctrlfd, V4L2_CID_ANALOGUE_GAIN, &g_gain_min, &g_gain_max, &g_again) < 0)
		g_again = -1;
	if (get_ctrl_range(g_ctrlfd, V4L2_CID_DIGITAL_GAIN, &g_dgain_min, &g_dgain_max, &g_dgain) < 0)
		g_dgain = -1;
	if (g_dgain >= 0 && g_dgain_limit >= 0) {
		if (g_dgain_limit < g_dgain_min)
			g_dgain_limit = g_dgain_min;
		if (g_dgain_limit > g_dgain_max)
			g_dgain_limit = g_dgain_max;
		if (g_dgain > g_dgain_limit && set_ctrl(g_ctrlfd, V4L2_CID_DIGITAL_GAIN, g_dgain_limit) == 0)
			g_dgain = g_dgain_limit;
	}
	if (g_again < 0) {
		g_gain_min = 0;
		g_gain_max = 0;
	}
	if (g_dgain < 0) {
		g_dgain_min = 0;
		g_dgain_max = 0;
	}
	controls_update_timing();
	ilog("controls: exposure[%d..%d]=%d gain[%d..%d]=%d digital[%d..%d]=%d ae=%d target=%d clip=%d/%d flicker_mode=%d hz=%d line_ns=%d digital_limit=%d soft_limit=%.1f",
		 g_exp_min, g_exp_max, g_exposure, g_gain_min, g_gain_max, g_again,
		 g_dgain_min, g_dgain_max, g_dgain, g_auto_exposure, g_target, g_ae_clip_target,
		 g_ae_clip_weight, g_flicker_mode, g_flicker_hz, g_line_time_ns,
		 g_dgain >= 0 && g_dgain_limit >= 0 ? g_dgain_limit : g_dgain_max,
		 g_max_soft_gain);
	return 0;
}

static void ae_init(void)
{
	if (g_auto_exposure && controls_init() < 0)
		g_auto_exposure = 0;
}

static int ae_dgain_max(void)
{
	if (g_dgain < 0)
		return -1;
	if (g_dgain_limit >= 0 && g_dgain_limit < g_dgain_max)
		return g_dgain_limit;
	return g_dgain_max;
}

static int clamp_i(int value, int vmin, int vmax)
{
	if (value < vmin)
		return vmin;
	if (value > vmax)
		return vmax;
	return value;
}

static int apply_exposure(int value)
{
	if (g_ctrlfd < 0 && controls_init() < 0)
		return -1;
	value = clamp_i(value, g_exp_min, g_exp_max);
	if (set_ctrl(g_ctrlfd, V4L2_CID_EXPOSURE, value) < 0) {
		ilog("control: exposure=%d failed: %s", value, strerror(errno));
		return -1;
	}
	g_exposure = value;
	g_ae_changed = 1;
	return 0;
}

static int apply_analogue_gain(int value)
{
	if (g_again < 0)
		return -1;
	value = clamp_i(value, g_gain_min, g_gain_max);
	if (set_ctrl(g_ctrlfd, V4L2_CID_ANALOGUE_GAIN, value) < 0) {
		ilog("control: analogue_gain=%d failed: %s", value, strerror(errno));
		return -1;
	}
	g_again = value;
	g_ae_changed = 1;
	return 0;
}

static int apply_digital_gain(int value)
{
	int dgain_max = ae_dgain_max();
	if (g_dgain < 0 || dgain_max < 0)
		return -1;
	value = clamp_i(value, g_dgain_min, dgain_max);
	if (set_ctrl(g_ctrlfd, V4L2_CID_DIGITAL_GAIN, value) < 0) {
		ilog("control: digital_gain=%d failed: %s", value, strerror(errno));
		return -1;
	}
	g_dgain = value;
	g_ae_changed = 1;
	return 0;
}

static const char *flicker_mode_name(void)
{
	switch (g_flicker_mode) {
	case FLICKER_MODE_FIXED_50: return "50";
	case FLICKER_MODE_FIXED_60: return "60";
	case FLICKER_MODE_AUTO: return "auto";
	case FLICKER_MODE_OFF:
	default: return "off";
	}
}

static int flicker_target_hz(void)
{
	if (g_flicker_mode == FLICKER_MODE_FIXED_50)
		return 50;
	if (g_flicker_mode == FLICKER_MODE_FIXED_60)
		return 60;
	if (g_flicker_mode == FLICKER_MODE_AUTO && g_flicker_active &&
	    (g_flicker_detected_hz == 50 || g_flicker_detected_hz == 60))
		return g_flicker_detected_hz;
	return 0;
}

static int flicker_exposure_quantum_for_hz(int hz)
{
	int fps, frame_units_100us, half_cycle_100us, range;
	if (hz != 50 && hz != 60)
		return 0;
	range = g_exp_max - g_exp_min;
	if (range < 2)
		return 0;
	if (g_ctrlfd >= 0)
		controls_update_timing();
	if (g_timing_ready && g_line_time_ns > 0) {
		int64_t half_ns = hz == 50 ? 10000000LL : 8333333LL;
		int lines = (int)((half_ns + g_line_time_ns / 2) / g_line_time_ns);
		/* Some high-FPS modes cannot fit a full mains half-cycle into one exposure;
		 * do not quantize those modes down to the minimum shutter. */
		if (lines >= range)
			return 0;
		return clamp_i(lines, 1, range);
	}
	fps = g_fps_cap > 0 ? g_fps_cap : 30;
	frame_units_100us = 10000 / fps;
	if (frame_units_100us < 1)
		frame_units_100us = 1;
	/* Mains flicker repeats every half-cycle: 10ms for 50Hz, 8.33ms for 60Hz. */
	half_cycle_100us = hz == 50 ? 100 : 83;
	{
		int quantum = (int)(((int64_t)range * half_cycle_100us + frame_units_100us / 2) /
					      frame_units_100us);
		if (quantum >= range)
			return 0;
		return clamp_i(quantum, 1, range);
	}
}

static int flicker_exposure_quantum(void)
{
	return flicker_exposure_quantum_for_hz(flicker_target_hz());
}

static int flicker_quantize_exposure_for_hz(int value, int direction, int hz)
{
	int offset, quantum = flicker_exposure_quantum_for_hz(hz);
	if (quantum <= 0)
		return value;
	value = clamp_i(value, g_exp_min, g_exp_max);
	offset = value - g_exp_min;
	if (direction > 0)
		value = g_exp_min + ((offset + quantum - 1) / quantum) * quantum;
	else if (direction < 0)
		value = g_exp_min + (offset / quantum) * quantum;
	else
		value = g_exp_min + ((offset + quantum / 2) / quantum) * quantum;
	return clamp_i(value, g_exp_min, g_exp_max);
}

static int flicker_alignment_tolerance(int quantum)
{
	int tolerance = quantum / 64;
	if (tolerance < 4)
		tolerance = 4;
	if (tolerance > 24)
		tolerance = 24;
	return tolerance;
}

static int flicker_preferred_exposure_for_hz(int value, int hz)
{
	int offset, quantum = flicker_exposure_quantum_for_hz(hz);
	int up, down, max_offset;

	if (quantum <= 0)
		return clamp_i(value, g_exp_min, g_exp_max);
	value = clamp_i(value, g_exp_min, g_exp_max);
	offset = value - g_exp_min;
	up = g_exp_min + ((offset + quantum - 1) / quantum) * quantum;
	if (up <= g_exp_max)
		return up;
	if (up - g_exp_max <= flicker_alignment_tolerance(quantum))
		return g_exp_max;
	max_offset = g_exp_max - g_exp_min;
	down = g_exp_min + (max_offset / quantum) * quantum;
	return clamp_i(down, g_exp_min, g_exp_max);
}

static int flicker_quantize_exposure_dir(int value, int direction)
{
	return flicker_quantize_exposure_for_hz(value, direction, flicker_target_hz());
}

static int flicker_quantize_exposure(int value)
{
	return flicker_quantize_exposure_dir(value, 0);
}

static int exposure_absolute_to_lines(int value_100us)
{
	int fps = g_fps_cap > 0 ? g_fps_cap : 30;
	int frame_units = 10000 / fps; /* 100us units per nominal frame */
	int lines;
	if (value_100us < 1)
		value_100us = 1;
	if (g_ctrlfd >= 0)
		controls_update_timing();
	if (g_timing_ready && g_line_time_ns > 0) {
		lines = (int)(((int64_t)value_100us * 100000LL + g_line_time_ns / 2) /
			      g_line_time_ns);
		return flicker_quantize_exposure(clamp_i(lines, g_exp_min, g_exp_max));
	}
	if (frame_units < 1)
		frame_units = 1;
	lines = g_exp_min + (int)(((int64_t)value_100us * (g_exp_max - g_exp_min)) / frame_units);
	return flicker_quantize_exposure(clamp_i(lines, g_exp_min, g_exp_max));
}

static int uvc_gain_to_sensor_gain(int value)
{
	if (g_again < 0)
		return -1;
	value = clamp_i(value, 0, 255);
	return g_gain_min + (int)(((int64_t)value * (g_gain_max - g_gain_min)) / 255);
}

static int meter_roi_active(void)
{
	return g_meter_roi_enabled && g_meter_right >= g_meter_left &&
		g_meter_bottom >= g_meter_top;
}

static int roi_norm_floor_to_pixel(int value, int pixels)
{
	value = clamp_i(value, 0, LMI_UVC_ROI_MAX_COORD);
	if (pixels <= 1)
		return 0;
	return (int)(((int64_t)value * pixels) / LMI_UVC_ROI_COORD_DEN);
}

static int roi_norm_ceil_to_pixel_exclusive(int value, int pixels)
{
	value = clamp_i(value, 0, LMI_UVC_ROI_MAX_COORD);
	if (pixels <= 1)
		return pixels;
	return (int)((((int64_t)value + 1) * pixels + LMI_UVC_ROI_COORD_DEN - 1) /
		     LMI_UVC_ROI_COORD_DEN);
}

static void meter_roi_bounds(int *left, int *top, int *right, int *bottom)
{
	int l = 0, t = 0, r = g_raw_w, b = g_raw_h;

	if (meter_roi_active()) {
		l = roi_norm_floor_to_pixel(g_meter_left, g_raw_w);
		t = roi_norm_floor_to_pixel(g_meter_top, g_raw_h);
		r = roi_norm_ceil_to_pixel_exclusive(g_meter_right, g_raw_w);
		b = roi_norm_ceil_to_pixel_exclusive(g_meter_bottom, g_raw_h);
	}
	l = clamp_i(l, 0, g_raw_w > 1 ? g_raw_w - 2 : 0) & ~1;
	t = clamp_i(t, 0, g_raw_h > 1 ? g_raw_h - 2 : 0) & ~1;
	r = clamp_i(r, l + 2, g_raw_w);
	b = clamp_i(b, t + 2, g_raw_h);
	*left = l;
	*top = t;
	*right = r;
	*bottom = b;
}

static double flicker_line_time_ns_for_detection(void)
{
	if (g_ctrlfd >= 0)
		controls_update_timing();
	if (g_timing_ready && g_line_time_ns > 0)
		return (double)g_line_time_ns;
	if (g_fps_cap > 0 && g_raw_h > 0)
		return 1000000000.0 / ((double)g_fps_cap * g_raw_h);
	return 0.0;
}

static double flicker_score_for_hz(const double *residual, int bands, int top,
				   int height, int hz, double line_ns, double energy)
{
	const double two_pi = 6.28318530717958647692;
	double half_ns, cs = 0.0, sn = 0.0, wn = 0.0;
	int i;

	if ((hz != 50 && hz != 60) || bands <= 1 || height <= 0 || line_ns <= 0.0 || energy <= 0.0)
		return 0.0;
	half_ns = hz == 50 ? 10000000.0 : 8333333.333333333;
	for (i = 0; i < bands; i++) {
		double y = top + ((i + 0.5) * height) / bands;
		double phase = two_pi * y * line_ns / half_ns;
		double c = cos(phase), s = sin(phase);
		cs += residual[i] * c;
		sn += residual[i] * s;
		wn += c * c + s * s;
	}
	if (wn <= 0.0)
		return 0.0;
	return sqrt((cs * cs + sn * sn) / (energy * wn));
}

static struct flicker_stats flicker_analyze_frame(void)
{
	struct flicker_stats st;
	double means[LMI_FLICKER_BANDS], residual[LMI_FLICKER_BANDS];
	double sum = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
	double mean_level, slope = 0.0, intercept = 0.0, energy = 0.0, rms;
	double line_ns;
	int left, top, right, bottom, height, width, bands, i;
	int use_full_frame = 0;

	memset(&st, 0, sizeof(st));
	meter_roi_bounds(&left, &top, &right, &bottom);
	height = bottom - top;
	width = right - left;
	if (height < LMI_FLICKER_MIN_BANDS * 4 || width < 64)
		use_full_frame = 1;
	if (use_full_frame) {
		left = 0;
		top = 0;
		right = g_raw_w;
		bottom = g_raw_h;
		height = bottom - top;
		width = right - left;
	}
	if (height < LMI_FLICKER_MIN_BANDS * 2 || width < 16)
		return st;
	bands = LMI_FLICKER_BANDS;
	if (height / 2 < bands)
		bands = height / 2;
	if (bands < LMI_FLICKER_MIN_BANDS)
		return st;
	if (bands > LMI_FLICKER_BANDS)
		bands = LMI_FLICKER_BANDS;
	for (i = 0; i < bands; i++) {
		uint64_t acc = 0, cnt = 0;
		int y0 = top + (height * i) / bands;
		int y1 = top + (height * (i + 1)) / bands;
		int row_step = (y1 - y0) / 4;
		int col_step = width / 96;
		int y, x;

		if (row_step < 2)
			row_step = 2;
		row_step &= ~1;
		if (row_step < 2)
			row_step = 2;
		if (col_step < 8)
			col_step = 8;
		if (col_step > 64)
			col_step = 64;
		col_step &= ~1;
		y0 &= ~1;
		y1 &= ~1;
		if (y1 <= y0)
			y1 = y0 + 2;
		if (y1 > bottom - 1)
			y1 = bottom - 1;
		for (y = y0; y + 1 < y1; y += row_step) {
			for (x = left & ~1; x + 1 < right; x += col_step) {
				acc += g_bayer[(size_t)y * g_raw_w + x];
				acc += g_bayer[(size_t)(y + 1) * g_raw_w + x + 1];
				cnt += 2;
			}
		}
		means[i] = cnt ? (double)acc / cnt : 0.0;
		sum += means[i];
	}
	if (bands <= 1)
		return st;
	mean_level = sum / bands - g_blacklevel;
	for (i = 0; i < bands; i++) {
		double x = i - (bands - 1) * 0.5;
		double y = means[i];
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
	}
	if (sxx > 0.0)
		slope = (sxy - sx * sy / bands) / (sxx - sx * sx / bands);
	intercept = sy / bands - slope * sx / bands;
	for (i = 0; i < bands; i++) {
		double x = i - (bands - 1) * 0.5;
		residual[i] = means[i] - (intercept + slope * x);
		energy += residual[i] * residual[i];
	}
	if (energy <= 0.0)
		return st;
	rms = sqrt(energy / bands);
	line_ns = flicker_line_time_ns_for_detection();
	st.bands = bands;
	st.score_50 = flicker_score_for_hz(residual, bands, top, height, 50, line_ns, energy);
	st.score_60 = flicker_score_for_hz(residual, bands, top, height, 60, line_ns, energy);
	st.score = st.score_50 >= st.score_60 ? st.score_50 : st.score_60;
	st.candidate_hz = st.score_50 >= st.score_60 ? 50 : 60;
	st.active = st.score >= 0.25 && rms >= 2.0 &&
		(mean_level <= 1.0 || rms / mean_level >= 0.004);
	return st;
}

static void flicker_update_state(const struct flicker_stats *st)
{
	struct timespec ts;
	int old_active = g_flicker_active;
	int old_hz = g_flicker_hz;
	int quantum;

	if (g_flicker_mode == FLICKER_MODE_OFF || !st)
		return;
	g_flicker_score_50 = st->score_50;
	g_flicker_score_60 = st->score_60;
	g_flicker_score = st->score;
	if (g_flicker_mode == FLICKER_MODE_AUTO) {
		if (st->active && (st->candidate_hz == 50 || st->candidate_hz == 60)) {
			int candidate_hz = st->candidate_hz;
			double best = st->score_50 >= st->score_60 ? st->score_50 : st->score_60;
			double diff = st->score_50 >= st->score_60 ? st->score_50 - st->score_60 : st->score_60 - st->score_50;

			/* The short row sweep gives close 50/60 scores on many LED scenes.  If the
			 * scores are not decisive, keep the previous lock or prefer the local
			 * 50Hz-safe shutter step; a clearly stronger 60Hz score still wins. */
			if (best > 0.0 && diff / best <= 0.12) {
				if (g_flicker_detected_hz == 50 || g_flicker_detected_hz == 60)
					candidate_hz = g_flicker_detected_hz;
				else if (g_flicker_pending_hz == 50 || g_flicker_pending_hz == 60)
					candidate_hz = g_flicker_pending_hz;
				else
					candidate_hz = 50;
			}
			if (g_flicker_pending_hz == candidate_hz)
				g_flicker_lock_count++;
			else {
				g_flicker_pending_hz = candidate_hz;
				g_flicker_lock_count = 1;
			}
			g_flicker_clear_count = 0;
			if (!g_flicker_active || g_flicker_detected_hz == candidate_hz ||
			    g_flicker_lock_count >= 3) {
				g_flicker_detected_hz = candidate_hz;
				g_flicker_active = 1;
			}
		} else {
			g_flicker_lock_count = 0;
			g_flicker_pending_hz = 0;
			if (++g_flicker_clear_count >= 4) {
				g_flicker_active = 0;
				g_flicker_detected_hz = 0;
			}
		}
	} else {
		g_flicker_detected_hz = st->active ? st->candidate_hz : 0;
		g_flicker_active = st->active;
	}
	g_flicker_hz = flicker_target_hz();
	quantum = flicker_exposure_quantum();
	clock_gettime(CLOCK_MONOTONIC, &ts);
	if (old_active != g_flicker_active || old_hz != g_flicker_hz ||
	    (int)ts.tv_sec - g_flicker_last_log_sec >= 2) {
		g_flicker_last_log_sec = (int)ts.tv_sec;
		ilog("flicker: mode=%s hz=%d detected=%d active=%d score=%.3f s50=%.3f s60=%.3f bands=%d quantum=%d source=%s exposure=%d again=%d dgain=%d",
		     flicker_mode_name(), g_flicker_hz, g_flicker_detected_hz,
		     g_flicker_active, g_flicker_score, g_flicker_score_50,
		     g_flicker_score_60, st->bands, quantum,
		     g_timing_ready ? "timing" : "fallback", g_exposure, g_again, g_dgain);
	}
}

static void ae_reset_history(void);

static void control_fifo_open(void)
{
	if (!g_control_fifo[0] || g_controlfd >= 0)
		return;
	g_controlfd = open(g_control_fifo, O_RDWR | O_NONBLOCK);
	if (g_controlfd < 0)
		ilog("control fifo: cannot open %s: %s", g_control_fifo, strerror(errno));
	else
		ilog("control fifo: listening on %s", g_control_fifo);
}

static void apply_control_line(char *line)
{
	char *key, *value, *end;
	long parsed;

	while (*line && isspace((unsigned char)*line))
		line++;
	if (!*line || *line == '#')
		return;
	end = line + strlen(line);
	while (end > line && isspace((unsigned char)end[-1]))
		*--end = '\0';
	value = strchr(line, '=');
	if (!value) {
		ilog("control fifo: ignored malformed command '%s'", line);
		return;
	}
	*value++ = '\0';
	key = line;
	if (!strcmp(key, "auto_exposure")) {
		parsed = strtol(value, NULL, 0);
		if (parsed) {
			if (controls_init() == 0)
				g_auto_exposure = 1;
		} else {
			g_auto_exposure = 0;
		}
		ilog("control: auto_exposure=%d", g_auto_exposure);
		return;
	}
	if (!strcmp(key, "flicker")) {
		int quantum, have_controls = 0;
		if (!strcmp(value, "50")) {
			g_flicker_mode = FLICKER_MODE_FIXED_50;
			g_flicker_active = 1;
			g_flicker_detected_hz = 50;
		} else if (!strcmp(value, "60")) {
			g_flicker_mode = FLICKER_MODE_FIXED_60;
			g_flicker_active = 1;
			g_flicker_detected_hz = 60;
		} else if (!strcmp(value, "auto")) {
			g_flicker_mode = FLICKER_MODE_AUTO;
			g_flicker_active = 0;
			g_flicker_detected_hz = 0;
		} else {
			g_flicker_mode = FLICKER_MODE_OFF;
			g_flicker_active = 0;
			g_flicker_detected_hz = 0;
		}
		g_flicker_hz = flicker_target_hz();
		g_flicker_pending_hz = 0;
		g_flicker_lock_count = 0;
		g_flicker_clear_count = 0;
		if (g_flicker_mode != FLICKER_MODE_OFF)
			have_controls = controls_init() == 0;
		quantum = flicker_exposure_quantum();
		if (have_controls && quantum > 0 && g_exposure >= 0) {
			int before = g_exposure;
			int snapped = flicker_preferred_exposure_for_hz(g_exposure, g_flicker_hz);
			if (snapped != before && apply_exposure(snapped) == 0) {
				ae_compensate_gain_for_exposure(before, g_exposure);
				ilog("control: flicker=%s mode=%s hz=%d quantum=%d exposure=%d->%d source=%s",
				     value, flicker_mode_name(), g_flicker_hz, quantum, before,
				     g_exposure, g_timing_ready ? "timing" : "fallback");
				return;
			}
			ilog("control: flicker=%s mode=%s hz=%d quantum=%d exposure=%d source=%s",
			     value, flicker_mode_name(), g_flicker_hz, quantum, g_exposure,
			     g_timing_ready ? "timing" : "fallback");
		} else {
			ilog("control: flicker=%s mode=%s hz=%d quantum=%d source=%s", value,
			     flicker_mode_name(), g_flicker_hz, quantum,
			     g_timing_ready ? "timing" : "fallback");
		}
		return;
	}
	if (!strcmp(key, "meter_roi")) {
		int top, left, bottom, right, auto_controls;
		if (sscanf(value, "%d,%d,%d,%d,%d", &top, &left, &bottom, &right,
			   &auto_controls) != 5) {
			ilog("control fifo: ignored malformed meter_roi='%s'", value);
			return;
		}
		top = clamp_i(top, 0, LMI_UVC_ROI_MAX_COORD);
		left = clamp_i(left, 0, LMI_UVC_ROI_MAX_COORD);
		bottom = clamp_i(bottom, 0, LMI_UVC_ROI_MAX_COORD);
		right = clamp_i(right, 0, LMI_UVC_ROI_MAX_COORD);
		auto_controls &= 0x00ff;
		g_meter_top = top;
		g_meter_left = left;
		g_meter_bottom = bottom;
		g_meter_right = right;
		g_meter_auto_controls = auto_controls;
		if (right < left || bottom < top)
			g_meter_roi_enabled = 0;
		else
			g_meter_roi_enabled = auto_controls & 1;
		ae_reset_history();
		if (g_meter_roi_enabled && controls_init() == 0)
			g_auto_exposure = 1;
		ilog("control: meter_roi=%d,%d,%d,%d auto=0x%x enabled=%d",
		     g_meter_top, g_meter_left, g_meter_bottom, g_meter_right,
		     g_meter_auto_controls, g_meter_roi_enabled);
		return;
	}
	if (controls_init() < 0) {
		ilog("control fifo: cannot apply '%s=%s' without sensor controls", key, value);
		return;
	}
	parsed = strtol(value, NULL, 0);
	if (!strcmp(key, "exposure_absolute")) {
		g_auto_exposure = 0;
		if (apply_exposure(exposure_absolute_to_lines((int)parsed)) == 0)
			ilog("control: exposure_absolute=%ld -> exposure=%d", parsed, g_exposure);
	} else if (!strcmp(key, "exposure")) {
		g_auto_exposure = 0;
		if (apply_exposure(flicker_quantize_exposure((int)parsed)) == 0)
			ilog("control: exposure=%d", g_exposure);
	} else if (!strcmp(key, "analogue_gain")) {
		g_auto_exposure = 0;
		if (apply_analogue_gain((int)parsed) == 0)
			ilog("control: analogue_gain=%d", g_again);
	} else if (!strcmp(key, "digital_gain")) {
		g_auto_exposure = 0;
		if (apply_digital_gain((int)parsed) == 0)
			ilog("control: digital_gain=%d", g_dgain);
	} else if (!strcmp(key, "gain")) {
		int again = uvc_gain_to_sensor_gain((int)parsed);
		g_auto_exposure = 0;
		if (again >= 0 && apply_analogue_gain(again) == 0)
			ilog("control: gain=%ld -> analogue_gain=%d", parsed, g_again);
	} else {
		ilog("control fifo: ignored unknown command '%s=%s'", key, value);
	}
}

static void control_fifo_drain(void)
{
	char buf[256];
	ssize_t n;
	unsigned int i;

	if (!g_control_fifo[0])
		return;
	control_fifo_open();
	if (g_controlfd < 0)
		return;
	while ((n = read(g_controlfd, buf, sizeof(buf))) > 0) {
		for (i = 0; i < (unsigned int)n; i++) {
			char c = buf[i];
			if (c == '\n' || c == '\r') {
				g_control_line[g_control_line_len] = '\0';
				apply_control_line(g_control_line);
				g_control_line_len = 0;
			} else if (g_control_line_len + 1 < sizeof(g_control_line)) {
				g_control_line[g_control_line_len++] = c;
			} else {
				g_control_line_len = 0;
				ilog("control fifo: dropped overlong command");
			}
		}
	}
	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		ilog("control fifo: read failed: %s", strerror(errno));
}

static int raw_percentile_luma(int step, int pct);

static int ae_scale_down_control(int value, int vmin, int vmax, double ratio)
{
	if (value < vmin)
		return value;
	if (ratio < 0.05)
		ratio = 0.05;
	if (ratio > 1.0)
		ratio = 1.0;
	return clamp_i(vmin + (int)((value - vmin) * ratio + 0.5), vmin, vmax);
}

static int ae_try_reduce_digital(int step)
{
	if (g_dgain < 0 || g_dgain <= g_dgain_min)
		return 0;
	if (step < 1)
		step = 1;
	return apply_digital_gain(g_dgain - step) == 0;
}

static int ae_try_reduce_analogue(int step)
{
	if (g_again < 0 || g_again <= g_gain_min)
		return 0;
	if (step < 1)
		step = 1;
	return apply_analogue_gain(g_again - step) == 0;
}

static int ae_analogue_soft_max(void)
{
	int cap;
	if (g_again < 0)
		return -1;
	cap = g_gain_min + ((g_gain_max - g_gain_min) * 3) / 5;
	return clamp_i(cap, g_gain_min, g_gain_max);
}

static int ae_scale_up_control(int value, int vmin, int vmax, double ratio)
{
	if (value < vmin)
		return value;
	if (ratio < 1.0)
		ratio = 1.0;
	if (ratio > 4.0)
		ratio = 4.0;
	return clamp_i(vmin + (int)((value - vmin) * ratio + 0.5), vmin, vmax);
}

static void ae_compensate_gain_for_exposure(int before, int after)
{
	double ratio;
	int target, soft_max;

	if (before <= 0 || after <= 0 || before == after)
		return;
	if (after > before) {
		ratio = (double)before / after;
		if (g_dgain >= 0 && g_dgain > g_dgain_min) {
			target = ae_scale_down_control(g_dgain, g_dgain_min, ae_dgain_max(), ratio);
			if (target < g_dgain)
				apply_digital_gain(target);
		}
		if (g_again >= 0 && g_again > g_gain_min) {
			target = ae_scale_down_control(g_again, g_gain_min, g_gain_max, ratio);
			if (target < g_again)
				apply_analogue_gain(target);
		}
		return;
	}

	/* If anti-flicker shortens the shutter, lift analogue gain only up to the
	 * soft ISO ceiling.  Digital gain remains the last resort to avoid noisy
	 * oscillation when a small ROI lands on a lamp. */
	ratio = (double)before / after;
	soft_max = ae_analogue_soft_max();
	if (g_again >= 0 && soft_max > g_again) {
		target = ae_scale_up_control(g_again, g_gain_min, soft_max, ratio);
		if (target > g_again)
			apply_analogue_gain(target);
	}
}

static void ae_reset_history(void)
{
	g_ae_filtered_mean = -1.0;
	g_ae_last_dir = 0;
	g_ae_dir_count = 0;
	g_ae_mean_window_count = 0;
	g_ae_mean_window_pos = 0;
}

static int ae_mean_window_average(void)
{
	int i, sum = 0;
	if (g_ae_mean_window_count <= 0)
		return -1;
	for (i = 0; i < g_ae_mean_window_count; i++)
		sum += g_ae_mean_window[i];
	return (sum + g_ae_mean_window_count / 2) / g_ae_mean_window_count;
}

static int ae_mean_window_range(void)
{
	int i, minv = 255, maxv = 0;
	if (g_ae_mean_window_count <= 1)
		return 0;
	for (i = 0; i < g_ae_mean_window_count; i++) {
		if (g_ae_mean_window[i] < minv)
			minv = g_ae_mean_window[i];
		if (g_ae_mean_window[i] > maxv)
			maxv = g_ae_mean_window[i];
	}
	return maxv - minv;
}

static void ae_mean_window_push(int mean)
{
	g_ae_mean_window[g_ae_mean_window_pos] = mean;
	g_ae_mean_window_pos = (g_ae_mean_window_pos + 1) %
		(int)(sizeof(g_ae_mean_window) / sizeof(g_ae_mean_window[0]));
	if (g_ae_mean_window_count < (int)(sizeof(g_ae_mean_window) / sizeof(g_ae_mean_window[0])))
		g_ae_mean_window_count++;
}

static int ae_direction_ready(int err, int deadband)
{
	int dir;
	if (err > deadband)
		dir = 1;
	else if (err < -deadband)
		dir = -1;
	else {
		g_ae_last_dir = 0;
		g_ae_dir_count = 0;
		return 0;
	}
	if (g_ae_last_dir == dir)
		g_ae_dir_count++;
	else {
		g_ae_last_dir = dir;
		g_ae_dir_count = 1;
	}
	if (abs(err) >= (meter_roi_active() ? 45 : 38) ||
	    g_ae_dir_count >= (meter_roi_active() ? 2 : 1))
		return dir;
	return 0;
}

/* AE damps ROI changes, uses analogue gain only as a small ISO-like trim,
 * and lets shutter/exposure participate early while power-line flicker control is
 * active.  Anti-flicker exposure steps are coarse, so gain is still used to trim
 * around the aligned shutter instead of hunting across several quanta. */
static void ae_update(int mean)
{
	int err, dir, deadband, exp_step, again_step, dgain_step, dgain_max, quantum;
	int target_exposure, requested_exposure, again_soft_max;
	int flicker_hz, abs_err, window_avg, window_range;
	double alpha;
	if (!g_auto_exposure || g_ctrlfd < 0)
		return;
	flicker_hz = flicker_target_hz();
	quantum = flicker_exposure_quantum_for_hz(flicker_hz);
	g_ae_last_hi = 0;
	if (g_ae_clip_target > 0) {
		int hi = raw_percentile_luma(16, 995);
		g_ae_last_hi = hi;
		if (hi > g_ae_clip_target) {
			mean += ((hi - g_ae_clip_target) * g_ae_clip_weight + 50) / 100;
			if (mean > 255)
				mean = 255;
		}
	}
	if (g_ae_filtered_mean < 0.0)
		g_ae_filtered_mean = mean;
	else {
		alpha = meter_roi_active() ? 0.26 : 0.34;
		if (abs(mean - (int)(g_ae_filtered_mean + 0.5)) > 70)
			alpha = meter_roi_active() ? 0.40 : 0.50;
		g_ae_filtered_mean = g_ae_filtered_mean * (1.0 - alpha) + mean * alpha;
	}
	mean = clamp_i((int)(g_ae_filtered_mean + 0.5), 0, 255);
	ae_mean_window_push(mean);
	window_avg = ae_mean_window_average();
	window_range = ae_mean_window_range();
	if (meter_roi_active() && flicker_hz && quantum <= 0 && window_avg >= 0 &&
	    g_ae_mean_window_count >= 4 && window_range > 34)
		mean = window_avg;
	g_ae_last_log = mean;
	err = g_target - mean;
	deadband = meter_roi_active() ? 14 : 9;
	if (meter_roi_active() && flicker_hz && quantum <= 0 && window_range > 28)
		deadband += 8;
	dir = ae_direction_ready(err, deadband);
	if (!dir)
		return;

	dgain_max = ae_dgain_max();
	again_soft_max = ae_analogue_soft_max();
	abs_err = abs(err);
	exp_step = (g_exp_max - g_exp_min) / 36;
	if (exp_step < 1)
		exp_step = 1;
	if (abs_err > 60)
		exp_step *= meter_roi_active() ? 2 : 3;
	else if (abs_err > 42 && !meter_roi_active())
		exp_step *= 2;
	else if (abs_err < 22 && exp_step > 1)
		exp_step = (exp_step + 1) / 2;
	again_step = (g_gain_max - g_gain_min) / 34 + 1;
	if (abs_err > 60)
		again_step *= meter_roi_active() ? 2 : 3;
	else if (abs_err > 42 && !meter_roi_active())
		again_step *= 2;
	else if (abs_err < 22 && again_step > 1)
		again_step = (again_step + 1) / 2;
	dgain_step = (g_dgain_max - g_dgain_min) / 40 + 1;

	if (flicker_hz && quantum > 0 && g_exposure >= 0) {
		int preferred = flicker_preferred_exposure_for_hz(g_exposure, flicker_hz);
		int tolerance = flicker_alignment_tolerance(quantum);
		int can_compensate = (g_again > g_gain_min) || (g_dgain > g_dgain_min) || err > 0;
		int not_aligned = abs(g_exposure - preferred) > tolerance;
		if (preferred != g_exposure && not_aligned &&
		    (err > 8 || (g_flicker_active && can_compensate)) &&
		    (err > -40 || can_compensate)) {
			int before = g_exposure;
			if (apply_exposure(preferred) == 0) {
				ae_compensate_gain_for_exposure(before, g_exposure);
				return;
			}
		}
	}

	if (dir > 0) {
		int tiny_iso_trim = err < (meter_roi_active() ? 18 : 22);
		int exposure_can_rise;
		target_exposure = g_exposure + exp_step;
		requested_exposure = target_exposure;
		if (flicker_hz && quantum > 0)
			target_exposure = flicker_preferred_exposure_for_hz(target_exposure, flicker_hz);
		exposure_can_rise = g_exposure < g_exp_max && target_exposure > g_exposure;
		if (g_again >= 0 && again_soft_max >= 0 && g_again < again_soft_max &&
		    ((!flicker_hz && tiny_iso_trim) || !exposure_can_rise)) {
			if (apply_analogue_gain(g_again + again_step) == 0)
				return;
		}
		if (exposure_can_rise && apply_exposure(target_exposure) == 0) {
			if (target_exposure > requested_exposure)
				ae_compensate_gain_for_exposure(requested_exposure, target_exposure);
			return;
		}
		if (g_again >= 0 && g_again < g_gain_max && err > 42 &&
		    apply_analogue_gain(g_again + again_step) == 0)
			return;
		if (g_dgain >= 0 && dgain_max >= 0 && g_dgain < dgain_max && err > 70)
			apply_digital_gain(g_dgain + dgain_step);
	} else {
		int exposure_should_fall = g_exposure > g_exp_min &&
			(err < -38 || g_ae_dir_count >= (meter_roi_active() ? 2 : 3));
		if (meter_roi_active() && flicker_hz && quantum <= 0 && window_range > 28 &&
		    err > -80 && g_exposure < g_exp_max)
			exposure_should_fall = 0;
		if (ae_try_reduce_digital(dgain_step))
			return;
		if (!exposure_should_fall && ae_try_reduce_analogue(again_step))
			return;
		if (exposure_should_fall) {
			target_exposure = g_exposure - exp_step;
			if (err < -80)
				target_exposure -= exp_step;
			if (flicker_hz && quantum > 0) {
				int before_quantize = target_exposure;
				target_exposure = flicker_quantize_exposure_for_hz(target_exposure, -1, flicker_hz);
				if (target_exposure >= g_exposure) {
					int next = g_exposure - (quantum > exp_step ? quantum : exp_step);
					target_exposure = flicker_quantize_exposure_for_hz(next, -1, flicker_hz);
				}
				if (target_exposure <= g_exp_min && before_quantize > g_exp_min)
					target_exposure = g_exp_min;
			}
			if (target_exposure < g_exposure && apply_exposure(target_exposure) == 0)
				return;
		}
		if (ae_try_reduce_analogue(again_step))
			return;
	}
}


static void tone_update(int mean)
{
	double target, desired;
	if (!g_auto_exposure || !g_auto_tone)
		return;
	if (g_max_soft_gain < 1.0)
		g_max_soft_gain = 1.0;
	if (mean < 1)
		mean = 1;
	target = g_target / 255.0;
	if (target < 0.02)
		target = 0.02;
	if (target > 0.90)
		target = 0.90;
	/* Convert the desired post-gamma luma back to the pre-gamma raw-meter scale.
	 * Sensor exposure/gains still run first; this software gain only lifts the
	 * userspace ISP output when the sensor controls are already insufficient. */
	desired = pow(target, g_gamma > 0.01 ? g_gamma : 1.0) * 255.0 / mean;
	if (desired < 1.0)
		desired = 1.0;
	if (desired > g_max_soft_gain)
		desired = g_max_soft_gain;
	g_soft_gain = g_soft_gain * 0.65 + desired * 0.35;
	if (g_soft_gain < 1.0)
		g_soft_gain = 1.0;
	if (g_soft_gain > g_max_soft_gain)
		g_soft_gain = g_max_soft_gain;
}

/* ---------------- RAW10 unpack ---------------- */

/* MIPI RAW10 packed: 4 pixels per 5 bytes. b0..b3 = high 8 bits of p0..p3,
 * b4 = low 2 bits packed (p0 in bits1:0, p1 in 3:2, p2 in 5:4, p3 in 7:6). */
static void unpack_raw10p(const uint8_t *src)
{
	int y;
	for (y = 0; y < g_raw_h; y++) {
		const uint8_t *s = src + (size_t)y * g_raw_stride;
		uint16_t *d = g_bayer + (size_t)y * g_raw_w;
		int x = 0;
		int groups = g_raw_w / 4;
		int g;
		for (g = 0; g < groups; g++) {
			uint8_t b0 = s[0], b1 = s[1], b2 = s[2], b3 = s[3], lo = s[4];
			d[0] = (uint16_t)((b0 << 2) | (lo & 3));
			d[1] = (uint16_t)((b1 << 2) | ((lo >> 2) & 3));
			d[2] = (uint16_t)((b2 << 2) | ((lo >> 4) & 3));
			d[3] = (uint16_t)((b3 << 2) | ((lo >> 6) & 3));
			s += 5;
			d += 4;
			x += 4;
		}
		for (; x < g_raw_w; x++) { /* tail (width not multiple of 4) */
			d[0] = (uint16_t)(s[0] << 2);
			s++;
			d++;
		}
	}
}

static inline int bclamp(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* fetch bayer with edge clamp */
static inline int BRAW(int x, int y)
{
	if (x < 0) x = 0; else if (x >= g_raw_w) x = g_raw_w - 1;
	if (y < 0) y = 0; else if (y >= g_raw_h) y = g_raw_h - 1;
	return g_bayer[(size_t)y * g_raw_w + x];
}

static inline int B(int x, int y)
{
	return BRAW(x, y);
}

static int raw_percentile_luma(int step, int pct)
{
	uint32_t hist[1024];
	uint32_t total = 0, want, acc = 0;
	int x, y, i, left, top, right, bottom;

	if (step < 2)
		step = 2;
	if (pct < 1)
		pct = 1;
	if (pct > 1000)
		pct = 1000;
	meter_roi_bounds(&left, &top, &right, &bottom);
	memset(hist, 0, sizeof(hist));
	for (y = top; y + 1 < bottom; y += step) {
		for (x = left; x + 1 < right; x += step) {
			int gr = bclamp(BRAW(x, y) - g_blacklevel, 0, 1023);
			int r = bclamp(BRAW(x + 1, y) - g_blacklevel, 0, 1023);
			int b = bclamp(BRAW(x, y + 1) - g_blacklevel, 0, 1023);
			int gb = bclamp(BRAW(x + 1, y + 1) - g_blacklevel, 0, 1023);
			int g = (gr + gb) >> 1;
			int l = (77 * r + 150 * g + 29 * b) >> 8;
			hist[bclamp(l, 0, 1023)]++;
			total++;
		}
	}
	if (!total)
		return 0;
	want = (uint32_t)(((uint64_t)total * pct + 999) / 1000);
	if (want < 1)
		want = 1;
	for (i = 0; i < 1024; i++) {
		acc += hist[i];
		if (acc >= want)
			return i;
	}
	return 1023;
}

/* ---------------- AWB (gray-world on a strided subsample) ---------------- */

static void compute_awb(double *rgain, double *bgain)
{
	uint64_t rs = 0, gs = 0, bs = 0, rn = 0, gn = 0, bn = 0;
	int y, x;
	int bl = g_blacklevel;
	/* SGRBG: (even,even)=Gr (odd,even)=R (even,odd)=B (odd,odd)=Gb.
	 * Sample one complete 2x2 Bayer quad from each sparse block. */
	for (y = 0; y + 1 < g_raw_h; y += 8) {
		for (x = 0; x + 1 < g_raw_w; x += 8) {
			int gr = g_bayer[(size_t)y * g_raw_w + x] - bl;
			int r = g_bayer[(size_t)y * g_raw_w + x + 1] - bl;
			int b = g_bayer[(size_t)(y + 1) * g_raw_w + x] - bl;
			int gb = g_bayer[(size_t)(y + 1) * g_raw_w + x + 1] - bl;
			if (gr < 0) gr = 0;
			if (r < 0) r = 0;
			if (b < 0) b = 0;
			if (gb < 0) gb = 0;
			gs += gr + gb;
			gn += 2;
			rs += r;
			rn++;
			bs += b;
			bn++;
		}
	}
	double rm = rn ? (double)rs / rn : 1, gm = gn ? (double)gs / gn : 1, bm = bn ? (double)bs / bn : 1;
	if (rm < 1)
		rm = 1;
	if (bm < 1)
		bm = 1;
	double rg = gm / rm, bg = gm / bm;
	if (rg < 0.5)
		rg = 0.5;
	if (rg > 4.0)
		rg = 4.0;
	if (bg < 0.5)
		bg = 0.5;
	if (bg > 4.0)
		bg = 4.0;
	*rgain = rg;
	*bgain = bg;
}

/* ---------------- demosaic + WB + gamma + scale + YUYV/NV12/MJPEG pack ----------------
 * To avoid a full-res RGB buffer at large sensor modes, we demosaic on the fly.
 * For MJPEG downscale we build summed-area tables over Bayer quads, then average
 * each output pixel's source footprint in O(1) and apply the gamma LUT once.  This
 * keeps large source modes practical while avoiding the old single-site Bayer
 * sample that made downscaled MJPEG look softer than the requested output size. */

static inline void demosaic_linear_at(int bx, int by, double rgain, double bgain,
					  int *R, int *Gout, int *Bout)
{
	int bl = g_blacklevel;
	int cx = bx & 1, cy = by & 1;
	int r, g, b;
	if (cy == 0 && cx == 1) {			/* R site */
		r = B(bx, by);
		g = (B(bx - 1, by) + B(bx + 1, by) + B(bx, by - 1) + B(bx, by + 1)) >> 2;
		b = (B(bx - 1, by - 1) + B(bx + 1, by - 1) + B(bx - 1, by + 1) + B(bx + 1, by + 1)) >> 2;
	} else if (cy == 1 && cx == 0) {	 /* B site */
		b = B(bx, by);
		g = (B(bx - 1, by) + B(bx + 1, by) + B(bx, by - 1) + B(bx, by + 1)) >> 2;
		r = (B(bx - 1, by - 1) + B(bx + 1, by - 1) + B(bx - 1, by + 1) + B(bx + 1, by + 1)) >> 2;
	} else if (cy == 0 && cx == 0) {	 /* Gr site: R horizontal, B vertical */
		g = B(bx, by);
		r = (B(bx - 1, by) + B(bx + 1, by)) >> 1;
		b = (B(bx, by - 1) + B(bx, by + 1)) >> 1;
	} else {							 /* Gb site: R vertical, B horizontal */
		g = B(bx, by);
		r = (B(bx, by - 1) + B(bx, by + 1)) >> 1;
		b = (B(bx - 1, by) + B(bx + 1, by)) >> 1;
	}
	r -= bl; g -= bl; b -= bl;
	if (r < 0)
		r = 0;
	if (g < 0)
		g = 0;
	if (b < 0)
		b = 0;
	r = (int)(r * rgain * g_soft_gain);
	g = (int)(g * g_soft_gain);
	b = (int)(b * bgain * g_soft_gain);
	if (r > 1023)
		r = 1023;
	if (b > 1023)
		b = 1023;
	if (g > 1023)
		g = 1023;
	*R = r; *Gout = g; *Bout = b;
}

static inline void demosaic_at(int bx, int by, double rgain, double bgain,
				   int *R, int *Gout, int *Bout)
{
	demosaic_linear_at(bx, by, rgain, bgain, R, Gout, Bout);
	*R = g_gamma_lut[*R]; *Gout = g_gamma_lut[*Gout]; *Bout = g_gamma_lut[*Bout];
}

static inline void source_span_scaled(int out_i, int out_n, int src_n, int scale_percent,
					  int *first, int *last)
{
	if (src_n > out_n) {
		long a = (long)out_i * src_n;
		long b = (long)(out_i + 1) * src_n;
		int lo = (int)((a + out_n - 1) / out_n);
		int hi = (int)((b + out_n - 1) / out_n) - 1;
		if (lo < 0)
			lo = 0;
		if (hi >= src_n)
			hi = src_n - 1;
		if (hi < lo)
			hi = lo;
		if (scale_percent > 0 && scale_percent != 100 && hi > lo) {
			int span = hi - lo + 1;
			int keep = (span * scale_percent + 99) / 100;
			if (keep < 1)
				keep = 1;
			if (keep > src_n)
				keep = src_n;
			if (keep < span) {
				int drop = span - keep;
				lo += drop / 2;
				hi -= drop - drop / 2;
			} else if (keep > span) {
				int add = keep - span;
				lo -= add / 2;
				hi += add - add / 2;
				if (lo < 0) {
					hi -= lo;
					lo = 0;
				}
				if (hi >= src_n) {
					lo -= hi - src_n + 1;
					hi = src_n - 1;
					if (lo < 0)
						lo = 0;
				}
			}
		}
		*first = lo;
		*last = hi;
		return;
	}
	/* Upscale or 1:1 fallback: sample nearest source coordinate. */
	*first = *last = bclamp((int)(((long)(out_i * 2 + 1) * src_n) / (out_n * 2)), 0, src_n - 1);
}

static inline int source_coord_scaled(int out_i, int out_n, int src_n)
{
	return bclamp((int)(((long)(out_i * 2 + 1) * src_n) / (out_n * 2)), 0, src_n - 1);
}

static inline int source_view_coord_scaled(int out_i, int out_n, int src_n, int offset)
{
	return offset + source_coord_scaled(out_i, out_n, src_n);
}

static void source_view_compute(void)
{
	long raw_aspect, out_aspect;
	int x = 0, y = 0, w = g_raw_w, h = g_raw_h;

	if (w < 2 || h < 2) {
		g_src_x = g_src_y = g_src_qx = g_src_qy = 0;
		g_src_w = w;
		g_src_h = h;
		g_src_qw = w / 2;
		g_src_qh = h / 2;
		return;
	}
	w &= ~1;
	h &= ~1;
	if (g_source_aspect == SOURCE_ASPECT_PRESERVE && g_out_w > 0 && g_out_h > 0) {
		raw_aspect = (long)w * g_out_h;
		out_aspect = (long)g_out_w * h;
		if (raw_aspect > out_aspect) {
			int nw = (int)(((long)h * g_out_w) / g_out_h);
			nw &= ~1;
			if (nw >= 2 && nw < w) {
				x = ((w - nw) / 2) & ~1;
				w = nw;
			}
		} else if (raw_aspect < out_aspect) {
			int nh = (int)(((long)w * g_out_h) / g_out_w);
			nh &= ~1;
			if (nh >= 2 && nh < h) {
				y = ((h - nh) / 2) & ~1;
				h = nh;
			}
		}
	}
	if (x + w > g_raw_w)
		w = (g_raw_w - x) & ~1;
	if (y + h > g_raw_h)
		h = (g_raw_h - y) & ~1;
	if (w < 2) {
		x = 0;
		w = g_raw_w & ~1;
	}
	if (h < 2) {
		y = 0;
		h = g_raw_h & ~1;
	}
	g_src_x = x;
	g_src_y = y;
	g_src_w = w;
	g_src_h = h;
	g_src_qx = g_src_x / 2;
	g_src_qy = g_src_y / 2;
	g_src_qw = g_src_w / 2;
	g_src_qh = g_src_h / 2;
}

static inline void source_view_span_scaled(int out_i, int out_n, int src_n, int offset,
						       int scale_percent, int *first, int *last)
{
	source_span_scaled(out_i, out_n, src_n, scale_percent, first, last);
	*first += offset;
	*last += offset;
}

static const char *source_aspect_name(void)
{
	return g_source_aspect == SOURCE_ASPECT_PRESERVE ? "preserve" : "stretch";
}


static int ensure_integral_tables(void)
{
	int qw = g_raw_w / 2;
	int qh = g_raw_h / 2;
	size_t n;

	if (qw < 1 || qh < 1)
		return -1;
	if (g_int_r && g_int_qw == qw && g_int_qh == qh)
		return 0;

	free(g_int_r);
	free(g_int_gr);
	free(g_int_gb);
	free(g_int_b);
	g_int_r = g_int_gr = g_int_gb = g_int_b = NULL;
	g_int_qw = qw;
	g_int_qh = qh;
	g_int_stride = qw + 1;
	n = (size_t)(qw + 1) * (qh + 1);
	g_int_r = calloc(n, sizeof(*g_int_r));
	g_int_gr = calloc(n, sizeof(*g_int_gr));
	g_int_gb = calloc(n, sizeof(*g_int_gb));
	g_int_b = calloc(n, sizeof(*g_int_b));
	return (g_int_r && g_int_gr && g_int_gb && g_int_b) ? 0 : -1;
}

static void build_integral_tables(void)
{
	int qx, qy;
	int bl = g_blacklevel;

	for (qy = 0; qy < g_int_qh; qy++) {
		uint32_t rr = 0, grr = 0, gbr = 0, br = 0;
		uint32_t *dr = g_int_r + (size_t)(qy + 1) * g_int_stride;
		uint32_t *dgr = g_int_gr + (size_t)(qy + 1) * g_int_stride;
		uint32_t *dgb = g_int_gb + (size_t)(qy + 1) * g_int_stride;
		uint32_t *db = g_int_b + (size_t)(qy + 1) * g_int_stride;
		uint32_t *pr = g_int_r + (size_t)qy * g_int_stride;
		uint32_t *pgr = g_int_gr + (size_t)qy * g_int_stride;
		uint32_t *pgb = g_int_gb + (size_t)qy * g_int_stride;
		uint32_t *pb = g_int_b + (size_t)qy * g_int_stride;

		dr[0] = dgr[0] = dgb[0] = db[0] = 0;
		for (qx = 0; qx < g_int_qw; qx++) {
			int x = qx * 2;
			int y = qy * 2;
			int grv = B(x, y) - bl;
			int rv = B(x + 1, y) - bl;
			int bv = B(x, y + 1) - bl;
			int gbv = B(x + 1, y + 1) - bl;
			if (grv < 0)
				grv = 0;
			if (rv < 0)
				rv = 0;
			if (bv < 0)
				bv = 0;
			if (gbv < 0)
				gbv = 0;
			rr += (uint32_t)rv;
			grr += (uint32_t)grv;
			gbr += (uint32_t)gbv;
			br += (uint32_t)bv;
			dr[qx + 1] = pr[qx + 1] + rr;
			dgr[qx + 1] = pgr[qx + 1] + grr;
			dgb[qx + 1] = pgb[qx + 1] + gbr;
			db[qx + 1] = pb[qx + 1] + br;
		}
	}
}

static inline uint32_t int_sum(const uint32_t *tab, int qx0, int qy0, int qx1, int qy1)
{
	const uint32_t *a = tab + (size_t)qy0 * g_int_stride;
	const uint32_t *b = tab + (size_t)qy1 * g_int_stride;
	return b[qx1] - b[qx0] - a[qx1] + a[qx0];
}

static inline void quad_sample(int qx, int qy, double rgain, double bgain,
				   int *R, int *Gout, int *Bout)
{
	int x = qx * 2;
	int y = qy * 2;
	if (qx < 0 || qx >= g_int_qw || qy < 0 || qy >= g_int_qh) {
		*R = *Gout = *Bout = 0;
		return;
	}
	*R = (int)(bclamp(B(x + 1, y) - g_blacklevel, 0, 1023) * rgain * g_soft_gain);
	*Gout = (int)(((bclamp(B(x, y) - g_blacklevel, 0, 1023) +
			  bclamp(B(x + 1, y + 1) - g_blacklevel, 0, 1023)) >> 1) * g_soft_gain);
	*Bout = (int)(bclamp(B(x, y + 1) - g_blacklevel, 0, 1023) * bgain * g_soft_gain);
}

static inline uint16_t luma10_clamped(int R, int Gout, int Bout)
{
	if (R > 1023)
		R = 1023;
	if (Gout > 1023)
		Gout = 1023;
	if (Bout > 1023)
		Bout = 1023;
	if (R < 0)
		R = 0;
	if (Gout < 0)
		Gout = 0;
	if (Bout < 0)
		Bout = 0;
	return (uint16_t)((77 * R + 150 * Gout + 29 * Bout) >> 8);
}

static inline int tone_map8(int v)
{
	if (v < 0)
		v = 0;
	if (v > 1023)
		v = 1023;
	v = g_gamma_lut[v];
	if (g_tone_highlight_knee > 0 && g_tone_highlight_max > g_tone_highlight_knee &&
		v > g_tone_highlight_knee) {
		int span = 255 - g_tone_highlight_knee;
		int outspan = g_tone_highlight_max - g_tone_highlight_knee;
		int d = v - g_tone_highlight_knee;
		v = g_tone_highlight_knee + (d * outspan + span / 2) / span;
		if (v > g_tone_highlight_max)
			v = g_tone_highlight_max;
	}
	return bclamp(v, 0, 255);
}

static inline void gamma_clamp_rgb(int *R, int *Gout, int *Bout)
{
	*R = tone_map8(*R);
	*Gout = tone_map8(*Gout);
	*Bout = tone_map8(*Bout);
}

static inline void bayer_area_rgb(int tx, int ty, int ow, int oh,
				  double rgain, double bgain,
				  int *R, int *Gout, int *Bout, uint16_t *luma10)
{
	int sx0, sx1, sy0, sy1;
	int qx0, qx1, qy0, qy1;
	uint32_t rs, grs, gbs, bs;
	int qn;

	source_view_span_scaled(tx, ow, g_src_w, g_src_x, g_mjpeg_area_scale, &sx0, &sx1);
	source_view_span_scaled(ty, oh, g_src_h, g_src_y, g_mjpeg_area_scale, &sy0, &sy1);

	/* Work in complete 2x2 Bayer quads: Gr/R on the first row, B/Gb on the
	 * second.  The summed-area table keeps high-source downscale sharp without a
	 * per-pixel loop over the whole footprint. */
	qx0 = sx0 / 2;
	qx1 = sx1 / 2 + 1;
	qy0 = sy0 / 2;
	qy1 = sy1 / 2 + 1;
	if (qx0 < 0)
		qx0 = 0;
	if (qy0 < 0)
		qy0 = 0;
	if (qx1 > g_int_qw)
		qx1 = g_int_qw;
	if (qy1 > g_int_qh)
		qy1 = g_int_qh;
	if (qx1 <= qx0)
		qx1 = qx0 + 1;
	if (qy1 <= qy0)
		qy1 = qy0 + 1;
	if (qx1 > g_int_qw)
		qx1 = g_int_qw;
	if (qy1 > g_int_qh)
		qy1 = g_int_qh;

	rs = int_sum(g_int_r, qx0, qy0, qx1, qy1);
	grs = int_sum(g_int_gr, qx0, qy0, qx1, qy1);
	gbs = int_sum(g_int_gb, qx0, qy0, qx1, qy1);
	bs = int_sum(g_int_b, qx0, qy0, qx1, qy1);
	qn = (qx1 - qx0) * (qy1 - qy0);
	if (qn < 1)
		qn = 1;

	*R = (int)(((double)rs / qn) * rgain * g_soft_gain);
	*Gout = (int)(((double)((uint64_t)grs + gbs) / (qn * 2)) * g_soft_gain);
	*Bout = (int)(((double)bs / qn) * bgain * g_soft_gain);
	if (g_mjpeg_bayer_despeckle > 0 && qn > 1) {
		int ds = g_mjpeg_bayer_despeckle > 100 ? 100 : g_mjpeg_bayer_despeckle;
		int cx = (qx0 + qx1) >> 1, cy = (qy0 + qy1) >> 1;
		int cr, cg, cb;
		if (cx >= g_int_qw)
			cx = g_int_qw - 1;
		if (cy >= g_int_qh)
			cy = g_int_qh - 1;
		cr = (int)(bclamp(B(cx * 2 + 1, cy * 2) - g_blacklevel, 0, 1023) * rgain * g_soft_gain);
		cg = (int)(((bclamp(B(cx * 2, cy * 2) - g_blacklevel, 0, 1023) +
				 bclamp(B(cx * 2 + 1, cy * 2 + 1) - g_blacklevel, 0, 1023)) >> 1) * g_soft_gain);
		cb = (int)(bclamp(B(cx * 2, cy * 2 + 1) - g_blacklevel, 0, 1023) * bgain * g_soft_gain);
		*R = (*R * (100 - ds) + cr * ds + 50) / 100;
		*Gout = (*Gout * (100 - ds) + cg * ds + 50) / 100;
		*Bout = (*Bout * (100 - ds) + cb * ds + 50) / 100;
	}
	if (luma10)
		*luma10 = luma10_clamped(*R, *Gout, *Bout);
	gamma_clamp_rgb(R, Gout, Bout);
}

static inline void bayer_center_rgb(int tx, int ty, int ow, int oh,
						double rgain, double bgain,
						int *R, int *Gout, int *Bout, uint16_t *luma10)
{
	int sx0, sx1, sy0, sy1;
	int qx, qy;

	source_view_span_scaled(tx, ow, g_src_w, g_src_x, g_mjpeg_area_scale, &sx0, &sx1);
	source_view_span_scaled(ty, oh, g_src_h, g_src_y, g_mjpeg_area_scale, &sy0, &sy1);
	qx = ((sx0 + sx1) >> 1) / 2;
	qy = ((sy0 + sy1) >> 1) / 2;
	quad_sample(qx, qy, rgain, bgain, R, Gout, Bout);
	if (luma10)
		*luma10 = luma10_clamped(*R, *Gout, *Bout);
	gamma_clamp_rgb(R, Gout, Bout);
}

static int ensure_center_maps(int ow, int oh)
{
	int i;

	if (g_center_qx && g_center_qy &&
		g_center_map_src_x == g_src_x && g_center_map_src_y == g_src_y &&
		g_center_map_src_w == g_src_w && g_center_map_src_h == g_src_h &&
		g_center_map_out_w == ow && g_center_map_out_h == oh &&
		g_center_map_area_scale == g_mjpeg_area_scale)
		return 0;

	free(g_center_qx);
	free(g_center_qy);
	g_center_qx = malloc((size_t)ow * sizeof(*g_center_qx));
	g_center_qy = malloc((size_t)oh * sizeof(*g_center_qy));
	if (!g_center_qx || !g_center_qy)
		return -1;

	for (i = 0; i < ow; i++) {
		int sx0, sx1, qx;
		source_view_span_scaled(i, ow, g_src_w, g_src_x, g_mjpeg_area_scale, &sx0, &sx1);
		qx = ((sx0 + sx1) >> 1) / 2;
		g_center_qx[i] = bclamp(qx, 0, g_int_qw - 1);
	}
	for (i = 0; i < oh; i++) {
		int sy0, sy1, qy;
		source_view_span_scaled(i, oh, g_src_h, g_src_y, g_mjpeg_area_scale, &sy0, &sy1);
		qy = ((sy0 + sy1) >> 1) / 2;
		g_center_qy[i] = bclamp(qy, 0, g_int_qh - 1);
	}
	g_center_map_src_x = g_src_x;
	g_center_map_src_y = g_src_y;
	g_center_map_src_w = g_src_w;
	g_center_map_src_h = g_src_h;
	g_center_map_out_w = ow;
	g_center_map_out_h = oh;
	g_center_map_area_scale = g_mjpeg_area_scale;
	return 0;
}

static int ensure_frac_maps(int ow, int oh)
{
	int i;

	if (g_frac_qx0 && g_frac_qx1 && g_frac_fx &&
		g_frac_qy0 && g_frac_qy1 && g_frac_fy &&
		g_frac_map_src_x == g_src_x && g_frac_map_src_y == g_src_y &&
		g_frac_map_src_w == g_src_w && g_frac_map_src_h == g_src_h &&
		g_frac_map_out_w == ow && g_frac_map_out_h == oh)
		return 0;

	free(g_frac_qx0);
	free(g_frac_qx1);
	free(g_frac_fx);
	free(g_frac_qy0);
	free(g_frac_qy1);
	free(g_frac_fy);
	g_frac_qx0 = g_frac_qx1 = g_frac_fx = NULL;
	g_frac_qy0 = g_frac_qy1 = g_frac_fy = NULL;

	g_frac_qx0 = malloc((size_t)ow * sizeof(*g_frac_qx0));
	g_frac_qx1 = malloc((size_t)ow * sizeof(*g_frac_qx1));
	g_frac_fx = malloc((size_t)ow * sizeof(*g_frac_fx));
	g_frac_qy0 = malloc((size_t)oh * sizeof(*g_frac_qy0));
	g_frac_qy1 = malloc((size_t)oh * sizeof(*g_frac_qy1));
	g_frac_fy = malloc((size_t)oh * sizeof(*g_frac_fy));
	if (!g_frac_qx0 || !g_frac_qx1 || !g_frac_fx ||
		!g_frac_qy0 || !g_frac_qy1 || !g_frac_fy)
		return -1;

	for (i = 0; i < ow; i++) {
		long sx_fp = ((long)(i * 2 + 1) * g_src_qw * 128) / ow + (long)g_src_qx * 256;
		int qx = (int)(sx_fp >> 8);
		int fx = (int)(sx_fp & 255);
		int qx1;

		if (qx < g_src_qx) {
			qx = g_src_qx;
			fx = 0;
		}
		if (qx >= g_src_qx + g_src_qw) {
			qx = g_src_qx + g_src_qw - 1;
			fx = 0;
		}
		qx1 = qx + 1;
		if (qx1 >= g_src_qx + g_src_qw)
			qx1 = qx;
		g_frac_qx0[i] = bclamp(qx, 0, g_int_qw - 1);
		g_frac_qx1[i] = bclamp(qx1, 0, g_int_qw - 1);
		g_frac_fx[i] = fx;
	}
	for (i = 0; i < oh; i++) {
		long sy_fp = ((long)(i * 2 + 1) * g_src_qh * 128) / oh + (long)g_src_qy * 256;
		int qy = (int)(sy_fp >> 8);
		int fy = (int)(sy_fp & 255);
		int qy1;

		if (qy < g_src_qy) {
			qy = g_src_qy;
			fy = 0;
		}
		if (qy >= g_src_qy + g_src_qh) {
			qy = g_src_qy + g_src_qh - 1;
			fy = 0;
		}
		qy1 = qy + 1;
		if (qy1 >= g_src_qy + g_src_qh)
			qy1 = qy;
		g_frac_qy0[i] = bclamp(qy, 0, g_int_qh - 1);
		g_frac_qy1[i] = bclamp(qy1, 0, g_int_qh - 1);
		g_frac_fy[i] = fy;
	}
	g_frac_map_src_x = g_src_x;
	g_frac_map_src_y = g_src_y;
	g_frac_map_src_w = g_src_w;
	g_frac_map_src_h = g_src_h;
	g_frac_map_out_w = ow;
	g_frac_map_out_h = oh;
	return 0;
}

static inline void quad_sample_linear_q(int qx, int qy, int rgain_q, int ggain_q,
					int bgain_q, int *R, int *Gout, int *Bout)
{
	const uint16_t *p;
	int gr, r, b, gb, g;

	if (qx < 0 || qx >= g_int_qw || qy < 0 || qy >= g_int_qh) {
		*R = *Gout = *Bout = 0;
		return;
	}
	p = g_bayer + (size_t)(qy * 2) * g_raw_w + qx * 2;
	gr = bclamp(p[0] - g_blacklevel, 0, 1023);
	r = bclamp(p[1] - g_blacklevel, 0, 1023);
	b = bclamp(p[g_raw_w] - g_blacklevel, 0, 1023);
	gb = bclamp(p[g_raw_w + 1] - g_blacklevel, 0, 1023);
	g = (gr + gb) >> 1;
	*R = (r * rgain_q + 128) >> 8;
	*Gout = (g * ggain_q + 128) >> 8;
	*Bout = (b * bgain_q + 128) >> 8;
}

static inline void bayer_area_frac_ycbcr_fast(int x, int y, int rgain_q,
					      int ggain_q, int bgain_q,
					      int *Y, int *Cb, int *Cr)
{
	int qx0 = g_frac_qx0[x], qx1 = g_frac_qx1[x], fx = g_frac_fx[x];
	int qy0 = g_frac_qy0[y], qy1 = g_frac_qy1[y], fy = g_frac_fy[y];
	int r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
	int w00, w10, w01, w11;
	int r, g, b;

	quad_sample_linear_q(qx0, qy0, rgain_q, ggain_q, bgain_q, &r00, &g00, &b00);
	quad_sample_linear_q(qx1, qy0, rgain_q, ggain_q, bgain_q, &r10, &g10, &b10);
	quad_sample_linear_q(qx0, qy1, rgain_q, ggain_q, bgain_q, &r01, &g01, &b01);
	quad_sample_linear_q(qx1, qy1, rgain_q, ggain_q, bgain_q, &r11, &g11, &b11);
	w00 = (256 - fx) * (256 - fy);
	w10 = fx * (256 - fy);
	w01 = (256 - fx) * fy;
	w11 = fx * fy;
	r = (r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11 + 32768) >> 16;
	g = (g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11 + 32768) >> 16;
	b = (b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11 + 32768) >> 16;
	r = tone_map8(r);
	g = tone_map8(g);
	b = tone_map8(b);
	*Y = lmi_jpeg_clamp_u8((77 * r + 150 * g + 29 * b) >> 8);
	*Cb = lmi_jpeg_clamp_u8(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
	*Cr = lmi_jpeg_clamp_u8(((128 * r - 107 * g - 21 * b) >> 8) + 128);
}

static uint8_t g_mjpeg_rlut[1024], g_mjpeg_glut[1024], g_mjpeg_blut[1024];

static void make_mjpeg_gain_lut(uint8_t lut[1024], int gain_q)
{
	for (int i = 0; i < 1024; i++) {
		int v = (i * gain_q + 128) >> 8;

		if (v > 1023)
			v = 1023;
		lut[i] = (uint8_t)tone_map8(v);
	}
}

static inline void bayer_center_ycbcr_fast(int qx, int qy, int rgain_q,
                                  int ggain_q, int bgain_q,
                                  int *Y, int *Cb, int *Cr)
{
	const uint16_t *p = g_bayer + (size_t)(qy * 2) * g_raw_w + qx * 2;
	int gr = bclamp(p[0] - g_blacklevel, 0, 1023);
	int r = bclamp(p[1] - g_blacklevel, 0, 1023);
	int b = bclamp(p[g_raw_w] - g_blacklevel, 0, 1023);
	int gb = bclamp(p[g_raw_w + 1] - g_blacklevel, 0, 1023);
	int g = (gr + gb) >> 1;

	(void)rgain_q;
	(void)ggain_q;
	(void)bgain_q;
	r = g_mjpeg_rlut[r];
	g = g_mjpeg_glut[g];
	b = g_mjpeg_blut[b];
	*Y = lmi_jpeg_clamp_u8((77 * r + 150 * g + 29 * b) >> 8);
	*Cb = lmi_jpeg_clamp_u8(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
	*Cr = lmi_jpeg_clamp_u8(((128 * r - 107 * g - 21 * b) >> 8) + 128);
}
struct lmi_mjpeg_chunk {
	uint8_t *buf;
	size_t cap;
	int size;
	long lumasum;
	int mcu_y0, mcu_y1;
	int width, height, quality;
	int scale_mode;
	int rgain_q, ggain_q, bgain_q;
};

static int lmi_mjpeg_encode_bayer_center420(uint8_t *dst, size_t cap,
						    int width, int height, int quality,
						    double rgain, double bgain, long *mean_out);

static int lmi_mjpeg_encode_bayer_center420_range(struct lmi_mjpeg_chunk *c)
{
	struct lmi_jpeg_writer w;
	struct lmi_jpeg_huff hdc_y, hac_y, hdc_c, hac_c;
	uint8_t qy[64], qc[64];
	float invqy[64], invqc[64];
	int pred_y = 0, pred_cb = 0, pred_cr = 0;
	long lumasum = 0;
	int my;

	memset(&w, 0, sizeof(w));
	w.buf = c->buf;
	w.cap = c->cap;
	lmi_jpeg_make_qtable(qy, lmi_jpeg_q_luma_base, c->quality);
	lmi_jpeg_make_qtable(qc, lmi_jpeg_q_chroma_base, c->quality);
	lmi_jpeg_make_inv_qtable_f(invqy, qy);
	lmi_jpeg_make_inv_qtable_f(invqc, qc);
	lmi_jpeg_build_huff(&hdc_y, lmi_jpeg_bits_dc_luma, lmi_jpeg_vals_dc_luma);
	lmi_jpeg_build_huff(&hac_y, lmi_jpeg_bits_ac_luma, lmi_jpeg_vals_ac_luma);
	lmi_jpeg_build_huff(&hdc_c, lmi_jpeg_bits_dc_chroma, lmi_jpeg_vals_dc_chroma);
	lmi_jpeg_build_huff(&hac_c, lmi_jpeg_bits_ac_chroma, lmi_jpeg_vals_ac_chroma);

	for (my = c->mcu_y0 * 16; my < c->mcu_y1 * 16; my += 16) {
		for (int mx = 0; mx < c->width; mx += 16) {
			float yblk[4][64];
			float cbblk[64];
			float crblk[64];
			int sum_cb[64];
			int sum_cr[64];

			memset(sum_cb, 0, sizeof(sum_cb));
			memset(sum_cr, 0, sizeof(sum_cr));
			for (int yy = 0; yy < 16; yy++) {
				int oy = my + yy;
				int cy = yy >> 1;
				int map_y = oy < c->height ? oy : c->height - 1;
				for (int xx = 0; xx < 16; xx++) {
					int ox = mx + xx;
					int map_x = ox < c->width ? ox : c->width - 1;
					int yv, cb, cr;
					int bi = (yy >= 8 ? 2 : 0) + (xx >= 8 ? 1 : 0);
					int ci = cy * 8 + (xx >> 1);

					if (c->scale_mode == MJPEG_SCALE_BAYER_AREA_FRAC) {
						bayer_area_frac_ycbcr_fast(map_x, map_y, c->rgain_q,
									      c->ggain_q, c->bgain_q,
									      &yv, &cb, &cr);
					} else {
						int qx = g_center_qx[map_x];
						int qy0 = g_center_qy[map_y];
						bayer_center_ycbcr_fast(qx, qy0, c->rgain_q,
								       c->ggain_q, c->bgain_q,
								       &yv, &cb, &cr);
					}
					yblk[bi][(yy & 7) * 8 + (xx & 7)] = (float)yv - 128.0f;
					sum_cb[ci] += cb;
					sum_cr[ci] += cr;
					if (ox < c->width && oy < c->height)
						lumasum += yv;
				}
			}
			for (int i = 0; i < 64; i++) {
				cbblk[i] = (float)((sum_cb[i] + 2) >> 2) - 128.0f;
				crblk[i] = (float)((sum_cr[i] + 2) >> 2) - 128.0f;
			}
			lmi_jpeg_encode_block_scaled_f(&w, yblk[0], invqy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block_scaled_f(&w, yblk[1], invqy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block_scaled_f(&w, yblk[2], invqy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block_scaled_f(&w, yblk[3], invqy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block_scaled_f(&w, cbblk, invqc, &hdc_c, &hac_c, &pred_cb);
			lmi_jpeg_encode_block_scaled_f(&w, crblk, invqc, &hdc_c, &hac_c, &pred_cr);
			if (w.err)
				return -1;
		}
	}
	lmi_jpeg_flush_bits(&w);
	if (w.err)
		return -1;
	c->size = (int)w.pos;
	c->lumasum = lumasum;
	return 0;
}

struct lmi_mjpeg_pool {
	pthread_mutex_t lock;
	pthread_cond_t start;
	pthread_cond_t done_cv;
	pthread_t tids[8];
	struct lmi_mjpeg_chunk *chunks;
	int nthreads;
	int generation;
	int done;
	int stop;
	int initialized;
};

static struct lmi_mjpeg_pool g_mjpeg_pool = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.start = PTHREAD_COND_INITIALIZER,
	.done_cv = PTHREAD_COND_INITIALIZER,
};
static uint8_t *g_mjpeg_chunk_bufs[8];
static size_t g_mjpeg_chunk_caps[8];

static void *lmi_mjpeg_pool_worker(void *arg)
{
	int idx = (int)(intptr_t)arg;
	int seen = 0;

	pthread_mutex_lock(&g_mjpeg_pool.lock);
	for (;;) {
		struct lmi_mjpeg_chunk *c;

		while (!g_mjpeg_pool.stop && g_mjpeg_pool.generation == seen)
			pthread_cond_wait(&g_mjpeg_pool.start, &g_mjpeg_pool.lock);
		if (g_mjpeg_pool.stop)
			break;
		seen = g_mjpeg_pool.generation;
		c = &g_mjpeg_pool.chunks[idx];
		pthread_mutex_unlock(&g_mjpeg_pool.lock);

		if (lmi_mjpeg_encode_bayer_center420_range(c) < 0)
			c->size = -1;

		pthread_mutex_lock(&g_mjpeg_pool.lock);
		g_mjpeg_pool.done++;
		if (g_mjpeg_pool.done >= g_mjpeg_pool.nthreads)
			pthread_cond_signal(&g_mjpeg_pool.done_cv);
	}
	pthread_mutex_unlock(&g_mjpeg_pool.lock);
	return NULL;
}

static int lmi_mjpeg_pool_init(int nthreads)
{
	int created = 0;

	pthread_mutex_lock(&g_mjpeg_pool.lock);
	if (g_mjpeg_pool.initialized) {
		int ok = g_mjpeg_pool.nthreads == nthreads;
		pthread_mutex_unlock(&g_mjpeg_pool.lock);
		return ok ? 0 : -1;
	}
	g_mjpeg_pool.nthreads = nthreads;
	g_mjpeg_pool.stop = 0;
	g_mjpeg_pool.generation = 0;
	g_mjpeg_pool.done = 0;
	while (created < nthreads) {
		if (pthread_create(&g_mjpeg_pool.tids[created], NULL,
				   lmi_mjpeg_pool_worker, (void *)(intptr_t)created) != 0) {
			g_mjpeg_pool.stop = 1;
			g_mjpeg_pool.nthreads = created;
			pthread_cond_broadcast(&g_mjpeg_pool.start);
			pthread_mutex_unlock(&g_mjpeg_pool.lock);
			for (int i = 0; i < created; i++)
				pthread_join(g_mjpeg_pool.tids[i], NULL);
			pthread_mutex_lock(&g_mjpeg_pool.lock);
			g_mjpeg_pool.stop = 0;
			g_mjpeg_pool.nthreads = 0;
			g_mjpeg_pool.initialized = 0;
			pthread_mutex_unlock(&g_mjpeg_pool.lock);
			return -1;
		}
		created++;
	}
	g_mjpeg_pool.initialized = 1;
	pthread_mutex_unlock(&g_mjpeg_pool.lock);
	return 0;
}

static int lmi_mjpeg_pool_run(int nthreads, struct lmi_mjpeg_chunk *chunks)
{
	if (lmi_mjpeg_pool_init(nthreads) < 0)
		return -1;

	pthread_mutex_lock(&g_mjpeg_pool.lock);
	if (g_mjpeg_pool.nthreads != nthreads) {
		pthread_mutex_unlock(&g_mjpeg_pool.lock);
		return -1;
	}
	g_mjpeg_pool.chunks = chunks;
	g_mjpeg_pool.done = 0;
	g_mjpeg_pool.generation++;
	pthread_cond_broadcast(&g_mjpeg_pool.start);
	while (g_mjpeg_pool.done < nthreads)
		pthread_cond_wait(&g_mjpeg_pool.done_cv, &g_mjpeg_pool.lock);
	g_mjpeg_pool.chunks = NULL;
	pthread_mutex_unlock(&g_mjpeg_pool.lock);
	return 0;
}

static int lmi_mjpeg_encode_bayer_direct420_threaded(uint8_t *dst, size_t cap,
							 int width, int height, int quality,
							 double rgain, double bgain, long *mean_out,
							 enum mjpeg_scale_mode scale_mode)
{
	struct lmi_jpeg_writer hdr;
	struct lmi_mjpeg_chunk chunks[8];
	uint8_t qy[64], qc[64];
	int rgain_q, ggain_q, bgain_q;
	int mcu_w, mcu_h, nthreads, rows_per_thread;
	size_t pos;
	long lumasum = 0;

	if (!dst || width <= 0 || height <= 0)
		return -1;
	if (scale_mode == MJPEG_SCALE_BAYER_AREA_FRAC) {
		if (ensure_frac_maps(width, height) < 0)
			return -1;
	} else if (scale_mode == MJPEG_SCALE_BAYER_CENTER) {
		if (ensure_center_maps(width, height) < 0)
			return -1;
	} else {
		return -1;
	}
	mcu_w = (width + 15) / 16;
	mcu_h = (height + 15) / 16;
	nthreads = g_mjpeg_fast_threads;
	if (scale_mode == MJPEG_SCALE_BAYER_CENTER && (nthreads < 2 || mcu_h < 2))
		return lmi_mjpeg_encode_bayer_center420(dst, cap, width, height, quality,
							  rgain, bgain, mean_out);
	if (nthreads < 1)
		nthreads = 1;
	if (nthreads > 8)
		nthreads = 8;
	if (nthreads > mcu_h)
		nthreads = mcu_h;
	memset(chunks, 0, sizeof(chunks));

	rgain_q = (int)(rgain * g_soft_gain * 256.0 + 0.5);
	ggain_q = (int)(g_soft_gain * 256.0 + 0.5);
	bgain_q = (int)(bgain * g_soft_gain * 256.0 + 0.5);
	if (rgain_q < 0) rgain_q = 0;
	if (ggain_q < 0) ggain_q = 0;
	if (bgain_q < 0) bgain_q = 0;
	make_mjpeg_gain_lut(g_mjpeg_rlut, rgain_q);
	make_mjpeg_gain_lut(g_mjpeg_glut, ggain_q);
	make_mjpeg_gain_lut(g_mjpeg_blut, bgain_q);

	memset(&hdr, 0, sizeof(hdr));
	hdr.buf = dst;
	hdr.cap = cap;
	lmi_jpeg_make_qtable(qy, lmi_jpeg_q_luma_base, quality);
	lmi_jpeg_make_qtable(qc, lmi_jpeg_q_chroma_base, quality);
	if (nthreads > 1)
		lmi_jpeg_restart_interval = mcu_w * ((mcu_h + nthreads - 1) / nthreads);
	else
		lmi_jpeg_restart_interval = 0;
	lmi_jpeg_emit_header(&hdr, width, height, 0x22, qy, qc);
	lmi_jpeg_restart_interval = 0;
	if (hdr.err)
		return -1;
	pos = hdr.pos;

	rows_per_thread = (mcu_h + nthreads - 1) / nthreads;
	for (int i = 0; i < nthreads; i++) {
		int y0 = i * rows_per_thread;
		int y1 = y0 + rows_per_thread;
		if (y1 > mcu_h)
			y1 = mcu_h;
		memset(&chunks[i], 0, sizeof(chunks[i]));
		chunks[i].cap = cap / nthreads + 65536;
		if (g_mjpeg_chunk_caps[i] < chunks[i].cap) {
			uint8_t *buf = realloc(g_mjpeg_chunk_bufs[i], chunks[i].cap);
			if (!buf)
				goto fail;
			g_mjpeg_chunk_bufs[i] = buf;
			g_mjpeg_chunk_caps[i] = chunks[i].cap;
		}
		chunks[i].buf = g_mjpeg_chunk_bufs[i];
		chunks[i].mcu_y0 = y0;
		chunks[i].mcu_y1 = y1;
		chunks[i].width = width;
		chunks[i].height = height;
		chunks[i].quality = quality;
		chunks[i].scale_mode = scale_mode;
		chunks[i].rgain_q = rgain_q;
		chunks[i].ggain_q = ggain_q;
		chunks[i].bgain_q = bgain_q;
	}
	if (lmi_mjpeg_pool_run(nthreads, chunks) < 0)
		goto fail;
	for (int i = 0; i < nthreads; i++) {
		if (chunks[i].size < 0 || pos + (size_t)chunks[i].size + 4 > cap)
			goto fail;
		memcpy(dst + pos, chunks[i].buf, (size_t)chunks[i].size);
		pos += (size_t)chunks[i].size;
		hdr.pos = pos;
		lumasum += chunks[i].lumasum;
		if (i + 1 < nthreads)
			lmi_jpeg_marker(&hdr, (uint8_t)(0xd0 + (i & 7)));
		pos = hdr.pos;
	}
	if (pos + 2 > cap)
		goto fail;
	dst[pos++] = 0xff;
	dst[pos++] = 0xd9;
	if (mean_out)
		*mean_out = lumasum / ((long)width * height);
	return (int)pos;

fail:
	return -1;
}

static int lmi_mjpeg_encode_bayer_center420(uint8_t *dst, size_t cap,
						     int width, int height, int quality,
						     double rgain, double bgain, long *mean_out)
{
	struct lmi_jpeg_writer w;
	struct lmi_jpeg_huff hdc_y, hac_y, hdc_c, hac_c;
	uint8_t qy[64], qc[64];
	float invqy[64], invqc[64];
	int pred_y = 0, pred_cb = 0, pred_cr = 0;
	int rgain_q, ggain_q, bgain_q;
	long lumasum = 0;
	int mx, my;

	if (!dst || width <= 0 || height <= 0 || ensure_center_maps(width, height) < 0)
		return -1;
	memset(&w, 0, sizeof(w));
	w.buf = dst;
	w.cap = cap;
	lmi_jpeg_make_qtable(qy, lmi_jpeg_q_luma_base, quality);
	lmi_jpeg_make_qtable(qc, lmi_jpeg_q_chroma_base, quality);
	lmi_jpeg_make_inv_qtable_f(invqy, qy);
	lmi_jpeg_make_inv_qtable_f(invqc, qc);
	lmi_jpeg_build_huff(&hdc_y, lmi_jpeg_bits_dc_luma, lmi_jpeg_vals_dc_luma);
	lmi_jpeg_build_huff(&hac_y, lmi_jpeg_bits_ac_luma, lmi_jpeg_vals_ac_luma);
	lmi_jpeg_build_huff(&hdc_c, lmi_jpeg_bits_dc_chroma, lmi_jpeg_vals_dc_chroma);
	lmi_jpeg_build_huff(&hac_c, lmi_jpeg_bits_ac_chroma, lmi_jpeg_vals_ac_chroma);

	rgain_q = (int)(rgain * g_soft_gain * 256.0 + 0.5);
	ggain_q = (int)(g_soft_gain * 256.0 + 0.5);
	bgain_q = (int)(bgain * g_soft_gain * 256.0 + 0.5);
	if (rgain_q < 0) rgain_q = 0;
	if (ggain_q < 0) ggain_q = 0;
	if (bgain_q < 0) bgain_q = 0;
	make_mjpeg_gain_lut(g_mjpeg_rlut, rgain_q);
	make_mjpeg_gain_lut(g_mjpeg_glut, ggain_q);
	make_mjpeg_gain_lut(g_mjpeg_blut, bgain_q);

	lmi_jpeg_emit_header(&w, width, height, 0x22, qy, qc);
	for (my = 0; my < height; my += 16) {
		for (mx = 0; mx < width; mx += 16) {
			float yblk[4][64];
			float cbblk[64];
			float crblk[64];
			int sum_cb[64];
			int sum_cr[64];

			memset(sum_cb, 0, sizeof(sum_cb));
			memset(sum_cr, 0, sizeof(sum_cr));
			for (int yy = 0; yy < 16; yy++) {
				int oy = my + yy;
				int cy = yy >> 1;
				int map_y = oy < height ? oy : height - 1;
				for (int xx = 0; xx < 16; xx++) {
					int ox = mx + xx;
					int map_x = ox < width ? ox : width - 1;
					int qx = g_center_qx[map_x];
					int qy0 = g_center_qy[map_y];
					int yv, cb, cr;
					int bi = (yy >= 8 ? 2 : 0) + (xx >= 8 ? 1 : 0);
					int ci = cy * 8 + (xx >> 1);

					bayer_center_ycbcr_fast(qx, qy0, rgain_q, ggain_q, bgain_q,
								   &yv, &cb, &cr);
					yblk[bi][(yy & 7) * 8 + (xx & 7)] = (float)yv - 128.0f;
					sum_cb[ci] += cb;
					sum_cr[ci] += cr;
					if (ox < width && oy < height)
						lumasum += yv;
				}
			}
			for (int i = 0; i < 64; i++) {
				cbblk[i] = (float)((sum_cb[i] + 2) >> 2) - 128.0f;
				crblk[i] = (float)((sum_cr[i] + 2) >> 2) - 128.0f;
			}
			lmi_jpeg_encode_block_scaled_f(&w, yblk[0], invqy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block_scaled_f(&w, yblk[1], invqy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block_scaled_f(&w, yblk[2], invqy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block_scaled_f(&w, yblk[3], invqy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block_scaled_f(&w, cbblk, invqc, &hdc_c, &hac_c, &pred_cb);
			lmi_jpeg_encode_block_scaled_f(&w, crblk, invqc, &hdc_c, &hac_c, &pred_cr);
			if (w.err)
				return -1;
		}
	}
	lmi_jpeg_flush_bits(&w);
	lmi_jpeg_marker(&w, 0xd9);
	if (w.err)
		return -1;
	if (mean_out)
		*mean_out = lumasum / ((long)width * height);
	return (int)w.pos;
}

static inline void bayer_area_frac_linear_rgb(int tx, int ty, int ow, int oh,
												int xoff, int yoff,
												double rgain, double bgain,
												int *R, int *Gout, int *Bout)
{
	long sx_fp = ((long)(tx * 2 + 1) * g_src_qw * 128) / ow + (long)g_src_qx * 256 + xoff;
	long sy_fp = ((long)(ty * 2 + 1) * g_src_qh * 128) / oh + (long)g_src_qy * 256 + yoff;
	int qx = (int)(sx_fp >> 8);
	int qy = (int)(sy_fp >> 8);
	int fx = (int)(sx_fp & 255);
	int fy = (int)(sy_fp & 255);
	int qx1, qy1;
	int r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
	int w00, w10, w01, w11;

	if (qx < g_src_qx) {
		qx = g_src_qx;
		fx = 0;
	}
	if (qy < g_src_qy) {
		qy = g_src_qy;
		fy = 0;
	}
	if (qx >= g_src_qx + g_src_qw) {
		qx = g_src_qx + g_src_qw - 1;
		fx = 0;
	}
	if (qy >= g_src_qy + g_src_qh) {
		qy = g_src_qy + g_src_qh - 1;
		fy = 0;
	}
	qx = bclamp(qx, 0, g_int_qw - 1);
	qy = bclamp(qy, 0, g_int_qh - 1);
	qx1 = qx + 1;
	qy1 = qy + 1;
	if (qx1 >= g_src_qx + g_src_qw)
		qx1 = qx;
	if (qy1 >= g_src_qy + g_src_qh)
		qy1 = qy;
	qx1 = bclamp(qx1, 0, g_int_qw - 1);
	qy1 = bclamp(qy1, 0, g_int_qh - 1);
	quad_sample(qx, qy, rgain, bgain, &r00, &g00, &b00);
	quad_sample(qx1, qy, rgain, bgain, &r10, &g10, &b10);
	quad_sample(qx, qy1, rgain, bgain, &r01, &g01, &b01);
	quad_sample(qx1, qy1, rgain, bgain, &r11, &g11, &b11);
	w00 = (256 - fx) * (256 - fy);
	w10 = fx * (256 - fy);
	w01 = (256 - fx) * fy;
	w11 = fx * fy;
	*R = (r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11 + 32768) >> 16;
	*Gout = (g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11 + 32768) >> 16;
	*Bout = (b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11 + 32768) >> 16;
}

static inline void bayer_area_frac_rgb(int tx, int ty, int ow, int oh,
									   double rgain, double bgain,
									   int *R, int *Gout, int *Bout, uint16_t *luma10)
{
	bayer_area_frac_linear_rgb(tx, ty, ow, oh, 0, 0, rgain, bgain, R, Gout, Bout);
	if (luma10)
		*luma10 = luma10_clamped(*R, *Gout, *Bout);
	gamma_clamp_rgb(R, Gout, Bout);
}

static inline void bayer_area_ref_rgb(int tx, int ty, int ow, int oh,
								   double rgain, double bgain,
								   int *R, int *Gout, int *Bout)
{
	bayer_area_rgb(tx, ty, ow, oh, rgain, bgain, R, Gout, Bout, NULL);
}

static inline void bayer_area_frac_4tap_rgb(int tx, int ty, int ow, int oh,
										double rgain, double bgain,
										int *R, int *Gout, int *Bout, uint16_t *luma10)
{
	int xoff, yoff;
	int r, g, b;
	int rsum = 0, gsum = 0, bsum = 0;

	xoff = (int)(((long)g_src_qw * 64 + ow / 2) / ow);
	yoff = (int)(((long)g_src_qh * 64 + oh / 2) / oh);
	if (xoff < 16)
		xoff = 16;
	if (yoff < 16)
		yoff = 16;
	if (xoff > 256)
		xoff = 256;
	if (yoff > 256)
		yoff = 256;
	bayer_area_frac_linear_rgb(tx, ty, ow, oh, -xoff, -yoff, rgain, bgain, &r, &g, &b);
	rsum += r; gsum += g; bsum += b;
	bayer_area_frac_linear_rgb(tx, ty, ow, oh, xoff, -yoff, rgain, bgain, &r, &g, &b);
	rsum += r; gsum += g; bsum += b;
	bayer_area_frac_linear_rgb(tx, ty, ow, oh, -xoff, yoff, rgain, bgain, &r, &g, &b);
	rsum += r; gsum += g; bsum += b;
	bayer_area_frac_linear_rgb(tx, ty, ow, oh, xoff, yoff, rgain, bgain, &r, &g, &b);
	rsum += r; gsum += g; bsum += b;
	*R = (rsum + 2) >> 2;
	*Gout = (gsum + 2) >> 2;
	*Bout = (bsum + 2) >> 2;
	if (luma10)
		*luma10 = luma10_clamped(*R, *Gout, *Bout);
	gamma_clamp_rgb(R, Gout, Bout);
}


static inline uint32_t int_sum_bounded(const uint32_t *tab, int qx0, int qy0, int qx1, int qy1)
{
	if (qx0 < 0)
		qx0 = 0;
	if (qy0 < 0)
		qy0 = 0;
	if (qx1 > g_int_qw)
		qx1 = g_int_qw;
	if (qy1 > g_int_qh)
		qy1 = g_int_qh;
	if (qx1 <= qx0 || qy1 <= qy0)
		return 0;
	return int_sum(tab, qx0, qy0, qx1, qy1);
}

static inline void bayer_area_box_scaled_rgb(int tx, int ty, int ow, int oh,
							 double rgain, double bgain, int area_scale,
							 int *R, int *Gout, int *Bout, uint16_t *luma10)
{
	int qx0, qx1, qy0, qy1;
	int qxc, qyc, radius;
	uint32_t rs, grs, gbs, bs;
	int qn;
	long src_qw = ((long)g_src_qw + ow - 1) / ow;
	long src_qh = ((long)g_src_qh + oh - 1) / oh;

	if (src_qw < 1)
		src_qw = 1;
	if (src_qh < 1)
		src_qh = 1;
	if (area_scale < 25)
		area_scale = 25;
	if (area_scale > 1000)
		area_scale = 1000;
	radius = (int)(((src_qw > src_qh ? src_qw : src_qh) * area_scale + 99) / 200);
	if (radius < 1)
		radius = 1;
	if (radius > 8)
		radius = 8;
	qxc = g_src_qx + (int)(((long)(tx * 2 + 1) * g_src_qw) / (ow * 2L));
	qyc = g_src_qy + (int)(((long)(ty * 2 + 1) * g_src_qh) / (oh * 2L));
	if (qxc < g_src_qx)
		qxc = g_src_qx;
	if (qyc < g_src_qy)
		qyc = g_src_qy;
	if (qxc >= g_src_qx + g_src_qw)
		qxc = g_src_qx + g_src_qw - 1;
	if (qyc >= g_src_qy + g_src_qh)
		qyc = g_src_qy + g_src_qh - 1;
	qxc = bclamp(qxc, 0, g_int_qw - 1);
	qyc = bclamp(qyc, 0, g_int_qh - 1);
	qx0 = qxc - radius;
	qx1 = qxc + radius + 1;
	qy0 = qyc - radius;
	qy1 = qyc + radius + 1;
	if (qx0 < g_src_qx)
		qx0 = g_src_qx;
	if (qy0 < g_src_qy)
		qy0 = g_src_qy;
	if (qx1 > g_src_qx + g_src_qw)
		qx1 = g_src_qx + g_src_qw;
	if (qy1 > g_src_qy + g_src_qh)
		qy1 = g_src_qy + g_src_qh;
	if (qx0 < 0)
		qx0 = 0;
	if (qy0 < 0)
		qy0 = 0;
	if (qx1 > g_int_qw)
		qx1 = g_int_qw;
	if (qy1 > g_int_qh)
		qy1 = g_int_qh;
	rs = int_sum_bounded(g_int_r, qx0, qy0, qx1, qy1);
	grs = int_sum_bounded(g_int_gr, qx0, qy0, qx1, qy1);
	gbs = int_sum_bounded(g_int_gb, qx0, qy0, qx1, qy1);
	bs = int_sum_bounded(g_int_b, qx0, qy0, qx1, qy1);
	qn = (qx1 - qx0) * (qy1 - qy0);
	if (qn < 1)
		qn = 1;
	*R = (int)(((double)rs / qn) * rgain * g_soft_gain);
	*Gout = (int)(((double)((uint64_t)grs + gbs) / (qn * 2)) * g_soft_gain);
	*Bout = (int)(((double)bs / qn) * bgain * g_soft_gain);
	if (luma10)
		*luma10 = luma10_clamped(*R, *Gout, *Bout);
	gamma_clamp_rgb(R, Gout, Bout);
}

static inline void bayer_area_box_rgb(int tx, int ty, int ow, int oh,
						  double rgain, double bgain,
						  int *R, int *Gout, int *Bout, uint16_t *luma10)
{
	bayer_area_box_scaled_rgb(tx, ty, ow, oh, rgain, bgain, g_mjpeg_area_scale,
					R, Gout, Bout, luma10);
}

static inline void bayer_quad_box_rgb(int tx, int ty, int ow, int oh,
							  double rgain, double bgain,
							  int *R, int *Gout, int *Bout, uint16_t *luma10)
{
	int qx0, qx1, qy0, qy1;
	uint32_t rs, gs, bs;
	int qn;

	/* Keep the downscale footprint phase-locked to integer Bayer quads.  The
	 * fractional quad sampler is sharp, but clipped lights can expose the sampling
	 * lattice as a square halo.  This mode averages only complete GR/BG quads that
	 * map to the output pixel, so every output sample has equal red/green/blue
	 * support before gamma/JPEG. */
	qx0 = g_src_qx + (int)(((long)tx * g_src_qw) / ow);
	qx1 = g_src_qx + (int)(((long)(tx + 1) * g_src_qw + ow - 1) / ow);
	qy0 = g_src_qy + (int)(((long)ty * g_src_qh) / oh);
	qy1 = g_src_qy + (int)(((long)(ty + 1) * g_src_qh + oh - 1) / oh);
	if (qx1 <= qx0)
		qx1 = qx0 + 1;
	if (qy1 <= qy0)
		qy1 = qy0 + 1;
	if (qx0 < g_src_qx)
		qx0 = g_src_qx;
	if (qy0 < g_src_qy)
		qy0 = g_src_qy;
	if (qx1 > g_src_qx + g_src_qw)
		qx1 = g_src_qx + g_src_qw;
	if (qy1 > g_src_qy + g_src_qh)
		qy1 = g_src_qy + g_src_qh;
	if (qx0 < 0)
		qx0 = 0;
	if (qy0 < 0)
		qy0 = 0;
	if (qx1 > g_int_qw)
		qx1 = g_int_qw;
	if (qy1 > g_int_qh)
		qy1 = g_int_qh;
	qn = (qx1 - qx0) * (qy1 - qy0);
	if (qn < 1)
		qn = 1;
	rs = int_sum(g_int_r, qx0, qy0, qx1, qy1);
	gs = int_sum(g_int_gr, qx0, qy0, qx1, qy1) + int_sum(g_int_gb, qx0, qy0, qx1, qy1);
	bs = int_sum(g_int_b, qx0, qy0, qx1, qy1);
	*R = (int)(((double)rs / qn) * rgain * g_soft_gain);
	*Gout = (int)(((double)gs / (qn * 2)) * g_soft_gain);
	*Bout = (int)(((double)bs / qn) * bgain * g_soft_gain);
	if (luma10)
		*luma10 = luma10_clamped(*R, *Gout, *Bout);
	gamma_clamp_rgb(R, Gout, Bout);
}

static inline void bayer_quad4_rgb(int tx, int ty, int ow, int oh,
						  double rgain, double bgain,
						  int *R, int *Gout, int *Bout, uint16_t *luma10)
{
	int qx, qy;
	int r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;

	qx = g_src_qx + (int)(((long)(tx * 2 + 1) * g_src_qw) / (ow * 2L));
	qy = g_src_qy + (int)(((long)(ty * 2 + 1) * g_src_qh) / (oh * 2L));
	if (qx < g_src_qx)
		qx = g_src_qx;
	if (qy < g_src_qy)
		qy = g_src_qy;
	if (qx >= g_src_qx + g_src_qw)
		qx = g_src_qx + g_src_qw - 1;
	if (qy >= g_src_qy + g_src_qh)
		qy = g_src_qy + g_src_qh - 1;
	qx = bclamp(qx, 0, g_int_qw - 1);
	qy = bclamp(qy, 0, g_int_qh - 1);
	quad_sample(qx, qy, rgain, bgain, &r00, &g00, &b00);
	quad_sample(qx + 1, qy, rgain, bgain, &r10, &g10, &b10);
	quad_sample(qx, qy + 1, rgain, bgain, &r01, &g01, &b01);
	quad_sample(qx + 1, qy + 1, rgain, bgain, &r11, &g11, &b11);
	*R = (r00 + r10 + r01 + r11 + 2) >> 2;
	*Gout = (g00 + g10 + g01 + g11 + 2) >> 2;
	*Bout = (b00 + b10 + b01 + b11 + 2) >> 2;
	if (luma10)
		*luma10 = luma10_clamped(*R, *Gout, *Bout);
	gamma_clamp_rgb(R, Gout, Bout);
}

static inline void demosaic_scaled_rgb(int tx, int ty, int ow, int oh,
						   double rgain, double bgain,
						   int *R, int *Gout, int *Bout, uint16_t *luma10)
{
	int sx0, sx1, sy0, sy1;
	int sx, sy, dx, dy, taps = 0;
	int64_t rs = 0, gs = 0, bs = 0;

	source_view_span_scaled(tx, ow, g_src_w, g_src_x, g_mjpeg_area_scale, &sx0, &sx1);
	source_view_span_scaled(ty, oh, g_src_h, g_src_y, g_mjpeg_area_scale, &sy0, &sy1);
	sx = (sx0 + sx1) >> 1;
	sy = (sy0 + sy1) >> 1;

	if (g_mjpeg_scale_mode == MJPEG_SCALE_DEMOSAIC_CENTER) {
		demosaic_linear_at(sx, sy, rgain, bgain, R, Gout, Bout);
		if (luma10)
			*luma10 = luma10_clamped(*R, *Gout, *Bout);
		gamma_clamp_rgb(R, Gout, Bout);
		return;
	}

	if (g_mjpeg_scale_mode == MJPEG_SCALE_DEMOSAIC_4TAP) {
		int xs[2] = { sx0, sx1 };
		int ys[2] = { sy0, sy1 };
		for (dy = 0; dy < 2; dy++) {
			for (dx = 0; dx < 2; dx++) {
				int r, g, b;
				demosaic_linear_at(xs[dx], ys[dy], rgain, bgain, &r, &g, &b);
				rs += r;
				gs += g;
				bs += b;
				taps++;
			}
		}
	} else {
		int xs[3] = { sx0, sx, sx1 };
		int ys[3] = { sy0, sy, sy1 };
		for (dy = 0; dy < 3; dy++) {
			for (dx = 0; dx < 3; dx++) {
				int r, g, b;
				demosaic_linear_at(xs[dx], ys[dy], rgain, bgain, &r, &g, &b);
				rs += r;
				gs += g;
				bs += b;
				taps++;
			}
		}
	}
	if (taps < 1)
		taps = 1;
	*R = (int)(rs / taps);
	*Gout = (int)(gs / taps);
	*Bout = (int)(bs / taps);
	if (luma10)
		*luma10 = luma10_clamped(*R, *Gout, *Bout);
	gamma_clamp_rgb(R, Gout, Bout);
}

static const char *mjpeg_scale_mode_name(void)
{
	switch (g_mjpeg_scale_mode) {
	case MJPEG_SCALE_BAYER_AREA_FRAC:
		return "bayer-area-frac";
	case MJPEG_SCALE_BAYER_AREA_FRAC_4TAP:
		return "bayer-area-frac-4tap";
	case MJPEG_SCALE_BAYER_AREA_BOX:
		return "bayer-area-box";
	case MJPEG_SCALE_BAYER_QUAD_BOX:
		return "bayer-quad-box";
	case MJPEG_SCALE_BAYER_QUAD4:
		return "bayer-quad4";
	case MJPEG_SCALE_BAYER_CENTER:
		return "bayer-center";
	case MJPEG_SCALE_DEMOSAIC_CENTER:
		return "demosaic-center";
	case MJPEG_SCALE_DEMOSAIC_4TAP:
		return "demosaic-4tap";
	case MJPEG_SCALE_DEMOSAIC_9TAP:
		return "demosaic-9tap";
	case MJPEG_SCALE_BAYER_AREA:
	default:
		return "bayer-area";
	}
}

static void smooth_rgb(uint8_t *dst, const uint8_t *src, int w, int h, int stride, int amount)
{
	int x, y, c;

	if (amount <= 0) {
		if (dst != src)
			memcpy(dst, src, (size_t)stride * h);
		return;
	}
	if (amount > 100)
		amount = 100;
	memcpy(dst, src, (size_t)stride * h);
	for (y = 1; y + 1 < h; y++) {
		for (x = 1; x + 1 < w; x++) {
			const uint8_t *p = src + (size_t)y * stride + x * 3;
			uint8_t *d = dst + (size_t)y * stride + x * 3;
			for (c = 0; c < 3; c++) {
				int blur = (src[(size_t)(y - 1) * stride + (x - 1) * 3 + c] +
						src[(size_t)(y - 1) * stride + x * 3 + c] * 2 +
						src[(size_t)(y - 1) * stride + (x + 1) * 3 + c] +
						src[(size_t)y * stride + (x - 1) * 3 + c] * 2 +
						p[c] * 4 +
						src[(size_t)y * stride + (x + 1) * 3 + c] * 2 +
						src[(size_t)(y + 1) * stride + (x - 1) * 3 + c] +
						src[(size_t)(y + 1) * stride + x * 3 + c] * 2 +
						src[(size_t)(y + 1) * stride + (x + 1) * 3 + c] + 8) >> 4;
				int v = (p[c] * (100 - amount) + blur * amount + 50) / 100;
				d[c] = (uint8_t)bclamp(v, 0, 255);
			}
		}
	}
}

static int ensure_highlight_tables(int w, int h)
{
	size_t n;

	if (g_hi_r && g_hi_w == w && g_hi_h == h)
		return 0;
	free(g_hi_r);
	free(g_hi_g);
	free(g_hi_b);
	free(g_hi_mask);
	g_hi_r = g_hi_g = g_hi_b = g_hi_mask = NULL;
	g_hi_w = w;
	g_hi_h = h;
	g_hi_stride = w + 1;
	n = (size_t)(w + 1) * (h + 1);
	g_hi_r = calloc(n, sizeof(*g_hi_r));
	g_hi_g = calloc(n, sizeof(*g_hi_g));
	g_hi_b = calloc(n, sizeof(*g_hi_b));
	g_hi_mask = calloc(n, sizeof(*g_hi_mask));
	return (g_hi_r && g_hi_g && g_hi_b && g_hi_mask) ? 0 : -1;
}

static inline uint32_t hi_sum(const uint32_t *tab, int x0, int y0, int x1, int y1)
{
	const uint32_t *a = tab + (size_t)y0 * g_hi_stride;
	const uint32_t *b = tab + (size_t)y1 * g_hi_stride;
	return b[x1] - b[x0] - a[x1] + a[x0];
}

static void highlight_smooth_rgb(uint8_t *dst, const uint8_t *src, int w, int h, int stride,
					 int amount, int threshold, int radius)
{
	int x, y;

	if (amount <= 0) {
		if (dst != src)
			memcpy(dst, src, (size_t)stride * h);
		return;
	}
	if (amount > 100)
		amount = 100;
	if (threshold < 0)
		threshold = 0;
	if (threshold > 255)
		threshold = 255;
	if (radius < 1)
		radius = 1;
	if (radius > 12)
		radius = 12;
	if (ensure_highlight_tables(w, h) < 0) {
		if (dst != src)
			memcpy(dst, src, (size_t)stride * h);
		return;
	}
	memcpy(dst, src, (size_t)stride * h);
	for (y = 0; y < h; y++) {
		uint32_t rr = 0, gg = 0, bb = 0, mm = 0;
		uint32_t *dr = g_hi_r + (size_t)(y + 1) * g_hi_stride;
		uint32_t *dg = g_hi_g + (size_t)(y + 1) * g_hi_stride;
		uint32_t *db = g_hi_b + (size_t)(y + 1) * g_hi_stride;
		uint32_t *dm = g_hi_mask + (size_t)(y + 1) * g_hi_stride;
		uint32_t *pr = g_hi_r + (size_t)y * g_hi_stride;
		uint32_t *pg = g_hi_g + (size_t)y * g_hi_stride;
		uint32_t *pb = g_hi_b + (size_t)y * g_hi_stride;
		uint32_t *pm = g_hi_mask + (size_t)y * g_hi_stride;
		dr[0] = dg[0] = db[0] = dm[0] = 0;
		for (x = 0; x < w; x++) {
			const uint8_t *p = src + (size_t)y * stride + x * 3;
			int luma = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
			rr += p[0];
			gg += p[1];
			bb += p[2];
			mm += luma >= threshold ? 1U : 0U;
			dr[x + 1] = pr[x + 1] + rr;
			dg[x + 1] = pg[x + 1] + gg;
			db[x + 1] = pb[x + 1] + bb;
			dm[x + 1] = pm[x + 1] + mm;
		}
	}
	for (y = 0; y < h; y++) {
		int y0 = y - radius, y1 = y + radius + 1;
		if (y0 < 0)
			y0 = 0;
		if (y1 > h)
			y1 = h;
		for (x = 0; x < w; x++) {
			int x0 = x - radius, x1 = x + radius + 1;
			uint32_t sumr, sumg, sumb, mask;
			int n, local, c;
			const uint8_t *p;
			uint8_t *d;
			if (x0 < 0)
				x0 = 0;
			if (x1 > w)
				x1 = w;
			mask = hi_sum(g_hi_mask, x0, y0, x1, y1);
			if (!mask)
				continue;
			n = (x1 - x0) * (y1 - y0);
			if (n < 1)
				n = 1;
			local = (int)((mask * amount) / (uint32_t)n);
			if (local < 1)
				continue;
			sumr = hi_sum(g_hi_r, x0, y0, x1, y1);
			sumg = hi_sum(g_hi_g, x0, y0, x1, y1);
			sumb = hi_sum(g_hi_b, x0, y0, x1, y1);
			p = src + (size_t)y * stride + x * 3;
			d = dst + (size_t)y * stride + x * 3;
			for (c = 0; c < 3; c++) {
				uint32_t sum = c == 0 ? sumr : (c == 1 ? sumg : sumb);
				int blur = (int)((sum + n / 2) / (uint32_t)n);
				int v = (p[c] * (100 - local) + blur * local + 50) / 100;
				d[c] = (uint8_t)bclamp(v, 0, 255);
			}
		}
	}
}

static void desaturate_highlights_rgb(uint8_t *dst, const uint8_t *src, int w, int h, int stride,
						  int amount, int threshold)
{
	int x, y;

	if (amount <= 0) {
		if (dst != src)
			memcpy(dst, src, (size_t)stride * h);
		return;
	}
	if (amount > 100)
		amount = 100;
	if (threshold < 0)
		threshold = 0;
	if (threshold > 255)
		threshold = 255;
	memcpy(dst, src, (size_t)stride * h);
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			const uint8_t *p = src + (size_t)y * stride + x * 3;
			uint8_t *d = dst + (size_t)y * stride + x * 3;
			int luma = (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
			int local, c;
			if (luma < threshold)
				continue;
			local = ((luma - threshold) * amount) / (255 - threshold + 1);
			for (c = 0; c < 3; c++) {
				int v = (p[c] * (100 - local) + luma * local + 50) / 100;
				d[c] = (uint8_t)bclamp(v, 0, 255);
			}
		}
	}
}

static void edge_despeckle_rgb(uint8_t *dst, const uint8_t *src, int w, int h, int stride,
						   int amount, int threshold, int radius)
{
	int x, y;

	if (amount <= 0) {
		if (dst != src)
			memcpy(dst, src, (size_t)stride * h);
		return;
	}
	if (amount > 100)
		amount = 100;
	if (threshold < 1)
		threshold = 1;
	if (threshold > 255)
		threshold = 255;
	if (radius < 1)
		radius = 1;
	if (radius > 4)
		radius = 4;
	if (ensure_highlight_tables(w, h) < 0) {
		if (dst != src)
			memcpy(dst, src, (size_t)stride * h);
		return;
	}
	for (y = 0; y < h; y++) {
		uint32_t rr = 0, gg = 0, bb = 0, mm = 0;
		uint32_t *dr = g_hi_r + (size_t)(y + 1) * g_hi_stride;
		uint32_t *dg = g_hi_g + (size_t)(y + 1) * g_hi_stride;
		uint32_t *db = g_hi_b + (size_t)(y + 1) * g_hi_stride;
		uint32_t *dm = g_hi_mask + (size_t)(y + 1) * g_hi_stride;
		uint32_t *pr = g_hi_r + (size_t)y * g_hi_stride;
		uint32_t *pg = g_hi_g + (size_t)y * g_hi_stride;
		uint32_t *pb = g_hi_b + (size_t)y * g_hi_stride;
		uint32_t *pm = g_hi_mask + (size_t)y * g_hi_stride;
		dr[0] = dg[0] = db[0] = dm[0] = 0;
		for (x = 0; x < w; x++) {
			const uint8_t *p = src + (size_t)y * stride + x * 3;
			int hi = p[0] > p[1] ? p[0] : p[1];
			if (p[2] > hi)
				hi = p[2];
			rr += p[0];
			gg += p[1];
			bb += p[2];
			mm += hi >= 220 ? 1U : 0U;
			dr[x + 1] = pr[x + 1] + rr;
			dg[x + 1] = pg[x + 1] + gg;
			db[x + 1] = pb[x + 1] + bb;
			dm[x + 1] = pm[x + 1] + mm;
		}
	}
	memcpy(dst, src, (size_t)stride * h);
	for (y = 0; y < h; y++) {
		int y0 = y - radius, y1 = y + radius + 1;
		if (y0 < 0)
			y0 = 0;
		if (y1 > h)
			y1 = h;
		for (x = 0; x < w; x++) {
			int x0 = x - radius, x1 = x + radius + 1;
			uint32_t sumr, sumg, sumb, mask;
			int n, ar, ag, ab, local, c;
			const uint8_t *p;
			uint8_t *d;
			if (x0 < 0)
				x0 = 0;
			if (x1 > w)
				x1 = w;
			mask = hi_sum(g_hi_mask, x0, y0, x1, y1);
			if (!mask)
				continue;
			n = (x1 - x0) * (y1 - y0);
			if (n < 1)
				n = 1;
			sumr = hi_sum(g_hi_r, x0, y0, x1, y1);
			sumg = hi_sum(g_hi_g, x0, y0, x1, y1);
			sumb = hi_sum(g_hi_b, x0, y0, x1, y1);
			ar = (int)((sumr + n / 2) / (uint32_t)n);
			ag = (int)((sumg + n / 2) / (uint32_t)n);
			ab = (int)((sumb + n / 2) / (uint32_t)n);
			p = src + (size_t)y * stride + x * 3;
			d = dst + (size_t)y * stride + x * 3;
			local = abs((int)p[0] - ar);
			if (abs((int)p[1] - ag) > local)
				local = abs((int)p[1] - ag);
			if (abs((int)p[2] - ab) > local)
				local = abs((int)p[2] - ab);
			if (local < threshold)
				continue;
			local = ((local - threshold) * amount) / (255 - threshold + 1);
			if (local > amount)
				local = amount;
			for (c = 0; c < 3; c++) {
				int avg = c == 0 ? ar : (c == 1 ? ag : ab);
				int v = (p[c] * (100 - local) + avg * local + 50) / 100;
				d[c] = (uint8_t)bclamp(v, 0, 255);
			}
		}
	}
}

static void antibloom_highlights_rgb(uint8_t *dst, const uint8_t *src, const uint16_t *luma10,
							 int w, int h, int stride, int amount,
							 int threshold, int radius)
{
	int x, y;

	if (amount <= 0 || !luma10) {
		if (dst != src)
			memcpy(dst, src, (size_t)stride * h);
		return;
	}
	if (amount > 100)
		amount = 100;
	if (threshold < 1)
		threshold = 1;
	if (threshold > 1023)
		threshold = 1023;
	if (radius < 1)
		radius = 1;
	if (radius > 16)
		radius = 16;
	if (ensure_highlight_tables(w, h) < 0) {
		if (dst != src)
			memcpy(dst, src, (size_t)stride * h);
		return;
	}
	for (y = 0; y < h; y++) {
		uint32_t rr = 0, gg = 0, bb = 0, mm = 0;
		uint32_t *dr = g_hi_r + (size_t)(y + 1) * g_hi_stride;
		uint32_t *dg = g_hi_g + (size_t)(y + 1) * g_hi_stride;
		uint32_t *db = g_hi_b + (size_t)(y + 1) * g_hi_stride;
		uint32_t *dm = g_hi_mask + (size_t)(y + 1) * g_hi_stride;
		uint32_t *pr = g_hi_r + (size_t)y * g_hi_stride;
		uint32_t *pg = g_hi_g + (size_t)y * g_hi_stride;
		uint32_t *pb = g_hi_b + (size_t)y * g_hi_stride;
		uint32_t *pm = g_hi_mask + (size_t)y * g_hi_stride;
		dr[0] = dg[0] = db[0] = dm[0] = 0;
		for (x = 0; x < w; x++) {
			const uint8_t *p = src + (size_t)y * stride + x * 3;
			rr += p[0];
			gg += p[1];
			bb += p[2];
			mm += luma10[(size_t)y * w + x] >= threshold ? 1U : 0U;
			dr[x + 1] = pr[x + 1] + rr;
			dg[x + 1] = pg[x + 1] + gg;
			db[x + 1] = pb[x + 1] + bb;
			dm[x + 1] = pm[x + 1] + mm;
		}
	}
	memcpy(dst, src, (size_t)stride * h);
	for (y = 0; y < h; y++) {
		int y0 = y - radius, y1 = y + radius + 1;
		if (y0 < 0)
			y0 = 0;
		if (y1 > h)
			y1 = h;
		for (x = 0; x < w; x++) {
			int x0 = x - radius, x1 = x + radius + 1;
			uint32_t sumr, sumg, sumb, mask;
			int n, local, c;
			const uint8_t *p;
			uint8_t *d;
			if (x0 < 0)
				x0 = 0;
			if (x1 > w)
				x1 = w;
			mask = hi_sum(g_hi_mask, x0, y0, x1, y1);
			if (!mask)
				continue;
			n = (x1 - x0) * (y1 - y0);
			if (n < 1)
				n = 1;
			local = (int)((mask * amount * 8U) / (uint32_t)n);
			if (local > 100)
				local = 100;
			if (local < 1)
				continue;
			sumr = hi_sum(g_hi_r, x0, y0, x1, y1);
			sumg = hi_sum(g_hi_g, x0, y0, x1, y1);
			sumb = hi_sum(g_hi_b, x0, y0, x1, y1);
			p = src + (size_t)y * stride + x * 3;
			d = dst + (size_t)y * stride + x * 3;
			for (c = 0; c < 3; c++) {
				uint32_t sum = c == 0 ? sumr : (c == 1 ? sumg : sumb);
				int blur = (int)((sum + n / 2) / (uint32_t)n);
				int v = (p[c] * (100 - local) + blur * local + 50) / 100;
				d[c] = (uint8_t)bclamp(v, 0, 255);
			}
		}
	}
}

static void sharpen_rgb(uint8_t *dst, const uint8_t *src, int w, int h, int stride, int amount)
{
	int x, y, c;

	if (amount <= 0) {
		if (dst != src)
			memcpy(dst, src, (size_t)stride * h);
		return;
	}
	memcpy(dst, src, (size_t)stride * h);
	for (y = 1; y + 1 < h; y++) {
		for (x = 1; x + 1 < w; x++) {
			const uint8_t *p = src + (size_t)y * stride + x * 3;
			uint8_t *d = dst + (size_t)y * stride + x * 3;
			for (c = 0; c < 3; c++) {
				int blur = (src[(size_t)(y - 1) * stride + x * 3 + c] +
						src[(size_t)(y + 1) * stride + x * 3 + c] +
						src[(size_t)y * stride + (x - 1) * 3 + c] +
						src[(size_t)y * stride + (x + 1) * 3 + c]) >> 2;
				int v = p[c] + ((p[c] - blur) * amount) / 100;
				d[c] = (uint8_t)bclamp(v, 0, 255);
			}
		}
	}
}

static void draw_motion_overlay_nv12(void)
{
	uint8_t *yp = g_frame;
	uint8_t *uvp = g_frame + (size_t)g_out_stride * g_out_h;
	int box = g_motion_overlay_size;
	int span_x, span_y, x0, y0, x1, y1;
	int x, y;
	uint8_t yval = (g_motion_overlay_seq & 1) ? 235 : 32;

	if (!g_motion_overlay || !g_nv12 || g_mjpeg || g_out_w < 16 || g_out_h < 16)
		return;
	if (box < 16)
		box = 16;
	if (box > g_out_w / 3)
		box = g_out_w / 3;
	if (box > g_out_h / 3)
		box = g_out_h / 3;
	if (box < 16)
		box = 16;
	box &= ~1;
	span_x = g_out_w - box - 2;
	span_y = g_out_h - box - 2;
	if (span_x < 2 || span_y < 2)
		return;
	x0 = 2 + (int)((g_motion_overlay_seq * 23U) % (unsigned int)span_x);
	y0 = 2 + (int)((g_motion_overlay_seq * 11U) % (unsigned int)span_y);
	x0 &= ~1;
	y0 &= ~1;
	x1 = x0 + box;
	y1 = y0 + box;
	for (y = y0; y < y1; y++) {
		uint8_t *row = yp + (size_t)y * g_out_stride;
		for (x = x0; x < x1; x++) {
			int border = y - y0 < 6 || y1 - y <= 6 || x - x0 < 6 || x1 - x <= 6;
			row[x] = border ? 255 : yval;
		}
	}
	for (y = y0 >> 1; y < y1 >> 1; y++) {
		uint8_t *row = uvp + (size_t)y * g_out_stride;
		for (x = x0; x < x1; x += 2) {
			row[x] = (g_motion_overlay_seq & 1) ? 32 : 220;
			row[x + 1] = (g_motion_overlay_seq & 1) ? 220 : 32;
		}
	}
	g_motion_overlay_seq++;
}

/* Map a physical (rotated) YUYV output pixel back to the logical (pre-rotation)
 * output pixel, so rotation folds into the existing source-coordinate sampling
 * with no extra full-frame buffer.  90 == counter-clockwise. */
static inline void rot_logical(int tx, int ty, int *lx, int *ly)
{
	switch (g_rotate) {
	case 90:	/* CCW */
		*lx = g_logical_w - 1 - ty;
		*ly = tx;
		break;
	case 270:	/* CW */
		*lx = ty;
		*ly = g_logical_h - 1 - tx;
		break;
	case 180:
		*lx = g_logical_w - 1 - tx;
		*ly = g_logical_h - 1 - ty;
		break;
	default:
		*lx = tx;
		*ly = ty;
		break;
	}
}

/* Motion-gated temporal denoise on the packed YUYV frame.  Static regions blend
 * with the previous frame (knocks down low-light sensor noise); regions that
 * moved beyond the threshold keep the current sample so motion does not ghost. */
static void denoise_yuyv(void)
{
	int n, i, a, b;
	if (g_denoise <= 0 || g_nv12 || g_mjpeg || !g_prev_frame)
		return;
	a = g_denoise > 100 ? 100 : g_denoise;
	b = 100 - a;
	n = g_out_stride * g_out_h;
	for (i = 0; i < n; i++) {
		int cur = g_frame[i];
		int prv = g_prev_frame[i];
		int out = (abs(cur - prv) < g_denoise_thresh)
			? (cur * b + prv * a + 50) / 100
			: cur;
		g_frame[i] = (uint8_t)out;
		g_prev_frame[i] = (uint8_t)out;
	}
}

static long pack_output(double rgain, double bgain)
{
	int ow = g_out_w, oh = g_out_h;
	int aw = ow > g_raw_w ? g_raw_w : ow;  /* never upsample beyond raw */
	long lumasum = 0;
	int ty, tx;
	/* map output (tx,ty) -> bayer source */
	if (g_mjpeg) {
		const uint8_t *jpeg_src = g_frame;
		(void)aw;
		g_stage_jpeg_ms = 0.0;
		g_stage_direct_mjpeg = 0;
		if ((g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_CENTER ||
			 g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_AREA_FRAC) &&
			g_mjpeg_subsampling == 420 && g_mjpeg_smooth == 0 &&
			g_mjpeg_highlight_smooth == 0 && g_mjpeg_desaturate_highlights == 0 &&
			g_mjpeg_antibloom == 0 && g_mjpeg_edge_despeckle == 0 &&
			g_sharpen == 0 && g_mjpeg_blend_frac == 0 &&
			g_mjpeg_highlight_area == 0 && g_mjpeg_highlight_box == 0 &&
			g_mjpeg_bayer_despeckle == 0) {
			struct timespec ja, jb;
			long mean = 0;
			clock_gettime(CLOCK_MONOTONIC, &ja);
			g_jpeg_size = lmi_mjpeg_encode_bayer_direct420_threaded(g_jpeg,
					(size_t)g_max_frame_bytes, ow, oh, g_mjpeg_quality,
					rgain, bgain, &mean, g_mjpeg_scale_mode);
			clock_gettime(CLOCK_MONOTONIC, &jb);
			g_stage_jpeg_ms = (jb.tv_sec - ja.tv_sec) * 1e3 +
				(jb.tv_nsec - ja.tv_nsec) / 1e6;
			g_stage_direct_mjpeg = 1;
			if (g_jpeg_size < 0) {
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				if ((int)ts.tv_sec != g_jpeg_drop_log_sec) {
					g_jpeg_drop_log_sec = (int)ts.tv_sec;
					ilog("MJPEG encode exceeded max-frame-bytes=%d; dropping", g_max_frame_bytes);
				}
				g_jpeg_size = 0;
			}
			return mean;
		}
		if (g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_AREA ||
			g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_AREA_BOX ||
			g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_QUAD_BOX ||
			g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_QUAD4 ||
			g_mjpeg_highlight_area > 0 ||
			g_mjpeg_highlight_box > 0)
			build_integral_tables();
		for (ty = 0; ty < oh; ty++) {
			uint8_t *row = g_frame + (size_t)ty * g_out_stride;
			for (tx = 0; tx < ow; tx++) {
				int R, Gg, Bb;
				int Y;
				uint8_t *p;
				uint16_t *luma10 = g_mjpeg_antibloom > 0 ?
					g_luma10 + (size_t)ty * ow + tx : NULL;
				if (g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_AREA)
					bayer_area_rgb(tx, ty, ow, oh, rgain, bgain, &R, &Gg, &Bb, luma10);
				else if (g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_AREA_FRAC)
					bayer_area_frac_rgb(tx, ty, ow, oh, rgain, bgain, &R, &Gg, &Bb, luma10);
				else if (g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_AREA_FRAC_4TAP)
					bayer_area_frac_4tap_rgb(tx, ty, ow, oh, rgain, bgain, &R, &Gg, &Bb, luma10);
				else if (g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_AREA_BOX)
					bayer_area_box_rgb(tx, ty, ow, oh, rgain, bgain, &R, &Gg, &Bb, luma10);
				else if (g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_QUAD_BOX)
					bayer_quad_box_rgb(tx, ty, ow, oh, rgain, bgain, &R, &Gg, &Bb, luma10);
				else if (g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_QUAD4)
					bayer_quad4_rgb(tx, ty, ow, oh, rgain, bgain, &R, &Gg, &Bb, luma10);
				else if (g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_CENTER)
					bayer_center_rgb(tx, ty, ow, oh, rgain, bgain, &R, &Gg, &Bb, luma10);
				else
					demosaic_scaled_rgb(tx, ty, ow, oh, rgain, bgain, &R, &Gg, &Bb, luma10);
				Y = (77 * R + 150 * Gg + 29 * Bb) >> 8;
				if (g_mjpeg_blend_frac > 0 && Y >= g_mjpeg_blend_threshold &&
					g_mjpeg_scale_mode != MJPEG_SCALE_BAYER_AREA_FRAC) {
					int fR, fG, fB, local;
					bayer_area_frac_rgb(tx, ty, ow, oh, rgain, bgain, &fR, &fG, &fB, NULL);
					local = ((Y - g_mjpeg_blend_threshold) * g_mjpeg_blend_frac) /
						(255 - g_mjpeg_blend_threshold + 1);
					if (local > g_mjpeg_blend_frac)
						local = g_mjpeg_blend_frac;
					R = (R * (100 - local) + fR * local + 50) / 100;
					Gg = (Gg * (100 - local) + fG * local + 50) / 100;
					Bb = (Bb * (100 - local) + fB * local + 50) / 100;
					Y = (77 * R + 150 * Gg + 29 * Bb) >> 8;
				}
				if (g_mjpeg_highlight_area > 0 && Y >= g_mjpeg_highlight_area_threshold &&
					g_mjpeg_scale_mode != MJPEG_SCALE_BAYER_AREA) {
					int aR, aG, aB, ay, local, delta;
					bayer_area_ref_rgb(tx, ty, ow, oh, rgain, bgain, &aR, &aG, &aB);
					ay = (77 * aR + 150 * aG + 29 * aB) >> 8;
					delta = abs(Y - ay);
					if (delta >= g_mjpeg_highlight_area_delta) {
						local = ((Y - g_mjpeg_highlight_area_threshold) * g_mjpeg_highlight_area) /
							(255 - g_mjpeg_highlight_area_threshold + 1);
						local += ((delta - g_mjpeg_highlight_area_delta) * g_mjpeg_highlight_area) /
							(255 - g_mjpeg_highlight_area_delta + 1);
						if (local > g_mjpeg_highlight_area)
							local = g_mjpeg_highlight_area;
						R = (R * (100 - local) + aR * local + 50) / 100;
						Gg = (Gg * (100 - local) + aG * local + 50) / 100;
						Bb = (Bb * (100 - local) + aB * local + 50) / 100;
						Y = (77 * R + 150 * Gg + 29 * Bb) >> 8;
					}
				}
				if (g_mjpeg_highlight_box > 0 && Y >= g_mjpeg_highlight_box_threshold) {
					int bR, bG, bB, by, local, delta;
					bayer_area_box_scaled_rgb(tx, ty, ow, oh, rgain, bgain,
								  g_mjpeg_highlight_box_area,
								  &bR, &bG, &bB, NULL);
					by = (77 * bR + 150 * bG + 29 * bB) >> 8;
					delta = abs(Y - by);
					if (delta >= g_mjpeg_highlight_box_delta) {
						local = ((Y - g_mjpeg_highlight_box_threshold) * g_mjpeg_highlight_box) /
							(255 - g_mjpeg_highlight_box_threshold + 1);
						local += ((delta - g_mjpeg_highlight_box_delta) * g_mjpeg_highlight_box) /
							(255 - g_mjpeg_highlight_box_delta + 1);
						if (local > g_mjpeg_highlight_box)
							local = g_mjpeg_highlight_box;
						R = (R * (100 - local) + bR * local + 50) / 100;
						Gg = (Gg * (100 - local) + bG * local + 50) / 100;
						Bb = (Bb * (100 - local) + bB * local + 50) / 100;
						Y = (77 * R + 150 * Gg + 29 * Bb) >> 8;
					}
				}
				p = row + tx * 3;
				p[0] = (uint8_t)R;
				p[1] = (uint8_t)Gg;
				p[2] = (uint8_t)Bb;
				lumasum += Y > 255 ? 255 : Y;
			}
		}
		if (g_mjpeg_smooth > 0) {
			smooth_rgb(g_filter, g_frame, ow, oh, g_out_stride, g_mjpeg_smooth);
			jpeg_src = g_filter;
		}
		if (g_mjpeg_highlight_smooth > 0) {
			uint8_t *dst = jpeg_src == g_filter ? g_filter2 : g_filter;
			highlight_smooth_rgb(dst, jpeg_src, ow, oh, g_out_stride,
						 g_mjpeg_highlight_smooth, g_mjpeg_highlight_threshold,
						 g_mjpeg_highlight_radius);
			jpeg_src = dst;
		}
		if (g_mjpeg_desaturate_highlights > 0) {
			uint8_t *dst = jpeg_src == g_filter ? g_filter2 : g_filter;
			desaturate_highlights_rgb(dst, jpeg_src, ow, oh, g_out_stride,
						   g_mjpeg_desaturate_highlights,
						   g_mjpeg_highlight_threshold);
			jpeg_src = dst;
		}
		if (g_mjpeg_antibloom > 0) {
			uint8_t *dst = jpeg_src == g_filter ? g_filter2 : g_filter;
			antibloom_highlights_rgb(dst, jpeg_src, g_luma10, ow, oh, g_out_stride,
						 g_mjpeg_antibloom, g_mjpeg_antibloom_threshold,
						 g_mjpeg_antibloom_radius);
			jpeg_src = dst;
		}
		if (g_mjpeg_edge_despeckle > 0) {
			uint8_t *dst = jpeg_src == g_filter ? g_filter2 : g_filter;
			edge_despeckle_rgb(dst, jpeg_src, ow, oh, g_out_stride,
					   g_mjpeg_edge_despeckle, g_mjpeg_edge_despeckle_threshold,
					   g_mjpeg_edge_despeckle_radius);
			jpeg_src = dst;
		}
		if (g_sharpen > 0) {
			sharpen_rgb(g_sharp, jpeg_src, ow, oh, g_out_stride, g_sharpen);
			jpeg_src = g_sharp;
		}
		{
			struct timespec ja, jb;
			clock_gettime(CLOCK_MONOTONIC, &ja);
			if (g_mjpeg_subsampling == 444)
				g_jpeg_size = lmi_jpeg_encode_rgb444(g_jpeg, (size_t)g_max_frame_bytes,
									   jpeg_src, ow, oh, g_out_stride, g_mjpeg_quality);
			else
				g_jpeg_size = lmi_jpeg_encode_rgb420(g_jpeg, (size_t)g_max_frame_bytes,
									   jpeg_src, ow, oh, g_out_stride, g_mjpeg_quality);
			clock_gettime(CLOCK_MONOTONIC, &jb);
			g_stage_jpeg_ms = (jb.tv_sec - ja.tv_sec) * 1e3 +
				(jb.tv_nsec - ja.tv_nsec) / 1e6;
		}
		if (g_jpeg_size < 0) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			if ((int)ts.tv_sec != g_jpeg_drop_log_sec) {
				g_jpeg_drop_log_sec = (int)ts.tv_sec;
				ilog("MJPEG encode exceeded max-frame-bytes=%d; dropping", g_max_frame_bytes);
			}
			g_jpeg_size = 0;
		}
		return lumasum / ((long)ow * oh);
	}
	if (g_nv12) {
		uint8_t *yp = g_frame;
		uint8_t *uvp = g_frame + (size_t)g_out_stride * oh;
		for (ty = 0; ty < oh; ty++) {
			int sy = source_view_coord_scaled(ty, oh, g_src_h, g_src_y);
			uint8_t *yrow = yp + (size_t)ty * g_out_stride;
			uint8_t *uvrow = uvp + (size_t)(ty >> 1) * g_out_stride;
			for (tx = 0; tx < ow; tx++) {
				int sx = source_view_coord_scaled(tx, ow, g_src_w, g_src_x);
				int R, Gg, Bb;
				demosaic_at(sx, sy, rgain, bgain, &R, &Gg, &Bb);
				int Y = (77 * R + 150 * Gg + 29 * Bb) >> 8;
				yrow[tx] = Y > 255 ? 255 : Y;
				lumasum += Y;
				if (!(ty & 1) && !(tx & 1)) {
					int U = (((-43 * R - 85 * Gg + 128 * Bb) >> 8) + 128);
					int V = (((128 * R - 107 * Gg - 21 * Bb) >> 8) + 128);
					uvrow[tx] = bclamp(U, 0, 255);
					uvrow[tx + 1] = bclamp(V, 0, 255);
				}
			}
		}
		draw_motion_overlay_nv12();
		return lumasum / ((long)ow * oh);
	}
	/* YUYV */
	(void)aw;
	for (ty = 0; ty < oh; ty++) {
		uint8_t *row = g_frame + (size_t)ty * g_out_stride;
		for (tx = 0; tx < ow; tx += 2) {
			int lx0, ly0, lx1, ly1;
			rot_logical(tx, ty, &lx0, &ly0);
			rot_logical(tx + 1, ty, &lx1, &ly1);
			int sx0 = source_view_coord_scaled(lx0, g_logical_w, g_src_w, g_src_x);
			int sy0 = source_view_coord_scaled(ly0, g_logical_h, g_src_h, g_src_y);
			int sx1 = source_view_coord_scaled(lx1, g_logical_w, g_src_w, g_src_x);
			int sy1 = source_view_coord_scaled(ly1, g_logical_h, g_src_h, g_src_y);
			int R0, Gg0, Bb0, R1, Gg1, Bb1;
			demosaic_at(sx0, sy0, rgain, bgain, &R0, &Gg0, &Bb0);
			demosaic_at(sx1, sy1, rgain, bgain, &R1, &Gg1, &Bb1);
			int Y0 = (77 * R0 + 150 * Gg0 + 29 * Bb0) >> 8;
			int Y1 = (77 * R1 + 150 * Gg1 + 29 * Bb1) >> 8;
			int ar = (R0 + R1) >> 1, ag = (Gg0 + Gg1) >> 1, ab = (Bb0 + Bb1) >> 1;
			int U = (((-43 * ar - 85 * ag + 128 * ab) >> 8) + 128);
			int V = (((128 * ar - 107 * ag - 21 * ab) >> 8) + 128);
			uint8_t *p = row + tx * 2;
			p[0] = Y0 > 255 ? 255 : Y0;
			p[1] = bclamp(U, 0, 255);
			p[2] = Y1 > 255 ? 255 : Y1;
			p[3] = bclamp(V, 0, 255);
			lumasum += Y0 + Y1;
		}
	}
	denoise_yuyv();
	return lumasum / ((long)ow * oh);
}

/* AE meter uses the raw green mean (pre-gamma), independent of the tone curve */
static int raw_mean_luma(void)
{
	uint64_t s = 0, n = 0;
	int y, x, left, top, right, bottom;
	/* Average both green sites from sparse 2x2 quads; sampling only one parity can
	 * alias with row/column structure and make AE look stuck under real scenes. */
	meter_roi_bounds(&left, &top, &right, &bottom);
	for (y = top; y + 1 < bottom; y += 8) {
		for (x = left; x + 1 < right; x += 8) {
			s += g_bayer[(size_t)y * g_raw_w + x];
			s += g_bayer[(size_t)(y + 1) * g_raw_w + x + 1];
			n += 2;
		}
	}
	if (!n)
		return 0;
	int v = (int)(s / n) - g_blacklevel;
	int maxv = 1023 - g_blacklevel;
	if (maxv < 1)
		maxv = 1;
	if (v < 0)
		v = 0;
	return (v * 255) / maxv;
}

/* ---------------- RAW capture ---------------- */

static int raw_start(void)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers rb;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	int i;

	g_rawfd = open(g_raw, O_RDWR | O_NONBLOCK);
	if (g_rawfd < 0) { ilog("open raw %s: %s", g_raw, strerror(errno)); return -1; }

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = type;
	if (xioctl(g_rawfd, VIDIOC_G_FMT, &fmt) < 0) { ilog("G_FMT: %s", strerror(errno)); return -1; }
	g_raw_w = fmt.fmt.pix_mp.width;
	g_raw_h = fmt.fmt.pix_mp.height;
	g_raw_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	g_raw_sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	{
		uint32_t pix = fmt.fmt.pix_mp.pixelformat;
		ilog("raw %s: %dx%d stride=%d size=%d fourcc=%c%c%c%c", g_raw, g_raw_w, g_raw_h,
			 g_raw_stride, g_raw_sizeimage,
			 (char)(pix & 0xff), (char)((pix >> 8) & 0xff),
			 (char)((pix >> 16) & 0xff), (char)((pix >> 24) & 0xff));
		if (pix != V4L2_PIX_FMT_SGRBG10P) {
			ilog("unsupported raw fourcc: expected pgAA packed RAW10");
			return -1;
		}
		if (g_raw_stride < ((g_raw_w + 3) / 4) * 5) {
			ilog("raw stride too small for packed RAW10 width");
			return -1;
		}
	}

	memset(&rb, 0, sizeof(rb));
	rb.count = NBUF;
	rb.type = type;
	rb.memory = V4L2_MEMORY_MMAP;
	if (xioctl(g_rawfd, VIDIOC_REQBUFS, &rb) < 0) { ilog("REQBUFS: %s", strerror(errno)); return -1; }
	g_nrb = rb.count;
	for (i = 0; i < g_nrb; i++) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[MAXPLANES];
		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.length = MAXPLANES;
		buf.m.planes = planes;
		if (xioctl(g_rawfd, VIDIOC_QUERYBUF, &buf) < 0) { ilog("QUERYBUF: %s", strerror(errno)); return -1; }
		g_rb[i].len = planes[0].length;
		g_rb[i].start = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
					 g_rawfd, planes[0].m.mem_offset);
		if (g_rb[i].start == MAP_FAILED) { ilog("mmap: %s", strerror(errno)); return -1; }
		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
		buf.length = MAXPLANES; buf.m.planes = planes;
		if (xioctl(g_rawfd, VIDIOC_QBUF, &buf) < 0) { ilog("QBUF: %s", strerror(errno)); return -1; }
	}
	if (xioctl(g_rawfd, VIDIOC_STREAMON, &type) < 0) { ilog("STREAMON: %s", strerror(errno)); return -1; }
	return 0;
}

/* ---------------- UVC compressed FIFO records ---------------- */

struct lmi_uvc_record_header {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t payload_size;
	uint32_t sequence;
};

static int write_all_nonblock(int fd, const uint8_t *data, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, data + off, len - off);
		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return -1;
		return -1;
	}
	return 0;
}

/* ---------------- output sink ---------------- */

static int out_open(void)
{
	if (g_rotate != 0 && g_rotate != 90 && g_rotate != 180 && g_rotate != 270) {
		ilog("ignoring unsupported --rotate %d (only 0/90/180/270)", g_rotate);
		g_rotate = 0;
	}
	if (g_rotate && (g_nv12 || g_mjpeg)) {
		ilog("--rotate only applies to the YUYV loopback path; ignoring for nv12/mjpeg");
		g_rotate = 0;
	}
	/* The parsed --out-width/height is the LOGICAL (pre-rotation) frame the source
	 * sampling maps into; 90/270 swap W/H for the physical loopback frame. */
	g_logical_w = g_out_w & ~1;
	g_logical_h = (g_nv12 || g_mjpeg) ? (g_out_h & ~1) : g_out_h;
	if (g_rotate == 90 || g_rotate == 270) {
		g_out_w = g_logical_h;
		g_out_h = g_logical_w;
	} else {
		g_out_w = g_logical_w;
		g_out_h = g_logical_h;
	}
	g_out_w &= ~1;
	if (g_nv12 || g_mjpeg)
		g_out_h &= ~1;
	ilog("orientation: rotate=%d (logical %dx%d -> frame %dx%d) denoise=%d/thr%d",
		 g_rotate, g_logical_w, g_logical_h, g_out_w, g_out_h, g_denoise, g_denoise_thresh);
	if (g_mjpeg) {
		g_out_stride = g_out_w * 3;
		g_out_size = g_out_stride * g_out_h;
		if (g_max_frame_bytes <= 0)
			g_max_frame_bytes = g_out_w * g_out_h;
	} else {
		g_out_stride = g_out_w * (g_nv12 ? 1 : 2);
		g_out_size = g_nv12 ? (g_out_stride * g_out_h * 3 / 2) : (g_out_stride * g_out_h);
	}
	if (g_loopback[0]) {
		struct v4l2_format fmt;
		if (g_mjpeg) {
			ilog("MJPEG output is only supported for --fifo UVC sink");
			return -1;
		}
		g_outfd = open(g_loopback, O_RDWR);
		if (g_outfd < 0) { ilog("open loopback %s: %s", g_loopback, strerror(errno)); return -1; }
		memset(&fmt, 0, sizeof(fmt));
		fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		fmt.fmt.pix.width = g_out_w;
		fmt.fmt.pix.height = g_out_h;
		fmt.fmt.pix.pixelformat = g_nv12 ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_YUYV;
		fmt.fmt.pix.field = V4L2_FIELD_NONE;
		fmt.fmt.pix.bytesperline = g_out_stride;
		fmt.fmt.pix.sizeimage = g_out_size;
		if (xioctl(g_outfd, VIDIOC_S_FMT, &fmt) < 0)
			ilog("loopback S_FMT (non-fatal): %s", strerror(errno));
		else {
			if (fmt.fmt.pix.bytesperline)
				g_out_stride = (int)fmt.fmt.pix.bytesperline;
			if (fmt.fmt.pix.sizeimage)
				g_out_size = (int)fmt.fmt.pix.sizeimage;
		}
		ilog("loopback %s: %dx%d stride=%d size=%d", g_loopback, g_out_w, g_out_h, g_out_stride, g_out_size);
	} else if (g_fifo[0]) {
		int cap;
		int pipe_target = g_mjpeg ?
			(g_max_frame_bytes + (int)sizeof(struct lmi_uvc_record_header)) * 3 :
			g_out_size * 2;
		g_outfd = open(g_fifo, O_WRONLY | (g_mjpeg ? 0 : 0));
		if (g_outfd < 0) { ilog("open fifo %s: %s", g_fifo, strerror(errno)); return -1; }
		cap = fcntl(g_outfd, F_SETPIPE_SZ, pipe_target);
		if (cap < 0)
			cap = fcntl(g_outfd, F_GETPIPE_SZ);
		if (cap <= 0) {
			ilog("fifo pipe capacity cannot be detected");
			return -1;
		}
		g_pipe_cap = cap;
		if (!g_mjpeg && g_pipe_cap < g_out_size)
			ilog("fifo pipe capacity %d is smaller than raw frame size %d; using blocking full-frame writes",
				 g_pipe_cap, g_out_size);
		if (g_mjpeg && g_pipe_cap < (int)sizeof(struct lmi_uvc_record_header) + 4096) {
			ilog("fifo pipe capacity %d is too small for MJPEG records", g_pipe_cap);
			return -1;
		}
		if (g_mjpeg && g_pipe_cap < g_max_frame_bytes + (int)sizeof(struct lmi_uvc_record_header))
			ilog("fifo pipe capacity %d is smaller than max-frame-bytes %d; larger MJPEG frames will be dropped",
				 g_pipe_cap, g_max_frame_bytes);
		ilog("fifo %s: %dx%d stride=%d size=%d pipe_cap=%d fmt=%s",
			 g_fifo, g_out_w, g_out_h, g_out_stride,
			 g_mjpeg ? g_max_frame_bytes : g_out_size, g_pipe_cap,
			 g_mjpeg ? "MJPEG" : (g_nv12 ? "NV12" : "YUYV"));
	} else if (g_dump[0]) {
		if (g_mjpeg) {
			ilog("MJPEG output is only supported for --fifo UVC sink");
			return -1;
		}
		g_outfd = open(g_dump, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (g_outfd < 0) { ilog("open dump %s: %s", g_dump, strerror(errno)); return -1; }
		ilog("dump %s: %dx%d stride=%d size=%d frames=%d", g_dump, g_out_w, g_out_h, g_out_stride, g_out_size, g_dump_frames);
	} else {
		ilog("no output sink (--loopback, --fifo, or --dump required)");
		return -1;
	}
	return 0;
}


static void out_write(void)
{
	size_t payload_size = g_mjpeg ? (size_t)g_jpeg_size : (size_t)g_out_size;
	const uint8_t *payload = g_mjpeg ? g_jpeg : g_frame;

	if (g_outfd < 0)
		return;
	if (g_mjpeg && g_jpeg_size <= 0)
		return;
	if (g_fifo[0] && g_mjpeg) {
		int inpipe = 0;
		size_t need = payload_size + sizeof(struct lmi_uvc_record_header);
		ioctl(g_outfd, FIONREAD, &inpipe);
		if (g_pipe_cap - inpipe < (int)need) {
			struct timespec ts;
			g_fifo_backpressure_drops++;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			if ((int)ts.tv_sec != g_fifo_backpressure_log_sec) {
				g_fifo_backpressure_log_sec = (int)ts.tv_sec;
				if ((int)need > g_pipe_cap) {
					g_mjpeg_pipe_short_log_sec = (int)ts.tv_sec;
					ilog("MJPEG record %zu bytes exceeds fifo pipe capacity %d; drops=%u", need, g_pipe_cap, g_fifo_backpressure_drops);
				} else {
					ilog("FIFO backpressure drop: need=%zu inpipe=%d cap=%d drops=%u", need, inpipe, g_pipe_cap, g_fifo_backpressure_drops);
				}
			}
			return; /* drop if feeder behind */
		}
	}
	if (g_mjpeg && g_fifo[0]) {
		uint8_t *p = g_jpeg - sizeof(struct lmi_uvc_record_header);
		struct lmi_uvc_record_header *hdr = (struct lmi_uvc_record_header *)p;
		hdr->magic = LMI_UVC_RECORD_MAGIC;
		hdr->version = LMI_UVC_RECORD_VERSION;
		hdr->header_size = sizeof(*hdr);
		hdr->payload_size = (uint32_t)payload_size;
		hdr->sequence = ++g_frame_seq;
		(void)payload;
		(void)write_all_nonblock(g_outfd, p, sizeof(*hdr) + payload_size);
		return;
	}
	{
		size_t off = 0;
		while (off < payload_size) {
			ssize_t n = write(g_outfd, payload + off, payload_size - off);
			if (n > 0) { off += (size_t)n; continue; }
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				if (g_fifo[0]) return; /* non-blocking fifo: drop rest */
				struct pollfd p = { g_outfd, POLLOUT, 0 };
				poll(&p, 1, 100);
				continue;
			}
			return; /* EPIPE etc. */
		}
	}
}


static volatile int g_run = 1;
#include <signal.h>
static void on_sig(int s) { (void)s; g_run = 0; }

static int parse_cpu_range(const char *arg, int *first, int *last)
{
	char *end;
	long a, b;

	errno = 0;
	a = strtol(arg, &end, 10);
	if (errno || end == arg || a < 0)
		return -1;
	if (*end == '\0')
		b = a;
	else if (*end == '-') {
		const char *p = end + 1;
		errno = 0;
		b = strtol(p, &end, 10);
		if (errno || end == p || *end != '\0' || b < a)
			return -1;
	} else {
		return -1;
	}
	if (a >= CPU_SETSIZE || b >= CPU_SETSIZE)
		return -1;
	*first = (int)a;
	*last = (int)b;
	return 0;
}

static void pin_cpu_range(void)
{
	cpu_set_t set;

	if (g_cpu_first < 0)
		return;
	CPU_ZERO(&set);
	for (int cpu = g_cpu_first; cpu <= g_cpu_last; cpu++)
		CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) < 0)
		ilog("cpu affinity %d-%d failed: %s", g_cpu_first, g_cpu_last, strerror(errno));
	else if (g_verbose || g_mjpeg)
		ilog("cpu affinity: %d-%d", g_cpu_first, g_cpu_last);
}

int main(int argc, char **argv)
{
	int i, frames = 0, total_frames = 0, ae_div = 0;
	struct timespec t0, tr;
	struct perf_stats ps;
	uint32_t last_raw_seq = 0;
	int have_raw_seq = 0;
	double rgain = 1.0, bgain = 1.0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--raw") && i + 1 < argc) g_raw = argv[++i];
		else if (!strcmp(argv[i], "--ctrl") && i + 1 < argc) g_ctrl = argv[++i];
		else if (!strcmp(argv[i], "--loopback") && i + 1 < argc) g_loopback = argv[++i];
		else if (!strcmp(argv[i], "--fifo") && i + 1 < argc) g_fifo = argv[++i];
		else if (!strcmp(argv[i], "--control-fifo") && i + 1 < argc) g_control_fifo = argv[++i];
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc) g_dump = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_dump_frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--out-width") && i + 1 < argc) g_out_w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--out-height") && i + 1 < argc) g_out_h = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) g_rotate = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--denoise") && i + 1 < argc) g_denoise = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--denoise-threshold") && i + 1 < argc) g_denoise_thresh = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--nv12")) g_nv12 = 1;
		else if (!strcmp(argv[i], "--mjpeg")) g_mjpeg = 1;
		else if (!strcmp(argv[i], "--mjpeg-quality") && i + 1 < argc) g_mjpeg_quality = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-sharpen") && i + 1 < argc) g_sharpen = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-smooth") && i + 1 < argc) g_mjpeg_smooth = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-highlight-smooth") && i + 1 < argc) g_mjpeg_highlight_smooth = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-desaturate-highlights") && i + 1 < argc) g_mjpeg_desaturate_highlights = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-antibloom") && i + 1 < argc) g_mjpeg_antibloom = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-antibloom-threshold") && i + 1 < argc) g_mjpeg_antibloom_threshold = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-antibloom-radius") && i + 1 < argc) g_mjpeg_antibloom_radius = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-edge-despeckle") && i + 1 < argc) g_mjpeg_edge_despeckle = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-edge-despeckle-threshold") && i + 1 < argc) g_mjpeg_edge_despeckle_threshold = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-edge-despeckle-radius") && i + 1 < argc) g_mjpeg_edge_despeckle_radius = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-highlight-threshold") && i + 1 < argc) g_mjpeg_highlight_threshold = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-highlight-radius") && i + 1 < argc) g_mjpeg_highlight_radius = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-area-scale") && i + 1 < argc) g_mjpeg_area_scale = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-subsampling") && i + 1 < argc) g_mjpeg_subsampling = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-fast-threads") && i + 1 < argc) g_mjpeg_fast_threads = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cpu-affinity") && i + 1 < argc) {
			if (parse_cpu_range(argv[++i], &g_cpu_first, &g_cpu_last) < 0) {
				fprintf(stderr, "invalid --cpu-affinity, expected N or N-M\n");
				return 2;
			}
		}
		else if (!strcmp(argv[i], "--mjpeg-bayer-despeckle") && i + 1 < argc) g_mjpeg_bayer_despeckle = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-blend-frac") && i + 1 < argc) g_mjpeg_blend_frac = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-blend-threshold") && i + 1 < argc) g_mjpeg_blend_threshold = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-highlight-area") && i + 1 < argc) g_mjpeg_highlight_area = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-highlight-area-threshold") && i + 1 < argc) g_mjpeg_highlight_area_threshold = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-highlight-area-delta") && i + 1 < argc) g_mjpeg_highlight_area_delta = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-highlight-box") && i + 1 < argc) g_mjpeg_highlight_box = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-highlight-box-threshold") && i + 1 < argc) g_mjpeg_highlight_box_threshold = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-highlight-box-delta") && i + 1 < argc) g_mjpeg_highlight_box_delta = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-highlight-box-area") && i + 1 < argc) g_mjpeg_highlight_box_area = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mjpeg-scale-mode") && i + 1 < argc) {
			const char *m = argv[++i];
			if (!strcmp(m, "bayer-area"))
				g_mjpeg_scale_mode = MJPEG_SCALE_BAYER_AREA;
			else if (!strcmp(m, "bayer-area-frac"))
				g_mjpeg_scale_mode = MJPEG_SCALE_BAYER_AREA_FRAC;
			else if (!strcmp(m, "bayer-area-frac-4tap"))
				g_mjpeg_scale_mode = MJPEG_SCALE_BAYER_AREA_FRAC_4TAP;
			else if (!strcmp(m, "bayer-area-box"))
				g_mjpeg_scale_mode = MJPEG_SCALE_BAYER_AREA_BOX;
			else if (!strcmp(m, "bayer-quad-box"))
				g_mjpeg_scale_mode = MJPEG_SCALE_BAYER_QUAD_BOX;
			else if (!strcmp(m, "bayer-quad4"))
				g_mjpeg_scale_mode = MJPEG_SCALE_BAYER_QUAD4;
			else if (!strcmp(m, "bayer-center"))
				g_mjpeg_scale_mode = MJPEG_SCALE_BAYER_CENTER;
			else if (!strcmp(m, "demosaic-center"))
				g_mjpeg_scale_mode = MJPEG_SCALE_DEMOSAIC_CENTER;
			else if (!strcmp(m, "demosaic-4tap"))
				g_mjpeg_scale_mode = MJPEG_SCALE_DEMOSAIC_4TAP;
			else if (!strcmp(m, "demosaic-9tap"))
				g_mjpeg_scale_mode = MJPEG_SCALE_DEMOSAIC_9TAP;
			else { fprintf(stderr, "unknown --mjpeg-scale-mode: %s\n", m); return 2; }
		}
		else if (!strcmp(argv[i], "--max-frame-bytes") && i + 1 < argc) g_max_frame_bytes = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--source-aspect") && i + 1 < argc) {
			const char *m = argv[++i];
			if (!strcmp(m, "stretch"))
				g_source_aspect = SOURCE_ASPECT_STRETCH;
			else if (!strcmp(m, "preserve"))
				g_source_aspect = SOURCE_ASPECT_PRESERVE;
			else { fprintf(stderr, "unknown --source-aspect: %s\n", m); return 2; }
		}
		else if (!strcmp(argv[i], "--gamma") && i + 1 < argc) g_gamma = atof(argv[++i]);
		else if (!strcmp(argv[i], "--tone-highlight-knee") && i + 1 < argc) g_tone_highlight_knee = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--tone-highlight-max") && i + 1 < argc) g_tone_highlight_max = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--black-level") && i + 1 < argc) g_blacklevel = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--fps-cap") && i + 1 < argc) g_fps_cap = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--auto-exposure")) g_auto_exposure = 1;
		else if (!strcmp(argv[i], "--target") && i + 1 < argc) g_target = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ae-clip-target") && i + 1 < argc) g_ae_clip_target = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ae-clip-weight") && i + 1 < argc) g_ae_clip_weight = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-awb")) g_awb = 0;
		else if (!strcmp(argv[i], "--no-auto-tone")) g_auto_tone = 0;
		else if (!strcmp(argv[i], "--max-digital-gain") && i + 1 < argc) g_dgain_limit = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--max-soft-gain") && i + 1 < argc) g_max_soft_gain = atof(argv[++i]);
		else if (!strcmp(argv[i], "--motion-overlay")) g_motion_overlay = 1;
		else if (!strcmp(argv[i], "--motion-overlay-size") && i + 1 < argc) g_motion_overlay_size = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-v")) g_verbose = 1;
		else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);
	pin_cpu_range();
	control_fifo_open();
	if (g_motion_overlay_size < 16)
		g_motion_overlay_size = 16;
	if (g_motion_overlay_size > 512)
		g_motion_overlay_size = 512;
	g_motion_overlay_size &= ~1;

	/* gamma LUT: 10-bit (post black-level range) -> 8-bit display */
	g_gamma_lut = malloc(1024);
	{
		int maxv = 1023 - g_blacklevel;
		if (maxv < 1) maxv = 1;
		double inv = (g_gamma > 0.01) ? 1.0 / g_gamma : 1.0;
		for (i = 0; i < 1024; i++) {
			double n = (double)i / maxv;
			if (n > 1.0) n = 1.0;
			int v = (int)(255.0 * pow(n, inv) + 0.5);
			g_gamma_lut[i] = v > 255 ? 255 : (v < 0 ? 0 : v);
		}
	}

	if (g_mjpeg_quality < 1) g_mjpeg_quality = 1;
	if (g_mjpeg_quality > 100) g_mjpeg_quality = 100;
	if (g_sharpen < 0) g_sharpen = 0;
	if (g_sharpen > 200) g_sharpen = 200;
	if (g_mjpeg_smooth < 0) g_mjpeg_smooth = 0;
	if (g_mjpeg_smooth > 100) g_mjpeg_smooth = 100;
	if (g_mjpeg_highlight_smooth < 0) g_mjpeg_highlight_smooth = 0;
	if (g_mjpeg_highlight_smooth > 100) g_mjpeg_highlight_smooth = 100;
	if (g_mjpeg_desaturate_highlights < 0) g_mjpeg_desaturate_highlights = 0;
	if (g_mjpeg_desaturate_highlights > 100) g_mjpeg_desaturate_highlights = 100;
	if (g_mjpeg_antibloom < 0) g_mjpeg_antibloom = 0;
	if (g_mjpeg_antibloom > 100) g_mjpeg_antibloom = 100;
	if (g_mjpeg_antibloom_threshold < 1) g_mjpeg_antibloom_threshold = 1;
	if (g_mjpeg_antibloom_threshold > 1023) g_mjpeg_antibloom_threshold = 1023;
	if (g_mjpeg_antibloom_radius < 1) g_mjpeg_antibloom_radius = 1;
	if (g_mjpeg_antibloom_radius > 16) g_mjpeg_antibloom_radius = 16;
	if (g_mjpeg_edge_despeckle < 0) g_mjpeg_edge_despeckle = 0;
	if (g_mjpeg_edge_despeckle > 100) g_mjpeg_edge_despeckle = 100;
	if (g_mjpeg_edge_despeckle_threshold < 1) g_mjpeg_edge_despeckle_threshold = 1;
	if (g_mjpeg_edge_despeckle_threshold > 255) g_mjpeg_edge_despeckle_threshold = 255;
	if (g_mjpeg_edge_despeckle_radius < 1) g_mjpeg_edge_despeckle_radius = 1;
	if (g_mjpeg_edge_despeckle_radius > 4) g_mjpeg_edge_despeckle_radius = 4;
	if (g_mjpeg_highlight_threshold < 0) g_mjpeg_highlight_threshold = 0;
	if (g_mjpeg_highlight_threshold > 255) g_mjpeg_highlight_threshold = 255;
	if (g_mjpeg_highlight_radius < 1) g_mjpeg_highlight_radius = 1;
	if (g_mjpeg_highlight_radius > 12) g_mjpeg_highlight_radius = 12;
	if (g_mjpeg_bayer_despeckle < 0) g_mjpeg_bayer_despeckle = 0;
	if (g_mjpeg_bayer_despeckle > 100) g_mjpeg_bayer_despeckle = 100;
	if (g_mjpeg_blend_frac < 0) g_mjpeg_blend_frac = 0;
	if (g_mjpeg_blend_frac > 100) g_mjpeg_blend_frac = 100;
	if (g_mjpeg_blend_threshold < 0) g_mjpeg_blend_threshold = 0;
	if (g_mjpeg_blend_threshold > 255) g_mjpeg_blend_threshold = 255;
	if (g_mjpeg_highlight_area < 0) g_mjpeg_highlight_area = 0;
	if (g_mjpeg_highlight_area > 100) g_mjpeg_highlight_area = 100;
	if (g_mjpeg_highlight_area_threshold < 0) g_mjpeg_highlight_area_threshold = 0;
	if (g_mjpeg_highlight_area_threshold > 255) g_mjpeg_highlight_area_threshold = 255;
	if (g_mjpeg_highlight_area_delta < 0) g_mjpeg_highlight_area_delta = 0;
	if (g_mjpeg_highlight_area_delta > 255) g_mjpeg_highlight_area_delta = 255;
	if (g_mjpeg_highlight_box < 0) g_mjpeg_highlight_box = 0;
	if (g_mjpeg_highlight_box > 100) g_mjpeg_highlight_box = 100;
	if (g_mjpeg_highlight_box_threshold < 0) g_mjpeg_highlight_box_threshold = 0;
	if (g_mjpeg_highlight_box_threshold > 255) g_mjpeg_highlight_box_threshold = 255;
	if (g_mjpeg_highlight_box_delta < 0) g_mjpeg_highlight_box_delta = 0;
	if (g_mjpeg_highlight_box_delta > 255) g_mjpeg_highlight_box_delta = 255;
	if (g_mjpeg_highlight_box_area < 25) g_mjpeg_highlight_box_area = 25;
	if (g_mjpeg_highlight_box_area > 1000) g_mjpeg_highlight_box_area = 1000;
	if (g_mjpeg_area_scale < 25) g_mjpeg_area_scale = 25;
	if (g_mjpeg_area_scale > 1000) g_mjpeg_area_scale = 1000;
	if (g_ae_clip_weight < 0)
		g_ae_clip_weight = 0;
	if (g_ae_clip_weight > 400)
		g_ae_clip_weight = 400;
	if (g_tone_highlight_knee < 0)
		g_tone_highlight_knee = 0;
	if (g_tone_highlight_knee > 254)
		g_tone_highlight_knee = 254;
	if (g_tone_highlight_max < 1)
		g_tone_highlight_max = 1;
	if (g_tone_highlight_max > 255)
		g_tone_highlight_max = 255;
	if (g_tone_highlight_max <= g_tone_highlight_knee)
		g_tone_highlight_knee = 0;
	if (g_mjpeg_subsampling != 444)
		g_mjpeg_subsampling = 420;
	if (g_mjpeg_scale_mode < MJPEG_SCALE_BAYER_AREA ||
		g_mjpeg_scale_mode > MJPEG_SCALE_DEMOSAIC_9TAP)
		g_mjpeg_scale_mode = MJPEG_SCALE_BAYER_AREA;
	if (g_mjpeg) {
		g_nv12 = 0;
		if (!g_fifo[0]) {
			ilog("--mjpeg currently requires --fifo UVC output");
			return 2;
		}
	}

	if (raw_start() < 0) return 1;
	source_view_compute();
	g_raw_copy = malloc((size_t)g_raw_sizeimage);
	g_bayer = malloc((size_t)g_raw_w * g_raw_h * sizeof(uint16_t));
	if (out_open() < 0) return 1;
	g_frame = calloc(1, g_out_size);
	if (g_denoise > 0 && !g_nv12 && !g_mjpeg)
		g_prev_frame = calloc(1, g_out_size);
	if (g_mjpeg) {
		uint8_t *jpeg_alloc = malloc((size_t)g_max_frame_bytes + sizeof(struct lmi_uvc_record_header));
		if (jpeg_alloc)
			g_jpeg = jpeg_alloc + sizeof(struct lmi_uvc_record_header);
		if (g_mjpeg_antibloom > 0)
			g_luma10 = malloc((size_t)g_out_w * g_out_h * sizeof(*g_luma10));
		if (g_mjpeg_smooth > 0 || g_mjpeg_highlight_smooth > 0 ||
			g_mjpeg_desaturate_highlights > 0 || g_mjpeg_antibloom > 0 ||
			g_mjpeg_edge_despeckle > 0)
			g_filter = malloc((size_t)g_out_size);
		if ((g_mjpeg_smooth > 0 && g_mjpeg_highlight_smooth > 0) ||
			((g_mjpeg_smooth > 0 || g_mjpeg_highlight_smooth > 0) && g_mjpeg_desaturate_highlights > 0) ||
			((g_mjpeg_smooth > 0 || g_mjpeg_highlight_smooth > 0 || g_mjpeg_desaturate_highlights > 0) &&
			 g_mjpeg_antibloom > 0) ||
			((g_mjpeg_smooth > 0 || g_mjpeg_highlight_smooth > 0 || g_mjpeg_desaturate_highlights > 0 ||
			  g_mjpeg_antibloom > 0) && g_mjpeg_edge_despeckle > 0))
			g_filter2 = malloc((size_t)g_out_size);
		if (g_sharpen > 0)
			g_sharp = malloc((size_t)g_out_size);
		if (ensure_integral_tables() < 0) {
			ilog("oom");
			return 1;
		}
	}
	if (!g_raw_copy || !g_bayer || !g_frame || (g_mjpeg && (!g_jpeg ||
		(g_mjpeg_antibloom > 0 && !g_luma10) ||
		((g_mjpeg_smooth > 0 || g_mjpeg_highlight_smooth > 0 ||
		  g_mjpeg_desaturate_highlights > 0 || g_mjpeg_antibloom > 0 ||
		  g_mjpeg_edge_despeckle > 0) && !g_filter) ||
		(((g_mjpeg_smooth > 0 && g_mjpeg_highlight_smooth > 0) ||
		  ((g_mjpeg_smooth > 0 || g_mjpeg_highlight_smooth > 0) && g_mjpeg_desaturate_highlights > 0) ||
		  ((g_mjpeg_smooth > 0 || g_mjpeg_highlight_smooth > 0 || g_mjpeg_desaturate_highlights > 0) &&
		   g_mjpeg_antibloom > 0) ||
		  ((g_mjpeg_smooth > 0 || g_mjpeg_highlight_smooth > 0 || g_mjpeg_desaturate_highlights > 0 ||
			g_mjpeg_antibloom > 0) && g_mjpeg_edge_despeckle > 0)) && !g_filter2) ||
		(g_sharpen > 0 && !g_sharp)))) { ilog("oom"); return 1; }
	ae_init();
	ilog("ready: raw %dx%d source=%d,%d %dx%d aspect=%s -> out %dx%d %s fps_cap=%d gamma=%.2f shoulder=%d/%d ae=%d mjpeg{quality=%d max=%d threads=%d direct_expected=%d sharpen=%d smooth=%d highlight=%d/%d/r%d desat=%d antibloom=%d/%d/r%d edge=%d/%d/r%d despeckle=%d blend=%d/%d harea=%d/%d/d%d hbox=%d/%d/d%d/a%d area=%d subsampling=%d scale=%s}",
		 g_raw_w, g_raw_h, g_src_x, g_src_y, g_src_w, g_src_h, source_aspect_name(),
		 g_out_w, g_out_h, g_mjpeg ? "MJPEG" : (g_nv12 ? "NV12" : "YUYV"),
		 g_fps_cap, g_gamma, g_tone_highlight_knee, g_tone_highlight_max,
		 g_auto_exposure, g_mjpeg ? g_mjpeg_quality : 0,
		 g_mjpeg ? g_max_frame_bytes : 0, g_mjpeg ? g_mjpeg_fast_threads : 0,
		 g_mjpeg && (g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_CENTER ||
			 g_mjpeg_scale_mode == MJPEG_SCALE_BAYER_AREA_FRAC) &&
			 g_mjpeg_subsampling == 420 && g_mjpeg_smooth == 0 &&
			 g_mjpeg_highlight_smooth == 0 && g_mjpeg_desaturate_highlights == 0 &&
			 g_mjpeg_antibloom == 0 && g_mjpeg_edge_despeckle == 0 &&
			 g_sharpen == 0 && g_mjpeg_blend_frac == 0 &&
			 g_mjpeg_highlight_area == 0 && g_mjpeg_highlight_box == 0 &&
			 g_mjpeg_bayer_despeckle == 0,
		 g_mjpeg ? g_sharpen : 0, g_mjpeg ? g_mjpeg_smooth : 0,
		 g_mjpeg ? g_mjpeg_highlight_smooth : 0, g_mjpeg ? g_mjpeg_highlight_threshold : 0,
		 g_mjpeg ? g_mjpeg_highlight_radius : 0, g_mjpeg ? g_mjpeg_desaturate_highlights : 0,
		 g_mjpeg ? g_mjpeg_antibloom : 0, g_mjpeg ? g_mjpeg_antibloom_threshold : 0,
		 g_mjpeg ? g_mjpeg_antibloom_radius : 0, g_mjpeg ? g_mjpeg_edge_despeckle : 0,
		 g_mjpeg ? g_mjpeg_edge_despeckle_threshold : 0, g_mjpeg ? g_mjpeg_edge_despeckle_radius : 0,
		 g_mjpeg ? g_mjpeg_bayer_despeckle : 0, g_mjpeg ? g_mjpeg_blend_frac : 0,
		 g_mjpeg ? g_mjpeg_blend_threshold : 0, g_mjpeg ? g_mjpeg_highlight_area : 0,
		 g_mjpeg ? g_mjpeg_highlight_area_threshold : 0, g_mjpeg ? g_mjpeg_highlight_area_delta : 0,
		 g_mjpeg ? g_mjpeg_highlight_box : 0, g_mjpeg ? g_mjpeg_highlight_box_threshold : 0,
		 g_mjpeg ? g_mjpeg_highlight_box_delta : 0, g_mjpeg ? g_mjpeg_highlight_box_area : 0,
		 g_mjpeg ? g_mjpeg_area_scale : 100,
		 g_mjpeg ? g_mjpeg_subsampling : 0, g_mjpeg ? mjpeg_scale_mode_name() : "none");

	clock_gettime(CLOCK_MONOTONIC, &t0);
	tr = t0;
	memset(&ps, 0, sizeof(ps));
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	while (g_run) {
		struct pollfd p = { g_rawfd, POLLIN, 0 };

		control_fifo_drain();
		struct v4l2_buffer buf;
		struct v4l2_plane planes[MAXPLANES];
		struct timespec t_loop, t_poll, t_dq, t_copy, t_qbuf, t_unpack, t_awb, t_pack, t_out, t_ae, t_sleep;
		double work_ms, budget_ms = g_fps_cap > 0 ? 1000.0 / g_fps_cap : 0.0;
		unsigned int drops_before = g_fifo_backpressure_drops;
		long mean;
		int pr;

		clock_gettime(CLOCK_MONOTONIC, &t_loop);
		pr = poll(&p, 1, 1000);
		clock_gettime(CLOCK_MONOTONIC, &t_poll);
		perf_add(&ps.poll_sum, &ps.poll_max, ts_ms(&t_loop, &t_poll));
		if (pr <= 0) {
			control_fifo_drain();
			if (pr == 0)
				ps.poll_timeouts++;
			continue;
		}
		control_fifo_drain();
		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type; buf.memory = V4L2_MEMORY_MMAP;
		buf.length = MAXPLANES; buf.m.planes = planes;
		if (xioctl(g_rawfd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN) {
				ps.dq_eagain++;
				continue;
			}
			ilog("DQBUF: %s", strerror(errno));
			break;
		}
		clock_gettime(CLOCK_MONOTONIC, &t_dq);
		perf_add(&ps.dq_sum, &ps.dq_max, ts_ms(&t_poll, &t_dq));
		if (buf.index >= (unsigned int)g_nrb || !g_rb[buf.index].start) {
			ilog("DQBUF returned invalid index=%u nbuf=%d", buf.index, g_nrb);
			break;
		}
		if (g_rb[buf.index].len < (size_t)g_raw_sizeimage) {
			ilog("raw buffer index=%u length=%zu smaller than sizeimage=%d", buf.index,
				 g_rb[buf.index].len, g_raw_sizeimage);
			break;
		}
		if (have_raw_seq && buf.sequence > last_raw_seq + 1)
			ps.raw_seq_lost += buf.sequence - last_raw_seq - 1;
		last_raw_seq = buf.sequence;
		have_raw_seq = 1;

		memcpy(g_raw_copy, g_rb[buf.index].start, (size_t)g_raw_sizeimage);
		clock_gettime(CLOCK_MONOTONIC, &t_copy);
		if (xioctl(g_rawfd, VIDIOC_QBUF, &buf) < 0) {
			ps.qbuf_errors++;
			ilog("QBUF index=%u seq=%u: %s", buf.index, buf.sequence, strerror(errno));
			break;
		}
		clock_gettime(CLOCK_MONOTONIC, &t_qbuf);
		perf_add(&ps.hold_sum, &ps.hold_max, ts_ms(&t_dq, &t_qbuf));
		perf_add(&ps.copy_sum, &ps.copy_max, ts_ms(&t_dq, &t_copy));
		perf_add(&ps.qbuf_sum, &ps.qbuf_max, ts_ms(&t_copy, &t_qbuf));
		unpack_raw10p(g_raw_copy);
		clock_gettime(CLOCK_MONOTONIC, &t_unpack);
		if (g_awb && (frames % 8) == 0)
			compute_awb(&rgain, &bgain);
		clock_gettime(CLOCK_MONOTONIC, &t_awb);
		mean = pack_output(rgain, bgain);
		clock_gettime(CLOCK_MONOTONIC, &t_pack);
		out_write();
		clock_gettime(CLOCK_MONOTONIC, &t_out);

		if ((g_auto_exposure || g_flicker_mode == FLICKER_MODE_AUTO) && (++ae_div % 3) == 0) {
			if (g_flicker_mode == FLICKER_MODE_AUTO) {
				struct flicker_stats st = flicker_analyze_frame();
				flicker_update_state(&st);
			}
			if (g_auto_exposure) {
				g_ae_last_log = raw_mean_luma();
				tone_update(g_ae_last_log);
				ae_update(g_ae_last_log);
			}
		}
		clock_gettime(CLOCK_MONOTONIC, &t_ae);

		frames++;
		total_frames++;
		ps.frames++;
		if (g_stage_direct_mjpeg)
			ps.direct_mjpeg++;
		if (g_fifo_backpressure_drops > drops_before)
			ps.fifo_drops += g_fifo_backpressure_drops - drops_before;
		perf_add(&ps.unpack_sum, &ps.unpack_max, ts_ms(&t_qbuf, &t_unpack));
		perf_add(&ps.awb_sum, &ps.awb_max, ts_ms(&t_unpack, &t_awb));
		perf_add(&ps.pack_sum, &ps.pack_max, ts_ms(&t_awb, &t_pack));
		perf_add(&ps.jpeg_sum, &ps.jpeg_max, g_stage_jpeg_ms);
		perf_add(&ps.out_sum, &ps.out_max, ts_ms(&t_pack, &t_out));
		perf_add(&ps.ae_sum, &ps.ae_max, ts_ms(&t_out, &t_ae));
		work_ms = ts_ms(&t_dq, &t_ae);
		perf_add(&ps.work_sum, &ps.work_max, work_ms);
		if (budget_ms > 0.0 && work_ms > budget_ms)
			ps.over_budget++;

		if (g_dump[0] && total_frames >= g_dump_frames)
			break;

		if (g_fps_cap > 0) {
			double spent = ts_sec(&t_dq, &t_ae);
			double budget = 1.0 / g_fps_cap;
			if (spent < budget) {
				struct timespec ts = { 0, (long)((budget - spent) * 1e9) };
				nanosleep(&ts, NULL);
				clock_gettime(CLOCK_MONOTONIC, &t_sleep);
				perf_add(&ps.sleep_sum, &ps.sleep_max, ts_ms(&t_ae, &t_sleep));
			}
		}

		clock_gettime(CLOCK_MONOTONIC, &tr);
		double el = ts_sec(&t0, &tr);
		if (el >= 2.0) {
			if (g_auto_exposure)
				ilog("fps=%.1f frame_ms{avg=%.1f max=%.1f budget=%.1f over=%u} raw{poll=%.1f/%.1f dq=%.2f/%.2f hold=%.1f/%.1f copy=%.1f/%.1f qbuf=%.2f/%.2f lost=%u timeout=%u eagain=%u qerr=%u} stage_avg/max{unpack=%.1f/%.1f awb=%.2f/%.2f pack=%.1f/%.1f jpeg=%.1f/%.1f out=%.1f/%.1f ae=%.2f/%.2f sleep=%.1f/%.1f direct=%u/%u fifo_drop=%u} outmean=%ld ae{mean=%d hi=%d exp=%d again=%d dgain=%d%s soft=%.1f} gains r=%.2f b=%.2f",
					 frames / el, perf_avg(ps.work_sum, ps.frames), ps.work_max, budget_ms, ps.over_budget,
					 perf_avg(ps.poll_sum, frames + ps.poll_timeouts), ps.poll_max,
					 perf_avg(ps.dq_sum, ps.frames), ps.dq_max,
					 perf_avg(ps.hold_sum, ps.frames), ps.hold_max,
					 perf_avg(ps.copy_sum, ps.frames), ps.copy_max,
					 perf_avg(ps.qbuf_sum, ps.frames), ps.qbuf_max,
					 ps.raw_seq_lost, ps.poll_timeouts, ps.dq_eagain, ps.qbuf_errors,
					 perf_avg(ps.unpack_sum, ps.frames), ps.unpack_max,
					 perf_avg(ps.awb_sum, ps.frames), ps.awb_max,
					 perf_avg(ps.pack_sum, ps.frames), ps.pack_max,
					 perf_avg(ps.jpeg_sum, ps.frames), ps.jpeg_max,
					 perf_avg(ps.out_sum, ps.frames), ps.out_max,
					 perf_avg(ps.ae_sum, ps.frames), ps.ae_max,
					 perf_avg(ps.sleep_sum, ps.frames), ps.sleep_max,
					 ps.direct_mjpeg, ps.frames, ps.fifo_drops,
					 mean, g_ae_last_log, g_ae_last_hi, g_exposure, g_again, g_dgain,
					 g_ae_changed ? " changed" : "", g_soft_gain, rgain, bgain);
			else
				ilog("fps=%.1f frame_ms{avg=%.1f max=%.1f budget=%.1f over=%u} raw{poll=%.1f/%.1f dq=%.2f/%.2f hold=%.1f/%.1f copy=%.1f/%.1f qbuf=%.2f/%.2f lost=%u timeout=%u eagain=%u qerr=%u} stage_avg/max{unpack=%.1f/%.1f awb=%.2f/%.2f pack=%.1f/%.1f jpeg=%.1f/%.1f out=%.1f/%.1f ae=%.2f/%.2f sleep=%.1f/%.1f direct=%u/%u fifo_drop=%u} outmean=%ld gains r=%.2f b=%.2f",
					 frames / el, perf_avg(ps.work_sum, ps.frames), ps.work_max, budget_ms, ps.over_budget,
					 perf_avg(ps.poll_sum, frames + ps.poll_timeouts), ps.poll_max,
					 perf_avg(ps.dq_sum, ps.frames), ps.dq_max,
					 perf_avg(ps.hold_sum, ps.frames), ps.hold_max,
					 perf_avg(ps.copy_sum, ps.frames), ps.copy_max,
					 perf_avg(ps.qbuf_sum, ps.frames), ps.qbuf_max,
					 ps.raw_seq_lost, ps.poll_timeouts, ps.dq_eagain, ps.qbuf_errors,
					 perf_avg(ps.unpack_sum, ps.frames), ps.unpack_max,
					 perf_avg(ps.awb_sum, ps.frames), ps.awb_max,
					 perf_avg(ps.pack_sum, ps.frames), ps.pack_max,
					 perf_avg(ps.jpeg_sum, ps.frames), ps.jpeg_max,
					 perf_avg(ps.out_sum, ps.frames), ps.out_max,
					 perf_avg(ps.ae_sum, ps.frames), ps.ae_max,
					 perf_avg(ps.sleep_sum, ps.frames), ps.sleep_max,
					 ps.direct_mjpeg, ps.frames, ps.fifo_drops,
					 mean, rgain, bgain);
			g_ae_changed = 0;
			frames = 0; t0 = tr;
			memset(&ps, 0, sizeof(ps));
		}
	}

	xioctl(g_rawfd, VIDIOC_STREAMOFF, &type);
	ilog("stopped");
	return 0;
}
