/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active.c
 * @brief   Which decoder is in force.  See nn_active.h.
 */
#include "nn_active.h"

#include "npu_desc.h"
#include "nn_preproc.h"
#include "plugin_run.h"

#include <string.h>

#include <stddef.h>

/*
 * The transform the plugin sees.  A copy rather than a pointer: the paths that
 * publish it own storage with different lifetimes, and a plugin asking after
 * one of them had moved on would read whatever was left.
 */
static struct nn_preproc_geom nn_geom_cur;
static uint8_t                nn_geom_cur_ok;

void nn_active_set_geom(const struct nn_preproc_geom *g)
{
	if (g == NULL) {
		nn_geom_cur_ok = 0u;
		return;
	}
	nn_geom_cur    = *g;
	nn_geom_cur_ok = 1u;
}

void nn_active_clear_geom(void)
{
	nn_geom_cur_ok = 0u;
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

int nn_active_is_plugin(void)
{
	return plugin_run_active() && plugin_run_slot(PLUGIN_SLOT_DECODE) != NULL;
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
 * [!] THE NULL CHECK IS HERE, NOT IN A BRANCH.  A plugin has no way to defend
 * against a null tensor array -- to_desc() would walk it -- and this is the one
 * place every caller passes through.  No caller in this firmware can pass null,
 * which is exactly why an omission here would have sat unnoticed.
 */
int nn_active_shapes_ok(const struct npu_tensor *outs, unsigned n)
{
	plugin_shapes_ok_fn fn =
		(plugin_shapes_ok_fn)plugin_run_slot(PLUGIN_SLOT_SHAPES_OK);

	if (outs == NULL)
		return 0;
	if (nn_active_is_plugin() && fn != NULL) {
		struct tensor_desc d[NPU_DESC_MAX_OUTPUTS];
		unsigned m = to_desc(outs, n, d, NPU_DESC_MAX_OUTPUTS);

		return fn(d, m);
	}
	return 0;   /* no decoder: nothing here can read any shape */
}

int nn_active_decode(const struct npu_tensor *outs, unsigned n)
{
	plugin_decode_fn fn = (plugin_decode_fn)plugin_run_slot(PLUGIN_SLOT_DECODE);

	if (outs == NULL)
		return BF_ERR_ARG;   /* see nn_active_shapes_ok */
	if (nn_active_is_plugin() && fn != NULL) {
		struct tensor_desc d[NPU_DESC_MAX_OUTPUTS];
		unsigned m = to_desc(outs, n, d, NPU_DESC_MAX_OUTPUTS);

		return fn(d, m);
	}
	/*
	 * [!] A BACKSTOP, NOT A PATH (issue #104).  There is no decoder in this
	 * firmware any more, so nobody should arrive here: nn_svc_grove.c decides
	 * plugin-or-raw in the one helper both its callers reach, and the stream
	 * refuses admission before a camera is lit.  It still answers rather than
	 * pretending, and it answers "no decoder is bound" -- not BF_ERR_MODEL,
	 * which means "not a detector" and routes to the shared class report.
	 */
	return BF_ERR_UNINIT;
}

void nn_active_draw(const struct plugin_painter *paint)
{
	plugin_draw_fn fn = (plugin_draw_fn)plugin_run_slot(PLUGIN_SLOT_DRAW);

	if (nn_active_is_plugin() && fn != NULL && paint != NULL)
		fn(paint);
	/* Otherwise nothing, and there is nothing else it could be: with no plugin
	 * there is no decoder, so there is no result to paint. */
}

int nn_active_can_draw(void)
{
	if (!nn_active_is_plugin())
		return 0;   /* nothing decodes, so nothing annotates */
	return plugin_run_slot(PLUGIN_SLOT_DRAW) != NULL;
}

int nn_active_report(nn_svc_write_fn write, void *ctx)
{
	plugin_report_fn fn = (plugin_report_fn)plugin_run_slot(PLUGIN_SLOT_REPORT);
	struct plugin_printer out;

	if (!nn_active_is_plugin() || fn == NULL || write == NULL)
		return 0;

	out.ctx   = ctx;
	out.write = write;
	return fn(&out);
}

/*
 * [!] THE THRESHOLD BELONGS TO THE DECODER THAT WILL USE IT, AND THERE MAY BE
 * NONE (issues #103, #104).
 *
 * A plugin owns its own threshold, so before issue #103 routed this, `nn thresh`
 * changed a firmware number the loaded plugin never read.  Since issue #104 the
 * other case is not "the resident decoder answers" but "nobody does": with no
 * plugin there is no decoder, and a classifier plugin declares no parameters
 * because it has no threshold to declare.  Both say so, rather than borrowing a
 * number from something that is not deciding anything.
 */
#define NN_ACTIVE_PARAM_THRESH_MILLI 0u

unsigned nn_active_get_thresh_milli(void)
{
	plugin_param_get_fn fn =
		(plugin_param_get_fn)plugin_run_slot(PLUGIN_SLOT_PARAM_GET);
	uint32_t v = 0u;

	if (nn_active_is_plugin() && fn != NULL &&
	    fn(NN_ACTIVE_PARAM_THRESH_MILLI, &v) == 0)
		return (unsigned)v;
	return NN_SVC_THRESH_NONE;
}

int nn_active_set_thresh_milli(unsigned milli)
{
	plugin_param_set_fn fn =
		(plugin_param_set_fn)plugin_run_slot(PLUGIN_SLOT_PARAM_SET);

	if (nn_active_is_plugin() && fn != NULL)
		return fn(NN_ACTIVE_PARAM_THRESH_MILLI, (uint32_t)milli) == 0
		               ? NN_ACTIVE_THRESH_OK : NN_ACTIVE_THRESH_REFUSED;
	return NN_ACTIVE_THRESH_NO_DECODER;
}
