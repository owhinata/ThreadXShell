/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_swap.c
 * @brief   The load's ending table.  See nn_swap.h.
 */
#include "nn_swap.h"

#include <string.h>

#include "nn_svc.h"   /* enum nn_model_state -- the operator's vocabulary */

void nn_swap_decide(int had_open, enum nn_swap_end end,
                    struct nn_swap_verdict *v)
{
	memset(v, 0, sizeof *v);

	switch (end) {
	case NN_SWAP_REFUSED:
		if (had_open) {
			/* Nothing moved.  The previous model, its plugin, its
			 * identity and its last result all stand. */
			v->state = (unsigned char)NN_MODEL_PREVIOUS;
			return;
		}
		/* This load brought the NPU up itself, and an NPU that is up with
		 * no model is a state nothing uses -- and one that would hold the
		 * flash lease against `blob write` for as long as it lasted. */
		break;
	case NN_SWAP_RESTORED:
		if (had_open) {
			v->state = (unsigned char)NN_MODEL_PREVIOUS;
			return;
		}
		break;   /* restored from nothing: fail closed, below */
	case NN_SWAP_UNDECODED:
		/* [!] NEW AND A FAILURE (issue #122 D6).  The operator asked for a
		 * model with a decoder and got the model alone; the status says so,
		 * the state says the model is the new one. */
		v->state      = (unsigned char)NN_MODEL_NEW;
		v->commit     = 1u;
		v->unload     = 1u;
		v->invalidate = 1u;
		return;
	case NN_SWAP_OPENED:
		v->state      = (unsigned char)NN_MODEL_NEW;
		v->ok         = 1u;
		v->commit     = 1u;
		v->invalidate = 1u;
		return;
	case NN_SWAP_LOST:
	default:
		break;
	}

	/* EMPTY: everything down, in unload's order.  The last result goes too --
	 * a no-op when nothing was open, and exactly right when something was. */
	v->state      = (unsigned char)NN_MODEL_EMPTY;
	v->forget     = 1u;
	v->unload     = 1u;
	v->hw_down    = 1u;
	v->invalidate = 1u;
}
