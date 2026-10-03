/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active.c
 * @brief   Which decoder is in force.  See nn_active.h.
 *
 * The decision and every call into the plugin's decoder slots are
 * svc/nn_active_core.c's, shared with wio-lite-ai (issue #126).  What is here
 * is this board's: the geometry and the transform, the conversion from
 * npu_tensor, and the three facts the shared file is handed below.
 *
 * [!] AND EVERY ENTRY INTO THE PLUGIN REQUIRES THE PLUGIN LEASE (issue #127).
 * Each wrapper below that can reach a plugin callback -- and the two that
 * write the geometry a callback reads -- asks nn_active_held() first, and a
 * caller that does not hold the lease gets NN_ACTIVE_NOT_HELD: not "no
 * plugin", not a decoder's own error, an answer of its own.  The shared file
 * stays ignorant of the lease (svc/nn_active_core.h); the board enforces it at
 * the board's branch point, which is the only way in.
 */
#include "nn_active.h"

#include "npu_desc.h"
#include "nn_preproc.h"
#include "nn_probe.h"
#include "nn_active_core.h"
#include "plugin_run.h"
#include "plugin_lease.h"

#include <string.h>

#include <stddef.h>

/*
 * The transform the plugin sees.  A copy rather than a pointer: the paths that
 * publish it own storage with different lifetimes, and a plugin asking after
 * one of them had moved on would read whatever was left.
 */
static struct nn_preproc_geom nn_geom_cur;
static uint8_t                nn_geom_cur_ok;

/*
 * Does the calling thread hold the plugin lease?  Counted when it does not, here
 * and only here, so every refusal is counted exactly once whoever forgot.
 */
static int nn_active_held(void)
{
	if (plugin_lease_held())
		return 1;
	plugin_lease_note_unheld();
	return 0;
}

/*
 * [!] THE GEOMETRY IS PART OF THE PLUGIN'S RESULT (issue #127), so writing it
 * needs the lease as much as calling the plugin does: to_frame() reads it from
 * inside draw() and report(), and a write by a thread that does not hold the
 * lease could land between a decode and the draw of the same frame.  Refused,
 * the previous geometry stands.
 */
int nn_active_set_geom(const struct nn_preproc_geom *g)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	if (g == NULL) {
		nn_geom_cur_ok = 0u;
		return 0;
	}
	nn_geom_cur    = *g;
	nn_geom_cur_ok = 1u;
	return 0;
}

int nn_active_clear_geom(void)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	nn_geom_cur_ok = 0u;
	return 0;
}

int nn_active_to_frame(void *ctx, float x, float y, float w, float h,
                       struct plugin_rect *out)
{
	struct nn_preproc_box b;

	(void)ctx;
	if (out == NULL || !nn_geom_cur_ok)
		return -1;
	if (nn_preproc_box(&nn_geom_cur, x, y, w, h, &b) != 0)
		return -1;
	out->x0 = b.x0;
	out->y0 = b.y0;
	out->x1 = b.x1;
	out->y1 = b.y1;
	return 0;
}

/* ---- the three facts the shared branch is handed ---------------------- */

/* Every sample the shared file takes goes to this board's per-slot, per-thread
 * record (issue #119).  The stack pointer was read in the shared function that
 * makes the call, immediately before it; nothing is pushed in between. */
static void nn_active_note(unsigned slot, uintptr_t sp)
{
	nn_probe_note(slot, sp, 0u);
}

static const struct nn_active_board nn_active_board = {
	.active = plugin_run_active,
	.slot   = plugin_run_slot,
	.note   = nn_active_note,
	/* [!] NO PLUGIN, NO SHAPE IS READABLE -- this board's answer, and not
	 * wio-lite-ai's.  Here the question is asked by the stream's admission
	 * only, and nothing without a plugin can read any shape; refusing is what
	 * stops a camera being lit for a preview that never annotates. */
	.shapes_without_plugin = 0,
};

int nn_active_is_plugin(void)
{
	return nn_active_core_is_plugin(&nn_active_board);
}

/* The tensors reach a plugin as svc/tensor.h descriptors, which is the contract
 * issue #97 established so that one decoder can read any board's tensors.  The
 * conversion is npu_desc_of(), reused rather than repeated: a second translation
 * could disagree with `nn out` and `nn info` about what a tensor is. */
static unsigned to_desc(const struct npu_tensor *outs, unsigned n,
                        struct tensor_desc *d, unsigned cap)
{
	unsigned i;

	if (n > cap)
		n = cap;
	for (i = 0u; i < n; i++)
		npu_desc_of(&d[i], &outs[i]);
	return n;
}

/*
 * [!] THE NULL CHECK IS HERE AS WELL AS IN THE SHARED FILE.  to_desc() would
 * walk a null tensor array before the shared file ever saw it.  No caller in
 * this firmware can pass null, which is exactly why an omission here would
 * have sat unnoticed.
 */
int nn_active_shapes_ok(const struct npu_tensor *outs, unsigned n)
{
	struct tensor_desc d[NPU_DESC_MAX_OUTPUTS];

	if (outs == NULL)
		return 0;
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	n = to_desc(outs, n, d, NPU_DESC_MAX_OUTPUTS);
	return nn_active_core_shapes_ok(&nn_active_board, d, n);
}

int nn_active_decode(const struct npu_tensor *outs, unsigned n)
{
	struct tensor_desc d[NPU_DESC_MAX_OUTPUTS];

	if (outs == NULL)
		return BF_ERR_ARG;   /* see nn_active_shapes_ok */
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	n = to_desc(outs, n, d, NPU_DESC_MAX_OUTPUTS);
	return nn_active_core_decode(&nn_active_board, d, n);
}

int nn_active_draw(const struct plugin_painter *paint)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	nn_active_core_draw(&nn_active_board, paint);
	return 0;
}

/* can_draw and can_report read the plugin's slot table rather than call into
 * it, and are checked anyway: what they answer is about the plugin a holder is
 * about to call, and a load replacing it between the question and the call is
 * exactly what the lease excludes. */
int nn_active_can_draw(void)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	return nn_active_core_can_draw(&nn_active_board);
}

int nn_active_can_report(void)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	return nn_active_core_can_report(&nn_active_board);
}

int nn_active_report(nn_svc_write_fn write, void *ctx)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	return nn_active_core_report(&nn_active_board, write, ctx);
}

int nn_active_get_thresh_milli(unsigned *milli)
{
	if (milli == NULL)
		return NN_ACTIVE_THRESH_REFUSED;
	*milli = NN_SVC_THRESH_NONE;
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	*milli = nn_active_core_get_thresh_milli(&nn_active_board);
	return NN_ACTIVE_THRESH_OK;
}

int nn_active_set_thresh_milli(unsigned milli)
{
	if (!nn_active_held())
		return NN_ACTIVE_NOT_HELD;
	return nn_active_core_set_thresh_milli(&nn_active_board, milli);
}
