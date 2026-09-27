/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint.c
 * @brief   The painter's loops.  See plugin_paint.h.
 *
 * What each primitive decides before its first store -- the clip, a blit's
 * source offset, the charge -- is shared with grove-vision-ai-v2
 * (svc/plugin_paint_span.c).  What is here is what this panel makes different:
 * native RGB565 stored into a rotated surface, columns contiguous.
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
 * The target build defines nothing and compiles a plain store, so the
 * instrumented question -- how many pixels does this write? -- is answered
 * without changing what runs on the panel thread.
 *
 * It counts STORES, not distinct pixels: on an odd, narrow rectangle the
 * clamped stroke makes the left and right bands overlap and the loop stores to
 * the overlapping column twice, and that second store is real work the budget
 * is charged for.  Issue #105 is the whole reason the two are distinguished.
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
 * File scope rather than a local, and that is a stack decision: the panel
 * thread's stack is small and a plugin's draw() is charged against what is left
 * of it.  Safe as a file scope object because draw() runs on that one thread
 * and the frame pipeline pre-pins one delivery, so two binds are never live at
 * once.
 */
struct paint_ctx {
	struct plugin_paint_budget *bud;
	uint16_t                   *fb;     /* portrait back buffer            */
	uint16_t                    sw, sh; /* landscape surface geometry      */
};

static struct paint_ctx paint_ctx;

/*
 * Landscape (x, y) to a framebuffer address.
 *
 * The mapping the board's factory firmware used and svc/gfx_rot.c documents:
 * landscape column x becomes frame-buffer row (rows - 1 - x), and the column's
 * pixels run along that row from y.  The panel is sh rows by sw columns in
 * portrait, so the frame-buffer stride is sh and there are sw rows.
 */
static uint16_t *at(const struct paint_ctx *c, int32_t x, int32_t y)
{
	return c->fb + (size_t)((uint32_t)c->sw - 1u - (uint32_t)x) *
	                       (size_t)c->sh + (size_t)(uint32_t)y;
}

/* ---- the primitives ------------------------------------------------------ */

/*
 * [!] THE OUTLINE IS CHARGED FOR WHAT IT WRITES, NOT FOR THE BOX IT ENCLOSES,
 * and the loop below is the row-major one rect_geom_writes() counts.
 *
 * A column-major loop would be faster here -- a landscape column is contiguous
 * in this frame buffer and a landscape row is strided -- and it would also have
 * to re-derive the count, because the degenerate cases differ: at h = 1 with
 * stroke 1 the row-major rule writes the single row once across the full width,
 * while a naive column-major one writes the top and bottom bands of the same
 * row twice.  Sharing the rule and not the loop means the loop follows the
 * rule.  The cost of the strided stores is bounded by the budget and measured
 * on hardware; this framebuffer is non-cacheable either way, so contiguity buys
 * the paired-store trick and nothing else.
 */
static void paint_rect(void *ctx, const struct plugin_rect *r, uint16_t rgb565,
                       uint16_t stroke)
{
	struct paint_ctx *c = (struct paint_ctx *)ctx;
	struct rect_geom g;
	uint32_t x, y;

	if (c == NULL || c->fb == NULL || r == NULL)
		return;
	/* The normalisation is the clip, and it answers "nothing to draw" for a
	 * stroke of zero. */
	if (!plugin_paint_rect_begin(c->bud, c->sw, c->sh, r, stroke, &g))
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
	if (!plugin_paint_fill_begin(c->bud, c->sw, c->sh, r, &b))
		return;

	/* Column-major: one landscape column is one contiguous run in the frame
	 * buffer.  Nothing counts spans here, so the order is free to be the fast
	 * one. */
	for (x = b.x0; x < b.x1; x++) {
		uint16_t *d = at(c, x, b.y0);

		for (y = b.y0; y < b.y1; y++)
			PAINT_PUT(d++, rgb565);
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
	if (!plugin_paint_blit_begin(c->bud, c->sw, c->sh, r, src_stride, &sp))
		return;

	/* Column-major again, gathering down a strided source column into a
	 * contiguous destination run -- the same shape svc/gfx_rot.c uses, without
	 * its staging buffer, because a plugin's chips are small and the colour key
	 * has to be tested per pixel anyway. */
	for (x = 0; x < (int32_t)sp.cols; x++) {
		const uint16_t *s = src + (size_t)sp.sy0 * src_stride +
		                    (sp.sx0 + (uint32_t)x);
		uint16_t *d = at(c, sp.x0 + x, sp.y0);

		for (y = 0; y < (int32_t)sp.rows; y++) {
			uint16_t px = *s;

			s += src_stride;
			/* The key is compared in the plugin's own colour space; this panel
			 * scans out native RGB565, so there is no other space to be in. */
			if (key >= 0 && px == (uint16_t)key) {
				d++;
				continue;
			}
			PAINT_PUT(d, px);
			d++;
		}
	}
}

/* ---- binding ------------------------------------------------------------- */

void plugin_paint_bind(struct plugin_painter *p,
                       struct plugin_paint_budget *bud,
                       uint16_t *fb, uint16_t sw, uint16_t sh)
{
	if (p == NULL)
		return;

	paint_ctx.bud = bud;
	paint_ctx.fb  = fb;
	paint_ctx.sw  = sw;
	paint_ctx.sh  = sh;

	plugin_paint_vtable(p, &paint_ctx, paint_rect, paint_fill_rect, paint_blit);
}
