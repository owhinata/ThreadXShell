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
 * skeleton into its OWN buffer during decode() -- on the inference worker
 * since issue #129, with no panel guard held -- and hands over spans here.
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
#include "plugin_paint_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The budget and the charge are shared with the other board that lets a plugin
 * paint (svc/plugin/plugin_paint_budget.h, issue #110): the loops have nothing in
 * common, the accounting is the part issue #105 got wrong, and a rule that
 * subtle should have one implementation. */

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
