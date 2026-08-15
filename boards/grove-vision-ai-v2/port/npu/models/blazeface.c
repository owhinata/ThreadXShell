/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blazeface.c
 * @brief   BlazeFace-front 128 face-detection decode (issue #45).  See blazeface.h.
 *
 * SSD anchor decode + hard NMS, in the MediaPipe/PINTO convention the ST model
 * zoo model comes from.  No libm: the threshold is compared against a stored
 * pre-sigmoid logit, and the reported confidence uses an algebraic sigmoid, so
 * expf() never appears -- this link has no libm and would not get one for a
 * confidence display.
 *
 * TWO THINGS DIFFER FROM THE DONOR IMPLEMENTATION, both deliberate.
 *
 * 1. THE ANCHOR CENTRES ARE COMPUTED, NOT TABULATED.  The Wio/F746 version
 *    fills two float[896] tables on first use behind a "ready" flag.  That is
 *    7 KB of DTCM and a piece of lazily-initialised static state in a file that
 *    otherwise has none.  The centres are (cell + 0.5) / grid, and the cell is
 *    an integer division of the anchor index, so computing them costs two
 *    divisions per anchor and removes the table and the flag together.
 *
 * 2. THE SCAN IS NEVER CUT SHORT.  The donor stops decoding the moment its
 *    64-candidate buffer fills, which silently makes two things wrong at once:
 *    the reported peak score is the maximum of a PREFIX of the anchors, and the
 *    candidates NMS sees are the FIRST 64 rather than the best 64 -- so a busy
 *    frame can drop the highest-scoring face because it sits in the 8x8 group,
 *    which is scanned last.  Here every one of the 896 anchors is visited and
 *    the candidate list is a bounded top-N: once full, a new candidate replaces
 *    the current minimum, and an exact tie is resolved towards the lower anchor
 *    index so the result does not depend on scan order.  (The same bug exists
 *    on the other two boards; fixing it there is separate work.)
 *
 * INT8 IN, NOT FLOAT32.  The model is flashed with its boundary QUANTIZE and
 * DEQUANTIZEs stripped (scripts/tflite_strip_boundary.cc), so what would have
 * been float32 tensors arrive quantised and each of the four carries its OWN
 * scale and zero point -- on this model they differ by a factor of 33.  Every
 * read therefore goes through the tensor it came from; a single shared
 * dequantisation constant would look right and be wrong on three tensors.
 *
 * [!] This translation unit carries -fno-tree-vectorize from board.cmake.  The
 * existing option is scoped to tflm_obj and does not reach here, and the loops
 * below are exactly the shape -O2 would vectorise; predicated MVE fails
 * check_mve_predication.py because the ThreadX M55 port does not preserve VPR.
 */
#include "blazeface.h"

#include <stddef.h>
#include <stdint.h>

/* Box and score scale: the regressors are in input pixels (x/y/w/h_scale in the
 * MediaPipe reference), and the input is 128x128. */
#define BF_INPUT       128.0f

#define BF_A512        512
#define BF_A384        384
#define BF_NANCHOR     (BF_A512 + BF_A384)   /* 896 */
#define BF_BOX_STRIDE  16    /* 4 bbox + 12 (6 keypoints x 2) per anchor */

#define BF_NMS_IOU     0.5f

/* One pre-NMS candidate.  The anchor index rides along so that evicting the
 * weakest entry from a full list is decidable without consulting scan order. */
struct bf_cand {
	struct bf_det det;
	int           anchor;
};

/* Plain .bss: 64 * 24 B.  Not NOLOAD and not in a carve-out -- it is written
 * before it is read on every decode, and the CPU is its only user. */
static struct bf_cand bf_cand[BF_MAX_CAND];

/* Diagnostics of the last decode.  Between them they separate "the model
 * responded to nothing" from "the threshold is too high" from "NMS merged
 * everything", which one detection count cannot. */
static float bf_last_max = -1e9f;
static int   bf_last_npass;
static int   bf_last_nkept;

float blazeface_last_max_score(void) { return bf_last_max; }
int   blazeface_last_npass(void)     { return bf_last_npass; }
int   blazeface_last_nkept(void)     { return bf_last_nkept; }

static float bf_fabsf(float v) { return v < 0.0f ? -v : v; }

/* Algebraic fast sigmoid (no expf); adequate for a displayed confidence. */
static float bf_sigmoid(float x)
{
	return 0.5f + 0.5f * x / (1.0f + bf_fabsf(x));
}

/*
 * Exact inverse of bf_sigmoid, so the threshold typed in and the score printed
 * out are on one scale.  p = 0.5 + 0.5x/(1+|x|) => u = 2p-1 = x/(1+|x|) =>
 * x = u/(1-|u|), for either sign.  The poles at u = +-1 are why the milli value
 * is restricted to 1..999 before it gets here.
 */
static float bf_logit(float p)
{
	float u = 2.0f * p - 1.0f;

	return u / (1.0f - bf_fabsf(u));
}

static unsigned bf_thresh_milli = BF_DEFAULT_THRESH_MILLI;
static float    bf_thresh_logit = 0.405f;   /* == bf_logit(0.644), see the header */

int blazeface_set_thresh_milli(unsigned milli)
{
	if (milli < 1u || milli > 999u)
		return -1;
	bf_thresh_milli = milli;
	bf_thresh_logit = bf_logit((float)milli / 1000.0f);
	return 0;
}

unsigned blazeface_get_thresh_milli(void) { return bf_thresh_milli; }
float    blazeface_get_thresh_logit(void) { return bf_thresh_logit; }

static int bf_finite(float x)
{
	return x == x && x < 3.0e38f && x > -3.0e38f;
}

/* IoU of two boxes given as top-left origin plus extent. */
static float bf_iou(const struct bf_det *a, const struct bf_det *b)
{
	float ax2 = a->x + a->w, ay2 = a->y + a->h;
	float bx2 = b->x + b->w, by2 = b->y + b->h;
	float ix1 = a->x > b->x ? a->x : b->x;
	float iy1 = a->y > b->y ? a->y : b->y;
	float ix2 = ax2 < bx2 ? ax2 : bx2;
	float iy2 = ay2 < by2 ? ay2 : by2;
	float iw = ix2 - ix1, ih = iy2 - iy1;
	float inter, uni;

	if (iw <= 0.0f || ih <= 0.0f)
		return 0.0f;
	inter = iw * ih;
	uni = a->w * a->h + b->w * b->h - inter;
	return uni > 0.0f ? inter / uni : 0.0f;
}

/*
 * Find an output tensor by (anchors, channels).
 *
 * Validates dtype, exact shape, a non-NULL buffer, a usable per-tensor scale,
 * and that the buffer is long enough for anchors*chan int8 elements -- so a
 * malformed or same-shaped-but-shorter tensor cannot cause an out-of-bounds
 * read.  Returns NULL rather than guessing.
 */
static const struct npu_tensor *bf_find(const struct npu_tensor *outs, unsigned n,
                                        int32_t anchors, int32_t chan)
{
	for (unsigned i = 0; i < n; i++) {
		const struct npu_tensor *t = &outs[i];

		if (!npu_tensor_is_int8(t->type))
			continue;
		if (t->rank != 3u || t->dims[0] != 1 ||
		    t->dims[1] != anchors || t->dims[2] != chan)
			continue;
		if (t->data == NULL)
			continue;
		if (t->bytes < (size_t)anchors * (size_t)chan)
			continue;
		/* A zero or non-finite scale would dequantise every value to the same
		 * number (or to NaN) and the decode would be meaningless rather than
		 * wrong in a visible way. */
		if (!bf_finite(t->scale) || t->scale <= 0.0f)
			continue;
		return t;
	}
	return NULL;
}

static float bf_deq(const struct npu_tensor *t, int8_t q)
{
	return ((float)q - (float)t->zero_point) * t->scale;
}

/*
 * Anchor centre for a global anchor index, normalised to the input.
 *
 * Layout, from the MediaPipe BlazeFace-front SSD configuration:
 *   [0, 512)    16x16 grid, 2 anchors per cell
 *   [512, 896)  8x8   grid, 6 anchors per cell
 * The anchors within a cell differ only in the box regressors the model
 * predicts for them, so the centre is a property of the cell alone.
 */
static void bf_anchor_centre(int idx, float *cx, float *cy)
{
	int cell, gx, gy, grid;

	if (idx < BF_A512) {
		cell = idx / 2;
		grid = 16;
	} else {
		cell = (idx - BF_A512) / 6;
		grid = 8;
	}
	gx = cell % grid;
	gy = cell / grid;
	*cx = ((float)gx + 0.5f) / (float)grid;
	*cy = ((float)gy + 0.5f) / (float)grid;
}

/*
 * Insert into the bounded top-N candidate list.
 *
 * @p nkept is the current occupancy.  Once full the weakest entry is evicted,
 * which is what makes the list "the best 64" rather than "the first 64".  The
 * eviction target is the lowest score, ties broken towards the HIGHER anchor
 * index -- combined with rejecting an incoming exact tie, that makes the kept
 * set a function of the scores alone and not of the order they arrived in.
 */
static int bf_offer(const struct bf_det *d, int anchor, int nkept)
{
	int worst;

	if (nkept < BF_MAX_CAND) {
		bf_cand[nkept].det    = *d;
		bf_cand[nkept].anchor = anchor;
		return nkept + 1;
	}

	worst = 0;
	for (int i = 1; i < BF_MAX_CAND; i++) {
		if (bf_cand[i].det.score < bf_cand[worst].det.score ||
		    (bf_cand[i].det.score == bf_cand[worst].det.score &&
		     bf_cand[i].anchor > bf_cand[worst].anchor))
			worst = i;
	}
	if (d->score > bf_cand[worst].det.score) {
		bf_cand[worst].det    = *d;
		bf_cand[worst].anchor = anchor;
	}
	return nkept;
}

/*
 * Decode one anchor group.  Visits EVERY anchor: the peak-score diagnostic and
 * the pass count are only meaningful over the whole set, and the candidate list
 * is bounded by eviction rather than by stopping.
 */
static int bf_decode_group(const struct npu_tensor *box,
                           const struct npu_tensor *score,
                           int anchors, int anchor_off, int nkept)
{
	const int8_t *bq = (const int8_t *)box->data;
	const int8_t *sq = (const int8_t *)score->data;

	for (int i = 0; i < anchors; i++) {
		const int8_t *r = bq + (size_t)i * BF_BOX_STRIDE;
		struct bf_det d;
		float raw, cx, cy, w, h, acx, acy;

		raw = bf_deq(score, sq[i]);
		if (!bf_finite(raw))
			continue;
		if (raw > bf_last_max)
			bf_last_max = raw;
		if (raw <= bf_thresh_logit)
			continue;

		bf_last_npass++;

		bf_anchor_centre(anchor_off + i, &acx, &acy);
		cx = bf_deq(box, r[0]) / BF_INPUT + acx;
		cy = bf_deq(box, r[1]) / BF_INPUT + acy;
		w  = bf_deq(box, r[2]) / BF_INPUT;
		h  = bf_deq(box, r[3]) / BF_INPUT;
		if (!bf_finite(cx) || !bf_finite(cy) || !bf_finite(w) || !bf_finite(h))
			continue;
		if (w <= 0.0f || h <= 0.0f)
			continue;

		d.x     = cx - w * 0.5f;
		d.y     = cy - h * 0.5f;
		d.w     = w;
		d.h     = h;
		d.score = bf_sigmoid(raw);

		nkept = bf_offer(&d, anchor_off + i, nkept);
	}
	return nkept;
}

int blazeface_decode(const struct npu_tensor *outs, unsigned n,
                     struct bf_det *out, int max)
{
	const struct npu_tensor *box512, *scr512, *box384, *scr384;
	int nkept = 0, nout = 0;
	uint8_t used[BF_MAX_CAND];

	if (outs == NULL || out == NULL || max <= 0)
		return -1;

	box512 = bf_find(outs, n, BF_A512, BF_BOX_STRIDE);
	scr512 = bf_find(outs, n, BF_A512, 1);
	box384 = bf_find(outs, n, BF_A384, BF_BOX_STRIDE);
	scr384 = bf_find(outs, n, BF_A384, 1);
	/* Not BlazeFace-shaped: touch nothing, including the diagnostics, so a
	 * caller that asks the wrong model cannot be told a stale peak score. */
	if (box512 == NULL || scr512 == NULL || box384 == NULL || scr384 == NULL)
		return -1;

	bf_last_max   = -1e9f;
	bf_last_npass = 0;
	bf_last_nkept = 0;

	nkept = bf_decode_group(box512, scr512, BF_A512, 0, nkept);
	nkept = bf_decode_group(box384, scr384, BF_A384, BF_A512, nkept);
	bf_last_nkept = nkept;

	/* Hard NMS: repeatedly take the highest-scoring unused candidate, then
	 * suppress every remaining one that overlaps it beyond BF_NMS_IOU. */
	for (int i = 0; i < nkept; i++)
		used[i] = 0u;

	while (nout < max) {
		int   best = -1;
		float best_s = 0.0f;

		for (int i = 0; i < nkept; i++)
			if (!used[i] && bf_cand[i].det.score > best_s) {
				best_s = bf_cand[i].det.score;
				best = i;
			}
		if (best < 0)
			break;

		out[nout++] = bf_cand[best].det;
		used[best] = 1u;
		for (int i = 0; i < nkept; i++)
			if (!used[i] &&
			    bf_iou(&bf_cand[best].det, &bf_cand[i].det) > BF_NMS_IOU)
				used[i] = 1u;
	}
	return nout;
}
