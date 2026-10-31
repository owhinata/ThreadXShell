/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The camera input refusals (svc/nn/nn_input_codes.h, issue #162) against this
 * board's OTHER nn_camera_start() codes: the shared table is tested in
 * shell/test/test_nn_input_codes.c, and what only this board can say is that
 * none of its values lands on a code that already means something else here.
 */
#include <assert.h>
#include <stdio.h>

#include "nn_camera.h"
#include "nn_input_codes.h"

int main(void)
{
	static const int input[] = {
		NNCAM_ERR_QUANT, NNCAM_ERR_IN_DTYPE, NNCAM_ERR_IN_RANK,
		NNCAM_ERR_IN_BATCH, NNCAM_ERR_IN_EMPTY, NNCAM_ERR_IN_CHANNELS,
		NNCAM_ERR_IN_ZP, NNCAM_ERR_IN_ALIGN, NNCAM_ERR_IN_BYTES,
	};
	static const int other[] = {
		NNCAM_OK, NNCAM_ERR_RUNNING, NNCAM_ERR_NOTRUN, NNCAM_ERR_SESSION,
		NNCAM_ERR_PSRAM, NNCAM_ERR_BAND, NNCAM_ERR_GEOM, NNCAM_ERR_INIT,
		NNCAM_ERR_TEARING, NNCAM_ERR_REARM, NNCAM_ERR_SHAPES,
		NNCAM_ERR_NODRAW, NNCAM_ERR_DECBUSY, NNCAM_ERR_TEST,
		NNCAM_ERR_STILL, NNCAM_ERR_CAMBUSY,
	};
	unsigned i, j;

	/* "no input" is this board's "no model": the same code on purpose. */
	assert(NNCAM_ERR_MODEL == NN_INPUT_START_ERR_MODEL);
	for (i = 0; i < sizeof input / sizeof input[0]; i++)
		for (j = 0; j < sizeof other / sizeof other[0]; j++)
			assert(input[i] != other[j]);
	printf("test_nncam_input: no input refusal collides with another start code\n");
	return 0;
}
