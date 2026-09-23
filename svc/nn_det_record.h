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
 *      marks the boundary and then waits for the worker, which may be
 *      milliseconds into an inference that will finish and publish afterwards.
 *      Doing it under the same lock does not help: the boundary happens first
 *      and the stale publish second.  So each session carries a GENERATION, the
 *      worker remembers it when it ARMS for a frame, and a publish whose
 *      generation no longer matches is dropped.
 *
 * [!] THE WORKER MUST SAMPLE THE GENERATION AT THE ARM, not after its wait
 * returns.  A stop and a fresh start can both happen while it is waiting, and a
 * value read afterwards would be the NEW session's -- so the old frame would
 * publish into it looking current.
 *
 * [!] A SESSION BOUNDARY IS NOT A MODEL CHANGE (issue #118).  They used to be
 * one call, which cleared the record at every start and stop -- so `nn dets`
 * after a `nn run` or a stopped stream said "nothing inferred", on two boards
 * and not the third.  They are two operations now:
 *
 *   - @ref nn_det_record_boundary: a session starts, re-arms or ends.  The
 *     generation moves, so an inference still in flight lands nowhere, and the
 *     last result STAYS -- it is still the last thing this model produced.
 *   - @ref nn_det_record_invalidate: the model or its decoder is replaced.  The
 *     result is cleared whatever became of the replacement, because it
 *     describes a model that is no longer the one a reader would ask about.
 *
 * With the result outliving its session, "valid" can no longer mean "this
 * session produced it", and nothing may use it that way.  What a session
 * produced is counted instead: the record counts every publish it ACCEPTS, under
 * the same lock as the publish, and hands the count out with every snapshot --
 * see @ref nn_det_last_valid.
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
	/**
	 * [!] THE DECODER'S OWN ACCOUNT STILL DESCRIBES THIS RESULT (issue #118).
	 * Set only by an ACCEPTED external publish.  A loaded plugin keeps its
	 * result privately, so its report is asked of the plugin -- and a decode
	 * whose publish the generation rule dropped has already rewritten that
	 * private state.  Asking it now would print a later frame's account beside
	 * this record's count.  The count stays; only the account is withheld.
	 */
	uint8_t          reportable;
	uint32_t         gen;    /**< session generation; see the file comment   */
	/** Publishes this record has accepted, ever.  Never reset: a reader keeps
	 *  its own base and subtracts (issue #118). */
	uint32_t         accepted;
	/** Moves on every @ref nn_det_record_invalidate, and only there: whether
	 *  the result a reader latched is still the one in force. */
	uint32_t         epoch;
	/** The generation the result was published under -- see
	 *  @ref nn_det_snapshot::current. */
	uint32_t         pub_gen;
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
	/** @ref nn_det_record::reportable, read in the same breath (issue #118). */
	uint8_t          reportable;
	/** @ref nn_det_record::accepted at this snapshot (issue #118). */
	uint32_t         accepted;
	/** @ref nn_det_record::epoch at this snapshot (issue #118). */
	uint32_t         epoch;
	/**
	 * [!] THE RESULT WAS PUBLISHED IN THE SESSION NOW IN FORCE (issue #118).
	 * `valid` survives a boundary now, so it no longer says this.  A reader
	 * that paints the result onto a LIVE picture needs exactly this answer:
	 * the panel keeps running after a stream stops, and a new stream's first
	 * frames would otherwise wear the previous stream's boxes.  A reader that
	 * reports the last result -- `nn dets` -- does not ask it.
	 */
	uint8_t          current;
};

/**
 * A session boundary: move the generation on and KEEP the result (issue #118).
 *
 * Called where a session starts, re-arms or ends.  At the end it is what makes
 * an in-flight decode harmless; at a start it is what keeps a decode armed under
 * the previous session out of this one.  The result it leaves standing is the
 * last one this model produced, and a reader that wants to know whether a
 * SESSION produced it compares @ref nn_det_record::accepted against a base it
 * took after this call -- see @ref nn_det_last_valid.
 *
 * [!] IT DOES NOT CLEAR, AND THAT IS THE CHANGE.  Until issue #118 the boundary
 * and the model change were one call, so the last result vanished at every stop
 * and `nn dets` after a `nn run` had nothing to read.
 */
void nn_det_record_boundary(struct nn_det_record *r);

/**
 * The model or its decoder changed: CLEAR the result (issue #118).
 *
 * Called whenever the identity a reader would ask about has actually changed --
 * a load that replaced the model, a load whose decoder was refused after the
 * model went in, a rollback that failed, an unload -- WHATEVER THE STATUS of the
 * operation that changed it, and before the new identity is visible to anyone.
 * The generation moves too, so a publish armed under the old identity cannot
 * land beside the new one; @ref nn_det_record::epoch moves so that a reader who
 * latched the old result can tell it has gone.
 */
void nn_det_record_invalidate(struct nn_det_record *r);

/** The generation a worker should remember when it arms for a frame. */
uint32_t nn_det_record_gen(const struct nn_det_record *r);

/**
 * Would a publish armed at @p gen still be taken?  Asked BEFORE a decode that
 * writes private state (issue #118).
 *
 * [!] A DROPPED PLUGIN PUBLISH IS NOT FREE: the decode that preceded it has
 * rewritten the plugin's own result, and the record then has to withhold the
 * account of what it holds (@ref nn_det_record::reportable).  A stop lands
 * while the worker is almost always inside an inference, so without this
 * question every stop cost the stopped stream's last account.  A worker that
 * asks it -- under whatever also excludes the boundary -- and skips the decode
 * when the answer is no leaves the plugin's result describing the record.
 */
int nn_det_record_admits(const struct nn_det_record *r, uint32_t gen);

/**
 * Publish one decode from a decoder whose boxes this firmware understands.
 *
 * @param n    the decoder's return: >= 0 faces, or a negative BF_ERR_* code
 * @param gen  what @ref nn_det_record_gen returned when this frame was ARMED
 *
 * @return non-zero if it was taken.  A taken publish is counted in
 *         @ref nn_det_record::accepted by this call, under the caller's lock;
 *         a dropped one is not counted anywhere, so a reader waiting for "a
 *         result of mine" waits on that count and never on a counter the
 *         board bumps beside it (issue #118).
 *
 * [!] A NEGATIVE @p n IS PUBLISHED AS ITSELF, NOT AS ZERO FACES (issue #57) AND
 * NOT AS -1 (issue #118).  Zero reads as a measurement.  And the codes are not
 * interchangeable: BF_ERR_MODEL means "not a detector" and routes to the class
 * report, while BF_ERR_UNINIT and BF_ERR_ARG mean this firmware is wired wrong
 * -- folded into -1, both of them sent the operator to a class report of a
 * model that was fine.
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
 * [!] BUT A DROPPED ONE IS NOT HARMLESS HERE (issue #118).  The decode that was
 * dropped has already run, and it rewrote the plugin's private result -- the
 * thing its report describes.  So a refusal clears
 * @ref nn_det_record::reportable: the count in the record stands, and asking the
 * plugin to describe it would describe the dropped frame instead.  An accepted
 * publish sets it again.
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

/**
 * Whether @p s holds a result produced since @p base was taken (issue #118).
 *
 * @param base  @ref nn_det_snapshot::accepted from a snapshot taken AFTER the
 *              session boundary that began the session asking, and before that
 *              session could publish -- everything accepted after the boundary
 *              is that session's, by the generation rule, so the difference is
 *              exactly its own publishes.  Taken late, it absorbs some of them
 *              and this answers "no" about a session that has a result: the
 *              safe direction, and the only one a late sample can err in.
 *
 * [!] BOTH HALVES, ALWAYS.  A count alone would call a result "this session's"
 * after the model it came from was unloaded; `valid` alone would call the
 * previous session's result this one's, which is exactly what stopped being
 * impossible when the boundary stopped clearing.
 *
 * The count is compared for inequality, not order: it wraps, and a session
 * would need four billion publishes before this misread one.
 */
int nn_det_last_valid(const struct nn_det_snapshot *s, uint32_t base);

#ifdef __cplusplus
}
#endif

#endif /* NN_DET_RECORD_H */
