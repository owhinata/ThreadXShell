/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_hold_rules.h
 * @brief   The decisions this board takes around the camera's holder table
 *          (issue #142): when the panel sink may be unlinked, how a stop came
 *          out, and whether the stream has ended under its holders.
 *
 * The table itself is svc/cam_holders.h; the locking and the acting are
 * cam_lcd_sink.c's.  What is here is pure, so test/test_cam_hold_rules.c can
 * walk every input -- the interesting ones (a stop that loses the API mutex, an
 * unlink that finds a callback in flight, a producer that gave up by itself
 * between two commands) cannot be typed at a console.
 *
 * [!] THE CODES BELOW ARE MIRRORED, NOT INCLUDED (camera.h pulls in the whole
 * datapath, which a host compiler cannot take).  cam_lcd_sink.c static-asserts
 * each one against camera.h, so a renumbered code is a build failure on the
 * board rather than a table deciding about values nobody returns.
 */
#ifndef CAM_HOLD_RULES_H
#define CAM_HOLD_RULES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors of camera.h -- see the note above. */
#define CAM_HOLD_CAM_OK        0
#define CAM_HOLD_CAM_TIMEOUT (-3)
#define CAM_HOLD_CAM_STATE   (-4)
#define CAM_HOLD_CAM_BUSY    (-7)
#define CAM_HOLD_CAM_LOCKED  (-8)

/**
 * May the panel sink be unlinked, given what camera_stream_stop() returned?
 *
 * [!] ONLY ON A CONFIRMED STOP -- camera.h's rule, and the second of the two
 * proofs a stop on this board needs (the first, that nn's overlay is no longer
 * in use, is the overlay's count and permits nothing here).  An empty holder
 * table is not this proof: it says nobody WANTS the stream, not that the
 * producer is out of consume().
 */
int cam_hold_may_detach(int cam_rc);

/** How a stop -- the last holder leaving, or the settling of a stream that
 *  ended by itself -- came out. */
enum cam_hold_end {
	/** The producer is stopped and the sink unlinked: the table may be
	 *  emptied. */
	CAM_HOLD_END_DONE = 0,
	/** Nothing is lost and the same stop can finish it: the camera API
	 *  stayed locked (the producer was never asked), or the unlink found a
	 *  transition in flight.  The table is kept. */
	CAM_HOLD_END_RETRY,
	/** Unconfirmed: a producer that never acknowledged, a camera that
	 *  refused, a panel thread that did not come back, or a code nobody
	 *  documented.  The table, the sink and every resource are kept, and
	 *  nothing starts again until reboot. */
	CAM_HOLD_END_TERMINAL,
};

/**
 * @param cam_rc            what camera_stream_stop() returned
 * @param detach_attempted  whether the unlink was run
 * @param detach_rc         what it returned; ignored unless attempted
 *
 * Unknown codes fail closed to TERMINAL, on both halves.
 */
enum cam_hold_end cam_hold_end_of(int cam_rc, int detach_attempted,
                                  int detach_rc);

/** What the holder code must settle before it answers anything else. */
enum cam_hold_settle {
	CAM_HOLD_SETTLE_NONE = 0,  /**< nothing owed                         */
	CAM_HOLD_SETTLE_STOP,      /**< a stop confirmed the camera and still
	                            *   owes the unlink                      */
	CAM_HOLD_SETTLE_ERROR,     /**< the stream ended by itself (the
	                            *   producer gave up) while held: confirm
	                            *   it and mark the holders lost         */
};

/**
 * @param linked     the panel sink is attached (its state, under the holder
 *                   lock -- every attach and unlink runs under it)
 * @param streaming  the camera says it is streaming
 * @param stopping   a stop already confirmed the camera and owes the unlink
 *
 * [!] "LINKED AND NOT STREAMING" IS AN ERROR END ONLY BECAUSE NOTHING ELSE CAN
 * PRODUCE IT.  Every stop of this stream runs under the holder lock and sets
 * @p stopping before it unlinks, and no other stream can start while the sink
 * is linked (camera.h, issue #63).  So a linked sink on a camera that is not
 * streaming, with no stop in progress, is a producer that left by itself.
 */
enum cam_hold_settle cam_hold_settle_of(int linked, int streaming,
                                        int stopping);

#ifdef __cplusplus
}
#endif

#endif /* CAM_HOLD_RULES_H */
