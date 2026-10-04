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
 * entry, and its replacement or removal.
 *
 * [!] THE API IS svc/plugin_lease_api.h (issue #130).  This file adds what only
 * this board knows -- the wait bound, the lock order, plugin_lease_init() -- and
 * plugin_lease.c implements the API with a ThreadX mutex.  wio still has a
 * plugin_lease.h of its own until it adopts the shared API.
 *
 * WHY THIS BOARD NEEDS ONE AT ALL.  Here the frame pipeline kept the (then)
 * producer's decode and the panel's draw apart (one delivery per sink, released
 * only after draw() returns), so the window that is open is the CONSOLE's: a
 * `nn thresh` or `nn dets` on a running stream enters the same plugin the
 * producer is decoding in, and a plugin's state is private and half-written
 * while it does.  The lock closes that window and makes "two callbacks of one
 * plugin never run at once" a property of every path rather than of the
 * pipeline's shape.
 *
 * [!] SINCE ISSUE #129 IT ALSO KEEPS THE DECODE AND THE DRAW APART.  The decode
 * moved to the inference worker, which the pipeline's one-delivery hand-off
 * does not reach: the worker decodes the next frame while the panel draws the
 * last result.  This lock is now the only thing between the two.
 *
 * WHO WAITS.  A console and, since issue #129, the inference worker -- both only
 * up to PLUGIN_LEASE_WAIT_MS.  A worker whose wait runs out does not decode
 * that frame and counts it as an error.  The panel TRIES and never waits: a
 * refused panel shows the frame without an overlay, counted as a miss
 * (plugin_lease_miss.h says what is counted and when a run ends).  The producer
 * no longer asks at all: it prepares the input and hands it to the worker.
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
 * worker (12) waiting lifts a console holder to 12 at most -- still below the
 * camera producer and the panel, which never wait.
 */
#ifndef PLUGIN_LEASE_H
#define PLUGIN_LEASE_H

#include <stdint.h>

#include "plugin_lease_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How long a console waits for the lease, in milliseconds.  THE one bound:
 * plugin_lease_take() takes no argument, so callers cannot each pick their own.
 *
 * From measurement (Epic #122 U1 spike, 2026-10-03): the longest legitimate
 * hold seen was 12.7 ms -- a console holding it while the producer (priority
 * 10) and the panel (9) preempted it.  A decode (on the inference worker since
 * issue #129) is tens of microseconds.  Roughly four times the longest observed, the same figure wio
 * waits; re-measure when a new path starts holding it.
 */
#define PLUGIN_LEASE_WAIT_MS  50u

/** Create the lease.  From tx_application_define(), before any thread runs.
 *  @return 0, or -1 when the mutex could not be created -- every acquire then
 *  fails, which leaves consoles BUSY and frames bare rather than unguarded. */
int plugin_lease_init(void);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_LEASE_H */
