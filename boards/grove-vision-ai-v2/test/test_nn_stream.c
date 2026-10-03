/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the live-inference teardown table (issues #99, #129,
 * port/npu/nn_stream_state.c).
 *
 * WHY THIS EXISTS.  `nn stream stop` runs two halves -- stop the camera
 * producer, then unlink the panel sink -- and each can come back unconfirmed for
 * reasons that call for opposite things.  Told "reboot" for a momentary lock
 * collision, an operator power-cycles a board a second stop would have fixed.
 * Told "retry" for a producer that never came back, they retry for ever while a
 * thread is still inside the sink and a later `nn model unload` can dismantle
 * the interpreter underneath it.
 *
 * None of it can be produced from a console.  This board has ONE console, its
 * background jobs run below the foreground shell under TX_NO_TIME_SLICE, and the
 * inputs that matter are microsecond windows inside another thread: a stop that
 * loses the camera API mutex, a detach that finds a callback still in flight.
 *
 * [!] THE TWO VECTORS THIS FILE IS REALLY FOR are the retryable ones.  Issue #99
 * changed their classification, and it changed it because the change itself
 * falsified the old premise: camera.h prescribed a reboot for lock contention
 * only because there was no command that could stop a stream on its own, and
 * this issue adds exactly that command.  Fold either of them back into terminal
 * and those two lines fail.
 *
 * [!] AND WHAT IT DOES NOT COVER.  This compiles nn_stream_state.c and nothing
 * else, so it says nothing about whether the caller detaches only when told to,
 * releases the claim exactly once, or leaves a retryable stop claimable again.
 * What holds those down is that there is one call site and it is short -- not
 * this file.
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

static void check_w(const char *what, int cam_rc, int wjoin, int attempted,
                    int detach_rc, enum nn_stream_act want_act,
                    enum nn_stream_why want_why)
{
	struct nn_stream_verdict v = { 0, 0 };

	nn_stream_stop_decide(cam_rc, wjoin, attempted, detach_rc, &v);
	if (v.act != (unsigned char)want_act || v.why != (unsigned char)want_why) {
		printf("  FAIL %-56s -> %s/%u, wanted %s/%u\n", what,
		       act_name(v.act), (unsigned)v.why,
		       act_name((int)want_act), (unsigned)want_why);
		fails++;
	} else {
		printf("  ok   %-56s %s\n", what, act_name(v.act));
	}
}

/* The issue #99 lines, which predate the worker: the stop did not join it, and
 * NOT_TRIED is what reproduces them (issue #129 stage 1). */
static void check(const char *what, int cam_rc, int attempted, int detach_rc,
                  enum nn_stream_act want_act, enum nn_stream_why want_why)
{
	check_w(what, cam_rc, NN_STREAM_WJOIN_NOT_TRIED, attempted, detach_rc,
	        want_act, want_why);
}

/*
 * [!] A FROZEN COPY OF THE TABLE AS IT WAS BEFORE THE WORKER (e295d7e), for the
 * sweep below.  Not a second implementation to keep in step: it must NOT be
 * edited when the table changes, because what it proves is that an input the
 * old table never had -- the worker join -- changes nothing where it says it
 * does not.
 */
static void decide_before_129(int cam_rc, int attempted, int detach_rc,
                              struct nn_stream_verdict *out)
{
	if (cam_rc != NN_STREAM_CAM_OK) {
		switch (cam_rc) {
		case NN_STREAM_CAM_LOCKED:
			out->act = NN_STREAM_ACT_RETRY;
			out->why = NN_STREAM_WHY_CAM_LOCKED;
			return;
		case NN_STREAM_CAM_TIMEOUT:
			out->act = NN_STREAM_ACT_TERMINAL;
			out->why = NN_STREAM_WHY_CAM_LOST;
			return;
		default:
			out->act = NN_STREAM_ACT_TERMINAL;
			out->why = NN_STREAM_WHY_CAM_STATE;
			return;
		}
	}
	if (!attempted) {
		out->act = NN_STREAM_ACT_TERMINAL;
		out->why = NN_STREAM_WHY_SINK_LOST;
		return;
	}
	switch (detach_rc) {
	case NN_STREAM_CAM_OK:
		out->act = NN_STREAM_ACT_DONE;
		out->why = NN_STREAM_WHY_OK;
		return;
	case NN_STREAM_CAM_BUSY:
		out->act = NN_STREAM_ACT_RETRY;
		out->why = NN_STREAM_WHY_SINK_BUSY;
		return;
	default:
		out->act = NN_STREAM_ACT_TERMINAL;
		out->why = NN_STREAM_WHY_SINK_LOST;
		return;
	}
}

/*
 * Every combination of the four inputs (issue #129).  Three properties, each
 * stated over the whole product rather than picked cases:
 *   1. NOT_TRIED reproduces the table before the worker, everywhere.
 *   2. A join that SUCCEEDED changes nothing either: the verdict is the
 *      producer's and the panel's, exactly as without a worker.
 *   3. A join that failed, or reported a value nobody defined, is terminal
 *      "worker did not return" on every confirmed producer stop, whatever the
 *      detach said -- and is ignored when the producer was not confirmed, where
 *      it should never have been attempted.
 */
static void sweep(void)
{
	static const int cams[] = {
		NN_STREAM_CAM_OK, NN_STREAM_CAM_TIMEOUT, NN_STREAM_CAM_STATE,
		NN_STREAM_CAM_BUSY, NN_STREAM_CAM_LOCKED, -99, 1,
	};
	static const int joins[] = {
		NN_STREAM_WJOIN_NOT_TRIED, NN_STREAM_WJOIN_OK,
		NN_STREAM_WJOIN_FAILED, 3, -1, 99,
	};
	static const int detaches[] = {
		NN_STREAM_CAM_OK, NN_STREAM_CAM_TIMEOUT, NN_STREAM_CAM_STATE,
		NN_STREAM_CAM_BUSY, NN_STREAM_CAM_LOCKED, -99, 1,
	};
	unsigned c, j, a, d, n = 0u, bad = 0u;

	for (c = 0u; c < sizeof cams / sizeof cams[0]; c++)
	for (j = 0u; j < sizeof joins / sizeof joins[0]; j++)
	for (a = 0u; a < 2u; a++)
	for (d = 0u; d < sizeof detaches / sizeof detaches[0]; d++) {
		struct nn_stream_verdict got = { 0xEE, 0xEE };
		struct nn_stream_verdict want = { 0xEE, 0xEE };
		int jn = joins[j];

		nn_stream_stop_decide(cams[c], jn, (int)a, detaches[d], &got);
		if (cams[c] == NN_STREAM_CAM_OK &&
		    jn != NN_STREAM_WJOIN_NOT_TRIED && jn != NN_STREAM_WJOIN_OK) {
			want.act = NN_STREAM_ACT_TERMINAL;
			want.why = NN_STREAM_WHY_WORKER_LOST;
		} else {
			decide_before_129(cams[c], (int)a, detaches[d], &want);
		}
		n++;
		if (got.act != want.act || got.why != want.why) {
			if (bad < 8u)
				printf("  FAIL cam %d join %d attempted %u detach %d "
				       "-> %s/%u, wanted %s/%u\n",
				       cams[c], jn, a, detaches[d],
				       act_name(got.act), (unsigned)got.why,
				       act_name(want.act), (unsigned)want.why);
			bad++;
		}
	}
	if (bad) {
		printf("  FAIL %u of %u combinations\n", bad, n);
		fails++;
	} else {
		printf("  ok   all %u combinations of (camera, worker join, "
		       "detach run, detach)\n", n);
	}
}

static void check_detach(const char *what, int cam_rc, int want)
{
	int got = nn_stream_may_detach(cam_rc);

	if (!got != !want) {
		printf("  FAIL %-56s -> %d, wanted %d\n", what, got, want);
		fails++;
	} else {
		printf("  ok   %-56s %s\n", what, got ? "detach" : "leave it linked");
	}
}

int main(void)
{
	printf("nn_stream_may_detach (issue #99, camera.h's rule)\n");

	/*
	 * [!] ONLY A CONFIRMED STOP.  Anything else means the producer may still be
	 * inside consume(), and unlinking there is the failure the camera's
	 * lost-producer state was invented to prevent.  Widen this to "not an
	 * outright failure" and the LOCKED line below goes green the wrong way.
	 */
	check_detach("a confirmed stop", NN_STREAM_CAM_OK, 1);
	check_detach("the mutex never came free -- the producer was never asked",
	             NN_STREAM_CAM_LOCKED, 0);
	check_detach("the producer never acknowledged", NN_STREAM_CAM_TIMEOUT, 0);
	check_detach("the camera refused", NN_STREAM_CAM_STATE, 0);
	check_detach("an unknown code", -99, 0);

	printf("nn_stream_may_join_worker (issue #129)\n");
	{
		/* The worker is joined on the producer's confirmation and nothing
		 * else: before it, the producer can still hand the worker a job. */
		static const int rcs[] = {
			NN_STREAM_CAM_OK, NN_STREAM_CAM_LOCKED, NN_STREAM_CAM_TIMEOUT,
			NN_STREAM_CAM_STATE, NN_STREAM_CAM_BUSY, -99, 1,
		};
		unsigned i;

		for (i = 0u; i < sizeof rcs / sizeof rcs[0]; i++) {
			int got = nn_stream_may_join_worker(rcs[i]);
			int want = (rcs[i] == NN_STREAM_CAM_OK);

			if (!got != !want) {
				printf("  FAIL may_join_worker(%d) -> %d, wanted %d\n",
				       rcs[i], got, want);
				fails++;
			} else {
				printf("  ok   may_join_worker(%d) %s\n", rcs[i],
				       got ? "join" : "do not try");
			}
		}
	}

	printf("nn_stream_stop_decide\n");

	/* The ordinary path. */
	check("both halves confirmed", NN_STREAM_CAM_OK, 1, NN_STREAM_CAM_OK,
	      NN_STREAM_ACT_DONE, NN_STREAM_WHY_OK);

	/*
	 * [!] RETRYABLE #1.  Nothing was touched -- the stop was never even
	 * requested -- and since issue #99 there is a `nn stream stop` that can ask
	 * again on its own.  Terminal here sends somebody to reboot for a
	 * microsecond of contention.
	 */
	check("the camera API stayed locked: retry, not reboot",
	      NN_STREAM_CAM_LOCKED, 0, 0,
	      NN_STREAM_ACT_RETRY, NN_STREAM_WHY_CAM_LOCKED);

	/*
	 * [!] RETRYABLE #2.  Issue #79: the detach puts the sink back to ATTACHED
	 * and asks to be called again; latching terminal strands the panel until
	 * reboot for something that clears by itself in a moment.
	 */
	check("the detach found work still in flight: retry, not reboot",
	      NN_STREAM_CAM_OK, 1, NN_STREAM_CAM_BUSY,
	      NN_STREAM_ACT_RETRY, NN_STREAM_WHY_SINK_BUSY);

	/* The genuinely terminal ones: a thread is still out there. */
	check("the producer never acknowledged the stop",
	      NN_STREAM_CAM_TIMEOUT, 0, 0,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_CAM_LOST);
	check("the camera refused the stop",
	      NN_STREAM_CAM_STATE, 0, 0,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_CAM_STATE);
	check("the panel thread did not finish",
	      NN_STREAM_CAM_OK, 1, NN_STREAM_CAM_TIMEOUT,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_SINK_LOST);
	check("the unlink was refused for good",
	      NN_STREAM_CAM_OK, 1, NN_STREAM_CAM_STATE,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_SINK_LOST);

	/*
	 * [!] FAIL CLOSED ON ANYTHING UNRECOGNISED, both halves.  A code this table
	 * has never seen is not evidence that anything is quiescent, and a board
	 * that cannot tell must not guess -- these are the two lines that fail if
	 * somebody extends an enum and gives the default arm the friendly answer.
	 */
	check("an unknown camera code is not evidence of a clean stop",
	      -99, 0, 0, NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_CAM_STATE);
	check("an unknown detach code is not evidence of a clean unlink",
	      NN_STREAM_CAM_OK, 1, -99,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_SINK_LOST);

	/*
	 * [!] A CONFIRMED STOP WITH THE DETACH SKIPPED IS A CALLER BUG, and the safe
	 * reading of one: the sink is still linked and the panel thread may still be
	 * in it, so the claim is not handed back.
	 */
	check("the camera stopped but the detach was never run",
	      NN_STREAM_CAM_OK, 0, NN_STREAM_CAM_OK,
	      NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_SINK_LOST);

	/* A stale detach code must not leak in when the detach did not run. */
	check("a stop that was refused ignores a leftover detach code",
	      NN_STREAM_CAM_LOCKED, 0, NN_STREAM_CAM_TIMEOUT,
	      NN_STREAM_ACT_RETRY, NN_STREAM_WHY_CAM_LOCKED);

	printf("nn_stream_stop_decide: the worker join (issue #129)\n");

	/* [!] THE ORDINARY PATH ONCE THE STOP JOINS: nothing changes. */
	check_w("producer, worker and panel all confirmed",
	        NN_STREAM_CAM_OK, NN_STREAM_WJOIN_OK, 1, NN_STREAM_CAM_OK,
	        NN_STREAM_ACT_DONE, NN_STREAM_WHY_OK);
	/* A retryable detach stays retryable behind a joined worker: the
	 * worker's half is settled and a second stop joins it again at once. */
	check_w("worker joined, the detach found work in flight: retry",
	        NN_STREAM_CAM_OK, NN_STREAM_WJOIN_OK, 1, NN_STREAM_CAM_BUSY,
	        NN_STREAM_ACT_RETRY, NN_STREAM_WHY_SINK_BUSY);

	/*
	 * [!] THE NEW TERMINAL.  A worker that did not come back may still be in
	 * the NPU or the plugin; the detach should not even have run, and if it
	 * did, its success is not the worker's.
	 */
	check_w("the worker did not return (detach not run)",
	        NN_STREAM_CAM_OK, NN_STREAM_WJOIN_FAILED, 0, 0,
	        NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_WORKER_LOST);
	check_w("the worker did not return, whatever the detach said",
	        NN_STREAM_CAM_OK, NN_STREAM_WJOIN_FAILED, 1, NN_STREAM_CAM_OK,
	        NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_WORKER_LOST);
	check_w("the worker did not return; a retryable detach is not retry",
	        NN_STREAM_CAM_OK, NN_STREAM_WJOIN_FAILED, 1, NN_STREAM_CAM_BUSY,
	        NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_WORKER_LOST);
	/* [!] Fail closed on a join result nobody defined. */
	check_w("an unknown join result is not evidence of a parked worker",
	        NN_STREAM_CAM_OK, 99, 1, NN_STREAM_CAM_OK,
	        NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_WORKER_LOST);

	/* The producer unconfirmed: the join is not tried, and a stray value
	 * passed anyway does not move the producer's verdict either way. */
	check_w("camera locked: retry, and the join was never tried",
	        NN_STREAM_CAM_LOCKED, NN_STREAM_WJOIN_NOT_TRIED, 0, 0,
	        NN_STREAM_ACT_RETRY, NN_STREAM_WHY_CAM_LOCKED);
	check_w("camera locked: a stray failed join does not make it terminal",
	        NN_STREAM_CAM_LOCKED, NN_STREAM_WJOIN_FAILED, 0, 0,
	        NN_STREAM_ACT_RETRY, NN_STREAM_WHY_CAM_LOCKED);
	check_w("producer lost: still the producer's terminal, not the worker's",
	        NN_STREAM_CAM_TIMEOUT, NN_STREAM_WJOIN_FAILED, 0, 0,
	        NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_CAM_LOST);
	check_w("producer lost: a stray joined worker does not rescue it",
	        NN_STREAM_CAM_TIMEOUT, NN_STREAM_WJOIN_OK, 0, 0,
	        NN_STREAM_ACT_TERMINAL, NN_STREAM_WHY_CAM_LOST);

	printf("nn_stream_stop_decide: every combination (issue #129)\n");
	sweep();

	if (fails) {
		printf("FAILED (%d)\n", fails);
		return 1;
	}
	printf("test_nn_stream: all cases pass\n");
	return 0;
}
