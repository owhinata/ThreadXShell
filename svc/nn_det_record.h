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
	int              ndet;   /**< items; see @ref nn_det_kind for what they are */
	struct bf_result res;    /**< what that decode reported, thresh included */
	/**
	 * [!] "THIS SESSION HAS PUBLISHED AN INFERENCE RESULT", which is not the
	 * same as "a decode ran" (issue #116).  A board with no decoder publishes
	 * the fact that the model ran and nothing interpreted its outputs, and that
	 * is a result of the current generation like any other -- @ref nn_det_kind
	 * is what says which it is.  0 means this session has published none yet.
	 */
	int              valid;
	uint8_t          kind;   /**< one of @ref nn_det_kind (issue #110)       */
	uint32_t         gen;    /**< session generation; see the file comment   */
};

/**
 * Where the result of one decode actually is (issues #103, #104).
 *
 * [!] ONE EXCLUSIVE FIELD, NOT A FLAG PER CASE.  This began as a single boolean
 * -- "the boxes are the plugin's" -- and issue #104 needed a third answer, for a
 * board with no decoder at all.  Added as a second boolean the two would have
 * had a meaningless combination, and a consumer meeting it would have had to
 * invent a precedence the producers never agreed on.
 *
 * [!] AND THE ZERO IS THE ORDINARY CASE ON PURPOSE.  Two boards fill the
 * caller's array and nothing else; making their answer the default is what lets
 * this be added without changing what they mean.  It does NOT excuse them from
 * saying so -- see @ref nn_det_record_snapshot.
 */
enum nn_det_kind {
	/** The count is faces and the boxes are in the caller's array. */
	NN_DET_CALLER_BOXES = 0,
	/**
	 * [!] THE BOXES ARE SOMEWHERE ELSE (issue #103).  The decoder that produced
	 * this count is a LOADED PLUGIN, whose result has a shape this firmware does
	 * not know -- not knowing it is the whole point of issue #78.  The caller's
	 * `dets` array is untouched and @ref nn_det_snapshot::res is zeroed, so a
	 * consumer must ask the board to describe the result (nn_svc_report())
	 * instead of reading boxes that were never written.
	 *
	 * The count stays meaningful beside it (it is the plugin's own, and negative
	 * values still carry BF_ERR_* meanings), which is why this is a separate
	 * field rather than a sentinel count: overloading would make "how many" and
	 * "where from" one number.
	 */
	NN_DET_PLUGIN_REPORT = 1,
	/**
	 * [!] NOTHING DECODED THESE OUTPUTS (issue #104).  A board with no decoder
	 * ran the model and has raw output tensors and no interpretation of them.
	 * The count means nothing here and neither do the boxes; a consumer reports
	 * the tensors themselves.  Issue #116 gave it a publish of its own
	 * (@ref nn_det_record_publish_raw), for a board that decodes -- or here,
	 * does not -- on a worker thread and has to say so through the record.
	 *
	 * Deliberately NOT spelled as a BF_ERR_* code: those are a decoder's
	 * vocabulary, and BF_ERR_MODEL in particular means "not a detector", which
	 * routes to the shared class report -- printing the top 5 of a detector's
	 * regression tensor as though they were classes.
	 */
	NN_DET_RAW_TENSORS = 2
};

/** A coherent read of @ref nn_det_record: boxes and diagnostics in one go. */
struct nn_det_snapshot {
	int              valid;
	int              ndet;
	struct bf_result res;
	uint8_t          kind;   /**< one of @ref nn_det_kind */
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
 * Publish one decode from a decoder whose boxes this firmware understands.
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
 * Publish one decode whose RESULT STAYED WITH THE DECODER (issue #110).
 *
 * A loaded plugin keeps its own result -- the firmware does not know its shape,
 * which is the point of issue #78 -- so what travels here is the fact that a
 * decode happened, how many items it produced, and the generation rule.  The
 * boxes do not, and neither do @ref bf_result's diagnostics, which belong to a
 * decoder that did not run.
 *
 * Same generation rule and same return as @ref nn_det_record_publish: a decode
 * from a retired session lands nowhere.
 *
 * [!] @p n IS NOT CLAMPED AND NOT NORMALISED.  The cap on the other path is the
 * size of the box array, and there is no box array here; the negative
 * normalisation is issue #57's rule about the SHARED decoder's vocabulary, and
 * a plugin's negative values are its own.  Folding either would make this
 * function state something about a result it cannot see.
 */
int nn_det_record_publish_external(struct nn_det_record *r, int n, uint32_t gen);

/**
 * Publish the fact that AN INFERENCE RAN AND NOTHING DECODED IT (issue #116).
 *
 * A firmware that carries no decoder still runs the model: the outputs are
 * there and what is missing is an interpretation of them.  That is a result --
 * it is reported as the tensors themselves -- and on a board whose inference
 * finishes on a worker thread it has to travel through the record like any
 * other, because a console waits on the inference counter and a caller must not
 * count a publish that was dropped.  Without this the counter could never move
 * for a bare model, and `nn run` would wait out its timeout over an inference
 * that had already finished.
 *
 * Same generation rule and same return as @ref nn_det_record_publish: a result
 * from a retired session lands nowhere, and a mismatch leaves the record
 * exactly as it was.
 *
 * [!] THE COUNT IS ZERO BECAUSE THERE IS NOTHING TO COUNT, not because a
 * decoder looked and found nothing.  @ref NN_DET_RAW_TENSORS is what tells
 * those apart; the number beside it is not a measurement anybody took, so there
 * is no other number to put there.
 *
 * [!] AND IT IS NOT SPELLED AS A BF_ERR_* CODE.  Those are a decoder's
 * vocabulary, and BF_ERR_MODEL in particular routes to the shared class report
 * -- the top 5 of a detector's regression tensor, printed as though the numbers
 * were classes.  The boxes and @ref bf_result's diagnostics are cleared for the
 * same reason they are on the external path: they describe a decoder that did
 * not run, and stale ones must not be left standing beside this.
 */
int nn_det_record_publish_raw(struct nn_det_record *r, uint32_t gen);

/**
 * Take a coherent snapshot, and up to @p max boxes with it.
 *
 * @param dets  optional
 *
 * [!] IT SETS @ref nn_det_snapshot::kind FROM THE RECORD (issue #110).  It used
 * to assert NN_DET_CALLER_BOXES, which was true while that was the only thing a
 * record could hold and is the sort of statement that survives the fact it was
 * based on.
 *
 * [!] AND THE BOXES ARE COPIED ONLY FOR THAT KIND.  The rule is written as "copy
 * when the record says caller boxes", not "copy unless it says otherwise": a
 * kind this function has not heard of must leave the caller's array alone, and
 * the version of that test which asks what the kind is NOT fails open the day
 * a fourth one appears.  A caller's array is therefore untouched by an external
 * result, canaries and all.
 *
 * [!] A BOARD'S OWN PROJECTION MUST CARRY THE KIND TOO.  Several boards copy
 * this snapshot into a struct of their own before the shared command sees it,
 * and a projection that drops the field restores exactly the bug above.
 * f746g-disco's projection does not carry it and its service layer restates
 * NN_DET_CALLER_BOXES instead -- correct there, because nothing on that board
 * produces another kind yet, and a thing to revisit when something does.
 */
void nn_det_record_snapshot(const struct nn_det_record *r,
                            struct nn_det_snapshot *out,
                            struct bf_det *dets, int max);

#ifdef __cplusplus
}
#endif

#endif /* NN_DET_RECORD_H */
