/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for what the inference worker does with a model's outputs (issue
 * #129, port/npu/nn_outputs.c).
 *
 * WHY THIS EXISTS.  When `nn run` moved onto the worker, the outputs were read
 * once, before the plugin question, with the stream's rule -- so a bare model
 * with more outputs than the decoder limit, or one unreadable output, ended
 * `nn run` with nothing, where it used to report every output it could
 * (review of a438f76).  Every model this board ships has fewer than eight
 * outputs and reads cleanly, so none of it can be seen on the board.  The cases
 * below are the three callers' contracts, and the nine-output model and the
 * read that fails half way are made up here.
 */
#include <stdio.h>
#include <string.h>

#include "nn_outputs.h"

static int fails;

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		fails++;
}

/* A model whose output `fail_at` cannot be read (or none, when past the end). */
struct fake {
	unsigned fail_at;
	unsigned reads;
};

static int fake_read(void *ctx, unsigned i)
{
	struct fake *f = ctx;

	f->reads++;
	return (i == f->fail_at) ? -1 : 0;
}

static int fake_desc(void *ctx, unsigned i, struct tensor_desc *out)
{
	struct fake *f = ctx;

	f->reads++;
	if (i == f->fail_at)
		return -1;
	out->rank = 1;
	out->dims[0] = (int32_t)(100u + i);   /* which output it was */
	return 0;
}

int main(void)
{
	unsigned n;
	enum nn_out_plan p;

	printf("nn_out_plan\n");
	n = 0xEEu;
	p = nn_out_plan(1, 0, 9u, 8u, &n);
	expect("[!] `nn run`, no plugin, nine outputs: reported raw, not refused",
	       p == NN_OUT_RAW);
	p = nn_out_plan(0, 0, 9u, 8u, &n);
	expect("a stream with no plugin is raw too (admission refuses it first)",
	       p == NN_OUT_RAW);
	n = 0xEEu;
	p = nn_out_plan(1, 1, 9u, 8u, &n);
	expect("[!] `nn run` with a plugin, nine outputs: the first eight decoded",
	       p == NN_OUT_DECODE && n == 8u);
	n = 0xEEu;
	p = nn_out_plan(0, 1, 9u, 8u, &n);
	expect("a stream with a plugin, nine outputs: refused, as it always was",
	       p == NN_OUT_REFUSE && n == 0xEEu);
	n = 0xEEu;
	p = nn_out_plan(0, 1, 8u, 8u, &n);
	expect("a stream at the limit decodes all of them",
	       p == NN_OUT_DECODE && n == 8u);
	n = 0xEEu;
	p = nn_out_plan(1, 1, 3u, 8u, &n);
	expect("`nn run` under the limit decodes all of them",
	       p == NN_OUT_DECODE && n == 3u);
	p = nn_out_plan(1, 1, 0u, 8u, &n);
	expect("no outputs at all is still a decode of none",
	       p == NN_OUT_DECODE && n == 0u);

	printf("nn_out_collect\n");
	{
		struct fake f = { 99u, 0u };

		expect("every read succeeds: the count comes back",
		       nn_out_collect(8u, fake_read, &f) == 8u && f.reads == 8u);
	}
	{
		struct fake f = { 3u, 0u };

		expect("[!] a read that fails is named by its index, and nothing after "
		       "it is read", nn_out_collect(8u, fake_read, &f) == 3u &&
		       f.reads == 4u);
	}
	{
		struct fake f = { 0u, 0u };

		expect("the first one failing is index 0, not success",
		       nn_out_collect(8u, fake_read, &f) == 0u);
	}

	printf("nn_out_raw_fill\n");
	{
		struct nn_raw_outputs raw;
		struct fake f = { 99u, 0u };

		memset(&raw, 0xA5, sizeof raw);
		nn_out_raw_fill(&raw, 9u, fake_desc, &f);
		expect("[!] nine outputs: count 9, eight described",
		       raw.count == 9 && raw.n == 8u &&
		       raw.out[0].dims[0] == 100 && raw.out[7].dims[0] == 107 &&
		       f.reads == 8u);
	}
	{
		struct nn_raw_outputs raw;
		struct fake f = { 2u, 0u };

		memset(&raw, 0xA5, sizeof raw);
		nn_out_raw_fill(&raw, 5u, fake_desc, &f);
		expect("[!] a read that fails at 2: count 5, the two before it kept, "
		       "the rest cleared",
		       raw.count == 5 && raw.n == 2u && raw.out[1].dims[0] == 101 &&
		       raw.out[2].rank == 0 && raw.out[2].dims[0] == 0);
	}
	{
		struct nn_raw_outputs raw;
		struct fake f = { 99u, 0u };

		nn_out_raw_fill(&raw, 0u, fake_desc, &f);
		expect("no outputs: count 0, nothing described, nothing read",
		       raw.count == 0 && raw.n == 0u && f.reads == 0u);
	}

	if (fails) {
		printf("FAILED (%d)\n", fails);
		return 1;
	}
	printf("test_nn_outputs: all cases pass\n");
	return 0;
}
