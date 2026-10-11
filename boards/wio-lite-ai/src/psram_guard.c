/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The OCTOSPI1 guard word (issue #142).  See psram_guard.h.  Pure: the atomic
 * application is psram.c's.
 */
#include "psram_guard.h"

#include <stddef.h>

static int well_formed(uint32_t w)
{
	/* No stray bits, and never a mark together with users. */
	if (w & ~(PSRAM_GUARD_RECONF | PSRAM_GUARD_COUNT))
		return 0;
	if ((w & PSRAM_GUARD_RECONF) && (w & PSRAM_GUARD_COUNT))
		return 0;
	return 1;
}

int psram_guard_user_take(uint32_t w, uint32_t *next)
{
	if (next == NULL || !well_formed(w) || (w & PSRAM_GUARD_RECONF))
		return 0;
	if ((w & PSRAM_GUARD_COUNT) >= PSRAM_GUARD_MAX_USERS)
		return 0;
	*next = w + 1u;
	return 1;
}

int psram_guard_user_give(uint32_t w, uint32_t *next)
{
	if (next == NULL || !well_formed(w) || (w & PSRAM_GUARD_RECONF))
		return 0;
	if ((w & PSRAM_GUARD_COUNT) == 0u)
		return 0;
	*next = w - 1u;
	return 1;
}

int psram_guard_reconf_take(uint32_t w, uint32_t *next)
{
	if (next == NULL || w != 0u)
		return 0;
	*next = PSRAM_GUARD_RECONF;
	return 1;
}

int psram_guard_reconf_give(uint32_t w, uint32_t *next)
{
	if (next == NULL || w != PSRAM_GUARD_RECONF)
		return 0;
	*next = 0u;
	return 1;
}

uint32_t psram_guard_users(uint32_t w)
{
	return well_formed(w) ? (w & PSRAM_GUARD_COUNT) : 0u;
}
