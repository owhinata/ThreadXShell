/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn.c
 * @brief   Backend-agnostic nn API dispatcher + inference timing
 * (owhinata/wio-lite-ai#9 P1, owhinata/stm32f746g-disco#81; one implementation
 * for wio-lite-ai and f746g-disco since issue #161).
 *
 * A thin layer over the compiled-in backend (nn_backend_vt_selected).  It adds
 * the cross-backend concerns the backends must not each reimplement:
 *   - a singleton public `struct nn_model` wrapping the backend's opaque handle,
 *     opened race-free even though several consoles can ask for it at once,
 *   - the single-session guard, and the external claim on the backend's memory
 *     (f746g-disco's `sdram test`, issue #152),
 *   - cycle-counter timing around nn_run() for nn_last_cycles().
 *
 * [!] THIS FILE OWNS NO STORAGE.  Every byte of state is the board's
 * nn_model_board_state (nn_dispatch_state.h), reached by an extern resolved at
 * link time; the critical section, the one-tick wait and the cycle counter are
 * the board's hooks.  So nothing here includes a HAL, a CMSIS device header or
 * tx_api.h, and the storage audit holds the compiled object to that per board.
 *
 * [!] NEGATIVE CODES ARE PASSED THROUGH, NEVER FOLDED.  This layer's own
 * refusals are the named NN_ERR_* codes (nn.h); a backend's reason for refusing
 * a model is returned exactly as the backend said it.
 *
 * Clean-room design; no third-party code reused.
 */
#include "nn.h"
#include "nn_backend.h"
#include "nn_detail.h"       /* NN_SVC_DETAIL_LIT: these reach `nn` (#122) */
#include "nn_dispatch_state.h"

#define NN_ST (&nn_model_board_state)

/* ---- public API ------------------------------------------------------------ */

int nn_init(void)
{
	int rc;

	if (NN_ST->inited)
		return 0;

	/* Timing is best-effort: with the counter stopped, inference still works
	 * and only nn_last_cycles() goes to 0, so this is not fatal to init. */
	NN_ST->cycles_ok = (nn_model_board_cycles_start() == 0) ? 1u : 0u;

	rc = nn_backend_vt_selected.init();
	if (rc != 0)
		return rc;

	NN_ST->inited = 1;
	return 0;
}

const struct nn_backend_info *nn_backend(void)
{
	return nn_backend_vt_selected.info;
}

int nn_model_open(struct nn_model **out)
{
	int rc;

	if (!out)
		return NN_ERR_ARG;

	/* Serialize the one-time singleton open/init across the consoles: claim an
	 * init latch under a brief critical section so exactly one thread runs
	 * backend init/open; any other waits for model.open to be published.  A
	 * pre-created mutex would just move the race into its own creation, and
	 * this way the already-open path stays lock-free. */
	for (;;) {
		uint32_t saved = nn_model_board_enter();
		int owner = 0;

		if (NN_ST->model.open) {
			nn_model_board_exit(saved);
			*out = &NN_ST->model;
			return 0;
		}
		/* [!] The first open of a backend with a model in the image
		 * (has_builtin: null, stedgeai) builds it into the backend's memory:
		 * refused, before anything is built, while an external claim holds
		 * that memory (issue #152).  Decided in the same critical section
		 * that takes the latch, so a claim and a build can never both be in.
		 * [!] ONLY THAT BACKEND (issue #159 Step 4c).  tflm's and
		 * stedgeai_reloc's first open builds nothing -- it only publishes the
		 * empty handle -- so refusing it would turn a fresh-boot `nn info`
		 * during `sdram test` into "busy" for a memory nothing touches. */
		if (NN_ST->scratch_claimed && nn_backend_vt_selected.has_builtin) {
			nn_model_board_exit(saved);
			return NN_ERR_SCRATCH_HELD;
		}
		if (!NN_ST->opening) { NN_ST->opening = 1; owner = 1; }
		nn_model_board_exit(saved);

		if (owner)
			break;
		nn_model_board_wait_tick();   /* another thread is opening; retry */
	}

	/* This thread owns the init. */
	rc = nn_init();
	if (rc == 0)
		rc = nn_backend_vt_selected.open(&NN_ST->model.impl);
	if (rc == 0) {
		NN_ST->model.last_cycles = 0;
		NN_ST->model.open = 1;            /* publish (waiters poll this) */
	}
	NN_ST->opening = 0;

	if (rc != 0)
		return rc;
	*out = &NN_ST->model;
	return 0;
}

void nn_model_close(struct nn_model *m)
{
	if (!m || !m->open)
		return;
	nn_backend_vt_selected.close(m->impl);
	m->impl = NULL;
	m->open = 0;
	m->last_cycles = 0;
}

const char *nn_model_name(const struct nn_model *m)
{
	if (!m || !m->open)
		return "(none)";
	return nn_backend_vt_selected.model_name(m->impl);
}

int nn_input_count(const struct nn_model *m)
{
	return (m && m->open) ? nn_backend_vt_selected.in_count(m->impl) : 0;
}

int nn_output_count(const struct nn_model *m)
{
	return (m && m->open) ? nn_backend_vt_selected.out_count(m->impl) : 0;
}

struct nn_tensor *nn_input(struct nn_model *m, int idx)
{
	return (m && m->open) ? nn_backend_vt_selected.input(m->impl, idx) : NULL;
}

struct nn_tensor *nn_output(struct nn_model *m, int idx)
{
	return (m && m->open) ? nn_backend_vt_selected.output(m->impl, idx) : NULL;
}

uint32_t nn_activations_bytes(const struct nn_model *m)
{
	return (m && m->open) ? nn_backend_vt_selected.activations_bytes(m->impl) : 0u;
}

uint32_t nn_arena_reserved(void)
{
	return nn_backend_vt_selected.arena_reserved;
}

/* A handle describes a model when it has inputs -- see nn_model_present(). */
static int nn_impl_has_model(void *impl)
{
	return impl != NULL && nn_backend_vt_selected.in_count(impl) > 0;
}

int nn_model_present(const struct nn_model *m)
{
	return (m && m->open) ? nn_impl_has_model(m->impl) : 0;
}

int nn_model_loaded(void)
{
	return nn_model_present(&NN_ST->model);
}

int nn_model_has_builtin(void)
{
	return nn_backend_vt_selected.has_builtin ? 1 : 0;
}

int nn_run(struct nn_model *m)
{
	uint32_t c0, c1;
	int rc;

	if (!m || !m->open)
		return NN_ERR_STATE;

	/* The board's read is fenced (a barrier drains outstanding accesses and
	 * flushes the pipeline), so the samples bracket the work and nothing
	 * either side leaks across. */
	c0 = nn_model_board_cycles();
	rc = nn_backend_vt_selected.run(m->impl);
	c1 = nn_model_board_cycles();

	/* Wrap-safe for one wrap only; nn_run()'s contract in nn.h bounds a single
	 * run well below that. */
	if (rc == 0)
		m->last_cycles = NN_ST->cycles_ok ? (uint32_t)(c1 - c0) : 0u;
	return rc;
}

uint32_t nn_last_cycles(const struct nn_model *m)
{
	return (m && m->open) ? m->last_cycles : 0u;
}

/* ---- runtime model swap ------------------------------------------------------
 *
 * These wrap the OPTIONAL vtable entries, and the reason they are written out
 * rather than left to the caller is that a NULL function pointer is a landmine:
 * a caller that reached through the vtable itself would branch to address 0.
 * Here it is a return value instead. */

int nn_model_load_region(void **buf, uint32_t *cap)
{
	if (!buf || !cap)
		return NN_ERR_ARG;
	if (NN_ST->scratch_claimed)
		return NN_ERR_SCRATCH_HELD;   /* the region is the claimed memory */
	if (!nn_backend_vt_selected.load_region)
		return NN_ERR_NOSUP;
	return nn_backend_vt_selected.load_region(buf, cap);
}

int nn_model_reload(const void *data, uint32_t len, const char *name,
                    int *model_after)
{
	void *impl = NULL;
	int rc;

	/* Unchanged on the early refusals below: nothing was touched. */
	if (model_after)
		*model_after = nn_model_present(&NN_ST->model);
	/* The singleton must already exist: reload REPLACES a model, and the open
	 * path is what runs backend init and settles the race between consoles. */
	if (!NN_ST->model.open)
		return NN_ERR_STATE;
	if (NN_ST->scratch_claimed)
		return NN_ERR_SCRATCH_HELD;   /* an external claim holds the memory */
	if (!nn_backend_vt_selected.reload)
		return NN_ERR_NOSUP;
	if (data == NULL)
		return NN_ERR_ARG;            /* an unload is nn_model_release() */

	/* The backend is transactional and reports the resulting handle in *impl:
	 * the new model (rc == 0), the restored previous one, or the empty handle
	 * when there was nothing to restore or even the restore failed (issue #131
	 * P16).  NULL breaks that contract; it is still mirrored -- the singleton
	 * closes and the next open asks the backend again -- rather than kept as a
	 * dangling impl. */
	rc = nn_backend_vt_selected.reload(data, len, name, &impl);
	if (impl) {
		NN_ST->model.impl = impl;
	} else {
		NN_ST->model.impl = NULL;
		NN_ST->model.open = 0;
	}
	NN_ST->model.last_cycles = 0;   /* it timed a model that is gone */
	/* This call's outcome, from the handle it just adopted -- see nn.h.  A
	 * MODEL, not an open handle: the empty handle is open and has none. */
	if (model_after)
		*model_after = nn_impl_has_model(impl);
	return rc;
}

int nn_model_release(void)
{
	int rc;

	if (!nn_backend_vt_selected.release)
		return NN_ERR_NOSUP;
	rc = nn_backend_vt_selected.release();
	if (rc != 0)
		return rc;
	NN_ST->model.last_cycles = 0;     /* it timed a model that is gone */
	return 0;
}

_Static_assert(NN_MODEL_ALIGN == 16u,
               "nn_model_strerror() spells NN_MODEL_ALIGN out; keep them together");

const char *nn_model_strerror(int rc)
{
	switch (rc) {
	case 0:                    return NN_SVC_DETAIL_LIT("ok");
	case NN_MODEL_ERR_VERSION: return NN_SVC_DETAIL_LIT("schema version this runtime cannot read");
	case NN_MODEL_ERR_OPS:     return NN_SVC_DETAIL_LIT("operator resolver could not be built");
	case NN_MODEL_ERR_ARENA:   return NN_SVC_DETAIL_LIT("activations do not fit the arena, or the model "
	                                                    "uses an operator this build did not register");
	case NN_MODEL_ERR_TENSOR:  return NN_SVC_DETAIL_LIT("a tensor has no buffer or too many dimensions");
	case NN_MODEL_ERR_SHAPE:   return NN_SVC_DETAIL_LIT("input/output tensor count out of range");
	case NN_MODEL_ERR_EMPTY:   return NN_SVC_DETAIL_LIT("empty or impossibly short model");
	case NN_MODEL_ERR_FORMAT:  return NN_SVC_DETAIL_LIT("not a valid model for this runtime");
	case NN_MODEL_ERR_SLOT:    return NN_SVC_DETAIL_LIT("model is not inside the staging region "
	                                                    "handed out");
	case NN_MODEL_ERR_ALIGN:   return NN_SVC_DETAIL_LIT("model does not start on a 16-byte boundary");
	/* ^ the sentence names the number; nn.h owns it -- see the assert above */
	case NN_ERR_ARG:           return NN_SVC_DETAIL_LIT("bad argument");
	case NN_ERR_NOSUP:         return NN_SVC_DETAIL_LIT("this backend cannot load a model at run time");
	case NN_ERR_STATE:         return NN_SVC_DETAIL_LIT("no model session is open");
	case NN_ERR_SCRATCH_HELD:  return NN_SVC_DETAIL_LIT("the model memory is held by a memory test; "
	                                                    "try again when it finishes");
	default:                   return NN_SVC_DETAIL_LIT("unknown error");
	}
}

/* ---- single-session guard -------------------------------------------------- */

/* A plain flag test-set under a brief critical section: interrupt-safe,
 * thread-agnostic (acquire and release may run on different threads), and needs
 * no one-time init, which would itself race between the consoles. */
int nn_session_try_acquire(void)
{
	uint32_t saved = nn_model_board_enter();
	int rc;

	if (NN_ST->scratch_claimed)
		rc = NN_ERR_SCRATCH_HELD;     /* an external claim holds the memory */
	else if (!NN_ST->session_busy) {
		NN_ST->session_busy = 1;
		rc = 0;
	} else
		rc = -1;                      /* busy */
	nn_model_board_exit(saved);
	return rc;
}

void nn_session_release(void)
{
	uint32_t saved = nn_model_board_enter();

	NN_ST->session_busy = 0;
	nn_model_board_exit(saved);
}

/* ---- external claim on the backend's memory (issue #152) ------------------- */

int nn_scratch_claim(void)
{
	uint32_t saved = nn_model_board_enter();
	int rc;

	/* [!] ONE CRITICAL SECTION for every question and the set.  The session
	 * flag, the open latch and the claim are all written under it, and a model
	 * can appear only under the session (load) or under the latch (the first
	 * open) -- so with both free here, nn_model_loaded() cannot change before
	 * the claim is set, and once it is set neither can be taken. */
	if (NN_ST->scratch_claimed || NN_ST->session_busy || NN_ST->opening)
		rc = -1;                      /* an nn command, a stream or a test */
	else if (nn_model_loaded())
		rc = -2;                      /* a model lives in the memory        */
	else {
		NN_ST->scratch_claimed = 1;
		rc = 0;
	}
	nn_model_board_exit(saved);
	return rc;
}

void nn_scratch_release(void)
{
	uint32_t saved = nn_model_board_enter();

	NN_ST->scratch_claimed = 0;
	nn_model_board_exit(saved);
}
