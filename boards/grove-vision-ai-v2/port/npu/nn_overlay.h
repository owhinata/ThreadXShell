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
 * WHY IT IS IN THE PORT AND NOT IN cmds/.  None of it runs on the shell thread
 * that typed the command: process() runs on the CAMERA PRODUCER, draw() on the
 * panel thread, and since issue #129 the invoke, decode and publish on the
 * INFERENCE WORKER (nn_worker.h), through nn_overlay_work().  A file under
 * cmds/ that quietly executed there would be the kind of layering accident
 * that reads fine and is discovered during a debugging session.
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
	uint32_t skipped;      /**< frames not inferred: a stop was pending, or
	                            the worker was busy (issue #129)          */
	uint32_t busy;         /**< of those, the worker was busy            */
	uint32_t frames;       /**< frames handed to process() (issue #129)  */
	uint32_t lease_timeouts; /**< inferred, but the lease was not had in
	                              time: not decoded (also in errors)     */
	uint32_t errors;       /**< invoke or decode refused                 */
	/*
	 * [!] Two kinds of decode failure, counted apart (issue #97).  There is no
	 * console on the thread that decodes (the producer then, the inference
	 * worker since issue #129), so a summary is the only place a failure
	 * can be explained -- and "the open model is not BlazeFace" calls for
	 * opening a different model while "the decoder is not initialised" calls
	 * for looking at the firmware.  One `errors` total cannot say which.
	 */
	uint32_t model_errors;   /**< the tensors were not BlazeFace-shaped   */
	uint32_t decoder_errors; /**< the decoder itself refused (a wiring fault) */
	uint32_t last_ms;      /**< the most recent inference, in ticks      */
	int      last_ndet;    /**< faces in the most recent frame           */

	/*
	 * The stage split (issue #60), across two threads since issue #129:
	 * `prep` is what the producer still spends inside consume() -- the part
	 * of `camera stats`' `sink` number this overlay owns -- and invoke,
	 * decode and the round trip are the worker's.  Totals since arm, over
	 * prof_frames frames -- only frames that completed every stage are
	 * counted, so the rows describe the same set.
	 */
	uint32_t prof_frames;  /**< frames in the stage totals below          */
	uint32_t prep_us;      /**< tensor setup + crop/resize into the input */
	uint32_t invoke_us;    /**< the NPU inference                        */
	uint32_t decode_us;    /**< the plugin's decode, under its lease     */
	uint32_t cycle_us;     /**< the worker's round trip, hand-over to
	                            publish (issue #129)                     */
	int      prof_ok;      /**< the EPK clock backing them is trusted    */

	/* The stack depth where a plugin is entered is not here any more: it is
	 * per slot and per thread, and taken at the entry itself rather than in
	 * this file's call sites (port/npu/nn_probe.h, issue #119). */

	/* What a plugin's draw() actually spends of its painter budget, and how
	 * often it was refused (issue #103).  Reported so the cap is judged
	 * against a measurement rather than defended in the abstract. */
	uint32_t draw_spent;   /**< high-water pixels charged in one draw     */
	uint32_t draw_refused; /**< primitives refused for want of budget     */

	/* How many frames the result drawn is behind the frame it is drawn on
	 * (issue #129): summed and counted per draw, and the worst. */
	uint32_t lag_sum;
	uint32_t lag_n;
	uint32_t lag_max;
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
 * @brief  The worker's half of one frame: invoke, plugin lease, then the shared
 *         frame path's geometry, decode, publish and account (issues #129,
 *         #130).
 *
 * Called by the inference worker only, after nn_overlay_take() -- so the job
 * the producer wrote is its alone, and the input and output tensors are not the
 * producer's until it returns.  It ends the job itself: a stream asks for the
 * next frame (DONE), a one-shot for nothing more (DONE_LAST).
 */
void nn_overlay_work(void);

/*
 * The hand-over word (svc/nn_handoff.h), which lives with the overlay's
 * frame-path state since issue #130 (svc/nn_core_frame.h).  For nn_worker.c.
 */
/** IDLE -> WANT.  @return non-zero if armed. */
int nn_overlay_want(void);
/** HANDED -> RUNNING.  @return non-zero if a job was taken (else a stale
 *  wake-up). */
int nn_overlay_take(void);
/** IDLE / WANT / FILLING -> IDLE, once the producer is confirmed out.
 *  @return non-zero if joined. */
int nn_overlay_join(void);

/** How a one-shot's frame ended (nn_overlay_shot()).  NONE until it has. */
#define NN_OV_SHOT_NONE          0u
#define NN_OV_SHOT_PUBLISHED     1u   /**< a result (or the raw outputs) went
                                           to the record                    */
#define NN_OV_SHOT_PREP_FAILED   2u   /**< the producer could not prepare it */
#define NN_OV_SHOT_STOPPED       3u   /**< a stop came first; no invoke      */
#define NN_OV_SHOT_NO_OUTPUTS    4u   /**< an output tensor was unreadable
                                           (nn_overlay_shot_index())      */
#define NN_OV_SHOT_INVOKE_FAILED 5u
#define NN_OV_SHOT_LEASE_TIMEOUT 6u   /**< inferred, not decoded             */
#define NN_OV_SHOT_NOT_HELD      7u   /**< the decode refused an unleased call */

/**
 * @brief  Arm the overlay for `nn run`: ONE frame, through the same producer
 *         prep and worker as a stream (issue #129).
 *
 * The producer prepares the first frame the worker wants and declines every
 * other; the worker parks for good after it (DONE_LAST).  Neither touches the
 * stream's statistics.  The caller arms the worker first.
 *
 * @return the vtable to pass to cam_capture_sink_attach_and_stream()
 */
const struct cam_lcd_overlay *nn_overlay_arm_oneshot(void);

/** How the one-shot's frame ended, NN_OV_SHOT_*.  Read it BEFORE the record's
 *  count: the worker publishes and only then sets this. */
int nn_overlay_shot(void);

/** With NN_OV_SHOT_NO_OUTPUTS: the index of the output that could not be read
 *  (review of a438f76 -- `nn run` used to name it).  Read after
 *  nn_overlay_shot(). */
unsigned nn_overlay_shot_index(void);

/**
 * @brief  Ask the overlay to stop doing work.
 *
 * [!] CALL THIS BEFORE camera_stream_stop(), always.
 *
 * It is checked at two points -- on the producer before preprocessing, and on
 * the worker immediately before the invoke -- so a frame that has not yet
 * begun the expensive part abandons it. That is what keeps an ordinary Ctrl+C
 * from waiting on an inference.
 *
 * It CANNOT cancel an invoke already waiting on the NPU: nothing can. It
 * narrows the window; the camera's lost-producer state and the stop's bounded
 * join of the worker (terminal when it runs out, issue #129) are what make the
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
