/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint.h
 * @brief   The painter a plugin draws through, and its budget (issue #103).
 *
 * [!] THE PLUGIN NEVER GETS THE FRAMEBUFFER POINTER.  The base keeps the buffer
 * and its geometry here and validates every rectangle against them, so a bug in
 * loaded code cannot write outside the frame.  That is the ONE property this
 * boundary provides -- it is not a sandbox, and everything AGENTS.md records
 * about what the plugin gates do not prove still applies.
 *
 * [!] AND IT IS DELIBERATELY NOT JUST `rect`.  A painter that could only draw
 * hollow boxes would be a detector-only painter, which reproduces the very
 * asymmetry issue #78 exists to remove, one layer up.  @ref plugin_painter's
 * `blit` is the general escape hatch: a plugin rasterises glyphs, a mask or a
 * skeleton into its OWN buffer during decode() -- on the producer thread, with
 * no guard held -- and hands over spans here.
 *
 * THE BUDGET, AND WHAT IT IS AND IS NOT.  draw() runs on the panel thread with
 * the panel guard held, and everything else that wants the panel is failing its
 * non-blocking acquire meanwhile.  So the work the BASE does on a plugin's
 * behalf is capped: pixels visited after clipping, every source pixel a
 * colour-key blit must read (including the transparent ones -- they cost a read
 * and a compare whether or not they are written), and the dispatch count.  The
 * charge is made BEFORE a primitive touches the framebuffer, so a refused
 * primitive leaves it unchanged rather than half-drawn.
 *
 * [!] IT IS NOT A BOUND ON ARBITRARY COMPUTATION INSIDE draw().  A plugin is
 * trusted native code; nothing here stops it spending a millisecond on
 * arithmetic before it calls anything.  What this bounds is the rendering the
 * base performs, which is the part the base is in a position to refuse.  The
 * guard contract in lcd_st7789.h is the other half and is not enforceable.
 */
#ifndef PLUGIN_PAINT_H
#define PLUGIN_PAINT_H

#include <stdint.h>

#include "plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What one draw() may spend, in "pixels visited" plus a charge per call. */
struct plugin_paint_budget {
	uint32_t pixels;      /**< remaining; decremented as work is charged  */
	uint32_t ops;         /**< remaining primitive calls                  */
	uint32_t refused;     /**< primitives refused for want of budget      */
};

/** Charged per primitive call, on top of the pixels it visits: a call that
 *  clips away entirely still costs a dispatch. */
#define PLUGIN_PAINT_OP_COST 1u

/**
 * @brief  Bind a painter to a framebuffer for the duration of one draw().
 *
 * @param p        filled in; hands to the plugin
 * @param bud      reset by the caller to this frame's allowance
 * @param fb       the staged frame, in WIRE byte order
 * @param w, h     its geometry
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
