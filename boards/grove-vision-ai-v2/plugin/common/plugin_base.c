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
	p->rect(p->ctx, r, rgb565, stroke);
}

void pl_paint_fill_rect(const struct plugin_painter *p,
                        const struct plugin_rect *r, uint16_t rgb565)
{
	p->fill_rect(p->ctx, r, rgb565);
}

void pl_paint_blit(const struct plugin_painter *p, const struct plugin_rect *r,
                   const uint16_t *src, uint32_t src_stride, int32_t key)
{
	p->blit(p->ctx, r, src, src_stride, key);
}

int pl_print_write(const struct plugin_printer *o, const char *s, size_t len)
{
	return o->write(o->ctx, s, len);
}
