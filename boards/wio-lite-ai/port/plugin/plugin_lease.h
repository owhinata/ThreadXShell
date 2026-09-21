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
 * that is the difference that makes this file necessary.  On
 * grove-vision-ai-v2 the frame pipeline pre-pins one delivery per sink, so the
 * producer's decode and the panel's draw can never overlap and a plugin needs
 * no lock of its own.  Here the preview (flip) thread runs at a HIGHER priority
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

#ifdef __cplusplus
extern "C" {
#endif

/** Create the lease.  From tx_application_define(), before any thread runs. */
int plugin_lease_init(void);

/**
 * Take it without waiting.
 * @return non-zero when it is held and the caller must release it.
 */
int plugin_lease_try(void);

/**
 * Take it, waiting at most @p ticks.
 *
 * [!] A FINITE DEADLINE, AND AN ANSWER WHEN IT PASSES.  Measuring how long a
 * wait took is not the same as bounding it: a console that waited forever
 * behind a wedged worker would be a shell that stopped responding, with no
 * line of output saying why.
 *
 * @return non-zero when it is held and the caller must release it.
 */
int plugin_lease_take(uint32_t ticks);

/** Release it.  Only the thread that took it may call this. */
void plugin_lease_give(void);

/** How many times a no-wait acquire has been refused, and the longest run of
 *  consecutive refusals.  A single refusal is ordinary; a run of them is a
 *  panel that has stopped annotating, which looks like a broken overlay. */
void plugin_lease_misses(uint32_t *total, uint32_t *worst_run);

/** Start a fresh accounting period.  Called when a stream is armed, not when
 *  one stops: the stats right after a stop still describe the run that just
 *  ended. */
void plugin_lease_misses_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_LEASE_H */
