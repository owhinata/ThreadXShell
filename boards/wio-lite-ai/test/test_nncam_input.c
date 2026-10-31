/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for port/nn/nncam_input.c -- the shared input admission test's
 * reasons (svc/nn/nn_input_contract.h) as nn_camera_start()'s codes and words
 * (issue #162).
 *
 * Every reason by name, against an expected table written out here (not read
 * back from the one under test); no reason folded into another or into the band
 * tiling refusal (NNCAM_ERR_GEOM); an undocumented reason is still a refusal
 * (NNCAM_ERR_MODEL), never NNCAM_OK; the reverse lookup words exactly the
 * worded codes and none of the board's other codes; and every sentence is
 * distinct and fits NN_SVC_DETAIL_MAX after formatting.
 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "nn_camera.h"
#include "nn_detail.h"          /* NN_SVC_DETAIL_MAX */
#include "nn_input_contract.h"
#include "nncam_input.h"

static const struct {
	int reason, nncam, worded;
} expect[] = {
	{ NN_INPUT_OK,             NNCAM_OK,              0 },
	{ NN_INPUT_ERR_ABSENT,     NNCAM_ERR_MODEL,       0 },
	{ NN_INPUT_ERR_DTYPE,      NNCAM_ERR_IN_DTYPE,    1 },
	{ NN_INPUT_ERR_RANK,       NNCAM_ERR_IN_RANK,     1 },
	{ NN_INPUT_ERR_BATCH,      NNCAM_ERR_IN_BATCH,    1 },
	{ NN_INPUT_ERR_EMPTY,      NNCAM_ERR_IN_EMPTY,    1 },
	{ NN_INPUT_ERR_CHANNELS,   NNCAM_ERR_IN_CHANNELS, 1 },
	{ NN_INPUT_ERR_SCALE,      NNCAM_ERR_QUANT,       1 },
	{ NN_INPUT_ERR_ZERO_POINT, NNCAM_ERR_IN_ZP,       1 },
	{ NN_INPUT_ERR_ALIGN,      NNCAM_ERR_IN_ALIGN,    1 },
	{ NN_INPUT_ERR_BYTES,      NNCAM_ERR_IN_BYTES,    1 },
};
#define N_EXPECT (sizeof expect / sizeof expect[0])

int main(void)
{
	static const int unknown[] = { -11, -12, 1, 2, 100, -100, INT_MIN, INT_MAX };
	char sent[N_EXPECT][256];
	unsigned i, j;
	int code;

	/* Every reason the shared header names is here (0 .. NN_INPUT_ERR_BYTES). */
	assert(N_EXPECT == (unsigned)(1 - NN_INPUT_ERR_BYTES));

	for (i = 0; i < N_EXPECT; i++) {
		assert(nncam_input_err_of(expect[i].reason) == expect[i].nncam);
		if (expect[i].reason != NN_INPUT_OK) {
			assert(expect[i].nncam != NNCAM_OK);
			assert(expect[i].nncam != NNCAM_ERR_GEOM);
		}
		assert(nncam_input_hint(expect[i].reason) != NULL);
	}
	/* No two reasons share a code. */
	for (i = 0; i < N_EXPECT; i++)
		for (j = i + 1; j < N_EXPECT; j++)
			assert(expect[i].nncam != expect[j].nncam);

	/* Fail closed: an undocumented reason is a refusal, never OK. */
	for (i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
		assert(nncam_input_err_of(unknown[i]) == NNCAM_ERR_MODEL);
		assert(nncam_input_hint(unknown[i]) != NULL);
	}

	/* The reverse: exactly the worded codes, and nothing else this board
	   returns (-1 .. -25 and beyond). */
	for (code = 10; code >= -40; code--) {
		int want = 0;

		for (i = 0; i < N_EXPECT; i++)
			if (expect[i].worded && expect[i].nncam == code)
				want = expect[i].reason;
		assert(nncam_input_reason_of(code) == want);
	}
	assert(nncam_input_reason_of(NNCAM_ERR_MODEL) == 0);
	assert(nncam_input_reason_of(NNCAM_ERR_GEOM) == 0);
	assert(nncam_input_reason_of(NNCAM_OK) == 0);

	/* The sentences: each worded reason reads differently, and fits. */
	for (i = 0; i < N_EXPECT; i++) {
		int n;

		sent[i][0] = '\0';
		if (!expect[i].worded)
			continue;
		n = snprintf(sent[i], sizeof sent[i], NNCAM_INPUT_DETAIL_FMT,
		             nn_input_refusal_name(expect[i].reason),
		             nncam_input_hint(expect[i].reason));
		assert(n > 0 && n <= NN_SVC_DETAIL_MAX);
		assert(strstr(sent[i], "unknown") == NULL);
		printf("  %4d  %s\n", expect[i].nncam, sent[i]);
	}
	for (i = 0; i < N_EXPECT; i++)
		for (j = i + 1; j < N_EXPECT; j++)
			if (expect[i].worded && expect[j].worded)
				assert(strcmp(sent[i], sent[j]) != 0);

	printf("test_nncam_input: all passed\n");
	return 0;
}
