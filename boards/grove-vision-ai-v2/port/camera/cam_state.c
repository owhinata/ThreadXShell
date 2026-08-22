/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The camera port's decision tables (issues #65, #72, #77).  See cam_state.h
 * for why they are here and not inline in camera.c: the cases that matter
 * cannot be produced on hardware, so they are written as pure functions and
 * walked by a host test.
 */
#include "cam_state.h"

int cam_api_may_acquire(int objects_ok, enum cam_state st)
{
	if (!objects_ok)
		return 0;
	return (st != CAM_ST_LOST);
}

enum cam_stop_action cam_stop_decide(enum cam_api_acquire acq,
                                     enum cam_state st)
{
	/*
	 * Nothing was asked of the producer, and nothing was touched.  That is
	 * all this says: the holder may be a stop that has already joined and
	 * not yet let go, or the producer may have left by itself, so `st` is
	 * not ours to read and the port is NOT poisoned on this path.
	 */
	if (acq == CAM_ACQ_UNAVAILABLE)
		return CAM_STOP_REFUSE_LOCKED;

	/*
	 * [!] ANYTHING THAT IS NOT "WE HOLD IT" REFUSES, tested that way round
	 * on purpose.  CAM_ACQ_ERROR is a mutex this port can no longer
	 * rely on -- the same class as never having created one, and kept apart
	 * from CAM_ACQ_UNAVAILABLE so that "could not ask" keeps its narrow
	 * meaning -- and a value added to the enum later lands here too.  Written
	 * as a default case beside HELD it would have fallen through to the state
	 * table instead, which can answer ALREADY: a confirmed stop, reported by
	 * a caller that does not hold the mutex.
	 */
	if (acq != CAM_ACQ_HELD)
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

enum cam_bus_owner cam_bus_decide(enum cam_api_acquire acq, enum cam_state st)
{
	/*
	 * [!] THE ACQUISITION FIRST, AND `st` IS NOT CONSULTED UNLESS WE HOLD IT.
	 *
	 * cam_state is published under this mutex by camera_stream_start(), so a
	 * value read without holding it is a value another thread may already have
	 * moved on from -- which is issue #77 itself, arriving one level down.  The
	 * caller passes a placeholder for the two refusals rather than a real read,
	 * so "ignored unless held" is true where the variable is TOUCHED and not
	 * merely here; this ordering is what makes that placeholder safe.
	 *
	 * Split the same way cam_stop_decide() splits it: contention is retryable
	 * and says nothing about the port, while a mutex that failed for any other
	 * reason is a broken kernel object and belongs with "nothing usable".
	 * Collapsing them would make a corrupt mutex look like a busy one and send
	 * the operator into a retry loop against a fault.
	 */
	if (acq == CAM_ACQ_UNAVAILABLE)
		return CAM_BUS_REFUSE_BUSY;
	if (acq != CAM_ACQ_HELD)
		return CAM_BUS_REFUSE_STATE;

	/*
	 * [!] ENUMERATED, NOT "ANYTHING BUT STREAMING".
	 *
	 * CAM_BUS_DIRECT is the permission to drive the vendor CIS driver, which
	 * has no locking of its own and which the producer uses too, so it is the
	 * one answer that can hurt.  A wider test -- `st != CAM_ST_STREAMING` --
	 * reads identically on every state anyone has thought about and fails open
	 * on CAM_ST_LOST, which IS reachable here (see cam_state.h: poison can land
	 * between the pre-mutex test and this acquire).  Direct on a poisoned port
	 * means cam_bringup() rebuilding it under a producer that never
	 * acknowledged a stop.
	 *
	 * So every state that may touch the bus is one somebody decided may touch
	 * it.  No `default:` -- -Wall names this file while a new member is still
	 * being added, and the fail-closed return below catches the value that
	 * reaches here before anyone updates the switch.
	 */
	switch (st) {
	case CAM_ST_STREAMING:
		return CAM_BUS_PRODUCER;
	case CAM_ST_DOWN:
	case CAM_ST_READY:
	case CAM_ST_FAULTED:
		return CAM_BUS_DIRECT;
	case CAM_ST_LOST:
		return CAM_BUS_REFUSE_STATE;
	}
	return CAM_BUS_REFUSE_STATE;
}

enum cam_drain_verdict cam_drain_decide(int drain_rc, int pins)
{
	/*
	 * The thread first: if it never came back, the pin count says nothing
	 * useful -- it is a reading of bookkeeping a live thread is still moving.
	 * Reporting PINNED there would name the wrong failure and send someone
	 * looking for a put() that is simply not due yet.
	 */
	if (drain_rc != 0)
		return CAM_DRAIN_THREAD;
	/*
	 * The thread is idle, so this count has stopped moving and zero is a
	 * decision rather than a reading (frame_pipeline_sink_pins()).  Anything
	 * else means a delivery was never handed back.
	 */
	if (pins != 0)
		return CAM_DRAIN_PINNED;
	return CAM_DRAIN_DONE;
}
