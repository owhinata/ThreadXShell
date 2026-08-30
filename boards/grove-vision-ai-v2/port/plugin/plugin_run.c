/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_run.c
 * @brief   The loader.  See plugin_run.h.
 */
#define LOG_TAG "plugin"
#include "log.h"

#include "plugin_run.h"
#include "plugin_mpu.h"

#include "WE2_device.h"          /* CMSIS core: MPU, SCB, caches, barriers */
#include "npu_hw.h"              /* npu_cache_clean()                      */
#include "nor_flash.h"           /* nor_lease_held()                       */

#include <string.h>

/* The reservation, from the linker script.  Declared as arrays so a bare
 * reference is already the address. */
extern uint8_t __plugin_start[], __plugin_end[];

/*
 * What the fault reporter is allowed to see.
 *
 * [!] IMMUTABLE ONCE PUBLISHED, AND THE NAME IS A COPY.  A fault can arrive at
 * any instant, including while a container is being replaced, so the reporter
 * must never follow a pointer into the plugin or into anything the loader is
 * still writing.  The loader fills one of these completely, then publishes a
 * pointer to it with a single store; unpublishing is another single store, and
 * it happens BEFORE the slot is touched again.
 */
struct plugin_active {
	uint32_t base;                       /* image base address            */
	uint32_t len;                        /* mem_size                      */
	char     name[PLUGIN_NAME_MAX];
	char     build_id[PLUGIN_BUILD_ID_MAX];
};

static struct plugin_active   pl_slot;
static struct plugin_active  *volatile pl_active;   /* published pointer */

static struct plugin_view     pl_view;
static int                    pl_started;

/* ---- helpers ------------------------------------------------------------- */

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
static enum plugin_mpu_verdict plugin_mpu_now(uint32_t lo, uint32_t hi)
{
	struct plugin_mpu_region rgn[PLUGIN_MPU_REGION_MAX];
	uint32_t ctrl, type, mair0, mair1, saved_rnr;
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

	return plugin_mpu_judge(ctrl, type, rgn, n, mair0, mair1, lo, hi);
}

/*
 * Make the CPU agree with itself about bytes it just wrote and is about to
 * execute.
 *
 * The I- and D-caches are separate and an instruction fetch does not snoop the
 * D-cache, so the image has to be pushed out of the data side and the stale
 * instruction side dropped, in that order, with barriers between.
 *
 * [!] THE WHOLE RESERVATION, NOT JUST THE IMAGE.  Maintenance is by address and
 * rounds outward to whole 32-byte lines; maintaining only the image would let
 * that rounding reach whatever follows it.  The reservation's start and end are
 * both 32-byte multiples (the linker script pins them), so maintaining all of
 * it touches nothing else.
 */
static void plugin_sync_caches(void)
{
	uint32_t base = (uint32_t)(uintptr_t)__plugin_start;
	uint32_t len  = (uint32_t)(__plugin_end - __plugin_start);

	npu_cache_clean((const void *)(uintptr_t)base, len);
	__DSB();
	SCB_InvalidateICache_by_Addr((volatile void *)(uintptr_t)base, (int32_t)len);
	__DSB();
	__ISB();
}

/* ---- load / unload ------------------------------------------------------- */

enum plugin_run_result plugin_run_load(const struct plugin_view *v,
                                       const void *container, uint32_t lease,
                                       const struct plugin_base_api *base)
{
	uint32_t res_base = (uint32_t)(uintptr_t)__plugin_start;
	uint32_t res_len  = (uint32_t)(__plugin_end - __plugin_start);
	enum plugin_mpu_verdict mv;
	plugin_entry_fn entry;
	uint32_t slot;

	if (v == NULL || container == NULL || base == NULL)
		return PLUGIN_RUN_ARG;
	if (!v->has_plugin)
		return PLUGIN_RUN_NO_PLUGIN;

	/* Whatever was there is gone from this point on. */
	plugin_run_unload();

	/* The window the image is read from must be pinned by the caller.  See
	 * plugin_run.h for why this is a check and not a new mechanism. */
	if (!nor_lease_held(lease)) {
		LOG_ERR("the flash lease is not live; refusing to read the image");
		return PLUGIN_RUN_NO_LEASE;
	}

	if (v->mem_size > res_len || v->link_addr != res_base) {
		LOG_ERR("image wants %lu B at 0x%08lx, reservation is %lu B at 0x%08lx",
		        (unsigned long)v->mem_size, (unsigned long)v->link_addr,
		        (unsigned long)res_len, (unsigned long)res_base);
		return PLUGIN_RUN_TOO_BIG;
	}

	/* Copy, then zero what has no initialiser.  bss and scratch are described
	 * separately by the manifest and both are memory-only, so neither is in the
	 * bytes that arrived. */
	memcpy((void *)(uintptr_t)res_base,
	       (const uint8_t *)container + v->image_off, v->file_size);
	memset((void *)(uintptr_t)(res_base + v->file_size), 0,
	       v->mem_size - v->file_size);

	plugin_sync_caches();

	/* [!] AFTER the caches, BEFORE the branch.  The vendor's enable_XIP()
	 * reconfigures the MPU, so this port cannot assume the reservation is still
	 * Normal and executable just because it was when the firmware started. */
	mv = plugin_mpu_now(res_base, res_base + res_len);
	if (mv != PLUGIN_MPU_OK) {
		LOG_ERR("the reservation is not executable: %s",
		        plugin_mpu_strerror(mv));
		return PLUGIN_RUN_MPU;
	}

	/* Publish before the branch: a fault inside entry() should name the plugin
	 * that caused it. */
	pl_view = *v;
	pl_slot.base = res_base;
	pl_slot.len  = v->mem_size;
	memcpy(pl_slot.name, v->name, sizeof pl_slot.name);
	memcpy(pl_slot.build_id, v->build_id, sizeof pl_slot.build_id);
	__DMB();
	pl_active = &pl_slot;            /* single aligned store; see the header */

	slot = v->slot[PLUGIN_SLOT_ENTRY];
	entry = (plugin_entry_fn)(uintptr_t)(res_base + slot);

	if (entry(base) != 0) {
		LOG_ERR("'%s' refused its own entry point", pl_slot.name);
		plugin_run_unload();
		return PLUGIN_RUN_ENTRY;
	}

	pl_started = 1;
	LOG_INF("'%s' (build %s) loaded: %lu B at 0x%08lx",
	        pl_slot.name, pl_slot.build_id,
	        (unsigned long)v->mem_size, (unsigned long)res_base);
	return PLUGIN_RUN_OK;
}

void plugin_run_unload(void)
{
	/* Unpublish FIRST.  Everything below rewrites what the fault reporter
	 * would have been reading. */
	pl_active = NULL;
	__DMB();

	pl_started = 0;
	memset(&pl_view, 0, sizeof pl_view);
	memset(&pl_slot, 0, sizeof pl_slot);
}

int plugin_run_active(void)
{
	return pl_started;
}

const struct plugin_view *plugin_run_view(void)
{
	return pl_started ? &pl_view : NULL;
}

const char *plugin_run_attribute(uint32_t pc, uint32_t *off)
{
	const struct plugin_active *a = pl_active;   /* one load, then immutable */

	if (a == NULL)
		return NULL;
	if (pc < a->base || pc - a->base >= a->len)
		return NULL;
	if (off != NULL)
		*off = pc - a->base;
	return a->name;
}

const char *plugin_run_strerror(enum plugin_run_result r)
{
	switch (r) {
	case PLUGIN_RUN_OK:        return "ok";
	case PLUGIN_RUN_ARG:       return "bad argument";
	case PLUGIN_RUN_NO_PLUGIN: return "the container carries no plugin";
	case PLUGIN_RUN_NO_LEASE:  return "the flash lease is not live";
	case PLUGIN_RUN_TOO_BIG:   return "it does not fit the reservation";
	case PLUGIN_RUN_MPU:       return "the reservation is not executable";
	case PLUGIN_RUN_ENTRY:     return "the plugin refused its own entry point";
	}
	return "unknown";
}
