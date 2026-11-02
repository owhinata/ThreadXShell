/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for port/nn/nn_run_says.c (issue #174): what a `nn run` whose one
 * frame ended without a result says on this board, for EVERY NN_OV_SHOT_*
 * value and values past them.
 *
 * Pinned: the status of each ending is what it was before the words moved
 * (LEASE_TIMEOUT BUSY, every other HW -- the table below is written out); the
 * endings the shared table names read exactly as nn_shot_says() says them (the
 * words wio-lite-ai and f746g-disco print); this board's own endings keep their
 * own sentences, with the output index and the code at their widest; and every
 * sentence fits NN_SVC_DETAIL_MAX.  None of these endings can be made on the
 * device: no shipped model has an unreadable output, and a lease that times
 * out or a prep that refuses a frame cannot be typed.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "nn_camera.h"
#include "nn_detail.h"
#include "nn_run_says.h"
#include "nn_shot.h"
#include "nn_svc.h"

static int fails, checks;

#define CHECK(cond, what)                                                   \
	do {                                                                \
		checks++;                                                   \
		if (!(cond)) {                                              \
			printf("  FAIL %s  (%s:%d)\n", what, __FILE__, __LINE__); \
			fails++;                                            \
		}                                                           \
	} while (0)

struct row {
	int         shot;
	unsigned    index;
	int         status;
	const char *text;
};

static const struct row rows[] = {
	/* the endings the shared table names: its words */
	{ (int)NN_OV_SHOT_PREP_FAILED, 0u, NN_SVC_ERR_HW,
	  "preprocessing refused the frame" },
	{ (int)NN_OV_SHOT_INVOKE_FAILED, 0u, NN_SVC_ERR_HW,
	  "inference failed" },
	{ (int)NN_OV_SHOT_LEASE_TIMEOUT, 0u, NN_SVC_ERR_BUSY,
	  "the plugin lease was not released in time; nothing was decoded" },
	{ (int)NN_OV_SHOT_NOT_HELD, 0u, NN_SVC_ERR_HW,
	  "the decode was refused: the plugin lease is not held" },
	/* this board's own */
	{ (int)NN_OV_SHOT_NO_OUTPUTS, 3u, NN_SVC_ERR_HW,
	  "output 3 of the model is unreadable" },
	{ (int)NN_OV_SHOT_NO_OUTPUTS, UINT_MAX, NN_SVC_ERR_HW,
	  "output 4294967295 of the model is unreadable" },
	{ (int)NN_OV_SHOT_PUBLISHED, 0u, NN_SVC_ERR_HW,
	  "the result was not accepted by the record" },
	{ (int)NN_OV_SHOT_STOPPED, 0u, NN_SVC_ERR_HW,
	  "the inference worker ended the run without a result (3)" },
	/* not an ending (NONE) and past the defined values: the fallback */
	{ (int)NN_OV_SHOT_NONE, 0u, NN_SVC_ERR_HW,
	  "the inference worker ended the run without a result (0)" },
	{ 8, 0u, NN_SVC_ERR_HW,
	  "the inference worker ended the run without a result (8)" },
	{ -1, 0u, NN_SVC_ERR_HW,
	  "the inference worker ended the run without a result (-1)" },
	{ INT_MIN, 0u, NN_SVC_ERR_HW,
	  "the inference worker ended the run without a result (-2147483648)" },
};

int main(void)
{
	static const struct { unsigned ov; enum nn_shot_reason shared; } same[] = {
		{ NN_OV_SHOT_PREP_FAILED,   NN_SHOT_PREP },
		{ NN_OV_SHOT_INVOKE_FAILED, NN_SHOT_INVOKE },
		{ NN_OV_SHOT_LEASE_TIMEOUT, NN_SHOT_LEASE_TIMEOUT },
		{ NN_OV_SHOT_NOT_HELD,      NN_SHOT_NOT_HELD },
	};
	size_t i;
	int v, covered[8] = { 0 };

	printf("nn_run_no_result_says: status and sentence of every ending\n");
	for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
		struct nn_op_result res;
		char what[220];
		int st;

		memset(&res, 0x5A, sizeof res);
		res.detail[sizeof res.detail - 1] = '\0';
		st = nn_run_no_result_says(rows[i].shot, rows[i].index, &res);
		snprintf(what, sizeof what, "shot %d: %d \"%s\"", rows[i].shot, st,
		         res.detail);
		CHECK(st == rows[i].status, what);
		CHECK(strcmp(res.detail, rows[i].text) == 0, what);
		CHECK(strlen(res.detail) <= NN_SVC_DETAIL_MAX, what);
		CHECK(res.claim == 0x5Au, "the claim is the teardown's, untouched");
		if (rows[i].shot >= 0 && rows[i].shot < 8)
			covered[rows[i].shot] = 1;
	}
	/* every NN_OV_SHOT_* value 0..7 has a row */
	for (v = 0; v < 8; v++) {
		char what[64];

		snprintf(what, sizeof what, "NN_OV_SHOT value %d has a row", v);
		CHECK(covered[v], what);
	}
	CHECK(NN_OV_SHOT_NOT_HELD == 7u,
	      "NN_OV_SHOT_NOT_HELD is still the last value (a new one needs a row)");
	/* the shared endings: the shared table's words AND status, so the two
	 * boards' sentence for the same ending can never drift again */
	for (i = 0; i < sizeof same / sizeof same[0]; i++) {
		struct nn_op_result res;
		const struct nn_shot_says *say = nn_shot_says(same[i].shared);
		char what[96];
		int st;

		st = nn_run_no_result_says((int)same[i].ov, 0u, &res);
		snprintf(what, sizeof what, "shot %u reads as nn_shot reason %d",
		         same[i].ov, (int)same[i].shared);
		CHECK(st == say->status && strcmp(res.detail, say->text) == 0,
		      what);
	}
	if (fails) {
		printf("test_nn_run_says: %d of %d checks FAILED\n", fails, checks);
		return 1;
	}
	printf("test_nn_run_says: all %d checks passed\n", checks);
	return 0;
}
