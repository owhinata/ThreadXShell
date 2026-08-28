/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_blazeface.c
 * @brief   Host tests for svc/blazeface.c (issue #97).
 *
 * The REAL decoder is compiled here -- not a copy of its arithmetic -- which is
 * the only arrangement that keeps this honest: a transcription would drift from
 * the firmware's and nothing would notice.  It is testable at all because the
 * decoder takes tensor DESCRIPTORS (svc/tensor.h) rather than reaching into any
 * inference singleton, so the tensors below are plain arrays.
 *
 * THIS FILE ABSORBS THE THREE BOARD TESTS IT REPLACED.  Before issue #97 there
 * were three decoders and three tests, and some properties were pinned by only
 * one of them: the per-tensor dequantisation and the candidate-cap bias came
 * from Grove's, the float32 path from wio's and f746's.  Folding the decoders
 * without folding the assertions would have quietly dropped coverage, so every
 * case any of the three had is here, run against BOTH element types wherever the
 * property is type-independent.
 *
 * WHY THESE CASES.  Every bug this decoder can have arrives on a board as a
 * plausible box in the wrong place, on an image nobody can see, and on Grove
 * each hypothesis costs a flash cycle of a NOR with about 100k of them:
 *
 *   - A SHARED DEQUANTISATION CONSTANT.  The four outputs carry four different
 *     scale/zero-point pairs (a factor of 33 between the two score tensors on
 *     the real model).  Code that dequantised with one of them would be right on
 *     a quarter of the tensors and wrong on the rest, and the wrongness is a
 *     scale factor -- boxes still look like boxes.
 *   - FLOAT32 PUT THROUGH THE AFFINE FORM.  f746/wio publish scale 0 for an
 *     unquantised tensor, so a decoder that dequantised unconditionally would
 *     multiply every value by zero and report a model that responds to nothing.
 *   - TENSORS FOUND BY THE WRONG RULE.  They are located by shape because the
 *     generated order is not the documented order and changes across Vela.
 *   - THE CANDIDATE CAP BIASING THE RESULT.  The pre-#47 implementation stopped
 *     decoding when its 64-candidate buffer filled, so a busy frame silently
 *     dropped the strongest face if it sat in the 8x8 group -- which is scanned
 *     last.  That case is reproduced exactly.
 *   - NMS not merging, or merging everything.
 *   - A FAILURE REPORTED AS A MEASUREMENT.  "Not BlazeFace-shaped", "never
 *     initialised" and "bad argument" are three different things, and none of
 *     them is "zero faces" (issue #57).
 *
 * [!] WHAT THIS FILE CANNOT COVER.  The decoder reads the threshold ONCE per
 * frame so that a concurrent set cannot apply to part of one.  That the value is
 * REPORTED is checked below; that it is read exactly once is not, and cannot be
 * from a single thread -- rewriting the decoder to re-read it per anchor leaves
 * every case here passing (verified).  The property is held by construction: one
 * load into a local before the anchor loops, and the boards' own gates do not
 * see it either.  Anyone changing that line is on their own.
 */
#include "blazeface.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the real model's quantisation --------------------------------------- */
/*
 * Read out of blazeface_front_128_int8.tflite with the board's own
 * verify_vela_model.  Using the REAL parameters rather than round numbers is the
 * point of the first test: the four are genuinely different, and the two score
 * tensors differ by a factor of 33.
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

static float  fbox512[A512 * STRIDE];
static float  fscr512[A512];
static float  fbox384[A384 * STRIDE];
static float  fscr384[A384];

static struct tensor_desc tens[4];

/* The decoder state and the scratch the BOARD owns on a real target.  Declaring
 * them separately here is not incidental -- it is the same split the firmware
 * uses, and it is what lets a board place only the scratch. */
static struct blazeface bf;
static struct bf_cand   scratch[BF_MAX_CAND];

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

/* --- tensor construction -------------------------------------------------- */

static void set_int8(struct tensor_desc *t, void *data, size_t bytes,
                     int32_t anchors, int32_t chan, float scale, int32_t zp)
{
	memset(t, 0, sizeof(*t));
	t->data       = data;
	t->bytes      = bytes;
	t->rank       = 3;
	t->dims[0]    = 1;
	t->dims[1]    = anchors;
	t->dims[2]    = chan;
	t->dtype      = TENSOR_DTYPE_INT8;
	t->scale      = scale;
	t->zero_point = zp;
}

static void set_f32(struct tensor_desc *t, void *data, size_t bytes,
                    int32_t anchors, int32_t chan)
{
	memset(t, 0, sizeof(*t));
	t->data    = data;
	t->bytes   = bytes;
	t->rank    = 3;
	t->dims[0] = 1;
	t->dims[1] = anchors;
	t->dims[2] = chan;
	t->dtype   = TENSOR_DTYPE_FLOAT32;
	/* scale/zero_point deliberately left at 0: that is exactly what f746 and
	 * wio publish for an unquantised tensor, so anything that demands a usable
	 * scale before it knows the type fails here rather than on hardware. */
}

/* Which element type the current scenario is built from. */
enum mode { MODE_INT8, MODE_F32 };

/*
 * Every tensor back to "nothing passes, nothing is offset".
 *
 * [!] IN INT8 THE QUIET BYTE IS THE ZERO POINT, NOT ZERO.  Filling the box
 * tensors with 0x00 does not mean "no offset": on this model the box zero point
 * is -47, so a zero byte dequantises to +14.4 pixels and every decoded box comes
 * out shifted by an eighth of the frame.  (Written the wrong way first, which is
 * how the anchor-centre test earned its keep before the decoder ever ran on
 * hardware.)  In float32 the quiet value really is 0.0.
 */
static void reset_tensors(enum mode m)
{
	if (m == MODE_INT8) {
		for (int i = 0; i < A512; i++)
			scr512[i] = (int8_t)Z_SCR512;
		for (int i = 0; i < A384; i++)
			scr384[i] = (int8_t)Z_SCR384;
		memset(box512, (int8_t)Z_BOX512, sizeof box512);
		memset(box384, (int8_t)Z_BOX384, sizeof box384);

		set_int8(&tens[0], box512, sizeof box512, A512, STRIDE,
		         S_BOX512, Z_BOX512);
		set_int8(&tens[1], scr512, sizeof scr512, A512, 1,
		         S_SCR512, Z_SCR512);
		set_int8(&tens[2], box384, sizeof box384, A384, STRIDE,
		         S_BOX384, Z_BOX384);
		set_int8(&tens[3], scr384, sizeof scr384, A384, 1,
		         S_SCR384, Z_SCR384);
	} else {
		for (int i = 0; i < A512; i++)
			fscr512[i] = 0.0f;
		for (int i = 0; i < A384; i++)
			fscr384[i] = 0.0f;
		for (int i = 0; i < A512 * STRIDE; i++)
			fbox512[i] = 0.0f;
		for (int i = 0; i < A384 * STRIDE; i++)
			fbox384[i] = 0.0f;

		set_f32(&tens[0], fbox512, sizeof fbox512, A512, STRIDE);
		set_f32(&tens[1], fscr512, sizeof fscr512, A512, 1);
		set_f32(&tens[2], fbox384, sizeof fbox384, A384, STRIDE);
		set_f32(&tens[3], fscr384, sizeof fscr384, A384, 1);
	}

	(void)blazeface_init(&bf, scratch, sizeof scratch);
}

static int8_t quantise(float v, float scale, int32_t zp)
{
	float q = v / scale + (float)zp;

	q = q < 0.0f ? q - 0.5f : q + 0.5f;      /* round, do not truncate */
	if (q < -128.0f)
		q = -128.0f;
	if (q > 127.0f)
		q = 127.0f;
	return (int8_t)q;
}

/*
 * Write one raw (pre-sigmoid) score at an anchor, and RETURN THE VALUE THE
 * DECODER WILL ACTUALLY SEE.
 *
 * [!] In int8 that is not the value asked for.  The real model's 8x8 score
 * tensor has zero point 126 and scale 1.2247, so the largest representable raw
 * score in that group is a single step above the zero point -- 1.2247, whatever
 * is requested.  (This is the ceiling that makes a saturated detection print
 * 775/1000 on hardware.)  Tests assert against the returned value so that they
 * pin the decoder's arithmetic and not the model's quantisation range.
 */
static float put_score(enum mode m, int anchor, float raw)
{
	if (anchor < A512) {
		if (m == MODE_INT8) {
			int8_t q = quantise(raw, S_SCR512, Z_SCR512);

			scr512[anchor] = q;
			return ((float)q - (float)Z_SCR512) * S_SCR512;
		}
		fscr512[anchor] = raw;
	} else {
		int i = anchor - A512;

		if (m == MODE_INT8) {
			int8_t q = quantise(raw, S_SCR384, Z_SCR384);

			scr384[i] = q;
			return ((float)q - (float)Z_SCR384) * S_SCR384;
		}
		fscr384[i] = raw;
	}
	return raw;
}

/* The confidence the decoder reports for a raw score: the algebraic sigmoid,
 * transcribed from the header's definition rather than from blazeface.c. */
static float ref_sigmoid(float x)
{
	float a = x < 0.0f ? -x : x;

	return 0.5f + 0.5f * x / (1.0f + a);
}

/* Write a box (centre offset in input pixels, extent in input pixels). */
static void put_box(enum mode m, int anchor, float dx, float dy, float w, float h)
{
	float v[4] = { dx, dy, w, h };

	if (anchor < A512) {
		for (int k = 0; k < 4; k++) {
			if (m == MODE_INT8) {
				box512[anchor * STRIDE + k] =
					quantise(v[k], S_BOX512, Z_BOX512);
			} else {
				fbox512[anchor * STRIDE + k] = v[k];
			}
		}
	} else {
		int i = anchor - A512;

		for (int k = 0; k < 4; k++) {
			if (m == MODE_INT8) {
				box384[i * STRIDE + k] =
					quantise(v[k], S_BOX384, Z_BOX384);
			} else {
				fbox384[i * STRIDE + k] = v[k];
			}
		}
	}
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

static const char *mode_name(enum mode m)
{
	return m == MODE_INT8 ? "int8" : "float32";
}

/* --- 1. dequantisation, per tensor --------------------------------------- */
/*
 * The same QUANTISED BYTES are written into both groups' box tensors for
 * geometrically identical anchors.  The two groups have different box scales
 * (0.3067 against 1.2020, a factor of 3.9), so a correct decoder returns boxes
 * whose sizes differ by exactly that ratio.  A decoder using one shared constant
 * returns two identical boxes -- which is the failure this catches and which no
 * amount of staring at a single decoded box would reveal.
 */
static void test_dequant_is_per_tensor(void)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	int n;
	float w512, w384;

	printf(" case: dequantisation uses each tensor's own scale\n");

	/* Anchor 0 (16x16 cell 0) and anchor 512 (8x8 cell 0) are both the
	 * top-left cell, so the only difference in the answer must come from the
	 * quantisation parameters. */
	reset_tensors(MODE_INT8);
	scr512[0] = 127;
	scr384[0] = 127;
	for (int k = 0; k < 4; k++) {
		box512[0 * STRIDE + k] = 40;
		box384[0 * STRIDE + k] = 40;
	}
	/* Keep only the 512 group so the two are decoded in isolation. */
	n = blazeface_decode(&bf, tens, 2, det, BF_MAX_DET, &res);
	expect("512-only decode is refused (needs all four tensors)",
	       n == BF_ERR_MODEL, "got %d", n);

	reset_tensors(MODE_INT8);
	scr512[0] = 127;
	for (int k = 0; k < 4; k++)
		box512[0 * STRIDE + k] = 40;
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	if (n != 1) {
		fail("512 group yields one box", "got %d", n);
		return;
	}
	w512 = det[0].w;

	reset_tensors(MODE_INT8);
	scr384[0] = 127;
	for (int k = 0; k < 4; k++)
		box384[0 * STRIDE + k] = 40;
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	if (n != 1) {
		fail("384 group yields one box", "got %d", n);
		return;
	}
	w384 = det[0].w;

	expect("the two groups' widths differ by their scale ratio",
	       close_to(w384 / w512, S_BOX384 / S_BOX512, 0.01f),
	       "w512 %.5f w384 %.5f ratio %.4f, expected %.4f",
	       (double)w512, (double)w384, (double)(w384 / w512),
	       (double)(S_BOX384 / S_BOX512));
}

/* --- 2. float32 is not put through the affine form ------------------------ */
/*
 * f746 and wio publish scale 0 for an unquantised tensor.  A decoder that ran
 * float32 through (q - zp) * scale would multiply everything by zero: no anchor
 * would ever pass, and the board would report a model that responds to nothing.
 */
static void test_float32_is_read_straight(void)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	int n;
	float cx, cy;

	printf(" case: float32 tensors are read straight (scale 0 is not applied)\n");

	reset_tensors(MODE_F32);
	put_score(MODE_F32, 0, 2.0f);
	put_box(MODE_F32, 0, 0.0f, 0.0f, 32.0f, 32.0f);

	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a float32 model decodes", n == 1, "got %d", n);
	if (n != 1)
		return;

	ref_centre(0, &cx, &cy);
	expect("the float32 box has the width it was given",
	       close_to(det[0].w, 32.0f / 128.0f, 0.001f),
	       "w %.5f, expected %.5f", (double)det[0].w, (double)(32.0f / 128.0f));
	expect("the float32 box sits on its anchor centre",
	       close_to(det[0].x + det[0].w * 0.5f, cx, 0.001f) &&
	       close_to(det[0].y + det[0].h * 0.5f, cy, 0.001f),
	       "centre (%.4f,%.4f), expected (%.4f,%.4f)",
	       (double)(det[0].x + det[0].w * 0.5f),
	       (double)(det[0].y + det[0].h * 0.5f), (double)cx, (double)cy);
	expect("the reported peak is the raw score, undistorted",
	       close_to(res.max_score, 2.0f, 0.001f),
	       "max_score %.4f", (double)res.max_score);
}

/* --- 3. anchor centres ---------------------------------------------------- */
/*
 * A box with zero offset and a known extent must land exactly on its anchor's
 * cell centre.  Checked at both ends of both groups, because an off-by-one in
 * the integer division that produces the cell shows up only away from index 0.
 */
static void test_anchor_centres(enum mode m)
{
	static const int probe[] = { 0, 1, 2, 511, 512, 517, 895 };
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	char what[96];

	printf(" case: anchor centres (%s)\n", mode_name(m));

	for (unsigned p = 0; p < sizeof probe / sizeof probe[0]; p++) {
		int a = probe[p];
		float cx, cy;
		int n;

		reset_tensors(m);
		put_score(m, a, 2.0f);
		put_box(m, a, 0.0f, 0.0f, 16.0f, 16.0f);

		n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
		if (n != 1) {
			snprintf(what, sizeof what, "anchor %d decodes", a);
			fail(what, "got %d", n);
			continue;
		}
		ref_centre(a, &cx, &cy);
		snprintf(what, sizeof what, "anchor %d lands on its cell centre", a);
		expect(what,
		       close_to(det[0].x + det[0].w * 0.5f, cx, 0.02f) &&
		       close_to(det[0].y + det[0].h * 0.5f, cy, 0.02f),
		       "centre (%.4f,%.4f), expected (%.4f,%.4f)",
		       (double)(det[0].x + det[0].w * 0.5f),
		       (double)(det[0].y + det[0].h * 0.5f),
		       (double)cx, (double)cy);
	}
}

/* --- 4. tensors are found by shape, and a bad one is refused -------------- */
static void test_shape_mismatch(void)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	struct tensor_desc save;
	int n;

	printf(" case: tensor lookup is by shape and rejects malformed input\n");

	/* Order must not matter: the generated order is not the documented one. */
	reset_tensors(MODE_INT8);
	put_score(MODE_INT8, 0, 2.0f);
	put_box(MODE_INT8, 0, 0.0f, 0.0f, 16.0f, 16.0f);
	save = tens[0];
	tens[0] = tens[3];
	tens[3] = save;
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a permuted tensor order still decodes", n == 1, "got %d", n);

	/* A short buffer of the right shape must be refused, not read past. */
	reset_tensors(MODE_INT8);
	tens[1].bytes = A512 - 1;
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a too-short buffer is refused", n == BF_ERR_MODEL, "got %d", n);

	/* Wrong rank. */
	reset_tensors(MODE_INT8);
	tens[1].rank = 2;
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a wrong-rank tensor is refused", n == BF_ERR_MODEL, "got %d", n);

	/* A negative dimension must not reach the arithmetic. */
	reset_tensors(MODE_INT8);
	tens[1].dims[1] = -512;
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a negative dimension is refused", n == BF_ERR_MODEL, "got %d", n);

	/* An element type this decoder does not read. */
	reset_tensors(MODE_INT8);
	tens[1].dtype = TENSOR_DTYPE_INT16;
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("an unsupported element type is refused",
	       n == BF_ERR_MODEL, "got %d", n);

	/* A zeroed descriptor tags as UNSUPPORTED, not int8. */
	reset_tensors(MODE_INT8);
	memset(&tens[1], 0, sizeof tens[1]);
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a zeroed descriptor is refused", n == BF_ERR_MODEL, "got %d", n);

	/* An int8 tensor with an unusable scale would dequantise everything to
	 * one number; refusing beats decoding something meaningless. */
	reset_tensors(MODE_INT8);
	tens[1].scale = 0.0f;
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("an int8 tensor with scale 0 is refused",
	       n == BF_ERR_MODEL, "got %d", n);

	reset_tensors(MODE_INT8);
	tens[1].data = NULL;
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a null data pointer is refused", n == BF_ERR_MODEL, "got %d", n);
}

/* --- 5. blazeface_shapes_ok agrees with the decoder ----------------------- */
static void test_shapes_ok(void)
{
	printf(" case: shapes_ok agrees with what a decode would find\n");

	reset_tensors(MODE_INT8);
	expect("four good int8 tensors are accepted",
	       blazeface_shapes_ok(tens, 4) != 0, "rejected");

	reset_tensors(MODE_F32);
	expect("four good float32 tensors are accepted",
	       blazeface_shapes_ok(tens, 4) != 0, "rejected");

	reset_tensors(MODE_INT8);
	expect("three tensors are not enough",
	       blazeface_shapes_ok(tens, 3) == 0, "accepted");

	expect("a null array is refused", blazeface_shapes_ok(NULL, 4) == 0,
	       "accepted");

	reset_tensors(MODE_INT8);
	tens[2].dims[1] = 385;
	expect("a wrong anchor count is refused",
	       blazeface_shapes_ok(tens, 4) == 0, "accepted");
}

/* --- 6. NMS --------------------------------------------------------------- */
static void test_nms(enum mode m)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	float strong, weak;
	int n;

	printf(" case: NMS merges overlaps and keeps separate faces (%s)\n",
	       mode_name(m));

	/* Two anchors in the SAME cell (0 and 1 share cell 0), each with a big
	 * box, so they overlap almost completely -- one survivor. */
	reset_tensors(m);
	strong = put_score(m, 0, 3.0f);
	put_box(m, 0, 0.0f, 0.0f, 48.0f, 48.0f);
	weak = put_score(m, 1, 2.0f);
	put_box(m, 1, 0.0f, 0.0f, 48.0f, 48.0f);
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("two coincident boxes merge to one", n == 1, "got %d", n);
	if (n >= 1)
		expect("the survivor is the higher-scoring one",
		       close_to(det[0].score, ref_sigmoid(strong), 0.002f) &&
		       ref_sigmoid(strong) > ref_sigmoid(weak),
		       "score %.4f, expected %.4f (the weaker one is %.4f)",
		       (double)det[0].score, (double)ref_sigmoid(strong),
		       (double)ref_sigmoid(weak));
	expect("both anchors are reported as having passed", res.npass == 2,
	       "npass %d", res.npass);

	/* Opposite corners of the 16x16 grid: no overlap, two survivors. */
	reset_tensors(m);
	put_score(m, 0, 3.0f);
	put_box(m, 0, 0.0f, 0.0f, 8.0f, 8.0f);
	put_score(m, 510, 3.0f);
	put_box(m, 510, 0.0f, 0.0f, 8.0f, 8.0f);
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("two distant boxes both survive", n == 2, "got %d", n);
}

/* --- 7. the candidate cap must not truncate the scan ---------------------- */
/*
 * The exact shape of issue #47.  Fill the 16x16 group with more passing anchors
 * than the candidate list holds, then put the STRONGEST face in the 8x8 group,
 * which is scanned last.  An implementation that stops decoding when the buffer
 * fills never sees it: it reports a peak score from the prefix and NMS chooses
 * among the first 64.  A bounded top-N sees all 896.
 */
static void test_cap_does_not_truncate(enum mode m)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	float mediocre = 0.0f, strongest, cx, cy;
	int n;

	printf(" case: a full candidate list does not cut the scan short (%s)\n",
	       mode_name(m));

	reset_tensors(m);
	/* 100 mediocre faces in the first group, spread over distinct cells so
	 * NMS does not merge them into nothing. */
	for (int i = 0; i < 100; i++) {
		int a = i * 2;   /* one anchor per cell */

		mediocre = put_score(m, a, 1.0f);
		put_box(m, a, 0.0f, 0.0f, 4.0f, 4.0f);
	}
	/* The strongest face of all, in the group scanned LAST.  Asking for 6.0
	 * gets 1.2247 back in int8 -- the 8x8 tensor's ceiling -- which is still
	 * comfortably above the mediocre 1.0, and that is all the case needs. */
	strongest = put_score(m, 890, 6.0f);
	put_box(m, 890, 0.0f, 0.0f, 8.0f, 8.0f);

	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("every passing anchor is counted, not just the first 64",
	       res.npass == 101, "npass %d, expected 101", res.npass);
	expect("the candidate list is capped", res.nkept == BF_MAX_CAND,
	       "nkept %d, expected %d", res.nkept, BF_MAX_CAND);
	expect("the last-scanned face really is the strongest here",
	       strongest > mediocre, "strongest %.4f vs mediocre %.4f",
	       (double)strongest, (double)mediocre);
	expect("the peak score is the global maximum, not the prefix's",
	       close_to(res.max_score, strongest, 0.002f),
	       "max_score %.4f, expected %.4f",
	       (double)res.max_score, (double)strongest);
	if (n < 1) {
		fail("the strongest face survives", "got %d detections", n);
		return;
	}
	/* [!] The assertion that matters is WHERE it is, not how big its score
	 * is: anchor 890 lives in the 8x8 group, which is scanned last, so a
	 * decoder that stopped at a full candidate list would never have seen it
	 * and would return one of the 100 from the 16x16 group instead. */
	ref_centre(890, &cx, &cy);
	expect("the strongest face is returned first, from the LAST-scanned group",
	       close_to(det[0].x + det[0].w * 0.5f, cx, 0.02f) &&
	       close_to(det[0].y + det[0].h * 0.5f, cy, 0.02f),
	       "top box centre (%.4f,%.4f), expected anchor 890 at (%.4f,%.4f)",
	       (double)(det[0].x + det[0].w * 0.5f),
	       (double)(det[0].y + det[0].h * 0.5f), (double)cx, (double)cy);
}

/* --- 8. threshold --------------------------------------------------------- */
static void test_threshold(enum mode m)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	int n;

	printf(" case: the threshold selects, and the result reports the one used"
	       " (%s)\n", mode_name(m));

	reset_tensors(m);
	put_score(m, 0, 0.5f);    /* sigmoid ~0.667: over the 644 default */
	put_box(m, 0, 0.0f, 0.0f, 16.0f, 16.0f);
	put_score(m, 100, 0.2f);  /* sigmoid ~0.583: under it */
	put_box(m, 100, 0.0f, 0.0f, 16.0f, 16.0f);

	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("only the anchor above the default threshold passes",
	       n == 1 && res.npass == 1, "n %d npass %d", n, res.npass);
	expect("the result carries the threshold it applied",
	       res.thresh_milli == BF_DEFAULT_THRESH_MILLI,
	       "thresh_milli %u", res.thresh_milli);

	/* Lower it and the second anchor joins. */
	expect("the threshold can be lowered",
	       blazeface_set_thresh_milli(&bf, 550u) == BF_OK, "rejected");
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a lower threshold admits the weaker anchor", res.npass == 2,
	       "npass %d", res.npass);
	expect("the result reports the NEW threshold", res.thresh_milli == 550u,
	       "thresh_milli %u", res.thresh_milli);

	/* Raise it above both and nothing passes. */
	(void)blazeface_set_thresh_milli(&bf, 900u);
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a high threshold admits nothing", n == 0 && res.npass == 0,
	       "n %d npass %d", n, res.npass);
	expect("zero detections is still a successful decode",
	       res.status == BF_OK, "status %d", res.status);
	expect("the peak score is still reported when nothing passes",
	       close_to(res.max_score, 0.5f, 0.05f),
	       "max_score %.4f", (double)res.max_score);

	/* The poles of the inverse sigmoid are refused, not clamped. */
	(void)blazeface_set_thresh_milli(&bf, 644u);
	expect("threshold 0 is refused",
	       blazeface_set_thresh_milli(&bf, 0u) == BF_ERR_ARG, "accepted");
	expect("threshold 1000 is refused",
	       blazeface_set_thresh_milli(&bf, 1000u) == BF_ERR_ARG, "accepted");
	expect("a refused threshold changes nothing",
	       blazeface_get_thresh_milli(&bf) == 644u,
	       "thresh %u", blazeface_get_thresh_milli(&bf));
	expect("1 and 999 are accepted",
	       blazeface_set_thresh_milli(&bf, 1u) == BF_OK &&
	       blazeface_set_thresh_milli(&bf, 999u) == BF_OK, "refused");
}

/* --- 9. the default threshold is the donor's tuned logit ------------------ */
/*
 * f746 carried this as a compile-time logit of 0.405 and the other two as 644
 * milli.  They are the same number in two units, and that equality is the reason
 * folding the three decoders did not change anyone's behaviour -- so it is
 * pinned here rather than left in a comment.
 */
static void test_default_threshold_is_the_donor_logit(void)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	int n;

	printf(" case: the default threshold is the donor's 0.405 logit\n");

	/* Just above and just below the logit the donor firmware used. */
	reset_tensors(MODE_F32);
	put_score(MODE_F32, 0, 0.4100f);
	put_box(MODE_F32, 0, 0.0f, 0.0f, 16.0f, 16.0f);
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a raw score just above 0.405 passes by default",
	       res.npass == 1, "npass %d", res.npass);
	(void)n;

	reset_tensors(MODE_F32);
	put_score(MODE_F32, 0, 0.4000f);
	put_box(MODE_F32, 0, 0.0f, 0.0f, 16.0f, 16.0f);
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a raw score just below 0.405 does not",
	       res.npass == 0, "npass %d", res.npass);
	(void)n;
}

/* --- 10. nothing passes --------------------------------------------------- */
static void test_nothing_passes(enum mode m)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	int n;

	printf(" case: a quiet frame decodes to nothing, cleanly (%s)\n",
	       mode_name(m));

	reset_tensors(m);
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a quiet frame yields no detections", n == 0, "got %d", n);
	expect("and no candidates", res.nkept == 0, "nkept %d", res.nkept);
	expect("and it is not an error", res.status == BF_OK,
	       "status %d", res.status);
}

/* --- 11. output cap ------------------------------------------------------- */
static void test_output_cap(void)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	int n;

	printf(" case: the caller's capacity is respected\n");

	reset_tensors(MODE_INT8);
	/* Twenty well-separated faces; ask for three. */
	for (int i = 0; i < 20; i++) {
		int a = i * 24;

		(void)put_score(MODE_INT8, a, 3.0f);
		put_box(MODE_INT8, a, 0.0f, 0.0f, 4.0f, 4.0f);
	}
	n = blazeface_decode(&bf, tens, 4, det, 3, &res);
	expect("no more than the requested number is written", n == 3,
	       "got %d", n);
	expect("but the pass count is not capped by it", res.npass == 20,
	       "npass %d", res.npass);
}

/* --- 12. failures are distinguishable ------------------------------------ */
/*
 * [!] The whole point of issue #57 and of splitting the codes: a decoder that
 * could not recognise the model, one that was never wired up, and one called
 * wrongly are three different faults, and NONE of them is "zero faces".
 */
static void test_error_codes_are_distinct(void)
{
	struct blazeface fresh;
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	int n;

	printf(" case: the failure codes are distinct and none of them is zero\n");

	reset_tensors(MODE_INT8);

	memset(&fresh, 0, sizeof fresh);
	n = blazeface_decode(&fresh, tens, 4, det, BF_MAX_DET, &res);
	expect("decoding before init reports UNINIT, not a model problem",
	       n == BF_ERR_UNINIT, "got %d", n);
	expect("and the result says so too", res.status == BF_ERR_UNINIT,
	       "status %d", res.status);

	n = blazeface_decode(&bf, tens, 4, NULL, BF_MAX_DET, &res);
	expect("a null output array reports ARG", n == BF_ERR_ARG, "got %d", n);

	n = blazeface_decode(&bf, tens, 4, det, 0, &res);
	expect("a zero capacity reports ARG", n == BF_ERR_ARG, "got %d", n);

	n = blazeface_decode(NULL, tens, 4, det, BF_MAX_DET, &res);
	expect("a null decoder reports ARG", n == BF_ERR_ARG, "got %d", n);

	n = blazeface_decode(&bf, NULL, 4, det, BF_MAX_DET, &res);
	expect("a null tensor array reports ARG", n == BF_ERR_ARG, "got %d", n);

	reset_tensors(MODE_INT8);
	tens[0].dims[1] = 999;
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("a non-BlazeFace model reports MODEL", n == BF_ERR_MODEL,
	       "got %d", n);

	expect("every failure code is negative and distinct",
	       BF_ERR_MODEL < 0 && BF_ERR_UNINIT < 0 && BF_ERR_ARG < 0 &&
	       BF_ERR_MODEL != BF_ERR_UNINIT && BF_ERR_UNINIT != BF_ERR_ARG &&
	       BF_ERR_MODEL != BF_ERR_ARG, "codes overlap");
	expect("and none of them is BF_OK", BF_OK == 0, "BF_OK is %d", BF_OK);
}

/* --- 13. a failed decode does not report stale diagnostics ---------------- */
static void test_failure_does_not_leak_diagnostics(void)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	int n;

	printf(" case: a failed decode reports no peak from the previous one\n");

	reset_tensors(MODE_INT8);
	put_score(MODE_INT8, 0, 5.0f);
	put_box(MODE_INT8, 0, 0.0f, 0.0f, 16.0f, 16.0f);
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("the good decode has a peak", n == 1 && res.max_score > 1.0f,
	       "n %d max %.4f", n, (double)res.max_score);

	tens[0].dims[1] = 999;   /* now not BlazeFace-shaped */
	n = blazeface_decode(&bf, tens, 4, det, BF_MAX_DET, &res);
	expect("the failed decode is a model error", n == BF_ERR_MODEL,
	       "got %d", n);
	expect("and it does NOT carry the previous frame's peak",
	       res.max_score < -1.0e8f, "max_score %.4f", (double)res.max_score);
	expect("nor its pass and kept counts",
	       res.npass == 0 && res.nkept == 0,
	       "npass %d nkept %d", res.npass, res.nkept);
}

/* --- 14. init validates what it is given --------------------------------- */
static void test_init_validation(void)
{
	struct blazeface local;
	struct bf_cand   small[1];
	unsigned char    raw[sizeof(struct bf_cand) * 2 + _Alignof(struct bf_cand)];
	struct bf_det    det[BF_MAX_DET];
	struct bf_result res;
	unsigned char   *misaligned;
	int n;

	printf(" case: init validates the scratch it is handed\n");

	expect("a null decoder is refused",
	       blazeface_init(NULL, scratch, sizeof scratch) == BF_ERR_ARG,
	       "accepted");
	expect("a null scratch is refused",
	       blazeface_init(&local, NULL, sizeof scratch) == BF_ERR_ARG,
	       "accepted");
	expect("a scratch too small for one candidate is refused",
	       blazeface_init(&local, small, sizeof(struct bf_cand) - 1u) ==
	       BF_ERR_ARG, "accepted");
	expect("room for exactly one candidate is enough",
	       blazeface_init(&local, small, sizeof small) == BF_OK, "refused");

	/* _Alignof(struct bf_cand) is 4 here, so offsetting by 1 byte is a real
	 * misalignment rather than a no-op. */
	if (_Alignof(struct bf_cand) > 1u) {
		misaligned = raw + 1;
		while (((uintptr_t)misaligned % _Alignof(struct bf_cand)) == 0u)
			misaligned++;
		expect("a misaligned scratch is refused",
		       blazeface_init(&local, (struct bf_cand *)(void *)misaligned,
		                      sizeof(struct bf_cand)) == BF_ERR_ARG,
		       "accepted");
	}

	/* [!] A FAILED RE-INIT MUST NOT LEAVE THE OLD BINDING STANDING.  This is
	 * the case that makes the "clear the flag first" ordering load-bearing:
	 * without it a decoder whose re-init failed keeps decoding into a buffer
	 * the caller believes it has replaced. */
	expect("a good init succeeds", blazeface_init(&local, scratch,
	       sizeof scratch) == BF_OK, "refused");
	expect("a failed re-init is refused",
	       blazeface_init(&local, NULL, sizeof scratch) == BF_ERR_ARG,
	       "accepted");
	reset_tensors(MODE_INT8);
	n = blazeface_decode(&local, tens, 4, det, BF_MAX_DET, &res);
	expect("and leaves the decoder UNUSABLE rather than still bound",
	       n == BF_ERR_UNINIT, "got %d", n);

	expect("the default threshold is set by init",
	       blazeface_get_thresh_milli(&bf) == BF_DEFAULT_THRESH_MILLI,
	       "thresh %u", blazeface_get_thresh_milli(&bf));
	expect("an uninitialised decoder reports no threshold",
	       blazeface_get_thresh_milli(&local) == 0u,
	       "thresh %u", blazeface_get_thresh_milli(&local));
	expect("and refuses to have one set",
	       blazeface_set_thresh_milli(&local, 500u) == BF_ERR_UNINIT,
	       "accepted");
}

/* --- 15. a smaller scratch is honoured, not overrun ----------------------- */
static void test_small_scratch(void)
{
	struct blazeface local;
	struct bf_cand   few[4];
	struct bf_det    det[BF_MAX_DET];
	struct bf_result res;
	int n;

	printf(" case: a scratch smaller than BF_MAX_CAND bounds the candidates\n");

	expect("init accepts a four-entry scratch",
	       blazeface_init(&local, few, sizeof few) == BF_OK, "refused");

	reset_tensors(MODE_INT8);
	for (int i = 0; i < 20; i++) {
		int a = i * 24;

		(void)put_score(MODE_INT8, a, 3.0f);
		put_box(MODE_INT8, a, 0.0f, 0.0f, 4.0f, 4.0f);
	}
	n = blazeface_decode(&local, tens, 4, det, BF_MAX_DET, &res);
	expect("the kept set is bounded by the scratch, not by BF_MAX_CAND",
	       res.nkept == 4, "nkept %d", res.nkept);
	expect("but every anchor is still scanned", res.npass == 20,
	       "npass %d", res.npass);
	expect("and detections still come out", n > 0 && n <= 4, "got %d", n);
}

int main(void)
{
	printf("test_blazeface\n");

	test_dequant_is_per_tensor();
	test_float32_is_read_straight();
	test_anchor_centres(MODE_INT8);
	test_anchor_centres(MODE_F32);
	test_shape_mismatch();
	test_shapes_ok();
	test_nms(MODE_INT8);
	test_nms(MODE_F32);
	test_cap_does_not_truncate(MODE_INT8);
	test_cap_does_not_truncate(MODE_F32);
	test_threshold(MODE_INT8);
	test_threshold(MODE_F32);
	test_default_threshold_is_the_donor_logit();
	test_nothing_passes(MODE_INT8);
	test_nothing_passes(MODE_F32);
	test_output_cap();
	test_error_codes_are_distinct();
	test_failure_does_not_leak_diagnostics();
	test_init_validation();
	test_small_scratch();

	if (failures) {
		printf("test_blazeface: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blazeface: all cases pass\n");
	return 0;
}
