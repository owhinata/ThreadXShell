/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_run.c
 * @brief   This board's half of the loader.  See plugin_run.h.
 */
#define LOG_TAG "plugin"
#include "log.h"

#include "plugin_run.h"
#include "plugin_mpu.h"

#include "WE2_device.h"          /* CMSIS core: MPU, SCB, caches, barriers */
#include "npu_hw.h"              /* npu_cache_clean()                      */
#include "nor_flash.h"           /* nor_lease_held()                       */

#include <stddef.h>

/* The reservation, from the linker script.  Declared as arrays so a bare
 * reference is already the address. */
extern uint8_t __plugin_start[], __plugin_end[];

/*
 * [!] STATIC AND PERMANENT, AND THE ENVIRONMENT IS A CONSTANT OVER IT.
 * plugin_run_attribute() is reachable from a fault handler, so the path from
 * "this board" to "the published pointer" has to be safe from cold boot: no
 * lazy initialisation, no allocation, no lock, and no pointer anybody can swap
 * for another object.  A file scope object plus a file scope const referring to
 * it is the whole of it.
 */
static struct plugin_exec_state pl_state;

/* ---- the three board-specific things ------------------------------------- */

/*
 * Take a consistent MPU snapshot and judge it.
 *
 * [!] READ UNDER A CRITICAL SECTION.  CTRL, TYPE, every region and both MAIRs
 * have to describe one configuration; read with preemption allowed, they could
 * straddle a reconfiguration and the verdict would be about a state that never
 * existed.  Keeping interrupts off for the whole of a callback is not viable
 * and is not what this does -- what holds the configuration still for the
 * plugin's lifetime is the flash lease the caller already holds (plugin_run.h).
 */
static int pl_exec_ok(uint32_t lo, uint32_t hi, const char **why)
{
	struct plugin_mpu_region rgn[PLUGIN_MPU_REGION_MAX];
	uint32_t ctrl, type, mair0, mair1, saved_rnr;
	enum plugin_mpu_verdict v;
	unsigned n, i;
	uint32_t pm = __get_PRIMASK();

	__disable_irq();
	ctrl      = MPU->CTRL;
	type      = MPU->TYPE;
	mair0     = MPU->MAIR0;
	mair1     = MPU->MAIR1;
	saved_rnr = MPU->RNR;

	n = (type >> PLUGIN_MPU_TYPE_DREGION_SHIFT) & PLUGIN_MPU_TYPE_DREGION_MASK;
	if (n > PLUGIN_MPU_REGION_MAX)
		n = PLUGIN_MPU_REGION_MAX;
	for (i = 0u; i < n; i++) {
		MPU->RNR = i;
		rgn[i].rbar = MPU->RBAR;
		rgn[i].rlar = MPU->RLAR;
	}
	MPU->RNR = saved_rnr;
	if (pm == 0u)
		__enable_irq();

	v = plugin_mpu_judge(ctrl, type, rgn, n, mair0, mair1, lo, hi);
	if (v == PLUGIN_MPU_OK)
		return 0;
	if (why != NULL)
		*why = plugin_mpu_strerror(v);
	return -1;
}

/*
 * Make the CPU agree with itself about bytes it just wrote and is about to
 * execute.
 *
 * The I- and D-caches are separate and an instruction fetch does not snoop the
 * D-cache, so the image has to be pushed out of the data side and the stale
 * instruction side dropped, in that order, with barriers between.
 */
static void pl_sync_caches(uint32_t base, uint32_t len)
{
	npu_cache_clean((const void *)(uintptr_t)base, len);
	__DSB();
	SCB_InvalidateICache_by_Addr((volatile void *)(uintptr_t)base,
	                             (int32_t)len);
	__DSB();
	__ISB();
}

/* The XIP window the image is read from must be pinned by the caller. */
static int pl_source_ok(const void *container, uintptr_t token,
                        const char **why)
{
	(void)container;
	if (nor_lease_held((uint32_t)token))
		return 0;
	if (why != NULL)
		*why = "the flash lease is not live";
	return -1;
}

static const struct plugin_exec_port pl_port = {
	.sync_caches = pl_sync_caches,
	.exec_ok     = pl_exec_ok,
	.source_ok   = pl_source_ok,
};

static const struct plugin_exec_env pl_env = {
	.state    = &pl_state,
	.port     = &pl_port,
	/* [!] THE RESERVATION IS THIS BOARD'S FACT, stated here rather than
	 * looked up inside the shared loader.  Link-time constants in an
	 * initialiser, so the environment stays in .rodata. */
	.res_lo = __plugin_start,
	.res_hi = __plugin_end,
};

/* ---- the board-facing API ------------------------------------------------ */

enum plugin_run_result plugin_run_load(const struct plugin_view *v,
                                       const void *container, uint32_t lease,
                                       const struct plugin_base_api *base)
{
	const char *why = NULL;
	enum plugin_run_result r;

	r = plugin_exec_load(&pl_env, v, container, (uintptr_t)lease, base, &why);
	switch (r) {
	case PLUGIN_RUN_OK:
		LOG_INF("'%s' (build %s) loaded: %lu B at 0x%08lx",
		        pl_state.slot.name, pl_state.slot.build_id,
		        (unsigned long)v->mem_size,
		        (unsigned long)(uintptr_t)pl_env.res_lo);
		break;
	case PLUGIN_RUN_NO_PLUGIN:
		break;                      /* legal; the caller carries on */
	case PLUGIN_RUN_TOO_BIG:
		LOG_ERR("image wants %lu B at 0x%08lx, reservation is %lu B at 0x%08lx",
		        (unsigned long)v->mem_size, (unsigned long)v->link_addr,
		        (unsigned long)(pl_env.res_hi - pl_env.res_lo),
		        (unsigned long)(uintptr_t)pl_env.res_lo);
		break;
	case PLUGIN_RUN_ENTRY:
		LOG_ERR("'%.*s' refused its own entry point or declares none",
		        (int)sizeof v->name, v->name);
		break;
	default:
		LOG_ERR("%s%s%s", plugin_run_strerror(r),
		        why != NULL ? ": " : "", why != NULL ? why : "");
		break;
	}
	return r;
}

void plugin_run_unload(void)
{
	plugin_exec_unload(&pl_env);
}

int plugin_run_active(void)
{
	return plugin_exec_active(&pl_env);
}

void *plugin_run_slot(unsigned slot)
{
	return plugin_exec_slot(&pl_env, slot);
}

const char *plugin_run_attribute(uint32_t pc, uint32_t *off)
{
	return plugin_exec_attribute(&pl_env, pc, off);
}
