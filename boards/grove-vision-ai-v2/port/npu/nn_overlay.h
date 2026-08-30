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
 * this, and it must still be there.  The `nn` gate, held for the whole life of
 * the stream, is what keeps a second caller out.
 */
#ifndef NN_OVERLAY_H
#define NN_OVERLAY_H

#include <stdint.h>

#include "cam_lcd_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a stream has done, for `nn stream stats`. */
struct nn_overlay_stats {
	uint32_t inferences;   /**< frames run through the NPU               */
	uint32_t detections;   /**< faces drawn, summed over frames          */
	uint32_t skipped;      /**< frames not inferred (a stop was pending) */
	uint32_t errors;       /**< invoke or decode refused                 */
	/*
	 * [!] Two kinds of decode failure, counted apart (issue #97).  There is no
	 * console on the producer thread, so a summary is the only place a failure
	 * can be explained -- and "the open model is not BlazeFace" calls for
	 * opening a different model while "the decoder is not initialised" calls
	 * for looking at the firmware.  One `errors` total cannot say which.
	 */
	uint32_t model_errors;   /**< the tensors were not BlazeFace-shaped   */
	uint32_t decoder_errors; /**< the decoder itself refused (a wiring fault) */
	uint32_t last_ms;      /**< the most recent inference, in ticks      */
	int      last_ndet;    /**< faces in the most recent frame           */

	/*
	 * The producer-side stage split (issue #60).  `camera stats` prints one
	 * `sink` number for everything a sink does on the producer; under
	 * `nn stream` almost all of it is this overlay's process(), and which
	 * STAGE of process() owns it decides what is worth optimising.  Totals
	 * since arm, over prof_frames frames -- only frames that completed all
	 * three stages are counted, so the three rows describe the same set.
	 */
	uint32_t prof_frames;  /**< frames in the stage totals below          */
	uint32_t prep_us;      /**< tensor setup + crop/resize into the input */
	uint32_t invoke_us;    /**< the NPU inference                        */
	uint32_t decode_us;    /**< anchor decode into boxes                 */
	int      prof_ok;      /**< the EPK clock backing them is trusted    */

	/*
	 * Stack already spent at each decoder call site (issue #103), high-water.
	 *
	 * [!] NOT THE THREAD'S PEAK.  `thread` scans ThreadX's fill and reports the
	 * deepest a thread ever got anywhere; this is how much is spent AT THE
	 * INSTANT a decoder is entered, which is the only one of the two that says
	 * what a callee may have.  The plugin admission policy of issue #103 is
	 * computed from these, not from the peak.
	 */
	uint32_t depth_decode; /**< producer thread, at the decode call site  */
	uint32_t depth_draw;   /**< panel thread, at the draw call site       */

	/* What a plugin's draw() actually spends of its painter budget, and how
	 * often it was refused (issue #103).  Reported so the cap is judged
	 * against a measurement rather than defended in the abstract. */
	uint32_t draw_spent;   /**< high-water pixels charged in one draw     */
	uint32_t draw_refused; /**< primitives refused for want of budget     */
};

/**
 * @brief  Arm the overlay for one stream, and hand back the vtable to attach.
 *
 * Resets the counters and clears the stop flag. The model must already be open
 * and BlazeFace-shaped -- the caller checks that, because it can refuse before
 * starting a stream and this cannot.
 *
 * @return the vtable to pass to cam_lcd_sink_attach_and_stream(); never NULL.
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

/**
 * Snapshot the counters.  Safe at any time: the word-sized counters need no
 * care, and the 64-bit stage accumulators behind prep/invoke/decode are copied
 * under a critical section (issue #60), same as the camera's own profile.
 */
void nn_overlay_stats(struct nn_overlay_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_OVERLAY_H */
