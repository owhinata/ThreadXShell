/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for how one `nn run` ends (issue #171, svc/nn/nn_shot.c): the
 * classification of a worker's job, every reason a record is refused, the first
 * ending winning, a stale run number, the wrap past 0, and the status and
 * sentence of every failure.
 *
 * WHY THIS EXISTS.  None of these endings can be made on hardware: an invoke
 * that fails, a lease not had in time, a frame whose tensor went away.  A run
 * that is not closed by them waits out its deadline and says TIMEOUT, so the
 * table is held to the rule here, every combination.
 */
#include <stdio.h>
#include <string.h>

#include "nn_detail.h"
#include "nn_shot.h"
#include "nn_svc.h"

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

static const char *reason_name(int r)
{
	switch (r) {
	case NN_SHOT_NONE:          return "NONE";
	case NN_SHOT_PUBLISHED:     return "PUBLISHED";
	case NN_SHOT_PREP:          return "PREP";
	case NN_SHOT_INVOKE:        return "INVOKE";
	case NN_SHOT_LEASE_TIMEOUT: return "LEASE_TIMEOUT";
	case NN_SHOT_NOT_HELD:      return "NOT_HELD";
	case NN_SHOT_NO_OUTPUTS:    return "NO_OUTPUTS";
	case NN_SHOT_UNKNOWN:       return "UNKNOWN";
	default:                    return "?";
	}
}

/* ---- nn_shot_of_done(): every (what, took), and values past the enum ------ */
static void test_of_done(void)
{
	int what, took;

	printf("nn_shot_of_done: every (what, took)\n");
	for (what = -1; what <= (int)NN_CORE_DONE_NO_OUTPUTS + 2; what++) {
		for (took = 0; took <= 1; took++) {
			int want, got;
			char line[96];

			/* The rule, stated once more on purpose. */
			switch (what) {
			case NN_CORE_DONE_RAW:
			case NN_CORE_DONE_DECODED:
				want = took ? NN_SHOT_PUBLISHED : NN_SHOT_NONE;
				break;
			case NN_CORE_DONE_RETIRED:
				want = NN_SHOT_NONE;
				break;
			case NN_CORE_DONE_NOT_HELD:
				want = NN_SHOT_NOT_HELD;
				break;
			case NN_CORE_DONE_NO_OUTPUTS:
				want = NN_SHOT_NO_OUTPUTS;
				break;
			default:
				want = NN_SHOT_UNKNOWN;   /* fail-closed */
				break;
			}
			got = (int)nn_shot_of_done((enum nn_core_done)what, took);
			snprintf(line, sizeof line, "what %d took %d -> %s (want %s)",
			         what, took, reason_name(got), reason_name(want));
			CHECK(got == want, line);
		}
	}
}

/* ---- begin / open ----------------------------------------------------------- */
static void test_begin(void)
{
	struct nn_shot s;
	uint32_t r1, r2;

	printf("nn_shot_begin / nn_shot_open\n");
	memset(&s, 0, sizeof s);
	CHECK(nn_shot_open(&s) == 0u, "zero state: no run is open");
	CHECK(!nn_shot_failed(&s, NULL), "zero state: nothing failed");
	CHECK(nn_shot_record(&s, 0u, NN_SHOT_PREP, 1) == 0 && s.ended == 0u,
	      "run 0 is never recorded");

	r1 = nn_shot_begin(&s);
	CHECK(r1 == 1u && nn_shot_open(&s) == r1, "first begin opens run 1");
	r2 = nn_shot_begin(&s);
	CHECK(r2 == 2u && nn_shot_open(&s) == r2, "the next begin opens run 2");

	/* [!] The wrap skips 0, and a begin clears what the last run ended with. */
	s.run = 0xFFFFFFFFu;
	s.ended = 0xFFFFFFFFu;
	s.reason = (uint8_t)NN_SHOT_INVOKE;
	CHECK(nn_shot_open(&s) == 0u, "an ended run is not open");
	r1 = nn_shot_begin(&s);
	CHECK(r1 == 1u, "the number wraps past 0 to 1");
	CHECK(s.ended == 0u && s.reason == (uint8_t)NN_SHOT_NONE,
	      "begin clears the last ending");
	CHECK(nn_shot_open(&s) == 1u && !nn_shot_failed(&s, NULL),
	      "after the wrap the run is open and has not failed");
}

/* ---- record: every reason x gen_current, on an open run --------------------- */
static void test_record_matrix(void)
{
	int r, g;

	printf("nn_shot_record: every reason x gen_current on an open run\n");
	for (r = NN_SHOT_NONE; r <= NN_SHOT_UNKNOWN + 2; r++) {
		for (g = 0; g <= 1; g++) {
			struct nn_shot s;
			uint32_t run;
			int open, writes, failed;
			enum nn_shot_reason why = NN_SHOT_NONE;
			int stored = (r > NN_SHOT_UNKNOWN) ? NN_SHOT_UNKNOWN : r;
			char line[120];

			memset(&s, 0, sizeof s);
			run = nn_shot_begin(&s);
			open = nn_shot_record(&s, run, (enum nn_shot_reason)r, g);
			/* The rule: NONE never writes; PUBLISHED writes whatever the
			 * generation; a failure writes only with the generation. */
			writes = (r != NN_SHOT_NONE) && (r == NN_SHOT_PUBLISHED || g);
			snprintf(line, sizeof line, "%s gen_current %d: %s",
			         reason_name(r), g,
			         writes ? "ends the run" : "leaves it open");
			CHECK(open == !writes && (nn_shot_open(&s) != 0u) == !writes,
			      line);
			if (writes) {
				snprintf(line, sizeof line, "%s gen_current %d: stored "
				         "as %s", reason_name(r), g,
				         reason_name(stored));
				CHECK(s.ended == run && s.reason == (uint8_t)stored,
				      line);
			}
			failed = nn_shot_failed(&s, &why);
			snprintf(line, sizeof line, "%s gen_current %d: failed %d",
			         reason_name(r), g, failed);
			CHECK(failed == (writes && r != NN_SHOT_PUBLISHED) &&
			      (!failed || (int)why == stored), line);
		}
	}
}

/* ---- the first ending wins ------------------------------------------------- */
static void test_first_wins(void)
{
	int a, b;

	printf("nn_shot_record: the first ending wins\n");
	for (a = NN_SHOT_PUBLISHED; a <= NN_SHOT_UNKNOWN; a++) {
		for (b = NN_SHOT_PUBLISHED; b <= NN_SHOT_UNKNOWN; b++) {
			struct nn_shot s;
			uint32_t run;
			int open;
			char line[96];

			memset(&s, 0, sizeof s);
			run = nn_shot_begin(&s);
			(void)nn_shot_record(&s, run, (enum nn_shot_reason)a, 1);
			open = nn_shot_record(&s, run, (enum nn_shot_reason)b, 1);
			snprintf(line, sizeof line, "%s then %s: stays %s",
			         reason_name(a), reason_name(b), reason_name(a));
			CHECK(!open && s.ended == run && s.reason == (uint8_t)a,
			      line);
		}
	}
}

/* ---- a stale run number ---------------------------------------------------- */
static void test_stale_run(void)
{
	struct nn_shot s;
	uint32_t old, cur;
	int r;

	printf("nn_shot_record: a number that is not the current run\n");
	memset(&s, 0, sizeof s);
	old = nn_shot_begin(&s);
	cur = nn_shot_begin(&s);
	for (r = NN_SHOT_NONE; r <= NN_SHOT_UNKNOWN; r++) {
		char line[96];
		int open = nn_shot_record(&s, old, (enum nn_shot_reason)r, 1);

		snprintf(line, sizeof line, "old run, %s: refused, answers closed",
		         reason_name(r));
		CHECK(!open && s.ended == 0u && nn_shot_open(&s) == cur, line);
	}
	CHECK(nn_shot_record(&s, cur + 1u, NN_SHOT_PREP, 1) == 0 &&
	      s.ended == 0u, "a number ahead of the current run is refused");
	/* An old number whose run had already ended stays refused too. */
	(void)nn_shot_record(&s, cur, NN_SHOT_INVOKE, 1);
	(void)nn_shot_begin(&s);
	CHECK(nn_shot_record(&s, cur, NN_SHOT_PUBLISHED, 1) == 0 &&
	      s.ended == 0u, "the previous run, ended, cannot end the new one");
}

/* ---- after_job: the worker's one call ------------------------------------- */
static void test_after_job(void)
{
	int what, took, g;

	printf("nn_shot_after_job: classify, record, answer the re-arm\n");
	for (what = NN_CORE_DONE_RAW; what <= (int)NN_CORE_DONE_NO_OUTPUTS + 1;
	     what++) {
		for (took = 0; took <= 1; took++) {
			for (g = 0; g <= 1; g++) {
				struct nn_shot s, t;
				uint32_t run;
				int a, b;
				char line[96];

				memset(&s, 0, sizeof s);
				memset(&t, 0, sizeof t);
				run = nn_shot_begin(&s);
				(void)nn_shot_begin(&t);
				a = nn_shot_after_job(&s, run, (enum nn_core_done)what,
				                      took, g);
				b = nn_shot_record(&t, run,
				                   nn_shot_of_done((enum nn_core_done)what,
				                                   took), g);
				snprintf(line, sizeof line,
				         "what %d took %d gen %d: re-arm %d", what,
				         took, g, a);
				CHECK(a == b && s.ended == t.ended &&
				      s.reason == t.reason, line);
			}
		}
	}
	/* [!] Spelled out: a publish the record took is the answer even after
	 * a boundary, and one it did not take re-arms. */
	{
		struct nn_shot s;
		uint32_t run;

		memset(&s, 0, sizeof s);
		run = nn_shot_begin(&s);
		CHECK(nn_shot_after_job(&s, run, NN_CORE_DONE_DECODED, 0, 1) == 1,
		      "a publish not taken: re-arm");
		CHECK(nn_shot_after_job(&s, run, NN_CORE_DONE_RETIRED, 0, 0) == 1,
		      "retired: re-arm");
		CHECK(nn_shot_after_job(&s, run, NN_CORE_DONE_RAW, 1, 0) == 0 &&
		      s.reason == (uint8_t)NN_SHOT_PUBLISHED &&
		      !nn_shot_failed(&s, NULL),
		      "a publish taken across a boundary: PUBLISHED, not a failure");
	}
}

/* ---- the table: status and sentence for every reason --------------------- */
static void test_says(void)
{
	int r;

	printf("nn_shot_says: status and sentence\n");
	for (r = -2; r <= NN_SHOT_UNKNOWN + 2; r++) {
		const struct nn_shot_says *w = nn_shot_says((enum nn_shot_reason)r);
		const struct nn_shot_says *u = nn_shot_says(NN_SHOT_UNKNOWN);
		int want = (r == NN_SHOT_LEASE_TIMEOUT) ? NN_SVC_ERR_BUSY
		                                       : NN_SVC_ERR_HW;
		int failure = (r >= NN_SHOT_PREP && r <= NN_SHOT_UNKNOWN);
		char line[160];

		snprintf(line, sizeof line, "%-13s (%d): status %d \"%s\"",
		         reason_name(r), r, w ? w->status : 0,
		         (w && w->text) ? w->text : "(null)");
		CHECK(w != NULL && w->text != NULL && w->text[0] != '\0' &&
		      strlen(w->text) <= NN_SVC_DETAIL_MAX && w->status == want &&
		      (failure || w == u), line);
	}
	/* Every failure has its own sentence. */
	{
		int a, b, distinct = 1;

		for (a = NN_SHOT_PREP; a <= NN_SHOT_UNKNOWN; a++)
			for (b = a + 1; b <= NN_SHOT_UNKNOWN; b++)
				if (strcmp(nn_shot_says((enum nn_shot_reason)a)->text,
				           nn_shot_says((enum nn_shot_reason)b)->text)
				    == 0)
					distinct = 0;
		CHECK(distinct, "every failure reason has its own sentence");
	}
	/*
	 * [!] NN_SHOT_SAYS_MAX IS THE LONGEST SENTENCE, EXACTLY (issue #174).  A
	 * board appends its own clause to these and checks that clause at build
	 * time against this constant (NN_REFUSAL_RUN_UNFINISHED()); a constant
	 * left behind when a sentence grew would pass a clause that truncates.
	 */
	{
		size_t longest = 0;

		for (r = -2; r <= NN_SHOT_UNKNOWN + 2; r++) {
			size_t n = strlen(nn_shot_says((enum nn_shot_reason)r)->text);
			char line[96];

			snprintf(line, sizeof line, "reason %d: %zu <= NN_SHOT_SAYS_MAX %u",
			         r, n, (unsigned)NN_SHOT_SAYS_MAX);
			CHECK(n <= NN_SHOT_SAYS_MAX, line);
			if (n > longest)
				longest = n;
		}
		CHECK(longest == NN_SHOT_SAYS_MAX,
		      "the longest sentence is exactly NN_SHOT_SAYS_MAX");
	}
}

int main(void)
{
	test_of_done();
	test_begin();
	test_record_matrix();
	test_first_wins();
	test_stale_run();
	test_after_job();
	test_says();
	if (fails) {
		printf("test_nn_shot: %d FAILED\n", fails);
		return 1;
	}
	printf("test_nn_shot: all passed\n");
	return 0;
}
