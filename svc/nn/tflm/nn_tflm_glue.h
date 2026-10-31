/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_glue.h
 * @brief   The TFLM glue every backend built on a MicroInterpreter needs in the same
 *          shape (issue #169): tensor descriptors, the display name, the empty
 *          state, the op resolver and the interpreter's construction (which zeroes
 *          the arena, issue #167) and destruction.
 *
 * Two backends include it: svc/nn/tflm/nn_tflm.cc (the CPU-kernel backend of
 * wio-lite-ai and f746g-disco, which adopts models from staging slots) and the
 * Ethos-U backend under svc/nn/ethosu/ (which interprets a model in place).  WHERE a
 * model may come from, and what is checked before the interpreter sees it, is each
 * backend's own policy and is NOT here -- that is the part that differs.
 *
 * [!] INCLUDED INTO ONE TRANSLATION UNIT, NOT LINKED.  Everything below is in an
 * unnamed namespace: each backend gets its own internal copy, compiled in its own
 * context (its own TFLM tree, its own op profile), exactly as if it were written in
 * that file.  That is why the move out of nn_tflm.cc left wio-lite-ai's and
 * f746g-disco's images byte-identical.
 *
 * [!] NO STORAGE.  Functions only; the state is the backend's named object, which
 * the including file names BEFORE including this header:
 *
 *   #define NN_TFLM_GLUE_STATE  nn_tflm_board_state   (the backend's state object)
 *   #define NN_TFLM_GLUE_FACTS  nn_tflm_board_facts   (the board's facts)
 *
 * WHY MACROS AND NOT TEMPLATES OR PARAMETERS.  The first version took the state as a
 * template parameter.  It compiled to the same logic, but GCC's inliner budgets the
 * whole translation unit: functions instantiated at the end of the TU, or taking the
 * state by reference, shifted inlining decisions in unrelated code (the flatbuffer
 * verifiers grew by 64 B) and the images stopped being byte-identical.  Naming the
 * object keeps every function textually what it was in nn_tflm.cc, in the same place
 * in the TU, which is the property the move had to prove.  For the same reason the
 * counts-last publication of the interpreter's I/O stays inside each backend's
 * build_interp(): split into a function here it changed that object's code (the
 * inliner again), so it is not shared yet.  The interpreter's construction IS shared
 * since issue #167 (nn_tflm_construct_interp(), below), because that issue changed
 * what construction must do and the change had to be the same in both backends.
 *
 * The state needs these members, by name:
 *
 *   tm             the descriptor handle: in[], out[], n_in, n_out, used, name
 *   model_name     char array the handle's name points into
 *   interp         tflite::MicroInterpreter *, nullptr when none exists
 *   interp_buf     raw storage the interpreter is placement-new'd into
 *   resolver       pointer to the backend's op resolver type, nullptr until built
 *   resolver_buf   raw storage the resolver is placement-new'd into
 *
 * and the facts need `name_prefix`.  The op list is the including file's
 * NN_TFLM_OPS_ALL (nn_tflm_ops.h), which must be in scope before this header.
 */
#ifndef NN_TFLM_GLUE_H
#define NN_TFLM_GLUE_H

#include "tensorflow/lite/micro/micro_interpreter.h"

#include "nn.h"

#include <new>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#ifndef NN_TFLM_OPS_ALL
#error "nn_tflm_glue.h needs the op list: include nn_tflm_ops.h first"
#endif
#if !defined(NN_TFLM_GLUE_STATE) || !defined(NN_TFLM_GLUE_FACTS)
#error "nn_tflm_glue.h needs NN_TFLM_GLUE_STATE and NN_TFLM_GLUE_FACTS defined first"
#endif

/* The backend's op resolver type, read off its state object. */
using nn_tflm_glue_resolver =
	std::remove_pointer<decltype(NN_TFLM_GLUE_STATE.resolver)>::type;

namespace {

/* Used by: tflm, ethosu.  TfLiteType -> the nn.h dtype; anything else is NONE. */
uint8_t nn_tflm_map_dtype(TfLiteType t)
{
	switch (t) {
	case kTfLiteInt8:    return NN_DTYPE_INT8;
	case kTfLiteUInt8:   return NN_DTYPE_UINT8;
	case kTfLiteInt16:   return NN_DTYPE_INT16;
	case kTfLiteInt32:   return NN_DTYPE_INT32;
	case kTfLiteFloat32: return NN_DTYPE_FLOAT32;
	default:             return NN_DTYPE_NONE;
	}
}

/* Used by: tflm, ethosu.  One TFLM tensor -> one nn_tensor descriptor. */
void nn_tflm_fill_tensor(struct nn_tensor *nt, const TfLiteTensor *tt)
{
	int nd = tt->dims ? tt->dims->size : 0;

	nt->data  = tt->data.data;
	nt->bytes = (uint32_t)tt->bytes;
	nt->ndim  = (uint8_t)(nd <= NN_MAX_DIMS ? nd : NN_MAX_DIMS);
	for (int i = 0; i < NN_MAX_DIMS; i++)
		nt->dims[i] = (i < nd) ? (uint16_t)tt->dims->data[i] : 1;
	nt->dtype      = nn_tflm_map_dtype(tt->type);
	nt->scale      = tt->params.scale;
	nt->zero_point = tt->params.zero_point;
}

/* Used by: tflm, ethosu.  Copy a display name into the backend-owned buffer: the
 * board's prefix, then @p name.  nullptr / "" means no model and reads "(none)",
 * never prefixed. */
void nn_tflm_set_model_name(const char *name)
{
	auto &s = NN_TFLM_GLUE_STATE;
	const int cap = (int)sizeof(s.model_name) - 1;
	int n = 0;

	if (name && name[0]) {
		const char *pre = NN_TFLM_GLUE_FACTS.name_prefix;

		for (; pre && *pre && n < cap; pre++)
			s.model_name[n++] = *pre;
		for (; *name && n < cap; name++)
			s.model_name[n++] = *name;
	} else {
		for (const char *src = "(none)"; *src && n < cap; src++)
			s.model_name[n++] = *src;
	}
	s.model_name[n] = '\0';
	s.tm.name = s.model_name;
}

/* Used by: tflm, ethosu.  No model: zero tensors, no activations, the name "(none)".
 * A legitimate steady state, not an error.  Only the descriptors change; the caller
 * has destroyed the interpreter. */
void nn_tflm_publish_empty()
{
	auto &s = NN_TFLM_GLUE_STATE;

	s.tm.n_in  = 0;
	s.tm.n_out = 0;
	s.tm.used  = 0u;
	nn_tflm_set_model_name(nullptr);
}

/*
 * Used by: tflm, ethosu.  The op resolver, constructed on first use into storage that
 * is never destroyed.
 *
 * [!] NOT a function-local `static` resolver, which is the obvious way to write this.
 * MicroMutableOpResolver<N> has a non-trivial destructor, so GCC emits a registration
 * for it -- and because this build uses -fno-use-cxa-atexit, that registration goes to
 * plain atexit(), pulling newlib's atexit, __register_exitproc and __call_exitprocs
 * into the image (measured on wio-lite-ai's first link) to run a destructor at a
 * program exit that cannot happen.  And a function-local static would be storage in a
 * shared file.
 *
 * Placement-new into the backend's raw storage removes the destructor from the program
 * entirely, which is also the truthful description of this object's lifetime.  No
 * __cxa_guard_* is needed (-fno-threadsafe-statics) because every path that reaches
 * here holds the single NN session: svc/nn/nn.h requires the caller to hold it across
 * a model swap, and a model swap is the only thing that calls this.
 */
nn_tflm_glue_resolver *nn_tflm_get_resolver()
{
	auto &s = NN_TFLM_GLUE_STATE;

	if (s.resolver)
		return s.resolver;

	nn_tflm_glue_resolver &resolver = *new (s.resolver_buf) nn_tflm_glue_resolver();
	bool ok = true;

	/* AddXxx() returns kTfLiteError only when the resolver's slots are exhausted, which
	 * cannot happen here -- NN_TFLM_OPS_TOTAL counts the very list being expanded -- so
	 * any failure is a build-time mistake and fatal for the whole backend rather than
	 * for one model.  The first refusal stops the rest. */
#define NN_TFLM_REGISTER(enum_suffix, method) \
	ok = ok && (resolver.method() == kTfLiteOk);
	NN_TFLM_OPS_ALL(NN_TFLM_REGISTER)
#undef NN_TFLM_REGISTER

	if (ok) {
		s.resolver = &resolver;
		return s.resolver;
	}
	/* Leave the pointer null so this is retried rather than a half-filled resolver
	 * being handed out.  Nothing to destroy: the object has no destructor in this
	 * program, by construction (see above). */
	return nullptr;
}

/* Used by: tflm, ethosu.  ~MicroInterpreter() is NOT trivial -- it calls
 * graph_.FreeSubgraphs() -- so a rebuild must destroy the previous one before
 * placement-new reuses the storage. */
void nn_tflm_destroy_interp()
{
	auto &s = NN_TFLM_GLUE_STATE;

	if (s.interp) {
		s.interp->~MicroInterpreter();
		s.interp = nullptr;
	}
}

/*
 * Used by: tflm, ethosu.  Construct the interpreter for @p model over the WHOLE arena
 * [@p arena, @p arena + @p bytes), destroying any previous one first, and ZERO THE
 * ARENA BETWEEN THE TWO (issue #167).
 *
 * [!] WHY THE ZEROING.  TFLM takes the per-operator NodeAndRegistration array from the
 * arena without clearing it (MicroAllocator::StartModelAllocation), and
 * MicroInterpreter fills it front to back, returning at the first operator the
 * resolver does not have (PrepareNodeAndRegistrationDataFromFlatbuffer).  The
 * elements after that one keep whatever the arena held before -- the previous model's
 * data.  Destroying that half-built interpreter then runs FreeSubgraphs(), which walks
 * EVERY operator and calls registration->free() whenever registration is non-null:
 * it calls the stale bytes as a function pointer.  On wio-lite-ai a 16-op model
 * refused at op 12, loaded after an 8-op one, jumped to garbage and froze the console.
 * With the arena zeroed every element not yet filled has registration == nullptr and
 * the destructor calls nothing.  TFLM is read-only (lib/, the SDK's tree), so the
 * backend does it.
 *
 * The zeroing comes AFTER the destruction, never before: the old interpreter's
 * destructor reads its own registrations out of this same arena.  That is why the
 * destruction is in here too (it is a no-op when the caller has already done it).
 *
 * The cost is one memset of the whole arena per load, restores included.  The caller
 * holds the NN session (svc/nn/nn.h) and has brought up the memory the arena lives in.
 */
tflite::MicroInterpreter *nn_tflm_construct_interp(const tflite::Model *model,
                                                   nn_tflm_glue_resolver &resolver,
                                                   uint8_t *arena, size_t bytes)
{
	auto &s = NN_TFLM_GLUE_STATE;

	nn_tflm_destroy_interp();
	memset(arena, 0, bytes);
	s.interp = new (s.interp_buf) tflite::MicroInterpreter(model, resolver, arena, bytes);
	return s.interp;
}

}  /* namespace */

#endif /* NN_TFLM_GLUE_H */
