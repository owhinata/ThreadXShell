/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_step.h
 * @brief   One step of the camera sink: may the producer go in, and has it come
 *          out (issue #142).
 *
 * WHAT A STEP IS.  Since issue #142 this board has ONE camera sink for the GUIX
 * preview and the nn ingest (cam_sink.h).  Its consume() runs each of them in
 * turn -- the display step, then the nn step -- and each can be switched off
 * while the base capture keeps running and the other step keeps working.  That
 * used to be two sinks, and switching one off was an unsubscribe followed by a
 * drain of that sink's pin.  With one sink there is no pin per step any more, so
 * each step counts for itself:
 *
 *   - ARMED: the producer may enter the step.  Read by the producer, together
 *     with the increment below, in ONE critical section.
 *   - BUSY: how many times the producer is inside the step right now (0 or 1 on
 *     this board -- there is one producer -- but a count, so that a second one
 *     would be counted rather than lost).
 *
 * [!] ZERO AFTER DISARM IS THE PROOF.  An owner that disarms the step and then
 * sees BUSY reach 0 knows the producer is not in the step AND cannot enter it
 * again: the producer only increments while ARMED, and it reads ARMED and
 * increments in the same critical section as this file's decision.  That is the
 * statement the old per-sink pin gave ("once the pin reaches zero, that callback
 * makes no further access to owner-owned state"), narrowed to one step.  nn uses
 * it for its JOIN: the producer is out of the input tensor.
 *
 * [!] ARMING REQUIRES ZERO.  A step is armed only while nobody is inside it, so
 * whatever the owner initialises just before arming (nn's session: the epoch,
 * the record boundary, the counters, the wake-up token) cannot be read half-way
 * by a producer that was still inside from before.  A refused arm moves nothing.
 *
 * WHY A PURE TABLE.  As with cam_drain.h and cam_own.h: the interesting cases --
 * a disarm while the producer is inside, a leave without an enter -- cannot be
 * typed at a console.  test/test_cam_step.c walks them.  The critical section
 * and the waiting are cam_sink.c's.
 */
#ifndef CAM_STEP_H
#define CAM_STEP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A step's producer-facing state.  Zero-initialised is disarmed and idle.
 *  Moved only through the functions below, inside the caller's critical
 *  section. */
struct cam_step_state {
	uint8_t  armed;   /**< the producer may enter                          */
	uint16_t busy;    /**< producer entries not yet left                   */
};

#define CAM_STEP_OK         0
#define CAM_STEP_E_ARMED  (-1)  /**< arm: already armed                     */
#define CAM_STEP_E_BUSY   (-2)  /**< arm: the producer is still inside      */
#define CAM_STEP_E_IDLE   (-3)  /**< leave: nobody entered                  */
#define CAM_STEP_E_FULL   (-4)  /**< enter: the count would wrap            */

/**
 * The producer, before the step.  Non-zero: it is in, and owes exactly one
 * cam_step_leave().  Zero: it must skip the step (disarmed, or a count that
 * would wrap -- refused rather than wrapped into a false zero).
 */
int cam_step_enter(struct cam_step_state *s);

/** The producer, after the step.  CAM_STEP_E_IDLE (and no change) for a leave
 *  with nobody inside -- a broken caller, never a reason to go negative. */
int cam_step_leave(struct cam_step_state *s);

/** May the step be armed now?  (The owner asks before it initialises.) */
int cam_step_may_arm(const struct cam_step_state *s);

/** Arm.  Refused, with nothing moved, unless cam_step_may_arm() holds. */
int cam_step_arm(struct cam_step_state *s);

/** Disarm.  Idempotent; BUSY is left alone -- it falls as the producer leaves. */
void cam_step_disarm(struct cam_step_state *s);

#ifdef __cplusplus
}
#endif

#endif /* CAM_STEP_H */
