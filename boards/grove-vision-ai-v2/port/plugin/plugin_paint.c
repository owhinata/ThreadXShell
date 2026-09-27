/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint.c
 * @brief   The painter's loops.  See plugin_paint.h.
 *
 * What each primitive decides before its first store -- the clip, a blit's
 * source offset, the charge -- is shared with wio-lite-ai
 * (svc/plugin_paint_span.c).  What is here is what this panel makes different:
 * pixels in wire byte order, row-major into the staged frame, and outlines
 * through the driver's own lcd_rect_wire().
 */
#include "plugin_paint.h"

#include "lcd_rect.h"
#include "lcd_st7789.h"
#include "plugin_paint_span.h"

#include <stddef.h>

/* The framebuffer holds pixels in WIRE order, which is the driver's knowledge:
 * lcd_rect_wire() swaps on the way in and so must everything here, or a
 * plugin's fills would come out in the wrong colours while its boxes did not. */
static inline uint16_t paint_wire(uint16_t rgb565)
{
	return (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
}

struct paint_ctx {
	struct plugin_paint_budget *bud;
	uint16_t *fb;
	uint16_t  w, h;
};

/* One context per bind.  draw() runs only on the panel thread and the frame
 * pipeline allows one outstanding delivery per sink, so there is never a second
 * bind live at the same time. */
static struct paint_ctx paint_ctx;

/* ---- the primitives ------------------------------------------------------ */

/*
 * [!] THE OUTLINE IS CHARGED FOR WHAT IT WRITES, NOT FOR THE BOX IT ENCLOSES
 * (issue #105).
 *
 * It used to be charged the enclosing area, described as "simpler and safely
 * pessimistic".  Pessimistic it was; safe it was not.  The cap is 19,200 pixels
 * a frame, so ONE close-up face -- a 200x200 box, 40,000 by that reckoning --
 * was refused outright, and a refusal draws nothing at all: the operator sees a
 * face with no box and no explanation, at exactly the distance where the
 * detector works best.  Adding a label beside each box only tightens it.
 *
 * The real cost is the stores lcd_rect_wire() issues, which svc/rect_geom.c
 * computes from the SAME normalisation the drawing loop uses, so the charge
 * (plugin_paint_rect_begin()) and the loop cannot disagree about clipping or
 * about a clamped stroke.  What that sharing
 * deliberately does NOT extend to is the test's expectation: test_plugin_paint.c
 * counts the stores the real loop makes and compares them with the budget this
 * deducted, and pins golden numbers besides -- otherwise the charge would be
 * checked against itself.
 */
static void paint_rect(void *ctx, const struct plugin_rect *r, uint16_t rgb565,
                       uint16_t stroke)
{
	struct paint_ctx *c = (struct paint_ctx *)ctx;
	struct rect_geom g;

	if (c == NULL || c->fb == NULL || r == NULL)
		return;
	/* The normalisation is the clip, and a stroke of zero -- which the driver
	 * rejects before it clips anything -- is "nothing to draw" there too. */
	if (!plugin_paint_rect_begin(c->bud, c->w, c->h, r, stroke, &g))
		return;

	lcd_rect_wire(c->fb, c->w, c->h, r->x0, r->y0, r->x1, r->y1, rgb565,
	              stroke);
}

static void paint_fill_rect(void *ctx, const struct plugin_rect *r,
                            uint16_t rgb565)
{
	struct paint_ctx *c = (struct paint_ctx *)ctx;
	struct plugin_paint_box b;
	int32_t x, y;
	uint16_t wire;

	if (c == NULL || c->fb == NULL)
		return;
	if (!plugin_paint_fill_begin(c->bud, c->w, c->h, r, &b))
		return;

	wire = paint_wire(rgb565);
	for (y = b.y0; y < b.y1; y++) {
		uint16_t *row = c->fb + (size_t)y * (size_t)c->w;

		for (x = b.x0; x < b.x1; x++)
			row[x] = wire;
	}
}

static void paint_blit(void *ctx, const struct plugin_rect *r,
                       const uint16_t *src, uint32_t src_stride, int32_t key)
{
	struct paint_ctx *c = (struct paint_ctx *)ctx;
	struct plugin_paint_blit_span sp;
	int32_t x, y;

	if (c == NULL || c->fb == NULL || src == NULL || r == NULL)
		return;
	/* The clip, the source offset and its safety argument, and the charge for
	 * every source pixel -- transparent ones included -- are all there. */
	if (!plugin_paint_blit_begin(c->bud, c->w, c->h, r, src_stride, &sp))
		return;

	for (y = 0; y < (int32_t)sp.rows; y++) {
		const uint16_t *s = src + (size_t)(sp.sy0 + (uint32_t)y) * src_stride +
		                    sp.sx0;
		uint16_t *d = c->fb + (size_t)(sp.y0 + y) * (size_t)c->w + sp.x0;

		for (x = 0; x < (int32_t)sp.cols; x++) {
			uint16_t px = s[x];

			/* The key is compared in the plugin's own colour space, before the
			 * wire swap, so a plugin picks a transparent colour without having
			 * to know this driver's byte order. */
			if (key >= 0 && px == (uint16_t)key)
				continue;
			d[x] = paint_wire(px);
		}
	}
}

/* ---- binding ------------------------------------------------------------- */

void plugin_paint_bind(struct plugin_painter *p,
                       struct plugin_paint_budget *bud,
                       uint16_t *fb, uint16_t w, uint16_t h)
{
	if (p == NULL)
		return;

	paint_ctx.bud = bud;
	paint_ctx.fb  = fb;
	paint_ctx.w   = w;
	paint_ctx.h   = h;

	plugin_paint_vtable(p, &paint_ctx, paint_rect, paint_fill_rect, paint_blit);
}
