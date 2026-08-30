/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_nn_det_record.c
 * @brief   Host tests for svc/nn_det_record.c (issue #97).
 *
 * THIS IS THE ONLY PLACE THE RULE CAN BE CHECKED.  What it guards is an ordering:
 *
 *     stop clears the record -> an inference that was ALREADY RUNNING finishes
 *     -> it publishes -> the stopped session's boxes are back
 *
 * That ordering is reachable on hardware -- a stop cannot cancel an inference in
 * flight, it can only wait for one, and the wait is bounded -- but it cannot be
 * INJECTED deterministically there.  So the decision is factored out of the
 * board's camera worker into a pure module, and the interleavings are written out
 * here by hand.  A test that said "run a stream, stop it, look for stale boxes"
 * would pass almost always and prove nothing.
 *
 * The second thing pinned here is subtler and was got wrong first: "this session
 * has not decoded a frame yet" is a THIRD state.  Leave the record alone across a
 * start and a fresh session shows the previous one's diagnostics; zero it instead
 * and status BF_OK claims a decode ran and found nothing.  Neither is a
 * measurement anybody took.
 */
#include "nn_det_record.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond, const char *fmt, ...)
{
	va_list ap;

	if (cond) {
		printf("  ok   %s\n", what);
		return;
	}
	printf("  FAIL %s: ", what);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	failures++;
}

/* A decode result that is recognisably "session N frame". */
static struct bf_result mk_res(int npass)
{
	struct bf_result r;

	r.status       = BF_OK;
	r.max_score    = (float)npass;
	r.npass        = npass;
	r.nkept        = npass;
	r.thresh_milli = 644u;
	return r;
}

static struct bf_det mk_det(float x)
{
	struct bf_det d;

	d.x = x;
	d.y = x;
	d.w = 0.1f;
	d.h = 0.1f;
	d.score = 0.9f;
	return d;
}

int main(void)
{
	struct nn_det_record rec;
	struct nn_det_snapshot snap;
	struct bf_det dets[BF_MAX_DET];
	struct bf_det one;
	uint32_t g0, g1;
	int took;

	printf("test_nn_det_record\n");

	memset(&rec, 0, sizeof rec);

	/* --- a fresh record has not decoded anything --------------------- */
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("a fresh record reports nothing decoded yet", snap.valid == 0,
	       "valid %d", snap.valid);

	/*
	 * --- the kind is WRITTEN, not inherited (issue #104) --------------
	 *
	 * [!] THE DESTINATION IS DIRTIED FIRST, ON PURPOSE.  Every caller in the
	 * tree happens to memset its snapshot before the first read, which is what
	 * hid this: the field was the one member the projection did not write, and
	 * the zero it needed arrived from somewhere else.  With a third kind that
	 * omission has teeth -- a snapshot read twice would carry the earlier
	 * routing, and the shared command would go looking elsewhere for boxes that
	 * are sitting in the caller's array.  This record only ever holds caller
	 * boxes, so there is exactly one right answer and it must be stated.
	 */
	snap.kind = (uint8_t)NN_DET_RAW_TENSORS;      /* a previous read's routing */
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("a snapshot states its kind rather than inheriting one",
	       snap.kind == (uint8_t)NN_DET_CALLER_BOXES, "kind %u",
	       (unsigned)snap.kind);

	/* --- the ordinary path ------------------------------------------- */
	nn_det_record_reset(&rec);          /* start a session */
	g0 = nn_det_record_gen(&rec);
	one = mk_det(0.25f);
	{
		struct bf_result r = mk_res(3);

		took = nn_det_record_publish(&rec, &one, 1, &r, g0);
	}
	expect("a publish from the current session is taken", took != 0, "dropped");
	nn_det_record_snapshot(&rec, &snap, dets, BF_MAX_DET);
	expect("and the snapshot is valid", snap.valid != 0, "valid %d", snap.valid);
	expect("with the boxes", snap.ndet == 1 && dets[0].x == 0.25f,
	       "ndet %d x %.3f", snap.ndet, (double)dets[0].x);
	expect("and the diagnostics that belong to them", snap.res.npass == 3,
	       "npass %d", snap.res.npass);
	expect("and they are the caller's boxes",
	       snap.kind == (uint8_t)NN_DET_CALLER_BOXES, "kind %u",
	       (unsigned)snap.kind);

	/* --- THE CASE THIS FILE EXISTS FOR ------------------------------- */
	/*
	 * The worker armed under g0 and is now inside an inference.  A stop runs
	 * (reset), and only afterwards does that inference finish and publish.
	 */
	nn_det_record_reset(&rec);          /* the stop */
	{
		struct bf_result r = mk_res(9);
		struct bf_det late = mk_det(0.75f);

		took = nn_det_record_publish(&rec, &late, 1, &r, g0);
	}
	expect("a decode that outlived its session is DROPPED", took == 0,
	       "taken");
	nn_det_record_snapshot(&rec, &snap, dets, BF_MAX_DET);
	expect("the stopped session leaves nothing behind", snap.valid == 0,
	       "valid %d", snap.valid);
	expect("not even a box count", snap.ndet == 0, "ndet %d", snap.ndet);

	/* --- and it stays dropped across a restart ----------------------- */
	/*
	 * The nastier shape: stop, then START, and only then the old inference
	 * lands.  Without a generation it would look like the new session's first
	 * frame -- valid, plausible, and from a stream that no longer exists.
	 */
	nn_det_record_reset(&rec);          /* the new start */
	g1 = nn_det_record_gen(&rec);
	expect("a new session has a new generation", g1 != g0, "g0 %u g1 %u",
	       (unsigned)g0, (unsigned)g1);
	{
		struct bf_result r = mk_res(9);
		struct bf_det late = mk_det(0.75f);

		took = nn_det_record_publish(&rec, &late, 1, &r, g0);
	}
	expect("the old session's decode does not land in the new one", took == 0,
	       "taken");
	nn_det_record_snapshot(&rec, &snap, dets, BF_MAX_DET);
	expect("which still reports nothing decoded yet", snap.valid == 0,
	       "valid %d", snap.valid);

	/* --- the new session's own frame is fine ------------------------- */
	{
		struct bf_result r = mk_res(5);
		struct bf_det d = mk_det(0.5f);

		took = nn_det_record_publish(&rec, &d, 1, &r, g1);
	}
	expect("the new session's own decode is taken", took != 0, "dropped");
	nn_det_record_snapshot(&rec, &snap, dets, BF_MAX_DET);
	expect("with its own numbers", snap.res.npass == 5 && dets[0].x == 0.5f,
	       "npass %d x %.3f", snap.res.npass, (double)dets[0].x);

	/* --- a failed decode is not zero faces (issue #57) --------------- */
	{
		struct bf_result r;

		memset(&r, 0, sizeof r);
		r.status = BF_ERR_MODEL;
		took = nn_det_record_publish(&rec, NULL, BF_ERR_MODEL, &r, g1);
	}
	expect("a model error publishes", took != 0, "dropped");
	nn_det_record_snapshot(&rec, &snap, dets, BF_MAX_DET);
	expect("as -1 rather than as zero faces", snap.ndet == -1,
	       "ndet %d", snap.ndet);
	expect("and it is still a decode that happened", snap.valid != 0,
	       "valid %d", snap.valid);
	expect("carrying the status", snap.res.status == BF_ERR_MODEL,
	       "status %d", snap.res.status);

	/* --- bounds ------------------------------------------------------- */
	{
		struct bf_result r = mk_res(1);
		struct bf_det many[BF_MAX_DET];

		for (int i = 0; i < BF_MAX_DET; i++)
			many[i] = mk_det((float)i / 100.0f);
		took = nn_det_record_publish(&rec, many, BF_MAX_DET + 4, &r, g1);
	}
	expect("more boxes than the record holds are clamped", took != 0,
	       "dropped");
	nn_det_record_snapshot(&rec, &snap, dets, BF_MAX_DET);
	expect("to its capacity", snap.ndet == BF_MAX_DET, "ndet %d", snap.ndet);

	nn_det_record_snapshot(&rec, &snap, dets, 2);
	expect("a caller asking for fewer boxes gets fewer",
	       snap.ndet == BF_MAX_DET, "ndet %d (the count is not clamped, the "
	       "copy is)", snap.ndet);

	/* --- null tolerance ----------------------------------------------- */
	expect("publishing into a null record is refused",
	       nn_det_record_publish(NULL, &one, 1, NULL, 0u) == 0, "taken");
	snap.kind = (uint8_t)NN_DET_PLUGIN_REPORT;    /* a previous read's routing */
	nn_det_record_snapshot(NULL, &snap, NULL, 0);
	expect("a snapshot of nothing carries no routing either",
	       snap.kind == (uint8_t)NN_DET_CALLER_BOXES, "kind %u",
	       (unsigned)snap.kind);
	expect("a snapshot of nothing is not valid", snap.valid == 0,
	       "valid %d", snap.valid);

	if (failures) {
		printf("test_nn_det_record: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_det_record: all cases pass\n");
	return 0;
}
