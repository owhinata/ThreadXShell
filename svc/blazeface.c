/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blazeface.c
 * @brief   BlazeFace-front 128 face-detection decode.  See blazeface.h.
 *
 * SSD anchor decode + hard NMS, in the MediaPipe/PINTO convention the ST model
 * zoo model comes from.  No libm: the threshold is compared against a
 * pre-sigmoid logit derived here, and the reported confidence uses an algebraic
 * sigmoid, so expf() never appears -- two of the three links have no libm and
 * would not get one for a confidence display.
 *
 * TWO THINGS DIFFER FROM THE ORIGINAL f746/wio IMPLEMENTATION, both deliberate
 * and both already true of the Grove copy this was folded from.
 *
 * 1. THE ANCHOR CENTRES ARE COMPUTED, NOT TABULATED.  Those versions filled two
 *    float[896] tables on first use behind a "ready" flag: 7 KB of carve-out and
 *    a piece of lazily-initialised state in a file that must own none at all.
 *    The centres are (cell + 0.5) / grid and the cell is an integer division of
 *    the anchor index, so computing them costs two divisions per anchor and
 *    removes the tables and the flag together.  The two forms agree exactly --
 *    the fill order makes cell == index/2 for the 16x16 group and
 *    (index-512)/6 for the 8x8 group, and both divisors are powers of two, so
 *    every value is representable either way.
 *
 * 2. THE SCAN IS NEVER CUT SHORT.  Those versions stopped decoding the moment
 *    the 64-candidate buffer filled, which made two things wrong at once: the
 *    reported peak score was the maximum of a PREFIX of the anchors, and the
 *    candidates NMS saw were the FIRST 64 rather than the best 64 -- so a busy
 *    frame could drop the highest-scoring face because it sat in the 8x8 group,
 *    which is scanned last (issue #47).  Here every one of the 896 anchors is
 *    visited and the candidate list is a bounded top-N.
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

static int bf_finite(float x)
{
	return x == x && x < 3.0e38f && x > -3.0e38f;
}

/*
 * The threshold word.
 *
 * Relaxed atomics rather than a lock: this is one aligned 32-bit word, the only
 * cross-thread field in the decoder, and the reader takes ONE snapshot per frame
 * (see the header).  There is no ordering to establish -- nothing else becomes
 * visible because of this store -- so relaxed is the whole requirement, and on
 * both Cortex-M7 and Cortex-M55 it lowers to a plain LDR/STR with no library
 * helper.  Using the builtins rather than _Atomic keeps struct blazeface an
 * ordinary type that a board can declare and a host test can inspect.
 */
static unsigned bf_thresh_load(const struct blazeface *bf)
{
	return __atomic_load_n(&bf->thresh_milli, __ATOMIC_RELAXED);
}

static void bf_thresh_store(struct blazeface *bf, unsigned milli)
{
	__atomic_store_n(&bf->thresh_milli, milli, __ATOMIC_RELAXED);
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

/* Bytes per element of the two types a BlazeFace output may arrive as, or 0 for
 * a type this decoder will not read. */
static size_t bf_elem_size(uint8_t dtype)
{
	if (dtype == TENSOR_DTYPE_INT8)
		return 1u;
	if (dtype == TENSOR_DTYPE_FLOAT32)
		return sizeof(float);
	return 0u;
}

/*
 * Read element @p i as a float.
 *
 * float32 is taken straight -- scale and zero_point are meaningless for it, and
 * f746/wio publish scale 0 for an unquantised tensor, so running it through the
 * affine form would zero every value.  int8 uses THIS tensor's parameters.
 */
static float bf_elem(const struct tensor_desc *t, size_t i)
{
	if (t->dtype == TENSOR_DTYPE_FLOAT32)
		return ((const float *)t->data)[i];
	return ((float)((const int8_t *)t->data)[i] - (float)t->zero_point) *
	       t->scale;
}

/*
 * Find an output tensor by (anchors, channels).
 *
 * Validates the element type, the exact shape, a non-NULL buffer, a usable
 * per-tensor scale WHERE ONE IS USED, and that the buffer is long enough for
 * anchors*chan elements -- so a malformed or same-shaped-but-shorter tensor
 * cannot cause an out-of-bounds read.  Returns NULL rather than guessing.
 *
 * The dims are compared for equality against the caller's positive constants, so
 * a negative or absurd dimension is rejected before it reaches the arithmetic;
 * the byte count is still computed with an explicit overflow guard because the
 * element size is a multiplier and the check must not be the thing that wraps.
 */
static const struct tensor_desc *bf_find(const struct tensor_desc *outs,
                                         unsigned n, int32_t anchors,
                                         int32_t chan)
{
	for (unsigned i = 0; i < n; i++) {
		const struct tensor_desc *t = &outs[i];
		size_t esz = bf_elem_size(t->dtype);
		size_t need;

		if (esz == 0u)
			continue;
		if (t->rank != 3u || t->dims[0] != 1 ||
		    t->dims[1] != anchors || t->dims[2] != chan)
			continue;
		if (t->data == NULL)
			continue;
		need = (size_t)anchors * (size_t)chan;
		if (need > SIZE_MAX / esz)
			continue;
		if (t->bytes < need * esz)
			continue;
		/* A zero or non-finite scale would dequantise every value to the
		 * same number (or to NaN) and the decode would be meaningless
		 * rather than wrong in a visible way.  Only integer tensors are
		 * dequantised, so only they are held to this. */
		if (t->dtype != TENSOR_DTYPE_FLOAT32 &&
		    (!bf_finite(t->scale) || t->scale <= 0.0f))
			continue;
		return t;
	}
	return NULL;
}

/*
 * Anchor centre for a global anchor index, normalised to the input.
 *
 * Layout, from the MediaPipe BlazeFace-front SSD configuration:
 *   [0, 512)    16x16 grid, 2 anchors per cell
 *   [512, 896)  8x8   grid, 6 anchors per cell
 * The anchors within a cell differ only in the box regressors the model predicts
 * for them, so the centre is a property of the cell alone.
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
 * which is what makes the list "the best N" rather than "the first N".  The
 * eviction target is the lowest score, ties broken towards the HIGHER anchor
 * index -- combined with rejecting an incoming exact tie, that makes the kept
 * set a function of the scores alone and not of the order they arrived in.
 */
static int bf_offer(struct blazeface *bf, const struct bf_det *d, int anchor,
                    int nkept)
{
	struct bf_cand *cand = bf->cand;
	int cap = (int)bf->ncand;
	int worst;

	if (nkept < cap) {
		cand[nkept].det    = *d;
		cand[nkept].anchor = anchor;
		return nkept + 1;
	}

	worst = 0;
	for (int i = 1; i < cap; i++) {
		if (cand[i].det.score < cand[worst].det.score ||
		    (cand[i].det.score == cand[worst].det.score &&
		     cand[i].anchor > cand[worst].anchor))
			worst = i;
	}
	if (d->score > cand[worst].det.score) {
		cand[worst].det    = *d;
		cand[worst].anchor = anchor;
	}
	return nkept;
}

/*
 * Decode one anchor group.  Visits EVERY anchor: the peak-score diagnostic and
 * the pass count are only meaningful over the whole set, and the candidate list
 * is bounded by eviction rather than by stopping.
 */
static int bf_decode_group(struct blazeface *bf, const struct tensor_desc *box,
                           const struct tensor_desc *score, int anchors,
                           int anchor_off, float thresh_logit,
                           struct bf_result *res, int nkept)
{
	for (int i = 0; i < anchors; i++) {
		size_t r = (size_t)i * BF_BOX_STRIDE;
		struct bf_det d;
		float raw, cx, cy, w, h, acx, acy;

		raw = bf_elem(score, (size_t)i);
		if (!bf_finite(raw))
			continue;
		if (raw > res->max_score)
			res->max_score = raw;
		if (raw <= thresh_logit)
			continue;

		res->npass++;

		bf_anchor_centre(anchor_off + i, &acx, &acy);
		cx = bf_elem(box, r + 0u) / BF_INPUT + acx;
		cy = bf_elem(box, r + 1u) / BF_INPUT + acy;
		w  = bf_elem(box, r + 2u) / BF_INPUT;
		h  = bf_elem(box, r + 3u) / BF_INPUT;
		if (!bf_finite(cx) || !bf_finite(cy) || !bf_finite(w) || !bf_finite(h))
			continue;
		if (w <= 0.0f || h <= 0.0f)
			continue;

		d.x     = cx - w * 0.5f;
		d.y     = cy - h * 0.5f;
		d.w     = w;
		d.h     = h;
		d.score = bf_sigmoid(raw);

		nkept = bf_offer(bf, &d, anchor_off + i, nkept);
	}
	return nkept;
}

/* Fill a caller's result block on a path that never ran a decode. */
static int bf_fail(struct bf_result *res, int status, unsigned thresh_milli)
{
	if (res != NULL) {
		res->status       = status;
		res->max_score    = -1e9f;
		res->npass        = 0;
		res->nkept        = 0;
		res->thresh_milli = thresh_milli;
	}
	return status;
}

int blazeface_init(struct blazeface *bf, struct bf_cand *scratch, size_t bytes)
{
	if (bf == NULL)
		return BF_ERR_ARG;

	/* Cleared FIRST so that a failure below leaves an already-initialised
	 * decoder unusable rather than still bound to its previous buffer. */
	bf->ready = 0u;
	bf->cand  = NULL;
	bf->ncand = 0u;

	if (scratch == NULL)
		return BF_ERR_ARG;
	if (((uintptr_t)scratch % _Alignof(struct bf_cand)) != 0u)
		return BF_ERR_ARG;
	if (bytes < sizeof(struct bf_cand))
		return BF_ERR_ARG;

	bf->cand  = scratch;
	bf->ncand = (unsigned)(bytes / sizeof(struct bf_cand));
	if (bf->ncand > (unsigned)BF_MAX_CAND)
		bf->ncand = (unsigned)BF_MAX_CAND;
	bf_thresh_store(bf, BF_DEFAULT_THRESH_MILLI);
	bf->ready = 1u;
	return BF_OK;
}

int blazeface_set_thresh_milli(struct blazeface *bf, unsigned milli)
{
	if (bf == NULL)
		return BF_ERR_ARG;
	if (!bf->ready)
		return BF_ERR_UNINIT;
	if (milli < 1u || milli > 999u)
		return BF_ERR_ARG;
	bf_thresh_store(bf, milli);
	return BF_OK;
}

unsigned blazeface_get_thresh_milli(const struct blazeface *bf)
{
	if (bf == NULL || !bf->ready)
		return 0u;
	return bf_thresh_load(bf);
}

int blazeface_shapes_ok(const struct tensor_desc *outs, unsigned n)
{
	if (outs == NULL)
		return 0;
	/* The same four lookups blazeface_decode() does, and deliberately the
	 * same code path rather than a second list of shapes to keep in step: a
	 * predicate that could disagree with the decoder would be worse than no
	 * predicate at all. */
	return bf_find(outs, n, BF_A512, BF_BOX_STRIDE) != NULL &&
	       bf_find(outs, n, BF_A512, 1) != NULL &&
	       bf_find(outs, n, BF_A384, BF_BOX_STRIDE) != NULL &&
	       bf_find(outs, n, BF_A384, 1) != NULL;
}

int blazeface_decode(struct blazeface *bf, const struct tensor_desc *outs,
                     unsigned n, struct bf_det *out, int max,
                     struct bf_result *res)
{
	const struct tensor_desc *box512, *scr512, *box384, *scr384;
	unsigned thresh_milli;
	float thresh_logit;
	int nkept = 0, nout = 0;
	uint8_t used[BF_MAX_CAND];

	if (bf == NULL || outs == NULL || out == NULL || max <= 0)
		return bf_fail(res, BF_ERR_ARG, 0u);
	if (!bf->ready)
		return bf_fail(res, BF_ERR_UNINIT, 0u);

	/* ONE snapshot for the whole frame, so a concurrent set lands either
	 * wholly before or wholly after this decode -- never inside it -- and the
	 * value reported below is the one actually applied. */
	thresh_milli = bf_thresh_load(bf);
	thresh_logit = bf_logit((float)thresh_milli / 1000.0f);

	box512 = bf_find(outs, n, BF_A512, BF_BOX_STRIDE);
	scr512 = bf_find(outs, n, BF_A512, 1);
	box384 = bf_find(outs, n, BF_A384, BF_BOX_STRIDE);
	scr384 = bf_find(outs, n, BF_A384, 1);
	/* Not BlazeFace-shaped: write nothing, and report a peak of "none seen"
	 * rather than a number, so a caller that asks the wrong model cannot be
	 * shown a score that belongs to whatever ran before. */
	if (box512 == NULL || scr512 == NULL || box384 == NULL || scr384 == NULL)
		return bf_fail(res, BF_ERR_MODEL, thresh_milli);

	{
		struct bf_result local;
		struct bf_result *r = res != NULL ? res : &local;

		r->status       = BF_OK;
		r->max_score    = -1e9f;
		r->npass        = 0;
		r->nkept        = 0;
		r->thresh_milli = thresh_milli;

		nkept = bf_decode_group(bf, box512, scr512, BF_A512, 0,
		                        thresh_logit, r, nkept);
		nkept = bf_decode_group(bf, box384, scr384, BF_A384, BF_A512,
		                        thresh_logit, r, nkept);
		r->nkept = nkept;
	}

	/* Hard NMS: repeatedly take the highest-scoring unused candidate, then
	 * suppress every remaining one that overlaps it beyond BF_NMS_IOU. */
	for (int i = 0; i < nkept; i++)
		used[i] = 0u;

	while (nout < max) {
		int   best = -1;
		float best_s = 0.0f;

		for (int i = 0; i < nkept; i++)
			if (!used[i] && bf->cand[i].det.score > best_s) {
				best_s = bf->cand[i].det.score;
				best = i;
			}
		if (best < 0)
			break;

		out[nout++] = bf->cand[best].det;
		used[best] = 1u;
		for (int i = 0; i < nkept; i++)
			if (!used[i] &&
			    bf_iou(&bf->cand[best].det, &bf->cand[i].det) > BF_NMS_IOU)
				used[i] = 1u;
	}
	return nout;
}
