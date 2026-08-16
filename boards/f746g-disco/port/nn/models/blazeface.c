/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blazeface.c
 * @brief   BlazeFace-front 128 face-detection decode (owhinata/stm32f746g-disco#81
 * owhinata/stm32f746g-disco#8). See blazeface.h.
 *
 * SSD anchor decode + NMS, ported from the MediaPipe/PINTO BlazeFace convention
 * (the ST Model Zoo model's provenance).  No libm dependency: score filtering
 * uses a hardcoded logit threshold (compare the raw pre-sigmoid score), and the
 * reported confidence uses an algebraic fast-sigmoid, so we never call expf().
 *
 * The four output tensors are located by SHAPE, not index (the generated order is
 * not the README order): C==16 => box regressors, C==1 => scores; anchors==512 is
 * the 16x16 layer, anchors==384 is the 8x8 layer.
 *
 * [!] THE SCAN IS NEVER CUT SHORT (issue #47).  This decoder used to stop the
 * moment its 64-candidate buffer filled, which made two things wrong at once and
 * neither of them visibly: the reported peak score became the maximum of a
 * PREFIX of the anchors, and NMS was handed the FIRST 64 candidates rather than
 * the best 64.  The 16x16 group is scanned first, so a frame that filled the
 * buffer there meant the 8x8 group was never visited at all -- and the strongest
 * face in the frame was dropped if it sat in it.  Everything that came out was
 * still a real detection, which is exactly why it survived inspection.
 *
 * So every one of the 896 anchors is visited, always, and the candidate list is
 * bounded by EVICTION instead: once full, a new candidate replaces the current
 * weakest.  The fix is the one Grove's decoder shipped with (#45); ported here
 * with its diagnostics but not its other two departures (computed anchor centres
 * and tensor descriptors), so the three ports stay comparable.
 */
#include "blazeface.h"
#include "nn.h"

#include <stddef.h>
#include <stdint.h>

#define BF_INPUT       128.0f     /* box/score scale (x/y/w/h_scale in MediaPipe) */
#define BF_A512        512
#define BF_A384        384
#define BF_NANCHOR     (BF_A512 + BF_A384)   /* 896 */
#define BF_BOX_STRIDE  16         /* 4 bbox + 12 (6 keypoints x 2) per anchor      */

/* Score threshold as a raw (pre-sigmoid) logit so filtering needs no expf.
 * logit(p): 0.5->0.0  0.6->0.405  0.7->0.847  0.75->1.099  0.8->1.386.
 * Default 0.6 -- tune here (higher = fewer, more confident boxes). */
#ifndef BF_SCORE_LOGIT
#define BF_SCORE_LOGIT 0.405f
#endif
#define BF_NMS_IOU     0.5f

/* One pre-NMS candidate.  The anchor index rides along so that evicting the
 * weakest entry from a full list is decidable without consulting scan order. */
struct bf_cand {
	struct bf_det det;
	int           anchor;
};

/* Anchor cell centres (normalized) + pre-NMS candidate scratch.  Placed in the
 * .sdram.ai arena (FMC bank3, MPU region1 cacheable) to keep them off the tight
 * internal SRAM (owhinata/stm32f746g-disco#94): bf_cx/bf_cy are computed once at init
 * and read many times, bf_cand is write-before-read scratch, and all access is
 * CPU-only (the worker is the sole, serialized caller), so there is no DMA-coherency
 * concern.  w=h=1 fixed for the anchors, so only the centre matters. */
static float bf_cx[BF_NANCHOR] __attribute__((aligned(32), section(".sdram.ai")));
static float bf_cy[BF_NANCHOR] __attribute__((aligned(32), section(".sdram.ai")));
static int   bf_anchors_ready;   /* stays in SRAM .bss (a 4-byte flag) */

static struct bf_cand bf_cand[BF_MAX_CAND]
	__attribute__((aligned(32), section(".sdram.ai")));

/* Diagnostics of the last decode.  The peak tells whether the model responds to
 * the input at all (tuning normalization/threshold); the two counts separate
 * "nothing passed" from "NMS merged everything" from "the cap bound", which the
 * detection count alone cannot. */
static float bf_last_max = -1e9f;
static int   bf_last_npass;
static int   bf_last_nkept;

float blazeface_last_max_score(void) { return bf_last_max; }
int   blazeface_last_npass(void)     { return bf_last_npass; }
int   blazeface_last_nkept(void)     { return bf_last_nkept; }

static void bf_gen_anchors(void)
{
	int idx = 0;

	/* layer 0: 16x16 grid, 2 anchors/cell -> 512 (matches the 1x512x* tensors) */
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
			for (int a = 0; a < 2; a++) {
				bf_cx[idx] = ((float)x + 0.5f) / 16.0f;
				bf_cy[idx] = ((float)y + 0.5f) / 16.0f;
				idx++;
			}
	/* layer 1: 8x8 grid, 6 anchors/cell -> 384 (matches the 1x384x* tensors) */
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			for (int a = 0; a < 6; a++) {
				bf_cx[idx] = ((float)x + 0.5f) / 8.0f;
				bf_cy[idx] = ((float)y + 0.5f) / 8.0f;
				idx++;
			}
	bf_anchors_ready = 1;   /* idx == BF_NANCHOR */
}

static float bf_fabsf(float v) { return v < 0.0f ? -v : v; }

/* Algebraic fast sigmoid (no expf); adequate for a displayed confidence. */
static float bf_sigmoid(float x)
{
	return 0.5f + 0.5f * x / (1.0f + bf_fabsf(x));
}

/* IoU of two normalized boxes. */
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

static int bf_finite(float x) { return x == x && x < 3.0e38f && x > -3.0e38f; }

/* Find an output tensor by (anchors, channels).  Validates dtype, exact shape,
 * a non-NULL buffer, and that it is big enough for anchors*chan floats -- so a
 * malformed / same-shaped-but-smaller tensor can never cause an OOB read. */
static const float *bf_find(struct nn_model *m, int anchors, int chan)
{
	for (int i = 0; i < nn_output_count(m); i++) {
		struct nn_tensor *t = nn_output(m, i);
		if (t && t->dtype == NN_DTYPE_FLOAT32 && t->ndim == 3 && t->dims[0] == 1 &&
		    t->dims[1] == anchors && t->dims[2] == chan && t->data &&
		    t->bytes >= (uint32_t)anchors * (uint32_t)chan * sizeof(float))
			return (const float *)t->data;
	}
	return NULL;
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
 * Decode one anchor group (box[anchors*16], score[anchors]) into bf_cand.
 *
 * Visits EVERY anchor (issue #47): the peak-score diagnostic and the pass count
 * are only meaningful over the whole set, and the candidate list is bounded by
 * eviction rather than by stopping.
 */
static int bf_decode_group(const float *box, const float *score, int anchors,
                           int anchor_off, int nkept)
{
	for (int i = 0; i < anchors; i++) {
		const float *r = box + (size_t)i * BF_BOX_STRIDE;
		struct bf_det d;
		float cx, cy, w, h;

		if (!bf_finite(score[i]))           /* reject NaN/Inf scores            */
			continue;
		if (score[i] > bf_last_max)         /* diagnostic: peak model response */
			bf_last_max = score[i];
		if (score[i] <= BF_SCORE_LOGIT)     /* pre-sigmoid threshold */
			continue;

		bf_last_npass++;

		cx = r[0] / BF_INPUT + bf_cx[anchor_off + i];
		cy = r[1] / BF_INPUT + bf_cy[anchor_off + i];
		w  = r[2] / BF_INPUT;
		h  = r[3] / BF_INPUT;
		if (!bf_finite(cx) || !bf_finite(cy) || !bf_finite(w) || !bf_finite(h))
			continue;                        /* reject NaN/Inf boxes             */
		if (w <= 0.0f || h <= 0.0f)
			continue;

		d.x     = cx - w * 0.5f;
		d.y     = cy - h * 0.5f;
		d.w     = w;
		d.h     = h;
		d.score = bf_sigmoid(score[i]);

		nkept = bf_offer(&d, anchor_off + i, nkept);
	}
	return nkept;
}

int blazeface_decode(struct nn_model *m, struct bf_det *out, int max)
{
	const float *box512, *scr512, *box384, *scr384;
	int nkept = 0, nout = 0;
	uint8_t used[BF_MAX_CAND] = { 0 };

	if (!m || !out || max <= 0)
		return -1;

	box512 = bf_find(m, BF_A512, BF_BOX_STRIDE);
	scr512 = bf_find(m, BF_A512, 1);
	box384 = bf_find(m, BF_A384, BF_BOX_STRIDE);
	scr384 = bf_find(m, BF_A384, 1);
	if (!box512 || !scr512 || !box384 || !scr384)
		return -1;                       /* not BlazeFace-shaped -> no-op */

	if (!bf_anchors_ready)
		bf_gen_anchors();

	bf_last_max   = -1e9f;
	bf_last_npass = 0;
	bf_last_nkept = 0;

	nkept = bf_decode_group(box512, scr512, BF_A512, 0, nkept);
	nkept = bf_decode_group(box384, scr384, BF_A384, BF_A512, nkept);
	bf_last_nkept = nkept;

	/* Hard NMS: repeatedly take the highest-scoring unused box, suppress the
	 * rest that overlap it beyond BF_NMS_IOU. */
	while (nout < max) {
		int best = -1;
		float best_s = 0.0f;

		for (int i = 0; i < nkept; i++)
			if (!used[i] && bf_cand[i].det.score > best_s) {
				best_s = bf_cand[i].det.score;
				best = i;
			}
		if (best < 0)
			break;

		out[nout++] = bf_cand[best].det;
		used[best] = 1;
		for (int i = 0; i < nkept; i++)
			if (!used[i] &&
			    bf_iou(&bf_cand[best].det, &bf_cand[i].det) > BF_NMS_IOU)
				used[i] = 1;
	}
	return nout;
}
