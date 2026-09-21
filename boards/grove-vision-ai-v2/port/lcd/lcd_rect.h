/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    lcd_rect.h
 * @brief   This board's framebuffer outline loop and its wire byte order
 *          (issue #105 = #78 Step 2).
 *
 * WHY THIS IS ITS OWN TRANSLATION UNIT.  It used to live in lcd_st7789.c, which
 * cannot be built on the host -- it drags in the SSPI driver and the vendor
 * SDK -- so test_plugin_paint.c stubbed lcd_rect_wire() with a call counter and
 * said so.  That was fine while the painter charged a number it made up on its
 * own.  It stopped being fine when the charge became a claim ABOUT THIS LOOP:
 * "the budget decrement equals the pixels this function writes" cannot be
 * checked by a test that replaced the function.
 *
 * [!] THE GEOMETRY RULE IS NOT HERE ANY MORE (issue #110).  svc/rect_geom.h
 * holds it, because wio-lite-ai's painter charges by the same rule while
 * drawing with a different loop into a differently ordered surface.  What stays
 * on this board is exactly the part that is this board's: the loop, and the
 * byte swap the ST7789 needs.  The test's obligations are unchanged and are
 * written on rect_geom.h.
 */
#ifndef LCD_RECT_H
#define LCD_RECT_H

#include <stdint.h>

#include "rect_geom.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RGB565 on the wire is big-endian: the ST7789 takes the high byte first.  The
 * framebuffer is uint16_t in little-endian memory, so every value stored into it
 * is byte-swapped here, once, at the point it is written.  Doing it in the
 * producer rather than in a pass over the buffer keeps the frame path to a
 * single write of each pixel.
 *
 * [!] THIS IS THIS PANEL'S, NOT A SHARED RULE.  wio-lite-ai's LTDC scans out
 * native little-endian RGB565 and must not inherit it.
 */
static inline uint16_t lcd_wire(uint16_t rgb565)
{
	return (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
}

/**
 * @brief  Draw a rectangle outline of @p stroke pixels into @p fb.
 *
 * Coordinates are half-open and signed, and clipping is rect_geom_norm()'s job:
 * a detection routinely runs past the edge of the image it was found in, and
 * making every caller clamp first would be the same arithmetic done twice,
 * differently.  A box thinner than two strokes comes out solid.
 *
 * PURE with respect to the driver: it touches no driver state and takes no
 * lock, which is what lets an overlay callback be handed the staged frame and
 * allowed to write it.  See lcd_st7789.h.
 */
void lcd_rect_wire(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                   int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint16_t rgb565, uint16_t stroke);

#ifdef __cplusplus
}
#endif

#endif /* LCD_RECT_H */
