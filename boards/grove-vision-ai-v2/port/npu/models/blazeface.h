/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blazeface.h
 * @brief   BlazeFace-front 128 face-detection decode (issue #45).
 *
 * Model-specific post-processing for ST model zoo's BlazeFace Front 128x128, a
 * MediaPipe/PINTO-derived SSD face detector.  It turns the four raw output
 * tensors (two anchor scales x {box regressors, scores}) into a short list of
 * face boxes: SSD anchor decode, then hard non-maximum suppression.
 *
 * IT DEPENDS ON npu.h AND NOTHING ELSE.  No hardware, no ThreadX, no libm, and
 * -- deliberately -- not on the NPU singleton either: the tensors arrive as an
 * ARRAY OF DESCRIPTORS from the caller.  That is what lets the host test drive
 * the real decoder with synthetic tensors instead of a stub, which matters
 * because every bug this file can have (a wrong zero point, an anchor grid off
 * by one, a candidate list that truncates) shows up on the board as a plausible
 * box in the wrong place, and each hypothesis otherwise costs a flash cycle.
 *
 * Anchors, 896 total, in the MediaPipe BlazeFace-front layout:
 *   - 16x16 grid, 2 anchors per cell -> 512   (the 1x512x* tensors)
 *   - 8x8   grid, 6 anchors per cell -> 384   (the 1x384x* tensors)
 * Each anchor is a cell-centre point; the box regressors are offsets from it.
 *
 * NOT A PURE FUNCTION.  The threshold and the diagnostics below are static, so
 * two concurrent decodes would interleave.  Serialisation is the caller's --
 * cmd_nn.c holds the same ownership gate for `nn detect` that it holds for
 * `nn run`.
 */
#ifndef BLAZEFACE_H
#define BLAZEFACE_H

#include "npu.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One detection: box normalised to the model input, plus a confidence. */
struct bf_det {
	float x, y, w, h;   /**< top-left origin, normalised to the 128x128 input */
	float score;        /**< sigmoid confidence, 0..1                         */
};

/** Detections returned at most (post-NMS). */
#define BF_MAX_DET 8

/**
 * Default score threshold, in milli-probability.
 *
 * The donor firmware tuned this as a raw pre-sigmoid logit of 0.405, and that
 * is the value proven to detect faces.  So the default here IS that logit,
 * expressed in the friendly unit through the same algebraic sigmoid the
 * reported scores use: 0.5 + 0.5 * 0.405 / 1.405 = 0.644.  Keeping one scale
 * matters more than a round number would look: "threshold 644" and "score 712"
 * are then comparable, and a box is shown exactly when its printed score clears
 * the printed threshold.
 */
#define BF_DEFAULT_THRESH_MILLI 644u

/**
 * Decode @p n output tensors into @p out[0..max).
 *
 * @return the number of detections (>= 0), or -1 if the tensors are NOT
 *         BlazeFace-shaped -- in which case nothing is written and no static
 *         state changes.  The four tensors are located BY SHAPE, not by index:
 *         the generated order is not the documented order, and it differs
 *         between the pre- and post-Vela graphs.
 *
 * Every tensor must be int8 with per-tensor quantisation; values are read as
 * (q - zero_point) * scale, which is the same arithmetic `nn run` uses to print
 * a class score.
 */
int blazeface_decode(const struct npu_tensor *outs, unsigned n,
                     struct bf_det *out, int max);

/**
 * @brief  Are these tensors BlazeFace-shaped, without decoding anything?
 *
 * For a caller that must decide BEFORE it commits to something expensive --
 * `nn preview` starts a camera stream, and discovering the wrong model is open
 * on every frame would be a preview that runs and silently never annotates.
 * Uses the same lookups blazeface_decode() does, so the two cannot disagree.
 *
 * @return non-zero if a decode would find all four tensors.
 */
int blazeface_shapes_ok(const struct npu_tensor *outs, unsigned n);

/**
 * Score threshold as a milli-probability on the same scale as bf_det.score.
 * Values outside 1..999 are REJECTED (returns non-zero and changes nothing):
 * 0 and 1000 are the poles of the inverse sigmoid.
 */
int      blazeface_set_thresh_milli(unsigned milli);
unsigned blazeface_get_thresh_milli(void);

/** The same threshold as the raw pre-sigmoid logit actually compared against. */
float blazeface_get_thresh_logit(void);

/**
 * Diagnostic: the highest raw (pre-sigmoid) score seen in the last decode.
 *
 * Over ALL 896 anchors, always -- the scan is never cut short, which is the
 * whole point of it.  A truncating scan reports the maximum of a prefix, and
 * then "no faces, peak 0.2" means nothing.
 */
float blazeface_last_max_score(void);

/**
 * Diagnostic: how many anchors passed the threshold in the last decode, BEFORE
 * NMS and BEFORE the candidate cap.  This is the honest count.
 */
int blazeface_last_npass(void);

/**
 * Diagnostic: how many of those were actually KEPT as candidates.  Less than
 * blazeface_last_npass() means the cap bound; the kept set is still the
 * highest-scoring ones, but NMS then only saw those.
 */
int blazeface_last_nkept(void);

/** Candidates retained before NMS.  Reported so the cap is never a surprise. */
#define BF_MAX_CAND 64

#ifdef __cplusplus
}
#endif

#endif /* BLAZEFACE_H */
