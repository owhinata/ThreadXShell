/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_cache_board.c
 * @brief   This board's half of the arena ownership protocol (issues #46, #169).
 *
 * The protocol -- when the arena is cleaned, when it is invalidated, what a
 * failed inference must prove first, and the vendor hooks that carry it -- is
 * svc/nn/ethosu/npu_cache.c, shared and stateless.  What it runs on is here:
 *
 *   npu_cache_board_state   its state, one zero-initialised object
 *   npu_cache_clean()       the CM55's D-cache line operations through the
 *   npu_cache_invalidate()  Himax hx_*DCache_by_Addr entry points; also used
 *                           by plugin_run.c on the plugin reservation
 *   npu_cache_fail_stop()   LOG_ERR, then interrupts off and spin
 */
#include "npu_hw.h"          /* npu_cache.h: the declarations below        */

#include "WE2_device.h"      /* __disable_irq                             */
#include "WE2_core.h"        /* hx_{Clean,Invalidate}DCache_by_Addr        */
#define LOG_TAG "npucache"
#include "log.h"             /* LOG_ERR                                   */

#define CACHE_LINE 32u

struct npu_cache_state npu_cache_board_state;

/*
 * Stop, loudly and permanently.
 *
 * Reached only when the CPU is about to touch an arena the NPU may still own
 * and there is no way left to establish otherwise.  Returning would corrupt
 * silently and intermittently, which is strictly worse than halting: the same
 * judgement tx_glue.c makes when the WFI preconditions do not hold.
 */
void npu_cache_fail_stop(const char *why)
{
	LOG_ERR("%s -- halting", why);
	__disable_irq();
	for (;;)
		;
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
