/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_bindings.c
 * @brief   The vtables a plugin callback is handed, filled from the board's
 *          table, and the record that says which word is which veneer (issue
 *          #165).  See plugin_bindings.h.
 *
 * No mutable storage: the table is the board's const object, the vtables are
 * the caller's.
 */
#include "plugin_bindings.h"

/*
 * [!] THE VENEER NAMES ARE THE PLUGIN'S (asset/common/plugin_base.c), and the
 * offsets are the struct's: a field moved in the struct moves here with it.
 * shell/test/test_plugin_bindings.c holds each pair against the member the
 * plugin's veneer of that name really calls through.
 */
__attribute__((used)) const struct plugin_bind_probe plugin_bind_probe = {
	PLUGIN_BIND_PROBE_MAGIC,
	(uint32_t)sizeof(struct plugin_bindings),
	PLUGIN_BIND_COUNT,
	{
		{ (uint32_t)offsetof(struct plugin_bindings, base.log),
		  "pl_base_log" },
		{ (uint32_t)offsetof(struct plugin_bindings, base.to_frame),
		  "pl_base_to_frame" },
		{ (uint32_t)offsetof(struct plugin_bindings, rect),
		  "pl_paint_rect" },
		{ (uint32_t)offsetof(struct plugin_bindings, fill_rect),
		  "pl_paint_fill_rect" },
		{ (uint32_t)offsetof(struct plugin_bindings, blit),
		  "pl_paint_blit" },
		{ (uint32_t)offsetof(struct plugin_bindings, write),
		  "pl_print_write" },
	},
};

void plugin_painter_init(struct plugin_painter *p, void *ctx)
{
	if (p == NULL)
		return;
	p->version   = PLUGIN_ABI_VERSION;
	p->size      = (uint32_t)sizeof(*p);
	p->ctx       = ctx;
	p->rect      = plugin_board_bindings.rect;
	p->fill_rect = plugin_board_bindings.fill_rect;
	p->blit      = plugin_board_bindings.blit;
}

void plugin_printer_init(struct plugin_printer *o, void *ctx)
{
	if (o == NULL)
		return;
	o->version = PLUGIN_ABI_VERSION;
	o->size    = (uint32_t)sizeof(*o);
	o->ctx     = ctx;
	o->write   = plugin_board_bindings.write;
}
