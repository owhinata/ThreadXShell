/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    lcd_rect.h
 * @brief   The framebuffer outline primitive, its geometry rule, and the cost
 *          of drawing it (issue #105 = #78 Step 2).
 *
 * WHY THIS IS ITS OWN TRANSLATION UNIT.  It used to live in lcd_st7789.c, which
 * cannot be built on the host -- it drags in the SSPI driver and the vendor
 * SDK -- so test_plugin_paint.c stubbed lcd_rect_wire() with a call counter and
 * said so.  That was fine while the painter charged a number it made up on its
 * own.  It stopped being fine when the charge became a claim ABOUT THIS LOOP:
 * "the budget decrement equals the pixels this function writes" cannot be
 * checked by a test that replaced the function.
 *
 * [!] THE GEOMETRY RULE IS SHARED, THE EXPECTED VALUE IS NOT.  @ref
 * lcd_rect_norm is used by both the drawing loop and the painter's charge, so
 * the two cannot disagree about clipping or about how a stroke is clamped.  The
 * TEST does not use @ref lcd_rect_writes as its expectation: it counts the
 * stores the real loop issues (see @ref LCD_RECT_COUNT_STORES) and compares that
 * against the budget the painter deducted, and it additionally pins a handful of
 * (geometry, stroke) pairs to golden numbers written out by hand.  Otherwise the
 * charge would be checked against itself, and a shared rule that drifted would
 * take the check with it.
 *
 * [!] THE COUNT IS WRITES, NOT DISTINCT PIXELS.  On an odd, narrow rectangle the
 * clamped stroke makes the left and right bands overlap, and the loop stores to
 * the overlapping column twice.  What the budget bounds is time spent with the
 * panel guard held, so the second store is real work and is counted.  An earlier
 * plan for this said "perimeter x stroke, without double counting" and would
 * have under-charged exactly there.
 */
#ifndef LCD_RECT_H
#define LCD_RECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RGB565 on the wire is big-endian: the ST7789 takes the high byte first.  The
 * framebuffer is uint16_t in little-endian memory, so every value stored into it
 * is byte-swapped here, once, at the point it is written.  Doing it in the
 * producer rather than in a pass over the buffer keeps the frame path to a
 * single write of each pixel.
 */
static inline uint16_t lcd_wire(uint16_t rgb565)
{
	return (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
}

/** A clipped rectangle and the stroke that will actually be drawn. */
struct lcd_rect_geom {
	uint32_t x0, y0;   /**< clipped origin, inside the framebuffer     */
	uint32_t w, h;     /**< clipped extents, both non-zero             */
	uint32_t t;        /**< stroke after clamping; at least 1          */
};

/**
 * @brief  Clip @p x0..@p y1 to the framebuffer and clamp @p stroke.
 *
 * The whole geometry rule, in one place: which pixels are in range, and how
 * thick the border ends up when the caller asked for more than half the box.
 *
 * @return non-zero when there is something to draw, with @p g filled; zero when
 *         there is not, and then @p g is untouched.
 */
int lcd_rect_norm(const uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                  int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                  uint16_t stroke, struct lcd_rect_geom *g);

/**
 * @brief  How many stores @ref lcd_rect_wire will issue for @p g.
 *
 * Saturates at UINT32_MAX rather than wrapping.  A caller charging a budget
 * wants an over-estimate at the extreme, never a small number.
 */
uint32_t lcd_rect_writes(const struct lcd_rect_geom *g);

/**
 * @brief  Draw a rectangle outline of @p stroke pixels into @p fb.
 *
 * Coordinates are half-open and signed, and clipping is this function's job: a
 * detection routinely runs past the edge of the image it was found in, and
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
