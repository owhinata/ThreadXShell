/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_claims.h
 * @brief   What the OPEN model's container claimed, and the `nn info` lines
 *          that say so, for a board whose load goes through the shared order
 *          (svc/nn/nn_core_model.c) -- wio-lite-ai since issue #108,
 *          f746g-disco since #159.
 *
 * [!] ONE COPY.  wio-lite-ai wrote the store, the snapshot and the lines in its
 * adapter (issues #108, #110); f746g-disco's Step 4a wrote the same three
 * again.  None of it is hardware: the store is a board-owned object passed in,
 * the critical section around every read and write is the board's, and what a
 * board adds is its own words for where the container came from (a blob slot,
 * a file on the card).  grove-vision-ai-v2 keeps its own lines: it reads its
 * claims under its NN gate, not through the shared order's counter.
 *
 * [!] THE CLAIMS FOLLOW THE MODEL THAT IS ACTUALLY OPEN.  They are settled only
 * after the backend has adopted (or refused) the model -- the shared order's
 * commit / forget -- never before, because a reload can refuse and leave the
 * PREVIOUS model active, and then these must still be the previous model's.
 *
 * [!] AND THERE IS A WINDOW, SO IT IS NAMED RATHER THAN HIDDEN (the #108
 * adversarial review).  The backend makes the new model visible inside its
 * reload; the claims are settled after it returns.  `nn info` takes no lock, so
 * a console that preempts a background load between those two points would
 * read the new model beside the old claims.  The shared order's transition
 * counter is odd from before the reload to after the claims are settled, and a
 * reader that sees it odd -- in the same critical section as its copy -- says
 * "a load is in progress" instead of reporting either set.  @ref
 * plugin_claims.from names what the claims came with, because `nn info`
 * prints the model and the plugin lines from two separate calls and a load
 * landing BETWEEN them is then visible to whoever reads them.
 *
 * PLAIN DATA: struct plugin_view carries integer offsets and copied bytes, no
 * callable pointer.
 *
 * No mutable storage: the store is the board's.  The store's two accessors are
 * static inline below; the `nn info` lines are plugin_claims.c.
 */
#ifndef PLUGIN_CLAIMS_H
#define PLUGIN_CLAIMS_H

#include <stdint.h>

#include "nn_svc.h"         /* nn_svc_write_fn */
#include "plugin_load.h"    /* struct plugin_view */

#ifdef __cplusplus
extern "C" {
#endif

/** The longest "from" a board may write, terminator included.  Each board
 *  asserts its own worst case fits (a truncated name can read as another). */
#define PLUGIN_CLAIMS_FROM_MAX 96u

/** One container's claims. */
struct plugin_claims {
	struct plugin_view view;
	/** The board's own words for where these bytes came from, formatted when
	 *  the load read them: wio-lite-ai "slot N, blob 'name'", f746g-disco
	 *  "<path>, N B, file crc32 X".  NUL-terminated. */
	char from[PLUGIN_CLAIMS_FROM_MAX];
};

/** Where the board keeps them.  Its static object; every access is inside the
 *  board's critical section. */
struct plugin_claims_store {
	struct plugin_claims c;
	uint8_t              valid;   /**< the open model is a container's */
};

/** What a snapshot found. */
enum plugin_claims_seen {
	PLUGIN_CLAIMS_NONE    = 0,   /**< the open model is not a container's */
	PLUGIN_CLAIMS_VALID   = 1,   /**< *out describes the open model        */
	PLUGIN_CLAIMS_LOADING = 2,   /**< a load / unload is between its steps */
};

/**
 * Settle the claims of the model the load committed (@p c), or none (NULL).
 * Called only by the board's commit / forget hooks of the shared order, inside
 * the board's critical section.
 *
 * Static inline, like svc/nn/nn_svc_adapter.h's helpers: a board's backend that
 * can never open a container still keeps the store (its commit and forget hooks
 * are the shared order's), and this way it links no plugin TU for it.
 */
static inline void plugin_claims_set(struct plugin_claims_store *s,
                                     const struct plugin_claims *c)
{
	if (c != NULL) {
		s->c     = *c;
		s->valid = 1u;
	} else {
		s->valid = 0u;
	}
}

/**
 * Snapshot them: LOADING when @p life_seq (the shared order's transition
 * counter, read in the SAME critical section) is odd; otherwise VALID with
 * @p out filled, or NONE.  Inside the board's critical section.
 */
static inline enum plugin_claims_seen
plugin_claims_get(const struct plugin_claims_store *s, uint32_t life_seq,
                  struct plugin_claims *out)
{
	if ((life_seq & 1u) != 0u)
		return PLUGIN_CLAIMS_LOADING;
	if (!s->valid)
		return PLUGIN_CLAIMS_NONE;
	*out = s->c;
	return PLUGIN_CLAIMS_VALID;
}

/**
 * @brief  Write `nn info`'s plugin lines from one snapshot.
 *
 *   LOADING                -> "plugin  : (a model load is in progress ...)"
 *   NONE                   -> "plugin  : (none) -- reservation ..."
 *   a container, no plugin -> "plugin  : (none in this container) -- ..."
 *                             and its "from" line
 *   a plugin               -> "plugin  : <name> (build, crc), running |
 *                             validated, not loaded", "from", "image", and
 *                             svc/plugin/plugin_info.c's stack lines
 *
 * @param running     whether the loader has branched into a plugin, taken in
 *                    the SAME critical section as the snapshot: asked later, a
 *                    load landing between would print one container's manifest
 *                    beside another's runtime state
 * @param res_base, res_len  the board's .plugin reservation
 * @param veneer_cost the c the board's loader charges (its policy's)
 *
 * Writes nothing with a NULL @p write.
 */
void plugin_claims_info(nn_svc_write_fn write, void *ctx,
                        enum plugin_claims_seen seen,
                        const struct plugin_claims *c, int running,
                        uint32_t res_base, uint32_t res_len,
                        uint32_t veneer_cost);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_CLAIMS_H */
