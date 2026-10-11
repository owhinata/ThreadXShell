/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the live-inference teardown table (issues #99, #129, #142,
 * port/nn/nn_stream_state.c).
 *
 * WHY THIS EXISTS.  `nn stream stop` -- and the end of `nn run` -- takes nn's
 * overlay off the panel sink and waits until nothing uses it, joins the
 * inference worker, and lets go of nn's place on the camera (issue #142: the
 * camera keeps streaming for the operator).  Each step can come back
 * unconfirmed for reasons that call for opposite things.  Told "reboot" for a
 * momentary collision, an operator power-cycles a board a second stop would
 * have fixed.  Told "retry" for a thread that never came back, they retry for
 * ever while it is still inside the overlay and a later `nn model unload` can
 * dismantle the interpreter underneath it.
 *
 * None of it can be produced from a console.  This board has ONE console, its
 * background jobs run below the foreground shell under TX_NO_TIME_SLICE, and the
 * inputs that matter are windows inside another thread.
 *
 * [!] THE TWO PROOFS ARE SEPARATE INPUTS (issue #142).  The overlay's count
 * reaching zero permits the worker join and -- with the worker joined -- the
 * record boundary and the leave; it permits no unlink of the panel sink, which
 * only a confirmed camera stop does (port/camera/cam_hold_rules.c, walked by
 * test_cam_hold_rules.c).  The sweep below holds the first half of that down
 * over the whole product of inputs.
 *
 * [!] AND WHAT IT DOES NOT COVER.  This compiles nn_stream_state.c and nothing
 * else, so it says nothing about whether the caller joins and leaves only when
 * told to, releases the claim exactly once, or leaves a retryable stop
 * claimable again.  What holds those down is that there is one call site and it
 * is short -- not this file.
 */
#include <stdio.h>

#include "nn_stream_state.h"

static int fails;

static const char *act_name(int a)
{
	switch (a) {
	case NN_STREAM_ACT_DONE:     return "DONE";
	case NN_STREAM_ACT_RETRY:    return "RETRY";
	case NN_STREAM_ACT_TERMINAL: return "TERMINAL";
	default:                     return "?";
	}
}

static void check(const char *what, int ov_rc, int wjoin, int attempted,
                  int leave_rc, enum nn_stream_act want_act,
                  enum nn_stream_why want_why)
{
	struct nn_stream_verdict v = { 0, 0 };

	nn_stream_stop_decide(ov_rc, wjoin, attempted, leave_rc, &v);
	if (v.act != (unsigned char)want_act || v.why != (unsigned char)want_why) {
		printf("  FAIL %-58s -> %s/%u, wanted %s/%u\n", what,
		       act_name(v.act), (unsigned)v.why,
		       act_name((int)want_act), (unsigned)want_why);
		fails++;
	} else {
		printf("  ok   %-58s %s\n", what, act_name(v.act));
	}
}

/*
 * The table stated a second way, for the sweep: each step by its one success
 * value, the two retryable answers named, everything else terminal.  A second
 * statement rather than a copy -- written as the precedence the header gives,
 * not as the switch the implementation uses.
 */
static void spec(int ov_rc, int wjoin, int attempted, int leave_rc,
                 struct nn_stream_verdict *out)
{
	if (ov_rc == NN_STREAM_CAM_TIMEOUT) {
		out->act = NN_STREAM_ACT_RETRY;
		out->why = NN_STREAM_WHY_OV_BUSY;
	} else if (ov_rc != NN_STREAM_CAM_OK) {
		out->act = NN_STREAM_ACT_TERMINAL;
		out->why = NN_STREAM_WHY_SINK_LOST;
	} else if (wjoin != NN_STREAM_WJOIN_OK) {
		out->act = NN_STREAM_ACT_TERMINAL;
		out->why = NN_STREAM_WHY_WORKER_LOST;
	} else if (!attempted) {
		out->act = NN_STREAM_ACT_TERMINAL;
		out->why = NN_STREAM_WHY_CAM_STATE;
	} else if (leave_rc == NN_STREAM_CAM_OK) {
		out->act = NN_STREAM_ACT_DONE;
		out->why = NN_STREAM_WHY_OK;
	} else if (leave_rc == NN_STREAM_CAM_BUSY) {
		out->act = NN_STREAM_ACT_RETRY;
		out->why = NN_STREAM_WHY_SINK_BUSY;
	} else if (leave_rc == NN_STREAM_CAM_LOCKED) {
		out->act = NN_STREAM_ACT_RETRY;
		out->why = NN_STREAM_WHY_CAM_LOCKED;
	} else if (leave_rc == NN_STREAM_CAM_TIMEOUT) {
		out->act = NN_STREAM_ACT_TERMINAL;
		out->why = NN_STREAM_WHY_CAM_LOST;
	} else {
		out->act = NN_STREAM_ACT_TERMINAL;
		out->why = NN_STREAM_WHY_CAM_STATE;
	}
}

static const int codes[] = {
	NN_STREAM_CAM_OK, NN_STREAM_CAM_TIMEOUT, NN_STREAM_CAM_STATE,
	NN_STREAM_CAM_BUSY, NN_STREAM_CAM_LOCKED, -99, 1,
};
static const int joins[] = {
	NN_STREAM_WJOIN_NOT_TRIED, NN_STREAM_WJOIN_OK,
	NN_STREAM_WJOIN_FAILED, 3, -1, 99,
};
#define NCODES (sizeof codes / sizeof codes[0])
#define NJOINS (sizeof joins / sizeof joins[0])

/*
 * Every combination of the four inputs, against spec(), and two properties
 * stated over the whole product rather than picked cases:
 *   1. DONE only ever comes with BOTH proofs this table owns -- the overlay
 *      shown unused and the worker joined -- and a leave that succeeded.
 *   2. Behind an overlay that was not shown unused, neither the join value
 *      nor the leave value passed moves the verdict: the join was not tried
 *      and the leave not run, so whatever arrives for them is a leftover.
 */
static void sweep(void)
{
	unsigned o, j, a, l, n = 0u, bad = 0u;

	for (o = 0u; o < NCODES; o++)
	for (j = 0u; j < NJOINS; j++)
	for (a = 0u; a < 2u; a++)
	for (l = 0u; l < NCODES; l++) {
		struct nn_stream_verdict got = { 0xEE, 0xEE };
		struct nn_stream_verdict want = { 0xEE, 0xEE };
		struct nn_stream_verdict first = { 0xEE, 0xEE };

		nn_stream_stop_decide(codes[o], joins[j], (int)a, codes[l], &got);
		spec(codes[o], joins[j], (int)a, codes[l], &want);
		n++;
		if (got.act != want.act || got.why != want.why) {
			if (bad < 8u)
				printf("  FAIL ov %d join %d attempted %u leave %d "
				       "-> %s/%u, wanted %s/%u\n",
				       codes[o], joins[j], a, codes[l],
				       act_name(got.act), (unsigned)got.why,
				       act_name(want.act), (unsigned)want.why);
			bad++;
		}
		if (got.act == NN_STREAM_ACT_DONE &&
		    !(nn_stream_may_release(codes[o], joins[j]) && a &&
		      codes[l] == NN_STREAM_CAM_OK)) {
			printf("  FAIL DONE without both proofs and a leave\n");
			bad++;
		}
		if (codes[o] != NN_STREAM_CAM_OK) {
			nn_stream_stop_decide(codes[o], NN_STREAM_WJOIN_NOT_TRIED,
			                      0, 0, &first);
			if (got.act != first.act || got.why != first.why) {
				printf("  FAIL a leftover join/leave moved an "
				       "unproven overlay (ov %d)\n", codes[o]);
				bad++;
			}
		}
	}
	if (bad) {
		printf("  FAIL %u of %u combinations\n", bad, n);
		fails++;
	} else {
		printf("  ok   all %u combinations of (overlay, worker join, "
		       "leave run, leave)\n", n);
	}
}

int main(void)
{
	unsigned i, j;

	printf("nn_stream_may_join_worker (issues #129, #142)\n");
	/* The worker is joined on the overlay's proof and nothing else: before
	 * it, the producer may still be in nn's step and hand it a job. */
	for (i = 0u; i < NCODES; i++) {
		int got = nn_stream_may_join_worker(codes[i]);
		int want = (codes[i] == NN_STREAM_CAM_OK);

		if (!got != !want) {
			printf("  FAIL may_join_worker(%d) -> %d, wanted %d\n",
			       codes[i], got, want);
			fails++;
		} else {
			printf("  ok   may_join_worker(%d) %s\n", codes[i],
			       got ? "join" : "do not try");
		}
	}

	printf("nn_stream_may_release (issues #118, #129, #142)\n");
	/*
	 * [!] ONLY BOTH.  The overlay timing out is "something was still counted
	 * inside" -- widen this to "not an outright failure" and the TIMEOUT rows
	 * go green the wrong way; and a worker not joined may still publish.
	 */
	{
		int bad = 0;

		for (i = 0u; i < NCODES; i++)
		for (j = 0u; j < NJOINS; j++) {
			int got = nn_stream_may_release(codes[i], joins[j]);
			int want = (codes[i] == NN_STREAM_CAM_OK &&
			            joins[j] == NN_STREAM_WJOIN_OK);

			if (!got != !want) {
				printf("  FAIL may_release(%d, %d) -> %d\n",
				       codes[i], joins[j], got);
				bad = 1;
			}
		}
		if (bad)
			fails++;
		else
			printf("  ok   only (overlay unused, worker joined) "
			       "releases\n");
	}

	printf("nn_stream_stop_decide\n");

	/* The ordinary path: the camera keeps streaming for the operator. */
	check("overlay unused, worker joined, nn left the camera",
	      NN_STREAM_CAM_OK, NN_STREAM_WJOIN_OK, 1, NN_STREAM_CAM_OK,
	      NN_STREAM_ACT_DONE, NN_STREAM_WHY_OK);

	/*
	 * [!] RETRYABLE: the overlay's drain timing out.  The overlay is off and
	 * stays off; a second stop waits for the count again.  The step rule of
	 * issue #142 -- a drain that runs out keeps the claim and is retried.
	 */
	check("the overlay was still counted in use at the deadline: retry",
	      NN_STREAM_CAM_TIMEOUT, NN_STREAM_WJOIN_NOT_TRIED, 0, 0,
	      NN_STREAM_ACT_RETRY, NN_STREAM_WHY_OV_BUSY);
	/* [!] RETRYABLE: the leave found a camera start or stop in progress. */
	check("the leave found a camera start or stop in progress: retry",
	      NN_STREAM_CAM_OK, NN_STREAM_WJOIN_OK, 1, NN_STREAM_CAM_BUSY,
	      NN_STREAM_ACT_RETRY, NN_STREAM_WHY_SINK_BUSY);
	/* [!] RETRYABLE: nn's was the last place and the camera API was locked
	 * -- nothing was asked. */
	check("the last place's camera stop never got the API: retry",
	      NN_STREAM_CAM_OK, NN_STREAM_WJOIN_OK, 1, NN_STREAM_CAM_LOCKED,
	      NN_STREAM_ACT_RETRY, NN_STREAM_WHY_CAM_LOCKED);

	/* The terminal ones: something may still be out there. */
	check("the panel sink is lost, or the camera ended unconfirmed",
	      NN_STREAM_CAM_STATE, NN_STREAM_WJOIN_NOT_TRIED, 0, 0,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_SINK_LOST);
	check("the worker did not return",
	      NN_STREAM_CAM_OK, NN_STREAM_WJOIN_FAILED, 0, 0,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_WORKER_LOST);
	check("the last place's camera stop was never acknowledged",
	      NN_STREAM_CAM_OK, NN_STREAM_WJOIN_OK, 1, NN_STREAM_CAM_TIMEOUT,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_CAM_LOST);
	check("the holder code refused for good",
	      NN_STREAM_CAM_OK, NN_STREAM_WJOIN_OK, 1, NN_STREAM_CAM_STATE,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_CAM_STATE);

	/* [!] FAIL CLOSED ON ANYTHING UNRECOGNISED, every part. */
	check("an unknown overlay code is not evidence nothing is inside",
	      -99, NN_STREAM_WJOIN_NOT_TRIED, 0, 0,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_SINK_LOST);
	check("an unknown join result is not evidence of a parked worker",
	      NN_STREAM_CAM_OK, 99, 1, NN_STREAM_CAM_OK,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_WORKER_LOST);
	check("an unknown leave code is not evidence nn let go",
	      NN_STREAM_CAM_OK, NN_STREAM_WJOIN_OK, 1, -99,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_CAM_STATE);

	/* [!] Caller bugs read the safe way: a join skipped behind a proven
	 * overlay, and a leave skipped behind both proofs. */
	check("the overlay was proven but the join never run",
	      NN_STREAM_CAM_OK, NN_STREAM_WJOIN_NOT_TRIED, 1, NN_STREAM_CAM_OK,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_WORKER_LOST);
	check("both proven but the leave never run",
	      NN_STREAM_CAM_OK, NN_STREAM_WJOIN_OK, 0, NN_STREAM_CAM_OK,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_CAM_STATE);

	/* A worker that did not return is not rescued by a leave that ran. */
	check("the worker did not return, whatever the leave said",
	      NN_STREAM_CAM_OK, NN_STREAM_WJOIN_FAILED, 1, NN_STREAM_CAM_OK,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_WORKER_LOST);
	/* And a retryable overlay is not made terminal by a stray failed join. */
	check("overlay busy: a stray failed join does not make it terminal",
	      NN_STREAM_CAM_TIMEOUT, NN_STREAM_WJOIN_FAILED, 1,
	      NN_STREAM_CAM_STATE,
	      NN_STREAM_ACT_RETRY, NN_STREAM_WHY_OV_BUSY);

	printf("nn_stream_stop_decide: every combination\n");
	sweep();

	if (fails) {
		printf("FAILED (%d)\n", fails);
		return 1;
	}
	printf("test_nn_stream: all cases pass\n");
	return 0;
}
