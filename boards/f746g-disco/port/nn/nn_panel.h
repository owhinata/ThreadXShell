/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_panel.h
 * @brief   The nn half of the GUIX camera preview: whether a plugin paints this
 *          frame, the record it paints from, its draw() and what it spent
 *          (issue #159 = #78 Step 4b).  tflm only.
 *
 * This board has no panel thread.  The GUIX thread (priority 14) stores each
 * camera frame's copy in its view buffer and, in its CAMERA_FRAME handler, draws
 * the overlay on it before the camera icon is redrawn from it
 * (ui/guix_camera_ui.c, owhinata/stm32f746g-disco#83).  That handler is the panel:
 * it calls nn_panel_draw() with the display's hooks -- port/nn does not include
 * the GUIX or LTDC ports -- and the order is this board's, the same as
 * wio-lite-ai's: the plugin lease (a TRY, svc/nn/nn_core_frame.c's
 * nn_core_draw()), then the frame lock, then the shared questions (the overlay
 * is derived, the record is valid, of this session, and the plugin's), then the
 * plugin's draw().  The frame lock is released here, not left to the caller.
 */
#ifndef NN_PANEL_H
#define NN_PANEL_H

#include <stdint.h>

#include "nn_core_frame.h"   /* enum nn_core_draw */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * What the panel needs from the display (ui/guix_camera_ui.c passes the GUIX
 * and LTDC ports' own functions).
 *
 * [!] view() AND touched() ARE CALLED ONLY WITH frame_lock HELD.  The view
 * buffer and its geometry are the preview session's, set and torn down under
 * the same lock.
 */
struct nn_panel_display {
	void      (*frame_lock)(void);
	void      (*frame_unlock)(void);
	/** The view buffer and its geometry, or NULL when no preview is armed. */
	uint16_t *(*view)(uint16_t *w, uint16_t *h);
	/** The CPU wrote the view buffer: both LTDC buffers are stale. */
	void      (*touched)(void);
};

/**
 * Let a loaded plugin paint this frame into the view buffer.
 *
 * @return NN_CORE_DRAW_NONE when no plugin is in force -- the caller draws the
 *         resident decoder's boxes instead (Step 4b keeps that fallback);
 *         otherwise MISSED (the lease was busy: the frame goes out bare,
 *         counted), DECLINED (no stream to annotate -- asked before the lease
 *         is tried, so a preview with no stream counts no misses -- or the
 *         record said no) or PAINTED.  Nothing is left locked on return.
 */
enum nn_core_draw nn_panel_draw(const struct nn_panel_display *d);

/**
 * What a plugin's draw() has spent: the high-water pixel charge of any one
 * frame, the primitives refused for want of budget, and the longest the frame
 * lock was held for a plugin draw, in microseconds.  Any argument may be NULL.
 */
void nn_panel_stats(uint32_t *spent, uint32_t *refused, uint32_t *hold_us);

/** The pixel budget one draw() gets on a @p w x @p h frame. */
uint32_t nn_panel_budget(uint16_t w, uint16_t h);

/**
 * Begin a fresh accounting period for the above and for the lease misses.
 * Called when a stream starts, not when one stops: `nn stream stats` right
 * after a stop still describes the stream that ended.
 */
void nn_panel_arm(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_PANEL_H */
