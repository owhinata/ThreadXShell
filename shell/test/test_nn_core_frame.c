/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the shared frame path (issue #130, svc/nn_core_frame.c): the
 * producer's parts, the worker's end of a job and the panel's draw, driven
 * through stand-in hooks.
 *
 * WHY THIS EXISTS.  Every sequence that matters here is a window between three
 * threads -- a band that arrives after its frame was abandoned, a stop that
 * joins while a frame is half written, a wake-up whose frame was already taken,
 * a panel that wins the lease over a record from the last session -- and none
 * can be typed at a console.  Both shapes a board hands frames in are walked: a
 * frame in four parts (wio-lite-ai's bands) and in one (grove-vision-ai-v2).
 *
 * [!] THE STAND-INS WATCH THE ORDER, NOT ONLY THE ANSWER.  The input written
 * outside FILLING, a hook called inside the critical section (a mutex wait
 * with interrupts off is a deadlock on hardware), the lease kept until after
 * the board counted (it must be given back BEFORE), a paint without the lease
 * or before the frame lock -- none of these shows in a return value.
 *
 * [!] AND THEY RECORD THE STATE AT EVERY BOUNDARY, BECAUSE A WINDOW CANNOT BE
 * SEEN IN AN OUTCOME.  Moving the result's frame out of the lease, the DONE
 * ahead of the count, or the producer's read of the word out of its critical
 * section all leave every return value as it was.  So the lease give records
 * res_frame, the account records the word, and a critical section's exit can
 * play another thread's JOIN at that instant: what the code read inside the
 * section and what it would have read after it then differ.
 */
#include <stdio.h>
#include <string.h>

#include "nn_core_frame.h"

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

/* ---- the stand-in board -------------------------------------------------- */

static struct nn_core_frame fr;

static struct {
	int in_cs;          /* inside the critical section now        */
	int hook_in_cs;     /* a hook was called inside it             */
	int prep_calls, prep_fail, prep_parts[8];
	int prep_outside_filling;
	int prep_join;      /* the prep hook joins (a JOIN under a writer) */
	int join_at_exit;   /* the Nth cs_exit (1-based) plays a stop's JOIN */
	int exits;
	uint32_t give_res_frame;   /* res_frame when the lease was given back */
	int acc_word;              /* the word when the board counted          */
	int infer_starts;
	int plugin, admits, outputs_rc, decode_rc, decode_n, took;
	int raw_calls, pub_calls, admits_calls, outputs_calls, decode_calls;
	uint32_t pub_gen; int pub_n;
	int acc_calls, acc_what, acc_n, acc_took, acc_with_lease;
	int held, try_ok, try_calls, try_who, gives, unheld;
	int lock_calls, lock_before_lease;
	int may_draw, rec_ok;
	struct nn_det_snapshot rec;
	int paint_calls, paint_without_lease, paint_without_lock;
} b;

static void note_hook(void)
{
	if (b.in_cs)
		b.hook_in_cs++;
}

static unsigned cs_enter(void)  { b.in_cs++; return 7u; }
static void cs_exit(unsigned s)
{
	(void)s;
	b.in_cs--;
	/* Another thread runs the instant interrupts come back on. */
	if (++b.exits == b.join_at_exit)
		(void)nn_handoff_step(&fr.word, (uint8_t)NN_HO_OP_JOIN);
}

static int prep(void *ctx, unsigned part, unsigned nparts)
{
	(void)ctx;
	(void)nparts;
	note_hook();
	if (fr.word != (uint8_t)NN_HO_FILLING)
		b.prep_outside_filling++;
	if (b.prep_calls < 8)
		b.prep_parts[b.prep_calls] = (int)part;
	b.prep_calls++;
	if (b.prep_join)
		(void)nn_handoff_step(&fr.word, (uint8_t)NN_HO_OP_JOIN);
	return b.prep_fail ? -1 : 0;
}

static void infer_start(void *ctx) { (void)ctx; note_hook(); b.infer_starts++; }
static int  is_plugin(void *ctx)   { (void)ctx; note_hook(); return b.plugin; }

static int admits(void *ctx, uint32_t gen)
{
	(void)ctx;
	(void)gen;
	note_hook();
	b.admits_calls++;
	return b.admits;
}

static int publish_raw(void *ctx, uint32_t gen)
{
	(void)ctx;
	note_hook();
	b.raw_calls++;
	b.pub_gen = gen;
	return b.took;
}

static int outputs(void *ctx)
{
	(void)ctx;
	note_hook();
	b.outputs_calls++;
	return b.outputs_rc;
}

static int decode(void *ctx, int *n)
{
	(void)ctx;
	note_hook();
	b.decode_calls++;
	if (b.decode_rc != 0)
		return b.decode_rc;
	*n = b.decode_n;
	return 0;
}

static int publish(void *ctx, int n, uint32_t gen)
{
	(void)ctx;
	note_hook();
	b.pub_calls++;
	b.pub_n = n;
	b.pub_gen = gen;
	return b.took;
}

static void account(void *ctx, enum nn_core_done what, int n, int took)
{
	(void)ctx;
	note_hook();
	b.acc_calls++;
	b.acc_what = (int)what;
	b.acc_n = n;
	b.acc_took = took;
	b.acc_with_lease = b.held;
	b.acc_word = (int)fr.word;
}

static int lease_try(enum plugin_lease_who who)
{
	note_hook();
	b.try_calls++;
	b.try_who = (int)who;
	if (b.try_ok)
		b.held = 1;
	return b.try_ok;
}

static int  lease_held(void)        { note_hook(); return b.held; }
static void lease_give(void)
{
	note_hook();
	b.gives++;
	b.held = 0;
	b.give_res_frame = fr.res_frame;
}
static void lease_note_unheld(void) { note_hook(); b.unheld++; }

static const struct nn_core_frame_ops ops = {
	.cs_enter = cs_enter, .cs_exit = cs_exit,
	.present = NULL, .prep = prep, .infer_start = infer_start,
	.is_plugin = is_plugin, .admits = admits, .publish_raw = publish_raw,
	.outputs = outputs, .decode = decode, .publish = publish,
	.account = account,
	.lease_try = lease_try, .lease_held = lease_held,
	.lease_give = lease_give, .lease_note_unheld = lease_note_unheld,
};

/* A board with no plugin mechanism at all (wio-lite-ai's null backend). */
static const struct nn_core_frame_ops ops_bare = {
	.cs_enter = cs_enter, .cs_exit = cs_exit,
	.prep = prep, .infer_start = infer_start,
	.is_plugin = is_plugin, .publish_raw = publish_raw,
	.account = account,
};

static void frame_lock(void *ctx)
{
	(void)ctx;
	note_hook();
	b.lock_calls++;
	if (!b.held)
		b.lock_before_lease++;
}

static int may_draw(void *ctx) { (void)ctx; note_hook(); return b.may_draw; }

static int record(void *ctx, struct nn_det_snapshot *s)
{
	(void)ctx;
	note_hook();
	*s = b.rec;
	return b.rec_ok;
}

static void paint(void *ctx)
{
	(void)ctx;
	note_hook();
	b.paint_calls++;
	if (!b.held)
		b.paint_without_lease++;
	if (b.lock_calls == 0)
		b.paint_without_lock++;
}

static const struct nn_core_panel panel = {
	.frame_lock = frame_lock, .may_draw = may_draw,
	.record = record, .paint = paint,
};

static void reset(uint8_t word)
{
	memset(&b, 0, sizeof b);
	memset(&fr, 0, sizeof fr);
	fr.word = word;
	b.admits = 1;
	b.took = 1;
}

/* ---- the producer --------------------------------------------------------- */

static void test_bands(void)
{
	enum nn_core_on_frame r[4];
	unsigned i;

	printf("a frame in four parts (wio-lite-ai's bands)\n");

	/* [!] NOT FROM A PART PART WAY THROUGH. */
	reset(NN_HO_WANT);
	r[1] = nn_core_on_frame(&fr, &ops, NULL, 1u, 4u);
	r[2] = nn_core_on_frame(&fr, &ops, NULL, 2u, 4u);
	r[3] = nn_core_on_frame(&fr, &ops, NULL, 3u, 4u);
	CHECK(r[1] == NN_CORE_FR_NONE && r[2] == NN_CORE_FR_NONE,
	      "a fill does not begin at a middle part");
	CHECK(r[3] == NN_CORE_FR_SKIPPED, "...and the frame is skipped once, at its last part");
	CHECK(b.prep_calls == 0 && fr.word == (uint8_t)NN_HO_WANT,
	      "...with nothing written, and the worker still wanting");

	/* [!] HANDED ONLY AT THE LAST PART. */
	for (i = 0u; i < 3u; i++)
		r[i] = nn_core_on_frame(&fr, &ops, NULL, i, 4u);
	CHECK(r[0] == NN_CORE_FR_WROTE && r[1] == NN_CORE_FR_WROTE &&
	      r[2] == NN_CORE_FR_WROTE, "parts 0..2 are written");
	CHECK(fr.word == (uint8_t)NN_HO_FILLING && b.infer_starts == 0,
	      "...and the frame is still FILLING, the worker not woken");
	CHECK(nn_core_frame_take(&fr, &ops) == 0,
	      "the worker cannot take a frame still being written");
	r[3] = nn_core_on_frame(&fr, &ops, NULL, 3u, 4u);
	CHECK(r[3] == NN_CORE_FR_HANDED && fr.word == (uint8_t)NN_HO_HANDED &&
	      b.infer_starts == 1, "the last part hands over and wakes the worker once");
	CHECK(b.prep_calls == 4 && b.prep_parts[0] == 0 && b.prep_parts[3] == 3,
	      "...every part written, in order");
	CHECK(fr.job_frame == fr.frame_no && fr.frame_no == 1u,
	      "...and the job is numbered by the frame it began in (a frame seen "
	      "only from its middle is not counted)");

	/* While the job waits, and while it runs: nobody writes. */
	for (i = 0u; i < 4u; i++)
		r[i] = nn_core_on_frame(&fr, &ops, NULL, i, 4u);
	CHECK(r[3] == NN_CORE_FR_SKIPPED && b.prep_calls == 4,
	      "a frame while one is handed over is skipped, not written");
	CHECK(nn_core_frame_take(&fr, &ops) == 1 && fr.word == (uint8_t)NN_HO_RUNNING,
	      "the worker takes it");
	for (i = 0u; i < 4u; i++)
		r[i] = nn_core_on_frame(&fr, &ops, NULL, i, 4u);
	CHECK(r[0] == NN_CORE_FR_NONE && r[3] == NN_CORE_FR_SKIPPED &&
	      b.prep_calls == 4,
	      "[!] RUNNING: no part is written (the inference reuses the input)");
	CHECK(nn_core_frame_take(&fr, &ops) == 0 && fr.word == (uint8_t)NN_HO_RUNNING,
	      "a stale wake-up while it runs takes nothing");
	nn_core_frame_done(&fr, &ops, 1);
	CHECK(fr.word == (uint8_t)NN_HO_WANT, "DONE: the worker wants the next frame");
	CHECK(nn_core_frame_take(&fr, &ops) == 0 && fr.word == (uint8_t)NN_HO_WANT,
	      "[!] the wake-up that came with the job it already ran does not run "
	      "it again");
	CHECK(b.prep_outside_filling == 0 && b.hook_in_cs == 0,
	      "the input was written only while FILLING, and no hook ran in the "
	      "critical section");
}

static void test_one_part(void)
{
	printf("a frame in one part (grove-vision-ai-v2)\n");
	reset(NN_HO_WANT);
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 0u, 1u) == NN_CORE_FR_HANDED &&
	      fr.word == (uint8_t)NN_HO_HANDED && b.prep_calls == 1 &&
	      b.infer_starts == 1, "the one part begins, writes and hands over");
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 0u, 1u) == NN_CORE_FR_SKIPPED &&
	      b.prep_calls == 1, "the next, while it waits, is skipped (busy)");
	reset(NN_HO_WANT);
	b.prep_fail = 1;
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 0u, 1u) == NN_CORE_FR_ABANDONED &&
	      fr.word == (uint8_t)NN_HO_WANT && b.infer_starts == 0,
	      "a refused prep abandons: the worker still wants, nobody is woken");
	b.prep_fail = 0;
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 0u, 1u) == NN_CORE_FR_HANDED,
	      "...and the next frame tries again");
}

static void test_abandon_and_join(void)
{
	enum nn_core_on_frame r;

	printf("the two ways out of FILLING\n");

	/* ABANDON: the producer's own, and the stream goes on. */
	reset(NN_HO_WANT);
	(void)nn_core_on_frame(&fr, &ops, NULL, 0u, 4u);
	b.prep_fail = 1;
	r = nn_core_on_frame(&fr, &ops, NULL, 1u, 4u);
	b.prep_fail = 0;
	CHECK(r == NN_CORE_FR_ABANDONED && fr.word == (uint8_t)NN_HO_WANT,
	      "ABANDON at part 1: the frame is dropped, the worker still wants");
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 2u, 4u) == NN_CORE_FR_NONE &&
	      nn_core_on_frame(&fr, &ops, NULL, 3u, 4u) == NN_CORE_FR_SKIPPED &&
	      b.prep_calls == 2,
	      "...the rest of it is not written, and it is skipped once");
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 0u, 4u) == NN_CORE_FR_WROTE,
	      "...and the next frame begins again at its first part");
	CHECK(nn_core_frame_abandon(&fr, &ops) == 1 && fr.word == (uint8_t)NN_HO_WANT,
	      "the producer's own abandon (a part it cannot use) drops a fill");
	CHECK(nn_core_frame_abandon(&fr, &ops) == 0 && fr.word == (uint8_t)NN_HO_WANT,
	      "...and with no fill, changes nothing");

	reset(NN_HO_WANT);
	(void)nn_core_on_frame(&fr, &ops, NULL, 0u, 4u);
	(void)nn_core_on_frame(&fr, &ops, NULL, 1u, 4u);
	(void)nn_core_on_frame(&fr, &ops, NULL, 2u, 4u);
	b.prep_fail = 1;
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 3u, 4u) == NN_CORE_FR_ABANDONED &&
	      b.infer_starts == 0,
	      "an abandon at the last part is an abandon, not a skip");
	b.prep_fail = 0;

	/* JOIN: the stop's, once the producer is out, and the stream is over. */
	reset(NN_HO_WANT);
	(void)nn_core_on_frame(&fr, &ops, NULL, 0u, 4u);
	(void)nn_core_on_frame(&fr, &ops, NULL, 1u, 4u);
	CHECK(nn_core_frame_join(&fr, &ops) == 1 && fr.word == (uint8_t)NN_HO_IDLE,
	      "JOIN takes a half-written frame to IDLE");
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 2u, 4u) == NN_CORE_FR_NONE &&
	      nn_core_on_frame(&fr, &ops, NULL, 3u, 4u) == NN_CORE_FR_SKIPPED &&
	      nn_core_on_frame(&fr, &ops, NULL, 0u, 4u) == NN_CORE_FR_NONE &&
	      b.prep_calls == 2,
	      "[!] ...and unlike ABANDON, no later part or frame is written");

	reset(NN_HO_HANDED);
	CHECK(nn_core_frame_join(&fr, &ops) == 0 && fr.word == (uint8_t)NN_HO_HANDED,
	      "a JOIN waits for a frame handed over");
	reset(NN_HO_RUNNING);
	CHECK(nn_core_frame_join(&fr, &ops) == 0 && fr.word == (uint8_t)NN_HO_RUNNING,
	      "...and for one running");

	/* [!] THE WORKER'S WAIT RUNNING OUT TAKES NOTHING AWAY. */
	reset(NN_HO_WANT);
	(void)nn_core_on_frame(&fr, &ops, NULL, 0u, 4u);
	CHECK(nn_core_frame_want(&fr, &ops) == 0 &&
	      nn_core_frame_discard(&fr, &ops) == 0 &&
	      fr.word == (uint8_t)NN_HO_FILLING,
	      "a worker re-arming after its wait ran out leaves the fill alone");
	(void)nn_core_on_frame(&fr, &ops, NULL, 1u, 4u);
	(void)nn_core_on_frame(&fr, &ops, NULL, 2u, 4u);
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 3u, 4u) == NN_CORE_FR_HANDED,
	      "...and the frame completes and is handed over");

	/* [!] THE WORD IS READ INSIDE THE SECTION THAT BEGAN THE FILL.  A stop's
	 * JOIN landing the instant that section ends must find the producer
	 * already committed to writing -- and the write then reported RACED.
	 * Read after the section instead, the same JOIN would make the producer
	 * quietly skip, and nothing would ever count the JOIN under a writer. */
	reset(NN_HO_WANT);
	b.join_at_exit = 1;
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 0u, 1u) == NN_CORE_FR_RACED &&
	      b.prep_calls == 1 && b.infer_starts == 0,
	      "a JOIN right after the BEGIN's section: the producer had begun, "
	      "and its write is reported RACED");

	/* A JOIN under a producer still writing is an invariant broken. */
	reset(NN_HO_WANT);
	(void)nn_core_on_frame(&fr, &ops, NULL, 0u, 4u);
	b.prep_join = 1;
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 1u, 4u) == NN_CORE_FR_RACED,
	      "a fill taken away mid-part is reported, not passed as written");
	reset(NN_HO_WANT);
	b.prep_join = 1;
	CHECK(nn_core_on_frame(&fr, &ops, NULL, 0u, 1u) == NN_CORE_FR_RACED &&
	      b.infer_starts == 0 && fr.word == (uint8_t)NN_HO_IDLE,
	      "...and at the last part it hands nothing over and wakes nobody");
}

static void test_discard(void)
{
	printf("a frame handed over before the worker's arm\n");
	reset(NN_HO_HANDED);
	CHECK(nn_core_frame_discard(&fr, &ops) == 1 && fr.word == (uint8_t)NN_HO_IDLE,
	      "is dropped without running, and the worker is parked");
	CHECK(nn_core_frame_want(&fr, &ops) == 1 && fr.word == (uint8_t)NN_HO_WANT,
	      "...so the arm that follows wants a fresh one");
	CHECK(nn_core_frame_discard(&fr, &ops) == 0 && fr.word == (uint8_t)NN_HO_WANT,
	      "nothing handed over: nothing to drop");
}

/* ---- the worker ---------------------------------------------------------- */

static void run_job(int plugin, int held, int more)
{
	reset(NN_HO_RUNNING);
	fr.job_frame = 41u;
	fr.res_frame = 7u;
	b.plugin = plugin;
	b.held = held;
	nn_core_on_infer_done(&fr, &ops, NULL, 0x1234u, more);
}

static void test_infer_done(void)
{
	printf("the worker's end of a job\n");

	run_job(0, 0, 0);
	CHECK(b.raw_calls == 1 && b.pub_gen == 0x1234u && b.gives == 0 &&
	      b.acc_what == NN_CORE_DONE_RAW && b.acc_took == 1,
	      "[!] no plugin: published raw under the armed generation, no lease");
	CHECK(fr.word == (uint8_t)NN_HO_IDLE, "...and DONE_LAST parks the worker");
	run_job(0, 1, 1);
	CHECK(b.raw_calls == 1 && b.gives == 1 && !b.acc_with_lease &&
	      fr.word == (uint8_t)NN_HO_WANT,
	      "a lease the worker took anyway is given back; DONE wants more");

	run_job(1, 0, 1);
	CHECK(b.acc_what == NN_CORE_DONE_NOT_HELD && b.unheld == 1 &&
	      b.decode_calls == 0 && b.pub_calls == 0 && b.raw_calls == 0,
	      "[!] plugin without the lease: nothing decoded or published, "
	      "counted as unheld");
	CHECK(fr.word == (uint8_t)NN_HO_WANT, "...and the job still ends");

	reset(NN_HO_RUNNING);
	b.plugin = 1; b.held = 1; b.admits = 0;
	nn_core_on_infer_done(&fr, &ops, NULL, 5u, 1);
	CHECK(b.acc_what == NN_CORE_DONE_RETIRED && b.outputs_calls == 0 &&
	      b.decode_calls == 0 && b.pub_calls == 0 && b.gives == 1,
	      "[!] the generation moved: asked before the outputs, nothing decoded");

	reset(NN_HO_RUNNING);
	b.plugin = 1; b.held = 1; b.outputs_rc = -1;
	nn_core_on_infer_done(&fr, &ops, NULL, 5u, 1);
	CHECK(b.acc_what == NN_CORE_DONE_NO_OUTPUTS && b.decode_calls == 0 &&
	      b.pub_calls == 0 && b.gives == 1, "outputs refused: not decoded");

	reset(NN_HO_RUNNING);
	b.plugin = 1; b.held = 1; b.decode_rc = -1;
	nn_core_on_infer_done(&fr, &ops, NULL, 5u, 1);
	CHECK(b.acc_what == NN_CORE_DONE_NOT_HELD && b.pub_calls == 0 &&
	      b.gives == 1, "a decode the entry refused publishes nothing");

	reset(NN_HO_RUNNING);
	fr.job_frame = 41u;
	b.plugin = 1; b.held = 1; b.decode_n = -3; b.took = 0;
	nn_core_on_infer_done(&fr, &ops, NULL, 0x99u, 0);
	CHECK(b.pub_calls == 1 && b.pub_n == -3 && b.pub_gen == 0x99u,
	      "[!] a negative count is published as itself, under the armed "
	      "generation");
	CHECK(b.acc_what == NN_CORE_DONE_DECODED && b.acc_n == -3 &&
	      b.acc_took == 0, "...and the board's table sees the count and the drop");
	CHECK(fr.res_frame == 41u,
	      "...the plugin now holds the job's frame, taken or not");
	CHECK(b.give_res_frame == 41u,
	      "[!] ...and that is noted BEFORE the lease is given back (the panel "
	      "reads it under the lease)");
	CHECK(b.acc_word == (int)NN_HO_RUNNING,
	      "[!] the board counts while the job is still RUNNING: the DONE that "
	      "frees the input comes last");
	CHECK(b.gives == 1 && !b.acc_with_lease,
	      "[!] the lease is given back BEFORE the board counts");
	CHECK(fr.word == (uint8_t)NN_HO_IDLE && b.hook_in_cs == 0,
	      "...the word parks last, and no hook ran in the critical section");

	reset(NN_HO_RUNNING);
	b.plugin = 0;
	nn_core_on_infer_done(&fr, &ops_bare, NULL, 3u, 0);
	CHECK(b.raw_calls == 1 && b.acc_what == NN_CORE_DONE_RAW &&
	      fr.word == (uint8_t)NN_HO_IDLE,
	      "a board with no plugin mechanism publishes raw with no lease hooks");
}

/* ---- the panel ------------------------------------------------------------ */

static void good_record(void)
{
	b.rec_ok = 1;
	b.rec.valid = 1;
	b.rec.current = 1u;
	b.rec.kind = (uint8_t)NN_DET_PLUGIN_REPORT;
}

static void test_draw(void)
{
	enum nn_core_draw r;

	printf("the panel's draw\n");

	reset(NN_HO_WANT);
	b.plugin = 1; b.try_ok = 1; b.may_draw = 1; good_record();
	r = nn_core_draw(&fr, &ops_bare, &panel, NULL);
	CHECK(r == NN_CORE_DRAW_NONE && b.paint_calls == 0,
	      "no plugin mechanism: nothing tried");

	reset(NN_HO_WANT);
	b.plugin = 0; b.try_ok = 1; b.may_draw = 1; good_record();
	r = nn_core_draw(&fr, &ops, &panel, NULL);
	CHECK(r == NN_CORE_DRAW_NONE && b.try_calls == 0 && b.lock_calls == 0,
	      "no plugin: the lease is not even tried, nothing locked");

	reset(NN_HO_WANT);
	b.plugin = 1; b.try_ok = 0; b.may_draw = 1; good_record();
	r = nn_core_draw(&fr, &ops, &panel, NULL);
	CHECK(r == NN_CORE_DRAW_MISSED && b.try_calls == 1 &&
	      b.try_who == (int)PLUGIN_LEASE_PANEL && b.lock_calls == 0 &&
	      b.paint_calls == 0 && b.gives == 0,
	      "[!] a busy lease is a miss: tried as the panel, nothing locked");

	reset(NN_HO_WANT);
	b.plugin = 1; b.try_ok = 1; b.may_draw = 0; good_record();
	r = nn_core_draw(&fr, &ops, &panel, NULL);
	CHECK(r == NN_CORE_DRAW_DECLINED && b.lock_calls == 1 &&
	      b.paint_calls == 0 && b.gives == 1 && b.held == 0,
	      "the board's switch off: declined, frame lock left held, lease back");

	reset(NN_HO_WANT);
	b.plugin = 1; b.try_ok = 1; b.may_draw = 1; good_record();
	b.rec_ok = 0;
	CHECK(nn_core_draw(&fr, &ops, &panel, NULL) == NN_CORE_DRAW_DECLINED &&
	      b.paint_calls == 0, "no record to read: declined");

	reset(NN_HO_WANT);
	b.plugin = 1; b.try_ok = 1; b.may_draw = 1; good_record();
	b.rec.current = 0u;
	CHECK(nn_core_draw(&fr, &ops, &panel, NULL) == NN_CORE_DRAW_DECLINED &&
	      b.paint_calls == 0,
	      "[!] the lease is not a licence: a result from the last session is "
	      "declined");

	reset(NN_HO_WANT);
	b.plugin = 1; b.try_ok = 1; b.may_draw = 1; good_record();
	b.rec.kind = (uint8_t)NN_DET_RAW_TENSORS;
	CHECK(nn_core_draw(&fr, &ops, &panel, NULL) == NN_CORE_DRAW_DECLINED &&
	      b.paint_calls == 0, "...and so is one nothing decoded");

	reset(NN_HO_WANT);
	b.plugin = 1; b.try_ok = 1; b.may_draw = 1; good_record();
	b.rec.valid = 0;
	CHECK(nn_core_draw(&fr, &ops, &panel, NULL) == NN_CORE_DRAW_DECLINED &&
	      b.paint_calls == 0, "...and an empty record");

	reset(NN_HO_WANT);
	b.plugin = 1; b.try_ok = 1; b.may_draw = 1; good_record();
	fr.frame_no = 12u;
	fr.res_frame = 9u;
	r = nn_core_draw(&fr, &ops, &panel, NULL);
	CHECK(r == NN_CORE_DRAW_PAINTED && b.paint_calls == 1 &&
	      b.paint_without_lease == 0 && b.paint_without_lock == 0 &&
	      b.lock_before_lease == 0,
	      "[!] painted: under the lease, after the frame lock, lease first");
	CHECK(b.gives == 1 && b.held == 0, "...and the lease is back before it returns");
	CHECK(fr.lag_n == 0u, "the lag waits for the frame to be out");
	nn_core_on_present_done(&fr, &ops);
	CHECK(fr.lag_n == 1u && fr.lag_sum == 3u && fr.lag_max == 3u,
	      "present done: the result was 3 frames behind");
	nn_core_on_present_done(&fr, &ops);
	CHECK(fr.lag_n == 1u, "...counted once");
	nn_core_frame_reset(&fr, &ops, 0);
	CHECK(fr.lag_n == 1u && fr.frame_no == 0u,
	      "a one-shot's reset keeps the stream's lag");
	nn_core_frame_reset(&fr, &ops, 1);
	CHECK(fr.lag_n == 0u && fr.lag_max == 0u, "a stream's reset clears it");
	CHECK(b.hook_in_cs == 0 && b.in_cs == 0,
	      "no hook ran in the critical section, and every section closed");
}

int main(void)
{
	test_bands();
	test_one_part();
	test_abandon_and_join();
	test_discard();
	test_infer_done();
	test_draw();
	if (fails) {
		printf("FAILED (%d)\n", fails);
		return 1;
	}
	printf("test_nn_core_frame: all cases pass\n");
	return 0;
}
