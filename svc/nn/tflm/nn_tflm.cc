/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm.cc
 * @brief   TensorFlow Lite Micro backend for the nn layer (owhinata/wio-lite-ai#9
 * phase 2c, Epic owhinata/stm32f746g-disco#80; one implementation for wio-lite-ai and
 * f746g-disco since issue #161).
 *
 * Bridges a TFLM MicroInterpreter to the backend-agnostic vtable in nn_backend.h.
 * Compiled only when CONFIG_NN_BACKEND=tflm, into the `tflm` static library alongside
 * the tflite-micro (+ CMSIS-NN) tree each board's cmake/tflite-micro.cmake generates
 * at configure time.
 *
 * WHAT IS THE BOARD'S.  Three things, and only these: where the arena and the model
 * slots live (nn_tflm_mem.h, defined in the board's nn_tflm_bufs.c), the state object
 * (nn_tflm_state.h, defined in the board's nn_tflm_board.cc together with its facts --
 * the display-name prefix), and compile definitions: the arena and slot sizes, the op
 * profile (nn_tflm_ops.h) and NN_TFLM_VERIFY.  Everything else -- the validation
 * order, the error codes, the double buffering, the transactional reload, the
 * descriptor publication order -- is one policy here.  This file owns no storage.
 *
 * The pieces the Ethos-U backend needs in the same shape (descriptors, the display
 * name, the empty state, the op resolver, the interpreter's destruction) are in
 * nn_tflm_glue.h, included below with this file's state named (issue #169).
 *
 * [!] THERE IS NO BUILT-IN MODEL.  open() succeeds with NO MODEL: zero inputs, zero
 * outputs, the name "(none)".  A model arrives by load_region() + reload() -- out of
 * wio-lite-ai's NOR blob store, off f746g-disco's SD card (since its issue #159 Step
 * 4c) -- and release() (`nn model unload`) returns to that state.  Three things fall
 * out of it, all wanted:
 *
 *   - `nn info` works on a board that has never been given a model, and reports the
 *     backend honestly instead of failing to open.
 *   - The nn_backend_vt contract that open() must not touch tensor bodies -- because
 *     `nn info` is called with the external memory down -- is satisfied structurally:
 *     with no model there are no tensors to touch, and the arena is first written by
 *     AllocateTensors() inside a load, which brings that memory up.
 *   - "Empty" is a steady state, so a refused load with nothing to restore, and a
 *     restore that failed too (issue #131 P16), both end there, open.
 *
 * WHAT RUNS WHEN.  Nothing here has a global constructor: the interpreter and the op
 * resolver are placement-new'd into the board's state object (raw storage) on first
 * use.  Static constructors would run from __libc_init_array, before the MPU, the
 * caches and the external memory exist.
 *
 * Clean-room bridge over a third-party runtime; the structure follows the donor
 * firmware ../stm32f746g-disco (owhinata/stm32f746g-disco#88).
 */
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "nn.h"
#include "nn_backend.h"
#include "nn_tflm_fit.h"     /* which slot a model range may be adopted from */
#include "nn_tflm_mem.h"     /* the board-placed arena, slots and capacity */
#include "nn_tflm_ops.h"     /* the op list, shared with verify_tflite */
#include "nn_tflm_state.h"   /* the board-defined state and facts */

/* The glue shared with the Ethos-U backend works on the object named here. */
#define NN_TFLM_GLUE_STATE  nn_tflm_board_state
#define NN_TFLM_GLUE_FACTS  nn_tflm_board_facts
#include "nn_tflm_glue.h"    /* descriptors, name, resolver, interpreter (shared) */

#include <new>
#include <cstddef>
#include <cstdint>

/* Compile-time canary.  CMSIS-NN's int8 kernels only take their SIMD path when the
 * compiler advertises the DSP extension; Cortex-M7 is armv7e-m, so GCC defines
 * __ARM_FEATURE_DSP.  If an -mcpu/-mfpu change ever drops it, the build must fail
 * loudly rather than quietly falling back to CMSIS-NN's plain-C path -- which would
 * look like nothing more than a disappointing benchmark, four times slower. */
#if defined(NN_TFLM_CMSIS_NN) && !defined(__ARM_FEATURE_DSP)
#error "CMSIS-NN build without __ARM_FEATURE_DSP -- the SIMD int8 path is not selected"
#endif

/* Whether the flatbuffer verifier runs on the board (0 or 1, from the board's cmake).
 * No default: wio-lite-ai turns it off (the same check runs on the PC, and it is the
 * largest single item in this backend's flash cost), f746g-disco keeps it on. */
#ifndef NN_TFLM_VERIFY
#error "NN_TFLM_VERIFY is not defined: the board's cmake/tflite-micro.cmake passes 0 or 1"
#endif

namespace {

/*
 * Build an interpreter for @p data (@p len bytes) and publish its I/O into the handle.
 *
 * On failure it leaves the interpreter destroyed and the handle UNTOUCHED -- which
 * means the handle's tensor pointers then refer to an interpreter that no longer
 * exists.  That is why every caller of this function on the failure path must
 * publish something before returning: see tflm_bk_reload().
 *
 * Returns 0 or one of the NN_MODEL_ERR_* codes in nn.h.
 */
int build_interp(const void *data, uint32_t len)
{
	nn_tflm_state &s = nn_tflm_board_state;
	const uint8_t *bytes = static_cast<const uint8_t *>(data);

	/* Validation, cheapest first.  The caller's CRC (a blob's, a container's) proves
	 * the bytes arrived intact -- but NOT that they are a whole .tflite: a file
	 * truncated on the PC before it was sent is stored faithfully and its CRC
	 * matches.  The structural checks below are what stands between that and a fault
	 * inside GetModel(). */
	if (len < 8u)
		return NN_MODEL_ERR_EMPTY;
	/* Flatbuffer file identifier, bytes 4..7 of any .tflite. */
	if (bytes[4] != 'T' || bytes[5] != 'F' || bytes[6] != 'L' || bytes[7] != '3')
		return NN_MODEL_ERR_FORMAT;

#if NN_TFLM_VERIFY
	/* The full structural walk: every offset, vector length and table in the buffer
	 * checked against `len` before anything dereferences them.  This is what catches
	 * the truncated-on-the-PC case above. */
	{
		flatbuffers::Verifier verifier(bytes, (size_t)len);
		if (!tflite::VerifyModelBuffer(verifier))
			return NN_MODEL_ERR_FORMAT;
	}
#endif

	const tflite::Model *model = tflite::GetModel(data);
	if (model->version() != TFLITE_SCHEMA_VERSION)
		return NN_MODEL_ERR_VERSION;

	nn_tflm_op_resolver *resolver = nn_tflm_get_resolver();
	if (!resolver)
		return NN_MODEL_ERR_OPS;

	nn_tflm_destroy_interp();
	s.interp = new (s.interp_buf) tflite::MicroInterpreter(
		model, *resolver, nn_tflm_arena, (size_t)NN_TFLM_ARENA_BYTES);

	/* Where a model that needs an unregistered op fails, and where one whose
	 * activations do not fit the arena fails.  TFLM does not distinguish the two in
	 * its return value; with TF_LITE_STRIP_ERROR_STRINGS there is no message either,
	 * which is the price paid for not linking MicroPrintf and stdio. */
	if (s.interp->AllocateTensors() != kTfLiteOk) {
		nn_tflm_destroy_interp();
		return NN_MODEL_ERR_ARENA;
	}

	/* 1..NN_MAX_IO inputs and outputs.  [!] Until issue #162 states the input
	 * contract, the camera producer fills input 0 only: a model with more inputs runs
	 * with the others undefined (the board READMEs say so). */
	size_t n_in  = s.interp->inputs_size();
	size_t n_out = s.interp->outputs_size();
	if (n_in < 1u || n_in > (size_t)NN_MAX_IO ||
	    n_out < 1u || n_out > (size_t)NN_MAX_IO) {
		nn_tflm_destroy_interp();
		return NN_MODEL_ERR_SHAPE;
	}
	/* Every tensor present, with a real buffer and a rank this API can describe.  Fail
	 * here rather than let a caller's indexing walk off the end of nn_tensor::dims. */
	for (size_t i = 0; i < n_in; i++) {
		TfLiteTensor *t = s.interp->input(i);
		if (!t || !t->data.data || !t->dims || t->dims->size > NN_MAX_DIMS) {
			nn_tflm_destroy_interp();
			return NN_MODEL_ERR_TENSOR;
		}
	}
	for (size_t i = 0; i < n_out; i++) {
		TfLiteTensor *t = s.interp->output(i);
		if (!t || !t->data.data || !t->dims || t->dims->size > NN_MAX_DIMS) {
			nn_tflm_destroy_interp();
			return NN_MODEL_ERR_TENSOR;
		}
	}

	/*
	 * Publish counts-last, and zero them first.
	 *
	 * `nn info` reads these descriptors WITHOUT the NN session -- deliberately,
	 * because nn_backend.h promises open() and the descriptor accessors work with the
	 * external memory down, and making an inspector wait on a running benchmark would
	 * be a poor trade.  So a second console can be walking the handle while this
	 * runs.  Filling the arrays with the counts still describing the OLD model would
	 * let it print one model's shapes under another's name.  Zeroed first, the only
	 * states it can observe are "no model" and the complete new one.
	 *
	 * This is ordering, not atomicity: it is worth exactly what it claims, and the
	 * reason it is enough is that nothing there is dereferenced -- `nn info` reads
	 * dims / dtype / scale out of the descriptor and never follows ->data, so a torn
	 * read costs a confusing line, not a fault.  No synchronisation is added here.
	 */
	s.tm.n_in  = 0;
	s.tm.n_out = 0;
	for (size_t i = 0; i < n_in; i++)
		nn_tflm_fill_tensor(&s.tm.in[i], s.interp->input(i));
	for (size_t i = 0; i < n_out; i++)
		nn_tflm_fill_tensor(&s.tm.out[i], s.interp->output(i));
	s.tm.used  = (uint32_t)s.interp->arena_used_bytes();
	s.tm.n_in  = (int)n_in;
	s.tm.n_out = (int)n_out;
	return 0;
}

}  /* namespace */

/*
 * The vtable callbacks.  Internal linkage plus C language linkage, so their addresses
 * have C function-pointer type and match struct nn_backend_vt exactly.
 */
extern "C" {

static int tflm_bk_init(void)
{
	return 0;   /* TFLM has no global runtime to bring up */
}

/* Opens with no model -- see the file header for why that is the normal state and not
 * a degraded one.  Touches no tensor body, so `nn info` works with the external
 * memory down. */
static int tflm_bk_open(void **impl_out)
{
	nn_tflm_state &s = nn_tflm_board_state;

	if (!s.tm.open) {
		nn_tflm_publish_empty();
		s.tm.open = true;
	}
	*impl_out = &s.tm;
	return 0;
}

static void tflm_bk_close(void *impl)
{
	(void)impl;   /* singleton: the interpreter and its arena outlive any one command */
}

/* Hand out the slot that is NOT live, so filling it cannot touch the flatbuffer the
 * running interpreter still holds pointers into.  With one slot a corrupt download
 * would take the working model down with it; with two, the worst case is a message.
 * The read limit is the board's capacity object (nn_tflm_mem.h), read through a
 * volatile so it is that object and not a folded constant. */
static int tflm_bk_load_region(void **buf, uint32_t *cap)
{
	const nn_tflm_state &s = nn_tflm_board_state;
	const unsigned inactive = (s.active_model != nullptr && s.active_slot == 0u) ? 1u : 0u;

	*buf = nn_tflm_model_buf[inactive];
	*cap = *(const volatile uint32_t *)&nn_tflm_stage_cap;
	return 0;
}

/*
 * Unload (svc/nn/nn.c calls this for `nn model unload`, issue #131 P16): destroy the
 * interpreter and stay open with no model.  The slots keep their bytes, but none of
 * them is ACTIVE any more, so the next load_region() hands out slot 0.
 */
static int tflm_bk_release(void)
{
	nn_tflm_state &s = nn_tflm_board_state;

	nn_tflm_destroy_interp();
	s.active_model = nullptr;
	s.active_len   = 0u;
	s.active_slot  = 0u;
	nn_tflm_publish_empty();
	s.tm.open = true;
	return 0;
}

/*
 * Adopt a model.  Transactional: on any rejection the previously active model is
 * rebuilt and reported, so a bad file costs a message and nothing else.  When nothing
 * was loaded before, or even the restore fails, the backend stays open and EMPTY
 * (issue #131 P16).  *impl_out is ALWAYS the resulting handle -- there is only one.
 */
static int tflm_bk_reload(const void *data, uint32_t len, const char *name,
                          void **impl_out)
{
	nn_tflm_state &s = nn_tflm_board_state;
	const void *old_model = s.active_model;
	const uint32_t old_len  = s.active_len;
	const uint32_t old_slot = s.active_slot;
	int slot;
	int rc;

	*impl_out = &s.tm;
	if (data == nullptr)                      /* nn.c never asks; release() unloads */
		return NN_ERR_ARG;

	/* Only our own staging slots are accepted, by containment, aligned, and never the
	 * live one: nn_tflm_fit.h, the rule its host test walks.  Interpreting from
	 * wherever the caller points would put the flatbuffer's lifetime and alignment in
	 * the caller's hands, and TFLM keeps pointers INTO the model for as long as the
	 * interpreter lives.  Nothing has changed on a refusal here. */
	{
		uintptr_t bases[NN_TFLM_MODEL_SLOTS];

		for (unsigned i = 0u; i < NN_TFLM_MODEL_SLOTS; i++)
			bases[i] = (uintptr_t)nn_tflm_model_buf[i];
		slot = nn_tflm_pick_slot(bases, NN_TFLM_MODEL_SLOTS, NN_TFLM_MODEL_MAX,
		                         (uintptr_t)data, len, NN_MODEL_ALIGN,
		                         old_model != nullptr ? (int)old_slot : -1);
		if (slot < 0)
			return slot;
	}

	rc = build_interp(data, len);
	if (rc == 0) {
		/* Published only after the interpreter exists AND its I/O was accepted. */
		s.active_model = data;
		s.active_len   = len;
		s.active_slot  = (uint32_t)slot;
		nn_tflm_set_model_name(name);
		s.tm.open = true;
		return 0;
	}

	/* Rejected.  build_interp() has destroyed the interpreter, so the handle currently
	 * describes tensors that no longer exist -- something must be published before
	 * returning, whatever the outcome. */
	if (old_model != nullptr) {
		/* The previous model's bytes are untouched (it lives in the OTHER slot), so
		 * this rebuild should not fail.  The name buffer still holds its name -- only
		 * success paths overwrite it -- so re-pointing the handle is all the naming
		 * needed. */
		if (build_interp(old_model, old_len) == 0) {
			s.active_model = old_model;
			s.active_len   = old_len;
			s.active_slot  = old_slot;
			s.tm.name      = s.model_name;
			s.tm.open      = true;
			return rc;                    /* report why the NEW model was refused */
		}
	}

	/* Nothing was loaded before -- or, catastrophically, even the known-good model
	 * could not be rebuilt.  Empty, and open (issue #131 P16): closing would make the
	 * next open start over and hand out descriptors nobody loaded.
	 *
	 * [!] EMPTY MEANS NO INTERPRETER EITHER -- the same clean-up as release().  A
	 * refusal in validation (identifier, verifier, version) returns before
	 * build_interp() destroys anything, so when both the new model and the restore
	 * are refused there, the previous interpreter is still alive: left alone, run()
	 * would Invoke() it on a slot load_region() is now free to hand out. */
	nn_tflm_destroy_interp();
	s.active_model = nullptr;
	s.active_len   = 0u;
	s.active_slot  = 0u;
	nn_tflm_publish_empty();
	s.tm.open = true;
	return rc;
}

static const char *tflm_bk_name(void *impl)
{
	return ((struct nn_tflm_model *)impl)->name;
}

static int tflm_bk_in_count(void *impl)  { return ((struct nn_tflm_model *)impl)->n_in; }
static int tflm_bk_out_count(void *impl) { return ((struct nn_tflm_model *)impl)->n_out; }

static struct nn_tensor *tflm_bk_input(void *impl, int idx)
{
	struct nn_tflm_model *m = (struct nn_tflm_model *)impl;

	return (idx >= 0 && idx < m->n_in) ? &m->in[idx] : nullptr;
}

static struct nn_tensor *tflm_bk_output(void *impl, int idx)
{
	struct nn_tflm_model *m = (struct nn_tflm_model *)impl;

	return (idx >= 0 && idx < m->n_out) ? &m->out[idx] : nullptr;
}

/* The arena TFLM actually PLANNED for the open model (0 with none) -- the `used`
 * line of `nn info`.  The reservation is arena_reserved below (issue #131 P3). */
static uint32_t tflm_bk_acts_bytes(void *impl)
{
	return ((struct nn_tflm_model *)impl)->used;
}

static int tflm_bk_run(void *impl)
{
	const nn_tflm_state &s = nn_tflm_board_state;

	(void)impl;
	if (!s.interp)
		return NN_ERR_STATE;   /* open, but no model loaded */
	return (s.interp->Invoke() == kTfLiteOk) ? 0 : -1;
}

/* The version string carries the build's POSTURE, not just its identity.  Whether the
 * flatbuffer verifier is compiled in changes what a failed `nn model load` can mean, so
 * `nn info` should not require reading CMakeCache.txt to find out.  With it off, the
 * equivalent check lives on the PC (the board's scripts/verify_tflite.cc) and someone
 * has to have run it -- which is exactly the kind of assumption worth printing. */
#if defined(NN_TFLM_CMSIS_NN)
#  if NN_TFLM_VERIFY
static const struct nn_backend_info g_info = { "tflm", "tflite-micro + CMSIS-NN" };
#  else
static const struct nn_backend_info g_info = {
	"tflm", "tflite-micro + CMSIS-NN, no on-board model verify" };
#  endif
#else
#  if NN_TFLM_VERIFY
static const struct nn_backend_info g_info = { "tflm", "tflite-micro (reference kernels)" };
#  else
static const struct nn_backend_info g_info = {
	"tflm", "tflite-micro (reference kernels), no on-board model verify" };
#  endif
#endif

/*
 * The one definition of the selected backend: a definition with external linkage even
 * though it is const, because nn_backend.h declared it extern first.
 *
 * The designated initializers are a GCC extension in C++17 (standard only from
 * C++20) and are used knowingly, despite CXX_EXTENSIONS OFF.  The alternative is
 * positional initialization of fourteen function pointers whose types are largely
 * interchangeable -- where inserting a field in nn_backend.h silently shifts every
 * entry after it and the build stays green.  GCC accepts in-declaration-order
 * designators and rejects out-of-order ones, which is the case this relies on.
 */
const struct nn_backend_vt nn_backend_vt_selected = {
	.info              = &g_info,
	.init              = tflm_bk_init,
	.open              = tflm_bk_open,
	.close             = tflm_bk_close,
	.model_name        = tflm_bk_name,
	.in_count          = tflm_bk_in_count,
	.out_count         = tflm_bk_out_count,
	.input             = tflm_bk_input,
	.output            = tflm_bk_output,
	.activations_bytes = tflm_bk_acts_bytes,
	.run               = tflm_bk_run,
	.load_region       = tflm_bk_load_region,
	.reload            = tflm_bk_reload,
	.release           = tflm_bk_release,
	.arena_reserved    = NN_TFLM_ARENA_BYTES,     /* nn_tflm_arena */
	.has_builtin       = 0u,                      /* empty until a load */
};

}  /* extern "C" */
