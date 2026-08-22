/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the owner-lifecycle table (issue #72, port/camera/cam_own.c).
 *
 * WHY THIS EXISTS.  Three subscribers on this board detach their sink while the
 * base capture keeps running, so each has an interval where the sink is
 * detached, still pinned, and about to be handed back.  The decisions that
 * protect that interval cannot be produced from a console: they need a drain
 * that spends its budget, or two owner commands genuinely in flight at once.  As
 * a decision over the state they are a table, and this walks it.
 *
 * [!] WHAT THIS CANNOT COVER, said plainly so the file does not imply otherwise.
 * It compiles cam_own.c's PURE half and cam_drain.c.  It cannot test:
 *   - the serialisation itself -- it drives linearised sequences, which is not
 *     the same as proving the critical section excludes what it must;
 *   - that DRAINING is entered BEFORE camera_unsubscribe(), which is an ordering
 *     in the callers, not a value in the table;
 *   - the drain polling, the ThreadX waits, or any owner's teardown effects.
 * Those stay in the owner files, held down by review and the hardware pass.
 * Calling this "the lifecycle is tested" would be the kind of claim issue #72
 * exists to correct.
 */
#include <stdio.h>

#include "cam_own.h"

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

static const char *sname(enum cam_own_state st)
{
	switch (st) {
	case CAM_OWN_IDLE:     return "IDLE";
	case CAM_OWN_STARTING: return "STARTING";
	case CAM_OWN_RUNNING:  return "RUNNING";
	case CAM_OWN_DRAINING: return "DRAINING";
	case CAM_OWN_SETTLING: return "SETTLING";
	case CAM_OWN_PENDING:  return "PENDING";
	}
	return "?";
}

/* A value no version of this enum has ever defined, to check the fail-closed
   returns.  This is what a state added later looks like to code built before
   it -- and the whole point of the switches having no default. */
#define BOGUS_STATE ((enum cam_own_state)99)

/* ---- the two entry tables ------------------------------------------------- */

static void test_start_table(void)
{
	CHECK(cam_own_start_decide(CAM_OWN_IDLE) == CAM_OWN_START_GO,
	      "IDLE must let a start through");
	CHECK(cam_own_start_decide(CAM_OWN_RUNNING) == CAM_OWN_START_RUNNING,
	      "RUNNING is the owner's own 'already running'");

	/* [!] The heart of it: a start walking into a teardown would re-attach the
	   sink, and frame_pipeline_attach() resets the pin count that teardown is
	   waiting on -- erasing the evidence, silently. */
	CHECK(cam_own_start_decide(CAM_OWN_DRAINING) == CAM_OWN_START_HELD,
	      "a start during DRAINING must be refused");
	CHECK(cam_own_start_decide(CAM_OWN_PENDING) == CAM_OWN_START_HELD,
	      "a start during PENDING must be refused");
	CHECK(cam_own_start_decide(CAM_OWN_SETTLING) == CAM_OWN_START_HELD,
	      "a start during SETTLING must be refused");
	CHECK(cam_own_start_decide(CAM_OWN_STARTING) == CAM_OWN_START_HELD,
	      "a second start during STARTING must be refused");
	CHECK(cam_own_start_decide(BOGUS_STATE) == CAM_OWN_START_HELD,
	      "an unknown state must refuse a start");
}

static void test_stop_table(void)
{
	CHECK(cam_own_stop_decide(CAM_OWN_RUNNING) == CAM_OWN_STOP_DRAIN,
	      "RUNNING is the case that detaches and drains");
	CHECK(cam_own_stop_decide(CAM_OWN_PENDING) == CAM_OWN_STOP_RETRY,
	      "PENDING must be retryable -- it is the only thing that clears it");
	CHECK(cam_own_stop_decide(CAM_OWN_SETTLING) == CAM_OWN_STOP_RETRY,
	      "SETTLING must be retryable too");
	CHECK(cam_own_stop_decide(CAM_OWN_IDLE) == CAM_OWN_STOP_IDLE,
	      "IDLE is the owner's 'not running'");
	CHECK(cam_own_stop_decide(CAM_OWN_DRAINING) == CAM_OWN_STOP_HELD,
	      "a second stop must not join a drain in progress");

	/* [!] A stop that ran a whole teardown between a start's claim and its
	   attach would leave the start finishing into a lifecycle that says IDLE --
	   two commands, both reporting success, one of them wrongly. */
	CHECK(cam_own_stop_decide(CAM_OWN_STARTING) == CAM_OWN_STOP_HELD,
	      "a stop racing a start must be refused, not interleaved");
	CHECK(cam_own_stop_decide(BOGUS_STATE) == CAM_OWN_STOP_HELD,
	      "an unknown state must refuse a stop");
}

/* A refusal must not move the state: two refused attempts in a row would
   otherwise walk the machine somewhere nobody chose. */
static void test_refusal_does_not_move(void)
{
	static const enum cam_own_state all[] = {
		CAM_OWN_IDLE, CAM_OWN_STARTING, CAM_OWN_RUNNING,
		CAM_OWN_DRAINING, CAM_OWN_SETTLING, CAM_OWN_PENDING, BOGUS_STATE,
	};

	for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
		enum cam_own_state st = all[i];
		enum cam_own_start sa = cam_own_start_decide(st);
		enum cam_own_stop  pa = cam_own_stop_decide(st);

		if (sa != CAM_OWN_START_GO)
			CHECK(cam_own_start_next(sa, st) == st,
			      "a refused start moved %s", sname(st));
		else
			CHECK(cam_own_start_next(sa, st) == CAM_OWN_STARTING,
			      "a granted start must claim STARTING");

		if (pa != CAM_OWN_STOP_DRAIN && pa != CAM_OWN_STOP_RETRY)
			CHECK(cam_own_stop_next(pa, st) == st,
			      "a refused stop moved %s", sname(st));
		else
			CHECK(cam_own_stop_next(pa, st) == CAM_OWN_DRAINING,
			      "a granted stop must own DRAINING from %s", sname(st));
	}
}

/* ---- the three commit points ---------------------------------------------- */

static void test_start_done(void)
{
	CHECK(cam_own_start_done(CAM_OWN_STARTING, 1) == CAM_OWN_RUNNING,
	      "a start that subscribed becomes RUNNING");
	CHECK(cam_own_start_done(CAM_OWN_STARTING, 0) == CAM_OWN_IDLE,
	      "a start that failed must release the claim, not keep it");

	/* A claim nobody made must not be finished into existence -- this is what a
	   deferred finish (the GUIX autostart, which runs on another thread) looks
	   like when the start it belonged to has already been accounted for. */
	CHECK(cam_own_start_done(CAM_OWN_RUNNING, 1) == CAM_OWN_RUNNING,
	      "finishing a start we do not own must be a no-op");
	CHECK(cam_own_start_done(CAM_OWN_RUNNING, 0) == CAM_OWN_RUNNING,
	      "a failed finish must not tear down a running owner");
	CHECK(cam_own_start_done(CAM_OWN_PENDING, 0) == CAM_OWN_PENDING,
	      "a failed finish must not clear a pinned sink");
	CHECK(cam_own_start_done(BOGUS_STATE, 1) == BOGUS_STATE,
	      "an unknown state must not be finished into RUNNING");
}

static void test_drain_next(void)
{
	CHECK(cam_own_drain_next(CAM_DRAIN_DONE, 1) == CAM_OWN_IDLE,
	      "drained and parked is the clean teardown");
	CHECK(cam_own_drain_next(CAM_DRAIN_DONE, 0) == CAM_OWN_SETTLING,
	      "sink released but the worker still going is SETTLING (the -2 case)");

	/* [!] The pin outranks the worker.  The sink is what a later start would
	   re-attach, so a pin still outstanding has to be the thing remembered --
	   recorded as merely SETTLING, a retry that only watched the worker would
	   declare the teardown finished. */
	CHECK(cam_own_drain_next(CAM_DRAIN_PINNED, 1) == CAM_OWN_PENDING,
	      "a pin outstanding is PENDING even with the worker parked");
	CHECK(cam_own_drain_next(CAM_DRAIN_PINNED, 0) == CAM_OWN_PENDING,
	      "a pin outstanding is PENDING with the worker busy too");

	/* WAIT never reaches a commit (the poll loop only returns DONE or PINNED),
	   but if it ever did it must not be read as success. */
	CHECK(cam_own_drain_next(CAM_DRAIN_WAIT, 1) != CAM_OWN_IDLE,
	      "an unfinished drain must never commit IDLE");
}

/* The one non-transition read: the deferred GUIX start's backstop. */
static void test_start_claimed(void)
{
	CHECK(cam_own_start_is_claimed(CAM_OWN_STARTING),
	      "STARTING is the state a deferred start may finish");
	CHECK(!cam_own_start_is_claimed(CAM_OWN_IDLE),
	      "a handler with no claim must not subscribe anything");
	CHECK(!cam_own_start_is_claimed(CAM_OWN_RUNNING),
	      "an already-running owner is not an outstanding claim");
	CHECK(!cam_own_start_is_claimed(CAM_OWN_DRAINING),
	      "[!] a stale event landing inside a drain must find no claim");
	CHECK(!cam_own_start_is_claimed(CAM_OWN_PENDING),
	      "nor while the sink is still pinned");
	CHECK(!cam_own_start_is_claimed(CAM_OWN_SETTLING),
	      "nor while a worker is still winding down");
	CHECK(!cam_own_start_is_claimed(BOGUS_STATE),
	      "an unknown state is not a claim");
}

static void test_settled(void)
{
	CHECK(cam_own_settled(CAM_OWN_SETTLING) == CAM_OWN_IDLE,
	      "a worker parking is what clears SETTLING");

	/* [!] A worker parks asynchronously, so this can land in any state.  Only
	   SETTLING may move: a stop still inside its drain commits its own result,
	   and letting a worker overwrite that would clear a PENDING pin. */
	CHECK(cam_own_settled(CAM_OWN_DRAINING) == CAM_OWN_DRAINING,
	      "a worker parking mid-drain must not pre-empt the stop's commit");
	CHECK(cam_own_settled(CAM_OWN_PENDING) == CAM_OWN_PENDING,
	      "a worker parking must not clear a pinned sink");
	CHECK(cam_own_settled(CAM_OWN_RUNNING) == CAM_OWN_RUNNING,
	      "a worker parking must not stop a running owner");
	CHECK(cam_own_settled(CAM_OWN_IDLE) == CAM_OWN_IDLE,
	      "a worker parking after the stop committed is a no-op");
	CHECK(cam_own_settled(BOGUS_STATE) == BOGUS_STATE,
	      "an unknown state must not be settled into IDLE");
}

/* ---- linearised sequences, in the shape of the owner call sites ------------ */

/* The exact two lines every entry point runs inside its critical section. */
static enum cam_own_start sim_start_take(enum cam_own_state *st)
{
	enum cam_own_start act = cam_own_start_decide(*st);

	*st = cam_own_start_next(act, *st);
	return act;
}

static enum cam_own_stop sim_stop_take(enum cam_own_state *st)
{
	enum cam_own_stop act = cam_own_stop_decide(*st);

	*st = cam_own_stop_next(act, *st);
	return act;
}

/* A deferred start (the GUIX preview): claimed on one thread, finished on
   another, with a stop able to run in between. */
static void seq_deferred_start(void)
{
	enum cam_own_state st = CAM_OWN_IDLE;

	/* The ordinary path: claim, then the handler finds its own claim. */
	CHECK(sim_start_take(&st) == CAM_OWN_START_GO, "start claims");
	CHECK(cam_own_start_is_claimed(st), "the handler must see the claim");
	st = cam_own_start_done(st, 1);
	CHECK(st == CAM_OWN_RUNNING, "running, got %s", sname(st));

	/* [!] The hole this closes: an event posted with NO claim (a start that
	   found the owner already RUNNING) arriving after a stop has drained and
	   released.  The handler must find nothing to finish -- otherwise it
	   subscribes a sink while the lifecycle says IDLE, or lands inside the
	   drain, where re-attaching resets the pin count the stop is watching. */
	CHECK(sim_stop_take(&st) == CAM_OWN_STOP_DRAIN, "stop drains");
	CHECK(!cam_own_start_is_claimed(st), "no claim to finish mid-drain");
	st = cam_own_drain_next(CAM_DRAIN_DONE, 1);
	CHECK(!cam_own_start_is_claimed(st), "and none after it committed");
}

/* A stop whose drain spends its budget with the pin still held: PENDING, and
   nothing may start until a later stop proves the pin is back. */
static void seq_timeout_then_recover(void)
{
	enum cam_own_state st = CAM_OWN_IDLE;

	CHECK(sim_start_take(&st) == CAM_OWN_START_GO, "start claims");
	st = cam_own_start_done(st, 1);
	CHECK(st == CAM_OWN_RUNNING, "running, got %s", sname(st));

	CHECK(sim_stop_take(&st) == CAM_OWN_STOP_DRAIN, "stop drains");
	CHECK(st == CAM_OWN_DRAINING, "the drain interval is a state, got %s",
	      sname(st));
	/* A start arriving inside the drain -- the window the first attempt at #72
	   left wide open. */
	CHECK(sim_start_take(&st) == CAM_OWN_START_HELD, "start refused mid-drain");
	CHECK(st == CAM_OWN_DRAINING, "a refused start moved the state");

	st = cam_own_drain_next(CAM_DRAIN_PINNED, 1);
	CHECK(st == CAM_OWN_PENDING, "timeout is remembered, got %s", sname(st));
	CHECK(sim_start_take(&st) == CAM_OWN_START_HELD, "start refused while pinned");

	/* Retry, still pinned: back to PENDING, still refusing. */
	CHECK(sim_stop_take(&st) == CAM_OWN_STOP_RETRY, "PENDING retries");
	st = cam_own_drain_next(CAM_DRAIN_PINNED, 1);
	CHECK(st == CAM_OWN_PENDING, "a retry that still sees the pin stays PENDING");

	/* Retry after the callback finally put its slot back. */
	CHECK(sim_stop_take(&st) == CAM_OWN_STOP_RETRY, "PENDING retries again");
	st = cam_own_drain_next(CAM_DRAIN_DONE, 1);
	CHECK(st == CAM_OWN_IDLE, "a poll that sees zero pins frees it, got %s",
	      sname(st));
	CHECK(sim_start_take(&st) == CAM_OWN_START_GO, "and a start is allowed again");
}

/* NN's -2: the sink is released but the worker is still inside an inference, so
   it releases the nn session itself on its way out. */
static void seq_pin_safe_worker_busy(void)
{
	enum cam_own_state st = CAM_OWN_RUNNING;

	CHECK(sim_stop_take(&st) == CAM_OWN_STOP_DRAIN, "stop drains");
	st = cam_own_drain_next(CAM_DRAIN_DONE, 0);
	CHECK(st == CAM_OWN_SETTLING, "pin safe, worker busy -> SETTLING, got %s",
	      sname(st));

	/* [!] This is why SETTLING is a state and not just the owner's flags: a
	   start allowed in here could acquire a NEW nn session, which the worker's
	   own release -- still owed from the old stream -- would then free. */
	CHECK(sim_start_take(&st) == CAM_OWN_START_HELD,
	      "no start while the worker still owes a session release");

	st = cam_own_settled(st);          /* the worker parks and says so */
	CHECK(st == CAM_OWN_IDLE, "the worker's parking finishes it, got %s",
	      sname(st));
	CHECK(sim_start_take(&st) == CAM_OWN_START_GO, "and a start is allowed again");
}

/* The same, finished by a second stop instead of by the worker: the retry has to
   be able to complete a teardown it did not begin. */
static void seq_settling_finished_by_retry(void)
{
	enum cam_own_state st = CAM_OWN_SETTLING;

	CHECK(sim_stop_take(&st) == CAM_OWN_STOP_RETRY, "SETTLING retries");
	CHECK(st == CAM_OWN_DRAINING, "the retry owns the teardown, got %s",
	      sname(st));
	st = cam_own_drain_next(CAM_DRAIN_DONE, 1);
	CHECK(st == CAM_OWN_IDLE, "and finishes it, got %s", sname(st));
}

/* The worker parking WHILE a stop is still draining: its settle must not land,
   and the stop's own commit is what decides. */
static void seq_worker_parks_mid_drain(void)
{
	enum cam_own_state st = CAM_OWN_RUNNING;

	CHECK(sim_stop_take(&st) == CAM_OWN_STOP_DRAIN, "stop drains");
	st = cam_own_settled(st);          /* worker parks early */
	CHECK(st == CAM_OWN_DRAINING, "the settle must not land mid-drain");
	st = cam_own_drain_next(CAM_DRAIN_PINNED, 1);
	CHECK(st == CAM_OWN_PENDING,
	      "the stop's own commit decides -- the pin is not lost to the worker");
}

/* Two commands in flight at once, linearised both ways round. */
static void seq_start_stop_pairs(void)
{
	enum cam_own_state st = CAM_OWN_IDLE;

	/* start first: the stop is refused until the start has finished. */
	CHECK(sim_start_take(&st) == CAM_OWN_START_GO, "start claims");
	CHECK(sim_stop_take(&st) == CAM_OWN_STOP_HELD, "stop refused mid-start");
	CHECK(st == CAM_OWN_STARTING, "a refused stop moved the state");
	st = cam_own_start_done(st, 1);
	CHECK(sim_stop_take(&st) == CAM_OWN_STOP_DRAIN, "and then it may stop");
	st = cam_own_drain_next(CAM_DRAIN_DONE, 1);

	/* stop first: the start is refused for the whole teardown. */
	st = CAM_OWN_RUNNING;
	CHECK(sim_stop_take(&st) == CAM_OWN_STOP_DRAIN, "stop claims");
	CHECK(sim_start_take(&st) == CAM_OWN_START_HELD, "start refused mid-stop");
	st = cam_own_drain_next(CAM_DRAIN_DONE, 1);
	CHECK(sim_start_take(&st) == CAM_OWN_START_GO, "and then it may start");
}

int main(void)
{
	test_start_table();
	test_stop_table();
	test_refusal_does_not_move();
	test_start_done();
	test_drain_next();
	test_start_claimed();
	test_settled();
	seq_deferred_start();
	seq_timeout_then_recover();
	seq_pin_safe_worker_busy();
	seq_settling_finished_by_retry();
	seq_worker_parks_mid_drain();
	seq_start_stop_pairs();

	if (failures != 0) {
		printf("test_cam_own: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cam_own: ok\n");
	return 0;
}
