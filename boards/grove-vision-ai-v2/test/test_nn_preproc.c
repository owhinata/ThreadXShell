/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_nn_preproc.c
 * @brief   Host tests for port/npu/nn_preproc.c (issue #48).
 *
 * The REAL preprocessing is compiled here, not a copy of its arithmetic -- the
 * same arrangement as test_blazeface.c, and for the same reason: a
 * transcription drifts from the firmware's and nothing notices.  It is testable
 * at all because nn_preproc.c takes plain buffers and dimensions and reaches
 * into no singleton.
 *
 * WHY THESE CASES.  A resize is the easiest thing in this firmware to get
 * subtly wrong and the hardest to see: the picture still looks like a picture
 * and the boxes still look like boxes.  What is pinned here is every choice
 * that would survive a glance at the panel:
 *
 *   - THE HALF-PIXEL CONVENTION.  Half a source pixel of bias shifts every box
 *     by the same amount; against a face it is invisible.  A linear ramp is the
 *     probe that catches it, because its resampled values are computable by
 *     hand.
 *   - THE SAMPLE/EDGE DISTINCTION.  The sampling map carries the -0.5 and the
 *     box-edge map must NOT.  Applying one convention to both is the single
 *     most plausible way to write this file wrong.
 *   - FLOOR VERSUS TRUNCATION.  Negative coordinates are reachable through the
 *     upscale path, where C's toward-zero cast is off by one.
 *   - THE ACCUMULATOR BOUND, exercised at saturation rather than argued about.
 *   - THE UNDEFINED-BEHAVIOUR EDGE: a non-finite box coordinate from the
 *     decoder, which neither clamps nor bounds its floats.
 */
#include "nn_preproc.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int ok, const char *fmt, ...)
{
	if (ok)
		return;
	failures++;
	printf("  FAIL %s: ", what);
	{
		va_list ap;
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
	printf("\n");
}

/* --- geometry ------------------------------------------------------------- */

static void test_geom_square_on_qvga(void)
{
	struct nn_preproc_geom g;

	/* The case the whole issue is about: a square model on this board's
	 * 320x240 frame takes the full height and is centred horizontally. */
	expect("320x240 -> 128x128 succeeds",
	       nn_preproc_geom(320, 240, 128, 128, &g) == 0, "refused");
	expect("the crop is the full height", g.h == 240u, "got %u", g.h);
	expect("the crop is square", g.w == 240u, "got %u", g.w);
	expect("the crop is centred horizontally", g.x == 40u, "got %u", g.x);
	expect("the crop starts at the top", g.y == 0u, "got %u", g.y);
}

static void test_geom_aspect_not_scale(void)
{
	struct nn_preproc_geom g;

	/* Wider than the frame: limited by width, so the crop is a letterbox
	 * band and the height is derived. */
	expect("a 2:1 input succeeds",
	       nn_preproc_geom(320, 240, 320, 160, &g) == 0, "refused");
	expect("a 2:1 input takes the full width", g.w == 320u, "got %u", g.w);
	expect("a 2:1 input derives the height", g.h == 160u, "got %u", g.h);
	expect("a 2:1 crop is centred vertically", g.y == 40u, "got %u", g.y);
}

static void test_geom_upscale_allowed(void)
{
	struct nn_preproc_geom g;

	/* [!] The code this replaced REFUSED an input larger than the frame.
	 * The crop follows the aspect ratio, not the direction of the scale, so
	 * there was never a reason. */
	expect("an input larger than the frame is allowed",
	       nn_preproc_geom(320, 240, 512, 512, &g) == 0, "refused");
	expect("the upscale crop is still the centred square",
	       g.w == 240u && g.h == 240u && g.x == 40u, "got %ux%u at %u",
	       g.w, g.h, g.x);
}

static void test_geom_degenerate_refused(void)
{
	struct nn_preproc_geom g;

	expect("a zero frame width is refused",
	       nn_preproc_geom(0, 240, 128, 128, &g) != 0, "accepted");
	expect("a zero input height is refused",
	       nn_preproc_geom(320, 240, 128, 0, &g) != 0, "accepted");
	expect("an out-of-range dimension is refused",
	       nn_preproc_geom(320, 240, 100000, 128, &g) != 0, "accepted");
	expect("a NULL output is refused",
	       nn_preproc_geom(320, 240, 128, 128, NULL) != 0, "accepted");
}

/* --- sampling ------------------------------------------------------------- */

/* Build a planar B/G/R frame from a per-pixel function. */
static void fill_frame(uint8_t *bgr, uint32_t w, uint32_t h,
                       uint8_t (*fn)(uint32_t x, uint32_t y, int plane))
{
	for (int p = 0; p < 3; p++)
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
				bgr[(size_t)p * w * h + (size_t)y * w + x] =
					fn(x, y, p);
}

static uint8_t ramp_x(uint32_t x, uint32_t y, int plane)
{
	(void)y; (void)plane;
	return (uint8_t)x;
}

static uint8_t chan_id(uint32_t x, uint32_t y, int plane)
{
	(void)x; (void)y;
	/* plane 0 = B, 1 = G, 2 = R on the wire from WDMA3. */
	return (uint8_t)(plane == 0 ? 10 : (plane == 1 ? 20 : 30));
}

static uint8_t saturated(uint32_t x, uint32_t y, int plane)
{
	(void)x; (void)y; (void)plane;
	return 255;
}

/* --- the incremental walk against a closed-form oracle (issue #60) --------- */

/*
 * An INDEPENDENT implementation of the whole resize, written the obvious way:
 * the closed form evaluated with a fresh 64-bit division at every destination
 * pixel, exactly as nn_preproc.c did before #60 replaced it with a stepped
 * walk.  Not a copy of the new arithmetic -- if it were, it could not catch
 * the failure it exists for.
 *
 * WHY IT IS HERE AT ALL.  The other cases in this file pin the CONVENTIONS
 * (half-pixel centres, floor, edges) on small hand-computable images.  They
 * cannot see the failure a stepped walk has: a per-step rounding error is zero
 * for the first few pixels and grows with k, so a 4- or 8-wide case passes
 * while a real 128-wide one is wrong at the right-hand edge.  This compares
 * every byte at a width larger than any model input this port loads, which is
 * the only shape of test that could fail.
 *
 * VERIFIED TO FAIL: dropping the carry in nn_walk_step() (the one line that
 * makes the walk exact rather than an accumulation of truncated steps) makes
 * this report thousands of differing bytes, while every other case in this
 * file still passes.
 */
static int64_t oracle_src_q16(uint32_t dst, uint32_t origin, uint32_t extent,
                              uint32_t dst_extent)
{
	int64_t num = (int64_t)(2u * dst + 1u) * (int64_t)extent * (int64_t)32768;

	return num / (int64_t)dst_extent + ((int64_t)origin << 16) - 32768;
}

static int32_t oracle_floor_q16(int64_t q)
{
	int64_t i = q / 65536;

	if (q < 0 && i * 65536 != q)
		i -= 1;
	return (int32_t)i;
}

static uint32_t oracle_clamp(int32_t v, uint32_t lo, uint32_t hi)
{
	if (v < (int32_t)lo)
		return lo;
	if (v > (int32_t)hi)
		return hi;
	return (uint32_t)v;
}

static void oracle_fill(const uint8_t *bgr, uint32_t frame_w, uint32_t frame_h,
                        const struct nn_preproc_geom *g, uint8_t *dst)
{
	uint32_t plane = frame_w * frame_h;

	for (uint32_t dy = 0; dy < g->dst_h; dy++) {
		int64_t  sy = oracle_src_q16(dy, g->y, g->h, g->dst_h);
		int32_t  iy = oracle_floor_q16(sy);
		uint32_t fy = (uint32_t)((sy - (int64_t)iy * 65536) >> 8) & 0xFFu;
		uint32_t y0 = oracle_clamp(iy,     g->y, g->y + g->h - 1u);
		uint32_t y1 = oracle_clamp(iy + 1, g->y, g->y + g->h - 1u);

		for (uint32_t dx = 0; dx < g->dst_w; dx++) {
			int64_t  sx = oracle_src_q16(dx, g->x, g->w, g->dst_w);
			int32_t  ix = oracle_floor_q16(sx);
			uint32_t fx = (uint32_t)((sx - (int64_t)ix * 65536) >> 8)
			              & 0xFFu;
			uint32_t x0 = oracle_clamp(ix,     g->x, g->x + g->w - 1u);
			uint32_t x1 = oracle_clamp(ix + 1, g->x, g->x + g->w - 1u);
			uint32_t w00 = (256u - fx) * (256u - fy);
			uint32_t w01 = fx * (256u - fy);
			uint32_t w10 = (256u - fx) * fy;
			uint32_t w11 = fx * fy;
			size_t i00 = (size_t)y0 * frame_w + x0;
			size_t i01 = (size_t)y0 * frame_w + x1;
			size_t i10 = (size_t)y1 * frame_w + x0;
			size_t i11 = (size_t)y1 * frame_w + x1;
			/* planar B,G,R in -> interleaved R,G,B out */
			static const int order[3] = { 2, 1, 0 };

			for (int c = 0; c < 3; c++) {
				const uint8_t *p = bgr + (size_t)order[c] * plane;
				uint32_t v = (p[i00] * w00 + p[i01] * w01 +
				              p[i10] * w10 + p[i11] * w11 +
				              32768u) >> 16;

				*dst++ = (uint8_t)(v - 128u);
			}
		}
	}
}

static void test_walk_matches_the_closed_form(void)
{
	/* Deliberately not a power of two in either direction, and wider than
	 * any model input this port loads, so a per-step error has room to
	 * accumulate and no ratio divides evenly. */
	enum { W = 320, H = 240, DW = 200, DH = 137 };
	static uint8_t frame[W * H * 3];
	static uint8_t got[DW * DH * 3];
	static uint8_t want[DW * DH * 3];
	struct nn_preproc_geom g;
	size_t bad = 0, first = 0;

	/* A ramp in x and y at once, so a drift in EITHER axis shows up. */
	for (int p = 0; p < 3; p++)
		for (uint32_t y = 0; y < H; y++)
			for (uint32_t x = 0; x < W; x++)
				frame[(size_t)p * W * H + (size_t)y * W + x] =
					(uint8_t)((x * 7u + y * 13u + (uint32_t)p * 29u)
					          & 0xFFu);

	expect("oracle geometry", nn_preproc_geom(W, H, DW, DH, &g) == 0,
	       "refused");
	expect("oracle fill", nn_preproc_fill(frame, W, H, &g, got) == 0,
	       "refused");
	oracle_fill(frame, W, H, &g, want);

	for (size_t i = 0; i < sizeof got; i++)
		if (got[i] != want[i]) {
			if (bad == 0)
				first = i;
			bad++;
		}

	expect("the stepped walk equals the closed form byte for byte",
	       bad == 0, "%zu of %zu bytes differ, first at %zu (got %d want %d)",
	       bad, sizeof got, first, got[first], want[first]);
}

/*
 * The same equivalence at the shape the firmware actually runs, and at an
 * upscale -- where the walk's coordinates go NEGATIVE and the carry interacts
 * with the floor correction.
 */
static void test_walk_matches_at_real_shapes(void)
{
	enum { W = 320, H = 240 };
	static uint8_t frame[W * H * 3];
	static uint8_t got[240 * 240 * 3];
	static uint8_t want[240 * 240 * 3];
	static const uint32_t shapes[][2] = {
		{ 128, 128 },   /* BlazeFace: what `nn stream` runs */
		{  96,  96 },   /* a smaller classifier             */
		{ 240, 240 },   /* upscale in x, identity in y      */
	};

	for (int p = 0; p < 3; p++)
		for (uint32_t y = 0; y < H; y++)
			for (uint32_t x = 0; x < W; x++)
				frame[(size_t)p * W * H + (size_t)y * W + x] =
					(uint8_t)((x * 3u + y * 5u + (uint32_t)p) & 0xFFu);

	for (size_t s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
		struct nn_preproc_geom g;
		size_t n;

		expect("shape geometry",
		       nn_preproc_geom(W, H, shapes[s][0], shapes[s][1], &g) == 0,
		       "refused %ux%u", shapes[s][0], shapes[s][1]);
		expect("shape fill",
		       nn_preproc_fill(frame, W, H, &g, got) == 0,
		       "refused %ux%u", shapes[s][0], shapes[s][1]);
		oracle_fill(frame, W, H, &g, want);

		n = (size_t)g.dst_w * g.dst_h * 3u;
		expect("the walk equals the closed form at a real shape",
		       memcmp(got, want, n) == 0, "%ux%u differs",
		       shapes[s][0], shapes[s][1]);
	}
}

static void test_identity_scale_is_a_copy(void)
{
	enum { W = 16, H = 16 };
	static uint8_t frame[W * H * 3];
	static uint8_t dst[W * H * 3];
	struct nn_preproc_geom g;
	int ok = 1;

	fill_frame(frame, W, H, ramp_x);
	expect("identity geometry", nn_preproc_geom(W, H, W, H, &g) == 0, "refused");
	expect("identity fill", nn_preproc_fill(frame, W, H, &g, dst) == 0, "refused");

	/*
	 * A 1:1 scale must reproduce the source EXACTLY.  With half-pixel
	 * centres the destination centre lands precisely on the source centre,
	 * so the fraction is zero and one tap carries the whole weight -- which
	 * is the cheapest complete check that the convention and the rounding
	 * agree.  A resize with an off-by-half convention fails here.
	 */
	for (uint32_t y = 0; y < H && ok; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint8_t want = (uint8_t)((uint8_t)x - 128u);

			if (dst[((size_t)y * W + x) * 3 + 0] != want) {
				expect("identity scale is a pure copy", 0,
				       "at (%u,%u) got %d want %d", x, y,
				       dst[((size_t)y * W + x) * 3], want);
				ok = 0;
				break;
			}
		}
}

static void test_channel_order_is_rgb(void)
{
	enum { W = 8, H = 8 };
	static uint8_t frame[W * H * 3];
	static uint8_t dst[W * H * 3];
	struct nn_preproc_geom g;

	fill_frame(frame, W, H, chan_id);
	(void)nn_preproc_geom(W, H, W, H, &g);
	(void)nn_preproc_fill(frame, W, H, &g, dst);

	/* Planar B,G,R in; interleaved R,G,B out.  Swapping these is the bug
	 * that makes a blue object come out yellow -- see the board README. */
	expect("the first output byte is R",
	       dst[0] == (uint8_t)(30u - 128u), "got %d", dst[0]);
	expect("the second output byte is G",
	       dst[1] == (uint8_t)(20u - 128u), "got %d", dst[1]);
	expect("the third output byte is B",
	       dst[2] == (uint8_t)(10u - 128u), "got %d", dst[2]);
}

static void test_halfpixel_downscale_by_two(void)
{
	enum { W = 8, H = 8 };
	static uint8_t frame[W * H * 3];
	static uint8_t dst[4 * 4 * 3];
	struct nn_preproc_geom g;

	fill_frame(frame, W, H, ramp_x);
	expect("2:1 geometry", nn_preproc_geom(W, H, 4, 4, &g) == 0, "refused");
	expect("2:1 fill", nn_preproc_fill(frame, W, H, &g, dst) == 0, "refused");

	/*
	 * Hand-computed, and this is the case that pins the convention.
	 *
	 * With half-pixel centres, destination 0 maps to source
	 * (0 + 0.5) * 2 - 0.5 = 0.5 -- exactly between samples 0 and 1, so the
	 * value on an x-ramp is 0.5.  Truncated to Q8 the two weights are 128
	 * each, and round-half-up gives 1 rather than 0.
	 *
	 * An align-corners implementation would map destination 0 to source 0
	 * and produce 0 here; a naive nearest/floor one would also produce 0.
	 * So this single value distinguishes the intended convention from both
	 * of the usual wrong ones.
	 */
	expect("dst 0 of a 2:1 ramp is the midpoint of samples 0 and 1",
	       dst[0] == (uint8_t)(1u - 128u), "got %d want %d",
	       (int8_t)dst[0], (int8_t)(uint8_t)(1u - 128u));
	/* destination 1 -> source 2.5 -> 3 after rounding up from 2.5 */
	expect("dst 1 of a 2:1 ramp is the midpoint of samples 2 and 3",
	       dst[3] == (uint8_t)(3u - 128u), "got %d want %d",
	       (int8_t)dst[3], (int8_t)(uint8_t)(3u - 128u));
}

static void test_upscale_clamps_the_first_sample(void)
{
	enum { W = 4, H = 4 };
	static uint8_t frame[W * H * 3];
	static uint8_t dst[8 * 8 * 3];
	struct nn_preproc_geom g;

	fill_frame(frame, W, H, ramp_x);
	expect("upscale geometry", nn_preproc_geom(W, H, 8, 8, &g) == 0, "refused");
	expect("upscale fill", nn_preproc_fill(frame, W, H, &g, dst) == 0, "refused");

	/*
	 * [!] THE NEGATIVE-COORDINATE CASE.
	 *
	 * Upscaling by two maps destination 0 to source (0.5)*0.5 - 0.5 =
	 * -0.25: negative.  A C cast truncating toward zero would give index 0
	 * and a fraction of -0.25 reinterpreted as a large positive weight --
	 * i.e. garbage that still looks like a picture.  With a mathematical
	 * floor the index is -1, both neighbours clamp to sample 0, and the
	 * output is exactly sample 0.
	 */
	expect("the first upscaled pixel clamps to the first source sample",
	       dst[0] == (uint8_t)(0u - 128u), "got %d", (int8_t)dst[0]);
	/* And the last clamps to the last source sample, by symmetry. */
	expect("the last upscaled pixel clamps to the last source sample",
	       dst[(7 * 8 + 7) * 3] == (uint8_t)(3u - 128u),
	       "got %d", (int8_t)dst[(7 * 8 + 7) * 3]);
}

static void test_accumulator_bound_at_saturation(void)
{
	enum { W = 16, H = 16 };
	static uint8_t frame[W * H * 3];
	static uint8_t dst[5 * 5 * 3];
	struct nn_preproc_geom g;
	int ok = 1;

	/*
	 * Every pixel 255 and a scale that puts every weight in play.  The
	 * accumulator's bound (255 * 256 * 256 = 2^24) holds because the
	 * weights sum to 256 per axis; if it did not, this is where the
	 * overflow would appear -- as pixels that are not 255.
	 */
	fill_frame(frame, W, H, saturated);
	(void)nn_preproc_geom(W, H, 5, 5, &g);
	(void)nn_preproc_fill(frame, W, H, &g, dst);

	for (size_t i = 0; i < sizeof dst && ok; i++)
		if (dst[i] != (uint8_t)(255u - 128u)) {
			expect("a saturated frame resamples to saturation", 0,
			       "byte %zu is %d", i, (int8_t)dst[i]);
			ok = 0;
		}
}

static void test_fill_rejects_bad_arguments(void)
{
	struct nn_preproc_geom g;
	static uint8_t frame[8 * 8 * 3];
	static uint8_t dst[8 * 8 * 3];

	(void)nn_preproc_geom(8, 8, 8, 8, &g);
	expect("a NULL source is refused",
	       nn_preproc_fill(NULL, 8, 8, &g, dst) != 0, "accepted");
	expect("a NULL destination is refused",
	       nn_preproc_fill(frame, 8, 8, &g, NULL) != 0, "accepted");

	/* A crop that does not lie inside the frame it is given: the geometry
	 * and the frame disagreeing is a caller bug, and reading outside the
	 * buffer is how it would otherwise show up. */
	g.w = 99u;
	expect("a crop wider than the frame is refused",
	       nn_preproc_fill(frame, 8, 8, &g, dst) != 0, "accepted");

	/*
	 * [!] AND THE DIMENSION BOUND, ASKED OF fill() DIRECTLY (issue #60).
	 *
	 * geom() has always refused these, so reaching fill() with them means a
	 * hand-built struct -- which the signature permits and which nothing
	 * else would catch.  Since #60 the bound is not merely a policy: it is
	 * what keeps nn_walk_init()'s extent * 2^15 inside a uint32.  A test
	 * that only ever went through geom() would pass with the check deleted.
	 */
	(void)nn_preproc_geom(8, 8, 8, 8, &g);
	expect("a frame wider than the arithmetic allows is refused",
	       nn_preproc_fill(frame, 100000u, 8, &g, dst) != 0, "accepted");
	expect("a frame taller than the arithmetic allows is refused",
	       nn_preproc_fill(frame, 8, 100000u, &g, dst) != 0, "accepted");

	(void)nn_preproc_geom(8, 8, 8, 8, &g);
	g.dst_w = 100000u;
	expect("a model input wider than the arithmetic allows is refused",
	       nn_preproc_fill(frame, 8, 8, &g, dst) != 0, "accepted");

	(void)nn_preproc_geom(8, 8, 8, 8, &g);
	g.dst_h = 100000u;
	expect("a model input taller than the arithmetic allows is refused",
	       nn_preproc_fill(frame, 8, 8, &g, dst) != 0, "accepted");
}

/* --- box mapping ---------------------------------------------------------- */

static void test_box_maps_through_the_crop(void)
{
	struct nn_preproc_geom g;
	struct nn_preproc_box b;

	(void)nn_preproc_geom(320, 240, 128, 128, &g);

	/*
	 * [!] EDGES CARRY NO HALF-PIXEL TERM.
	 *
	 * The whole normalised square must map to the whole crop.  If the
	 * sampling convention's -0.5 were applied here, these would come out
	 * half a source pixel low -- which against a face is invisible, and is
	 * exactly why it is asserted on the corners instead.
	 */
	expect("the full square maps to the full crop",
	       nn_preproc_box(&g, 0.0f, 0.0f, 1.0f, 1.0f, &b) == 0, "rejected");
	expect("the box left edge is the crop origin", b.x0 == 40, "got %d", b.x0);
	expect("the box top edge is the crop origin", b.y0 == 0, "got %d", b.y0);
	expect("the box right edge is the crop end", b.x1 == 280, "got %d", b.x1);
	expect("the box bottom edge is the crop end", b.y1 == 240, "got %d", b.y1);

	/* A centred quarter-square lands centred in the crop. */
	expect("a centred box maps centred",
	       nn_preproc_box(&g, 0.25f, 0.25f, 0.5f, 0.5f, &b) == 0, "rejected");
	expect("the centred box left edge", b.x0 == 100, "got %d", b.x0);
	expect("the centred box right edge", b.x1 == 220, "got %d", b.x1);
}

static void test_box_clips(void)
{
	struct nn_preproc_geom g;
	struct nn_preproc_box b;

	(void)nn_preproc_geom(320, 240, 128, 128, &g);

	/* The decoder emits boxes that run past the input; they are clipped to
	 * the crop, never dropped for merely overhanging it. */
	expect("a box overhanging every edge still maps",
	       nn_preproc_box(&g, -0.5f, -0.5f, 2.0f, 2.0f, &b) == 0, "rejected");
	expect("an overhanging box clips to the crop left", b.x0 == 40, "got %d", b.x0);
	expect("an overhanging box clips to the crop right", b.x1 == 280, "got %d", b.x1);
	expect("an overhanging box clips to the crop top", b.y0 == 0, "got %d", b.y0);
	expect("an overhanging box clips to the crop bottom", b.y1 == 240, "got %d", b.y1);

	/* Entirely outside: nothing to draw, and saying so is the contract. */
	expect("a box entirely left of the crop is rejected",
	       nn_preproc_box(&g, -3.0f, 0.2f, 0.5f, 0.5f, &b) != 0, "accepted");
	expect("a box entirely below the crop is rejected",
	       nn_preproc_box(&g, 0.2f, 4.0f, 0.5f, 0.5f, &b) != 0, "accepted");
	/* Zero extent has no pixels either. */
	expect("a zero-width box is rejected",
	       nn_preproc_box(&g, 0.5f, 0.5f, 0.0f, 0.5f, &b) != 0, "accepted");
}

static void test_box_rejects_non_finite(void)
{
	struct nn_preproc_geom g;
	struct nn_preproc_box b;
	const float zero = 0.0f;
	float inf = 1.0f / zero;      /* no <math.h>, and none is wanted here */
	float nan = zero / zero;

	(void)nn_preproc_geom(320, 240, 128, 128, &g);

	/*
	 * [!] THE UNDEFINED-BEHAVIOUR EDGE.  blazeface.c neither clamps nor
	 * bounds its floats, so a degenerate model output reaches this
	 * function.  Converting an out-of-range float to an int is undefined,
	 * and this is the only place that can catch it.
	 */
	expect("an infinite origin is rejected",
	       nn_preproc_box(&g, inf, 0.2f, 0.3f, 0.3f, &b) != 0, "accepted");
	expect("a NaN extent is rejected",
	       nn_preproc_box(&g, 0.2f, 0.2f, nan, 0.3f, &b) != 0, "accepted");
	expect("a negative infinity is rejected",
	       nn_preproc_box(&g, 0.2f, -inf, 0.3f, 0.3f, &b) != 0, "accepted");

	/* An absurd but finite box is clamped rather than rejected outright,
	 * and then clips like any other. */
	expect("an absurd finite box does not crash and clips",
	       nn_preproc_box(&g, -1.0e20f, -1.0e20f, 2.0e20f, 2.0e20f, &b) == 0,
	       "rejected");
	expect("the absurd box clipped to the crop",
	       b.x0 == 40 && b.x1 == 280, "got %d..%d", b.x0, b.x1);

	expect("a NULL geometry is rejected",
	       nn_preproc_box(NULL, 0.2f, 0.2f, 0.3f, 0.3f, &b) != 0, "accepted");
}

int main(void)
{
	printf("test_nn_preproc\n");
	test_geom_square_on_qvga();
	test_geom_aspect_not_scale();
	test_geom_upscale_allowed();
	test_geom_degenerate_refused();
	test_identity_scale_is_a_copy();
	test_walk_matches_the_closed_form();
	test_walk_matches_at_real_shapes();
	test_channel_order_is_rgb();
	test_halfpixel_downscale_by_two();
	test_upscale_clamps_the_first_sample();
	test_accumulator_bound_at_saturation();
	test_fill_rejects_bad_arguments();
	test_box_maps_through_the_crop();
	test_box_clips();
	test_box_rejects_non_finite();

	if (failures) {
		printf("test_nn_preproc: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_preproc: all cases pass\n");
	return 0;
}
