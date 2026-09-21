/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_nn_det_record.c
 * @brief   Host tests for svc/nn_det_record.c (issues #97, #110, #116).
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

	/* --- an external result (issue #110) ------------------------------ */
	{
		struct bf_det canary[BF_MAX_DET];
		struct bf_result r_ext = mk_res(1);
		uint32_t g;
		unsigned i;
		int untouched;

		/* Start from a live caller-boxes record so the transition is the
		 * thing under test, not a fresh one. */
		nn_det_record_reset(&rec);
		g = nn_det_record_gen(&rec);
		(void)nn_det_record_publish(&rec, &one, 1, &r_ext, g);

		for (i = 0u; i < (unsigned)BF_MAX_DET; i++)
			canary[i].score = 1234;
		nn_det_record_snapshot(&rec, &snap, canary, BF_MAX_DET);
		expect("a caller-boxes record still says so",
		       snap.kind == (uint8_t)NN_DET_CALLER_BOXES, "kind %u",
		       (unsigned)snap.kind);
		expect("and still fills the caller's array", canary[0].score != 1234,
		       "untouched");

		expect("an external publish is taken",
		       nn_det_record_publish_external(&rec, 3, g) != 0, "dropped");
		for (i = 0u; i < (unsigned)BF_MAX_DET; i++)
			canary[i].score = 1234;
		nn_det_record_snapshot(&rec, &snap, canary, BF_MAX_DET);
		expect("[!] the snapshot routes the caller elsewhere",
		       snap.kind == (uint8_t)NN_DET_PLUGIN_REPORT, "kind %u",
		       (unsigned)snap.kind);
		expect("it carries the count", snap.ndet == 3, "ndet %d", snap.ndet);
		untouched = 1;
		for (i = 0u; i < (unsigned)BF_MAX_DET; i++)
			if (canary[i].score != 1234)
				untouched = 0;
		expect("[!] and the caller's array is untouched, every element of it",
		       untouched, "a box was written");
		expect("the previous decoder's diagnostics do not travel with it",
		       snap.res.thresh_milli == 0u && snap.res.npass == 0,
		       "thresh %u npass %d", (unsigned)snap.res.thresh_milli,
		       snap.res.npass);

		expect("[!] a negative external result keeps its own value",
		       nn_det_record_publish_external(&rec, -7, g) != 0, "dropped");
		nn_det_record_snapshot(&rec, &snap, NULL, 0);
		expect("not folded to -1 the way the shared decoder's is",
		       snap.ndet == -7, "ndet %d", snap.ndet);

		expect("[!] and a count larger than the box array is not clamped",
		       nn_det_record_publish_external(&rec, BF_MAX_DET + 5, g) != 0,
		       "dropped");
		nn_det_record_snapshot(&rec, &snap, canary, BF_MAX_DET);
		expect("there is no array for it to be clamped to",
		       snap.ndet == BF_MAX_DET + 5, "ndet %d", snap.ndet);

		/* Back the other way: the kind must not stick. */
		(void)nn_det_record_publish(&rec, &one, 1, &r_ext, g);
		nn_det_record_snapshot(&rec, &snap, canary, BF_MAX_DET);
		expect("a later caller-boxes publish routes back",
		       snap.kind == (uint8_t)NN_DET_CALLER_BOXES, "kind %u",
		       (unsigned)snap.kind);
		expect("and fills the array again", canary[0].score != 1234,
		       "untouched");

		/* The generation rule is the same rule. */
		nn_det_record_reset(&rec);
		expect("[!] an external publish from a retired session lands nowhere",
		       nn_det_record_publish_external(&rec, 2, g) == 0, "taken");
		nn_det_record_snapshot(&rec, &snap, NULL, 0);
		expect("the reset record is not valid and routes nowhere",
		       snap.valid == 0 &&
		               snap.kind == (uint8_t)NN_DET_CALLER_BOXES,
		       "valid %d kind %u", snap.valid, (unsigned)snap.kind);

		expect("an external publish into a null record is refused",
		       nn_det_record_publish_external(NULL, 1, 0u) == 0, "taken");
	}

	/* --- an inference nothing decoded (issue #116) --------------------- */
	/*
	 * [!] THIS IS A PUBLISHED RESULT, NOT THE ABSENCE OF ONE.  A firmware that
	 * carries no decoder still runs the model; the outputs exist and only the
	 * interpretation is missing.  The console's wait is on the INFERENCE
	 * COUNTER, which a caller may only bump for a publish that was taken, so a
	 * board that cannot say "one ran and nobody read it" would leave `nn run`
	 * waiting out its timeout over an inference that had already finished.
	 *
	 * And it is under the same generation rule as the other two, for the same
	 * reason -- a stop cannot cancel an inference in flight.  That is easier to
	 * get wrong here than on the box path, because there are no boxes arriving
	 * from the dead session to notice; all that lands is a `valid` flag.
	 */
	{
		struct bf_det canary[BF_MAX_DET];
		struct bf_result r_live = mk_res(7);
		uint32_t g, stale;
		unsigned i;
		int untouched, boxes_left;

		/* Start from a LIVE caller-boxes record, so what is under test is the
		 * transition and not a record that was already empty. */
		nn_det_record_reset(&rec);
		g = nn_det_record_gen(&rec);
		(void)nn_det_record_publish(&rec, &one, 1, &r_live, g);

		expect("an inference nothing decoded is published",
		       nn_det_record_publish_raw(&rec, g) != 0, "dropped");

		for (i = 0u; i < (unsigned)BF_MAX_DET; i++)
			canary[i].score = 1234;
		nn_det_record_snapshot(&rec, &snap, canary, BF_MAX_DET);
		expect("[!] and it routes the caller to the tensors themselves",
		       snap.kind == (uint8_t)NN_DET_RAW_TENSORS, "kind %u",
		       (unsigned)snap.kind);
		expect("it counts as a result this session produced", snap.valid != 0,
		       "valid %d", snap.valid);
		expect("[!] with a count of zero, which is not a measurement",
		       snap.ndet == 0, "ndet %d", snap.ndet);
		untouched = 1;
		for (i = 0u; i < (unsigned)BF_MAX_DET; i++)
			if (canary[i].score != 1234)
				untouched = 0;
		expect("the caller's array is untouched, every element of it",
		       untouched, "a box was written");
		expect("[!] no decoder ran, so no diagnostics travel with it",
		       snap.res.npass == 0 && snap.res.nkept == 0 &&
		               snap.res.max_score == 0.0f &&
		               snap.res.thresh_milli == 0u,
		       "npass %d nkept %d max %.3f thresh %u", snap.res.npass,
		       snap.res.nkept, (double)snap.res.max_score,
		       (unsigned)snap.res.thresh_milli);
		boxes_left = 0;
		for (i = 0u; i < (unsigned)BF_MAX_DET; i++)
			if (rec.dets[i].x != 0.0f || rec.dets[i].score != 0.0f)
				boxes_left = 1;
		expect("[!] and the boxes it replaced are gone from the record itself",
		       !boxes_left, "a box of the decode before it survived");

		/* The kind must not stick, the same way the external one does not. */
		(void)nn_det_record_publish(&rec, &one, 1, &r_live, g);
		for (i = 0u; i < (unsigned)BF_MAX_DET; i++)
			canary[i].x = 1234.0f;
		nn_det_record_snapshot(&rec, &snap, canary, BF_MAX_DET);
		expect("a later caller-boxes publish routes back",
		       snap.kind == (uint8_t)NN_DET_CALLER_BOXES && canary[0].x == 0.25f,
		       "kind %u x %.3f", (unsigned)snap.kind, (double)canary[0].x);

		/*
		 * [!] A MISMATCH CHANGES NOTHING -- checked against a record that has
		 * something to lose.  An implementation that clears first and tests the
		 * generation afterwards passes the "lands nowhere" case below (an empty
		 * record stays empty) while destroying the live session's result.
		 */
		stale = g;
		nn_det_record_reset(&rec);
		g = nn_det_record_gen(&rec);
		(void)nn_det_record_publish(&rec, &one, 1, &r_live, g);
		expect("[!] an inference from a retired session is dropped",
		       nn_det_record_publish_raw(&rec, stale) == 0, "taken");
		for (i = 0u; i < (unsigned)BF_MAX_DET; i++)
			canary[i].x = 1234.0f;
		nn_det_record_snapshot(&rec, &snap, canary, BF_MAX_DET);
		expect("and the live session's result is exactly as it was",
		       snap.valid != 0 && snap.ndet == 1 &&
		               snap.kind == (uint8_t)NN_DET_CALLER_BOXES &&
		               snap.res.npass == 7 && canary[0].x == 0.25f,
		       "valid %d ndet %d kind %u npass %d x %.3f", snap.valid,
		       snap.ndet, (unsigned)snap.kind, snap.res.npass,
		       (double)canary[0].x);

		/* And the plain form of the same rule: nothing of a retired session
		 * survives into the record it tried to land in. */
		nn_det_record_reset(&rec);
		expect("a retired session cannot make an empty record valid either",
		       nn_det_record_publish_raw(&rec, g) == 0, "taken");
		nn_det_record_snapshot(&rec, &snap, NULL, 0);
		expect("which still reports nothing published yet", snap.valid == 0,
		       "valid %d", snap.valid);

		expect("publishing an undecoded inference into a null record is "
		       "refused",
		       nn_det_record_publish_raw(NULL, 0u) == 0, "taken");
	}

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
