/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_cache.c
 * @brief   Cache maintenance for the NPU, owned by this port (issue #44).
 *
 * WHY THE VENDOR HOOKS ARE NEUTERED
 *
 * Upstream's ethosu_driver.c says, in as many words, that the application
 * should do cache maintenance and that you should NOT override the weak
 * ethosu_flush_dcache() / ethosu_invalidate_dcache(), because upstream's are
 * empty.  In THIS SDK they are not empty: Himax filled them in with
 * hx_CleanDCache_by_Addr() / hx_InvalidateDCache_by_Addr(), so the advice
 * inverts -- leaving them alone means the driver does maintenance, and it does
 * it at the wrong time.
 *
 * ethosu_wait() falls through from ETHOSU_JOB_RUNNING into the ETHOSU_JOB_DONE
 * case and invalidates every buffer in the invalidate mask BEFORE taking the
 * completion semaphore -- i.e. while the NPU is still writing the arena.  An
 * invalidate against a range the CPU may still hold dirty also discards
 * whatever the CPU wrote there.  Either way the window between that invalidate
 * and actual completion is unprotected, so a speculative refill can leave stale
 * lines behind exactly where the output tensor lands.
 *
 * So both hooks become no-ops and the sequence lives here, where it can be
 * correct:
 *
 *   1. CPU writes the input tensor.
 *   2. npu_cache_clean() the input (and anything else the NPU reads) to PoC.
 *   3. Invoke.  Nothing touches the shared ranges while it runs.
 *   4. Wait for CONFIRMED completion.
 *   5. npu_cache_invalidate() the outputs before reading them.
 *
 * ALIGNMENT is not a detail here.  hx_*DCache_by_Addr operate in cache lines;
 * a range that starts or ends mid-line either misses the bytes outside it or
 * takes a neighbour's line with it.  The arena is 32-byte aligned and a whole
 * number of lines by construction (npu_arena.c), and the helpers below round
 * outward so a sub-range of it is still handled a whole line at a time.
 * Rounding outward is safe only because everything either side belongs to the
 * same arena -- which is why these take the arena's bounds and clamp.
 */
#include "npu.h"
#include "npu_hw.h"

#include "WE2_core.h"   /* hx_CleanDCache_by_Addr / hx_InvalidateDCache_by_Addr */

#define CACHE_LINE 32u

/* Override the SDK's non-empty weak hooks.  Strong definitions here win at
 * link time; check the build's symbol table if that ever seems not to happen,
 * because a silent revert to the vendor version reintroduces the early
 * invalidate described above. */
void ethosu_flush_dcache(uint32_t *p, size_t bytes)
{
	(void)p;
	(void)bytes;
	/* Deliberately nothing: npu_cache_clean() has already run. */
}

void ethosu_invalidate_dcache(uint32_t *p, size_t bytes)
{
	(void)p;
	(void)bytes;
	/* Deliberately nothing: the driver would do this too early.  See above. */
}

/* Round a range outward to whole cache lines. */
static void line_span(const void *p, size_t bytes, uint32_t *start,
                      int32_t *len)
{
	uint32_t lo = (uint32_t)(uintptr_t)p & ~(CACHE_LINE - 1u);
	uint32_t hi = ((uint32_t)(uintptr_t)p + (uint32_t)bytes + CACHE_LINE - 1u)
	              & ~(CACHE_LINE - 1u);

	*start = lo;
	*len   = (int32_t)(hi - lo);
}

void npu_cache_clean(const void *p, size_t bytes)
{
	uint32_t start;
	int32_t  len;

	if (p == NULL || bytes == 0u)
		return;
	line_span(p, bytes, &start, &len);
	hx_CleanDCache_by_Addr((volatile void *)start, len);
}

void npu_cache_invalidate(void *p, size_t bytes)
{
	uint32_t start;
	int32_t  len;

	if (p == NULL || bytes == 0u)
		return;
	line_span(p, bytes, &start, &len);
	hx_InvalidateDCache_by_Addr((volatile void *)start, len);
}
