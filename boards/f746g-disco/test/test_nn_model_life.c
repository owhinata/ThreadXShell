/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's `nn info` / `nn model load` / `nn model unload`
 * (port/nn/nn_svc_f746_model.c) behind the shared order, issue #131 step 7d.
 *
 * shell/test/test_nn_core_model.c walks the shared order against a stand-in
 * board.  What it cannot say is whether THIS board's hooks feed it the right
 * facts.  So this builds the real adapter half, the real svc/nn/nn.c (over
 * shell/test/nn_model_host.c, the board hooks modelled), the
 * real svc/nn/nn_core_model.c and svc/nn/nn_swap.c, over a stub backend that keeps
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
 *   - nothing is called inside the board's critical section;
 *   - the board is empty from boot and `builtin` is not a source it takes
 *     (issue #159 Step 4c): SPEC, worded, nothing touched;
 *   - since issue #194 the model is a blob, read by name out of the QSPI asset
 *     store (a stand-in, nn_blob_stub.c): `--path` is refused as SPEC; a bad
 *     name, a name in no slot, a name in two, unreadable headers, a busy store,
 *     a blob longer than the staging buffer, a payload read that fails and a
 *     copy whose CRC disagrees with the header each refuse with their own
 *     status and words; and the blob flag is taken once and given back once on
 *     every one of those paths and on success.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nn_blob_stub.h"
#include "nn.h"
#include "nn_backend.h"
#include "nn_camera.h"
#include "nn_svc.h"
#include "sdram.h"
#include "tx_api.h"

int            test_tx_masked;

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
	/* Nothing in the image (issue #159 Step 4c): the first open builds
	 * nothing, as the tflm backend's does. */
	stub.adopted = 1;
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
	.release = st_release, .arena_reserved = 524288u, .has_builtin = 0u,
};

/* ---- the console's side ------------------------------------------------- */

static const char model_bytes[] = "a pretend model of 32 bytes ....";

static struct nn_op_result res;
static enum nn_model_state state;
static int stream_holds;   /* the test holds the session, as a stream would */

static void load(uint8_t tag, const char *what)
{
	struct nn_spec spec;
	unsigned acq;

	memset(&spec, 0, sizeof spec);
	memset(&res, 0, sizeof res);
	spec.tag  = tag;
	if (tag == NN_SPEC_PATH)
		spec.path = what;
	else
		strncpy(spec.name, what ? what : "", NN_SPEC_NAME_MAX);
	state = (enum nn_model_state)99;
	acq = nn_blob_stub.acquires;
	nn_svc_model_load(&spec, NULL, NULL, &res, &state);
	assert(test_tx_masked == 0);
	assert(res.claim == NN_CLAIM_NONE);
	/* [!] The blob flag: taken at most once, and never left held. */
	assert(!nn_blob_stub.held);
	assert(nn_blob_stub.acquires - acq <= 1u);
	assert(nn_blob_stub.acquires == nn_blob_stub.releases);
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
	unsigned reads;

	nn_blob_stub_reset();
	nn_blob_stub_put(1u, "a", model_bytes, 32u);

	/* Boot: nothing in the image, so the first `nn info` finds no model. */
	info_is(0, "(none)", NN_AVAIL_OK);

	/* [!] `builtin` IS NOT A SOURCE HERE ANY MORE (issue #159 Step 4c): the
	 * shared SPEC refusal, worded, refused before anything is acquired. */
	load(NN_SPEC_BUILTIN, NULL);
	assert(res.status == NN_SVC_ERR_SPEC && state == NN_MODEL_EMPTY);
	assert(strstr(res.detail, "built-in model") != NULL);
	assert(stub.reloads == 0 && invalidations == 0 && !stub.has);

	/* [!] `--path` is not a source here any more (issue #194). */
	load(NN_SPEC_PATH, "a.tflite");
	assert(res.status == NN_SVC_ERR_SPEC && state == NN_MODEL_EMPTY);
	assert(strstr(res.detail, "it has no filesystem load (send the file "
	                          "with `blob write`)") != NULL);
	assert(stub.reloads == 0 && nn_blob_stub.acquires == 0u);

	/* The first model comes from the store, by name. */
	load(NN_SPEC_NAME, "a");
	assert(res.status == NN_SVC_OK && state == NN_MODEL_NEW && stub.has);
	info_is(1, "m", NN_AVAIL_OK);
	stub.reloads  = 0;
	invalidations = 0;

	/* A spec this board does not take: SPEC, worded, nothing touched. */
	load(NN_SPEC_SLOT, NULL);
	assert(res.status == NN_SVC_ERR_SPEC && state == NN_MODEL_PREVIOUS);
	assert(strstr(res.detail, "load by slot number") != NULL);
	assert(stub.reloads == 0);

	/* A name the store could never have stored: ARG, nothing acquired. */
	load(NN_SPEC_NAME, "has space");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_PREVIOUS);
	assert(strstr(res.detail, "is not a blob name") != NULL);
	assert(nn_blob_stub.finds == 1u);   /* the first load's only */

	/* [!] THE STORE'S ANSWERS, each its own status and words, each over a
	 * model and each leaving it (PREVIOUS), the flag back every time. */
	reads = nn_blob_stub.reads;
	load(NN_SPEC_NAME, "nothere");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_PREVIOUS);
	assert(strstr(res.detail, "'nothere': ") != NULL &&
	       strstr(res.detail, "(`blob list` shows what is where)") != NULL);
	nn_blob_stub_put(2u, "twice", model_bytes, 32u);
	nn_blob_stub_put(3u, "twice", model_bytes, 32u);
	load(NN_SPEC_NAME, "twice");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_PREVIOUS);
	assert(strstr(res.detail, "'twice': ") != NULL);
	nn_blob_stub.find_rc = BLOB_ERR_IO;
	load(NN_SPEC_NAME, "a");
	nn_blob_stub.find_rc = BLOB_OK;
	assert(res.status == NN_SVC_ERR_HW && state == NN_MODEL_PREVIOUS);
	assert(strstr(res.detail, "cannot read the blob headers (-2") != NULL);
	nn_blob_stub.held_elsewhere = 1;     /* a `blob write`, a `qspi test` */
	load(NN_SPEC_NAME, "a");
	nn_blob_stub.held_elsewhere = 0;
	assert(res.status == NN_SVC_ERR_BUSY && state == (enum nn_model_state)99);
	assert(strstr(res.detail, "blob busy") != NULL);
	nn_blob_stub_put(2u, "big", model_bytes, 65u);   /* the stub's region: 64 B */
	load(NN_SPEC_NAME, "big");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_PREVIOUS);
	assert(strcmp(res.detail, "slot 2 is 65 B, staging holds 64 B") == 0);
	/* [!] None of those read a payload byte. */
	assert(nn_blob_stub.reads == reads);
	/* The read itself fails, and the copy disagrees with the header: HW. */
	nn_blob_stub.read_rc = BLOB_ERR_IO;
	load(NN_SPEC_NAME, "a");
	nn_blob_stub.read_rc = BLOB_OK;
	assert(res.status == NN_SVC_ERR_HW && state == NN_MODEL_PREVIOUS);
	assert(strcmp(res.detail, "QSPI read failed (-2)") == 0);
	nn_blob_stub.info[1].crc32 ^= 1u;
	load(NN_SPEC_NAME, "a");
	nn_blob_stub.info[1].crc32 ^= 1u;
	assert(res.status == NN_SVC_ERR_HW && state == NN_MODEL_PREVIOUS);
	assert(strstr(res.detail, "CRC32 mismatch -- stored ") != NULL);
	assert(nn_blob_stub.last_read_len == 32u);
	assert(stub.reloads == 0 && invalidations == 0 && stub.has);

	/* Refusals before the claim keep their words. */
	camera_running = 1;
	load(NN_SPEC_NAME, "a");
	/* BUSY reports no state and leaves the caller's untouched (#138). */
	assert(res.status == NN_SVC_ERR_BUSY && state == (enum nn_model_state)99);
	assert(strcmp(res.detail, "stop the inference stream first "
	                          "(`nn stream stop`)") == 0);
	unload();
	assert(res.status == NN_SVC_ERR_BUSY && stub.has);
	camera_running = 0;
	sdram_up = 0;
	load(NN_SPEC_NAME, "a");
	assert(res.status == NN_SVC_ERR_STATE && state == NN_MODEL_PREVIOUS);
	sdram_up = 1;
	assert(nn_session_try_acquire() == 0);      /* a stream holds it */
	stream_holds = 1;
	load(NN_SPEC_NAME, "a");
	assert(res.status == NN_SVC_ERR_BUSY && state == (enum nn_model_state)99);
	assert(strcmp(res.detail, "NN busy (a stream, run or bench is "
	                          "active)") == 0);
	/* ...and `nn info` during a stream still gets the copy: no load is in
	 * flight, the counter has not moved. */
	info_is(1, "m", NN_AVAIL_OK);
	stream_holds = 0;
	nn_session_release();
	assert(stub.reloads == 0 && invalidations == 0);


	/* A refused model over a model: PREVIOUS, the backend restored it, no
	 * release, the last result kept. */
	stub.refuse_next = 1;
	load(NN_SPEC_NAME, "a");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_PREVIOUS);
	assert(strcmp(res.detail, "the model was refused (-7)") == 0);
	assert(stub.releases == 0 && invalidations == 0 && stub.has);

	/* A load, with `nn info` asked in the middle of the swap: BUSY on all
	 * three lines -- not a name and a size from two models. */
	stub.ask_info = 1;
	load(NN_SPEC_NAME, "a");
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
	load(NN_SPEC_NAME, "a");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_EMPTY);
	assert(stub.releases == rel + 1 && !stub.has);

	/* [!] "RESTORED" WITH NOTHING TO RESTORE: a backend that claims it rebuilt
	 * a previous model when there was none.  Before step 7d this read as
	 * PREVIOUS; now it is EMPTY and the model is released. */
	rel = stub.releases;
	stub.refuse_next  = 1;
	stub.restore_lies = 1;
	load(NN_SPEC_NAME, "a");
	assert(res.status == NN_SVC_ERR_ARG && state == NN_MODEL_EMPTY);
	assert(stub.releases == rel + 1 && !stub.has);
	info_is(0, "(none)", NN_AVAIL_OK);

	/* A read that fails from empty: EMPTY, no reload. */
	rl = stub.reloads;
	nn_blob_stub.read_rc = BLOB_ERR_IO;
	load(NN_SPEC_NAME, "a");
	nn_blob_stub.read_rc = BLOB_OK;
	assert(res.status == NN_SVC_ERR_HW && state == NN_MODEL_EMPTY);
	assert(stub.reloads == rl);

	/* A load from the store brings a model back. */
	load(NN_SPEC_NAME, "a");
	assert(res.status == NN_SVC_OK && state == NN_MODEL_NEW && stub.has);

	/* A "success" that left no model: a failure (HW), EMPTY, released. */
	rel = stub.releases;
	inv = invalidations;
	stub.null_handle = 1;
	load(NN_SPEC_NAME, "a");
	assert(res.status == NN_SVC_ERR_HW && state == NN_MODEL_EMPTY);
	assert(stub.releases == rel + 1 && invalidations == inv + 1);
	/* The singleton closed; the next open builds nothing. */
	info_is(0, "(none)", NN_AVAIL_OK);
	assert(stub.has == 0);

	printf("test_nn_model_life: OK\n");
	return 0;
}
