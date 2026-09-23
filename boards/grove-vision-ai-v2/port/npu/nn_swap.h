/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_swap.h
 * @brief   Where a `nn model load` ends up, and what that obliges (issue #122).
 *
 * A load over an open model is a REPLACEMENT with a rollback.  The adapter runs
 * it in one order -- resolve the new model under the lease it already holds ->
 * close the old interpreter -> open the new one -> only then swap the plugin --
 * and stops at the first step that fails.  Which step that was, and whether a
 * model was open when it began, decide everything that follows: the state the
 * operator is told, whether the status is a failure, whose identity the adapter
 * keeps, whether the plugin is unpublished, whether the NPU goes down and
 * whether the last result goes with it.
 *
 * WHY IT IS A PURE FUNCTION IN ITS OWN FILE.  The interesting ends cannot be
 * produced from a console on demand: a backend that refuses the new model and
 * then refuses the one it was running a moment ago needs an interpreter that
 * fails AllocateTensors() on bytes it accepted before.  So the decision is
 * separated from the code that acts on it and a host test walks every entry,
 * the same arrangement port/npu/nn_stream_state.c has for the stop.
 *
 * [!] WHAT IT DOES NOT COVER.  This decides; it does not act.  That the adapter
 * swaps the plugin only after the backend took the new model, and resolves into
 * staging rather than into its own state, is held down by there being one call
 * site and it being short -- not by this file.
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
	/** The backend refused the new model and nothing is open now -- nothing
	 *  was, or the previous model could not be reopened either. */
	NN_SWAP_LOST,
	/** The backend refused the new model and the previous one was reopened
	 *  from the same bytes, under the same lease.  Its plugin was never
	 *  touched. */
	NN_SWAP_RESTORED,
	/** The backend took the new model and its plugin was refused (issue #122
	 *  D6).  The copy into the reservation has already destroyed the previous
	 *  plugin, so the new model stays open with no decoder. */
	NN_SWAP_UNDECODED,
	/** The backend took the new model, and its plugin started or it carries
	 *  none. */
	NN_SWAP_OPENED,
};

/** What the adapter must do.  Every flag is an obligation, not a hint. */
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
