/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint.c
 * @brief   The painter.  See plugin_paint.h.
 */
#include "plugin_paint.h"

#include "lcd_st7789.h"

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

/*
 * Clip a half-open rectangle to the framebuffer.
 *
 * Signed and clipped for the same reason lcd_rect_wire() is: a detection
 * routinely runs past the edge of the image it was found in, and making the
 * plugin clamp first would be the same arithmetic done twice, differently.
 */
static int clip(const struct paint_ctx *c, const struct plugin_rect *r,
                int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
	if (r == NULL)
		return 0;
	*x0 = r->x0 < 0 ? 0 : r->x0;
	*y0 = r->y0 < 0 ? 0 : r->y0;
	*x1 = r->x1 > (int32_t)c->w ? (int32_t)c->w : r->x1;
	*y1 = r->y1 > (int32_t)c->h ? (int32_t)c->h : r->y1;
	return *x1 > *x0 && *y1 > *y0;
}

/*
 * Charge @p pixels of work plus one dispatch, or refuse.
 *
 * [!] CHECKED BEFORE THE FRAMEBUFFER IS TOUCHED, and written so the check
 * cannot itself overflow: the form is `cost <= remaining`, never
 * `spent + cost <= limit`.  The second is exactly what a nonsense rectangle
 * would wrap, and then the comparison that was supposed to be the guard passes.
 * A refused primitive draws nothing at all -- half a box is worse than none,
 * because it looks like a rendering bug rather than a budget.
 */
static int charge(struct paint_ctx *c, uint32_t pixels)
{
	struct plugin_paint_budget *b = c->bud;

	if (b == NULL)
		return 0;
	if (b->ops < PLUGIN_PAINT_OP_COST || pixels > b->pixels) {
		b->refused++;
		return 0;
	}
	b->ops    -= PLUGIN_PAINT_OP_COST;
	b->pixels -= pixels;
	return 1;
}

/* ---- the primitives ------------------------------------------------------ */

static void paint_rect(void *ctx, const struct plugin_rect *r, uint16_t rgb565,
                       uint16_t stroke)
{
	struct paint_ctx *c = (struct paint_ctx *)ctx;
	int32_t x0, y0, x1, y1;
	uint32_t cost;

	if (c == NULL || c->fb == NULL)
		return;
	if (!clip(c, r, &x0, &y0, &x1, &y1)) {
		(void)charge(c, 0u);       /* a dispatch that drew nothing still costs */
		return;
	}
	/* An outline visits its edges, but charging the enclosing area is both
	 * simpler and safely pessimistic -- and it is what a solid box (a rectangle
	 * thinner than two strokes) actually costs. */
	cost = (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
	if (!charge(c, cost))
		return;

	lcd_rect_wire(c->fb, c->w, c->h, x0, y0, x1, y1, rgb565, stroke);
}

static void paint_fill_rect(void *ctx, const struct plugin_rect *r,
                            uint16_t rgb565)
{
	struct paint_ctx *c = (struct paint_ctx *)ctx;
	int32_t x0, y0, x1, y1, x, y;
	uint16_t wire;

	if (c == NULL || c->fb == NULL)
		return;
	if (!clip(c, r, &x0, &y0, &x1, &y1)) {
		(void)charge(c, 0u);
		return;
	}
	if (!charge(c, (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0)))
		return;

	wire = paint_wire(rgb565);
	for (y = y0; y < y1; y++) {
		uint16_t *row = c->fb + (size_t)y * (size_t)c->w;

		for (x = x0; x < x1; x++)
			row[x] = wire;
	}
}

static void paint_blit(void *ctx, const struct plugin_rect *r,
                       const uint16_t *src, uint32_t src_stride, int32_t key)
{
	struct paint_ctx *c = (struct paint_ctx *)ctx;
	int32_t x0, y0, x1, y1, x, y;
	uint32_t rows, cols, sx0, sy0;

	if (c == NULL || c->fb == NULL || src == NULL || r == NULL)
		return;
	if (src_stride == 0u) {
		(void)charge(c, 0u);
		return;
	}
	if (!clip(c, r, &x0, &y0, &x1, &y1)) {
		(void)charge(c, 0u);
		return;
	}

	/* Which part of the source survived the clip.  The source is the plugin's
	 * own buffer and its extent is r's width and height -- clipping moves the
	 * origin, so the source offset moves with it. */
	sx0  = (uint32_t)(x0 - r->x0);
	sy0  = (uint32_t)(y0 - r->y0);
	cols = (uint32_t)(x1 - x0);
	rows = (uint32_t)(y1 - y0);

	/*
	 * [!] EVERY SOURCE PIXEL IS CHARGED, INCLUDING THE TRANSPARENT ONES.  What
	 * the budget bounds is time spent with the panel guard held, and a
	 * colour-keyed pixel costs a read and a compare whether or not it is
	 * written.  Charging only what lands would let a mostly-transparent bitmap
	 * of any size through for almost nothing.
	 */
	if (!charge(c, cols * rows))
		return;

	for (y = 0; y < (int32_t)rows; y++) {
		const uint16_t *s = src + (size_t)(sy0 + (uint32_t)y) * src_stride + sx0;
		uint16_t *d = c->fb + (size_t)(y0 + y) * (size_t)c->w + x0;

		for (x = 0; x < (int32_t)cols; x++) {
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

	p->ctx       = &paint_ctx;
	p->rect      = paint_rect;
	p->fill_rect = paint_fill_rect;
	p->blit      = paint_blit;
}
