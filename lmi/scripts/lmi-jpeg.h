/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tiny baseline JPEG encoder for lmi-isp MJPEG output.
 *
 * This file intentionally has no libc/rootfs dependencies beyond libm.  It emits
 * sequential 8-bit JFIF JPEG with the standard baseline Huffman tables and can
 * use either 4:2:0 or 4:4:4 sampling.  It is optimized for a live camera preview
 * path rather than archival still-image quality.
 */
#ifndef LMI_JPEG_H
#define LMI_JPEG_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct lmi_jpeg_writer {
	uint8_t *buf;
	size_t cap;
	size_t pos;
	uint32_t bitbuf;
	int bitcnt;
	int err;
};

struct lmi_jpeg_huff {
	uint16_t code[256];
	uint8_t size[256];
};

static const uint8_t lmi_jpeg_zigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10,
	17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63,
};

static const uint8_t lmi_jpeg_q_luma_base[64] = {
	16, 11, 10, 16, 24, 40, 51, 61,
	12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56,
	14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77,
	24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103, 99,
};

static const uint8_t lmi_jpeg_q_chroma_base[64] = {
	17, 18, 24, 47, 99, 99, 99, 99,
	18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99,
	47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
};

static const uint8_t lmi_jpeg_bits_dc_luma[17] =
	{ 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t lmi_jpeg_vals_dc_luma[12] =
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t lmi_jpeg_bits_dc_chroma[17] =
	{ 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0 };
static const uint8_t lmi_jpeg_vals_dc_chroma[12] =
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static const uint8_t lmi_jpeg_bits_ac_luma[17] =
	{ 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t lmi_jpeg_vals_ac_luma[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
	0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
	0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
	0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
	0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
	0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
	0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
	0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
	0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
	0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
	0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa,
};

static const uint8_t lmi_jpeg_bits_ac_chroma[17] =
	{ 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t lmi_jpeg_vals_ac_chroma[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
	0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
	0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
	0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
	0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
	0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
	0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
	0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
	0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
	0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa,
};

static inline int lmi_jpeg_clamp_u8(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void lmi_jpeg_build_huff(struct lmi_jpeg_huff *h, const uint8_t bits[17], const uint8_t *vals)
{
	unsigned int code = 0;
	unsigned int k = 0;
	int len;
	memset(h, 0, sizeof(*h));
	for (len = 1; len <= 16; len++) {
		int i;
		for (i = 0; i < bits[len]; i++) {
			uint8_t sym = vals[k++];
			h->code[sym] = (uint16_t)code;
			h->size[sym] = (uint8_t)len;
			code++;
		}
		code <<= 1;
	}
}

static void lmi_jpeg_put_raw(struct lmi_jpeg_writer *w, uint8_t v)
{
	if (w->pos >= w->cap) {
		w->err = 1;
		return;
	}
	w->buf[w->pos++] = v;
}

static void lmi_jpeg_put_entropy_byte(struct lmi_jpeg_writer *w, uint8_t v)
{
	lmi_jpeg_put_raw(w, v);
	if (v == 0xff)
		lmi_jpeg_put_raw(w, 0x00);
}

static void lmi_jpeg_put_be16(struct lmi_jpeg_writer *w, unsigned int v)
{
	lmi_jpeg_put_raw(w, (uint8_t)(v >> 8));
	lmi_jpeg_put_raw(w, (uint8_t)v);
}

static void lmi_jpeg_marker(struct lmi_jpeg_writer *w, uint8_t marker)
{
	lmi_jpeg_put_raw(w, 0xff);
	lmi_jpeg_put_raw(w, marker);
}

static void lmi_jpeg_bits(struct lmi_jpeg_writer *w, unsigned int bits, int size)
{
	if (size <= 0)
		return;
	w->bitbuf = (w->bitbuf << size) | (bits & ((1u << size) - 1));
	w->bitcnt += size;
	while (w->bitcnt >= 8) {
		uint8_t b = (uint8_t)(w->bitbuf >> (w->bitcnt - 8));
		lmi_jpeg_put_entropy_byte(w, b);
		w->bitcnt -= 8;
		if (w->bitcnt)
			w->bitbuf &= (1u << w->bitcnt) - 1;
		else
			w->bitbuf = 0;
	}
}

static void lmi_jpeg_huff_bits(struct lmi_jpeg_writer *w, const struct lmi_jpeg_huff *h, uint8_t sym)
{
	lmi_jpeg_bits(w, h->code[sym], h->size[sym]);
}

static void lmi_jpeg_flush_bits(struct lmi_jpeg_writer *w)
{
	if (w->bitcnt > 0)
		lmi_jpeg_bits(w, (1u << (8 - w->bitcnt)) - 1, 8 - w->bitcnt);
}

static int lmi_jpeg_category(int v)
{
	int a = v < 0 ? -v : v;
	int n = 0;
	while (a) {
		n++;
		a >>= 1;
	}
	return n;
}

static unsigned int lmi_jpeg_value_bits(int v, int n)
{
	if (n == 0)
		return 0;
	if (v >= 0)
		return (unsigned int)v;
	return (unsigned int)((1 << n) - 1 + v);
}

static void lmi_jpeg_make_qtable(uint8_t q[64], const uint8_t base[64], int quality)
{
	int scale;
	int i;
	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;
	scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	for (i = 0; i < 64; i++) {
		int v = (base[i] * scale + 50) / 100;
		if (v < 1)
			v = 1;
		if (v > 255)
			v = 255;
		q[i] = (uint8_t)v;
	}
}

static void lmi_jpeg_fdct_quant(const double in[64], int out[64], const uint8_t q[64])
{
	double tmp[64];
	int i;
	for (i = 0; i < 8; i++) {
		const double *d = in + i * 8;
		double *t = tmp + i * 8;
		double tmp0 = d[0] + d[7], tmp7 = d[0] - d[7];
		double tmp1 = d[1] + d[6], tmp6 = d[1] - d[6];
		double tmp2 = d[2] + d[5], tmp5 = d[2] - d[5];
		double tmp3 = d[3] + d[4], tmp4 = d[3] - d[4];
		double tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
		double tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
		double z1;
		t[0] = tmp10 + tmp11;
		t[4] = tmp10 - tmp11;
		z1 = (tmp12 + tmp13) * 0.7071067811865476;
		t[2] = tmp13 + z1;
		t[6] = tmp13 - z1;
		tmp10 = tmp4 + tmp5;
		tmp11 = tmp5 + tmp6;
		tmp12 = tmp6 + tmp7;
		{
			double z5 = (tmp10 - tmp12) * 0.3826834323650898;
			double z2 = 0.5411961001461970 * tmp10 + z5;
			double z4 = 1.3065629648763766 * tmp12 + z5;
			double z3 = tmp11 * 0.7071067811865476;
			double z11 = tmp7 + z3;
			double z13 = tmp7 - z3;
			t[5] = z13 + z2;
			t[3] = z13 - z2;
			t[1] = z11 + z4;
			t[7] = z11 - z4;
		}
	}
	for (i = 0; i < 8; i++) {
		double d0 = tmp[i + 0] + tmp[i + 56], d7 = tmp[i + 0] - tmp[i + 56];
		double d1 = tmp[i + 8] + tmp[i + 48], d6 = tmp[i + 8] - tmp[i + 48];
		double d2 = tmp[i + 16] + tmp[i + 40], d5 = tmp[i + 16] - tmp[i + 40];
		double d3 = tmp[i + 24] + tmp[i + 32], d4 = tmp[i + 24] - tmp[i + 32];
		double tmp10 = d0 + d3, tmp13 = d0 - d3;
		double tmp11 = d1 + d2, tmp12 = d1 - d2;
		double col[8];
		double z1;
		col[0] = tmp10 + tmp11;
		col[4] = tmp10 - tmp11;
		z1 = (tmp12 + tmp13) * 0.7071067811865476;
		col[2] = tmp13 + z1;
		col[6] = tmp13 - z1;
		tmp10 = d4 + d5;
		tmp11 = d5 + d6;
		tmp12 = d6 + d7;
		{
			double z5 = (tmp10 - tmp12) * 0.3826834323650898;
			double z2 = 0.5411961001461970 * tmp10 + z5;
			double z4 = 1.3065629648763766 * tmp12 + z5;
			double z3 = tmp11 * 0.7071067811865476;
			double z11 = d7 + z3;
			double z13 = d7 - z3;
			col[5] = z13 + z2;
			col[3] = z13 - z2;
			col[1] = z11 + z4;
			col[7] = z11 - z4;
		}
		for (int r = 0; r < 8; r++) {
			int idx = r * 8 + i;
			double scaled = col[r] / (q[idx] * 8.0);
			out[idx] = (int)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
		}
	}
}

static void lmi_jpeg_encode_block(struct lmi_jpeg_writer *w, const double block[64],
				  const uint8_t q[64], const struct lmi_jpeg_huff *hdc,
				  const struct lmi_jpeg_huff *hac, int *pred)
{
	int coeff[64];
	int zz[64];
	int diff, nbits, run;
	lmi_jpeg_fdct_quant(block, coeff, q);
	for (int i = 0; i < 64; i++)
		zz[i] = coeff[lmi_jpeg_zigzag[i]];
	diff = zz[0] - *pred;
	*pred = zz[0];
	nbits = lmi_jpeg_category(diff);
	lmi_jpeg_huff_bits(w, hdc, (uint8_t)nbits);
	lmi_jpeg_bits(w, lmi_jpeg_value_bits(diff, nbits), nbits);
	run = 0;
	for (int i = 1; i < 64; i++) {
		int v = zz[i];
		if (v == 0) {
			run++;
			continue;
		}
		while (run > 15) {
			lmi_jpeg_huff_bits(w, hac, 0xf0);
			run -= 16;
		}
		nbits = lmi_jpeg_category(v);
		lmi_jpeg_huff_bits(w, hac, (uint8_t)((run << 4) | nbits));
		lmi_jpeg_bits(w, lmi_jpeg_value_bits(v, nbits), nbits);
		run = 0;
	}
	if (run)
		lmi_jpeg_huff_bits(w, hac, 0x00);
}

static void lmi_jpeg_emit_dqt(struct lmi_jpeg_writer *w, const uint8_t qy[64], const uint8_t qc[64])
{
	lmi_jpeg_marker(w, 0xdb);
	lmi_jpeg_put_be16(w, 2 + 65 * 2);
	lmi_jpeg_put_raw(w, 0x00);
	for (int i = 0; i < 64; i++)
		lmi_jpeg_put_raw(w, qy[lmi_jpeg_zigzag[i]]);
	lmi_jpeg_put_raw(w, 0x01);
	for (int i = 0; i < 64; i++)
		lmi_jpeg_put_raw(w, qc[lmi_jpeg_zigzag[i]]);
}

static void lmi_jpeg_emit_dht_one(struct lmi_jpeg_writer *w, uint8_t tc_th,
				  const uint8_t bits[17], const uint8_t *vals)
{
	int count = 0;
	for (int i = 1; i <= 16; i++)
		count += bits[i];
	lmi_jpeg_marker(w, 0xc4);
	lmi_jpeg_put_be16(w, 2 + 1 + 16 + count);
	lmi_jpeg_put_raw(w, tc_th);
	for (int i = 1; i <= 16; i++)
		lmi_jpeg_put_raw(w, bits[i]);
	for (int i = 0; i < count; i++)
		lmi_jpeg_put_raw(w, vals[i]);
}

static const uint8_t *lmi_jpeg_rgb_at(const uint8_t *rgb, int width, int height,
				       int stride, int x, int y)
{
	if (x < 0)
		x = 0;
	else if (x >= width)
		x = width - 1;
	if (y < 0)
		y = 0;
	else if (y >= height)
		y = height - 1;
	return rgb + (size_t)y * stride + (size_t)x * 3;
}

static void lmi_jpeg_rgb_to_ycbcr(const uint8_t *p, int *y, int *cb, int *cr)
{
	int r = p[0], g = p[1], b = p[2];
	*y = lmi_jpeg_clamp_u8((77 * r + 150 * g + 29 * b) >> 8);
	*cb = lmi_jpeg_clamp_u8(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
	*cr = lmi_jpeg_clamp_u8(((128 * r - 107 * g - 21 * b) >> 8) + 128);
}

static void lmi_jpeg_emit_header(struct lmi_jpeg_writer *w, int width, int height,
					 int sampling_y, const uint8_t qy[64], const uint8_t qc[64])
{
	lmi_jpeg_marker(w, 0xd8); /* SOI */
	lmi_jpeg_marker(w, 0xe0); /* APP0 JFIF */
	lmi_jpeg_put_be16(w, 16);
	lmi_jpeg_put_raw(w, 'J'); lmi_jpeg_put_raw(w, 'F'); lmi_jpeg_put_raw(w, 'I');
	lmi_jpeg_put_raw(w, 'F'); lmi_jpeg_put_raw(w, 0);
	lmi_jpeg_put_raw(w, 1); lmi_jpeg_put_raw(w, 1);
	lmi_jpeg_put_raw(w, 0);
	lmi_jpeg_put_be16(w, 1); lmi_jpeg_put_be16(w, 1);
	lmi_jpeg_put_raw(w, 0); lmi_jpeg_put_raw(w, 0);
	lmi_jpeg_emit_dqt(w, qy, qc);
	lmi_jpeg_marker(w, 0xc0); /* SOF0 */
	lmi_jpeg_put_be16(w, 17);
	lmi_jpeg_put_raw(w, 8);
	lmi_jpeg_put_be16(w, (unsigned int)height);
	lmi_jpeg_put_be16(w, (unsigned int)width);
	lmi_jpeg_put_raw(w, 3);
	lmi_jpeg_put_raw(w, 1); lmi_jpeg_put_raw(w, (uint8_t)sampling_y); lmi_jpeg_put_raw(w, 0);
	lmi_jpeg_put_raw(w, 2); lmi_jpeg_put_raw(w, 0x11); lmi_jpeg_put_raw(w, 1);
	lmi_jpeg_put_raw(w, 3); lmi_jpeg_put_raw(w, 0x11); lmi_jpeg_put_raw(w, 1);
	lmi_jpeg_emit_dht_one(w, 0x00, lmi_jpeg_bits_dc_luma, lmi_jpeg_vals_dc_luma);
	lmi_jpeg_emit_dht_one(w, 0x10, lmi_jpeg_bits_ac_luma, lmi_jpeg_vals_ac_luma);
	lmi_jpeg_emit_dht_one(w, 0x01, lmi_jpeg_bits_dc_chroma, lmi_jpeg_vals_dc_chroma);
	lmi_jpeg_emit_dht_one(w, 0x11, lmi_jpeg_bits_ac_chroma, lmi_jpeg_vals_ac_chroma);
	lmi_jpeg_marker(w, 0xda); /* SOS */
	lmi_jpeg_put_be16(w, 12);
	lmi_jpeg_put_raw(w, 3);
	lmi_jpeg_put_raw(w, 1); lmi_jpeg_put_raw(w, 0x00);
	lmi_jpeg_put_raw(w, 2); lmi_jpeg_put_raw(w, 0x11);
	lmi_jpeg_put_raw(w, 3); lmi_jpeg_put_raw(w, 0x11);
	lmi_jpeg_put_raw(w, 0); lmi_jpeg_put_raw(w, 63); lmi_jpeg_put_raw(w, 0);
}

static int lmi_jpeg_encode_rgb420(uint8_t *dst, size_t cap, const uint8_t *rgb,
				       int width, int height, int stride, int quality)
{
	struct lmi_jpeg_writer w;
	struct lmi_jpeg_huff hdc_y, hac_y, hdc_c, hac_c;
	uint8_t qy[64], qc[64];
	int pred_y = 0, pred_cb = 0, pred_cr = 0;
	int mx, my;
	if (!dst || !rgb || width <= 0 || height <= 0 || stride < width * 3)
		return -1;
	memset(&w, 0, sizeof(w));
	w.buf = dst;
	w.cap = cap;
	lmi_jpeg_make_qtable(qy, lmi_jpeg_q_luma_base, quality);
	lmi_jpeg_make_qtable(qc, lmi_jpeg_q_chroma_base, quality);
	lmi_jpeg_build_huff(&hdc_y, lmi_jpeg_bits_dc_luma, lmi_jpeg_vals_dc_luma);
	lmi_jpeg_build_huff(&hac_y, lmi_jpeg_bits_ac_luma, lmi_jpeg_vals_ac_luma);
	lmi_jpeg_build_huff(&hdc_c, lmi_jpeg_bits_dc_chroma, lmi_jpeg_vals_dc_chroma);
	lmi_jpeg_build_huff(&hac_c, lmi_jpeg_bits_ac_chroma, lmi_jpeg_vals_ac_chroma);

	lmi_jpeg_emit_header(&w, width, height, 0x22, qy, qc);

	for (my = 0; my < height; my += 16) {
		for (mx = 0; mx < width; mx += 16) {
			double yblk[4][64];
			double cbblk[64];
			double crblk[64];
			for (int yy = 0; yy < 16; yy++) {
				for (int xx = 0; xx < 16; xx++) {
					int yv, cb, cr;
					const uint8_t *p = lmi_jpeg_rgb_at(rgb, width, height, stride, mx + xx, my + yy);
					lmi_jpeg_rgb_to_ycbcr(p, &yv, &cb, &cr);
					int bi = (yy >= 8 ? 2 : 0) + (xx >= 8 ? 1 : 0);
					yblk[bi][(yy & 7) * 8 + (xx & 7)] = (double)yv - 128.0;
				}
			}
			for (int cy = 0; cy < 8; cy++) {
				for (int cx = 0; cx < 8; cx++) {
					int sum_cb = 0, sum_cr = 0;
					for (int dy = 0; dy < 2; dy++) {
						for (int dx = 0; dx < 2; dx++) {
							int yv, cb, cr;
							const uint8_t *p = lmi_jpeg_rgb_at(rgb, width, height, stride,
										       mx + cx * 2 + dx, my + cy * 2 + dy);
							lmi_jpeg_rgb_to_ycbcr(p, &yv, &cb, &cr);
							(void)yv;
							sum_cb += cb;
							sum_cr += cr;
						}
					}
					cbblk[cy * 8 + cx] = (double)((sum_cb + 2) >> 2) - 128.0;
					crblk[cy * 8 + cx] = (double)((sum_cr + 2) >> 2) - 128.0;
				}
			}
			lmi_jpeg_encode_block(&w, yblk[0], qy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block(&w, yblk[1], qy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block(&w, yblk[2], qy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block(&w, yblk[3], qy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block(&w, cbblk, qc, &hdc_c, &hac_c, &pred_cb);
			lmi_jpeg_encode_block(&w, crblk, qc, &hdc_c, &hac_c, &pred_cr);
			if (w.err)
				return -1;
		}
	}
	lmi_jpeg_flush_bits(&w);
	lmi_jpeg_marker(&w, 0xd9); /* EOI */
	if (w.err)
		return -1;
	return (int)w.pos;
}

static int lmi_jpeg_encode_rgb444(uint8_t *dst, size_t cap, const uint8_t *rgb,
				       int width, int height, int stride, int quality)
{
	struct lmi_jpeg_writer w;
	struct lmi_jpeg_huff hdc_y, hac_y, hdc_c, hac_c;
	uint8_t qy[64], qc[64];
	int pred_y = 0, pred_cb = 0, pred_cr = 0;
	int mx, my;
	if (!dst || !rgb || width <= 0 || height <= 0 || stride < width * 3)
		return -1;
	memset(&w, 0, sizeof(w));
	w.buf = dst;
	w.cap = cap;
	lmi_jpeg_make_qtable(qy, lmi_jpeg_q_luma_base, quality);
	lmi_jpeg_make_qtable(qc, lmi_jpeg_q_chroma_base, quality);
	lmi_jpeg_build_huff(&hdc_y, lmi_jpeg_bits_dc_luma, lmi_jpeg_vals_dc_luma);
	lmi_jpeg_build_huff(&hac_y, lmi_jpeg_bits_ac_luma, lmi_jpeg_vals_ac_luma);
	lmi_jpeg_build_huff(&hdc_c, lmi_jpeg_bits_dc_chroma, lmi_jpeg_vals_dc_chroma);
	lmi_jpeg_build_huff(&hac_c, lmi_jpeg_bits_ac_chroma, lmi_jpeg_vals_ac_chroma);

	lmi_jpeg_emit_header(&w, width, height, 0x11, qy, qc);

	for (my = 0; my < height; my += 8) {
		for (mx = 0; mx < width; mx += 8) {
			double yblk[64];
			double cbblk[64];
			double crblk[64];
			for (int yy = 0; yy < 8; yy++) {
				for (int xx = 0; xx < 8; xx++) {
					int yv, cb, cr;
					int bi = yy * 8 + xx;
					const uint8_t *p = lmi_jpeg_rgb_at(rgb, width, height, stride, mx + xx, my + yy);
					lmi_jpeg_rgb_to_ycbcr(p, &yv, &cb, &cr);
					yblk[bi] = (double)yv - 128.0;
					cbblk[bi] = (double)cb - 128.0;
					crblk[bi] = (double)cr - 128.0;
				}
			}
			lmi_jpeg_encode_block(&w, yblk, qy, &hdc_y, &hac_y, &pred_y);
			lmi_jpeg_encode_block(&w, cbblk, qc, &hdc_c, &hac_c, &pred_cb);
			lmi_jpeg_encode_block(&w, crblk, qc, &hdc_c, &hac_c, &pred_cr);
			if (w.err)
				return -1;
		}
	}
	lmi_jpeg_flush_bits(&w);
	lmi_jpeg_marker(&w, 0xd9); /* EOI */
	if (w.err)
		return -1;
	return (int)w.pos;
}

#endif /* LMI_JPEG_H */
