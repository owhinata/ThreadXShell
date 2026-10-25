/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_core.h
 * @brief   The policy around the shared stream lifecycle, and the stream's
 *          numbers, written once for every board (issue #130).
 *
 * WHY THIS EXISTS.  svc/nn_stream_life.c is the one machine (issue #99), but
 * each board still wrapped it in its own copy of the same policy: admit under a
 * critical section, mint a generation and latch the stream's baselines with it,
 * claim a stop by generation, settle a teardown by its disposition and freeze
 * the clock and the final numbers only if the settle took, and answer a poll in
 * two phases checked by the transition counter.  Three copies of that drifted
 * the way issue #99's three copies of the generation rule did.  This is the one
 * copy; what genuinely differs between boards is a PROPERTY in
 * @ref nn_core_board, never a board name.
 *
 * WHAT STAYS WITH THE BOARD.  How a start comes up and what it asks before it
 * does (the admission questions are deliberately different), how and in what
 * order a stop tears down and where its record boundary falls, the words every
 * refusal is printed with, and the board's own lines.  This file sees none of
 * that: it is called before and after.
 *
 * [!] IT OWNS NO STORAGE.  The state is the board's @ref nn_core, declared in
 * the adapter; the properties are the board's const @ref nn_core_board.  The
 * derived audit (cmake/shared_storage_gate.cmake) compiles this file like every
 * other svc/ TU and fails the build if it ever grows a mutable byte.
 *
 * [!] IT INCLUDES NO BOARD HEADER AND NO tx_api.h.  A critical section, the
 * clock, the board's counters and its record are reached through hooks.  Every
 * hook that takes a lock of its own (the counters, the record) is called with
 * the critical section NOT held -- waiting for a mutex with interrupts disabled
 * is a deadlock, not a slow path.
 *
 * [!] NOTHING HERE IS BELOW A PLUGIN CALLBACK.  These run on the console, before
 * and after the board's own start and stop; no hook enters a plugin.  Keep it so:
 * the stack below a veneer is derived from the shipped image, and this file
 * must never appear on that path.
 */
#ifndef NN_CORE_H
#define NN_CORE_H

#include <stdint.h>

#include "nn_det_record.h"   /* struct nn_det_snapshot, nn_det_last_valid() */
#include "nn_stream_life.h"
#include "nn_svc.h"          /* struct nn_stream_stats, enum nn_claim       */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The board's counters, as one sample.
 *
 * [!] EACH IS MONOTONIC FOR THE WHOLE OF ONE LOGICAL STREAM, and that is the
 * board's promise, not this file's.  A counter that went back to zero part way
 * through a stream -- on a re-attach, say -- would make every later poll of that
 * stream wrong.  The fields named in @ref nn_core_board::based are latched by
 * the commit as the stream's base and reported as the difference; the others
 * the board zeroes itself at its start and they are reported as they are.
 */
struct nn_core_raw {
	/** Frames OFFERED to inference: `skipped` must be a subset of it. */
	uint32_t offered;
	uint32_t skipped;
	uint32_t infers;
	uint32_t errors;
	uint32_t model_errors;
	uint32_t decoder_errors;
	uint32_t last_us;      /**< the most recent inference; never subtracted */
	/** The board's own "the producer is up".  A board that has no such fact
	 *  says 1, and then `running` follows the lifecycle alone. */
	uint8_t  producing;
};

/** Which counters a commit latches a base for (@ref nn_core_board::based). */
#define NN_CORE_BASED_OFFERED         0x01u
#define NN_CORE_BASED_SKIPPED         0x02u
#define NN_CORE_BASED_INFERS          0x04u
#define NN_CORE_BASED_ERRORS          0x08u
#define NN_CORE_BASED_MODEL_ERRORS    0x10u
#define NN_CORE_BASED_DECODER_ERRORS  0x20u
#define NN_CORE_BASED_ALL             0x3fu

/** The baselines a commit latches, one per field of nn_core_raw; zero for a
 *  field the board does not base. */
struct nn_core_base {
	uint32_t offered;
	uint32_t skipped;
	uint32_t infers;
	uint32_t errors;
	uint32_t model_errors;
	uint32_t decoder_errors;
};

/** A stopping stream's numbers, and the record epoch its `last` is good for. */
struct nn_core_final {
	struct nn_stream_stats stats;
	/** The record's epoch when `last` was taken: a model change since then
	 *  took that result away, and a latched answer follows it (issue #118). */
	uint32_t epoch;
};

/**
 * One board's live-inference state.  Zero-initialised static storage in the
 * adapter; this file never declares one.
 */
struct nn_core {
	struct nn_stream_life life;
	uint32_t t0;     /**< the clock when the current stream was committed */
	uint32_t ms;     /**< its elapsed time, frozen once it has ended      */
	/**
	 * [!] THE RECORD'S ACCEPTED COUNT WHEN THIS STREAM WAS COMMITTED (issue
	 * #118).  The last result outlives a stop, so a new stream opens with the
	 * previous one's result still in the record; `last` is this stream's only
	 * once the record has accepted a publish since this base.
	 */
	uint32_t acc0;
	/**
	 * [!] THE BOARD'S COUNTERS WHEN THIS STREAM WAS COMMITTED.  The stats are
	 * defined per GENERATION, and a board that keeps its counters running
	 * across a re-arm (or simply never resets them) would otherwise satisfy a
	 * `--frames` waiter at once with numbers from before it started.
	 */
	struct nn_core_base base;
	/**
	 * [!] A STREAM'S NUMBERS ARE LATCHED WHEN IT ENDS (issue #120).  A `nn run`
	 * drives the same counters and the same record, so a poll that kept
	 * deriving them after the stream ended reported the last `nn run` as
	 * though it were the stream.  A poll of this generation reads them here.
	 */
	struct nn_core_final final;
	uint32_t final_gen;   /**< whose they are; ANY = nobody's */
};

/**
 * What one board is.  Const, in the board's .rodata; every hook is the board's.
 *
 * [!] PROPERTIES, NOT BOARD NAMES.  @ref rearm is "this board can re-arm a
 * stream whose capture died under it", not "this is wio".
 */
struct nn_core_board {
	/** The board's critical section: returns the posture to restore. */
	unsigned (*cs_enter)(void);
	void     (*cs_exit)(unsigned posture);
	/** The wall clock, in ticks, and its rate. */
	uint32_t (*ticks)(void);
	uint32_t ticks_per_s;
	/**
	 * Sample the board's counters.  Called OUTSIDE the critical section (it
	 * may take the board's own locks).  @p keep is passed through from
	 * nn_core_take_final() so a board that latches more than the stats can
	 * keep the very sample the stats were computed from; NULL otherwise.
	 */
	void (*counts)(struct nn_core_raw *raw, void *keep);
	/** Snapshot the board's decode record.  Called OUTSIDE the critical
	 *  section (it may take the record's lock). */
	void (*record)(struct nn_det_snapshot *snap);
	/**
	 * A transient claim decided TOGETHER with the lifecycle, in the same
	 * critical section: a start that took one and then found the other busy
	 * would have to unwind a claim another job may have taken in between.
	 * All three are called under the critical section.  NULL on a board whose
	 * claim is taken elsewhere (by its own start, under its own lock).
	 */
	int  (*gate_held)(void);
	void (*gate_take)(int oneshot);   /**< after a start was admitted     */
	void (*gate_give)(void);          /**< after an abort or finish took  */
	/**
	 * A board that latches more about an ending stream than its stats copies
	 * it here, under the same critical section as the settle that took, so
	 * the two can never describe different generations.  NULL if none.
	 */
	void (*latch_extra)(const void *extra);
	/**
	 * NN_CORE_BASED_* of the counters that run ACROSS the board's own start
	 * (cumulative, or from boot), and so need a base latched by the commit.
	 * A counter the board zeroes at its start, before the record's base is
	 * read, is left out and reported as it is.
	 *
	 * [!] A BASED COUNTER IS SAMPLED BEFORE THE RECORD'S ACCEPTED COUNT, and
	 * that order is only safe for a publisher that publishes to the record
	 * BEFORE it counts.  A publish landing between the two samples is then
	 * counted and not this stream's result -- never this stream's result and
	 * not counted, which would leave `infers` one short for the whole
	 * generation.  A board whose worker counts first cannot base that
	 * counter at the commit at all (no order of the two samples is safe for
	 * it); it zeroes it at its start instead.
	 */
	uint8_t  based;
	/** This board can re-arm a running stream (its capture can die under it
	 *  while the worker keeps its guards): a start from RUNNING is a re-arm. */
	uint8_t  rearm;
	/** The stream's clock runs only while the board says its producer is up
	 *  (@ref nn_core_raw::producing); otherwise it follows the lifecycle. */
	uint8_t  clock_needs_producer;
};

/**
 * Why the transient claim is held, for a start that found it held: read from
 * the lifecycle, so the refusal names the holder (issue #122).  Pure; called
 * under the critical section by the admissions below.
 */
enum nn_stream_start_claim nn_core_gate_refusal(const struct nn_stream_life *l);

/**
 * Admit a `nn stream start`, BEFORE the board touches its worker.
 *
 * Refused while the board's gate is held (named by nn_core_gate_refusal());
 * otherwise IDLE -> STARTING, or on a @ref nn_core_board::rearm board RUNNING
 * -> STARTING.  The gate is taken in the same critical section when it was
 * admitted.  The caller then commits or aborts.
 */
enum nn_stream_start_claim nn_core_admit(struct nn_core *c,
                                         const struct nn_core_board *b);

/** `nn run`'s admission: a one-shot, never re-armed (issue #120).  The caller
 *  then commits with nn_core_oneshot_commit() or aborts. */
enum nn_stream_start_claim nn_core_oneshot_admit(struct nn_core *c,
                                                 const struct nn_core_board *b);

/** ...and its commit.  [!] NO BASELINES: those describe the last STREAM, which
 *  `nn stream stats` keeps reporting.
 *  @return the generation, or NN_STREAM_GEN_ANY if refused */
uint32_t nn_core_oneshot_commit(struct nn_core *c,
                                const struct nn_core_board *b);

/**
 * `nn run` admitted AND committed in one critical section, for a board whose
 * one-shot starts its hardware only after the commit.  A refused commit is
 * aborted and reported BUSY.
 *
 * @return the generation, or NN_STREAM_GEN_ANY with nothing held; @p why says
 *         which refusal
 */
uint32_t nn_core_oneshot_claim(struct nn_core *c, const struct nn_core_board *b,
                               enum nn_stream_start_claim *why);

/** A start that failed after its admission: back to where it was claimed from,
 *  and the gate given back if the transition happened.
 *  @return non-zero if it happened */
int nn_core_abort(struct nn_core *c, const struct nn_core_board *b);

/**
 * Everything came up: mint the stream's generation, and latch its baselines
 * WITH it.  Outside the critical section, in this order: the board's based
 * counters (@ref nn_core_board::based; not sampled at all if there are none),
 * THEN the record's accepted count -- read from the record here when @p acc0 is
 * NULL, or the value the board took at its own record boundary.
 *
 * [!] Published only once the generation exists, so a refused commit cannot
 * leave this generation's numbers describing another's.
 *
 * @return the generation, or NN_STREAM_GEN_ANY if refused -- nothing changed,
 *         and the board fails closed
 */
uint32_t nn_core_commit(struct nn_core *c, const struct nn_core_board *b,
                        const uint32_t *acc0);

/** Test the generation and claim the stop, in ONE critical section -- see
 *  nn_stream_life_claim_stop() for why the two halves are one call. */
enum nn_stream_stop_claim nn_core_claim_stop(struct nn_core *c,
                                             const struct nn_core_board *b,
                                             uint32_t gen);

/**
 * A one-shot that ended with nothing started: claim its stop by its own
 * generation, settle it and give the gate back, in ONE critical section.
 * Refused -- an invariant failure -- nothing changes and the gate stays held.
 * @return non-zero if it settled
 */
int nn_core_oneshot_end(struct nn_core *c, const struct nn_core_board *b,
                        uint32_t gen);

/**
 * A stopping stream's final numbers, taken after the board's teardown and
 * before the settle.  @p keep is passed to the board's counts hook.
 */
void nn_core_take_final(struct nn_core *c, const struct nn_core_board *b,
                        struct nn_core_final *f, void *keep);

/**
 * Settle a claimed teardown by its disposition -- the one place every stop
 * (`nn stream stop`, and `nn run`'s own) does it.
 *
 *   RETRYABLE  -> stoppable again, same generation
 *   TERMINAL   -> LOST for good; the gate is never given back
 *   otherwise  -> IDLE, the generation kept; the gate given back
 *
 * [!] The elapsed freeze and the latch ride on the transition, not beside it:
 * a settle that was refused must not date, or describe, a generation that is
 * still running.  And only a STREAM's end is dated and latched -- a one-shot
 * ending must not overwrite the last stream's numbers.
 *
 * @param f      the stream's final numbers, or NULL (a one-shot's settle)
 * @param extra  passed to @ref nn_core_board::latch_extra when they latch
 */
void nn_core_settle(struct nn_core *c, const struct nn_core_board *b,
                    enum nn_claim claim, const struct nn_core_final *f,
                    const void *extra);

/**
 * `nn stream stats`: two phases checked by the transition counter.
 *
 * Phase 1, under the critical section: the generation, the phase, the counter
 * and the baselines -- or, for an ended stream, its latch.  Phase 2, outside
 * it: the board's counters and its record.  Accepted only if the counter did
 * not move: a retryable stop returns to the same phase and generation, so
 * those two alone cannot reject a reading that straddled one.
 *
 * @return NN_SVC_OK, NN_SVC_ERR_STATE (nothing has ever streamed),
 *         NN_SVC_ERR_GEN (@p gen is not the current one), NN_SVC_ERR_STALE
 *         (sample again) or NN_SVC_ERR_ARG
 */
int nn_core_poll(struct nn_core *c, const struct nn_core_board *b, uint32_t gen,
                 struct nn_stream_stats *out);

/** Whether the last stream has ended and been latched.  Call under the board's
 *  critical section, together with whatever else it reads of that latch. */
int nn_core_ended(const struct nn_core *c);

/* ---- the arithmetic, pure ------------------------------------------------ */

/** How far a counter has moved since its base, modulo 2^32: a counter that
 *  runs from boot wraps, and the difference across the wrap is still right. */
static inline uint32_t nn_core_since(uint32_t now, uint32_t base)
{
	return now - base;
}

/** The per-generation counts from one sample and the latched base -- the one
 *  computation a poll and a stop's latch share. */
void nn_core_counts(const struct nn_core_raw *raw,
                    const struct nn_core_base *base,
                    struct nn_stream_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_CORE_H */
