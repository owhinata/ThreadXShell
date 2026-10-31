/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active_lease.h
 * @brief   Every entry into the plugin's decoder slots, behind the plugin
 *          lease, for a board whose model API is `struct nn_model` (issue #159,
 *          moved out of wio-lite-ai's port/nn/nn_active.c).
 *
 * svc/nn/nn_active_core.c makes the decision and the indirect calls; it does
 * not know that a lease exists.  This file is the layer above it that does:
 * each entry below asks whether the CALLING THREAD holds the plugin lease
 * (svc/plugin/plugin_lease_api.h, implemented by the board) before anything
 * reaches a plugin callback or the slot table a holder is about to call
 * through, and a caller that does not gets NN_ACTIVE_NOT_HELD.
 *
 * [!] ONE COPY FOR THE BOARDS THAT SHARE THE SHAPE.  wio-lite-ai wrote this as
 * its own port/nn/nn_active.c (issue #110, lease checks #130); f746g-disco needs
 * exactly the same entries, the same answers and the same counting, and nothing
 * about them is hardware.  So they live here once, and a board supplies only
 * its facts (struct nn_active_lease_board): the shared branch's board struct,
 * and how to read its model's outputs.  grove-vision-ai-v2 keeps its own
 * wrappers: its entries take caller-collected `struct nn_tensor` arrays (the
 * worker reads them once per frame) and a geometry this shape has no place
 * for.
 *
 * [!] ONE BRANCH POINT, NOT ONE PER CALLER (issues #103, #110).  Every way a
 * board's code reaches a plugin's decoder slots is a function below -- the
 * worker's decode, the panel's draw, the consoles' report and threshold, the
 * stream admission's shape question.  A plugin carries its OWN threshold, so
 * with the routing done in one place and not the others, `nn thresh 700` would
 * change a number the plugin never read -- a divergence a differential test
 * giving two decoders the same threshold would never see.
 *
 * [!] AND A PLUGIN'S RESULT IS PRIVATE.  The firmware does not know its shape,
 * which is the entire point of issue #78, so a plugin draws and reports it
 * itself.  @ref nn_active_decode returns a count and nothing else.
 *
 * [!] WITH NO PLUGIN, NOTHING DECODES (issues #104, #116).  These boards carry no
 * decoder of their own once a container is the only way one arrives, so each
 * answer below has a no-plugin form, and every one of them has to agree that
 * there is nobody: no threshold, nothing painted, no words.  The shape question
 * is the one exception -- see @ref nn_active_shapes_ok.
 *
 * Owns no mutable storage (svc/, audited per board).  The descriptors it builds
 * are on the caller's stack, in a frame outside the one the depth is sampled in
 * (svc/nn/nn_active_core.h), so they are inside the number.
 */
#ifndef NN_ACTIVE_LEASE_H
#define NN_ACTIVE_LEASE_H

#include <stddef.h>
#include <stdint.h>

#include "blazeface.h"      /* BF_ERR_* -- the shared decode vocabulary */
#include "nn_active_core.h" /* struct nn_active_board, NN_ACTIVE_THRESH_* */
#include "nn_svc.h"         /* nn_svc_write_fn, NN_SVC_THRESH_NONE */
#include "plugin_abi.h"
#include "tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The board's model handle.  Opaque here, as it is in the board's own nn.h. */
struct nn_model;

/**
 * The caller does not hold the plugin lease, so nothing was entered (issue
 * #130; grove-vision-ai-v2's since #127, and the same value).
 *
 * [!] AN ANSWER OF ITS OWN.  Not 0 ("no plugin", or "the plugin said no"), not
 * a BF_ERR_* (the decoder's own refusal), not an NN_ACTIVE_THRESH_* -- a path
 * that reached the plugin without the lease is a firmware bug, and folding it
 * into an answer the operator already knows would send them to a different
 * model or container instead.  Every refusal is also counted, once, by the
 * entry check itself (plugin_lease_unheld()).
 *
 * [!] AND IT IS NEGATIVE, SO `if (!answer)` IS NOT A TEST FOR "NO".  The
 * yes/no entries below return 1, 0 or this; compare against 1.
 */
#define NN_ACTIVE_NOT_HELD (-64)
_Static_assert(NN_ACTIVE_NOT_HELD != BF_ERR_MODEL &&
               NN_ACTIVE_NOT_HELD != BF_ERR_UNINIT &&
               NN_ACTIVE_NOT_HELD != BF_ERR_ARG &&
               NN_ACTIVE_NOT_HELD != NN_ACTIVE_THRESH_OK &&
               NN_ACTIVE_NOT_HELD != NN_ACTIVE_THRESH_REFUSED &&
               NN_ACTIVE_NOT_HELD != NN_ACTIVE_THRESH_NO_DECODER,
               "NOT_HELD must not collide with another answer");

/**
 * How many output descriptors an entry builds, on its caller's stack.  A board
 * whose model can have more outputs than this states so at build time against
 * its own bound (it asserts its NN_MAX_IO is not larger); more are not read.
 */
#define NN_ACTIVE_MAX_OUTPUTS 8u

/** What a board hands this file.  A constant in the board's .rodata. */
struct nn_active_lease_board {
	/** The shared branch's facts: is a plugin live, where its slots are,
	 *  where a depth is recorded, the no-plugin shape answer. */
	const struct nn_active_board *core;
	/** How many outputs @p m has; negative is read as none. */
	int (*output_count)(struct nn_model *m);
	/**
	 * Output @p i of @p m as a neutral descriptor (the board's own tensor
	 * translation).  0 with @p d filled, or non-zero for an output the backend
	 * counts but cannot hand over -- a hole, which this file turns into a
	 * zeroed descriptor.
	 */
	int (*output_desc)(struct nn_model *m, unsigned i, struct tensor_desc *d);
};

/**
 * The board's facts.  DEFINED BY THE BOARD (port/nn/nn_active.c), declared here
 * the way svc/plugin/plugin_lease_api.h declares the lease: the entries below
 * keep the names the board's callers already use, so there is no second set of
 * one-line forwarders per board.
 */
extern const struct nn_active_lease_board nn_active_facts;

/** Is a plugin in force?  When it is, the result belongs to it.
 *
 *  The one question NOT checked against the lease: the worker and the panel
 *  ask it to decide whether to take the lease at all. */
int nn_active_is_plugin(void);

/**
 * @brief  Can the active decoder read this model's outputs?
 *
 * Asked before something expensive is committed to -- lighting a camera and
 * finding out per frame that the wrong model is open is a stream that runs and
 * silently never annotates.
 *
 * [!] WITH NO PLUGIN THIS ANSWERS THE BOARD'S shapes_without_plugin, AND ON THE
 * BOARDS THAT USE THIS FILE THAT IS YES (issue #116).  Nothing is going to read
 * the outputs, so nothing can object to their shape -- and this is the
 * admission `nn run` passes through too, which on a bare model runs the
 * inference and reports the tensors themselves.  A stream with no decoder is
 * stopped one question further down, by @ref nn_active_can_draw.
 *
 * Under the plugin lease.  A NULL @p m is refused (0) before the lease is asked.
 *
 * @return 1, 0, or NN_ACTIVE_NOT_HELD
 */
int nn_active_shapes_ok(struct nn_model *m);

/**
 * @brief  Decode this model's current outputs with the PLUGIN.
 *
 * @return the plugin's own count, a negative BF_ERR_*, or NN_ACTIVE_NOT_HELD.
 *         The RESULT ITSELF stays with the plugin -- ask it to draw or to
 *         report.  A NULL @p m is BF_ERR_ARG, before the lease is asked.
 *
 * Only ever called when @ref nn_active_is_plugin: with no plugin the worker
 * publishes "an inference ran and nothing decoded it" instead of calling this
 * at all.  Reached anyway it answers BF_ERR_UNINIT -- deliberately not
 * BF_ERR_MODEL, which means "not a detector" and routes to the shared class
 * report.
 *
 * [!] THE CALLER MUST HOLD THE PLUGIN LEASE, and this checks that it does: on
 * a board where the panel outranks the worker, a draw could otherwise read
 * state this call is halfway through writing.
 */
int nn_active_decode(struct nn_model *m);

/**
 * Let the active decoder paint its own result.  A no-op with no plugin.
 * Called on the panel's thread, under the plugin lease and then the frame lock.
 * @return 0, or NN_ACTIVE_NOT_HELD when nothing was painted for that reason
 */
int nn_active_draw(const struct plugin_painter *paint);

/**
 * @brief  Will the active decoder put anything on the panel?
 *
 * A plugin need not draw: DRAW is an optional slot.  A caller about to light a
 * camera and a panel has to know, because "the stream runs and never
 * annotates" is indistinguishable from a broken one.
 *
 * [!] AND WITH NO PLUGIN THE ANSWER IS NO (issue #116): this is the ONE
 * question that refuses a live overlay on a bare model.  @ref
 * nn_active_shapes_ok deliberately does not, because it is also `nn run`'s
 * admission.
 *
 * Reads the slot table rather than calling into it, and is checked anyway: what
 * it answers is about the plugin a holder is about to call, and a load
 * replacing it between the question and the call is what the lease excludes.
 *
 * @return 1, 0, or NN_ACTIVE_NOT_HELD
 */
int nn_active_can_draw(void);

/** Will it describe its result in words?  REPORT is an optional slot, and
 *  "it said nothing" and "it has nothing to say with" are different answers.
 *  @return 1, 0, or NN_ACTIVE_NOT_HELD */
int nn_active_can_report(void);

/** Ask it to.  Returns what its own report returned, 0 with no plugin, or
 *  NN_ACTIVE_NOT_HELD. */
int nn_active_report(nn_svc_write_fn write, void *ctx);

/**
 * The threshold, from whichever decoder will actually use it.
 *
 * [!] A PLUGIN OWNS ITS OWN.  Routed here, `nn thresh` reaches the number that
 * decides something.  A plugin that declares no parameters has none, and says
 * so -- as does a board with no plugin loaded, which holds no threshold
 * anywhere: NN_SVC_THRESH_NONE from the getter and NN_ACTIVE_THRESH_NO_DECODER
 * from the setter, which is not the same answer as refusing the value.
 *
 * [!] A STATUS AND AN OUT-PARAMETER (issue #130; Grove's form since #127).  The
 * value alone had no room for "the caller does not hold the lease": every
 * unsigned is either a threshold or NN_SVC_THRESH_NONE, and the second is an
 * answer the operator reads as "this decoder has no threshold".
 *
 * @return NN_ACTIVE_THRESH_OK with @p milli set, NN_ACTIVE_NOT_HELD (and
 *         @p milli NN_SVC_THRESH_NONE), or NN_ACTIVE_THRESH_REFUSED for a null
 *         @p milli
 */
int nn_active_get_thresh_milli(unsigned *milli);

/** Set it: NN_ACTIVE_THRESH_OK, _REFUSED (the decoder rejected the value) or
 *  _NO_DECODER (nothing holds a threshold) -- svc/nn/nn_active_core.h -- or
 *  NN_ACTIVE_NOT_HELD. */
int nn_active_set_thresh_milli(unsigned milli);

#ifdef __cplusplus
}
#endif

#endif /* NN_ACTIVE_LEASE_H */
