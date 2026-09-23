/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_nn_det_record.c
 * @brief   Host tests for svc/nn_det_record.c (issues #97, #110, #116, #118).
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

/*
 * --- issue #118: a result outlives its session ----------------------------
 *
 * [!] THE BOUNDARY AND THE MODEL CHANGE ARE TWO OPERATIONS NOW.  Every case
 * below starts from a record that HOLDS something, because every mistake worth
 * catching here is one that destroys or resurrects a live result -- and an empty
 * record cannot show either.
 */
static void test_outlives_session(void)
{
	struct nn_det_record rec;
	struct nn_det_snapshot snap;
	struct bf_det dets[BF_MAX_DET];
	struct bf_det one = mk_det(0.25f), late = mk_det(0.75f);
	struct bf_result r3 = mk_res(3), r9 = mk_res(9);
	uint32_t g0, g1, base, acc, ep;

	printf("test_nn_det_record: a result outlives its session (#118)\n");
	memset(&rec, 0, sizeof rec);

	/* --- a boundary moves the generation and keeps the result ------- */
	nn_det_record_boundary(&rec);                  /* a session starts */
	g0 = nn_det_record_gen(&rec);
	expect("a publish of the session is taken",
	       nn_det_record_publish(&rec, &one, 1, &r3, g0) != 0, "dropped");
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	acc = snap.accepted;
	ep  = snap.epoch;

	nn_det_record_boundary(&rec);                  /* ...and stops */
	g1 = nn_det_record_gen(&rec);
	expect("[!] a boundary moves the generation", g1 != g0, "g0 %u g1 %u",
	       (unsigned)g0, (unsigned)g1);
	memset(dets, 0, sizeof dets);
	nn_det_record_snapshot(&rec, &snap, dets, BF_MAX_DET);
	expect("[!] and the result stays -- it is still the last thing the model "
	       "produced",
	       snap.valid != 0 && snap.ndet == 1 && snap.res.npass == 3 &&
	               dets[0].x == 0.25f &&
	               snap.kind == (uint8_t)NN_DET_CALLER_BOXES,
	       "valid %d ndet %d npass %d x %.3f kind %u", snap.valid, snap.ndet,
	       snap.res.npass, (double)dets[0].x, (unsigned)snap.kind);
	expect("[!] but it is no longer the CURRENT session's result",
	       snap.current == 0u, "current %u", (unsigned)snap.current);
	expect("a boundary is not a publish and not a model change",
	       snap.accepted == acc && snap.epoch == ep,
	       "accepted %u->%u epoch %u->%u", (unsigned)acc,
	       (unsigned)snap.accepted, (unsigned)ep, (unsigned)snap.epoch);

	/* The in-flight inference of the ended session still lands nowhere, and
	 * -- the part that is new -- it does not disturb the result it failed to
	 * replace. */
	expect("[!] a decode armed before the boundary is dropped",
	       nn_det_record_publish(&rec, &late, 1, &r9, g0) == 0, "taken");
	memset(dets, 0, sizeof dets);
	nn_det_record_snapshot(&rec, &snap, dets, BF_MAX_DET);
	expect("and the kept result is exactly as it was, and still not current",
	       snap.valid != 0 && snap.res.npass == 3 && dets[0].x == 0.25f &&
	               snap.accepted == acc && snap.current == 0u,
	       "valid %d npass %d x %.3f accepted %u", snap.valid,
	       snap.res.npass, (double)dets[0].x, (unsigned)snap.accepted);

	/* --- what a session produced is COUNTED, not inferred from valid -- */
	/*
	 * [!] THIS IS THE READER'S VIEW OF THE CHANGE.  `valid` stays set across the
	 * boundary now, so a reader that asked "is there a result?" would be told
	 * yes about a session that has published nothing.  The base is taken after
	 * the boundary, as a board takes it at its commit.
	 */
	nn_det_record_boundary(&rec);                  /* a new session */
	g1 = nn_det_record_gen(&rec);
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	base = snap.accepted;
	expect("[!] a new session with an old result has no result of its own",
	       snap.valid != 0 && nn_det_last_valid(&snap, base) == 0,
	       "valid %d last_valid %d", snap.valid,
	       nn_det_last_valid(&snap, base));

	/* A late publish of the PREVIOUS session is not counted as this one's. */
	(void)nn_det_record_publish(&rec, &late, 1, &r9, g0);
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("[!] a dropped publish is not counted",
	       snap.accepted == base && nn_det_last_valid(&snap, base) == 0,
	       "accepted %u base %u", (unsigned)snap.accepted, (unsigned)base);

	expect("the session's own publish is taken",
	       nn_det_record_publish(&rec, &late, 1, &r9, g1) != 0, "dropped");
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("[!] a publish of the session in force is current",
	       snap.current != 0u, "current 0");
	expect("and counted: accepted moves by exactly one",
	       snap.accepted == base + 1u, "accepted %u base %u",
	       (unsigned)snap.accepted, (unsigned)base);
	expect("so the session has a result of its own",
	       nn_det_last_valid(&snap, base) != 0, "last_valid 0");

	/* Every kind counts, and every refusal of every kind does not. */
	acc = snap.accepted;
	(void)nn_det_record_publish_external(&rec, 2, g0);
	(void)nn_det_record_publish_raw(&rec, g0);
	(void)nn_det_record_publish(&rec, &one, 1, &r3, g0);
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("[!] no refused publish of any kind is counted",
	       snap.accepted == acc, "accepted %u -> %u", (unsigned)acc,
	       (unsigned)snap.accepted);
	(void)nn_det_record_publish_external(&rec, 2, g1);
	(void)nn_det_record_publish_raw(&rec, g1);
	(void)nn_det_record_publish(&rec, &one, 1, &r3, g1);
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("and every accepted publish of every kind is",
	       snap.accepted == acc + 3u, "accepted %u -> %u", (unsigned)acc,
	       (unsigned)snap.accepted);

	/*
	 * The two interleavings a board's commit sees (plan review r5/r6).  The
	 * base is sampled after the boundary and before the commit:
	 *   - a publish between the boundary and the sample is ABSORBED into the
	 *     base -- the session says "none yet" about a result it has: the safe
	 *     direction;
	 *   - a publish between the sample and the commit is COUNTED, and it is
	 *     this session's, because the generation rule admits nothing else
	 *     after the boundary.
	 */
	nn_det_record_boundary(&rec);
	g1 = nn_det_record_gen(&rec);
	(void)nn_det_record_publish_raw(&rec, g1);     /* before the sample */
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	base = snap.accepted;
	expect("a publish before the sample is absorbed (reads as none yet)",
	       nn_det_last_valid(&snap, base) == 0, "last_valid 1");
	(void)nn_det_record_publish_raw(&rec, g1);     /* after the sample */
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("a publish after the sample is counted",
	       nn_det_last_valid(&snap, base) != 0, "last_valid 0");

	/* --- a model change clears, whatever it is counted as ----------- */
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	acc = snap.accepted;
	ep  = snap.epoch;
	(void)nn_det_record_publish_external(&rec, 4, g1);
	g0 = g1;
	nn_det_record_invalidate(&rec);
	g1 = nn_det_record_gen(&rec);
	memset(dets, 0x5A, sizeof dets);
	nn_det_record_snapshot(&rec, &snap, dets, BF_MAX_DET);
	expect("[!] a model change clears the result",
	       snap.valid == 0 && snap.ndet == 0 && snap.reportable == 0u &&
	               snap.current == 0u &&
	               snap.kind == (uint8_t)NN_DET_CALLER_BOXES &&
	               snap.res.npass == 0,
	       "valid %d ndet %d reportable %u kind %u npass %d", snap.valid,
	       snap.ndet, (unsigned)snap.reportable, (unsigned)snap.kind,
	       snap.res.npass);
	{
		unsigned i;
		int left = 0;

		for (i = 0u; i < (unsigned)BF_MAX_DET; i++)
			if (rec.dets[i].x != 0.0f || rec.dets[i].score != 0.0f)
				left = 1;
		expect("the boxes of the old model are gone from the record", !left,
		       "a box survived");
	}
	expect("[!] so a session that published under the old model has no "
	       "result any more",
	       nn_det_last_valid(&snap, acc) == 0, "last_valid 1");
	expect("[!] and the epoch moved, so a reader's latch can tell",
	       snap.epoch != ep, "epoch %u", (unsigned)snap.epoch);
	expect("[!] but the count did not go back: a base on it stays a base",
	       snap.accepted == acc + 1u, "accepted %u (was %u)",
	       (unsigned)snap.accepted, (unsigned)acc);
	expect("the generation moved too", g1 != g0, "g %u", (unsigned)g1);
	expect("[!] so a publish armed under the old model lands nowhere",
	       nn_det_record_publish_external(&rec, 5, g0) == 0, "taken");
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("and the cleared record stays clear", snap.valid == 0,
	       "valid %d", snap.valid);

	/* --- a dropped DECODE withholds the account, not the count ------- */
	/*
	 * [!] A plugin keeps its result privately; the record holds its count.  A
	 * decode whose publish is dropped has still rewritten the plugin's private
	 * state -- so what the record holds and what the plugin would describe no
	 * longer agree, and only one of them may be shown.
	 */
	expect("an external publish of the session is taken",
	       nn_det_record_publish_external(&rec, 2, g1) != 0, "dropped");
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("its decoder can describe it", snap.reportable != 0u,
	       "reportable 0");
	expect("[!] and it is the current session's, whichever kind published it",
	       snap.current != 0u, "current 0");
	nn_det_record_boundary(&rec);                  /* the stream stops */
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("[!] a boundary runs no decoder, so the account stays",
	       snap.reportable != 0u && snap.valid != 0, "reportable %u valid %d",
	       (unsigned)snap.reportable, snap.valid);
	acc = snap.accepted;
	expect("the in-flight decode's publish is dropped",
	       nn_det_record_publish_external(&rec, 6, g1) == 0, "taken");
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("[!] after it the account is withheld",
	       snap.reportable == 0u, "reportable 1");
	expect("[!] and the count and the result are exactly as they were",
	       snap.valid != 0 && snap.ndet == 2 && snap.accepted == acc &&
	               snap.kind == (uint8_t)NN_DET_PLUGIN_REPORT,
	       "valid %d ndet %d accepted %u kind %u", snap.valid, snap.ndet,
	       (unsigned)snap.accepted, (unsigned)snap.kind);
	g1 = nn_det_record_gen(&rec);
	(void)nn_det_record_publish_external(&rec, 3, g1);
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("the next accepted decode can be described again",
	       snap.reportable != 0u && snap.ndet == 3, "reportable %u ndet %d",
	       (unsigned)snap.reportable, snap.ndet);
	nn_det_record_boundary(&rec);
	g1 = nn_det_record_gen(&rec);
	(void)nn_det_record_publish_raw(&rec, g1);
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("a result no plugin produced has no account to give",
	       snap.reportable == 0u, "reportable 1");
	expect("[!] and an undecoded result of the session in force is current",
	       snap.current != 0u, "current 0");
	(void)nn_det_record_publish_external(&rec, 3, g1);
	(void)nn_det_record_publish(&rec, &one, 1, &r3, g1);
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("nor does the resident decoder's (its boxes are all here)",
	       snap.reportable == 0u, "reportable 1");

	/* --- the decoder's negative codes are not interchangeable -------- */
	{
		static const int codes[] = { BF_ERR_MODEL, BF_ERR_UNINIT, BF_ERR_ARG };
		unsigned i;

		for (i = 0u; i < sizeof codes / sizeof codes[0]; i++) {
			struct bf_result r;

			memset(&r, 0, sizeof r);
			r.status = codes[i];
			(void)nn_det_record_publish(&rec, NULL, codes[i], &r, g1);
			nn_det_record_snapshot(&rec, &snap, NULL, 0);
			expect("[!] a negative decode code is published as itself",
			       snap.ndet == codes[i] && snap.valid != 0,
			       "code %d published as %d", codes[i], snap.ndet);
		}
	}

	/* --- the snapshot writes every field it hands out ---------------- */
	memset(&snap, 0xA5, sizeof snap);              /* a previous read's */
	nn_det_record_snapshot(&rec, &snap, NULL, 0);
	expect("[!] the snapshot states the count, the epoch and the account",
	       snap.accepted == rec.accepted && snap.epoch == rec.epoch &&
	               snap.reportable == rec.reportable && snap.current == 1u,
	       "accepted %u/%u epoch %u/%u reportable %u/%u",
	       (unsigned)snap.accepted, (unsigned)rec.accepted,
	       (unsigned)snap.epoch, (unsigned)rec.epoch,
	       (unsigned)snap.reportable, (unsigned)rec.reportable);
	nn_det_record_snapshot(NULL, &snap, NULL, 0);
	expect("a snapshot of nothing is nothing, counts included",
	       snap.valid == 0 && snap.accepted == 0u && snap.reportable == 0u,
	       "valid %d accepted %u", snap.valid, (unsigned)snap.accepted);
	expect("and a null snapshot has no result since anything",
	       nn_det_last_valid(NULL, 0u) == 0, "last_valid 1");

	/* Null tolerance of the two new operations. */
	nn_det_record_boundary(NULL);
	nn_det_record_invalidate(NULL);
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
	nn_det_record_invalidate(&rec);          /* a model goes in */
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
	 * The worker armed under g0 and is now inside an inference.  Something ends
	 * the session -- here the harsher of the two, a model change, which also
	 * clears -- and only afterwards does that inference finish and publish.  The boundary, which
	 * keeps the result, has its own cases in test_outlives_session().
	 */
	nn_det_record_invalidate(&rec);          /* the model changes */
	{
		struct bf_result r = mk_res(9);
		struct bf_det late = mk_det(0.75f);

		took = nn_det_record_publish(&rec, &late, 1, &r, g0);
	}
	expect("a decode that outlived its session is DROPPED", took == 0,
	       "taken");
	nn_det_record_snapshot(&rec, &snap, dets, BF_MAX_DET);
	expect("the retired model leaves nothing behind", snap.valid == 0,
	       "valid %d", snap.valid);
	expect("not even a box count", snap.ndet == 0, "ndet %d", snap.ndet);

	/* --- and it stays dropped across a restart ----------------------- */
	/*
	 * The nastier shape: two changes, and only then the old inference
	 * lands.  Without a generation it would look like the new session's first
	 * frame -- valid, plausible, and from a stream that no longer exists.
	 */
	nn_det_record_invalidate(&rec);          /* ...and changes again */
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
		nn_det_record_invalidate(&rec);
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
		expect("not folded to -1",
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
		nn_det_record_invalidate(&rec);
		expect("[!] an external publish from a retired session lands nowhere",
		       nn_det_record_publish_external(&rec, 2, g) == 0, "taken");
		nn_det_record_snapshot(&rec, &snap, NULL, 0);
		expect("the invalidated record is not valid and routes nowhere",
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
		nn_det_record_invalidate(&rec);
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
		nn_det_record_invalidate(&rec);
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
		nn_det_record_invalidate(&rec);
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

	test_outlives_session();

	if (failures) {
		printf("test_nn_det_record: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_det_record: all cases pass\n");
	return 0;
}
