/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint_span.h
 * @brief   Everything a painter primitive decides before its first store:
 *          the clip, the source offset of a blit, the charge, and how the
 *          painter is filled in (issue #126).
 *
 * WHAT IS SHARED AND WHAT IS NOT.  Two boards let a plugin paint.  Their
 * pixel loops stay theirs, because each has a hardware reason to differ:
 * grove-vision-ai-v2 stores byte-swapped RGB565 row by row into a staged SRAM
 * frame and draws outlines with its own -O3 routine; wio-lite-ai stores native
 * RGB565 column by column into a rotated PSRAM surface.  What happens BEFORE
 * the loop has no such reason, and is here: which part of the rectangle lies
 * on the surface, which part of a blit's source that is, what the primitive is
 * charged, and in what order a refusal is decided.
 *
 * [!] THE LOOPS ARE NOT CALLED THROUGH HERE.  A primitive asks one question of
 * this file and then runs its own loop; there is no per-pixel indirect call,
 * and nothing here knows how a pixel is stored.
 *
 * [!] EVERY begin FUNCTION CHARGES BEFORE IT SAYS YES, AND CHARGES A DISPATCH
 * WHEN IT SAYS "NOTHING TO DRAW".  A primitive that clips away entirely still
 * cost a call; one that is refused for want of budget draws nothing at all.
 * The caller touches the framebuffer only on a non-zero return.
 *
 * [!] WHAT IS CHARGED FOR AN OUTLINE IS WHAT THE LOOP STORES, and that rule is
 * svc/rect_geom.c's.  Each board's host test counts the stores ITS loop makes
 * and compares them with the budget this deducted -- the expectation is not
 * shared, or the charge would be checked against itself.
 *
 * No mutable storage: the budget is the caller's and every result is written
 * to the caller's frame.
 */
#ifndef PLUGIN_PAINT_SPAN_H
#define PLUGIN_PAINT_SPAN_H

#include <stdint.h>

#include "plugin_abi.h"
#include "plugin_paint_budget.h"
#include "rect_geom.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A rectangle clipped to the surface: half-open and never empty. */
struct plugin_paint_box {
	int32_t x0, y0, x1, y1;
};

/** What a blit copies: @ref cols x @ref rows pixels from source offset
 *  (@ref sx0, @ref sy0) to surface position (@ref x0, @ref y0). */
struct plugin_paint_blit_span {
	int32_t  x0, y0;       /**< destination, on the surface              */
	uint32_t cols, rows;   /**< extent after the clip                    */
	uint32_t sx0, sy0;     /**< where that extent starts in the source   */
};

/**
 * @brief  Clip half-open @p r to a @p w x @p h surface.
 *
 * Signed and clipped because a detection routinely runs past the edge of the
 * image it was found in, and making the plugin clamp first would be the same
 * arithmetic done twice, differently.
 *
 * @return non-zero with @p b filled when something is left; zero for a NULL
 *         @p r or nothing on the surface.
 */
int plugin_paint_clip(uint16_t w, uint16_t h, const struct plugin_rect *r,
                      struct plugin_paint_box *b);

/**
 * @brief  An outline: normalise it (svc/rect_geom.c) and charge the stores it
 *         will make.
 *
 * @return non-zero with @p g filled when the caller may draw it.
 */
int plugin_paint_rect_begin(struct plugin_paint_budget *bud, uint16_t w,
                            uint16_t h, const struct plugin_rect *r,
                            uint16_t stroke, struct rect_geom *g);

/**
 * @brief  A fill: clip it and charge its area.
 *
 * @return non-zero with @p b filled when the caller may draw it.
 */
int plugin_paint_fill_begin(struct plugin_paint_budget *bud, uint16_t w,
                            uint16_t h, const struct plugin_rect *r,
                            struct plugin_paint_box *b);

/**
 * @brief  A blit: clip it, find the part of the source that survived, and
 *         charge every source pixel the loop will read.
 *
 * A @p src_stride of zero is nothing to draw.
 *
 * @return non-zero with @p s filled when the caller may draw it.
 */
int plugin_paint_blit_begin(struct plugin_paint_budget *bud, uint16_t w,
                            uint16_t h, const struct plugin_rect *r,
                            uint32_t src_stride,
                            struct plugin_paint_blit_span *s);

/**
 * @brief  Fill in a painter: version and size first, then @p ctx and the three
 *         primitives.
 *
 * The plugin's veneer refuses a painter without its version and size (issue
 * #111), so a member left out here is a draw that silently does nothing.
 */
void plugin_paint_vtable(struct plugin_painter *p, void *ctx,
                         void (*rect)(void *ctx, const struct plugin_rect *r,
                                      uint16_t rgb565, uint16_t stroke),
                         void (*fill_rect)(void *ctx,
                                           const struct plugin_rect *r,
                                           uint16_t rgb565),
                         void (*blit)(void *ctx, const struct plugin_rect *r,
                                      const uint16_t *src, uint32_t src_stride,
                                      int32_t key));

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_PAINT_SPAN_H */
