/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_paint_budget.h
 * @brief   What one plugin draw() may spend, and the one place it is charged
 *          (issues #105, #110).
 *
 * WHAT IS SHARED AND WHAT IS NOT.  Two boards let a plugin paint, and their
 * drawing loops have nothing in common: one stores byte-swapped RGB565 into a
 * staged framebuffer in SRAM, the other stores native RGB565 into a rotated
 * surface in external PSRAM.  What they must not differ about is the ACCOUNTING,
 * because that is the part issue #105 got wrong -- an outline charged for the
 * area it enclosed rather than the pixels it wrote, so one close-up face
 * exceeded the cap and its box silently vanished.  A rule that subtle should
 * have one implementation.
 *
 * [!] WHAT THE BUDGET BOUNDS IS TIME SPENT WITH THE PANEL GUARD HELD.  It is
 * denominated in pixels because that is proportional to the work and cheap to
 * compute before the framebuffer is touched -- not because a pixel is a fixed
 * number of cycles.  A board still has to MEASURE the hold time its allowance
 * buys, and say so; issue #110 measured a loop that pairs two pixels into one
 * store, and a colour-keyed blit reads source pixels it never writes.
 *
 * [!] AND IT IS NOT A BOUND ON WHAT draw() COMPUTES.  A plugin may do as much
 * arithmetic as it likes between primitives; what is capped is the work the
 * BASE performs on its behalf.  The expensive rasterising belongs in decode(),
 * which runs on the producer with no guard held.
 */
#ifndef PLUGIN_PAINT_BUDGET_H
#define PLUGIN_PAINT_BUDGET_H

#include <stdint.h>

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
 * @brief  Charge @p pixels of work plus one dispatch, or refuse.
 *
 * @return non-zero when the caller may proceed; zero when it must draw nothing
 *         at all.
 *
 * [!] CALL IT BEFORE THE FRAMEBUFFER IS TOUCHED.  A refused primitive that had
 * already written half a box looks like a rendering bug rather than a budget.
 *
 * [!] AND THE COMPARISON CANNOT ITSELF OVERFLOW.  The form is
 * `cost <= remaining`, never `spent + cost <= limit` -- the second is exactly
 * what a nonsense rectangle wraps, and then the comparison that was supposed to
 * be the guard passes.
 */
int plugin_paint_charge(struct plugin_paint_budget *b, uint32_t pixels);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_PAINT_BUDGET_H */
