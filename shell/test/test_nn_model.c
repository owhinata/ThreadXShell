/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the model singleton of wio-lite-ai and f746g-disco
 * (svc/nn/nn.c), issues #131 P16, #152 and #161.
 *
 * The real nn.c is compiled against nn_model_host.c (the board's state object
 * and hooks, modelled) and a stub backend that keeps the contract nn_backend.h
 * states: a model in the image (has_builtin) is adopted by the first open only,
 * release() empties, and a reload ALWAYS hands back the handle -- the empty one
 * when no model is left.  An unload is release(); nn.c refuses reload(NULL)
 * itself, so the stub fails the test if it ever sees one.
 *
 * What is checked is nn.c's half, the half the adapters read:
 *   - only the first open reaches the backend; nothing after it -- an open
 *     after an unload included -- builds anything;
 *   - the reload's answer is whether a MODEL is left, not whether the handle is
 *     open: a refused load from empty must read EMPTY ("nothing is loaded"),
 *     not PREVIOUS, through the shared table the adapters use;
 *   - a refused load whose restore ALSO failed leaves the singleton open and
 *     empty, and the next load goes through (f746g-disco's #131 P16 rule, which
 *     wio-lite-ai's dispatcher follows since #161);
 *   - every refusal code -- the backend's and this layer's own -- reaches the
 *     caller exactly as it was returned, never folded into another;
 *   - each critical section restores the mask value it found: a caller that
 *     already had interrupts masked still has them masked afterwards;
 *   - `arena` is the backend's reservation and `used` goes to 0 with no model;
 *   - the external memory claim f746g-disco's `sdram test` takes (issue #152):
 *     refused while the session is held, while the first open is building and
 *     while a model is loaded; while it is held the session cannot be taken,
 *     the first open builds nothing, and the load region and reload refuse.
 *
 * Built four times.  The default is a backend with a model in the image (the
 * null stub, f746g-disco's stedgeai).  With TEST_FIRST_BUILD_FAILS the stub's
 * one adoption of that model fails: the first open reports it, and the second
 * -- nn.c's latch taken again -- reaches the backend, builds nothing and opens
 * EMPTY.  With TEST_EMPTY_FROM_BOOT the backend has nothing in the image (tflm,
 * stedgeai_reloc): the first open builds nothing, and only a load brings a
 * model.  With TEST_NO_SWAP the backend cannot swap (the null stub's shape):
 * the three optional entries are NULL and nn.c answers NN_ERR_NOSUP for them.
 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "nn.h"
#include "nn_backend.h"
#include "nn_svc.h"
#include "nn_swap.h"
#include "nn_model_host.h"
#include "nn_dispatch_state.h"   /* the hooks, for the nesting check */

/* No critical section is left open, and the mask is as the test left it. */
static void cs_clean(void)
{
	assert(nn_host_depth == 0);
	assert(nn_host_mask == 0u);
}

/* ---- stub backend ------------------------------------------------------- */

static struct {
	int adopted;        /* the first open has run                       */
	int has;            /* a model is there                             */
	int open_calls;
	int refuse_next;    /* the next reload is refused with this (<0)    */
	int restore_fails;  /* ...and the restore fails too: stays empty    */
	int region_rc;      /* load_region() answers this                   */
	int release_rc;     /* release() answers this                       */
	int claim_in_open;  /* nn_scratch_claim() answer seen during a build */
	int null_handle;    /* answer a reload with NULL (contract breach)  */
	struct nn_tensor in;
} stub;

static char stub_buf[64];

static int st_init(void) { return 0; }
static int stub_builds;     /* how many times the built-in model was built */

static int st_open(void **impl)
{
	stub.open_calls++;
	if (!stub.adopted) {
		/* Mid-build the open latch is taken: a claim must be refused. */
		stub.claim_in_open = nn_scratch_claim();
		stub.adopted = 1;       /* the one adoption, whatever its outcome */
#ifdef TEST_EMPTY_FROM_BOOT
		stub.has = 0;           /* nothing in the image: builds nothing */
#else
		stub_builds++;
#endif
#if defined(TEST_EMPTY_FROM_BOOT)
#elif defined(TEST_FIRST_BUILD_FAILS)
		stub.has = 0;
		return -3;              /* the build failed; stays empty */
#else
		stub.has = 1;           /* the built-in model, once */
#endif
	}
	*impl = &stub;
	return 0;
}
static void st_close(void *impl) { (void)impl; }
static const char *st_name(void *impl) { (void)impl; return stub.has ? "m" : "(none)"; }
static int st_in_count(void *impl) { (void)impl; return stub.has ? 1 : 0; }
static int st_out_count(void *impl) { (void)impl; return stub.has ? 1 : 0; }
static struct nn_tensor *st_tensor(void *impl, int idx)
{
	(void)impl;
	return (stub.has && idx == 0) ? &stub.in : NULL;
}
static uint32_t st_acts(void *impl) { (void)impl; return stub.has ? 1234u : 0u; }
static int st_run(void *impl) { (void)impl; return stub.has ? 0 : -1; }
#ifndef TEST_NO_SWAP
static int st_region(void **buf, uint32_t *cap)
{
	if (stub.region_rc != 0)
		return stub.region_rc;
	*buf = stub_buf;
	*cap = sizeof stub_buf;
	return 0;
}
static int st_reload(const void *data, uint32_t len, const char *name,
                     void **impl)
{
	(void)len; (void)name;
	assert(data != NULL);       /* nn.c refuses an unload by reload */
	*impl = stub.null_handle ? NULL : (void *)&stub;
	if (stub.refuse_next) {
		int rc = stub.refuse_next;

		stub.refuse_next = 0;
		if (stub.restore_fails) {
			stub.restore_fails = 0;
			stub.has = 0;       /* open and EMPTY (#131 P16) */
		}
		return rc;              /* whatever was there stays (or nothing) */
	}
	stub.has = 1;
	return 0;
}
static int st_release(void)
{
	if (stub.release_rc != 0)
		return stub.release_rc;
	stub.has = 0;
	return 0;
}
#endif

const struct nn_backend_vt nn_backend_vt_selected = {
	.info = &(const struct nn_backend_info){ "stub", "" },
	.init = st_init, .open = st_open, .close = st_close,
	.model_name = st_name, .in_count = st_in_count, .out_count = st_out_count,
	.input = st_tensor, .output = st_tensor, .activations_bytes = st_acts,
	.run = st_run,
#ifndef TEST_NO_SWAP
	.load_region = st_region, .reload = st_reload, .release = st_release,
#endif
	.arena_reserved = 524288u,
#ifdef TEST_EMPTY_FROM_BOOT
	.has_builtin = 0u,
#else
	.has_builtin = 1u,
#endif
};

#if !defined(TEST_FIRST_BUILD_FAILS) && !defined(TEST_EMPTY_FROM_BOOT) && \
    !defined(TEST_NO_SWAP)
/* ---- the critical-section discipline ------------------------------------ */

/*
 * The stub's own save / restore, nested: the inner exit restores the value the
 * inner enter found (masked), so the outer section is still masked; only the
 * outer exit unmasks.  Then nn.c's sections, entered by a caller that already
 * had the mask set: each one must hand it back set.
 */
static void check_nesting(void)
{
	struct nn_model *m = NULL;
	uint32_t outer, inner;
	int enters;

	cs_clean();
	outer = nn_model_board_enter();
	assert(outer == 0u && nn_host_mask == 1u && nn_host_depth == 1);
	inner = nn_model_board_enter();
	assert(inner == 1u && nn_host_depth == 2);
	nn_model_board_exit(inner);
	assert(nn_host_mask == 1u && nn_host_depth == 1);   /* still masked */
	nn_model_board_exit(outer);
	cs_clean();

	/* A caller with the mask already set, as a section of its own would be. */
	nn_host_mask = 1u;
	enters = nn_host_enters;
	assert(nn_model_open(&m) == 0);
	assert(nn_host_mask == 1u && nn_host_depth == 0);
	if (nn_session_try_acquire() == 0) {
		assert(nn_host_mask == 1u && nn_host_depth == 0);
		nn_session_release();
	}
	assert(nn_host_mask == 1u && nn_host_depth == 0);
	(void)nn_scratch_claim();
	assert(nn_host_mask == 1u && nn_host_depth == 0);
	nn_scratch_release();
	assert(nn_host_mask == 1u && nn_host_depth == 0);
	assert(nn_host_enters > enters);   /* the sections did run */
	nn_host_mask = 0u;
	cs_clean();
}
#endif

#if defined(TEST_FIRST_BUILD_FAILS)
int main(void)
{
	struct nn_model *m = NULL;
	int after = -1;

	/* The first open is the adoption, and its failure is that caller's --
	 * the backend's code, unchanged. */
	assert(nn_model_open(&m) == -3);
	assert(stub.open_calls == 1 && stub_builds == 1);
	cs_clean();
	/* No singleton yet: a reload is an early refusal, by this layer. */
	assert(nn_model_reload(stub_buf, 8u, "x", &after) == NN_ERR_STATE);
	assert(after == 0);

	/* The latch is free again; the next open reaches the backend, which
	 * builds nothing and opens empty. */
	assert(nn_model_open(&m) == 0 && m != NULL);
	assert(stub.open_calls == 2 && stub_builds == 1);
	assert(!nn_model_present(m));
	assert(strcmp(nn_model_name(m), "(none)") == 0);

	/* And it stays that way: open again, nothing reaches the backend. */
	assert(nn_model_open(&m) == 0);
	assert(stub.open_calls == 2 && stub_builds == 1 && !nn_model_present(m));

	/* Only an explicit load brings a model; reload(NULL) is not one. */
	assert(nn_model_reload(NULL, 0u, NULL, &after) == NN_ERR_ARG && after == 0);
	assert(nn_model_reload(stub_buf, 8u, "x", &after) == 0 && after == 1);
	assert(nn_model_present(m));

	/* The counter was found not running at init: timing reads 0. */
	assert(nn_run(m) == 0 && nn_last_cycles(m) == 0u);
	cs_clean();

	printf("test_nn_model (first build fails): OK\n");
	return 0;
}
#elif defined(TEST_EMPTY_FROM_BOOT)
/*
 * [!] EMPTY FROM BOOT (issue #159 Step 4c).  The tflm build has no model in the
 * image: the first open -- the #131 latch is still there -- finds the backend
 * empty and builds nothing, so `nn info` says (none) and `nn run` refuses until
 * a load.
 */
int main(void)
{
	struct nn_model *m = NULL, *m2 = NULL;
	int after = -1;

	assert(nn_model_has_builtin() == 0);

	/* [!] `sdram test` HOLDING THE MEMORY DOES NOT REFUSE THIS FIRST OPEN
	 * (issue #159 Step 4c): it builds nothing there, so a fresh-boot `nn
	 * info` during the test answers (none), not busy.  The claim still keeps
	 * the session out, and the load region and a reload still refuse. */
	assert(nn_scratch_claim() == 0);
	assert(nn_model_open(&m) == 0 && m != NULL);
	assert(stub.open_calls == 1 && stub_builds == 0);
	assert(stub.claim_in_open == -1);              /* the latch was taken */
	assert(!nn_model_present(m));
	assert(nn_session_try_acquire() == NN_ERR_SCRATCH_HELD);
	after = -1;
	assert(nn_model_reload(stub_buf, 8u, "x", &after) == NN_ERR_SCRATCH_HELD);
	assert(after == 0);
	nn_scratch_release();
	cs_clean();
	assert(!nn_model_present(m) && !nn_model_loaded());
	assert(strcmp(nn_model_name(m), "(none)") == 0);
	assert(nn_input_count(m) == 0 && nn_activations_bytes(m) == 0u);
	assert(nn_arena_reserved() == 524288u);
	assert(nn_run(m) != 0);

	/* Later opens reach nothing and build nothing. */
	assert(nn_model_open(&m2) == 0 && m2 == m && stub.open_calls == 1);

	/* `builtin` is not a source: reload(NULL) is refused and leaves it empty. */
	after = -1;
	assert(nn_model_reload(NULL, 0u, NULL, &after) == NN_ERR_ARG && after == 0);
	assert(!nn_model_present(m));

	/* A load brings one; an unload empties it again; a load after the unload
	 * brings one back (the unload is not a terminal state). */
	after = -1;
	assert(nn_model_reload(stub_buf, 8u, "x", &after) == 0 && after == 1);
	assert(nn_model_present(m) && nn_run(m) == 0);
	assert(nn_model_release() == 0 && !nn_model_present(m));
	assert(nn_model_open(&m2) == 0 && stub.open_calls == 1);
	assert(!nn_model_present(m));
	after = -1;
	assert(nn_model_reload(stub_buf, 8u, "x", &after) == 0 && after == 1);
	assert(nn_model_present(m) && strcmp(nn_model_name(m), "m") == 0);
	assert(nn_model_open(&m2) == 0 && stub.open_calls == 1);
	cs_clean();

	printf("test_nn_model (empty from boot): OK\n");
	return 0;
}
#elif defined(TEST_NO_SWAP)
/*
 * A backend that cannot swap (the null stub on both boards): the optional
 * entries are NULL, and nn.c answers NN_ERR_NOSUP for each rather than
 * branching through them -- including the unload, which on wio-lite-ai used to
 * be reload(NULL) and answered NN_ERR_NOSUP the same way.
 */
int main(void)
{
	struct nn_model *m = NULL;
	void *buf = NULL;
	uint32_t cap = 0u;
	int after = -1;

	assert(nn_model_load_region(&buf, &cap) == NN_ERR_NOSUP);
	assert(nn_model_load_region(NULL, &cap) == NN_ERR_ARG);
	assert(nn_model_open(NULL) == NN_ERR_ARG);
	assert(nn_model_open(&m) == 0 && nn_model_present(m));
	assert(nn_model_reload(stub_buf, 8u, "x", &after) == NN_ERR_NOSUP);
	assert(after == 1);                       /* nothing was touched */
	assert(nn_model_reload(NULL, 0u, NULL, &after) == NN_ERR_NOSUP);
	assert(nn_model_release() == NN_ERR_NOSUP);
	assert(nn_model_present(m));              /* the unload kept the model */
	assert(nn_run(NULL) == NN_ERR_STATE);
	cs_clean();

	/* The four codes this layer returns are distinct, and each has its own
	 * sentence. */
	assert(strcmp(nn_model_strerror(NN_ERR_ARG), nn_model_strerror(NN_ERR_NOSUP)) != 0);
	assert(strcmp(nn_model_strerror(NN_ERR_STATE), nn_model_strerror(NN_ERR_NOSUP)) != 0);
	assert(strcmp(nn_model_strerror(NN_ERR_SCRATCH_HELD), "unknown error") != 0);
	assert(strcmp(nn_model_strerror(-12345), "unknown error") == 0);

	printf("test_nn_model (no swap): OK\n");
	return 0;
}
#else
/* What the adapter tells the operator after a reload (svc/nn/nn_swap.c). */
static int state_of(int rc, int model_after)
{
	struct nn_swap_verdict v;

	nn_swap_decide(1, nn_swap_end_of(rc, model_after, 0), &v);
	return v.state;
}

/* Every code a backend can refuse with -- the named ones, the unnamed small
 * negatives a backend may still use, and the extremes -- comes back as it went
 * in: from reload(), from load_region() and from release(). */
static void check_codes_pass_through(struct nn_model *m)
{
	static const int codes[] = {
		NN_MODEL_ERR_VERSION, NN_MODEL_ERR_OPS, NN_MODEL_ERR_ARENA,
		NN_MODEL_ERR_TENSOR, NN_MODEL_ERR_SHAPE, NN_MODEL_ERR_EMPTY,
		NN_MODEL_ERR_FORMAT, NN_MODEL_ERR_SLOT, NN_MODEL_ERR_ALIGN,
		-10, -11, -49, -51, -89, -93, -128, INT_MIN,
	};
	void *buf;
	uint32_t cap;
	int after;

	for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; i++) {
		int was = nn_model_present(m);

		stub.refuse_next = codes[i];
		after = -1;
		assert(nn_model_reload(stub_buf, 8u, "x", &after) == codes[i]);
		assert(after == was && nn_model_present(m) == was);

		stub.region_rc = codes[i];
		assert(nn_model_load_region(&buf, &cap) == codes[i]);
		stub.region_rc = 0;

		stub.release_rc = codes[i];
		assert(nn_model_release() == codes[i]);
		assert(nn_model_present(m) == was);   /* a failed release changed nothing */
		stub.release_rc = 0;
	}
	cs_clean();
}

int main(void)
{
	struct nn_model *m = NULL, *m2 = NULL;
	int after = -1, rc;
	void *buf;
	uint32_t cap;

	/* The counter runs, and each read advances it by 100. */
	nn_host_cycles_start_rc = 0;
	nn_host_cycles_step = 100u;

	/* A reload before the singleton exists is an early refusal: no model. */
	assert(nn_model_reload(stub_buf, 8u, "x", &after) == NN_ERR_STATE);
	assert(after == 0 && stub.open_calls == 0);
	assert(nn_model_load_region(NULL, &cap) == NN_ERR_ARG);

	/* [!] The memory claim on a fresh boot (issue #152): nothing loaded,
	 * nothing using nn, so it is granted -- and while it is held the session
	 * cannot be taken and the FIRST open builds nothing. */
	assert(nn_scratch_claim() == 0);
	assert(nn_scratch_claim() == -1);                 /* one holder      */
	assert(nn_session_try_acquire() == NN_ERR_SCRATCH_HELD);
	assert(nn_model_open(&m) == NN_ERR_SCRATCH_HELD);
	assert(stub.open_calls == 0 && stub_builds == 0);  /* nothing built   */
	assert(nn_model_load_region(&buf, &cap) == NN_ERR_SCRATCH_HELD);
	assert(!nn_model_loaded());
	nn_scratch_release();
	cs_clean();

	/* Refused while the session is held, granted again once it is not; a
	 * second acquire is busy (-1), not the claim's code. */
	assert(nn_session_try_acquire() == 0);
	assert(nn_session_try_acquire() == -1);
	assert(nn_scratch_claim() == -1);
	nn_session_release();
	cs_clean();

	/* The first open adopts the built-in model; the second reaches nothing.
	 * The claim tried mid-build was refused (the open latch was taken). */
	m = NULL;
	assert(nn_model_open(&m) == 0 && m != NULL);
	assert(stub.claim_in_open == -1);
	assert(nn_model_open(&m2) == 0 && m2 == m);
	assert(stub.open_calls == 1 && stub_builds == 1);
	assert(nn_model_present(m));
	assert(nn_arena_reserved() == 524288u);
	assert(nn_activations_bytes(m) == 1234u);
	assert(nn_model_has_builtin() == 1);
	cs_clean();

	/* Timing: the run is bracketed by two fenced reads. */
	assert(nn_last_cycles(m) == 0u);
	assert(nn_run(m) == 0 && nn_last_cycles(m) == 100u);

	check_nesting();

	/* [!] reload(NULL) IS NOT AN UNLOAD (issue #161; it was on wio-lite-ai):
	 * refused by this layer, the model stays and its timing too. */
	after = -1;
	assert(nn_model_reload(NULL, 0u, NULL, &after) == NN_ERR_ARG && after == 1);
	assert(nn_model_present(m) && nn_last_cycles(m) == 100u);

	/* Unload: empty, still open, and NO open afterwards builds anything. */
	assert(nn_model_release() == 0);
	assert(!nn_model_present(m) && nn_last_cycles(m) == 0u);
	assert(nn_model_open(&m2) == 0 && m2 == m);
	assert(stub.open_calls == 1);
	assert(!nn_model_present(m));
	assert(strcmp(nn_model_name(m), "(none)") == 0);
	assert(nn_input_count(m) == 0 && nn_activations_bytes(m) == 0u);
	assert(nn_arena_reserved() == 524288u);       /* a fact of the build */
	assert(nn_run(m) != 0);

	/* The claim: refused over a model, granted on the empty singleton.  An
	 * open now returns the singleton as it is and builds nothing; a reload
	 * refuses and leaves it empty. */
	assert(nn_model_reload(stub_buf, 8u, "x", &after) == 0 &&
	       nn_model_present(m));
	assert(nn_scratch_claim() == -2);
	assert(nn_model_release() == 0 && !nn_model_present(m));
	assert(nn_scratch_claim() == 0);
	assert(nn_session_try_acquire() == NN_ERR_SCRATCH_HELD);
	assert(nn_model_open(&m2) == 0 && m2 == m && stub.open_calls == 1);
	after = -1;
	assert(nn_model_reload(stub_buf, 8u, "x", &after) == NN_ERR_SCRATCH_HELD);
	assert(after == 0 && !nn_model_present(m));
	assert(nn_model_load_region(&buf, &cap) == NN_ERR_SCRATCH_HELD);
	nn_scratch_release();
	assert(nn_session_try_acquire() == 0);
	nn_session_release();
	cs_clean();

	/* [!] A refused load FROM EMPTY: the handle is open, but no model is
	 * left -- "nothing is loaded", not "the previous model is still active". */
	assert(nn_model_load_region(&buf, &cap) == 0);
	stub.refuse_next = -7;
	after = -1;
	rc = nn_model_reload(buf, 8u, "x", &after);
	assert(rc == -7 && after == 0);
	assert(state_of(rc, after) == NN_MODEL_EMPTY);
	assert(!nn_model_present(m));

	/* A load after the unload brings one. */
	after = -1;
	rc = nn_model_reload(buf, 8u, "x", &after);
	assert(rc == 0 && after == 1 && nn_model_present(m));
	assert(state_of(rc, after) == NN_MODEL_NEW);

	/* A refused load OVER a model keeps it: PREVIOUS. */
	stub.refuse_next = -7;
	after = -1;
	rc = nn_model_reload(buf, 8u, "x", &after);
	assert(rc == -7 && after == 1);
	assert(state_of(rc, after) == NN_MODEL_PREVIOUS);

	/* Every refusal code, unchanged. */
	check_codes_pass_through(m);

	/* [!] THE SWAP FAILS AND SO DOES THE RESTORE (issue #131 P16, both boards
	 * since #161): the singleton stays OPEN and EMPTY -- an open reaches no
	 * backend and builds nothing -- the answer is EMPTY, and the next load
	 * goes through. */
	assert(nn_model_present(m));
	stub.refuse_next = NN_MODEL_ERR_ARENA;
	stub.restore_fails = 1;
	after = -1;
	rc = nn_model_reload(buf, 8u, "x", &after);
	assert(rc == NN_MODEL_ERR_ARENA && after == 0);
	assert(state_of(rc, after) == NN_MODEL_EMPTY);
	assert(!nn_model_present(m) && !nn_model_loaded());
	assert(nn_model_open(&m2) == 0 && m2 == m && stub.open_calls == 1);
	assert(strcmp(nn_model_name(m), "(none)") == 0);
	assert(nn_run(m) != 0);
	after = -1;
	rc = nn_model_reload(buf, 8u, "x", &after);
	assert(rc == 0 && after == 1 && nn_model_present(m));
	assert(state_of(rc, after) == NN_MODEL_NEW);
	assert(nn_model_open(&m2) == 0 && stub.open_calls == 1);

	/* A backend that broke the contract and handed back NULL: no model, and
	 * the singleton closes rather than keep a dangling handle. */
	stub.null_handle = 1;
	stub.refuse_next = -7;
	after = -1;
	rc = nn_model_reload(buf, 8u, "x", &after);
	assert(rc == -7 && after == 0 && state_of(rc, after) == NN_MODEL_EMPTY);
	assert(!nn_model_present(m));
	cs_clean();

	printf("test_nn_model: OK\n");
	return 0;
}
#endif
