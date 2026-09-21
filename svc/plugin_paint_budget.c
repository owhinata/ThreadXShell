/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint_budget.c
 * @brief   The charge.  See plugin_paint_budget.h.
 *
 * [!] NO MUTABLE STORAGE: the budget is the caller's, one per draw() call.
 */
#include "plugin_paint_budget.h"

#include <stddef.h>

int plugin_paint_charge(struct plugin_paint_budget *b, uint32_t pixels)
{
	if (b == NULL)
		return 0;
	if (b->ops < PLUGIN_PAINT_OP_COST || pixels > b->pixels) {
		b->refused++;
		return 0;
	}
	b->ops    -= PLUGIN_PAINT_OP_COST;
	b->pixels -= pixels;
	return 1;
}
