/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_input_codes.c
 * @brief   The input admission reasons as start codes.  See nn_input_codes.h.
 *
 * Owns no storage (the table is const), includes no HAL and no ThreadX.
 */
#include "nn_input_codes.h"

#include "nn_input_contract.h"  /* enum nn_input_refusal */

#include <stddef.h>
#include <stdint.h>

struct nn_input_code {
	int8_t      reason;  /**< enum nn_input_refusal                         */
	int8_t      code;    /**< NN_INPUT_START_*                              */
	uint8_t     worded;  /**< 1: the sentence is built from the reason name */
	const char *hint;    /**< appended to the name ("" when none)           */
};

static const struct nn_input_code nn_input_codes[] = {
	{ NN_INPUT_OK,             NN_INPUT_START_OK,            0, "" },
	{ NN_INPUT_ERR_ABSENT,     NN_INPUT_START_ERR_MODEL,     0, "" },
	{ NN_INPUT_ERR_DTYPE,      NN_INPUT_START_ERR_DTYPE,     1, "" },
	{ NN_INPUT_ERR_RANK,       NN_INPUT_START_ERR_RANK,      1, "" },
	{ NN_INPUT_ERR_BATCH,      NN_INPUT_START_ERR_BATCH,     1, "" },
	{ NN_INPUT_ERR_EMPTY,      NN_INPUT_START_ERR_EMPTY,     1, "" },
	{ NN_INPUT_ERR_CHANNELS,   NN_INPUT_START_ERR_CHANNELS,  1, "" },
	/* A per-axis quantised input reaches here as scale 0 (#51). */
	{ NN_INPUT_ERR_SCALE,      NN_INPUT_START_ERR_QUANT,     1,
	  " -- a per-axis quantized input reads as scale 0" },
	{ NN_INPUT_ERR_ZERO_POINT, NN_INPUT_START_ERR_ZP,        1, "" },
	{ NN_INPUT_ERR_ALIGN,      NN_INPUT_START_ERR_ALIGN,     1, "" },
	{ NN_INPUT_ERR_BYTES,      NN_INPUT_START_ERR_BYTES,     1, "" },
};

#define NN_INPUT_CODES_N (sizeof nn_input_codes / sizeof nn_input_codes[0])

int nn_input_start_err_of(int reason)
{
	size_t i;

	for (i = 0; i < NN_INPUT_CODES_N; i++)
		if (nn_input_codes[i].reason == reason)
			return nn_input_codes[i].code;
	return NN_INPUT_START_ERR_MODEL;   /* an undocumented reason is still a refusal */
}

int nn_input_start_reason_of(int code)
{
	size_t i;

	for (i = 0; i < NN_INPUT_CODES_N; i++)
		if (nn_input_codes[i].worded && nn_input_codes[i].code == code)
			return nn_input_codes[i].reason;
	return 0;
}

const char *nn_input_start_hint(int reason)
{
	size_t i;

	for (i = 0; i < NN_INPUT_CODES_N; i++)
		if (nn_input_codes[i].reason == reason)
			return nn_input_codes[i].hint;
	return "";
}
