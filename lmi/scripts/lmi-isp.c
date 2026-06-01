// SPDX-License-Identifier: GPL-2.0
//
// lmi-isp.c -- fast C software ISP + streamer for the lmi OV13B10 camera.
//
// Captures RAW10 (MIPI packed GRBG, V4L2 fourcc "pgAA") from a CAMSS RDI video
// node whose media pipeline + sensor mode were already configured by
// lmi-camera-web-preview.py, runs a real software ISP in C
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
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "lmi-jpeg.h"

#ifndef F_GETPIPE_SZ
#define F_GETPIPE_SZ 1032
#endif
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif
#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN (V4L2_CID_IMAGE_SOURCE_CLASS_BASE + 5)
#endif

#define NBUF 4
#define MAXPLANES 1
#define LMI_UVC_RECORD_MAGIC 0x43564d4cU /* "LMVC" little-endian */
#define LMI_UVC_RECORD_VERSION 1

/* ---- config ---- */
static const char *g_raw = "/dev/video3";
static const char *g_ctrl = "";		  /* sensor subdev for AE (optional) */
static const char *g_loopback = "";	   /* v4l2loopback OUTPUT node, or "" */
static const char *g_fifo = "";		   /* FIFO for the UVC feeder, or "" */
static const char *g_dump = "";		   /* processed frame dump for regression, or "" */
static int g_dump_frames = 1;
static int g_out_w = 1280, g_out_h = 720;
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

/* ---- ISP buffers ---- */
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

/* ---- AE state ---- */
static int g_exposure = -1, g_again = -1, g_dgain = -1;
static int g_exp_min, g_exp_max, g_gain_min, g_gain_max, g_dgain_min, g_dgain_max;
static int g_ae_last_log;
static int g_ae_last_hi;
static int g_ae_changed;
static int g_auto_tone = 1;
static int g_dgain_limit = -1;
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

/* ---------------- sensor controls / AE ---------------- */

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

static int set_ctrl(int fd, unsigned int id, int val)
{
	struct v4l2_control c;
	memset(&c, 0, sizeof(c));
	c.id = id;
	c.value = val;
	return xioctl(fd, VIDIOC_S_CTRL, &c);
}

static int g_ctrlfd = -1;

static void ae_init(void)
{
	if (!g_auto_exposure || !g_ctrl[0])
		return;
	g_ctrlfd = open(g_ctrl, O_RDWR);
	if (g_ctrlfd < 0) {
		ilog("AE: cannot open ctrl subdev %s: %s", g_ctrl, strerror(errno));
		g_auto_exposure = 0;
		return;
	}
	if (get_ctrl_range(g_ctrlfd, V4L2_CID_EXPOSURE, &g_exp_min, &g_exp_max, &g_exposure) < 0)
		g_auto_exposure = 0;
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
	ilog("AE: exposure[%d..%d]=%d gain[%d..%d]=%d digital[%d..%d]=%d target=%d clip=%d/%d digital_limit=%d soft_limit=%.1f",
		 g_exp_min, g_exp_max, g_exposure, g_gain_min, g_gain_max, g_again,
		 g_dgain_min, g_dgain_max, g_dgain, g_target, g_ae_clip_target,
		 g_ae_clip_weight,
		 g_dgain >= 0 && g_dgain_limit >= 0 ? g_dgain_limit : g_dgain_max,
		 g_max_soft_gain);
}

static int ae_dgain_max(void)
{
	if (g_dgain < 0)
		return -1;
	if (g_dgain_limit >= 0 && g_dgain_limit < g_dgain_max)
		return g_dgain_limit;
	return g_dgain_max;
}

static int raw_percentile_luma(int step, int pct);

/* simple proportional AE on the pre-gamma mean luma */
static void ae_update(int mean)
{
	int err, step, dgain_max;
	if (!g_auto_exposure || g_ctrlfd < 0)
		return;
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
	err = g_target - mean;
	if (err > -6 && err < 6)
		return;
	/* prefer exposure, then analogue gain, then digital gain */
	dgain_max = ae_dgain_max();
	step = (g_exp_max - g_exp_min) / 16;
	if (step < 1)
		step = 1;
	if (err > 0) {
		if (g_exposure < g_exp_max) {
			g_exposure += step * 2;
			if (g_exposure > g_exp_max)
				g_exposure = g_exp_max;
			if (set_ctrl(g_ctrlfd, V4L2_CID_EXPOSURE, g_exposure) == 0)
				g_ae_changed = 1;
		} else if (g_again >= 0 && g_again < g_gain_max) {
			g_again += (g_gain_max - g_gain_min) / 16 + 1;
			if (g_again > g_gain_max)
				g_again = g_gain_max;
			if (set_ctrl(g_ctrlfd, V4L2_CID_ANALOGUE_GAIN, g_again) == 0)
				g_ae_changed = 1;
		} else if (g_dgain >= 0 && dgain_max >= 0 && g_dgain < dgain_max) {
			g_dgain += (g_dgain_max - g_dgain_min) / 16 + 1;
			if (g_dgain > dgain_max)
				g_dgain = dgain_max;
			if (set_ctrl(g_ctrlfd, V4L2_CID_DIGITAL_GAIN, g_dgain) == 0)
				g_ae_changed = 1;
		}
	} else {
		if (g_dgain > g_dgain_min) {
			g_dgain -= (g_dgain_max - g_dgain_min) / 16 + 1;
			if (g_dgain < g_dgain_min)
				g_dgain = g_dgain_min;
			if (set_ctrl(g_ctrlfd, V4L2_CID_DIGITAL_GAIN, g_dgain) == 0)
				g_ae_changed = 1;
		} else if (g_again > g_gain_min) {
			g_again -= (g_gain_max - g_gain_min) / 16 + 1;
			if (g_again < g_gain_min)
				g_again = g_gain_min;
			if (set_ctrl(g_ctrlfd, V4L2_CID_ANALOGUE_GAIN, g_again) == 0)
				g_ae_changed = 1;
		} else if (g_exposure > g_exp_min) {
			g_exposure -= step;
			if (g_exposure < g_exp_min)
				g_exposure = g_exp_min;
			if (set_ctrl(g_ctrlfd, V4L2_CID_EXPOSURE, g_exposure) == 0)
				g_ae_changed = 1;
		}
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
	int x, y, i;

	if (step < 2)
		step = 2;
	if (pct < 1)
		pct = 1;
	if (pct > 1000)
		pct = 1000;
	memset(hist, 0, sizeof(hist));
	for (y = 0; y + 1 < g_raw_h; y += step) {
		for (x = 0; x + 1 < g_raw_w; x += step) {
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
 * keeps the 4160x2340 detail-first source mode practical while avoiding the old
 * single-site Bayer sample that made 1280x720 look much lower than true 720p. */

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

	source_span_scaled(tx, ow, g_raw_w, g_mjpeg_area_scale, &sx0, &sx1);
	source_span_scaled(ty, oh, g_raw_h, g_mjpeg_area_scale, &sy0, &sy1);

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

	source_span_scaled(tx, ow, g_raw_w, g_mjpeg_area_scale, &sx0, &sx1);
	source_span_scaled(ty, oh, g_raw_h, g_mjpeg_area_scale, &sy0, &sy1);
	qx = ((sx0 + sx1) >> 1) / 2;
	qy = ((sy0 + sy1) >> 1) / 2;
	quad_sample(qx, qy, rgain, bgain, R, Gout, Bout);
	if (luma10)
		*luma10 = luma10_clamped(*R, *Gout, *Bout);
	gamma_clamp_rgb(R, Gout, Bout);
}

static inline void bayer_area_frac_linear_rgb(int tx, int ty, int ow, int oh,
												int xoff, int yoff,
												double rgain, double bgain,
												int *R, int *Gout, int *Bout)
{
	long sx_fp = ((long)(tx * 2 + 1) * g_int_qw * 128) / ow + xoff;
	long sy_fp = ((long)(ty * 2 + 1) * g_int_qh * 128) / oh + yoff;
	int qx = (int)(sx_fp >> 8);
	int qy = (int)(sy_fp >> 8);
	int fx = (int)(sx_fp & 255);
	int fy = (int)(sy_fp & 255);
	int qx1, qy1;
	int r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
	int w00, w10, w01, w11;

	if (qx < 0) {
		qx = 0;
		fx = 0;
	}
	if (qy < 0) {
		qy = 0;
		fy = 0;
	}
	if (qx >= g_int_qw) {
		qx = g_int_qw - 1;
		fx = 0;
	}
	if (qy >= g_int_qh) {
		qy = g_int_qh - 1;
		fy = 0;
	}
	qx1 = qx + 1;
	qy1 = qy + 1;
	if (qx1 >= g_int_qw)
		qx1 = qx;
	if (qy1 >= g_int_qh)
		qy1 = qy;
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

	xoff = (int)(((long)g_int_qw * 64 + ow / 2) / ow);
	yoff = (int)(((long)g_int_qh * 64 + oh / 2) / oh);
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
	long src_qw = ((long)g_int_qw + ow - 1) / ow;
	long src_qh = ((long)g_int_qh + oh - 1) / oh;

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
	qxc = (int)(((long)(tx * 2 + 1) * g_int_qw) / (ow * 2L));
	qyc = (int)(((long)(ty * 2 + 1) * g_int_qh) / (oh * 2L));
	if (qxc < 0)
		qxc = 0;
	if (qyc < 0)
		qyc = 0;
	if (qxc >= g_int_qw)
		qxc = g_int_qw - 1;
	if (qyc >= g_int_qh)
		qyc = g_int_qh - 1;
	qx0 = qxc - radius;
	qx1 = qxc + radius + 1;
	qy0 = qyc - radius;
	qy1 = qyc + radius + 1;
	rs = int_sum_bounded(g_int_r, qx0, qy0, qx1, qy1);
	grs = int_sum_bounded(g_int_gr, qx0, qy0, qx1, qy1);
	gbs = int_sum_bounded(g_int_gb, qx0, qy0, qx1, qy1);
	bs = int_sum_bounded(g_int_b, qx0, qy0, qx1, qy1);
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
	(void)ow;
	(void)oh;

	/* Keep the downscale footprint phase-locked to integer Bayer quads.  The
	 * fractional quad sampler is sharp, but clipped lights can expose the sampling
	 * lattice as a square halo.  This mode averages only complete GR/BG quads that
	 * map to the output pixel, so every output sample has equal red/green/blue
	 * support before gamma/JPEG. */
	qx0 = (int)(((long)tx * g_int_qw) / g_out_w);
	qx1 = (int)(((long)(tx + 1) * g_int_qw + g_out_w - 1) / g_out_w);
	qy0 = (int)(((long)ty * g_int_qh) / g_out_h);
	qy1 = (int)(((long)(ty + 1) * g_int_qh + g_out_h - 1) / g_out_h);
	if (qx1 <= qx0)
		qx1 = qx0 + 1;
	if (qy1 <= qy0)
		qy1 = qy0 + 1;
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
	(void)ow;
	(void)oh;

	qx = (int)(((long)(tx * 2 + 1) * g_int_qw) / (g_out_w * 2L));
	qy = (int)(((long)(ty * 2 + 1) * g_int_qh) / (g_out_h * 2L));
	if (qx < 0)
		qx = 0;
	if (qy < 0)
		qy = 0;
	if (qx >= g_int_qw)
		qx = g_int_qw - 1;
	if (qy >= g_int_qh)
		qy = g_int_qh - 1;
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

	source_span_scaled(tx, ow, g_raw_w, g_mjpeg_area_scale, &sx0, &sx1);
	source_span_scaled(ty, oh, g_raw_h, g_mjpeg_area_scale, &sy0, &sy1);
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
		if (g_mjpeg_subsampling == 444)
			g_jpeg_size = lmi_jpeg_encode_rgb444(g_jpeg, (size_t)g_max_frame_bytes,
								   jpeg_src, ow, oh, g_out_stride, g_mjpeg_quality);
		else
			g_jpeg_size = lmi_jpeg_encode_rgb420(g_jpeg, (size_t)g_max_frame_bytes,
								   jpeg_src, ow, oh, g_out_stride, g_mjpeg_quality);
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
			int sy = (int)((long)ty * g_raw_h / oh);
			uint8_t *yrow = yp + (size_t)ty * g_out_stride;
			uint8_t *uvrow = uvp + (size_t)(ty >> 1) * g_out_stride;
			for (tx = 0; tx < ow; tx++) {
				int sx = (int)((long)tx * g_raw_w / ow);
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
		return lumasum / ((long)ow * oh);
	}
	/* YUYV */
	(void)aw;
	for (ty = 0; ty < oh; ty++) {
		int sy = (int)((long)ty * g_raw_h / oh);
		uint8_t *row = g_frame + (size_t)ty * g_out_stride;
		for (tx = 0; tx < ow; tx += 2) {
			int sx0 = (int)((long)tx * g_raw_w / ow);
			int sx1 = (int)((long)(tx + 1) * g_raw_w / ow);
			int R0, Gg0, Bb0, R1, Gg1, Bb1;
			demosaic_at(sx0, sy, rgain, bgain, &R0, &Gg0, &Bb0);
			demosaic_at(sx1, sy, rgain, bgain, &R1, &Gg1, &Bb1);
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
	return lumasum / ((long)ow * oh);
}

/* AE meter uses the raw green mean (pre-gamma), independent of the tone curve */
static int raw_mean_luma(void)
{
	uint64_t s = 0, n = 0;
	int y, x;
	/* Average both green sites from sparse 2x2 quads; sampling only one parity can
	 * alias with row/column structure and make AE look stuck under real scenes. */
	for (y = 0; y + 1 < g_raw_h; y += 8) {
		for (x = 0; x + 1 < g_raw_w; x += 8) {
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
	g_out_w &= ~1;
	if (g_nv12 || g_mjpeg)
		g_out_h &= ~1;
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
			(g_max_frame_bytes + (int)sizeof(struct lmi_uvc_record_header)) * 2 :
			g_out_size * 2;
		g_outfd = open(g_fifo, O_WRONLY | (g_mjpeg ? 0 : O_NONBLOCK));
		if (g_outfd < 0) { ilog("open fifo %s: %s", g_fifo, strerror(errno)); return -1; }
		cap = fcntl(g_outfd, F_SETPIPE_SZ, pipe_target);
		if (cap < 0)
			cap = fcntl(g_outfd, F_GETPIPE_SZ);
		if (cap <= 0) {
			ilog("fifo pipe is smaller than one frame and cannot be resized");
			return -1;
		}
		g_pipe_cap = cap;
		if (!g_mjpeg && g_pipe_cap < g_out_size) {
			ilog("fifo pipe capacity %d is smaller than frame size %d", g_pipe_cap, g_out_size);
			return -1;
		}
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
	if (g_fifo[0]) {
		int inpipe = 0;
		size_t need = payload_size + (g_mjpeg ? sizeof(struct lmi_uvc_record_header) : 0);
		ioctl(g_outfd, FIONREAD, &inpipe);
		if (g_pipe_cap - inpipe < (int)need) {
			if (g_mjpeg && (int)need > g_pipe_cap) {
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				if ((int)ts.tv_sec != g_mjpeg_pipe_short_log_sec) {
					g_mjpeg_pipe_short_log_sec = (int)ts.tv_sec;
					ilog("MJPEG record %zu bytes exceeds fifo pipe capacity %d; dropping", need, g_pipe_cap);
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

int main(int argc, char **argv)
{
	int i, frames = 0, total_frames = 0, ae_div = 0;
	struct timespec t0, tr;
	double rgain = 1.0, bgain = 1.0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--raw") && i + 1 < argc) g_raw = argv[++i];
		else if (!strcmp(argv[i], "--ctrl") && i + 1 < argc) g_ctrl = argv[++i];
		else if (!strcmp(argv[i], "--loopback") && i + 1 < argc) g_loopback = argv[++i];
		else if (!strcmp(argv[i], "--fifo") && i + 1 < argc) g_fifo = argv[++i];
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc) g_dump = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_dump_frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--out-width") && i + 1 < argc) g_out_w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--out-height") && i + 1 < argc) g_out_h = atoi(argv[++i]);
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
		else if (!strcmp(argv[i], "-v")) g_verbose = 1;
		else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

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
	g_bayer = malloc((size_t)g_raw_w * g_raw_h * sizeof(uint16_t));
	if (out_open() < 0) return 1;
	g_frame = calloc(1, g_out_size);
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
	if (!g_bayer || !g_frame || (g_mjpeg && (!g_jpeg ||
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
	ilog("ready: raw %dx%d -> out %dx%d %s gamma=%.2f shoulder=%d/%d ae=%d sharpen=%d smooth=%d highlight=%d/%d/r%d desat=%d antibloom=%d/%d/r%d edge=%d/%d/r%d despeckle=%d blend=%d/%d harea=%d/%d/d%d hbox=%d/%d/d%d/a%d area=%d subsampling=%d scale=%s", g_raw_w, g_raw_h,
		 g_out_w, g_out_h, g_mjpeg ? "MJPEG" : (g_nv12 ? "NV12" : "YUYV"),
		 g_gamma, g_tone_highlight_knee, g_tone_highlight_max,
		 g_auto_exposure, g_mjpeg ? g_sharpen : 0, g_mjpeg ? g_mjpeg_smooth : 0,
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
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	while (g_run) {
		struct pollfd p = { g_rawfd, POLLIN, 0 };
		struct v4l2_buffer buf;
		struct v4l2_plane planes[MAXPLANES];
		struct timespec fa, fb;
		int pr = poll(&p, 1, 1000);
		if (pr <= 0) continue;
		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type; buf.memory = V4L2_MEMORY_MMAP;
		buf.length = MAXPLANES; buf.m.planes = planes;
		if (xioctl(g_rawfd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN) continue;
			ilog("DQBUF: %s", strerror(errno));
			break;
		}
		clock_gettime(CLOCK_MONOTONIC, &fa);
		unpack_raw10p(g_rb[buf.index].start);
		if (g_awb && (frames % 8) == 0)
			compute_awb(&rgain, &bgain);
		long mean = pack_output(rgain, bgain);
		out_write();
		clock_gettime(CLOCK_MONOTONIC, &fb);
		xioctl(g_rawfd, VIDIOC_QBUF, &buf);

		if (g_auto_exposure && (++ae_div % 3) == 0) {
			g_ae_last_log = raw_mean_luma();
			tone_update(g_ae_last_log);
			ae_update(g_ae_last_log);
		}

		frames++;
		total_frames++;
		clock_gettime(CLOCK_MONOTONIC, &tr);
		double el = (tr.tv_sec - t0.tv_sec) + (tr.tv_nsec - t0.tv_nsec) / 1e9;
		if (el >= 2.0) {
			double ms = (fb.tv_sec - fa.tv_sec) * 1e3 + (fb.tv_nsec - fa.tv_nsec) / 1e6;
			if (g_auto_exposure)
				ilog("fps=%.1f isp=%.1fms outmean=%ld ae{mean=%d hi=%d exp=%d again=%d dgain=%d%s soft=%.1f} gains r=%.2f b=%.2f",
					 frames / el, ms, mean, g_ae_last_log, g_ae_last_hi, g_exposure, g_again, g_dgain,
					 g_ae_changed ? " changed" : "", g_soft_gain, rgain, bgain);
			else
				ilog("fps=%.1f isp=%.1fms outmean=%ld gains r=%.2f b=%.2f", frames / el, ms, mean, rgain, bgain);
			g_ae_changed = 0;
			frames = 0; t0 = tr;
		}
		if (g_dump[0] && total_frames >= g_dump_frames)
			break;

		if (g_fps_cap > 0) {
			double spent = (fb.tv_sec - fa.tv_sec) + (fb.tv_nsec - fa.tv_nsec) / 1e9;
			double budget = 1.0 / g_fps_cap;
			if (spent < budget) {
				struct timespec ts = { 0, (long)((budget - spent) * 1e9) };
				nanosleep(&ts, NULL);
			}
		}
	}

	xioctl(g_rawfd, VIDIOC_STREAMOFF, &type);
	ilog("stopped");
	return 0;
}
