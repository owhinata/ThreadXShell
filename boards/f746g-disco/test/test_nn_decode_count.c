/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's decode counting table (issue #130 step 6c, #122
 * P8, port/nn/nn_decode_count.c).
 *
 * WHY THIS EXISTS.  The rows that matter cannot be typed: the resident decoder
 * answers BF_ERR_UNINIT only if its init failed at boot, and BF_ERR_ARG only for
 * a wiring fault in this file's caller.  Each must be counted as a decoder
 * fault -- folded into nothing it reads as a healthy stream.  And BF_ERR_MODEL
 * must NOT be counted (D6): on this board it is the top-5 result of every
 * classifier stream, so counting it would make every such stream all errors.
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
	row(1, 0, NN_DC_NONE);
	row(INT_MAX, 0, NN_DC_NONE);
	row(BF_ERR_MODEL, 0, NN_DC_NONE);    /* the top-5 result (D6)          */
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

/* The rules, over a sweep: no negative of a stream but BF_ERR_MODEL is
 * dropped, and no count is an error. */
static void test_rules(void)
{
	for (int nd = -200; nd <= 200; nd++) {
		enum nn_decode_count got = nn_decode_count_of(nd, 0);

		if (nd >= 0 || nd == BF_ERR_MODEL)
			CHECK(got == NN_DC_NONE, "%d counted as %d", nd, (int)got);
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
	test_rules();
	if (fails) {
		printf("test_nn_decode_count: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_nn_decode_count: OK\n");
	return 0;
}
