/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the policy around the shared stream lifecycle (issue #130,
 * svc/nn_core.c): the baselines a commit latches, the two-phase poll, and the
 * settle's disposition table.
 *
 * WHY THIS EXISTS.  Until issue #130 each board carried its own copy of this
 * policy, and test_nn_stream_life.c could only say of them that each was three
 * lines long.  There is one copy now, and it runs on the host against hooks
 * that stand in for a board: a critical section that counts its own depth, a
 * clock, counters and a record that can move between the two phases of a poll.
 *
 * [!] EVERY TABLE IS WALKED UNDER EACH OF THREE PROPERTY SETS, because the
 * properties are where the boards differ and a rule that held for one set only
 * would hold on one board only.  The sets are named for what they are -- plain,
 * re-arming with a board latch, gated -- not for which board has them.
 *
 * [!] AND THE HOOKS CHECK WHERE THEY ARE CALLED FROM.  A counter or a record
 * read inside the critical section is a deadlock on hardware (they take
 * mutexes); a gate or a latch touched outside it is a race.  Neither shows in
 * a return value, so the stand-ins record a violation instead.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nn_core.h"

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

/* ---- the stand-in board -------------------------------------------------- */

static int      cs_depth;
static int      misplaced;        /* a hook called on the wrong side of the CS */
static uint32_t now;              /* ticks                                     */
static struct nn_core_raw raw_now;
static struct nn_det_snapshot rec_now;
static int      gate_busy;
static int      gate_takes, gate_gives, gate_last_oneshot;
static int      latches;
static uint32_t latched_extra;
/* Something another thread does while a poll is in its second phase. */
static void   (*during_phase2)(void);
/* Something another thread does between the first and the second sample a
 * commit takes, whichever two they are. */
static void   (*between_samples)(void);

static void run_between(void)
{
	if (between_samples != NULL) {
		void (*f)(void) = between_samples;

		between_samples = NULL;
		f();
	}
}

static unsigned t_cs_enter(void)
{
	return (unsigned)cs_depth++;
}

static void t_cs_exit(unsigned s)
{
	cs_depth--;
	if ((unsigned)cs_depth != s)
		misplaced++;
}

static uint32_t t_ticks(void)
{
	return now;
}

static void t_counts(struct nn_core_raw *raw, void *keep)
{
	if (cs_depth != 0)
		misplaced++;
	*raw = raw_now;
	if (keep != NULL)
		*(uint32_t *)keep = raw_now.offered;
	if (during_phase2 != NULL) {
		void (*f)(void) = during_phase2;

		during_phase2 = NULL;
		f();
	}
	run_between();
}

static void t_record(struct nn_det_snapshot *snap)
{
	if (cs_depth != 0)
		misplaced++;
	*snap = rec_now;
	run_between();
}

static int t_gate_held(void)
{
	if (cs_depth == 0)
		misplaced++;
	return gate_busy;
}

static void t_gate_take(int oneshot)
{
	if (cs_depth == 0)
		misplaced++;
	gate_busy = 1;
	gate_takes++;
	gate_last_oneshot = oneshot;
}

static void t_gate_give(void)
{
	if (cs_depth == 0)
		misplaced++;
	gate_busy = 0;
	gate_gives++;
}

static void t_latch(const void *extra)
{
	if (cs_depth == 0)
		misplaced++;
	latches++;
	if (extra != NULL)
		latched_extra = *(const uint32_t *)extra;
}

/* No re-arm, no gate, no counter based (the board zeroes them at its start),
 * a clock that needs the producer. */
static const struct nn_core_board b_plain = {
	.cs_enter = t_cs_enter, .cs_exit = t_cs_exit,
	.ticks = t_ticks, .ticks_per_s = 1000u,
	.counts = t_counts, .record = t_record,
	.based = 0u,
	.rearm = 0u, .clock_needs_producer = 1u,
};
/* Re-arms a running stream, every counter cumulative, latches board lines
 * beside the stats. */
static const struct nn_core_board b_rearm = {
	.cs_enter = t_cs_enter, .cs_exit = t_cs_exit,
	.ticks = t_ticks, .ticks_per_s = 1000u,
	.counts = t_counts, .record = t_record,
	.latch_extra = t_latch,
	.based = NN_CORE_BASED_ALL,
	.rearm = 1u, .clock_needs_producer = 0u,
};
/* A transient claim decided with the lifecycle; only the offered count runs
 * from boot, and the record's base is taken by the board at its boundary. */
static const struct nn_core_board b_gate = {
	.cs_enter = t_cs_enter, .cs_exit = t_cs_exit,
	.ticks = t_ticks, .ticks_per_s = 1000u,
	.counts = t_counts, .record = t_record,
	.based = NN_CORE_BASED_OFFERED,
	.gate_held = t_gate_held, .gate_take = t_gate_take,
	.gate_give = t_gate_give,
	.rearm = 0u, .clock_needs_producer = 0u,
};

static const struct {
	const char *name;
	const struct nn_core_board *b;
} sets[] = {
	{ "plain", &b_plain },
	{ "rearm", &b_rearm },
	{ "gated", &b_gate },
};
#define NSETS (sizeof sets / sizeof sets[0])

static void reset_world(void)
{
	cs_depth = 0;
	misplaced = 0;
	now = 1000u;
	memset(&raw_now, 0, sizeof raw_now);
	raw_now.producing = 1u;
	memset(&rec_now, 0, sizeof rec_now);
	gate_busy = 0;
	gate_takes = gate_gives = 0;
	gate_last_oneshot = -1;
	latches = 0;
	latched_extra = 0u;
	during_phase2 = NULL;
	between_samples = NULL;
}

/* A stream admitted and committed, the record at @p acc.  The gated set
 * hands the commit the base it took at its own boundary; the others let the
 * commit read the record. */
static uint32_t start(struct nn_core *c, const struct nn_core_board *b,
                      uint32_t acc)
{
	if (nn_core_admit(c, b) != NN_STREAM_START_GO)
		return NN_STREAM_GEN_ANY;
	rec_now.accepted = acc;
	if (b->gate_held != NULL)
		return nn_core_commit(c, b, &acc);
	return nn_core_commit(c, b, NULL);
}

static char name_buf[160];

static const char *nm(const char *set, const char *what)
{
	snprintf(name_buf, sizeof name_buf, "[%s] %s", set, what);
	return name_buf;
}

/* ---- the baselines ------------------------------------------------------- */

/* What a poll should report for one counter: since the commit if the board
 * bases it, as it is otherwise. */
static uint32_t want(const struct nn_core_board *b, uint8_t bit, uint32_t now_v,
                     uint32_t at_commit)
{
	return (b->based & bit) ? now_v - at_commit : now_v;
}

static void baselines(const char *set, const struct nn_core_board *b)
{
	struct nn_core c = { 0 };
	struct nn_stream_stats st;
	struct nn_core_raw at;
	uint32_t g;

	reset_world();
	/* Counters that were already running before this stream: a re-arm, a
	 * board that never resets them -- or, for a counter it does not base,
	 * what it has counted since its own start. */
	raw_now.offered = 100u; raw_now.skipped = 40u; raw_now.infers = 55u;
	raw_now.errors = 5u; raw_now.model_errors = 2u; raw_now.decoder_errors = 1u;
	rec_now.valid = 1; rec_now.ndet = 3;
	at = raw_now;
	g = start(&c, b, 7u);
	ok(nm(set, "a stream commits"), g != NN_STREAM_GEN_ANY);

	ok(nm(set, "a poll right after the commit: based counters read zero, "
	           "the others as the board counted them"),
	   nn_core_poll(&c, b, g, &st) == NN_SVC_OK &&
	   st.frames == want(b, NN_CORE_BASED_OFFERED, 100u, at.offered) &&
	   st.skipped == want(b, NN_CORE_BASED_SKIPPED, 40u, at.skipped) &&
	   st.infers == want(b, NN_CORE_BASED_INFERS, 55u, at.infers) &&
	   st.errors == want(b, NN_CORE_BASED_ERRORS, 5u, at.errors) &&
	   st.model_errors == want(b, NN_CORE_BASED_MODEL_ERRORS, 2u,
	                           at.model_errors) &&
	   st.decoder_errors == want(b, NN_CORE_BASED_DECODER_ERRORS, 1u,
	                             at.decoder_errors));
	ok(nm(set, "...and the previous stream's record result is not its own"),
	   st.last_valid == 0u);

	raw_now.offered += 10u; raw_now.skipped += 4u; raw_now.infers += 6u;
	raw_now.errors += 1u; raw_now.model_errors += 1u; raw_now.last_us = 1234u;
	rec_now.accepted = 8u; rec_now.ndet = 2;
	ok(nm(set, "every count moves with the board's"),
	   nn_core_poll(&c, b, g, &st) == NN_SVC_OK &&
	   st.frames == want(b, NN_CORE_BASED_OFFERED, 110u, at.offered) &&
	   st.skipped == want(b, NN_CORE_BASED_SKIPPED, 44u, at.skipped) &&
	   st.infers == want(b, NN_CORE_BASED_INFERS, 61u, at.infers) &&
	   st.errors == want(b, NN_CORE_BASED_ERRORS, 6u, at.errors) &&
	   st.model_errors == want(b, NN_CORE_BASED_MODEL_ERRORS, 3u,
	                           at.model_errors) &&
	   st.decoder_errors == want(b, NN_CORE_BASED_DECODER_ERRORS, 1u,
	                             at.decoder_errors) &&
	   st.last_us == 1234u);
	ok(nm(set, "a publish since the base is this stream's result"),
	   st.last_valid == 1u && st.last_ndet == 2);
	ok(nm(set, "no hook was called on the wrong side of the CS"),
	   misplaced == 0 && cs_depth == 0);
}

/* [!] A counter that runs from boot wraps, and the difference across the
 * wrap is still the number of frames (modular, not clamped). */
static void wrap(const char *set, const struct nn_core_board *b)
{
	struct nn_core c = { 0 };
	struct nn_stream_stats st;
	uint32_t g;

	if (!(b->based & NN_CORE_BASED_OFFERED))
		return;
	reset_world();
	raw_now.offered = 0xFFFFFFF0u;
	g = start(&c, b, 0u);
	raw_now.offered = 0x10u;
	ok(nm(set, "a based counter that wraps after the commit still counts"),
	   nn_core_poll(&c, b, g, &st) == NN_SVC_OK && st.frames == 0x20u);
	ok(nm(set, "nn_core_since() is modular"),
	   nn_core_since(0x10u, 0xFFFFFFF0u) == 0x20u &&
	   nn_core_since(5u, 5u) == 0u);
}

/* [!] A PUBLISH BETWEEN THE COMMIT'S TWO SAMPLES (review, issue #130).  The
 * stand-in publisher publishes to the record and only then counts, as the
 * boards that base their inference count do.  Landing between the samples it
 * may be counted and not this stream's result, never the other way round:
 * `last` valid with `infers` 0 would leave the count one short for the whole
 * generation. */
static void publish_in_between(void)
{
	rec_now.accepted++;
	rec_now.valid = 1;
	raw_now.infers++;
}

static void commit_interleaving(const char *set, const struct nn_core_board *b)
{
	struct nn_core c = { 0 };
	struct nn_stream_stats st;
	uint32_t g;

	reset_world();
	raw_now.infers = 20u;
	between_samples = publish_in_between;
	g = start(&c, b, 3u);
	ok(nm(set, "a publish between the commit's samples landed"),
	   between_samples == NULL && rec_now.accepted == 4u);
	ok(nm(set, "...and is never this stream's result yet uncounted"),
	   nn_core_poll(&c, b, g, &st) == NN_SVC_OK &&
	   !(st.last_valid == 1u && st.infers == 0u));
}

/* A commit that is refused publishes nothing (issue #99's rule). */
static void refused_commit(const char *set, const struct nn_core_board *b)
{
	struct nn_core c = { 0 };
	struct nn_core_base before;

	reset_world();
	raw_now.offered = 77u;
	before = c.base;
	{
		uint32_t a = 9u;

		ok(nm(set, "a commit with nothing admitted is refused"),
		   nn_core_commit(&c, b, &a) == NN_STREAM_GEN_ANY);
	}
	ok(nm(set, "...and latches no baseline"),
	   memcmp(&before, &c.base, sizeof before) == 0 && c.acc0 == 0u &&
	   c.t0 == 0u);
}

/* ---- the poll ------------------------------------------------------------ */

static struct nn_core *phase2_core;
static const struct nn_core_board *phase2_board;

/* Another console's stop that came back retryable: same phase, same
 * generation afterwards -- only the transition counter moved. */
static void retryable_stop(void)
{
	(void)nn_core_claim_stop(phase2_core, phase2_board, NN_STREAM_GEN_ANY);
	nn_core_settle(phase2_core, phase2_board, NN_CLAIM_RETRYABLE, NULL, NULL);
}

static void poll_phases(const char *set, const struct nn_core_board *b)
{
	struct nn_core c = { 0 };
	struct nn_stream_stats st;
	uint32_t g;
	uint8_t phase0 = 0u, phase1 = 0u;
	uint32_t g1 = 0u;

	reset_world();
	ok(nm(set, "nothing has ever streamed: STATE"),
	   nn_core_poll(&c, b, NN_STREAM_GEN_ANY, &st) == NN_SVC_ERR_STATE);
	g = start(&c, b, 0u);
	ok(nm(set, "somebody else's generation: GEN"),
	   nn_core_poll(&c, b, g + 1u, &st) == NN_SVC_ERR_GEN);
	ok(nm(set, "a NULL out is refused"),
	   nn_core_poll(&c, b, g, NULL) == NN_SVC_ERR_ARG);

	nn_stream_life_snapshot(&c.life, NULL, &phase0, NULL, NULL);
	phase2_core = &c;
	phase2_board = b;
	during_phase2 = retryable_stop;
	ok(nm(set, "a retryable stop across phase 2 is STALE"),
	   nn_core_poll(&c, b, g, &st) == NN_SVC_ERR_STALE);
	nn_stream_life_snapshot(&c.life, &g1, &phase1, NULL, NULL);
	ok(nm(set, "...although the phase and generation did not change"),
	   phase0 == phase1 && g1 == g);
	ok(nm(set, "...and the next poll is accepted"),
	   nn_core_poll(&c, b, g, &st) == NN_SVC_OK && st.running == 1u);
	ok(nm(set, "no hook was called on the wrong side of the CS"),
	   misplaced == 0 && cs_depth == 0);
}

static void poll_clock(const char *set, const struct nn_core_board *b)
{
	struct nn_core c = { 0 };
	struct nn_stream_stats st;
	uint32_t g;

	reset_world();
	g = start(&c, b, 0u);
	now += 2500u;
	ok(nm(set, "a running stream's clock is live"),
	   nn_core_poll(&c, b, g, &st) == NN_SVC_OK && st.elapsed_ms == 2500u);
	raw_now.producing = 0u;
	ok(nm(set, "a stream whose producer is down is not running"),
	   nn_core_poll(&c, b, g, &st) == NN_SVC_OK && st.running == 0u);
	ok(nm(set, b->clock_needs_producer
	           ? "...and its clock is the frozen one"
	           : "...but its clock follows the lifecycle"),
	   st.elapsed_ms == (b->clock_needs_producer ? 0u : 2500u));
}

static void poll_oneshot(const char *set, const struct nn_core_board *b)
{
	struct nn_core c = { 0 };
	struct nn_stream_stats st;
	enum nn_stream_start_claim why = NN_STREAM_START_BUSY;
	struct nn_core_final f;
	uint32_t g, o;

	reset_world();
	/* A stream that ends without a latch (a settle given no numbers) leaves
	 * the live path, which must not count a one-shot's publish. */
	g = start(&c, b, 0u);
	(void)nn_core_claim_stop(&c, b, g);
	nn_core_settle(&c, b, NN_CLAIM_NONE, NULL, NULL);
	o = nn_core_oneshot_claim(&c, b, &why);
	ok(nm(set, "a one-shot is admitted and committed in one call"),
	   o != NN_STREAM_GEN_ANY && why == NN_STREAM_START_GO);
	rec_now.accepted = 1u;
	rec_now.valid = 1;
	ok(nm(set, "a one-shot is not a running stream, and its publish is not "
	           "the stream's last"),
	   nn_core_poll(&c, b, NN_STREAM_GEN_ANY, &st) == NN_SVC_OK &&
	   st.running == 0u && st.last_valid == 0u);
	ok(nm(set, "an operator's stop is refused while the one-shot lives"),
	   nn_core_claim_stop(&c, b, NN_STREAM_GEN_ANY) == NN_STREAM_STOP_ONESHOT);
	ok(nm(set, "the one-shot ends itself"),
	   nn_core_oneshot_end(&c, b, o) == 1);
	ok(nm(set, "...once only"), nn_core_oneshot_end(&c, b, o) == 0);
	(void)f;
}

/* ---- the latch ----------------------------------------------------------- */

static void latch(const char *set, const struct nn_core_board *b)
{
	struct nn_core c = { 0 };
	struct nn_core_final f;
	struct nn_stream_stats st;
	uint32_t g, keep = 0u;

	reset_world();
	rec_now.epoch = 4u;
	g = start(&c, b, 0u);
	raw_now.offered = 30u;
	raw_now.infers = 29u;
	rec_now.accepted = 1u; rec_now.valid = 1; rec_now.ndet = 5;
	now += 1000u;
	ok(nm(set, "the stop is claimed"),
	   nn_core_claim_stop(&c, b, g) == NN_STREAM_STOP_GO);
	nn_core_take_final(&c, b, &f, &keep);
	ok(nm(set, "the final numbers are the stream's, with its epoch"),
	   f.stats.frames == 30u && f.stats.infers == 29u &&
	   f.stats.last_valid == 1u && f.stats.last_ndet == 5 && f.epoch == 4u &&
	   f.stats.running == 0u);
	ok(nm(set, "...and the board kept the very sample they came from"),
	   keep == 30u);
	nn_core_settle(&c, b, NN_CLAIM_NONE, &f, &keep);
	ok(nm(set, "an ended stream is latched"), nn_core_ended(&c));

	/* After the stop, a `nn run` moves the counters and the record. */
	raw_now.offered = 31u;
	raw_now.infers = 30u;
	rec_now.accepted = 2u;
	now += 5000u;
	ok(nm(set, "a poll of the ended stream reads its latch, not the counters"),
	   nn_core_poll(&c, b, g, &st) == NN_SVC_OK && st.frames == 30u &&
	   st.infers == 29u && st.elapsed_ms == 1000u && st.last_valid == 1u);
	rec_now.epoch = 5u;
	ok(nm(set, "a model change since the stop takes the latched last away"),
	   nn_core_poll(&c, b, NN_STREAM_GEN_ANY, &st) == NN_SVC_OK &&
	   st.last_valid == 0u && st.frames == 30u);
	ok(nm(set, b->latch_extra != NULL ? "the board's lines latched with it"
	                                  : "no board latch to call"),
	   b->latch_extra != NULL ? (latches == 1 && latched_extra == 30u)
	                          : latches == 0);
	ok(nm(set, "no hook was called on the wrong side of the CS"),
	   misplaced == 0 && cs_depth == 0);
}

/* ---- the settle's disposition table -------------------------------------- */

struct settle_case {
	enum nn_claim claim;
	int oneshot;
	uint8_t phase;      /* where it ends            */
	int latched;        /* the stream's numbers kept */
	int gave;           /* the gate given back       */
};

static const struct settle_case settle_table[] = {
	/* claim               oneshot phase                       latch gave */
	{ NN_CLAIM_NONE,        0, NN_STREAM_PHASE_IDLE,     1, 1 },
	{ NN_CLAIM_CALLER,      0, NN_STREAM_PHASE_IDLE,     1, 1 },
	{ NN_CLAIM_RETRYABLE,   0, NN_STREAM_PHASE_RUNNING,  0, 0 },
	{ NN_CLAIM_TERMINAL,    0, NN_STREAM_PHASE_LOST,     1, 0 },
	{ NN_CLAIM_NONE,        1, NN_STREAM_PHASE_IDLE,     0, 1 },
	{ NN_CLAIM_CALLER,      1, NN_STREAM_PHASE_IDLE,     0, 1 },
	{ NN_CLAIM_RETRYABLE,   1, NN_STREAM_PHASE_RUNNING,  0, 0 },
	{ NN_CLAIM_TERMINAL,    1, NN_STREAM_PHASE_LOST,     0, 0 },
};

static void settle(const char *set, const struct nn_core_board *b)
{
	unsigned i;

	for (i = 0u; i < sizeof settle_table / sizeof settle_table[0]; i++) {
		const struct settle_case *k = &settle_table[i];
		struct nn_core c = { 0 };
		struct nn_core_final f;
		enum nn_stream_start_claim why = NN_STREAM_START_BUSY;
		uint32_t g, keep = 0u;
		uint8_t phase = 0xffu;
		char what[96];
		int gives0;

		reset_world();
		if (k->oneshot) {
			g = nn_core_oneshot_claim(&c, b, &why);
		} else {
			g = start(&c, b, 0u);
		}
		raw_now.offered = 12u;
		now += 700u;
		(void)nn_core_claim_stop(&c, b, g);
		nn_core_take_final(&c, b, &f, &keep);
		gives0 = gate_gives;
		nn_core_settle(&c, b, k->claim, k->oneshot ? NULL : &f, &keep);
		nn_stream_life_snapshot(&c.life, NULL, &phase, NULL, NULL);
		snprintf(what, sizeof what, "settle claim %d on a %s",
		         (int)k->claim, k->oneshot ? "one-shot" : "stream");
		ok(nm(set, what),
		   phase == k->phase &&
		   (c.final_gen == g) == (k->latched != 0) &&
		   (c.ms == 700u) == (k->latched != 0) &&
		   (gate_gives - gives0) == ((k->gave && b->gate_give) ? 1 : 0) &&
		   latches == ((k->latched && b->latch_extra) ? 1 : 0) &&
		   misplaced == 0 && cs_depth == 0);
	}

	/* [!] A REFUSED SETTLE HAS NO SIDE EFFECTS: nothing claimed the stop. */
	{
		struct nn_core c = { 0 };
		struct nn_core_final f;
		uint32_t g;

		reset_world();
		g = start(&c, b, 0u);
		memset(&f, 0, sizeof f);
		nn_core_settle(&c, b, NN_CLAIM_NONE, &f, NULL);
		nn_core_settle(&c, b, NN_CLAIM_TERMINAL, &f, NULL);
		ok(nm(set, "a settle nobody claimed changes nothing"),
		   c.final_gen == 0u && c.ms == 0u && latches == 0 &&
		   gate_gives == 0 && g != NN_STREAM_GEN_ANY &&
		   c.life.phase == NN_STREAM_PHASE_RUNNING);
	}
}

/* ---- admission ------------------------------------------------------------ */

static void admission(const char *set, const struct nn_core_board *b)
{
	struct nn_core c = { 0 };
	uint32_t g, g2;
	enum nn_stream_start_claim r;

	reset_world();
	g = start(&c, b, 0u);
	r = nn_core_admit(&c, b);
	if (b->rearm) {
		ok(nm(set, "a start over a running stream is a re-arm"),
		   r == NN_STREAM_START_GO);
		g2 = nn_core_commit(&c, b, NULL);
		ok(nm(set, "...which mints a new generation"),
		   g2 != NN_STREAM_GEN_ANY && g2 != g);
	} else if (b->gate_held != NULL) {
		ok(nm(set, "a start over a running stream finds the gate held, "
		           "named by the lifecycle"),
		   r == NN_STREAM_START_RUNNING && gate_takes == 1);
	} else {
		ok(nm(set, "a start over a running stream is refused RUNNING"),
		   r == NN_STREAM_START_RUNNING);
	}
	ok(nm(set, "a one-shot is never a re-arm"),
	   nn_core_oneshot_admit(&c, b) != NN_STREAM_START_GO);
	ok(nm(set, "no hook was called on the wrong side of the CS"),
	   misplaced == 0 && cs_depth == 0);
}

static void abort_gives_back(const char *set, const struct nn_core_board *b)
{
	struct nn_core c = { 0 };

	reset_world();
	ok(nm(set, "a start is admitted"),
	   nn_core_admit(&c, b) == NN_STREAM_START_GO);
	ok(nm(set, b->gate_take ? "...with the gate, as a stream"
	                        : "...with no gate to take"),
	   b->gate_take ? (gate_busy && gate_last_oneshot == 0) : !gate_busy);
	ok(nm(set, "an abort takes"), nn_core_abort(&c, b) == 1);
	ok(nm(set, "...gives the gate back"), gate_busy == 0);
	ok(nm(set, "...and a second abort is refused, giving nothing"),
	   nn_core_abort(&c, b) == 0 &&
	   gate_gives == (b->gate_give ? 1 : 0));
}

/* The gate's holder names the refusal (the gated property only). */
static void gate_refusals(void)
{
	struct nn_stream_life l = { 0 };

	ok("[pure] an idle lifecycle under a held gate: an operation (BUSY)",
	   nn_core_gate_refusal(&l) == NN_STREAM_START_BUSY);
	l.phase = NN_STREAM_PHASE_RUNNING;
	l.kind = NN_STREAM_KIND_STREAM;
	ok("[pure] a running stream: RUNNING",
	   nn_core_gate_refusal(&l) == NN_STREAM_START_RUNNING);
	l.kind = NN_STREAM_KIND_ONESHOT;
	ok("[pure] a running one-shot: ONESHOT",
	   nn_core_gate_refusal(&l) == NN_STREAM_START_ONESHOT);
	l.phase = NN_STREAM_PHASE_STOPPING;
	l.kind = NN_STREAM_KIND_STREAM;
	ok("[pure] a stream mid-stop: BUSY",
	   nn_core_gate_refusal(&l) == NN_STREAM_START_BUSY);
	l.phase = NN_STREAM_PHASE_LOST;
	ok("[pure] LOST: DEAD", nn_core_gate_refusal(&l) == NN_STREAM_START_DEAD);
}

/* A gated one-shot whose end is refused keeps the gate. */
static void oneshot_end_keeps_gate(void)
{
	struct nn_core c = { 0 };
	enum nn_stream_start_claim why = NN_STREAM_START_BUSY;
	uint32_t o;

	reset_world();
	o = nn_core_oneshot_claim(&c, &b_gate, &why);
	ok("[gated] a one-shot takes the gate as an operation",
	   gate_busy && gate_last_oneshot == 1);
	ok("[gated] ...a second is refused by the gate, named ONESHOT",
	   nn_core_oneshot_claim(&c, &b_gate, &why) == NN_STREAM_GEN_ANY &&
	   why == NN_STREAM_START_ONESHOT);
	ok("[gated] an end by the wrong generation is refused and keeps the gate",
	   nn_core_oneshot_end(&c, &b_gate, o + 1u) == 0 && gate_busy);
	ok("[gated] the right one settles and gives it back",
	   nn_core_oneshot_end(&c, &b_gate, o) == 1 && !gate_busy);
}

int main(void)
{
	unsigned i;

	printf("nn_core (issue #130)\n");
	for (i = 0u; i < NSETS; i++) {
		baselines(sets[i].name, sets[i].b);
		wrap(sets[i].name, sets[i].b);
		commit_interleaving(sets[i].name, sets[i].b);
		refused_commit(sets[i].name, sets[i].b);
		poll_phases(sets[i].name, sets[i].b);
		poll_clock(sets[i].name, sets[i].b);
		poll_oneshot(sets[i].name, sets[i].b);
		latch(sets[i].name, sets[i].b);
		settle(sets[i].name, sets[i].b);
		admission(sets[i].name, sets[i].b);
		abort_gives_back(sets[i].name, sets[i].b);
	}
	gate_refusals();
	oneshot_end_keeps_gate();
	if (fails) {
		printf("nn_core: %d failure(s)\n", fails);
		return 1;
	}
	printf("nn_core: all passed\n");
	return 0;
}
