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
#include "plugin_run.h"

#include <stddef.h>

/*
 * Turn a slot offset into something callable.
 *
 * [!] THE ONE PLACE THIS HAPPENS.  svc/plugin_load.c deliberately returns
 * integers and no function pointers, so that "the validation step does not
 * execute anything" is a property of its types.  Here is where the offsets
 * become addresses, and it is guarded by plugin_run_active() -- which is only
 * true after the loader has copied the image, synchronised the caches, checked
 * the MPU and had the plugin's own entry point accept.
 *
 * The Thumb bit is kept: the manifest carried it, plugin_load.c insisted on it,
 * and stripping it here would produce a branch to Arm state on a core that has
 * none.
 */
static void *slot_addr(unsigned slot)
{
	const struct plugin_view *v = plugin_run_view();

	if (v == NULL || slot >= PLUGIN_SLOT_COUNT)
		return NULL;
	if (v->slot[slot] == PLUGIN_SLOT_ABSENT)
		return NULL;
	return (void *)(uintptr_t)(v->link_addr + v->slot[slot]);
}

int nn_active_is_plugin(void)
{
	return plugin_run_active() && slot_addr(PLUGIN_SLOT_DECODE) != NULL;
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

int nn_active_shapes_ok(const struct npu_tensor *outs, unsigned n)
{
	plugin_shapes_ok_fn fn =
		(plugin_shapes_ok_fn)slot_addr(PLUGIN_SLOT_SHAPES_OK);

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
	plugin_decode_fn fn = (plugin_decode_fn)slot_addr(PLUGIN_SLOT_DECODE);

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
	plugin_draw_fn fn = (plugin_draw_fn)slot_addr(PLUGIN_SLOT_DRAW);

	if (nn_active_is_plugin() && fn != NULL && paint != NULL)
		fn(paint);
	/* Otherwise nothing: the resident decoder's boxes are drawn by the caller,
	 * which knows what they are. */
}

int nn_active_report(nn_svc_write_fn write, void *ctx)
{
	plugin_report_fn fn = (plugin_report_fn)slot_addr(PLUGIN_SLOT_REPORT);
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
		(plugin_param_get_fn)slot_addr(PLUGIN_SLOT_PARAM_GET);
	uint32_t v = 0u;

	if (nn_active_is_plugin() && fn != NULL &&
	    fn(NN_ACTIVE_PARAM_THRESH_MILLI, &v) == 0)
		return (unsigned)v;
	return nn_decoder_get_thresh_milli();
}

int nn_active_set_thresh_milli(unsigned milli)
{
	plugin_param_set_fn fn =
		(plugin_param_set_fn)slot_addr(PLUGIN_SLOT_PARAM_SET);

	if (nn_active_is_plugin() && fn != NULL)
		return fn(NN_ACTIVE_PARAM_THRESH_MILLI, (uint32_t)milli) == 0;
	return nn_decoder_set_thresh_milli(milli) == BF_OK;
}
