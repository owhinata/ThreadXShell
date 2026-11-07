/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the container claims under the shared load order (issue #174):
 * svc/nn/nn_core_model.c + svc/nn/nn_swap.c driving a board whose commit /
 * forget hooks settle a svc/plugin/plugin_claims.h store, the way all three
 * boards' adapters do (grove-vision-ai-v2 since issue #174: a load resolves a
 * container's claims into a "next" copy, and only the commit makes them the
 * store's).
 *
 * WHAT IS PINNED.  The claims follow the model that is ACTUALLY open -- the
 * stand-in backend's own state, not the adapter's idea of it -- after every
 * ending a load or an unload can have:
 *
 *   a container loaded            -> its claims
 *   the swap refused, previous    -> the PREVIOUS model's claims, untouched
 *     model restored
 *   the swap refused, nothing     -> none
 *     left
 *   the plugin refused            -> the new container's claims (its model IS
 *                                    open; `nn info` says "validated, not
 *                                    loaded")
 *   a bare model                  -> none
 *   the lookup or the lease       -> whatever was there, untouched
 *     refused
 *   an unload (twice)             -> none
 *
 * and, at EVERY hook the order calls and after each operation, what a `nn info`
 * reader would see: the counter and the store copied in one critical section
 * either say BUSY or describe the open model -- never the new model beside
 * the old claims, nor the other way round.
 *
 * And (issue #199) against which copy: the reader's snapshot is checked
 * against the value an `nn info` copy carries.  A copy taken at that moment
 * behaves as above; one taken before the operation began is BUSY as soon as
 * the counter has moved -- odd, or even again with other claims -- and the
 * same as the fresh one while it has not; a copy with no value is always BUSY.
 */
#include <stdio.h>
#include <string.h>

#include "nn_core_model.h"
#include "nn_svc.h"
#include "plugin_claims.h"

static int fails, checks;

#define CHECK(cond, what)                                                   \
	do {                                                                \
		checks++;                                                   \
		if (!(cond)) {                                              \
			printf("  FAIL %s  (%s:%d)\n", what, __FILE__, __LINE__); \
			fails++;                                            \
		}                                                           \
	} while (0)

/* ---- the stand-in backend: what is ACTUALLY open -------------------------- */

struct model {
	int  id;          /* 0: none */
	int  container;
	char from[PLUGIN_CLAIMS_FROM_MAX];
};

static struct model open_now;       /* the backend's own state */

enum swap_mode { SWAP_OK, SWAP_RESTORE, SWAP_EMPTY };
static enum swap_mode swap_mode;
static int fetch_refuses, lease_refuses, plugin_refuses;

/* ---- the board: the store, the "next" copy, the critical section ---------- */

static struct nn_core_model life;
static struct plugin_claims_store store;
static struct plugin_claims next;    /* what the lookup resolved into */
static int cs_depth;
static const char *phase = "";
/* `nn info`'s copy from before the current operation (issue #199). */
static struct nn_svc_info before;

static unsigned cs_enter(void) { cs_depth++; return 0u; }
static void cs_exit(unsigned p) { (void)p; cs_depth--; }

/* What `nn info` does: the counter and the store in ONE critical section. */
static void reader(const char *where)
{
	struct plugin_claims c;
	enum plugin_claims_seen seen;
	unsigned p = cs_enter();
	char what[160];

	struct plugin_claims c_old;
	enum plugin_claims_seen seen_old, seen_none;
	struct nn_svc_info now, none;
	uint32_t seq;

	memset(&c, 0, sizeof c);
	memset(&c_old, 0, sizeof c_old);
	memset(&none, 0, sizeof none);
	/* a copy taken at this moment carries the counter when it is even */
	memset(&now, 0, sizeof now);
	now.seq_known = (uint8_t)((life.seq & 1u) == 0u);
	now.seq = life.seq;
	seen = plugin_claims_get(&store, life.seq, &now, &c);
	seen_old = plugin_claims_get(&store, life.seq, &before, &c_old);
	seen_none = plugin_claims_get(&store, life.seq, &none, &c_old);
	seq = life.seq;
	cs_exit(p);
	snprintf(what, sizeof what, "%s: reader at %s", phase, where);
	CHECK(seen_none == PLUGIN_CLAIMS_BUSY, what);   /* no value: always */
	if (seq != before.seq)
		CHECK(seen_old == PLUGIN_CLAIMS_BUSY, what);
	else
		CHECK(seen_old == seen, what);
	if (seen == PLUGIN_CLAIMS_BUSY) {
		CHECK((seq & 1u) != 0u, what);   /* only while it is odd */
		return;   /* the honest answer while a change is in progress */
	}
	if (open_now.id == 0 || !open_now.container) {
		CHECK(seen == PLUGIN_CLAIMS_NONE, what);
		return;
	}
	CHECK(seen == PLUGIN_CLAIMS_VALID &&
	      strcmp(c.from, open_now.from) == 0, what);
}

/* One load's resolution (the board's job). */
struct tjob {
	struct model m;
};

static int b_swap(struct nn_core_model_job *j, int *after)
{
	const struct tjob *t = j->board;

	reader("swap");
	switch (swap_mode) {
	case SWAP_OK:
		open_now = t->m;
		*after = 1;
		return NN_SVC_OK;
	case SWAP_RESTORE:
		*after = (open_now.id != 0);   /* the previous one, rebuilt */
		return NN_SVC_ERR_HW;
	case SWAP_EMPTY:
	default:
		memset(&open_now, 0, sizeof open_now);
		*after = 0;
		return NN_SVC_ERR_HW;
	}
}
static void b_release(void)
{
	reader("release");
	memset(&open_now, 0, sizeof open_now);
}
static int b_has(void) { return open_now.id != 0; }

static const struct nn_core_model_backend backend = {
	.swap = b_swap, .release = b_release, .has_model = b_has,
};

static int b_claim(struct nn_core_model_job *j) { (void)j; return NN_SVC_OK; }
static void b_unclaim(void) { reader("claim_give"); }

/* The lookup: a container's claims go to the "next" copy, not the store. */
static int b_fetch(struct nn_core_model_job *j)
{
	const struct tjob *t = j->board;

	reader("fetch");
	if (fetch_refuses)
		return NN_SVC_ERR_ARG;
	j->bare = !t->m.container;
	if (t->m.container) {
		memset(&next, 0, sizeof next);
		next.view.has_plugin = 1u;
		snprintf(next.from, sizeof next.from, "%s", t->m.from);
	}
	return NN_SVC_OK;
}
static int b_lease(struct nn_core_model_job *j)
{
	(void)j;
	reader("lease_take");
	return !lease_refuses;
}
static void b_unlease(void) { reader("lease_give"); }
static int b_pstart(struct nn_core_model_job *j)
{
	(void)j;
	reader("plugin_start");
	return plugin_refuses ? NN_SVC_ERR_ARG : NN_SVC_OK;
}
static void b_punload(void) { reader("plugin_unload"); }
static void b_inval(void) { reader("invalidate"); }

/* The board's settle: in its OWN short critical section (the shared order
 * calls these outside one), and nothing else inside it. */
static void claims_set(const struct plugin_claims *c)
{
	unsigned p = cs_enter();

	plugin_claims_set(&store, c);
	cs_exit(p);
}
static void b_forget(void)
{
	CHECK(cs_depth == 0, "forget is called outside the critical section");
	reader("forget (before)");
	claims_set(NULL);
	reader("forget (after)");
}
static void b_commit(struct nn_core_model_job *j)
{
	const struct tjob *t = j->board;

	CHECK(cs_depth == 0, "commit is called outside the critical section");
	reader("commit (before)");
	claims_set(t->m.container ? &next : NULL);
	reader("commit (after)");
}

static const struct nn_core_model_board board = {
	.cs_enter = cs_enter, .cs_exit = cs_exit, .backend = &backend,
	.claim_take = b_claim, .claim_give = b_unclaim,
	.fetch = b_fetch, .lease_take = b_lease, .lease_give = b_unlease,
	.plugin_start = b_pstart, .plugin_unload = b_punload,
	.invalidate = b_inval, .forget = b_forget, .commit = b_commit,
};

/* ---- the operations ------------------------------------------------------ */

static void knobs(enum swap_mode s, int fetch, int lease, int plugin)
{
	swap_mode = s;
	fetch_refuses = fetch;
	lease_refuses = lease;
	plugin_refuses = plugin;
}

static void load(const char *what, int id, int container)
{
	struct nn_spec spec;
	struct nn_op_result res;
	struct nn_core_model_job j;
	struct tjob t;
	enum nn_model_state st = NN_MODEL_EMPTY;

	phase = what;
	memset(&spec, 0, sizeof spec);
	snprintf(spec.name, sizeof spec.name, "m");
	memset(&t, 0, sizeof t);
	t.m.id = id;
	t.m.container = container;
	snprintf(t.m.from, sizeof t.m.from, "slot %d, blob 'm%d'", id, id);
	memset(&res, 0, sizeof res);
	j.spec = &spec;
	j.res = &res;
	j.board = &t;
	before.seq_known = 1u;
	before.seq = life.seq;
	nn_core_model_load(&life, &board, &j, &st);
	reader("the end");
	CHECK((life.seq & 1u) == 0u, "the counter is even when the load returns");
}

static void unload(const char *what)
{
	struct nn_op_result res;
	struct nn_core_model_job j;

	phase = what;
	memset(&res, 0, sizeof res);
	j.spec = NULL;
	j.res = &res;
	j.board = NULL;
	before.seq_known = 1u;
	before.seq = life.seq;
	nn_core_model_unload(&life, &board, &j);
	reader("the end");
	CHECK((life.seq & 1u) == 0u, "the counter is even when the unload returns");
}

/* The store says exactly @p want_id's claims (0: none), and so does the
 * backend. */
static void expect(const char *what, int want_id)
{
	char line[160];

	snprintf(line, sizeof line, "%s: the open model is %d", what, want_id);
	CHECK(open_now.id == want_id, line);
	snprintf(line, sizeof line, "%s: the claims are %d's", what, want_id);
	if (want_id == 0) {
		CHECK(!store.valid, line);
	} else {
		char from[PLUGIN_CLAIMS_FROM_MAX];

		snprintf(from, sizeof from, "slot %d, blob 'm%d'", want_id,
		         want_id);
		CHECK(store.valid && strcmp(store.c.from, from) == 0, line);
	}
}

int main(void)
{
	printf("plugin claims under the shared load order\n");

	knobs(SWAP_OK, 0, 0, 0);
	load("container A", 1, 1);
	expect("container A", 1);

	knobs(SWAP_RESTORE, 0, 0, 0);
	load("container B refused, A restored", 2, 1);
	expect("container B refused, A restored", 1);

	knobs(SWAP_OK, 0, 0, 1);
	load("container C, its plugin refused", 3, 1);
	expect("container C, its plugin refused", 3);

	knobs(SWAP_OK, 0, 0, 0);
	load("bare D", 4, 0);
	CHECK(open_now.id == 4 && !store.valid, "bare D: open, no claims");

	knobs(SWAP_RESTORE, 0, 0, 0);
	load("container E refused over bare D", 5, 1);
	CHECK(open_now.id == 4 && !store.valid,
	      "container E refused over bare D: D open, still no claims");

	knobs(SWAP_OK, 0, 0, 0);
	load("container F", 6, 1);
	expect("container F", 6);

	knobs(SWAP_OK, 1, 0, 0);
	load("lookup refused over F", 7, 1);
	expect("lookup refused over F", 6);

	knobs(SWAP_OK, 0, 1, 0);
	load("lease refused over F", 8, 1);
	expect("lease refused over F", 6);

	knobs(SWAP_EMPTY, 0, 0, 0);
	load("container G refused, nothing left", 9, 1);
	expect("container G refused, nothing left", 0);

	knobs(SWAP_OK, 0, 0, 0);
	load("container H", 10, 1);
	expect("container H", 10);

	unload("unload H");
	expect("unload H", 0);
	unload("unload nothing");
	expect("unload nothing", 0);

	CHECK(cs_depth == 0, "every critical section was left");
	if (fails) {
		printf("test_plugin_claims_order: %d of %d checks FAILED\n", fails,
		       checks);
		return 1;
	}
	printf("test_plugin_claims_order: all %d checks passed\n", checks);
	return 0;
}
