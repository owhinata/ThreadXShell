/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the decode counting table and tally (issue #130 step 6c, #122
 * P8, svc/nn/nn_decode_count.c; one table since issue #159 Step 4c).  Since
 * issue #163 the one copy wio-lite-ai and f746g-disco both link (and since
 * issue #164 grove-vision-ai-v2 too); this file merges the two boards' tests,
 * every case of both kept.
 *
 * WHY THIS EXISTS.  The rows that matter cannot be typed: a plugin answers
 * BF_ERR_UNINIT or BF_ERR_ARG only for a wiring fault no shipped container
 * produces, and a code nobody documented has no container at all.  Each must be
 * counted as a decoder fault -- folding a wiring fault into "the model" sends the
 * operator to a different model, and folding any negative into nothing reads as
 * a healthy stream.  And BF_ERR_MODEL from a plugin that passed the admission is
 * a container whose two halves disagree: model_errors, never an inference with
 * nobody found.  Every row is spelled out against the real BF_ERR_* values, then
 * the rules are checked over a sweep of answers.  Since issue #164 the tally is
 * the one rule of all three boards (grove-vision-ai-v2 too), and a one-shot
 * counts nothing in it.
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

static void row(int nd, enum nn_decode_count want)
{
	enum nn_decode_count got = nn_decode_count_of(nd);

	CHECK(got == want, "nd %d: got %d, want %d", nd, (int)got, (int)want);
}

static void test_rows(void)
{
	/* grove-vision-ai-v2's table (one-shots: test_oneshot()). */
	row(0, NN_DC_NONE);               /* decoded nobody: a measurement  */
	row(1, NN_DC_NONE);
	row(5, NN_DC_NONE);
	row(INT_MAX, NN_DC_NONE);
	row(BF_ERR_MODEL, NN_DC_MODEL);   /* the container's halves disagree:
	                                   * load a different model          */
	row(BF_ERR_UNINIT, NN_DC_DECODER);
	row(BF_ERR_ARG, NN_DC_DECODER);
	row(-4, NN_DC_DECODER);           /* undocumented: a fault, never
	                                   * "the model"                     */
	row(-64, NN_DC_DECODER);
	row(INT_MIN, NN_DC_DECODER);
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

	for (i = 0u; i < 3u; i++) {
		CHECK(nn_decode_tally_of(0, 0, all[i]) == NN_TALLY_NOTHING,
		      "a dropped publish (dc %d) was counted", (int)all[i]);
		CHECK(nn_decode_tally_of(0, 1, all[i]) == NN_TALLY_NOTHING,
		      "a dropped one-shot publish (dc %d) was counted",
		      (int)all[i]);
	}
	CHECK(nn_decode_tally_of(1, 0, NN_DC_NONE) == NN_TALLY_INFER,
	      "a taken result is not an infer");
	CHECK(nn_decode_tally_of(1, 0, NN_DC_DECODER) == NN_TALLY_DECODER,
	      "a taken decoder refusal is not a decoder error");
	CHECK(nn_decode_tally_of(1, 0, NN_DC_MODEL) == NN_TALLY_MODEL,
	      "a taken model mismatch is not a model error");

	/* A plugin's one publish of a stream, dropped by the stop's boundary: a
	 * face found, and nothing counted. */
	if (nn_decode_tally_of(0, 0, nn_decode_count_of(1)) == NN_TALLY_INFER)
		infers++;
	CHECK(infers == 0u, "[!] the stop-refused publish counted %u infer(s)",
	      infers);
	CHECK(nn_decode_tally_of(0, 0, nn_decode_count_of(BF_ERR_MODEL)) ==
	      NN_TALLY_NOTHING, "a dropped plugin mismatch was counted");
	/* ...and a stream's raw publish (no plugin, NN_DC_NONE): taken, an infer
	 * with no detections; dropped, nothing. */
	CHECK(nn_decode_tally_of(1, 0, NN_DC_NONE) == NN_TALLY_INFER,
	      "a taken raw publish of a stream is not an infer");
	CHECK(nn_decode_tally_of(0, 0, NN_DC_NONE) == NN_TALLY_NOTHING,
	      "a dropped raw publish was counted");
}

/*
 * [!] A ONE-SHOT COUNTS NOTHING (issue #164), raw or decoded, taken or not, a
 * result or a refusal: `nn run` prints its own result.  Before #164 wio-lite-ai
 * and f746g-disco counted a taken one-shot -- even a refused decode -- as an
 * infer, and grove-vision-ai-v2 counted nothing.
 */
static void test_oneshot(void)
{
	static const int nds[] = { 0, 1, 3, INT_MAX, BF_ERR_MODEL, BF_ERR_UNINIT,
	                           BF_ERR_ARG, -4, INT_MIN };
	static const int shots[] = { 1, 2, -1 };   /* any non-zero is a one-shot */
	unsigned i, j;

	for (i = 0u; i < sizeof nds / sizeof nds[0]; i++)
		for (j = 0u; j < sizeof shots / sizeof shots[0]; j++) {
			CHECK(nn_decode_tally_of(1, shots[j],
			                         nn_decode_count_of(nds[i])) ==
			      NN_TALLY_NOTHING,
			      "[!] a taken one-shot (nd %d, %d) was counted",
			      nds[i], shots[j]);
			CHECK(nn_decode_tally_of(0, shots[j],
			                         nn_decode_count_of(nds[i])) ==
			      NN_TALLY_NOTHING,
			      "a dropped one-shot (nd %d) was counted", nds[i]);
		}
	CHECK(nn_decode_tally_of(1, 1, NN_DC_NONE) == NN_TALLY_NOTHING,
	      "a one-shot's raw publish was counted");
}

/* The rules, over a sweep: no negative of a stream is dropped or counted as an
 * infer, BF_ERR_MODEL alone is a model error, and no count is an error. */
static void test_rules(void)
{
	for (int nd = -200; nd <= 200; nd++) {
		enum nn_decode_count got = nn_decode_count_of(nd);
		enum nn_decode_tally t = nn_decode_tally_of(1, 0, got);

		if (nd >= 0) {
			CHECK(got == NN_DC_NONE, "%d counted as %d", nd, (int)got);
			CHECK(t == NN_TALLY_INFER, "count %d tallied %d", nd,
			      (int)t);
		} else if (nd == BF_ERR_MODEL) {
			CHECK(got == NN_DC_MODEL, "BF_ERR_MODEL got %d", (int)got);
			CHECK(t == NN_TALLY_MODEL, "BF_ERR_MODEL tallied %d",
			      (int)t);
		} else {
			CHECK(got == NN_DC_DECODER, "negative %d got %d", nd,
			      (int)got);
			CHECK(t == NN_TALLY_DECODER, "negative %d tallied %d", nd,
			      (int)t);
		}
		CHECK(nn_decode_tally_of(1, 1, got) == NN_TALLY_NOTHING,
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
	test_oneshot();
	test_rules();
	if (fails) {
		printf("test_nn_decode_count: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_nn_decode_count: OK\n");
	return 0;
}
