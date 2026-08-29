/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_lcd_sink.h
 * @brief   Frame sink that puts camera frames on the ST7789 panel (issue #35),
 *          asynchronously since issue #57.
 *
 * The glue between two drivers, so it lives with neither: camera.c stays free
 * of the panel and lcd_st7789.c stays free of the camera.
 *
 * [!] THE BLIT DOES NOT RUN ON THE PRODUCER ANY MORE (issue #57).  consume()
 * hands the pre-pinned descriptor to this module's own panel thread and returns;
 * the blit -- a 153,600-byte byte-swap copy plus a ~26 ms SPI DMA wait -- happens
 * there, while the producer is already capturing the next frame.  That is the
 * whole point: those 26 ms were the largest single term in the frame period
 * (epic #56).  svc/frame_pipeline specifies this exact shape -- publish pre-pins
 * the slot, the sink may put from its own thread -- so the shared core is
 * unchanged.
 *
 * What it costs is that "the producer has stopped" no longer means "nothing is
 * using the frame", which is why detach() below is the interesting function.
 */
#ifndef CAM_LCD_SINK_H
#define CAM_LCD_SINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Optional per-frame overlay (issue #48).
 *
 * Two callbacks because the work splits at the panel guard, and that split is
 * the point:
 *
 *   - process() runs FIRST, ON THE PRODUCER THREAD, with NO guard held, and may
 *     take as long as it needs -- `nn stream` runs a whole NPU inference here.
 *     It stays on the producer deliberately: the raw WDMA3 buffer it reads is
 *     stable only until consume() returns (the datapath is re-armed after
 *     publish), so this is the one place the model sees the frame that is about
 *     to be shown.
 *   - draw() runs INSIDE the guard, on the staged frame, immediately before the
 *     DMA -- and therefore ON THE PANEL THREAD since issue #57.  It is bound by
 *     the contract in lcd_st7789.h: no blocking, no re-entry, only
 *     lcd_rect_wire().
 *
 * The two run on DIFFERENT THREADS but never overlap, and that is not a
 * coincidence to be preserved by luck: the pipeline pre-pins one delivery per
 * sink and refuses a second while the first is outstanding (FRAME_POLICY_DROP),
 * so process() cannot run again until the panel thread has released the frame --
 * which it does only after draw() has returned.  Whatever process() leaves for
 * draw() is therefore published by that hand-off and needs no lock.
 *
 * A non-zero return from process() means "no overlay for this frame": draw() is
 * skipped, the frame is still shown, and the preview keeps running.  A failed
 * inference must not blank the picture.
 */
struct cam_lcd_overlay {
	void *ctx;
	int  (*process)(void *ctx, const void *pixels, uint16_t w, uint16_t h);
	void (*draw)(void *ctx, uint16_t *fb, uint16_t fb_w, uint16_t fb_h);
};

/**
 * @brief  Create the panel thread and its ThreadX objects (issue #57).
 *
 * Call from tx_application_define(), next to lcd_create_objects() and
 * camera_create_objects().  Touches no hardware; the thread parks immediately
 * and does nothing until a frame is handed to it.  attach() refuses if this did
 * not succeed -- a sink whose worker does not exist would accept a frame, pin
 * its slot for ever and stall the ring.
 */
void cam_lcd_sink_create_objects(void);

/**
 * @brief  Start the camera with this sink attached, and show its frames.
 *
 * Brings the panel up if needed, rotates it to landscape (the camera delivers
 * 320x240 and the panel is natively 240x320 -- MADCTL transposes for free on
 * this SPI panel, so no CPU-side rotation is involved), and then starts the
 * stream WITH this sink, in the camera's own one-step transaction.
 *
 * [!] IT STARTS THE STREAM (issue #63), which is why it is not just "attach"
 * any more.  Subscribing and starting used to be two calls the caller made in
 * order, and `camera bench` -- which starts a stream owning no sink -- could get
 * in between them, stranding this sink on a stream nobody here owns.  The two
 * are indivisible now, so the caller has one call and one answer.
 *
 * The panel work stays OUTSIDE that transaction: bring-up is a reset pulse, an
 * init table and a priming transfer, and none of it needs the camera held.  Only
 * the pipeline registration is inside.
 *
 * ON FAILURE NOTHING IS ATTACHED and no stream is running -- including for
 * CAM_ERR_BUSY, which now covers both "another preview owns this sink" and
 * "another command owns the camera".  There is nothing for the caller to
 * unwind but its own state.
 *
 * @p ov may be NULL for a plain preview.  It is an ARGUMENT rather than a
 * separate setter so that an overlay cannot outlive the command that wanted it:
 * there is no state a crashed or forgotten teardown could leave behind for the
 * next `camera preview` to inherit.  The pointer must stay valid until
 * detach() returns CAM_OK.
 *
 * @return CAM_OK, with the stream running; CAM_ERR_BUSY if a preview already
 *         owns the sink or another command owns the camera; CAM_ERR_STATE if a
 *         previous detach could not be completed (see below); CAM_ERR_HAL if the
 *         panel or the camera would not come up.
 */
int cam_lcd_sink_attach_and_stream(const struct cam_lcd_overlay *ov);

/**
 * @brief  Is the panel spoken for by this sink? (issue #99)
 *
 * For the `lcd` commands that change PERSISTENT panel state -- rotation, MADCTL,
 * the backlight -- rather than one transfer.  Since live inference stopped
 * blocking its console those became typeable during a stream, and a rotation
 * mid-stream leaves the sink's fixed-size blits refused or clamped while every
 * layer still reports success.
 *
 * [!] ASK IT WHILE HOLDING THE PANEL GUARD.  Answered and then acted on
 * afterwards it is a TOCTOU: the attach path takes the guard from bring-up until
 * the sink is linked precisely so that a caller holding it either finishes first
 * or sees the sink.
 *
 * @return non-zero if this sink is attached, coming up, or lost
 */
int cam_lcd_sink_linked(void);

/**
 * @brief  Stop showing frames: unlink the sink, then drain the panel thread.
 *
 * [!] THE CALLER MUST HAVE A CONFIRMED STOP FIRST -- camera_stream_stop() ==
 * CAM_OK, never a timeout (issue #48).  That proves the producer is idle, which
 * is what makes the count of outstanding deliveries here stop growing.
 *
 * THE ORDER IS THE DESIGN (issue #57), and it is svc/frame_pipeline's own:
 * unlink so that no further consume() can be issued, and only then wait for the
 * work already handed over.  Draining first would leave the sink linked across a
 * window in which another command can start a fresh stream -- camera_stream_stop()
 * releases the camera API mutex before it returns, and `camera bench` starts a
 * stream while owning no sink -- and the sink would then be delivered frames
 * belonging to a stream its owner had already finished with.
 *
 * A drain that does not finish inside its deadline is this module's CAM_ST_LOST:
 * nothing is torn down, the overlay is NOT cleared, the frame stays pinned, and
 * every later attach() is refused until reboot.  The caller must treat that
 * exactly as it treats a camera that never acknowledged a stop -- in particular
 * `nn stream` must keep its lease, because the panel thread may still be inside
 * a draw() that reads the detections.  All of this module's state is static, so
 * keeping it costs nothing; releasing it while a thread may still be in there
 * cannot be survived.
 *
 * @return CAM_OK once the panel thread is provably idle (and only then may the
 *         caller release anything the sink points at); CAM_ERR_TIMEOUT if the
 *         drain failed; CAM_ERR_STATE if the sink was already poisoned or the
 *         camera refused the unsubscribe.
 */
int cam_lcd_sink_detach(void);

/**
 * Delivery counters for `camera stats`, plus the blit timing (issue #57).
 *
 * The timing is here rather than in the producer's profile because it is
 * measured on the panel thread, and it exists so that the acceptance test for
 * #57 can be "the 26 ms MOVED" rather than "the producer's number went down" --
 * the latter is also what a silently broken sink looks like.  Same time source
 * as the producer's profile (TIMER2, which keeps counting through WFI) and the
 * same honesty rule: when tx_glue_profile_ok() says the source is not
 * trustworthy, no number is printed.
 */
struct cam_lcd_sink_stats {
	uint32_t delivered;      /**< frames handed to the panel thread          */
	uint32_t dropped;        /**< pipeline dropped: sink still busy          */
	uint32_t errors;         /**< blit failures                              */
	uint32_t busy;           /**< frames skipped because the panel was taken */
	uint32_t overlay_errors; /**< process() refused: frame shown, no boxes   */

	/**
	 * Deliveries accepted, and slots handed back (issue #71).
	 *
	 * [!] THEY ARE NOT REQUIRED TO MATCH WHILE A STREAM RUNS -- one frame is
	 * normally in flight, so `accepted` leads by one.  They are an invariant
	 * only after a successful drain, which is where detach checks them and
	 * poisons the sink if they disagree.  Reported here so the pair can be
	 * watched without ending the preview to find out.
	 */
	uint32_t    accepted;
	uint32_t    puts;

	int         prof_ok;     /**< 0 = the time source is not trustworthy     */
	const char *prof_why;    /**< why not, when prof_ok is 0 (else NULL)     */
	uint32_t    blit_frames; /**< blits counted into blit_us                 */
	uint32_t    blit_us;     /**< TOTAL microseconds, not per frame          */

	/**
	 * How long the slot is HELD: hand-off to pin returned (issue #71).
	 *
	 * Not a subset of blit_us -- a different span with a different end.
	 * blit_us is the panel thread's whole interval, transfer included; this
	 * one stops when the pin goes back at the staging seam, which is what
	 * the frame period is now bounded by.  Counted only on frames the
	 * driver's staged hook ended, so the fallback paths (panel taken, driver
	 * refused early) cannot inflate it.  TOTAL microseconds, not per frame.
	 */
	uint32_t    hold_frames;
	uint32_t    hold_us;

	/** Why the sink is finished, or NULL.  Deliberately not the same field
	 *  as prof_why: one says the clock cannot be trusted, this one says the
	 *  sink cannot be used again until reboot. */
	const char *fault;
};
void cam_lcd_sink_stats(struct cam_lcd_sink_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* CAM_LCD_SINK_H */
