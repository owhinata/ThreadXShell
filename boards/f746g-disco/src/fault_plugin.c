/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fault_plugin.c
 * @brief   See fault_plugin.h.  A pure decision over the loader's published
 *          state, apart from src/fault.c so it can be run on the host.
 */
#include "fault_plugin.h"

#include "plugin_run_core.h"

#include <stddef.h>

enum fault_end fault_plugin_end(uint32_t pc, int pc_valid, int debugger,
                                const char **who, uint32_t *off)
{
	const char *name = NULL;

	if (pc_valid)
		name = plugin_run_attribute(pc, off);
	if (who != NULL)
		*who = name;
	if (name == NULL)
		return FAULT_END_DUMP_HALT;
	return debugger ? FAULT_END_DEBUG_HALT : FAULT_END_RESET;
}
