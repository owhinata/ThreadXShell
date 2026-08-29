/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_stream_life.c
 * @brief   The one live-inference lifecycle (issue #99).  See the header for why
 *          it is shared, and for the rule that every call is made under the
 *          caller's critical section.
 *
 * This file owns no storage: the state is the board's.
 */
#include "nn_stream_life.h"

#include <stddef.h>   /* NULL */

enum nn_stream_start_claim nn_stream_life_begin(struct nn_stream_life *l)
{
	if (l == NULL)
		return NN_STREAM_START_BUSY;
	/*
	 * [!] ONLY FROM IDLE, enumerated rather than excluded.  "Not running" would
	 * admit LOST -- a lifecycle whose teardown was never confirmed, where
	 * something may still be inside what a new start would rebuild.
	 *
	 * And the refusals are told apart, because a caller that folds them prints
	 * "a start or a stop is already in progress" at somebody whose only mistake
	 * was starting a second stream.
	 */
	switch ((enum nn_stream_phase)l->phase) {
	case NN_STREAM_PHASE_IDLE:
		break;
	case NN_STREAM_PHASE_RUNNING:
		return NN_STREAM_START_RUNNING;
	case NN_STREAM_PHASE_LOST:
		return NN_STREAM_START_DEAD;
	case NN_STREAM_PHASE_STARTING:
	case NN_STREAM_PHASE_STOPPING:
	default:
		return NN_STREAM_START_BUSY;
	}
	l->prev  = l->phase;
	l->phase = (uint8_t)NN_STREAM_PHASE_STARTING;
	l->seq++;
	return NN_STREAM_START_GO;
}

int nn_stream_life_rearm(struct nn_stream_life *l)
{
	if (l == NULL)
		return 0;
	/* [!] ONLY FROM RUNNING.  Not from STOPPING -- a teardown owns the stream
	 * and a re-arm that pushed through would destroy its claim -- and not from
	 * LOST, which is unrecoverable by construction. */
	if (l->phase != (uint8_t)NN_STREAM_PHASE_RUNNING)
		return 0;
	l->prev  = l->phase;
	l->phase = (uint8_t)NN_STREAM_PHASE_STARTING;
	l->seq++;
	return 1;
}

uint32_t nn_stream_life_commit(struct nn_stream_life *l)
{
	if (l == NULL)
		return NN_STREAM_GEN_ANY;
	/* [!] Refuse rather than force -- see the header.  Nothing is changed, and
	 * the caller is left to undo the hardware it brought up. */
	if (l->phase != (uint8_t)NN_STREAM_PHASE_STARTING)
		return NN_STREAM_GEN_ANY;

	if (l->next == NN_STREAM_GEN_ANY)
		l->next = 1u;              /* first ever, and the wrap lands here too */
	l->gen = l->next++;
	/* [!] Skip the reserved value on the way out as well, so the NEXT mint
	 * cannot produce it either. */
	if (l->next == NN_STREAM_GEN_ANY)
		l->next = 1u;
	l->phase = (uint8_t)NN_STREAM_PHASE_RUNNING;
	l->seq++;
	return l->gen;
}

int nn_stream_life_abort(struct nn_stream_life *l)
{
	if (l == NULL)
		return 0;
	if (l->phase != (uint8_t)NN_STREAM_PHASE_STARTING)
		return 0;               /* not ours to unwind */
	/* Nothing came up, so nothing was minted: the previous generation stays
	   whatever it was, and the phase goes back WHERE IT CAME FROM -- a re-arm
	   that failed leaves the stream it was re-arming still running. */
	l->phase = l->prev;
	l->seq++;
	return 1;
}

enum nn_stream_stop_claim nn_stream_life_claim_stop(struct nn_stream_life *l,
                                                    uint32_t gen)
{
	if (l == NULL)
		return NN_STREAM_STOP_IDLE;

	switch ((enum nn_stream_phase)l->phase) {
	case NN_STREAM_PHASE_IDLE:
		return NN_STREAM_STOP_IDLE;
	case NN_STREAM_PHASE_LOST:
		return NN_STREAM_STOP_DEAD;
	case NN_STREAM_PHASE_RUNNING:
		break;
	case NN_STREAM_PHASE_STARTING:
	case NN_STREAM_PHASE_STOPPING:
	default:
		/* Somebody else owns the transition.  Fail closed on an unknown phase
		   too: a value this does not recognise is not permission. */
		return NN_STREAM_STOP_BUSY;
	}

	switch (nn_stream_gen_check(l->gen, gen)) {
	case NN_STREAM_GEN_GO:
		break;
	case NN_STREAM_GEN_WRONG:
		return NN_STREAM_STOP_WRONG_GEN;
	case NN_STREAM_GEN_IDLE:
	default:
		/* RUNNING with no generation cannot be reached by the transitions
		   above; refuse rather than tear down on a reading that makes no
		   sense. */
		return NN_STREAM_STOP_BUSY;
	}

	/* [!] CLAIMED HERE, in the same call that compared the generation.  From
	 * this point no other stop is admitted and no start can begin, which is
	 * what stops two callers from both running a teardown for one stream. */
	l->phase = (uint8_t)NN_STREAM_PHASE_STOPPING;
	l->seq++;
	return NN_STREAM_STOP_GO;
}

int nn_stream_life_finish(struct nn_stream_life *l)
{
	if (l == NULL)
		return 0;
	/* [!] ONLY THE CALLER THAT CLAIMED THE TEARDOWN MAY SETTLE IT.  Without
	 * this the machine is back to trusting adapter discipline -- the very
	 * criticism that made it a shared module rather than a shadow recorder. */
	if (l->phase != (uint8_t)NN_STREAM_PHASE_STOPPING)
		return 0;
	/* [!] The generation is NOT cleared.  A waiter whose stream simply ended
	 * must be able to tell that from "a successor is running", and those are
	 * opposite things to tell an operator. */
	l->phase = (uint8_t)NN_STREAM_PHASE_IDLE;
	l->seq++;
	return 1;
}

int nn_stream_life_retry(struct nn_stream_life *l)
{
	if (l == NULL)
		return 0;
	/* [!] ONLY THE CALLER THAT CLAIMED THE TEARDOWN MAY SETTLE IT.  Without
	 * this the machine is back to trusting adapter discipline -- the very
	 * criticism that made it a shared module rather than a shadow recorder. */
	if (l->phase != (uint8_t)NN_STREAM_PHASE_STOPPING)
		return 0;
	l->phase = (uint8_t)NN_STREAM_PHASE_RUNNING;
	l->seq++;
	return 1;
}

int nn_stream_life_poison(struct nn_stream_life *l)
{
	if (l == NULL)
		return 0;
	/* [!] ONLY THE CALLER THAT CLAIMED THE TEARDOWN MAY SETTLE IT.  Without
	 * this the machine is back to trusting adapter discipline -- the very
	 * criticism that made it a shared module rather than a shadow recorder. */
	if (l->phase != (uint8_t)NN_STREAM_PHASE_STOPPING)
		return 0;
	l->phase = (uint8_t)NN_STREAM_PHASE_LOST;
	l->seq++;
	return 1;
}

void nn_stream_life_snapshot(const struct nn_stream_life *l, uint32_t *gen,
                             uint8_t *phase, uint32_t *seq)
{
	if (l == NULL)
		return;
	if (gen != NULL)
		*gen = l->gen;
	if (phase != NULL)
		*phase = l->phase;
	if (seq != NULL)
		*seq = l->seq;
}

unsigned char nn_stream_disp_of(int rc, const struct nn_stream_disp *tab,
                                unsigned n)
{
	unsigned i;

	if (tab != NULL) {
		for (i = 0u; i < n; i++) {
			if (tab[i].rc == rc)
				return tab[i].claim;
		}
	}
	/*
	 * [!] FAIL CLOSED.  An unlisted code says nothing about whether anything is
	 * quiescent, and "retryable" would send an operator round a loop for ever
	 * while a worker might still be live.  A board that wants a code treated as
	 * recoverable has to say so.
	 */
	return (unsigned char)NN_STREAM_CLAIM_TERMINAL;
}
