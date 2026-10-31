/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_bench.c
 * @brief   The one `nn bench` procedure (issue #163); see nn_bench.h.
 */
#include "nn_bench.h"

#include <string.h>

#include "nn_svc_adapter.h" /* nn_detail_set, nn_result */

/* [!] EVERY input, not only the first: a model with several inputs measured
 * with the others left as the last reset left them is not the same work twice
 * (Grove filled input 0 only before issue #163). */
static void nn_bench_fill(struct nn_model *m)
{
	int i;

	for (i = 0; i < nn_input_count(m); i++) {
		struct nn_tensor *t = nn_input(m, i);

		if (t != NULL && t->data != NULL)
			memset(t->data, (int)NN_BENCH_FILL, t->bytes);
	}
}

void nn_bench(const struct nn_bench_ops *ops, uint32_t iters,
              struct nn_bench_stats *out, nn_svc_cancel_fn cancel, void *ctx,
              struct nn_op_result *res)
{
	struct nn_bench_clock clk;
	struct nn_model *m;
	uint32_t i;

	nn_detail_clear();
	memset(out, 0, sizeof *out);
	memset(&clk, 0, sizeof clk);
	out->min_us = 0xFFFFFFFFu;

	/* [!] ONE HOLD FROM THE FILL TO THE LAST RUN (issue #163): the model that
	 * is filled is the model that is measured.  A failed acquire holds nothing
	 * and wrote its own answer. */
	m = ops->acquire(res);
	if (m == NULL)
		return;

	nn_bench_fill(m);
	if (ops->clock_start != NULL)
		ops->clock_start(&clk);

	for (i = 0u; i < iters; i++) {
		uint32_t us;

		if (nn_svc_cancelled(cancel, ctx)) {
			nn_detail_set("cancelled after %lu of %lu run(s)",
			              (unsigned long)i, (unsigned long)iters);
			ops->release();
			nn_result(res, NN_SVC_ERR_CANCEL, NN_CLAIM_NONE);
			return;
		}
		if (ops->run_begin != NULL)
			ops->run_begin(&clk);
		if (nn_run(m) != 0) {
			nn_detail_set("inference failed on run %lu of %lu",
			              (unsigned long)i + 1u, (unsigned long)iters);
			ops->release();
			nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
			return;
		}
		us = ops->run_end_us(&clk, m);
		out->total_us += us;
		if (us < out->min_us)
			out->min_us = us;
		if (us > out->max_us)
			out->max_us = us;
		out->runs++;
	}
	ops->release();

	if (out->runs == 0u)
		out->min_us = 0u;
	else
		out->avg_us = (uint32_t)(out->total_us / out->runs);
	/* Say what the conversion assumed. */
	out->clock_mhz = (ops->clock_mhz != NULL) ? ops->clock_mhz(&clk) : 0u;
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}
