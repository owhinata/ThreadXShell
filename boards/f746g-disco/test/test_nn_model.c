/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's model singleton (port/nn/nn.c), issue #131 P16.
 *
 * The real nn.c is compiled against nn_shim/ (HAL + ThreadX stand-ins) and a
 * stub backend that keeps the backend contract nn_backend.h now states: the
 * built-in model is adopted by the first open only, release() empties, and a
 * reload ALWAYS hands back the handle -- the empty one when no model is left.
 *
 * What is checked is nn.c's half, the half the adapter reads:
 *   - only the first open reaches the backend; nothing after it -- an open
 *     after an unload included -- builds anything;
 *   - the reload's answer is whether a MODEL is left, not whether the handle is
 *     open: a refused load from empty must read EMPTY ("nothing is loaded"),
 *     not PREVIOUS, through the shared table the adapter uses;
 *   - `arena` is the backend's reservation and `used` goes to 0 with no model.
 *
 * Built twice.  With TEST_FIRST_BUILD_FAILS the stub's one adoption of the
 * built-in model fails: the first open reports it, and the second -- nn.c's
 * latch taken again -- reaches the backend, builds nothing and opens EMPTY.
 *
 * The tflm backend itself needs TFLM and is not built here; it is the stub's
 * contract that this pins, and nn_backend.h is where that contract is written.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nn.h"
#include "nn_backend.h"
#include "nn_svc.h"
#include "nn_swap.h"
#include "tx_api.h"
#include "stm32f7xx_hal.h"

DWT_Type       test_dwt = { .CTRL = DWT_CTRL_NOCYCCNT_Msk };
CoreDebug_Type test_coredebug;

UINT tx_thread_sleep(ULONG ticks)
{
	(void)ticks;
	assert(!"the open latch was contended in a single-threaded test");
	return 0;
}

/* ---- stub backend ------------------------------------------------------- */

static struct {
	int adopted;        /* the first open has run                       */
	int has;            /* a model is there                             */
	int open_calls;
	int refuse_next;    /* the next reload of SD bytes is refused       */
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
		stub.adopted = 1;       /* the one adoption, whatever its outcome */
		stub_builds++;
#ifdef TEST_FIRST_BUILD_FAILS
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
static int st_region(void **buf, uint32_t *cap)
{
	*buf = stub_buf;
	*cap = sizeof stub_buf;
	return 0;
}
static int st_reload(const void *data, uint32_t len, const char *name,
                     void **impl)
{
	(void)len; (void)name;
	*impl = stub.null_handle ? NULL : (void *)&stub;
	if (data != NULL && stub.refuse_next) {
		stub.refuse_next = 0;
		return -7;              /* refused: whatever was there stays */
	}
	stub.has = 1;
	return 0;
}
static int st_release(void) { stub.has = 0; return 0; }

const struct nn_backend_vt nn_backend_vt_selected = {
	.info = &(const struct nn_backend_info){ "stub", "" },
	.init = st_init, .open = st_open, .close = st_close,
	.model_name = st_name, .in_count = st_in_count, .out_count = st_out_count,
	.input = st_tensor, .output = st_tensor, .activations_bytes = st_acts,
	.run = st_run, .load_region = st_region, .reload = st_reload,
	.release = st_release, .arena_reserved = 524288u, .has_builtin = 1u,
};


#ifdef TEST_FIRST_BUILD_FAILS
int main(void)
{
	struct nn_model *m = NULL;
	int after = -1;

	/* The first open is the adoption, and its failure is that caller's. */
	assert(nn_model_open(&m) == -3);
	assert(stub.open_calls == 1 && stub_builds == 1);
	/* No singleton yet: a reload is still an early refusal. */
	assert(nn_model_reload(NULL, 0u, NULL, &after) == -1 && after == 0);

	/* The latch is free again; the next open reaches the backend, which
	 * builds nothing and opens empty. */
	assert(nn_model_open(&m) == 0 && m != NULL);
	assert(stub.open_calls == 2 && stub_builds == 1);
	assert(!nn_model_present(m));
	assert(strcmp(nn_model_name(m), "(none)") == 0);

	/* And it stays that way: open again, nothing reaches the backend. */
	assert(nn_model_open(&m) == 0);
	assert(stub.open_calls == 2 && stub_builds == 1 && !nn_model_present(m));

	/* Only an explicit `nn model load builtin` brings a model. */
	assert(nn_model_reload(NULL, 0u, NULL, &after) == 0 && after == 1);
	assert(nn_model_present(m));

	printf("test_nn_model (first build fails): OK\n");
	return 0;
}
#else
/* What the adapter tells the operator after a reload (nn_svc_f746.c). */
static int state_of(int rc, int model_after)
{
	struct nn_swap_verdict v;

	nn_swap_decide(1, nn_swap_end_of(rc, model_after, 0), &v);
	return v.state;
}

int main(void)
{
	struct nn_model *m = NULL, *m2 = NULL;
	int after = -1, rc;
	void *buf;
	uint32_t cap;

	/* A reload before the singleton exists is an early refusal: no model. */
	assert(nn_model_reload(NULL, 0u, NULL, &after) == -1 && after == 0);
	assert(stub.open_calls == 0);

	/* The first open adopts the built-in model; the second reaches nothing. */
	assert(nn_model_open(&m) == 0 && m != NULL);
	assert(nn_model_open(&m2) == 0 && m2 == m);
	assert(stub.open_calls == 1 && stub_builds == 1);
	assert(nn_model_present(m));
	assert(nn_arena_reserved() == 524288u);
	assert(nn_activations_bytes(m) == 1234u);
	assert(nn_model_has_builtin() == 1);

	/* Unload: empty, still open, and NO open afterwards builds anything. */
	assert(nn_model_release() == 0);
	assert(!nn_model_present(m));
	assert(nn_model_open(&m2) == 0 && m2 == m);
	assert(stub.open_calls == 1);
	assert(!nn_model_present(m));
	assert(strcmp(nn_model_name(m), "(none)") == 0);
	assert(nn_input_count(m) == 0 && nn_activations_bytes(m) == 0u);
	assert(nn_arena_reserved() == 524288u);       /* a fact of the build */
	assert(nn_run(m) != 0);

	/* [!] A refused load FROM EMPTY: the handle is open, but no model is
	 * left -- "nothing is loaded", not "the previous model is still active". */
	assert(nn_model_load_region(&buf, &cap) == 0);
	stub.refuse_next = 1;
	after = -1;
	rc = nn_model_reload(buf, 8u, "x", &after);
	assert(rc < 0 && after == 0);
	assert(state_of(rc, after) == NN_MODEL_EMPTY);
	assert(!nn_model_present(m));

	/* The built-in model comes back only through an explicit load. */
	after = -1;
	rc = nn_model_reload(NULL, 0u, NULL, &after);
	assert(rc == 0 && after == 1 && nn_model_present(m));
	assert(state_of(rc, after) == NN_MODEL_NEW);

	/* A refused load OVER a model keeps it: PREVIOUS. */
	stub.refuse_next = 1;
	after = -1;
	rc = nn_model_reload(buf, 8u, "x", &after);
	assert(rc < 0 && after == 1);
	assert(state_of(rc, after) == NN_MODEL_PREVIOUS);

	/* A loaded SD model. */
	after = -1;
	rc = nn_model_reload(buf, 8u, "x", &after);
	assert(rc == 0 && after == 1 && state_of(rc, after) == NN_MODEL_NEW);

	/* A backend that broke the contract and handed back NULL: no model, and
	 * the singleton closes rather than keep a dangling handle. */
	stub.null_handle = 1;
	stub.refuse_next = 1;
	after = -1;
	rc = nn_model_reload(buf, 8u, "x", &after);
	assert(rc < 0 && after == 0 && state_of(rc, after) == NN_MODEL_EMPTY);
	assert(!nn_model_present(m));

	printf("test_nn_model: OK\n");
	return 0;
}
#endif
