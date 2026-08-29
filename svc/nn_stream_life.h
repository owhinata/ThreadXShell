/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_stream_life.h
 * @brief   One live-inference lifecycle, shared by every board (issue #99).
 *
 * WHY THIS IS SHARED RATHER THAN THREE COPIES.  The first cut of issue #99 gave
 * the Grove adapter a real state machine -- starting, running, stopping, lost --
 * and gave the other two only a generation comparison.  That looked like the
 * same gate and was not: comparing the generation without CLAIMING the stop
 * transition lets two callers both receive "go" for the same stream, and then
 *
 *   1. the first finishes stopping generation G;
 *   2. somebody starts G+1;
 *   3. the second, delayed, executes its stop -- against G+1.
 *
 * A stream is torn down by a caller that never started it, which is the exact
 * failure the generation was introduced to prevent.  The bug was not that two
 * boards were written carelessly; it was that the rule existed in three places
 * and only one of them was complete.  So there is now one machine, the boards
 * own only its storage, and a host test walks the interleavings that no console
 * can produce.
 *
 * [!] EVERY TRANSITION REPORTS WHETHER IT HAPPENED, and a caller with side
 * effects of its own must apply them only on success.  A guard that refuses
 * silently is only half a guard: a board whose wrapper cleared its hardware
 * claim regardless would release it on exactly the invariant failure the guard
 * was added to catch -- which is the fail-open direction, not the safe one.
 *
 * [!] EVERY CALL HERE MUST BE MADE UNDER THE CALLER'S CRITICAL SECTION.  These
 * functions are pure state transitions with no locking of their own, because
 * what "a critical section" means belongs to the port -- and the one thing that
 * must be indivisible is the test-and-claim, which is why it is a single call
 * rather than a query the caller acts on afterwards.
 *
 * [!] AND IT OWNS NO STORAGE.  The state lives in the board's own
 * @ref nn_stream_life, so each board keeps its own memory map, and this file is
 * audited by cmake/check_no_mutable_storage.py alongside the shared decoder and
 * the shared command.
 */
#ifndef NN_STREAM_LIFE_H
#define NN_STREAM_LIFE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * "Whatever is running now", which is what an operator typing `nn stream stop`
 * means.
 *
 * [!] NEVER HANDED OUT AS A REAL GENERATION, and skipped on wrap.  A waiter that
 * held it would have the operator's authority to stop any stream -- precisely
 * what the generation withholds from it.
 */
#define NN_STREAM_GEN_ANY  0u

/** Where one board's live inference has got to. */
enum nn_stream_phase {
	NN_STREAM_PHASE_IDLE = 0,
	NN_STREAM_PHASE_STARTING,  /**< a start owns it; nothing is up yet     */
	NN_STREAM_PHASE_RUNNING,
	NN_STREAM_PHASE_STOPPING,  /**< a stop owns it                        */
	/** A teardown that could not be confirmed.  Refuses for good: a thread may
	 *  still be inside what would be torn down, and the board's claim is never
	 *  given back. */
	NN_STREAM_PHASE_LOST,
};

/**
 * A board's stream lifecycle.  Zero-initialised static storage in the adapter;
 * this file never declares one.
 */
struct nn_stream_life {
	uint8_t  phase;  /**< enum nn_stream_phase                            */
	/** What STARTING was entered from, so an abort puts it back rather than
	 *  guessing IDLE -- a re-armed stream aborts to RUNNING. */
	uint8_t  prev;
	uint32_t gen;    /**< the last generation minted; KEPT after it ends  */
	uint32_t next;   /**< the next to mint; 0 means "start at 1"          */
	/**
	 * [!] BUMPED ON EVERY TRANSITION, and that is not the same information as
	 * the phase and the generation.  A retryable stop deliberately returns to
	 * RUNNING with its generation intact, so a reader that sampled those two
	 * either side of a whole failed teardown would find them unchanged and
	 * accept a torn reading.  This is what makes such a read a seqlock rather
	 * than an ABA.
	 */
	uint32_t seq;
};

/** What the generation comparison alone says. */
enum nn_stream_gen_verdict {
	NN_STREAM_GEN_GO = 0,   /**< it is ours, or "whatever is running" */
	NN_STREAM_GEN_IDLE,     /**< nothing has ever run                 */
	NN_STREAM_GEN_WRONG,    /**< ours is gone; this is somebody else's */
};

/**
 * The generation rule on its own.
 *
 * Pure and inline so it costs no storage and a host test can walk it.  A board
 * never calls this directly -- nn_stream_life_claim_stop() does, under the
 * caller's critical section, together with the claim.
 */
static inline enum nn_stream_gen_verdict
nn_stream_gen_check(uint32_t current, uint32_t requested)
{
	if (current == NN_STREAM_GEN_ANY)
		return NN_STREAM_GEN_IDLE;
	/* [!] "Whatever is running" matches deliberately: it is the operator
	 * typing `nn stream stop`.  A WAITER never passes it -- it passes the
	 * generation it was given -- and that is the whole of how the two are
	 * told apart. */
	if (requested != NN_STREAM_GEN_ANY && requested != current)
		return NN_STREAM_GEN_WRONG;
	return NN_STREAM_GEN_GO;
}

/** What a stop may do. */
enum nn_stream_stop_claim {
	NN_STREAM_STOP_GO = 0,   /**< claimed; the caller now owns the teardown */
	NN_STREAM_STOP_IDLE,     /**< nothing is running                        */
	NN_STREAM_STOP_WRONG_GEN,/**< that stream has already been replaced     */
	NN_STREAM_STOP_BUSY,     /**< a start or another stop owns the transition */
	NN_STREAM_STOP_DEAD,     /**< a previous teardown was never confirmed   */
};

/** Why a start was or was not admitted.  Three refusals, because they call for
 *  three different sentences: "one is already running" is not "somebody is
 *  mid-transition, try again" and neither is "this is dead until reboot". */
enum nn_stream_start_claim {
	NN_STREAM_START_GO = 0,
	NN_STREAM_START_RUNNING,  /**< a stream is already up                     */
	NN_STREAM_START_BUSY,     /**< a start or a stop owns the transition      */
	NN_STREAM_START_DEAD,     /**< a previous teardown was never confirmed    */
};

/**
 * Claim IDLE -> STARTING.
 *
 * [!] CLAIM IT BEFORE STARTING THE HARDWARE, NOT AFTER.  Recording the start
 * once the worker is already running leaves a window in which this says IDLE
 * while the board is streaming -- long enough for a concurrent stop to be told
 * "not running", and on the board that can RE-ARM a live stream, long enough for
 * the re-arm to overwrite a stop that had already claimed STOPPING.  The whole
 * point of the phase is that it is claimed across the operation.
 *
 * @return NN_STREAM_START_GO if it was taken -- the caller must then commit or
 *         abort -- or which of the three refusals it was
 */
enum nn_stream_start_claim nn_stream_life_begin(struct nn_stream_life *l);

/**
 * Claim RUNNING -> STARTING, for a start that re-arms a stream already up.
 *
 * One board can re-arm after its capture died under it: the worker keeps its
 * guards and its counters, and the operation succeeds without ever having
 * stopped.  That is a real transition and it needs admitting like any other --
 * begin() cannot represent it, and letting a re-arm skip admission is what let
 * it stamp on a teardown already in progress.
 *
 * @return non-zero if it was taken; abort() returns it to RUNNING
 */
int nn_stream_life_rearm(struct nn_stream_life *l);

/**
 * Everything came up: mint a generation and go RUNNING.
 *
 * [!] ONLY FROM STARTING, and it refuses otherwise rather than forcing the
 * phase.  Committing from anywhere would let a start overwrite a stop that owns
 * the teardown, and even resurrect a lifecycle latched LOST -- a phase whose
 * whole meaning is that something may still be inside it.
 *
 * @return the new generation, or NN_STREAM_GEN_ANY if it was refused, in which
 *         case nothing changed and the caller has hardware it must undo
 */
uint32_t nn_stream_life_commit(struct nn_stream_life *l);

/** A start that failed after begin() or rearm(): back to whichever phase it was
 *  claimed from, nothing minted.
 *  @return non-zero if the transition happened */
int nn_stream_life_abort(struct nn_stream_life *l);

/**
 * Test the generation AND claim the stop, indivisibly.
 *
 * [!] THE TWO HALVES ARE ONE CALL BECAUSE THAT IS THE BUG.  A caller that asked
 * "is this still my stream?" and then acted would leave the window in which a
 * second stop is admitted for the same generation -- and one of the two then
 * runs its teardown against whatever is there by the time it gets round to it.
 * On NN_STREAM_STOP_GO the phase is already STOPPING, so nothing else is
 * admitted until the caller reports back.
 */
enum nn_stream_stop_claim nn_stream_life_claim_stop(struct nn_stream_life *l,
                                                    uint32_t gen);

/** Both halves confirmed: STOPPING -> IDLE.  The generation is KEPT, so a
 *  waiter can still tell "mine ended" from "a successor is running".
 *  Refuses unless the caller claimed the teardown.
 *  @return non-zero if the transition happened */
int nn_stream_life_finish(struct nn_stream_life *l);

/**
 * The teardown did not finish but can be repeated: STOPPING -> RUNNING, same
 * generation.
 *
 * [!] RETRYABLE MEANS STOPPABLE AGAIN.  Leaving it in STOPPING would turn one
 * moment of lock contention into a stream nothing can ever tear down, which is
 * the outcome the retryable answer exists to avoid.
 *
 * @return non-zero if the transition happened
 */
int nn_stream_life_retry(struct nn_stream_life *l);

/** Unconfirmed: STOPPING -> LOST, for good.
 *  @return non-zero if the transition happened */
int nn_stream_life_poison(struct nn_stream_life *l);

/** Coherent snapshot for a poll's first phase.  Any argument may be NULL. */
void nn_stream_life_snapshot(const struct nn_stream_life *l, uint32_t *gen,
                             uint8_t *phase, uint32_t *seq);

/* ---- disposition ---------------------------------------------------------
 *
 * Mirrors of enum nn_claim (svc/nn_svc.h), which cannot be included here without
 * a cycle.  nn_svc.h static-asserts them equal, so a drift is a build failure
 * rather than a table deciding about values nobody means.
 */
#define NN_STREAM_CLAIM_NONE       0
#define NN_STREAM_CLAIM_CALLER     1
#define NN_STREAM_CLAIM_RETRYABLE  2
#define NN_STREAM_CLAIM_TERMINAL   3

/** One board's stop code and what authority it leaves the caller. */
struct nn_stream_disp {
	int           rc;
	unsigned char claim;   /**< NN_STREAM_CLAIM_*  */
};

/**
 * Map a board's stop return onto cleanup authority, failing closed.
 *
 * [!] AN UNLISTED CODE IS TERMINAL, AND THAT IS THE POINT OF THE FUNCTION.  Two
 * boards previously ended their mapping with a catch-all "retryable", so any new
 * or corrupted return -- a value carrying no evidence at all about whether a
 * worker is quiescent -- told the operator to keep retrying while resources
 * might still be live.  svc/nn_svc.h states the rule this restores: a board that
 * CANNOT TELL must fail closed rather than guess.
 *
 * The table is the board's, because the codes are; the default is here, because
 * the rule is not.
 *
 * @param tab  the documented codes, in any order; @p n its length
 */
unsigned char nn_stream_disp_of(int rc, const struct nn_stream_disp *tab,
                                unsigned n);

#ifdef __cplusplus
}
#endif

#endif /* NN_STREAM_LIFE_H */
