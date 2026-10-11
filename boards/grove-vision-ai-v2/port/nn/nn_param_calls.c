/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_param_calls.c
 * @brief   The gate claim and the threshold-call count.  See nn_param_calls.h.
 */
#include "nn_param_calls.h"

int nn_gate_claim(uint8_t *busy, uint8_t *owner, uint16_t calls, uint8_t who)
{
	if (*busy)
		return 0;
	/* [!] A threshold call inside the plugin holds a pointer into it;
	 * replacing or removing the plugin now would pull the code out from
	 * under it. */
	if (who == (uint8_t)NN_OWNER_SWAP && calls != 0u)
		return 0;
	*busy  = 1u;
	*owner = who;
	return 1;
}

int nn_param_calls_enter(uint8_t busy, uint8_t owner, uint16_t *calls)
{
	if (busy && owner == (uint8_t)NN_OWNER_SWAP)
		return 0;
	if (*calls >= NN_PARAM_CALLS_MAX)
		return 0;
	(*calls)++;
	return 1;
}

void nn_param_calls_leave(uint16_t *calls)
{
	if (*calls != 0u)
		(*calls)--;
}
