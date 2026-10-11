/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    cam_preview.h
 * @brief   Live camera preview on the LCD (owhinata/wio-lite-ai#8 phase 3c,
 * owhinata/wio-lite-ai#35).
 *
 * Shows the camera on the panel, rotated into the landscape drawing surface
 * (owhinata/wio-lite-ai#38).  It is the first step of the camera's band sink
 * (port/camera/cam_band.c, issue #142): the band stream lands 60-row slices in
 * AXI-SRAM, the preview transposes each band into the LTDC back buffer as it
 * arrives, and presents once the fourth one is in.  It owns no stream of its own
 * -- whoever holds the camera (`camera stream start`, `nn stream start`) does.
 *
 * That replaced pinning whole frames out of the PSRAM ring, where the transpose's
 * strided side was the external OCTOSPI1 -- 25 ms of CPU per frame and enough
 * extra bus traffic to make the DCMI's FIFO complain.  (The whole-frame stream
 * into that ring was removed by issue #142.)
 *
 * This lives in app/ rather than in either driver because it is the one thing
 * that needs BOTH -- port/camera for the pixels and port/ltdc for the display --
 * and port/ modules do not reach sideways into each other.  It is the same shape
 * as app/nx_net.c gluing NetX Duo to the RTL8720 link.
 */
#ifndef CAM_PREVIEW_H
#define CAM_PREVIEW_H

#include <stdint.h>

/**
 * The preview thread's stack, in DTCM.  Published here since issue #108 because
 * the plugin policy in port/nn/nn_svc_wio.c asserts its allowance for a draw
 * callback strictly below it.
 *
 * [!] 1024 UNTIL ISSUE #110, WHEN THE THREAD GOT A NEW JOB.  It used to flip
 * buffers and draw up to eight rectangles; it now hosts a loaded plugin's
 * draw() and the painter under it, and the derived allowance at 1024 --
 * 1024 - 105 measured at the call site - 208 asynchronous reserve = 711 -- left
 * the detector's 588 B fitting by margin thin enough that any plugin with a
 * label to draw would not.  Sizing the allowance to what today's plugin happens
 * to need is how a limit stops being one; the thread is sized for its work
 * instead.  512 B out of the 4,544 DTCM has spare.
 */
#define CAM_PREVIEW_STACK_BYTES  1536u

#ifdef __cplusplus
extern "C" {
#endif

/** Create the preview thread (parked) and arm the preview's step of the band
 *  sink.  Call from tx_application_define(). */
int cam_preview_init(void);

/*
 * There is no "turn the preview on" any more (issue #142): `camera preview
 * on|off` is gone.  The preview is what the panel shows while the camera
 * streams AND the display is on -- `camera stream start` / `nn stream start`
 * and `lcd on`.  cam_preview_init() arms its step of the band sink for good.
 */

/** Frames actually presented, frames skipped (the previous one not presented
 *  yet, or the frame lock held by another thread -- the producer never waits
 *  for it, issue #142), and how long the most recent frame's transpose took in
 *  microseconds -- summed over its four bands, so it stays directly comparable to
 *  the 25.0 ms per frame the whole-frame-from-PSRAM version measured before issue
 *  owhinata/wio-lite-ai#35.  Reported by `camera info`.  Any argument may be
 * NULL. */
void cam_preview_stats(uint32_t *shown, uint32_t *dropped, uint32_t *blit_us);

/** Zero shown / dropped (`camera stream start` that lit the camera).  A count
 *  the producer is incrementing at that moment may survive: the producer
 *  outranks the console, so the two stores cannot interleave mid-update. */
void cam_preview_stats_reset(void);

/* The plugin-draw counters (cam_preview_plugin_draw_stats() / _arm()) moved
 * with the rest of this thread's nn half to port/nn/nn_panel.h (issue #140). */

#ifdef __cplusplus
}
#endif

#endif /* CAM_PREVIEW_H */
