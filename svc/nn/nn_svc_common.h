/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_common.h
 * @brief   The parts of the `nn` adapter that were word for word the same on
 *          the boards that run them: `nn info`'s model section (wio-lite-ai,
 *          f746g-disco) and `nn thresh` (all three) -- issue #174.
 *
 * WHAT STAYS WITH THE BOARD.  How it reads the shared load order's transition
 * counter (inside its own critical section), and for `nn thresh` the hooks:
 * the plugin lease, the decoder's threshold entries, and -- on a board whose
 * console calls into the plugin are counted from outside its gate
 * (grove-vision-ai-v2, svc/nn/nn_param_calls.h) -- the count's way in and out.
 * The board keeps `nn_svc_info()` / `nn_svc_thresh_get()` / `_set()` as
 * one-line wrappers that pass them.
 *
 * NOT HERE: grove-vision-ai-v2's `nn info` model section, which reads under its
 * gate and its owner word, a different rule (issue #174 D6); and every board's
 * `nn_svc_info_extra()` (the plugin lines), which is already
 * svc/plugin/plugin_claims.c's on all three.
 *
 * [!] NO STORAGE, NO tx_api.h, NO BOARD HEADER.  The hooks are the board's,
 * passed in; the inference API is svc/nn/nn.h's.
 */
#ifndef NN_SVC_COMMON_H
#define NN_SVC_COMMON_H

#include <stdint.h>

#include "nn_svc.h"   /* struct nn_svc_info, NN_SVC_* */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * `nn info`'s backend, model and arena, for a board whose inference goes
 * through svc/nn/nn.h (wio-lite-ai, f746g-disco).
 *
 * The name and the activations figure are copied under the NN session when it
 * is free and copied anyway when it is not (a stream holds it for its whole
 * life, and that is when `nn info` is worth asking); the copy then stands only
 * if the shared load order's counter says no load or unload was between its
 * two reads, nor ran whole in between (nn_core_model_copy_stands()), and is
 * BUSY otherwise.  A backend whose memory another command holds
 * (NN_ERR_SCRATCH_HELD: f746g-disco's `sdram test`) is BUSY too, never "no
 * model".
 *
 * @param life_seq  the board's read of the shared load order's transition
 *                  counter, in its critical section.  A hook, not the order's
 *                  board table: `nn info` is in every build, and a build with
 *                  no `nn model load` must not keep the load path alive
 *                  through it.
 */
void nn_svc_common_info(struct nn_svc_info *out, uint32_t (*life_seq)(void));

/*
 * [!] AND THE COPY CARRIES WHICH STATE IT IS OF (issue #199): the counter's
 * value, read before the session is given back when it was taken, else the
 * value both reads agreed on.  A BUSY copy -- the backend's memory held, or the
 * counter moved -- carries none.  A copy from an open that failed ("nothing
 * loaded", not BUSY) carries the counter when the reads before the open and
 * after the failure agree and are even, and none otherwise: a failed open
 * gives its initialisation back, so another console may open and load in
 * between.
 */

/**
 * nn_svc_info_stands() for a board that answers `nn info` through
 * nn_svc_common_info(): @p life_seq is the same read of the counter.
 */
int nn_svc_common_info_stands(const struct nn_svc_info *info,
                              uint32_t (*life_seq)(void));

/**
 * What `nn thresh` reaches, the board's.
 *
 * [!] get == NULL AND set == NULL IS A BOARD WITH NO DECODER AT ALL (a backend
 * that cannot load a container): get answers NN_SVC_OK with
 * NN_SVC_THRESH_NONE and set NN_SVC_ERR_STATE -- the same answers a build with
 * a decoder gives with none loaded.  Then no other hook is called.
 */
struct nn_svc_thresh_ops {
	/** Count this console call in, before the lease; non-zero if it may
	 *  enter.  NULL on a board that protects the plugin with the lease
	 *  alone. */
	int  (*enter)(void);
	/** Count it out, after the lease is given back.  NULL with @ref enter;
	 *  a table with only one of the two is refused (BUSY, nothing entered). */
	void (*leave)(void);
	/** The plugin lease, bounded; non-zero if held.  Required with get / set:
	 *  missing, the call is BUSY and nothing is entered (fail-closed). */
	int  (*lease_take)(void);
	void (*lease_give)(void);
	/** The decoder's entries (nn_active_get/set_thresh_milli()): they answer
	 *  NN_ACTIVE_THRESH_* (svc/nn/nn_active_core.h) or @ref not_held. */
	int  (*get)(unsigned *milli);
	int  (*set)(unsigned milli);
	/** The board's NN_ACTIVE_NOT_HELD: the entry was reached without the
	 *  lease (an answer of its own; answered BUSY). */
	int  not_held;
};

/**
 * `nn thresh` with no value.  @p milli is NN_SVC_THRESH_NONE unless the
 * decoder answered.  The order, on every exit: count in -> lease -> the entry
 * -> lease back -> count out; a refused count or lease enters nothing.
 *
 * @return NN_SVC_OK, NN_SVC_ERR_ARG (NULL @p milli) or NN_SVC_ERR_BUSY (the
 *         count or the lease refused, or the entry did not answer)
 */
int nn_svc_common_thresh_get(const struct nn_svc_thresh_ops *o,
                             unsigned *milli);

/**
 * `nn thresh <milli>`.  Same order.
 *
 * @return NN_SVC_OK; NN_SVC_ERR_STATE (nothing holds a threshold);
 *         NN_SVC_ERR_BUSY (the count or the lease refused, or not held);
 *         NN_SVC_ERR_ARG (the decoder refused the value, or any other answer)
 */
int nn_svc_common_thresh_set(const struct nn_svc_thresh_ops *o,
                             unsigned milli);

#ifdef __cplusplus
}
#endif

#endif /* NN_SVC_COMMON_H */
