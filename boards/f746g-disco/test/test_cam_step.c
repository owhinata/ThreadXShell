/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the camera sink's step machine (issue #142,
 * port/camera/cam_step.c).
 *
 * WHY THIS EXISTS.  Since issue #142 the GUIX preview and the nn ingest are two
 * steps of one camera sink, and `gui stop` / `nn stream stop` switch their own
 * step off while the base keeps running.  What lets them release what the step
 * reads is "disarmed, and the count reached zero" -- the producer is out and
 * cannot come back.  The cases that would break it cannot be typed: a disarm
 * while the producer is inside, a re-arm before it has left, a leave with no
 * enter.  Each is walked here, against the one function set.
 *
 * [!] WHAT IT DOES NOT COVER.  The critical section around each call, the
 * mutex around the arming, and the order in consume() (display, nn, then the
 * put) are cam_sink.c's -- review and the hardware pass hold those.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cam_step.h"

static int fails;

#define EQ(got, want, what)                                                  \
	do {                                                                 \
		long g_ = (long)(got), w_ = (long)(want);                    \
		if (g_ != w_) {                                              \
			printf("  FAIL %s:%d %s: got %ld, wanted %ld\n",     \
			       __func__, __LINE__, what, g_, w_);            \
			fails++;                                             \
		}                                                            \
	} while (0)

/* A zero-initialised step is disarmed and idle: the producer skips it. */
static void test_initial(void)
{
	struct cam_step_state s;

	memset(&s, 0, sizeof s);
	EQ(cam_step_enter(&s), 0, "disarmed: skipped");
	EQ(s.busy, 0, "skip does not count");
	EQ(cam_step_may_arm(&s), 1, "may arm");
	EQ(cam_step_leave(&s), CAM_STEP_E_IDLE, "leave with nobody in");
	EQ(s.busy, 0, "no underflow");
}

/* Armed: enter counts, leave uncounts, and the producer may go round again. */
static void test_enter_leave(void)
{
	struct cam_step_state s;

	memset(&s, 0, sizeof s);
	EQ(cam_step_arm(&s), CAM_STEP_OK, "arm");
	EQ(cam_step_arm(&s), CAM_STEP_E_ARMED, "arm twice");
	EQ(cam_step_may_arm(&s), 0, "armed: may not arm");
	EQ(cam_step_enter(&s), 1, "enter");
	EQ(s.busy, 1, "one inside");
	EQ(cam_step_leave(&s), CAM_STEP_OK, "leave");
	EQ(s.busy, 0, "out");
	EQ(cam_step_enter(&s), 1, "next frame");
	EQ(cam_step_leave(&s), CAM_STEP_OK, "leave");
}

/* [!] The proof: disarm while the producer is inside.  The count stays until
 * it leaves; meanwhile the step can neither be entered nor re-armed, and once
 * it is zero nothing can bring it back up. */
static void test_disarm_while_inside(void)
{
	struct cam_step_state s;

	memset(&s, 0, sizeof s);
	EQ(cam_step_arm(&s), CAM_STEP_OK, "arm");
	EQ(cam_step_enter(&s), 1, "producer inside");
	cam_step_disarm(&s);
	EQ(s.busy, 1, "disarm leaves the count");
	EQ(cam_step_may_arm(&s), 0, "inside: may not arm");
	EQ(cam_step_arm(&s), CAM_STEP_E_BUSY, "re-arm refused while inside");
	EQ(s.armed, 0, "refused arm moved nothing");
	EQ(cam_step_enter(&s), 0, "a second producer entry is refused");
	EQ(s.busy, 1, "still one");
	EQ(cam_step_leave(&s), CAM_STEP_OK, "producer leaves");
	EQ(s.busy, 0, "zero: the proof");
	/* After zero, the producer cannot come back in until someone arms. */
	EQ(cam_step_enter(&s), 0, "cannot come back");
	EQ(s.busy, 0, "still zero");
	/* And arming is allowed again, from zero. */
	EQ(cam_step_may_arm(&s), 1, "may arm again");
	EQ(cam_step_arm(&s), CAM_STEP_OK, "re-armed");
	cam_step_disarm(&s);
	cam_step_disarm(&s);
	EQ(s.armed, 0, "disarm is idempotent");
}

/* The count does not wrap into a false zero. */
static void test_saturation(void)
{
	struct cam_step_state s;

	memset(&s, 0, sizeof s);
	s.armed = 1;
	s.busy = UINT16_MAX - 1u;
	EQ(cam_step_enter(&s), 1, "last entry fits");
	EQ(s.busy, UINT16_MAX, "at the top");
	EQ(cam_step_enter(&s), 0, "refused at the top");
	EQ(s.busy, UINT16_MAX, "not wrapped");
}

int main(void)
{
	test_initial();
	test_enter_leave();
	test_disarm_while_inside();
	test_saturation();

	if (fails) {
		printf("test_cam_step: %d FAILED\n", fails);
		return 1;
	}
	printf("test_cam_step: all passed\n");
	return 0;
}
