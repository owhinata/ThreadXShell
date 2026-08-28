/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_camera.h
 * @brief   Live camera -> NN inference glue (owhinata/stm32f746g-disco#81, Epic
 * owhinata/stm32f746g-disco#80).
 *
 * Bridges the camera frame pipeline (port/camera) to the nn inference API
 * (port/nn/nn.h): a synchronous copy push sink resizes + converts each RGB565
 * camera frame into the model's int8 input, and a low-priority (prio 18)
 * best-effort worker thread runs inference on it.  Follows the nx_mjpeg.c
 * eth_sink lifecycle (thread created once + parked) and the codex-reviewed
 * double-buffer ownership rule (the buffer the worker feeds to nn_run() is never
 * written by the sink).  Since Epic owhinata/stm32f746g-disco#99 Phase 1
 * (owhinata/stm32f746g-disco#100) nncam is a plain camera *subscriber*: `ai stream
 * start/stop` enable/disable it and it attaches to the base capture (`camera stream`)
 * only while the base runs. A base detach (stop / DCMI overrun / cascade) PAUSES it
 * (frames stop) but keeps it enabled and holding the nn session; it re-attaches when
 * the base restarts.
 *
 * Drives `ai stream start|stop` and `ai run`.  Model-specific detection decode
 * (BlazeFace anchors + NMS) is layered above in port/nn/models
 * (owhinata/stm32f746g-disco#81 task 8); with the `null` backend this loop runs
 * end-to-end with 0 detections.
 */
#ifndef NN_CAMERA_H
#define NN_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"            /* enum camera_res */
#include "blazeface.h"  /* struct bf_det */

#ifdef __cplusplus
extern "C" {
#endif

struct nn_camera_stats {
	bool     running;
	uint8_t  res;          /**< enum camera_res of the active stream */
	uint32_t frames;       /**< frames delivered to the sink          */
	uint32_t drops;        /**< frames dropped (no free stage buffer)  */
	uint32_t infers;       /**< inferences completed                   */
	uint32_t errors;       /**< nn_run() failures                      */
	uint32_t last_us;      /**< latency of the last inference (us)     */
	uint32_t fps_x100;     /**< average inference rate x100 since start */
	uint32_t detections;   /**< detections from the last inference (task 8) */
};

/**
 * Enable live camera inference (`ai stream start`).  @p res is a display hint
 * only -- the input adapts to whatever geometry the base capture publishes
 * (owhinata/stm32f746g-disco#100).  Claims the single nn session (refused -6 if `ai
 * bench`/another stream holds it) and registers nncam as an RGB565 subscriber of the
 * base: it attaches immediately if the base is already running, otherwise it stays
 * enabled + idle and attaches at the next `camera stream start`.  Non-blocking.
 * Returns 0 or <0 (-2 already running / still tearing down / a teardown owns the
 * sink, -3 model, -4 geometry, -5 objects, -6 nn session busy).
 */
int  nn_camera_start(enum camera_res res);

/** Disable live inference (`ai stream stop`): unsubscribe from the base (which
 *  keeps running for other subscribers), wait for the producer to hand this
 *  sink's frame back, and release the nn session after the worker parks.
 *  Bounded waits (wall clock).  Returns:
 *    0   stopped;
 *   -1   not running;
 *   -2   the worker is still mid-inference -- it releases the session as it
 *        exits, and the sink is already released, so this is not a refusal;
 *   -7   the sink did not hand its frame back (issue #72).  A producer callback
 *        may still be preprocessing into our staging buffers, so `ai stream
 *        start` is refused until a later stop re-polls and finds it clear.
 *        Retryable by design: if the callback never returns, every retry keeps
 *        refusing, which is what a terminal state would have given anyway;
 *   -8   a start or another stop owns the lifecycle -- nothing was done. */
int  nn_camera_stop(void);

/** True while inference is enabled. */
bool nn_camera_running(void);

/** Snapshot current stats (any time). */
void nn_camera_stats_get(struct nn_camera_stats *out);

/** Copy the latest detections into @p out[0..max); returns the count copied. */
int nn_camera_dets_get(struct bf_det *out, int max);

/**
 * One decode's boxes and the diagnostics that belong to them.
 *
 * [!] READ TOGETHER OR NOT AT ALL (issue #97).  The worker publishes both under
 * one lock.  `ai stream stats` does not decode anything itself -- it reports what
 * the worker last published -- so fetching the boxes and then asking the decoder
 * for its "last" numbers paired this frame's boxes with whatever had been decoded
 * by the time it got round to printing.
 */
struct nn_camera_decode {
	int              valid;  /**< 0 = nothing decoded in this session yet     */
	int              ndet;   /**< faces, or -1 for "not a BlazeFace model"    */
	struct bf_result res;    /**< status, peak, pass/kept, threshold APPLIED  */
};

/**
 * Take a coherent snapshot of the last published decode.
 *
 * @param dets  optional; the boxes, up to @p max of them
 * @return non-zero if a snapshot was taken (zero before the first attach, when
 *         the lock does not exist yet).  `valid == 0` means this session has not
 *         decoded a frame -- which is NOT a decode that found nothing, and must
 *         not be printed as one.
 */
int nn_camera_decode_get(struct nn_camera_decode *out, struct bf_det *dets,
                         int max);

/** Runtime float32 input normalization: 1 = [-1,1], 0 = [0,1] (tuning, no reflash). */
void nn_camera_set_norm(int signed_range);
int  nn_camera_get_norm(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_CAMERA_H */
