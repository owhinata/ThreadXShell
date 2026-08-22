/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The owner-lifecycle table (issue #72).  See cam_own.h for the machine, why
 * DRAINING is entered before the unsubscribe, and why the serialiser is never
 * held across the work.
 *
 * The first half is pure -- no register, no log, no kernel call -- so
 * test/test_cam_own.c can walk transitions no console can produce.  The second
 * half is the one interrupt-disabled section that every state WRITE goes
 * through, and is built for the target only.
 */
#include "cam_own.h"

/* ---- the table (pure) ----------------------------------------------------- */

enum cam_own_start cam_own_start_decide(enum cam_own_state st)
{
	/*
	 * Spelled out per state rather than as "not IDLE -> refuse": every state
	 * that lets a start through has to be one somebody decided lets a start
	 * through.  A state added later reaches the fail-closed return below and
	 * refuses until it is listed here -- and -Wall names this file while it is
	 * still being added, because this switch has no default.
	 */
	switch (st) {
	case CAM_OWN_IDLE:
		return CAM_OWN_START_GO;
	case CAM_OWN_RUNNING:
		return CAM_OWN_START_RUNNING;
	case CAM_OWN_STARTING:
	case CAM_OWN_DRAINING:
	case CAM_OWN_SETTLING:
	case CAM_OWN_PENDING:
		return CAM_OWN_START_HELD;
	}
	return CAM_OWN_START_HELD;
}

enum cam_own_state cam_own_start_next(enum cam_own_start act,
                                      enum cam_own_state st)
{
	/* Only a granted claim moves the state; a refusal must leave it exactly as
	   it was, or two refused starts would walk the machine somewhere new. */
	return (act == CAM_OWN_START_GO) ? CAM_OWN_STARTING : st;
}

enum cam_own_stop cam_own_stop_decide(enum cam_own_state st)
{
	switch (st) {
	case CAM_OWN_RUNNING:
		return CAM_OWN_STOP_DRAIN;
	/*
	 * Both of these are "our sink is already detached, finish what an earlier
	 * stop could not".  PENDING re-polls the pin; SETTLING re-checks the
	 * worker.  They share a path because polling a pin that is already zero
	 * costs nothing, and because an owner in PENDING may ALSO have a worker
	 * that never parked -- one retry has to be able to finish both.
	 */
	case CAM_OWN_PENDING:
	case CAM_OWN_SETTLING:
		return CAM_OWN_STOP_RETRY;
	/*
	 * [!] STARTING refuses instead of tearing down.  A stop that ran a whole
	 * teardown between a start's claim and its attach would leave the start to
	 * finish into a lifecycle that says IDLE -- both commands reporting
	 * success, one of them wrongly.
	 */
	case CAM_OWN_STARTING:
	case CAM_OWN_DRAINING:
		return CAM_OWN_STOP_HELD;
	case CAM_OWN_IDLE:
		return CAM_OWN_STOP_IDLE;
	}
	return CAM_OWN_STOP_HELD;
}

enum cam_own_state cam_own_stop_next(enum cam_own_stop act,
                                     enum cam_own_state st)
{
	/* A granted stop owns the sink from here to its commit -- which is the
	   point: DRAINING is entered BEFORE camera_unsubscribe(), so no start can
	   re-attach the sink while its pins are being watched. */
	if (act == CAM_OWN_STOP_DRAIN || act == CAM_OWN_STOP_RETRY)
		return CAM_OWN_DRAINING;
	return st;
}

enum cam_own_state cam_own_start_done(enum cam_own_state st, int ok)
{
	/* Nothing but the claiming start can move STARTING (every other entry
	   point refuses there), so this only ever sees STARTING.  Tested that way
	   round anyway: a stray finish must not invent a lifecycle. */
	if (st != CAM_OWN_STARTING)
		return st;
	return ok ? CAM_OWN_RUNNING : CAM_OWN_IDLE;
}

enum cam_own_state cam_own_drain_next(enum cam_drain_step step,
                                      int worker_parked)
{
	/*
	 * [!] The pin outranks the worker.  The sink is the thing a later start
	 * would re-attach and clobber, so a pin still outstanding has to be what
	 * gets remembered -- an owner recorded as merely SETTLING would let a
	 * retry that only watched its worker declare the teardown finished.
	 */
	if (step != CAM_DRAIN_DONE)
		return CAM_OWN_PENDING;
	return worker_parked ? CAM_OWN_IDLE : CAM_OWN_SETTLING;
}

int cam_own_start_is_claimed(enum cam_own_state st)
{
	return (st == CAM_OWN_STARTING);
}

enum cam_own_state cam_own_settled(enum cam_own_state st)
{
	/* A DRAINING owner is mid-teardown and commits its own result; moving it
	   from here would let a worker's parking overwrite a PENDING pin. */
	return (st == CAM_OWN_SETTLING) ? CAM_OWN_IDLE : st;
}

/* ---- the serialised half (target only) ------------------------------------ */

#if defined(__ARM_ARCH)

#include "stm32f7xx_hal.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

/*
 * One shape, used six times: read the state, apply one pure decision, store the
 * result, all with interrupts off.  What this buys is that a decision and the
 * transition it implies cannot be split -- the failure the first attempt at #72
 * had, where a `volatile` read decided and something else acted on it.
 */
enum cam_own_start cam_own_start_take(volatile enum cam_own_state *st)
{
	uint32_t primask = __get_PRIMASK();
	enum cam_own_start act;

	__disable_irq();
	act = cam_own_start_decide(*st);
	*st = cam_own_start_next(act, *st);
	__set_PRIMASK(primask);
	return act;
}

enum cam_own_stop cam_own_stop_take(volatile enum cam_own_state *st)
{
	uint32_t primask = __get_PRIMASK();
	enum cam_own_stop act;

	__disable_irq();
	act = cam_own_stop_decide(*st);
	*st = cam_own_stop_next(act, *st);
	__set_PRIMASK(primask);
	return act;
}

void cam_own_start_finish(volatile enum cam_own_state *st, int ok)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	*st = cam_own_start_done(*st, ok);
	__set_PRIMASK(primask);
}

void cam_own_drain_finish(volatile enum cam_own_state *st,
                          enum cam_drain_step step, int worker_parked)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	*st = cam_own_drain_next(step, worker_parked);
	__set_PRIMASK(primask);
}

void cam_own_settle(volatile enum cam_own_state *st)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	*st = cam_own_settled(*st);
	__set_PRIMASK(primask);
}

/*
 * The one read that is not part of a transition.  It is safe for its single
 * caller and for nothing else: only the side that finishes a claim can move
 * STARTING, so a deferred start handler asking "is my claim still there" gets a
 * durable answer.  Every OTHER use of an owner state is a decision taken
 * together with its transition -- a general reader would invite exactly the
 * shape this issue removes, where something reads a state and acts on it after
 * it has changed.
 */
int cam_own_start_claimed(volatile enum cam_own_state *st)
{
	uint32_t primask = __get_PRIMASK();
	int claimed;

	__disable_irq();
	claimed = cam_own_start_is_claimed(*st);
	__set_PRIMASK(primask);
	return claimed;
}

#endif /* __ARM_ARCH */
