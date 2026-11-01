/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the CALL ORDERS around one `nn run` (issue #171): the real
 * svc/nn/nn_shot.c, nn_core_frame.c, nn_handoff.c and nn_run_wait.c, driven by
 * a stand-in board that makes the calls f746g-disco and wio-lite-ai make, in
 * their order:
 *
 *   producer  is the run open -> nn_core_on_frame() -> a refused prep ->
 *             record PREP
 *   worker    take -> invoke failed / lease not had -> record -> DONE_LAST ->
 *             re-arm only if the run is still open and the board still runs;
 *             or nn_core_on_infer_done() (DONE_LAST) -> nn_shot_after_job() ->
 *             the same re-arm question
 *
 * WHERE EACH BOARD SAMPLES THE GENERATION, and so where this stand-in does:
 *   f746g-disco  in the prep's first hold of nncam_lock (prep() below); the
 *                worker checks that frame's value (nncam_job_gen, b.job_gen)
 *   wio-lite-ai  the producer at band 0, in the hold that asks "open?"
 *                (b.p_gen); the worker at its arm, in the hold that asks
 *                "open?" (b.w_gen), and it re-arms at its NEXT step's arm
 *   f746g-disco re-arms right after the record (the run still open and
 *   nncam_run still up); wio-lite-ai's record does not decide the re-arm.
 *   console   failed, then published, every poll; lost and the cancel only
 *             when neither is true
 *
 * WHY THIS EXISTS.  The table test (test_nn_shot.c) holds every answer; it
 * cannot show that a closed run takes no second frame, that a boundary during
 * the inference re-arms instead of ending the run, or that the stop's JOIN
 * still finds the word where it can end it.  None of those can be typed on
 * hardware either.  The board's own wrappers (its locks, its threads) cannot be
 * brought to the host, so the WHERE of each lock is the diff review's; the
 * decisions are here, through the same functions the board calls.
 */
#include <stdio.h>
#include <string.h>

#include "nn_core_frame.h"
#include "nn_run_wait.h"
#include "nn_shot.h"

static int fails;

#define CHECK(cond, what)                                                   \
	do {                                                                \
		if (cond) {                                                 \
			printf("  ok   %s\n", what);                        \
		} else {                                                    \
			printf("  FAIL %s  (%s:%d)\n", what, __FILE__, __LINE__); \
			fails++;                                            \
		}                                                           \
	} while (0)

/* ---- the stand-in board ----------------------------------------------------- */

static struct nn_core_frame fr;
static struct nn_shot shot;

static struct {
	uint32_t gen;        /* the record's generation                         */
	uint32_t accepted;   /* the record's accepted count (never goes back)   */
	int      running;    /* the board's run flag (f746 nncam_run)           */
	/* producer */
	uint32_t p_run;          /* sampled at the first part, under the lock  */
	uint32_t p_gen;          /* the frame's generation: wio at the first
	                          * part, f746 in the prep's first hold         */
	uint32_t job_gen;        /* f746: the frame's, read by the worker (the
	                          * board's nncam_job_gen)                      */
	int      prep_fail, prep_calls, prep_records;
	int      f746_shape;     /* the prep records PREP inside itself (f746)  */
	/* worker */
	uint32_t w_run;          /* the run the worker serves                   */
	uint32_t w_gen;          /* wio: sampled at the arm, with "open?"       */
	int      acc_what, acc_took, acc_calls;
	int      infer_starts;
	/* console */
	int      cancel_asks;
} b;

static unsigned cs_enter(void) { return 0u; }
static void cs_exit(unsigned s) { (void)s; }

static int prep(void *ctx, unsigned part, unsigned nparts)
{
	(void)ctx;
	(void)part;
	(void)nparts;
	b.prep_calls++;
	if (b.f746_shape) {
		/* f746g-disco: the prep's FIRST hold samples the frame's
		 * generation (and the board's nncam_job_gen). */
		b.p_gen = b.gen;
		b.job_gen = b.gen;
	}
	if (!b.prep_fail)
		return 0;
	if (b.f746_shape) {
		/* f746g-disco: inside the prep's last hold of its lock, before
		 * the ABANDON that nn_core_on_frame() does on the -1. */
		(void)nn_shot_record(&shot, b.p_run, NN_SHOT_PREP,
		                     b.p_gen == b.gen);
		b.prep_records++;
	}
	return -1;
}

static void infer_start(void *ctx) { (void)ctx; b.infer_starts++; }
static int  is_plugin(void *ctx)   { (void)ctx; return 0; }
static int  admits(void *ctx, uint32_t gen) { (void)ctx; return gen == b.gen; }

static int publish_raw(void *ctx, uint32_t gen)
{
	(void)ctx;
	if (gen != b.gen)
		return 0;   /* the generation rule: not taken */
	b.accepted++;
	return 1;
}

static int  decode(void *ctx, int *n) { (void)ctx; *n = 0; return 0; }
static int  publish(void *ctx, int n, uint32_t gen)
{
	(void)n;
	return publish_raw(ctx, gen);
}

static void account(void *ctx, enum nn_core_done what, int n, int took)
{
	(void)ctx;
	(void)n;
	/* The worker-only variables the board's account leaves for the record
	 * after nn_core_on_infer_done() returns. */
	b.acc_what = (int)what;
	b.acc_took = took;
	b.acc_calls++;
}

static const struct nn_core_frame_ops ops = {
	.cs_enter = cs_enter, .cs_exit = cs_exit,
	.prep = prep, .infer_start = infer_start,
	.is_plugin = is_plugin, .admits = admits, .publish_raw = publish_raw,
	.decode = decode, .publish = publish, .account = account,
};

static void reset(int f746_shape)
{
	memset(&fr, 0, sizeof fr);
	memset(&shot, 0, sizeof shot);
	memset(&b, 0, sizeof b);
	b.gen = 1u;
	b.f746_shape = f746_shape;
}

/*
 * wio-lite-ai's arm, at the top of every worker step (nncam_oneshot_arm()):
 * "is the run open?" and the generation in ONE hold, and a want only on an
 * open run.  The step runs only while the board runs.
 */
static int worker_arm_wio(void)
{
	uint32_t run;

	if (!b.running)
		return 0;
	run = nn_shot_open(&shot);
	if (run == 0u)
		return 0;
	b.w_run = run;
	b.w_gen = b.gen;
	return nn_core_frame_want(&fr, &ops);
}

/* The start: begin the run (record lock), then the worker's first arm --
 * f746g-disco's at its run-loop entry, wio-lite-ai's at its first step. */
static uint32_t start_run(void)
{
	uint32_t run = nn_shot_begin(&shot);

	b.running = 1;
	if (b.f746_shape) {
		b.w_run = nn_shot_open(&shot);
		if (b.w_run != 0u)
			(void)nn_core_frame_want(&fr, &ops);
	} else {
		(void)worker_arm_wio();
	}
	return run;
}

/* The generation the worker checks its job against. */
static uint32_t worker_gen(void)
{
	return b.f746_shape ? b.job_gen : b.w_gen;
}

/* A boundary in the record: a detach / attach / stop moved the generation. */
static void boundary(void) { b.gen++; }

/*
 * One part on the producer.  At part 0 it asks whether the run is open BEFORE
 * BEGIN, and samples the generation for the frame in the same hold; a closed
 * run's part 0 never reaches nn_core_on_frame().  wio-lite-ai records a refused
 * prep after the ABANDON; f746g-disco inside its prep (above).
 * @return the frame path's answer, or -1 for a part 0 kept out.
 */
static int producer_part(unsigned part, unsigned nparts)
{
	enum nn_core_on_frame r;

	if (part == 0u) {
		uint32_t run = nn_shot_open(&shot);

		if (run == 0u)
			return -1;
		b.p_run = run;
		if (!b.f746_shape)
			b.p_gen = b.gen;   /* wio: same hold; f746: in the prep */
	}
	r = nn_core_on_frame(&fr, &ops, NULL, part, nparts);
	if (r == NN_CORE_FR_ABANDONED && !b.f746_shape) {
		(void)nn_shot_record(&shot, b.p_run, NN_SHOT_PREP,
		                     b.p_gen == b.gen);
		b.prep_records++;
	}
	return (int)r;
}

static void producer_frame(unsigned nparts)
{
	unsigned p;

	for (p = 0u; p < nparts; p++)
		(void)producer_part(p, nparts);
}

/*
 * The worker's re-arm after a job.  f746g-disco: right after the record, on
 * the record's answer AND the run flag read after it.  wio-lite-ai: the
 * record's answer is not used; the NEXT step's arm asks "open?" itself and
 * samples its generation in the same hold (worker_arm_wio()).
 */
static int worker_rearm(int open)
{
	if (!b.f746_shape)
		return worker_arm_wio();
	if (open && b.running)
		return nn_core_frame_want(&fr, &ops);
	return 0;
}

enum job { JOB_OK, JOB_INVOKE_FAIL, JOB_LEASE_TIMEOUT };

/*
 * One job on the worker.  @p boundary_at: 1 a boundary during the inference
 * (before the record), 2 a boundary after the publish and before the record.
 * @return the re-arm answer (non-zero armed), -1 if nothing to take.
 */
static int worker_job(enum job j, int boundary_at)
{
	uint32_t gen = worker_gen();
	int open;

	if (!nn_core_frame_take(&fr, &ops))
		return -1;
	if (boundary_at == 1)
		boundary();
	if (j != JOB_OK) {
		open = nn_shot_record(&shot, b.w_run,
		                      j == JOB_INVOKE_FAIL ? NN_SHOT_INVOKE
		                                           : NN_SHOT_LEASE_TIMEOUT,
		                      gen == b.gen);
		nn_core_frame_done(&fr, &ops, 0);   /* a one-shot: DONE_LAST */
		return worker_rearm(open);
	}
	nn_core_on_infer_done(&fr, &ops, NULL, gen, 0);
	if (boundary_at == 2)
		boundary();
	open = nn_shot_after_job(&shot, b.w_run, (enum nn_core_done)b.acc_what,
	                         b.acc_took, gen == b.gen);
	return worker_rearm(open);
}

/* The stop: the run flag down, a boundary, then the JOIN. */
static int stop_join(void)
{
	b.running = 0;
	boundary();
	return nn_core_frame_join(&fr, &ops);
}

/* One poll of the console: failed, then published, both every time; lost and
 * the cancel only when neither is true. */
static enum nn_run_end poll_once(uint32_t base, int lost, int expired)
{
	enum nn_shot_reason why = NN_SHOT_NONE;
	int fin = nn_shot_failed(&shot, &why);
	int pub = (b.accepted != base);
	int can = 0;

	if (!pub && !fin && !lost) {
		b.cancel_asks++;
		can = 0;
	}
	return nn_run_wait_step(pub, fin, lost, can, expired);
}

/* ---- the sequences ---------------------------------------------------------- */

static void test_prep_failed_then_no_begin(void)
{
	uint32_t base;
	int shape;

	for (shape = 1; shape >= 0; shape--) {
		const unsigned np = shape ? 1u : 4u;

		printf("%s: a refused prep, then one more frame\n",
		       shape ? "f746g-disco" : "wio-lite-ai");
		reset(shape);
		(void)start_run();
		base = b.accepted;
		b.prep_fail = 1;
		producer_frame(np);
		CHECK(b.prep_calls == 1 && b.prep_records == 1,
		      "the prep ran once and its refusal was recorded");
		CHECK(fr.word == (uint8_t)NN_HO_WANT,
		      "FILLING left only by the ABANDON");
		CHECK(nn_shot_open(&shot) == 0u, "the run is closed");
		b.prep_fail = 0;
		producer_frame(np);
		CHECK(b.prep_calls == 1 && fr.word == (uint8_t)NN_HO_WANT &&
		      b.infer_starts == 0,
		      "[!] the next frame is not begun: nothing written, nothing "
		      "handed");
		CHECK(poll_once(base, 0, 0) == NN_RUN_NO_RESULT &&
		      b.cancel_asks == 0, "the wait says NO_RESULT, no cancel asked");
		CHECK(nn_shot_says(shot.reason)->status != 0,
		      "the reason has a status");
		CHECK(stop_join() && fr.word == (uint8_t)NN_HO_IDLE,
		      "the stop's JOIN ends WANT");
	}
}

static void test_boundary_during_job(enum job j, const char *what)
{
	uint32_t base;
	int armed;

	printf("a boundary during %s: re-arm, the next frame is INFERRED\n",
	       what);
	reset(1);
	(void)start_run();
	base = b.accepted;
	producer_frame(1u);
	armed = worker_job(j, 1);
	CHECK(armed == 1 && fr.word == (uint8_t)NN_HO_WANT,
	      "not recorded (generation moved): the worker re-armed");
	CHECK(nn_shot_open(&shot) != 0u && !nn_shot_failed(&shot, NULL),
	      "the run is still open and has not failed");
	CHECK(poll_once(base, 0, 0) == NN_RUN_WAITING, "the wait keeps waiting");
	producer_frame(1u);
	armed = worker_job(JOB_OK, 0);
	CHECK(armed == 0 && fr.word == (uint8_t)NN_HO_IDLE,
	      "published: no re-arm, the word parked");
	CHECK(shot.reason == (uint8_t)NN_SHOT_PUBLISHED, "ended as PUBLISHED");
	CHECK(poll_once(base, 0, 0) == NN_RUN_INFERRED, "the wait says INFERRED");
}

static void test_boundary_after_record(void)
{
	uint32_t base;

	printf("a boundary after the record: the ending stands\n");
	reset(1);
	(void)start_run();
	base = b.accepted;
	producer_frame(1u);
	CHECK(worker_job(JOB_INVOKE_FAIL, 0) == 0, "recorded: no re-arm");
	boundary();
	{
		enum nn_shot_reason why = NN_SHOT_NONE;

		CHECK(nn_shot_failed(&shot, &why) && why == NN_SHOT_INVOKE,
		      "still failed with INVOKE after the boundary");
	}
	CHECK(poll_once(base, 0, 1) == NN_RUN_NO_RESULT,
	      "NO_RESULT even with the deadline passed in the same poll");
	CHECK(stop_join() && fr.word == (uint8_t)NN_HO_IDLE, "the stop joins");
}

static void test_publish_then_detach(void)
{
	uint32_t base;

	printf("a publish the record took, then a detach before the record\n");
	reset(1);
	(void)start_run();
	base = b.accepted;
	producer_frame(1u);
	CHECK(worker_job(JOB_OK, 2) == 0, "no re-arm");
	CHECK(shot.reason == (uint8_t)NN_SHOT_PUBLISHED &&
	      !nn_shot_failed(&shot, NULL),
	      "[!] PUBLISHED whatever the generation, and not a failure");
	CHECK(poll_once(base, 1, 1) == NN_RUN_INFERRED && b.cancel_asks == 0,
	      "the wait says INFERRED (lost and deadline too) and never asks "
	      "the cancel");
}

static void test_publish_not_taken(void)
{
	uint32_t base;

	printf("a publish the record did not take: re-arm\n");
	reset(1);
	(void)start_run();
	base = b.accepted;
	producer_frame(1u);
	/* The generation moved before the publish (during the inference): the
	 * record refuses it, the job is RETIRED or not taken. */
	CHECK(worker_job(JOB_OK, 1) == 1 && fr.word == (uint8_t)NN_HO_WANT,
	      "re-armed");
	CHECK(b.accepted == base && nn_shot_open(&shot) != 0u,
	      "nothing accepted, the run still open");
	producer_frame(1u);
	CHECK(b.prep_calls == 2, "the next frame is taken for the same run");
}

static void test_stop_between_record_and_rearm(void)
{
	printf("a stop between the record and the re-arm\n");
	reset(1);
	(void)start_run();
	producer_frame(1u);
	CHECK(nn_core_frame_take(&fr, &ops), "the worker took the job");
	{
		uint32_t gen = worker_gen();
		int open;

		boundary();   /* the generation moved during the inference */
		open = nn_shot_record(&shot, b.w_run, NN_SHOT_INVOKE,
		                      gen == b.gen);
		nn_core_frame_done(&fr, &ops, 0);
		CHECK(open, "not recorded: still open");
		b.running = 0;   /* the stop lands here */
		CHECK(worker_rearm(open) == 0 && fr.word == (uint8_t)NN_HO_IDLE,
		      "the run flag read after the record: no re-arm");
		CHECK(nn_core_frame_join(&fr, &ops) &&
		      fr.word == (uint8_t)NN_HO_IDLE, "the stop joins IDLE");
	}

	/* The other order: the worker re-armed before the stop's flag. */
	reset(1);
	(void)start_run();
	producer_frame(1u);
	CHECK(worker_job(JOB_INVOKE_FAIL, 1) == 1 &&
	      fr.word == (uint8_t)NN_HO_WANT, "re-armed before the stop");
	CHECK(stop_join() && fr.word == (uint8_t)NN_HO_IDLE,
	      "the stop's JOIN takes WANT back to IDLE");
}

static void test_stop_after_ending(void)
{
	printf("the stop after the run ended\n");
	reset(1);
	(void)start_run();
	producer_frame(1u);
	CHECK(worker_job(JOB_LEASE_TIMEOUT, 0) == 0 &&
	      fr.word == (uint8_t)NN_HO_IDLE, "ended, parked");
	CHECK(stop_join() && fr.word == (uint8_t)NN_HO_IDLE, "the JOIN is taken");
	CHECK(nn_shot_says(shot.reason)->status ==
	      nn_shot_says(NN_SHOT_LEASE_TIMEOUT)->status,
	      "the reason is still LEASE_TIMEOUT after the stop");
	/* The next start: a new number, nothing carried over. */
	(void)start_run();
	CHECK(!nn_shot_failed(&shot, NULL) && nn_shot_open(&shot) != 0u &&
	      fr.word == (uint8_t)NN_HO_WANT, "the next start begins clean");
}

static void test_failed_and_published(void)
{
	uint32_t base;

	printf("failed and published both true in one poll\n");
	reset(1);
	(void)start_run();
	base = b.accepted;
	producer_frame(1u);
	(void)worker_job(JOB_INVOKE_FAIL, 0);
	b.accepted++;   /* a publish the console sees in the same poll */
	CHECK(poll_once(base, 0, 1) == NN_RUN_INFERRED && b.cancel_asks == 0,
	      "INFERRED, and the cancel is not asked");
}

static void test_wio_wait_ran_out_filling(void)
{
	uint32_t prun, pgen;

	printf("wio-lite-ai: the worker's wait ran out with FILLING\n");
	reset(0);
	(void)start_run();
	(void)producer_part(0u, 4u);
	(void)producer_part(1u, 4u);
	CHECK(fr.word == (uint8_t)NN_HO_FILLING, "two bands written, FILLING");
	prun = b.p_run;
	pgen = b.p_gen;
	/* The next step of the worker: samples its own generation and tries to
	 * arm -- refused, the producer is filling. */
	CHECK(!worker_arm_wio() && nn_shot_open(&shot) != 0u &&
	      fr.word == (uint8_t)NN_HO_FILLING,
	      "[!] the arm is refused and FILLING is not taken away");
	CHECK(b.p_run == prun && b.p_gen == pgen,
	      "the frame's sampled run and generation are unchanged");
	(void)producer_part(2u, 4u);
	(void)producer_part(3u, 4u);
	CHECK(fr.word == (uint8_t)NN_HO_HANDED && b.infer_starts == 1,
	      "the frame is finished and handed");
	CHECK(worker_job(JOB_OK, 0) == 0 &&
	      shot.reason == (uint8_t)NN_SHOT_PUBLISHED, "and published");
}

static void test_wio_cut_fill_rewritten(void)
{
	uint32_t g0;

	printf("wio-lite-ai: a fill cut part way, then the next band 0\n");
	reset(0);
	(void)start_run();
	(void)producer_part(0u, 4u);
	(void)producer_part(1u, 4u);
	g0 = b.p_gen;
	/* Bands 2 and 3 never came.  Something moved the generation between
	 * the two frames, so the sample can be told apart. */
	boundary();
	CHECK(producer_part(0u, 4u) == (int)NN_CORE_FR_WROTE &&
	      fr.word == (uint8_t)NN_HO_FILLING,
	      "band 0 written again over the cut fill");
	CHECK(b.p_gen == b.gen && b.p_gen != g0,
	      "[!] the sample is the new band 0's");
	b.prep_fail = 1;
	(void)producer_part(1u, 4u);
	/* Checked against the earlier frame's sample (g0) this refusal would
	 * find the generation moved and leave the run open. */
	CHECK(fr.word == (uint8_t)NN_HO_WANT && shot.ended == shot.run &&
	      shot.reason == (uint8_t)NN_SHOT_PREP,
	      "a refusal in that frame is checked against the new sample: PREP");
}

int main(void)
{
	test_prep_failed_then_no_begin();
	test_boundary_during_job(JOB_INVOKE_FAIL, "the inference");
	test_boundary_during_job(JOB_LEASE_TIMEOUT, "the lease wait");
	test_boundary_after_record();
	test_publish_then_detach();
	test_publish_not_taken();
	test_stop_between_record_and_rearm();
	test_stop_after_ending();
	test_failed_and_published();
	test_wio_wait_ran_out_filling();
	test_wio_cut_fill_rewritten();
	if (fails) {
		printf("test_nn_shot_seq: %d FAILED\n", fails);
		return 1;
	}
	printf("test_nn_shot_seq: all passed\n");
	return 0;
}
