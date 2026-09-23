/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the load's ending table (issue #122, port/npu/nn_swap.c).
 *
 * WHY THIS EXISTS.  A load over an open model is a replacement with a rollback,
 * and every ending obliges something different: which model the operator is
 * told is open, whether the status is a failure, whose identity the adapter
 * keeps, whether the plugin is unpublished, whether the NPU comes down, whether
 * the last result goes.  Two of the endings cannot be typed at all -- a backend
 * that refuses the new model and then the one it was running a moment ago, and
 * a "restored" with nothing to restore -- and the one the operator reaches most
 * easily (a refused name over an open model) must touch nothing.
 *
 * Two halves: the table, every entry spelled out; and the rules the table must
 * obey whatever its entries say, over every input including ones it does not
 * know.  A change that moves an obligation from one row to another passes the
 * rules and fails the table; one that invents a row the rules forbid fails both.
 *
 * [!] WHAT IT DOES NOT COVER.  This compiles nn_swap.c and nothing else, so it
 * says nothing about whether the adapter swaps the plugin after the backend,
 * reopens the previous model from the committed address, or acts on every flag.
 * One call site, kept short, is what holds those.
 */
#include <stdio.h>

#include "nn_svc.h"
#include "nn_swap.h"

static int fails;

static const char *state_name(unsigned s)
{
	switch (s) {
	case NN_MODEL_EMPTY:    return "EMPTY";
	case NN_MODEL_NEW:      return "NEW";
	case NN_MODEL_PREVIOUS: return "PREVIOUS";
	default:                return "?";
	}
}

/* want: state ok commit forget unload hw_down invalidate */
static void row(const char *what, int had_open, int end, unsigned st,
                int ok, int commit, int forget, int unload, int hw_down,
                int invalidate)
{
	struct nn_swap_verdict v;

	nn_swap_decide(had_open, (enum nn_swap_end)end, &v);
	if (v.state != st || v.ok != ok || v.commit != commit ||
	    v.forget != forget || v.unload != unload || v.hw_down != hw_down ||
	    v.invalidate != invalidate) {
		printf("  FAIL %-46s -> %s ok%u commit%u forget%u unload%u down%u "
		       "inval%u; wanted %s ok%d commit%d forget%d unload%d down%d "
		       "inval%d\n", what, state_name(v.state), v.ok, v.commit,
		       v.forget, v.unload, v.hw_down, v.invalidate, state_name(st),
		       ok, commit, forget, unload, hw_down, invalidate);
		fails++;
	}
}

static void rules(int had_open, int end)
{
	struct nn_swap_verdict v;
	char what[64];

	nn_swap_decide(had_open, (enum nn_swap_end)end, &v);
	(void)snprintf(what, sizeof what, "had_open=%d end=%d", had_open, end);

#define RULE(cond, text)                                                     \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("  FAIL %-24s %s\n", what, text);             \
			fails++;                                             \
		}                                                            \
	} while (0)

	RULE(v.state == NN_MODEL_EMPTY || v.state == NN_MODEL_NEW ||
	     v.state == NN_MODEL_PREVIOUS, "a state the operator has a word for");
	/* PREVIOUS is "nothing moved": no flag at all may be set. */
	RULE(v.state != NN_MODEL_PREVIOUS ||
	     (!v.ok && !v.commit && !v.forget && !v.unload && !v.hw_down &&
	      !v.invalidate), "PREVIOUS touches nothing");
	RULE(v.state != NN_MODEL_PREVIOUS || had_open,
	     "PREVIOUS only when something was open");
	/* Whatever changed what is open takes the last result with it. */
	RULE(v.state == NN_MODEL_PREVIOUS || v.invalidate,
	     "a change invalidates the record");
	/* Down means everything down: no NPU up with no model, no plugin left. */
	RULE(!v.hw_down || (v.forget && v.unload && v.state == NN_MODEL_EMPTY),
	     "hw_down is a whole teardown");
	RULE(v.state != NN_MODEL_EMPTY || v.hw_down, "EMPTY brings the NPU down");
	RULE(!v.commit || (v.state == NN_MODEL_NEW && !v.forget && !v.hw_down),
	     "commit is NEW and keeps the NPU");
	RULE(v.state != NN_MODEL_NEW || v.commit, "NEW commits the identity");
	RULE(!v.ok || (v.state == NN_MODEL_NEW && v.commit && !v.unload),
	     "success is NEW with its plugin kept");
#undef RULE
}

int main(void)
{
	int h, e;

	printf("test_nn_swap\n");

	/*                                          state              ok cm fg un dn iv */
	row("refused over an open model",   1, NN_SWAP_REFUSED,   NN_MODEL_PREVIOUS, 0, 0, 0, 0, 0, 0);
	row("refused with nothing open",    0, NN_SWAP_REFUSED,   NN_MODEL_EMPTY,    0, 0, 1, 1, 1, 1);
	row("backend lost, previous too",   1, NN_SWAP_LOST,      NN_MODEL_EMPTY,    0, 0, 1, 1, 1, 1);
	row("backend refused, none open",   0, NN_SWAP_LOST,      NN_MODEL_EMPTY,    0, 0, 1, 1, 1, 1);
	row("backend refused, restored",    1, NN_SWAP_RESTORED,  NN_MODEL_PREVIOUS, 0, 0, 0, 0, 0, 0);
	row("restored from nothing",        0, NN_SWAP_RESTORED,  NN_MODEL_EMPTY,    0, 0, 1, 1, 1, 1);
	row("plugin refused over open (D6)",1, NN_SWAP_UNDECODED, NN_MODEL_NEW,      0, 1, 0, 1, 0, 1);
	row("plugin refused, first load",   0, NN_SWAP_UNDECODED, NN_MODEL_NEW,      0, 1, 0, 1, 0, 1);
	row("opened over an open model",    1, NN_SWAP_OPENED,    NN_MODEL_NEW,      1, 1, 0, 0, 0, 1);
	row("opened, first load",           0, NN_SWAP_OPENED,    NN_MODEL_NEW,      1, 1, 0, 0, 0, 1);
	row("unknown ending, open",         1, 99,                NN_MODEL_EMPTY,    0, 0, 1, 1, 1, 1);
	row("unknown ending, none open",    0, -1,                NN_MODEL_EMPTY,    0, 0, 1, 1, 1, 1);
	/* had_open is a truth value, not a 0/1 */
	row("had_open = 2 is open",         2, NN_SWAP_REFUSED,   NN_MODEL_PREVIOUS, 0, 0, 0, 0, 0, 0);
	row("had_open = 2, restored",       2, NN_SWAP_RESTORED,  NN_MODEL_PREVIOUS, 0, 0, 0, 0, 0, 0);

	for (h = 0; h <= 2; h++)
		for (e = -1; e <= NN_SWAP_OPENED + 1; e++)
			rules(h, e);

	if (fails) {
		printf("test_nn_swap: %d FAILED\n", fails);
		return 1;
	}
	printf("test_nn_swap: all passed\n");
	return 0;
}
