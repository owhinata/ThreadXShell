/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    lcd_rect.c
 * @brief   The outline loop.  See lcd_rect.h.
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

void lcd_rect_wire(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                   int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint16_t rgb565, uint16_t stroke)
{
	struct rect_geom g;
	uint16_t wire;
	uint32_t y;

	/* [!] THE NULL BUFFER IS CHECKED HERE.  rect_geom_norm() used to take the
	 * pointer solely to reject it; it is pure geometry now (issue #110), so
	 * the function that is about to write the buffer is the one that asks
	 * whether it has one. */
	if (fb == NULL)
		return;
	if (!rect_geom_norm(fb_w, fb_h, x0, y0, x1, y1, stroke, &g))
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
