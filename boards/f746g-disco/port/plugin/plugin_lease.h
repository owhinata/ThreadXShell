/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_lease.h
 * @brief   Who may be inside the loaded plugin (issue #159 = #78 Step 4b).
 *
 * The API is svc/plugin/plugin_lease_api.h; this board implements it with a
 * ThreadX mutex (plugin_lease.c, the same implementation as wio-lite-ai's and
 * grove-vision-ai-v2's: the lock is the RTOS's, so it is the board's -- plan
 * #159 D6).  This file adds what only this board knows: the wait bound, the lock
 * order and plugin_lease_init().
 *
 * [!] THIS BOARD HAS NO STRUCTURAL EXCLUSION BETWEEN decode() AND draw().  The
 * plugin's draw() runs on the GUIX thread (priority 14) in its CAMERA_FRAME
 * handler, and its decode() on the inference worker (priority 18): the GUIX
 * thread outranks the worker and can preempt it mid-decode.  A plugin's result
 * is PRIVATE and stays inside the plugin, so without this the draw would read
 * half-written state.  Consoles (16 / 17) ask it for the report, the parameters
 * and a load or unload.
 *
 * [!] THE ORDER IS ALWAYS LEASE, THEN THE FRAME LOCK (ltdc_lock_frame()).  The
 * GUIX handler tries this first and takes the frame lock second; the worker
 * takes this and then the record lock (nncam_lock).  Nothing takes it the other
 * way round.
 *
 * [!] AND THE PANEL DOES NOT WAIT.  A draw that blocked here would hold the GUIX
 * thread for as long as a decode takes.  It tries, and on a refusal the frame is
 * shown without the plugin's overlay -- counted as a miss
 * (svc/plugin/plugin_lease_miss.h), because a frame presented bare is not
 * something the preview's own counters can see.
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
 * -- waits for the lease, in milliseconds.  THE one bound: plugin_lease_take()
 * takes no argument, so callers cannot each pick their own.  The same figure as
 * the other two boards', with the same extra tick (plugin_lease.c): a ThreadX
 * timeout of N ticks can expire N-1 tick periods after the call.  The holders
 * are the worker's decode-and-publish, the GUIX draw and a console's report or
 * parameter call -- all short beside the bound; re-measure when a new path
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
