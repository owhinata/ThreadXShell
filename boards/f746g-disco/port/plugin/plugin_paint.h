/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint.h
 * @brief   What a plugin may paint with on this board (issue #159 = #78
 *          Step 4b).
 *
 * The plugin is handed a @ref plugin_painter, never the buffer: the base keeps
 * the buffer and its geometry and validates every rectangle against them, so a
 * bug in loaded code cannot write outside the picture.  That is the ONE
 * property this boundary provides; it is not a sandbox.
 *
 * [!] THE SURFACE IS THE CAMERA VIEW BUFFER (cam_view_buf), NOT THE PANEL.  The
 * GUIX preview stores each frame into that buffer (DMA2D, under the frame
 * lock) and redraws the camera icon from it; the plugin's draw() runs in the
 * GUIX thread's CAMERA_FRAME handler between the store and the redraw, where
 * the resident decoder's boxes were drawn until issue #159 Step 4c
 * (owhinata/stm32f746g-disco#83), and its
 * strokes ride the buffer into both LTDC framebuffers through the same
 * freshness flags as the frame.  So the coordinates are the camera frame's --
 * w x h, QVGA or QQVGA, row-major with stride w, not rotated -- and the byte
 * order is native RGB565, which the DMA2D and the LTDC both read as is.
 *
 * [!] IT DRAWS WITH THE CPU, INCLUDING THE FILLS, AND THAT IS THE DESIGN.  The
 * view buffer is non-cacheable SDRAM (bank 1), so CPU stores are coherent with
 * the DMA2D reads that later blit it -- the access the resident overlay used to
 * make before issue #159 Step 4c removed it.  A DMA2D fill would need a way to
 * know it had stopped writing before the frame lock is let go, which this port
 * does not have.
 */
#ifndef PLUGIN_PAINT_H
#define PLUGIN_PAINT_H

#include <stdint.h>

#include "plugin_abi.h"
#include "plugin_paint_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Set up the drawing target for the duration of one draw(), and return
 *         it as the context the shared draw builds the painter around
 *         (nn_active_draw_on(), issue #165).
 *
 * @param bud   reset by the caller to this frame's allowance
 * @param fb    the frame the primitives store into
 *
 * The context is valid only until draw() returns.  The painter itself is not
 * the board's: until #165 this was plugin_paint_bind(), which filled one the
 * caller then handed on, and a painter in the board's hands could be edited
 * after it was filled.
 */
void *plugin_paint_target(struct plugin_paint_budget *bud,
                          uint16_t *fb, uint16_t w, uint16_t h);

/*
 * The three primitives, for the board's plugin_board_bindings (issue #165):
 * the table is what the painter is filled from, and what the build derives the
 * painter veneers' roots from.  Not for calling directly.
 */
void paint_rect(void *ctx, const struct plugin_rect *r, uint16_t rgb565,
                uint16_t stroke);
void paint_fill_rect(void *ctx, const struct plugin_rect *r, uint16_t rgb565);
void paint_blit(void *ctx, const struct plugin_rect *r, const uint16_t *src,
                uint32_t src_stride, int32_t key);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_PAINT_H */
