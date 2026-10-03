/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_lease_miss.h
 * @brief   What a refused no-wait acquire of the plugin lease counts, as a pure
 *          function over a state the caller owns (issue #127).
 *
 * Split out of plugin_lease.c so that the rules can be walked on the host; the
 * storage, and the critical section every caller runs these in, stay there.
 *
 * WHAT IS COUNTED.  The same thing wio-lite-ai's lease counts: frames that were
 * presented without an overlay because the plugin was busy.  The difference is
 * that here TWO threads ask, and a frame can be lost at either of them:
 *
 *   - the PRODUCER, before it decodes.  A refusal there means no decode for this
 *     frame, so process() declines and the panel never draws it -- the panel does
 *     not ask, and the frame is counted once, here;
 *   - the PANEL, before it draws a frame the producer did decode.  A refusal there
 *     is the same frame's only miss.
 *
 * [!] SINCE ISSUE #129 ONLY THE PANEL ASKS.  The decode moved to the inference
 * worker, which waits for the lease (bounded) instead of trying it, and counts a
 * wait that runs out as an error, not here.  The producer's row stays for the
 * rules below, and nothing calls it on this board now.
 *
 * So each frame produces at most one refusal, and every refusal is one frame.
 * That is a property of the CALLERS (the producer's refusal ends the frame), not
 * something this file can see; the host test drives the two in the order the
 * frame takes and checks it holds.
 *
 * [!] THE RUN ENDS ONLY ON A FRAME THAT WAS ANNOTATED, which is the panel getting
 * the lease.  The producer getting it says only that the frame got as far as the
 * panel; resetting there would let a panel that misses every frame report a run
 * of 1 forever, because each of its refusals would follow a producer success.
 */
#ifndef PLUGIN_LEASE_MISS_H
#define PLUGIN_LEASE_MISS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Which of the two no-wait askers this is.  Anything else is treated as the
 *  producer: it counts a refusal but never ends a run. */
enum plugin_lease_who {
	PLUGIN_LEASE_PRODUCER = 0,  /**< camera producer, before the decode */
	PLUGIN_LEASE_PANEL,         /**< panel thread, before the draw      */
};

struct plugin_lease_miss {
	uint32_t total;   /**< refusals since the last reset             */
	uint32_t run;     /**< the current run of refusals               */
	uint32_t worst;   /**< the longest run since the last reset      */
};

/** Account one no-wait acquire by @p who; @p held is whether it succeeded. */
void plugin_lease_miss_note(struct plugin_lease_miss *m,
                            enum plugin_lease_who who, int held);

/** Read the two numbers the stats line prints.  Either pointer may be NULL. */
void plugin_lease_miss_read(const struct plugin_lease_miss *m,
                            uint32_t *total, uint32_t *worst_run);

/** Start a fresh accounting period. */
void plugin_lease_miss_reset(struct plugin_lease_miss *m);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_LEASE_MISS_H */
