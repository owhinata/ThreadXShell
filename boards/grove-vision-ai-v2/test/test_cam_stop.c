/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the stop's decision table (issue #65,
 * port/camera/cam_state.c).
 *
 * WHY THIS EXISTS.  camera_stream_stop() has to separate three answers that all
 * look like "the camera is not streaming": the port is poisoned, there was
 * nothing to stop, and the mutex never came free.  Only the middle one may
 * report success, because by camera.h's rule success is what permits the caller
 * to detach a sink -- and a sink unlinked while a lost producer is inside it is
 * the failure the whole CAM_ST_LOST state exists to prevent.
 *
 * None of that can be produced from a console.  The dangerous case in
 * particular -- a stop that waited for the API mutex and woke up owning it with
 * the port already poisoned by ANOTHER stop's join timeout -- needs two stops
 * overlapping on a board with one shell.  So the table is a pure function and
 * this walks it.
 *
 * [!] The vector that carries the whole point is HELD + CAM_ST_LOST.  LOST is
 * also "not streaming", so the moment a "not streaming -> success" shortcut is
 * put back in front of the poison test, that line fails -- which is what this
 * file is for.
 *
 * [!] AND HERE IS WHAT IT DOES NOT COVER, because a test that is believed to
 * prove more than it does is worse than no test.  This compiles cam_state.c and
 * nothing else: camera_stream_stop() itself is never built or run here, so a
 * shortcut added AHEAD of the call, a mutex released twice, or a success
 * returned after a failed join would all leave this file green.  What holds
 * those down is that the decision has exactly one implementation and the
 * function that calls it is short -- not this test.  What this test does hold
 * down is the table itself, including the two ways it could be made to fail
 * open by someone extending an enum: see the "future member" vectors below.
 */
#include <stdio.h>

#include "cam_state.h"

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

static const char *acq_name(enum cam_stop_acquire a)
{
	switch (a) {
	case CAM_STOP_ACQ_HELD:    return "held";
	case CAM_STOP_ACQ_TIMEOUT: return "timeout";
	case CAM_STOP_ACQ_ERROR:   return "error";
	}
	return "?";
}

static const char *state_name(enum cam_state s)
{
	switch (s) {
	case CAM_ST_DOWN:      return "down";
	case CAM_ST_READY:     return "ready";
	case CAM_ST_STREAMING: return "streaming";
	case CAM_ST_FAULTED:   return "faulted";
	case CAM_ST_LOST:      return "lost";
	}
	return "?";
}

static const char *action_name(enum cam_stop_action a)
{
	switch (a) {
	case CAM_STOP_REFUSE_STATE:  return "refuse-state";
	case CAM_STOP_REFUSE_LOCKED: return "refuse-locked";
	case CAM_STOP_ALREADY:       return "already";
	case CAM_STOP_JOIN:          return "join";
	}
	return "?";
}

static void expect(enum cam_stop_acquire acq, enum cam_state st,
                   enum cam_stop_action want)
{
	enum cam_stop_action got = cam_stop_decide(acq, st);

	CHECK(got == want, "decide(%s, %s) = %s, want %s",
	      acq_name(acq), state_name(st), action_name(got),
	      action_name(want));
}

/* ---- before the mutex ---------------------------------------------------- */

static void test_may_acquire(void)
{
	static const enum cam_state live[] = {
		CAM_ST_DOWN, CAM_ST_READY, CAM_ST_STREAMING, CAM_ST_FAULTED,
	};
	unsigned i;

	/* Nothing was ever created: there is no mutex to take. */
	for (i = 0u; i < sizeof live / sizeof live[0]; i++)
		CHECK(!cam_api_may_acquire(0, live[i]),
		      "may_acquire(no objects, %s) allowed the mutex",
		      state_name(live[i]));
	CHECK(!cam_api_may_acquire(0, CAM_ST_LOST),
	      "may_acquire(no objects, lost) allowed the mutex");

	/* [!] Poisoned refuses BEFORE the mutex (issue #48): a lost producer
	 * may still hold things, so a refused call must never queue. */
	CHECK(!cam_api_may_acquire(1, CAM_ST_LOST),
	      "may_acquire(lost) would have queued on the mutex");

	for (i = 0u; i < sizeof live / sizeof live[0]; i++)
		CHECK(cam_api_may_acquire(1, live[i]),
		      "may_acquire(%s) refused a usable port",
		      state_name(live[i]));
}

/* ---- after the attempt --------------------------------------------------- */

static void test_decide(void)
{
	static const enum cam_state every[] = {
		CAM_ST_DOWN, CAM_ST_READY, CAM_ST_STREAMING, CAM_ST_FAULTED,
		CAM_ST_LOST,
	};
	unsigned i;

	/*
	 * The mutex never came free.  Nothing was asked, so the state was never
	 * read under the lock and cannot be believed -- the answer must not
	 * depend on it, for ANY value, and must never be success.
	 */
	for (i = 0u; i < sizeof every / sizeof every[0]; i++)
		expect(CAM_STOP_ACQ_TIMEOUT, every[i], CAM_STOP_REFUSE_LOCKED);

	/* A mutex that failed for another reason is a broken kernel object,
	 * not contention: kept apart so "could not ask" stays narrow. */
	for (i = 0u; i < sizeof every / sizeof every[0]; i++)
		expect(CAM_STOP_ACQ_ERROR, every[i], CAM_STOP_REFUSE_STATE);

	/* Held: the state is now readable, and this is the real table. */
	expect(CAM_STOP_ACQ_HELD, CAM_ST_STREAMING, CAM_STOP_JOIN);
	expect(CAM_STOP_ACQ_HELD, CAM_ST_DOWN,      CAM_STOP_ALREADY);
	expect(CAM_STOP_ACQ_HELD, CAM_ST_READY,     CAM_STOP_ALREADY);
	expect(CAM_STOP_ACQ_HELD, CAM_ST_FAULTED,   CAM_STOP_ALREADY);

	/*
	 * [!] THE ONE THAT MATTERS.  Poisoned is also "not streaming", so a
	 * shortcut placed ahead of the poison test would answer ALREADY here --
	 * a confirmed stop that never happened, which the caller would act on
	 * by detaching a sink the lost producer may still be inside.
	 */
	expect(CAM_STOP_ACQ_HELD, CAM_ST_LOST, CAM_STOP_REFUSE_STATE);
}

/* ---- the property the callers actually rely on --------------------------- */

static void test_only_already_is_success(void)
{
	static const enum cam_stop_acquire acqs[] = {
		CAM_STOP_ACQ_HELD, CAM_STOP_ACQ_TIMEOUT, CAM_STOP_ACQ_ERROR,
	};
	static const enum cam_state every[] = {
		CAM_ST_DOWN, CAM_ST_READY, CAM_ST_STREAMING, CAM_ST_FAULTED,
		CAM_ST_LOST,
	};
	unsigned a, s;

	/*
	 * camera.h: only CAM_OK permits a detach, and the stop returns CAM_OK
	 * for exactly two actions -- ALREADY, and JOIN when the producer
	 * acknowledges.  So no combination that leaves a producer possibly
	 * running may come back as ALREADY.
	 */
	for (a = 0u; a < sizeof acqs / sizeof acqs[0]; a++)
		for (s = 0u; s < sizeof every / sizeof every[0]; s++) {
			enum cam_stop_action got =
				cam_stop_decide(acqs[a], every[s]);

			if (every[s] == CAM_ST_LOST ||
			    acqs[a] != CAM_STOP_ACQ_HELD)
				CHECK(got != CAM_STOP_ALREADY,
				      "decide(%s, %s) reported a stop that "
				      "never happened",
				      acq_name(acqs[a]), state_name(every[s]));
		}
}

/* ---- the enums are going to grow ----------------------------------------- */

/*
 * [!] A MEMBER ADDED LATER MUST REFUSE, not inherit the friendliest answer.
 *
 * Both enums are the kind that grows: a second poison state, an acquisition
 * that was interrupted rather than timed out.  The dangerous default is the
 * silent one -- an unknown acquisition treated as "we hold the mutex" answers
 * from the state table, and an unknown state treated as "not streaming" answers
 * ALREADY, which is a CONFIRMED STOP.  Either would be a caller detaching a
 * sink on the strength of an enumerator nobody had thought about yet.
 *
 * The next unused value stands in for that future member.  -Wall names the file
 * when the state switch grows a member with no case; this is the other half,
 * for the value that reaches the code before anyone updates it.
 */
static void test_future_members(void)
{
	static const enum cam_state every[] = {
		CAM_ST_DOWN, CAM_ST_READY, CAM_ST_STREAMING, CAM_ST_FAULTED,
		CAM_ST_LOST,
	};
	enum cam_stop_acquire future_acq = (enum cam_stop_acquire)3;
	enum cam_state future_state = (enum cam_state)5;
	unsigned i;

	for (i = 0u; i < sizeof every / sizeof every[0]; i++)
		CHECK(cam_stop_decide(future_acq, every[i]) ==
		      CAM_STOP_REFUSE_STATE,
		      "an unknown acquisition + %s did not refuse",
		      state_name(every[i]));

	CHECK(cam_stop_decide(CAM_STOP_ACQ_HELD, future_state) ==
	      CAM_STOP_REFUSE_STATE,
	      "an unknown state reported a stop that never happened");
	CHECK(cam_stop_decide(CAM_STOP_ACQ_TIMEOUT, future_state) ==
	      CAM_STOP_REFUSE_LOCKED,
	      "an unknown state changed what a lock timeout means");

	/*
	 * cam_api_may_acquire() is deliberately NOT pinned for an unknown state.
	 * Either answer is defensible -- a second poison state would want a
	 * refusal, and anything else would not -- and a test that guessed would
	 * fail on the commit that decides correctly.  The refusal that matters
	 * is CAM_ST_LOST's, above; the fail-closed answer after the mutex is
	 * what covers the rest.
	 */
}

int main(void)
{
	test_may_acquire();
	test_decide();
	test_only_already_is_success();
	test_future_members();

	if (failures != 0) {
		printf("test_cam_stop: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cam_stop: ok\n");
	return 0;
}
