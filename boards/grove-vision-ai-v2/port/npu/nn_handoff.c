/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_handoff.c
 * @brief   The worker hand-over table (issue #129).  See nn_handoff.h.
 */
#include <stddef.h>   /* NULL */

#include "nn_handoff.h"

int nn_handoff_settled(uint8_t state)
{
	/* [!] ENUMERATED, NOT "NOT BUSY".  An unknown value is not parked. */
	return state == (uint8_t)NN_HO_IDLE || state == (uint8_t)NN_HO_WANT;
}

int nn_handoff_step(uint8_t *state, uint8_t op)
{
	uint8_t from, want, to;

	if (state == NULL)
		return 0;
	from = *state;

	switch (op) {
	case NN_HO_OP_ARM:
		want = (uint8_t)NN_HO_IDLE;
		to   = (uint8_t)NN_HO_WANT;
		break;
	case NN_HO_OP_HAND:
		want = (uint8_t)NN_HO_WANT;
		to   = (uint8_t)NN_HO_HANDED;
		break;
	case NN_HO_OP_TAKE:
		want = (uint8_t)NN_HO_HANDED;
		to   = (uint8_t)NN_HO_RUNNING;
		break;
	case NN_HO_OP_DONE:
		want = (uint8_t)NN_HO_RUNNING;
		to   = (uint8_t)NN_HO_WANT;
		break;
	case NN_HO_OP_DONE_LAST:
		want = (uint8_t)NN_HO_RUNNING;
		to   = (uint8_t)NN_HO_IDLE;
		break;
	case NN_HO_OP_JOIN:
		/* The one operation with two sources: a parked worker, wanting or
		 * not.  A job handed over but not yet taken is NOT parked -- the
		 * worker will still wake and run it -- so it is refused like a
		 * running one. */
		if (!nn_handoff_settled(from))
			return 0;
		*state = (uint8_t)NN_HO_IDLE;
		return 1;
	default:
		return 0;
	}

	if (from != want)
		return 0;
	*state = to;
	return 1;
}
