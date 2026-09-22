/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_probe_rtos.c
 * @brief   The firmware half of the plugin stack probe (issue #119): which
 *          thread a sample was taken on, and the records.  See nn_probe.h.
 *
 * Kept to what only the board can do -- ask ThreadX who is running and read
 * IPSR -- plus the one critical section.  Every decision is nn_probe.c's and has
 * a host test; nothing here decides anything a test could not see.
 */
#include "nn_probe.h"

#include <string.h>

#include "cam_lcd_sink.h"     /* CAM_PANEL_PRIO, CAM_PANEL_STACK_BYTES       */
#include "camera.h"           /* CAM_PRODUCER_PRIO, CAM_PRODUCER_STACK_BYTES */
#include "cli_config.h"       /* CLI_{INSTANCE,BG_JOB}_{PRIORITY,STACK_SIZE} */
#include "nn_plugin_stack.h"  /* GROVE_PLUGIN_ON_* -- the bit order         */
#include "plugin_abi.h"       /* PLUGIN_SLOT_COUNT                           */
#include "tx_api.h"

/* The report marks coverage with nn_plugin_stack.h's masks, indexed by context. */
_Static_assert(GROVE_PLUGIN_ON_PRODUCER == (1u << NN_PROBE_PRODUCER) &&
               GROVE_PLUGIN_ON_PANEL    == (1u << NN_PROBE_PANEL) &&
               GROVE_PLUGIN_ON_CONSOLE  == (1u << NN_PROBE_CONSOLE) &&
               GROVE_PLUGIN_ON_BG       == (1u << NN_PROBE_BG),
               "nn_plugin_stack.h's thread bits must be nn_probe.h's contexts");

/*
 * How each context's threads were created.  Two threads created alike would be
 * one context to this table, so the priorities are asserted apart: a thread is
 * then at most one of them, and one that is none of them is not counted.
 */
static const struct nn_probe_thread_class nn_probe_threads[NN_PROBE_CTX_COUNT] = {
	[NN_PROBE_PRODUCER] = { CAM_PRODUCER_PRIO,     CAM_PRODUCER_STACK_BYTES },
	[NN_PROBE_PANEL]    = { CAM_PANEL_PRIO,        CAM_PANEL_STACK_BYTES },
	[NN_PROBE_CONSOLE]  = { CLI_INSTANCE_PRIORITY, CLI_INSTANCE_STACK_SIZE },
	[NN_PROBE_BG]       = { CLI_BG_JOB_PRIORITY,   CLI_BG_JOB_STACK_SIZE },
};
_Static_assert(CAM_PRODUCER_PRIO != CAM_PANEL_PRIO &&
               CAM_PRODUCER_PRIO != CLI_INSTANCE_PRIORITY &&
               CAM_PRODUCER_PRIO != CLI_BG_JOB_PRIORITY &&
               CAM_PANEL_PRIO != CLI_INSTANCE_PRIORITY &&
               CAM_PANEL_PRIO != CLI_BG_JOB_PRIORITY &&
               CLI_INSTANCE_PRIORITY != CLI_BG_JOB_PRIORITY,
               "the probe tells the four plugin threads apart by priority");

/* Written under TX_DISABLE only; read by nn_probe_snapshot() the same way. */
static struct nn_probe_row nn_probe_rows[PLUGIN_SLOT_COUNT];

/* Nonzero in an exception handler, where tx_thread_identify() names the thread
 * that was interrupted rather than anything running on its stack. */
static inline uint32_t nn_probe_ipsr(void)
{
	uint32_t v;

	__asm__ volatile ("mrs %0, ipsr" : "=r"(v));
	return v;
}

void nn_probe_note(unsigned slot, uintptr_t sp, uint32_t extra)
{
	TX_INTERRUPT_SAVE_AREA
	TX_THREAD *t = NULL;
	enum nn_probe_ctx ctx = NN_PROBE_UNKNOWN;
	uint32_t depth = 0u, left = 0u, stack = 0u;
	int ok = 0;

	if (slot >= (unsigned)PLUGIN_SLOT_COUNT)
		return;

	/*
	 * [!] EVERYTHING THAT CAN BE WORKED OUT FIRST IS WORKED OUT FIRST.  The
	 * critical section below is the compare-and-store and nothing else: no
	 * call into ThreadX, no formatting, no walk.  The thread fields read here
	 * are the running thread's own and do not change under it.
	 */
	if (nn_probe_ipsr() == 0u)
		t = tx_thread_identify();
	if (t != NULL) {
		stack = (uint32_t)t->tx_thread_stack_size;
		ctx = nn_probe_classify(nn_probe_threads,
		                        (uint32_t)t->tx_thread_user_priority, stack);
		ok = ctx != NN_PROBE_UNKNOWN &&
		     nn_probe_measure(sp, (uintptr_t)t->tx_thread_stack_start, stack,
		                      extra, &depth, &left) == NN_PROBE_OK;
	}

	TX_DISABLE
	if (ok)
		nn_probe_record(&nn_probe_rows[slot], (unsigned)ctx, depth, left,
		                stack);
	else
		nn_probe_reject(&nn_probe_rows[slot]);
	TX_RESTORE
}

void nn_probe_snapshot(unsigned slot, struct nn_probe_row *out)
{
	TX_INTERRUPT_SAVE_AREA

	if (out == NULL)
		return;
	if (slot >= (unsigned)PLUGIN_SLOT_COUNT) {
		memset(out, 0, sizeof *out);
		return;
	}
	TX_DISABLE
	*out = nn_probe_rows[slot];
	TX_RESTORE
}
