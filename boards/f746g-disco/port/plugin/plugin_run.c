/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_run.c
 * @brief   This board's binding of the shared plugin loader (issue #110 =
 *          #78 Step 3b on wio-lite-ai; this board since #159 = Step 4b, with the
 *          board-independent half in svc/plugin/plugin_run_core.c).
 *
 * The loader is svc/plugin/plugin_exec.c, and its lease checks, log lines and
 * staging-region check are svc/plugin/plugin_run_core.c, shared with
 * wio-lite-ai.  What is this board's, and lives here, is what touches its
 * hardware (plan #159 D6) and its facts:
 *
 *   - the state, in permanently allocated static memory;
 *   - the reservation, from this board's linker script -- the top 32 KB of
 *     internal SRAM (0x20048000), executable through the default memory map:
 *     no MPU region covers it (src/bsp.c sets SDRAM banks only, PRIVDEFENA);
 *   - cache maintenance: the M7's I- and D-caches are separate and an
 *     instruction fetch does not snoop the D-cache, and this SRAM is cached
 *     write-back through the default map;
 *   - the Armv7-M MPU read-back, judged by svc/plugin/plugin_mpu_v7m.c --
 *     test/test_plugin_mpu_f746.c holds that verdict on this board's own
 *     region set;
 *   - where the loader's lines are logged.
 *
 * [!] THE TWO HARDWARE FUNCTIONS ARE wio-lite-ai's, WORD FOR WORD, AND STAY
 * HERE.  Both boards are Cortex-M7 with the same CMSIS core; what differs is
 * the device header.  The plan names the MPU read-back and the cache
 * maintenance as board-owned, and the Armv7-M shared place for them is the one
 * issue #130 D3 declined.
 *
 * [!] svc/nn/nn.h IS DELIBERATELY NOT INCLUDED.  The staging region travels
 * from the caller (svc/plugin/plugin_run_core.h says why).
 */
#define LOG_TAG "plugin"
#include "log.h"

#include "plugin_run_core.h"
#include "plugin_mpu_v7m.h"

#include "stm32f7xx_hal.h"       /* CMSIS core: MPU, SCB, caches, barriers */

#include <stddef.h>

/* The reservation, from the linker script.  Declared as arrays so a bare
 * reference is already the address. */
extern uint8_t __plugin_start[], __plugin_end[];

/*
 * [!] STATIC AND PERMANENT, AND THE ENVIRONMENT IS A CONSTANT OVER IT.
 * plugin_run_attribute() is reachable from a fault handler, so the path from
 * "this board" to "the published pointer" has to be safe from cold boot: no
 * lazy initialisation, no allocation, no lock, and no pointer anybody can swap
 * for another object.  A file scope object plus a const referring to it is the
 * whole of it.
 *
 * It is ordinary .bss in internal SRAM, below the reservation -- deliberately
 * NOT in the reservation, which the loader overwrites.
 */
static struct plugin_exec_state pl_state;

/*
 * Take a consistent MPU snapshot and judge it.
 *
 * [!] READ UNDER A CRITICAL SECTION.  CTRL, TYPE and every region have to
 * describe ONE configuration; read with preemption allowed they could straddle
 * a reconfiguration and the verdict would be about a state that never existed.
 *
 * [!] AND IT IS READ AT ALL, on a board where nothing reconfigures the MPU
 * after src/bsp.c configures it at boot.  "Nothing does" is a claim about today's
 * source; this is the check that survives tomorrow's, and it costs one read of
 * a handful of registers once per model load.
 *
 * [!] NOT INLINED, so that the shared exec_ok hook -- where entry()'s stack
 * depth is sampled -- keeps a small frame of its own instead of taking on this
 * one's region table (issue #126, as grove-vision-ai-v2 did in #119).
 */
__attribute__((noinline))
int plugin_run_mpu_ok(uint32_t lo, uint32_t hi, const char **why)
{
	struct pl_mpu7_region rgn[PL_MPU7_REGION_MAX] = { { 0u, 0u } };
	uint32_t ctrl, type, saved_rnr;
	enum pl_mpu7_verdict v;
	unsigned n, i;
	uint32_t pm = __get_PRIMASK();

	__disable_irq();
	ctrl      = MPU->CTRL;
	type      = MPU->TYPE;
	saved_rnr = MPU->RNR;

	n = (type >> PL_MPU7_TYPE_DREGION_SHIFT) & PL_MPU7_TYPE_DREGION_MASK;
	/* [!] NOT CLAMPED.  A snapshot that stops short omits the HIGHER-numbered
	 * regions, and on Armv7-M those are the ones that win -- so the judge is
	 * told the real count and refuses rather than answering the wrong
	 * question.  Reading only what fits keeps this loop in bounds. */
	for (i = 0u; i < n && i < PL_MPU7_REGION_MAX; i++) {
		MPU->RNR = i;
		rgn[i].rbar = MPU->RBAR;
		rgn[i].rasr = MPU->RASR;
	}
	MPU->RNR = saved_rnr;
	if (pm == 0u)
		__enable_irq();

	v = pl_mpu7_judge(ctrl, type, rgn, PL_MPU7_REGION_MAX, lo, hi);
	if (v == PL_MPU7_OK)
		return 0;
	if (why != NULL)
		*why = pl_mpu7_strerror(v);
	return -1;
}

/*
 * Make the CPU agree with itself about bytes it just wrote and is about to
 * execute.
 *
 * The I- and D-caches are separate and an instruction fetch does not snoop the
 * D-cache (PM0253 sec 4.8), so the image has to be pushed out of the data side
 * and the stale instruction side dropped, in that order, with barriers between.
 * No D-cache INVALIDATE: these bytes were produced by this CPU, and the copy
 * that matters is the one already in its own cache.
 */
static void pl_sync_caches(uint32_t base, uint32_t len)
{
	SCB_CleanDCache_by_Addr((uint32_t *)(uintptr_t)base, (int32_t)len);
	__DSB();
	SCB_InvalidateICache_by_Addr((void *)(uintptr_t)base, (int32_t)len);
	__DSB();
	__ISB();
}

static const struct plugin_exec_port pl_port = PLUGIN_RUN_PORT(pl_sync_caches);

const struct plugin_exec_env plugin_run_env = {
	.state  = &pl_state,
	.port   = &pl_port,
	/* [!] THE RESERVATION IS THIS BOARD'S FACT, stated here rather than
	 * looked up inside the shared loader.  Link-time constants in an
	 * initialiser, so the environment stays in .rodata. */
	.res_lo = __plugin_start,
	.res_hi = __plugin_end,
};

void plugin_run_log(int error, const char *text)
{
	if (error)
		LOG_ERR("%s", text);
	else
		LOG_INF("%s", text);
}
