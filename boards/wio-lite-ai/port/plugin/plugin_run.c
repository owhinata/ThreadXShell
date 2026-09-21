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
#include "plugin_mpu_v7m.h"

/*
 * [!] port/nn/nn.h IS DELIBERATELY NOT INCLUDED.  It was, for
 * nn_model_load_region(), and asking that question here is the bug this file's
 * header describes: the backend is double-slotted and the answer changes under
 * the very operation this runs after.  With the header gone the mistake is not
 * available to make again, which is a better guard than a comment saying not
 * to.
 */

#include "stm32h7xx_hal.h"       /* CMSIS core: MPU, SCB, caches, barriers */

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
 *
 * It is ordinary .bss in AXI-SRAM, below the reservation -- deliberately NOT in
 * the reservation, which the loader overwrites.
 */
static struct plugin_exec_state pl_state;

/* ---- the three board-specific things ------------------------------------- */

/*
 * Take a consistent MPU snapshot and judge it.
 *
 * [!] READ UNDER A CRITICAL SECTION.  CTRL, TYPE and every region have to
 * describe ONE configuration; read with preemption allowed they could straddle
 * a reconfiguration and the verdict would be about a state that never existed.
 *
 * [!] AND IT IS READ AT ALL, on a board where nothing reconfigures the MPU
 * after mpu_config() runs at boot.  "Nothing does" is a claim about today's
 * source; this is the check that survives tomorrow's, and it costs one read of
 * a handful of registers once per model load.
 */
static int pl_exec_ok(uint32_t lo, uint32_t hi, const char **why)
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

/*
 * The staging region the CALLER was handed, travelling as the port token.
 *
 * [!] A STACK OBJECT OF plugin_run_load()'s, not a file-scope one.  It is
 * valid for exactly the call that passes it, which is the whole lifetime the
 * check needs -- and a static would be a second place for "which region" to
 * live, on a board where getting that wrong is what this hook exists to
 * catch.
 */
struct pl_source {
	const uint8_t *lo;
	uint32_t       cap;
};

/*
 * The image is read out of the backend's staging buffer, and the whole of what
 * will be read has to be inside it.
 *
 * [!] THIS IS A WEAKER OBLIGATION THAN GROVE'S, AND STILL A CHECK.  There is no
 * window here to be taken down -- the container is a copy in RAM whose CRC the
 * caller has already verified -- so what can go wrong is not a lifetime but a
 * provenance: a caller handing over a pointer into some other buffer.
 *
 * [!] AND THE REGION IS THE CALLER'S TOKEN, NOT A QUERY.  This asked
 * nn_model_load_region() itself, and the backend is DOUBLE-SLOTTED: that call
 * hands out the INACTIVE slot, so once nn_model_reload() adopted the staged
 * model the query began answering with the other one and every container was
 * refused on hardware.  The caller holds the NN session across load_region()
 * and reload() -- that is the rule port/nn/nn.h states -- so the region it was
 * handed is the fact, and it hands it here.
 *
 * It deliberately does NOT claim the session is still held.  A predicate for
 * that would answer "somebody holds it", which is a fact about another
 * thread's lifetime and not about this caller's right to these bytes.
 */
static int pl_source_ok(const void *container, uint32_t len, uintptr_t token,
                        const char **why)
{
	const struct pl_source *src = (const struct pl_source *)(uintptr_t)token;
	const uint8_t *c = (const uint8_t *)container;

	if (src == NULL || src->lo == NULL || src->cap == 0u) {
		if (why != NULL)
			*why = "the caller named no staging region";
		return -1;
	}
	if (c < src->lo || len > src->cap || c + len > src->lo + src->cap) {
		if (why != NULL)
			*why = "the image is not inside the staging region the caller "
			       "was handed";
		return -1;
	}
	return 0;
}

static const struct plugin_exec_port pl_port = {
	.sync_caches = pl_sync_caches,
	.exec_ok     = pl_exec_ok,
	.source_ok   = pl_source_ok,
};

static const struct plugin_exec_env pl_env = {
	.state  = &pl_state,
	.port   = &pl_port,
	/* [!] THE RESERVATION IS THIS BOARD'S FACT, stated here rather than
	 * looked up inside the shared loader.  Link-time constants in an
	 * initialiser, so the environment stays in .rodata. */
	.res_lo = __plugin_start,
	.res_hi = __plugin_end,
};

/* ---- the board-facing API ------------------------------------------------ */

enum plugin_run_result plugin_run_load(const struct plugin_view *v,
                                       const void *container,
                                       const void *stage, uint32_t cap,
                                       const struct plugin_base_api *base)
{
	struct pl_source src = { (const uint8_t *)stage, cap };
	const char *why = NULL;
	enum plugin_run_result r;

	r = plugin_exec_load(&pl_env, v, container, (uintptr_t)&src, base, &why);
	switch (r) {
	case PLUGIN_RUN_OK:
		LOG_INF("'%s' (build %s) loaded: %lu B at 0x%08lx",
		        pl_state.slot.name, pl_state.slot.build_id,
		        (unsigned long)v->mem_size,
		        (unsigned long)plugin_run_res_base());
		break;
	case PLUGIN_RUN_NO_PLUGIN:
		break;                      /* legal; the caller carries on */
	case PLUGIN_RUN_TOO_BIG:
		LOG_ERR("image wants %lu B at 0x%08lx, reservation is %lu B at 0x%08lx",
		        (unsigned long)v->mem_size, (unsigned long)v->link_addr,
		        (unsigned long)plugin_run_res_len(),
		        (unsigned long)plugin_run_res_base());
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

/*
 * [!] noinline, AND THAT IS A PLACEMENT DECISION.  Its one caller is the fault
 * handler, which lives in ITCM for interrupt latency, and
 * cmake/check_itcm_residency.py refuses any reference out of ITCM that is not
 * named with a reason.  Inlined, LTO left the shared loader's argument check
 * out of line and the veneer pointed at an internal clone -- a target whose
 * NAME is an artefact of this month's inlining.  Out of line, the handler
 * makes exactly one call to a symbol that means something, and the allowance
 * says why a fault may cost a flash fetch.
 */
__attribute__((noinline))
const char *plugin_run_attribute(uint32_t pc, uint32_t *off)
{
	return plugin_exec_attribute(&pl_env, pc, off);
}

uint32_t plugin_run_res_base(void)
{
	return (uint32_t)(uintptr_t)pl_env.res_lo;
}

uint32_t plugin_run_res_len(void)
{
	return (uint32_t)(pl_env.res_hi - pl_env.res_lo);
}
