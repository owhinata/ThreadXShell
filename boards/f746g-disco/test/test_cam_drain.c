/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the sink-drain decision (issue #72,
 * port/camera/cam_drain.c).
 *
 * WHY THIS EXISTS.  Three subscribers on this board detach while the base
 * capture keeps running, and each then has to wait for its sink to go idle
 * before releasing what that sink reads.  The branch that matters is the wait
 * that does NOT finish -- and nothing a console can type produces it: it needs a
 * consume() that never returns, or one that returns without putting its slot
 * back.  Both are bugs, not inputs.
 *
 * [!] THE VECTOR THAT CARRIES THE POINT is "zero on the poll where the deadline
 * also expired".  A drain that completed at the exact moment its budget ran out
 * has completed, and an implementation that tested the clock first would strand
 * the teardown that was entitled to proceed.  Issue #65 landed on that mistake
 * from the other direction (counting iterations instead of wall clock), and the
 * cure is the same: the state decides, the deadline only bounds the waiting.
 *
 * [!] AND WHAT THIS DOES NOT COVER.  It compiles cam_drain.c and nothing else.
 * Whether one count is ENOUGH is not a property of this file -- it rests on the
 * put-last rule holding in all three consume() callbacks, which only review and
 * the comments beside them enforce.  The owners' loops and lifecycles are in
 * guix_camera_ui.c, nn_camera.c and nx_mjpeg.c (and test_cam_own.c).
 */
#include <stdio.h>

#include "cam_drain.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

static const char *name(enum cam_drain_step s)
{
	switch (s) {
	case CAM_DRAIN_DONE:   return "DONE";
	case CAM_DRAIN_WAIT:   return "WAIT";
	case CAM_DRAIN_PINNED: return "PINNED";
	}
	return "?";
}

/* Idle is idle, whatever the clock says. */
static void test_done(void)
{
	CHECK(cam_sink_drain_step(0, 0) == CAM_DRAIN_DONE,
	      "idle with time left must be DONE, got %s",
	      name(cam_sink_drain_step(0, 0)));

	/*
	 * [!] The whole reason this is a function.  The count reached zero on the
	 * same poll that spent the last of the budget: the drain COMPLETED, and
	 * reporting a timeout here would hold a teardown that had every right to
	 * proceed.  An implementation that checks the deadline first fails exactly
	 * this line.
	 */
	CHECK(cam_sink_drain_step(0, 1) == CAM_DRAIN_DONE,
	      "idle ON the deadline must still be DONE, got %s",
	      name(cam_sink_drain_step(0, 1)));
}

/* Still pinned and time left: keep polling, do not decide. */
static void test_wait(void)
{
	CHECK(cam_sink_drain_step(1, 0) == CAM_DRAIN_WAIT,
	      "a held pin with time left must WAIT");
	CHECK(cam_sink_drain_step(3, 0) == CAM_DRAIN_WAIT,
	      "several held pins with time left must WAIT");
}

/* Out of time with a pin outstanding: the owner may not release. */
static void test_timeout(void)
{
	CHECK(cam_sink_drain_step(1, 1) == CAM_DRAIN_PINNED,
	      "a held pin at the deadline is PINNED");
	CHECK(cam_sink_drain_step(2, 1) == CAM_DRAIN_PINNED,
	      "several held pins at the deadline are PINNED");
}

/*
 * [!] A negative count must NOT be read as released.  The pipeline saturates its
 * decrement at zero (svc/frame_pipeline.c), so a negative one cannot come from
 * the pipeline at all -- it means the sink's bookkeeping was written by
 * something that had no business writing it.  Letting the teardown proceed on
 * that value would free what a callback is reading, on the strength of a number
 * nobody can explain.  Refusing costs a subsystem until reboot in a situation
 * that is already memory corruption; the earlier "forgive a counting slip"
 * reading was fail-open, and this is the line that says so.
 */
static void test_negative_is_not_released(void)
{
	CHECK(cam_sink_drain_step(-1, 0) == CAM_DRAIN_WAIT,
	      "a negative pin count with time left must keep waiting");
	CHECK(cam_sink_drain_step(-5, 1) == CAM_DRAIN_PINNED,
	      "a negative pin count at the deadline must refuse the release");
}

/* Nothing but DONE may let an owner release what its sink reads. */
static void test_only_done_releases(void)
{
	static const struct { int pins, late; } v[] = {
		{ 1, 0 }, { 1, 1 }, { 2, 0 }, { 2, 1 }, { 64, 1 },
		{ -1, 0 }, { -1, 1 },
	};

	for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
		CHECK(cam_sink_drain_step(v[i].pins, v[i].late) != CAM_DRAIN_DONE,
		      "pins=%d late=%d must not report DONE", v[i].pins, v[i].late);
}

int main(void)
{
	test_done();
	test_wait();
	test_timeout();
	test_negative_is_not_released();
	test_only_done_releases();

	if (failures != 0) {
		printf("test_cam_drain: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cam_drain: ok\n");
	return 0;
}
