// SPDX-License-Identifier: GPL-2.0
//
// lmi-isp.c -- fast C software ISP + streamer for the lmi OV13B10 camera.
//
// Captures RAW10 (MIPI packed GRBG, V4L2 fourcc "pgAA") from a CAMSS RDI video
// node whose media pipeline + sensor mode were already configured by
// lmi-camera-web-preview.py, runs a real software ISP in C
//   10-bit unpack -> black-level -> gray-world AWB -> bilinear demosaic
//   -> gamma tone curve (10-bit linear -> 8-bit display) -> scale -> YUYV/NV12
// and writes processed frames to a v4l2loopback OUTPUT node (write()), a FIFO
// consumed by lmi-uvc-gadget, or a finite dump file for regression checks. Optional auto-exposure drives the sensor
// subdev's exposure/gain controls.
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

#ifndef F_GETPIPE_SZ
#define F_GETPIPE_SZ 1032
#endif
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif

#define NBUF 4
#define MAXPLANES 1

/* ---- config ---- */
static const char *g_raw = "/dev/video3";
static const char *g_ctrl = "";          /* sensor subdev for AE (optional) */
static const char *g_loopback = "";       /* v4l2loopback OUTPUT node, or "" */
static const char *g_fifo = "";           /* FIFO for the UVC feeder, or "" */
static const char *g_dump = "";           /* processed frame dump for regression, or "" */
static int g_dump_frames = 1;
static int g_out_w = 1280, g_out_h = 720;
static int g_nv12 = 0;                     /* 0 = YUYV, 1 = NV12 */
static double g_gamma = 2.2;
static int g_blacklevel = 64;              /* 10-bit black level */
static int g_fps_cap = 30;
static int g_auto_exposure = 0;
static int g_target = 105;
static int g_awb = 1;                      /* gray-world AWB */
static int g_verbose = 0;

/* ---- raw capture state ---- */
static int g_rawfd = -1;
static int g_raw_w, g_raw_h, g_raw_stride, g_raw_sizeimage;
struct rbuf { void *start; size_t len; };
static struct rbuf g_rb[NBUF];
static int g_nrb;

/* ---- output sink state ---- */
static int g_outfd = -1;                   /* loopback OR fifo OR dump fd */
static int g_out_stride, g_out_size;
static int g_pipe_cap = 1 << 20;

/* ---- ISP buffers ---- */
static uint16_t *g_bayer;                  /* raw_w * raw_h, 10-bit values */
static uint8_t *g_gamma_lut;               /* 1024 -> 8-bit */
static uint8_t *g_frame;                   /* output YUYV/NV12 */

/* ---- AE state ---- */
static int g_exposure = -1, g_again = -1;
static int g_exp_min, g_exp_max, g_gain_min, g_gain_max;

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

static void set_ctrl(int fd, unsigned int id, int val)
{
	struct v4l2_control c;
	memset(&c, 0, sizeof(c));
	c.id = id;
	c.value = val;
	xioctl(fd, VIDIOC_S_CTRL, &c);
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
	ilog("AE: exposure[%d..%d]=%d gain[%d..%d]=%d", g_exp_min, g_exp_max, g_exposure,
	     g_gain_min, g_gain_max, g_again);
}

/* simple proportional AE on the pre-gamma mean luma */
static void ae_update(int mean)
{
	int err, step;
	if (!g_auto_exposure || g_ctrlfd < 0)
		return;
	err = g_target - mean;
	if (err > -6 && err < 6)
		return;
	/* prefer exposure, then gain */
	step = (g_exp_max - g_exp_min) / 16;
	if (step < 1)
		step = 1;
	if (err > 0) {
		if (g_exposure < g_exp_max) {
			g_exposure += step * 2;
			if (g_exposure > g_exp_max) g_exposure = g_exp_max;
			set_ctrl(g_ctrlfd, V4L2_CID_EXPOSURE, g_exposure);
		} else if (g_again >= 0 && g_again < g_gain_max) {
			g_again += (g_gain_max - g_gain_min) / 16 + 1;
			if (g_again > g_gain_max) g_again = g_gain_max;
			set_ctrl(g_ctrlfd, V4L2_CID_ANALOGUE_GAIN, g_again);
		}
	} else {
		if (g_again > g_gain_min) {
			g_again -= (g_gain_max - g_gain_min) / 16 + 1;
			if (g_again < g_gain_min) g_again = g_gain_min;
			set_ctrl(g_ctrlfd, V4L2_CID_ANALOGUE_GAIN, g_again);
		} else if (g_exposure > g_exp_min) {
			g_exposure -= step;
			if (g_exposure < g_exp_min) g_exposure = g_exp_min;
			set_ctrl(g_ctrlfd, V4L2_CID_EXPOSURE, g_exposure);
		}
	}
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
static inline int B(int x, int y)
{
	if (x < 0) x = 0; else if (x >= g_raw_w) x = g_raw_w - 1;
	if (y < 0) y = 0; else if (y >= g_raw_h) y = g_raw_h - 1;
	return g_bayer[(size_t)y * g_raw_w + x];
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

/* ---------------- demosaic + WB + gamma + scale + YUYV/NV12 pack ----------------
 * To avoid a full-res RGB buffer at large sensor modes, we demosaic on the fly
 * at the scaled output sampling positions: for each output pixel we map to a Bayer (x,y),
 * bilinear-demosaic R/G/B there, apply WB + gamma LUT, then pack. This is one
 * pass and keeps memory tiny. */

static inline void demosaic_at(int bx, int by, double rgain, double bgain,
			       int *R, int *Gout, int *Bout)
{
	int bl = g_blacklevel;
	int cx = bx & 1, cy = by & 1;
	int r, g, b;
	if (cy == 0 && cx == 1) {            /* R site */
		r = B(bx, by);
		g = (B(bx - 1, by) + B(bx + 1, by) + B(bx, by - 1) + B(bx, by + 1)) >> 2;
		b = (B(bx - 1, by - 1) + B(bx + 1, by - 1) + B(bx - 1, by + 1) + B(bx + 1, by + 1)) >> 2;
	} else if (cy == 1 && cx == 0) {     /* B site */
		b = B(bx, by);
		g = (B(bx - 1, by) + B(bx + 1, by) + B(bx, by - 1) + B(bx, by + 1)) >> 2;
		r = (B(bx - 1, by - 1) + B(bx + 1, by - 1) + B(bx - 1, by + 1) + B(bx + 1, by + 1)) >> 2;
	} else if (cy == 0 && cx == 0) {     /* Gr site: R horizontal, B vertical */
		g = B(bx, by);
		r = (B(bx - 1, by) + B(bx + 1, by)) >> 1;
		b = (B(bx, by - 1) + B(bx, by + 1)) >> 1;
	} else {                             /* Gb site: R vertical, B horizontal */
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
	r = (int)(r * rgain); b = (int)(b * bgain);
	if (r > 1023)
		r = 1023;
	if (b > 1023)
		b = 1023;
	if (g > 1023)
		g = 1023;
	*R = g_gamma_lut[r]; *Gout = g_gamma_lut[g]; *Bout = g_gamma_lut[b];
}

static long pack_output(double rgain, double bgain)
{
	int ow = g_out_w, oh = g_out_h;
	int aw = ow > g_raw_w ? g_raw_w : ow;  /* never upsample beyond raw */
	long lumasum = 0;
	int ty, tx;
	/* map output (tx,ty) -> bayer source */
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
	for (y = 0; y < g_raw_h; y += 8)
		for (x = 0; x < g_raw_w; x += 8) { s += g_bayer[(size_t)y * g_raw_w + x]; n++; }
	if (!n) return 0;
	int v = (int)(s / n) - g_blacklevel;
	int maxv = 1023 - g_blacklevel;
	if (maxv < 1)
		maxv = 1;
	if (v < 0) v = 0;
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

/* ---------------- output sink ---------------- */

static int out_open(void)
{
	g_out_w &= ~1;
	if (g_nv12)
		g_out_h &= ~1;
	g_out_stride = g_out_w * (g_nv12 ? 1 : 2);
	g_out_size = g_nv12 ? (g_out_stride * g_out_h * 3 / 2) : (g_out_stride * g_out_h);
	if (g_loopback[0]) {
		struct v4l2_format fmt;
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
		g_outfd = open(g_fifo, O_WRONLY | O_NONBLOCK);
		if (g_outfd < 0) { ilog("open fifo %s: %s", g_fifo, strerror(errno)); return -1; }
		cap = fcntl(g_outfd, F_SETPIPE_SZ, g_out_size * 3);
		if (cap < 0)
			cap = fcntl(g_outfd, F_GETPIPE_SZ);
		if (cap <= 0) {
			ilog("fifo pipe is smaller than one frame and cannot be resized");
			return -1;
		}
		g_pipe_cap = cap;
		if (g_pipe_cap < g_out_size) {
			ilog("fifo pipe capacity %d is smaller than frame size %d", g_pipe_cap, g_out_size);
			return -1;
		}
		ilog("fifo %s: %dx%d stride=%d size=%d pipe_cap=%d", g_fifo, g_out_w, g_out_h, g_out_stride, g_out_size, g_pipe_cap);
	} else if (g_dump[0]) {
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
	if (g_outfd < 0) return;
	if (g_fifo[0]) {
		int inpipe = 0;
		ioctl(g_outfd, FIONREAD, &inpipe);
		if (g_pipe_cap - inpipe < g_out_size) return; /* drop if feeder behind */
	}
	size_t off = 0;
	while (off < (size_t)g_out_size) {
		ssize_t n = write(g_outfd, g_frame + off, g_out_size - off);
		if (n > 0) { off += n; continue; }
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (g_fifo[0]) return; /* non-blocking fifo: drop rest */
			struct pollfd p = { g_outfd, POLLOUT, 0 };
			poll(&p, 1, 100);
			continue;
		}
		return; /* EPIPE etc. */
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
		else if (!strcmp(argv[i], "--gamma") && i + 1 < argc) g_gamma = atof(argv[++i]);
		else if (!strcmp(argv[i], "--black-level") && i + 1 < argc) g_blacklevel = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--fps-cap") && i + 1 < argc) g_fps_cap = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--auto-exposure")) g_auto_exposure = 1;
		else if (!strcmp(argv[i], "--target") && i + 1 < argc) g_target = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-awb")) g_awb = 0;
		else if (!strcmp(argv[i], "-v")) g_verbose = 1;
		else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

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

	if (raw_start() < 0) return 1;
	g_bayer = malloc((size_t)g_raw_w * g_raw_h * sizeof(uint16_t));
	if (out_open() < 0) return 1;
	g_frame = calloc(1, g_out_size);
	if (!g_bayer || !g_frame) { ilog("oom"); return 1; }
	ae_init();
	ilog("ready: raw %dx%d -> out %dx%d %s gamma=%.2f ae=%d", g_raw_w, g_raw_h,
	     g_out_w, g_out_h, g_nv12 ? "NV12" : "YUYV", g_gamma, g_auto_exposure);

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

		if (g_auto_exposure && (++ae_div % 3) == 0)
			ae_update(raw_mean_luma());

		frames++;
		total_frames++;
		clock_gettime(CLOCK_MONOTONIC, &tr);
		double el = (tr.tv_sec - t0.tv_sec) + (tr.tv_nsec - t0.tv_nsec) / 1e9;
		if (el >= 2.0) {
			double ms = (fb.tv_sec - fa.tv_sec) * 1e3 + (fb.tv_nsec - fa.tv_nsec) / 1e6;
			ilog("fps=%.1f isp=%.1fms outmean=%ld gains r=%.2f b=%.2f", frames / el, ms, mean, rgain, bgain);
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
