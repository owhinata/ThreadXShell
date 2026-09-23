/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's load ending table (issue #122,
 * port/nn/nn_load_end.c).
 *
 * WHY THIS EXISTS.  A load either leaves the new model, rolls back to the
 * previous one, or leaves nothing -- and a new model whose plugin is refused is
 * a fourth answer: open, with no decoder, and a failure (D6).  Each obliges
 * something different: the state the operator is told, the status, whether the
 * plugin is unpublished, whether the last result goes, whose container claims
 * describe what is open.  Two of the rows cannot be produced on hardware.
 *
 * P2 lives here too.  "Nothing was loaded and the new model was refused" is
 * model_after == 0, and it must read EMPTY -- until #122 the board reported
 * PREVIOUS there because an empty TFLM singleton counts as open.
 *
 * Two halves: every row spelled out, and rules the table must obey over every
 * input.  [!] WHAT IT DOES NOT COVER: that the adapter swaps the plugin only
 * when nn_load_swaps_plugin() says so, and acts on every flag -- one call site
 * in port/nn/nn_svc_wio.c holds that.
 */
#include <stdio.h>

#include "nn_svc.h"
#include "nn_load_end.h"

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

static void row(const char *what, int rc, int after, int refused, int swaps,
                unsigned st, int ok, int unload, int invalidate, int claims)
{
	struct nn_load_verdict v;
	int s = nn_load_swaps_plugin(rc, after) ? 1 : 0;

	nn_load_decide(rc, after, refused, &v);
	if (s != swaps || v.state != st || v.ok != ok || v.unload != unload ||
	    v.invalidate != invalidate || v.claims != claims) {
		printf("  FAIL %-40s -> swap%d %s ok%u unload%u inval%u claims%u; "
		       "wanted swap%d %s ok%d unload%d inval%d claims%d\n", what,
		       s, state_name(v.state), v.ok, v.unload, v.invalidate,
		       v.claims, swaps, state_name(st), ok, unload, invalidate,
		       claims);
		fails++;
	}
}

static void rules(int rc, int after, int refused)
{
	struct nn_load_verdict v;
	char what[64];

	nn_load_decide(rc, after, refused, &v);
	(void)snprintf(what, sizeof what, "rc=%d after=%d refused=%d", rc, after,
	               refused);
#define RULE(cond, text)                                                     \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("  FAIL %-32s %s\n", what, text);             \
			fails++;                                             \
		}                                                            \
	} while (0)
	RULE(v.state == NN_MODEL_EMPTY || v.state == NN_MODEL_NEW ||
	     v.state == NN_MODEL_PREVIOUS, "a state the operator has a word for");
	RULE(v.state != NN_MODEL_PREVIOUS ||
	     (!v.ok && !v.unload && !v.invalidate &&
	      v.claims == NN_LOAD_CLAIMS_KEEP), "PREVIOUS touches nothing");
	RULE(v.state == NN_MODEL_PREVIOUS || v.invalidate,
	     "a change invalidates the record");
	RULE(v.state != NN_MODEL_EMPTY ||
	     (v.unload && v.claims == NN_LOAD_CLAIMS_NONE),
	     "EMPTY leaves no decoder and no claims");
	RULE(!after || v.state != NN_MODEL_EMPTY, "a model left is not EMPTY");
	RULE(after || v.state == NN_MODEL_EMPTY, "no model left is EMPTY");
	RULE(v.state != NN_MODEL_NEW || v.claims == NN_LOAD_CLAIMS_NEW,
	     "NEW takes this load's claims");
	RULE(!v.ok || (v.state == NN_MODEL_NEW && !v.unload && rc == 0),
	     "success is NEW, from a reload that succeeded, decoder kept");
	RULE(!v.ok || !refused, "a refused plugin is never success");
	/* The plugin is swapped exactly when the ending can be NEW. */
	RULE(nn_load_swaps_plugin(rc, after) == (rc == 0 && after != 0),
	     "the plugin moves only when the backend took the model");
#undef RULE
}

int main(void)
{
	static const int rcs[] = { 0, -1, -7, -73 };
	unsigned i;
	int a, r;

	printf("test_nn_load_end\n");

	/*                                          rc  aft ref swap state             ok un iv claims */
	row("new model, plugin started or none",     0, 1, 0, 1, NN_MODEL_NEW,      1, 0, 1, NN_LOAD_CLAIMS_NEW);
	row("new model, plugin refused (D6)",        0, 1, 1, 1, NN_MODEL_NEW,      0, 1, 1, NN_LOAD_CLAIMS_NEW);
	row("refused, previous rebuilt",            -7, 1, 0, 0, NN_MODEL_PREVIOUS, 0, 0, 0, NN_LOAD_CLAIMS_KEEP);
	row("refused, refused flag ignored",        -7, 1, 1, 0, NN_MODEL_PREVIOUS, 0, 0, 0, NN_LOAD_CLAIMS_KEEP);
	row("refused, previous lost",               -7, 0, 0, 0, NN_MODEL_EMPTY,    0, 1, 1, NN_LOAD_CLAIMS_NONE);
	row("first load refused (P2)",              -5, 0, 0, 0, NN_MODEL_EMPTY,    0, 1, 1, NN_LOAD_CLAIMS_NONE);
	row("success that left no model",            0, 0, 0, 0, NN_MODEL_EMPTY,    0, 1, 1, NN_LOAD_CLAIMS_NONE);
	row("model_after = 2 is a model",            0, 2, 0, 1, NN_MODEL_NEW,      1, 0, 1, NN_LOAD_CLAIMS_NEW);

	for (i = 0; i < sizeof rcs / sizeof rcs[0]; i++)
		for (a = 0; a <= 2; a++)
			for (r = 0; r <= 1; r++)
				rules(rcs[i], a, r);

	if (fails) {
		printf("test_nn_load_end: %d FAILED\n", fails);
		return 1;
	}
	printf("test_nn_load_end: all passed\n");
	return 0;
}
