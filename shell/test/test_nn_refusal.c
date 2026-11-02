/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the one table of `nn` admission refusals and `nn run` endings
 * (issue #174, svc/nn/nn_refusal.c).
 *
 * WHY THIS EXISTS.  The sentences are picked from a table and one is composed
 * with a number and a board's clause, so the build-time check in nn_detail.h
 * sees none of the final texts; and most of the answers cannot be typed (a dead
 * lifecycle, a start and a stop in flight, a value outside the enum).  So:
 *
 *   (a) every input -- answer x operation x gate or no gate, values outside
 *       the enum included -- is run, and the whole sentence compared with the
 *       expected text below and with NN_SVC_DETAIL_MAX;
 *   (b) the status and the claim of every defined answer are held to the
 *       values the three boards' nine switches gave them (the table below is
 *       written out, not derived), and a value outside the enum to BUSY, NONE;
 *   (c) a sentence with a number is measured with the number at its widest;
 *   (d) NN_REFUSAL_RUN_END_MAX is no shorter than any base sentence and equal
 *       to the longest, so a board's build-time clause check cannot pass on a
 *       stale constant.
 *
 * Built with -DNN_REFUSAL_NEG_CLAUSE it must NOT compile: a clause one
 * character longer than the bound allows (run_host_tests.sh checks it fails on
 * the assertion's message).
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "nn_core.h"
#include "nn_detail.h"
#include "nn_refusal.h"
#include "nn_run_wait.h"
#include "nn_stream_life.h"
#include "nn_svc.h"

static int fails;
static int checks;

#define CHECK(cond, what)                                                   \
	do {                                                                \
		checks++;                                                   \
		if (!(cond)) {                                              \
			printf("  FAIL %s  (%s:%d)\n", what, __FILE__, __LINE__); \
			fails++;                                            \
		}                                                           \
	} while (0)

static int fake_gate_held(void) { return 1; }

static const struct nn_core_board board_nogate;   /* every hook NULL */
static const struct nn_core_board board_gate = { .gate_held = fake_gate_held };

/* ---- the expected answers, written out ----------------------------------- */

#define T_DEAD  "a previous teardown was never confirmed; only a reset clears " \
                "it (`reboot`, or the board's reset button if it refuses)"
#define T_BUSY_GATE   "another nn command, or a start or a stop, holds it -- retry"
#define T_BUSY_NOGATE "a start or a stop is already in progress -- retry"

enum op { OP_RUN, OP_START, OP_STOP, OP_N };
static const char *const op_name[OP_N] = { "run", "start", "stop" };

struct row {
	int         op;
	int         why;      /* the enum value; -1 marks "the default" row */
	int         status;
	int         claim;
	const char *nogate;   /* the sentence on a board without a gate */
	const char *gate;     /* ... and with one */
};

#define DEFAULT_ROW (-1)

static const struct row rows[] = {
	/* run (nn_core_oneshot_admit / nn_core_oneshot_claim's why) */
	{ OP_RUN, NN_STREAM_START_RUNNING, NN_SVC_ERR_BUSY, NN_CLAIM_NONE,
	  "a stream is already running -- `nn stream stats`",
	  "a stream is already running -- `nn stream stats`" },
	{ OP_RUN, NN_STREAM_START_ONESHOT, NN_SVC_ERR_BUSY, NN_CLAIM_NONE,
	  "another `nn run` holds the camera, or one returned with its teardown "
	  "unfinished (`nn stream stop` finishes it)",
	  "another `nn run` holds the camera, or one returned with its teardown "
	  "unfinished (`nn stream stop` finishes it)" },
	{ OP_RUN, NN_STREAM_START_DEAD, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL,
	  T_DEAD, T_DEAD },
	{ OP_RUN, NN_STREAM_START_BUSY, NN_SVC_ERR_BUSY, NN_CLAIM_NONE,
	  T_BUSY_NOGATE, T_BUSY_GATE },
	{ OP_RUN, DEFAULT_ROW, NN_SVC_ERR_BUSY, NN_CLAIM_NONE,
	  T_BUSY_NOGATE, T_BUSY_GATE },
	/* start (nn_core_admit) */
	{ OP_START, NN_STREAM_START_RUNNING, NN_SVC_ERR_STATE, NN_CLAIM_NONE,
	  "a stream is already running (`nn stream stats`)",
	  "a stream is already running (`nn stream stats`)" },
	{ OP_START, NN_STREAM_START_ONESHOT, NN_SVC_ERR_BUSY, NN_CLAIM_NONE,
	  "a `nn run` holds the camera, or one returned with its teardown "
	  "unfinished (`nn stream stop` finishes it)",
	  "a `nn run` holds the camera, or one returned with its teardown "
	  "unfinished (`nn stream stop` finishes it)" },
	{ OP_START, NN_STREAM_START_DEAD, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL,
	  T_DEAD, T_DEAD },
	{ OP_START, NN_STREAM_START_BUSY, NN_SVC_ERR_BUSY, NN_CLAIM_NONE,
	  T_BUSY_NOGATE, T_BUSY_GATE },
	{ OP_START, DEFAULT_ROW, NN_SVC_ERR_BUSY, NN_CLAIM_NONE,
	  T_BUSY_NOGATE, T_BUSY_GATE },
	/* stop (nn_core_claim_stop) */
	{ OP_STOP, NN_STREAM_STOP_IDLE, NN_SVC_ERR_STATE, NN_CLAIM_NONE,
	  "not running", "not running" },
	{ OP_STOP, NN_STREAM_STOP_WRONG_GEN, NN_SVC_ERR_GEN, NN_CLAIM_NONE,
	  "that stream has already been replaced by another",
	  "that stream has already been replaced by another" },
	{ OP_STOP, NN_STREAM_STOP_BUSY, NN_SVC_ERR_BUSY, NN_CLAIM_NONE,
	  "a start or another stop owns the stream -- retry",
	  "a start or another stop owns the stream -- retry" },
	{ OP_STOP, NN_STREAM_STOP_DEAD, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL,
	  T_DEAD, T_DEAD },
	{ OP_STOP, NN_STREAM_STOP_ONESHOT, NN_SVC_ERR_BUSY, NN_CLAIM_NONE,
	  "a `nn run` holds the camera and stops it itself -- retry when it "
	  "returns",
	  "a `nn run` holds the camera and stops it itself -- retry when it "
	  "returns" },
	/* [!] The one answer issue #174 changed: wio-lite-ai and f746g-disco
	 * answered an unknown stop STATE, NONE (decided with the user). */
	{ OP_STOP, DEFAULT_ROW, NN_SVC_ERR_BUSY, NN_CLAIM_NONE,
	  "a start or another stop owns the stream -- retry",
	  "a start or another stop owns the stream -- retry" },
};
#define N_ROWS (sizeof rows / sizeof rows[0])

/* The defined answers of each enum, GO included; everything else is "outside". */
static int is_defined(int op, int why)
{
	if (op == OP_STOP)
		return why >= (int)NN_STREAM_STOP_GO &&
		       why <= (int)NN_STREAM_STOP_ONESHOT;
	return why >= (int)NN_STREAM_START_GO &&
	       why <= (int)NN_STREAM_START_ONESHOT;
}

static const struct row *row_of(int op, int why)
{
	size_t i;
	int key = is_defined(op, why) ? why : DEFAULT_ROW;

	for (i = 0; i < N_ROWS; i++)
		if (rows[i].op == op && rows[i].why == key)
			return &rows[i];
	return NULL;
}

static int call(int op, int why, const struct nn_core_board *b,
                struct nn_op_result *res)
{
	switch (op) {
	case OP_RUN:
		return nn_refusal_run((enum nn_stream_start_claim)why, b, res);
	case OP_START:
		return nn_refusal_start((enum nn_stream_start_claim)why, b, res);
	default:
		return nn_refusal_stop((enum nn_stream_stop_claim)why, b, res);
	}
}

static void poison(struct nn_op_result *res)
{
	memset(res, 0x5A, sizeof *res);
	res->detail[sizeof res->detail - 1] = '\0';
}

/* ---- (a) + (b): every refusal ------------------------------------------- */
static void test_refusals(void)
{
	/* The values outside the enums that are tried: below, just past, far. */
	static const int outside[] = { -1, 5, 6, 7, 99, INT_MAX, INT_MIN };
	int op, g, k, n_cases = 0;
	size_t longest = 0;

	printf("nn_refusal_{run,start,stop}: every answer x op x gate\n");
	for (op = 0; op < OP_N; op++) {
		int hi = (op == OP_STOP) ? (int)NN_STREAM_STOP_ONESHOT
		                         : (int)NN_STREAM_START_ONESHOT;
		int n_try = hi + 1 + (int)(sizeof outside / sizeof outside[0]);

		for (k = 0; k < n_try; k++) {
			int why = (k <= hi) ? k : outside[k - hi - 1];

			for (g = 0; g <= 1; g++) {
				const struct nn_core_board *b =
					g ? &board_gate : &board_nogate;
				struct nn_op_result res, before;
				char what[160];
				int rc;

				poison(&res);
				before = res;
				rc = call(op, why, b, &res);
				snprintf(what, sizeof what, "%s why %d gate %d",
				         op_name[op], why, g);
				n_cases++;
				if (why == 0) {   /* GO: not a refusal */
					CHECK(rc == 0, what);
					CHECK(memcmp(&res, &before, sizeof res) == 0,
					      what);
					continue;
				}
				{
					const struct row *r = row_of(op, why);
					const char *want;

					CHECK(r != NULL, what);
					if (r == NULL)
						continue;
					want = g ? r->gate : r->nogate;
					CHECK(rc != 0, what);
					CHECK(res.status == r->status, what);
					CHECK(res.claim == (uint8_t)r->claim, what);
					CHECK(strcmp(res.detail, want) == 0, what);
					CHECK(strlen(want) <= NN_SVC_DETAIL_MAX, what);
					CHECK(strlen(res.detail) <= NN_SVC_DETAIL_MAX,
					      what);
					if (strcmp(res.detail, want) != 0)
						printf("    got  \"%s\"\n    want \"%s\"\n",
						       res.detail, want);
					if (strlen(res.detail) > longest)
						longest = strlen(res.detail);
				}
			}
		}
	}
	/* A NULL result is refused without a write, and still answers. */
	CHECK(nn_refusal_start(NN_STREAM_START_DEAD, &board_nogate, NULL) != 0,
	      "start DEAD, NULL result: still a refusal");
	CHECK(nn_refusal_stop(NN_STREAM_STOP_GO, &board_nogate, NULL) == 0,
	      "stop GO, NULL result: not a refusal");
	/* A NULL board has no gate. */
	{
		struct nn_op_result res;

		poison(&res);
		(void)nn_refusal_run(NN_STREAM_START_BUSY, NULL, &res);
		CHECK(strcmp(res.detail, T_BUSY_NOGATE) == 0,
		      "run BUSY, NULL board: the no-gate sentence");
	}
	printf("  %d refusal cases, longest sentence %zu of %d\n", n_cases,
	       longest, NN_SVC_DETAIL_MAX);
}

/* ---- (b) once more, as the boards' nine switches read ------------------- */
/*
 * The rows above are the oracle for (a); this walks the same values from the
 * other side -- one line per case of the switches as they stood on main
 * 7865ce4 -- so a row edited in both places at once still has to agree with
 * this list.
 */
static void test_status_table(void)
{
	static const struct {
		int op, why, status, claim;
	} sw[] = {
		{ OP_RUN,   NN_STREAM_START_RUNNING,  NN_SVC_ERR_BUSY,  NN_CLAIM_NONE },
		{ OP_RUN,   NN_STREAM_START_ONESHOT,  NN_SVC_ERR_BUSY,  NN_CLAIM_NONE },
		{ OP_RUN,   NN_STREAM_START_DEAD,     NN_SVC_ERR_HW,    NN_CLAIM_TERMINAL },
		{ OP_RUN,   NN_STREAM_START_BUSY,     NN_SVC_ERR_BUSY,  NN_CLAIM_NONE },
		{ OP_START, NN_STREAM_START_RUNNING,  NN_SVC_ERR_STATE, NN_CLAIM_NONE },
		{ OP_START, NN_STREAM_START_ONESHOT,  NN_SVC_ERR_BUSY,  NN_CLAIM_NONE },
		{ OP_START, NN_STREAM_START_DEAD,     NN_SVC_ERR_HW,    NN_CLAIM_TERMINAL },
		{ OP_START, NN_STREAM_START_BUSY,     NN_SVC_ERR_BUSY,  NN_CLAIM_NONE },
		{ OP_STOP,  NN_STREAM_STOP_IDLE,      NN_SVC_ERR_STATE, NN_CLAIM_NONE },
		{ OP_STOP,  NN_STREAM_STOP_WRONG_GEN, NN_SVC_ERR_GEN,   NN_CLAIM_NONE },
		{ OP_STOP,  NN_STREAM_STOP_BUSY,      NN_SVC_ERR_BUSY,  NN_CLAIM_NONE },
		{ OP_STOP,  NN_STREAM_STOP_DEAD,      NN_SVC_ERR_HW,    NN_CLAIM_TERMINAL },
		{ OP_STOP,  NN_STREAM_STOP_ONESHOT,   NN_SVC_ERR_BUSY,  NN_CLAIM_NONE },
		/* outside the enum: BUSY, NONE for all three (issue #174) */
		{ OP_RUN,   99,                       NN_SVC_ERR_BUSY,  NN_CLAIM_NONE },
		{ OP_START, 99,                       NN_SVC_ERR_BUSY,  NN_CLAIM_NONE },
		{ OP_STOP,  99,                       NN_SVC_ERR_BUSY,  NN_CLAIM_NONE },
	};
	size_t i;
	int g;

	printf("nn_refusal: status and claim as the nine switches gave them\n");
	for (i = 0; i < sizeof sw / sizeof sw[0]; i++) {
		for (g = 0; g <= 1; g++) {
			struct nn_op_result res;
			char what[96];

			poison(&res);
			(void)call(sw[i].op, sw[i].why,
			           g ? &board_gate : &board_nogate, &res);
			snprintf(what, sizeof what, "%s why %d gate %d: %d / %d",
			         op_name[sw[i].op], sw[i].why, g, sw[i].status,
			         sw[i].claim);
			CHECK(res.status == sw[i].status &&
			      res.claim == (uint8_t)sw[i].claim, what);
		}
	}
	printf("  %zu switch cases x 2 boards\n", sizeof sw / sizeof sw[0]);
}

/* ---- (a) + (c): `nn run`'s endings -------------------------------------- */

/* Exactly the room left after the longest base sentence (159 - 56 = 103). */
#define CLAUSE_MAX_LIT \
	"; 0123456789012345678901234567890123456789012345678901234567890123456789" \
	"012345678901234567890123456789a"
#ifdef NN_REFUSAL_NEG_CLAUSE
static const char *const clause_over =
	NN_REFUSAL_RUN_UNFINISHED(CLAUSE_MAX_LIT "b");   /* must not compile */
#endif

static void base_of(int end, uint32_t ms, char *out, size_t cap, int *status)
{
	switch (end) {
	case NN_RUN_LOST:
		snprintf(out, cap, "the camera stream was lost before an "
		         "inference completed");
		*status = NN_SVC_ERR_HW;
		break;
	case NN_RUN_CANCELLED:
		snprintf(out, cap, "cancelled before an inference completed");
		*status = NN_SVC_ERR_CANCEL;
		break;
	default:   /* TIMEOUT, and anything the caller should not pass */
		snprintf(out, cap, "no inference completed within %lu ms",
		         (unsigned long)ms);
		*status = NN_SVC_ERR_TIMEOUT;
		break;
	}
}

static void test_run_end(void)
{
	static const int ends[] = {
		NN_RUN_WAITING, NN_RUN_INFERRED, NN_RUN_NO_RESULT, NN_RUN_LOST,
		NN_RUN_CANCELLED, NN_RUN_TIMEOUT, -1, 6, 99, INT_MAX,
	};
	static const uint32_t waits[] = { 0u, 1u, 1000u, 10000u, UINT32_MAX };
	static const int claims[] = {
		NN_CLAIM_NONE, NN_CLAIM_CALLER, NN_CLAIM_RETRYABLE,
		NN_CLAIM_TERMINAL,
	};
	const char *const clauses[] = {
		NULL, "",
		NN_REFUSAL_RUN_UNFINISHED("; the teardown did not finish either "
		                          "(`nn stream stop` finishes it)"),
		NN_REFUSAL_RUN_UNFINISHED(CLAUSE_MAX_LIT),
	};
	size_t e, w, c, u;
	int n_cases = 0;
	size_t longest = 0;

	printf("nn_refusal_run_end: every end x wait x claim x clause\n");
	CHECK(strlen(CLAUSE_MAX_LIT) ==
	      (size_t)NN_SVC_DETAIL_MAX - NN_REFUSAL_RUN_END_MAX,
	      "the widest clause the check admits is exactly the room left");
	for (e = 0; e < sizeof ends / sizeof ends[0]; e++)
	for (w = 0; w < sizeof waits / sizeof waits[0]; w++)
	for (c = 0; c < sizeof claims / sizeof claims[0]; c++)
	for (u = 0; u < sizeof clauses / sizeof clauses[0]; u++) {
		struct nn_op_result res;
		char want[2 * NN_SVC_DETAIL_MAX], base[96], what[128];
		int want_status, rc;

		base_of(ends[e], waits[w], base, sizeof base, &want_status);
		snprintf(want, sizeof want, "%s%s", base,
		         (claims[c] != NN_CLAIM_NONE && clauses[u] != NULL)
		         ? clauses[u] : "");
		poison(&res);
		rc = nn_refusal_run_end((enum nn_run_end)ends[e], waits[w],
		                        (enum nn_claim)claims[c], clauses[u],
		                        &res);
		snprintf(what, sizeof what, "end %d wait %lu claim %d clause %zu",
		         ends[e], (unsigned long)waits[w], claims[c], u);
		n_cases++;
		CHECK(rc == want_status, what);
		CHECK(res.status == want_status, what);
		CHECK(res.claim == (uint8_t)claims[c], what);
		CHECK(strlen(want) <= NN_SVC_DETAIL_MAX, what);
		CHECK(strcmp(res.detail, want) == 0, what);
		if (strcmp(res.detail, want) != 0)
			printf("    got  \"%s\"\n    want \"%s\"\n", res.detail,
			       want);
		if (strlen(res.detail) > longest)
			longest = strlen(res.detail);
	}
	/* The longest sentence there is fills the field to the last character. */
	CHECK(longest == NN_SVC_DETAIL_MAX,
	      "the longest base + the widest clause is exactly the bound");
	CHECK(nn_refusal_run_end(NN_RUN_LOST, 0u, NN_CLAIM_NONE, NULL, NULL) ==
	      NN_SVC_ERR_HW, "NULL result: the status, nothing written");
	printf("  %d ending cases, longest sentence %zu of %d\n", n_cases,
	       longest, NN_SVC_DETAIL_MAX);
}

/* ---- (d): the constant a board's clause check is built on -------------- */
static void test_run_end_max(void)
{
	static const int ends[] = {
		NN_RUN_WAITING, NN_RUN_INFERRED, NN_RUN_NO_RESULT, NN_RUN_LOST,
		NN_RUN_CANCELLED, NN_RUN_TIMEOUT, -1, 99,
	};
	size_t e, longest = 0;

	printf("NN_REFUSAL_RUN_END_MAX: no base sentence is longer, the "
	       "longest is equal\n");
	for (e = 0; e < sizeof ends / sizeof ends[0]; e++) {
		struct nn_op_result res;
		char what[96];

		poison(&res);
		/* The number at its widest: a uint32_t's ten digits. */
		(void)nn_refusal_run_end((enum nn_run_end)ends[e], UINT32_MAX,
		                         NN_CLAIM_NONE, NULL, &res);
		snprintf(what, sizeof what, "end %d: %zu <= %u", ends[e],
		         strlen(res.detail), (unsigned)NN_REFUSAL_RUN_END_MAX);
		CHECK(strlen(res.detail) <= NN_REFUSAL_RUN_END_MAX, what);
		if (strlen(res.detail) > longest)
			longest = strlen(res.detail);
	}
	CHECK(longest == NN_REFUSAL_RUN_END_MAX,
	      "the longest base sentence is exactly NN_REFUSAL_RUN_END_MAX");
	printf("  longest base sentence %zu, constant %u\n", longest,
	       (unsigned)NN_REFUSAL_RUN_END_MAX);
}

int main(void)
{
	test_refusals();
	test_status_table();
	test_run_end();
	test_run_end_max();
	if (fails) {
		printf("test_nn_refusal: %d of %d checks FAILED\n", fails, checks);
		return 1;
	}
	printf("test_nn_refusal: all %d checks passed\n", checks);
	return 0;
}
