/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * Host unit test for port/nn/models/blazeface.c (issue #47).
 *
 * The REAL decoder is compiled here, not a copy of its arithmetic -- a transcription
 * would drift from the firmware's and nothing would notice.  It is testable at all
 * because blazeface.c reaches into the nn layer through exactly two functions,
 * nn_output_count() and nn_output(), and nn.h is HAL/ThreadX-free; struct nn_model is
 * opaque there (nn.c defines it), which is precisely what lets the stub at the top of
 * this file define its own.
 *
 * [!] WHY THIS BOARD GETS A TEST NOW.  The decoder's failures are silent: a wrong
 * anchor scale or an off-by-512 into the second anchor group draws a plausible
 * rectangle in the wrong place, and on hardware that is indistinguishable from bad
 * exposure or a wrong normalization.  Worse, the decoder does not run at all in the
 * default CONFIG_NN_BACKEND=null build -- it is compiled, but blazeface_decode()
 * returns -1 without touching anything, because the null backend's stub tensors are
 * not BlazeFace-shaped.  Exercising it therefore needs a non-default build AND a
 * generated model on the board.  This file is where the arithmetic is actually
 * pinned; wio-lite-ai has had the equivalent since its own port, and the truncation
 * bug that #47 fixes lived in BOTH copies unnoticed.
 *
 * The threshold here is the compile-time BF_SCORE_LOGIT (0.405), not a runtime knob
 * -- that is the one place this board's decoder differs from wio's, so the scores
 * below are chosen against that fixed value.
 */
#include "blazeface.h"
#include "nn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
			exit(1);                                                             \
		}                                                                        \
	} while (0)

#define CLOSE(a, b) (fabs((double)(a) - (double)(b)) < 1e-4)

/* ------------------------------------------------------------------ stub nn ---- */

#define A512 512
#define A384 384
#define BOXC 16

struct nn_model {
	int dummy;
};

static struct nn_model stub_model;

static float box512[A512 * BOXC];
static float scr512[A512];
static float box384[A384 * BOXC];
static float scr384[A384];

/* NN_MAX_IO is 8; BlazeFace publishes 4 outputs. */
static struct nn_tensor stub_out[4];
static int stub_nout;

int nn_output_count(const struct nn_model *m)
{
	(void)m;
	return stub_nout;
}

struct nn_tensor *nn_output(struct nn_model *m, int idx)
{
	(void)m;
	if (idx < 0 || idx >= stub_nout)
		return NULL;
	return &stub_out[idx];
}

static void set_tensor(int i, void *data, uint32_t bytes, uint16_t a, uint16_t c,
                       uint8_t dtype)
{
	memset(&stub_out[i], 0, sizeof(stub_out[i]));
	stub_out[i].data = data;
	stub_out[i].bytes = bytes;
	stub_out[i].ndim = 3;
	stub_out[i].dims[0] = 1;
	stub_out[i].dims[1] = a;
	stub_out[i].dims[2] = c;
	stub_out[i].dims[3] = 1;
	stub_out[i].dtype = dtype;
}

/*
 * Publish BlazeFace-shaped outputs, deliberately NOT in the order the decoder looks
 * them up: bf_find() is supposed to match on shape, so a build that quietly went
 * back to indexing must fail here.  The quiet score is well below BF_SCORE_LOGIT.
 */
static void publish_blazeface(void)
{
	memset(box512, 0, sizeof(box512));
	memset(box384, 0, sizeof(box384));
	for (int i = 0; i < A512; i++)
		scr512[i] = -10.0f;
	for (int i = 0; i < A384; i++)
		scr384[i] = -10.0f;

	set_tensor(0, scr384, sizeof(scr384), A384, 1, NN_DTYPE_FLOAT32);
	set_tensor(1, box512, sizeof(box512), A512, BOXC, NN_DTYPE_FLOAT32);
	set_tensor(2, scr512, sizeof(scr512), A512, 1, NN_DTYPE_FLOAT32);
	set_tensor(3, box384, sizeof(box384), A384, BOXC, NN_DTYPE_FLOAT32);
	stub_nout = 4;
}

/* Place one detection in the 16x16 layer at grid cell (gx, gy), anchor a (0..1). */
static int put512(int gx, int gy, int a, float dx, float dy, float w, float h,
                  float raw_score)
{
	int idx = ((gy * 16) + gx) * 2 + a;

	box512[idx * BOXC + 0] = dx;
	box512[idx * BOXC + 1] = dy;
	box512[idx * BOXC + 2] = w;
	box512[idx * BOXC + 3] = h;
	scr512[idx] = raw_score;
	return idx;
}

/* Place one detection in the 8x8 layer at grid cell (gx, gy), anchor a (0..5). */
static int put384(int gx, int gy, int a, float dx, float dy, float w, float h,
                  float raw_score)
{
	int idx = ((gy * 8) + gx) * 6 + a;

	box384[idx * BOXC + 0] = dx;
	box384[idx * BOXC + 1] = dy;
	box384[idx * BOXC + 2] = w;
	box384[idx * BOXC + 3] = h;
	scr384[idx] = raw_score;
	return idx;
}

/* ---------------------------------------------------------------- the tests ---- */

/* A model whose outputs are not BlazeFace-shaped must be a no-op, not a crash: the
 * CONFIG_NN_BACKEND=null build publishes exactly this and the decoder is called in
 * anyway (it is linked unconditionally). */
static void test_not_blazeface(void)
{
	struct bf_det out[BF_MAX_DET];

	/* the null backend's shapes: 1x256x16 int8 + 1x256 float32 */
	set_tensor(0, box512, sizeof(box512), 256, BOXC, NN_DTYPE_INT8);
	set_tensor(1, scr512, sizeof(scr512), 256, 1, NN_DTYPE_FLOAT32);
	stub_nout = 2;
	CHECK(blazeface_decode(&stub_model, out, BF_MAX_DET) == -1);

	/* right shapes, wrong dtype (int8 scores) */
	publish_blazeface();
	stub_out[2].dtype = NN_DTYPE_INT8;
	CHECK(blazeface_decode(&stub_model, out, BF_MAX_DET) == -1);

	/* right shape, buffer too small for anchors*chan floats -- the OOB guard */
	publish_blazeface();
	stub_out[1].bytes = 16u;
	CHECK(blazeface_decode(&stub_model, out, BF_MAX_DET) == -1);

	/* NULL buffer */
	publish_blazeface();
	stub_out[3].data = NULL;
	CHECK(blazeface_decode(&stub_model, out, BF_MAX_DET) == -1);

	/* bad arguments */
	publish_blazeface();
	CHECK(blazeface_decode(NULL, out, BF_MAX_DET) == -1);
	CHECK(blazeface_decode(&stub_model, NULL, BF_MAX_DET) == -1);
	CHECK(blazeface_decode(&stub_model, out, 0) == -1);
}

/*
 * The load-bearing arithmetic: a single anchor in the 16x16 layer must decode to the
 * box computed by hand from the MediaPipe convention.  Cell (8,8) has centre
 * (8.5/16, 8.5/16) = (0.53125, 0.53125); the regressors are divided by 128 and the
 * box is centre-to-corner.
 */
static void test_decode_512(void)
{
	struct bf_det out[BF_MAX_DET];
	int n;

	publish_blazeface();
	put512(8, 8, 0, 0.0f, 0.0f, 25.6f, 25.6f, 2.0f);

	n = blazeface_decode(&stub_model, out, BF_MAX_DET);
	CHECK(n == 1);
	CHECK(CLOSE(out[0].w, 0.2f));                 /* 25.6 / 128            */
	CHECK(CLOSE(out[0].h, 0.2f));
	CHECK(CLOSE(out[0].x, 0.53125f - 0.1f));      /* centre - w/2          */
	CHECK(CLOSE(out[0].y, 0.53125f - 0.1f));
	/* algebraic sigmoid: 0.5 + 0.5*2/(1+2) = 0.8333 */
	CHECK(CLOSE(out[0].score, 0.83333f));
	CHECK(CLOSE(blazeface_last_max_score(), 2.0f));
	CHECK(blazeface_last_npass() == 1);
	CHECK(blazeface_last_nkept() == 1);

	/* a non-zero centre offset shifts the box by dx/128, dy/128 */
	publish_blazeface();
	put512(8, 8, 0, 12.8f, -12.8f, 25.6f, 25.6f, 2.0f);
	n = blazeface_decode(&stub_model, out, BF_MAX_DET);
	CHECK(n == 1);
	CHECK(CLOSE(out[0].x, 0.53125f + 0.1f - 0.1f));
	CHECK(CLOSE(out[0].y, 0.53125f - 0.1f - 0.1f));
}

/*
 * The 8x8 layer is indexed with anchor_off = 512.  An off-by-512 here would still
 * produce boxes -- just centred on the wrong cell -- so it is worth its own case.
 * Cell (2,5) of the 8x8 grid has centre (2.5/8, 5.5/8) = (0.3125, 0.6875).
 */
static void test_decode_384(void)
{
	struct bf_det out[BF_MAX_DET];
	int n;

	publish_blazeface();
	put384(2, 5, 3, 0.0f, 0.0f, 12.8f, 12.8f, 3.0f);

	n = blazeface_decode(&stub_model, out, BF_MAX_DET);
	CHECK(n == 1);
	CHECK(CLOSE(out[0].w, 0.1f));
	CHECK(CLOSE(out[0].x, 0.3125f - 0.05f));
	CHECK(CLOSE(out[0].y, 0.6875f - 0.05f));
}

/* NMS: identical overlapping boxes collapse to one, distant boxes both survive, and
 * the survivor is the higher-scoring of an overlapping pair. */
static void test_nms(void)
{
	struct bf_det out[BF_MAX_DET];
	int n;

	/* two anchors of the SAME cell with the same box -> IoU 1.0 -> one output */
	publish_blazeface();
	put512(8, 8, 0, 0.0f, 0.0f, 25.6f, 25.6f, 2.0f);
	put512(8, 8, 1, 0.0f, 0.0f, 25.6f, 25.6f, 3.0f);
	n = blazeface_decode(&stub_model, out, BF_MAX_DET);
	CHECK(blazeface_last_npass() == 2);
	CHECK(n == 1);
	CHECK(CLOSE(out[0].score, 0.5f + 0.5f * 3.0f / 4.0f));   /* the higher one */

	/* opposite corners of the frame -> no overlap -> both survive */
	publish_blazeface();
	put512(1, 1, 0, 0.0f, 0.0f, 12.8f, 12.8f, 2.0f);
	put512(14, 14, 0, 0.0f, 0.0f, 12.8f, 12.8f, 2.5f);
	n = blazeface_decode(&stub_model, out, BF_MAX_DET);
	CHECK(n == 2);
	CHECK(out[0].score > out[1].score);   /* emitted highest-score first */

	/* max caps the output even when more survive */
	publish_blazeface();
	for (int i = 0; i < 6; i++)
		put512(i * 2, 1, 0, 0.0f, 0.0f, 6.4f, 6.4f, 2.0f + (float)i);
	CHECK(blazeface_decode(&stub_model, out, 3) == 3);
}

/*
 * The compile-time threshold has to reach the comparison, and the peak has to be
 * reported even when nothing passes -- that is what separates "the model is dead"
 * from "the threshold is too high", which the detection count cannot.
 */
static void test_threshold(void)
{
	struct bf_det out[BF_MAX_DET];

	/* 0.5 is above BF_SCORE_LOGIT (0.405); 0.3 is below. */
	publish_blazeface();
	put512(8, 8, 0, 0.0f, 0.0f, 25.6f, 25.6f, 0.5f);
	CHECK(blazeface_decode(&stub_model, out, BF_MAX_DET) == 1);

	publish_blazeface();
	put512(8, 8, 0, 0.0f, 0.0f, 25.6f, 25.6f, 0.3f);
	CHECK(blazeface_decode(&stub_model, out, BF_MAX_DET) == 0);
	CHECK(CLOSE(blazeface_last_max_score(), 0.3f));
	CHECK(blazeface_last_npass() == 0);
	CHECK(blazeface_last_nkept() == 0);
}

/*
 * [!] THE REGRESSION THIS FILE EXISTS FOR (issue #47).
 *
 * More than BF_MAX_CAND anchors pass the threshold, and the SINGLE STRONGEST one is
 * placed at the LAST anchor of the 8x8 group -- the last thing scanned.  A decoder
 * that stops when its candidate buffer fills never reaches it: it reports a peak
 * score from the prefix it saw, and hands NMS a candidate set that excludes the best
 * face in the frame.  The boxes it returns are all real detections, which is exactly
 * why the bug survived inspection here for as long as it did.
 *
 * Ported from the Grove decoder's test (#45), which is where the corrected decoder
 * shipped first.
 */
static void test_cap_does_not_truncate(void)
{
	struct bf_det out[BF_MAX_DET];
	const int n_pass_512 = 100;    /* well past BF_MAX_CAND on its own */
	int n;

	publish_blazeface();

	/* A hundred mid-strength candidates in the 16x16 group, which is scanned first
	 * and would fill any fixed buffer before the 8x8 group is reached.  Written
	 * straight into the arrays: what matters is the anchor INDEX, not the cell. */
	for (int i = 0; i < n_pass_512; i++) {
		scr512[i] = 1.0f;
		box512[i * BOXC + 2] = 12.8f;
		box512[i * BOXC + 3] = 12.8f;
	}

	/* One much stronger candidate at anchor 895: cell 63 of the 8x8 grid = (7,7),
	 * anchor 5 of that cell.  Centre (7.5/8, 7.5/8). */
	put384(7, 7, 5, 0.0f, 0.0f, 12.8f, 12.8f, 5.0f);

	n = blazeface_decode(&stub_model, out, BF_MAX_DET);

	/* The peak is the real maximum, not a prefix's.  Before the fix this was 1.0. */
	CHECK(CLOSE(blazeface_last_max_score(), 5.0f));

	/* Every anchor over the threshold is counted, uncapped. */
	CHECK(blazeface_last_npass() == n_pass_512 + 1);

	/* The kept set is capped, and says so separately. */
	CHECK(blazeface_last_nkept() == BF_MAX_CAND);
	CHECK(blazeface_last_npass() > blazeface_last_nkept());

	/* And the strongest anchor survives the cap and comes out first.  Before the
	 * fix the scan never reached it, so the top box was one of the mid ones. */
	CHECK(n > 0);
	CHECK(CLOSE(out[0].score, 0.5f + 0.5f * 5.0f / 6.0f));
	CHECK(CLOSE(out[0].x + out[0].w * 0.5f, 7.5f / 8.0f));
	CHECK(CLOSE(out[0].y + out[0].h * 0.5f, 7.5f / 8.0f));
}

/* NaN/Inf in either the score or the box must be dropped, not propagated. */
static void test_nonfinite(void)
{
	struct bf_det out[BF_MAX_DET];
	float inf = (float)(1.0 / 0.0);
	float nan = (float)(0.0 / 0.0);

	publish_blazeface();
	scr512[0] = nan;
	scr512[2] = inf;
	put512(8, 8, 0, nan, 0.0f, 25.6f, 25.6f, 2.0f);   /* good score, bad box */
	CHECK(blazeface_decode(&stub_model, out, BF_MAX_DET) == 0);

	/* a zero or negative extent is not a box either */
	publish_blazeface();
	put512(8, 8, 0, 0.0f, 0.0f, 0.0f, 25.6f, 2.0f);
	CHECK(blazeface_decode(&stub_model, out, BF_MAX_DET) == 0);
	publish_blazeface();
	put512(8, 8, 0, 0.0f, 0.0f, -25.6f, 25.6f, 2.0f);
	CHECK(blazeface_decode(&stub_model, out, BF_MAX_DET) == 0);
}

int main(void)
{
	test_not_blazeface();
	test_decode_512();
	test_decode_384();
	test_nms();
	test_threshold();
	test_cap_does_not_truncate();
	test_nonfinite();
	printf("test_blazeface: ok\n");
	return 0;
}
