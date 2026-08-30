/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active.c
 * @brief   Which decoder is in force.  See nn_active.h.
 */
#include "nn_active.h"

#include "nn_decoder.h"
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
 * conversion is nn_decoder_desc(), reused rather than repeated: a second
 * translation could disagree with the resident path about what a tensor is. */
static unsigned to_desc(const struct npu_tensor *outs, unsigned n,
                        struct tensor_desc *d, unsigned cap)
{
	unsigned i;

	if (n > cap)
		n = cap;
	for (i = 0u; i < n; i++)
		nn_decoder_desc(&d[i], &outs[i]);
	return n;
}

/*
 * [!] THE NULL CHECK IS HERE, NOT IN EACH BRANCH.  nn_decoder.c defends against
 * a null tensor array and a plugin has no way to; routed one side at a time,
 * the two decoders would answer a caller's mistake differently -- the resident
 * one with BF_ERR_ARG, the plugin one by walking a null pointer in to_desc().
 * Neither caller in this firmware can pass null, which is exactly why the
 * asymmetry would have sat here unnoticed.
 */
int nn_active_shapes_ok(const struct npu_tensor *outs, unsigned n)
{
	plugin_shapes_ok_fn fn =
		(plugin_shapes_ok_fn)plugin_run_slot(PLUGIN_SLOT_SHAPES_OK);

	if (outs == NULL)
		return 0;
	if (nn_active_is_plugin() && fn != NULL) {
		struct tensor_desc d[NN_DECODER_MAX_OUTPUTS];
		unsigned m = to_desc(outs, n, d, NN_DECODER_MAX_OUTPUTS);

		return fn(d, m);
	}
	return nn_decoder_shapes_ok(outs, n);
}

int nn_active_decode(const struct npu_tensor *outs, unsigned n,
                     struct bf_det *out, int max, struct bf_result *res)
{
	plugin_decode_fn fn = (plugin_decode_fn)plugin_run_slot(PLUGIN_SLOT_DECODE);

	if (outs == NULL) {
		/* Nothing was decoded, so say so in @p res too rather than leaving the
		 * caller to publish a previous frame's diagnostics -- which is the
		 * rule nn_decoder_run() already follows on this same path. */
		if (res != NULL) {
			memset(res, 0, sizeof(*res));
			res->status = BF_ERR_ARG;
		}
		return BF_ERR_ARG;   /* see nn_active_shapes_ok */
	}
	if (nn_active_is_plugin() && fn != NULL) {
		struct tensor_desc d[NN_DECODER_MAX_OUTPUTS];
		unsigned m = to_desc(outs, n, d, NN_DECODER_MAX_OUTPUTS);

		/* out/res are deliberately untouched -- see the header. */
		(void)out;
		(void)max;
		(void)res;
		return fn(d, m);
	}
	return nn_decoder_run(outs, n, out, max, res);
}

void nn_active_draw(const struct plugin_painter *paint)
{
	plugin_draw_fn fn = (plugin_draw_fn)plugin_run_slot(PLUGIN_SLOT_DRAW);

	if (nn_active_is_plugin() && fn != NULL && paint != NULL)
		fn(paint);
	/* Otherwise nothing: the resident decoder's boxes are drawn by the caller,
	 * which knows what they are. */
}

int nn_active_can_draw(void)
{
	if (!nn_active_is_plugin())
		return 1;
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
 * [!] THE THRESHOLD FOLLOWS THE DECODER THAT WILL USE IT.
 *
 * A plugin owns its own threshold, so setting the firmware's while a plugin is
 * loaded would change a number nothing reads and leave the operator watching
 * `nn thresh` report a value the boxes do not obey.  When no plugin declares
 * the parameter callbacks, the resident decoder answers -- and that is not a
 * fallback for tidiness: a plugin with no parameters has no threshold to set,
 * and saying so through the resident one would be a lie in the other direction.
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
	return nn_decoder_get_thresh_milli();
}

int nn_active_set_thresh_milli(unsigned milli)
{
	plugin_param_set_fn fn =
		(plugin_param_set_fn)plugin_run_slot(PLUGIN_SLOT_PARAM_SET);

	if (nn_active_is_plugin() && fn != NULL)
		return fn(NN_ACTIVE_PARAM_THRESH_MILLI, (uint32_t)milli) == 0;
	return nn_decoder_set_thresh_milli(milli) == BF_OK;
}
