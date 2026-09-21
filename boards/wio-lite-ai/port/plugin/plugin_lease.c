/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_lease.c
 * @brief   The result lease.  See plugin_lease.h.
 */
#define LOG_TAG "plugin"
#include "log.h"

#include "plugin_lease.h"

#include "tx_api.h"

static TX_MUTEX pl_lease;
static int      pl_lease_ready;

/*
 * The miss counters.
 *
 * [!] WRITTEN, READ AND ARMED UNDER THE SAME RULE.  The writer is the panel
 * thread and it updates two words; a reader that only masked interrupts on its
 * own side could still see the run counter from after an increment beside the
 * total from before it.  Interrupt masking cannot reach backwards, so all three
 * sites mask -- the same discipline the draw budget's counters follow.
 */
static uint32_t pl_miss_total;
static uint32_t pl_miss_run;      /* the current consecutive run */
static uint32_t pl_miss_worst;

int plugin_lease_init(void)
{
	if (pl_lease_ready)
		return 0;
	/* TX_INHERIT: the panel outranks the worker, and a console asking for the
	 * lease outranks neither -- without inheritance a medium-priority thread
	 * could keep the worker off the CPU while the console waits on it. */
	if (tx_mutex_create(&pl_lease, "pl_lease", TX_INHERIT) != TX_SUCCESS) {
		LOG_ERR("lease mutex create failed");
		return -1;
	}
	pl_lease_ready = 1;
	return 0;
}

static void note_hit(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	pl_miss_run = 0u;
	TX_RESTORE
}

static void note_miss(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	pl_miss_total++;
	pl_miss_run++;
	if (pl_miss_run > pl_miss_worst)
		pl_miss_worst = pl_miss_run;
	TX_RESTORE
}

int plugin_lease_try(void)
{
	if (!pl_lease_ready)
		return 0;
	if (tx_mutex_get(&pl_lease, TX_NO_WAIT) != TX_SUCCESS) {
		note_miss();
		return 0;
	}
	note_hit();
	return 1;
}

int plugin_lease_take(uint32_t ticks)
{
	if (!pl_lease_ready)
		return 0;
	return tx_mutex_get(&pl_lease, ticks) == TX_SUCCESS;
}

void plugin_lease_give(void)
{
	if (pl_lease_ready)
		(void)tx_mutex_put(&pl_lease);
}

void plugin_lease_misses(uint32_t *total, uint32_t *worst_run)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (total != NULL)
		*total = pl_miss_total;
	if (worst_run != NULL)
		*worst_run = pl_miss_worst;
	TX_RESTORE
}

void plugin_lease_misses_reset(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	pl_miss_total = 0u;
	pl_miss_run   = 0u;
	pl_miss_worst = 0u;
	TX_RESTORE
}
