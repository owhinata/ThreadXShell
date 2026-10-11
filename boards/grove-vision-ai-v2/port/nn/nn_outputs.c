/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_outputs.c
 * @brief   What the worker does with a model's outputs (issue #129).  See
 *          nn_outputs.h.
 */
#include <stddef.h>
#include <string.h>

#include "nn_outputs.h"

enum nn_out_plan nn_out_plan(int oneshot, int has_plugin, unsigned n_out,
                             unsigned max, unsigned *n_use)
{
	/* [!] THE PLUGIN QUESTION FIRST: a bare model is reported whatever its
	 * output count, because the limit is a decoder's and there is none. */
	if (!has_plugin)
		return NN_OUT_RAW;
	if (n_out > max) {
		/* A stream cannot annotate from a partial set and refuses the
		 * frame, as it always did; `nn run` decodes the first `max`, as
		 * it always did. */
		if (!oneshot)
			return NN_OUT_REFUSE;
		n_out = max;
	}
	if (n_use != NULL)
		*n_use = n_out;
	return NN_OUT_DECODE;
}

unsigned nn_out_collect(unsigned n, nn_out_read_fn read, void *ctx)
{
	unsigned i;

	for (i = 0u; i < n; i++)
		if (read == NULL || read(ctx, i) != 0)
			return i;
	return n;
}

void nn_out_raw_fill(struct nn_raw_outputs *raw, unsigned count,
                     nn_out_desc_fn desc, void *ctx)
{
	unsigned i;

	if (raw == NULL)
		return;
	memset(raw, 0, sizeof *raw);
	raw->count = (int32_t)count;
	for (i = 0u; i < count && i < (unsigned)NN_RAW_OUTPUTS_MAX; i++) {
		if (desc == NULL || desc(ctx, i, &raw->out[i]) != 0)
			break;
		raw->n = (uint8_t)(i + 1u);
	}
}
