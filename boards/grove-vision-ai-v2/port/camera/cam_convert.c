/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Planar B/G/R -> RGB565 packing and per-plane statistics (issue #35).
 * See cam_convert.h for why this is a translation unit of its own.
 */
#include <stddef.h>

#include "cam_convert.h"

/*
 * sRGB encode: linear 8-bit in, display-encoded 8-bit out.
 *
 * WHY THE PIPELINE NEEDS THIS AT ALL.  A camera sensor measures light, so its
 * output is LINEAR in luminance.  Displays -- this panel included, which is why
 * its init table programs gamma curves -- expect the opposite: values already
 * encoded with roughly a 1/2.2 power law, because human vision is far more
 * sensitive to differences in the dark end than in the bright.
 *
 * Sending linear samples to a display that expects encoded ones does not fail;
 * it produces a picture that is dark and flat, with the midtones crushed toward
 * black.  Measured on this board, a perfectly ordinary indoor scene averaged 49
 * -- which as a LINEAR value is a normal midtone, and as a display value is
 * nearly black.  Through this table it becomes 121, and the histogram lands
 * where the eye expects it.
 *
 * Generated from the standard sRGB transfer function:
 *     S = 12.92 * L                     for L <= 0.0031308
 *     S = 1.055 * L^(1/2.4) - 0.055     otherwise
 * with L = i/255, rounded to nearest.  256 bytes of .rodata, no runtime
 * arithmetic and no libm -- which this image does not link.
 */
static const uint8_t cam_srgb_encode[256] = {
	  0,  13,  22,  28,  34,  38,  42,  46,  50,  53,  56,  59,
	 61,  64,  66,  69,  71,  73,  75,  77,  79,  81,  83,  85,
	 86,  88,  90,  92,  93,  95,  96,  98,  99, 101, 102, 104,
	105, 106, 108, 109, 110, 112, 113, 114, 115, 117, 118, 119,
	120, 121, 122, 124, 125, 126, 127, 128, 129, 130, 131, 132,
	133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144,
	145, 146, 147, 148, 148, 149, 150, 151, 152, 153, 154, 155,
	155, 156, 157, 158, 159, 159, 160, 161, 162, 163, 163, 164,
	165, 166, 167, 167, 168, 169, 170, 170, 171, 172, 173, 173,
	174, 175, 175, 176, 177, 178, 178, 179, 180, 180, 181, 182,
	182, 183, 184, 185, 185, 186, 187, 187, 188, 189, 189, 190,
	190, 191, 192, 192, 193, 194, 194, 195, 196, 196, 197, 197,
	198, 199, 199, 200, 200, 201, 202, 202, 203, 203, 204, 205,
	205, 206, 206, 207, 208, 208, 209, 209, 210, 210, 211, 212,
	212, 213, 213, 214, 214, 215, 215, 216, 216, 217, 218, 218,
	219, 219, 220, 220, 221, 221, 222, 222, 223, 223, 224, 224,
	225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231,
	231, 232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237,
	237, 238, 238, 238, 239, 239, 240, 240, 241, 241, 242, 242,
	243, 243, 244, 244, 245, 245, 246, 246, 246, 247, 247, 248,
	248, 249, 249, 250, 250, 251, 251, 251, 252, 252, 253, 253,
	254, 254, 255, 255,
};

/* (v - black) * gain/256: pedestal off first, then scale, saturating at 255. */
static inline uint8_t cam_gain8(uint8_t v, uint16_t gain, uint8_t black,
                                int gamma)
{
	uint32_t s;

	v = (v > black) ? (uint8_t)(v - black) : 0u;
	s = ((uint32_t)v * gain) >> 8;
	if (s > 255u)
		s = 255u;
	/* Encode LAST.  Black level and gain are corrections to a LINEAR
	 * measurement and only mean what they say in linear space; encoding
	 * first would make the gain a contrast control and the black level a
	 * crush. */
	return gamma ? cam_srgb_encode[s] : (uint8_t)s;
}

/* Push a channel away from the pixel's own luma: out = y + (v - y) * sat.
 * Saturating, and signed in the middle because v - y is a difference. */
static inline uint8_t cam_sat8(uint8_t v, uint32_t y, uint16_t sat)
{
	int32_t d = ((int32_t)v - (int32_t)y) * (int32_t)sat / 256;
	int32_t s = (int32_t)y + d;

	if (s < 0)
		return 0u;
	if (s > 255)
		return 255u;
	return (uint8_t)s;
}

void cam_bgr_planar_to_rgb565(const uint8_t *bgr, uint16_t *out,
                              uint32_t pixels)
{
	cam_bgr_planar_to_rgb565_wb(bgr, out, pixels, 0);
}

void cam_bgr_planar_to_rgb565_wb(const uint8_t *bgr, uint16_t *out,
                                 uint32_t pixels, const struct cam_wb *wb)
{
	const uint8_t *b = bgr;
	const uint8_t *g = bgr + pixels;
	const uint8_t *r = bgr + 2u * pixels;
	uint32_t i;

	if (bgr == 0 || out == 0)
		return;

	/* Three sequential reads and one sequential write per pixel.  Written as
	 * plain scalar C on purpose: the obvious "improvement" here is to widen
	 * it, and MVE is exactly what the image may not contain. */
	if (wb == 0 || (wb->r == CAM_WB_UNITY && wb->g == CAM_WB_UNITY &&
	                wb->b == CAM_WB_UNITY && wb->black == 0u &&
	                wb->gamma == 0u &&
	                (wb->sat == CAM_SAT_UNITY || wb->sat == 0u))) {
		/* Unity is the common case and worth not paying for: three
		 * multiplies and three compares per pixel over 76,800 pixels is
		 * not free, and a preview that has not been balanced yet should
		 * cost exactly what it did before this existed. */
		for (i = 0u; i < pixels; i++)
			out[i] = (uint16_t)(((uint16_t)(r[i] >> 3) << 11) |
			                    ((uint16_t)(g[i] >> 2) << 5) |
			                     (uint16_t)(b[i] >> 3));
		return;
	}

	for (i = 0u; i < pixels; i++) {
		/* Black level and white balance first, in linear light and
		 * without the encode -- saturation is a linear-light operation
		 * and the curve has to come after it. */
		uint8_t rv = cam_gain8(r[i], wb->r, wb->black, 0);
		uint8_t gv = cam_gain8(g[i], wb->g, wb->black, 0);
		uint8_t bv = cam_gain8(b[i], wb->b, wb->black, 0);

		if (wb->sat != CAM_SAT_UNITY && wb->sat != 0u) {
			/* Luma, weighted the way the eye is: green carries most
			 * of the brightness.  (R + 2G + B) / 4 is the cheap
			 * standard approximation and needs no multiplies. */
			uint32_t y = ((uint32_t)rv + 2u * gv + bv) / 4u;

			rv = cam_sat8(rv, y, wb->sat);
			gv = cam_sat8(gv, y, wb->sat);
			bv = cam_sat8(bv, y, wb->sat);
		}

		if (wb->gamma) {
			rv = cam_srgb_encode[rv];
			gv = cam_srgb_encode[gv];
			bv = cam_srgb_encode[bv];
		}

		out[i] = (uint16_t)(((uint16_t)(rv >> 3) << 11) |
		                    ((uint16_t)(gv >> 2) << 5) |
		                     (uint16_t)(bv >> 3));
	}
}

void cam_plane_stats(const uint8_t *plane, uint32_t pixels,
                     struct cam_plane_stats *out)
{
	uint32_t sum = 0u;
	uint8_t  min = 255u;
	uint8_t  max = 0u;
	uint32_t i;

	if (out == 0)
		return;
	if (plane == 0 || pixels == 0u) {
		out->min = 255u;
		out->max = 0u;
		out->mean_x100 = 0u;
		return;
	}

	for (i = 0u; i < pixels; i++) {
		uint8_t v = plane[i];

		if (v < min)
			min = v;
		if (v > max)
			max = v;
		sum += v;
	}

	out->min = min;
	out->max = max;
	/* 320*240 pixels * 255 * 100 fits in 32 bits with room to spare
	 * (1.96e9 < 4.29e9), so the scaling happens before the divide and the
	 * mean keeps two decimals without any floating point. */
	out->mean_x100 = (sum * 100u) / pixels;
}

uint32_t cam_plane_mosaic_x100(const uint8_t *plane, uint32_t w, uint32_t h)
{
	uint32_t sum[4] = { 0u, 0u, 0u, 0u };
	uint32_t cnt[4] = { 0u, 0u, 0u, 0u };
	uint32_t mean[4];
	uint32_t lo, hi, i, x, y;

	if (plane == 0 || w < 2u || h < 2u)
		return 0u;

	for (y = 0u; y < h; y++) {
		const uint8_t *row = plane + (size_t)y * w;
		uint32_t phase_y = (y & 1u) << 1;

		for (x = 0u; x < w; x++) {
			uint32_t ph = phase_y | (x & 1u);

			sum[ph] += row[x];
			cnt[ph]++;
		}
	}

	for (i = 0u; i < 4u; i++) {
		if (cnt[i] == 0u)
			return 0u;
		mean[i] = (sum[i] * 100u) / cnt[i];
	}

	lo = mean[0];
	hi = mean[0];
	for (i = 1u; i < 4u; i++) {
		if (mean[i] < lo)
			lo = mean[i];
		if (mean[i] > hi)
			hi = mean[i];
	}
	return hi - lo;
}

void cam_frame_means_x100(const uint8_t *bgr, uint32_t pixels, uint32_t step,
                          uint32_t out[3])
{
	uint32_t sum[3] = { 0u, 0u, 0u };
	uint32_t n = 0u;
	uint32_t i, c;

	if (out == 0)
		return;
	for (c = 0u; c < 3u; c++)
		out[c] = 0u;
	if (bgr == 0 || pixels == 0u || step == 0u)
		return;

	for (i = 0u; i < pixels; i += step) {
		sum[0] += bgr[i];
		sum[1] += bgr[pixels + i];
		sum[2] += bgr[2u * pixels + i];
		n++;
	}
	if (n == 0u)
		return;
	for (c = 0u; c < 3u; c++)
		out[c] = (sum[c] * 100u) / n;
}

void cam_bayer_phase_means_x100(const uint8_t *plane, uint32_t w, uint32_t h,
                                uint32_t out[4])
{
	uint32_t sum[4] = { 0u, 0u, 0u, 0u };
	uint32_t cnt[4] = { 0u, 0u, 0u, 0u };
	uint32_t i, x, y;

	if (out == 0)
		return;
	for (i = 0u; i < 4u; i++)
		out[i] = 0u;
	if (plane == 0 || w < 2u || h < 2u)
		return;

	for (y = 0u; y < h; y++) {
		const uint8_t *row = plane + (size_t)y * w;
		uint32_t phase_y = (y & 1u) << 1;

		for (x = 0u; x < w; x++) {
			uint32_t ph = phase_y | (x & 1u);

			sum[ph] += row[x];
			cnt[ph]++;
		}
	}
	for (i = 0u; i < 4u; i++)
		if (cnt[i] != 0u)
			out[i] = (sum[i] * 100u) / cnt[i];
}

uint32_t cam_frame_colour_x100(const uint8_t *bgr, uint32_t pixels)
{
	const uint8_t *b = bgr;
	const uint8_t *g = bgr + pixels;
	const uint8_t *r = bgr + 2u * pixels;
	uint32_t sum = 0u;
	uint32_t i;

	if (bgr == 0 || pixels == 0u)
		return 0u;

	for (i = 0u; i < pixels; i++) {
		uint8_t lo = b[i], hi = b[i];

		if (g[i] < lo) lo = g[i];
		if (g[i] > hi) hi = g[i];
		if (r[i] < lo) lo = r[i];
		if (r[i] > hi) hi = r[i];
		sum += (uint32_t)(hi - lo);
	}
	/* 320*240 * 255 * 100 = 1.96e9: fits, but only just, so the scale goes
	 * on the sum and not on the accumulator. */
	return (sum * 100u) / pixels;
}

uint32_t cam_plane_row_seam_x100(const uint8_t *plane, uint32_t w, uint32_t h,
                                 uint32_t *row_out)
{
	uint32_t prev_mean_x100 = 0u;
	uint32_t worst = 0u;
	uint32_t worst_row = 0u;
	uint32_t y;

	if (row_out != 0)
		*row_out = 0u;
	if (plane == 0 || w == 0u || h < 2u)
		return 0u;

	for (y = 0u; y < h; y++) {
		const uint8_t *row = plane + (size_t)y * w;
		uint32_t sum = 0u;
		uint32_t mean_x100;
		uint32_t x;

		for (x = 0u; x < w; x++)
			sum += row[x];
		mean_x100 = (sum * 100u) / w;

		if (y > 0u) {
			uint32_t step = (mean_x100 > prev_mean_x100) ?
			                (mean_x100 - prev_mean_x100) :
			                (prev_mean_x100 - mean_x100);

			if (step > worst) {
				worst = step;
				worst_row = y;
			}
		}
		prev_mean_x100 = mean_x100;
	}

	if (row_out != 0)
		*row_out = worst_row;
	return worst;
}
