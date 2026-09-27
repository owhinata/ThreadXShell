/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint_span.c
 * @brief   The part of a painter primitive before its loop.  See
 *          plugin_paint_span.h.
 *
 * [!] NO MUTABLE STORAGE: the budget is the caller's, the results go to the
 * caller's frame.
 */
#include "plugin_paint_span.h"

#include <stddef.h>

int plugin_paint_clip(uint16_t w, uint16_t h, const struct plugin_rect *r,
                      struct plugin_paint_box *b)
{
	if (r == NULL || b == NULL)
		return 0;
	b->x0 = r->x0 < 0 ? 0 : r->x0;
	b->y0 = r->y0 < 0 ? 0 : r->y0;
	b->x1 = r->x1 > (int32_t)w ? (int32_t)w : r->x1;
	b->y1 = r->y1 > (int32_t)h ? (int32_t)h : r->y1;
	return b->x1 > b->x0 && b->y1 > b->y0;
}

int plugin_paint_rect_begin(struct plugin_paint_budget *bud, uint16_t w,
                            uint16_t h, const struct plugin_rect *r,
                            uint16_t stroke, struct rect_geom *g)
{
	/* No plugin_paint_clip() here: rect_geom_norm() is the clip, and asking it
	 * is what keeps this from being a second opinion about the same rectangle.
	 * It also answers "nothing to draw" for a stroke of zero. */
	if (r == NULL ||
	    !rect_geom_norm(w, h, r->x0, r->y0, r->x1, r->y1, stroke, g)) {
		(void)plugin_paint_charge(bud, 0u);  /* nothing drawn still costs */
		return 0;
	}
	return plugin_paint_charge(bud, rect_geom_writes(g));
}

int plugin_paint_fill_begin(struct plugin_paint_budget *bud, uint16_t w,
                            uint16_t h, const struct plugin_rect *r,
                            struct plugin_paint_box *b)
{
	if (!plugin_paint_clip(w, h, r, b)) {
		(void)plugin_paint_charge(bud, 0u);
		return 0;
	}
	return plugin_paint_charge(bud, (uint32_t)(b->x1 - b->x0) *
	                                (uint32_t)(b->y1 - b->y0));
}

int plugin_paint_blit_begin(struct plugin_paint_budget *bud, uint16_t w,
                            uint16_t h, const struct plugin_rect *r,
                            uint32_t src_stride,
                            struct plugin_paint_blit_span *s)
{
	struct plugin_paint_box b;

	if (src_stride == 0u || !plugin_paint_clip(w, h, r, &b)) {
		(void)plugin_paint_charge(bud, 0u);
		return 0;
	}

	/* Which part of the source survived the clip.  The source is the plugin's
	 * own buffer and its extent is r's width and height -- clipping moves the
	 * origin, so the source offset moves with it.
	 *
	 * [!] WIDENED BEFORE THE SUBTRACTION, ON BOTH AXES.  `x0 - r->x0` is signed
	 * arithmetic and r->x0 comes from loaded code: at INT32_MIN the difference
	 * is not representable and the subtraction is undefined.  That is not a
	 * theoretical input when the coordinate was computed from a model's output.
	 *
	 * The offset itself is in range by construction, and it is worth writing
	 * down because it is the only reason this is safe: the clip keeps
	 * x0 >= r->x0 and x0 < x1 <= r->x1, so sx0 is strictly less than the
	 * source width r declares.  What that width DESCRIBES is still the plugin's
	 * claim about its own buffer, which this boundary does not verify -- see
	 * plugin_abi.h. */
	s->x0   = b.x0;
	s->y0   = b.y0;
	s->sx0  = (uint32_t)((int64_t)b.x0 - (int64_t)r->x0);
	s->sy0  = (uint32_t)((int64_t)b.y0 - (int64_t)r->y0);
	s->cols = (uint32_t)(b.x1 - b.x0);
	s->rows = (uint32_t)(b.y1 - b.y0);

	/*
	 * [!] EVERY SOURCE PIXEL IS CHARGED, INCLUDING THE TRANSPARENT ONES.  What
	 * the budget bounds is time spent with the panel guard held, and a
	 * colour-keyed pixel costs a read and a compare whether or not it is
	 * written.  Charging only what lands would let a mostly-transparent bitmap
	 * of any size through for almost nothing.
	 */
	return plugin_paint_charge(bud, s->cols * s->rows);
}

void plugin_paint_vtable(struct plugin_painter *p, void *ctx,
                         void (*rect)(void *ctx, const struct plugin_rect *r,
                                      uint16_t rgb565, uint16_t stroke),
                         void (*fill_rect)(void *ctx,
                                           const struct plugin_rect *r,
                                           uint16_t rgb565),
                         void (*blit)(void *ctx, const struct plugin_rect *r,
                                      const uint16_t *src, uint32_t src_stride,
                                      int32_t key))
{
	if (p == NULL)
		return;
	p->version   = PLUGIN_ABI_VERSION;
	p->size      = (uint32_t)sizeof(*p);
	p->ctx       = ctx;
	p->rect      = rect;
	p->fill_rect = fill_rect;
	p->blit      = blit;
}
