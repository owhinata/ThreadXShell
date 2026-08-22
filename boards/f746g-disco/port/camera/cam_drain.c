/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The sink-drain decision (issue #72).  See cam_drain.h for why this board needs
 * a drain at all, why one count is enough, and why the decision is separated
 * from the waiting.
 *
 * Pure: no register, no log, no kernel call.  The owners supply the count and
 * own the sleeping.
 */
#include "cam_drain.h"

enum cam_drain_step cam_sink_drain_step(int pins, int deadline_passed)
{
	/*
	 * The count first, unconditionally.  A drain that completed on the same
	 * poll that spent the last of its budget has completed, and reporting a
	 * timeout there would strand a teardown that was entitled to proceed.
	 *
	 * [!] EXACTLY ZERO, not "zero or less".  unpin_locked() saturates at zero
	 * (svc/frame_pipeline.c), so the pipeline cannot produce a negative count
	 * at all -- a negative one means the sink's bookkeeping has been written by
	 * something that had no business writing it.  Reading that as "released"
	 * would let a teardown free what a callback is reading on the strength of a
	 * value nobody can explain, which is the fail-open shape this whole issue is
	 * about.  Refusing costs a subsystem until reboot in a situation that is
	 * already memory corruption.
	 */
	if (pins == 0)
		return CAM_DRAIN_DONE;

	if (!deadline_passed)
		return CAM_DRAIN_WAIT;

	/*
	 * Out of time with a pin outstanding.  Three different things reach here and
	 * the owner cannot tell them apart from the count: a consume() still
	 * running (entitled to its pin), or one that returned without putting (a
	 * missing put, and the ring one slot short for good).  Neither lets the
	 * owner release what the sink reads, and that is the only decision this
	 * function is asked for -- so they share an answer here, and the owner's
	 * PENDING state keeps refusing until a later poll proves zero.  (The third
	 * is the impossible negative above, which never gets its own answer for the
	 * same reason: the owner's decision is the same either way.)
	 */
	return CAM_DRAIN_PINNED;
}
