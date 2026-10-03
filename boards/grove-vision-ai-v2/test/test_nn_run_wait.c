/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for how `nn run`'s wait ends (issue #129, port/npu/nn_run_wait.c).
 *
 * WHY THIS EXISTS.  Several of the endings can hold in the same poll -- a
 * publish and the deadline, a publish and a Ctrl+C, a lost stream and a
 * finished worker -- and only the order decides which status the operator gets.
 * None of those coincidences can be typed.  So every one of the 32 combinations
 * is checked against the rule written out here, and the promotion after the
 * wait against every ending.
 */
#include <stdio.h>

#include "nn_run_wait.h"

static int fails;

static const char *end_name(int e)
{
	switch (e) {
	case NN_RUN_WAITING:   return "WAITING";
	case NN_RUN_INFERRED:  return "INFERRED";
	case NN_RUN_NO_RESULT: return "NO_RESULT";
	case NN_RUN_LOST:      return "LOST";
	case NN_RUN_CANCELLED: return "CANCELLED";
	case NN_RUN_TIMEOUT:   return "TIMEOUT";
	default:               return "?";
	}
}

static void check(const char *what, int got, int want)
{
	if (got != want) {
		printf("  FAIL %-58s -> %s, wanted %s\n", what, end_name(got),
		       end_name(want));
		fails++;
	} else {
		printf("  ok   %-58s %s\n", what, end_name(got));
	}
}

int main(void)
{
	unsigned m;
	int e;

	printf("nn_run_wait_step: every combination\n");
	for (m = 0u; m < 32u; m++) {
		int pub = (m >> 0) & 1, fin = (m >> 1) & 1, lost = (m >> 2) & 1;
		int can = (m >> 3) & 1, exp = (m >> 4) & 1;
		int want;
		char what[80];

		/* The rule, stated once more on purpose: what the table is held to. */
		want = pub  ? NN_RUN_INFERRED  :
		       fin  ? NN_RUN_NO_RESULT :
		       lost ? NN_RUN_LOST      :
		       can  ? NN_RUN_CANCELLED :
		       exp  ? NN_RUN_TIMEOUT   : NN_RUN_WAITING;
		snprintf(what, sizeof what, "pub %d fin %d lost %d cancel %d "
		         "expired %d", pub, fin, lost, can, exp);
		check(what, nn_run_wait_step(pub, fin, lost, can, exp), want);
	}

	printf("named cases\n");
	/* [!] The ones that matter most, spelled out. */
	check("a result that made it beats the deadline",
	      nn_run_wait_step(1, 1, 0, 0, 1), NN_RUN_INFERRED);
	check("a result that made it beats a Ctrl+C",
	      nn_run_wait_step(1, 1, 0, 1, 0), NN_RUN_INFERRED);
	check("a finished worker is why nothing came, not a timeout",
	      nn_run_wait_step(0, 1, 0, 0, 1), NN_RUN_NO_RESULT);
	check("nothing yet is waiting", nn_run_wait_step(0, 0, 0, 0, 0),
	      NN_RUN_WAITING);

	printf("nn_run_wait_final: only a timeout is promoted\n");
	for (e = NN_RUN_WAITING; e <= NN_RUN_TIMEOUT; e++) {
		char what[80];

		snprintf(what, sizeof what, "%s, published after all",
		         end_name(e));
		check(what, nn_run_wait_final((enum nn_run_end)e, 1),
		      e == NN_RUN_TIMEOUT ? NN_RUN_INFERRED : e);
		snprintf(what, sizeof what, "%s, still nothing published",
		         end_name(e));
		check(what, nn_run_wait_final((enum nn_run_end)e, 0), e);
	}

	if (fails) {
		printf("FAILED (%d)\n", fails);
		return 1;
	}
	printf("test_nn_run_wait: all cases pass\n");
	return 0;
}
