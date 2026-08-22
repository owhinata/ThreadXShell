/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_state.h
 * @brief   The camera port's state machine, and the decisions taken over it
 *          (issues #48, #65, #72, #77).
 *
 * WHY THIS IS ITS OWN FILE.  The states live here rather than in camera.c
 * because the decisions taken over them are the parts of camera.c that CANNOT
 * be exercised on hardware.  For the stop it is a call which waited for the API
 * mutex and woke up owning it with the port already poisoned; for the bus
 * routing it is a call overtaken by a whole stream start, or by a stop that
 * poisons the port, between one statement and the next.  Neither can be
 * produced from a console -- this board has one shell, and its background jobs
 * run BELOW the foreground one under TX_NO_TIME_SLICE, so `cmd &; cmd2` cannot
 * even get the two threads into the right order.  As pure functions over the
 * state they are tables, and test/test_cam_stop.c walks them.
 *
 * So this file owns the PRECEDENCE; camera.c owns the sequencing and everything
 * that touches hardware.  Splitting it anywhere else would leave the precedence
 * untested, which is exactly the bug issue #65 turned out to be.
 *
 * [!] THE PRECEDENCE IS NOT THE SAME SHAPE IN BOTH TABLES, and assuming it is
 * would leave one of them untested.  In cam_stop_decide() it is an ORDERING
 * hazard -- CAM_ST_LOST is also "not streaming", so a shortcut placed ahead of
 * the poison test answers success.  In cam_bus_decide() the states are distinct
 * enumerators that no reordering can confuse; what fails open there is a WIDER
 * test that stops enumerating.  Each function says which one it is.
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

/**
 * How an attempt on the API mutex ended (issues #65, #77).
 *
 * [!] SHARED BY BOTH TABLES BELOW, and neutral about the wait on purpose.  The
 * stop waits (bounded), every other entry point uses TX_NO_WAIT -- but both
 * consume the SAME fact and only disagree about the policy to apply to it, so
 * one input enum with two tables is the honest shape.  A second, parallel
 * acquisition enum would be two vocabularies for one thing, and they drift.
 *
 * CAM_ACQ_UNAVAILABLE therefore means "the caller did not obtain the mutex
 * within whatever wait it was willing to do" -- the stop's deadline, or the
 * zero wait of everything else.  What that IMPLIES is table-specific and stays
 * in the tables: for a stop it is "nothing was asked" (CAM_ERR_LOCKED, and the
 * port is NOT poisoned); for a bus caller it is simply "busy, ask again".
 */
enum cam_api_acquire {
	CAM_ACQ_HELD = 0,    /**< this caller owns the mutex               */
	CAM_ACQ_UNAVAILABLE, /**< the mutex was not free within the wait   */
	CAM_ACQ_ERROR,       /**< the mutex refused for any other reason   */
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
enum cam_stop_action cam_stop_decide(enum cam_api_acquire acq,
                                     enum cam_state st);

/** Who may act on the sensor bus, once the mutex attempt has ended (#77). */
enum cam_bus_owner {
	CAM_BUS_REFUSE_STATE = 0, /**< nothing usable; refuse             */
	CAM_BUS_REFUSE_BUSY,      /**< the mutex was not obtained         */
	CAM_BUS_PRODUCER,         /**< a stream owns the sensor bus       */
	CAM_BUS_DIRECT,           /**< this caller owns it                */
};

/**
 * @brief  Who owns the sensor bus for this call?  (Issue #77.)
 *
 * @param acq  how the acquisition ended
 * @param st   the state, read while HOLDING the mutex (meaningless otherwise)
 *
 * WHY OWNERSHIP AND NOT AN ACTION.  Two groups of entry points ask this and
 * want different things from the same answer: camera_probe() and
 * camera_capture() turn CAM_BUS_PRODUCER into a refusal, while the tuning
 * setters turn it into a queued request for the producer.  A table that
 * returned "refuse" or "queue" could serve only one of them, and the second
 * would grow its own ordering -- which is how five entry points came to have
 * five of them (and camera_probe() a sixth) in the first place.
 *
 * [!] CAM_BUS_DIRECT IS THE ONLY DANGEROUS ANSWER.  It is the permission to
 * drive the vendor CIS driver, which has no locking and which the producer
 * uses too.  So everything here is arranged so that direct is reached only by
 * being named: unknown acquisitions and unknown states fall to a refusal, and
 * the state switch lists the states that may act rather than testing for the
 * one that may not.
 *
 * [!] AND CAM_ST_LOST IS REACHABLE HERE, WITH THE MUTEX HELD.  The poison test
 * is before the mutex (cam_api_may_acquire(), issue #48), so poison can land in
 * the gap: a caller passes that test while a stream is running, is preempted, a
 * stop takes the mutex, fails its join, writes CAM_ST_LOST and releases -- and
 * the caller then acquires and reads a poisoned state.  Classifying that as
 * "not streaming, therefore direct" would send it into cam_bringup(), which
 * accepts only READY and STREAMING as already-up and would therefore TEAR THE
 * PORT DOWN AND REBUILD IT under a producer that never acknowledged a stop.
 * That is the exact action CAM_ST_LOST exists to prevent.
 *
 * This is issue #65's "re-test the poison on the far side of the acquire",
 * arriving on a path that does not wait -- and it does not depend on an
 * unlikely schedule being likely.  The common interleaving ends in
 * CAM_BUS_REFUSE_BUSY (the stop still holds the mutex when the delayed caller
 * runs); this one needs the caller to stay unscheduled until the stop is done.
 * ThreadX does not promise it will run in that window, so the ordering is
 * permitted, and safety may not rest on the usual outcome.
 *
 * By contrast, "poison before STREAMING" is NOT a precedence inside this table
 * the way it is in cam_stop_decide(): the two are distinct enumerators, so no
 * reordering of equality tests can confuse them.  What can fail open is a
 * WIDER test -- `st != CAM_ST_STREAMING -> direct` -- which is why the states
 * are enumerated.
 */
enum cam_bus_owner cam_bus_decide(enum cam_api_acquire acq, enum cam_state st);

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
