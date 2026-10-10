/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_decode_count.c
 * @brief   This board's decode counting table.  See nn_decode_count.h.
 */
#include "nn_decode_count.h"

#include "blazeface.h"   /* BF_ERR_* -- the decoder's vocabulary */

enum nn_decode_count nn_decode_count_of(int nd, int oneshot)
{
	/* BF_ERR_MODEL is the top-5 result of a model that is not BlazeFace-
	 * shaped (D6), not a refusal. */
	if (oneshot || nd >= 0 || nd == BF_ERR_MODEL)
		return NN_DC_NONE;
	return NN_DC_DECODER;   /* BF_ERR_UNINIT, BF_ERR_ARG, and any other */
}
