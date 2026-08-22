/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_own.h
 * @brief   The lifecycle a subscriber's OWNER runs through (issue #72).
 *
 * WHAT THIS IS FOR.  cam_drain.h answers "may I release what my sink reads yet";
 * this answers the question that comes before it -- "may I start / re-open /
 * reuse this sink at all right now".  The three owners on this board (the GUIX
 * preview, the NN stream, the MJPEG server) each detach a sink while the base
 * capture keeps running, so each has an interval in which the sink is nobody's:
 * detached, still pinned, and about to be handed back.  A start that walked into
 * that interval would re-subscribe the sink, and frame_pipeline_attach() resets
 * `_pins`, `_busy` and `_pending` -- erasing the very evidence the drain is
 * waiting on.
 *
 * [!] SO THIS IS A STATE ENTERED ON THE WAY IN, NOT A FLAG SET ON FAILURE.  The
 * first attempt at issue #72 set a flag only when a drain expired, which left
 * the whole drain interval open; DRAINING is entered BEFORE
 * camera_unsubscribe(), so there is no such window.
 *
 *     IDLE -> STARTING -> RUNNING -> DRAINING -> IDLE      (drained, worker parked)
 *              |                        |------> SETTLING  (drained, worker busy)
 *              \-> IDLE (start failed)  \------> PENDING   (budget spent, pinned)
 *
 *     SETTLING -> IDLE          (the owner's worker parks and says so)
 *     SETTLING -> DRAINING      (a later stop re-polls and finishes the teardown)
 *     PENDING  -> DRAINING      (likewise; only a poll that sees zero pins frees it)
 *
 * STARTING exists so that a stop racing a start is REFUSED rather than
 * interleaved with it.  Without it a stop could run a whole teardown between a
 * start's claim and its attach, and both would report success.
 *
 * SETTLING is "the sink is free but my worker thread has not parked".  It is a
 * state and not just the owners' existing flags because the session hand-off
 * hangs off it: NN's stop may return -2 and leave the worker to release the nn
 * session itself, and a start allowed in at that moment could acquire a NEW
 * session that the old stop then releases.
 *
 * [!] THE SERIALISER IS NOT HELD ACROSS THE WORK.  Take it, make the transition,
 * release it, do the draining and the teardown, take it again to commit.  The
 * transitions below are the whole of what runs under it -- a table lookup and a
 * store.  Holding it across a drain would hold it across tx_thread_sleep(), and
 * holding it across a callback's own lock (nncam_lock, ltdc_lock) would invert a
 * lock order against threads that take those while the producer runs.  Either
 * turns a teardown into a deadlock, which is worse than the bug being fixed.
 *
 * WHY A PURE TABLE.  The transitions that matter cannot be produced from a
 * console: they need a drain that times out, or two owner commands genuinely in
 * flight at once.  As a decision over the state it is a table, and
 * test/test_cam_own.c walks it (same reasoning as cam_drain.h and Grove's
 * cam_state.h).  What the table CANNOT cover is the serialisation itself and the
 * ordering in the callers -- those are held down by review and by the hardware
 * pass; see the plan's verification note and the comments at each call site.
 */
#ifndef CAM_OWN_H
#define CAM_OWN_H

#include "cam_drain.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where a sink's owner is in its lifecycle. */
enum cam_own_state {
	CAM_OWN_IDLE = 0,  /**< nothing of ours attached; a start may proceed   */
	CAM_OWN_STARTING,  /**< a start is in flight (claimed, not yet live)    */
	CAM_OWN_RUNNING,   /**< subscribed (attached or enabled-idle)           */
	CAM_OWN_DRAINING,  /**< a teardown owns the sink right now              */
	CAM_OWN_SETTLING,  /**< sink released; our worker has not parked yet    */
	CAM_OWN_PENDING,   /**< [!] the sink still holds a pin -- see below     */
};

/*
 * [!] CAM_OWN_PENDING: the drain spent its budget with a pin outstanding.
 *
 * Retryable rather than terminal, and it is worth saying why, because the
 * obvious argument for retrying is wrong.  "The outstanding callback is not
 * blocked on anything" is FALSE here: the GUIX sink takes ltdc_lock, the NN sink
 * takes nncam_lock, and the final put takes the pipeline mutex -- all
 * TX_WAIT_FOREVER.  Retry is right anyway because it is fail-closed in both
 * directions.  If the callback never comes back, every later poll sees the pin
 * and refuses reuse forever, which is operationally what a terminal state would
 * give.  If it does come back, a poll can prove it and the owner recovers.
 *
 * Grove's CAM_ST_LOST stays terminal for the opposite reason: there the thing
 * that did not come back is a THREAD, and nothing later can learn whether it
 * ever will.
 */

/** What a start attempt may do. */
enum cam_own_start {
	CAM_OWN_START_GO = 0,  /**< claim it: the caller now owns the start     */
	CAM_OWN_START_RUNNING, /**< already up (the owner's own "already" error) */
	CAM_OWN_START_HELD,    /**< a start or a teardown owns the sink         */
};

/** What a stop attempt must do. */
enum cam_own_stop {
	CAM_OWN_STOP_DRAIN = 0, /**< detach, drain, settle, commit              */
	CAM_OWN_STOP_RETRY,     /**< already detached: re-poll, settle, commit  */
	CAM_OWN_STOP_HELD,      /**< someone else owns this transition          */
	CAM_OWN_STOP_IDLE,      /**< nothing of ours is up                      */
};

/** @brief  May a start proceed?  (Pure.) */
enum cam_own_start cam_own_start_decide(enum cam_own_state st);

/** @brief  The state to store after @p act was decided from @p st.  (Pure.)
 *
 *  A refused attempt returns @p st unchanged -- refusing must never move the
 *  state, or two refused starts in a row would walk the machine somewhere new. */
enum cam_own_state cam_own_start_next(enum cam_own_start act,
                                      enum cam_own_state st);

/** @brief  What must this stop do?  (Pure.) */
enum cam_own_stop cam_own_stop_decide(enum cam_own_state st);

/** @brief  The state to store after @p act was decided from @p st.  (Pure.) */
enum cam_own_state cam_own_stop_next(enum cam_own_stop act,
                                     enum cam_own_state st);

/** @brief  The state to store when a claimed start finishes.  (Pure.)
 *
 *  @param ok  non-zero if the owner is now subscribed. */
enum cam_own_state cam_own_start_done(enum cam_own_state st, int ok);

/** @brief  The state to store when a teardown finishes.  (Pure.)
 *
 *  @param step           the drain's final answer (cam_sink_drain_step())
 *  @param worker_parked  non-zero if the owner's worker thread is parked; pass 1
 *                        for an owner that has no worker (the GUIX preview).
 *
 *  [!] A pin outstanding outranks the worker: the sink is what a later start
 *  would clobber, so PENDING is the state that has to be remembered. */
enum cam_own_state cam_own_drain_next(enum cam_drain_step step,
                                      int worker_parked);

/** @brief  Is @p st a start claim waiting to be finished?  (Pure.) */
int cam_own_start_is_claimed(enum cam_own_state st);

/** @brief  The state to store when the owner's worker parks by itself.  (Pure.)
 *
 *  Only SETTLING moves.  A DRAINING owner is mid-teardown and will commit its
 *  own result; anything else has nothing to do with this worker's parking. */
enum cam_own_state cam_own_settled(enum cam_own_state st);

#if defined(__ARM_ARCH)
/*
 * The serialised half.  Each of these applies exactly one of the pure decisions
 * above inside one interrupt-disabled section, so that every WRITE to an owner's
 * state goes through this file -- the discipline issue #65 arrived at from the
 * other direction (a poisoned state must be guarded at every assignment point,
 * not just the interesting one).
 *
 * PRIMASK rather than a mutex: the section is a table lookup and a store, it has
 * to work from the GUIX thread, a shell thread and (for cam_own_settle) an owner
 * worker alike, and there is no object to create at boot -- camera_ui_start()
 * runs before the scheduler.  A mutex here would also add a lock order to
 * reason about on exactly the teardown paths this is trying to make safe.
 *
 * Not built for the host test, which compiles the table half only; a firmware
 * build that somehow missed this block fails to link rather than losing the
 * serialisation quietly.
 */
enum cam_own_start cam_own_start_take(volatile enum cam_own_state *st);
enum cam_own_stop  cam_own_stop_take(volatile enum cam_own_state *st);
void cam_own_start_finish(volatile enum cam_own_state *st, int ok);
void cam_own_drain_finish(volatile enum cam_own_state *st,
                          enum cam_drain_step step, int worker_parked);
void cam_own_settle(volatile enum cam_own_state *st);
/**
 * @brief  Is there a start claim outstanding on @p st right now?
 *
 * The backstop for a DEFERRED start: the GUIX preview claims on one thread and
 * finishes on another, so its handler has to be able to ask "is the claim I am
 * about to finish actually mine".  Only the finishing side can move STARTING, so
 * a false answer here is durable -- there is no claim, and the handler must not
 * subscribe anything.  This is the only read of an owner state that is not part
 * of a transition, and it exists because that one caller genuinely cannot make
 * its decision and its transition at the same instant.
 */
int cam_own_start_claimed(volatile enum cam_own_state *st);
#endif /* __ARM_ARCH */

#ifdef __cplusplus
}
#endif

#endif /* CAM_OWN_H */
