/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    rect_geom.h
 * @brief   The outline geometry rule, and the cost of drawing it (issues #105,
 *          #110).
 *
 * WHAT IS SHARED AND WHAT IS NOT.  Two boards draw a plugin's rectangles onto a
 * panel, and they draw them differently: grove-vision-ai-v2 stores byte-swapped
 * RGB565 into a staged framebuffer in SRAM, wio-lite-ai stores native RGB565
 * into a rotated surface in external PSRAM.  What they must NOT differ about is
 * which pixels a rectangle covers and how much work that is, because the
 * painter's budget is charged from the second and the drawing loop obeys the
 * first -- and a board whose charge and loop disagree either under-charges (the
 * budget stops bounding the panel guard's hold time) or over-charges (boxes
 * silently vanish, which is how issue #105 found the problem).
 *
 * So the rule lives here and every drawing loop stays with its board.
 *
 * [!] PURE GEOMETRY: NO FRAMEBUFFER.  This took a `const uint16_t *fb` while it
 * lived beside grove's loop, purely to reject a null one.  A caller that is
 * about to write a buffer is the one that has to decide whether it has a buffer;
 * folding that into the geometry rule made a pure function look like it knew
 * where the pixels went, and it is the sort of parameter a second board copies
 * without needing.
 *
 * [!] THE GEOMETRY RULE IS SHARED, THE EXPECTED VALUE IS NOT.  @ref
 * rect_geom_norm is used by both a board's drawing loop and its painter's
 * charge, so the two cannot disagree.  A board's TEST must not use @ref
 * rect_geom_writes as its expectation: it counts the stores the real loop issues
 * -- at a seam inside that loop -- and compares them against the budget the
 * painter deducted, and it additionally pins (geometry, stroke) pairs to golden
 * numbers written out by hand.  Otherwise the charge is checked against itself,
 * and a shared rule that drifted would take the check with it.
 *
 * [!] THE COUNT IS WRITES, NOT DISTINCT PIXELS.  On an odd, narrow rectangle the
 * clamped stroke makes the left and right bands overlap, and the loop stores to
 * the overlapping column twice.  What the budget bounds is time spent with the
 * panel guard held, so the second store is real work and is counted.  An earlier
 * plan said "perimeter x stroke, without double counting" and would have
 * under-charged exactly there.
 *
 * [!] AND IT IS AN UPPER BOUND ON WORK, NOT A TIME.  Issue #110 measured a board
 * whose loop emits paired 32-bit stores, so two pixels can share one store; a
 * colour-keyed blit reads source pixels it does not write at all.  The number
 * here is what the budget is denominated in, deliberately -- it is proportional
 * to the work and cheap to compute before the buffer is touched.  A board still
 * has to MEASURE the hold time its budget buys.
 */
#ifndef RECT_GEOM_H
#define RECT_GEOM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A clipped rectangle and the stroke that will actually be drawn. */
struct rect_geom {
	uint32_t x0, y0;   /**< clipped origin, inside the surface         */
	uint32_t w, h;     /**< clipped extents, both non-zero             */
	uint32_t t;        /**< stroke after clamping; at least 1          */
};

/**
 * @brief  Clip @p x0..@p y1 to a @p fb_w by @p fb_h surface and clamp @p stroke.
 *
 * The whole geometry rule, in one place: which pixels are in range, and how
 * thick the border ends up when the caller asked for more than half the box.
 * Coordinates are half-open and signed, because a detection routinely runs past
 * the edge of the image it was found in and making every caller clamp first
 * would be the same arithmetic done twice, differently.
 *
 * @return non-zero when there is something to draw, with @p g filled; zero when
 *         there is not, and then @p g is untouched.
 */
int rect_geom_norm(uint16_t fb_w, uint16_t fb_h,
                   int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint16_t stroke, struct rect_geom *g);

/**
 * @brief  How many pixel writes an outline of @p g covers.
 *
 * Saturates at UINT32_MAX rather than wrapping.  A caller charging a budget
 * wants an over-estimate at the extreme, never a small number.
 */
uint32_t rect_geom_writes(const struct rect_geom *g);

#ifdef __cplusplus
}
#endif

#endif /* RECT_GEOM_H */
