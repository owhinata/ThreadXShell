/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for svc/nn/nn_input_codes.{h,c} -- the camera input admission test's
 * reasons (svc/nn/nn_input_contract.h) as the start codes and sentences
 * wio-lite-ai and f746g-disco both report (issue #162).
 *
 * Every reason by name, against an expected table written out here with the
 * numbers spelled out (not read back from the one under test); no two reasons
 * share a code; an undocumented reason is still a refusal (the "no model" code),
 * never OK; the reverse lookup words exactly the worded codes; every sentence is
 * distinct and fits NN_SVC_DETAIL_MAX after formatting.  Each board checks on
 * its own that these codes do not collide with its other start codes
 * (boards/<board>/test/test_nncam_input.c).
 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "nn_detail.h"          /* NN_SVC_DETAIL_MAX */
#include "nn_input_codes.h"
#include "nn_input_contract.h"

static const struct {
	int reason, code, worded;
} expect[] = {
	{ NN_INPUT_OK,               0, 0 },
	{ NN_INPUT_ERR_ABSENT,      -3, 0 },
	{ NN_INPUT_ERR_DTYPE,      -18, 1 },
	{ NN_INPUT_ERR_RANK,       -19, 1 },
	{ NN_INPUT_ERR_BATCH,      -20, 1 },
	{ NN_INPUT_ERR_EMPTY,      -21, 1 },
	{ NN_INPUT_ERR_CHANNELS,   -22, 1 },
	{ NN_INPUT_ERR_SCALE,      -11, 1 },
	{ NN_INPUT_ERR_ZERO_POINT, -23, 1 },
	{ NN_INPUT_ERR_ALIGN,      -24, 1 },
	{ NN_INPUT_ERR_BYTES,      -25, 1 },
};
#define N_EXPECT (sizeof expect / sizeof expect[0])

int main(void)
{
	static const int unknown[] = { -11, -12, 1, 2, 100, -100, INT_MIN, INT_MAX };
	char sent[N_EXPECT][256];
	unsigned i, j;
	int code;

	/* Every reason the contract names is here (0 .. NN_INPUT_ERR_BYTES). */
	assert(N_EXPECT == (unsigned)(1 - NN_INPUT_ERR_BYTES));
	/* The names carry the numbers written out above. */
	assert(NN_INPUT_START_OK == 0 && NN_INPUT_START_ERR_MODEL == -3 &&
	       NN_INPUT_START_ERR_QUANT == -11);
	assert(NN_INPUT_START_ERR_DTYPE == -18 && NN_INPUT_START_ERR_RANK == -19 &&
	       NN_INPUT_START_ERR_BATCH == -20 && NN_INPUT_START_ERR_EMPTY == -21 &&
	       NN_INPUT_START_ERR_CHANNELS == -22 && NN_INPUT_START_ERR_ZP == -23 &&
	       NN_INPUT_START_ERR_ALIGN == -24 && NN_INPUT_START_ERR_BYTES == -25);

	for (i = 0; i < N_EXPECT; i++) {
		assert(nn_input_start_err_of(expect[i].reason) == expect[i].code);
		if (expect[i].reason != NN_INPUT_OK)
			assert(expect[i].code != 0);
		assert(nn_input_start_hint(expect[i].reason) != NULL);
	}
	for (i = 0; i < N_EXPECT; i++)
		for (j = i + 1; j < N_EXPECT; j++)
			assert(expect[i].code != expect[j].code);

	/* Fail closed: an undocumented reason is a refusal, never OK. */
	for (i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
		assert(nn_input_start_err_of(unknown[i]) == NN_INPUT_START_ERR_MODEL);
		assert(nn_input_start_hint(unknown[i]) != NULL);
	}

	/* The reverse: exactly the worded codes, and no other value. */
	for (code = 10; code >= -60; code--) {
		int want = 0;

		for (i = 0; i < N_EXPECT; i++)
			if (expect[i].worded && expect[i].code == code)
				want = expect[i].reason;
		assert(nn_input_start_reason_of(code) == want);
	}

	/* The sentences: each worded reason reads differently, and fits. */
	for (i = 0; i < N_EXPECT; i++) {
		int n;

		sent[i][0] = '\0';
		if (!expect[i].worded)
			continue;
		n = snprintf(sent[i], sizeof sent[i], NN_INPUT_START_DETAIL_FMT,
		             nn_input_refusal_name(expect[i].reason),
		             nn_input_start_hint(expect[i].reason));
		assert(n > 0 && n <= NN_SVC_DETAIL_MAX);
		assert(strstr(sent[i], "unknown") == NULL);
		printf("  %4d  %s\n", expect[i].code, sent[i]);
	}
	for (i = 0; i < N_EXPECT; i++)
		for (j = i + 1; j < N_EXPECT; j++)
			if (expect[i].worded && expect[j].worded)
				assert(strcmp(sent[i], sent[j]) != 0);

	printf("test_nn_input_codes: all passed\n");
	return 0;
}
