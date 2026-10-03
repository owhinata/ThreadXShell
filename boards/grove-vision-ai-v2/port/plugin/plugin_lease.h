/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_lease.h
 * @brief   Who may be inside the loaded plugin (issue #127, Epic #122 U1).
 *
 * The same kind of exclusion as wio-lite-ai's port/plugin/plugin_lease.h, with
 * the same name and the same meaning of a miss: ONE lock that every path into
 * the plugin takes -- its decode, its draw, its report, its parameters, its
 * entry, and its replacement or removal.  The two are separate files on
 * purpose for now; folding them into one type is Epic #122 Phase 3b.
 *
 * WHY THIS BOARD NEEDS ONE AT ALL.  Here the frame pipeline already keeps the
 * producer's decode and the panel's draw apart (one delivery per sink, released
 * only after draw() returns), so the window that is open is the CONSOLE's: a
 * `nn thresh` or `nn dets` on a running stream enters the same plugin the
 * producer is decoding in, and a plugin's state is private and half-written
 * while it does.  The lock closes that window and makes "two callbacks of one
 * plugin never run at once" a property of every path rather than of the
 * pipeline's shape.
 *
 * WHO WAITS.  Only a console, and only up to PLUGIN_LEASE_WAIT_MS.  The producer
 * and the panel TRY and never wait: a refused producer skips the decode of that
 * frame, a refused panel shows the frame without an overlay, and both are
 * counted (plugin_lease_miss.h says what is counted and when a run ends).
 *
 * [!] THE ORDER IS THE PANEL GUARD, THEN THIS -- THE OPPOSITE OF wio.  The draw
 * runs inside the panel guard, so the panel can only ask for this after it holds
 * the guard.  That is safe because the panel only TRIES: a thread that never
 * waits cannot close a cycle, whichever order it holds things in.  What must
 * not exist is the other half of a cycle -- a holder of this lock that waits for
 * the panel guard, a camera API mutex or a pipeline lock.  No holder touches
 * the LCD.  (wio's panel takes its lease first and the frame lock second, and
 * its worker waits on neither while holding the lease.)
 *
 * [!] A TIMEOUT IS AN ANSWER, NOT A LICENCE.  A console whose wait ran out
 * reports BUSY or STALE; it does not carry on into the plugin.
 *
 * TX_INHERIT: a background job (priority 17) holding this while a console (16)
 * waits for it would otherwise be starved by whatever runs between them.  The
 * producer and the panel never wait, so inheritance never lifts them.
 */
#ifndef PLUGIN_LEASE_H
#define PLUGIN_LEASE_H

#include <stdint.h>

#include "plugin_lease_miss.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How long a console waits for the lease, in milliseconds.  THE one bound:
 * plugin_lease_take() takes no argument, so callers cannot each pick their own.
 *
 * From measurement (Epic #122 U1 spike, 2026-10-03): the longest legitimate
 * hold seen was 12.7 ms -- a console holding it while the producer (priority
 * 10) and the panel (9) preempted it.  A decode on the producer is tens of
 * microseconds.  Roughly four times the longest observed, the same figure wio
 * waits; re-measure when a new path starts holding it.
 */
#define PLUGIN_LEASE_WAIT_MS  50u

/** Create the lease.  From tx_application_define(), before any thread runs.
 *  @return 0, or -1 when the mutex could not be created -- every acquire then
 *  fails, which leaves consoles BUSY and frames bare rather than unguarded. */
int plugin_lease_init(void);

/**
 * Take it without waiting, as @p who, and count the outcome.
 * @return non-zero when it is held and the caller must release it.
 */
int plugin_lease_try(enum plugin_lease_who who);

/**
 * Take it, waiting at most PLUGIN_LEASE_WAIT_MS of wall-clock time.  Not
 * counted as a miss: a console that times out says so itself.
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
 * themselves -- nn_active.c's wrappers and the loader -- once per refusal, so
 * no caller has to remember to.  Not a miss: a miss is the lock doing its job,
 * and this is a path that forgot to take it, which no correct build has.
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

#endif /* PLUGIN_LEASE_H */
