/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint.h
 * @brief   What a plugin may paint with on this board (issue #110 = #78
 *          Step 3b).
 *
 * The plugin is handed a @ref plugin_painter, never the framebuffer: the base
 * keeps the buffer and its geometry and validates every rectangle against them,
 * so a bug in loaded code cannot write outside the frame.  That is the ONE
 * property this boundary provides; it is not a sandbox, and everything in
 * plugin_exec.h's honest-scope note still applies.
 *
 * [!] IT DRAWS WITH THE CPU, INCLUDING THE FILLS, AND THAT IS THE DESIGN.  The
 * panel's own primitives (ltdc_fill_rect) run on DMA2D, and a plugin's draw()
 * must not: the frame transaction ends when ltdc_flip() presents, and this port
 * has no way to establish that an outstanding DMA2D transfer has stopped
 * writing before then.  The small-transfer path polls with a HAL call that
 * returns WITHOUT aborting on timeout, ltdc_dma2d_fill() discards that result,
 * and ltdc_fill_rect() returns void -- so a timed-out fill can still be in
 * flight when its destination becomes the displayed buffer, and suppressing
 * that one flip does not help because the next producer frame reuses the
 * buffer.  Repairing that is the port's own problem and its own issue; a
 * painter that never arms a transfer does not have it.  The rotating blit this
 * panel already uses (svc/gfx_rot.c) writes the same framebuffer with the CPU,
 * so this is the access the panel already performs, not a new one.
 *
 * [!] THE COORDINATES ARE LANDSCAPE 320x240, like everything else that draws
 * here, and the rotation to the panel's 240x320 happens in the loops.
 *
 * [!] AND THE BYTE ORDER IS NATIVE.  grove-vision-ai-v2 swaps every pixel on
 * the way in because the ST7789 takes the high byte first over SPI; the LTDC
 * scans out little-endian RGB565 straight from memory and must not inherit it.
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
 * @brief  Bind a painter to a back buffer for the duration of one draw().
 *
 * @param p     filled in; handed to the plugin
 * @param bud   reset by the caller to this frame's allowance
 * @param fb    the back buffer, in the panel's PORTRAIT layout
 * @param sw    the landscape surface width  (the panel's height)
 * @param sh    the landscape surface height (the panel's width)
 *
 * The painter is valid only until draw() returns.  Nothing here is stored
 * anywhere a later call could reach.
 */
void plugin_paint_bind(struct plugin_painter *p,
                       struct plugin_paint_budget *bud,
                       uint16_t *fb, uint16_t sw, uint16_t sh);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_PAINT_H */
