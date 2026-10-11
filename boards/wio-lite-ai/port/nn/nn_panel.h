/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    nn_panel.h
 * @brief   The nn half of the preview thread (issue #140): whether to paint, the
 * record it paints from, the plugin's draw() and what it spent.
 *
 * The display half -- the band transposes, the frame lock, the flip -- stays in
 * src/cam_preview.c.  port/nn may not include port/ltdc, so the display is handed
 * in as hooks (struct nn_panel_display) by the one caller.  The order is this
 * board's: the result lease (a TRY), then the frame lock, which is LEFT HELD
 * across the caller's flip; the lease is given back before the flip.
 */
#ifndef NN_PANEL_H
#define NN_PANEL_H

#include <stdint.h>

#include "nn_core_frame.h"   /* struct nn_core_panel, enum nn_core_draw (#130) */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * What the panel needs from the display (src/cam_preview.c passes the LTDC
 * driver's own functions).
 *
 * [!] back_buffer / surface_w / surface_h ARE CALLED ONLY WITH frame_lock HELD.
 * A back buffer sampled before the lock can be the FRONT one after a flip from
 * another thread; ltdc_back_buffer() is valid only under ltdc_lock_frame().
 */
struct nn_panel_display {
	void      (*frame_lock)(void);   /* recursive; left held for the flip */
	uint16_t *(*back_buffer)(void);
	uint16_t  (*surface_w)(void);
	uint16_t  (*surface_h)(void);
};

/**
 * The panel's half of the shared frame path (issue #130): try the plugin lease,
 * take @p p's frame lock, check the record and let the plugin paint, on this
 * board's frame-path state.  Returns enum nn_core_draw; on DECLINED or PAINTED
 * the frame lock is HELD for the caller to present under and release.
 */
int  nn_camera_draw(const struct nn_core_panel *p, void *ctx);
/** The painted frame is out (svc/nn_core_frame.h). */
void nn_camera_present_done(void);

#if defined(CONFIG_NN_BACKEND_TFLM)
/**
 * nn_camera_draw() with this board's panel hooks over @p d.  Same return and
 * the same frame-lock rule: on DECLINED or PAINTED the lock is held.
 */
int  nn_panel_draw(const struct nn_panel_display *d);

/**
 * What a loaded plugin's draw() has spent on the panel: the high-water pixel
 * charge of any one frame, and how many primitives were refused for want of
 * budget (issue #110).  Either argument may be NULL.
 *
 * A pixel budget is not a bound on hold time -- these pixels are CPU stores
 * into non-cacheable external PSRAM -- so this is one of the numbers the board
 * README's acceptance criteria are stated against, not the whole of it.
 */
void cam_preview_plugin_draw_stats(uint32_t *spent, uint32_t *refused);

/**
 * Begin a fresh accounting period for the above and for the lease misses.
 *
 * Called when a stream is ARMED, not when one stops: `nn stream stats` right
 * after a stop still has to describe the run that just ended.
 */
void cam_preview_plugin_draw_arm(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NN_PANEL_H */
