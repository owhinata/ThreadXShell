/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for when `nn stream` may give the nn session back (issue #130,
 * port/nn/nn_sess_release.c).
 *
 * WHY THIS EXISTS.  Since #130 the camera producer writes the model's input
 * tensor, inside the arena the session guards, so the session may go back only
 * once a sink drain has confirmed the producer out.  The drain that does not
 * confirm needs a consume() that outlives a 100 ms budget at priority 10, and no
 * console can produce that.  So the decision is walked in full, and then the four
 * interleavings of the stop and the worker are played through the same
 * decide-and-clear step the firmware runs under nncam_lock: each must give the
 * session back exactly once, and never before the drain.
 */
#include <stdio.h>

#include "nn_sess_release.h"

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

/* ---- the whole table ------------------------------------------------------ */

static void test_table(void)
{
	static const int whos[] = {
		NN_SESS_WORKER_OUT, NN_SESS_STOP_PARKED, NN_SESS_STOP_BUSY, 7, -1,
	};

	for (int drained = 0; drained <= 1; drained++)
		for (int holds = 0; holds <= 1; holds++)
			for (unsigned i = 0; i < sizeof whos / sizeof whos[0]; i++) {
				int who = whos[i];
				int want = drained && holds &&
				           (who == NN_SESS_WORKER_OUT ||
				            who == NN_SESS_STOP_PARKED);
				int got = nn_sess_may_release(drained, holds,
				                              (enum nn_sess_who)who);

				CHECK(got == want,
				      "drained %d holds %d who %d: got %d want %d",
				      drained, holds, who, got, want);
			}
}

/* ---- the interleavings ----------------------------------------------------- */

/* The firmware's state, and its decide-and-clear (nncam_release_session()). */
static struct {
	int drained, holds, released, released_undrained;
} st;

static void begin(void)
{
	st.drained = 0;   /* nn_camera_start() clears it ... */
	st.holds = 1;     /* ... and holds the session       */
	st.released = 0;
	st.released_undrained = 0;
}

static void release(enum nn_sess_who who)
{
	if (nn_sess_may_release(st.drained, st.holds, who)) {
		st.holds = 0;
		st.released++;
		if (!st.drained)
			st.released_undrained++;
	}
}

static void drain_done(void) { st.drained = 1; }

static void ended(const char *what)
{
	CHECK(st.released == 1, "%s: released %d times, want exactly once",
	      what, st.released);
	CHECK(st.released_undrained == 0, "%s: released before the drain", what);
}

static void test_interleavings(void)
{
	/* Drain DONE, worker already parked: the worker left first and found no
	 * drain yet; the stop releases. */
	begin();
	release(NN_SESS_WORKER_OUT);
	CHECK(st.released == 0, "a worker out before the drain keeps the hold");
	drain_done();
	release(NN_SESS_STOP_PARKED);
	ended("drain DONE, worker parked");

	/* Drain DONE, worker still in the model (-2): it releases on its way out. */
	begin();
	drain_done();
	release(NN_SESS_STOP_BUSY);
	CHECK(st.released == 0, "a stop whose worker is busy keeps the hold");
	release(NN_SESS_WORKER_OUT);
	ended("drain DONE, -2, worker parks later");

	/* [!] Drain ran out (-7), worker parked: NOBODY may release, until a
	 * retrying stop confirms the drain. */
	begin();
	release(NN_SESS_WORKER_OUT);
	release(NN_SESS_STOP_PARKED);
	CHECK(st.released == 0 && st.holds == 1,
	      "[!] a drain that ran out keeps the hold for both callers");
	drain_done();          /* the retry's drain */
	release(NN_SESS_STOP_PARKED);
	ended("-7, then a retry that drains");

	/* Worker parks between the stop's settle poll and its commit. */
	begin();
	drain_done();
	release(NN_SESS_STOP_BUSY);
	release(NN_SESS_WORKER_OUT);
	release(NN_SESS_STOP_PARKED);   /* a late extra ask changes nothing */
	ended("worker parks between poll and commit");
}

int main(void)
{
	test_table();
	test_interleavings();
	if (failures) {
		printf("test_nn_sess_release: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_sess_release: all cases pass\n");
	return 0;
}
