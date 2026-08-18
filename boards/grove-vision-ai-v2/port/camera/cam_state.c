/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The stop's decision table (issue #65).  See cam_state.h for why it is here
 * and not inline in camera.c: the case that matters cannot be produced on
 * hardware, so it is written as a pure function and walked by a host test.
 */
#include "cam_state.h"

int cam_api_may_acquire(int objects_ok, enum cam_state st)
{
	if (!objects_ok)
		return 0;
	return (st != CAM_ST_LOST);
}

enum cam_stop_action cam_stop_decide(enum cam_stop_acquire acq,
                                     enum cam_state st)
{
	/*
	 * Nothing was asked of the producer, and nothing was touched.  That is
	 * all this says: the holder may be a stop that has already joined and
	 * not yet let go, or the producer may have left by itself, so `st` is
	 * not ours to read and the port is NOT poisoned on this path.
	 */
	if (acq == CAM_STOP_ACQ_TIMEOUT)
		return CAM_STOP_REFUSE_LOCKED;

	/*
	 * [!] ANYTHING THAT IS NOT "WE HOLD IT" REFUSES, tested that way round
	 * on purpose.  CAM_STOP_ACQ_ERROR is a mutex this port can no longer
	 * rely on -- the same class as never having created one, and kept apart
	 * from the timeout so "could not ask" keeps its narrow meaning -- and a
	 * value added to the enum later lands here too.  Written as a default
	 * case beside HELD it would have fallen through to the state table
	 * instead, which can answer ALREADY: a confirmed stop, reported by a
	 * caller that does not hold the mutex.
	 */
	if (acq != CAM_STOP_ACQ_HELD)
		return CAM_STOP_REFUSE_STATE;

	/*
	 * [!] Poison first -- see the note in cam_state.h.  CAM_ST_LOST is also
	 * "not streaming", so the order of these two is the correctness.
	 *
	 * Spelled out per state rather than as "not streaming -> ALREADY": every
	 * state that reports SUCCESS has to be one somebody decided reports
	 * success.  A state added later reaches the fail-closed return below and
	 * refuses until it is listed here -- and -Wall names the file while it
	 * is still being added, because this switch has no default.
	 */
	switch (st) {
	case CAM_ST_STREAMING:
		return CAM_STOP_JOIN;
	case CAM_ST_DOWN:
	case CAM_ST_READY:
	case CAM_ST_FAULTED:
		return CAM_STOP_ALREADY;
	case CAM_ST_LOST:
		return CAM_STOP_REFUSE_STATE;
	}
	return CAM_STOP_REFUSE_STATE;
}
