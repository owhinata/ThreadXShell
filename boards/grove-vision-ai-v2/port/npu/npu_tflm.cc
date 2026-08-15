/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_tflm.cc
 * @brief   TFLite Micro glue behind npu.h (issue #44).
 *
 * The only C++ in this port outside the SDK startup, and it is contained on
 * purpose: nothing above npu.h sees a TFLite type.
 *
 * ONE OPERATOR.  MicroMutableOpResolver<1> with AddEthosU() and nothing else.
 * A Vela-compiled model folds its whole graph into the single `ethos-u` custom
 * operator, so there is no CPU kernel to register -- which is also why no
 * CMSIS-NN (i.e. no Helium) comes anywhere near this image.  A model that Vela
 * did NOT fully offload fails at AllocateTensors with a missing-op error rather
 * than silently falling back to a scalar reference kernel, and that is the
 * behaviour we want: a silent fallback would be slow in a way nobody would
 * think to look for.
 *
 * NO HEAP, NO LAZY STATICS.  Every object lives in file-scope storage and is
 * constructed with placement new at a point this file chooses.  Two reasons:
 * the port's rule that permanent state is statically allocated, and
 * -fno-threadsafe-statics being on globally, which makes a function-local
 * static's first-use initialisation unsafe the moment two threads can reach it.
 * The shell can reach it from two threads (foreground and background jobs), so
 * "it is only initialised once" is not something to leave to the ABI.
 *
 * VALIDATE BEFORE WALKING.  tflite::GetModel() is a cast, not a parse: it hands
 * back a pointer and every subsequent access follows offsets out of the buffer.
 * Pointed at erased flash (all 0xFF) those offsets are garbage, and the first
 * thing to notice would be a fault. So the address range and the "TFL3" file
 * identifier are checked first, on raw bytes.
 */
#include "npu.h"

#include <new>
#include <string.h>

#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

/* The flash read window the model must live in.  BASE_ADDR_FLASH1_R_ALIAS and
 * FLASH1_SIZE from the SDK's WE2_device_addr.h, spelled out here so this file
 * does not drag the whole device header into a C++ TU. */
constexpr uint32_t kFlashBase = 0x3A000000u;
constexpr uint32_t kFlashSize = 0x01000000u;

/* Smallest buffer that could hold a flatbuffer header worth inspecting: root
 * offset + the 4-byte file identifier at offset 4. */
constexpr uint32_t kMinModelBytes = 16u;

/* Storage for the interpreter and resolver.  Raw bytes plus placement new, so
 * nothing is constructed until npu_open() says so and nothing is destroyed
 * until npu_close() does.  alignas() from the types themselves rather than a
 * guessed number. */
alignas(tflite::MicroInterpreter) uint8_t
	g_interp_store[sizeof(tflite::MicroInterpreter)];
alignas(tflite::MicroMutableOpResolver<1>) uint8_t
	g_resolver_store[sizeof(tflite::MicroMutableOpResolver<1>)];

tflite::MicroInterpreter          *g_interp   = nullptr;
tflite::MicroMutableOpResolver<1> *g_resolver = nullptr;

void describe(const TfLiteTensor *t, struct npu_tensor *out)
{
	memset(out, 0, sizeof(*out));
	out->data  = t->data.data;
	out->bytes = t->bytes;
	out->type  = static_cast<int8_t>(t->type);

	/* Quantisation parameters matter to the caller: the model is int8 and the
	 * pixels arriving from the camera are uint8, so somebody has to know the
	 * zero point to convert between them. */
	out->zero_point = t->params.zero_point;
	out->scale      = t->params.scale;

	if (t->dims != nullptr) {
		int rank = t->dims->size;

		if (rank > 4)
			rank = 4;               /* npu_tensor carries four; report what fits */
		out->rank = static_cast<uint8_t>(rank);
		for (int i = 0; i < rank; i++)
			out->dims[i] = t->dims->data[i];
	}
}

}  /* namespace */

extern "C" int npu_open(uint32_t model_addr, void *arena, size_t arena_bytes)
{
	if (g_interp != nullptr)
		return NPU_ERR_STATE;
	if (arena == nullptr || arena_bytes == 0u)
		return NPU_ERR_ARENA;

	/* Range first: everything after this dereferences the address. */
	if (model_addr < kFlashBase ||
	    model_addr > kFlashBase + kFlashSize - kMinModelBytes)
		return NPU_ERR_MODEL_ADDR;

	/* Then the file identifier, on raw bytes.  Flatbuffers put it at offset 4;
	 * erased flash reads 0xFF and fails here instead of inside the parser. */
	const uint8_t *raw = reinterpret_cast<const uint8_t *>(model_addr);
	if (memcmp(raw + 4, "TFL3", 4) != 0)
		return NPU_ERR_MODEL_MAGIC;

	const tflite::Model *model =
		tflite::GetModel(reinterpret_cast<const void *>(model_addr));

	if (model->version() != TFLITE_SCHEMA_VERSION)
		return NPU_ERR_SCHEMA;

	g_resolver = new (g_resolver_store) tflite::MicroMutableOpResolver<1>();
	if (g_resolver->AddEthosU() != kTfLiteOk) {
		g_resolver->~MicroMutableOpResolver<1>();
		g_resolver = nullptr;
		return NPU_ERR_OPS;
	}

	g_interp = new (g_interp_store) tflite::MicroInterpreter(
		model, *g_resolver, static_cast<uint8_t *>(arena), arena_bytes);

	if (g_interp->AllocateTensors() != kTfLiteOk) {
		/* Unwind fully: a half-open NPU is the state the shell must never be
		 * able to observe. */
		npu_close();
		return NPU_ERR_ARENA;
	}
	if (g_interp->inputs_size() < 1u || g_interp->outputs_size() < 1u) {
		npu_close();
		return NPU_ERR_TENSORS;
	}
	return NPU_OK;
}

extern "C" void npu_close(void)
{
	if (g_interp != nullptr) {
		g_interp->~MicroInterpreter();
		g_interp = nullptr;
	}
	if (g_resolver != nullptr) {
		g_resolver->~MicroMutableOpResolver<1>();
		g_resolver = nullptr;
	}
}

extern "C" int npu_input(struct npu_tensor *out)
{
	if (g_interp == nullptr || out == nullptr)
		return NPU_ERR_STATE;

	const TfLiteTensor *t = g_interp->input(0);

	if (t == nullptr)
		return NPU_ERR_TENSORS;
	describe(t, out);
	return NPU_OK;
}

extern "C" int npu_output(unsigned index, struct npu_tensor *out)
{
	if (g_interp == nullptr || out == nullptr)
		return NPU_ERR_STATE;
	if (index >= g_interp->outputs_size())
		return NPU_ERR_TENSORS;

	const TfLiteTensor *t = g_interp->output(index);

	if (t == nullptr)
		return NPU_ERR_TENSORS;
	describe(t, out);
	return NPU_OK;
}

extern "C" unsigned npu_output_count(void)
{
	return g_interp == nullptr ? 0u
	                           : static_cast<unsigned>(g_interp->outputs_size());
}

extern "C" int npu_invoke(void)
{
	if (g_interp == nullptr)
		return NPU_ERR_STATE;
	return g_interp->Invoke() == kTfLiteOk ? NPU_OK : NPU_ERR_INVOKE;
}

extern "C" size_t npu_arena_used(void)
{
	return g_interp == nullptr ? 0u : g_interp->arena_used_bytes();
}

extern "C" const char *npu_status_name(int status)
{
	switch (status) {
	case NPU_OK:              return "ok";
	case NPU_ERR_STATE:       return "wrong state";
	case NPU_ERR_MODEL_ADDR:  return "model address outside the flash window";
	case NPU_ERR_MODEL_MAGIC: return "no TFL3 model at that address";
	case NPU_ERR_SCHEMA:      return "model schema version mismatch";
	case NPU_ERR_OPS:         return "operator set not supported";
	case NPU_ERR_ARENA:       return "arena too small for this model";
	case NPU_ERR_TENSORS:     return "unexpected tensor layout";
	case NPU_ERR_INVOKE:      return "inference failed";
	default:                  return "unknown";
	}
}

extern "C" const char *npu_type_name(int8_t type)
{
	switch (static_cast<TfLiteType>(type)) {
	case kTfLiteInt8:    return "int8";
	case kTfLiteUInt8:   return "uint8";
	case kTfLiteInt16:   return "int16";
	case kTfLiteInt32:   return "int32";
	case kTfLiteFloat32: return "f32";
	default:             return "?";
	}
}
