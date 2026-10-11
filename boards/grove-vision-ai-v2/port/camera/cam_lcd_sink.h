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
 * using the frame", which is why the unlink in cam_lcd_sink.c is the
 * interesting function.
 *
 * [!] SINCE ISSUE #142 THIS SINK IS STARTED BY ITS HOLDERS, NOT BY A COMMAND.
 * The camera streams to the panel while, and only while, somebody holds it:
 * the operator (`camera stream start`) or nn (`nn stream start`, `nn run`), in
 * the svc/cam_holders.h table kept here -- camera.c knows nothing of the panel
 * and stays that way.  The first holder links this sink and starts the camera
 * (one step, issue #63); the last one to leave stops it, and only a CONFIRMED
 * stop unlinks it (port/camera/cam_hold_rules.h).  The operator may not leave
 * while nn holds it ("still in use: nn", issue #142 D8).  nn's work rides on
 * the stream as the overlay below, put on and taken off without touching the
 * camera.
 */
#ifndef CAM_LCD_SINK_H
#define CAM_LCD_SINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  The optional per-frame overlay (issue #48): nn's step of this sink.
 *
 * Two callbacks because the work splits at the panel guard, and that split is
 * the point:
 *
 *   - process() runs FIRST, ON THE PRODUCER THREAD, with NO guard held.  It
 *     stays on the producer deliberately: the raw WDMA3 buffer it reads is
 *     stable only until consume() returns (the datapath is re-armed after
 *     publish), so this is the one place the model sees the frame that is about
 *     to be shown.  Since issue #129 it only prepares the input; the inference
 *     is the worker's.
 *   - draw() runs INSIDE the guard, on the staged frame, immediately before the
 *     DMA -- and therefore ON THE PANEL THREAD since issue #57.  It is bound by
 *     the contract in lcd_st7789.h: no blocking, no re-entry, only
 *     lcd_rect_wire().  May be NULL: `nn run`'s overlay prepares a frame and
 *     draws nothing (issue #142 -- a one-shot does not paint the last result).
 *
 * The two run on DIFFERENT THREADS but never overlap, and that is not a
 * coincidence to be preserved by luck: the pipeline pre-pins one delivery per
 * sink and refuses a second while the first is outstanding (FRAME_POLICY_DROP),
 * so process() cannot run again until the panel thread has released the frame --
 * which it does only after draw() has returned.  Whatever process() leaves for
 * draw() is therefore published by that hand-off and needs no lock.
 *
 * [!] HELD BY POINTER AND COUNTED (issue #142).  The sink keeps a pointer, not a
 * copy, and the producer reads it ONCE per frame -- in the same critical section
 * as it counts itself in (svc/cam_step.h) -- and hands what it read to the panel
 * thread with the frame.  The panel never reads the pointer the sink keeps.  The
 * count stays up from that read until the panel thread has finished with the
 * frame -- drawn, or skipped because the panel was taken or dark -- so it
 * reaching zero after cam_lcd_sink_overlay_remove() says neither thread is in
 * either callback and neither can enter again.  The vtable must outlive that.
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
 * The panel thread's stack, in DTCM.  See cam_lcd_sink.c for how the number was
 * chosen; the array and the TX_THREAD stay there.
 *
 * Published since issue #119 because a plugin's draw() runs on this thread, and
 * the plugin policy (port/nn/nn_plugin_stack.h) asserts draw's allowance
 * strictly below it -- an allowance equal to the whole stack is a check that
 * cannot fire (issue #103).
 */
#define CAM_PANEL_STACK_BYTES 2048u

/**
 * The panel thread's ThreadX priority.  Why it sits where it does, and the
 * asserts that keep it there, are in cam_lcd_sink.c.
 *
 * Published since issue #119 for the same reason as the stack above: the plugin
 * stack probe (port/nn/nn_probe_rtos.c) recognises this thread by the priority
 * it was created with and the size of its stack, and cannot recognise it by a
 * number it cannot see.
 */
#define CAM_PANEL_PRIO 9u

/**
 * @brief  Create the panel thread and its ThreadX objects (issue #57).
 *
 * Call from tx_application_define(), next to lcd_create_objects() and
 * camera_create_objects().  Touches no hardware; the thread parks immediately
 * and does nothing until a frame is handed to it.  A hold refuses if this did
 * not succeed -- a sink whose worker does not exist would accept a frame, pin
 * its slot for ever and stall the ring.
 */
void cam_lcd_sink_create_objects(void);

/*
 * ---- who holds the stream (issue #142) -------------------------------------
 *
 * Every function below runs the holder table under this file's holder lock, in
 * the same locked region as the camera start or stop its answer calls for.
 * Before answering anything else each one SETTLES what is owed: a stop that
 * confirmed the camera but still owes the unlink is finished, and a stream that
 * ended by itself while held (the producer gave up) is confirmed with a stop,
 * unlinked, and every holder marked lost -- a holder's mark goes only when that
 * holder takes hold again.
 *
 * [!] AN UNCONFIRMED LOSS IS TERMINAL.  A camera that never acknowledged a stop
 * (CAM_ST_LOST) or a panel thread that did not come back leaves the table, the
 * sink and every resource exactly where they are; every later call answers
 * CAM_ERR_STATE and only a reboot clears it.
 */
struct cam_holder;   /* svc/cam_holders.h */

/** Refused: another holder still needs the stream (issue #142 D8).  The
 *  operator stays and nothing changed.  This file's, not camera.h's: only the
 *  holder table can say it. */
#define CAM_LCD_ERR_IN_USE (-20)

/**
 * @brief  Take hold of the camera's stream to the panel.
 *
 * @param h        the holder; NULL for the operator (`camera stream start`)
 * @param ov       nn's overlay to put on the sink with this hold, or NULL
 * @param started  out (may be NULL): non-zero if this call started the camera
 *
 * A holder other than the operator joins a running stream, or -- when nothing
 * streams -- takes the operator's place too and starts it (issue #142 D6: `nn
 * run` as well), so the camera keeps streaming after nn lets go.  Starting brings
 * the panel up, rotates it to landscape and starts the camera WITH this sink, in
 * the camera's own one-step transaction (issue #63).  The overlay is put on in
 * the same locked region; a refusal of it gives back exactly what this call
 * registered (svc/cam_holders.h's ticket) and stops nothing it did not start.
 *
 * [!] NEVER WAITS FOR THE HOLDER LOCK: a camera start or stop in progress makes
 * this CAM_ERR_BUSY, retryable (only a stop waits).
 *
 * @return CAM_OK; CAM_ERR_BUSY (retry: a start or stop in progress, the camera
 *         owned by `camera bench` or another command, or the overlay still
 *         counted in use); CAM_ERR_STATE (terminal, or the table full);
 *         CAM_ERR_HAL / other CAM_ERR_* if the panel or the camera would not
 *         come up -- in every failure nothing this call added is left behind.
 */
int cam_lcd_sink_hold(const struct cam_holder *h,
                      const struct cam_lcd_overlay *ov, int *started);

/**
 * @brief  Let go of the camera's stream.
 *
 * @param h       the holder; NULL for the operator (`camera stream stop`)
 * @param users   out (may be NULL): who refused the operator's leave
 * @param max     room in @p users
 * @param nusers  out (may be NULL): how many names went to @p users
 *
 * Not a holder: CAM_OK, nothing changes.  [!] The operator while nn holds it:
 * CAM_LCD_ERR_IN_USE, and the operator STAYS (D8).  The last holder: the camera
 * is stopped, and only on a CONFIRMED stop is the sink unlinked and the table
 * emptied.  Waits for the holder lock, bounded (a stop in progress may hold it
 * for a camera join and a panel drain).
 *
 * @return CAM_OK; CAM_LCD_ERR_IN_USE; CAM_ERR_BUSY (retry: the lock was not had
 *         in time, or the unlink found a transition in flight -- the camera is
 *         stopped and the next release finishes it); CAM_ERR_LOCKED (retry: the
 *         camera API stayed locked, nothing was asked); CAM_ERR_TIMEOUT /
 *         CAM_ERR_STATE (terminal: see above)
 */
int cam_lcd_sink_release(const struct cam_holder *h, const char **users,
                         int max, int *nusers);

/**
 * @brief  Take the overlay off, and wait until nothing uses it (issue #142).
 *
 * The producer stops entering nn's step at once (the pointer goes and the step
 * is disarmed in one critical section); then this waits, by the wall clock, for
 * the count to reach zero -- the producer out of process() and the panel thread
 * out of draw() and finished with the frame that carried it.  Zero is the proof
 * nn's stop runs on (the worker JOIN, then the record boundary and nn's leave).
 * The camera keeps streaming.
 *
 * Idempotent: nothing on (taken off by an earlier call, or by the unlink at the
 * end of a stream) waits only for the count.
 *
 * @return CAM_OK once nothing is inside; CAM_ERR_TIMEOUT when the count had not
 *         reached zero at the deadline (retryable: the overlay stays off, and a
 *         later call waits again); CAM_ERR_STATE when the holder code is
 *         terminal or the sink is lost (nothing is proven, everything is kept)
 */
int cam_lcd_sink_overlay_remove(void);

/**
 * The holders' names in registration order (static strings, up to @p max) and,
 * in *@p lost (may be NULL), how many were marked lost at the last confirmed end
 * of a stream.  Does not wait for the holder lock.
 *
 * @return the count; -1 when a start or stop held the lock (nothing copied)
 */
int cam_lcd_sink_holders(const char **names, int max, int *lost);

/**
 * @brief  Is the panel spoken for by this sink? (issue #99)
 *
 * For the `lcd` commands that change PERSISTENT panel state -- rotation, MADCTL
 * -- rather than one transfer.  A rotation mid-stream leaves the sink's
 * fixed-size blits refused or clamped while every layer still reports success.
 *
 * [!] ASK IT WHILE HOLDING THE PANEL GUARD.  Answered and then acted on
 * afterwards it is a TOCTOU: the start path takes the guard from bring-up until
 * the sink is linked precisely so that a caller holding it either finishes first
 * or sees the sink.
 *
 * @return non-zero if this sink is attached, coming up, going down, or lost
 */
int cam_lcd_sink_linked(void);

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
	uint32_t dark;           /**< frames skipped because the panel was dark
	                          *   (`lcd off`, issue #142)                  */
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
