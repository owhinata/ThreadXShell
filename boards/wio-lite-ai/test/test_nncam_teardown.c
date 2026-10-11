/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for "has the last nn teardown finished" (issue #142 review,
 * port/nn/nncam_teardown.h).
 *
 * WHY THIS EXISTS.  A `nn stream stop` whose band drain times out needs a
 * producer stuck inside nn's callback, which cannot be produced at a console.
 * The three answers that depend on it -- a fresh start refused before it
 * touches anything, a worker that keeps the guards, a second stop that still
 * drains -- are walked here over the state sequence nn_camera.c moves through.
 */
#include <stdio.h>

#include "nncam_teardown.h"

static int fails;

#define EQ(got, want, what)                                                  \
	do {                                                                 \
		int g_ = (got), w_ = (want);                                 \
		if (g_ != w_) {                                              \
			printf("  FAIL %s:%d %s: got %d, wanted %d\n",       \
			       __func__, __LINE__, what, g_, w_);            \
			fails++;                                             \
		}                                                            \
	} while (0)

/* Never started, or cleanly stopped: everything free. */
static void test_idle(void)
{
	struct nncam_td s = { 0, 0, 1, 0 };

	EQ(nncam_td_fresh_start_ok(&s), 1, "idle: start");
	EQ(nncam_td_stop_notrun(&s), 1, "idle: nothing to stop");
	EQ(nncam_td_worker_gives(&s), 1, "idle: worker gives");
}

/* A stop raised the mark, disarmed the step and the drain timed out: the
 * producer is still inside (step not idle).  The worker then leaves its loop. */
static void test_drain_timed_out(void)
{
	struct nncam_td s = { 0, 1, 0, 1 };

	EQ(nncam_td_fresh_start_ok(&s), 0, "undrained: start refused");
	EQ(nncam_td_worker_gives(&s), 0, "undrained: worker keeps the guards");
	EQ(nncam_td_stop_notrun(&s), 0, "undrained: a stop still has work");

	/* The producer came out on its own, but no stop has seen it: still
	   refused, still the stop's to finish. */
	s.step_idle = 1;
	EQ(nncam_td_fresh_start_ok(&s), 0, "drained unseen: start refused");
	EQ(nncam_td_worker_gives(&s), 0, "drained unseen: worker keeps");
	EQ(nncam_td_stop_notrun(&s), 0, "drained unseen: stop has work");

	/* The second stop's drain succeeds: the mark goes, it gives. */
	s.drain_pending = 0;
	s.holds_guards  = 0;
	EQ(nncam_td_fresh_start_ok(&s), 1, "settled: start");
	EQ(nncam_td_stop_notrun(&s), 1, "settled: nothing to stop");
}

/* Nothing held any more, but the step is still occupied (the guards went back
 * by some other path): a stop must not say "not running". */
static void test_step_busy_only(void)
{
	struct nncam_td s = { 0, 0, 0, 0 };

	EQ(nncam_td_stop_notrun(&s), 0, "busy step: stop has work");
	EQ(nncam_td_fresh_start_ok(&s), 0, "busy step: start refused");

	/* Nor while a mark no stop has cleared is up, the step empty and
	   nothing held: only a stop that sees the drain lowers it. */
	s.step_idle = 1;
	s.drain_pending = 1;
	EQ(nncam_td_stop_notrun(&s), 0, "pending mark: stop has work");
}

/* A stop in progress (mark raised, drain not done yet) while the worker
 * leaves: it must not give. */
static void test_worker_during_stop(void)
{
	struct nncam_td s = { 0, 1, 1, 1 };

	EQ(nncam_td_worker_gives(&s), 0, "mid-stop: worker keeps");
	s.drain_pending = 0;
	EQ(nncam_td_worker_gives(&s), 1, "drained: worker may give");
}

/* Running: a stop has work whatever else holds. */
static void test_running(void)
{
	struct nncam_td s = { 1, 1, 0, 0 };

	EQ(nncam_td_stop_notrun(&s), 0, "running: stop");
	s.holds_guards = 0;
	s.step_idle = 1;
	EQ(nncam_td_stop_notrun(&s), 0, "running alone: stop");
}

int main(void)
{
	test_idle();
	test_drain_timed_out();
	test_step_busy_only();
	test_worker_during_stop();
	test_running();
	if (fails) {
		printf("test_nncam_teardown: %d FAILED\n", fails);
		return 1;
	}
	printf("test_nncam_teardown: all passed\n");
	return 0;
}
