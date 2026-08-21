/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_state.h
 * @brief   The camera port's state machine, and what a stop does in each state
 *          (issues #48, #65).
 *
 * WHY THIS IS ITS OWN FILE.  The states live here rather than in camera.c
 * because the stop's decision -- refuse, refuse without having asked, nothing to
 * do, or join the producer -- is the one part of camera.c that CANNOT be
 * exercised on hardware.  The case that matters is a stop which waited for the
 * API mutex and woke up owning it with the port already poisoned, and there is
 * no way to produce that from a console.  As a pure function over the state it
 * is a table, and test/test_cam_stop.c walks it.
 *
 * So this file owns the PRECEDENCE (poison before "not streaming", and never
 * the other way round); camera.c owns the sequencing and everything that
 * touches hardware.  Splitting it anywhere else would leave the precedence
 * untested, which is exactly the bug issue #65 turned out to be.
 */
#ifndef CAM_STATE_H
#define CAM_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

enum cam_state {
	CAM_ST_DOWN = 0,   /**< nothing brought up                        */
	CAM_ST_READY,      /**< powered, wrapped, sensor identified       */
	CAM_ST_STREAMING,
	CAM_ST_FAULTED,    /**< terminal; the next open() rebuilds it all */
	CAM_ST_LOST,       /**< [!] UNRECOVERABLE -- see below            */
};

/*
 * [!] CAM_ST_LOST: the producer never acknowledged a stop (issue #48).
 *
 * Every other state describes hardware the calling thread controls.  This one
 * describes hardware that MAY STILL BE IN USE by a producer thread which did
 * not come back inside the join deadline -- so the one thing that must not
 * happen is another command rebuilding, quiescing or powering it underneath
 * that thread.
 *
 * CAM_ST_FAULTED cannot express this: it is explicitly the recoverable
 * terminal state, and cam_bringup() treats it as "rebuild everything", which is
 * exactly the action that would be catastrophic here.  So this is separate, and
 * it is reached only from the join timeout.
 *
 * WHY REBOOT AND NOT A RETRY.  There is nothing to wait for.  The producer is
 * blocked somewhere the deadline already proved is longer than expected -- a
 * lost NPU interrupt, a wedged vendor driver -- and no later call can learn
 * whether it has finished, because the acknowledgement it would have used is
 * the very thing that did not arrive.  Refusing forever is the only honest
 * answer, and it is cheap: this state is unreachable unless something is
 * already badly wrong.
 */

/** How the stop's attempt on the API mutex ended (issue #65). */
enum cam_stop_acquire {
	CAM_STOP_ACQ_HELD = 0, /**< this caller owns the mutex            */
	CAM_STOP_ACQ_TIMEOUT,  /**< the bounded wait expired              */
	CAM_STOP_ACQ_ERROR,    /**< the mutex refused for any other reason */
};

/** What camera_stream_stop() should do (issue #65). */
enum cam_stop_action {
	CAM_STOP_REFUSE_STATE = 0, /**< refuse: nothing usable to stop     */
	CAM_STOP_REFUSE_LOCKED,    /**< refuse: never asked the producer   */
	CAM_STOP_ALREADY,          /**< nothing to join; report success    */
	CAM_STOP_JOIN,             /**< ask the producer and wait for it   */
};

/**
 * @brief  May a stop go on to take the API mutex?  (Before the mutex.)
 *
 * @return non-zero to acquire, zero to refuse with a state error.
 *
 * The poison test is BEFORE the mutex on purpose (issue #48): a lost producer
 * may still hold things, so making a refused call queue behind anything would
 * turn a clear refusal into a hang.
 */
int cam_api_may_acquire(int objects_ok, enum cam_state st);

/**
 * @brief  What the stop does once the attempt on the mutex has ended.
 *
 * @param acq  how the acquisition ended
 * @param st   the state, read while HOLDING the mutex (meaningless otherwise)
 *
 * [!] THE POISON COMES FIRST, and that is the whole reason this is a function.
 * CAM_ST_LOST is also "not streaming", so a "not streaming -> success" test
 * placed ahead of it would hand the caller a confirmed stop it never got --
 * and by camera.h's rule the caller would then detach a sink the lost producer
 * may be inside.  A stop can now block behind another stop's join, which is
 * exactly how it wakes up holding the mutex with the state already poisoned.
 */
enum cam_stop_action cam_stop_decide(enum cam_stop_acquire acq,
                                     enum cam_state st);

/** Why a sink's owner may not release what the sink reads (issue #72). */
enum cam_drain_verdict {
	CAM_DRAIN_DONE = 0, /**< thread idle, holding nothing: release       */
	CAM_DRAIN_THREAD,   /**< the thread never came back                  */
	CAM_DRAIN_PINNED,   /**< it came back still holding a pipeline slot  */
};

/**
 * @brief  May a sink's owner tear down what the sink points at?
 *
 * @param drain_rc  0 if the owner's own drain reported the thread idle
 * @param pins      the pipeline's count for this sink, taken AFTER that drain
 *
 * Two failures, not one, and they need different sentences: a thread that never
 * returned is somewhere unknown, while a thread that returned holding a slot has
 * a bookkeeping bug and the ring is one slot short.  Collapsing them into "drain
 * failed" is what left the second invisible until the NEXT stream ran out of
 * slots (issue #72).
 *
 * [!] THE COUNT DECIDES, NOT THE CLOCK.  @p drain_rc must already reflect a
 * final look at the state -- a drain that gives up the moment its deadline
 * passes, without re-reading, rejects a hand-off that completed at that same
 * instant.  This function cannot see that mistake; cam_panel_drain() avoids it
 * by testing the counters before the deadline on every pass.
 */
enum cam_drain_verdict cam_drain_decide(int drain_rc, int pins);

#ifdef __cplusplus
}
#endif

#endif /* CAM_STATE_H */
