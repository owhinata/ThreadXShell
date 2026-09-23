/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint.c
 * @brief   The painter's loops.  See plugin_paint.h.
 */
#include "plugin_paint.h"

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

/*
 * Clip a half-open rectangle to the surface.
 *
 * Signed and clipped for the same reason rect_geom_norm() is: a detection
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
	*x1 = r->x1 > (int32_t)c->sw ? (int32_t)c->sw : r->x1;
	*y1 = r->y1 > (int32_t)c->sh ? (int32_t)c->sh : r->y1;
	return *x1 > *x0 && *y1 > *y0;
}

static int charge(struct paint_ctx *c, uint32_t pixels)
{
	return plugin_paint_charge(c->bud, pixels);
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
	/* No separate clip() here: rect_geom_norm() is the clip, and asking it is
	 * what keeps this from being a second opinion about the same rectangle.
	 * It also answers "nothing to draw" for a stroke of zero. */
	if (!rect_geom_norm(c->sw, c->sh, r->x0, r->y0, r->x1, r->y1, stroke, &g)) {
		(void)charge(c, 0u);       /* a dispatch that drew nothing still costs */
		return;
	}
	if (!charge(c, rect_geom_writes(&g)))
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
	int32_t x0, y0, x1, y1, x, y;

	if (c == NULL || c->fb == NULL)
		return;
	if (!clip(c, r, &x0, &y0, &x1, &y1)) {
		(void)charge(c, 0u);
		return;
	}
	if (!charge(c, (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0)))
		return;

	/* Column-major: one landscape column is one contiguous run in the frame
	 * buffer.  Nothing counts spans here, so the order is free to be the fast
	 * one. */
	for (x = x0; x < x1; x++) {
		uint16_t *d = at(c, x, y0);

		for (y = y0; y < y1; y++)
			PAINT_PUT(d++, rgb565);
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
	 * origin, so the source offset moves with it.
	 *
	 * [!] WIDENED BEFORE THE SUBTRACTION, ON BOTH AXES.  `x0 - r->x0` is signed
	 * arithmetic and r->x0 comes from loaded code: at INT32_MIN the difference
	 * is not representable and the subtraction is undefined.  That is not a
	 * theoretical input when the coordinate was computed from a model's output.
	 *
	 * The offset itself is in range by construction: clip() keeps x0 >= r->x0
	 * and x0 < x1 <= r->x1, so sx0 is strictly less than the source width r
	 * declares.  What that width DESCRIBES is still the plugin's claim about
	 * its own buffer, which this boundary does not verify -- see plugin_abi.h. */
	sx0  = (uint32_t)((int64_t)x0 - (int64_t)r->x0);
	sy0  = (uint32_t)((int64_t)y0 - (int64_t)r->y0);
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

	/* Column-major again, gathering down a strided source column into a
	 * contiguous destination run -- the same shape svc/gfx_rot.c uses, without
	 * its staging buffer, because a plugin's chips are small and the colour key
	 * has to be tested per pixel anyway. */
	for (x = 0; x < (int32_t)cols; x++) {
		const uint16_t *s = src + (size_t)sy0 * src_stride + (sx0 + (uint32_t)x);
		uint16_t *d = at(c, x0 + x, y0);

		for (y = 0; y < (int32_t)rows; y++) {
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

	/* Version and size first: the plugin's veneer refuses a painter without
	 * them (issue #111), so a member left out here is a draw that silently
	 * does nothing. */
	p->version   = PLUGIN_ABI_VERSION;
	p->size      = (uint32_t)sizeof(*p);
	p->ctx       = &paint_ctx;
	p->rect      = paint_rect;
	p->fill_rect = paint_fill_rect;
	p->blit      = paint_blit;
}
