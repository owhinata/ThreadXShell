/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_ethosu.cc
 * @brief   The Ethos-U backend for the nn layer (issue #169): a TFLM
 *          MicroInterpreter whose one operator hands a Vela command stream to the
 *          NPU, behind the vtable in svc/nn/nn_backend.h.
 *
 * Built for grove-vision-ai-v2 (Ethos-U55) against the TFLM tree that board's SDK
 * pins.  svc/nn/nn.c dispatches to it exactly as it dispatches to the tflm backend
 * on the other two boards; what differs is WHERE a model comes from and what is
 * proved about it before the interpreter sees it, and that is this file's policy.
 *
 * [!] A MODEL IS INTERPRETED IN PLACE.  The flatbuffer is read where it lies, inside
 * the board's memory-mapped read window (nn_ethosu_board_facts), and never copied:
 * there is no staging buffer, no second slot and no load_region().  The interpreter
 * keeps pointers into those bytes for as long as the model is loaded, and nothing
 * here can make them stay put -- the CALLER's lease on the window does (svc/nn/nn.h,
 * "WHERE data MAY POINT"), from before reload() until the model is gone, the
 * restore of the previous model included.
 *
 * [!] THE ORDER OF THE CHECKS IS THE POINT (issues #45, #46, #93), cheapest and
 * least trusting first: the range inside the window, then the length (it has a
 * floor, because the identifier is read at offset 4), then the alignment, then the
 * file identifier on raw bytes, then the BOUNDED flatbuffer verifier (npu_verify.h,
 * the one place its limits are declared -- the host gate runs the same one), and
 * only then is the buffer followed as a model: the schema version, then the
 * Ethos-U payload walk (npu_model_scan / npu_payload: exactly one COMMAND_STREAM,
 * last, in input tensor 0, never a variable tensor), then the op resolver (one
 * operator), then AllocateTensors.  Each refusal is its own code and none is folded
 * into another (nn.h); a refusal before AllocateTensors changes nothing.
 *
 * [!] THE ARENA'S CACHE MAINTENANCE IS NOT HERE AND NOT THE CALLER'S.  The protocol
 * (svc/nn/ethosu/npu_cache.c) maintains the whole arena at the two instants it
 * changes hands, from the driver's own hooks inside Invoke().  This file only tells
 * it which arena is live -- registered before AllocateTensors, taken back whenever
 * the interpreter is destroyed -- and closes out each Invoke() with the backstop
 * that catches a launch which never handed the arena back.
 *
 * WHAT IS THE BOARD'S.  The state object and the facts (nn_ethosu_state.h), the
 * arena's placement (nn_ethosu_mem.h), the NN_TFLM_OPS_ETHOSU_PROFILE define, and
 * everything about the NPU's bring-up, its interrupt and the window's lease.  The
 * glue that does not depend on staging is shared with the tflm backend
 * (svc/nn/tflm/nn_tflm_glue.h).  This file owns no storage.
 */
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "nn.h"
#include "nn_backend.h"
#include "nn_detail.h"        /* NN_SVC_DETAIL_LIT: these reach `nn` (#122) */
#include "nn_ethosu.h"
#include "nn_ethosu_io.h"     /* the I/O rule, shared with the host gate */
#include "nn_ethosu_mem.h"    /* the board-placed arena */
#include "nn_ethosu_state.h"  /* the board-defined state and facts; the op profile */
#include "npu_cache.h"        /* the arena ownership protocol (npu_cache.c) */
#include "npu_model_scan.h"   /* the payload walk, shared with the host gate */
#include "npu_verify.h"       /* the bounded verifier and its limits */

/* The glue shared with the tflm backend works on the object named here. */
#define NN_TFLM_GLUE_STATE  nn_ethosu_board_state
#define NN_TFLM_GLUE_FACTS  nn_ethosu_board_facts
#include "nn_tflm_glue.h"

#include <new>
#include <cstddef>
#include <cstdint>
#include <cstring>

static_assert(NN_TFLM_OPS_TOTAL == 1,
              "the Ethos-U backend registers one operator: the ethos-u custom op");
static_assert(NN_ETHOSU_ARENA_BYTES % 32u == 0u,
              "the arena is maintained whole, so it must be whole cache lines");

namespace {

/* Destroy the interpreter AND take the arena back from the cache protocol: a stale
 * base there would have the flush hook cleaning memory nothing owns any more. */
void drop_interp()
{
	nn_tflm_destroy_interp();
	npu_cache_set_arena(nullptr, 0u);
}

/*
 * Is [@p a, @p a + @p len) wholly inside the board's read window?  Subtraction-based:
 * `a + len` is never formed, so a length near 2^32 is refused rather than wrapped
 * into looking contained.
 */
int window_check(uintptr_t a, uint32_t len)
{
	const uint32_t base = nn_ethosu_board_facts.window_base;
	const uint32_t size = nn_ethosu_board_facts.window_size;

	if (a < base || a - base >= size)
		return NN_MODEL_ERR_WINDOW;
	if (len < NPU_MODEL_MIN_BYTES)
		return NN_MODEL_ERR_EMPTY;
	if (len > size - (uint32_t)(a - base))
		return NN_MODEL_ERR_WINDOW;
	return 0;
}


/*
 * Build an interpreter for @p data (@p len bytes, in place) and publish its I/O.
 *
 * A refusal before the interpreter is touched leaves everything as it was -- the
 * previous interpreter included.  A refusal after that point leaves NO interpreter
 * and the handle untouched, so every caller on the failure path must publish
 * something before returning: see ethosu_bk_reload().
 *
 * Returns 0 or one of the NN_MODEL_ERR_* codes in nn.h.
 */
int build_interp(const void *data, uint32_t len)
{
	auto &s = nn_ethosu_board_state;
	const uintptr_t a = reinterpret_cast<uintptr_t>(data);
	const uint8_t *raw = static_cast<const uint8_t *>(data);
	int rc;

	/* Range and length first: everything after this dereferences the address,
	 * and everything after the verifier trusts the length. */
	rc = window_check(a, len);
	if (rc != 0)
		return rc;
	if ((a % NN_MODEL_ALIGN) != 0u)
		return NN_MODEL_ERR_ALIGN;

	/* The file identifier, on raw bytes inside the declared length (the floor
	 * above is what makes offset 4..7 inside it).  Erased flash reads 0xFF and
	 * fails here rather than inside the parser.  Its own code: "this is not a
	 * model" is not "this model is malformed". */
	if (memcmp(raw + 4, "TFL3", 4) != 0)
		return NN_MODEL_ERR_IDENT;

	/* [!] AND ONLY NOW IS THE BUFFER SAFE TO FOLLOW (issue #93).  GetModel() is a
	 * cast and the first accessor after it already follows an offset. */
	if (!npu_verify_model_buffer(raw, (size_t)len))
		return NN_MODEL_ERR_FORMAT;

	const tflite::Model *model = tflite::GetModel(data);

	if (model->version() != TFLITE_SCHEMA_VERSION)
		return NN_MODEL_ERR_VERSION;

	/* Before anything can be launched: prove no driver action follows the command
	 * stream (the walk is shared with the host gate, scripts/verify_vela_model.cc,
	 * which reaches this same verdict on the same bytes). */
	if (npu_model_payload_refusal(model) != nullptr)
		return NN_MODEL_ERR_PAYLOAD;

	nn_ethosu_op_resolver *resolver = nn_tflm_get_resolver();
	if (!resolver)
		return NN_MODEL_ERR_OPS;

	/* From here the previous interpreter is gone.  The cache protocol is told
	 * which arena before AllocateTensors (setting it also clears the protocol's
	 * state, so the previous registration needs no separate unwind), and every
	 * refusal below goes through drop_interp() and takes it back again. */
	nn_tflm_destroy_interp();
	npu_cache_set_arena(nn_ethosu_arena, (size_t)NN_ETHOSU_ARENA_BYTES);
	s.interp = new (s.interp_buf) tflite::MicroInterpreter(
		model, *resolver, nn_ethosu_arena, (size_t)NN_ETHOSU_ARENA_BYTES);

	if (s.interp->AllocateTensors() != kTfLiteOk) {
		rc = NN_MODEL_ERR_ARENA;
		goto refused;
	}

	/* 1..NN_MAX_IO inputs and outputs, every tensor present with a buffer and a
	 * rank this API can describe -- the tflm backend's rule, so `nn` means the
	 * same thing by a loaded model on every board.  Inputs first, then outputs,
	 * in one walk. */
	{
		size_t n_in  = s.interp->inputs_size();
		size_t n_out = s.interp->outputs_size();

		if (!nn_ethosu_io_count_ok(n_in) || !nn_ethosu_io_count_ok(n_out)) {
			rc = NN_MODEL_ERR_SHAPE;
			goto refused;
		}
		for (size_t i = 0; i < n_in + n_out; i++) {
			if (!nn_ethosu_tensor_ok(i < n_in ? s.interp->input(i)
			                                  : s.interp->output(i - n_in))) {
				rc = NN_MODEL_ERR_TENSOR;
				goto refused;
			}
		}

		/* Counts-last, zeroed first: `nn info` reads these descriptors without
		 * the session (nn_backend.h), so the only states it may observe are "no
		 * model" and the complete new one.  Ordering, not atomicity -- nothing
		 * there is dereferenced. */
		s.tm.n_in  = 0;
		s.tm.n_out = 0;
		for (size_t i = 0; i < n_in; i++)
			nn_tflm_fill_tensor(&s.tm.in[i], s.interp->input(i));
		for (size_t i = 0; i < n_out; i++)
			nn_tflm_fill_tensor(&s.tm.out[i], s.interp->output(i));
		s.tm.used  = (uint32_t)s.interp->arena_used_bytes();
		s.tm.n_in  = (int)n_in;
		s.tm.n_out = (int)n_out;
	}
	return 0;

refused:
	/* After the interpreter was touched: none is left, and the arena goes back
	 * to the cache protocol with it. */
	drop_interp();
	return rc;
}

/* No model and no interpreter: the steady empty state (issue #131 P16). */
void go_empty()
{
	auto &s = nn_ethosu_board_state;

	drop_interp();
	s.active_model = nullptr;
	s.active_len   = 0u;
	nn_tflm_publish_empty();
	s.tm.open = true;
}

}  /* namespace */

extern "C" {

static int ethosu_bk_init(void)
{
	return 0;   /* the NPU's bring-up is the board's, before a model is loaded */
}

/* Opens with no model -- the empty state release() also ends in, entered the
 * same way (nothing is built yet, so there is nothing to destroy).  Touches no
 * tensor body. */
static int ethosu_bk_open(void **impl_out)
{
	auto &s = nn_ethosu_board_state;

	if (!s.tm.open)
		go_empty();
	*impl_out = &s.tm;
	return 0;
}

static void ethosu_bk_close(void *impl)
{
	(void)impl;   /* singleton: the interpreter outlives any one command */
}

/* Unload: destroy the interpreter and stay open with no model. */
static int ethosu_bk_release(void)
{
	go_empty();
	return 0;
}

/*
 * Adopt the model at @p data, in place.  Transactional: on any rejection the
 * previously active model is rebuilt FROM THE SAME BYTES -- which the caller's lease
 * still holds in place -- and reported; when nothing was loaded before, or even the
 * restore fails, the backend stays open and EMPTY.  *impl_out is always the
 * resulting handle.
 */
static int ethosu_bk_reload(const void *data, uint32_t len, const char *name,
                            void **impl_out)
{
	auto &s = nn_ethosu_board_state;
	const void *old_model = s.active_model;
	const uint32_t old_len = s.active_len;
	int rc;

	/* nn.c refuses data == NULL before calling (an unload is release()). */
	*impl_out = &s.tm;

	/* The handle is already open: nn.c calls reload() only on an open
	 * singleton, and nothing here closes it. */
	rc = build_interp(data, len);
	if (rc == 0) {
		s.active_model = data;
		s.active_len   = len;
		nn_tflm_set_model_name(name);
		return 0;
	}

	/* Rejected.  If the refusal came after the interpreter was touched, the
	 * handle describes tensors that no longer exist -- something must be
	 * published before returning, whatever the outcome. */
	if (old_model != nullptr && build_interp(old_model, old_len) == 0) {
		/* The name buffer still holds the previous name (only success paths
		 * overwrite it) and the handle already points at it. */
		s.active_model = old_model;
		s.active_len   = old_len;
		return rc;                       /* why the NEW model was refused */
	}

	/* [!] EMPTY MEANS NO INTERPRETER EITHER.  A refusal before the interpreter
	 * was touched leaves the previous one alive; with nothing to restore it
	 * must not survive the handle that described it. */
	go_empty();
	return rc;
}

static const char *ethosu_bk_name(void *impl)
{
	return ((struct nn_ethosu_model *)impl)->name;
}

static int ethosu_bk_in_count(void *impl)  { return ((struct nn_ethosu_model *)impl)->n_in; }
static int ethosu_bk_out_count(void *impl) { return ((struct nn_ethosu_model *)impl)->n_out; }

static struct nn_tensor *ethosu_bk_input(void *impl, int idx)
{
	struct nn_ethosu_model *m = (struct nn_ethosu_model *)impl;

	return (idx >= 0 && idx < m->n_in) ? &m->in[idx] : nullptr;
}

static struct nn_tensor *ethosu_bk_output(void *impl, int idx)
{
	struct nn_ethosu_model *m = (struct nn_ethosu_model *)impl;

	return (idx >= 0 && idx < m->n_out) ? &m->out[idx] : nullptr;
}

static uint32_t ethosu_bk_acts_bytes(void *impl)
{
	return ((struct nn_ethosu_model *)impl)->used;
}

/*
 * One inference.  Blocks the calling thread on the driver's semaphore (a ThreadX
 * one, the board's npu_rtos.c), so the rest of the shell keeps running.
 *
 * No cache maintenance here, and none may be added: TFLM has already written the
 * arena on its way out of Invoke().  npu_cache_after_invoke() only closes out the
 * protocol's state and fail-stops on a launch that never handed the arena back.
 */
static int ethosu_bk_run(void *impl)
{
	const auto &s = nn_ethosu_board_state;

	(void)impl;
	if (!s.interp)
		return NN_ERR_STATE;   /* open, but no model loaded */

	const TfLiteStatus st = s.interp->Invoke();

	npu_cache_after_invoke();
	return (st == kTfLiteOk) ? 0 : -1;
}

static const struct nn_backend_info g_info = {
	"ethosu", "tflite-micro + Ethos-U (models read in place)" };

const struct nn_backend_vt nn_backend_vt_selected = {
	.info              = &g_info,
	.init              = ethosu_bk_init,
	.open              = ethosu_bk_open,
	.close             = ethosu_bk_close,
	.model_name        = ethosu_bk_name,
	.in_count          = ethosu_bk_in_count,
	.out_count         = ethosu_bk_out_count,
	.input             = ethosu_bk_input,
	.output            = ethosu_bk_output,
	.activations_bytes = ethosu_bk_acts_bytes,
	.run               = ethosu_bk_run,
	.load_region       = nullptr,                 /* in place: nothing to stage */
	.reload            = ethosu_bk_reload,
	.release           = ethosu_bk_release,
	.arena_reserved    = NN_ETHOSU_ARENA_BYTES,   /* nn_ethosu_arena */
	.has_builtin       = 0u,                      /* empty until a load */
};

/* The in-place refusals in this backend's words; every other code in nn.c's. */
const char *nn_ethosu_strerror(int rc)
{
	switch (rc) {
	case NN_MODEL_ERR_WINDOW:  return NN_SVC_DETAIL_LIT("model address outside the flash window");
	case NN_MODEL_ERR_IDENT:   return NN_SVC_DETAIL_LIT("no TFL3 model at that address");
	case NN_MODEL_ERR_PAYLOAD: return NN_SVC_DETAIL_LIT("model payload has actions after the "
	                                                    "command stream");
	default:                   return nn_model_strerror(rc);
	}
}

}  /* extern "C" */
