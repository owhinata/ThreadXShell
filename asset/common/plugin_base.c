/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_base.c
 * @brief   Named veneers for the base vtable (issue #101).
 *
 * [!] THE ONLY PLACE A PLUGIN MAY CALL THROUGH A FUNCTION POINTER.  Every call
 * into the base goes through one of these, and nothing else in the image is
 * allowed an indirect call at all.
 *
 * The reason is the stack gate.  A plugin's stack budget has to be a TRANSITIVE
 * bound, and -fstack-usage supplies only per-function frames: it has no call
 * edges, and a linked ELF keeps no record of which C type an indirect `blx`
 * came from.  An analyser meeting a bare `blx r3` can only fail closed.
 *
 * Funnelling the vtable through named functions turns each one into an ORDINARY
 * DIRECT EDGE the analyser can follow, and leaves exactly one indirect call per
 * veneer -- at a site whose slot is known by name, so the base's own worst-case
 * cost for that slot can be added there.  This repo has solved the same problem
 * once before: the NOR seam funnels vendor writes through named `--wrap`
 * wrappers and judges the result from ld's map rather than the ELF, for the same
 * reason -- what the compiler knew is gone by the time the linker is done.
 *
 * These are deliberately not `static inline`: inlining would put the indirect
 * call back into the caller and undo the whole arrangement.
 *
 * [!] THE PAINTER AND PRINTER VENEERS CHECK BEFORE THEY CALL (issue #111).  Both
 * vtables now lead with a version and a size, like struct plugin_base_api, and
 * each veneer calls through a member only when the version is this ABI's and
 * the size reaches that member (PLUGIN_CALLS_HAS).  A painter too short or of
 * another version draws nothing; a printer of either kind refuses the write, so
 * the plugin's report propagates a failure as it already must for a sink that
 * said no.  The base vtable is checked once, in each plugin's entry, and keeps
 * that arrangement.
 */
#include "plugin_base.h"

void pl_base_log(const struct plugin_base_api *base, const char *s, size_t len)
{
	base->log(base->ctx, s, len);
}

int pl_base_to_frame(const struct plugin_base_api *base, float x, float y,
                     float w, float h, struct plugin_rect *out)
{
	return base->to_frame(base->ctx, x, y, w, h, out);
}

void pl_paint_rect(const struct plugin_painter *p, const struct plugin_rect *r,
                   uint16_t rgb565, uint16_t stroke)
{
	if (!PLUGIN_CALLS_HAS(p, struct plugin_painter, rect))
		return;
	p->rect(p->ctx, r, rgb565, stroke);
}

void pl_paint_fill_rect(const struct plugin_painter *p,
                        const struct plugin_rect *r, uint16_t rgb565)
{
	if (!PLUGIN_CALLS_HAS(p, struct plugin_painter, fill_rect))
		return;
	p->fill_rect(p->ctx, r, rgb565);
}

void pl_paint_blit(const struct plugin_painter *p, const struct plugin_rect *r,
                   const uint16_t *src, uint32_t src_stride, int32_t key)
{
	if (!PLUGIN_CALLS_HAS(p, struct plugin_painter, blit))
		return;
	p->blit(p->ctx, r, src, src_stride, key);
}

int pl_print_write(const struct plugin_printer *o, const char *s, size_t len)
{
	if (!PLUGIN_CALLS_HAS(o, struct plugin_printer, write))
		return -1;
	return o->write(o->ctx, s, len);
}
