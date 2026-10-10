/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the load's ending table (issue #122; shared by every board
 * since issue #131, svc/nn_swap.c).
 *
 * WHY THIS EXISTS.  A load over an open model is a replacement with a rollback,
 * and every ending obliges something different: which model the operator is
 * told is open, whether the status is a failure, whose identity the adapter
 * keeps, whether the plugin is unpublished, whether the NPU comes down, whether
 * the last result goes.  Several of the endings cannot be typed at all -- a
 * backend that refuses the new model and then the one it was running a moment
 * ago, a plugin the device refuses after the host packer ran the device's own
 * validator over it, a "restored" with nothing to restore -- and the one the
 * operator reaches most easily (a refused name over an open model) must touch
 * nothing.
 *
 * FOUR PARTS.
 *   1. grove-vision-ai-v2's table: the ending it names itself x had_open, every
 *      entry spelled out (moved here from that board's test/test_nn_swap.c).
 *   2. wio-lite-ai's table: the reload's rc x whether a model was left x a
 *      refused plugin, through nn_swap_end_of() with had_open = 1 as that
 *      adapter passes it (moved from its test/test_nn_load_end.c).  Its
 *      "claims" column is commit (NEW) / forget (NONE) / neither (KEEP).
 *   3. f746g-disco's: the reload's rc x whether a model was left, with no
 *      plugin; the state and the invalidation come from the table, the status
 *      from rc.  The one input where those disagree is unreachable, and says so.
 *   4. The rules the table must obey whatever its entries say, over every input
 *      of both ways in, including endings it does not know.
 * A change that moves an obligation from one row to another passes the rules
 * and fails a table; one that invents a row the rules forbid fails both.
 *
 * [!] WHAT IT DOES NOT COVER.  This compiles nn_swap.c and nothing else, so it
 * says nothing about whether an adapter swaps the plugin after the backend,
 * reopens the previous model from the committed address, or acts on every flag
 * it has a use for.  One call site per board, kept short, is what holds those.
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

/* ---- 1. grove-vision-ai-v2: the ending it names ------------------------- */

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

/* ---- 2. wio-lite-ai: one reload call ------------------------------------ */

/* wio-lite-ai's old verdict had a three-valued "claims"; the table's commit and
 * forget are the same three values, and the adapter reads them that way. */
enum { CLAIMS_KEEP = 0, CLAIMS_NEW, CLAIMS_NONE };

static int claims_of(const struct nn_swap_verdict *v)
{
	if (v->commit)
		return CLAIMS_NEW;
	if (v->forget)
		return CLAIMS_NONE;
	return CLAIMS_KEEP;
}

static void wio_row(const char *what, int rc, int after, int refused,
                    int swaps, unsigned st, int ok, int unload,
                    int invalidate, int claims)
{
	struct nn_swap_verdict v;
	int s = nn_swap_swaps_plugin(rc, after) ? 1 : 0;

	/* As port/nn/nn_svc_wio.c calls it: had_open is not read until #131 7d. */
	nn_swap_decide(1, nn_swap_end_of(rc, after, refused), &v);
	if (s != swaps || v.state != st || v.ok != ok || v.unload != unload ||
	    v.invalidate != invalidate || claims_of(&v) != claims) {
		printf("  FAIL wio %-36s -> swap%d %s ok%u unload%u inval%u "
		       "claims%d; wanted swap%d %s ok%d unload%d inval%d "
		       "claims%d\n", what, s, state_name(v.state), v.ok,
		       v.unload, v.invalidate, claims_of(&v), swaps,
		       state_name(st), ok, unload, invalidate, claims);
		fails++;
	}
}

/* ---- 3. f746g-disco: one reload call, no plugin ------------------------- */

/*
 * The adapter takes the state and the invalidation from the table and the
 * status from rc (port/nn/nn_svc_f746.c).  @p reachable says whether this
 * board's backends can produce the input at all; on an unreachable one the
 * table's ok and rc's status are allowed to disagree, and only there.
 */
static void f746_row(const char *what, int rc, int open_after, int reachable,
                     unsigned st, int invalidate)
{
	struct nn_swap_verdict v;
	int status_ok = (rc == 0);

	nn_swap_decide(1, nn_swap_end_of(rc, open_after, 0), &v);
	if (v.state != st || v.invalidate != invalidate) {
		printf("  FAIL f746 %-35s -> %s inval%u; wanted %s inval%d\n",
		       what, state_name(v.state), v.invalidate, state_name(st),
		       invalidate);
		fails++;
	}
	/* The adapter used to invalidate exactly when the state was not
	 * PREVIOUS; the table must give the same answer on every input. */
	if ((v.state != NN_MODEL_PREVIOUS) != (v.invalidate != 0)) {
		printf("  FAIL f746 %-35s invalidation is not 'state != PREVIOUS'\n",
		       what);
		fails++;
	}
	if (reachable && (v.ok != 0) != status_ok) {
		printf("  FAIL f746 %-35s table ok%u but rc says %s\n", what,
		       v.ok, status_ok ? "ok" : "failed");
		fails++;
	}
	if (!reachable && (v.ok != 0) == status_ok) {
		printf("  FAIL f746 %-35s marked unreachable but the two agree -- "
		       "update this row\n", what);
		fails++;
	}
}

/* ---- 4. the rules ------------------------------------------------------- */

#define RULE(cond, text)                                                     \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("  FAIL %-32s %s\n", what, text);             \
			fails++;                                             \
		}                                                            \
	} while (0)

static void rules(int had_open, int end)
{
	struct nn_swap_verdict v;
	char what[64];

	nn_swap_decide(had_open, (enum nn_swap_end)end, &v);
	(void)snprintf(what, sizeof what, "had_open=%d end=%d", had_open, end);

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
	RULE(v.state != NN_MODEL_EMPTY || (v.unload && v.forget),
	     "EMPTY leaves no decoder and no identity");
	RULE(!v.commit || (v.state == NN_MODEL_NEW && !v.forget && !v.hw_down),
	     "commit is NEW and keeps the NPU");
	RULE(v.state != NN_MODEL_NEW || v.commit, "NEW commits the identity");
	RULE(!v.ok || (v.state == NN_MODEL_NEW && v.commit && !v.unload),
	     "success is NEW with its plugin kept");
}

/* The way in through nn_swap_end_of(), over every (rc, after, refused). */
static void end_of_rules(int rc, int after, int refused)
{
	enum nn_swap_end e = nn_swap_end_of(rc, after, refused);
	struct nn_swap_verdict v;
	char what[64];
	int h;

	(void)snprintf(what, sizeof what, "rc=%d after=%d refused=%d", rc,
	               after, refused);

	RULE(e == NN_SWAP_LOST || e == NN_SWAP_RESTORED ||
	     e == NN_SWAP_UNDECODED || e == NN_SWAP_OPENED,
	     "an ending after the backend (never REFUSED)");
	RULE(after || e == NN_SWAP_LOST, "no model left is LOST");
	RULE(!after || e != NN_SWAP_LOST, "a model left is not LOST");
	RULE(e != NN_SWAP_RESTORED || rc != 0, "RESTORED is a refusal");
	RULE((e == NN_SWAP_UNDECODED || e == NN_SWAP_OPENED) ==
	     (nn_swap_swaps_plugin(rc, after) != 0),
	     "the plugin moves exactly when the ending can be NEW");
	RULE(nn_swap_swaps_plugin(rc, after) == (rc == 0 && after != 0),
	     "the plugin moves only when the backend took the model");
	RULE(e != NN_SWAP_UNDECODED || refused, "UNDECODED needs a refusal");
	RULE(e != NN_SWAP_OPENED || !refused, "a refused plugin is never OPENED");

	for (h = 0; h <= 1; h++) {
		nn_swap_decide(h, e, &v);
		RULE(!v.ok || !refused, "a refused plugin is never success");
		RULE(!v.ok || rc == 0, "success needs a reload that succeeded");
		RULE(!after || v.state != NN_MODEL_EMPTY ||
		     (e == NN_SWAP_RESTORED && !h),
		     "a model left is not EMPTY (except a contradicted RESTORED)");
		RULE(after || v.state == NN_MODEL_EMPTY, "no model left is EMPTY");
	}
}
#undef RULE

int main(void)
{
	static const int rcs[] = { 0, -1, -2, -5, -7, -10, -73, 1 };
	unsigned i;
	int h, e, a, r;

	printf("test_nn_swap\n");

	/* 1. grove-vision-ai-v2                     state              ok cm fg un dn iv */
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

	/* 2. wio-lite-ai                                rc  aft ref swap state             ok un iv claims */
	wio_row("new model, plugin started or none",     0, 1, 0, 1, NN_MODEL_NEW,      1, 0, 1, CLAIMS_NEW);
	wio_row("new model, plugin refused (D6)",        0, 1, 1, 1, NN_MODEL_NEW,      0, 1, 1, CLAIMS_NEW);
	wio_row("refused, previous rebuilt",            -7, 1, 0, 0, NN_MODEL_PREVIOUS, 0, 0, 0, CLAIMS_KEEP);
	wio_row("refused, refused flag ignored",        -7, 1, 1, 0, NN_MODEL_PREVIOUS, 0, 0, 0, CLAIMS_KEEP);
	wio_row("refused, previous lost",               -7, 0, 0, 0, NN_MODEL_EMPTY,    0, 1, 1, CLAIMS_NONE);
	wio_row("first load refused (P2)",              -5, 0, 0, 0, NN_MODEL_EMPTY,    0, 1, 1, CLAIMS_NONE);
	wio_row("success that left no model",            0, 0, 0, 0, NN_MODEL_EMPTY,    0, 1, 1, CLAIMS_NONE);
	wio_row("model_after = 2 is a model",            0, 2, 0, 1, NN_MODEL_NEW,      1, 0, 1, CLAIMS_NEW);

	/* 3. f746g-disco                                rc  open reach state            iv */
	f746_row("--path or builtin taken",              0, 1, 1, NN_MODEL_NEW,      1);
	f746_row("refused, previous rebuilt (tflm)",   -10, 1, 1, NN_MODEL_PREVIOUS, 0);
	f746_row("backend cannot swap (-2)",            -2, 1, 1, NN_MODEL_PREVIOUS, 0);
	f746_row("refused, previous lost",              -7, 0, 1, NN_MODEL_EMPTY,    1);
	/* [!] UNREACHABLE: every f746 backend's reload hands back a NULL handle
	 * only together with a failure (tflm: the catastrophic restore; the reloc
	 * backend never does), so rc == 0 with no model left cannot happen.  The
	 * table says EMPTY and failed; the adapter would say EMPTY and ok. */
	f746_row("success that left no model",           0, 0, 0, NN_MODEL_EMPTY,    1);

	/* 4. the rules */
	for (h = 0; h <= 2; h++)
		for (e = -1; e <= NN_SWAP_OPENED + 1; e++)
			rules(h, e);
	for (i = 0; i < sizeof rcs / sizeof rcs[0]; i++)
		for (a = 0; a <= 2; a++)
			for (r = 0; r <= 1; r++)
				end_of_rules(rcs[i], a, r);

	if (fails) {
		printf("test_nn_swap: %d FAILED\n", fails);
		return 1;
	}
	printf("test_nn_swap: all passed\n");
	return 0;
}
