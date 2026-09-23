/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_load_end.c
 * @brief   The load's ending table.  See nn_load_end.h.
 */
#include "nn_load_end.h"

#include <string.h>

#include "nn_svc.h"   /* enum nn_model_state -- the operator's vocabulary */

int nn_load_swaps_plugin(int reload_rc, int model_after)
{
	return reload_rc == 0 && model_after;
}

void nn_load_decide(int reload_rc, int model_after, int plugin_refused,
                    struct nn_load_verdict *v)
{
	memset(v, 0, sizeof *v);

	if (!model_after) {
		/* Nothing is open: the new model was refused and there was no
		 * previous one, or even the previous one could not be rebuilt.  No
		 * decoder may survive it, no container describes it, and the last
		 * result belonged to something that is gone. */
		v->state      = (unsigned char)NN_MODEL_EMPTY;
		v->unload     = 1u;
		v->invalidate = 1u;
		v->claims     = (unsigned char)NN_LOAD_CLAIMS_NONE;
		return;
	}
	if (reload_rc != 0) {
		/* Refused and rolled back: nothing moved, so nothing here moves. */
		v->state  = (unsigned char)NN_MODEL_PREVIOUS;
		v->claims = (unsigned char)NN_LOAD_CLAIMS_KEEP;
		return;
	}
	v->state      = (unsigned char)NN_MODEL_NEW;
	v->invalidate = 1u;
	v->claims     = (unsigned char)NN_LOAD_CLAIMS_NEW;
	if (plugin_refused) {
		/* [!] NEW AND A FAILURE (issue #122 D6): the copy into the
		 * reservation already destroyed the previous plugin, so there is
		 * nothing to roll back to -- the model stays open with no decoder,
		 * and the unload is explicit whatever the loader already did. */
		v->unload = 1u;
		return;
	}
	v->ok = 1u;
}
