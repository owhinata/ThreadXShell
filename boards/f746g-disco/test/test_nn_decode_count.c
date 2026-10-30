/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's decode counting table (issue #130 step 6c, #122
 * P8, port/nn/nn_decode_count.c; one table since issue #159 Step 4c).
 *
 * WHY THIS EXISTS.  The rows that matter cannot be typed: a plugin answers
 * BF_ERR_UNINIT or BF_ERR_ARG only for a wiring fault, and each must be counted
 * as a decoder fault -- folded into nothing it reads as a healthy stream.  And
 * BF_ERR_MODEL from a plugin that passed the admission is a container whose
 * two halves disagree: model_errors, never an inference with nobody found.
 * Every row is spelled out against the real BF_ERR_* values, then the rules are
 * checked over a sweep of answers.
 */
#include <limits.h>
#include <stdio.h>

#include "blazeface.h"
#include "nn_decode_count.h"

static int fails;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			fails++;                                               \
		}                                                              \
	} while (0)

static void row(int nd, int oneshot, enum nn_decode_count want)
{
	enum nn_decode_count got = nn_decode_count_of(nd, oneshot);

	CHECK(got == want, "nd %d oneshot %d: got %d want %d", nd, oneshot,
	      (int)got, (int)want);
}

static void test_rows(void)
{
	/* A stream. */
	row(0, 0, NN_DC_NONE);               /* decoded nobody: a measurement  */
	row(5, 0, NN_DC_NONE);
	row(INT_MAX, 0, NN_DC_NONE);
	row(BF_ERR_MODEL, 0, NN_DC_MODEL);   /* the container's halves disagree */
	row(BF_ERR_UNINIT, 0, NN_DC_DECODER);
	row(BF_ERR_ARG, 0, NN_DC_DECODER);
	row(-4, 0, NN_DC_DECODER);           /* undocumented: a fault          */
	row(-64, 0, NN_DC_DECODER);
	row(INT_MIN, 0, NN_DC_DECODER);

	/* A one-shot counts nothing: `nn run` reports its own result. */
	row(0, 1, NN_DC_NONE);
	row(3, 1, NN_DC_NONE);
	row(BF_ERR_MODEL, 1, NN_DC_NONE);
	row(BF_ERR_UNINIT, 1, NN_DC_NONE);
	row(BF_ERR_ARG, 1, NN_DC_NONE);
	row(INT_MIN, 1, NN_DC_NONE);
	row(BF_ERR_UNINIT, -1, NN_DC_NONE);  /* any non-zero is a one-shot     */
	row(BF_ERR_ARG, 2, NN_DC_NONE);
}

/*
 * [!] ONLY WHAT THE RECORD TOOK IS COUNTED (issue #159 Step 4b-iii, #122 P18).
 * The case that cannot be typed on purpose: a stop that lands during the one
 * inference of a short stream drops its publish, and the stats must then say no
 * inference -- not "1 infer" beside `last : nothing decoded yet`.  Kept through
 * Step 4c: the plugin's decode and the raw publish both go through it.
 */
static void test_tally(void)
{
	static const enum nn_decode_count all[3] = { NN_DC_NONE, NN_DC_DECODER,
	                                             NN_DC_MODEL };
	unsigned i, infers = 0u;

	for (i = 0u; i < 3u; i++)
		CHECK(nn_decode_tally_of(0, all[i]) == NN_TALLY_NOTHING,
		      "a dropped publish (dc %d) was counted", (int)all[i]);
	CHECK(nn_decode_tally_of(1, NN_DC_NONE) == NN_TALLY_INFER,
	      "a taken result is not an infer");
	CHECK(nn_decode_tally_of(1, NN_DC_DECODER) == NN_TALLY_DECODER,
	      "a taken decoder refusal is not a decoder error");
	CHECK(nn_decode_tally_of(1, NN_DC_MODEL) == NN_TALLY_MODEL,
	      "a taken model mismatch is not a model error");

	/* A plugin's one publish of a stream, dropped by the stop's boundary: a
	 * face found, and nothing counted. */
	if (nn_decode_tally_of(0, nn_decode_count_of(1, 0)) == NN_TALLY_INFER)
		infers++;
	CHECK(infers == 0u, "[!] the stop-refused publish counted %u infer(s)",
	      infers);
	CHECK(nn_decode_tally_of(0, nn_decode_count_of(BF_ERR_MODEL, 0)) ==
	      NN_TALLY_NOTHING, "a dropped plugin mismatch was counted");
	/* ...and a raw publish (no plugin) the same. */
	CHECK(nn_decode_tally_of(0, NN_DC_NONE) == NN_TALLY_NOTHING,
	      "a dropped raw publish was counted");
}

/* The rules, over a sweep: no negative of a stream is dropped, BF_ERR_MODEL
 * alone is a model error, and no count is an error. */
static void test_rules(void)
{
	for (int nd = -200; nd <= 200; nd++) {
		enum nn_decode_count got = nn_decode_count_of(nd, 0);

		if (nd >= 0)
			CHECK(got == NN_DC_NONE, "%d counted as %d", nd, (int)got);
		else if (nd == BF_ERR_MODEL)
			CHECK(got == NN_DC_MODEL, "BF_ERR_MODEL got %d", (int)got);
		else
			CHECK(got == NN_DC_DECODER, "negative %d got %d", nd,
			      (int)got);
		CHECK(nn_decode_count_of(nd, 1) == NN_DC_NONE,
		      "one-shot %d counted", nd);
	}
	/* The codes the table is keyed on are distinct, or it folds them. */
	CHECK(BF_ERR_MODEL != BF_ERR_UNINIT && BF_ERR_MODEL != BF_ERR_ARG,
	      "BF_ERR_* collide");
}

int main(void)
{
	test_rows();
	test_tally();
	test_rules();
	if (fails) {
		printf("test_nn_decode_count: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_nn_decode_count: OK\n");
	return 0;
}
