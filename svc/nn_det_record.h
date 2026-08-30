/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_det_record.h
 * @brief   The published result of one detection decode, and the generation rule
 *          that keeps a dead session's decode from landing in a live one
 *          (issue #97).
 *
 * A camera stream decodes on a worker thread and a console prints on another, so
 * the boxes have to be published somewhere in between.  Two things about that are
 * easy to get wrong and neither is visible on hardware:
 *
 *   1. THE BOXES AND THEIR DIAGNOSTICS MUST TRAVEL TOGETHER.  Read separately,
 *      a console pairs this frame's boxes with whatever the decoder was last
 *      asked -- a different frame, while a stream runs.
 *   2. STOPPING A STREAM DOES NOT STOP AN INFERENCE ALREADY RUNNING.  A stop
 *      clears the record and then waits for the worker, which may be M?ms into an
 *      inference that will finish and publish afterwards.  Clearing under the
 *      same lock does not help: the clear happens first and the stale publish
 *      second.  So each session carries a GENERATION, the worker remembers it
 *      when it ARMS for a frame, and a publish whose generation no longer matches
 *      is dropped.
 *
 * [!] THE WORKER MUST SAMPLE THE GENERATION AT THE ARM, not after its wait
 * returns.  A stop and a fresh start can both happen while it is waiting, and a
 * value read afterwards would be the NEW session's -- so the old frame would
 * publish into it looking current.
 *
 * THIS FILE OWNS NO STORAGE.  The board declares the record and provides the
 * mutual exclusion; these are the decisions, factored out so they can be tested.
 * The ordering they encode cannot be produced deterministically on hardware.
 */
#ifndef NN_DET_RECORD_H
#define NN_DET_RECORD_H

#include <stdint.h>

#include "blazeface.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The last decode a session published.
 *
 * Board-allocated and board-locked: every function here must be called with the
 * board's detection lock held, exactly as the board's own publish did before.
 */
struct nn_det_record {
	struct bf_det    dets[BF_MAX_DET];
	int              ndet;   /**< faces, or -1 for "not a BlazeFace model"   */
	struct bf_result res;    /**< what that decode reported, thresh included */
	int              valid;  /**< 0 = this session has not decoded yet       */
	uint32_t         gen;    /**< session generation; see the file comment   */
};

/** A coherent read of @ref nn_det_record: boxes and diagnostics in one go. */
struct nn_det_snapshot {
	int              valid;
	int              ndet;
	struct bf_result res;

	/*
	 * [!] THE BOXES ARE SOMEWHERE ELSE (issue #103).  Set when the decoder that
	 * produced this count is a LOADED PLUGIN, whose result has a shape this
	 * firmware does not know -- not knowing it is the whole point of issue #78.
	 * The caller's `dets` array is then untouched and @ref res is zeroed, so a
	 * consumer must ask the board to describe the result (nn_svc_report())
	 * instead of reading boxes that were never written.
	 *
	 * A flag rather than a sentinel count: the count is still meaningful (it is
	 * the plugin's own, and negative values still carry BF_ERR_* meanings), and
	 * overloading it would make "how many" and "where from" one number.
	 */
	uint8_t          external;
};

/**
 * Begin a new session: invalidate the record and move the generation on.
 *
 * Called at start AND at stop.  At stop it is what makes an in-flight decode
 * harmless; at start it is what stops a fresh session showing the previous one's
 * numbers.
 */
void nn_det_record_reset(struct nn_det_record *r);

/** The generation a worker should remember when it arms for a frame. */
uint32_t nn_det_record_gen(const struct nn_det_record *r);

/**
 * Publish one decode.
 *
 * @param n    the decoder's return: >= 0 faces, or a negative BF_ERR_* code
 * @param gen  what @ref nn_det_record_gen returned when this frame was ARMED
 *
 * @return non-zero if it was taken.  A caller must not count a decode that was
 *         dropped: `nn run` waits on the inference counter and then reads the
 *         record, so a counter bumped for a publish that did not happen hands it
 *         the previous frame's boxes as this one's.
 *
 * [!] A NEGATIVE @p n IS PUBLISHED AS -1, NOT AS ZERO FACES (issue #57).  Zero
 * reads as a measurement, and it would sit next to diagnostics the decoder
 * returned too early to touch.
 */
int nn_det_record_publish(struct nn_det_record *r, const struct bf_det *d, int n,
                          const struct bf_result *res, uint32_t gen);

/**
 * Take a coherent snapshot, and up to @p max boxes with it.
 *
 * @param dets  optional
 */
void nn_det_record_snapshot(const struct nn_det_record *r,
                            struct nn_det_snapshot *out,
                            struct bf_det *dets, int max);

#ifdef __cplusplus
}
#endif

#endif /* NN_DET_RECORD_H */
