/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_refusal.h
 * @brief   What a refused `nn run` / `nn stream start` / `nn stream stop`
 *          answers, and the words `nn run` ends with, written once for every
 *          board (issue #174).
 *
 * WHY THIS EXISTS.  The status and the claim disposition of every admission
 * refusal were already the same on all three boards (issue #164), but each
 * adapter still carried its own switch with its own sentences -- nine of them,
 * which had drifted apart in their words ("only a reboot clears it" on two
 * boards, "only a reset clears it ..." on the third; "holds the NPU" for the
 * same holder another board called "the camera"; one board's `nn run` refused
 * BUSY with no explanation at all).  This is the one copy.
 *
 * INPUT.  The lifecycle's answer (nn_core_admit(), nn_core_oneshot_admit(), the
 * @p why of nn_core_oneshot_claim(), nn_core_claim_stop()), which operation
 * asked (one function each), and the board's @ref nn_core_board.  What differs
 * between boards is read from a PROPERTY that struct already has, never from a
 * board name:
 *
 *   gate_held != NULL  the board has a gate an ordinary nn command (a load, a
 *                      bench, a run) holds, so its BUSY is "another nn command
 *                      or a start or a stop", not only "a start or a stop"
 *
 * OUTPUT.  The caller's result: status, claim disposition and the sentence,
 * written straight into @ref nn_op_result::detail (nn_svc_adapter.h: there is
 * no shared diagnostic buffer).
 *
 * THE TABLE (status, claim).  Every defined answer keeps the value the three
 * boards gave it before this file existed:
 *
 *                 run              start            stop
 *   RUNNING       BUSY, NONE       STATE, NONE      -
 *   ONESHOT       BUSY, NONE       BUSY, NONE       BUSY, NONE
 *   DEAD          HW, TERMINAL     HW, TERMINAL     HW, TERMINAL
 *   BUSY          BUSY, NONE       BUSY, NONE       BUSY, NONE
 *   WRONG_GEN     -                -                GEN, NONE
 *   IDLE          -                -                STATE, NONE
 *   not in enum   BUSY, NONE       BUSY, NONE       BUSY, NONE
 *
 * [!] A VALUE OUTSIDE THE ENUM IS BUSY, NONE FOR ALL THREE OPERATIONS, and for
 * the stop that is the ONE answer this file changed (issue #174, decided with
 * the user): wio-lite-ai and f746g-disco answered an unknown stop STATE, NONE,
 * grove-vision-ai-v2 BUSY, NONE.  No caller can produce such a value, but the
 * shared table has to say one thing, and STATE is the wrong one: the operator's
 * `nn stream stop` reads STATE as "not running" and reports success, which
 * fails open over a stream nobody has established is stopped.  BUSY says
 * nothing was attempted and nothing is the caller's to release.
 *
 * [!] GO IS NOT A REFUSAL.  Each function answers 0 for it and writes nothing,
 * so a caller can say `if (nn_refusal_start(...)) return;`.
 *
 * Pure: no mutable storage, no lock, no board header, no tx_api.h.  Every input
 * is enumerated by shell/test/test_nn_refusal.c, which compares each composed
 * sentence with its expected text and with NN_SVC_DETAIL_MAX (a sentence picked
 * from a table, or composed with a number, cannot be seen by the build-time
 * check in nn_detail.h).
 */
#ifndef NN_REFUSAL_H
#define NN_REFUSAL_H

#include <stddef.h>
#include <stdint.h>

#include "nn_core.h"         /* struct nn_core_board                     */
#include "nn_detail.h"       /* NN_SVC_DETAIL_MAX                        */
#include "nn_run_wait.h"     /* enum nn_run_end                          */
#include "nn_stream_life.h"  /* enum nn_stream_start_claim / _stop_claim */
#include "nn_svc.h"          /* struct nn_op_result, enum nn_claim       */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * `nn run` was not admitted: @p why from nn_core_oneshot_admit(), or from
 * nn_core_oneshot_claim() when it returned NN_STREAM_GEN_ANY (a commit that
 * claim refused is reported BUSY by it, and is answered here as BUSY).
 *
 * @return 0 for NN_STREAM_START_GO (nothing written), else non-zero with @p res
 *         filled
 */
int nn_refusal_run(enum nn_stream_start_claim why,
                   const struct nn_core_board *b, struct nn_op_result *res);

/** `nn stream start` was not admitted (nn_core_admit()).  Same return. */
int nn_refusal_start(enum nn_stream_start_claim why,
                     const struct nn_core_board *b, struct nn_op_result *res);

/** `nn stream stop` (or a waiter's stop by generation) was not claimed
 *  (nn_core_claim_stop()).  0 for NN_STREAM_STOP_GO, else non-zero. */
int nn_refusal_stop(enum nn_stream_stop_claim why,
                    const struct nn_core_board *b, struct nn_op_result *res);

/*
 * ---- how `nn run` ended without a result: the base sentences -------------
 *
 * LOST, CANCELLED and TIMEOUT read the same on every board; what follows them
 * -- what the board's teardown did not release -- is a hardware fact and stays
 * the board's (wio-lite-ai's band stream and PSRAM guard, f746g-disco's DCMI
 * frame, grove-vision-ai-v2's stop verdict).  The board passes that clause as
 * @p unfinished, and it is appended only when @p claim is not NN_CLAIM_NONE --
 * the one condition every board appended it on.
 *
 *   LOST       NN_SVC_ERR_HW       "the camera stream was lost before an
 *                                   inference completed"
 *   CANCELLED  NN_SVC_ERR_CANCEL   "cancelled before an inference completed"
 *   TIMEOUT    NN_SVC_ERR_TIMEOUT  "no inference completed within <ms> ms"
 *   any other  as TIMEOUT          (the boards' switches ended `case TIMEOUT:
 *                                   default:`; INFERRED and NO_RESULT are the
 *                                   caller's to answer before this)
 */

/**
 * The longest base sentence above, in characters without the terminator, with
 * the number at its widest (a uint32_t's ten digits).  A board's @p unfinished
 * is checked against it at build time by NN_REFUSAL_RUN_UNFINISHED().
 *
 * [!] HELD TO THE SENTENCES BY THE HOST TEST: every composed base sentence must
 * be no longer than this, and the longest must be EXACTLY this.  A constant
 * left behind when a sentence grew would let a board's check pass over a
 * result that truncates.
 */
#define NN_REFUSAL_RUN_END_MAX 56u

/**
 * A board's clause after a run's base sentence, as a checked literal: the
 * build fails if the longest base sentence plus @p s (its own separator
 * included, e.g. "; the teardown did not finish either") would not fit in
 * NN_SVC_DETAIL_MAX characters -- the field's terminator is the + 1u.  `"" s`
 * refuses anything that is not a literal.
 */
#ifndef __cplusplus
#define NN_REFUSAL_RUN_UNFINISHED(s)                                           \
	(0u * sizeof(struct {                                                  \
		_Static_assert((size_t)NN_REFUSAL_RUN_END_MAX + sizeof("" s) <= \
		               (size_t)NN_SVC_DETAIL_MAX + 1u,                 \
		               "nn run clause does not fit after the longest " \
		               "base sentence");                               \
		char nn_refusal_check_;                                        \
	}) + ("" s))
#endif

/**
 * Fill @p res for a run that ended @p end: its status, @p claim (the board's
 * teardown disposition, given back unchanged) and the base sentence, followed
 * by @p unfinished when @p claim is not NN_CLAIM_NONE.
 *
 * @param wait_ms     the wait the run gave up after, in milliseconds (TIMEOUT)
 * @param unfinished  the board's clause, NULL for none; wrap a literal in
 *                    NN_REFUSAL_RUN_UNFINISHED()
 * @return the status written
 */
int nn_refusal_run_end(enum nn_run_end end, uint32_t wait_ms,
                       enum nn_claim claim, const char *unfinished,
                       struct nn_op_result *res);

#ifdef __cplusplus
}
#endif

#endif /* NN_REFUSAL_H */
