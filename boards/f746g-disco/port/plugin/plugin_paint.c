/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint.c
 * @brief   The painter's loops.  See plugin_paint.h.
 *
 * What each primitive decides before its first store -- the clip, a blit's
 * source offset, the charge -- is shared (svc/plugin/plugin_paint_span.c).  What
 * is here is what this surface makes different: native RGB565 stored row-major
 * into the camera view buffer, not rotated.
 */
#include "plugin_paint.h"

#include "plugin_paint_span.h"
#include "rect_geom.h"

#include <stddef.h>

/*
 * The pixel-store seam.
 *
 * [!] IN THE LOOPS, NOT IN THE TEST HARNESS.  A counter wrapped around the
 * caller would observe the test's idea of the loop; this observes the loop.
 * The target build defines nothing and compiles a plain store.  It counts
 * STORES, not distinct pixels: the stores are what the budget is charged for
 * (issue #105).
 */
#ifdef PLUGIN_PAINT_COUNT_STORES
extern unsigned long plugin_paint_stores;
#define PAINT_PUT(p, v) do { plugin_paint_stores++; *(p) = (v); } while (0)
#else
#define PAINT_PUT(p, v) do { *(p) = (v); } while (0)
#endif

/*
 * What the plugin is given, and nothing more.
 *
 * File scope rather than a local, and that is a stack decision: draw() is
 * charged against what is left of the GUIX thread's stack.  Safe as a file
 * scope object because draw() runs on that one thread, one CAMERA_FRAME at a
 * time, so two binds are never live at once.
 */
struct paint_ctx {
	struct plugin_paint_budget *bud;
	uint16_t                   *fb;     /* view buffer, stride w */
	uint16_t                    w, h;
};

static struct paint_ctx paint_ctx;

static uint16_t *at(const struct paint_ctx *c, int32_t x, int32_t y)
{
	return c->fb + (size_t)(uint32_t)y * (size_t)c->w + (size_t)(uint32_t)x;
}

/* ---- the primitives ------------------------------------------------------ */

/*
 * [!] THE OUTLINE IS CHARGED FOR WHAT IT WRITES, NOT FOR THE BOX IT ENCLOSES,
 * and the loop below is the row-major one rect_geom_writes() counts -- which on
 * this surface is also the contiguous order.
 */
static void paint_rect(void *ctx, const struct plugin_rect *r, uint16_t rgb565,
                       uint16_t stroke)
{
	struct paint_ctx *c = (struct paint_ctx *)ctx;
	struct rect_geom g;
	uint32_t x, y;

	if (c == NULL || c->fb == NULL || r == NULL)
		return;
	if (!plugin_paint_rect_begin(c->bud, c->w, c->h, r, stroke, &g))
		return;

	for (y = g.y0; y < g.y0 + g.h; y++) {
		int edge = (y < g.y0 + g.t) || (y >= g.y0 + g.h - g.t);

		if (edge) {
			for (x = g.x0; x < g.x0 + g.w; x++)
				PAINT_PUT(at(c, (int32_t)x, (int32_t)y), rgb565);
		} else {
			for (x = g.x0; x < g.x0 + g.t; x++)
				PAINT_PUT(at(c, (int32_t)x, (int32_t)y), rgb565);
			for (x = g.x0 + g.w - g.t; x < g.x0 + g.w; x++)
				PAINT_PUT(at(c, (int32_t)x, (int32_t)y), rgb565);
		}
	}
}

static void paint_fill_rect(void *ctx, const struct plugin_rect *r,
                            uint16_t rgb565)
{
	struct paint_ctx *c = (struct paint_ctx *)ctx;
	struct plugin_paint_box b;
	int32_t x, y;

	if (c == NULL || c->fb == NULL)
		return;
	if (!plugin_paint_fill_begin(c->bud, c->w, c->h, r, &b))
		return;

	for (y = b.y0; y < b.y1; y++) {
		uint16_t *d = at(c, b.x0, y);

		for (x = b.x0; x < b.x1; x++)
			PAINT_PUT(d++, rgb565);
	}
}

static void paint_blit(void *ctx, const struct plugin_rect *r,
                       const uint16_t *src, uint32_t src_stride, int32_t key)
{
	struct paint_ctx *c = (struct paint_ctx *)ctx;
	struct plugin_paint_blit_span sp;
	uint32_t x, y;

	if (c == NULL || c->fb == NULL || src == NULL || r == NULL)
		return;
	/* The clip, the source offset and its safety argument, and the charge for
	 * every source pixel -- transparent ones included -- are all there. */
	if (!plugin_paint_blit_begin(c->bud, c->w, c->h, r, src_stride, &sp))
		return;

	for (y = 0u; y < sp.rows; y++) {
		const uint16_t *s = src + (size_t)(sp.sy0 + y) * src_stride + sp.sx0;
		uint16_t *d = at(c, sp.x0, sp.y0 + (int32_t)y);

		for (x = 0u; x < sp.cols; x++) {
			uint16_t px = s[x];

			/* The key is compared in the plugin's own colour space; this
			 * surface is native RGB565, so there is no other space. */
			if (key >= 0 && px == (uint16_t)key)
				continue;
			PAINT_PUT(&d[x], px);
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
