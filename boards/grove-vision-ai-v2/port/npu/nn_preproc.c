/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Camera frame -> model input, and detection box -> frame pixels (issue #48).
 * See nn_preproc.h for why this has no dependencies.
 */
#include "nn_preproc.h"

#include <stddef.h>

/*
 * Fixed point.
 *
 * Coordinates are Q16; interpolation weights are Q8.  Both are chosen so the
 * whole pipeline stays in integers a Cortex-M55 multiplies in one instruction,
 * and so that the bounds below are provable rather than plausible.
 */
#define NN_Q          16
#define NN_HALF_Q16   (1 << (NN_Q - 1))    /* 0.5 in Q16 */
#define NN_W_ONE      256                  /* 1.0 in Q8  */

/*
 * The largest frame or model dimension this will compute with.
 *
 * Not a hardware limit -- a limit on the arithmetic.  The coordinate numerator
 * below is (2*dst + 1) * extent * 2^15; at 4096 that is about 1.1e15, which a
 * signed 64-bit value holds with four orders of magnitude to spare.  Anything
 * larger is refused rather than wrapped.
 */
#define NN_MAX_DIM    4096u

/* 1.0 in Q16.  A positive constant, so shifting BY it is fine; what follows is
 * careful never to shift a negative value. */
#define NN_ONE_Q16 ((int64_t)1 << NN_Q)

/*
 * [!] MATHEMATICAL FLOOR, not C truncation -- and no shifting of negatives.
 *
 * Splitting a Q16 coordinate into an integer index and a fraction has to round
 * DOWN, including for negative coordinates -- and negative coordinates are
 * reachable, not theoretical: the -0.5 of the half-pixel convention makes the
 * first destination pixel negative whenever the scale is an upscale.
 *
 * Division and multiplication rather than shifts, because BOTH shift forms are
 * a problem on a negative value: `>>` is implementation-defined and `<<` is
 * undefined behaviour.  An earlier version of this function used both while
 * claiming in its comment to avoid exactly that, which is worse than not having
 * the comment.  `/` truncates toward zero by definition, so one correction step
 * turns it into a floor.
 */
static int32_t nn_floor_q16(int64_t q)
{
	int64_t i = q / NN_ONE_Q16;

	if (q < 0 && i * NN_ONE_Q16 != q)
		i -= 1;
	return (int32_t)i;
}

/*
 * The Q8 fraction of a Q16 coordinate, measured FROM ITS FLOOR.
 *
 * Taking it relative to the floor is what makes it non-negative by
 * construction -- it lies in [0, 65536) whatever the sign of @p q -- so the one
 * shift here is on a positive value and needs no argument about signedness.
 * Truncated, and paired with (NN_W_ONE - f), so the two weights sum to exactly
 * 256 and no rounding step exists that could produce 257.
 */
static uint32_t nn_frac_q8(int64_t q, int32_t floor_i)
{
	int64_t frac = q - (int64_t)floor_i * NN_ONE_Q16;

	return (uint32_t)(frac >> 8) & 0xFFu;
}

/*
 * Source sample index for destination index @p dst, half-pixel centres:
 *
 *     src = origin + (dst + 0.5) * extent / dst_extent - 0.5
 *
 * in Q16, which rearranges to the form below so that the only division is one
 * exact integer division of non-negative operands -- its rounding is therefore
 * plain truncation and never meets C's toward-zero behaviour on a negative
 * value.  Computed PER DESTINATION PIXEL rather than accumulated by adding a
 * step, so truncation cannot drift along a row.
 *
 * The 64-bit intermediate is a generality requirement, not a fix for an
 * overflow at today's numbers: 240 -> 128 peaks at 255*240*32768, about 2.0e9,
 * which fits a signed 32-bit value at 93% of its range -- but an extent of 320
 * already exceeds it at 2.67e9.  Keeping the width off the shape of whichever
 * model happens to be loaded is worth more than the cycles.
 */
static int64_t nn_src_q16(uint32_t dst, uint32_t origin, uint32_t extent,
                          uint32_t dst_extent)
{
	int64_t num = (int64_t)(2u * dst + 1u) * (int64_t)extent
	              * (int64_t)(1 << (NN_Q - 1));

	return num / (int64_t)dst_extent
	       + ((int64_t)origin << NN_Q)
	       - NN_HALF_Q16;
}

int nn_preproc_geom(uint32_t frame_w, uint32_t frame_h,
                    uint32_t dst_w, uint32_t dst_h,
                    struct nn_preproc_geom *out)
{
	uint32_t w, h;

	if (out == NULL)
		return -1;
	if (frame_w == 0u || frame_h == 0u || dst_w == 0u || dst_h == 0u)
		return -1;
	if (frame_w > NN_MAX_DIM || frame_h > NN_MAX_DIM ||
	    dst_w > NN_MAX_DIM || dst_h > NN_MAX_DIM)
		return -1;

	/*
	 * The largest centred rectangle with the INPUT's aspect ratio.  Compared
	 * as a cross product so there is no division and no float: the crop is
	 * limited by width when frame_w * dst_h <= frame_h * dst_w, and by
	 * height otherwise.  Both products are bounded by NN_MAX_DIM^2, which is
	 * 2^24 -- comfortably inside 32 bits.
	 */
	if ((uint64_t)frame_w * dst_h <= (uint64_t)frame_h * dst_w) {
		w = frame_w;
		h = (uint32_t)(((uint64_t)frame_w * dst_h) / dst_w);
	} else {
		h = frame_h;
		w = (uint32_t)(((uint64_t)frame_h * dst_w) / dst_h);
	}

	/* The integer division above can only round the derived side DOWN, so it
	 * always fits -- but a zero would mean an aspect ratio so extreme the
	 * crop has no area, and that is refused rather than drawn. */
	if (w == 0u || h == 0u)
		return -1;

	out->x     = (frame_w - w) / 2u;
	out->y     = (frame_h - h) / 2u;
	out->w     = w;
	out->h     = h;
	out->dst_w = dst_w;
	out->dst_h = dst_h;
	return 0;
}

int nn_preproc_fill(const uint8_t *bgr, uint32_t frame_w, uint32_t frame_h,
                    const struct nn_preproc_geom *g, uint8_t *dst)
{
	const uint8_t *pb, *pg, *pr;
	uint32_t plane;

	if (bgr == NULL || g == NULL || dst == NULL)
		return -1;
	if (frame_w == 0u || frame_h == 0u)
		return -1;
	if (g->x + g->w > frame_w || g->y + g->h > frame_h)
		return -1;
	if (g->w == 0u || g->h == 0u || g->dst_w == 0u || g->dst_h == 0u)
		return -1;

	plane = frame_w * frame_h;
	pb = bgr;
	pg = bgr + plane;
	pr = bgr + 2u * plane;

	for (uint32_t dy = 0; dy < g->dst_h; dy++) {
		int64_t  sy_q  = nn_src_q16(dy, g->y, g->h, g->dst_h);
		int32_t  y0    = nn_floor_q16(sy_q);
		uint32_t fy    = nn_frac_q8(sy_q, y0);
		uint32_t wy1   = fy;
		uint32_t wy0   = (uint32_t)NN_W_ONE - fy;
		int32_t  y1    = y0 + 1;
		uint32_t row0, row1;

		/* Clamp to the crop.  This is what defines the edge pixels: the
		 * alternative is sampling a row the model was never shown. */
		if (y0 < (int32_t)g->y)
			y0 = (int32_t)g->y;
		if (y1 < (int32_t)g->y)
			y1 = (int32_t)g->y;
		if (y0 > (int32_t)(g->y + g->h - 1u))
			y0 = (int32_t)(g->y + g->h - 1u);
		if (y1 > (int32_t)(g->y + g->h - 1u))
			y1 = (int32_t)(g->y + g->h - 1u);

		row0 = (uint32_t)y0 * frame_w;
		row1 = (uint32_t)y1 * frame_w;

		for (uint32_t dx = 0; dx < g->dst_w; dx++) {
			int64_t  sx_q = nn_src_q16(dx, g->x, g->w, g->dst_w);
			int32_t  x0   = nn_floor_q16(sx_q);
			uint32_t fx   = nn_frac_q8(sx_q, x0);
			uint32_t wx1  = fx;
			uint32_t wx0  = (uint32_t)NN_W_ONE - fx;
			int32_t  x1   = x0 + 1;
			uint32_t w00, w01, w10, w11;
			uint32_t i00, i01, i10, i11;

			if (x0 < (int32_t)g->x)
				x0 = (int32_t)g->x;
			if (x1 < (int32_t)g->x)
				x1 = (int32_t)g->x;
			if (x0 > (int32_t)(g->x + g->w - 1u))
				x0 = (int32_t)(g->x + g->w - 1u);
			if (x1 > (int32_t)(g->x + g->w - 1u))
				x1 = (int32_t)(g->x + g->w - 1u);

			/*
			 * Four taps, Q8 x Q8 = Q16.
			 *
			 * The weights sum to exactly 256 on each axis, so the
			 * whole accumulator is bounded by 255 * 256 * 256 = 2^24
			 * -- seven bits of headroom in a signed 32-bit value, and
			 * no 64-bit intermediate anywhere in the inner loop.  A
			 * bound that holds because of how the weights are BUILT
			 * is worth more than one that holds because the pixels
			 * happened to be small.
			 */
			w00 = wx0 * wy0;
			w01 = wx1 * wy0;
			w10 = wx0 * wy1;
			w11 = wx1 * wy1;

			i00 = row0 + (uint32_t)x0;
			i01 = row0 + (uint32_t)x1;
			i10 = row1 + (uint32_t)x0;
			i11 = row1 + (uint32_t)x1;

#define NN_TAP(p) ((uint32_t)((p)[i00] * w00 + (p)[i01] * w01 + \
                              (p)[i10] * w10 + (p)[i11] * w11 + \
                              (1u << (NN_Q - 1))) >> NN_Q)

			/* Interleaved R, G, B out of planar B, G, R in.  The
			 * uint8 -> int8 shift is a deliberate wrap: the bit
			 * pattern is the answer, the arithmetic is not. */
			*dst++ = (uint8_t)(NN_TAP(pr) - 128u);
			*dst++ = (uint8_t)(NN_TAP(pg) - 128u);
			*dst++ = (uint8_t)(NN_TAP(pb) - 128u);
#undef NN_TAP
		}
	}
	return 0;
}

/*
 * Is @p v a finite float, without <math.h>?
 *
 * The decoder produces its boxes with plain arithmetic and no clamping, so a
 * degenerate model output can reach here as an infinity or a NaN.  A NaN fails
 * every comparison including against itself, which is exactly what makes the
 * pair of tests below reject it.
 */
static int nn_finite(float v)
{
	return (v == v) && (v > -1.0e30f) && (v < 1.0e30f);
}

/* Normalised coordinates outside this are certainly not a face; the range is
 * far outside the image so no plausible detection is altered, and far inside
 * the range where the multiply and the cast below are defined. */
#define NN_NORM_LIMIT 8.0f

static float nn_clampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

/*
 * floor() and ceil() to int, without <math.h>.
 *
 * A float-to-int cast truncates toward zero, which is neither of these for a
 * negative value -- and negative values arrive here routinely, because a box
 * may start off the left edge of the crop.  Both are only ever called on values
 * already clamped to +/-NN_NORM_LIMIT times a dimension bounded by NN_MAX_DIM,
 * so the cast itself is always in range.
 */
static int32_t nn_floorf_to_int(float v)
{
	int32_t i = (int32_t)v;

	if ((float)i > v)
		i -= 1;
	return i;
}

static int32_t nn_ceilf_to_int(float v)
{
	int32_t i = (int32_t)v;

	if ((float)i < v)
		i += 1;
	return i;
}

int nn_preproc_box(const struct nn_preproc_geom *g,
                   float nx, float ny, float nw, float nh,
                   struct nn_preproc_box *out)
{
	float l, t, r, b;
	int32_t x0, y0, x1, y1;

	if (g == NULL || out == NULL)
		return -1;
	if (!nn_finite(nx) || !nn_finite(ny) || !nn_finite(nw) || !nn_finite(nh))
		return -1;

	l = nn_clampf(nx, -NN_NORM_LIMIT, NN_NORM_LIMIT);
	t = nn_clampf(ny, -NN_NORM_LIMIT, NN_NORM_LIMIT);
	r = nn_clampf(nx + nw, -NN_NORM_LIMIT, NN_NORM_LIMIT);
	b = nn_clampf(ny + nh, -NN_NORM_LIMIT, NN_NORM_LIMIT);

	/*
	 * [!] EDGES, so no half-pixel term -- see the file header.  This is the
	 * same transform nn_src_q16() applies, written for a continuous
	 * coordinate instead of a sample index.
	 */
	x0 = nn_floorf_to_int(l * (float)g->w + (float)g->x);
	y0 = nn_floorf_to_int(t * (float)g->h + (float)g->y);
	x1 = nn_ceilf_to_int(r * (float)g->w + (float)g->x);
	y1 = nn_ceilf_to_int(b * (float)g->h + (float)g->y);

	/* Clip to the crop: a box may never name a pixel the model did not
	 * see. */
	if (x0 < (int32_t)g->x)
		x0 = (int32_t)g->x;
	if (y0 < (int32_t)g->y)
		y0 = (int32_t)g->y;
	if (x1 > (int32_t)(g->x + g->w))
		x1 = (int32_t)(g->x + g->w);
	if (y1 > (int32_t)(g->y + g->h))
		y1 = (int32_t)(g->y + g->h);

	if (x1 <= x0 || y1 <= y0)
		return -1;   /* clipped away entirely: draw nothing */

	out->x0 = x0;
	out->y0 = y0;
	out->x1 = x1;
	out->y1 = y1;
	return 0;
}
