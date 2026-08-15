/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_blazeface.c
 * @brief   Host tests for port/npu/models/blazeface.c (issue #45).
 *
 * The REAL decoder is compiled here -- not a copy of its arithmetic -- which is
 * the only arrangement that keeps this honest: a transcription would drift from
 * the firmware's and nothing would notice.  It is testable at all because
 * blazeface.c takes tensor DESCRIPTORS rather than reaching into the NPU
 * singleton, so the tensors below are plain arrays.
 *
 * WHY THESE CASES.  Every bug this decoder can have arrives on the board as a
 * plausible box in the wrong place, on an image nobody can see, and each
 * hypothesis costs a flash cycle of a NOR with about 100k of them.  The four
 * that matter:
 *
 *   - A SHARED DEQUANTISATION CONSTANT.  The four outputs carry four different
 *     scale/zero-point pairs (a factor of 33 between the two score tensors on
 *     the real model).  Code that dequantised with one of them would be right
 *     on a quarter of the tensors and wrong on the rest, and the wrongness is a
 *     scale factor -- boxes still look like boxes.
 *   - TENSORS FOUND BY THE WRONG RULE.  They are located by shape because the
 *     generated order is not the documented order and changes across Vela.
 *   - THE CANDIDATE CAP BIASING THE RESULT.  The donor implementation stops
 *     decoding when its 64-candidate buffer fills, so a busy frame silently
 *     drops the strongest face if it sits in the 8x8 group -- which is scanned
 *     last.  That case is reproduced exactly.
 *   - NMS not merging, or merging everything.
 *
 * npu_tensor_is_int8() is DEFINED HERE.  On the board it lives in npu_tflm.cc,
 * the one translation unit that can see TfLiteType, and a C++ static_assert
 * there pins the enumerator.  This file therefore never needs to know the
 * numeric value: it hands the decoder tensors tagged with its own constant and
 * the same predicate that reads it, which is exactly the contract blazeface.c
 * relies on.
 */
#include "blazeface.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The tag this file uses for "int8".  Its numeric identity is the firmware's
 * business (see the file comment); nothing here depends on the value. */
#define T_INT8   9
#define T_FLOAT  1

bool npu_tensor_is_int8(int8_t type)
{
	return type == (int8_t)T_INT8;
}

/* --- the real model's quantisation --------------------------------------- */
/*
 * Read out of blazeface_front_128_int8.tflite with the board's own
 * verify_vela_model.  Using the REAL parameters rather than round numbers is
 * the point of the first test: the four are genuinely different, and the two
 * score tensors differ by a factor of 33.
 */
#define S_BOX512   0.306708127f
#define Z_BOX512   (-47)
#define S_SCR512   0.0369369201f
#define Z_SCR512   49
#define S_BOX384   1.2020129f
#define Z_BOX384   (-47)
#define S_SCR384   1.22469842f
#define Z_SCR384   126

#define A512       512
#define A384       384
#define STRIDE     16

static int8_t box512[A512 * STRIDE];
static int8_t scr512[A512];
static int8_t box384[A384 * STRIDE];
static int8_t scr384[A384];

static struct npu_tensor tens[4];

static int failures;

static void fail(const char *what, const char *fmt, ...)
{
	va_list ap;

	printf("  FAIL %s: ", what);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	failures++;
}

static void ok(const char *what)
{
	printf("  ok   %s\n", what);
}

static void expect(const char *what, int cond, const char *fmt, ...)
{
	va_list ap;

	if (cond) {
		ok(what);
		return;
	}
	printf("  FAIL %s: ", what);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	failures++;
}

static int close_to(float a, float b, float tol)
{
	float d = a - b;

	return (d < 0.0f ? -d : d) <= tol;
}

static void set_tensor(struct npu_tensor *t, void *data, size_t bytes,
                       int32_t anchors, int32_t chan, float scale, int32_t zp)
{
	memset(t, 0, sizeof(*t));
	t->data       = data;
	t->bytes      = bytes;
	t->rank       = 3;
	t->dims[0]    = 1;
	t->dims[1]    = anchors;
	t->dims[2]    = chan;
	t->type       = (int8_t)T_INT8;
	t->scale      = scale;
	t->zero_point = zp;
}

/*
 * Every tensor back to "nothing passes, nothing is offset".
 *
 * [!] THE QUIET BYTE IS THE ZERO POINT, NOT ZERO.  Filling the box tensors with
 * 0x00 does not mean "no offset": on this model the box zero point is -47, so a
 * zero byte dequantises to +14.4 pixels and every decoded box comes out shifted
 * by an eighth of the frame.  (Written the wrong way first, which is how the
 * anchor-centre test earned its keep before the decoder ever ran on hardware.)
 */
static void reset_tensors(void)
{
	for (int i = 0; i < A512; i++)
		scr512[i] = (int8_t)Z_SCR512;
	for (int i = 0; i < A384; i++)
		scr384[i] = (int8_t)Z_SCR384;
	memset(box512, (int8_t)Z_BOX512, sizeof box512);
	memset(box384, (int8_t)Z_BOX384, sizeof box384);

	set_tensor(&tens[0], box512, sizeof box512, A512, STRIDE, S_BOX512, Z_BOX512);
	set_tensor(&tens[1], scr512, sizeof scr512, A512, 1,      S_SCR512, Z_SCR512);
	set_tensor(&tens[2], box384, sizeof box384, A384, STRIDE, S_BOX384, Z_BOX384);
	set_tensor(&tens[3], scr384, sizeof scr384, A384, 1,      S_SCR384, Z_SCR384);

	(void)blazeface_set_thresh_milli(BF_DEFAULT_THRESH_MILLI);
}

/* The anchor centre, transcribed from the MediaPipe BlazeFace-front SSD
 * configuration rather than from blazeface.c: 16x16 with 2 anchors per cell,
 * then 8x8 with 6. */
static void ref_centre(int anchor, float *cx, float *cy)
{
	int cell, grid;

	if (anchor < A512) {
		cell = anchor / 2;
		grid = 16;
	} else {
		cell = (anchor - A512) / 6;
		grid = 8;
	}
	*cx = ((float)(cell % grid) + 0.5f) / (float)grid;
	*cy = ((float)(cell / grid) + 0.5f) / (float)grid;
}

/* --- 1. dequantisation, per tensor --------------------------------------- */
/*
 * The same QUANTISED BYTES are written into both groups' box tensors for
 * geometrically identical anchors.  The two groups have different box scales
 * (0.3067 against 1.2020, a factor of 3.9), so a correct decoder returns boxes
 * whose sizes differ by exactly that ratio.  A decoder using one shared
 * constant returns two identical boxes -- which is the failure this catches and
 * which no amount of staring at a single decoded box would reveal.
 */
static void test_dequant_is_per_tensor(void)
{
	struct bf_det det[BF_MAX_DET];
	int n;
	const int8_t qw = 40;                 /* the same byte in both groups */

	/* Anchor 0 of the 16x16 group: cell (0,0). */
	reset_tensors();
	scr512[0] = 127;
	box512[0 * STRIDE + 2] = qw;
	box512[0 * STRIDE + 3] = qw;
	n = blazeface_decode(tens, 4, det, BF_MAX_DET);
	expect("one box from the 16x16 group", n == 1, "got %d", n);

	float w512 = n == 1 ? det[0].w : 0.0f;

	/* Anchor 512 of the 8x8 group: also cell (0,0). */
	reset_tensors();
	scr384[0] = 127;
	box384[0 * STRIDE + 2] = qw;
	box384[0 * STRIDE + 3] = qw;
	n = blazeface_decode(tens, 4, det, BF_MAX_DET);
	expect("one box from the 8x8 group", n == 1, "got %d", n);

	float w384 = n == 1 ? det[0].w : 0.0f;

	/* Transcribed, not borrowed: (q - zp) * scale / 128. */
	float ref512 = ((float)qw - (float)Z_BOX512) * S_BOX512 / 128.0f;
	float ref384 = ((float)qw - (float)Z_BOX384) * S_BOX384 / 128.0f;

	expect("the 16x16 box uses its own scale and zero point",
	       close_to(w512, ref512, 1e-5f), "got %g, want %g", w512, ref512);
	expect("the 8x8 box uses its own scale and zero point",
	       close_to(w384, ref384, 1e-5f), "got %g, want %g", w384, ref384);
	expect("the same bytes decode differently in the two groups",
	       !close_to(w512, w384, 1e-4f),
	       "both came out %g -- one dequantisation constant is being shared",
	       w512);
}

/* --- 2. anchor centres ---------------------------------------------------- */
/*
 * A box with zero offset regressors sits on its anchor's centre.  Checked in
 * both groups and away from the origin, because an anchors-per-cell mistake
 * (2 against 6) leaves cell (0,0) correct and everything else wrong.
 */
static void test_anchor_centres(void)
{
	struct bf_det det[BF_MAX_DET];
	const int probe[] = { 273, 511, 512, 700, 895 };

	for (unsigned k = 0; k < sizeof probe / sizeof probe[0]; k++) {
		int a = probe[k];
		float cx, cy, gcx, gcy;
		int n;

		reset_tensors();
		if (a < A512) {
			scr512[a] = 127;
			/* w = h = one dequantised unit, so the box has a centre. */
			box512[a * STRIDE + 2] = (int8_t)(Z_BOX512 + 10);
			box512[a * STRIDE + 3] = (int8_t)(Z_BOX512 + 10);
		} else {
			scr384[a - A512] = 127;
			box384[(a - A512) * STRIDE + 2] = (int8_t)(Z_BOX384 + 10);
			box384[(a - A512) * STRIDE + 3] = (int8_t)(Z_BOX384 + 10);
		}
		n = blazeface_decode(tens, 4, det, BF_MAX_DET);
		if (n != 1) {
			fail("anchor centre", "anchor %d gave %d detections", a, n);
			continue;
		}
		ref_centre(a, &cx, &cy);
		gcx = det[0].x + det[0].w * 0.5f;
		gcy = det[0].y + det[0].h * 0.5f;
		if (!close_to(gcx, cx, 1e-5f) || !close_to(gcy, cy, 1e-5f)) {
			fail("anchor centre", "anchor %d at (%g,%g), want (%g,%g)",
			     a, gcx, gcy, cx, cy);
			continue;
		}
		printf("  ok   anchor %d sits on its cell centre (%g,%g)\n", a, cx, cy);
	}
}

/* --- 3. shape mismatch ---------------------------------------------------- */
static void test_shape_mismatch(void)
{
	struct bf_det det[BF_MAX_DET];
	struct npu_tensor bad[4];
	float before_max;
	int before_npass;
	int n;

	/* A good decode first, so the diagnostics hold known values. */
	reset_tensors();
	scr512[4] = 127;
	box512[4 * STRIDE + 2] = 40;
	box512[4 * STRIDE + 3] = 40;
	(void)blazeface_decode(tens, 4, det, BF_MAX_DET);
	before_max   = blazeface_last_max_score();
	before_npass = blazeface_last_npass();

	/* One tensor short. */
	memcpy(bad, tens, sizeof bad);
	n = blazeface_decode(bad, 3, det, BF_MAX_DET);
	expect("three tensors is refused", n == -1, "got %d", n);

	/* Right count, wrong channel count on one of them. */
	memcpy(bad, tens, sizeof bad);
	bad[0].dims[2] = 15;
	n = blazeface_decode(bad, 4, det, BF_MAX_DET);
	expect("a wrong channel count is refused", n == -1, "got %d", n);

	/* Right shape, wrong element type. */
	memcpy(bad, tens, sizeof bad);
	bad[1].type = (int8_t)T_FLOAT;
	n = blazeface_decode(bad, 4, det, BF_MAX_DET);
	expect("a float32 tensor is refused", n == -1, "got %d", n);

	/* Right shape, buffer too short for it -- the case that would read out of
	 * bounds rather than merely give a wrong answer. */
	memcpy(bad, tens, sizeof bad);
	bad[2].bytes = (size_t)A384 * STRIDE - 1u;
	n = blazeface_decode(bad, 4, det, BF_MAX_DET);
	expect("a buffer shorter than its shape is refused", n == -1, "got %d", n);

	/* A scale that would dequantise everything to the same number. */
	memcpy(bad, tens, sizeof bad);
	bad[1].scale = 0.0f;
	n = blazeface_decode(bad, 4, det, BF_MAX_DET);
	expect("a zero scale is refused", n == -1, "got %d", n);

	memcpy(bad, tens, sizeof bad);
	bad[3].data = NULL;
	n = blazeface_decode(bad, 4, det, BF_MAX_DET);
	expect("a null buffer is refused", n == -1, "got %d", n);

	/* And a refusal must leave the diagnostics alone: a caller that asked the
	 * wrong model must not be shown a stale peak as if it were this frame's. */
	expect("a refusal does not touch the diagnostics",
	       blazeface_last_max_score() == before_max &&
	       blazeface_last_npass() == before_npass,
	       "peak %g/%g npass %d/%d", blazeface_last_max_score(), before_max,
	       blazeface_last_npass(), before_npass);
}

/* --- 4. NMS --------------------------------------------------------------- */
static void test_nms(void)
{
	struct bf_det det[BF_MAX_DET];
	int n;

	/* Two anchors in the same cell (0 and 1 are cell (0,0)) with the same big
	 * box: they overlap completely, so NMS must return one. */
	reset_tensors();
	for (int a = 0; a < 2; a++) {
		scr512[a] = 127;
		box512[a * STRIDE + 2] = 100;
		box512[a * STRIDE + 3] = 100;
	}
	n = blazeface_decode(tens, 4, det, BF_MAX_DET);
	expect("two identical boxes collapse to one", n == 1, "got %d", n);
	expect("NMS ran on both candidates", blazeface_last_npass() == 2,
	       "npass %d", blazeface_last_npass());

	/* Two anchors at opposite corners with small boxes: no overlap, so both
	 * survive.  Anchor 0 is cell (0,0); anchor 510 is cell (15,15). */
	reset_tensors();
	scr512[0]   = 127;
	scr512[510] = 127;
	box512[0 * STRIDE + 2] = 10;
	box512[0 * STRIDE + 3] = 10;
	box512[510 * STRIDE + 2] = 10;
	box512[510 * STRIDE + 3] = 10;
	n = blazeface_decode(tens, 4, det, BF_MAX_DET);
	expect("two distant boxes both survive", n == 2, "got %d", n);

	/* The strongest comes out first: NMS picks by score, not by index. */
	reset_tensors();
	scr512[0]   = (int8_t)(Z_SCR512 + 20);
	scr512[510] = 127;
	box512[0 * STRIDE + 2] = 10;
	box512[0 * STRIDE + 3] = 10;
	box512[510 * STRIDE + 2] = 10;
	box512[510 * STRIDE + 3] = 10;
	n = blazeface_decode(tens, 4, det, BF_MAX_DET);
	expect("the strongest detection is reported first",
	       n == 2 && det[0].score > det[1].score,
	       "n=%d scores %g, %g", n, n > 0 ? det[0].score : 0.0f,
	       n > 1 ? det[1].score : 0.0f);
}

/* --- 5. the candidate cap must not bias the result ------------------------ */
/*
 * [!] THE REGRESSION THIS FILE EXISTS FOR.
 *
 * More than BF_MAX_CAND anchors pass the threshold, and the SINGLE STRONGEST
 * one is placed near the end of the 8x8 group -- the last thing scanned.  A
 * decoder that stops when its buffer fills never reaches it: it reports a peak
 * score from the prefix it saw, and hands NMS a candidate set that excludes the
 * best face in the frame.  The boxes it returns are all real detections, which
 * is exactly why the bug survives inspection.
 *
 * Every anchor is given a box far from every other, so NMS suppresses nothing
 * and the result is purely about which candidates were kept.
 */
static void test_cap_does_not_truncate(void)
{
	struct bf_det det[BF_MAX_DET];
	const int n_pass_512 = 100;      /* well past BF_MAX_CAND on its own */
	const int strongest  = A512 + A384 - 1;   /* the very last anchor */
	float want_peak;
	int n;

	reset_tensors();

	/* A hundred mid-strength candidates in the 16x16 group, which is scanned
	 * first and would fill any fixed buffer before the 8x8 group is reached. */
	for (int a = 0; a < n_pass_512; a++) {
		scr512[a] = (int8_t)(Z_SCR512 + 20);
		box512[a * STRIDE + 2] = (int8_t)(Z_BOX512 + 4);
		box512[a * STRIDE + 3] = (int8_t)(Z_BOX512 + 4);
	}

	/* One much stronger candidate, last. */
	{
		int j = strongest - A512;

		scr384[j] = 127;
		box384[j * STRIDE + 2] = (int8_t)(Z_BOX384 + 4);
		box384[j * STRIDE + 3] = (int8_t)(Z_BOX384 + 4);
	}

	want_peak = (127.0f - (float)Z_SCR384) * S_SCR384;

	n = blazeface_decode(tens, 4, det, BF_MAX_DET);

	expect("the peak score is the real maximum, not a prefix's",
	       close_to(blazeface_last_max_score(), want_peak, 1e-4f),
	       "peak %g, want %g -- the scan stopped early",
	       blazeface_last_max_score(), want_peak);

	expect("every anchor over the threshold is counted",
	       blazeface_last_npass() == n_pass_512 + 1,
	       "npass %d, want %d", blazeface_last_npass(), n_pass_512 + 1);

	expect("the kept set is capped, and says so",
	       blazeface_last_nkept() == BF_MAX_CAND,
	       "nkept %d, want %d", blazeface_last_nkept(), BF_MAX_CAND);

	expect("the pass count and the kept count are reported separately",
	       blazeface_last_npass() > blazeface_last_nkept(),
	       "npass %d nkept %d", blazeface_last_npass(),
	       blazeface_last_nkept());

	/* And the strongest anchor is the one that comes out first. */
	{
		float cx, cy, gcx, gcy;

		ref_centre(strongest, &cx, &cy);
		gcx = n > 0 ? det[0].x + det[0].w * 0.5f : -1.0f;
		gcy = n > 0 ? det[0].y + det[0].h * 0.5f : -1.0f;
		expect("the strongest candidate survives the cap",
		       n > 0 && close_to(gcx, cx, 1e-5f) && close_to(gcy, cy, 1e-5f),
		       "top box at (%g,%g), want the last anchor's cell (%g,%g)",
		       gcx, gcy, cx, cy);
	}
}

/* --- 6. the threshold ------------------------------------------------------ */
static void test_threshold(void)
{
	unsigned before = blazeface_get_thresh_milli();

	expect("0 is refused", blazeface_set_thresh_milli(0) != 0, "accepted");
	expect("1000 is refused", blazeface_set_thresh_milli(1000) != 0, "accepted");
	expect("a huge value is refused", blazeface_set_thresh_milli(99999u) != 0,
	       "accepted");
	expect("a refused threshold changes nothing",
	       blazeface_get_thresh_milli() == before, "now %u, was %u",
	       blazeface_get_thresh_milli(), before);

	expect("1 is accepted", blazeface_set_thresh_milli(1) == 0, "refused");
	expect("999 is accepted", blazeface_set_thresh_milli(999) == 0, "refused");
	expect("the threshold reads back", blazeface_get_thresh_milli() == 999u,
	       "got %u", blazeface_get_thresh_milli());

	/*
	 * The logit and the milli value are the same point on one scale, and the
	 * default's logit is the donor's tuned 0.405.  The tolerance is the
	 * RESOLUTION OF AN INTEGER MILLI, not slack: near p = 0.644 the inverse
	 * sigmoid moves about 0.004 per milli, so a round trip through 644 lands
	 * within a fraction of one step.  Anything tighter would be testing the
	 * rounding rather than the value.
	 */
	(void)blazeface_set_thresh_milli(BF_DEFAULT_THRESH_MILLI);
	expect("the default threshold is the donor's tuned logit",
	       close_to(blazeface_get_thresh_logit(), 0.405f, 2e-3f),
	       "logit %g", blazeface_get_thresh_logit());

	/* A lower threshold cannot find fewer anchors than a higher one. */
	{
		struct bf_det det[BF_MAX_DET];
		int hi, lo;

		reset_tensors();
		for (int a = 0; a < 20; a++)
			scr512[a] = (int8_t)(Z_SCR512 + 12 + a);
		(void)blazeface_set_thresh_milli(900);
		(void)blazeface_decode(tens, 4, det, BF_MAX_DET);
		hi = blazeface_last_npass();
		(void)blazeface_set_thresh_milli(500);
		(void)blazeface_decode(tens, 4, det, BF_MAX_DET);
		lo = blazeface_last_npass();
		expect("lowering the threshold cannot find fewer anchors", lo >= hi,
		       "900 -> %d, 500 -> %d", hi, lo);
	}
	(void)blazeface_set_thresh_milli(BF_DEFAULT_THRESH_MILLI);
}

/* --- 7. the empty frame ---------------------------------------------------- */
static void test_nothing_passes(void)
{
	struct bf_det det[BF_MAX_DET];
	int n;

	reset_tensors();
	n = blazeface_decode(tens, 4, det, BF_MAX_DET);
	expect("an empty frame gives no detections", n == 0, "got %d", n);
	expect("and no candidates", blazeface_last_npass() == 0 &&
	       blazeface_last_nkept() == 0, "npass %d nkept %d",
	       blazeface_last_npass(), blazeface_last_nkept());
	/* The peak is still reported, and it is the score every anchor holds:
	 * "nothing passed" and "the model saw nothing" are different states and
	 * this is the number that tells them apart. */
	expect("but the peak is still reported",
	       close_to(blazeface_last_max_score(), 0.0f, 1e-5f),
	       "peak %g", blazeface_last_max_score());

	/* max = 0 and a null output must be refused rather than written to. */
	expect("max <= 0 is refused",
	       blazeface_decode(tens, 4, det, 0) == -1, "accepted");
	expect("a null output is refused",
	       blazeface_decode(tens, 4, NULL, BF_MAX_DET) == -1, "accepted");
	expect("null tensors are refused",
	       blazeface_decode(NULL, 4, det, BF_MAX_DET) == -1, "accepted");
}

/* --- 8. the output cap ------------------------------------------------------ */
static void test_output_cap(void)
{
	struct bf_det det[BF_MAX_DET];
	int n;

	/* More non-overlapping faces than BF_MAX_DET: the caller's array bounds
	 * the answer and nothing writes past it. */
	reset_tensors();
	for (int cell = 0; cell < 16; cell++) {
		int a = cell * 2 * 17;         /* spread across the 16x16 grid */

		if (a >= A512)
			break;
		scr512[a] = 127;
		box512[a * STRIDE + 2] = (int8_t)(Z_BOX512 + 2);
		box512[a * STRIDE + 3] = (int8_t)(Z_BOX512 + 2);
	}
	n = blazeface_decode(tens, 4, det, BF_MAX_DET);
	expect("the detection count is capped by the caller's array",
	       n == BF_MAX_DET, "got %d, want %d", n, BF_MAX_DET);
}

int main(void)
{
	printf("test_blazeface\n");
	test_dequant_is_per_tensor();
	test_anchor_centres();
	test_shape_mismatch();
	test_nms();
	test_cap_does_not_truncate();
	test_threshold();
	test_nothing_passes();
	test_output_cap();

	if (failures) {
		printf("test_blazeface: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blazeface: all cases pass\n");
	return 0;
}
