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
 * GUIX thread's CAMERA_FRAME handler between the store and the redraw, as the
 * resident decoder's boxes always have (owhinata/stm32f746g-disco#83), and its
 * strokes ride the buffer into both LTDC framebuffers through the same
 * freshness flags as the frame.  So the coordinates are the camera frame's --
 * w x h, QVGA or QQVGA, row-major with stride w, not rotated -- and the byte
 * order is native RGB565, which the DMA2D and the LTDC both read as is.
 *
 * [!] IT DRAWS WITH THE CPU, INCLUDING THE FILLS, AND THAT IS THE DESIGN.  The
 * view buffer is non-cacheable SDRAM (bank 1), so CPU stores are coherent with
 * the DMA2D reads that later blit it -- the access the resident overlay already
 * performs (guix_display_cam_overlay_box()).  A DMA2D fill would need a way to
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
 * @brief  Bind a painter to the view buffer for the duration of one draw().
 *
 * @param p     filled in; handed to the plugin
 * @param bud   reset by the caller to this frame's allowance
 * @param fb    the view buffer: @p w x @p h RGB565, row-major, stride @p w
 * @param w     its width
 * @param h     its height
 *
 * The painter is valid only until draw() returns.  Nothing here is stored
 * anywhere a later call could reach.
 */
void plugin_paint_bind(struct plugin_painter *p,
                       struct plugin_paint_budget *bud,
                       uint16_t *fb, uint16_t w, uint16_t h);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_PAINT_H */
