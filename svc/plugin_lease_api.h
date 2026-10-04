/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_lease_api.h
 * @brief   The plugin lease's API: who may be inside the loaded plugin
 *          (issue #127; declared here since issue #130, Epic #122 Phase 3b).
 *
 * ONE lock that every path into the plugin takes -- its decode, its draw, its
 * report, its parameters, its entry, and its replacement or removal -- so that
 * two callbacks of one plugin never run at once.
 *
 * [!] DECLARED HERE, IMPLEMENTED BY THE BOARD.  The lock is a ThreadX mutex, and
 * svc/ includes neither tx_api.h nor a board header, so the definitions (and the
 * miss counters, run through plugin_lease_miss.h) are the board's plugin_lease.c.
 * The board's own plugin_lease.h includes this and adds what only it knows: how
 * long plugin_lease_take() waits, the order against its other locks, and
 * plugin_lease_init().  A board that has not adopted this API yet keeps a
 * plugin_lease.h of its own; the different file name is what keeps the two
 * apart -- never the include order.
 *
 * WHO WAITS.  A console and the inference worker wait, only up to the board's
 * one bound.  The panel TRIES and never waits: a refused panel shows the frame
 * without an overlay, counted as a miss (plugin_lease_miss.h says what is
 * counted and when a run ends).
 *
 * [!] A TIMEOUT IS AN ANSWER, NOT A LICENCE.  A caller whose wait ran out
 * reports BUSY or STALE; it does not carry on into the plugin.
 */
#ifndef PLUGIN_LEASE_API_H
#define PLUGIN_LEASE_API_H

#include <stdint.h>

#include "plugin_lease_miss.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Take it without waiting, as @p who, and count the outcome.
 * @return non-zero when it is held and the caller must release it.
 */
int plugin_lease_try(enum plugin_lease_who who);

/**
 * Take it, waiting at most the board's one bound of wall-clock time (the
 * board's plugin_lease.h names it; this takes no argument, so callers cannot
 * each pick their own).  Not counted as a miss: a caller that times out says
 * so itself.
 *
 * [!] A FINITE DEADLINE, AND AN ANSWER WHEN IT PASSES.  A console that waited
 * forever behind a wedged holder would be a shell that stopped responding, with
 * no line of output saying why.
 *
 * @return non-zero when it is held and the caller must release it.
 */
int plugin_lease_take(void);

/** Release it.  Only the thread that took it may call this. */
void plugin_lease_give(void);

/**
 * Whether the CALLING THREAD holds the lease right now.  What the entry points
 * into the plugin check before they call through: a path that reached them
 * without taking the lease is refused, not trusted.
 *
 * Exact for the question it asks: only the caller can make itself the owner,
 * so a "yes" cannot go stale under it, and another thread's hold is a "no".
 * Never true outside a thread (an ISR, or before the scheduler runs).
 */
int plugin_lease_held(void);

/**
 * Count one call into the plugin that its entry point refused because the
 * caller did not hold the lease (issue #127).  Called by the entry checks
 * themselves -- the board's decoder wrappers and the loader -- once per
 * refusal, so no caller has to remember to.  Not a miss: a miss is the lock
 * doing its job, and this is a path that forgot to take it, which no correct
 * build has.
 */
void plugin_lease_note_unheld(void);

/** How many entries were refused for want of the lease, since boot. */
uint32_t plugin_lease_unheld(void);

/** How many no-wait acquires have been refused, and the longest run of them.
 *  A single refusal is ordinary; a run is an overlay that has stopped. */
void plugin_lease_misses(uint32_t *total, uint32_t *worst_run);

/** Start a fresh accounting period for the misses (not the unheld entries,
 *  which count from boot).  Called when a stream is armed, not when one stops: the stats right after a
 *  stop still describe the run that just ended. */
void plugin_lease_misses_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_LEASE_API_H */
