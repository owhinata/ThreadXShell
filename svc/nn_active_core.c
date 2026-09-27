/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active_core.c
 * @brief   Whether a plugin decodes, and every call into its decoder slots.
 *          See nn_active_core.h.
 *
 * [!] NO MUTABLE STORAGE and no lock: the board's struct is const, the plugin's
 * state is the plugin's, and who may call in is the board's to decide.
 */
#include "nn_active_core.h"

/* The parameter id the threshold travels as (asset/plugins/blazeface). */
#define NN_ACTIVE_PARAM_THRESH_MILLI 0u

int nn_active_core_is_plugin(const struct nn_active_board *b)
{
	return b->active() && b->slot(PLUGIN_SLOT_DECODE) != NULL;
}

/*
 * [!] THE NULL CHECK IS HERE, NOT IN A BRANCH.  A plugin has no way to defend
 * against a null tensor array, and this is the one place every caller passes
 * through.
 */
int nn_active_core_shapes_ok(const struct nn_active_board *b,
                             const struct tensor_desc *d, unsigned n)
{
	plugin_shapes_ok_fn fn =
		(plugin_shapes_ok_fn)b->slot(PLUGIN_SLOT_SHAPES_OK);

	if (d == NULL)
		return 0;
	if (nn_active_core_is_plugin(b) && fn != NULL) {
		b->note(PLUGIN_SLOT_SHAPES_OK, NN_ACTIVE_CORE_SP());
		return fn(d, n);
	}
	/* The board's answer, and the boards differ on purpose: see the header. */
	return b->shapes_without_plugin;
}

int nn_active_core_decode(const struct nn_active_board *b,
                          const struct tensor_desc *d, unsigned n)
{
	plugin_decode_fn fn = (plugin_decode_fn)b->slot(PLUGIN_SLOT_DECODE);

	if (d == NULL)
		return BF_ERR_ARG;
	if (nn_active_core_is_plugin(b) && fn != NULL) {
		b->note(PLUGIN_SLOT_DECODE, NN_ACTIVE_CORE_SP());
		return fn(d, n);
	}
	/*
	 * [!] A BACKSTOP, NOT A PATH (issues #104, #116).  Neither board carries a
	 * decoder of its own, and both ask whether a plugin is in force before
	 * they get here.  A caller arriving anyway is told nothing is bound -- not
	 * BF_ERR_MODEL, which means "not a detector".
	 */
	return BF_ERR_UNINIT;
}

void nn_active_core_draw(const struct nn_active_board *b,
                         const struct plugin_painter *paint)
{
	plugin_draw_fn fn = (plugin_draw_fn)b->slot(PLUGIN_SLOT_DRAW);

	if (nn_active_core_is_plugin(b) && fn != NULL && paint != NULL) {
		b->note(PLUGIN_SLOT_DRAW, NN_ACTIVE_CORE_SP());
		fn(paint);
	}
	/* Otherwise nothing: with no plugin there is no result to paint. */
}

int nn_active_core_can_draw(const struct nn_active_board *b)
{
	if (!nn_active_core_is_plugin(b))
		return 0;   /* nothing decodes, so nothing annotates */
	return b->slot(PLUGIN_SLOT_DRAW) != NULL;
}

int nn_active_core_can_report(const struct nn_active_board *b)
{
	if (!nn_active_core_is_plugin(b))
		return 0;   /* there is no result for anyone to describe */
	return b->slot(PLUGIN_SLOT_REPORT) != NULL;
}

int nn_active_core_report(const struct nn_active_board *b,
                          nn_svc_write_fn write, void *ctx)
{
	plugin_report_fn fn = (plugin_report_fn)b->slot(PLUGIN_SLOT_REPORT);
	struct plugin_printer out;

	if (!nn_active_core_is_plugin(b) || fn == NULL || write == NULL)
		return 0;

	/* Version and size first: the plugin's veneer refuses a printer without
	 * them (issue #111). */
	out.version = PLUGIN_ABI_VERSION;
	out.size    = (uint32_t)sizeof(out);
	out.ctx     = ctx;
	out.write   = write;
	b->note(PLUGIN_SLOT_REPORT, NN_ACTIVE_CORE_SP());
	return fn(&out);
}

/*
 * [!] THE THRESHOLD BELONGS TO THE DECODER THAT WILL USE IT, AND THERE MAY BE
 * NONE (issues #103, #104, #116).  A plugin owns its own, so `nn thresh` has to
 * reach it here; with no plugin -- or a plugin that declares no parameters,
 * such as the classifier -- nothing holds one, and that is said rather than
 * borrowing a number from something that is not deciding anything.
 */
unsigned nn_active_core_get_thresh_milli(const struct nn_active_board *b)
{
	plugin_param_get_fn fn =
		(plugin_param_get_fn)b->slot(PLUGIN_SLOT_PARAM_GET);
	uint32_t v = 0u;

	if (!nn_active_core_is_plugin(b) || fn == NULL)
		return NN_SVC_THRESH_NONE;
	b->note(PLUGIN_SLOT_PARAM_GET, NN_ACTIVE_CORE_SP());
	if (fn(NN_ACTIVE_PARAM_THRESH_MILLI, &v) == 0)
		return (unsigned)v;
	return NN_SVC_THRESH_NONE;
}

int nn_active_core_set_thresh_milli(const struct nn_active_board *b,
                                    unsigned milli)
{
	plugin_param_set_fn fn =
		(plugin_param_set_fn)b->slot(PLUGIN_SLOT_PARAM_SET);

	if (!nn_active_core_is_plugin(b) || fn == NULL)
		return NN_ACTIVE_THRESH_NO_DECODER;
	b->note(PLUGIN_SLOT_PARAM_SET, NN_ACTIVE_CORE_SP());
	return fn(NN_ACTIVE_PARAM_THRESH_MILLI, (uint32_t)milli) == 0
	               ? NN_ACTIVE_THRESH_OK : NN_ACTIVE_THRESH_REFUSED;
}
