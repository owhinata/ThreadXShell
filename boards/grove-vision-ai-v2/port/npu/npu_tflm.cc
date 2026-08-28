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
 *
 * [!] AND THAT WAS NOT ENOUGH ONCE THE MODEL CAME OUT OF THE ASSET STORE
 * (issue #93).  The identifier says the first eight bytes look right; it says
 * nothing about the offsets behind them.  A model used to be something the host
 * build had verified and written to a fixed address, so "it is where we put it"
 * carried the rest of the argument -- but `nn open <name>` reads a payload an
 * operator sent, and the blob CRC that guards the transfer is computed over the
 * stream that ARRIVED, so a file that was already broken on the PC verifies
 * perfectly and lands here intact.  Hence a real bounds check, in this order:
 *
 *     range -> length -> identifier -> flatbuffer verifier -> payload walk
 *
 * The verifier is the generated tflite::VerifyModelBuffer(), which is what
 * makes every offset in the buffer a checked one; the payload walk after it
 * (npu_model_scan) is about what the model MEANS and needs the offsets to be
 * trustworthy before it starts.
 */
#include "npu.h"
#include "npu_hw.h"
#include "npu_model_scan.h"

#include <new>
#include <string.h>

#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

/* The bounded flatbuffer check and its limits, shared with the host gate so
 * that scripts/verify_vela_model.cc reaches THIS verdict and not one that
 * resembles it (issue #93). */
#include "npu_verify.h"

namespace {

/* The flash read window the model must live in.  BASE_ADDR_FLASH1_R_ALIAS and
 * FLASH1_SIZE from the SDK's WE2_device_addr.h, spelled out here so this file
 * does not drag the whole device header into a C++ TU.
 *
 * The other spelling of the base is NOR_XIP_BASE (port/nor/nor_flash.h), which
 * is what everything above this boundary uses; npu_model_len_max() is how a
 * caller asks THIS file where the window ends rather than assuming the two
 * agree. */
constexpr uint32_t kFlashBase = 0x3A000000u;
constexpr uint32_t kFlashSize = 0x01000000u;

/* The length floor: npu_verify.h owns it, because it is a property of where the
 * file identifier sits rather than of this file. */
constexpr uint32_t kMinModelBytes = NPU_MODEL_MIN_BYTES;

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

	/*
	 * [!] A HIGHER-RANK TENSOR IS REFUSED, NOT TRUNCATED (issue #97).  This
	 * used to clamp the rank to four and report the first four dimensions,
	 * which is worse than losing information: a truncated shape can still
	 * MATCH a consumer's lookup, and then the consumer reads a tensor it was
	 * not looking for while every check it makes passes.  Leaving rank 0 (and
	 * the dims zeroed by the memset above) says "not representable here", and
	 * every shape test downstream fails on it.
	 */
	if (t->dims != nullptr && t->dims->size >= 0 && t->dims->size <= 4) {
		int rank = t->dims->size;

		out->rank = static_cast<uint8_t>(rank);
		for (int i = 0; i < rank; i++)
			out->dims[i] = t->dims->data[i];
	}
}

}  /* namespace */

extern "C" uint32_t npu_model_len_min(void)
{
	return kMinModelBytes;
}

extern "C" uint32_t npu_model_len_max(uint32_t model_addr)
{
	if (model_addr < kFlashBase || model_addr >= kFlashBase + kFlashSize)
		return 0u;
	return kFlashBase + kFlashSize - model_addr;
}

extern "C" int npu_open(uint32_t model_addr, uint32_t model_len, void *arena,
                        size_t arena_bytes)
{
	if (g_interp != nullptr)
		return NPU_ERR_STATE;
	if (arena == nullptr || arena_bytes == 0u)
		return NPU_ERR_ARENA;

	/* Range and length first: everything after this dereferences the address,
	 * and everything after the verifier trusts the length.
	 *
	 * Subtraction-based, in the same discipline as nor_span.c and
	 * blob_map_check(): `model_addr + model_len` is never formed, so a length
	 * near 2^32 is refused rather than wrapped into looking contained. */
	if (model_addr < kFlashBase || model_addr >= kFlashBase + kFlashSize)
		return NPU_ERR_MODEL_ADDR;
	if (model_len < kMinModelBytes ||
	    model_len > kFlashBase + kFlashSize - model_addr)
		return NPU_ERR_MODEL_ADDR;

	/* Then the file identifier, on raw bytes.  Flatbuffers put it at offset 4;
	 * erased flash reads 0xFF and fails here instead of inside the parser.
	 * Inside the declared length, because kMinModelBytes is a floor on it. */
	const uint8_t *raw = reinterpret_cast<const uint8_t *>(model_addr);
	if (memcmp(raw + 4, "TFL3", 4) != 0)
		return NPU_ERR_MODEL_MAGIC;

	/* [!] AND ONLY NOW IS THE BUFFER SAFE TO FOLLOW (issue #93).  Every check
	 * above is on bytes at known offsets; this is the one that makes the
	 * OFFSETS trustworthy, and it has to come before GetModel() because
	 * GetModel() is a cast and the first accessor after it is already
	 * following one. */
	if (!npu_verify_model_buffer(raw, model_len))
		return NPU_ERR_MODEL_FORMAT;

	const tflite::Model *model =
		tflite::GetModel(reinterpret_cast<const void *>(model_addr));

	if (model->version() != TFLITE_SCHEMA_VERSION)
		return NPU_ERR_SCHEMA;

	/* Before anything can be launched: prove no driver action follows the
	 * command stream.  The scan lives in npu_model_scan.cc so that the host
	 * gate (scripts/verify_vela_model.cc) reaches this same verdict on the
	 * same bytes -- see that file's header for why sharing the LOCATOR matters
	 * as much as sharing the walk. */
	if (npu_model_payload_refusal(model) != nullptr)
		return NPU_ERR_PAYLOAD;

	g_resolver = new (g_resolver_store) tflite::MicroMutableOpResolver<1>();
	if (g_resolver->AddEthosU() != kTfLiteOk) {
		g_resolver->~MicroMutableOpResolver<1>();
		g_resolver = nullptr;
		return NPU_ERR_OPS;
	}

	/* The cache protocol maintains this arena as a whole, so it has to be told
	 * which one.  Set before AllocateTensors so that every unwind below goes
	 * through npu_close() and clears it again. */
	npu_cache_set_arena(arena, arena_bytes);

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
	/* Take the arena back from the cache protocol.  A stale base here would
	 * have the flush hook cleaning memory nothing owns any more. */
	npu_cache_set_arena(nullptr, 0u);
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

	const TfLiteStatus st = g_interp->Invoke();

	/* Cache maintenance is NOT done here and must not be: TFLM has already
	 * written the arena on its way out of Invoke().  This only closes out the
	 * protocol's state and catches a launch that never handed the arena back. */
	npu_cache_after_invoke();

	return st == kTfLiteOk ? NPU_OK : NPU_ERR_INVOKE;
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
	case NPU_ERR_PAYLOAD:     return "model payload has actions after the "
	                                 "command stream";
	case NPU_ERR_MODEL_FORMAT: return "model flatbuffer is malformed";
	default:                  return "unknown";
	}
}

/*
 * The one TFLite fact the C side is allowed to ask about.
 *
 * npu_tensor::type is a TfLiteType narrowed to int8_t, so the enumerator has to
 * survive that narrowing for the question to be answerable at all -- and the
 * decoder casts a buffer on the answer.  Both are checked here, at compile
 * time, in the only translation unit that can see the enumeration.
 */
static_assert(static_cast<int>(kTfLiteInt8) ==
              static_cast<int>(static_cast<int8_t>(kTfLiteInt8)),
              "kTfLiteInt8 does not survive npu_tensor's int8_t type field");

extern "C" bool npu_tensor_is_int8(int8_t type)
{
	return static_cast<TfLiteType>(type) == kTfLiteInt8;
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
