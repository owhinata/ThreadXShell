/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_lease.c
 * @brief   The plugin lease.  See plugin_lease.h.
 */
#define LOG_TAG "plugin"
#include "log.h"

#include "plugin_lease.h"

#include <stddef.h>
#include <stdint.h>

#include "tx_api.h"

/*
 * The wait in ticks, from the one constant in the header.
 *
 * Rounded up to whole ticks, then ONE MORE: a ThreadX timeout of N ticks expires
 * at the N-th tick boundary, which can come N-1 tick periods after the call.
 * Without the extra tick a 1 ms bound could return at once.
 */
#define PL_WAIT_TICKS                                                         \
	((ULONG)((((uint32_t)PLUGIN_LEASE_WAIT_MS *                           \
	           (uint32_t)TX_TIMER_TICKS_PER_SECOND) + 999u) / 1000u) + 1u)

_Static_assert(PLUGIN_LEASE_WAIT_MS > 0u, "a console must be able to wait");
_Static_assert(PL_WAIT_TICKS >= 2u, "the bound is at least one full tick");

static TX_MUTEX pl_lease;
static uint8_t  pl_lease_ready;

/*
 * The miss counters.
 *
 * [!] TWO WRITERS, ONE READER, ONE RULE.  The producer and the panel both note
 * their tries (since issue #129 only the panel does: plugin_lease_miss.h), and
 * the stats line reads total and worst as a pair.  Every one of
 * them runs plugin_lease_miss_*() with interrupts disabled, so a reader never
 * sees a run from after one writer's update beside a total from before it, and
 * the two writers never lose each other's increments.  Masking on one side only
 * would not do: it cannot reach back into a store the other side was midway
 * through.
 */
static struct plugin_lease_miss pl_miss;

/* Entries refused because their caller did not hold the lease, since boot --
 * never reset, like the stack records: a correct build never counts one, and
 * an arm must not wipe the evidence that one did.  Any thread can write it, so
 * under the same rule as the misses. */
static uint32_t pl_unheld;

int plugin_lease_init(void)
{
	if (pl_lease_ready)
		return 0;
	if (tx_mutex_create(&pl_lease, (CHAR *)"plugin", TX_INHERIT) !=
	    TX_SUCCESS) {
		LOG_ERR("lease mutex create failed");
		return -1;
	}
	pl_lease_ready = 1u;
	return 0;
}

int plugin_lease_try(enum plugin_lease_who who)
{
	TX_INTERRUPT_SAVE_AREA
	int held;

	held = pl_lease_ready &&
	       tx_mutex_get(&pl_lease, TX_NO_WAIT) == TX_SUCCESS;
	TX_DISABLE
	plugin_lease_miss_note(&pl_miss, who, held);
	TX_RESTORE
	return held;
}

int plugin_lease_take(void)
{
	if (!pl_lease_ready)
		return 0;
	return tx_mutex_get(&pl_lease, PL_WAIT_TICKS) == TX_SUCCESS;
}

void plugin_lease_give(void)
{
	if (pl_lease_ready)
		(void)tx_mutex_put(&pl_lease);
}

int plugin_lease_held(void)
{
	TX_THREAD *self;
	TX_THREAD *owner = NULL;
	ULONG count = 0u;

	if (!pl_lease_ready)
		return 0;
	/* NULL outside a thread: an ISR or pre-scheduler code holds nothing, and
	 * must not match an unowned mutex's NULL owner. */
	/* TODO(#130 step 6a): add the ISR test -- inside an ISR tx_thread_identify()
	 * is the INTERRUPTED thread (see wio-lite-ai's plugin_lease_held()). */
	self = tx_thread_identify();
	if (self == NULL)
		return 0;
	if (tx_mutex_info_get(&pl_lease, NULL, &count, &owner, NULL, NULL,
	                      NULL) != TX_SUCCESS)
		return 0;
	return count != 0u && owner == self;
}

void plugin_lease_note_unheld(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (pl_unheld != UINT32_MAX)
		pl_unheld++;
	TX_RESTORE
}

uint32_t plugin_lease_unheld(void)
{
	TX_INTERRUPT_SAVE_AREA
	uint32_t n;

	TX_DISABLE
	n = pl_unheld;
	TX_RESTORE
	return n;
}

void plugin_lease_misses(uint32_t *total, uint32_t *worst_run)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	plugin_lease_miss_read(&pl_miss, total, worst_run);
	TX_RESTORE
}

void plugin_lease_misses_reset(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	plugin_lease_miss_reset(&pl_miss);
	TX_RESTORE
}
