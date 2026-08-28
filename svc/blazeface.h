/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blazeface.h
 * @brief   BlazeFace-front 128 face-detection decode, shared by all boards
 *          (issue #97; was three diverged copies under boards/ * /port/ * /models/).
 *
 * Model-specific post-processing for the MediaPipe/PINTO-derived BlazeFace Front
 * 128x128 SSD face detector.  It turns four raw output tensors (two anchor scales
 * x {box regressors, scores}) into a short list of boxes: SSD anchor decode, then
 * hard non-maximum suppression.
 *
 * IT DEPENDS ON tensor.h AND THE C LIBRARY HEADERS, AND NOTHING ELSE.  No
 * hardware, no ThreadX, no HAL, no libm, and not on any inference singleton: the
 * tensors arrive as an ARRAY OF DESCRIPTORS from the caller.  That is what lets
 * the host test drive the real decoder with synthetic tensors instead of a stub,
 * which matters because every bug this file can have (a wrong zero point, an
 * anchor grid off by one, a candidate list that truncates) shows up on a board as
 * a plausible box in the wrong place, and each hypothesis otherwise costs a flash
 * cycle.
 *
 * Anchors, 896 total, in the MediaPipe BlazeFace-front layout:
 *   - 16x16 grid, 2 anchors per cell -> 512   (the 1x512x* tensors)
 *   - 8x8   grid, 6 anchors per cell -> 384   (the 1x384x* tensors)
 * Each anchor is a cell-centre point; the box regressors are offsets from it.
 *
 * BOTH ELEMENT TYPES.  Grove flashes the model with its boundary QUANTIZE and
 * DEQUANTIZEs stripped, so its four tensors arrive int8, each with its OWN scale
 * and zero point (they differ by a factor of 33 on that model).  f746 and wio run
 * float32 graphs.  One decoder serves both: float32 is read straight, int8 goes
 * through (q - zero_point) * scale USING THE TENSOR IT CAME FROM.  A single
 * shared dequantisation constant would look right and be wrong on three tensors.
 *
 * @section storage Where the state lives
 *
 * THIS TRANSLATION UNIT OWNS NO MUTABLE STORAGE, and a per-board build gate
 * enforces that.  The split follows svc/frame_pipeline, whose engine likewise
 * owns nothing: on all three boards `struct frame_pipeline` is a plain board
 * static and only the ring's slot memory carries a section attribute.
 *
 *   - @ref blazeface is ORDINARY, INITIALISED memory the board declares plainly.
 *     It must NOT be given a placement attribute: the carve-outs the scratch
 *     lives in are NOLOAD on two boards (wio `.psram_ai`, f746 `.sdram`), so an
 *     object with initialised fields placed there is never loaded -- and NOLOAD
 *     keeps stale values across a warm reset, so it would fail by appearing to
 *     work.  Keeping the state in internal RAM also means a board whose external
 *     memory failed to come up can still read and set the threshold.
 *   - The CANDIDATE SCRATCH is injected by the board through blazeface_init(),
 *     so each board keeps the placement it already had (wio `.psram_ai`, f746
 *     `.sdram.ai`, Grove plain .bss) and its own post-link residency gate keeps
 *     pointing at a board-owned symbol.
 *
 * @section serial Serialisation
 *
 * DECODING IS NOT REENTRANT: one @ref blazeface plus its scratch is one decode at
 * a time, and serialising is the caller's job -- the same contract svc/ymodem
 * states.  All three boards already hold an ownership gate across a decode.
 *
 * THE THRESHOLD IS THE EXCEPTION and is deliberately NOT under that contract.  It
 * is a single word, written atomically and read exactly ONCE at the top of a
 * decode; the value used is reported in @ref bf_result.  So a threshold changed
 * from another thread takes effect on the next frame and can never apply to part
 * of one, and a caller printing boxes prints the threshold that produced them
 * instead of whatever is current by the time it formats the line.
 */
#ifndef BLAZEFACE_H
#define BLAZEFACE_H

#include <stddef.h>
#include <stdint.h>

#include "tensor.h"

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

/** Candidates retained before NMS.  Reported so the cap is never a surprise. */
#define BF_MAX_CAND 64

/**
 * Default score threshold, in milli-probability.
 *
 * The donor firmware tuned this as a raw pre-sigmoid logit of 0.405, and that is
 * the value proven to detect faces.  So the default here IS that logit, expressed
 * in the friendly unit through the same algebraic sigmoid the reported scores
 * use: 0.5 + 0.5 * 0.405 / 1.405 = 0.644.  Keeping one scale matters more than a
 * round number would look: "threshold 644" and "score 712" are then comparable,
 * and a box is shown exactly when its printed score clears the printed threshold.
 */
#define BF_DEFAULT_THRESH_MILLI 644u

/**
 * One pre-NMS candidate.  Public because the BOARD owns the array (see
 * @ref storage); the anchor index rides along so that evicting the weakest entry
 * from a full list is decidable without consulting scan order.
 */
struct bf_cand {
	struct bf_det det;
	int32_t       anchor;
};

/**
 * Status of a decode.
 *
 * [!] THESE ARE NOT INTERCHANGEABLE and a caller must not fold them together.
 * BF_ERR_MODEL is the one a user can cause and the only one that means "this is
 * not a BlazeFace model"; reporting it for a wiring fault would send someone
 * looking at the model.  Folding a failure into "0 detections" is worse still --
 * that reads as a measurement (issue #57).
 */
#define BF_OK           0     /**< decode ran; the count is the return value    */
#define BF_ERR_MODEL  (-1)    /**< tensors are not BlazeFace-shaped             */
#define BF_ERR_UNINIT (-2)    /**< blazeface_init() has not succeeded on this   */
#define BF_ERR_ARG    (-3)    /**< a null pointer or a non-positive capacity    */

/**
 * Decoder state.  The board declares one of these as PLAIN static storage; the
 * fields are private and are set up by blazeface_init().
 */
struct blazeface {
	struct bf_cand *cand;          /**< board-owned scratch                     */
	unsigned        ncand;         /**< entries in @ref cand, <= BF_MAX_CAND    */
	unsigned        thresh_milli;  /**< read/written atomically; see @ref serial*/
	unsigned        ready;         /**< nonzero once init succeeded             */
};

/**
 * What one decode did, beyond the box count.
 *
 * Between them these separate "the model responded to nothing" from "the
 * threshold is too high" from "NMS merged everything", which one detection count
 * cannot.  They are returned per decode rather than read back from the decoder
 * afterwards, because the caller that reports them is often not the one that
 * decoded: a stream worker decodes and a console prints, and reading globals at
 * print time pairs this frame's boxes with some other frame's diagnostics.
 */
struct bf_result {
	int      status;        /**< BF_OK or one of the BF_ERR_* codes             */
	float    max_score;     /**< highest RAW pre-sigmoid score, over ALL anchors*/
	int      npass;         /**< anchors over the threshold, before the cap     */
	int      nkept;         /**< how many of those were kept as candidates      */
	unsigned thresh_milli;  /**< the threshold THIS decode applied              */
};

/**
 * Bind @p bf to a board-owned candidate scratch buffer.
 *
 * @param scratch  board-owned array; keeps whatever placement the board gives it
 * @param bytes    its size; must hold at least one @ref bf_cand
 *
 * @return BF_OK, or BF_ERR_ARG if @p scratch is null, misaligned for
 *         @ref bf_cand, or too small.
 *
 * The alignment and capacity are checked at run time even though a board's
 * definition satisfies both by construction: this takes an arbitrary pointer, so
 * the linker guarantees that hold for the board's own symbol say nothing here.
 *
 * [!] On entry the ready flag is cleared and it is set again only on success, so
 * a FAILED re-init of an already-initialised decoder leaves it unusable rather
 * than quietly still bound to the old buffer.  Re-init is subject to the same
 * caller serialisation as a decode.
 */
int blazeface_init(struct blazeface *bf, struct bf_cand *scratch, size_t bytes);

/**
 * Score threshold as a milli-probability, on the same scale as bf_det.score.
 *
 * Values outside 1..999 are REJECTED (returns BF_ERR_ARG and changes nothing):
 * 0 and 1000 are the poles of the inverse sigmoid, and a silently clamped
 * threshold is a setting that does not do what it says.
 */
int      blazeface_set_thresh_milli(struct blazeface *bf, unsigned milli);
unsigned blazeface_get_thresh_milli(const struct blazeface *bf);

/**
 * @brief  Are these tensors BlazeFace-shaped, without decoding anything?
 *
 * For a caller that must decide BEFORE it commits to something expensive --
 * Grove's `nn preview` starts a camera stream, and discovering the wrong model is
 * open on every frame would be a preview that runs and silently never annotates.
 * Uses the same lookups blazeface_decode() does, so the two cannot disagree.
 *
 * @return non-zero if a decode would find all four tensors.
 */
int blazeface_shapes_ok(const struct tensor_desc *outs, unsigned n);

/**
 * Decode @p n output tensors into @p out[0..max).
 *
 * @param res  optional; filled on EVERY path, including the failures, so a
 *             caller never publishes diagnostics left over from a previous frame
 *
 * @return the number of detections (>= 0), or one of the BF_ERR_* codes.  The
 *         four tensors are located BY SHAPE, not by index: the generated order is
 *         not the documented order, and it differs between the pre- and post-Vela
 *         graphs.
 */
int blazeface_decode(struct blazeface *bf, const struct tensor_desc *outs,
                     unsigned n, struct bf_det *out, int max,
                     struct bf_result *res);

#ifdef __cplusplus
}
#endif

#endif /* BLAZEFACE_H */
