/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    cam_band.h
 * @brief   The camera's one band sink, run as two steps: the LCD preview, then
 *          the NN ingest (owhinata/wio-lite-ai#9 phase 3; issue #142).
 *
 * port/camera exposes ONE band callback, and two consumers want 60-row bands:
 * the LCD preview (src/cam_preview.c) and the NN ingest
 * (port/nn/nn_producer.c).  This module is that callback -- the band sink -- and
 * runs each consumer as a STEP of it, in a fixed order: the preview first, then
 * the NN, so the picture never waits on inference bookkeeping.  Every step
 * makes its own frame-level decision: the preview dropping a frame (a flip
 * still pending, the display off, the frame lock busy) must not make the NN skip
 * one, and vice versa.
 *
 * [!] A STEP IS NOT A REASON FOR THE STREAM TO RUN (issue #142).  Who holds the
 * stream open is port/camera/camera.c's holder table -- `camera stream start`
 * and nn -- and the band stream runs while somebody holds it.  The steps only
 * say who is fed while it runs: the preview's step is armed for the board's
 * whole life (it decides per frame, from the display's state, whether to draw),
 * the NN's while an `nn stream` / `nn run` is in progress.  This used to be a
 * refcount of PREVIEW and NN claims that started and stopped the stream itself.
 *
 * THE STEP MACHINE is svc/cam_step.h's, shared with f746g-disco's camera sink:
 * the producer reads a step's ARMED mark and counts itself in (BUSY) in one
 * PRIMASK section, so once an owner has disarmed a step and seen BUSY reach 0
 * the producer is out of it and cannot come back -- the proof the NN stop needs
 * before it gives the input tensor back.
 *
 * Callbacks are supplied when a step is enabled rather than looked up here, and
 * that is a layering requirement: this file is built whenever BSP_ENABLE_CAMERA
 * is on, while cam_preview.c needs BSP_ENABLE_CAMERA *and* BSP_ENABLE_LCD.
 *
 * Threading: enable / disable are a PRIMASK section each; drain sleeps (thread
 * context only).  The sink runs on the camera's producer thread, never an ISR.
 */
#ifndef CAM_BAND_H
#define CAM_BAND_H

#include <stdint.h>

/** Step order is this enum's order: the preview is served first. */
enum cam_band_client {
	CAM_BAND_PREVIEW = 0,
	CAM_BAND_NN      = 1,
	CAM_BAND_CLIENTS
};

/**
 * One band, on the camera's producer thread.  @p px is CAMERA_BAND_ROWS rows of
 * RGB565, CAMERA_FRAME_WIDTH wide, in AXI-SRAM, valid ONLY for the duration of the
 * call and read-only (the DMA owns it).  Bands arrive 0..3 in order with no gaps
 * inside a frame.  Whatever runs here delays the next band, so it must finish well
 * inside a band period (~18.5 ms at ~13.5 fps).
 */
typedef void (*cam_band_client_fn)(unsigned band, const uint16_t *px, unsigned rows);

#define CAM_BAND_OK         0
#define CAM_BAND_ERR_ARG   (-1)  /**< bad client id or NULL callback              */
#define CAM_BAND_ERR_BUSY  (-3)  /**< enable: armed, or the producer is still
                                  *   inside from before; drain: still inside   */

/** The band sink itself: port/camera/camera.c starts the band stream with it. */
void cam_band_sink(void *ctx, unsigned band, const uint16_t *px, unsigned rows);

/**
 * Arm step @p c with @p fn.  Refused (CAM_BAND_ERR_BUSY, nothing moved) while it
 * is armed or the producer is still inside it from before -- an owner that
 * initialises its step state just before arming needs nobody reading it.
 */
int cam_band_step_enable(enum cam_band_client c, cam_band_client_fn fn);

/** Disarm step @p c.  Idempotent.  The producer may still be inside: drain. */
void cam_band_step_disable(enum cam_band_client c);

/**
 * Wait (wall clock, at most @p ticks) for the producer to leave a DISABLED step.
 *
 * [!] ON CAM_BAND_ERR_BUSY THE CALLER MUST NOT FREE ANYTHING THE STEP TOUCHES.
 * The drain waits for an in-flight callback to return; if it does not (a producer
 * stuck mid-callback), the step's buffers may still be written.  For the NN that
 * means the model session and the OCTOSPI1 guard stay held.  Retryable: a later
 * drain finishes what this one could not, and the step cannot be re-armed until
 * one has.
 */
int cam_band_step_drain(enum cam_band_client c, uint32_t ticks);

/** Is step @p c armed? */
int cam_band_step_armed(enum cam_band_client c);

#endif /* CAM_BAND_H */
