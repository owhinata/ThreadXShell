/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    guix_camera_ui.h
 * @brief   Camera GUIX app (owhinata/stm32f746g-disco#61/#68): the camera UI that
 * merges the former guix_app (widget tree) and guix_camera (frame sink + preview
 *          controller) into one application-layer module.
 *
 * This is the presentation layer (ui/), above port/guix.  The board boots with
 * the UI ON (owhinata/stm32f746g-disco#60) showing the live camera preview (RGB565,
 * default QVGA owhinata/stm32f746g-disco#84, drawn native 1:1 centred on the 480x272
 * panel). The UI has two full-screen pages (owhinata/stm32f746g-disco#68): the clean
 * live preview, and a settings page reached by tapping the image that holds the
 * OV5640 image-quality controls, a preview-resolution selector (qqvga/qvga,
 * owhinata/stm32f746g-disco#69/#84) and Back (see guix_camera_ui.c). Lifecycle:
 *
 *   camera_ui_init()   register the GUIX widget-tree builder with guix_glue
 *                      (boot-safe: no GUIX/camera I/O).  Call once from
 *                      tx_application_define().
 *   camera_ui_start()  bring GUIX up (or resume) and subscribe the live preview.
 *                      Shared by the boot path (owhinata/stm32f746g-disco#60) and
 * `gui start`. Boot-safe:  it only starts GUIX and posts a one-shot autostart event;
 *                      the base-capture bring-up + camera probe (blocking I2C) runs
 *                      LATER on the GUIX system thread, never in
 *                      tx_application_define().
 *   camera_ui_stop()   unsubscribe the preview (the base keeps running), blank the
 *                      screen and hand the display back to `lcd` (`gui stop`).
 *                      Thread context only.
 *
 * Why the deferral: the boot base-capture bring-up (camera_stream_start()) probes
 * the OV5640 over I2C (blocking), which must not run before the scheduler.
 * camera_ui_start() therefore posts GX_EVENT_CAMERA_AUTOSTART; the GUIX thread
 * subscribes the preview and (once, at boot) starts the base once scheduling is
 * live.  A volatile flag protocol serialises that against a shell-thread `gui stop`.
 */
#ifndef GUIX_CAMERA_UI_H
#define GUIX_CAMERA_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes beyond the GUIX_* ones the bring-up passes through (issue #72).
   Kept clear of GUIX_ERR (-1) and GUIX_ERR_STATE (-2), which camera_ui_start()
   still returns unchanged from guix_start(). */
#define CAMERA_UI_OK         0
#define CAMERA_UI_ERR_BUSY  -3   /* a start or a teardown owns the preview sink */
#define CAMERA_UI_ERR_PINS  -4   /* the sink did not hand its frame back        */

/** Register the GUIX widget-tree builder with guix_glue.  No GUIX/camera I/O, so
 *  it is safe to call from tx_application_define() before the scheduler.  Call
 *  once at boot, before any camera_ui_start(). */
void camera_ui_init(void);

/** Bring the GUIX camera UI up (or resume it) and request the live preview.
 *  Shared by the boot path (owhinata/stm32f746g-disco#60) and `gui start`.
 * Boot-safe. Returns 0 on success, a negative GUIX bring-up error
 * (GUIX_ERR_STATE: display down), or CAMERA_UI_ERR_BUSY while a preview
 * teardown still owns the sink (issue #72). */
int camera_ui_start(void);

/** Stop the live preview (unsubscribe from the base -- the base keeps running for
 *  other subscribers), blank the screen and hand the display back to `lcd`
 *  (`gui stop`).  Thread context only.  Idempotent.  Since Epic
 * owhinata/stm32f746g-disco#99 Phase 1 (owhinata/stm32f746g-disco#100) this no
 * longer stops the base capture nor the AI subscriber.
 *
 * Returns 0, or:
 *   CAMERA_UI_ERR_BUSY  another start/stop owns the lifecycle -- nothing done;
 *   CAMERA_UI_ERR_PINS  the sink did not hand its frame back inside the budget.
 *
 * [!] CAMERA_UI_ERR_PINS MUST NOT BE REPORTED AS A STOP (issue #72).  The
 * preview surface is still armed and GUIX still holds the LCD, precisely
 * because a producer callback may still be copying into it.  It is recoverable
 * rather than terminal: a later `gui stop` re-polls and finishes the teardown,
 * and `gui start` uses the existing restart path. */
int camera_ui_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* GUIX_CAMERA_UI_H */
