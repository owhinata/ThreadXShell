/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the order of a model load and unload (issue #131,
 * svc/nn_core_model.c).
 *
 * WHY THIS EXISTS.  test_nn_swap.c walks the table that says where a load
 * ended; it cannot say whether the code around the table acts in the right
 * ORDER -- the plugin swapped only after the backend took the model, a plugin
 * touched only under the lease, the hardware brought down on every ending that
 * leaves nothing open, BUSY answered only before anything changed, the
 * transition counter odd across every change.  None of that shows in a return
 * value, and most of the endings that matter (a backend that refuses the new
 * model and then the previous one, a plugin refused after the host packer
 * validated it) cannot be typed on a console.
 *
 * So the board here is a stand-in whose every hook appends its name to a
 * trace, and whose hooks check where they are called from: inside the critical
 * section (never allowed), a plugin touched without the lease, a change made
 * while the counter is even after the lease was taken.  Every ending is walked
 * and its whole trace compared, and the rules are checked over all of them.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nn_core_model.h"

static int fails;

static void ok(const char *what, int cond)
{
	if (!cond) {
		printf("  FAIL %s\n", what);
		fails++;
	}
}

/* ---- the stand-in board -------------------------------------------------- */

static struct nn_core_model life;
static char trace[512];
static int  cs_depth;
static int  misplaced;     /* a hook called inside the critical section      */
static int  unleased;      /* a plugin touched without the lease              */
static int  even_change;   /* a change while leased and the counter was even  */
static int  claim_held, lease_held, swapped;
static const struct nn_core_model_board board, board_nolease;
static const struct nn_core_model_board *cur = &board;

/* The scenario. */
static int model_open;            /* has_model()                               */
static int r_check, r_admit, r_claim, r_prepare, r_fetch, r_lease;
static int r_swap, r_after, r_plugin, bare;

static void put(const char *s)
{
	if (cs_depth != 0)
		misplaced++;
	if (trace[0] != '\0')
		strncat(trace, " ", sizeof trace - strlen(trace) - 1u);
	strncat(trace, s, sizeof trace - strlen(trace) - 1u);
}

/* A change to what is open: while the lease is held, the counter must be odd. */
static void change(const char *s)
{
	put(s);
	if (lease_held && (life.seq & 1u) == 0u)
		even_change++;
}

static unsigned cs_enter(void)
{
	cs_depth++;
	return 7u;
}

static void cs_exit(unsigned posture)
{
	(void)posture;
	cs_depth--;
	/* the counter's moves, in the trace */
	put((life.seq & 1u) ? "ODD" : "EVEN");
}

static int  b_swap(struct nn_core_model_job *j, int *after)
{
	(void)j;
	change("swap");
	ok("swap only under the lease and the claim",
	   (lease_held || cur->lease_take == NULL) && claim_held &&
	   (life.seq & 1u) != 0u);
	swapped = 1;
	*after = r_after;
	return r_swap;
}
static void b_release(void)      { change("release"); }
static int  b_has(void)          { return model_open; }
static void b_refused(struct nn_core_model_job *j) { (void)j; put("spec_refused"); }
static int  b_check(struct nn_core_model_job *j)   { (void)j; put("check"); return r_check; }
static int  b_admit(struct nn_core_model_job *j)   { (void)j; put("admit"); return r_admit; }
static int  b_claim(void)
{
	put("claim");
	if (r_claim)
		claim_held = 1;
	return r_claim;
}
static void b_unclaim(void)
{
	put("unclaim");
	ok("claim given back only when held", claim_held);
	claim_held = 0;
}
static int  b_prepare(struct nn_core_model_job *j)
{
	put(j->had_open ? "prepare(open)" : "prepare");
	return r_prepare;
}
static int  b_fetch(struct nn_core_model_job *j)
{
	put("fetch");
	j->bare = bare;
	return r_fetch;
}
static int  b_lease(struct nn_core_model_job *j)
{
	(void)j;
	put("lease");
	if (r_lease)
		lease_held = 1;
	return r_lease;
}
static void b_unlease(void)
{
	put("unlease");
	ok("lease given back only when held", lease_held);
	lease_held = 0;
}
static int  b_pstart(struct nn_core_model_job *j)
{
	(void)j;
	change("pstart");
	if (!lease_held)
		unleased++;
	return r_plugin;
}
static void b_punload(void)
{
	change("punload");
	if (!lease_held)
		unleased++;
}
static void b_hwdown(void)       { change("hw_down"); }
static void b_inval(void)        { change("inval"); }
static void b_forget(void)       { change("forget"); }
static void b_commit(struct nn_core_model_job *j) { (void)j; change("commit"); }
static void b_geom(int leased)
{
	change(leased ? "geom(L)" : "geom");
	if (leased && !lease_held)
		unleased++;
}

static const struct nn_core_model_backend backend = {
	.swap = b_swap, .release = b_release, .has_model = b_has,
};

static const struct nn_core_model_board board = {
	.cs_enter = cs_enter, .cs_exit = cs_exit, .backend = &backend,
	.tags = NN_CORE_MODEL_TAG(NN_SPEC_NAME) | NN_CORE_MODEL_TAG(NN_SPEC_ADDR),
	.spec_refused = b_refused, .check_spec = b_check, .admit = b_admit,
	.claim_take = b_claim, .claim_give = b_unclaim, .prepare = b_prepare,
	.fetch = b_fetch, .lease_take = b_lease, .lease_give = b_unlease,
	.plugin_start = b_pstart, .plugin_unload = b_punload,
	.hw_down = b_hwdown, .invalidate = b_inval, .forget = b_forget,
	.commit = b_commit, .geom_clear = b_geom,
};

/* The same board with no plugin lease: it has no plugin, so no plugin hook may
 * ever be called and the geometry's plugin copy is never touched. */
static const struct nn_core_model_board board_nolease = {
	.cs_enter = cs_enter, .cs_exit = cs_exit, .backend = &backend,
	.tags = NN_CORE_MODEL_TAG(NN_SPEC_NAME) | NN_CORE_MODEL_TAG(NN_SPEC_ADDR),
	.spec_refused = b_refused, .check_spec = b_check, .admit = b_admit,
	.claim_take = b_claim, .claim_give = b_unclaim, .prepare = b_prepare,
	.fetch = b_fetch, .lease_take = NULL, .lease_give = NULL,
	.plugin_start = b_pstart, .plugin_unload = b_punload,
	.hw_down = b_hwdown, .invalidate = b_inval, .forget = b_forget,
	.commit = b_commit, .geom_clear = b_geom,
};


static void reset(int open)
{
	trace[0] = '\0';
	misplaced = unleased = even_change = 0;
	claim_held = lease_held = swapped = 0;
	model_open = open;
	r_check = r_admit = r_prepare = r_fetch = r_swap = r_plugin = NN_SVC_OK;
	r_claim = r_lease = 1;
	r_after = 1;
	bare = 0;
}

static const char *state_name(enum nn_model_state s)
{
	switch (s) {
	case NN_MODEL_EMPTY:    return "EMPTY";
	case NN_MODEL_NEW:      return "NEW";
	case NN_MODEL_PREVIOUS: return "PREVIOUS";
	default:                return "?";
	}
}

/* The rules every ending obeys, whatever its trace. */
static void rules(const char *what, int status, uint32_t seq0)
{
	char buf[160];

	snprintf(buf, sizeof buf, "%s: no hook inside the critical section", what);
	ok(buf, misplaced == 0 && cs_depth == 0);
	snprintf(buf, sizeof buf, "%s: no plugin touched without the lease", what);
	ok(buf, unleased == 0);
	snprintf(buf, sizeof buf, "%s: every change while leased is under an odd "
	         "counter", what);
	ok(buf, even_change == 0);
	snprintf(buf, sizeof buf, "%s: the counter ends even", what);
	ok(buf, (life.seq & 1u) == 0u);
	snprintf(buf, sizeof buf, "%s: the counter moves only if the lease was "
	         "taken", what);
	ok(buf, (life.seq != seq0) == (strstr(trace, "ODD") != NULL) &&
	        (life.seq == seq0 || life.seq == seq0 + 2u));
	snprintf(buf, sizeof buf, "%s: claim and lease given back", what);
	ok(buf, !claim_held && !lease_held);
	/* [!] issue #131 stage 8 relies on this. */
	snprintf(buf, sizeof buf, "%s: BUSY only before the backend", what);
	ok(buf, status != NN_SVC_ERR_BUSY || !swapped);
}

static void load(const char *what, int tag, int want_status,
                 enum nn_model_state want_state, const char *want_trace)
{
	struct nn_spec spec;
	struct nn_op_result res;
	struct nn_core_model_job j;
	enum nn_model_state st = NN_MODEL_NEW;
	uint32_t seq0 = life.seq;

	memset(&spec, 0, sizeof spec);
	memset(&res, 0, sizeof res);
	spec.tag = (enum nn_spec_tag)tag;
	res.claim = 0xEEu;
	j.spec = &spec;
	j.res = &res;
	j.board = NULL;
	nn_core_model_load(&life, cur, &j, &st);

	if (res.status != want_status || st != want_state ||
	    strcmp(trace, want_trace) != 0 || res.claim != NN_CLAIM_NONE) {
		printf("  FAIL load: %s\n       got  %d %s [%s]\n       want %d %s [%s]\n",
		       what, res.status, state_name(st), trace, want_status,
		       state_name(want_state), want_trace);
		fails++;
	}
	rules(what, res.status, seq0);
	/* EMPTY after the claim was taken and the preparation stood: the
	 * hardware comes down (the table's hw_down covers every EMPTY row). */
	if (st == NN_MODEL_EMPTY && strstr(trace, "fetch") != NULL)
		ok("EMPTY after the preparation takes the hardware down",
		   strstr(trace, "hw_down") != NULL);
}

static void unload(const char *what, int want_status, const char *want_trace)
{
	struct nn_op_result res;
	struct nn_core_model_job j;
	uint32_t seq0 = life.seq;

	memset(&res, 0, sizeof res);
	j.spec = NULL;
	j.res = &res;
	j.board = NULL;
	nn_core_model_unload(&life, cur, &j);
	if (res.status != want_status || strcmp(trace, want_trace) != 0) {
		printf("  FAIL unload: %s\n       got  %d [%s]\n       want %d [%s]\n",
		       what, res.status, trace, want_status, want_trace);
		fails++;
	}
	rules(what, res.status, seq0);
}

#define N NN_SPEC_NAME

int main(void)
{
	/* ---- refusals before the claim: nothing acquired, nothing changed ---- */
	reset(1);
	load("tag this board does not take", NN_SPEC_PATH, NN_SVC_ERR_SPEC,
	     NN_MODEL_PREVIOUS, "spec_refused");
	reset(0);
	load("tag out of range", 40, NN_SVC_ERR_SPEC, NN_MODEL_EMPTY,
	     "spec_refused");
	reset(1); r_check = NN_SVC_ERR_ARG;
	load("argument refused", N, NN_SVC_ERR_ARG, NN_MODEL_PREVIOUS, "check");
	reset(1); r_admit = NN_SVC_ERR_STATE;
	load("precondition refused", N, NN_SVC_ERR_STATE, NN_MODEL_PREVIOUS,
	     "check admit");
	reset(1); r_claim = 0;
	load("claim refused, open", N, NN_SVC_ERR_BUSY, NN_MODEL_PREVIOUS,
	     "check admit claim");
	reset(0); r_claim = 0;
	load("claim refused, empty", N, NN_SVC_ERR_BUSY, NN_MODEL_EMPTY,
	     "check admit claim");

	/* ---- the preparation: it gives back what it took; no table ---------- */
	reset(0); r_prepare = NN_SVC_ERR_HW;
	load("bring-up refused", N, NN_SVC_ERR_HW, NN_MODEL_EMPTY,
	     "check admit claim prepare unclaim");

	/* ---- REFUSED: before the backend --------------------------------- */
	reset(1); r_fetch = NN_SVC_ERR_ARG;
	load("lookup refused over an open model", N, NN_SVC_ERR_ARG,
	     NN_MODEL_PREVIOUS, "check admit claim prepare(open) fetch unclaim");
	reset(0); r_fetch = NN_SVC_ERR_ARG;
	load("lookup refused from empty", N, NN_SVC_ERR_ARG, NN_MODEL_EMPTY,
	     "check admit claim prepare fetch release hw_down inval forget geom "
	     "unclaim");
	reset(1); r_lease = 0;
	load("lease refused over an open model", N, NN_SVC_ERR_BUSY,
	     NN_MODEL_PREVIOUS,
	     "check admit claim prepare(open) fetch lease unclaim");
	reset(0); r_lease = 0;
	load("lease refused from empty", N, NN_SVC_ERR_BUSY, NN_MODEL_EMPTY,
	     "check admit claim prepare fetch lease release hw_down inval forget "
	     "geom unclaim");

	/* ---- the backend ------------------------------------------------- */
	reset(1); r_swap = NN_SVC_ERR_HW; r_after = 1;
	load("backend refused, previous reopened", N, NN_SVC_ERR_HW,
	     NN_MODEL_PREVIOUS,
	     "check admit claim prepare(open) fetch lease ODD swap EVEN unlease "
	     "unclaim");
	reset(1); r_swap = NN_SVC_ERR_HW; r_after = 0;
	load("backend refused, previous lost", N, NN_SVC_ERR_HW, NN_MODEL_EMPTY,
	     "check admit claim prepare(open) fetch lease ODD swap punload release "
	     "hw_down inval forget geom(L) EVEN unlease unclaim");
	reset(0); r_swap = NN_SVC_ERR_HW; r_after = 0;
	load("backend refused from empty", N, NN_SVC_ERR_HW, NN_MODEL_EMPTY,
	     "check admit claim prepare fetch lease ODD swap punload release "
	     "hw_down inval forget geom(L) EVEN unlease unclaim");
	/* [!] "restored" with nothing to restore: fail closed */
	reset(0); r_swap = NN_SVC_ERR_HW; r_after = 1;
	load("restored from empty (contradiction)", N, NN_SVC_ERR_HW,
	     NN_MODEL_EMPTY,
	     "check admit claim prepare fetch lease ODD swap punload release "
	     "hw_down inval forget geom(L) EVEN unlease unclaim");
	/* [!] BUSY from past the backend is not BUSY */
	reset(1); r_swap = NN_SVC_ERR_BUSY; r_after = 1;
	load("backend says busy", N, NN_SVC_ERR_HW, NN_MODEL_PREVIOUS,
	     "check admit claim prepare(open) fetch lease ODD swap EVEN unlease "
	     "unclaim");

	/* [!] a "success" that left no model is a failure, and LOST */
	reset(1); r_swap = NN_SVC_OK; r_after = 0;
	load("backend ok but no model left, open", N, NN_SVC_ERR_HW,
	     NN_MODEL_EMPTY,
	     "check admit claim prepare(open) fetch lease ODD swap punload release "
	     "hw_down inval forget geom(L) EVEN unlease unclaim");
	reset(0); r_swap = NN_SVC_OK; r_after = 0;
	load("backend ok but no model left, empty", N, NN_SVC_ERR_HW,
	     NN_MODEL_EMPTY,
	     "check admit claim prepare fetch lease ODD swap punload release "
	     "hw_down inval forget geom(L) EVEN unlease unclaim");

	/* ---- the plugin, only after the backend took the model ----------- */
	reset(1);
	load("opened, plugin started", N, NN_SVC_OK, NN_MODEL_NEW,
	     "check admit claim prepare(open) fetch lease ODD swap pstart inval "
	     "commit geom(L) EVEN unlease unclaim");
	reset(0);
	load("opened from empty", NN_SPEC_ADDR, NN_SVC_OK, NN_MODEL_NEW,
	     "check admit claim prepare fetch lease ODD swap pstart inval commit "
	     "geom(L) EVEN unlease unclaim");
	reset(1); bare = 1;
	load("bare model: the plugin is unloaded", N, NN_SVC_OK, NN_MODEL_NEW,
	     "check admit claim prepare(open) fetch lease ODD swap punload inval "
	     "commit geom(L) EVEN unlease unclaim");
	reset(1); r_plugin = NN_SVC_ERR_ARG;
	load("plugin refused: new, failed, no decoder", N, NN_SVC_ERR_ARG,
	     NN_MODEL_NEW,
	     "check admit claim prepare(open) fetch lease ODD swap pstart punload "
	     "inval commit geom(L) EVEN unlease unclaim");
	reset(1); r_plugin = NN_SVC_ERR_BUSY;
	load("plugin says busy", N, NN_SVC_ERR_HW, NN_MODEL_NEW,
	     "check admit claim prepare(open) fetch lease ODD swap pstart punload "
	     "inval commit geom(L) EVEN unlease unclaim");

	/* ---- unload ------------------------------------------------------ */
	reset(1); r_admit = NN_SVC_ERR_STATE;
	unload("precondition refused", NN_SVC_ERR_STATE, "admit");
	reset(1); r_claim = 0;
	unload("claim refused", NN_SVC_ERR_BUSY, "admit claim");
	reset(1); r_lease = 0;
	unload("lease refused: nothing changes, the counter stays",
	       NN_SVC_ERR_BUSY, "admit claim lease unclaim");
	reset(1);
	unload("plugin first, then the backend", NN_SVC_OK,
	       "admit claim lease ODD punload inval geom(L) unlease release "
	       "hw_down forget EVEN unclaim");
	reset(0);
	unload("nothing open: idempotent", NN_SVC_OK,
	       "admit claim lease ODD punload inval geom(L) unlease release "
	       "hw_down forget EVEN unclaim");

	/* ---- a board with no plugin lease never touches a plugin ---------- */
	cur = &board_nolease;
	reset(1);
	load("no lease: opened", N, NN_SVC_OK, NN_MODEL_NEW,
	     "check admit claim prepare(open) fetch ODD swap inval commit geom "
	     "EVEN unclaim");
	reset(1); bare = 1;
	load("no lease: bare", N, NN_SVC_OK, NN_MODEL_NEW,
	     "check admit claim prepare(open) fetch ODD swap inval commit geom "
	     "EVEN unclaim");
	reset(1); r_swap = NN_SVC_ERR_HW; r_after = 0;
	load("no lease: lost", N, NN_SVC_ERR_HW, NN_MODEL_EMPTY,
	     "check admit claim prepare(open) fetch ODD swap release hw_down inval "
	     "forget geom EVEN unclaim");
	reset(1);
	unload("no lease: unload", NN_SVC_OK,
	       "admit claim ODD inval geom release hw_down forget EVEN unclaim");
	cur = &board;

	/* ---- a copy taken without the claim ------------------------------ */
	ok("claim free: the copy stands", nn_core_model_copy_stands(1u, 9u, 1));
	ok("between steps at the start: busy", !nn_core_model_copy_stands(3u, 3u, 0));
	ok("between steps at the end: busy", !nn_core_model_copy_stands(2u, 3u, 0));
	ok("a whole load in between: busy", !nn_core_model_copy_stands(2u, 4u, 0));
	ok("nothing moved: the copy stands", nn_core_model_copy_stands(4u, 4u, 0));
	ok("across the wrap: busy",
	   !nn_core_model_copy_stands(0xFFFFFFFEu, 0u, 0));
	{
		uint32_t s = life.seq;

		ok("the counter reads under the critical section",
		   nn_core_model_seq(&life, &board) == s && cs_depth == 0);
	}

	if (fails) {
		printf("test_nn_core_model: %d FAILED\n", fails);
		return 1;
	}
	printf("test_nn_core_model: all passed\n");
	return 0;
}
