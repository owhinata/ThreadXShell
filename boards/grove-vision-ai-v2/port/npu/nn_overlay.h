/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_overlay.h
 * @brief   Face detection over the live preview (issue #48).
 *
 * The NN half of cam_lcd_sink.h's overlay contract: inference on each published
 * frame, and the boxes drawn onto the staged image before it goes to the panel.
 *
 * WHY IT IS IN THE PORT AND NOT IN cmds/.  Both callbacks run on the CAMERA
 * PRODUCER THREAD, not on the shell thread that typed the command.  A file
 * under cmds/ that quietly executed there would be the kind of layering
 * accident that reads fine and is discovered during a debugging session.
 *
 * OWNERSHIP.  All state here is static and none of it is ever freed, which is
 * what makes the camera's lost-producer path survivable (see camera.h): if a
 * stop is never acknowledged, a producer still inside consume() keeps touching
 * this, and it must still be there.  The `nn` gate held by the command for the
 * whole preview is what keeps a second caller out.
 */
#ifndef NN_OVERLAY_H
#define NN_OVERLAY_H

#include <stdint.h>

#include "cam_lcd_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a preview did, for the command's closing summary. */
struct nn_overlay_stats {
	uint32_t inferences;   /**< frames run through the NPU               */
	uint32_t detections;   /**< faces drawn, summed over frames          */
	uint32_t skipped;      /**< frames not inferred (a stop was pending) */
	uint32_t errors;       /**< invoke or decode refused                 */
	uint32_t last_ms;      /**< the most recent inference, in ticks      */
	int      last_ndet;    /**< faces in the most recent frame           */
};

/**
 * @brief  Arm the overlay for one preview, and hand back the vtable to attach.
 *
 * Resets the counters and clears the stop flag. The model must already be open
 * and BlazeFace-shaped -- the caller checks that, because it can refuse before
 * starting a stream and this cannot.
 *
 * @return the vtable to pass to cam_lcd_sink_attach(); never NULL.
 */
const struct cam_lcd_overlay *nn_overlay_arm(void);

/**
 * @brief  Ask the overlay to stop doing work.
 *
 * [!] CALL THIS BEFORE camera_stream_stop(), always.
 *
 * It is checked at three points -- before preprocessing, immediately before the
 * invoke, and again after inference before the panel is touched -- so a frame
 * that has not yet begun the expensive part abandons it. That is what keeps an
 * ordinary Ctrl+C from waiting on an inference.
 *
 * It CANNOT cancel an invoke already waiting on the NPU: nothing can. It
 * narrows the window; the camera's lost-producer state is what makes the
 * remainder safe.
 */
void nn_overlay_request_stop(void);

/** Snapshot the counters. Safe at any time; each field is a single word. */
void nn_overlay_stats(struct nn_overlay_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_OVERLAY_H */
