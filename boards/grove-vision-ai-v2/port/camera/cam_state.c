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
	switch (acq) {
	case CAM_STOP_ACQ_TIMEOUT:
		/*
		 * Nothing was asked of the producer, and nothing was touched.
		 * This says only that: the holder may be a stop that has
		 * already joined and not yet let go, or the producer may have
		 * left by itself, so `st` is not ours to read and the port is
		 * NOT poisoned on this path.
		 */
		return CAM_STOP_REFUSE_LOCKED;
	case CAM_STOP_ACQ_ERROR:
		/*
		 * Not contention: a mutex that fails for any other reason is a
		 * kernel object this port can no longer rely on, which is the
		 * same class as never having created one.  Kept apart from the
		 * timeout so that "could not ask" keeps its narrow meaning.
		 */
		return CAM_STOP_REFUSE_STATE;
	case CAM_STOP_ACQ_HELD:
	default:
		break;
	}

	/* [!] Poison first -- see the note in cam_state.h.  CAM_ST_LOST is also
	 * "not streaming", so the order of these two tests is the correctness. */
	if (st == CAM_ST_LOST)
		return CAM_STOP_REFUSE_STATE;
	if (st != CAM_ST_STREAMING)
		return CAM_STOP_ALREADY;
	return CAM_STOP_JOIN;
}
