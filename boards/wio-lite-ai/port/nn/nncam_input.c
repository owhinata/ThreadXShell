/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nncam_input.c
 * @brief   The input admission reasons as this board's codes.  See nncam_input.h.
 */
#include "nncam_input.h"

#include "nn_camera.h"          /* NNCAM_* */
#include "nn_input_contract.h"  /* enum nn_input_refusal */

#include <stddef.h>
#include <stdint.h>

struct nncam_input_map {
	int8_t      reason;  /**< enum nn_input_refusal                         */
	int8_t      nncam;   /**< NNCAM_* code nn_camera_start() returns        */
	uint8_t     worded;  /**< 1: the sentence is built from the reason name */
	const char *hint;    /**< appended to the name ("" when none)           */
};

static const struct nncam_input_map nncam_input_map[] = {
	{ NN_INPUT_OK,             NNCAM_OK,              0, "" },
	{ NN_INPUT_ERR_ABSENT,     NNCAM_ERR_MODEL,       0, "" },
	{ NN_INPUT_ERR_DTYPE,      NNCAM_ERR_IN_DTYPE,    1, "" },
	{ NN_INPUT_ERR_RANK,       NNCAM_ERR_IN_RANK,     1, "" },
	{ NN_INPUT_ERR_BATCH,      NNCAM_ERR_IN_BATCH,    1, "" },
	{ NN_INPUT_ERR_EMPTY,      NNCAM_ERR_IN_EMPTY,    1, "" },
	{ NN_INPUT_ERR_CHANNELS,   NNCAM_ERR_IN_CHANNELS, 1, "" },
	/* A per-axis quantised input reaches here as scale 0 (#51). */
	{ NN_INPUT_ERR_SCALE,      NNCAM_ERR_QUANT,       1,
	  " -- a per-axis quantized input reads as scale 0" },
	{ NN_INPUT_ERR_ZERO_POINT, NNCAM_ERR_IN_ZP,       1, "" },
	{ NN_INPUT_ERR_ALIGN,      NNCAM_ERR_IN_ALIGN,    1, "" },
	{ NN_INPUT_ERR_BYTES,      NNCAM_ERR_IN_BYTES,    1, "" },
};

#define NNCAM_INPUT_N (sizeof nncam_input_map / sizeof nncam_input_map[0])

int nncam_input_err_of(int reason)
{
	size_t i;

	for (i = 0; i < NNCAM_INPUT_N; i++)
		if (nncam_input_map[i].reason == reason)
			return nncam_input_map[i].nncam;
	return NNCAM_ERR_MODEL;   /* an undocumented reason is still a refusal */
}

int nncam_input_reason_of(int nncam_rc)
{
	size_t i;

	for (i = 0; i < NNCAM_INPUT_N; i++)
		if (nncam_input_map[i].worded && nncam_input_map[i].nncam == nncam_rc)
			return nncam_input_map[i].reason;
	return 0;
}

const char *nncam_input_hint(int reason)
{
	size_t i;

	for (i = 0; i < NNCAM_INPUT_N; i++)
		if (nncam_input_map[i].reason == reason)
			return nncam_input_map[i].hint;
	return "";
}
