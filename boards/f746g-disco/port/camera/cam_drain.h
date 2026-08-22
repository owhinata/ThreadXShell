/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_drain.h
 * @brief   When a sink's owner may release what the sink reads (issue #72).
 *
 * WHY THIS BOARD NEEDS A DRAIN AND WIO DOES NOT.  Here `camera_unsubscribe()`
 * detaches a subscriber while the BASE CAPTURE KEEPS RUNNING -- that is the
 * cascade contract, and it is the whole point of a subscriber.  So a publish can
 * be in flight across the unlink: frame_pipeline_publish() copies the sinks it
 * will deliver to into a local array, drops the pipeline lock, and only then
 * calls consume() on each.  A sink detached inside that window still gets its
 * consume() call, and holds its pin until that call puts it back.
 *
 * [!] THIS IS WHAT THE EXISTING COMMENTS GOT WRONG, and why no owner here ever
 * drained.  nx_mjpeg.c said "in-flight is always 0"; nn_camera.c said "release
 * the pin (in-flight 0)".  Both are true of the CONSUME BODY -- every path in
 * both sinks puts before returning -- and neither is true immediately after
 * detach, which is when the owner wants to know.
 *
 * [!] ONE COUNT, AND ONLY BECAUSE OF THE PUT-LAST RULE.  Every consume() on this
 * board now makes camera_frame_put() its LAST STATEMENT, so what the pin count
 * proves is exactly what an owner needs:
 *
 *     once the pin reaches zero, that callback makes no further access to
 *     owner-owned state or resources.
 *
 * Note what it does NOT prove: not that the C function has returned (there is
 * still its epilogue, and frame_pipeline_publish() updates the sink's statistics
 * after consume() returns).  That is enough here only because the sink objects
 * themselves are static -- nobody frees them.  An owner that ever gave its sink
 * a dynamic lifetime would need more than this, and the rule note beside each
 * callback is what keeps a future edit from quietly moving work back after the
 * put.
 *
 * WHY IT IS A PURE FUNCTION.  The branch that matters is the drain that does not
 * finish, and nothing a console can type produces it: it needs a consume() that
 * never returns, or one that returns without putting.  As a decision over a
 * count and a deadline it is a table, and test/test_cam_drain.c walks it.
 * Grove reached the same conclusion for its own teardown (cam_state.h).
 */
#ifndef CAM_DRAIN_H
#define CAM_DRAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/** One poll of a sink drain. */
enum cam_drain_step {
	CAM_DRAIN_DONE = 0, /**< released everything: the owner may tear down  */
	CAM_DRAIN_WAIT,     /**< not yet, and there is time left: poll again   */
	CAM_DRAIN_PINNED,   /**< out of time, the sink still holds a slot      */
};

/**
 * @brief  Decide what a draining owner does next.
 *
 * @param pins            what frame_pipeline_sink_pins() reports for this sink
 * @param deadline_passed non-zero once the owner's time budget is spent
 *
 * [!] THE COUNT DECIDES, THE DEADLINE ONLY BOUNDS.  Zero on the final poll is
 * DONE even when the deadline has passed on that same poll -- testing the clock
 * first would reject a drain at the exact moment it completed.  That is the
 * mistake issue #65 landed on from the other direction, and the reason this is
 * a function rather than an `if` in three files.
 *
 * [!] ONLY EXACTLY ZERO IS DONE.  The pipeline saturates its decrement at zero,
 * so a negative count is not a counting slip to be forgiven -- it is bookkeeping
 * somebody else wrote over, and letting a teardown proceed on it is fail-open.
 */
enum cam_drain_step cam_sink_drain_step(int pins, int deadline_passed);

#ifdef __cplusplus
}
#endif

#endif /* CAM_DRAIN_H */
