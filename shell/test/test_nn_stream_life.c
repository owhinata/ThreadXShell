/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the shared live-inference lifecycle (issue #99,
 * svc/nn_stream_life.c).
 *
 * WHY THIS EXISTS.  `nn stream start --frames n` waits on one console while
 * another console -- or, on the one-console board, a background job -- can stop
 * that stream and start a new one.  A waiter that then tore down "the stream"
 * would release a camera, an NPU and a bus guard belonging to whoever started
 * the successor.
 *
 * [!] AND THE GENERATION ALONE DOES NOT CLOSE THAT.  The first cut of issue #99
 * compared generations on two boards without CLAIMING the stop, which admitted
 * two callers for one stream; one finished stopping G, a third started G+1, and
 * the delayed second ran its teardown against G+1.  The adversarial review found
 * it, and `two_stops_then_successor()` below is that exact interleaving.
 *
 * [!] NONE OF IT CAN BE TYPED.  The window is between two statements of another
 * thread, and the board where it matters most has ONE console whose background
 * jobs run below the foreground shell under TX_NO_TIME_SLICE.  So the machine is
 * one pure module all three boards drive, and this file is the only thing that
 * exercises the orderings.
 *
 * [!] WHAT IT DOES NOT COVER.  This compiles nn_stream_life.c alone, so it says
 * nothing about whether a board actually wraps these calls in a critical
 * section, nor whether every exit from a board's stop settles the lifecycle.
 * What holds those down is that each board has one call site per transition and
 * they are three lines long -- not this file.
 */
#include <stdio.h>
#include <stdint.h>

#include "nn_stream_life.h"

static int fails;

static void ok(const char *what, int cond)
{
	if (!cond) {
		printf("  FAIL %s\n", what);
		fails++;
	} else {
		printf("  ok   %s\n", what);
	}
}

static const char *gname(enum nn_stream_gen_verdict v)
{
	switch (v) {
	case NN_STREAM_GEN_GO:    return "GO";
	case NN_STREAM_GEN_IDLE:  return "IDLE";
	case NN_STREAM_GEN_WRONG: return "WRONG";
	default:                  return "?";
	}
}

static void gcheck(const char *what, uint32_t cur, uint32_t req,
                   enum nn_stream_gen_verdict want)
{
	enum nn_stream_gen_verdict got = nn_stream_gen_check(cur, req);

	if (got != want) {
		printf("  FAIL %-52s cur=%lu req=%lu -> %s, wanted %s\n", what,
		       (unsigned long)cur, (unsigned long)req, gname(got),
		       gname(want));
		fails++;
	} else {
		printf("  ok   %-52s %s\n", what, gname(got));
	}
}

/* One full start, for the tests that need a running stream. */
static uint32_t run(struct nn_stream_life *l)
{
	(void)nn_stream_life_begin(l);
	return nn_stream_life_commit(l);
}

static const char *sname(enum nn_stream_start_claim c)
{
	switch (c) {
	case NN_STREAM_START_GO:      return "GO";
	case NN_STREAM_START_RUNNING: return "RUNNING";
	case NN_STREAM_START_BUSY:    return "BUSY";
	case NN_STREAM_START_DEAD:    return "DEAD";
	default:                      return "?";
	}
}

static void scheck(const char *what, struct nn_stream_life *l,
                   enum nn_stream_start_claim want)
{
	enum nn_stream_start_claim got = nn_stream_life_begin(l);

	if (got != want) {
		printf("  FAIL %-52s -> %s, wanted %s\n", what, sname(got),
		       sname(want));
		fails++;
	} else {
		printf("  ok   %-52s %s\n", what, sname(got));
	}
}

/* ...and one full stop.  It has to CLAIM before it settles, like a board does --
   writing this helper is what turned up that an earlier test here was settling
   without a claim and getting away with it. */
static void stop(struct nn_stream_life *l, uint32_t gen)
{
	(void)nn_stream_life_claim_stop(l, gen);
	nn_stream_life_finish(l);
}

/*
 * [!] THE INTERLEAVING THE ADVERSARIAL REVIEW FOUND.
 *
 *   1. waiter A and console B both ask to stop generation G;
 *   2. one of them wins and completes the teardown;
 *   3. somebody starts G+1;
 *   4. the loser finally runs -- and must NOT be holding a claim on G+1.
 *
 * The claim is what makes step 1 admit exactly one of them.
 */
static void two_stops_then_successor(void)
{
	struct nn_stream_life l = { 0, 0, 0, 0, 0 };
	uint32_t g, g2;

	printf("two stops racing, then a successor start\n");

	g = run(&l);
	ok("the first stop is admitted",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_GO);
	/* [!] The line the whole claim exists for.  Without it this returns GO and
	 * two callers each go on to run a teardown. */
	ok("the SECOND stop for the same generation is refused",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_BUSY);
	ok("...and an operator's `stop whatever is running` is refused too",
	   nn_stream_life_claim_stop(&l, NN_STREAM_GEN_ANY) == NN_STREAM_STOP_BUSY);
	scheck("...and a start mid-teardown is BUSY", &l, NN_STREAM_START_BUSY);

	nn_stream_life_finish(&l);   /* the first stop claimed it above */
	g2 = run(&l);
	ok("the successor got a different generation", g2 != g);
	ok("the loser's stop cannot touch the successor",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_WRONG_GEN);
	ok("the successor is still running after that refusal",
	   nn_stream_life_claim_stop(&l, g2) == NN_STREAM_STOP_GO);
}

/*
 * [!] THE SECOND THING THE ADVERSARIAL REVIEW FOUND, and it was introduced BY the
 * fix for the first.  Centralising the machine is only worth anything if the
 * machine owns start ADMISSION too: while the boards recorded the start after
 * calling their worker -- and ignored whether the claim had been granted -- a
 * commit could overwrite a stop that already owned the teardown, and even
 * resurrect a lifecycle latched LOST.
 */
static void a_start_cannot_stamp_on_a_teardown(void)
{
	struct nn_stream_life l = { 0, 0, 0, 0, 0 };
	uint32_t g;

	printf("a start cannot overwrite a teardown\n");

	g = run(&l);
	ok("a stop claims the teardown",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_GO);
	scheck("a fresh start while the teardown is owned is BUSY",
	       &l, NN_STREAM_START_BUSY);
	ok("...and so is a RE-ARM, which is the one that could reach here",
	   nn_stream_life_rearm(&l) == 0);
	/* [!] And a commit that was not admitted changes nothing, rather than
	 * forcing RUNNING over the stop that is in progress. */
	ok("a commit that was never admitted is refused",
	   nn_stream_life_commit(&l) == NN_STREAM_GEN_ANY);
	ok("the teardown still owns it",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_BUSY);

	nn_stream_life_poison(&l);
	ok("a commit cannot resurrect a lifecycle latched LOST",
	   nn_stream_life_commit(&l) == NN_STREAM_GEN_ANY);
	ok("...it is still DEAD afterwards",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_DEAD);
}

/* The re-arm this board really does: a start that succeeds over a live stream,
   keeping its worker and counters but taking a NEW generation. */
static void rearm_mints_a_new_generation(void)
{
	struct nn_stream_life l = { 0, 0, 0, 0, 0 };
	uint32_t g, g2;

	printf("re-arming a live stream\n");

	g = run(&l);
	ok("a re-arm is admitted from RUNNING", nn_stream_life_rearm(&l) == 1);
	g2 = nn_stream_life_commit(&l);
	ok("it minted a NEW generation", g2 != g && g2 != NN_STREAM_GEN_ANY);
	/* [!] Which is the point: a waiter from before the outage must lose its
	 * authority, or it can stop the re-armed stream it never started. */
	ok("the pre-outage waiter can no longer stop it",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_WRONG_GEN);
	ok("the re-armed generation can",
	   nn_stream_life_claim_stop(&l, g2) == NN_STREAM_STOP_GO);
	nn_stream_life_retry(&l);

	/* [!] A FAILED re-arm must go back to RUNNING, not to IDLE: the stream it
	 * was re-arming is still up, and reporting idle would make the next stop
	 * say "not running" about a live worker. */
	ok("a re-arm is admitted again", nn_stream_life_rearm(&l) == 1);
	nn_stream_life_abort(&l);
	ok("an aborted re-arm left the stream RUNNING, not idle",
	   nn_stream_life_claim_stop(&l, g2) == NN_STREAM_STOP_GO);
}

/*
 * [!] SETTLING IS THE CLAIMANT'S ALONE.  Round 3 noted that finish/retry/poison
 * did not check the phase, so the machine was still trusting adapter discipline
 * for half of its job -- the same criticism that turned it from three copies
 * into one module.  A settle from anywhere would let a start that had already
 * taken STARTING be dragged back to IDLE by a stale stop.
 */
static void only_the_claimant_may_settle(void)
{
	struct nn_stream_life l = { 0, 0, 0, 0, 0 };
	uint32_t g;

	printf("only the caller that claimed the teardown may settle it\n");

	g = run(&l);
	/* [!] AND IT SAYS SO, rather than refusing silently.  A wrapper with side
	 * effects of its own -- releasing a hardware claim, freezing a clock -- must
	 * be able to skip them, or the guard fails open on exactly the invariant
	 * failure it was added to catch. */
	ok("a finish without a claim reports that it did nothing",
	   nn_stream_life_finish(&l) == 0);
	ok("...and really did nothing",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_GO);
	ok("the claimant's retry reports success",
	   nn_stream_life_retry(&l) == 1);

	ok("a poison without a claim reports that it did nothing",
	   nn_stream_life_poison(&l) == 0);
	ok("...and really did nothing",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_GO);
	ok("the claimant's finish reports success",
	   nn_stream_life_finish(&l) == 1);

	(void)nn_stream_life_begin(&l);
	ok("a retry cannot drag a start in progress into RUNNING",
	   nn_stream_life_retry(&l) == 0);
	ok("...so the start can still commit",
	   nn_stream_life_commit(&l) != NN_STREAM_GEN_ANY);
	ok("an abort outside a start reports that it did nothing",
	   nn_stream_life_abort(&l) == 0);
}

static void retry_and_poison(void)
{
	struct nn_stream_life l = { 0, 0, 0, 0, 0 };
	uint32_t g;

	printf("retryable and terminal teardowns\n");

	g = run(&l);
	(void)nn_stream_life_claim_stop(&l, g);
	nn_stream_life_retry(&l);
	/* [!] Retryable must leave it STOPPABLE AGAIN, with the same generation.
	 * Parked in STOPPING, one moment of lock contention would wedge the stream
	 * for good -- the outcome the retryable answer exists to avoid. */
	ok("after a retryable teardown the same generation can stop again",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_GO);

	nn_stream_life_poison(&l);
	ok("after an unconfirmed teardown a stop reports DEAD",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_DEAD);
	/* [!] LOST is not "not running": a start here would rebuild hardware a live
	 * thread may still be inside. */
	/* [!] DEAD, not BUSY: only a reset clears it, and "retry" would be a lie. */
	scheck("...and a start from LOST is DEAD", &l, NN_STREAM_START_DEAD);
}

static void ended_is_not_replaced(void)
{
	struct nn_stream_life l = { 0, 0, 0, 0, 0 };
	uint32_t g;

	printf("a finished stream still answers for its own generation\n");

	g = run(&l);
	(void)nn_stream_life_claim_stop(&l, g);
	nn_stream_life_finish(&l);
	/* [!] The generation is KEPT after it ends, so a waiter can tell "mine
	 * finished" (IDLE) from "a successor is running" (WRONG_GEN).  Clearing it
	 * would collapse two opposite messages into one. */
	ok("its own stop now reports IDLE, not WRONG_GEN",
	   nn_stream_life_claim_stop(&l, g) == NN_STREAM_STOP_IDLE);
	{
		uint32_t got = 0u;

		nn_stream_life_snapshot(&l, &got, NULL, NULL);
		ok("the generation survived the finish", got == g);
	}
}

static void seq_moves_on_every_transition(void)
{
	struct nn_stream_life l = { 0, 0, 0, 0, 0 };
	uint32_t a, b, g;

	printf("the transition counter\n");

	g = run(&l);
	nn_stream_life_snapshot(&l, NULL, NULL, &a);
	(void)nn_stream_life_claim_stop(&l, g);
	nn_stream_life_retry(&l);
	nn_stream_life_snapshot(&l, NULL, NULL, &b);
	/*
	 * [!] THE ABA THE COUNTER EXISTS FOR.  A retryable stop returns to RUNNING
	 * with the same generation, so a reader sampling phase and generation
	 * either side of a whole failed teardown finds them unchanged.  Only this
	 * moving makes such a read a seqlock.
	 */
	ok("a whole failed teardown moves the counter", b != a);
}

static void wrap_skips_the_reserved_value(void)
{
	struct nn_stream_life l = { 0, 0, 0, 0, 0 };
	uint32_t g;

	printf("generation wrap\n");

	l.next = 0xFFFFFFFFu;
	g = run(&l);
	ok("the last generation before wrap is ordinary", g == 0xFFFFFFFFu);
	stop(&l, g);
	g = run(&l);
	/* [!] Handing out the reserved value would give a waiter the operator's
	 * "stop whatever is running" authority -- exactly what is withheld. */
	ok("the generation after wrap is not the reserved value",
	   g != NN_STREAM_GEN_ANY);
}

static void disposition_fails_closed(void)
{
	/* A board's table, shaped like the real ones. */
	static const struct nn_stream_disp tab[] = {
		{  0, NN_STREAM_CLAIM_NONE      },
		{ -1, NN_STREAM_CLAIM_NONE      },
		{ -7, NN_STREAM_CLAIM_RETRYABLE },
	};
	const unsigned n = (unsigned)(sizeof tab / sizeof tab[0]);

	printf("stop disposition\n");

	ok("a documented success maps to NONE",
	   nn_stream_disp_of(0, tab, n) == NN_STREAM_CLAIM_NONE);
	ok("a documented in-progress code maps to RETRYABLE",
	   nn_stream_disp_of(-7, tab, n) == NN_STREAM_CLAIM_RETRYABLE);
	/*
	 * [!] THE LINE THIS FUNCTION EXISTS FOR.  Two boards used to end their
	 * mapping with a catch-all RETRYABLE, so any new or corrupted return told
	 * the operator to keep retrying while resources might still be live.  A code
	 * carrying no evidence is not evidence.
	 */
	ok("an UNDOCUMENTED code is TERMINAL, not retryable",
	   nn_stream_disp_of(-99, tab, n) == NN_STREAM_CLAIM_TERMINAL);
	ok("...and so is one just past the table",
	   nn_stream_disp_of(-8, tab, n) == NN_STREAM_CLAIM_TERMINAL);
	ok("an empty table refuses everything",
	   nn_stream_disp_of(0, tab, 0u) == NN_STREAM_CLAIM_TERMINAL);
	ok("a null table refuses everything",
	   nn_stream_disp_of(0, NULL, n) == NN_STREAM_CLAIM_TERMINAL);
}

int main(void)
{
	printf("nn_stream_gen_check (issue #99)\n");
	gcheck("idle, operator asks for whatever is running",
	       NN_STREAM_GEN_ANY, NN_STREAM_GEN_ANY, NN_STREAM_GEN_IDLE);
	gcheck("idle, a stale waiter asks for its own generation",
	       NN_STREAM_GEN_ANY, 7u, NN_STREAM_GEN_IDLE);
	gcheck("running, operator asks for whatever is running",
	       7u, NN_STREAM_GEN_ANY, NN_STREAM_GEN_GO);
	gcheck("running, the waiter that started it", 7u, 7u, NN_STREAM_GEN_GO);
	gcheck("running, a stale waiter from the PREVIOUS generation",
	       8u, 7u, NN_STREAM_GEN_WRONG);
	gcheck("running, a waiter from a LATER generation (refuse)",
	       7u, 8u, NN_STREAM_GEN_WRONG);
	gcheck("generation UINT32_MAX vs a stale 1",
	       0xFFFFFFFFu, 1u, NN_STREAM_GEN_WRONG);

	printf("start / commit / abort\n");
	{
		struct nn_stream_life l = { 0, 0, 0, 0, 0 };
		uint32_t g;

		scheck("a start is admitted from idle", &l, NN_STREAM_START_GO);
		/* [!] THE THREE REFUSALS ARE TOLD APART.  Folded into one, an operator
		 * starting a second stream is told "a start or a stop is already in
		 * progress" -- which was the first thing the f746 bench run hit. */
		scheck("a second start mid-start is BUSY, not RUNNING",
		       &l, NN_STREAM_START_BUSY);
		ok("a stop mid-start is refused",
		   nn_stream_life_claim_stop(&l, NN_STREAM_GEN_ANY) ==
		   NN_STREAM_STOP_BUSY);
		nn_stream_life_abort(&l);
		nn_stream_life_snapshot(&l, &g, NULL, NULL);
		ok("an aborted start minted nothing", g == NN_STREAM_GEN_ANY);
		ok("...and a stop then reports idle",
		   nn_stream_life_claim_stop(&l, NN_STREAM_GEN_ANY) ==
		   NN_STREAM_STOP_IDLE);
		scheck("...and a fresh start is admitted", &l, NN_STREAM_START_GO);
		g = nn_stream_life_commit(&l);
		ok("the first generation is not the reserved value",
		   g != NN_STREAM_GEN_ANY);
		/* [!] RUNNING, not BUSY.  Starting a second stream is an ordinary
		 * mistake and gets its own sentence; the f746 bench run was told "a
		 * start or a stop is already in progress" for it. */
		scheck("a start over a running stream is RUNNING", &l,
		       NN_STREAM_START_RUNNING);
	}

	two_stops_then_successor();
	a_start_cannot_stamp_on_a_teardown();
	rearm_mints_a_new_generation();
	only_the_claimant_may_settle();
	retry_and_poison();
	ended_is_not_replaced();
	seq_moves_on_every_transition();
	wrap_skips_the_reserved_value();
	disposition_fails_closed();

	if (fails) {
		printf("FAILED (%d)\n", fails);
		return 1;
	}
	printf("test_nn_stream_life: all cases pass\n");
	return 0;
}
