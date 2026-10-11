/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_sink.h
 * @brief   The camera sink: one RGB565 subscriber of the base capture that runs
 *          the GUIX preview and the nn ingest as two steps (issue #142).
 *
 * WHAT CHANGED.  The GUIX preview and the nn ingest used to be two subscribers,
 * each subscribed by its own start and unsubscribed (then pin-drained) by its own
 * stop.  They are now two STEPS of one sink.  The sink is subscribed once, the
 * first time any step is enabled, and stays subscribed: the base attaches it
 * when it streams RGB565 and detaches it when it stops.  `gui stop` and
 * `nn stream stop` no longer unsubscribe anything -- they switch their own step
 * off and wait for the producer to leave it (cam_step.h).  The other subscriber
 * on this board, the MJPEG server, stays a subscriber of its own: it needs a JPEG
 * base, which this sink never sees.
 *
 * THE ORDER IN consume() IS FIXED: the display step, then the nn step, then
 * camera_frame_put() LAST.  The order of the steps is the board's (src/main.c
 * registers them); the put-last rule is this file's.
 *
 * WHO CALLS WHAT.
 *   - The base (camera.c, under its camera lock) calls the sink's open() on
 *     attach and close() on detach.  Each forwards to every ENABLED step: open()
 *     initialises it for the new geometry and arms it, close() disarms it and
 *     tells it the base went away (a pause, not a stop -- as before).
 *   - A step's owner calls camera_sink_step_enable() / _disable() /
 *     _drain().  Enabling a step while the base is attached runs its open() at
 *     once, as subscribing an attached base used to; disabling it runs its
 *     close(), as the unsubscribe's detach used to.
 *   - The producer runs consume().
 *
 * [!] THE STEP'S OWNER STILL HOLDS ITS TRANSITION CLAIM (cam_own.h) across
 * disable -> drain -> its own clean-up -> commit, exactly as it held it across
 * unsubscribe -> pin drain -> commit.  Nothing here replaces that machine: this
 * file only replaces the thing it watched (a sink's pins) with the step's count.
 *
 * LOCKS.  open()/close() run under the camera lock and take this file's mutex;
 * enable / disable take only this file's mutex (and call camera_subscribe()
 * before taking it, never under it).  So the order is camera lock -> this mutex
 * -> whatever a step's open() / close() takes (nncam_lock, the LTDC frame lock).
 * The producer takes neither: it reads a step's state in a PRIMASK section.
 */
#ifndef CAM_SINK_H
#define CAM_SINK_H

#include <stdint.h>

#include "cam_drain.h"      /* enum cam_drain_step */
#include "cam_step.h"       /* struct cam_step_state */
#include "frame.h"          /* enum frame_format / struct frame_desc */

#ifdef __cplusplus
extern "C" {
#endif

/** A step's callbacks.  The same shapes the two subscribers had, minus the put:
 *  the sink puts, after the last step. */
struct cam_step_ops {
	/** The base is attached and the step enabled: initialise for this geometry.
	 *  <0 leaves the step enabled but not armed for this attach.  [!] Unreachable
	 *  today (only QQVGA / QVGA RGB565 stream, and both steps accept them); if it
	 *  ever happens on attach, `nn stream` sits silently with no frames. */
	int  (*open)(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h);
	/** One frame, pin held by the sink.  Must NOT put. */
	int  (*consume)(void *ctx, const struct frame_desc *f);
	/** The step was armed and is not any more: the base detached, or the owner
	 *  disabled it.  Non-blocking, no camera API re-entry (runs under the
	 *  camera lock on a detach). */
	void (*close)(void *ctx);
};

/** One step.  The owner defines it statically with name / ops / ctx; the rest
 *  is this file's. */
struct cam_sink_step {
	const char                *name;
	const struct cam_step_ops *ops;
	void                      *ctx;
	/* cam_sink.c only */
	struct cam_step_state      st;    /**< armed / busy: PRIMASK               */
	uint8_t                    want;  /**< enabled by the owner: the mutex     */
};

/** The steps the sink runs, in order.  Two on this board. */
#define CAM_SINK_MAX_STEPS 2u

/**
 * Register the steps, in the order consume() runs them, and create the mutex.
 * Once, at boot, before anything enables a step (safe before the scheduler).
 * Returns 0, CAM_ERR_PARAM or CAM_ERR_STATE.
 */
int camera_sink_init(struct cam_sink_step *const *steps, unsigned n);

/**
 * Enable @p s: subscribe the sink if it is not yet, and arm the step now if the
 * base is attached (running its open()).  Returns 0, or CAM_ERR_BUSY (already
 * enabled, the producer still inside from before, or its open() refused this
 * geometry), CAM_ERR_STATE (not initialised), or what camera_subscribe()
 * returned.
 */
int camera_sink_step_enable(struct cam_sink_step *s);

/** Disable @p s: disarm it (running its close() if it was armed).  Idempotent.
 *  The producer may still be inside -- drain before releasing anything. */
void camera_sink_step_disable(struct cam_sink_step *s);

/** Wait (wall clock, at most @p ticks) for the producer to leave a DISABLED
 *  step.  CAM_DRAIN_DONE is the proof in cam_step.h; CAM_DRAIN_PINNED means it
 *  is still inside, and a later call may finish. */
enum cam_drain_step camera_sink_step_drain(struct cam_sink_step *s,
                                           uint32_t ticks);

/** The producer's entries into @p s not yet left (for a log line). */
int camera_sink_step_busy(struct cam_sink_step *s);

/** Is anything other than @p self receiving frames: another subscriber of the
 *  base, or another step of this sink?  Best effort, for the GUI's resolution
 *  button (a base restart would interrupt it). */
int camera_sink_others_live(const struct cam_sink_step *self);

#ifdef __cplusplus
}
#endif

#endif /* CAM_SINK_H */
