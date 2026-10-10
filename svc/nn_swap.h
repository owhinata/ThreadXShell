/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_swap.h
 * @brief   Where a `nn model load` ends up, and what that obliges -- one table
 *          for every board (issues #122, #131).
 *
 * A load over an open model is a REPLACEMENT with a rollback.  An adapter runs
 * it in one order -- resolve the new model -> hand it to the backend, which
 * either takes it or restores what was open -> only then swap the plugin --
 * and stops at the first step that fails.  Which step that was, and whether a
 * model was open when it began, decide everything that follows: the state the
 * operator is told, whether the status is a failure, whose identity the adapter
 * keeps, whether the plugin is unpublished, whether the hardware goes down and
 * whether the last result goes.
 *
 * TWO WAYS IN, ONE TABLE.  A board that knows where its load stopped names the
 * ending itself (grove-vision-ai-v2 closes and reopens its interpreter step by
 * step).  A board whose backend runs the whole replacement as one call
 * (wio-lite-ai, f746g-disco) only learns what that call returned and whether a
 * model was left; nn_swap_end_of() turns those into the same ending.  Either
 * way nn_swap_decide() is the one list of obligations.
 *
 * WHY IT IS A PURE FUNCTION IN ITS OWN FILE.  The interesting ends cannot be
 * produced from a console on demand: a backend that refuses the new model and
 * then refuses the one it was running a moment ago, or a plugin the device
 * refuses after the host packer ran the device's own validator over it.  So
 * the decision is separated from the code that acts on it and
 * shell/test/test_nn_swap.c walks every entry, for every board's inputs.
 *
 * [!] SHARED, SO IT OWNS NO MUTABLE STORAGE AND KNOWS NO BOARD.  It includes no
 * RTOS or board header; the shared-storage audit compiles it in each board's
 * context.
 *
 * [!] WHAT IT DOES NOT COVER.  This decides; it does not act.  That an adapter
 * swaps the plugin only after the backend took the new model, and acts on
 * every flag it has a use for, is held down by there being one call site per
 * board and it being short -- not by this file.
 */
#ifndef NN_SWAP_H
#define NN_SWAP_H

#ifdef __cplusplus
extern "C" {
#endif

/** The step a load stopped at.  Ordered as the load runs. */
enum nn_swap_end {
	/** Refused before the backend was touched: the name, the CRC, the
	 *  container.  Whatever was open is still open, plugin and all. */
	NN_SWAP_REFUSED = 0,
	/** The backend did not leave the new model, and nothing is open now --
	 *  nothing was, or the previous model could not be reopened either. */
	NN_SWAP_LOST,
	/** The backend refused the new model and the previous one was reopened
	 *  from the same bytes.  Its plugin was never touched. */
	NN_SWAP_RESTORED,
	/** The backend took the new model and its plugin was refused (issue #122
	 *  D6).  The copy into the reservation has already destroyed the previous
	 *  plugin, so the new model stays open with no decoder. */
	NN_SWAP_UNDECODED,
	/** The backend took the new model, and its plugin started or it carries
	 *  none. */
	NN_SWAP_OPENED,
};

/** What the adapter must do.  Every flag is an obligation, not a hint; a board
 *  that has nothing to bring down (no NPU of its own) has no use for hw_down. */
struct nn_swap_verdict {
	unsigned char state;       /**< enum nn_model_state, for the operator      */
	unsigned char ok;          /**< the load's status is success               */
	unsigned char commit;      /**< the NEW identity becomes the adapter's     */
	unsigned char forget;      /**< the adapter's identity is cleared          */
	unsigned char unload;      /**< unpublish the plugin, explicitly           */
	unsigned char hw_down;     /**< close the interpreter and bring the NPU
	                            *   down, which returns the flash lease        */
	unsigned char invalidate;  /**< the last result goes: what is open changed */
};

/**
 * Should the plugin be swapped at all?  Only when the backend TOOK the new
 * model -- a plugin swapped before that, or after a rollback, would destroy the
 * previous model's decoder for nothing (the reservation is one region).
 *
 * @param rc           what the backend's replacement returned (0 = took it)
 * @param model_after  whether it left a model open (its own answer, not a
 *                     question asked afterwards)
 */
int nn_swap_swaps_plugin(int rc, int model_after);

/**
 * The ending of a load whose backend ran the replacement as one call.
 *
 * @param plugin_refused  the plugin was refused after the backend took the new
 *                        model; ignored when nn_swap_swaps_plugin() said no
 *
 * [!] A SUCCESS THAT LEFT NO MODEL IS LOST.  A backend that refuses a model
 * with no inputs cannot produce it -- and if one did, the answer that cannot
 * leave a decoder behind a model that is gone is the one to give.  This never
 * answers REFUSED: that ending is before the backend, where @p rc does not
 * exist yet.
 */
enum nn_swap_end nn_swap_end_of(int rc, int model_after, int plugin_refused);

/**
 * Decide.  @p had_open is whether a model was open when the load began.
 *
 * [!] AN END THAT CONTRADICTS @p had_open, OR ONE THIS DOES NOT KNOW, IS LOST.
 * "Restored" with nothing to restore is not a state the adapter can be in, and
 * the answer that cannot leave a half-open NPU behind is the one that takes
 * everything down.
 */
void nn_swap_decide(int had_open, enum nn_swap_end end,
                    struct nn_swap_verdict *v);

#ifdef __cplusplus
}
#endif

#endif /* NN_SWAP_H */
