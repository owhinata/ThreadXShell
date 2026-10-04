/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the inference worker's hand-over word (issue #129; shared and
 * given FILLING by issue #130, svc/nn_handoff.c).
 *
 * WHY THIS EXISTS.  The camera producer writes the model's input tensor in
 * place, with no staging copy, so whether it may do so at a given instant is
 * this word and nothing else.  The sequences that would break it -- a stop that
 * lands while a frame is handed over but not yet taken, a stale wake-up after
 * the worker already took its job, a start that finds the worker not parked, a
 * band that arrives after its frame was abandoned -- are microsecond windows
 * between three threads and cannot be typed.
 *
 * [!] THE TABLE BELOW IS WRITTEN OUT, NOT COMPUTED.  Every (state, operation)
 * pair has its expected answer spelled in the test, including the states and
 * operations nobody defined.  A transition added by mistake (HAND from WANT, a
 * BEGIN from FILLING, a JOIN that accepts HANDED) or one dropped (TAKE never
 * moving, ABANDON going to IDLE) turns a line red.
 *
 * [!] AND WHAT IT DOES NOT COVER.  This compiles nn_handoff.c alone, so it says
 * nothing about whether its caller runs each step inside one critical section,
 * or whether the producer writes the input only while FILLING.  Those are
 * svc/nn_core_frame.c's, walked by test_nn_core_frame.c.
 */
#include <stdio.h>

#include "nn_handoff.h"

static int fails;

#define BAD_STATE 5u     /* one past the last defined state */
#define BAD_OP    8u     /* one past the last defined operation */

static const char *st_name(unsigned s)
{
	switch (s) {
	case NN_HO_IDLE:    return "IDLE";
	case NN_HO_WANT:    return "WANT";
	case NN_HO_FILLING: return "FILLING";
	case NN_HO_HANDED:  return "HANDED";
	case NN_HO_RUNNING: return "RUNNING";
	default:            return "?";
	}
}

static const char *op_name(unsigned o)
{
	switch (o) {
	case NN_HO_OP_ARM:       return "ARM";
	case NN_HO_OP_BEGIN:     return "BEGIN";
	case NN_HO_OP_HAND:      return "HAND";
	case NN_HO_OP_ABANDON:   return "ABANDON";
	case NN_HO_OP_TAKE:      return "TAKE";
	case NN_HO_OP_DONE:      return "DONE";
	case NN_HO_OP_DONE_LAST: return "DONE_LAST";
	case NN_HO_OP_JOIN:      return "JOIN";
	default:                 return "?";
	}
}

/* -1 in `to` means "refused": the step returns 0 and the word is unchanged. */
#define NO (-1)

struct row {
	unsigned from;
	int      to[BAD_OP + 1u];   /* indexed by operation, BAD_OP last */
};

#define I  NN_HO_IDLE
#define W  NN_HO_WANT
#define F  NN_HO_FILLING
#define H  NN_HO_HANDED
#define R  NN_HO_RUNNING
/*               ARM BEGIN HAND ABANDON TAKE DONE DONE_LAST JOIN ?op */
static const struct row table[] = {
	{ I,         { W,  NO,   NO,  NO,     NO,  NO,  NO,       I,   NO } },
	{ W,         { NO, F,    NO,  NO,     NO,  NO,  NO,       I,   NO } },
	/* [!] FILLING is left only by the producer (HAND, ABANDON) or by a JOIN
	 * the stop calls once the producer is confirmed out.  A worker cannot
	 * take it, and a second BEGIN cannot restart it. */
	{ F,         { NO, NO,   H,   W,      NO,  NO,  NO,       I,   NO } },
	{ H,         { NO, NO,   NO,  NO,     R,   NO,  NO,       NO,  NO } },
	{ R,         { NO, NO,   NO,  NO,     NO,  W,   I,        NO,  NO } },
	/* [!] A word nobody can explain is not evidence that the input is free:
	 * every operation is refused, JOIN included, so a stop waits it out to
	 * its deadline and reports the worker lost. */
	{ BAD_STATE, { NO, NO,   NO,  NO,     NO,  NO,  NO,       NO,  NO } },
	{ 0xFFu,     { NO, NO,   NO,  NO,     NO,  NO,  NO,       NO,  NO } },
};

static void walk_table(void)
{
	unsigned r, op;

	printf("nn_handoff_step: every (state, operation)\n");
	for (r = 0u; r < sizeof table / sizeof table[0]; r++) {
		for (op = 0u; op <= BAD_OP; op++) {
			uint8_t w = (uint8_t)table[r].from;
			int want = table[r].to[op];
			int moved = nn_handoff_step(&w, (uint8_t)op);
			int ok;

			if (want == NO)
				ok = (moved == 0 && w == (uint8_t)table[r].from);
			else
				ok = (moved == 1 && w == (uint8_t)want);
			if (!ok) {
				printf("  FAIL %-7s %-9s -> moved %d now %s(%u), wanted "
				       "%s\n", st_name(table[r].from), op_name(op),
				       moved, st_name(w), (unsigned)w,
				       want == NO ? "refused, unchanged"
				                  : st_name((unsigned)want));
				fails++;
			} else {
				printf("  ok   %-7s %-9s %s\n", st_name(table[r].from),
				       op_name(op),
				       want == NO ? "refused" : st_name((unsigned)want));
			}
		}
	}
	/* An operation far out of range, and a NULL word. */
	{
		uint8_t w = (uint8_t)NN_HO_WANT;

		if (nn_handoff_step(&w, 0xFFu) != 0 || w != (uint8_t)NN_HO_WANT) {
			printf("  FAIL an operation 0xFF moved the word\n");
			fails++;
		} else {
			printf("  ok   an operation 0xFF is refused\n");
		}
		if (nn_handoff_step(NULL, (uint8_t)NN_HO_OP_ARM) != 0) {
			printf("  FAIL a NULL word was stepped\n");
			fails++;
		} else {
			printf("  ok   a NULL word is refused\n");
		}
	}
}

static void check_settled(unsigned s, int want)
{
	int got = nn_handoff_settled((uint8_t)s);

	if (!got != !want) {
		printf("  FAIL settled(%s/%u) -> %d, wanted %d\n", st_name(s), s,
		       got, want);
		fails++;
	} else {
		printf("  ok   settled(%s/%u) %s\n", st_name(s), s,
		       got ? "parked" : "not parked");
	}
}

/* One step of a sequence, with what it must answer. */
static void seq(uint8_t *w, unsigned op, int want_moved, unsigned want_state,
                const char *what)
{
	int moved = nn_handoff_step(w, (uint8_t)op);

	if (moved != want_moved || *w != (uint8_t)want_state) {
		printf("  FAIL %-58s -> moved %d now %s, wanted %d %s\n", what, moved,
		       st_name(*w), want_moved, st_name(want_state));
		fails++;
	} else {
		printf("  ok   %s\n", what);
	}
}

int main(void)
{
	uint8_t w;

	walk_table();

	printf("nn_handoff_settled: what a join accepts\n");
	check_settled(NN_HO_IDLE, 1);
	check_settled(NN_HO_WANT, 1);
	/* A frame part way through filling is joinable -- by a stop that has
	 * confirmed the producer out, which is the only caller of JOIN. */
	check_settled(NN_HO_FILLING, 1);
	/* [!] Handed over but not yet taken is NOT parked: the worker will still
	 * wake and run it.  A join that took this for parked would let the stop
	 * cross the record boundary under a decode still to come. */
	check_settled(NN_HO_HANDED, 0);
	check_settled(NN_HO_RUNNING, 0);
	check_settled(BAD_STATE, 0);
	check_settled(0xFFu, 0);

	printf("sequences\n");

	/* A stream: arm, two frames, a busy frame refused, the stop. */
	w = (uint8_t)NN_HO_IDLE;
	seq(&w, NN_HO_OP_ARM,  1, NN_HO_WANT,    "start arms a parked worker");
	seq(&w, NN_HO_OP_HAND, 0, NN_HO_WANT,
	    "a hand-over before anything was written is refused");
	seq(&w, NN_HO_OP_BEGIN, 1, NN_HO_FILLING, "the producer begins frame 1");
	seq(&w, NN_HO_OP_BEGIN, 0, NN_HO_FILLING,
	    "a second first part does not restart the fill");
	seq(&w, NN_HO_OP_TAKE, 0, NN_HO_FILLING,
	    "the worker cannot take a frame still being written");
	seq(&w, NN_HO_OP_HAND, 1, NN_HO_HANDED,  "the producer hands frame 1 over");
	seq(&w, NN_HO_OP_BEGIN, 0, NN_HO_HANDED,
	    "frame 2 while frame 1 waits: busy, the input is not the producer's");
	seq(&w, NN_HO_OP_ABANDON, 0, NN_HO_HANDED,
	    "a handed-over frame cannot be abandoned");
	seq(&w, NN_HO_OP_JOIN, 0, NN_HO_HANDED,
	    "a stop now waits: the handed frame will still run");
	seq(&w, NN_HO_OP_TAKE, 1, NN_HO_RUNNING, "the worker takes frame 1");
	seq(&w, NN_HO_OP_TAKE, 0, NN_HO_RUNNING,
	    "a stale wake-up finds nothing to take");
	seq(&w, NN_HO_OP_BEGIN, 0, NN_HO_RUNNING,
	    "a frame during the invoke: busy, not inferred -- the inference "
	    "reuses the input");
	seq(&w, NN_HO_OP_JOIN, 0, NN_HO_RUNNING, "a stop waits for the running job");
	seq(&w, NN_HO_OP_DONE, 1, NN_HO_WANT,    "the worker asks for the next frame");
	seq(&w, NN_HO_OP_TAKE, 0, NN_HO_WANT,
	    "the wake-up that raced the job's own is stale too");
	seq(&w, NN_HO_OP_JOIN, 1, NN_HO_IDLE,    "the stop joins a parked worker");
	seq(&w, NN_HO_OP_BEGIN, 0, NN_HO_IDLE,
	    "a producer frame after the join is refused");
	seq(&w, NN_HO_OP_JOIN, 1, NN_HO_IDLE,    "a retried stop joins again at once");

	/* A one-shot: it wants one frame and no more. */
	w = (uint8_t)NN_HO_IDLE;
	seq(&w, NN_HO_OP_ARM,       1, NN_HO_WANT,    "a one-shot arms");
	seq(&w, NN_HO_OP_BEGIN,     1, NN_HO_FILLING, "its frame is begun");
	seq(&w, NN_HO_OP_HAND,      1, NN_HO_HANDED,  "and handed over");
	seq(&w, NN_HO_OP_TAKE,      1, NN_HO_RUNNING, "and taken");
	seq(&w, NN_HO_OP_DONE_LAST, 1, NN_HO_IDLE,
	    "it ends wanting nothing more");
	seq(&w, NN_HO_OP_BEGIN,     0, NN_HO_IDLE,
	    "so the next frame is not the worker's");

	/* [!] THE TWO WAYS OUT OF FILLING ARE NOT THE SAME (issue #130).  The
	 * producer's own ABANDON keeps the worker wanting; the stop's JOIN parks
	 * it.  A mutation that sent ABANDON to IDLE would stop a stream for good
	 * after one bad band; one that sent JOIN to WANT would leave a stopped
	 * stream licensing a producer that comes back. */
	w = (uint8_t)NN_HO_WANT;
	seq(&w, NN_HO_OP_BEGIN,   1, NN_HO_FILLING, "a fill begins");
	seq(&w, NN_HO_OP_ABANDON, 1, NN_HO_WANT,
	    "ABANDON: the producer drops it; the worker still wants a frame");
	seq(&w, NN_HO_OP_ABANDON, 0, NN_HO_WANT,
	    "a second ABANDON finds nothing to drop");
	seq(&w, NN_HO_OP_BEGIN,   1, NN_HO_FILLING,
	    "the next first part begins again");
	seq(&w, NN_HO_OP_JOIN,    1, NN_HO_IDLE,
	    "JOIN: the stop parks it, the frame is gone");
	seq(&w, NN_HO_OP_ABANDON, 0, NN_HO_IDLE,
	    "an ABANDON after the join changes nothing");
	seq(&w, NN_HO_OP_HAND,    0, NN_HO_IDLE,
	    "nor does a late hand-over");

	/* [!] A start that finds the worker not parked refuses: it does not wait
	 * and it does not reset the word. */
	w = (uint8_t)NN_HO_RUNNING;
	seq(&w, NN_HO_OP_ARM, 0, NN_HO_RUNNING, "start refused over a running job");
	w = (uint8_t)NN_HO_WANT;
	seq(&w, NN_HO_OP_ARM, 0, NN_HO_WANT,
	    "start refused over a stream never joined");
	w = (uint8_t)NN_HO_FILLING;
	seq(&w, NN_HO_OP_ARM, 0, NN_HO_FILLING,
	    "start refused over a frame never joined");

	if (fails) {
		printf("FAILED (%d)\n", fails);
		return 1;
	}
	printf("test_nn_handoff: all cases pass\n");
	return 0;
}
