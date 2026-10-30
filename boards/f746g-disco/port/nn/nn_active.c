/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active.c
 * @brief   This board's facts for the shared decoder branch.  See nn_active.h.
 *
 * The decision and every call into the plugin's decoder slots are
 * svc/nn/nn_active_core.c's; the lease-checked entries above it are
 * svc/nn/nn_active_lease.c's (both shared with wio-lite-ai).  What is here is
 * this board's: reading its model's outputs, where a depth sample goes, the
 * transform and the log sink of the base vtable, and the facts the shared
 * files are handed.  tflm only: no other backend can carry a container.
 *
 * [!] EVERY ENTRY INTO THE PLUGIN REQUIRES THE PLUGIN LEASE (issue #130's rule,
 * this board's since #159).  nn_active_lease.c asks plugin_lease_held()
 * (port/plugin/plugin_lease.c) before each of them, and a caller that does not
 * hold the lease gets NN_ACTIVE_NOT_HELD.  nn_active_is_plugin() is the one
 * question left unchecked: the worker and the GUIX handler ask it to decide
 * whether to take the lease at all.
 */
#define LOG_TAG "nn"
#include "log.h"

#include "nn_active.h"

#include "nn_active_core.h"
#include "nn_camera.h"       /* nn_camera_note_depth_at(), the frame geometry */
#include "nn_decoder.h"      /* nn_decoder_desc(): nn_tensor -> tensor_desc   */
#include "plugin_run_core.h"

/* ---- tensors ------------------------------------------------------------- */

/*
 * The loop over the outputs -- the count, the cap, a hole read as a zeroed
 * descriptor -- is the shared entries' (svc/nn/nn_active_lease.c); what is this
 * board's is reading its own model handle and translating one tensor, which is
 * the translation `nn out` and `nn info` already use (nn_decoder_desc()): a
 * second one could disagree with them about what a tensor is.
 */
_Static_assert(NN_MAX_IO <= NN_ACTIVE_MAX_OUTPUTS,
               "the shared entries would not read every output of a model");

static int nn_active_output_count(struct nn_model *m)
{
	return nn_output_count(m);
}

/* 0 with @p d filled; non-zero for an output the backend counts but cannot
 * hand over, which the shared entries turn into a zeroed descriptor. */
static int nn_active_output_desc(struct nn_model *m, unsigned i,
                                 struct tensor_desc *d)
{
	struct nn_tensor *t = nn_output(m, (int)i);

	if (t == NULL)
		return -1;
	nn_decoder_desc(d, t);
	return 0;
}

/* ---- the facts the shared files are handed ------------------------------ */

/*
 * Where a sample goes.  One high-water per THREAD a plugin is entered on, as
 * wio-lite-ai keeps them: decode runs on the inference worker, draw on the GUIX
 * thread in its CAMERA_FRAME handler, and every other slot -- shapes_ok, report
 * and the parameters -- on a console.  entry() is a console slot too, sampled
 * in the shared loader's exec_ok hook (plugin_run_note_entry() below).  The
 * stack pointer is read in the shared function that makes the indirect call,
 * immediately before it (svc/nn/nn_active_core.h).
 */
static void nn_active_note(unsigned slot, uintptr_t sp)
{
	nn_camera_note_depth_at(slot == PLUGIN_SLOT_DECODE ? NNCAM_SITE_DECODE :
	                        slot == PLUGIN_SLOT_DRAW   ? NNCAM_SITE_DRAW :
	                                                     NNCAM_SITE_SHELL,
	                        sp);
}

/* entry(), from the loader's exec_ok hook: a console slot. */
void plugin_run_note_entry(uintptr_t sp)
{
	nn_camera_note_depth_at(NNCAM_SITE_SHELL, sp);
}

static const struct nn_active_board nn_active_board = {
	.active = plugin_run_active,
	.slot   = plugin_run_slot,
	.note   = nn_active_note,
	/*
	 * [!] NOBODY IS GOING TO READ THEM, SO NOBODY OBJECTS -- wio-lite-ai's
	 * answer (issue #116), and the one Step 4c needs here: this is the
	 * admission `nn run` and `nn stream start` pass through, and a bare model
	 * still runs and reports its tensors.  While Step 4b keeps the resident
	 * decoder, the admission asks this only with a plugin in force, so the
	 * answer is not reached yet.
	 */
	.shapes_without_plugin = 1,
};

/* The shared entries' facts (svc/nn/nn_active_lease.h). */
const struct nn_active_lease_board nn_active_facts = {
	.core         = &nn_active_board,
	.output_count = nn_active_output_count,
	.output_desc  = nn_active_output_desc,
};

/* ---- the base vtable ----------------------------------------------------- */

/*
 * The transform: normalised model-input coordinates to pixels of the frame the
 * stream attached with -- see nn_active.h for why that is a multiply here.
 *
 * Non-finite coordinates are rejected before anything is cast: a degenerate
 * model produces them, and the conversion of a NaN to an integer is undefined.
 * The comparison form catches NaN because every comparison with one is false.
 * With no stream attached there is no frame, and nothing is mapped.
 */
int nn_active_to_frame(void *ctx, float x, float y, float w, float h,
                       struct plugin_rect *out)
{
	uint16_t fw, fh;
	float sw, sh, x1f, y1f;

	(void)ctx;
	if (out == NULL)
		return -1;
	if (!(x > -1000.0f && x < 1000.0f) || !(y > -1000.0f && y < 1000.0f) ||
	    !(w > -1000.0f && w < 1000.0f) || !(h > -1000.0f && h < 1000.0f))
		return -1;
	nn_camera_frame_geom(&fw, &fh);
	if (fw == 0u || fh == 0u)
		return -1;
	sw = (float)fw;
	sh = (float)fh;

	x1f = (x + w) * sw;
	y1f = (y + h) * sh;
	out->x0 = (int32_t)(x * sw);
	out->y0 = (int32_t)(y * sh);
	out->x1 = (int32_t)x1f;
	out->y1 = (int32_t)y1f;
	return 0;
}

/*
 * The inference worker has no console, so this is the only way a decode
 * failure can explain itself.
 *
 * [!] THE BYTES GO TO THE LOG WITHOUT THE FORMATTER (issue #112's rule).  The
 * bytes a plugin hands over are not NUL-terminated, and svc/fmt.c implements
 * neither a precision nor `*`.  log_write_bytes() takes them by length; a text
 * too long for the record is cut and ends " ...", and an empty or NULL one
 * writes nothing.  It also keeps the formatter out from below the log veneer,
 * so the veneer cost cmake/check_veneer_base_cost.py derives has no exception.
 */
static void nn_plugin_log(void *ctx, const char *s, size_t len)
{
	(void)ctx;
	if (s == NULL || len == 0u)
		return;
	if (LOG_LEVEL_INF <= LOG_COMPILE_LEVEL)
		log_write_bytes(LOG_LEVEL_INF, LOG_TAG, "plugin: ", s, len, " ...");
}

static const struct plugin_base_api nn_plugin_base = {
	.version  = PLUGIN_ABI_VERSION,
	.size     = sizeof(struct plugin_base_api),
	.ctx      = NULL,
	.log      = nn_plugin_log,
	.to_frame = nn_active_to_frame,
};

const struct plugin_base_api *nn_active_base(void)
{
	return &nn_plugin_base;
}
