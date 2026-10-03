/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_capture_sink.h
 * @brief   A frame sink with no panel, for `nn run` (issue #129).
 *
 * `nn run` infers on the same worker a stream does, fed the same way: the
 * camera producer prepares a frame into the model's input from inside
 * consume() and hands it over.  It used to stop the camera on one frame with
 * camera_capture() and prepare it on the console; a one-shot that ran that way
 * would be a second way in to the per-frame path.  So it streams, through this
 * sink, and the worker takes the first frame it is offered.
 *
 * WHY NOT THE PANEL SINK.  `nn run` has never lit the panel (neither does wio's),
 * and it works on a board whose panel was never brought up.  This sink touches
 * no LCD and has no thread.
 *
 * THE CONTRACT (svc/frame_pipeline.h), and how this keeps it:
 *   - ONE PIN, ONE PUT, EVERY FRAME.  consume() is synchronous: it calls the
 *     process hook and then camera_frame_put() before it returns, whether the
 *     hook took the frame or not.  Frames after the first are not inferred --
 *     the hook declines them -- and are handed straight back.
 *   - REGISTERED ONLY WITH THE STREAM START.  The attach is
 *     camera_stream_start(&sink), the camera's one indivisible operation (issue
 *     #63); there is no other subscribe.  On any failure nothing is linked.
 *   - DETACHED ONLY AFTER A CONFIRMED STOP, and only once the pipeline agrees
 *     it holds nothing: unlink, then every accepted frame handed back, then
 *     camera_sink_pins() == 0.  A refusal there latches the sink LOST -- it is
 *     never repaired, the same answer cam_lcd_sink.c gives.
 *
 * The return codes are the camera's, so the stop's table
 * (port/npu/nn_stream_state.c) reads this detach exactly as it reads the panel's:
 * CAM_ERR_BUSY is retryable, anything else but CAM_OK is terminal.
 */
#ifndef CAM_CAPTURE_SINK_H
#define CAM_CAPTURE_SINK_H

#include "cam_lcd_sink.h"   /* struct cam_lcd_overlay: only .process is used */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the camera with this sink attached.  @p ov->process is called on the
 * producer for every frame, its answer ignored; @p ov must stay valid until
 * cam_capture_sink_detach() returns CAM_OK.
 *
 * @return CAM_OK with the stream running; CAM_ERR_BUSY if this sink is already
 *         attached or another command owns the camera; CAM_ERR_STATE if a
 *         previous detach could not be completed; else the camera's refusal.
 *         On failure nothing is attached.
 */
int cam_capture_sink_attach_and_stream(const struct cam_lcd_overlay *ov);

/**
 * Unlink after camera_stream_stop() returned CAM_OK.
 * @return CAM_OK (also when nothing is attached); CAM_ERR_BUSY retry;
 *         CAM_ERR_STATE refused for good -- the sink stays owned.
 */
int cam_capture_sink_detach(void);

#ifdef __cplusplus
}
#endif

#endif /* CAM_CAPTURE_SINK_H */
