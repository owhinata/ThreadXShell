/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_shot.h
 * @brief   How one `nn run` ended, and whether it is still open (issue #171).
 *
 * `nn run` is a one-shot: ONE frame, through the same producer and worker as a
 * stream.  On wio-lite-ai and f746g-disco its wait used to know only whether the
 * record accepted a publish, so a run whose frame could not be prepared, whose
 * inference failed or whose lease was not had in time waited out its deadline
 * and said TIMEOUT.  This file keeps, per board, the run's number and the first
 * thing that ended it, so the wait can say NO_RESULT and why, and so the
 * producer and the worker can tell that the run is over and take no more
 * frames for it.
 *
 * THE STATE is the board's @ref nn_shot (zero is "no run"):
 *
 *   run     the current run's number; 0 is never a run
 *   ended   the run the ending below belongs to (0: none has ended)
 *   reason  that ending, enum nn_shot_reason
 *
 * A run is OPEN while `run != 0` and `ended != run`.
 *
 * [!] WHO CALLS, UNDER WHICH LOCK.  Every call is one whole transition -- the
 * test and the change -- and the board makes it inside ONE hold of the lock
 * that moves its record's generation (f746g-disco `nncam_lock`, wio-lite-ai
 * `nncam_det_lock`).  The same hold that asks "is the generation I sampled
 * still current" passes the answer as @p gen_current, so no boundary can fall
 * between the question and the write.  NOT the interrupt mask (PRIMASK) the
 * hand-over word is stepped under: a board never mixes the two for this state.
 *
 *   nn_shot_begin()     the start, before the run is let go (the worker parked,
 *                       the producer outside a frame -- the start's own
 *                       preconditions; so no writer of the old number is alive)
 *   nn_shot_open()      the producer, at the first part of a one-shot frame,
 *                       BEFORE it BEGINs a fill; and the worker before it arms
 *   nn_shot_record()    the producer (prep failed) and the worker (invoke
 *                       failed, lease not had), with the generation question
 *   nn_shot_after_job() the worker, after the board counted the job: the
 *                       classification, the record and the re-arm answer in one
 *   nn_shot_failed()    the console, on every poll, before "published"
 *
 * [!] FILLING HAS THE SAME TWO WAYS OUT (nn_handoff.h): ABANDON and JOIN.  This
 * file does not add a third.  A closed run is kept off the input by the
 * producer asking nn_shot_open() BEFORE BEGIN -- it never enters FILLING for a
 * closed run -- not by taking FILLING away.
 *
 * [!] A PUBLISH THE RECORD TOOK ENDS THE RUN AS PUBLISHED, WHATEVER THE
 * GENERATION (issue #118: an accepted publish is answered by the record's
 * accepted count, and that count never goes back).  It is recorded so that the
 * run closes; it is NOT a failure, and nn_shot_failed() does not report it, so
 * the console never passes it to the wait as "finished".
 *
 * [!] AN ENDING NOBODY CAN EXPLAIN IS NO_RESULT, NOT "KEEP WAITING".  A worker
 * answer this table does not know ends the run as NN_SHOT_UNKNOWN: left open,
 * the run would wait out its deadline and print no reason.
 *
 * [!] THE FIRST ENDING WINS.  Once a run has ended, every later record for it
 * is refused and leaves the state as it is.
 *
 * [!] THERE IS NO "INVALIDATE".  A stop does not clear anything here; the next
 * nn_shot_begin() writes a new number over the old one, and every record that
 * carries the old number is refused from then on.  The number wraps past 0, and
 * a wrap cannot meet a live writer of the same number: begin is called only
 * while none is alive.
 *
 * Pure: no mutable storage, no lock, no board header, no tx_api.h.  Walked by
 * shell/test/test_nn_shot.c (the table and every transition) and
 * shell/test/test_nn_shot_seq.c (the call orders of the two boards, driven
 * through the real nn_core_frame.c and nn_handoff.c).
 */
#ifndef NN_SHOT_H
#define NN_SHOT_H

#include <stdint.h>

#include "nn_core_frame.h"   /* enum nn_core_done */

#ifdef __cplusplus
extern "C" {
#endif

/** What ended a one-shot run.  NONE until something has. */
enum nn_shot_reason {
	NN_SHOT_NONE = 0,
	/** The record accepted a publish of this run.  Not a failure. */
	NN_SHOT_PUBLISHED,
	/** The producer could not prepare the frame. */
	NN_SHOT_PREP,
	/** The inference failed. */
	NN_SHOT_INVOKE,
	/** The plugin lease was not had in time: inferred, not decoded. */
	NN_SHOT_LEASE_TIMEOUT,
	/** The decode was reached without the lease (NN_CORE_DONE_NOT_HELD). */
	NN_SHOT_NOT_HELD,
	/** The outputs could not be handed to the decoder
	 *  (NN_CORE_DONE_NO_OUTPUTS). */
	NN_SHOT_NO_OUTPUTS,
	/** A worker answer this file does not know: fail-closed. */
	NN_SHOT_UNKNOWN,
};

/** One board's one-shot state.  Zero-initialised static storage in the board. */
struct nn_shot {
	uint32_t run;     /**< the current run; 0 is none          */
	uint32_t ended;   /**< the run @ref reason belongs to      */
	uint8_t  reason;  /**< enum nn_shot_reason                 */
};

/** What the console reports for a failure: the `nn` status and the sentence. */
struct nn_shot_says {
	int         status;  /**< NN_SVC_ERR_* (nn_svc.h)               */
	const char *text;    /**< a whole sentence, <= NN_SVC_DETAIL_MAX */
};

/** A new run: the next number (never 0), nothing ended.  @return the run. */
uint32_t nn_shot_begin(struct nn_shot *s);

/** @return the current run while it is open, else 0. */
uint32_t nn_shot_open(const struct nn_shot *s);

/**
 * How a job the worker finished ends the run.  RAW or DECODED that the record
 * took: PUBLISHED.  RAW or DECODED not taken, and RETIRED: NONE -- not an
 * ending, the next frame is this run's.  NOT_HELD, NO_OUTPUTS: their reasons.
 * Anything else: UNKNOWN.
 */
enum nn_shot_reason nn_shot_of_done(enum nn_core_done what, int took);

/**
 * End run @p run with @p reason, if it may: @p run is the current run, it has
 * not ended, and either @p reason is PUBLISHED or @p gen_current says the
 * generation the caller sampled for this frame is still the record's.  NONE
 * writes nothing; an out-of-range reason is written as UNKNOWN.
 *
 * @return non-zero if @p run is still open after the call (the worker re-arms
 *         on it, the producer keeps taking frames for it)
 */
int nn_shot_record(struct nn_shot *s, uint32_t run, enum nn_shot_reason reason,
                   int gen_current);

/**
 * The worker's end of a job: nn_shot_of_done() then nn_shot_record().
 * @return non-zero if the worker may arm for another frame of @p run.
 */
int nn_shot_after_job(struct nn_shot *s, uint32_t run, enum nn_core_done what,
                      int took, int gen_current);

/**
 * Did the current run end WITHOUT a result?  PUBLISHED and NONE answer no.
 * @return non-zero with @p *reason set (when @p reason is not NULL)
 */
int nn_shot_failed(const struct nn_shot *s, enum nn_shot_reason *reason);

/**
 * The longest @ref nn_shot_says::text, in characters without the terminator.
 * A board that follows a sentence with its own clause checks that clause at
 * build time against it (nn_refusal.h, NN_REFUSAL_RUN_UNFINISHED()).
 *
 * [!] HELD TO THE TABLE BY shell/test/test_nn_shot.c: no sentence is longer,
 * and the longest is EXACTLY this (issue #174).
 */
#define NN_SHOT_SAYS_MAX 62u

/** The status and sentence for a failure.  Any reason that is not one --
 *  NONE, PUBLISHED, out of range -- answers as UNKNOWN.  Never NULL. */
const struct nn_shot_says *nn_shot_says(enum nn_shot_reason reason);

#ifdef __cplusplus
}
#endif

#endif /* NN_SHOT_H */
