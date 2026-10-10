/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's `nn info` / `nn model load` / `nn model unload`
 * (port/nn/nn_svc_f746_model.c) behind the shared order, issue #131 step 7d.
 *
 * shell/test/test_nn_core_model.c walks the shared order against a stand-in
 * board.  What it cannot say is whether THIS board's hooks feed it the right
 * facts.  So this builds the real adapter half, the real port/nn/nn.c, the
 * real svc/nn_core_model.c and svc/nn_swap.c, over a stub backend that keeps
 * nn_backend.h's contract (and can be told to break it), and walks:
 *
 *   - had_open is read under the claim: a refused load over a model is
 *     PREVIOUS and touches nothing; the same refusal from empty is EMPTY;
 *   - every EMPTY ending releases the backend -- including a "restored" with
 *     nothing to restore, which must come out with no model at all;
 *   - a swap that "succeeds" with no model left is a failure (HW), not "loaded";
 *   - the refusals before the claim keep their status and words, and change
 *     nothing (the counter does not move);
 *   - `nn info` from another console while a swap is in flight answers BUSY on
 *     all three lines (the stub's reload asks it), and a held session with no
 *     load in flight -- a stream -- still gets the copy;
 *   - nothing is called inside the board's critical section.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nn.h"
#include "nn_backend.h"
#include "nn_camera.h"
#include "nn_svc.h"
#include "sdram.h"
#include "tx_api.h"
#include "stm32f7xx_hal.h"

DWT_Type       test_dwt = { .CTRL = DWT_CTRL_NOCYCCNT_Msk };
CoreDebug_Type test_coredebug;
int            test_tx_masked;

UINT tx_thread_sleep(ULONG ticks)
{
	(void)ticks;
	assert(!"the open latch was contended in a single-threaded test");
	return 0;
}

/* ---- the rest of the board ---------------------------------------------- */

static int camera_running, sdram_up = 1, invalidations;

bool nn_camera_running(void)
{
	assert(test_tx_masked == 0);
	return camera_running != 0;
}

void nn_camera_record_invalidate(void)
{
	assert(test_tx_masked == 0);
	invalidations++;
}

int sdram_is_up(void)
{
	return sdram_up;
}

/* ---- stub backend ------------------------------------------------------- */

static struct {
	int adopted;
	int has;            /* a model is there                               */
	int reloads, releases;
	int refuse_next;    /* the next reload of SD bytes is refused          */
	int restore_lies;   /* ...and claims to have restored a model anyway   */
	int null_handle;    /* answer the next reload with NULL                */
	int ask_info;       /* the next reload asks `nn info` mid-swap         */
	struct nn_svc_info mid;
	struct nn_tensor in;
} stub;

static char stub_buf[64];

static int st_init(void) { return 0; }
static int st_open(void **impl)
{
	if (!stub.adopted) {
		stub.adopted = 1;
		stub.has     = 1;   /* the built-in model, once */
	}
	*impl = &stub;
	return 0;
}
static void st_close(void *impl) { (void)impl; }
static const char *st_name(void *impl) { (void)impl; return stub.has ? "m" : "(none)"; }
static int st_in_count(void *impl) { (void)impl; return stub.has ? 1 : 0; }
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
	assert(test_tx_masked == 0);
	stub.reloads++;
	if (stub.ask_info) {
		/* Another console's `nn info`, in the middle of the swap. */
		stub.ask_info = 0;
		nn_svc_info(&stub.mid);
	}
	if (stub.null_handle) {
		stub.null_handle = 0;
		*impl = NULL;
		stub.has = 0;
		return 0;               /* a "success" that left nothing */
	}
	*impl = &stub;
	if (data != NULL && stub.refuse_next) {
		stub.refuse_next = 0;
		if (stub.restore_lies) {
			stub.restore_lies = 0;
			stub.has = 1;   /* "restored" -- from nothing */
		}
		return -7;
	}
	stub.has = 1;
	return 0;
}
static int st_release(void)
{
	assert(test_tx_masked == 0);
	stub.releases++;
	stub.has = 0;
	return 0;
}

const struct nn_backend_vt nn_backend_vt_selected = {
	.info = &(const struct nn_backend_info){ "stub", "" },
	.init = st_init, .open = st_open, .close = st_close,
	.model_name = st_name, .in_count = st_in_count, .out_count = st_in_count,
	.input = st_tensor, .output = st_tensor, .activations_bytes = st_acts,
	.run = st_run, .load_region = st_region, .reload = st_reload,
	.release = st_release, .arena_reserved = 524288u, .has_builtin = 1u,
};

/* ---- the console's side ------------------------------------------------- */

static int read_rc;
static int st_read(void *ctx, const char *path, void *buf, uint32_t cap,
                   uint32_t *len)
{
	(void)ctx; (void)path; (void)buf;
	*len = cap;
	return read_rc;
}

static struct nn_op_result res;
static enum nn_model_state state;
static int stream_holds;   /* the test holds the session, as a stream would */

static void load(uint8_t tag, const char *path)
{
	struct nn_spec spec;

	memset(&spec, 0, sizeof spec);
	memset(&res, 0, sizeof res);
	spec.tag  = tag;
	spec.path = path;
	state = (enum nn_model_state)99;
	nn_svc_model_load(&spec, st_read, NULL, &res, &state);
	assert(test_tx_masked == 0);
	assert(res.claim == NN_CLAIM_NONE);
	/* The session always goes back. */
	if (!stream_holds) {
		assert(nn_session_try_acquire() == 0);
		nn_session_release();
	}
}

static void unload(void)
{
	memset(&res, 0, sizeof res);
	nn_svc_model_unload(&res);
	assert(test_tx_masked == 0);
	assert(nn_session_try_acquire() == 0);
	nn_session_release();
}

static void info_is(int active, const char *name, int avail)
{
	struct nn_svc_info i;

	nn_svc_info(&i);
	assert(test_tx_masked == 0);
	assert(i.model_active == (uint8_t)active);
	assert(strcmp(i.model, name) == 0);
	assert(i.avail_identity == (uint8_t)avail);
	assert(i.arena_bytes == 524288u);
}

int main(void)
{
	int rel, inv, rl;

	/* Boot: the first `nn info` adopts the built-in model. */
	info_is(1, "m", NN_AVAIL_OK);

	/* A spec this board does not take: SPEC, worded, nothing touched. */
	load(NN_SPEC_NAME, NULL);
	assert(res.status == NN_SVC_ERR_SPEC && state == NN_MODEL_PREVIOUS);
	assert(strstr(res.detail, "asset store") != NULL);
	assert(stub.reloads == 0);

	/* Refusals before the claim keep their words. */
	camera_running = 1;
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_ERR_BUSY && state == NN_MODEL_PREVIOUS);
	assert(strcmp(res.detail, "stop the inference stream first "
	                          "(`nn stream stop`)") == 0);
	unload();
	assert(res.status == NN_SVC_ERR_BUSY && stub.has);
	camera_running = 0;
	sdram_up = 0;
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_ERR_STATE && state == NN_MODEL_PREVIOUS);
	sdram_up = 1;
	assert(nn_session_try_acquire() == 0);      /* a stream holds it */
	stream_holds = 1;
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_ERR_BUSY && state == NN_MODEL_PREVIOUS);
	assert(strcmp(res.detail, "NN busy (a stream, run or bench is "
	                          "active)") == 0);
	/* ...and `nn info` during a stream still gets the copy: no load is in
	 * flight, the counter has not moved. */
	info_is(1, "m", NN_AVAIL_OK);
	stream_holds = 0;
	nn_session_release();
	assert(stub.reloads == 0 && invalidations == 0);

	/* A read that fails over a model: PREVIOUS, nothing moved. */
	read_rc = -1;
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_PREVIOUS);
	assert(res.detail[0] == '\0');
	assert(stub.reloads == 0 && stub.releases == 0 && invalidations == 0);
	read_rc = 0;

	/* A refused model over a model: PREVIOUS, the backend restored it, no
	 * release, the last result kept. */
	stub.refuse_next = 1;
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_PREVIOUS);
	assert(strcmp(res.detail, "the model was refused (-7)") == 0);
	assert(stub.releases == 0 && invalidations == 0 && stub.has);

	/* A load, with `nn info` asked in the middle of the swap: BUSY on all
	 * three lines -- not a name and a size from two models. */
	stub.ask_info = 1;
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_OK && state == NN_MODEL_NEW);
	assert(stub.mid.avail_identity == NN_AVAIL_BUSY &&
	       stub.mid.avail_runtime == NN_AVAIL_BUSY &&
	       stub.mid.avail_tensors == NN_AVAIL_BUSY);
	assert(stub.mid.model[0] == '\0' && stub.mid.arena_used == 0u);
	assert(stub.mid.arena_bytes == 524288u);
	assert(invalidations == 1);
	info_is(1, "m", NN_AVAIL_OK);

	/* Unload: empty, idempotent, the last result goes. */
	unload();
	assert(res.status == NN_SVC_OK && !stub.has && stub.releases == 1);
	assert(invalidations == 2);
	info_is(0, "(none)", NN_AVAIL_OK);
	unload();
	assert(res.status == NN_SVC_OK && stub.releases == 2);

	/* A refused model FROM EMPTY: had_open = 0 under the claim, so EMPTY --
	 * "nothing is loaded" -- and the backend is released all the same. */
	rel = stub.releases;
	stub.refuse_next = 1;
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_EMPTY);
	assert(stub.releases == rel + 1 && !stub.has);

	/* [!] "RESTORED" WITH NOTHING TO RESTORE: a backend that claims it rebuilt
	 * a previous model when there was none.  Before step 7d this read as
	 * PREVIOUS; now it is EMPTY and the model is released. */
	rel = stub.releases;
	stub.refuse_next  = 1;
	stub.restore_lies = 1;
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_EMPTY);
	assert(stub.releases == rel + 1 && !stub.has);
	info_is(0, "(none)", NN_AVAIL_OK);

	/* A read that fails from empty: EMPTY, released (a no-op), no reload. */
	rl = stub.reloads;
	read_rc = -1;
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_EMPTY);
	assert(stub.reloads == rl);
	read_rc = 0;

	/* `builtin` brings the built-in model back. */
	load(NN_SPEC_BUILTIN, NULL);
	assert(res.status == NN_SVC_OK && state == NN_MODEL_NEW && stub.has);

	/* A "success" that left no model: a failure (HW), EMPTY, released. */
	rel = stub.releases;
	inv = invalidations;
	stub.null_handle = 1;
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_ERR_HW && state == NN_MODEL_EMPTY);
	assert(stub.releases == rel + 1 && invalidations == inv + 1);
	/* The singleton closed; the next open builds nothing. */
	info_is(0, "(none)", NN_AVAIL_OK);
	assert(stub.has == 0);

	printf("test_nn_model_life: OK\n");
	return 0;
}
