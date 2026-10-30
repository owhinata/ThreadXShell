/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active.c
 * @brief   Which decoder is in force.  See nn_active.h.
 *
 * The decision and every call into the plugin's decoder slots are
 * svc/nn/nn_active_core.c's, shared with grove-vision-ai-v2 (issue #126), and
 * the lease-checked entries above it are svc/nn/nn_active_lease.c's, shared
 * with f746g-disco (issue #159).  What is here is this board's: the conversion
 * from nn_tensor, the transform and the log sink of the base vtable, and the
 * facts the shared files are handed.
 *
 * [!] EVERY ENTRY INTO THE PLUGIN STILL REQUIRES THE PLUGIN LEASE (issue #130,
 * as grove-vision-ai-v2's since #127) -- the check moved with the entries, it
 * did not go.  nn_active_lease.c asks plugin_lease_held() (this board's
 * port/plugin/plugin_lease.c) before each of them, and a caller that does not
 * hold the lease gets NN_ACTIVE_NOT_HELD.  nn_active_is_plugin() is the one
 * question left unchecked, as on Grove: the worker and the panel ask it to
 * decide whether to take the lease at all.
 */
#define LOG_TAG "nn"
#include "log.h"

#include "nn_active.h"

#include "nn_active_core.h"
#include "nn_camera.h"       /* nn_camera_note_depth_at() */
#include "nn_desc.h"         /* nn_tensor -> tensor_desc */
#include "plugin_run.h"

/* ---- tensors ------------------------------------------------------------- */

/*
 * The outputs reach a plugin as svc/nn/tensor.h descriptors.  The loop over
 * them -- the count, the cap, a hole read as a zeroed descriptor -- is the
 * shared entries' (svc/nn/nn_active_lease.c); what is this board's is reading
 * its own model handle and translating one tensor, which is nn_desc_of(),
 * reused rather than repeated: a second translation could disagree with
 * `nn out` and `nn info` about what a tensor is.
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
	nn_desc_of(d, t);
	return 0;
}

/* ---- the facts the shared files are handed ------------------------------ */

/*
 * Where a sample goes (issue #126).  This board keeps one high-water per
 * THREAD a plugin is entered on, not per slot: decode runs on the worker, draw
 * on the preview thread, and every other slot -- shapes_ok, report and the
 * parameters -- on a console.  entry() is a console slot too, sampled in the
 * loader's exec_ok hook (port/plugin/plugin_run.c).
 *
 * [!] UNTIL ISSUE #126 THESE WERE SAMPLED BY THE CALLERS, before they called
 * into this file, so the frames between -- the descriptor array above all --
 * were not in the number: an under-count, in the unsafe direction.  The stack
 * pointer is now read in the shared function that makes the indirect call,
 * immediately before it.
 */
static void nn_active_note(unsigned slot, uintptr_t sp)
{
	nn_camera_note_depth_at(slot == PLUGIN_SLOT_DECODE ? NNCAM_SITE_DECODE :
	                        slot == PLUGIN_SLOT_DRAW   ? NNCAM_SITE_DRAW :
	                                                     NNCAM_SITE_SHELL,
	                        sp);
}

/* entry(), from the loader's exec_ok hook: a console slot (plugin_run.h). */
void plugin_run_note_entry(uintptr_t sp)
{
	nn_camera_note_depth_at(NNCAM_SITE_SHELL, sp);
}

static const struct nn_active_board nn_active_board = {
	.active = plugin_run_active,
	.slot   = plugin_run_slot,
	.note   = nn_active_note,
	/*
	 * [!] NOBODY IS GOING TO READ THEM, SO NOBODY OBJECTS (issue #116) -- this
	 * board's answer, and not grove-vision-ai-v2's.  This is the admission both
	 * `nn run` and `nn stream start` pass through, and refusing here would
	 * refuse `nn run` on every bare model -- which still runs the inference and
	 * reports the output tensors themselves.  What stops a STREAM with no
	 * decoder is nn_active_can_draw(), one question lower down and only asked
	 * when a panel was requested.
	 */
	.shapes_without_plugin = 1,
};

/* The shared entries' facts (svc/nn/nn_active_lease.h): the branch above, and
 * how to read this board's model. */
const struct nn_active_lease_board nn_active_facts = {
	.core         = &nn_active_board,
	.output_count = nn_active_output_count,
	.output_desc  = nn_active_output_desc,
};

/* ---- the base vtable ----------------------------------------------------- */

/*
 * The transform.  See the header for why it is a constant on this board.
 *
 * Non-finite coordinates are rejected before anything is cast: a degenerate
 * model produces them, and the conversion of a NaN to an integer is undefined.
 * The comparison form catches NaN because every comparison with one is false.
 */
int nn_active_to_frame(void *ctx, float x, float y, float w, float h,
                       struct plugin_rect *out)
{
	const float sw = (float)NN_ACTIVE_FRAME_W;
	const float sh = (float)NN_ACTIVE_FRAME_H;
	float x1f, y1f;

	(void)ctx;
	if (out == NULL)
		return -1;
	if (!(x > -1000.0f && x < 1000.0f) || !(y > -1000.0f && y < 1000.0f) ||
	    !(w > -1000.0f && w < 1000.0f) || !(h > -1000.0f && h < 1000.0f))
		return -1;

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
 * [!] THE BYTES GO TO THE LOG WITHOUT THE FORMATTER (issue #112).  The bytes a
 * plugin hands over are not NUL-terminated.  Until #112 they were copied into a
 * 64-byte stack buffer and terminated so that "%s" could print them (the
 * earlier `LOG_INF("plugin: %.*s", ...)` printed the format string instead:
 * svc/fmt.c implements neither a precision nor `*`).  log_write_bytes() takes
 * them by length, so they need neither the copy nor the buffer; a text too long
 * for the record is cut and ends " ...", and an empty or NULL one writes nothing,
 * as before.  The line is now bounded by the record (LOG_MSG_MAX) rather than by
 * the buffer.
 *
 * It also takes the formatter out from below the log veneer, so what the plugin
 * veneer charges for is derived by cmake/check_veneer_base_cost.py without an
 * exception.
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
