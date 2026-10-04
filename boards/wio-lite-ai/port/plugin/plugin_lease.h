/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_lease.h
 * @brief   Who may touch a loaded plugin's private result (issue #110 =
 *          #78 Step 3b).
 *
 * [!] THIS BOARD HAS NO STRUCTURAL EXCLUSION BETWEEN decode() AND draw(), and
 * that is the difference that made this file necessary.  On
 * grove-vision-ai-v2 the frame pipeline pre-pins one delivery per sink, so
 * while the producer decoded (until issue #129) its decode and the panel's draw
 * could never overlap.  Since #129 the decode is on an inference worker there
 * too, and Grove's own plugin lease is what keeps the two apart.  Here the
 * preview (flip) thread runs at a HIGHER priority
 * than the inference worker and nothing separates them: the worker is
 * preemptible mid-decode, and the panel is what preempts it.
 *
 * That was harmless while the worker handed over BOXES -- it filled a local
 * array, took the detection mutex, copied, and released, so a reader either saw
 * the previous decode or the new one.  A plugin's result is PRIVATE and stays
 * inside the plugin, so there is nothing to copy and the reader would be
 * looking at half-written state.
 *
 * WHAT IT COVERS.  Everything that touches a plugin: its decode, its draw, its
 * report, its parameters, and its replacement or removal.  Not the record --
 * that keeps its own mutex, because the panel still reads box counts without
 * caring whose they are.
 *
 * [!] THE ORDER IS ALWAYS LEASE, THEN THE FRAME LOCK.  The panel takes this
 * first and the frame lock second; the worker takes this and then the detection
 * mutex.  Nothing takes it the other way round, and a shortcut that acquired it
 * from inside an overlay helper -- after the frame lock -- would invert that.
 *
 * [!] THE API IS svc/plugin_lease_api.h (issue #130, as grove-vision-ai-v2's
 * since #127).  This file adds what only this board knows -- the wait bound,
 * the lock order above, plugin_lease_init() -- and plugin_lease.c implements
 * the API with a ThreadX mutex.  With it came the two things this board's lease
 * lacked: plugin_lease_held(), which every entry into the plugin now asks
 * before it calls through (port/nn/nn_active.c, port/plugin/plugin_run.c), and
 * the count of entries refused because the caller did not hold it.
 *
 * [!] THE ORDER IS THE OPPOSITE OF GROVE'S, AND STAYS SO.  Grove's panel asks
 * for its lease inside its panel guard; this panel takes the lease first and
 * the frame lock second.  Both are safe for the same reason -- each panel only
 * TRIES -- and neither is to be "aligned" with the other.
 *
 * [!] AND THE PANEL DOES NOT WAIT.  A draw that blocked here would hold a lock
 * wider than the thing it is protecting, for as long as a decode takes.  It
 * asks, and on a refusal it presents the picture without an overlay -- which is
 * the failure this pipeline already has for a process() that declines.  What it
 * must also do is COUNT those, because the existing preview counters see a
 * frame that was presented, not one that was presented bare.
 */
#ifndef PLUGIN_LEASE_H
#define PLUGIN_LEASE_H

#include <stdint.h>

#include "plugin_lease_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How long a console -- and the inference worker, and a stop's record boundary
 * -- waits for the lease, in milliseconds.  THE one bound:
 * plugin_lease_take() takes no argument, so callers cannot each pick their own.
 * Until issue #130 three files each passed 50 ticks of their own.
 *
 * The same figure as grove-vision-ai-v2's, and with the same extra tick
 * (plugin_lease.c): a ThreadX timeout of N ticks can expire N-1 tick periods
 * after the call, so the 50 ticks this board used to pass could give up after
 * 49 ms.  Here the holders are the worker's decode-and-publish, the panel's
 * draw and a console's report or parameter call -- all short beside the bound
 * (the README's stream run reads 0 panel misses); re-measure when a new path
 * starts holding it.
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
