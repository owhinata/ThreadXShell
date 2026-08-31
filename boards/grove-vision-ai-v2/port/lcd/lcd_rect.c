/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    lcd_rect.c
 * @brief   The outline primitive.  See lcd_rect.h.
 */
#include "lcd_rect.h"

#include <stddef.h>

/*
 * The pixel-store seam.
 *
 * [!] IN THE DRIVER, NOT IN THE TEST HARNESS.  A counter wrapped around the
 * caller would observe the test's idea of the loop; this observes the loop.  The
 * target build defines nothing and compiles a plain store, so the instrumented
 * question -- how many stores does this issue? -- is answered without changing
 * what runs on the panel thread.
 *
 * The host test defines LCD_RECT_COUNT_STORES and supplies the counter.  It has
 * to be a count of STORES and not of touched pixels: the two differ on an odd,
 * narrow rectangle, and that difference is the thing the budget was getting
 * wrong.
 */
#ifdef LCD_RECT_COUNT_STORES
extern unsigned long lcd_rect_stores;
#define LCD_RECT_PUT(p, v) do { lcd_rect_stores++; *(p) = (v); } while (0)
#else
#define LCD_RECT_PUT(p, v) do { *(p) = (v); } while (0)
#endif

int lcd_rect_norm(const uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                  int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                  uint16_t stroke, struct lcd_rect_geom *g)
{
	int32_t t = (int32_t)stroke;

	if (g == NULL)
		return 0;
	if (fb == NULL || fb_w == 0u || fb_h == 0u || t <= 0)
		return 0;

	/* Clip first, so everything below indexes inside the buffer by
	 * construction rather than by a bounds test per pixel. */
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int32_t)fb_w)
		x1 = (int32_t)fb_w;
	if (y1 > (int32_t)fb_h)
		y1 = (int32_t)fb_h;
	if (x1 <= x0 || y1 <= y0)
		return 0;

	/* A box thinner than two strokes becomes solid rather than drawing its
	 * two edges over each other -- the same rendering the Wio's preview
	 * settled on, and the honest one: at that size there is no interior to
	 * show through.
	 *
	 * [!] THE ORDER IS PART OF THE RULE.  The vertical clamp compares against
	 * the stroke the horizontal one may already have reduced, so swapping them
	 * gives a different border on a rectangle that is narrow in both axes. */
	if (t > (x1 - x0) / 2)
		t = (x1 - x0 + 1) / 2;
	if (t > (y1 - y0) / 2)
		t = (y1 - y0 + 1) / 2;
	if (t <= 0)
		t = 1;

	g->x0 = (uint32_t)x0;
	g->y0 = (uint32_t)y0;
	g->w  = (uint32_t)(x1 - x0);
	g->h  = (uint32_t)(y1 - y0);
	g->t  = (uint32_t)t;
	return 1;
}

uint32_t lcd_rect_writes(const struct lcd_rect_geom *g)
{
	uint64_t edge, total;

	if (g == NULL || g->w == 0u || g->h == 0u || g->t == 0u)
		return 0u;

	/* Rows belonging to the top or bottom band write the full width; the rest
	 * write the two side bands, which is 2t stores even where they overlap. */
	edge = (uint64_t)g->t * 2u;
	if (edge > (uint64_t)g->h)
		edge = (uint64_t)g->h;

	total = edge * (uint64_t)g->w +
	        ((uint64_t)g->h - edge) * (uint64_t)g->t * 2u;

	/* Saturate.  A budget wants an over-estimate at the extreme, never the
	 * small number a wrap would produce. */
	return total > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)total;
}

void lcd_rect_wire(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                   int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint16_t rgb565, uint16_t stroke)
{
	struct lcd_rect_geom g;
	uint16_t wire;
	uint32_t y;

	if (!lcd_rect_norm(fb, fb_w, fb_h, x0, y0, x1, y1, stroke, &g))
		return;

	wire = lcd_wire(rgb565);
	for (y = g.y0; y < g.y0 + g.h; y++) {
		int edge = (y < g.y0 + g.t) || (y >= g.y0 + g.h - g.t);
		uint16_t *row = fb + (size_t)y * (size_t)fb_w;
		uint32_t x;

		if (edge) {
			for (x = g.x0; x < g.x0 + g.w; x++)
				LCD_RECT_PUT(&row[x], wire);
		} else {
			for (x = g.x0; x < g.x0 + g.t; x++)
				LCD_RECT_PUT(&row[x], wire);
			for (x = g.x0 + g.w - g.t; x < g.x0 + g.w; x++)
				LCD_RECT_PUT(&row[x], wire);
		}
	}
}
