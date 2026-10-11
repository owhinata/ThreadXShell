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
 * Not a hardware limit -- a limit on the arithmetic, and since issue #60 it is
 * what makes the whole resize fit in 32-bit integers.  Every quantity below is
 * bounded by it:
 *
 *   walk base   num(k)/den < 2 * extent * 2^15 = extent * 2^16 <= 2.68e8
 *   walk bias   (origin << 16)                                 <= 2.68e8
 *   coordinate  base + bias                                    <= 5.37e8
 *   walk step   (2 * extent * 2^15) / den                      <= 2.68e8
 *   remainders  < den                                          <= 4096
 *
 * The largest is 5.37e8, a quarter of a signed 32-bit value's range.  That
 * matters on a Cortex-M55: 32-bit divide and multiply are single instructions
 * where the 64-bit forms are calls and register pairs, and the inner loop runs
 * 16,384 times per frame.  Anything larger is refused rather than wrapped.
 *
 * [!] The 2^15 rather than 2^16 in num(k) is not cosmetic -- it is what keeps
 * `base` inside the bound above.  See nn_walk_init().
 */
#define NN_MAX_DIM    4096u

/* 1.0 in Q16.  A positive constant, so shifting BY it is fine; what follows is
 * careful never to shift a negative value. */
#define NN_ONE_Q16 ((int32_t)1 << NN_Q)

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
static int32_t nn_floor_q16(int32_t q)
{
	int32_t i = q / NN_ONE_Q16;

	if (q < 0 && i * NN_ONE_Q16 != q)
		i -= 1;
	return i;
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
static uint32_t nn_frac_q8(int32_t q, int32_t floor_i)
{
	int32_t frac = q - floor_i * NN_ONE_Q16;

	return (uint32_t)(frac >> 8) & 0xFFu;
}

/*
 * Source sample index for destination index k, half-pixel centres:
 *
 *     src(k) = origin + (k + 0.5) * extent / dst_extent - 0.5
 *
 * in Q16, which rearranges to
 *
 *     src(k) = num(k) / dst_extent + (origin << 16) - 0.5,
 *     num(k) = (2k + 1) * extent * 2^15
 *
 * so that the only division is one exact integer division of non-negative
 * operands -- its rounding is therefore plain truncation and never meets C's
 * toward-zero behaviour on a negative value.
 *
 * ---------------------------------------------------------------------------
 * [!] WHY THIS IS A WALK AND NOT A DIVISION PER PIXEL (issue #60).
 *
 * It used to evaluate the formula above at every destination pixel, and the
 * comment defending that said: computed per pixel "rather than accumulated by
 * adding a step, so truncation cannot drift along a row".  The hazard is real
 * -- adding a TRUNCATED step per pixel accumulates its rounding error -- but
 * the conclusion did not follow, and the price was severe: dst_extent is a
 * variable, so this is a 64-bit division by a non-constant, which on a
 * Cortex-M55 is a call to __aeabi_ldivmod.  Measured at 10,305 us per frame
 * for a 240x240 -> 128x128 resize, about 250 cycles per output pixel for
 * twelve multiply-adds -- the single largest term in `nn stream`'s producer
 * work after the inference itself.
 *
 * The walk below is the same sequence, EXACT, because it carries the remainder
 * rather than dropping it.  num(k) advances by a constant
 *
 *     step = num(k+1) - num(k) = 2 * extent * 2^15,
 *
 * so with (q, r) = (num/den, num%den) held together, one step is
 *
 *     q += step/den;  r += step%den;  if (r >= den) { r -= den; q += 1; }
 *
 * and the carry runs at most once because both r and step%den are < den.  No
 * information is discarded at any step, so this is not an accumulation of
 * truncated values -- it is the same quotient the division would have
 * produced, for every k.  test_nn_preproc.c pins that against an independent
 * closed-form oracle rather than leaving it as an argument.
 *
 * [!] AND IT IS WHAT LETS THE WHOLE THING BE 32-BIT.  The 64-bit intermediate
 * this file used to carry was needed for num(k) -- (2k+1) * extent * 2^15
 * reaches 1.1e15 at NN_MAX_DIM -- and num(k) is exactly what the walk never
 * forms.  What is left is the quotient and a remainder below den, both inside
 * the bounds listed at NN_MAX_DIM.  On this core that is the difference
 * between single instructions and library calls, 16,384 times a frame.
 */
struct nn_src_walk {
	int32_t  base;    /* num(k) / den                                  */
	uint32_t rem;     /* num(k) % den, which is what makes it exact    */
	uint32_t den;     /* dst_extent                                    */
	uint32_t step_q;  /* step / den                                    */
	uint32_t step_r;  /* step % den, always < den                      */
	int32_t  bias;    /* (origin << NN_Q) - 0.5, added on read         */
};

/*
 * Seed at k = 0.  The four divisions here are the only ones left, and they are
 * per FRAME -- see nn_preproc_fill().  They are 32-bit, which this core does in
 * hardware.
 *
 * num0 and step are formed in uint32 and that is checked, not assumed:
 * extent <= NN_MAX_DIM = 4096, so num0 = extent * 2^15 <= 1.34e8 and
 * step = 2 * num0 <= 2.68e8, both well inside uint32.
 */
static void nn_walk_init(struct nn_src_walk *w, uint32_t origin,
                         uint32_t extent, uint32_t dst_extent)
{
	uint32_t num0 = extent * (uint32_t)(1 << (NN_Q - 1));
	uint32_t step = 2u * num0;

	w->den    = dst_extent;
	w->base   = (int32_t)(num0 / w->den);
	w->rem    = num0 % w->den;
	w->step_q = step / w->den;
	w->step_r = step % w->den;
	w->bias   = (int32_t)(origin << NN_Q) - NN_HALF_Q16;
}

static int32_t nn_walk_q16(const struct nn_src_walk *w)
{
	return w->base + w->bias;
}

static void nn_walk_step(struct nn_src_walk *w)
{
	w->base += (int32_t)w->step_q;
	w->rem  += w->step_r;
	/* At most one carry: r < den and step_r < den, so r + step_r < 2*den. */
	if (w->rem >= w->den) {
		w->rem  -= w->den;
		w->base += 1;
	}
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
	struct nn_src_walk wx_row, wy;
	uint32_t plane;

	if (bgr == NULL || g == NULL || dst == NULL)
		return -1;
	if (frame_w == 0u || frame_h == 0u)
		return -1;
	if (g->x + g->w > frame_w || g->y + g->h > frame_h)
		return -1;
	if (g->w == 0u || g->h == 0u || g->dst_w == 0u || g->dst_h == 0u)
		return -1;
	/*
	 * [!] AND THE BOUND THE 32-BIT ARITHMETIC RESTS ON (issue #60).
	 *
	 * nn_preproc_geom() already refuses anything larger, so on every path
	 * this port actually takes these are redundant -- which is precisely the
	 * argument that would let them be omitted, and precisely why they are
	 * here.  This function takes a caller-supplied struct and does not know
	 * that geom() built it; without this a hand-made one wraps num0 in
	 * nn_walk_init() and the resize reads outside the frame.  The check that
	 * makes a bound true has to sit where the bound is used.
	 *
	 * Bounding frame_w and frame_h bounds the extents too: the test above
	 * has already established g->x + g->w <= frame_w and g->y + g->h <=
	 * frame_h.
	 */
	if (frame_w > NN_MAX_DIM || frame_h > NN_MAX_DIM ||
	    g->dst_w > NN_MAX_DIM || g->dst_h > NN_MAX_DIM)
		return -1;

	plane = frame_w * frame_h;
	pb = bgr;
	pg = bgr + plane;
	pr = bgr + 2u * plane;

	/*
	 * The x mapping does not depend on the row, so its seed is computed
	 * ONCE for the frame and each row starts from a copy -- which is what
	 * keeps the divisions in nn_walk_init() off the row loop as well as off
	 * the pixel loop.  Four divisions per frame, against the 16,512 this
	 * function used to do.
	 */
	nn_walk_init(&wx_row, g->x, g->w, g->dst_w);
	nn_walk_init(&wy, g->y, g->h, g->dst_h);

	for (uint32_t dy = 0; dy < g->dst_h; dy++, nn_walk_step(&wy)) {
		struct nn_src_walk wx = wx_row;
		int32_t  sy_q  = nn_walk_q16(&wy);
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

		for (uint32_t dx = 0; dx < g->dst_w; dx++, nn_walk_step(&wx)) {
			int32_t  sx_q = nn_walk_q16(&wx);
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
