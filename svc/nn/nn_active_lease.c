/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active_lease.c
 * @brief   The lease-checked entries into the plugin's decoder slots.  See
 *          nn_active_lease.h.
 *
 * Moved out of wio-lite-ai's port/nn/nn_active.c (issue #159) so that
 * f746g-disco takes the same entries instead of a third copy of them.  The
 * board's facts arrive through nn_active_facts; the lease through
 * svc/plugin/plugin_lease_api.h, which the board implements.
 *
 * No mutable storage.
 */
#include "nn_active_lease.h"

#include "plugin_lease_api.h"   /* plugin_lease_held() -- the entry check (#130) */

#include <string.h>

/*
 * Does the calling thread hold the plugin lease?  Counted when it does not, here
 * and only here, so every refusal is counted exactly once whoever forgot.
 */
static int nn_active_held(void)
{
	if (plugin_lease_held())
		return 1;
	plugin_lease_note_unheld();
	return 0;
}

/*
 * The outputs reach a plugin as svc/nn/tensor.h descriptors, the contract issue
 * #97 established so that one decoder can read any board's tensors.  The
 * translation of one tensor is the board's (output_desc); a second translation
 * here could disagree with `nn out` and `nn info` about what a tensor is.
 *
 * A hole in the output set becomes a zeroed descriptor -- UNSUPPORTED -- rather
 * than a refusal here: it is not a model-shape problem, and letting the decoder
 * decide whether the tensors it wants are still there reaches the same answer by
 * a defensible route.
 */
static unsigned to_desc(struct nn_model *m, struct tensor_desc *d, unsigned cap)
{
	const struct nn_active_lease_board *b = &nn_active_facts;
	int n = b->output_count(m);
	unsigned i, out;

	if (n < 0)
		n = 0;
	out = (unsigned)n;
	if (out > cap)
		out = cap;
	for (i = 0u; i < out; i++) {
		if (b->output_desc(m, i, &d[i]) != 0)
			memset(&d[i], 0, sizeof d[i]);
	}
	return out;
}

int nn_active_is_plugin(void)
{
	return nn_active_core_is_plugin(nn_active_facts.core);
}

int nn_active_shapes_ok(struct nn_model *m)
{
	struct tensor_desc d[NN_ACTIVE_MAX_OUTPUTS];
	unsigned n;

	if (m == NULL)
		return 0;
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	n = to_desc(m, d, NN_ACTIVE_MAX_OUTPUTS);
	return nn_active_core_shapes_ok(nn_active_facts.core, d, n);
}

int nn_active_decode(struct nn_model *m)
{
	struct tensor_desc d[NN_ACTIVE_MAX_OUTPUTS];
	unsigned n;

	if (m == NULL)
		return BF_ERR_ARG;
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	n = to_desc(m, d, NN_ACTIVE_MAX_OUTPUTS);
	return nn_active_core_decode(nn_active_facts.core, d, n);
}

int nn_active_draw(const struct plugin_painter *paint)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	nn_active_core_draw(nn_active_facts.core, paint);
	return 0;
}

/* can_draw and can_report read the plugin's slot table rather than call into
 * it, and are checked anyway: what they answer is about the plugin a holder is
 * about to call, and a load replacing it between the question and the call is
 * exactly what the lease excludes. */
int nn_active_can_draw(void)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	return nn_active_core_can_draw(nn_active_facts.core);
}

int nn_active_can_report(void)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	return nn_active_core_can_report(nn_active_facts.core);
}

int nn_active_report(nn_svc_write_fn write, void *ctx)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	return nn_active_core_report(nn_active_facts.core, write, ctx);
}

int nn_active_get_thresh_milli(unsigned *milli)
{
	if (milli == NULL)
		return NN_ACTIVE_THRESH_REFUSED;
	*milli = NN_SVC_THRESH_NONE;
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	*milli = nn_active_core_get_thresh_milli(nn_active_facts.core);
	return NN_ACTIVE_THRESH_OK;
}

int nn_active_set_thresh_milli(unsigned milli)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	return nn_active_core_set_thresh_milli(nn_active_facts.core, milli);
}
